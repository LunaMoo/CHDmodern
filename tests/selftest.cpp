// license:BSD-3-Clause
// copyright-holders:CHDmodern contributors
/***************************************************************************

    selftest.cpp

    Round-trip test: compress synthetic data into CHDs with every generic
    codec using MAME's own compressor (C++ API), then read it back through
    the C API - both by path and through custom I/O callbacks - and compare.

***************************************************************************/

#include "chdmodern/chdmodern.h"

#include "cdrom.h"
#include "chd.h"
#include "chdcodec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
bool g_keep = false;   // --keep: leave the generated CHDs for inspection

#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

#define CHECK_OK(expr) \
	do { int const _r = (expr); if (_r != CHDM_OK) { std::fprintf(stderr, "FAILED %s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #expr, _r, chdm_last_error_message()); ++g_failures; } } while (0)


// Data that is partly compressible (text-like), partly noise, partly
// audio-like and partly zero, so every codec gets exercised.
std::vector<uint8_t> make_test_data(std::size_t size)
{
	std::vector<uint8_t> data(size);
	std::mt19937 rng(12345);
	for (std::size_t i = 0; i < size; i++)
	{
		std::size_t const region = (i / 65536) % 4;
		switch (region)
		{
		case 0: data[i] = uint8_t("CHDmodern test pattern "[i % 23]); break;
		case 1: data[i] = uint8_t(rng()); break;
		case 2: data[i] = uint8_t(int16_t(8000.0 * std::sin(double(i / 2) * 0.05)) >> ((i & 1) * 8)); break;
		default: data[i] = 0; break;
		}
	}
	return data;
}


class memory_compressor : public chd_file_compressor
{
public:
	explicit memory_compressor(std::vector<uint8_t> const &data) : m_data(data) { }

	uint32_t read_data(void *dest, uint64_t offset, uint32_t length) override
	{
		uint32_t avail = 0;
		if (offset < m_data.size())
			avail = uint32_t(std::min<uint64_t>(length, m_data.size() - offset));
		std::memcpy(dest, m_data.data() + offset, avail);
		std::memset(static_cast<uint8_t *>(dest) + avail, 0, length - avail);
		return length;
	}

private:
	std::vector<uint8_t> const &m_data;
};


bool create_chd(std::string const &path, std::vector<uint8_t> const &data, chd_codec_type codec, bool dvd)
{
	// chd_file_compressor is over 1 MB: keep it off the stack (Windows' is 1 MB)
	auto const holder = std::make_unique<memory_compressor>(data);
	memory_compressor &chd = *holder;
	chd_codec_type const compression[4] = { codec, CHD_CODEC_NONE, CHD_CODEC_NONE, CHD_CODEC_NONE };
	uint32_t const hunkbytes = 2048 * 8;
	if (chd.create(path, data.size(), hunkbytes, 2048, compression))
		return false;
	if (dvd && chd.write_metadata(DVD_METADATA_TAG, 0, std::string()))
		return false;
	if (chd.write_metadata(CHD_MAKE_TAG('T','E','S','T'), 0, std::string("hello metadata")))
		return false;

	chd.compress_begin();
	double progress, ratio;
	std::error_condition err;
	while ((err = chd.compress_continue(progress, ratio)) == chd_file::error::WALKING_PARENT || err == chd_file::error::COMPRESSING)
	{
	}
	if (err)
	{
		std::fprintf(stderr, "compression failed: %s\n", err.message().c_str());
		return false;
	}
	return true;
}


struct mem_io
{
	std::vector<uint8_t> bytes;
	bool closed = false;
};

uint64_t mem_size(void *user) { return static_cast<mem_io *>(user)->bytes.size(); }

size_t mem_read(void *user, uint64_t offset, void *buffer, size_t length)
{
	auto &b = static_cast<mem_io *>(user)->bytes;
	if (offset >= b.size())
		return 0;
	size_t const n = std::min<size_t>(length, b.size() - offset);
	std::memcpy(buffer, b.data() + offset, n);
	return n;
}

void mem_close(void *user) { static_cast<mem_io *>(user)->closed = true; }


void verify(chdm_file *file, std::vector<uint8_t> const &data, uint32_t codec, bool dvd, bool child = false)
{
	chdm_info info;
	CHECK_OK(chdm_get_info(file, &info));
	CHECK(info.version == 5);
	CHECK(info.logical_bytes == data.size());
	CHECK(info.unit_bytes == 2048);
	CHECK(info.compression[0] == codec);
	CHECK(info.compressed == (codec != 0));
	CHECK(info.has_parent == (child ? 1 : 0));
	CHECK(info.kind == (dvd ? CHDM_KIND_DVD : CHDM_KIND_UNKNOWN));

	// whole image through hunks
	std::vector<uint8_t> hunk(info.hunk_bytes);
	for (uint32_t h = 0; h < info.hunk_count; h++)
	{
		CHECK_OK(chdm_read_hunk(file, h, hunk.data()));
		std::size_t const off = std::size_t(h) * info.hunk_bytes;
		std::size_t const n = std::min<std::size_t>(info.hunk_bytes, data.size() - off);
		if (std::memcmp(hunk.data(), data.data() + off, n) != 0)
		{
			std::fprintf(stderr, "FAILED: hunk %u differs\n", h);
			++g_failures;
			break;
		}
	}

	// unaligned byte reads spanning hunks
	std::vector<uint8_t> buf(70000);
	CHECK_OK(chdm_read_bytes(file, 12345, buf.data(), uint32_t(buf.size())));
	CHECK(std::memcmp(buf.data(), data.data() + 12345, buf.size()) == 0);

	// unit (sector) reads
	CHECK_OK(chdm_read_units(file, 37, buf.data(), 3));
	CHECK(std::memcmp(buf.data(), data.data() + 37 * 2048, 3 * 2048) == 0);

	// metadata
	char meta[64] = { 0 };
	uint32_t len = 0, tag = 0;
	CHECK_OK(chdm_read_metadata(file, CHDM_TAG('T','E','S','T'), 0, meta, sizeof(meta), &len, &tag, nullptr));
	CHECK(len == 15 && std::strcmp(meta, "hello metadata") == 0);
	CHECK(tag == CHDM_TAG('T','E','S','T'));
	CHECK(chdm_read_metadata(file, CHDM_TAG('N','O','P','E'), 0, meta, sizeof(meta), nullptr, nullptr, nullptr) == CHDM_ERR_METADATA_NOT_FOUND);

	// errors
	CHECK(chdm_read_hunk(file, info.hunk_count, hunk.data()) == CHDM_ERR_HUNK_OUT_OF_RANGE);
	chdm_cd *cd = nullptr;
	CHECK(chdm_cd_open(file, &cd) == CHDM_ERR_UNSUPPORTED_FORMAT);
}



// ---------------------------------------------------------------------------
//  CD-ROM: a MODE1_RAW data track followed by an audio track, compressed
//  with the CD codecs exactly like "chdman createcd" does.
// ---------------------------------------------------------------------------

constexpr uint32_t DATA_FRAMES = 400;   // multiples of TRACK_PADDING
constexpr uint32_t AUDIO_FRAMES = 200;

std::vector<uint8_t> make_cd_frames()
{
	uint32_t const total = DATA_FRAMES + AUDIO_FRAMES;
	std::vector<uint8_t> frames(std::size_t(total) * cdrom_file::FRAME_SIZE, 0);
	std::mt19937 rng(777);
	for (uint32_t f = 0; f < total; f++)
	{
		uint8_t *const frame = &frames[std::size_t(f) * cdrom_file::FRAME_SIZE];
		if (f < DATA_FRAMES)
		{
			static const uint8_t sync[12] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
			std::memcpy(frame, sync, 12);
			frame[15] = 1; // mode 1
			for (int i = 0; i < 2048; i++)
				frame[16 + i] = (f & 1) ? uint8_t(rng()) : uint8_t("ISO9660 user data "[(f + i) % 18]);
		}
		else
		{
			for (int i = 0; i < 2352; i += 2)
			{
				int16_t const s = int16_t(12000.0 * std::sin(double(f * 1176 + i / 2) * 0.01));
				frame[i] = uint8_t(s >> 8);
				frame[i + 1] = uint8_t(s);
			}
		}
	}
	return frames;
}

bool create_cd_chd(std::string const &path, std::vector<uint8_t> const &frames)
{
	cdrom_file::toc toc{};
	toc.numtrks = 2;
	toc.numsessions = 1;
	auto &t0 = toc.tracks[0];
	t0.trktype = cdrom_file::CD_TRACK_MODE1_RAW;
	t0.subtype = cdrom_file::CD_SUB_NONE;
	t0.datasize = 2352;
	t0.frames = DATA_FRAMES;
	t0.pgtype = cdrom_file::CD_TRACK_MODE1;
	t0.pgsub = cdrom_file::CD_SUB_NONE;
	auto &t1 = toc.tracks[1];
	t1.trktype = cdrom_file::CD_TRACK_AUDIO;
	t1.subtype = cdrom_file::CD_SUB_NONE;
	t1.datasize = 2352;
	t1.frames = AUDIO_FRAMES;
	t1.pgtype = cdrom_file::CD_TRACK_MODE1;
	t1.pgsub = cdrom_file::CD_SUB_NONE;

	auto const holder = std::make_unique<memory_compressor>(frames);
	memory_compressor &chd = *holder;
	chd_codec_type const compression[4] = { CHD_CODEC_CD_LZMA, CHD_CODEC_CD_ZLIB, CHD_CODEC_CD_FLAC, CHD_CODEC_CD_ZSTD };
	if (chd.create(path, frames.size(), cdrom_file::FRAMES_PER_HUNK * cdrom_file::FRAME_SIZE, cdrom_file::FRAME_SIZE, compression))
		return false;
	if (cdrom_file::write_metadata(&chd, toc))
		return false;
	chd.compress_begin();
	double progress, ratio;
	std::error_condition err;
	while ((err = chd.compress_continue(progress, ratio)) == chd_file::error::WALKING_PARENT || err == chd_file::error::COMPRESSING)
	{
	}
	return !err;
}

void test_cd(std::string const &dir)
{
	std::printf("cd-rom     ... ");
	std::fflush(stdout);
	int const before = g_failures;
	std::string const path = dir + "/selftest_cd.chd";
	std::vector<uint8_t> const frames = make_cd_frames();
	if (!create_cd_chd(path, frames))
	{
		std::printf("create FAILED\n");
		++g_failures;
		return;
	}

	chdm_file *file = nullptr;
	CHECK_OK(chdm_open(path.c_str(), CHDM_OPEN_READ, nullptr, &file));
	if (!file)
		return;
	chdm_info info;
	CHECK_OK(chdm_get_info(file, &info));
	CHECK(info.kind == CHDM_KIND_CDROM);
	CHECK(info.compression[0] == CHDM_TAG('c','d','l','z'));

	chdm_cd *cd = nullptr;
	CHECK_OK(chdm_cd_open(file, &cd));
	if (cd)
	{
		CHECK(chdm_cd_track_count(cd) == 2);
		chdm_cd_track t0, t1;
		CHECK_OK(chdm_cd_get_track(cd, 0, &t0));
		CHECK_OK(chdm_cd_get_track(cd, 1, &t1));
		CHECK(t0.type == CHDM_CD_MODE1_RAW && t0.frames == DATA_FRAMES);
		CHECK(t1.type == CHDM_CD_AUDIO && t1.frames == AUDIO_FRAMES);
		CHECK(chdm_cd_track_for_lba(cd, t0.logical_start + 5) == 0);
		CHECK(chdm_cd_track_for_lba(cd, t1.logical_start + 5) == 1);

		uint8_t sector[2352];
		for (uint32_t f = 0; f < DATA_FRAMES; f++)
		{
			uint8_t const *const src = &frames[std::size_t(f) * cdrom_file::FRAME_SIZE];
			CHECK_OK(chdm_cd_read_sector(cd, t0.logical_start + f, sector, CHDM_CD_MODE1));
			if (std::memcmp(sector, src + 16, 2048) != 0)
			{
				std::fprintf(stderr, "FAILED: CD data sector %u differs\n", f);
				++g_failures;
				break;
			}
			CHECK_OK(chdm_cd_read_sector(cd, t0.logical_start + f, sector, CHDM_CD_MODE1_RAW));
			if (std::memcmp(sector, src, 16) != 0)
			{
				std::fprintf(stderr, "FAILED: CD raw header %u differs\n", f);
				++g_failures;
				break;
			}
		}
		for (uint32_t f = 0; f < AUDIO_FRAMES; f++)
		{
			uint8_t const *const src = &frames[std::size_t(DATA_FRAMES + f) * cdrom_file::FRAME_SIZE];
			CHECK_OK(chdm_cd_read_sector(cd, t1.logical_start + f, sector, CHDM_CD_AUDIO));
			if (std::memcmp(sector, src, 2352) != 0)
			{
				std::fprintf(stderr, "FAILED: CD audio sector %u differs\n", f);
				++g_failures;
				break;
			}
		}
		// asking for 2048-byte data from an audio track must fail
		CHECK(chdm_cd_read_sector(cd, t1.logical_start, sector, CHDM_CD_MODE1) != CHDM_OK);
		chdm_cd_close(cd);
	}
	chdm_close(file);
	// raw CD data isn't an image anyone can use
	CHECK(chdm_extract_raw(path.c_str(), (dir + "/selftest_cd.bin").c_str(), nullptr, nullptr) == CHDM_ERR_UNSUPPORTED_FORMAT);
	if (!g_keep)
		std::remove(path.c_str());
	std::printf(g_failures == before ? "ok\n" : "FAILED\n");
}


// ---------------------------------------------------------------------------
//  Parent / child CHDs
// ---------------------------------------------------------------------------

struct parent_lookup
{
	std::string path;
	int calls = 0;
};

chdm_file *find_parent(void *user, const uint8_t sha1[20])
{
	auto *lookup = static_cast<parent_lookup *>(user);
	lookup->calls++;
	chdm_file *parent = nullptr;
	if (chdm_open(lookup->path.c_str(), CHDM_OPEN_READ, nullptr, &parent) != CHDM_OK)
		return nullptr;
	chdm_info info;
	chdm_get_info(parent, &info);
	if (std::memcmp(info.sha1, sha1, 20) != 0)
	{
		chdm_close(parent);
		return nullptr;
	}
	return parent;
}

void test_parent(std::string const &dir, std::vector<uint8_t> const &parent_data)
{
	std::printf("parent     ... ");
	std::fflush(stdout);
	int const before = g_failures;
	std::string const parent_path = dir + "/selftest_parent.chd";
	std::string const child_path = dir + "/selftest_child.chd";
	std::string const other_path = dir + "/selftest_other.chd";

	// child = parent with a few modified sectors
	std::vector<uint8_t> child_data = parent_data;
	for (std::size_t i = 100000; i < 100000 + 5000; i++)
		child_data[i] ^= 0x5a;

	if (!create_chd(parent_path, parent_data, CHD_CODEC_ZLIB, true) || !create_chd(other_path, child_data, CHD_CODEC_ZLIB, true))
	{
		std::printf("create FAILED\n");
		++g_failures;
		return;
	}
	{
		chd_file parent;
		CHECK(!parent.open(parent_path));
		auto const holder = std::make_unique<memory_compressor>(child_data);
		memory_compressor &child = *holder;
		chd_codec_type const compression[4] = { CHD_CODEC_LZMA, CHD_CODEC_NONE, CHD_CODEC_NONE, CHD_CODEC_NONE };
		CHECK(!child.create(child_path, child_data.size(), 2048 * 8, compression, parent));
		CHECK(!child.write_metadata(DVD_METADATA_TAG, 0, std::string()));
		CHECK(!child.write_metadata(CHD_MAKE_TAG('T','E','S','T'), 0, std::string("hello metadata")));
		child.compress_begin();
		double progress, ratio;
		std::error_condition err;
		while ((err = child.compress_continue(progress, ratio)) == chd_file::error::WALKING_PARENT || err == chd_file::error::COMPRESSING)
		{
		}
		CHECK(!err);
	}

	// no parent supplied: opens, but hunks living in the parent can't be read
	chdm_file *file = nullptr;
	CHECK_OK(chdm_open(child_path.c_str(), CHDM_OPEN_READ, nullptr, &file));
	if (file)
	{
		chdm_info info;
		chdm_get_info(file, &info);
		CHECK(info.has_parent == 1 && info.parent_missing == 1);
		std::vector<uint8_t> hunk(info.hunk_bytes);
		CHECK(chdm_read_hunk(file, 0, hunk.data()) == CHDM_ERR_REQUIRES_PARENT);
		chdm_close(file);
		file = nullptr;
	}

	// wrong parent
	chdm_file *other = nullptr;
	CHECK_OK(chdm_open(other_path.c_str(), CHDM_OPEN_READ, nullptr, &other));
	CHECK(chdm_open(child_path.c_str(), CHDM_OPEN_READ, other, &file) == CHDM_ERR_INVALID_PARENT);
	chdm_close(other);

	// explicit parent
	chdm_file *parent = nullptr;
	CHECK_OK(chdm_open(parent_path.c_str(), CHDM_OPEN_READ, nullptr, &parent));
	CHECK_OK(chdm_open(child_path.c_str(), CHDM_OPEN_READ, parent, &file));
	if (file)
	{
		chdm_info info;
		chdm_get_info(file, &info);
		CHECK(info.has_parent == 1 && info.parent_missing == 0);
		verify(file, child_data, CHDM_TAG('l','z','m','a'), true, true);
		chdm_close(file);
	}
	chdm_close(parent);

	// resolver callback
	parent_lookup lookup{ parent_path };
	file = nullptr;
	CHECK_OK(chdm_open_ex(child_path.c_str(), CHDM_OPEN_READ, find_parent, &lookup, &file));
	CHECK(lookup.calls == 1);
	if (file)
	{
		verify(file, child_data, CHDM_TAG('l','z','m','a'), true, true);
		chdm_close(file);
	}

	if (!g_keep)
	{
		std::remove(parent_path.c_str());
		std::remove(child_path.c_str());
		std::remove(other_path.c_str());
	}
	std::printf(g_failures == before ? "ok\n" : "FAILED\n");
}

} // anonymous namespace


std::vector<uint8_t> read_file(std::string const &path)
{
	std::vector<uint8_t> bytes;
	if (FILE *f = std::fopen(path.c_str(), "rb"))
	{
		std::fseek(f, 0, SEEK_END);
		bytes.resize(std::size_t(std::ftell(f)));
		std::fseek(f, 0, SEEK_SET);
		if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
			bytes.clear();
		std::fclose(f);
	}
	return bytes;
}

bool file_exists(std::string const &path)
{
	if (FILE *f = std::fopen(path.c_str(), "rb"))
	{
		std::fclose(f);
		return true;
	}
	return false;
}

// chdm_create_dvd / chdm_extract_raw: ISO -> CHD -> ISO with both PSP-style hunk sizes.
void test_convert(std::string const &dir, std::vector<uint8_t> const &data)
{
	std::printf("convert    ... ");
	std::fflush(stdout);
	int const before = g_failures;

	std::string const iso = dir + "/selftest_convert.iso";
	std::string const chd = dir + "/selftest_convert.chd";
	std::string const back = dir + "/selftest_convert_back.iso";
	if (FILE *f = std::fopen(iso.c_str(), "wb"))
	{
		CHECK(std::fwrite(data.data(), 1, data.size(), f) == data.size());
		std::fclose(f);
	}

	uint32_t const zstd[4] = { CHDM_TAG('z','s','t','d'), 0, 0, 0 };
	struct { uint32_t hunk; uint32_t const *codecs; uint32_t first; } const cases[] = {
		{ 2048, nullptr, CHD_CODEC_LZMA },
		{ 4096, zstd,    CHD_CODEC_ZSTD },
	};
	for (auto const &c : cases)
	{
		double last = -1.0;
		auto const track = [] (void *user, double p) -> int { *static_cast<double *>(user) = p; return 0; };
		CHECK_OK(chdm_create_dvd(iso.c_str(), chd.c_str(), c.hunk, c.codecs, track, &last));
		CHECK(last == 1.0);

		chdm_file *file = nullptr;
		CHECK_OK(chdm_open(chd.c_str(), CHDM_OPEN_READ, nullptr, &file));
		if (file)
		{
			chdm_info info;
			CHECK_OK(chdm_get_info(file, &info));
			CHECK(info.kind == CHDM_KIND_DVD);
			CHECK(info.hunk_bytes == c.hunk && info.unit_bytes == 2048);
			CHECK(info.logical_bytes == data.size());
			CHECK(info.compression[0] == c.first);
			chdm_close(file);
		}

		last = -1.0;
		CHECK_OK(chdm_extract_raw(chd.c_str(), back.c_str(), track, &last));
		CHECK(last == 1.0);
		CHECK(read_file(back) == data);
		std::remove(back.c_str());
		std::remove(chd.c_str());
	}

	// cancelling removes the partial output
	auto const cancel = [] (void *, double) -> int { return 1; };
	CHECK(chdm_create_dvd(iso.c_str(), chd.c_str(), 2048, nullptr, cancel, nullptr) == CHDM_ERR_CANCELLED);
	CHECK(!file_exists(chd));
	CHECK_OK(chdm_create_dvd(iso.c_str(), chd.c_str(), 2048, zstd, nullptr, nullptr));
	CHECK(chdm_extract_raw(chd.c_str(), back.c_str(), cancel, nullptr) == CHDM_ERR_CANCELLED);
	CHECK(!file_exists(back));

	// bad input
	CHECK(chdm_create_dvd(iso.c_str(), chd.c_str(), 3000, nullptr, nullptr, nullptr) == CHDM_ERR_INVALID_ARGUMENT);
	CHECK(chdm_create_dvd((dir + "/does-not-exist.iso").c_str(), chd.c_str(), 2048, nullptr, nullptr, nullptr) == CHDM_ERR_NOT_FOUND);
	if (FILE *f = std::fopen(iso.c_str(), "ab"))
	{
		std::fputc(0, f);  // no longer a whole number of sectors
		std::fclose(f);
	}
	CHECK(chdm_create_dvd(iso.c_str(), back.c_str(), 2048, nullptr, nullptr, nullptr) == CHDM_ERR_INVALID_DATA);
	CHECK(!file_exists(back));

	std::printf(g_failures == before ? "ok\n" : "FAILED\n");
	std::remove(iso.c_str());
	std::remove(chd.c_str());
}

// chdm_create_dvd_io / chdm_extract_raw_io: the same ISO through memory, which must give
// the same bytes as going through files.
void test_convert_io(std::string const &dir, std::vector<uint8_t> const &data)
{
	std::printf("convert io ... ");
	std::fflush(stdout);
	int const before = g_failures;

	std::string const iso = dir + "/selftest_convert_io.iso";
	std::string const chd_file_path = dir + "/selftest_convert_io_file.chd";
	std::string const chd_mem_path = dir + "/selftest_convert_io_mem.chd";
	if (FILE *f = std::fopen(iso.c_str(), "wb"))
	{
		CHECK(std::fwrite(data.data(), 1, data.size(), f) == data.size());
		std::fclose(f);
	}

	struct source { std::vector<uint8_t> const *data; };
	source src { &data };
	chdm_io in {};
	in.user = &src;
	in.size = [] (void *user) -> uint64_t { return static_cast<source *>(user)->data->size(); };
	in.read_at = [] (void *user, uint64_t offset, void *buffer, size_t length) -> size_t
	{
		std::vector<uint8_t> const &d = *static_cast<source *>(user)->data;
		if (offset >= d.size())
			return 0;
		size_t const n = size_t(std::min<uint64_t>(length, d.size() - offset));
		std::memcpy(buffer, d.data() + offset, n);
		return n;
	};
	in.close = [] (void *) { std::abort(); };  // never called by the conversion

	uint32_t const zstd[4] = { CHDM_TAG('z','s','t','d'), 0, 0, 0 };
	CHECK_OK(chdm_create_dvd(iso.c_str(), chd_file_path.c_str(), 2048, zstd, nullptr, nullptr));
	CHECK_OK(chdm_create_dvd_io(&in, chd_mem_path.c_str(), 2048, zstd, nullptr, nullptr));
	CHECK(read_file(chd_file_path) == read_file(chd_mem_path));

	// chdm_create_dvd_to_output: the CHD itself into memory, byte for byte the same file
	struct chd_buffer { std::vector<uint8_t> bytes; };
	chd_buffer mem_chd;
	chdm_output o {};
	o.user = &mem_chd;
	o.size = [] (void *user) -> uint64_t { return static_cast<chd_buffer *>(user)->bytes.size(); };
	o.read_at = [] (void *user, uint64_t offset, void *buffer, size_t length) -> size_t
	{
		std::vector<uint8_t> const &b = static_cast<chd_buffer *>(user)->bytes;
		if (offset >= b.size())
			return 0;
		size_t const n = size_t(std::min<uint64_t>(length, b.size() - offset));
		std::memcpy(buffer, b.data() + offset, n);
		return n;
	};
	o.write_at = [] (void *user, uint64_t offset, const void *buffer, size_t length) -> size_t
	{
		std::vector<uint8_t> &b = static_cast<chd_buffer *>(user)->bytes;
		if (offset + length > b.size())
			b.resize(size_t(offset + length));
		std::memcpy(b.data() + offset, buffer, length);
		return length;
	};
	CHECK_OK(chdm_create_dvd_to_output(&in, &o, 2048, zstd, nullptr, nullptr));
	CHECK(mem_chd.bytes == read_file(chd_file_path));
	chd_buffer cancelled_chd;
	o.user = &cancelled_chd;
	CHECK(chdm_create_dvd_to_output(&in, &o, 2048, zstd, [] (void *, double) -> int { return 1; }, nullptr) == CHDM_ERR_CANCELLED);

	struct sink { std::vector<uint8_t> out; uint64_t expected = 0; bool refuse = false; };
	chdm_writer w {};
	sink dst;
	w.user = &dst;
	w.begin = [] (void *user, uint64_t total) -> int
	{
		sink &s = *static_cast<sink *>(user);
		if (s.refuse)
			return 1;
		s.expected = total;
		s.out.reserve(size_t(total));
		return 0;
	};
	w.write_at = [] (void *user, uint64_t offset, const void *buffer, size_t length) -> size_t
	{
		sink &s = *static_cast<sink *>(user);
		if (offset != s.out.size())
			return size_t(-1);
		auto const *p = static_cast<uint8_t const *>(buffer);
		s.out.insert(s.out.end(), p, p + length);
		return length;
	};
	double last = -1.0;
	auto const track = [] (void *user, double p) -> int { *static_cast<double *>(user) = p; return 0; };
	CHECK_OK(chdm_extract_raw_io(chd_mem_path.c_str(), &w, track, &last));
	CHECK(last == 1.0);
	CHECK(dst.expected == data.size());
	CHECK(dst.out == data);

	// begin can refuse, and cancelling stops it
	sink refuse;
	refuse.refuse = true;
	w.user = &refuse;
	CHECK(chdm_extract_raw_io(chd_mem_path.c_str(), &w, nullptr, nullptr) == CHDM_ERR_OUT_OF_MEMORY);
	sink cancelled;
	w.user = &cancelled;
	auto const cancel = [] (void *, double) -> int { return 1; };
	CHECK(chdm_extract_raw_io(chd_mem_path.c_str(), &w, cancel, nullptr) == CHDM_ERR_CANCELLED);
	CHECK(chdm_create_dvd_io(&in, chd_mem_path.c_str(), 2048, nullptr, cancel, nullptr) == CHDM_ERR_CANCELLED);
	CHECK(!file_exists(chd_mem_path));

	std::printf(g_failures == before ? "ok\n" : "FAILED\n");
	std::remove(iso.c_str());
	std::remove(chd_file_path.c_str());
	std::remove(chd_mem_path.c_str());
}

// ---- CD images (chdm_create_cd / chdm_extract_cd / chdm_verify_cd) ----------------------------

namespace cdimg {

std::mt19937 g_rng(1234);

std::vector<uint8_t> random_bytes(size_t n)
{
	std::vector<uint8_t> v(n);
	for (auto &b : v)
		b = uint8_t(g_rng() >> 7);
	return v;
}

// Raw sectors with a sync, an address and a mode byte (the data after it is noise: nothing checks EDC/ECC).
std::vector<uint8_t> raw_data_frames(uint32_t count, uint8_t mode, uint32_t lba)
{
	std::vector<uint8_t> v = random_bytes(size_t(count) * 2352);
	for (uint32_t f = 0; f < count; f++)
	{
		uint8_t *s = &v[size_t(f) * 2352];
		std::memset(s, 0xff, 12);
		s[0] = s[11] = 0;
		uint32_t const a = lba + f + 150;
		auto bcd = [](uint32_t x) { return uint8_t(((x / 10) << 4) | (x % 10)); };
		s[12] = bcd(a / 4500);
		s[13] = bcd((a / 75) % 60);
		s[14] = bcd(a % 75);
		s[15] = mode;
	}
	return v;
}

void put(std::string const &path, std::vector<uint8_t> const &data)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	CHECK(f != nullptr);
	if (f)
	{
		CHECK(data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size());
		std::fclose(f);
	}
}

void put(std::string const &path, std::string const &text) { put(path, std::vector<uint8_t>(text.begin(), text.end())); }

std::string get_text(std::string const &path)
{
	std::vector<uint8_t> const v = read_file(path);
	return std::string(v.begin(), v.end());
}

std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t> const *> parts)
{
	std::vector<uint8_t> v;
	for (auto *p : parts)
		v.insert(v.end(), p->begin(), p->end());
	return v;
}

std::string fresh_dir(std::string const &dir, char const *name)
{
	std::string const d = dir + "/" + name;
	std::error_code ec;
	std::filesystem::remove_all(d, ec);
	std::filesystem::create_directories(d, ec);
	return d;
}

void le16(std::vector<uint8_t> &v, size_t o, uint32_t x) { v[o] = uint8_t(x); v[o + 1] = uint8_t(x >> 8); }
void le32(std::vector<uint8_t> &v, size_t o, uint32_t x) { le16(v, o, x & 0xffff); le16(v, o + 2, x >> 16); }

} // namespace cdimg

void test_cd_images(std::string const &dir)
{
	using namespace cdimg;
	std::printf("cd images  ... ");
	std::fflush(stdout);
	int const before = g_failures;
	static uint32_t const cd_codecs[4] = { CHD_CODEC_CD_LZMA, CHD_CODEC_CD_ZLIB, CHD_CODEC_CD_FLAC, CHD_CODEC_NONE };

	// --- a Redump-style split cue: data, audio with its pregap in the file, audio with a PREGAP
	std::vector<uint8_t> const t1 = raw_data_frames(30, 1, 0);
	std::vector<uint8_t> const t2 = random_bytes(40 * 2352);
	std::vector<uint8_t> const t3 = random_bytes(23 * 2352);
	std::string const cue_text =
		"FILE \"Game (Track 1).bin\" BINARY\n"
		"  TRACK 01 MODE1/2352\n"
		"    INDEX 01 00:00:00\n"
		"FILE \"Game (Track 2).bin\" BINARY\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 00 00:00:00\n"
		"    INDEX 01 00:00:10\n"
		"FILE \"Game (Track 3).bin\" BINARY\n"
		"  TRACK 03 AUDIO\n"
		"    PREGAP 00:00:05\n"
		"    INDEX 01 00:00:00\n";
	std::string const a = fresh_dir(dir, "selftest_cue");
	put(a + "/Game (Track 1).bin", t1);
	put(a + "/Game (Track 2).bin", t2);
	put(a + "/Game (Track 3).bin", t3);
	put(a + "/Game.cue", cue_text);
	std::string const chd = a + "/Game.chd";

	chdm_cd_image_info info;
	CHECK_OK(chdm_cd_inspect((a + "/Game.cue").c_str(), &info));
	CHECK(info.tracks == 3 && info.data_tracks == 1 && info.audio_tracks == 2 && info.files == 4);
	CHECK(info.frames == 93 && info.data_bytes == 93u * 2352);
	CHECK(std::strcmp(info.format, "cue") == 0 && std::strcmp(info.first_data, "MODE1/2352") == 0);
	CHECK(info.chd_bytes == (32u + 40u + 24u) * 2448u);
	// a track file on its own is read through the cue sheet that names it
	CHECK_OK(chdm_cd_inspect((a + "/Game (Track 2).bin").c_str(), &info));
	CHECK(info.tracks == 3);
	int nfiles = 0;
	CHECK_OK(chdm_cd_image_files((a + "/Game.cue").c_str(), [](void *u, const char *) { ++*static_cast<int *>(u); }, &nfiles));
	CHECK(nfiles == 4);

	double last = -1;
	auto track = [](void *u, double p) -> int { *static_cast<double *>(u) = p; return 0; };
	CHECK_OK(chdm_create_cd((a + "/Game.cue").c_str(), chd.c_str(), 0, cd_codecs, track, &last));
	CHECK(last == 1.0);
	CHECK_OK(chdm_verify_cd((a + "/Game.cue").c_str(), chd.c_str(), nullptr, nullptr));
	CHECK_OK(chdm_cd_inspect(chd.c_str(), &info));
	CHECK(info.tracks == 3 && info.audio_tracks == 2 && std::strcmp(info.format, "chd") == 0);

	// read back through the CD reader: audio comes out as it was in the .bin
	{
		chdm_file *file = nullptr;
		CHECK_OK(chdm_open(chd.c_str(), CHDM_OPEN_READ, nullptr, &file));
		chdm_info ci;
		if (file && chdm_get_info(file, &ci) == CHDM_OK)
			CHECK(ci.kind == CHDM_KIND_CDROM && ci.hunk_bytes == CHDM_CD_HUNK_BYTES_DEFAULT);
		if (file)
			chdm_close(file);
	}

	// split extract gives back the same files and the same cue sheet
	std::string const out = fresh_dir(dir, "selftest_cue_out");
	last = -1;
	CHECK_OK(chdm_extract_cd(chd.c_str(), (out + "/Game.cue").c_str(), 1, track, &last));
	CHECK(last == 1.0);
	CHECK(get_text(out + "/Game.cue") == cue_text);
	CHECK(read_file(out + "/Game (Track 1).bin") == t1);
	CHECK(read_file(out + "/Game (Track 2).bin") == t2);
	CHECK(read_file(out + "/Game (Track 3).bin") == t3);
	CHECK_OK(chdm_verify_cd((out + "/Game.cue").c_str(), chd.c_str(), nullptr, nullptr));

	// one .bin
	std::string const one = fresh_dir(dir, "selftest_cue_one");
	CHECK_OK(chdm_extract_cd(chd.c_str(), (one + "/Game.cue").c_str(), 0, nullptr, nullptr));
	std::vector<uint8_t> const joined = cat({ &t1, &t2, &t3 });
	CHECK(read_file(one + "/Game.bin") == joined);
	std::string const one_text = get_text(one + "/Game.cue");
	CHECK(one_text ==
		"FILE \"Game.bin\" BINARY\n"
		"  TRACK 01 MODE1/2352\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 00 00:00:30\n"
		"    INDEX 01 00:00:40\n"
		"  TRACK 03 AUDIO\n"
		"    PREGAP 00:00:05\n"
		"    INDEX 01 00:00:70\n");
	CHECK_OK(chdm_verify_cd((one + "/Game.cue").c_str(), chd.c_str(), nullptr, nullptr));

	// a renamed image: the cue's FILE names a .bin that isn't there, the one with the cue's name is used
	std::string const ren = fresh_dir(dir, "selftest_cue_renamed");
	put(ren + "/Renamed.bin", joined);
	std::string renamed = one_text;
	renamed.replace(renamed.find("Game.bin"), 8, "Game (USA).bin");
	put(ren + "/Renamed.cue", renamed);
	CHECK_OK(chdm_verify_cd((ren + "/Renamed.cue").c_str(), chd.c_str(), nullptr, nullptr));
	CHECK(std::filesystem::directory_iterator(ren) != std::filesystem::directory_iterator());
	{
		int count = 0;
		for (auto const &e : std::filesystem::directory_iterator(ren)) { (void)e; count++; }
		CHECK(count == 2);  // the corrected copy of the cue sheet is gone again
	}

	// a different image doesn't verify; a cancelled extract leaves nothing
	std::vector<uint8_t> t3b = t3;
	t3b[5000] ^= 1;
	put(out + "/Game (Track 3).bin", t3b);
	CHECK(chdm_verify_cd((out + "/Game.cue").c_str(), chd.c_str(), nullptr, nullptr) == CHDM_ERR_INVALID_DATA);
	std::string const can = fresh_dir(dir, "selftest_cue_cancel");
	CHECK(chdm_extract_cd(chd.c_str(), (can + "/Game.cue").c_str(), 1, [](void *, double) -> int { return 1; }, nullptr) == CHDM_ERR_CANCELLED);
	CHECK(std::filesystem::is_empty(can));
	CHECK(chdm_create_cd((a + "/Game.cue").c_str(), (can + "/x.chd").c_str(), 0, nullptr, [](void *, double) -> int { return 1; }, nullptr) == CHDM_ERR_CANCELLED);
	CHECK(std::filesystem::is_empty(can));
	CHECK(chdm_create_cd((a + "/Game.cue").c_str(), (can + "/x.chd").c_str(), 4096, nullptr, nullptr, nullptr) == CHDM_ERR_INVALID_ARGUMENT);

	// --- CloneCD: one .img, INDEX 0/1 pregap in the file, a .sub that isn't stored
	std::string const c = fresh_dir(dir, "selftest_ccd");
	std::vector<uint8_t> const img = cat({ &t1, &t2, &t3 });
	put(c + "/Disc.img", img);
	put(c + "/Disc.sub", random_bytes(93 * 96));
	put(c + "/Disc.ccd",
		"[CloneCD]\r\nVersion=3\r\n[Disc]\r\nTocEntries=6\r\nSessions=1\r\nDataTracksScrambled=0\r\n"
		"[Session 1]\r\nPreGapMode=1\r\nPreGapSubC=0\r\n"
		"[Entry 0]\r\nSession=1\r\nPoint=0x01\r\nADR=0x01\r\nControl=0x04\r\nPLBA=0\r\n"
		"[Entry 1]\r\nSession=1\r\nPoint=0x02\r\nADR=0x01\r\nControl=0x00\r\nPLBA=40\r\n"
		"[Entry 2]\r\nSession=1\r\nPoint=0x03\r\nADR=0x01\r\nControl=0x00\r\nPLBA=70\r\n"
		"[TRACK 1]\r\nMODE=1\r\nINDEX 1=0\r\n"
		"[TRACK 2]\r\nMODE=0\r\nINDEX 0=30\r\nINDEX 1=40\r\n"
		"[TRACK 3]\r\nMODE=0\r\nINDEX 1=70\r\n");
	CHECK_OK(chdm_cd_inspect((c + "/Disc.ccd").c_str(), &info));
	CHECK(info.tracks == 3 && info.audio_tracks == 2 && info.files == 3 && info.frames == 93 && std::strcmp(info.format, "ccd") == 0);
	CHECK_OK(chdm_cd_inspect((c + "/Disc.img").c_str(), &info));  // through the .ccd of the same name
	CHECK(info.tracks == 3);
	CHECK_OK(chdm_create_cd((c + "/Disc.img").c_str(), (c + "/Disc.chd").c_str(), 0, nullptr, nullptr, nullptr));
	CHECK_OK(chdm_verify_cd((c + "/Disc.ccd").c_str(), (c + "/Disc.chd").c_str(), nullptr, nullptr));
	CHECK_OK(chdm_extract_cd((c + "/Disc.chd").c_str(), (c + "/Back.cue").c_str(), 0, nullptr, nullptr));
	CHECK(read_file(c + "/Back.bin") == img);
	CHECK(get_text(c + "/Back.cue") ==
		"FILE \"Back.bin\" BINARY\n"
		"  TRACK 01 MODE1/2352\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 00 00:00:30\n"
		"    INDEX 01 00:00:40\n"
		"  TRACK 03 AUDIO\n"
		"    INDEX 01 00:00:70\n");

	// --- Alcohol 120%: 2448-byte sectors (subchannel skipped), a gap between the tracks
	std::string const m = fresh_dir(dir, "selftest_mds");
	std::vector<uint8_t> mdf;
	for (uint32_t f = 0; f < 30; f++)
	{
		mdf.insert(mdf.end(), t1.begin() + f * 2352, t1.begin() + (f + 1) * 2352);
		std::vector<uint8_t> const sub = random_bytes(96);
		mdf.insert(mdf.end(), sub.begin(), sub.end());
	}
	for (uint32_t f = 0; f < 40; f++)
	{
		mdf.insert(mdf.end(), t2.begin() + f * 2352, t2.begin() + (f + 1) * 2352);
		std::vector<uint8_t> const sub = random_bytes(96);
		mdf.insert(mdf.end(), sub.begin(), sub.end());
	}
	put(m + "/Alc.mdf", mdf);
	{
		size_t const sess = 0x58, tracks = sess + 24, nblocks = 3, extra = tracks + nblocks * 80, footer = extra + 2 * 8, name = footer + 16;
		std::vector<uint8_t> d(name + 6, 0);
		std::memcpy(d.data(), "MEDIA DESCRIPTOR", 16);
		d[0x10] = 1; d[0x11] = 5;
		le16(d, 0x12, 0);          // CD-ROM
		le16(d, 0x14, 1);          // sessions
		le32(d, 0x50, uint32_t(sess));
		le32(d, sess + 0, uint32_t(-150));
		le32(d, sess + 4, 30 + 12 + 40);
		le16(d, sess + 8, 1);
		d[sess + 10] = uint8_t(nblocks);
		d[sess + 11] = 1;
		le16(d, sess + 12, 1);
		le16(d, sess + 14, 2);
		le32(d, sess + 20, uint32_t(tracks));
		// leadout
		uint8_t *lo = &d[tracks];
		lo[2] = 0x14; lo[4] = 0xa2;
		uint32_t const end = 30 + 12 + 40 + 150;
		lo[9] = uint8_t(end / 4500); lo[10] = uint8_t((end / 75) % 60); lo[11] = uint8_t(end % 75);
		struct { uint8_t mode, ctl; uint32_t start, len; uint64_t ofs; } const tr[2] = {
			{ 0xaa, 0x14, 0, 30, 0 },
			{ 0xa9, 0x10, 42, 40, 30 * 2448 },   // 12 sectors after track 1 that aren't in the file
		};
		for (int i = 0; i < 2; i++)
		{
			size_t const b = tracks + (i + 1) * 80;
			d[b + 0] = tr[i].mode; d[b + 1] = 0x08; d[b + 2] = tr[i].ctl; d[b + 4] = uint8_t(i + 1);
			le32(d, b + 12, uint32_t(extra + i * 8));
			le16(d, b + 16, 2448);
			le32(d, b + 36, tr[i].start);
			le32(d, b + 40, uint32_t(tr[i].ofs));
			le32(d, b + 48, 1);
			le32(d, b + 52, uint32_t(footer));
			le32(d, extra + i * 8 + 0, i == 0 ? 150 : 0);
			le32(d, extra + i * 8 + 4, tr[i].len);
		}
		le32(d, footer, uint32_t(name));
		std::memcpy(&d[name], "*.mdf", 5);
		put(m + "/Alc.mds", d);
	}
	CHECK_OK(chdm_cd_inspect((m + "/Alc.mdf").c_str(), &info));
	CHECK(info.tracks == 2 && info.audio_tracks == 1 && info.frames == 70 && info.files == 2 && std::strcmp(info.format, "mds") == 0);
	CHECK_OK(chdm_create_cd((m + "/Alc.mds").c_str(), (m + "/Alc.chd").c_str(), 0, nullptr, nullptr, nullptr));
	CHECK_OK(chdm_verify_cd((m + "/Alc.mds").c_str(), (m + "/Alc.chd").c_str(), nullptr, nullptr));
	CHECK_OK(chdm_extract_cd((m + "/Alc.chd").c_str(), (m + "/Alc.cue").c_str(), 0, nullptr, nullptr));
	CHECK(read_file(m + "/Alc.bin") == cat({ &t1, &t2 }));
	CHECK(get_text(m + "/Alc.cue") ==
		"FILE \"Alc.bin\" BINARY\n"
		"  TRACK 01 MODE1/2352\n"
		"    INDEX 01 00:00:00\n"
		"  TRACK 02 AUDIO\n"
		"    PREGAP 00:00:12\n"
		"    INDEX 01 00:00:30\n");
	CHECK_OK(chdm_verify_cd((m + "/Alc.cue").c_str(), (m + "/Alc.chd").c_str(), nullptr, nullptr));

	// --- bare images: raw 2352, 2048-byte ISO, and a MODE2/2336 cue
	std::string const b = fresh_dir(dir, "selftest_bare");
	std::vector<uint8_t> const raw2 = raw_data_frames(50, 2, 0);
	put(b + "/Raw.bin", raw2);
	CHECK_OK(chdm_cd_inspect((b + "/Raw.bin").c_str(), &info));
	CHECK(info.tracks == 1 && std::strcmp(info.first_data, "MODE2/2352") == 0 && std::strcmp(info.format, "bin") == 0);
	CHECK_OK(chdm_create_cd((b + "/Raw.bin").c_str(), (b + "/Raw.chd").c_str(), 0, nullptr, nullptr, nullptr));
	CHECK_OK(chdm_extract_cd((b + "/Raw.chd").c_str(), (b + "/RawBack.cue").c_str(), 1, nullptr, nullptr));
	CHECK(read_file(b + "/RawBack (Track 1).bin") == raw2);
	CHECK(chdm_verify_cd((b + "/Raw.bin").c_str(), chd.c_str(), nullptr, nullptr) == CHDM_ERR_INVALID_DATA);

	std::vector<uint8_t> const iso = random_bytes(21 * 2048);
	put(b + "/Plain.iso", iso);
	CHECK_OK(chdm_cd_inspect((b + "/Plain.iso").c_str(), &info));
	CHECK(info.tracks == 1 && std::strcmp(info.first_data, "MODE1/2048") == 0);
	CHECK_OK(chdm_create_cd((b + "/Plain.iso").c_str(), (b + "/Plain.chd").c_str(), 0, nullptr, nullptr, nullptr));
	CHECK_OK(chdm_verify_cd((b + "/Plain.iso").c_str(), (b + "/Plain.chd").c_str(), nullptr, nullptr));
	CHECK_OK(chdm_extract_cd((b + "/Plain.chd").c_str(), (b + "/PlainBack.cue").c_str(), 0, nullptr, nullptr));
	CHECK(read_file(b + "/PlainBack.bin") == iso);

	std::vector<uint8_t> const m2 = random_bytes(17 * 2336);
	put(b + "/Xa.bin", m2);
	put(b + "/Xa.cue", "FILE \"Xa.bin\" BINARY\n  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n");
	CHECK_OK(chdm_create_cd((b + "/Xa.cue").c_str(), (b + "/Xa.chd").c_str(), 0, nullptr, nullptr, nullptr));
	CHECK_OK(chdm_extract_cd((b + "/Xa.chd").c_str(), (b + "/XaBack.cue").c_str(), 0, nullptr, nullptr));
	CHECK(read_file(b + "/XaBack.bin") == m2);
	CHECK(get_text(b + "/XaBack.cue") == "FILE \"XaBack.bin\" BINARY\n  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n");

	// a DVD CHD isn't a CD
	put(b + "/Odd.bin", random_bytes(1000));
	CHECK(chdm_cd_inspect((b + "/Odd.bin").c_str(), &info) == CHDM_ERR_UNSUPPORTED_FORMAT);
	CHECK(chdm_cd_inspect((b + "/missing.cue").c_str(), &info) == CHDM_ERR_NOT_FOUND);

	std::printf(g_failures == before ? "ok\n" : "FAILED\n");
	if (!g_keep)
		for (char const *d : { "selftest_cue", "selftest_cue_out", "selftest_cue_one", "selftest_cue_renamed", "selftest_cue_cancel", "selftest_ccd", "selftest_mds", "selftest_bare" })
		{
			std::error_code ec;
			std::filesystem::remove_all(dir + "/" + d, ec);
		}
}

int main(int argc, char *argv[])
{
	std::string const dir = (argc > 1) ? argv[1] : ".";
	g_keep = (argc > 2) && !std::strcmp(argv[2], "--keep");
	chdm_set_log_callback([] (void *, int level, const char *msg) { std::fprintf(stderr, "[chd log %d] %s", level, msg); }, nullptr);
	std::vector<uint8_t> const data = make_test_data(1024 * 1024 + 2048 * 5); // not a hunk multiple

	struct { chd_codec_type codec; bool dvd; } const cases[] = {
		{ CHD_CODEC_NONE,    false },
		{ CHD_CODEC_ZLIB,    true  },
		{ CHD_CODEC_ZSTD,    true  },
		{ CHD_CODEC_LZMA,    true  },
		{ CHD_CODEC_HUFFMAN, false },
		{ CHD_CODEC_FLAC,    false },
	};

	for (auto const &c : cases)
	{
		char name[5];
		chdm_codec_name(c.codec, name);
		std::string const path = dir + "/selftest_" + name + ".chd";
		std::printf("codec %-4s ... ", name);
		std::fflush(stdout);

		if (!create_chd(path, data, c.codec, c.dvd))
		{
			std::printf("create FAILED\n");
			++g_failures;
			continue;
		}

		int const before = g_failures;

		// by path
		chdm_file *file = nullptr;
		CHECK_OK(chdm_open(path.c_str(), CHDM_OPEN_READ, nullptr, &file));
		if (file)
		{
			verify(file, data, c.codec, c.dvd);
			chdm_close(file);
		}

		// through custom I/O
		mem_io mem;
		if (FILE *f = std::fopen(path.c_str(), "rb"))
		{
			std::fseek(f, 0, SEEK_END);
			mem.bytes.resize(std::size_t(std::ftell(f)));
			std::fseek(f, 0, SEEK_SET);
			CHECK(std::fread(mem.bytes.data(), 1, mem.bytes.size(), f) == mem.bytes.size());
			std::fclose(f);
		}
		chdm_io const io = { &mem, mem_size, mem_read, mem_close };
		file = nullptr;
		CHECK_OK(chdm_open_io(&io, nullptr, &file));
		if (file)
		{
			verify(file, data, c.codec, c.dvd);
			chdm_close(file);
		}
		CHECK(mem.closed);

		std::printf(g_failures == before ? "ok\n" : "FAILED\n");
		if (!g_keep)
			std::remove(path.c_str());
	}

	test_cd(dir);
	test_parent(dir, data);
	test_convert(dir, data);
	test_convert_io(dir, data);
	test_cd_images(dir);

	// error paths
	chdm_file *file = nullptr;
	CHECK(chdm_open((dir + "/does-not-exist.chd").c_str(), CHDM_OPEN_READ, nullptr, &file) == CHDM_ERR_NOT_FOUND);
	CHECK(file == nullptr);

	mem_io junk;
	junk.bytes.assign(4096, 0x55);
	chdm_io const junk_io = { &junk, mem_size, mem_read, mem_close };
	CHECK(chdm_open_io(&junk_io, nullptr, &file) == CHDM_ERR_INVALID_FILE);
	CHECK(!junk.closed); // ownership stays with the caller on failure

	std::printf(g_failures ? "%d check(s) FAILED\n" : "all tests passed\n", g_failures);
	return g_failures ? 1 : 0;
}

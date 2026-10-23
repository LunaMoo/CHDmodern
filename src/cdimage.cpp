// license:BSD-3-Clause
// copyright-holders:CHDmodern contributors
/***************************************************************************

    cdimage.cpp

    CD images to and from CD CHDs: chdm_create_cd, chdm_extract_cd,
    chdm_verify_cd and friends.

    The cue / gdi / nrg / cdrdao parsers and the CHD side are MAME's
    (cdrom.cpp, chd.cpp); the compressor and the cue writer follow chdman's
    createcd / extractcd.  CloneCD (.ccd), Alcohol (.mds) and bare raw
    images are read here into the same track tables.

***************************************************************************/

#include "chdmodern/chdmodern.h"
#include "internal.h"

#include "cdrom.h"
#include "chd.h"
#include "corefile.h"
#include "osdcore.h"
#include "strformat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

using chdm_internal::set_error;
using chdm_internal::translate;
using chdm_internal::current_exception_error;

namespace {

constexpr uint32_t FRAME_SIZE = cdrom_file::FRAME_SIZE;

//**************************************************************************
//  PATHS AND FILES
//**************************************************************************

// UTF-8 paths, split by hand: std::filesystem isn't there on every target (iOS before 13).
#ifdef _WIN32
constexpr char PATH_SEP = '\\';
bool is_sep(char c) { return c == '/' || c == '\\'; }
std::wstring widen(std::string const &s)
{
	if (s.empty())
		return std::wstring();
	int const n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
	return w;
}
std::string narrow(wchar_t const *w)
{
	int const n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1)
		return std::string();
	std::string s(size_t(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	return s;
}
#else
constexpr char PATH_SEP = '/';
bool is_sep(char c) { return c == '/'; }
#endif

struct path_t
{
	std::string s;
	path_t() = default;
	path_t(std::string v) : s(std::move(v)) { }
	bool empty() const { return s.empty(); }
	size_t name_pos() const
	{
		size_t i = s.size();
		while (i > 0 && !is_sep(s[i - 1]))
			i--;
		return i;
	}
	path_t parent_path() const
	{
		size_t i = name_pos();
		if (i == 0)
			return path_t();
		size_t j = i - 1;
		while (j > 0 && is_sep(s[j - 1]))
			j--;
		return path_t(s.substr(0, j == 0 ? 1 : j));
	}
	path_t filename() const { return path_t(s.substr(name_pos())); }
	path_t stem() const
	{
		std::string f = s.substr(name_pos());
		size_t const dot = f.rfind('.');
		return path_t(dot == std::string::npos || dot == 0 ? f : f.substr(0, dot));
	}
	path_t extension() const
	{
		std::string f = s.substr(name_pos());
		size_t const dot = f.rfind('.');
		return path_t(dot == std::string::npos || dot == 0 ? std::string() : f.substr(dot));
	}
	path_t operator/(path_t const &o) const
	{
		if (s.empty())
			return o;
		return path_t(is_sep(s.back()) ? s + o.s : s + PATH_SEP + o.s);
	}
};

path_t to_path(std::string const &utf8) { return path_t(utf8); }
std::string to_utf8(path_t const &p) { return p.s; }

std::string lower(std::string s)
{
	for (char &c : s)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string extension_of(std::string const &path)
{
	std::string ext = to_utf8(to_path(path).extension());
	if (!ext.empty() && ext[0] == '.')
		ext.erase(0, 1);
	return lower(ext);
}

bool file_exists(path_t const &p)
{
	if (p.empty())
		return false;
#ifdef _WIN32
	DWORD const a = GetFileAttributesW(widen(p.s).c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(p.s.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

uint64_t file_size_of(std::string const &path)
{
	osd_file::ptr file;
	uint64_t size = 0;
	if (osd_file::open(path, OPEN_FLAG_READ, file, size))
		return 0;
	return size;
}

// The names of the files (not folders) in dir.
std::vector<std::string> list_files(path_t const &dir)
{
	std::vector<std::string> out;
	std::string const d = dir.empty() ? std::string(".") : dir.s;
#ifdef _WIN32
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(widen(to_utf8(path_t(d) / path_t("*"))).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			out.push_back(narrow(fd.cFileName));
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	if (DIR *dp = opendir(d.c_str()))
	{
		while (dirent *e = readdir(dp))
		{
			std::string const name = e->d_name;
			if (name != "." && name != ".." && file_exists(path_t(d) / path_t(name)))
				out.push_back(name);
		}
		closedir(dp);
	}
#endif
	std::sort(out.begin(), out.end());
	return out;
}

// A file in dir whose name matches name ignoring case (for cue sheets written on Windows).
path_t find_case_insensitive(path_t const &dir, std::string const &name)
{
	if (file_exists(dir / to_path(name)))
		return dir / to_path(name);
	std::string const want = lower(name);
	for (std::string const &f : list_files(dir))
		if (lower(f) == want)
			return dir / path_t(f);
	return path_t();
}

bool read_whole_file(std::string const &path, std::vector<uint8_t> &out, uint64_t limit)
{
	util::core_file::ptr file;
	if (util::core_file::open(path, OPEN_FLAG_READ, file))
		return false;
	uint64_t size = 0;
	if (file->length(size) || size > limit)
		return false;
	out.resize(size_t(size));
	size_t done = 0;
	while (done < out.size())
	{
		size_t actual = 0;
		if (file->read_some_at(done, out.data() + done, out.size() - done, actual) || actual == 0)
			return false;
		done += actual;
	}
	return true;
}

bool read_at(std::string const &path, uint64_t offset, void *buf, size_t len)
{
	util::core_file::ptr file;
	if (util::core_file::open(path, OPEN_FLAG_READ, file))
		return false;
	size_t done = 0;
	while (done < len)
	{
		size_t actual = 0;
		if (file->read_some_at(offset + done, static_cast<uint8_t *>(buf) + done, len - done, actual) || actual == 0)
			return false;
		done += actual;
	}
	return true;
}

bool write_all(util::core_file &file, uint64_t offset, void const *data, size_t length)
{
	uint8_t const *p = static_cast<uint8_t const *>(data);
	while (length > 0)
	{
		size_t actual = 0;
		if (file.write_some_at(offset, p, length, actual) || actual == 0)
			return false;
		offset += actual;
		p += actual;
		length -= actual;
	}
	return true;
}

std::vector<std::string> text_lines(std::vector<uint8_t> const &data)
{
	std::vector<std::string> lines;
	std::string cur;
	size_t start = 0;
	if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
		start = 3;  // UTF-8 BOM
	for (size_t i = start; i < data.size(); i++)
	{
		char const c = char(data[i]);
		if (c == '\n' || c == '\r')
		{
			if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n')
				i++;
			lines.push_back(cur);
			cur.clear();
		}
		else
			cur += c;
	}
	if (!cur.empty())
		lines.push_back(cur);
	return lines;
}

std::string trim(std::string const &s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

// The file name on a cue sheet FILE line ("" when line isn't one), and where it starts and ends.
bool cue_file_name(std::string const &line, std::string &name, size_t &begin, size_t &end)
{
	size_t i = 0;
	while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
		i++;
	if (line.compare(i, 4, "FILE") != 0 || i + 4 >= line.size() || !std::isspace(static_cast<unsigned char>(line[i + 4])))
		return false;
	i += 4;
	while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
		i++;
	if (i >= line.size())
		return false;
	if (line[i] == '"')
	{
		size_t const close = line.find('"', i + 1);
		if (close == std::string::npos)
			return false;
		begin = i;
		end = close + 1;
		name = line.substr(i + 1, close - i - 1);
	}
	else
	{
		size_t j = i;
		while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j])))
			j++;
		begin = i;
		end = j;
		name = line.substr(i, j - i);
	}
	return true;
}

//**************************************************************************
//  CD SOURCES
//**************************************************************************

// A CD image read into MAME's track tables, plus what chdman's reader doesn't know: how many
// bytes each frame takes in the file (Alcohol images can carry 96 bytes of subchannel data per
// sector, which isn't stored).
struct cd_source
{
	cdrom_file::toc toc;
	cdrom_file::track_input_info info;
	uint32_t stride[cdrom_file::MAX_TRACKS] = {};
	std::string format;
	std::vector<std::string> files;  // descriptor first
	std::unique_ptr<chd_file> chd;    // a CHD as the source
	uint64_t chd_bytes = 0;           // logical size of the CHD this makes

	cd_source() { std::memset(&toc, 0, sizeof(toc)); info.reset(); }
};

void add_file(cd_source &src, std::string const &path)
{
	if (!path.empty() && std::find(src.files.begin(), src.files.end(), path) == src.files.end())
		src.files.push_back(path);
}

void set_raw_track(cdrom_file::track_info &t, uint32_t type)
{
	t.trktype = type;
	t.subtype = cdrom_file::CD_SUB_NONE;
	t.subsize = 0;
	t.pgsub = cdrom_file::CD_SUB_NONE;
	t.pgsubsize = 0;
	t.datasize = 2352;
}

// MODE1/2352 or MODE2/2352 from a raw sector's sync and mode bytes, or -1.
int raw_sector_type(uint8_t const *s)
{
	static uint8_t const sync[12] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	if (std::memcmp(s, sync, 12) != 0)
		return -1;
	if (s[15] == 1)
		return cdrom_file::CD_TRACK_MODE1_RAW;
	if (s[15] == 2)
		return cdrom_file::CD_TRACK_MODE2_RAW;
	return -1;
}

int error_message(int code, std::string const &msg)
{
	return set_error(code, msg);
}

// ---- cue sheets ------------------------------------------------------------------------------

// MAME's parser, after replacing FILE names that don't exist (a renamed image: "Game.cue" saying
// FILE "Game (USA).bin") with the cue sheet's own name.
int load_cue(std::string const &path, cd_source &src)
{
	std::vector<uint8_t> raw;
	if (!read_whole_file(path, raw, 1 << 20))
		return error_message(CHDM_ERR_IO, "can't read " + path);
	path_t const cue = to_path(path);
	path_t const dir = cue.parent_path();
	std::string const stem = to_utf8(cue.stem());

	std::vector<std::string> lines = text_lines(raw);
	bool changed = false;
	for (std::string &line : lines)
	{
		std::string name;
		size_t b, e;
		if (!cue_file_name(line, name, b, e))
			continue;
		if (file_exists(dir / to_path(name)))
			continue;
		path_t found = find_case_insensitive(dir, name);
		if (found.empty())
		{
			std::string ext = to_utf8(to_path(name).extension());
			for (std::string const &cand : { stem + ext, stem + ".bin", stem + ".img" })
			{
				found = find_case_insensitive(dir, cand);
				if (!found.empty())
					break;
			}
		}
		if (found.empty())
			return error_message(CHDM_ERR_NOT_FOUND, "the cue sheet names " + name + ", which doesn't exist (nor " + stem + ".bin)");
		line = line.substr(0, b) + "\"" + to_utf8(found.filename()) + "\"" + line.substr(e);
		changed = true;
	}

	std::string parse_path = path;
	std::string temp;
	if (changed)
	{
		// MAME's parser takes a path, and resolves FILE names next to it: write the fixed sheet there.
		std::random_device rd;
		temp = to_utf8(dir / to_path(util::string_format(".chdm-%08x-%s.cue", rd(), stem)));
		util::core_file::ptr out;
		std::string text;
		for (std::string const &l : lines)
			text += l + "\n";
		if (util::core_file::open(temp, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, out) || !write_all(*out, 0, text.data(), text.size()))
			return error_message(CHDM_ERR_ACCESS_DENIED, "the cue sheet names files that don't exist, and a corrected copy can't be written next to it");
		out.reset();
		parse_path = temp;
	}
	std::error_condition const err = cdrom_file::parse_cue(parse_path, src.toc, src.info);
	if (!temp.empty())
		osd_file::remove(temp);
	if (err)
		return translate(err);
	add_file(src, path);
	return CHDM_OK;
}

// ---- CloneCD --------------------------------------------------------------------------------

int load_ccd(std::string const &path, cd_source &src)
{
	std::vector<uint8_t> raw;
	if (!read_whole_file(path, raw, 1 << 20))
		return error_message(CHDM_ERR_IO, "can't read " + path);

	// [Section] key=value, keys may contain spaces ("INDEX 1").
	std::map<std::string, std::map<std::string, std::string>> ini;
	std::string section;
	for (std::string const &l : text_lines(raw))
	{
		std::string const line = trim(l);
		if (line.empty() || line[0] == ';')
			continue;
		if (line.front() == '[' && line.back() == ']')
		{
			section = lower(trim(line.substr(1, line.size() - 2)));
			continue;
		}
		size_t const eq = line.find('=');
		if (eq != std::string::npos)
			ini[section][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
	}
	auto num = [&](std::string const &sec, std::string const &key, long def) -> long {
		auto s = ini.find(sec);
		if (s == ini.end())
			return def;
		auto k = s->second.find(key);
		return k == s->second.end() ? def : std::strtol(k->second.c_str(), nullptr, 0);
	};
	if (ini.find("clonecd") == ini.end() && ini.find("disc") == ini.end())
		return error_message(CHDM_ERR_INVALID_DATA, path + " isn't a CloneCD control file");
	if (num("disc", "sessions", 1) > 1)
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, "multi-session CloneCD images aren't supported");

	path_t const ccd = to_path(path);
	path_t img = find_case_insensitive(ccd.parent_path(), to_utf8(ccd.stem()) + ".img");
	if (img.empty())
		return error_message(CHDM_ERR_NOT_FOUND, to_utf8(ccd.stem()) + ".img is missing next to " + to_utf8(ccd.filename()));
	std::string const img_path = to_utf8(img);
	uint64_t const img_frames = file_size_of(img_path) / 2352;

	// Control bits of each track, from the TOC entries.
	std::map<int, int> control;
	for (auto const &[name, keys] : ini)
		if (name.rfind("entry ", 0) == 0)
		{
			long const point = num(name, "point", 0);
			if (point >= 1 && point <= 99)
				control[int(point)] = int(num(name, "control", 0));
		}

	struct T { int number, mode; long idx0, idx1; };
	std::vector<T> tracks;
	for (int n = 1; n <= 99; n++)
	{
		std::string const sec = "track " + std::to_string(n);
		if (ini.find(sec) == ini.end())
			continue;
		T t{ n, int(num(sec, "mode", -1)), num(sec, "index 0", -1), num(sec, "index 1", -1) };
		if (t.idx1 < 0)
			return error_message(CHDM_ERR_INVALID_DATA, util::string_format("track %d has no INDEX 1 in %s", n, to_utf8(ccd.filename())));
		tracks.push_back(t);
	}
	if (tracks.empty())
		return error_message(CHDM_ERR_INVALID_DATA, to_utf8(ccd.filename()) + " lists no tracks");

	src.toc.numtrks = uint32_t(tracks.size());
	src.toc.numsessions = 1;
	for (size_t i = 0; i < tracks.size(); i++)
	{
		T const &t = tracks[i];
		cdrom_file::track_info &ti = src.toc.tracks[i];
		uint32_t type;
		if (t.mode == 0)
			type = cdrom_file::CD_TRACK_AUDIO;
		else if (t.mode == 1)
			type = cdrom_file::CD_TRACK_MODE1_RAW;
		else if (t.mode == 2)
			type = cdrom_file::CD_TRACK_MODE2_RAW;
		else
			return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, util::string_format("track %d has an unknown mode %d", t.number, t.mode));
		set_raw_track(ti, type);
		long const start = (t.idx0 >= 0 && t.idx0 <= t.idx1) ? t.idx0 : t.idx1;
		long end;
		if (i + 1 < tracks.size())
			end = tracks[i + 1].idx0 >= 0 ? tracks[i + 1].idx0 : tracks[i + 1].idx1;
		else
			end = long(img_frames);
		if (start < 0 || end <= start || uint64_t(end) > img_frames)
			return error_message(CHDM_ERR_INVALID_DATA, util::string_format("track %d doesn't fit in %s", t.number, to_utf8(img.filename())));
		ti.frames = uint32_t(end - start);
		if (t.idx1 > start)
		{
			ti.pregap = uint32_t(t.idx1 - start);
			ti.pgtype = type;
			ti.pgdatasize = 2352;
		}
		auto c = control.find(t.number);
		ti.control_flags = c == control.end() ? 0 : uint32_t(c->second & 0x0b);
		src.info.track[i].fname = img_path;
		src.info.track[i].offset = uint32_t(uint64_t(start) * 2352);
		src.info.track[i].swap = type == cdrom_file::CD_TRACK_AUDIO;
		src.info.track[i].idx[0] = int32_t(start);
		src.info.track[i].idx[1] = int32_t(t.idx1);
	}
	add_file(src, path);
	add_file(src, img_path);
	std::string const sub = to_utf8(find_case_insensitive(ccd.parent_path(), to_utf8(ccd.stem()) + ".sub"));
	add_file(src, sub);
	return CHDM_OK;
}

// ---- Alcohol 120% ---------------------------------------------------------------------------

uint16_t le16(uint8_t const *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(uint8_t const *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(uint8_t const *p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

int load_mds(std::string const &path, cd_source &src)
{
	std::vector<uint8_t> m;
	if (!read_whole_file(path, m, 16 << 20))
		return error_message(CHDM_ERR_IO, "can't read " + path);
	path_t const mds = to_path(path);
	std::string const name = to_utf8(mds.filename());
	auto bad = [&](char const *what) { return error_message(CHDM_ERR_INVALID_DATA, name + ": " + what); };
	if (m.size() < 0x58 || std::memcmp(m.data(), "MEDIA DESCRIPTOR", 16) != 0)
		return bad("not an Alcohol 120% descriptor");
	if (m[0x10] >= 2)
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, name + ": MDS version 2 (encrypted) isn't supported");
	uint16_t const medium = le16(&m[0x12]);
	if (medium >= 0x10)
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, name + " describes a DVD: convert its .mdf as a plain image");
	if (le16(&m[0x14]) != 1)
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, name + ": multi-session Alcohol images aren't supported");
	uint32_t const sess = le32(&m[0x50]);
	if (uint64_t(sess) + 24 > m.size())
		return bad("truncated session block");
	uint8_t const *s = &m[sess];
	uint32_t const blocks = s[10];
	uint32_t const tracks_ofs = le32(s + 20);
	if (uint64_t(tracks_ofs) + uint64_t(blocks) * 80 > m.size())
		return bad("truncated track blocks");

	struct T { int number; uint8_t mode, adr_ctl; uint32_t sector_size, start, length; uint64_t offset; std::string file; uint32_t span = 0; };
	std::vector<T> tracks;
	int32_t leadout = -1;
	for (uint32_t b = 0; b < blocks; b++)
	{
		uint8_t const *t = &m[tracks_ofs + b * 80];
		uint8_t const point = t[4];
		if (point == 0xa2)
			leadout = int32_t((t[9] * 60 + t[10]) * 75 + t[11]) - 150;
		if (point < 1 || point > 99)
			continue;
		T tr{ point, t[0], t[2], le16(t + 16), le32(t + 36), 0, le64(t + 40), std::string() };
		uint32_t const extra = le32(t + 12);
		if (extra && uint64_t(extra) + 8 <= m.size())
			tr.length = le32(&m[extra + 4]);
		uint32_t const footer = le32(t + 52);
		if (footer && uint64_t(footer) + 16 <= m.size())
		{
			uint32_t const fn = le32(&m[footer]);
			bool const wide = le32(&m[footer + 4]) != 0;
			std::string file;
			if (wide)
			{
				for (uint64_t i = fn; i + 1 < m.size() && (m[i] | m[i + 1]); i += 2)
				{
					uint32_t const c = le16(&m[i]);  // file names are ASCII in practice; keep what UTF-8 needs
					if (c < 0x80) file += char(c);
					else if (c < 0x800) { file += char(0xc0 | (c >> 6)); file += char(0x80 | (c & 0x3f)); }
					else { file += char(0xe0 | (c >> 12)); file += char(0x80 | ((c >> 6) & 0x3f)); file += char(0x80 | (c & 0x3f)); }
				}
			}
			else
			{
				for (uint64_t i = fn; i < m.size() && m[i]; i++)
					file += char(m[i]);
			}
			tr.file = file;
		}
		tracks.push_back(tr);
	}
	if (tracks.empty())
		return bad("no tracks");
	std::sort(tracks.begin(), tracks.end(), [](T const &a, T const &b) { return a.number < b.number; });

	src.toc.numtrks = uint32_t(tracks.size());
	src.toc.numsessions = 1;
	for (size_t i = 0; i < tracks.size(); i++)
	{
		T &t = tracks[i];
		cdrom_file::track_info &ti = src.toc.tracks[i];
		if (t.length == 0)
		{
			int64_t const end = i + 1 < tracks.size() ? int64_t(tracks[i + 1].start) : int64_t(leadout);
			if (end <= int64_t(t.start))
				return bad("can't tell how long a track is");
			t.length = uint32_t(end - t.start);
		}
		t.span = t.length;
		// "*.mdf" means the descriptor's own name.
		std::string file = t.file.empty() ? std::string("*.mdf") : t.file;
		if (!file.empty() && file[0] == '*')
			file = to_utf8(mds.stem()) + file.substr(1);
		path_t const mdf = find_case_insensitive(mds.parent_path(), file);
		if (mdf.empty())
			return error_message(CHDM_ERR_NOT_FOUND, file + " is missing next to " + name);
		std::string const mdf_path = to_utf8(mdf);

		uint32_t const ss = t.sector_size;
		uint32_t datasize;
		if (ss == 2352 || ss == 2448)
			datasize = 2352;
		else if (ss == 2048 || ss == 2048 + 96)
			datasize = 2048;
		else if (ss == 2336 || ss == 2336 + 96)
			datasize = 2336;
		else
			return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, util::string_format("%s: track %d has %u-byte sectors", name, t.number, ss));

		uint32_t type;
		if ((t.mode & 0x0f) == 0x09)
			type = cdrom_file::CD_TRACK_AUDIO;
		else if (datasize == 2048)
			type = cdrom_file::CD_TRACK_MODE1;
		else if (datasize == 2336)
			type = cdrom_file::CD_TRACK_MODE2;
		else
		{
			// raw data sectors: the mode byte of the first one says which
			uint8_t sector[16];
			int const detected = read_at(mdf_path, t.offset, sector, sizeof(sector)) ? raw_sector_type(sector) : -1;
			type = detected >= 0 ? uint32_t(detected) : ((t.mode & 0x0f) == 0x0a ? cdrom_file::CD_TRACK_MODE1_RAW : cdrom_file::CD_TRACK_MODE2_RAW);
		}
		set_raw_track(ti, type);
		ti.datasize = datasize;
		ti.frames = t.length;
		ti.control_flags = uint32_t(t.adr_ctl & 0x0b);
		// Sectors between the previous track's end and this track's start: when the file has them
		// right before this track's data they are the track's pregap, otherwise the CHD records a
		// pregap without data.
		if (i > 0)
		{
			T const &prev = tracks[i - 1];
			int64_t const gap = int64_t(t.start) - int64_t(prev.start) - int64_t(prev.span);
			if (gap > 0)
			{
				ti.pregap = uint32_t(gap);
				ti.pgtype = type;
				ti.pgdatasize = 0;
				uint64_t const prev_end = prev.offset + uint64_t(prev.length) * prev.sector_size;  // prev.offset/length may include its own pregap
				if (prev.file == t.file && t.offset == prev_end + uint64_t(gap) * ss)
				{
					t.offset = prev_end;
					t.length += uint32_t(gap);
					ti.frames = t.length;
					ti.pgdatasize = datasize;
				}
			}
		}
		if (t.offset > 0xffffffffull || file_size_of(mdf_path) < t.offset + uint64_t(t.length) * ss)
			return error_message(CHDM_ERR_INVALID_DATA, util::string_format("%s: track %d doesn't fit in %s", name, t.number, file));
		src.info.track[i].fname = mdf_path;
		src.info.track[i].offset = uint32_t(t.offset);
		src.info.track[i].swap = type == cdrom_file::CD_TRACK_AUDIO;
		src.stride[i] = ss;
		add_file(src, mdf_path);
	}
	src.files.insert(src.files.begin(), path);
	return CHDM_OK;
}

// ---- bare images ----------------------------------------------------------------------------

// The cue sheet / CloneCD / Alcohol descriptor a bare image belongs to, if any.
std::string find_descriptor(std::string const &image)
{
	path_t const p = to_path(image);
	path_t const dir = p.parent_path();
	std::string const stem = to_utf8(p.stem());
	for (char const *ext : { ".cue", ".ccd", ".mds" })
	{
		path_t const d = find_case_insensitive(dir, stem + ext);
		if (!d.empty())
			return to_utf8(d);
	}
	// A cue sheet with another name that names this file.
	std::string const want = lower(to_utf8(p.filename()));
	for (std::string const &f : list_files(dir))
	{
		path_t const cand = dir / path_t(f);
		if (lower(to_utf8(cand.extension())) != ".cue")
			continue;
		std::vector<uint8_t> raw;
		if (!read_whole_file(to_utf8(cand), raw, 1 << 20))
			continue;
		for (std::string const &line : text_lines(raw))
		{
			std::string name;
			size_t b, e;
			if (cue_file_name(line, name, b, e) && lower(to_utf8(to_path(name).filename())) == want)
				return to_utf8(cand);
		}
	}
	return std::string();
}

int load_bare(std::string const &path, cd_source &src)
{
	uint64_t const size = file_size_of(path);
	uint8_t sector[16] = {};
	bool const have = size >= 16 && read_at(path, 0, sector, sizeof(sector));
	int const type = have ? raw_sector_type(sector) : -1;
	if (type >= 0 && size % 2352 == 0)
	{
		src.toc.numtrks = 1;
		src.toc.numsessions = 1;
		set_raw_track(src.toc.tracks[0], uint32_t(type));
		src.toc.tracks[0].frames = uint32_t(size / 2352);
		src.info.track[0].fname = path;
		add_file(src, path);
		return CHDM_OK;
	}
	if (size > 0 && size % 2048 == 0)
	{
		std::error_condition const err = cdrom_file::parse_iso(path, src.toc, src.info);
		if (err)
			return translate(err);
		add_file(src, path);
		return CHDM_OK;
	}
	return error_message(CHDM_ERR_UNSUPPORTED_FORMAT,
		to_utf8(to_path(path).filename()) + " has no raw CD sectors and isn't made of 2048-byte sectors (an audio-only image needs its cue sheet)");
}

// ---- CHDs as sources ------------------------------------------------------------------------

int load_chd(std::string const &path, cd_source &src)
{
	src.chd = std::make_unique<chd_file>();
	std::error_condition err = src.chd->open(path);
	if (err)
		return translate(err);
	if (src.chd->unit_bytes() != FRAME_SIZE || src.chd->hunk_bytes() % FRAME_SIZE != 0)
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, "not a CD CHD");
	err = cdrom_file::parse_metadata(src.chd.get(), src.toc);
	if (err)
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, "not a CD CHD (no CD track metadata)");
	add_file(src, path);
	return CHDM_OK;
}

// ---- all of them ----------------------------------------------------------------------------

int load_source(std::string const &path_in, cd_source &src)
{
	if (!file_exists(to_path(path_in)))
		return error_message(CHDM_ERR_NOT_FOUND, path_in + " doesn't exist");
	std::string path = path_in;
	std::string ext = extension_of(path);
	if (ext == "bin" || ext == "img" || ext == "raw" || ext == "iso" || ext == "mdf")
	{
		std::string const desc = find_descriptor(path);
		if (!desc.empty())
		{
			path = desc;
			ext = extension_of(desc);
		}
		else if (ext == "mdf")
			return error_message(CHDM_ERR_NOT_FOUND, "the .mds that describes this .mdf is missing");
	}

	int rc;
	std::error_condition err;
	if (ext == "cue")
		rc = load_cue(path, src);
	else if (ext == "ccd")
		rc = load_ccd(path, src);
	else if (ext == "mds")
		rc = load_mds(path, src);
	else if (ext == "chd")
		rc = load_chd(path, src);
	else if (ext == "bin" || ext == "img" || ext == "raw" || ext == "iso")
		rc = load_bare(path, src);
	else if (ext == "gdi" || ext == "nrg" || ext == "toc")
	{
		err = cdrom_file::parse_toc(path, src.toc, src.info);
		rc = err ? translate(err) : CHDM_OK;
		if (rc == CHDM_OK)
		{
			add_file(src, path);
			for (uint32_t t = 0; t < src.toc.numtrks; t++)
				add_file(src, src.info.track[t].fname);
		}
	}
	else
		return error_message(CHDM_ERR_UNSUPPORTED_FORMAT, "not a CD image: " + path);
	if (rc != CHDM_OK)
		return rc;
	src.format = ext == "raw" || ext == "img" ? std::string("bin") : ext;
	if (src.toc.numtrks == 0 || src.toc.numtrks > cdrom_file::MAX_TRACKS)
		return error_message(CHDM_ERR_INVALID_DATA, "no tracks");

	if (ext == "cue")
		for (uint32_t t = 0; t < src.toc.numtrks; t++)
			add_file(src, src.info.track[t].fname);

	if (src.chd)
	{
		uint64_t total = 0;
		for (uint32_t t = 0; t < src.toc.numtrks; t++)
			total += uint64_t(src.toc.tracks[t].frames + src.toc.tracks[t].extraframes) * FRAME_SIZE;
		src.chd_bytes = total;
		return CHDM_OK;
	}

	if (src.toc.flags & cdrom_file::CD_FLAG_GDROM)
	{
		err = cdrom_file::adjust_high_density_area(src.toc, src.info);
		if (err)
			return translate(err);
	}

	// Every track is padded to whole 4-frame groups, as chdman does (cdrom.cpp undoes it).
	uint64_t total = 0;
	for (uint32_t t = 0; t < src.toc.numtrks; t++)
	{
		cdrom_file::track_info &ti = src.toc.tracks[t];
		if (ti.datasize == 0)
			return error_message(CHDM_ERR_INVALID_DATA, util::string_format("track %u has no sector size", t + 1));
		uint32_t const padded = (ti.frames + cdrom_file::TRACK_PADDING - 1) / cdrom_file::TRACK_PADDING;
		ti.extraframes = padded * cdrom_file::TRACK_PADDING - ti.frames;
		total += uint64_t(ti.frames + ti.extraframes) * FRAME_SIZE;
		if (src.stride[t] == 0)
			src.stride[t] = ti.datasize + ti.subsize;
		// The data must be there (GD-ROM tracks borrow frames from the next file, see splitframes).
		if (ti.splitframes == 0)
		{
			uint64_t const need = src.info.track[t].offset + uint64_t(ti.frames - ti.padframes) * src.stride[t];
			if (file_size_of(src.info.track[t].fname) < need)
				return error_message(CHDM_ERR_INVALID_DATA, util::string_format("%s is shorter than track %u needs",
					to_utf8(to_path(src.info.track[t].fname).filename()), t + 1));
		}
	}
	src.chd_bytes = total;
	return CHDM_OK;
}

//**************************************************************************
//  READING FRAMES
//**************************************************************************

// The CHD's logical data as the source makes it: chdman's chd_cd_compressor::read_data, plus
// file strides and CHD sources. offset and length are whole frames.
class frame_reader
{
public:
	explicit frame_reader(cd_source &src) : m_src(src) { }

	bool read(uint64_t offset, uint8_t *dest, uint32_t length)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_src.chd)
		{
			std::error_condition const err = m_src.chd->read_bytes(offset, dest, length);
			if (err)
				m_error = "can't read the source CHD: " + err.message();
			return !err;
		}
		std::memset(dest, 0, length);
		cdrom_file::toc const &toc = m_src.toc;
		uint64_t startoffs = 0;
		uint32_t remaining = length;
		for (uint32_t t = 0; t < toc.numtrks && remaining; t++)
		{
			cdrom_file::track_info const &ti = toc.tracks[t];
			uint64_t const endoffs = startoffs + uint64_t(ti.frames + ti.extraframes) * FRAME_SIZE;
			if (offset >= startoffs && offset < endoffs)
			{
				uint64_t const stride = m_src.stride[t];
				uint32_t const copy = ti.datasize + ti.subsize;
				uint64_t const track_start = m_src.info.track[t].offset;
				uint64_t const track_end = track_start + stride * ti.frames;
				uint64_t split_start = track_end - uint64_t(ti.splitframes) * stride;
				uint64_t const pad_start = split_start - uint64_t(ti.padframes) * stride;
				if (ti.splitframes == 0)
					split_start = UINT64_MAX;
				while (remaining && offset < endoffs)
				{
					uint64_t const frame_start = track_start + ((offset - startoffs) / FRAME_SIZE) * stride;
					if (frame_start < track_end)
					{
						if (frame_start >= pad_start && frame_start < split_start)
						{
							// padding (GD-ROM): zeros
						}
						else
						{
							bool const next = frame_start >= split_start;
							std::string const &fname = next ? m_src.info.track[t + 1].fname : m_src.info.track[t].fname;
							if (!open(fname))
								return false;
							uint64_t const pos = next ? frame_start - split_start : frame_start;
							size_t done = 0;
							while (done < copy)
							{
								size_t actual = 0;
								if (m_file->read_some_at(pos + done, dest + done, copy - done, actual) || actual == 0)
								{
									m_error = "can't read " + fname;
									return false;
								}
								done += actual;
							}
						}
						if (m_src.info.track[t].swap)
							for (uint32_t i = 0; i + 1 < 2352; i += 2)
								std::swap(dest[i], dest[i + 1]);
					}
					offset += FRAME_SIZE;
					dest += FRAME_SIZE;
					remaining -= FRAME_SIZE;
				}
			}
			startoffs = endoffs;
		}
		return true;
	}

	std::string const &error() const { return m_error; }

private:
	bool open(std::string const &fname)
	{
		if (m_file && m_open == fname)
			return true;
		m_file.reset();
		if (util::core_file::open(fname, OPEN_FLAG_READ, m_file))
		{
			m_error = "can't open " + fname;
			return false;
		}
		m_open = fname;
		return true;
	}

	cd_source &m_src;
	std::mutex m_mutex;
	util::core_file::ptr m_file;
	std::string m_open, m_error;
};

class cd_compressor final : public chd_file_compressor
{
public:
	explicit cd_compressor(frame_reader &reader) : m_reader(reader) { }

	uint32_t read_data(void *dest, uint64_t offset, uint32_t length) override
	{
		if (!m_reader.read(offset, static_cast<uint8_t *>(dest), length))
		{
			m_failed = true;
			std::memset(dest, 0, length);
		}
		return length;
	}

	bool failed() const { return m_failed; }

private:
	frame_reader &m_reader;
	std::atomic<bool> m_failed { false };
};

std::string type_name(cdrom_file::track_info const &t)
{
	switch (t.trktype)
	{
	case cdrom_file::CD_TRACK_AUDIO: return "AUDIO";
	case cdrom_file::CD_TRACK_MODE1:
	case cdrom_file::CD_TRACK_MODE1_RAW: return util::string_format("MODE1/%04u", t.datasize);
	default: return util::string_format("MODE2/%04u", t.datasize);
	}
}

std::string msf(uint32_t frames)
{
	return util::string_format("%02u:%02u:%02u", frames / (75 * 60), (frames / 75) % 60, frames % 75);
}

} // anonymous namespace


//**************************************************************************
//  C API
//**************************************************************************

extern "C" {

int chdm_cd_inspect(const char *input_path, chdm_cd_image_info *info)
{
	if (!input_path || !info)
		return set_error(CHDM_ERR_INVALID_ARGUMENT);
	std::memset(info, 0, sizeof(*info));
	try
	{
		auto src = std::make_unique<cd_source>();
		int const rc = load_source(input_path, *src);
		if (rc != CHDM_OK)
			return rc;
		info->tracks = src->toc.numtrks;
		info->sessions = std::max<uint32_t>(1, src->toc.numsessions);
		info->gdrom = (src->toc.flags & (cdrom_file::CD_FLAG_GDROM | cdrom_file::CD_FLAG_GDROMLE)) ? 1 : 0;
		info->files = uint32_t(src->files.size());
		info->chd_bytes = src->chd_bytes;
		for (uint32_t t = 0; t < src->toc.numtrks; t++)
		{
			cdrom_file::track_info const &ti = src->toc.tracks[t];
			if (ti.trktype == cdrom_file::CD_TRACK_AUDIO)
				info->audio_tracks++;
			else
			{
				if (info->data_tracks++ == 0)
					std::snprintf(info->first_data, sizeof(info->first_data), "%s", type_name(ti).c_str());
			}
			info->frames += ti.frames;
			info->data_bytes += uint64_t(ti.frames) * (ti.datasize + ti.subsize);
		}
		std::snprintf(info->format, sizeof(info->format), "%s", src->format.c_str());
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}

int chdm_cd_image_files(const char *input_path, void (*fn)(void *user, const char *path), void *user)
{
	if (!input_path || !fn)
		return set_error(CHDM_ERR_INVALID_ARGUMENT);
	try
	{
		auto src = std::make_unique<cd_source>();
		int const rc = load_source(input_path, *src);
		if (rc != CHDM_OK)
			return rc;
		for (std::string const &f : src->files)
			fn(user, f.c_str());
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}

int chdm_create_cd(const char *input_path, const char *output_path, uint32_t hunk_bytes,
		const uint32_t compression[4], chdm_progress_func progress, void *user)
{
	if (!input_path || !output_path)
		return set_error(CHDM_ERR_INVALID_ARGUMENT);
	if (hunk_bytes == 0)
		hunk_bytes = CHDM_CD_HUNK_BYTES_DEFAULT;
	if (hunk_bytes % FRAME_SIZE != 0)
		return set_error(CHDM_ERR_INVALID_ARGUMENT, "the hunk size of a CD CHD must be a multiple of 2448 bytes");

	bool created = false;
	int result = CHDM_OK;
	try
	{
		auto src = std::make_unique<cd_source>();
		result = load_source(input_path, *src);
		if (result != CHDM_OK)
			return result;

		chd_codec_type codecs[4] = { CHD_CODEC_CD_LZMA, CHD_CODEC_CD_ZLIB, CHD_CODEC_CD_FLAC, CHD_CODEC_NONE };
		if (compression)
			for (int i = 0; i < 4; i++)
				codecs[i] = compression[i];

		frame_reader reader(*src);
		auto const compressor = std::make_unique<cd_compressor>(reader);  // over 1 MB: not on the stack
		cd_compressor &chd = *compressor;
		std::error_condition err = chd.create(output_path, src->chd_bytes, hunk_bytes, FRAME_SIZE, codecs);
		created = true;
		if (!err)
			err = cdrom_file::write_metadata(&chd, src->toc);
		if (!err && progress && progress(user, 0.0))
			result = set_error(CHDM_ERR_CANCELLED);  // before any work: small images can finish in one step
		if (!err && result == CHDM_OK)
		{
			chd.compress_begin();
			double done = 0.0, ratio = 0.0;
			while ((err = chd.compress_continue(done, ratio)) == chd_file::error::WALKING_PARENT || err == chd_file::error::COMPRESSING)
			{
				if (progress && progress(user, done))
				{
					chd.compress_abort();
					result = set_error(CHDM_ERR_CANCELLED);
					break;
				}
			}
		}
		if (result == CHDM_OK)
		{
			if (err)
				result = translate(err);
			else if (chd.failed())
				result = set_error(CHDM_ERR_IO, reader.error().empty() ? std::string("failed to read the CD image") : reader.error());
			else if (progress)
				progress(user, 1.0);
		}
	}
	catch (...)
	{
		result = current_exception_error();
	}
	if (result != CHDM_OK && created)
		osd_file::remove(output_path);
	return result;
}

int chdm_extract_cd(const char *input_path, const char *cue_path, int split_bins, chdm_progress_func progress, void *user)
{
	if (!input_path || !cue_path)
		return set_error(CHDM_ERR_INVALID_ARGUMENT);
	std::vector<std::string> written;
	int result = CHDM_OK;
	try
	{
		chd_file chd;
		std::error_condition err = chd.open(input_path);
		if (err)
			return translate(err);
		std::unique_ptr<cdrom_file> cdrom;
		try
		{
			cdrom = std::make_unique<cdrom_file>(&chd);
		}
		catch (...)
		{
			return set_error(CHDM_ERR_UNSUPPORTED_FORMAT, "not a CD or GD-ROM CHD");
		}
		cdrom_file::toc const &toc = cdrom->get_toc();
		bool const gdrom = cdrom->is_gdrom();
		bool const split = split_bins || gdrom;

		path_t const cue = to_path(cue_path);
		path_t const dir = cue.parent_path();
		std::string const base = to_utf8(cue.stem());
		if (base.find('"') != std::string::npos)
			return set_error(CHDM_ERR_INVALID_ARGUMENT, "the file name must not contain quotation marks");

		std::vector<std::string> names(toc.numtrks);
		uint64_t total = 0;
		for (uint32_t t = 0; t < toc.numtrks; t++)
		{
			names[t] = split ? util::string_format(toc.numtrks >= 10 ? "%s (Track %02u).bin" : "%s (Track %u).bin", base, t + 1) : base + ".bin";
			total += uint64_t(toc.tracks[t].frames - toc.tracks[t].padframes + toc.tracks[t].splitframes) * toc.tracks[t].datasize;
		}

		std::string text;
		util::core_file::ptr bin;
		std::string open_name;
		uint64_t outputoffs = 0, done = 0;
		uint32_t discoffs = 0;
		int session = -1;
		std::vector<uint8_t> buffer;
		for (uint32_t t = 0; t < toc.numtrks && result == CHDM_OK; t++)
		{
			cdrom_file::track_info const &ti = toc.tracks[t];
			if (names[t] != open_name)
			{
				bin.reset();
				open_name = names[t];
				std::string const path = to_utf8(dir / to_path(open_name));
				if (util::core_file::open(path, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, bin))
				{
					result = set_error(CHDM_ERR_FILE_NOT_WRITEABLE, "can't create " + path);
					break;
				}
				written.push_back(path);
				outputoffs = 0;
				discoffs = 0;
			}

			if (gdrom)
			{
				if (t == 0)
					text += "REM SINGLE-DENSITY AREA\n";
				else if (ti.physframeofs == cdrom_file::GDI_HIGH_DENSITY_AREA)
					text += "REM HIGH-DENSITY AREA\n";
			}
			if (toc.numsessions > 1 && session != int(ti.session))
			{
				text += util::string_format("REM SESSION %02u\n", ti.session + 1);
				session = int(ti.session);
			}
			if (outputoffs == 0)
				text += util::string_format("FILE \"%s\" BINARY\n", open_name);
			text += util::string_format("  TRACK %02u %s\n", t + 1, type_name(ti));
			if (ti.pregap > 0 && ti.pgdatasize == 0)
			{
				text += "    PREGAP " + msf(ti.pregap) + "\n";
				text += "    INDEX 01 " + msf(discoffs) + "\n";
			}
			else if (ti.pregap > 0)
			{
				text += "    INDEX 00 " + msf(discoffs) + "\n";
				text += "    INDEX 01 " + msf(discoffs + ti.pregap) + "\n";
			}
			else
				text += "    INDEX 01 " + msf(discoffs) + "\n";
			if (ti.postgap > 0)
				text += "    POSTGAP " + msf(ti.postgap) + "\n";

			uint32_t const actual = ti.frames - ti.padframes + ti.splitframes;
			uint32_t const per = ti.datasize;
			buffer.resize(size_t(std::max<uint32_t>(1, (1u << 20) / per)) * per);
			size_t used = 0;
			for (uint32_t f = 0; f < actual; f++)
			{
				uint32_t trk, ofs;
				if (t > 0 && f < ti.splitframes)
				{
					trk = t - 1;
					ofs = toc.tracks[trk].frames - ti.splitframes + f;
				}
				else
				{
					trk = t;
					ofs = f - ti.splitframes;
				}
				if (!cdrom->read_data(cdrom->get_track_start_phys(trk) + ofs, &buffer[used], toc.tracks[trk].trktype, true))
				{
					result = set_error(CHDM_ERR_DECOMPRESSION_ERROR, util::string_format("can't read frame %u of track %u", f, trk + 1));
					break;
				}
				if (toc.tracks[trk].trktype == cdrom_file::CD_TRACK_AUDIO)
					for (uint32_t i = 0; i + 1 < toc.tracks[trk].datasize; i += 2)
						std::swap(buffer[used + i], buffer[used + i + 1]);
				used += toc.tracks[trk].datasize;
				discoffs++;
				if (used + per > buffer.size() || f + 1 == actual)
				{
					if (!write_all(*bin, outputoffs, buffer.data(), used))
					{
						result = set_error(CHDM_ERR_IO, "can't write " + open_name);
						break;
					}
					outputoffs += used;
					done += used;
					used = 0;
					if (progress && total && progress(user, double(done) / double(total)))
					{
						result = set_error(CHDM_ERR_CANCELLED);
						break;
					}
				}
			}
			discoffs += ti.padframes;
		}
		bin.reset();

		if (result == CHDM_OK)
		{
			util::core_file::ptr cuefile;
			if (util::core_file::open(cue_path, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, cuefile))
				result = set_error(CHDM_ERR_FILE_NOT_WRITEABLE, std::string("can't create ") + cue_path);
			else
			{
				written.push_back(cue_path);
				if (!write_all(*cuefile, 0, text.data(), text.size()))
					result = set_error(CHDM_ERR_IO, std::string("can't write ") + cue_path);
			}
		}
		if (result == CHDM_OK && progress)
			progress(user, 1.0);
	}
	catch (...)
	{
		result = current_exception_error();
	}
	if (result != CHDM_OK)
		for (std::string const &f : written)
			osd_file::remove(f);
	return result;
}

int chdm_verify_cd(const char *image_path, const char *chd_path, chdm_progress_func progress, void *user)
{
	if (!image_path || !chd_path)
		return set_error(CHDM_ERR_INVALID_ARGUMENT);
	try
	{
		auto src = std::make_unique<cd_source>();
		int rc = load_source(image_path, *src);
		if (rc != CHDM_OK)
			return rc;

		chd_file chd;
		std::error_condition err = chd.open(chd_path);
		if (err)
			return translate(err);
		auto toc = std::make_unique<cdrom_file::toc>();
		std::memset(toc.get(), 0, sizeof(*toc));
		if (chd.unit_bytes() != FRAME_SIZE || cdrom_file::parse_metadata(&chd, *toc))
			return set_error(CHDM_ERR_INVALID_DATA, "not a CD CHD");

		auto differ = [](std::string const &what) { return set_error(CHDM_ERR_INVALID_DATA, what); };
		if (toc->numtrks != src->toc.numtrks)
			return differ(util::string_format("the CHD has %u tracks, the image %u", toc->numtrks, src->toc.numtrks));
		for (uint32_t t = 0; t < toc->numtrks; t++)
		{
			cdrom_file::track_info const &a = toc->tracks[t], &b = src->toc.tracks[t];
			if (a.trktype != b.trktype || a.datasize != b.datasize)
				return differ(util::string_format("track %u is %s in the CHD and %s in the image", t + 1, type_name(a), type_name(b)));
			if (a.frames != b.frames)
				return differ(util::string_format("track %u has %u frames in the CHD and %u in the image", t + 1, a.frames, b.frames));
			if (a.pregap != b.pregap || (a.pgdatasize != 0) != (b.pgdatasize != 0) || a.postgap != b.postgap)
				return differ(util::string_format("track %u has other gaps in the CHD than in the image", t + 1));
		}
		if (chd.logical_bytes() != src->chd_bytes)
			return differ("the CHD's size doesn't match the image");

		frame_reader reader(*src);
		uint32_t const hunk = chd.hunk_bytes();
		std::vector<uint8_t> a(hunk), b(hunk);
		uint64_t const total = chd.logical_bytes();
		for (uint64_t ofs = 0; ofs < total; ofs += hunk)
		{
			uint32_t const len = uint32_t(std::min<uint64_t>(hunk, total - ofs));
			err = chd.read_hunk(uint32_t(ofs / hunk), a.data());
			if (err)
				return translate(err);
			if (!reader.read(ofs, b.data(), len))
				return set_error(CHDM_ERR_IO, reader.error());
			if (std::memcmp(a.data(), b.data(), len) != 0)
			{
				uint32_t i = 0;
				while (i < len && a[i] == b[i])
					i++;
				return differ(util::string_format("frame %u differs", uint32_t((ofs + i) / FRAME_SIZE)));
			}
			if (progress && progress(user, double(ofs + len) / double(total)))
				return set_error(CHDM_ERR_CANCELLED);
		}
		if (progress)
			progress(user, 1.0);
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}

} // extern "C"

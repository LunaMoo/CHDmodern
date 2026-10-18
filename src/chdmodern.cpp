// license:BSD-3-Clause
// copyright-holders:CHDmodern contributors
/***************************************************************************

    chdmodern.cpp

    C API wrapper around MAME's chd_file / cdrom_file.

***************************************************************************/

#include "chdmodern/chdmodern.h"

#include "cdrom.h"
#include "chd.h"
#include "corefile.h"
#include "ioprocs.h"
#include "osdcore.h"
#include "strformat.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>


struct chdm_file
{
	std::unique_ptr<chd_file> chd = std::make_unique<chd_file>();
};

struct chdm_cd
{
	std::unique_ptr<cdrom_file> cd;
};


namespace {

//**************************************************************************
//  ERROR HANDLING
//**************************************************************************

thread_local std::string g_last_error;

int set_error(int code, std::string message = std::string()) noexcept
{
	try
	{
		g_last_error = message.empty() ? std::string(chdm_error_string(code)) : std::move(message);
	}
	catch (...)
	{
		// leave the previous message in place
	}
	return code;
}

int translate(std::error_condition const &err) noexcept
{
	if (!err)
		return CHDM_OK;

	int code = CHDM_ERR_UNKNOWN;
	if (err.category() == chd_category())
	{
		code = err.value();
	}
	else if (err == std::errc::not_enough_memory)
	{
		code = CHDM_ERR_OUT_OF_MEMORY;
	}
	else if (err == std::errc::no_such_file_or_directory)
	{
		code = CHDM_ERR_NOT_FOUND;
	}
	else if (err == std::errc::permission_denied || err == std::errc::operation_not_permitted || err == std::errc::read_only_file_system)
	{
		code = CHDM_ERR_ACCESS_DENIED;
	}
	else if (err == std::errc::invalid_argument)
	{
		code = CHDM_ERR_INVALID_ARGUMENT;
	}
	else if (err == std::errc::not_supported || err == std::errc::operation_not_supported)
	{
		code = CHDM_ERR_NOT_SUPPORTED;
	}
	else if (err.category() == std::generic_category() || err.category() == std::system_category() || err == std::errc::io_error)
	{
		code = CHDM_ERR_IO;
	}

	std::string msg;
	try { msg = err.message(); } catch (...) { }
	return set_error(code, std::move(msg));
}

int current_exception_error() noexcept
{
	try
	{
		throw;
	}
	catch (std::bad_alloc const &)
	{
		return set_error(CHDM_ERR_OUT_OF_MEMORY);
	}
	catch (std::exception const &e)
	{
		return set_error(CHDM_ERR_UNKNOWN, e.what());
	}
	catch (...)
	{
		return set_error(CHDM_ERR_UNKNOWN);
	}
}

//**************************************************************************
//  LOGGING
//**************************************************************************

class log_forwarder final : public osd_output
{
public:
	void output_callback(osd_output_channel channel, util::format_argument_pack<char> const &args) override
	{
		chdm_log_func const func = m_func;
		if (!func)
			return;
		int level;
		switch (channel)
		{
		case OSD_OUTPUT_CHANNEL_ERROR:   level = CHDM_LOG_ERROR; break;
		case OSD_OUTPUT_CHANNEL_WARNING: level = CHDM_LOG_WARNING; break;
		case OSD_OUTPUT_CHANNEL_INFO:    level = CHDM_LOG_INFO; break;
		case OSD_OUTPUT_CHANNEL_DEBUG:   level = CHDM_LOG_DEBUG; break;
		default:                         level = CHDM_LOG_VERBOSE; break;
		}
		try
		{
			std::string const message = util::string_format(args);
			func(m_user, level, message.c_str());
		}
		catch (...)
		{
		}
	}

	chdm_log_func m_func = nullptr;
	void *m_user = nullptr;
	bool m_pushed = false;
};

log_forwarder g_log_forwarder;


#define CHDM_REQUIRE(cond) \
	do { if (!(cond)) return set_error(CHDM_ERR_INVALID_ARGUMENT); } while (0)


//**************************************************************************
//  CUSTOM I/O ADAPTER
//**************************************************************************

class callback_reader final : public util::random_read_write
{
public:
	explicit callback_reader(chdm_io const &io) noexcept : m_io(io) { }
	~callback_reader() override
	{
		if (m_owns_user && m_io.close)
			m_io.close(m_io.user);
	}

	// called once the CHD opened successfully; from then on the reader is
	// responsible for calling io->close
	void take_ownership() noexcept { m_owns_user = true; }

	// random_access
	std::error_condition seek(std::int64_t offset, int whence) noexcept override
	{
		std::uint64_t base;
		switch (whence)
		{
		case SEEK_SET: base = 0; break;
		case SEEK_CUR: base = m_pointer; break;
		case SEEK_END: base = m_io.size(m_io.user); break;
		default: return std::errc::invalid_argument;
		}
		if (offset < 0 && std::uint64_t(-offset) > base)
			return std::errc::invalid_argument;
		m_pointer = base + offset;
		return std::error_condition();
	}

	std::error_condition tell(std::uint64_t &result) noexcept override
	{
		result = m_pointer;
		return std::error_condition();
	}

	std::error_condition length(std::uint64_t &result) noexcept override
	{
		result = m_io.size(m_io.user);
		return std::error_condition();
	}

	// reading
	std::error_condition read_some(void *buffer, std::size_t length, std::size_t &actual) noexcept override
	{
		std::error_condition const err = read_some_at(m_pointer, buffer, length, actual);
		if (!err)
			m_pointer += actual;
		return err;
	}

	std::error_condition read_some_at(std::uint64_t offset, void *buffer, std::size_t length, std::size_t &actual) noexcept override
	{
		std::size_t const r = m_io.read_at(m_io.user, offset, buffer, length);
		if (r == std::size_t(-1))
		{
			actual = 0;
			return std::errc::io_error;
		}
		actual = r;
		return std::error_condition();
	}

	// writing is not supported
	std::error_condition finalize() noexcept override { return std::error_condition(); }
	std::error_condition flush() noexcept override { return std::error_condition(); }
	std::error_condition write_some(void const *, std::size_t, std::size_t &actual) noexcept override
	{
		actual = 0;
		return std::errc::read_only_file_system;
	}
	std::error_condition write_some_at(std::uint64_t, void const *, std::size_t, std::size_t &actual) noexcept override
	{
		actual = 0;
		return std::errc::read_only_file_system;
	}

private:
	chdm_io       m_io;
	std::uint64_t m_pointer = 0;
	bool          m_owns_user = false;
};


// A CHD being created through the caller's chdm_output.
class callback_output final : public util::random_read_write
{
public:
	explicit callback_output(chdm_output const &out) noexcept : m_out(out) { }

	std::error_condition seek(std::int64_t offset, int whence) noexcept override
	{
		std::uint64_t base;
		switch (whence)
		{
		case SEEK_SET: base = 0; break;
		case SEEK_CUR: base = m_pointer; break;
		case SEEK_END: base = m_out.size(m_out.user); break;
		default: return std::errc::invalid_argument;
		}
		if (offset < 0 && std::uint64_t(-offset) > base)
			return std::errc::invalid_argument;
		m_pointer = base + offset;
		return std::error_condition();
	}
	std::error_condition tell(std::uint64_t &result) noexcept override
	{
		result = m_pointer;
		return std::error_condition();
	}
	std::error_condition length(std::uint64_t &result) noexcept override
	{
		result = m_out.size(m_out.user);
		return std::error_condition();
	}

	std::error_condition read_some(void *buffer, std::size_t length, std::size_t &actual) noexcept override
	{
		std::error_condition const err = read_some_at(m_pointer, buffer, length, actual);
		if (!err)
			m_pointer += actual;
		return err;
	}
	std::error_condition read_some_at(std::uint64_t offset, void *buffer, std::size_t length, std::size_t &actual) noexcept override
	{
		std::size_t const r = m_out.read_at(m_out.user, offset, buffer, length);
		if (r == std::size_t(-1))
		{
			actual = 0;
			return std::errc::io_error;
		}
		actual = r;
		return std::error_condition();
	}

	std::error_condition finalize() noexcept override { return std::error_condition(); }
	std::error_condition flush() noexcept override { return std::error_condition(); }
	std::error_condition write_some(void const *buffer, std::size_t length, std::size_t &actual) noexcept override
	{
		std::error_condition const err = write_some_at(m_pointer, buffer, length, actual);
		if (!err)
			m_pointer += actual;
		return err;
	}
	std::error_condition write_some_at(std::uint64_t offset, void const *buffer, std::size_t length, std::size_t &actual) noexcept override
	{
		std::size_t const w = m_out.write_at(m_out.user, offset, buffer, length);
		if (w != length)
		{
			actual = 0;
			return std::errc::io_error;
		}
		actual = w;
		return std::error_condition();
	}

private:
	chdm_output   m_out;
	std::uint64_t m_pointer = 0;
};


chdm_kind detect_kind(chd_file const &chd) noexcept
{
	if (!chd.check_is_dvd()) return CHDM_KIND_DVD;
	if (!chd.check_is_gd())  return CHDM_KIND_GDROM;
	if (!chd.check_is_cd())  return CHDM_KIND_CDROM;
	if (!chd.check_is_hd())  return CHDM_KIND_HARDDISK;
	if (!chd.check_is_av())  return CHDM_KIND_AV;
	return CHDM_KIND_UNKNOWN;
}

chd_file::open_parent_func make_parent_func(chdm_find_parent_func find_parent, void *user)
{
	if (!find_parent)
		return nullptr;
	return [find_parent, user] (util::sha1_t const &sha1) -> std::unique_ptr<chd_file>
	{
		chdm_file *const parent = find_parent(user, sha1.m_raw);
		if (!parent)
			return nullptr;
		// the child takes over the parent; chd_file verifies the SHA-1
		std::unique_ptr<chd_file> result = std::move(parent->chd);
		delete parent;
		return result;
	};
}


int open_path(const char *path, int mode, chd_file *parent, chdm_find_parent_func find_parent, void *user, chdm_file **out)
{
	CHDM_REQUIRE(path && out);
	*out = nullptr;
	try
	{
		auto file = std::make_unique<chdm_file>();
		std::error_condition const err = file->chd->open(path, mode == CHDM_OPEN_WRITE, parent, make_parent_func(find_parent, user));
		if (err)
			return translate(err);
		*out = file.release();
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}


int open_io(const chdm_io *io, chd_file *parent, chdm_find_parent_func find_parent, void *user, chdm_file **out)
{
	CHDM_REQUIRE(io && io->size && io->read_at && out);
	*out = nullptr;
	try
	{
		auto file = std::make_unique<chdm_file>();
		auto reader = std::make_unique<callback_reader>(*io);
		callback_reader *const raw = reader.get();

		// on failure chd_file destroys the reader; it does not own io->user
		// yet, so io->close is not called and the caller keeps ownership
		std::error_condition const err = file->chd->open(util::random_read_write::ptr(std::move(reader)), false, parent, make_parent_func(find_parent, user));
		if (err)
			return translate(err);
		raw->take_ownership();
		*out = file.release();
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}


//**************************************************************************
//  CONVERTING WHOLE IMAGES
//**************************************************************************

// Feeds a raw image (a file, or the caller's callbacks) to the compressor,
// zero-padding the last hunk. read_data runs on the compressor's read queue,
// so a failure is only recorded here and reported once compression has finished.
class image_compressor final : public chd_file_compressor
{
public:
	image_compressor(util::random_read_write &file, uint64_t length) noexcept : m_file(file), m_length(length) { }

	uint32_t read_data(void *dest, uint64_t offset, uint32_t length) override
	{
		uint8_t *const out = static_cast<uint8_t *>(dest);
		uint32_t done = 0;
		if (offset < m_length)
		{
			uint32_t const want = uint32_t(std::min<uint64_t>(length, m_length - offset));
			while (done < want)
			{
				std::size_t actual = 0;
				if (m_file.read_some_at(offset + done, out + done, want - done, actual) || actual == 0)
				{
					m_read_failed = true;
					break;
				}
				done += uint32_t(actual);
			}
		}
		std::memset(out + done, 0, length - done);
		return length;
	}

	bool read_failed() const noexcept { return m_read_failed; }

private:
	util::random_read_write &m_file;
	uint64_t m_length;
	std::atomic<bool> m_read_failed { false };
};

// Compresses length bytes of input into a new DVD CHD at output_path (deleted on failure), or
// through output when output_path is null.
int create_dvd(util::random_read_write &input, uint64_t length, const char *output_path, util::random_read_write *output,
		uint32_t hunk_bytes, const uint32_t compression[4], chdm_progress_func progress, void *user)
{
	if (length == 0 || (length % 2048) != 0)
		return set_error(CHDM_ERR_INVALID_DATA, "input size is not a multiple of 2048 bytes");

	bool output_created = false;
	int result = CHDM_OK;
	try
	{
		chd_codec_type codecs[4] = { CHD_CODEC_LZMA, CHD_CODEC_ZLIB, CHD_CODEC_HUFFMAN, CHD_CODEC_FLAC };
		if (compression)
		{
			for (int i = 0; i < 4; i++)
				codecs[i] = compression[i];
		}

		// The compressor's worker threads read from input until it's destroyed, which happens
		// before we return. On the heap: chd_file_compressor is over 1 MB, more than a whole
		// default thread stack on Windows (and on most non-main threads elsewhere).
		auto const compressor = std::make_unique<image_compressor>(input, length);
		image_compressor &chd = *compressor;
		std::error_condition err = output_path
				? chd.create(output_path, length, hunk_bytes, 2048, codecs)
				: chd.create(*output, length, hunk_bytes, 2048, codecs);
		output_created = output_path != nullptr;
		if (!err)
			err = chd.write_metadata(DVD_METADATA_TAG, 0, std::string());
		if (!err)
		{
			chd.compress_begin();
			double done = 0.0, ratio = 0.0;
			while ((err = chd.compress_continue(done, ratio)) == chd_file::error::WALKING_PARENT || err == chd_file::error::COMPRESSING)
			{
				if (progress && progress(user, done))
				{
					// the queued reads still call read_data, so they have to finish first
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
			else if (chd.read_failed())
				result = set_error(CHDM_ERR_IO, "failed to read the input image");
			else if (progress)
				progress(user, 1.0);
		}
	}
	catch (...)
	{
		result = current_exception_error();
	}

	if (result != CHDM_OK && output_created)
		osd_file::remove(output_path);
	return result;
}

// Writes the logical data of the CHD at input_path through write(offset, data, length).
// begin(total) runs first and can refuse.
template <typename Begin, typename Write>
int extract_raw(const char *input_path, Begin &&begin, Write &&write, chdm_progress_func progress, void *user)
{
	chd_file chd;
	std::error_condition err = chd.open(input_path);
	if (err)
		return translate(err);
	chdm_kind const kind = detect_kind(chd);
	if (kind == CHDM_KIND_CDROM || kind == CHDM_KIND_GDROM || kind == CHDM_KIND_AV)
		return set_error(CHDM_ERR_UNSUPPORTED_FORMAT, "CD-ROM, GD-ROM and A/V CHDs can't be extracted as raw data");

	uint64_t const total = chd.logical_bytes();
	int result = begin(total);
	if (result != CHDM_OK)
		return result;

	uint32_t const hunk_bytes = chd.hunk_bytes();
	std::vector<uint8_t> buffer(hunk_bytes);
	for (uint32_t hunk = 0; result == CHDM_OK && uint64_t(hunk) * hunk_bytes < total; hunk++)
	{
		uint64_t const offset = uint64_t(hunk) * hunk_bytes;
		err = chd.read_hunk(hunk, buffer.data());
		if (err)
			return translate(err);
		std::size_t const length = std::size_t(std::min<uint64_t>(hunk_bytes, total - offset));
		if (!write(offset, buffer.data(), length))
			return set_error(CHDM_ERR_IO, "failed to write the output");
		if (progress && progress(user, double(offset + length) / double(total)))
			result = set_error(CHDM_ERR_CANCELLED);
	}
	return result;
}

bool write_all(util::core_file &file, uint64_t offset, uint8_t const *data, std::size_t length) noexcept
{
	while (length > 0)
	{
		std::size_t actual = 0;
		if (file.write_some_at(offset, data, length, actual) || actual == 0)
			return false;
		offset += actual;
		data += actual;
		length -= actual;
	}
	return true;
}

} // anonymous namespace


//**************************************************************************
//  PUBLIC API
//**************************************************************************

extern "C" {

const char *chdm_version(void)
{
	return CHDMODERN_VERSION_STRING;
}


uint32_t chdm_version_number(void)
{
	return CHDMODERN_VERSION_NUMBER;
}


const char *chdm_error_string(int err)
{
	switch (err)
	{
	case CHDM_OK:                        return "no error";
	case CHDM_ERR_NO_INTERFACE:          return "no drive interface";
	case CHDM_ERR_NOT_OPEN:              return "file not open";
	case CHDM_ERR_ALREADY_OPEN:          return "file already open";
	case CHDM_ERR_INVALID_FILE:          return "invalid file";
	case CHDM_ERR_INVALID_DATA:          return "invalid data";
	case CHDM_ERR_REQUIRES_PARENT:       return "requires parent";
	case CHDM_ERR_FILE_NOT_WRITEABLE:    return "file not writeable";
	case CHDM_ERR_CODEC_ERROR:           return "codec error";
	case CHDM_ERR_INVALID_PARENT:        return "invalid parent";
	case CHDM_ERR_HUNK_OUT_OF_RANGE:     return "hunk out of range";
	case CHDM_ERR_DECOMPRESSION_ERROR:   return "decompression error";
	case CHDM_ERR_COMPRESSION_ERROR:     return "compression error";
	case CHDM_ERR_CANT_VERIFY:           return "can't verify file";
	case CHDM_ERR_METADATA_NOT_FOUND:    return "metadata not found";
	case CHDM_ERR_INVALID_METADATA_SIZE: return "invalid metadata size";
	case CHDM_ERR_UNSUPPORTED_VERSION:   return "unsupported CHD version";
	case CHDM_ERR_VERIFY_INCOMPLETE:     return "incomplete verify";
	case CHDM_ERR_INVALID_METADATA:      return "invalid metadata";
	case CHDM_ERR_INVALID_STATE:         return "invalid state";
	case CHDM_ERR_OPERATION_PENDING:     return "operation pending";
	case CHDM_ERR_UNSUPPORTED_FORMAT:    return "unsupported format";
	case CHDM_ERR_UNKNOWN_COMPRESSION:   return "unknown compression type";
	case CHDM_ERR_WALKING_PARENT:        return "currently examining parent";
	case CHDM_ERR_COMPRESSING:           return "currently compressing";
	case CHDM_ERR_IO:                    return "I/O error";
	case CHDM_ERR_NOT_FOUND:             return "file not found";
	case CHDM_ERR_ACCESS_DENIED:         return "access denied";
	case CHDM_ERR_OUT_OF_MEMORY:         return "out of memory";
	case CHDM_ERR_INVALID_ARGUMENT:      return "invalid argument";
	case CHDM_ERR_NOT_SUPPORTED:         return "operation not supported";
	case CHDM_ERR_CANCELLED:             return "cancelled";
	default:                             return "unknown error";
	}
}

const char *chdm_last_error_message(void)
{
	return g_last_error.c_str();
}


void chdm_set_log_callback(chdm_log_func func, void *user)
{
	if (g_log_forwarder.m_pushed)
	{
		osd_output::pop(&g_log_forwarder);
		g_log_forwarder.m_pushed = false;
	}
	g_log_forwarder.m_func = func;
	g_log_forwarder.m_user = user;
	if (func)
	{
		osd_output::push(&g_log_forwarder);
		g_log_forwarder.m_pushed = true;
	}
}


int chdm_open(const char *path, int mode, chdm_file *parent, chdm_file **out)
{
	return open_path(path, mode, parent ? parent->chd.get() : nullptr, nullptr, nullptr, out);
}


int chdm_open_ex(const char *path, int mode, chdm_find_parent_func find_parent, void *user, chdm_file **out)
{
	return open_path(path, mode, nullptr, find_parent, user, out);
}


int chdm_open_io(const chdm_io *io, chdm_file *parent, chdm_file **out)
{
	return open_io(io, parent ? parent->chd.get() : nullptr, nullptr, nullptr, out);
}


int chdm_open_io_ex(const chdm_io *io, chdm_find_parent_func find_parent, void *user, chdm_file **out)
{
	return open_io(io, nullptr, find_parent, user, out);
}


void chdm_close(chdm_file *file)
{
	delete file;
}


int chdm_get_info(chdm_file *file, chdm_info *info)
{
	CHDM_REQUIRE(file && info);
	try
	{
		chd_file &chd = *file->chd;
		std::memset(info, 0, sizeof(*info));
		info->version = chd.version();
		info->hunk_bytes = chd.hunk_bytes();
		info->hunk_count = chd.hunk_count();
		info->unit_bytes = chd.unit_bytes();
		info->unit_count = chd.unit_count();
		info->logical_bytes = chd.logical_bytes();
		for (int i = 0; i < 4; i++)
			info->compression[i] = chd.compression(i);
		info->compressed = chd.compressed() ? 1 : 0;
		info->kind = detect_kind(chd);

		util::sha1_t sha = chd.sha1();
		std::memcpy(info->sha1, sha.m_raw, 20);
		sha = chd.raw_sha1();
		std::memcpy(info->raw_sha1, sha.m_raw, 20);
		sha = chd.parent_sha1();
		std::memcpy(info->parent_sha1, sha.m_raw, 20);
		info->has_parent = (sha != util::sha1_t::null) ? 1 : 0;
		info->parent_missing = chd.parent_missing() ? 1 : 0;
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}


void chdm_codec_name(uint32_t codec, char out[5])
{
	if (!out)
		return;
	if (codec == 0)
	{
		std::strcpy(out, "none");
		return;
	}
	for (int i = 0; i < 4; i++)
	{
		char const c = char(codec >> (24 - 8 * i));
		out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	out[4] = 0;
}


int chdm_read_hunk(chdm_file *file, uint32_t hunk, void *buffer)
{
	CHDM_REQUIRE(file && buffer);
	try { return translate(file->chd->read_hunk(hunk, buffer)); }
	catch (...) { return current_exception_error(); }
}


int chdm_read_units(chdm_file *file, uint64_t unit, void *buffer, uint32_t count)
{
	CHDM_REQUIRE(file && buffer);
	try { return translate(file->chd->read_units(unit, buffer, count)); }
	catch (...) { return current_exception_error(); }
}


int chdm_read_bytes(chdm_file *file, uint64_t offset, void *buffer, uint32_t length)
{
	CHDM_REQUIRE(file && (buffer || !length));
	try { return translate(file->chd->read_bytes(offset, buffer, length)); }
	catch (...) { return current_exception_error(); }
}


int chdm_read_metadata(chdm_file *file, uint32_t tag, uint32_t index, void *buffer, uint32_t buffer_size, uint32_t *out_length, uint32_t *out_tag, uint8_t *out_flags)
{
	CHDM_REQUIRE(file && (buffer || !buffer_size));
	try
	{
		std::vector<uint8_t> data;
		chd_metadata_tag resulttag = 0;
		uint8_t resultflags = 0;
		std::error_condition const err = file->chd->read_metadata(tag, index, data, resulttag, resultflags);
		if (err)
			return translate(err);
		if (buffer_size)
			std::memcpy(buffer, data.data(), std::min<std::size_t>(buffer_size, data.size()));
		if (out_length)
			*out_length = uint32_t(data.size());
		if (out_tag)
			*out_tag = resulttag;
		if (out_flags)
			*out_flags = resultflags;
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}


//**************************************************************************
//  CD-ROM
//**************************************************************************

int chdm_cd_open(chdm_file *file, chdm_cd **out)
{
	CHDM_REQUIRE(file && out);
	*out = nullptr;
	if (file->chd->check_is_cd() && file->chd->check_is_gd())
		return set_error(CHDM_ERR_UNSUPPORTED_FORMAT, "not a CD-ROM or GD-ROM CHD");
	try
	{
		auto cd = std::make_unique<chdm_cd>();
		cd->cd = std::make_unique<cdrom_file>(file->chd.get());
		*out = cd.release();
		return CHDM_OK;
	}
	catch (std::nullptr_t)
	{
		// cdrom_file signals bad track metadata by throwing nullptr
		return set_error(CHDM_ERR_INVALID_METADATA, "invalid CD track metadata");
	}
	catch (...)
	{
		return current_exception_error();
	}
}


void chdm_cd_close(chdm_cd *cd)
{
	delete cd;
}


uint32_t chdm_cd_track_count(chdm_cd *cd)
{
	return cd ? uint32_t(cd->cd->get_last_track()) : 0;
}


int chdm_cd_get_track(chdm_cd *cd, uint32_t track, chdm_cd_track *out)
{
	CHDM_REQUIRE(cd && out);
	if (track >= uint32_t(cd->cd->get_last_track()))
		return set_error(CHDM_ERR_INVALID_ARGUMENT, "track number out of range");
	auto const &t = cd->cd->get_toc().tracks[track];
	out->type = t.trktype;
	out->subcode_type = t.subtype;
	out->data_size = t.datasize;
	out->subcode_size = t.subsize;
	out->frames = t.frames;
	out->pregap = t.pregap;
	out->postgap = t.postgap;
	out->session = t.session;
	out->logical_start = t.logframeofs;
	out->physical_start = t.physframeofs;
	out->control_flags = t.control_flags;
	return CHDM_OK;
}


uint32_t chdm_cd_track_for_lba(chdm_cd *cd, uint32_t lba)
{
	return cd ? cd->cd->get_track(lba) : 0;
}


int chdm_cd_read_sector(chdm_cd *cd, uint32_t lba, void *buffer, uint32_t type)
{
	CHDM_REQUIRE(cd && buffer && type <= CHDM_CD_RAW_DONTCARE);
	try
	{
		if (!cd->cd->read_data(lba, buffer, type))
			return set_error(CHDM_ERR_INVALID_DATA, "failed to read CD sector (out of range or incompatible track type)");
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}


int chdm_cd_read_subcode(chdm_cd *cd, uint32_t lba, void *buffer)
{
	CHDM_REQUIRE(cd && buffer);
	try
	{
		if (!cd->cd->read_subcode(lba, buffer))
			return set_error(CHDM_ERR_INVALID_DATA, "failed to read CD subcode");
		return CHDM_OK;
	}
	catch (...)
	{
		return current_exception_error();
	}
}


int chdm_create_dvd(const char *input_path, const char *output_path, uint32_t hunk_bytes,
		const uint32_t compression[4], chdm_progress_func progress, void *user)
{
	CHDM_REQUIRE(input_path && output_path && *input_path && *output_path);
	if (hunk_bytes == 0)
		hunk_bytes = CHDM_DVD_HUNK_BYTES_DEFAULT;
	CHDM_REQUIRE(hunk_bytes % 2048 == 0);

	try
	{
		util::core_file::ptr input;
		std::error_condition err = util::core_file::open(input_path, OPEN_FLAG_READ, input);
		if (err)
			return translate(err);
		uint64_t length = 0;
		err = input->length(length);
		if (err)
			return translate(err);
		return create_dvd(*input, length, output_path, nullptr, hunk_bytes, compression, progress, user);
	}
	catch (...)
	{
		return current_exception_error();
	}
}


int chdm_create_dvd_io(const chdm_io *input, const char *output_path, uint32_t hunk_bytes,
		const uint32_t compression[4], chdm_progress_func progress, void *user)
{
	CHDM_REQUIRE(input && input->size && input->read_at && output_path && *output_path);
	if (hunk_bytes == 0)
		hunk_bytes = CHDM_DVD_HUNK_BYTES_DEFAULT;
	CHDM_REQUIRE(hunk_bytes % 2048 == 0);

	try
	{
		// Never takes ownership, so input->close isn't called.
		callback_reader reader(*input);
		return create_dvd(reader, input->size(input->user), output_path, nullptr, hunk_bytes, compression, progress, user);
	}
	catch (...)
	{
		return current_exception_error();
	}
}


int chdm_create_dvd_to_output(const chdm_io *input, const chdm_output *output, uint32_t hunk_bytes,
		const uint32_t compression[4], chdm_progress_func progress, void *user)
{
	CHDM_REQUIRE(input && input->size && input->read_at);
	CHDM_REQUIRE(output && output->read_at && output->write_at && output->size);
	if (hunk_bytes == 0)
		hunk_bytes = CHDM_DVD_HUNK_BYTES_DEFAULT;
	CHDM_REQUIRE(hunk_bytes % 2048 == 0);

	try
	{
		callback_reader reader(*input);
		callback_output out(*output);
		return create_dvd(reader, input->size(input->user), nullptr, &out, hunk_bytes, compression, progress, user);
	}
	catch (...)
	{
		return current_exception_error();
	}
}


int chdm_extract_raw(const char *input_path, const char *output_path,
		chdm_progress_func progress, void *user)
{
	CHDM_REQUIRE(input_path && output_path && *input_path && *output_path);

	bool output_created = false;
	int result = CHDM_OK;
	try
	{
		util::core_file::ptr output;
		auto begin = [&](uint64_t) -> int
		{
			std::error_condition const err = util::core_file::open(output_path, OPEN_FLAG_WRITE | OPEN_FLAG_CREATE, output);
			if (err)
				return translate(err);
			output_created = true;
			return CHDM_OK;
		};
		auto write = [&](uint64_t offset, uint8_t const *data, std::size_t length)
		{
			return write_all(*output, offset, data, length);
		};
		result = extract_raw(input_path, begin, write, progress, user);
		if (result == CHDM_OK && output->finalize())
			result = set_error(CHDM_ERR_IO, "failed to write the output file");
	}
	catch (...)
	{
		result = current_exception_error();
	}

	if (result != CHDM_OK && output_created)
		osd_file::remove(output_path);
	return result;
}


int chdm_extract_raw_io(const char *input_path, const chdm_writer *output,
		chdm_progress_func progress, void *user)
{
	CHDM_REQUIRE(input_path && *input_path && output && output->write_at);

	try
	{
		auto begin = [&](uint64_t total) -> int
		{
			if (output->begin && output->begin(output->user, total))
				return set_error(CHDM_ERR_OUT_OF_MEMORY, "the output refused the image size");
			return CHDM_OK;
		};
		auto write = [&](uint64_t offset, uint8_t const *data, std::size_t length)
		{
			return output->write_at(output->user, offset, data, length) == length;
		};
		return extract_raw(input_path, begin, write, progress, user);
	}
	catch (...)
	{
		return current_exception_error();
	}
}

} // extern "C"

// license:BSD-3-Clause
// copyright-holders:CHDmodern contributors
/***************************************************************************

    osd_portable.cpp

    Minimal, portable implementation of the parts of MAME's OSD layer that
    the CHD code needs: diagnostic output, timing, file access (osd_file),
    a thread-pool backed work queue (osd_work_*), and a few small helpers.  Uses only the C++
    standard library plus POSIX or Win32 file APIs, so it builds anywhere
    a C++20 compiler is available (desktop, Android, iOS, consoles, ...).

    Define CHDMODERN_NO_THREADS to run all work items synchronously on the
    calling thread (for targets without std::thread).

***************************************************************************/

#include "osdcore.h"
#include "osdfile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include "strconv.h"
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

// Large files on 32-bit Linux and Android.  There off_t is 32 bits unless the
// whole program is built with _FILE_OFFSET_BITS=64, which a host project (and
// Android's NDK before API 24) usually isn't.  With a 32-bit off_t, open() and
// fstat() fail with EOVERFLOW on files of 2 GiB or more, and pread()/pwrite()
// offsets past 2 GiB wrap around.  So use the explicit 64-bit variants there
// (glibc and bionic both provide them); everywhere else off_t is 64-bit
// already (64-bit targets, macOS/iOS, the BSDs, musl).
#if !defined(_WIN32) && (defined(__GLIBC__) || defined(__BIONIC__)) && !defined(__LP64__) && !(defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64)
#  define CHDM_LFS64 1
#endif

#if !defined(_WIN32)
namespace {

#if defined(CHDM_LFS64)
using file_off_t = off64_t;
inline ssize_t file_pread(int fd, void *buf, std::size_t n, std::uint64_t off) noexcept { return ::pread64(fd, buf, n, file_off_t(off)); }
inline ssize_t file_pwrite(int fd, void const *buf, std::size_t n, std::uint64_t off) noexcept { return ::pwrite64(fd, buf, n, file_off_t(off)); }
inline int file_truncate(int fd, std::uint64_t off) noexcept { return ::ftruncate64(fd, file_off_t(off)); }
inline file_off_t file_seek_end(int fd) noexcept { return ::lseek64(fd, 0, SEEK_END); }
#  if defined(O_LARGEFILE)
constexpr int FILE_OPEN_FLAGS = O_LARGEFILE;
#  else
constexpr int FILE_OPEN_FLAGS = 0;
#  endif
#else
using file_off_t = off_t;
static_assert(sizeof(off_t) >= 8, "64-bit file offsets are required; build with -D_FILE_OFFSET_BITS=64");
inline ssize_t file_pread(int fd, void *buf, std::size_t n, std::uint64_t off) noexcept { return ::pread(fd, buf, n, file_off_t(off)); }
inline ssize_t file_pwrite(int fd, void const *buf, std::size_t n, std::uint64_t off) noexcept { return ::pwrite(fd, buf, n, file_off_t(off)); }
inline int file_truncate(int fd, std::uint64_t off) noexcept { return ::ftruncate(fd, file_off_t(off)); }
inline file_off_t file_seek_end(int fd) noexcept { return ::lseek(fd, 0, SEEK_END); }
constexpr int FILE_OPEN_FLAGS = 0;
#endif

} // anonymous namespace
#endif

#if !defined(CHDMODERN_NO_THREADS)
#  include <thread>
#endif


//**************************************************************************
//  DIAGNOSTIC OUTPUT
//**************************************************************************

// Same semantics as MAME's osdcore.cpp: messages go to the most recently
// pushed osd_output delegate, and are dropped when there is none.

namespace {

constexpr int OUTPUT_STACK_SIZE = 10;
osd_output *s_output_stack[OUTPUT_STACK_SIZE];
int s_output_top = -1;
std::mutex s_output_mutex;

void dispatch_output(osd_output_channel channel, util::format_argument_pack<char> const &args)
{
	std::lock_guard<std::mutex> lock(s_output_mutex);
	if (s_output_top >= 0)
		s_output_stack[s_output_top]->output_callback(channel, args);
}

} // anonymous namespace

void osd_output::push(osd_output *delegate)
{
	std::lock_guard<std::mutex> lock(s_output_mutex);
	if (s_output_top < OUTPUT_STACK_SIZE - 1)
	{
		delegate->m_chain = (s_output_top >= 0) ? s_output_stack[s_output_top] : nullptr;
		s_output_stack[++s_output_top] = delegate;
	}
}

void osd_output::pop(osd_output *delegate)
{
	std::lock_guard<std::mutex> lock(s_output_mutex);
	int found = -1;
	for (int i = 0; i <= s_output_top; i++)
	{
		if (s_output_stack[i] == delegate)
		{
			found = i;
			break;
		}
	}
	if (found < 0)
		return;
	if (found < s_output_top)
		s_output_stack[found + 1]->m_chain = s_output_stack[found]->m_chain;
	for (int i = found; i < s_output_top; i++)
		s_output_stack[i] = s_output_stack[i + 1];
	s_output_top--;
}

void osd_vprintf_error(util::format_argument_pack<char> const &args)   { dispatch_output(OSD_OUTPUT_CHANNEL_ERROR, args); }
void osd_vprintf_warning(util::format_argument_pack<char> const &args) { dispatch_output(OSD_OUTPUT_CHANNEL_WARNING, args); }
void osd_vprintf_info(util::format_argument_pack<char> const &args)    { dispatch_output(OSD_OUTPUT_CHANNEL_INFO, args); }
void osd_vprintf_verbose(util::format_argument_pack<char> const &args) { dispatch_output(OSD_OUTPUT_CHANNEL_VERBOSE, args); }
void osd_vprintf_debug(util::format_argument_pack<char> const &args)   { dispatch_output(OSD_OUTPUT_CHANNEL_DEBUG, args); }


//**************************************************************************
//  TIMING
//**************************************************************************

osd_ticks_t osd_ticks() noexcept
{
	return osd_ticks_t(std::chrono::steady_clock::now().time_since_epoch().count());
}

osd_ticks_t osd_ticks_per_second() noexcept
{
	return osd_ticks_t(std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num);
}

void osd_sleep(osd_ticks_t duration) noexcept
{
#if !defined(CHDMODERN_NO_THREADS)
	std::this_thread::sleep_for(std::chrono::steady_clock::duration(duration));
#else
	(void)duration;
#endif
}


//**************************************************************************
//  STRING CONVERSION (Windows only)
//**************************************************************************

#if defined(_WIN32)

namespace osd::text {

std::wstring &to_wstring(std::wstring &dst, std::string_view s)
{
	dst.clear();
	if (s.empty())
		return dst;
	int const len = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	dst.resize(len);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), dst.data(), len);
	return dst;
}

std::wstring &to_wstring(std::wstring &dst, const char *s) { return to_wstring(dst, std::string_view(s)); }
std::wstring to_wstring(std::string_view s) { std::wstring r; return to_wstring(r, s); }
std::wstring to_wstring(const char *s) { std::wstring r; return to_wstring(r, s); }

std::string &from_wstring(std::string &dst, std::wstring_view s)
{
	dst.clear();
	if (s.empty())
		return dst;
	int const len = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
	dst.resize(len);
	WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), dst.data(), len, nullptr, nullptr);
	return dst;
}

std::string &from_wstring(std::string &dst, const WCHAR *s) { return from_wstring(dst, std::wstring_view(s)); }
std::string from_wstring(const std::wstring_view s) { std::string r; return from_wstring(r, s); }
std::string from_wstring(const WCHAR *s) { std::string r; return from_wstring(r, s); }

} // namespace osd::text

#endif // _WIN32


//**************************************************************************
//  FILE ACCESS
//**************************************************************************

namespace {

#if defined(_WIN32)

std::error_condition win_error_to_condition(DWORD err) noexcept
{
	switch (err)
	{
	case ERROR_SUCCESS:             return std::error_condition();
	case ERROR_OUTOFMEMORY:
	case ERROR_NOT_ENOUGH_MEMORY:   return std::errc::not_enough_memory;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:      return std::errc::no_such_file_or_directory;
	case ERROR_ACCESS_DENIED:       return std::errc::permission_denied;
	case ERROR_SHARING_VIOLATION:   return std::errc::device_or_resource_busy;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:      return std::errc::file_exists;
	case ERROR_INVALID_PARAMETER:   return std::errc::invalid_argument;
	case ERROR_DISK_FULL:           return std::errc::no_space_on_device;
	default:                        return std::error_condition(int(err), std::system_category());
	}
}

class win_osd_file final : public osd_file
{
public:
	explicit win_osd_file(HANDLE h) noexcept : m_handle(h) { }
	~win_osd_file() override { CloseHandle(m_handle); }

	std::error_condition read(void *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual) noexcept override
	{
		OVERLAPPED ov{};
		ov.Offset = DWORD(offset);
		ov.OffsetHigh = DWORD(offset >> 32);
		DWORD got = 0;
		if (!ReadFile(m_handle, buffer, length, &got, &ov))
		{
			DWORD const err = GetLastError();
			if (err != ERROR_HANDLE_EOF)
				return win_error_to_condition(err);
		}
		actual = got;
		return std::error_condition();
	}

	std::error_condition write(void const *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual) noexcept override
	{
		OVERLAPPED ov{};
		ov.Offset = DWORD(offset);
		ov.OffsetHigh = DWORD(offset >> 32);
		DWORD put = 0;
		if (!WriteFile(m_handle, buffer, length, &put, &ov))
			return win_error_to_condition(GetLastError());
		actual = put;
		return std::error_condition();
	}

	std::error_condition truncate(std::uint64_t offset) noexcept override
	{
		LARGE_INTEGER li;
		li.QuadPart = LONGLONG(offset);
		if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN) || !SetEndOfFile(m_handle))
			return win_error_to_condition(GetLastError());
		return std::error_condition();
	}

	std::error_condition flush() noexcept override
	{
		// data is not cached in user space; nothing to do
		return std::error_condition();
	}

private:
	HANDLE m_handle;
};

std::error_condition create_path_recursive(std::wstring const &path) noexcept
{
	auto const sep = path.find_last_of(L"\\/");
	if (sep != std::wstring::npos && sep > 0)
	{
		std::wstring const parent = path.substr(0, sep);
		if (parent.size() > 2 || (parent.size() == 2 && parent[1] != L':'))
		{
			std::error_condition const err = create_path_recursive(parent);
			if (err)
				return err;
		}
	}
	DWORD const attr = GetFileAttributesW(path.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES)
		return std::error_condition();
	if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
		return win_error_to_condition(GetLastError());
	return std::error_condition();
}

#else // POSIX

class posix_osd_file final : public osd_file
{
public:
	explicit posix_osd_file(int fd) noexcept : m_fd(fd) { }
	~posix_osd_file() override { ::close(m_fd); }

	std::error_condition read(void *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual) noexcept override
	{
		std::uint32_t total = 0;
		auto *dst = static_cast<std::uint8_t *>(buffer);
		while (total < length)
		{
			ssize_t const r = file_pread(m_fd, dst + total, std::min<std::uint32_t>(length - total, MAX_IO_CHUNK), offset + total);
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				return std::error_condition(errno, std::generic_category());
			}
			if (r == 0)
				break;
			total += std::uint32_t(r);
		}
		actual = total;
		return std::error_condition();
	}

	std::error_condition write(void const *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual) noexcept override
	{
		std::uint32_t total = 0;
		auto const *src = static_cast<std::uint8_t const *>(buffer);
		while (total < length)
		{
			ssize_t const r = file_pwrite(m_fd, src + total, std::min<std::uint32_t>(length - total, MAX_IO_CHUNK), offset + total);
			if (r < 0)
			{
				if (errno == EINTR)
					continue;
				return std::error_condition(errno, std::generic_category());
			}
			if (r == 0)
				return std::errc::io_error; // no progress; don't spin forever
			total += std::uint32_t(r);
		}
		actual = total;
		return std::error_condition();
	}

	std::error_condition truncate(std::uint64_t offset) noexcept override
	{
		if (file_truncate(m_fd, offset) < 0)
			return std::error_condition(errno, std::generic_category());
		return std::error_condition();
	}

	std::error_condition flush() noexcept override
	{
		// writes go straight to the kernel; nothing cached in user space
		return std::error_condition();
	}

private:
	// keeps each call below SSIZE_MAX on 32-bit targets
	static constexpr std::uint32_t MAX_IO_CHUNK = 0x40000000;

	int m_fd;
};

std::error_condition create_path_recursive(std::string const &path) noexcept
{
	auto const sep = path.find_last_of('/');
	if (sep != std::string::npos && sep > 0)
	{
		std::error_condition const err = create_path_recursive(path.substr(0, sep));
		if (err)
			return err;
	}
	struct stat st;
	if (!::stat(path.c_str(), &st))
		return std::error_condition();
	if (::mkdir(path.c_str(), 0777) < 0 && errno != EEXIST)
		return std::error_condition(errno, std::generic_category());
	return std::error_condition();
}

#endif

} // anonymous namespace


std::error_condition osd_file::open(std::string const &path, std::uint32_t openflags, ptr &file, std::uint64_t &filesize) noexcept
{
	try
	{
#if defined(_WIN32)
		std::wstring const wpath = osd::text::to_wstring(path);
		DWORD access = 0, disposition = OPEN_EXISTING;
		DWORD const share = FILE_SHARE_READ | ((openflags & OPEN_FLAG_WRITE) ? 0 : FILE_SHARE_WRITE);
		if (openflags & OPEN_FLAG_READ)
			access |= GENERIC_READ;
		if (openflags & OPEN_FLAG_WRITE)
			access |= GENERIC_WRITE;
		if (openflags & OPEN_FLAG_CREATE)
			disposition = CREATE_ALWAYS;
		if (!access)
			return std::errc::invalid_argument;

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY != WINAPI_FAMILY_DESKTOP_APP)
		HANDLE h = CreateFile2(wpath.c_str(), access, share, disposition, nullptr);
#else
		HANDLE h = CreateFileW(wpath.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
		if (h == INVALID_HANDLE_VALUE && (openflags & OPEN_FLAG_CREATE) && (openflags & OPEN_FLAG_CREATE_PATHS) && GetLastError() == ERROR_PATH_NOT_FOUND)
		{
			auto const sep = wpath.find_last_of(L"\\/");
			if (sep != std::wstring::npos)
			{
				std::error_condition const err = create_path_recursive(wpath.substr(0, sep));
				if (err)
					return err;
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY != WINAPI_FAMILY_DESKTOP_APP)
				h = CreateFile2(wpath.c_str(), access, share, disposition, nullptr);
#else
				h = CreateFileW(wpath.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
			}
		}
		if (h == INVALID_HANDLE_VALUE)
			return win_error_to_condition(GetLastError());

		LARGE_INTEGER size;
		if (!GetFileSizeEx(h, &size))
		{
			std::error_condition const err = win_error_to_condition(GetLastError());
			CloseHandle(h);
			return err;
		}
		file = std::make_unique<win_osd_file>(h);
		filesize = std::uint64_t(size.QuadPart);
		return std::error_condition();
#else
		int flags;
		if (openflags & OPEN_FLAG_WRITE)
		{
			flags = (openflags & OPEN_FLAG_READ) ? O_RDWR : O_WRONLY;
			if (openflags & OPEN_FLAG_CREATE)
				flags |= O_CREAT | O_TRUNC;
		}
		else if (openflags & OPEN_FLAG_READ)
		{
			flags = O_RDONLY;
		}
		else
		{
			return std::errc::invalid_argument;
		}
#ifdef O_CLOEXEC
		flags |= O_CLOEXEC;
#endif

		int fd = ::open(path.c_str(), flags | FILE_OPEN_FLAGS, 0666);
		if (fd < 0 && errno == ENOENT && (openflags & OPEN_FLAG_CREATE) && (openflags & OPEN_FLAG_CREATE_PATHS))
		{
			auto const sep = path.find_last_of('/');
			if (sep != std::string::npos && sep > 0)
			{
				std::error_condition const err = create_path_recursive(path.substr(0, sep));
				if (err)
					return err;
				fd = ::open(path.c_str(), flags | FILE_OPEN_FLAGS, 0666);
			}
		}
		if (fd < 0)
			return std::error_condition(errno, std::generic_category());

		// lseek rather than fstat: a 32-bit struct stat can't hold the size of a
		// large file.  Reads and writes use explicit offsets, so the file
		// pointer's position doesn't matter.
		file_off_t const size = file_seek_end(fd);
		if (size < 0)
		{
			std::error_condition const err(errno, std::generic_category());
			::close(fd);
			return err;
		}
		file = std::make_unique<posix_osd_file>(fd);
		filesize = std::uint64_t(size);
		return std::error_condition();
#endif
	}
	catch (...)
	{
		return std::errc::not_enough_memory;
	}
}


std::error_condition osd_file::openpty(ptr &file, std::string &name) noexcept
{
	return std::errc::not_supported;
}


std::error_condition osd_file::remove(std::string const &filename) noexcept
{
	try
	{
#if defined(_WIN32)
		if (!DeleteFileW(osd::text::to_wstring(filename).c_str()))
			return win_error_to_condition(GetLastError());
#else
		if (::unlink(filename.c_str()) < 0)
			return std::error_condition(errno, std::generic_category());
#endif
		return std::error_condition();
	}
	catch (...)
	{
		return std::errc::not_enough_memory;
	}
}


bool osd_get_physical_drive_geometry(const char *filename, uint32_t *cylinders, uint32_t *heads, uint32_t *sectors, uint32_t *bps) noexcept
{
	return false;
}


//**************************************************************************
//  MISC
//**************************************************************************

int osd_uchar_from_osdchar(char32_t *uchar, const char *osdchar, size_t count)
{
	// all platforms are treated as UTF-8 here
	if (!count)
	{
		*uchar = 0;
		return 0;
	}
	auto const *s = reinterpret_cast<const unsigned char *>(osdchar);
	char32_t c = s[0];
	int len = 1;
	if (c >= 0xf0 && count >= 4)      { c = ((c & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f); len = 4; }
	else if (c >= 0xe0 && count >= 3) { c = ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); len = 3; }
	else if (c >= 0xc0 && count >= 2) { c = ((c & 0x1f) << 6) | (s[1] & 0x3f); len = 2; }
	*uchar = c;
	return len;
}


const char *osd_getenv(const char *name)
{
	return std::getenv(name);
}


void osd_break_into_debugger(const char *message)
{
	(void)message;
}


//**************************************************************************
//  WORK QUEUES
//**************************************************************************

struct osd_work_item
{
	osd_work_queue *  queue;
	osd_work_callback callback;
	void *            param;
	void *            result = nullptr;
	bool              auto_release;
	bool              done = false;     // protected by queue->mutex
};

struct osd_work_queue
{
	std::mutex                   mutex;
	std::condition_variable      work_available;
	std::condition_variable      work_done;
	std::deque<osd_work_item *>  pending;
	unsigned                     active = 0;     // items currently executing
	bool                         exiting = false;
#if !defined(CHDMODERN_NO_THREADS)
	std::vector<std::thread>     threads;
#endif
};

namespace {

std::chrono::nanoseconds ticks_to_duration(osd_ticks_t ticks)
{
	osd_ticks_t const tps = osd_ticks_per_second();
	osd_ticks_t const secs = ticks / tps;
	// clamp absurdly long timeouts so the arithmetic below can't overflow
	if (secs > 86400ULL * 365)
		return std::chrono::hours(24 * 365);
	return std::chrono::seconds(secs) + std::chrono::nanoseconds((ticks % tps) * 1'000'000'000ULL / tps);
}

void run_item(osd_work_item *item, int threadid)
{
	void *const result = item->callback(item->param, threadid);
	osd_work_queue &queue = *item->queue;
	{
		std::lock_guard<std::mutex> lock(queue.mutex);
		--queue.active;
		if (item->auto_release)
		{
			delete item;
		}
		else
		{
			item->result = result;
			item->done = true;
		}
	}
	queue.work_done.notify_all();
}

#if !defined(CHDMODERN_NO_THREADS)
void worker(osd_work_queue *queue, int threadid)
{
	for (;;)
	{
		osd_work_item *item;
		{
			std::unique_lock<std::mutex> lock(queue->mutex);
			queue->work_available.wait(lock, [queue] { return queue->exiting || !queue->pending.empty(); });
			if (queue->pending.empty())
				return; // exiting and drained
			item = queue->pending.front();
			queue->pending.pop_front();
			++queue->active;
		}
		run_item(item, threadid);
	}
}
#endif

} // anonymous namespace


osd_work_queue *osd_work_queue_alloc(int flags)
{
	auto *queue = new (std::nothrow) osd_work_queue;
	if (!queue)
		return nullptr;
#if !defined(CHDMODERN_NO_THREADS)
	unsigned threads = 1;
	if ((flags & WORK_QUEUE_FLAG_MULTI) && !(flags & WORK_QUEUE_FLAG_IO))
		threads = std::clamp(std::thread::hardware_concurrency(), 1U, unsigned(WORK_MAX_THREADS));
	if (char const *env = std::getenv("CHDMODERN_NUMPROCESSORS"))
	{
		int const n = std::atoi(env);
		if (n > 0)
			threads = std::min(unsigned(n), unsigned(WORK_MAX_THREADS));
	}
	try
	{
		for (unsigned i = 0; i < threads; i++)
			queue->threads.emplace_back(worker, queue, int(i));
	}
	catch (...)
	{
		osd_work_queue_free(queue);
		return nullptr;
	}
#else
	(void)flags;
#endif
	return queue;
}


int osd_work_queue_items(osd_work_queue *queue)
{
	std::lock_guard<std::mutex> lock(queue->mutex);
	return int(queue->pending.size() + queue->active);
}


bool osd_work_queue_wait(osd_work_queue *queue, osd_ticks_t timeout)
{
	std::unique_lock<std::mutex> lock(queue->mutex);
	return queue->work_done.wait_for(lock, ticks_to_duration(timeout), [queue] { return queue->pending.empty() && !queue->active; });
}


void osd_work_queue_free(osd_work_queue *queue)
{
	if (!queue)
		return;
#if !defined(CHDMODERN_NO_THREADS)
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->exiting = true;
	}
	queue->work_available.notify_all();
	for (auto &t : queue->threads)
		t.join();
#endif
	// anything left over was never run; free auto-release items
	for (auto *item : queue->pending)
		if (item->auto_release)
			delete item;
	delete queue;
}


osd_work_item *osd_work_item_queue_multiple(osd_work_queue *queue, osd_work_callback callback, int32_t numitems, void *parambase, int32_t paramstep, uint32_t flags)
{
	osd_work_item *last = nullptr;
	bool const auto_release = (flags & WORK_ITEM_FLAG_AUTO_RELEASE) != 0;
	auto *param = static_cast<std::uint8_t *>(parambase);
	for (int32_t i = 0; i < numitems; i++, param += paramstep)
	{
		auto *item = new (std::nothrow) osd_work_item{ queue, callback, param, nullptr, auto_release, false };
		if (!item)
			return nullptr;
#if defined(CHDMODERN_NO_THREADS)
		{
			std::lock_guard<std::mutex> lock(queue->mutex);
			++queue->active;
		}
		if (!auto_release)
			last = item;
		run_item(item, 0);
#else
		{
			std::lock_guard<std::mutex> lock(queue->mutex);
			queue->pending.push_back(item);
		}
		queue->work_available.notify_one();
		last = auto_release ? nullptr : item;
#endif
	}
	return last;
}


bool osd_work_item_wait(osd_work_item *item, osd_ticks_t timeout)
{
	osd_work_queue &queue = *item->queue;
	std::unique_lock<std::mutex> lock(queue.mutex);
	return queue.work_done.wait_for(lock, ticks_to_duration(timeout), [item] { return item->done; });
}


void *osd_work_item_result(osd_work_item *item)
{
	return item->result;
}


void osd_work_item_release(osd_work_item *item)
{
	if (!item)
		return;
	osd_work_queue &queue = *item->queue;
	{
		std::unique_lock<std::mutex> lock(queue.mutex);
		queue.work_done.wait(lock, [item] { return item->done; });
	}
	delete item;
}

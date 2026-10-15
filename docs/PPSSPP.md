# Using CHDmodern in PPSSPP

PPSSPP reads `.chd` UMD images through libchdr in
`Core/FileSystems/BlockDevices.cpp` (`CHDFileBlockDevice`). CHDmodern can
replace libchdr there with MAME's own implementation; the C API maps almost
one-to-one onto what the block device already does, and it also makes the
long-standing "Support parent/child CHD files" TODO easy.

> Status: the code below follows PPSSPP's current `CHDFileBlockDevice` but
> has not been compiled inside the PPSSPP tree yet — it is the starting point
> for the PR, not a finished patch.

## 1. Add the library

```sh
git submodule add https://github.com/LunaMoo/CHDmodern ext/CHDmodern
```

In PPSSPP's `CMakeLists.txt`, after zlib and zstd are set up, reuse PPSSPP's
own copies so only one zlib/zstd ends up in the binary:

```cmake
if(USE_SYSTEM_ZSTD)
	set(CHDMODERN_SYSTEM_ZSTD ON CACHE BOOL "" FORCE)
else()
	set(CHDMODERN_ZSTD_TARGET libzstd_static CACHE STRING "" FORCE)
endif()
set(CHDMODERN_ZLIB_TARGET ${ZLIB_LIBRARY} CACHE STRING "" FORCE)
add_subdirectory(ext/CHDmodern)

target_link_libraries(${CoreLibName} chdmodern)   # replaces chdr-static
```

libchdr bundles its own LZMA SDK, so remove libchdr from the link in the same
change to avoid duplicate `Lzma*` symbols (CHDmodern brings LZMA and libFLAC
itself).

### Windows (PPSSPP.sln)

Add `ext/CHDmodern/msvc/chdmodern.vcxproj` and the `chdm_lzma`, `chdm_flac`
and `chdm_utf8proc` projects next to it to `PPSSPP.sln`, reference
`chdmodern` from Core instead of libchdr, add `ext\CHDmodern\include` to
Core's include path, and create `ext/CHDmodern.settings.props` so the
library matches PPSSPP's static CRT and reuses its zlib/zstd:

```xml
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <ChdModernStaticCRT>true</ChdModernStaticCRT>
    <ChdModernBundledZlib>false</ChdModernBundledZlib>
    <ChdModernZlibInclude>$(MSBuildThisFileDirectory)zlib</ChdModernZlibInclude>
    <ChdModernBundledZstd>false</ChdModernBundledZstd>
    <ChdModernZstdInclude>$(MSBuildThisFileDirectory)zstd\lib</ChdModernZstdInclude>
  </PropertyGroup>
</Project>
```

### Other non-CMake builds

`Android.mk` and libretro's `Makefile.common` need the file list from
CHDmodern's `CMakeLists.txt` (`CHDMODERN_SOURCES` + the `third_party`
sources), compiled as C++20 / C99 with `ZLIB_CONST LSB_FIRST CRLF=2
UTF8PROC_STATIC FLAC__NO_DLL Z7_ST`, and include paths `include`,
`src/mame/util`, `src/mame/osd`, `third_party`, `third_party/flac/include`,
`third_party/utf8proc`.

## 2. BlockDevices.h

```cpp
struct chdm_file;

class CHDFileBlockDevice : public BlockDevice {
public:
	CHDFileBlockDevice(FileLoader *fileLoader);
	~CHDFileBlockDevice();
	bool ReadBlock(int blockNumber, u8 *outPtr, bool uncached = false) override;
	bool ReadBlocks(u32 minBlock, int count, u8 *outPtr) override;
	u32 GetNumBlocks() const override { return numBlocks; }
	bool IsDisc() const override { return true; }
private:
	chdm_file *chd_ = nullptr;
	std::vector<u8> readBuffer;
	u32 unitBytes = 0;
	u32 currentHunk = 0xFFFFFFFF;
	u32 blocksPerHunk = 0;
	u32 numBlocks = 0;
};
```

## 3. BlockDevices.cpp

```cpp
#include <chdmodern/chdmodern.h>

// Reads a CHD through PPSSPP's FileLoader (works for content:// URIs, HTTP, ...).
static chdm_io MakeChdIO(FileLoader *loader, bool ownsLoader) {
	chdm_io io{};
	io.user = loader;
	io.size = [](void *user) -> uint64_t {
		return ((FileLoader *)user)->FileSize();
	};
	io.read_at = [](void *user, uint64_t offset, void *buffer, size_t length) -> size_t {
		return ((FileLoader *)user)->ReadAt(offset, length, buffer);
	};
	if (ownsLoader)
		io.close = [](void *user) { delete (FileLoader *)user; };
	return io;
}

// Parent lookup: find the sibling .chd whose SHA-1 matches. Candidates are
// first opened without a resolver (cheap, and no recursion into the child
// itself); only the match is reopened with the resolver, so a parent that is
// itself a child resolves its own parent the same way. CHDmodern verifies the
// SHA-1 again, and the child takes ownership of what is returned.
static chdm_file *FindParentChd(void *user, const uint8_t parentSha1[20]) {
	const Path &dir = *(const Path *)user;
	std::vector<File::FileInfo> files;
	if (!File::GetFilesInDir(dir, &files))
		return nullptr;
	for (const auto &file : files) {
		if (file.fullName.GetFileExtension() != ".chd")
			continue;
		FileLoader *loader = ConstructFileLoader(file.fullName);
		chdm_io io = MakeChdIO(loader, true);
		chdm_file *candidate = nullptr;
		if (chdm_open_io(&io, nullptr, &candidate) != CHDM_OK) {
			delete loader;  // still ours when the open fails
			continue;
		}
		chdm_info info;
		chdm_get_info(candidate, &info);
		bool match = memcmp(info.sha1, parentSha1, 20) == 0;
		if (match && !info.parent_missing)
			return candidate;
		chdm_close(candidate);  // also deletes the loader
		if (match) {
			// the parent has a parent of its own
			loader = ConstructFileLoader(file.fullName);
			io = MakeChdIO(loader, true);
			if (chdm_open_io_ex(&io, FindParentChd, user, &candidate) == CHDM_OK)
				return candidate;
			delete loader;
			return nullptr;
		}
	}
	return nullptr;
}

CHDFileBlockDevice::CHDFileBlockDevice(FileLoader *fileLoader)
	: BlockDevice(fileLoader) {
	chdm_set_log_callback([](void *, int level, const char *msg) {
		if (level <= CHDM_LOG_WARNING)
			WARN_LOG(Log::Loader, "CHD: %s", msg);
	}, nullptr);

	Path dir = fileLoader->GetPath().NavigateUp();
	chdm_io io = MakeChdIO(fileLoader, false);  // BlockDevice owns fileLoader
	chdm_info info;
	int err = chdm_open_io_ex(&io, FindParentChd, &dir, &chd_);
	if (err == CHDM_OK) {
		chdm_get_info(chd_, &info);
		if (info.parent_missing)
			err = CHDM_ERR_REQUIRES_PARENT;
	}
	if (err != CHDM_OK) {
		errorString_ = StringFromFormat("CHD error: %s: %s",
			fileLoader->GetPath().c_str(), chdm_last_error_message());
		chdm_close(chd_);
		chd_ = nullptr;
		return;
	}

	// "chdman createdvd" images: 2048-byte units, read directly.
	// "chdman createcd" images have 2448-byte frames instead; for those use
	// chdm_cd_open() + chdm_cd_read_sector(cd, lba, buf, CHDM_CD_MODE1).
	unitBytes = info.unit_bytes;
	readBuffer.resize(info.hunk_bytes);
	blocksPerHunk = info.hunk_bytes / info.unit_bytes;
	numBlocks = (u32)info.unit_count;
}

CHDFileBlockDevice::~CHDFileBlockDevice() {
	chdm_close(chd_);  // also closes the parent chain
}

bool CHDFileBlockDevice::ReadBlock(int blockNumber, u8 *outPtr, bool uncached) {
	if (!chd_) {
		ERROR_LOG(Log::Loader, "ReadBlock: CHD not open. %s", fileLoader_->GetPath().c_str());
		return false;
	}
	if ((u32)blockNumber >= numBlocks) {
		memset(outPtr, 0, GetBlockSize());
		return false;
	}
	u32 hunk = blockNumber / blocksPerHunk;
	u32 blockInHunk = blockNumber % blocksPerHunk;

	if (currentHunk != hunk) {
		if (chdm_read_hunk(chd_, hunk, readBuffer.data()) != CHDM_OK) {
			ERROR_LOG(Log::Loader, "CHD read failed: %d %d %s", blockNumber, hunk, chdm_last_error_message());
			NotifyReadError();
		}
		currentHunk = hunk;
	}
	memcpy(outPtr, readBuffer.data() + blockInHunk * unitBytes, GetBlockSize());
	return true;
}

// ReadBlocks() is unchanged.
```

Notes:

* The scan in `FindParentChd` opens every sibling `.chd` once per lookup;
  for folders with many CHDs it is worth caching SHA-1 → path.
* `chdm_set_log_callback` is process-wide; calling it once at startup is
  enough.

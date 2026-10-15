# CHDmodern

MAME's CHD (**C**ompressed **H**unks of **D**ata) implementation —
[`src/lib/util/chd.cpp`](https://github.com/mamedev/mame/blob/master/src/lib/util/chd.cpp)
and everything it needs — extracted from MAME into a standalone library that
any program can link against.

* **The real MAME code.** `src/mame/` is a verbatim copy of MAME's sources
  (plus one small, documented fix in `patches/`). Reading *and* writing,
  every codec MAME supports (zlib, zstd, LZMA, Huffman, FLAC, the CD codecs,
  A/V Huffman), CHD versions 3–5, parent/child CHDs, CD-ROM / GD-ROM / DVD /
  hard-disk / laserdisc metadata.
* **Easy to stay current.** `tools/sync-from-mame.sh /path/to/mame`
  re-imports the sources from any MAME checkout and re-applies the patches.
  The imported revision is recorded in `MAME_REVISION`.
* **No MAME OSD layer.** The few OS services the code needs (files, a work
  queue, timing, diagnostics) are provided by `src/osd_portable.cpp`, which
  only uses the C++ standard library plus POSIX or Win32 file calls.
* **Stable C API** (`include/chdmodern/chdmodern.h`) usable from C, any C++
  standard, or FFI; C++20 consumers can also use MAME's own `chd_file` /
  `cdrom_file` classes directly.
* **Embeddable.** Plain CMake, bundled dependencies (the exact versions MAME
  ships) that can be swapped for the host project's own zlib / zstd / FLAC /
  LZMA to avoid duplicate symbols.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build              # round-trip self-test, every codec + CD
```

Requires CMake ≥ 3.20 and a C++20 compiler (current GCC, Clang, MSVC or
Xcode; tested here with GCC 13 and Clang). The library is built as C++20 internally; the C API header
is plain C, so the consuming project can use any language standard.

| Option | Default | Meaning |
|---|---|---|
| `CHDMODERN_SHARED` | `OFF` | Build a shared library. Only the C API is exported. |
| `CHDMODERN_BUILD_TOOLS` | top-level only | Build `chdm-info` and `chdm-extract`. |
| `CHDMODERN_BUILD_TESTS` | top-level only | Build the self-test. |
| `CHDMODERN_INSTALL` | top-level only | Install rules + `find_package(CHDmodern)` config. |
| `CHDMODERN_NO_THREADS` | `OFF` | Run CHD work items synchronously (targets without `std::thread`). |
| `CHDMODERN_SYSTEM_{ZLIB,ZSTD,FLAC}` | `OFF` | Use the system library instead of the bundled copy. |
| `CHDMODERN_{ZLIB,ZSTD,FLAC,LZMA}_TARGET` | empty | Use an existing CMake target (e.g. the host project's own zlib). |

### Visual Studio

Two ways, both without installing anything beyond Visual Studio with the
"Desktop development with C++" workload:

* **Solution** — open `msvc/CHDmodern.sln`. Configurations Debug/Release,
  platforms x86, x64 and ARM64. Output goes to `build/msvc/<Platform>/<Configuration>/`;
  `chdmodern.lib` already contains the bundled zlib/zstd/LZMA/FLAC/utf8proc,
  so a host links just that one library.
* **DLL from the solution** — the `chdmodern_dll` project builds
  `chdmodern.dll` and its import library `chdmodern.lib` into
  `build/msvc/<Platform>/<Configuration>/dll/` (a separate folder, so the
  import library doesn't overwrite the static `chdmodern.lib`). It exports
  only the C API; programs using it define `CHDMODERN_SHARED` and link the
  import library — `tests/dlltest.c` (project `chdm-dlltest`) is a minimal
  example. To build just the DLL: right-click `chdmodern_dll` → *Build*, or
  `msbuild msvc\CHDmodern.sln /t:chdmodern_dll /p:Configuration=Release /p:Platform=x64`.
* **CMake** — *File → Open → Folder…* on the repository; Visual Studio picks
  up `CMakePresets.json` (x64 Debug/Release, x86, ARM64).

To use the solution's projects inside another solution, add
`msvc/chdmodern.vcxproj` (plus the `chdm_*` third-party projects it
references) and put a `CHDmodern.settings.props` next to the CHDmodern
folder to adapt it, for example for a host that links the CRT statically and
has its own zlib and zstd:

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

All options are described at the top of `msvc/CHDmodern.Common.props`
(for the DLL with the host's own zlib/zstd, also set `ChdModernDllLinkLibs`
to their `.lib` files). The
`.vcxproj` files are generated from the source lists by `tools/gen-msvc.py`
(the sync script runs it), so don't edit them by hand.

## Using it

### From CMake

```cmake
add_subdirectory(ext/CHDmodern)          # or: find_package(CHDmodern)
target_link_libraries(myapp PRIVATE CHDmodern::chdmodern)
```

### C API

```c
#include <chdmodern/chdmodern.h>

chdm_file *chd;
if (chdm_open("game.chd", CHDM_OPEN_READ, NULL, &chd) != CHDM_OK) {
	fprintf(stderr, "%s\n", chdm_last_error_message());
	return 1;
}

chdm_info info;
chdm_get_info(chd, &info);          /* hunk/unit sizes, codecs, SHA-1s, kind */

uint8_t sector[2048];
chdm_read_units(chd, 16, sector, 1);    /* DVD/UMD: sector 16 = ISO9660 PVD */

chdm_close(chd);
```

* **Custom I/O** — `chdm_open_io()` reads through callbacks (`size`,
  `read_at`, optional `close`), so a host can feed the CHD from its own file
  layer, an archive, a content URI, a network stream, …
* **Parent CHDs** — pass the opened parent as the `parent` argument, or let
  `chdm_open_ex()` / `chdm_open_io_ex()` ask a callback for it by SHA-1
  (chains of parents resolve the same way). `chdm_info.parent_missing` tells
  whether a child was opened without its parent.
* **CD-ROM / GD-ROM** — `chdm_cd_open()` gives track info and sector reads
  converted to the requested format (e.g. 2048-byte user data from
  MODE1/2352 raw sectors), using MAME's `cdrom_file`.
* **Metadata** — `chdm_read_metadata()` with any tag (`CHDM_TAG('C','H','T','2')`, …).
* **Diagnostics** — `chdm_set_log_callback()` receives MAME's messages.
* **Whole images** — `chdm_create_dvd()` compresses a 2048-byte-sector image
  (a DVD or PSP UMD ISO) into a DVD CHD like `chdman createdvd`, with a
  choice of hunk size and codecs; `chdm_extract_raw()` writes a CHD's data
  back out, like `chdman extractdvd`. Both report progress through a
  callback that can also cancel, which deletes the partial output.

`tools/chdm-info.c` and `tools/chdm-extract.c` are complete, small examples.

### C++ (MAME API)

When linking the static library, MAME's headers are on the include path:

```cpp
#include "chd.h"
#include "cdrom.h"

chd_file chd;
if (auto err = chd.open("disc.chd"))
	throw std::system_error(err.value(), err.category());
```

This also gives access to CHD *creation* (`chd_file_compressor`) — see
`tests/selftest.cpp`, which compresses data with every codec exactly the way
`chdman` does.

## Embedding in an emulator (PPSSPP and others)

See [docs/PPSSPP.md](docs/PPSSPP.md) for a drop-in replacement of PPSSPP's
libchdr-based `CHDFileBlockDevice`, including reusing PPSSPP's own zlib and
zstd.

## Layout

```
include/chdmodern/chdmodern.h   public C API
src/chdmodern.cpp               C API implementation
src/osd_portable.cpp            portable replacement for MAME's OSD layer
src/mame/util, src/mame/osd     MAME sources (imported, do not edit by hand)
patches/                        local fixes re-applied on every import
third_party/                    zlib, zstd, LZMA SDK, libFLAC, utf8proc (from MAME's 3rdparty)
tools/                          sync script, VS project generator, example tools
msvc/                           Visual Studio solution and projects
tests/                          self-test
```

## Verification

Besides the self-test, CHDs produced by this library were cross-checked hunk
by hunk (and by overall SHA-1) against [libchdr](https://github.com/rtissera/libchdr),
an independent implementation of the format, for every codec including the
CD codecs.

Known upstream behaviour, unrelated to reading: MAME's multi-threaded
*compressor* hands work-item state between threads through `volatile` fields,
which ThreadSanitizer reports as data races; the LZMA SDK and MAME's FLAC
wrapper do deliberate unaligned stores while compressing. Decompression, which
is all an emulator needs, is single-threaded and clean under ASan, UBSan and
TSan.

## License

BSD-3-Clause, like the MAME sources it is built from. Bundled third-party
libraries keep their own (permissive) licenses; see [LICENSE](LICENSE).

# Local patches to MAME sources

`src/mame/` and `third_party/` are verbatim copies of MAME's code (`tools/sync-from-mame.sh`
re-imports them), except for the patches in this directory, which the sync
script re-applies in order. Except where noted, each one is also a candidate for upstreaming to
MAME, after which it can be deleted here.

| Patch | Why |
|---|---|
| `0001-chdcodec-align-zlib-allocations.patch` | MAME's zlib allocator returned blocks offset by a 4-byte header, i.e. only 4-byte aligned. zlib keeps 8-byte members in them, which is undefined behaviour (flagged by UBSan, can fault on strict-alignment CPUs). The header is now padded to `alignof(std::max_align_t)`. |
| `0002-chd-compress-abort.patch` | Adds `chd_file_compressor::compress_abort()`, which waits for the reads and compressions already queued. Without it a compression can't be stopped early: the queued reads call the derived class's `read_data`, so destroying it mid-compression called a pure virtual function. `chdm_create_dvd()` uses it when its progress callback cancels. |
| `0003-unicode-drop-utf8proc.patch` | Removes `normalize_unicode()` and `uchar_toupper()`/`uchar_tolower()`, the only users of utf8proc. Nothing in the CHD code calls them, but they pulled utf8proc's ~300 KB of Unicode tables into every binary. CHDmodern-specific (MAME uses them elsewhere), so not for upstreaming. |
| `0004-flac-no-neon-intrinsics-on-msvc.patch` | libFLAC's `config.h` (MAME's) enables the NEON intrinsics on every ARM64 compiler, but `lpc_intrin_neon.c` initialises vectors GCC/Clang-style, which MSVC rejects (C2078), so MSVC ARM64 builds failed. They now use FLAC's portable C code. |

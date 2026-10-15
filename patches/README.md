# Local patches to MAME sources

`src/mame/` is a verbatim copy of MAME's code (`tools/sync-from-mame.sh`
re-imports it), except for the patches in this directory, which the sync
script re-applies in order. Each one is also a candidate for upstreaming to
MAME, after which it can be deleted here.

| Patch | Why |
|---|---|
| `0001-chdcodec-align-zlib-allocations.patch` | MAME's zlib allocator returned blocks offset by a 4-byte header, i.e. only 4-byte aligned. zlib keeps 8-byte members in them, which is undefined behaviour (flagged by UBSan, can fault on strict-alignment CPUs). The header is now padded to `alignof(std::max_align_t)`. |

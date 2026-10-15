#!/usr/bin/env bash
# Re-imports the unmodified MAME sources CHDmodern is built from.
#
# Usage: tools/sync-from-mame.sh /path/to/mame-checkout
#
# Everything under src/mame/ and third_party/ is overwritten with the
# corresponding files from the given MAME checkout. CHDmodern's own code
# (src/osd_portable.cpp, src/chdmodern.cpp, include/) is never touched.
# The few local fixes to MAME code live in patches/ and are re-applied.
# Afterwards, review `git diff`, rebuild and run the tests.

set -euo pipefail

MAME="${1:?usage: $0 /path/to/mame}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$MAME/src/lib/util/chd.cpp" ] || { echo "not a MAME checkout: $MAME" >&2; exit 1; }

copy() { # copy <src-dir> <dst-dir> <files...>
	local src="$1" dst="$2"; shift 2
	mkdir -p "$dst"
	for f in "$@"; do
		mkdir -p "$dst/$(dirname "$f")"
		cp "$src/$f" "$dst/$f"
	done
}

# --- MAME lib/util: chd.cpp and its transitive dependencies ---------------
rm -rf "$ROOT/src/mame"
copy "$MAME/src/lib/util" "$ROOT/src/mame/util" \
	avhuff.cpp avhuff.h bitmap.cpp bitmap.h bitstream.h cdrom.cpp cdrom.h \
	chd.cpp chd.h chdcodec.cpp chdcodec.h corefile.cpp corefile.h \
	corestr.cpp corestr.h coretmpl.h endianness.h flac.cpp flac.h \
	hashing.cpp hashing.h huffman.cpp huffman.h ioprocs.cpp ioprocs.h \
	ioprocsfill.h md5.cpp md5.h multibyte.h palette.cpp palette.h \
	path.cpp path.h strformat.cpp strformat.h unicode.cpp unicode.h \
	utilfwd.h vecstream.cpp vecstream.h abi.h

# --- MAME OSD headers (implementation lives in src/osd_portable.cpp) ------
copy "$MAME/src/osd" "$ROOT/src/mame/osd" \
	osdcomm.h osdcore.h osdfile.h strconv.h \
	eminline.h eigcc.h eigccarm.h eigccppc.h eigccx86.h eivc.h eivcarm.h eivcx86.h

# --- third-party libraries, in the versions MAME ships ----------------------
TP="$ROOT/third_party"
rm -rf "$TP"

copy "$MAME/3rdparty/zlib" "$TP/zlib" LICENSE README \
	adler32.c compress.c crc32.c crc32.h deflate.c deflate.h gzguts.h \
	infback.c inffast.c inffast.h inffixed.h inflate.c inflate.h \
	inftrees.c inftrees.h trees.c trees.h uncompr.c zconf.h zlib.h zutil.c zutil.h

copy "$MAME/3rdparty/zstd" "$TP/zstd" LICENSE README.md
(cd "$MAME/3rdparty/zstd" && find lib/common lib/compress lib/decompress -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \)) |
	while read -r f; do copy "$MAME/3rdparty/zstd" "$TP/zstd" "$f"; done
copy "$MAME/3rdparty/zstd" "$TP/zstd" lib/zstd.h lib/zstd_errors.h lib/zdict.h

copy "$MAME/3rdparty/lzma" "$TP/lzma" \
	C/7zTypes.h C/7zWindows.h C/Compiler.h C/CpuArch.c C/CpuArch.h \
	C/LzFind.c C/LzFind.h C/LzFindOpt.c C/LzHash.h C/LzmaDec.c C/LzmaDec.h \
	C/LzmaEnc.c C/LzmaEnc.h C/Precomp.h

copy "$MAME/3rdparty/flac" "$TP/flac" COPYING.Xiph AUTHORS
(cd "$MAME/3rdparty/flac" && find include/FLAC include/share src/libFLAC/include src/libFLAC/deduplication -type f \( -name '*.h' -o -name '*.c' \)) |
	while read -r f; do copy "$MAME/3rdparty/flac" "$TP/flac" "$f"; done
copy "$MAME/3rdparty/flac" "$TP/flac" \
	src/libFLAC/bitmath.c src/libFLAC/bitreader.c src/libFLAC/bitwriter.c \
	src/libFLAC/cpu.c src/libFLAC/crc.c src/libFLAC/fixed.c \
	src/libFLAC/fixed_intrin_avx2.c src/libFLAC/fixed_intrin_sse2.c \
	src/libFLAC/fixed_intrin_sse42.c src/libFLAC/fixed_intrin_ssse3.c \
	src/libFLAC/float.c src/libFLAC/format.c src/libFLAC/lpc.c \
	src/libFLAC/lpc_intrin_avx2.c src/libFLAC/lpc_intrin_fma.c \
	src/libFLAC/lpc_intrin_neon.c src/libFLAC/lpc_intrin_sse2.c \
	src/libFLAC/lpc_intrin_sse41.c src/libFLAC/md5.c src/libFLAC/memory.c \
	src/libFLAC/stream_decoder.c src/libFLAC/stream_encoder.c \
	src/libFLAC/stream_encoder_framing.c src/libFLAC/stream_encoder_intrin_avx2.c \
	src/libFLAC/stream_encoder_intrin_sse2.c src/libFLAC/stream_encoder_intrin_ssse3.c \
	src/libFLAC/window.c src/share/win_utf8_io/win_utf8_io.c

copy "$MAME/3rdparty/utf8proc" "$TP/utf8proc" LICENSE.md utf8proc.c utf8proc.h utf8proc_data.c

# --- CHDmodern's small fixes on top of MAME (see patches/README.md) -------
for p in "$ROOT"/patches/*.patch; do
	[ -e "$p" ] || continue
	echo "applying $(basename "$p")"
	# patches use MAME's paths (src/lib/util/...); map them onto src/mame/util
	sed -e 's#^\(---\|+++\) [ab]/src/lib/util/#\1 src/mame/util/#' "$p" |
		patch -d "$ROOT" -p0 --no-backup-if-mismatch --forward
done

rev="$(git -C "$MAME" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "$rev" > "$ROOT/MAME_REVISION"
echo "Imported MAME $rev"

# keep the Visual Studio projects in step with the source lists
if command -v python3 >/dev/null 2>&1; then
	python3 "$ROOT/tools/gen-msvc.py"
else
	echo "note: python3 not found - run tools/gen-msvc.py to update msvc/" >&2
fi

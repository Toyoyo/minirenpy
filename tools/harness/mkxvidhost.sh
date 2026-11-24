#!/bin/bash
#
# Build xvidcore's decode-only subset as a host static library, for avitest.
#
#   tools/harness/mkxvidhost.sh [out.a]
#
# Default output is libs/xvidcore-1.3.7/build/host/libxvidhost.a, which is
# where run.sh looks. Rebuilt only when a source is newer than it, because 26
# translation units at -O2 is several seconds and the suite runs often.
#
# --- why a library and not a list of sources in srcs_for ------------------
#
# Every other harness names its engine sources and run.sh hands them to one
# gcc at -O0. That is right for the engine: -O0 is for the debugger, and the
# engine's files are what is being debugged. It is wrong here twice over.
#
# -O0 would recompile 26 vendor files on every single run of the suite, and
# the figure avitest prints -- decode milliseconds, as a ratio against
# pl_mpeg -- would be measured against an unoptimised decoder and mean
# nothing. So this builds once, at -O2, and caches.
#
# --- what this library is NOT ---------------------------------------------
#
# It is the **pure C** decoder. ARCH_IS_GENERIC, no nasm, no MMX.
#
# That is not a shortcut, it is the only thing possible: xvidcore's SIMD is
# nasm assembling to win32 COFF with OpenWatcom's cdecl naming, and neither
# the assembler output nor the Pentium III instructions it contains belong on
# this host. The same split CLAUDE.md describes for src/yuvmmx.c applies --
# check each half where it can be checked, and say which half ran. avitest
# says so in its own output, so nobody reads its timing line as a target
# figure.
#
# The target library is the other half: libs/xvidcore-1.3.7/build/win32,
# built by Makefile.watcom, with the asm in. The user builds that one.
#
# --- the two defines ------------------------------------------------------
#
# ARCH_IS_GENERIC selects portab.h's portable paths; without an ARCH_IS_*
# define at all portab.h stops with "#error You are trying to compile Xvid
# without defining the architecture type", which is the useful kind of
# refusal. ARCH_IS_64BIT is the host's pointer size and has nothing to do
# with the target's -- the Watcom build passes ARCH_IS_32BIT, which is the
# same reason the engine's own C89 rule exists: the two builds disagree about
# the width of a long and the code has to be right under both.
#
# enc_stubs.c comes from build/win32 and is not vendor code: xvid.c takes the
# addresses of enc_create, enc_encode and enc_destroy unconditionally, so a
# decode-only build does not link without them. It is shared with the Watcom
# build rather than duplicated.

set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XV="$ROOT/libs/xvidcore-1.3.7"
S="$XV/src"
OUT=${1:-$XV/build/host/libxvidhost.a}
OBJ=$(dirname "$OUT")/obj

if [ ! -d "$S" ]; then
    echo "mkxvidhost.sh: no xvidcore at $S" >&2
    exit 1
fi

# The decode-only subset, the same list Makefile.watcom compiles, minus the
# asm. Kept in this order so a diff against that file is readable.
SRCS="
    xvid.c
    decoder.c
    bitstream/bitstream.c
    bitstream/cbp.c
    bitstream/mbcoding.c
    dct/idct.c
    dct/fdct.c
    motion/gmc.c
    motion/motion_comp.c
    motion/estimation_common.c
    motion/sad.c
    image/colorspace.c
    image/image.c
    image/interpolate8x8.c
    image/qpel.c
    image/postprocessing.c
    image/font.c
    utils/mem_align.c
    utils/emms.c
    utils/mbtransquant.c
    utils/mem_transfer.c
    prediction/mbprediction.c
    quant/quant_mpeg.c
    quant/quant_matrix.c
    quant/quant_h263.c
"

# Up to date? Compare against every source and against this script, so that a
# change to the flags below forces a rebuild too.
if [ -f "$OUT" ]; then
    stale=0
    [ "$0" -nt "$OUT" ] && stale=1
    for f in $SRCS; do
        [ "$S/$f" -nt "$OUT" ] && stale=1
    done
    [ "$XV/build/win32/enc_stubs.c" -nt "$OUT" ] && stale=1
    [ $stale -eq 0 ] && exit 0
fi

mkdir -p "$OBJ"
rm -f "$OBJ"/*.o "$OUT"

# -w: it is somebody else's code, and -Wall on it is noise that would bury a
# warning from ours. Same reasoning as the Makefile's -w1 on plmimpl.c.
CFLAGS="-O2 -w -DNDEBUG -DARCH_IS_GENERIC -DARCH_IS_64BIT
        -I$S -I$S/bitstream -I$S/dct -I$S/image -I$S/motion
        -I$S/prediction -I$S/quant -I$S/utils"

for f in $SRCS; do
    gcc $CFLAGS -c "$S/$f" -o "$OBJ/$(basename "$f" .c).o"
done
gcc $CFLAGS -c "$XV/build/win32/enc_stubs.c" -o "$OBJ/enc_stubs.o"

ar rcs "$OUT" "$OBJ"/*.o
echo "built $OUT"

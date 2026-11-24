/* mp3impl.c - the one translation unit that instantiates minimp3.
 *
 * minimp3 is written in C99 (declarations after statements), so this file is
 * the only one compiled with -za99; everything else stays C89. Keeping the
 * implementation isolated also means the decoder's many static tables are
 * emitted exactly once.
 *
 * SIMD is disabled: the target is a Pentium III, but Open Watcom does not
 * provide the SSE intrinsics minimp3 reaches for, and the scalar path decodes
 * 44.1kHz stereo comfortably on that hardware.
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#define MINIMP3_ONLY_MP3     /* the game ships nothing but MPEG-1 Layer III */

#include "minimp3.h"

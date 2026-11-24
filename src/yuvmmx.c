/* yuvmmx.c - the MMX inner loop for YUV 4:2:0 -> 0xAARRGGBB at 1:1.
 *
 * Compiled only when MR_YUV_MMX is defined, which the two Watcom makefiles do
 * and the host harness build never does. See yuvmmx.h for the arithmetic and
 * for how it is checked, and yuv.h for why this is the loop that got
 * hand-written when the decoder's hotter ones did not.
 *
 * **This file cannot be compiled, let alone run, on the build host.** It is
 * Open Watcom inline assembly. Everything that *can* be checked without it
 * has been pulled out so that it is: the one piece of novel arithmetic lives
 * in yuvmmx.h and yuvtest proves it exhaustively over all 256 luma values,
 * and probe/mmxcheck.c compares this against the C path frame by frame on the
 * target. mmxcheck passed on the real machine under DOS on 2026-11-03, so the
 * transcription is confirmed and not merely compiled; `simd=0` in mrpy.ini
 * remains the way out if a particular card or extender ever argues.
 *
 * --- what it does, four pixels at a time ----------------------------------
 *
 * The split is per-pixel work in MMX, per-chroma work in scalar C, and
 * yuvmmx.h argues the case for drawing the line there. Concretely, for each
 * group of four pixels:
 *
 *   scalar   two 4:2:0 chroma samples cover four luma pixels, so two sets of
 *            r/g/b offsets are computed with the same three multiplies the C
 *            path uses, and written into four-word vectors with each offset
 *            duplicated for the pixel pair that shares it.
 *
 *   MMX      four luma bytes are widened to words, biased by 16, run through
 *            the decomposed multiply, and added to the three offset vectors.
 *            `packuswb` then saturates each component to 0..255 -- which is
 *            the whole of the clamp table, for free and with no memory
 *            traffic -- and three `punpck`s interleave the components
 *            straight into the destination's B,G,R,A byte order.
 *
 * That last step is why the destination format is worth having chosen
 * carefully. mr.h's pixels are 0xAARRGGBB in a DWORD, which on little-endian
 * x86 is the byte sequence B,G,R,A -- so the interleave that MMX does
 * naturally is exactly the layout GDI and the pack loops want, and there is
 * no shuffle at the end.
 *
 * --- emms, and why it is not optional -------------------------------------
 *
 * MMX registers alias the x87 register stack. Leaving this function with the
 * tag word marked in use makes the next floating-point instruction anywhere
 * in the engine produce a wrong answer or a fault -- and the engine is full
 * of them: both makefiles build with -fp5 -fpi87, the mixer's gains are
 * floats, and mr_surface_blur and the transforms are float throughout.
 *
 * The failure would also not look like this file. It would look like the
 * audio going quiet, or a dissolve coming out wrong, some arbitrary distance
 * after a cutscene -- which is the kind of bug that costs a weekend. So
 * `emms` runs once at the end of the row, outside the loop, and the only
 * instructions between the last `movq` and it are integer.
 *
 * Once per row and not once per group: `emms` is not cheap, and nothing
 * between the groups touches the FPU.
 */

#include "mr.h"
#include "yuv.h"
#include "yuvmmx.h"

#ifdef MR_YUV_MMX

/* Constants in memory, because MMX has no immediate form.
 *
 * Eight-byte aligned on purpose. An unaligned `movq` is legal and does not
 * fault, it is merely slower -- but this is the inner loop of the thing the
 * MMX was added for, and a split cache line on every load would give back a
 * good part of what the vectorisation won. The union with a double is the
 * portable way to ask Watcom for 8-byte alignment; there is no alignment
 * attribute in C89. */
typedef union {
    short  w[4];
    mr_u32 d[2];
    double align;
} mmx_const;

static const mmx_const k_black = { { MR_YUV_BLACK, MR_YUV_BLACK,
                                     MR_YUV_BLACK, MR_YUV_BLACK } };
static const mmx_const k_luma  = { { MR_YUV_K_LUMA_LO, MR_YUV_K_LUMA_LO,
                                     MR_YUV_K_LUMA_LO, MR_YUV_K_LUMA_LO } };
static const mmx_const k_alpha = { { -1, -1, -1, -1 } };   /* 0xFF bytes */

/* Scratch for the chroma offsets of one group of four pixels, aligned for
 * the same reason. Module statics rather than locals so the alignment is
 * guaranteed by the union rather than by whatever the stack frame happens to
 * be; nothing here is reentrant, and the engine is single-threaded. */
static mmx_const rv, gv, bv;

/* Convert the 4-pixel groups of one row. Returns the number of pixels done,
 * which is n rounded down to a multiple of four; the caller finishes the tail
 * in C.
 *
 * Returning a count rather than demanding a multiple of four is deliberate.
 * mr_yuv_fit rounds the picture to an even size, not to a multiple of four,
 * so a 638-pixel-wide picture is perfectly reachable -- and a kernel that
 * refused the whole row would silently drop back to scalar for the entire
 * frame on half the window sizes a player might choose. Two pixels of tail
 * cost nothing; a frame of scalar conversion costs a frame.
 */
int mr_yuv_row_mmx(mr_u32 *dst, const mr_u8 *y,
                   const mr_u8 *cb, const mr_u8 *cr, int n)
{
    int x;
    int done = n & ~3;

    for (x = 0; x < done; x += 4) {
        const mr_u8 *yp = y + x;
        mr_u32 *dp = dst + x;
        int c = x >> 1;             /* first of the two chroma samples */
        int i;

        /* The chroma half, in plain C and identical to yuv.c's. Two samples,
         * each shared by the pixel pair beside it. */
        for (i = 0; i < 2; i++) {
            int icb = (int)cb[c + i] - 128;
            int icr = (int)cr[c + i] - 128;
            int r = (icr * MR_YUV_K_R_CR) >> 16;
            int g = (icb * MR_YUV_K_G_CB + icr * MR_YUV_K_G_CR) >> 16;
            int b = (icb * MR_YUV_K_B_CB) >> 16;

            rv.w[i * 2] = rv.w[i * 2 + 1] = (short)r;
            gv.w[i * 2] = gv.w[i * 2 + 1] = (short)g;
            bv.w[i * 2] = bv.w[i * 2 + 1] = (short)b;
        }

        _asm {
            mov       esi, yp
            mov       edi, dp

            pxor      mm7, mm7              ; zero, for the byte widening

            movd      mm0, [esi]            ; Y0..Y3, as bytes
            punpcklbw mm0, mm7              ; ...as four words
            psubw     mm0, k_black          ; Y - 16, may go negative

            ; yt = (Y-16) + (((Y-16) * 10773) >> 16), which yuvmmx.h shows is
            ; exactly ((Y-16) * 76309) >> 16. pmulhw is the >>16.
            movq      mm1, mm0
            pmulhw    mm1, k_luma
            paddw     mm0, mm1              ; yt, four words in -19..278

            movq      mm2, mm0
            paddw     mm2, rv               ; R, unclamped
            movq      mm3, mm0
            psubw     mm3, gv               ; G, unclamped
            movq      mm4, mm0
            paddw     mm4, bv               ; B, unclamped

            ; Saturate to 0..255. This is the clamp table, in one instruction
            ; per component and with no load.
            packuswb  mm2, mm2              ; R0..R3 in the low four bytes
            packuswb  mm3, mm3              ; G
            packuswb  mm4, mm4              ; B

            ; Interleave into B,G,R,A -- which little-endian x86 reads back as
            ; the 0xAARRGGBB a surface holds.
            movq      mm5, k_alpha
            punpcklbw mm4, mm3              ; B0 G0 B1 G1 B2 G2 B3 G3
            punpcklbw mm2, mm5              ; R0 FF R1 FF R2 FF R3 FF
            movq      mm6, mm4              ; keep a copy: punpcklwd clobbers
            punpcklwd mm4, mm2              ; pixels 0 and 1
            punpckhwd mm6, mm2              ; pixels 2 and 3

            movq      [edi], mm4
            movq      [edi + 8], mm6
        }
    }

    /* Once per row, and before anything can touch the FPU. See the header. */
    if (done > 0) {
        _asm {
            emms
        }
    }

    return done;
}

#endif /* MR_YUV_MMX */

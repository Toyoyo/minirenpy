/* blendmmx.c - the MMX inner loop for mr_blit's translucent branch.
 *
 * Compiled only when MR_BLEND_MMX is defined, which the two Watcom makefiles
 * do and the host harness build never does. See blendmmx.h for the arithmetic
 * and for how it is checked.
 *
 * **This file cannot be compiled, let alone run, on the build host.** It is
 * Open Watcom inline assembly. Everything that *can* be checked without it
 * has been pulled out so that it is: the lane arithmetic lives in blendmmx.h
 * and blendtest proves it exhaustively over all 16,777,216 (src, dst, alpha)
 * triples, and probe/mmxcheck.c compares this against the scalar path pixel
 * by pixel on the target. `simd=0` in mrpy.ini is the way out if a particular
 * machine ever argues.
 *
 * --- why this loop and not the others -------------------------------------
 *
 * Measured on the target, the textbox panel blend ran at 129.7 ns/px over
 * 148,000 pixels -- 19.2ms, on every frame that is not a cached dialogue band
 * -- and 144,076 of those pixels are at partial alpha, so neither of
 * mr_blit's fast branches applies to them. It was the most expensive blit per
 * pixel in the engine and the only compositor loop left that was compute-bound
 * rather than memory-bound: the floor for its 12 bytes a pixel is ~45-52
 * ns/px, from `memcpy` at ~30 over 8 bytes and `mr_lerp_surface` at ~52 over
 * the same 12.
 *
 * **It now runs at 81.4, so the panel costs 12.1ms and a full frame is 7.1ms
 * cheaper.** That is -37% on the loop and about -6% on a still frame, and
 * nothing at all on a typing frame, where the band cache skips the panel
 * anyway. `probe/blendchk.c` passed on the machine, so the pixels are right
 * and not merely plausible.
 *
 * Everything else was measured and left alone. Sprite blends already ran at
 * 47.9-49.0 ns/px, which is that floor, because the `a == 0` and `a == 255`
 * branches skip the arithmetic on most of a bounding box -- and the shipped
 * kernel keeps them there at a measured 49.5, after a first version that did
 * not and cost them 20%. `scale` and `pack` are memory-bound too. NOTES.md
 * has all three rounds of figures under "What the MMX blend bought, over
 * three rounds".
 *
 * --- one pixel per iteration, not four ------------------------------------
 *
 * A pixel unpacked to 16-bit lanes is four words, which fills a 64-bit MMX
 * register exactly -- so the parallelism here is across the four *channels*,
 * not across pixels. (An earlier note claimed two pixels fit a register; they
 * do not.)
 *
 * The original reason given here for one at a time was that more "would buy
 * only loop overhead, on a loop whose ceiling is memory bandwidth either way".
 * **Measured, that was wrong.** The shipped kernel runs at 81.4 ns/px against
 * a ~45-52 floor, so it is not bandwidth-bound: the limit is the *latency* of
 * its own dependency chain. From punpcklbw to packuswb is nine dependent
 * operations and pmullw alone is 3 cycles on a P6, and one pixel per iteration
 * exposes all of it with no second chain to interleave against. Two pixels
 * would hide most of it, worth perhaps 4ms more on a full frame.
 *
 * It stays at one deliberately. There are no further measurements available on
 * this target, and the one regression this kernel has caused so far came from
 * exactly that -- changing the loop and reasoning about the result instead of
 * timing it. See NOTES.md, "An _asm block changes the cost of the C around
 * it".
 *
 * That choice is also what lets the two early-outs stay exactly as the scalar
 * loop has them, which blendmmx.h argues is worth more than the pixels it
 * costs: the kernel is a drop-in for the inner loop, pixel for pixel, so
 * mmxcheck is comparing like with like and a sprite does not get slower.
 *
 * --- emms, and why it is not optional -------------------------------------
 *
 * MMX registers alias the x87 register stack. Leaving this function with the
 * tag word marked in use makes the next floating-point instruction anywhere
 * in the engine produce a wrong answer or a fault -- and the engine is full
 * of them: both makefiles build with -fp5 -fpi87, the mixer's gains are
 * floats, and mr_surface_blur and the transforms are float throughout. The
 * failure would not look like this file, either; it would look like the audio
 * going quiet or a dissolve coming out wrong, an arbitrary distance away.
 *
 * So `emms` runs once per call, and only when the vector path was actually
 * entered -- a row of entirely transparent or entirely opaque pixels touches
 * no MMX register and needs no emms. yuvmmx.c does the same thing with the
 * same reasoning.
 */

#include "mr.h"
#include "blendmmx.h"

#ifdef MR_BLEND_MMX

/* Constants in memory, because MMX has no immediate form.
 *
 * Eight-byte aligned on purpose: an unaligned `movq` is legal and merely
 * slower, but this is the inner loop the MMX was added for and a split cache
 * line on every load would give back a good part of what it won. The union
 * with a double is the portable way to ask Watcom for 8-byte alignment; there
 * is no alignment attribute in C89. */
typedef union {
    short  w[4];
    mr_u32 d[2];
    double align;
} mmx_const;

/* 255 in every lane, for turning alpha into 255-alpha with one psubw. */
static const mmx_const k_255 = { { 255, 255, 255, 255 } };

/* The MR_DIV255 rounding bias, in every lane. */
static const mmx_const k_128 = { { 128, 128, 128, 128 } };

/* 255 in the alpha lane only, and zero in the others.
 *
 * mr.h's pixels are 0xAARRGGBB in a DWORD, which on little-endian x86 is the
 * byte sequence B,G,R,A -- so after `punpcklbw` with zero the alpha is word
 * three, the highest. `por` with this sets that lane to 255 and leaves the
 * colour lanes alone, which is what MR_ARGB(255, ...) does in the scalar
 * loop. It works as an OR rather than needing a mask because every lane is
 * already in 0..255 by then, so OR-ing 255 into one of them yields exactly
 * 255. */
static const mmx_const k_amask = { { 0, 0, 0, 255 } };

void mr_blend_row_mmx(mr_u32 *dp, const mr_u32 *sp, int n)
{
    int x;
    int touched = 0;

    for (x = 0; x < n; x++) {
        mr_u32 s = sp[x];
        unsigned a = MR_A(s);

        /* The two early-outs, byte for byte as gfx.c has them. blendmmx.h
         * explains why they are not folded into the vector path: `a == 0`
         * must leave the destination's alpha untouched, and nothing beats a
         * store for `a == 255`. */
        if (a == 0)
            continue;
        if (a == 255) {
            dp[x] = s;
            continue;
        }

        /* The two pointers the _asm block needs, declared *here* and not at
         * the top of the loop, which is not a style choice.
         *
         * Watcom has to materialise any local an _asm block names into a
         * stack slot -- it cannot know the assembly will not take its
         * address. Computed at the top of the loop they cost two address
         * calculations and two stores on *every* pixel, including the ones
         * the early-outs then throw away. A sprite is mostly such pixels, and
         * measured that is what it cost: an A/B of the same binary with
         * simd=0 put the sprite blend at ~49 ns/px scalar against ~59 with
         * the kernel, a 20% regression on the path this change was supposed
         * to leave alone, while the panel it was written for went 129.7 ->
         * 81.5 -- a 20% regression on the path this file's own header
         * asserted it would leave alone.
         *
         * Narrowed to here, with the alpha re-read below rather than handed
         * over in `a`, the skip path is load/shift/mask/branch in registers,
         * within an instruction or two of gfx.c's own. **Confirmed on the
         * target at 49.5 ns/px** -- scalar parity, and in one batch identical
         * to the scalar log in both milliseconds and pixel count. */
        {
        mr_u32 *d = dp + x;
        const mr_u32 *q = sp + x;

        touched = 1;

        _asm {
            mov       esi, q
            mov       edi, d

            ; The alpha re-read here rather than handed over in `a`, for the
            ; reason the two pointers are declared where they are: a local an
            ; _asm block names has to live in a stack slot, and `a` is tested
            ; by the early-outs on every pixel. Naming it cost a store and a
            ; memory compare per *skipped* pixel, which is most of a sprite.
            ; The source pixel is in L1 already -- it was loaded a few
            ; instructions ago -- so this load is cheaper than the slot.
            mov       eax, [esi]
            shr       eax, 18H              ; alpha, 1..254

            pxor      mm7, mm7              ; zero, for the byte widening

            ; Alpha into all four lanes without going through memory. The
            ; obvious spelling -- four 16-bit stores to an aligned scratch,
            ; then one movq -- is a narrow-store/wide-load pair that cannot
            ; store-forward on a P6, which is the same stall that made mr.h's
            ; channel macros masks instead of casts (NOTES.md, "The channel
            ; macros cost three store-forwarding stalls per pixel"). Three
            ; register ops are cheaper than one load that waits.
            movd      mm2, eax              ; [a, 0, 0, 0]
            punpcklwd mm2, mm2              ; [a, a, 0, 0]
            punpckldq mm2, mm2              ; [a, a, a, a]

            movq      mm3, k_255
            psubw     mm3, mm2              ; 255-a in all four lanes

            movd      mm0, [esi]            ; source pixel, as bytes
            movd      mm1, [edi]            ; destination pixel
            punpcklbw mm0, mm7              ; ...as four words, B G R A
            punpcklbw mm1, mm7

            pmullw    mm0, mm2              ; s*a        <= 65025
            pmullw    mm1, mm3              ; d*(255-a)  <= 65025
            paddw     mm0, mm1              ; sum        <= 65025

            ; MR_DIV255: t = x + 128; (t + (t >> 8)) >> 8. blendmmx.h shows
            ; every step stays under 65536, which is the whole reason this is
            ; allowed to happen in 16-bit lanes.
            paddw     mm0, k_128            ; t          <= 65153
            movq      mm1, mm0
            psrlw     mm1, 8
            paddw     mm0, mm1              ; t + t>>8   <= 65407
            psrlw     mm0, 8                ; 0..255 per lane

            por       mm0, k_amask          ; alpha = 255, as MR_ARGB does
            packuswb  mm0, mm0              ; back to B,G,R,A bytes
            movd      [edi], mm0
        }
        }
    }

    /* Once per call, and before anything can touch the FPU. See the header.
     * Skipped when no pixel took the vector path, because then no MMX
     * register was written and the x87 tag word is untouched. */
    if (touched) {
        _asm {
            emms
        }
    }
}

#endif /* MR_BLEND_MMX */

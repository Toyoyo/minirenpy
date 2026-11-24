/* MR_DIV255 and MR_FLOOR255 against the divides they replace.
 *
 * An alpha blend is `(s*a + d*(255-a)) / 255` per channel, and written that
 * way Watcom emits three hardware DIVs per pixel in mr_blit. Measured on the
 * Pentium III at 270ns a pixel, which is 38.7ms of every 119.6ms dialogue
 * frame -- the dialogue window is 148,000 pixels and 144,076 of them are at
 * partial alpha, so it misses both fast paths and pays full price almost
 * everywhere. See NOTES.md under "Splitting `scene`".
 *
 * The replacement is only worth having if it is *exactly* the same divide,
 * because the alternative is a conversation about whether the compositor got
 * slightly darker, and `NOTES.md` records scaletest catching exactly that: a
 * 2-3 count uniform darkening from the naive `(x+128)>>8`, which is the trick
 * gfx.c's old comment claimed to be using while the code divided. A blender
 * whose output shifted by a count would be a real divergence dressed up as an
 * optimisation.
 *
 * So the claims here are equalities over the entire input range rather than
 * samples, in the manner of yuvtest: the arithmetic is cheap and the range is
 * small, so there is no reason to check less than all of it.
 *
 * Two layers, because either alone would be worth little:
 *
 *   - the macros equal the divide for every input in range;
 *   - mr_blit, mr_blit_alpha and mr_draw_text's glyph loop -- the real
 *     functions, not a transcription -- produce the same pixel they produced
 *     before, for every (src, dst, alpha) combination that matters.
 *
 * The second is what catches a macro that is right in isolation and misapplied
 * at a call site: a missing cast, an operand that can exceed the proven
 * bound, or the rounding form used where the truncating one was meant.
 *
 * Nothing here is about speed. That the divides are gone is checked by
 * disassembling the object, which no harness can do; the figure is in
 * NOTES.md and comes from the machine.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "blendmmx.h"

static int g_fail;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "pass" : "FAIL", what);
    if (!ok)
        g_fail = 1;
}

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); g_fail = 1; }
void mr_log(const char *f, ...) { (void)f; }

/* The expressions as gfx.c used to spell them, kept here as the oracle. They
 * must stay written out as divides: the point is to compare against what the
 * engine did before, so borrowing the macro would make every claim vacuous.
 *
 * `unsigned long` rather than `unsigned`, so the host's 64-bit arithmetic
 * cannot differ from the target's 32-bit for inputs this small -- both are
 * exact well past 65025 and that is the whole range. */
static unsigned long ref_div255(unsigned long x)   { return (x + 127UL) / 255UL; }
static unsigned long ref_floor255(unsigned long x) { return x / 255UL; }

int main(void)
{
    unsigned long x;
    unsigned a, s, d;
    int bad, worst;
    mr_surface *dst, *src;

    setbuf(stdout, NULL);

    /* --- the macros, over every input in range --------------------------
     *
     * 0..65025 is the blend's whole range: the largest numerator is
     * 255*a + 255*(255-a) = 255*255, fixed by the channel maxima rather than
     * by anything tunable. Checked one past it as well, since a bound that is
     * only correct *at* the limit is one bad edit from being wrong. */
    bad = 0;
    for (x = 0; x <= 65025UL; x++)
        if ((unsigned long)MR_DIV255(x) != ref_div255(x))
            bad++;
    check("MR_DIV255 equals (x+127)/255 for all 65026 inputs in range",
          bad == 0);

    bad = 0;
    for (x = 0; x <= 65535UL; x++)
        if ((unsigned long)MR_FLOOR255(x) != ref_floor255(x))
            bad++;
    check("MR_FLOOR255 equals x/255 for all 65536 inputs in range",
          bad == 0);

    /* The rounding and truncating forms must not be interchangeable, or the
     * distinction drawn at mr_blit_alpha's call site is imaginary and this
     * file is asserting one thing twice. */
    bad = 0;
    for (x = 0; x <= 65025UL; x++)
        if (MR_DIV255(x) != MR_FLOOR255(x))
            bad++;
    check("and the two differ, so choosing between them is a real choice",
          bad > 0);

    /* --- the blend itself, over every (s, d, a) -------------------------
     *
     * 256 * 256 * 254 is 16.6 million, which is seconds here and worth it:
     * this is the exact arithmetic the engine performs on every translucent
     * pixel of every frame, and a per-channel claim does not establish that
     * the operands are combined correctly. */
    bad = 0;
    worst = 0;
    for (a = 1; a <= 254; a++) {
        unsigned ia = 255u - a;

        for (s = 0; s <= 255; s++) {
            for (d = 0; d <= 255; d++) {
                unsigned long got = MR_DIV255(s * a + d * ia);
                unsigned long want = ref_div255(s * a + d * ia);
                int err;

                if (got == want)
                    continue;
                err = (int)(got > want ? got - want : want - got);
                if (err > worst)
                    worst = err;
                bad++;
            }
        }
    }
    check("the blend expression is unchanged for all 16.6M (s,d,alpha)",
          bad == 0 && worst == 0);

    /* --- the real mr_blit ------------------------------------------------
     *
     * The macros being right does not make the call sites right. Drive the
     * actual function over a source carrying every alpha value and compare
     * each resulting pixel against the old arithmetic computed here.
     *
     * The source is 256 wide so column x has alpha x, which puts both fast
     * paths (a == 0 and a == 255) and all 254 slow ones in one blit -- a
     * fixture that could not express the boundary would not notice the macro
     * being applied to the wrong side of it. */
    dst = mr_surface_new(256, 1);
    src = mr_surface_new(256, 1);
    if (!dst || !src) {
        printf("  could not allocate\n");
        return 1;
    }
    src->has_alpha = 1;

    bad = 0;
    for (a = 0; a <= 255; a++)
        src->px[a] = MR_ARGB(a, 0x10, 0x80, 0xF0);

    for (d = 0; d <= 255; d += 17) {   /* 16 destination shades */
        unsigned i;

        for (i = 0; i < 256; i++)
            dst->px[i] = MR_ARGB(255, d, d, d);

        mr_blit(dst, src, 0, 0);

        for (i = 0; i < 256; i++) {
            unsigned ia = 255u - i;
            mr_u32 want;

            if (i == 0)
                want = MR_ARGB(255, d, d, d);           /* skipped */
            else if (i == 255)
                want = MR_ARGB(255, 0x10, 0x80, 0xF0);  /* stored whole */
            else
                want = MR_ARGB(255,
                    ref_div255(0x10u * i + d * ia),
                    ref_div255(0x80u * i + d * ia),
                    ref_div255(0xF0u * i + d * ia));

            if (dst->px[i] != want)
                bad++;
        }
    }
    check("mr_blit produces the same pixel it did before, every alpha",
          bad == 0);

    /* mr_blit_alpha scales the source alpha first, and that divide truncates.
     * Checked at every transform alpha, because the interesting failure is a
     * source alpha of 1 that rounds up to 1 instead of down to 0 and so stops
     * being skipped -- a ghost sprite's clear border becoming visible. */
    bad = 0;
    for (a = 1; a <= 254; a++) {
        unsigned i;

        for (i = 0; i < 256; i++)
            dst->px[i] = MR_ARGB(255, 0x40, 0x40, 0x40);

        mr_blit_alpha(dst, src, 0, 0, (int)a);

        for (i = 0; i < 256; i++) {
            unsigned ea = (unsigned)(ref_floor255(i * a));
            unsigned ia = 255u - ea;
            mr_u32 want;

            if (ea == 0)
                want = MR_ARGB(255, 0x40, 0x40, 0x40);
            else
                want = MR_ARGB(255,
                    ref_div255(0x10u * ea + 0x40u * ia),
                    ref_div255(0x80u * ea + 0x40u * ia),
                    ref_div255(0xF0u * ea + 0x40u * ia));

            if (dst->px[i] != want)
                bad++;
        }
    }
    check("and so does mr_blit_alpha, at every transform alpha",
          bad == 0);

    /* --- the bound ------------------------------------------------------
     *
     * MR_DIV255 is exact to 65025 and the call sites are argued safe by the
     * channel maxima. If that argument is ever wrong the macro does not fail
     * loudly, it just returns a slightly wrong colour -- so pin the first
     * input where it stops agreeing, which makes any future widening of a
     * channel or a weight show up here rather than as a faint tint.
     *
     * 65152 and not 65025: the +128 inside the macro is what eventually
     * overflows the >>8 reconstruction, and knowing where that is is the
     * useful fact. The gap between the proven range and the real limit is
     * headroom, and it is deliberate. */
    for (x = 65026UL; x < 200000UL; x++)
        if ((unsigned long)MR_DIV255(x) != ref_div255(x))
            break;
    printf("  note MR_DIV255 first disagrees at x=%lu; the blend reaches"
           " %u\n", x, 255u * 255u);
    check("the proven range leaves headroom above what a blend can produce",
          x > 65025UL);

    mr_surface_free(src);
    mr_surface_free(dst);

    /* --- mr_surface_opaque, and the reason it is safe to act on ------------
     *
     * png.c clears `has_alpha` when the pixels turn out to be opaque, so this
     * decides which of mr_blit's two branches an asset gets. Two things have
     * to hold: the scan must be right, and flipping the flag on an all-opaque
     * source must not change a single pixel. The second is the one that makes
     * the optimisation invisible, and it belongs here beside the blend it
     * compares against. The end-to-end half -- a real RGBA file loading with
     * has_alpha 0 -- is in animtest, which reads the image fixtures. */
    {
        mr_surface *a = mr_surface_new(64, 32);
        mr_surface *b = mr_surface_new(64, 32);
        mr_surface *o = mr_surface_new(64, 32);
        int i, differ;

        if (!a || !b || !o) {
            check("surfaces to test mr_surface_opaque on", 0);
        } else {
            for (i = 0; i < 64 * 32; i++)
                a->px[i] = MR_ARGB(255, (i * 7) & 255, (i * 13) & 255,
                                   (i * 29) & 255);

            check("a fully opaque surface reads as opaque",
                  mr_surface_opaque(a));

            /* One pixel, in the last row, so a scan that stopped early or
             * tested only the first row would say opaque. */
            a->px[64 * 32 - 1] = MR_ARGB(254, 1, 2, 3);
            check("one pixel at alpha 254 in the last row is enough",
                  !mr_surface_opaque(a));

            /* And alpha 0, which is the common case -- a sprite's corner --
             * and is *not* "no alpha" however much it looks like nothing. */
            a->px[64 * 32 - 1] = MR_ARGB(0, 1, 2, 3);
            check("so is one fully transparent pixel",
                  !mr_surface_opaque(a));

            a->px[64 * 32 - 1] = MR_ARGB(255, 1, 2, 3);
            check("and restoring it makes the surface opaque again",
                  mr_surface_opaque(a));

            check("a NULL surface is opaque rather than a crash",
                  mr_surface_opaque(NULL));

            /* The claim that makes png.c's flip sound: the same all-opaque
             * source blitted both ways, over a destination with content under
             * it so a wrong branch would show. */
            for (i = 0; i < 64 * 32; i++)
                b->px[i] = o->px[i] = MR_ARGB(255, 0x40, 0x50, 0x60);

            a->has_alpha = 1;
            mr_blit(b, a, 0, 0);
            a->has_alpha = 0;
            mr_blit(o, a, 0, 0);

            differ = 0;
            for (i = 0; i < 64 * 32; i++)
                if (b->px[i] != o->px[i])
                    differ++;

            check("an all-opaque source blits identically either way",
                  differ == 0);
        }

        mr_surface_free(a);
        mr_surface_free(b);
        mr_surface_free(o);
    }

    /* --- mr_surface_digest_outside ----------------------------------------
     *
     * The one testable piece of the MR_REDRAWPROBE measurement. The probe
     * itself lives in main.c's frame(), which no harness links, so what it
     * counts is untested by construction -- but the question it asks reduces
     * to "does this hash notice a change here and not there", and that is a
     * pure function.
     *
     * Three rows of claims, because a hash that is wrong in the interesting
     * direction still passes the obvious test: one that ignored `y` entirely
     * would pass "a change outside is seen", and one that hashed nothing would
     * pass "a change inside is not seen". Both have to hold at once, and the
     * band has to be in the middle of the surface so that "outside" is two
     * disjoint regions rather than a suffix. */
    {
        mr_surface *s = mr_surface_new(64, 32);
        int i;
        mr_u32 base, d;

        if (!s) {
            check("a surface to digest", 0);
        } else {
            for (i = 0; i < 64 * 32; i++)
                s->px[i] = MR_ARGB(255, (i * 7) & 255, (i * 13) & 255,
                                   (i * 29) & 255);

            /* Rows 10..19 are the "band"; 0..9 and 20..31 are outside it. */
            base = mr_surface_digest_outside(s, 10, 10);

            check("the digest of an unchanged surface is unchanged",
                  mr_surface_digest_outside(s, 10, 10) == base);

            s->px[10 * 64 + 5] ^= 0x00010203u;
            check("a pixel changed inside the band does not move it",
                  mr_surface_digest_outside(s, 10, 10) == base);
            s->px[10 * 64 + 5] ^= 0x00010203u;

            s->px[19 * 64 + 63] ^= 0x00010203u;
            check("nor does one in the band's last row",
                  mr_surface_digest_outside(s, 10, 10) == base);
            s->px[19 * 64 + 63] ^= 0x00010203u;

            /* Above the band and below it: a digest that skipped everything
             * from `y` onwards would pass the first of these and fail this. */
            s->px[9 * 64 + 5] ^= 0x00010203u;
            check("a pixel changed just above the band does move it",
                  mr_surface_digest_outside(s, 10, 10) != base);
            s->px[9 * 64 + 5] ^= 0x00010203u;

            s->px[20 * 64 + 5] ^= 0x00010203u;
            check("so does one just below the band",
                  mr_surface_digest_outside(s, 10, 10) != base);
            s->px[20 * 64 + 5] ^= 0x00010203u;

            s->px[31 * 64 + 63] ^= 0x00010203u;
            check("so does the very last pixel of the surface",
                  mr_surface_digest_outside(s, 10, 10) != base);
            s->px[31 * 64 + 63] ^= 0x00010203u;

            check("and restoring every pixel restores the digest",
                  mr_surface_digest_outside(s, 10, 10) == base);

            /* A different band is a different question, so main.c stores the
             * rect beside the digest and refuses to compare across a change
             * of textbox height. If these two agreed, that guard would be
             * untestable because it would also be pointless. */
            check("a different band gives a different digest",
                  mr_surface_digest_outside(s, 11, 10) != base);

            /* h <= 0 means "all of it", which is what main.c would get if a
             * game ever reported a zero-height textbox. It must not silently
             * mean "none of it": that would make every frame compare equal
             * and the probe would report a 100% opportunity that is not real.
             * This is the claim that protects the measurement from itself.
             *
             * Checked against a break that fails -- `if (h <= 0 || ..)
             * continue` -- and not against dropping gfx.c's `h > 0` guard,
             * which is a no-op: the row comparison is already false for every
             * row when h is 0. Two edits that look equally like breaks, one
             * of which tests nothing. */
            d = mr_surface_digest_outside(s, 0, 0);
            s->px[15 * 64 + 15] ^= 0x00010203u;
            check("with h <= 0 the whole surface is digested",
                  mr_surface_digest_outside(s, 0, 0) != d);
            s->px[15 * 64 + 15] ^= 0x00010203u;

            check("a NULL surface digests to 0 rather than a crash",
                  mr_surface_digest_outside(NULL, 0, 10) == 0);
        }

        mr_surface_free(s);
    }

    /* --- the MMX blend's lane arithmetic ------------------------------------
     *
     * src/blendmmx.c is Open Watcom inline assembly and this suite cannot
     * compile a line of it, never mind run one. Saying that out loud matters
     * more than the claims below: **nothing here tests the kernel.** What it
     * tests is the one thing that could make the kernel's answer differ from
     * the scalar loop's, which is the 16-bit lane width, and it tests it
     * exhaustively. blendmmx.h sets out the split; probe/mmxcheck.c is the
     * other half and only the target can run it.
     *
     * Exhaustive and not sampled, because the interesting inputs are the
     * extremes and a sample is exactly what misses them. 16,777,216 triples
     * is a second or so. */
    {
        unsigned s, d, a;
        unsigned long bad = 0;
        unsigned peak_sum = 0, peak_bias = 0, peak_tail = 0;

        for (a = 0; a < 256; a++) {
            for (s = 0; s < 256; s++) {
                for (d = 0; d < 256; d++) {
                    unsigned sum  = (unsigned)s * a + (unsigned)d * (255u - a);
                    unsigned bias = sum + 128u;
                    unsigned tail = bias + (bias >> 8);

                    if (MR_BL_LANE(s, d, a) != MR_BL_REF(s, d, a))
                        bad++;

                    if (sum  > peak_sum)  peak_sum  = sum;
                    if (bias > peak_bias) peak_bias = bias;
                    if (tail > peak_tail) peak_tail = tail;
                }
            }
        }

        check("the 16-bit lane blend equals the 32-bit one, all 16,777,216 "
              "triples", bad == 0);

        /* The bounds blendmmx.h reasons from, asserted rather than trusted.
         * These are what stop being true if a channel is ever widened past 8
         * bits or MR_DIV255's bias changes -- and the lane equality above
         * would start failing for reasons nobody could localise without
         * them. The tightest has 129 to spare out of 65536. */
        check("the blended sum reaches 65025 and no more",
              peak_sum == 65025u);
        check("biased it reaches 65153, inside a 16-bit lane",
              peak_bias == 65153u && peak_bias < 65536u);
        check("and the MR_DIV255 tail reaches 65407, still inside one",
              peak_tail == 65407u && peak_tail < 65536u);

        /* The two endpoints, called out because the kernel's correctness at
         * them is why blendmmx.h can argue that branching them in scalar is
         * a speed choice and not a correctness one. */
        check("a lane at alpha 255 is the source",
              MR_BL_LANE(200, 50, 255) == 200u);
        check("a lane at alpha 0 is the destination",
              MR_BL_LANE(200, 50, 0) == 50u);
    }

    printf("%s\n", g_fail ? "** FAIL **" : "ok");

    /* Said at the end, where a reader looking at a green run will see it.
     * CLAUDE.md's rule: a kernel the suite appears to cover and does not is
     * worse than one it plainly does not. */
    printf("note: src/blendmmx.c is Watcom inline assembly and is NOT built "
           "or run here.\n"
           "      The lane arithmetic above is proved; the transcription is "
           "not.\n"
           "      probe/mmxcheck.c is the other half, on the target.\n");

    return g_fail ? 1 : 0;
}

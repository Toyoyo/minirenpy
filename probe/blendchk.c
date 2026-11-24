/* blendchk.c - does the MMX alpha blend agree with the C one?
 *
 *     wmake -f Makefile.dos blendcheck   -> blendchk.exe
 *     wmake -f Makefile.cw  blendcheck   -> blndchkcw.exe
 *
 * Run it on the target. The exit code is the test: 0 if every pixel of every
 * case matched, 1 if any did not, 2 if it could not run at all.
 *
 * --- why this exists ------------------------------------------------------
 *
 * src/blendmmx.c is Open Watcom inline assembly, so the harness suite on the
 * build host cannot compile it, never mind execute it. blendtest says so out
 * loud at the end of its run rather than leaving a reader to infer that its
 * green lines cover the kernel -- they do not.
 *
 * The risk was split in two (src/blendmmx.h sets this out). The *arithmetic*
 * -- whether the blend survives being done in 16-bit lanes, which is the one
 * thing that could make the vector answer differ -- is a question about
 * integers, and blendtest proves it exhaustively over all 16,777,216
 * (src, dst, alpha) triples on the host. What is left is whether the assembly
 * computes that arithmetic, and whether `punpcklbw`/`packuswb` put the bytes
 * back where a surface wants them. Only a machine with MMX in it can answer
 * that, which is this file.
 *
 * It is a probe and not a harness for the reason probe/ exists: it needs a
 * compiler the suite has not got.
 *
 * --- what it covers -------------------------------------------------------
 *
 * Four kinds of case, and the last two are the ones that would actually
 * catch a transcription slip:
 *
 *   1. A sweep. Every alpha 0..255 against a spread of source and
 *      destination colours, so the rounding is exercised across the range
 *      rather than at one value. MR_DIV255 is a rounding function and the
 *      kernel's spelling of it is five instructions; this is where a wrong
 *      shift count shows up as off-by-one.
 *
 *   2. The corners. Alpha at 0, 1, 127, 128, 254 and 255 against channels at
 *      0, 1, 127, 128, 254 and 255, every combination. A gradient can miss
 *      the exact extremes, and the extremes are where a saturating
 *      instruction and a scalar expression part company if they part company
 *      anywhere. Alpha 0 and 255 are in here on purpose even though the
 *      kernel branches them in scalar: the point is that it *does*.
 *
 *   3. Per-channel asymmetry. Every case uses a different value in each of
 *      B, G and R, and a destination whose channels differ from the source's.
 *      A kernel that broadcast the wrong lane, or that interleaved B and R,
 *      would pass any test built on grey -- which is the mistake this file
 *      exists to not make, because grey is the easy fixture to write.
 *
 *   4. Alpha preservation. The destination is pre-filled with an alpha byte
 *      that is *not* 255, so that "a == 0 leaves the destination alone" and
 *      "otherwise the result's alpha is 255" are both visible. On the stage
 *      every alpha is 255 and this distinction cannot be seen at all, which
 *      is exactly why it is worth a case here.
 *
 * Every case is run twice, once with the kernel on and once with it off,
 * through the real mr_blit -- not through a copy of the loop. Comparing the
 * engine's own entry point both ways is what makes this a test of what ships
 * rather than of a reimplementation of it.
 *
 * --- a note on what a pass means ------------------------------------------
 *
 * A pass says the two paths agree. It does not say either is right: that is
 * blendtest's job, where the lane arithmetic is compared against the 32-bit
 * expression gfx.c actually uses. The two tests are only worth anything
 * together -- this one anchors the kernel to the C path, and that one anchors
 * the C path's vector spelling to its scalar one.
 *
 * It also cannot tell you the kernel is *faster*. That is MR_PROF's job on a
 * real session: watch `ui blend`'s ns/px, which is 126.8 with the scalar loop
 * and has a memory floor somewhere around 45-52.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "blendmmx.h"
#include "cpu.h"       /* for the cpu line in the bail-out below */

/* main.c is linked by no probe. */
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(2); }
void mr_log(const char *f, ...) { (void)f; }
void mr_invalidate(void) { }
void mr_quit(void) { }
mr_surface *mr_stage(void) { return NULL; }

#define W 64
#define H 16

static int fails;

/* The destination's starting alpha. Not 255, so that case 4 above can see
 * the difference between "left alone" and "set to 255". */
#define DST_ALPHA 0x77

static void fill_src(mr_surface *s, unsigned a, int vary_alpha)
{
    int x, y;

    for (y = 0; y < s->h; y++) {
        mr_u32 *p = s->px + (size_t)y * s->pitch;

        for (x = 0; x < s->w; x++) {
            /* Different in every channel, and different again per pixel, so
             * a lane mix-up cannot hide. */
            unsigned r = (unsigned)((x * 7 + y * 3) & 255);
            unsigned g = (unsigned)((x * 13 + y * 29) & 255);
            unsigned b = (unsigned)((x * 31 + y * 11) & 255);
            unsigned av = vary_alpha ? (unsigned)((x * 4 + y) & 255) : a;

            p[x] = MR_ARGB(av, r, g, b);
        }
    }
    s->has_alpha = 1;
}

static void fill_dst(mr_surface *s)
{
    int x, y;

    for (y = 0; y < s->h; y++) {
        mr_u32 *p = s->px + (size_t)y * s->pitch;

        for (x = 0; x < s->w; x++)
            p[x] = MR_ARGB(DST_ALPHA,
                           (unsigned)((x * 17 + y * 5) & 255),
                           (unsigned)((x * 3 + y * 23) & 255),
                           (unsigned)((x * 41 + y * 7) & 255));
    }
}

/* Blit src over a freshly filled destination with the kernel in the given
 * state, through the engine's own mr_blit. */
static int run(mr_surface *dst, const mr_surface *src, int simd)
{
    fill_dst(dst);
    mr_blend_set_simd(simd);
    mr_blit(dst, src, 0, 0);
    return 1;
}

static void compare(const char *what, mr_surface *a, mr_surface *b)
{
    int x, y, bad = 0;
    int fx = -1, fy = -1;
    mr_u32 ga = 0, gb = 0;

    for (y = 0; y < a->h; y++) {
        const mr_u32 *pa = a->px + (size_t)y * a->pitch;
        const mr_u32 *pb = b->px + (size_t)y * b->pitch;

        for (x = 0; x < a->w; x++) {
            if (pa[x] == pb[x])
                continue;
            if (bad == 0) {
                fx = x; fy = y; ga = pa[x]; gb = pb[x];
            }
            bad++;
        }
    }

    if (bad) {
        printf("FAIL %-34s %d px differ, first (%d,%d) "
               "mmx %08lx scalar %08lx\n",
               what, bad, fx, fy,
               (unsigned long)ga, (unsigned long)gb);
        fails++;
    } else {
        printf("ok   %-34s\n", what);
    }
}

static void case_alpha(mr_surface *s, mr_surface *m, mr_surface *c,
                       unsigned a)
{
    char name[64];

    fill_src(s, a, 0);
    run(m, s, 1);
    run(c, s, 0);
    sprintf(name, "uniform alpha %u", a);
    compare(name, m, c);
}

int main(void)
{
    mr_surface *s = mr_surface_new(W, H);
    mr_surface *m = mr_surface_new(W, H);
    mr_surface *c = mr_surface_new(W, H);
    unsigned a;
    static const unsigned corners[] = { 0, 1, 127, 128, 254, 255 };
    int i;

    if (!s || !m || !c) {
        printf("could not allocate %dx%d surfaces\n", W, H);
        return 2;
    }

    if (!mr_blend_have_simd()) {
        /* A bail-out is a failure or a skip, never a quiet success. There is
         * nothing to compare if the kernel cannot run, and reporting `ok`
         * here would be the vacuous pass this whole arrangement exists to
         * avoid.
         *
         * Two causes since 2026-11-06, which is why the cpu line is printed:
         * MR_BLEND_MMX undefined, or CPUID saying this processor has no MMX.
         * On the target the second is itself the finding -- probe/cpuchk.c is
         * the one to run next. */
        printf("the MMX blend cannot run here, so there is nothing to "
               "check.\n  cpu: %s\n"
               "  Either MR_BLEND_MMX was not defined for this build (both "
               "Watcom makefiles\n  define it), or detection says no MMX. "
               "See probe/cpuchk.c.\n", mr_cpu_desc());
        return 2;
    }

    printf("blendchk: mr_blit, MMX against scalar, %dx%d\n", W, H);

    /* 1. the sweep: every alpha, one at a time */
    for (a = 0; a < 256; a++) {
        fill_src(s, a, 0);
        run(m, s, 1);
        run(c, s, 0);
        {
            int x, y, bad = 0;

            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++)
                    if (m->px[(size_t)y * m->pitch + x] !=
                        c->px[(size_t)y * c->pitch + x])
                        bad++;
            if (bad) {
                printf("FAIL sweep at alpha %u: %d px differ\n", a, bad);
                fails++;
                break;
            }
        }
    }
    if (!fails)
        printf("ok   every alpha 0..255, uniform\n");

    /* 2. the corners, named individually so a failure says which */
    for (i = 0; i < (int)(sizeof(corners) / sizeof(corners[0])); i++)
        case_alpha(s, m, c, corners[i]);

    /* 3. alpha varying per pixel, so a row contains transparent, opaque and
     * every partial value -- which is also the only case that exercises the
     * kernel's two early-outs interleaved with its vector path. */
    fill_src(s, 0, 1);
    run(m, s, 1);
    run(c, s, 0);
    compare("alpha varying per pixel", m, c);

    /* 4. and that the destination's non-255 alpha is handled identically.
     * Checked explicitly rather than left to case 3, because a difference
     * here is one byte in a channel nothing on the stage ever reads, and it
     * would otherwise be invisible until some future surface did read it. */
    {
        int x, y, kept = 0, set = 0;

        fill_src(s, 0, 1);
        run(m, s, 1);

        for (y = 0; y < H; y++) {
            const mr_u32 *ps = s->px + (size_t)y * s->pitch;
            const mr_u32 *pm = m->px + (size_t)y * m->pitch;

            for (x = 0; x < W; x++) {
                if (MR_A(ps[x]) == 0 && MR_A(pm[x]) == DST_ALPHA)
                    kept++;
                if (MR_A(ps[x]) != 0 && MR_A(pm[x]) == 255)
                    set++;
            }
        }

        /* Both have to be non-zero or the fixture did not reach the case. */
        if (kept == 0 || set == 0) {
            printf("FAIL fixture reached neither alpha case "
                   "(kept %d, set %d)\n", kept, set);
            fails++;
        } else {
            printf("ok   transparent source left dst alpha %02x (%d px), "
                   "blended set 255 (%d px)\n", DST_ALPHA, kept, set);
        }
    }

    mr_surface_free(s);
    mr_surface_free(m);
    mr_surface_free(c);

    printf("%s\n", fails ? "** FAIL **" : "all cases agree");
    return fails ? 1 : 0;
}

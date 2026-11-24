/* mmxcheck.c - does the MMX colour conversion agree with the C one?
 *
 *     wmake -f Makefile.dos mmxcheck     -> mmxchk.exe
 *     wmake -f Makefile.cw  mmxcheck     -> mmxchkcw.exe
 *
 * Run it on the target. The exit code is the test: 0 if every pixel of every
 * case matched, 1 if any did not, 2 if it could not run at all.
 *
 * --- why this exists ------------------------------------------------------
 *
 * src/yuvmmx.c is Open Watcom inline assembly, so the harness suite on the
 * build host cannot compile it, never mind execute it. yuvtest says so out
 * loud at the end of its run rather than leaving a reader to infer that its
 * green lines cover the MMX path -- they do not.
 *
 * The risk was split in two (yuvmmx.h sets this out). The *arithmetic* -- the
 * decomposition of the 76309 multiply into an add and a 10773 multiply,
 * because MMX has no 17-bit lane -- is a question about integers, and yuvtest
 * proves it exhaustively over all 256 luma values on the host. What is left
 * is whether the assembly computes that arithmetic, and whether the packing
 * and interleaving put the bytes where a surface wants them. Only a machine
 * with MMX in it can answer that, which is this file.
 *
 * It is a probe and not a harness for the reason probe/ exists: it needs a
 * compiler the suite has not got.
 *
 * --- what it covers -------------------------------------------------------
 *
 * Three kinds of case, and the second two are the ones that matter:
 *
 *   1. A sweep. Y across its whole range against chroma across its whole
 *      range, so both ends of the saturation are reached. `packuswb` and the
 *      C path's clamp table are two different mechanisms for the same job and
 *      this is where they would part company.
 *
 *   2. The corners. All sixteen combinations of Y, Cb and Cr at 0, 16, 128
 *      and 255 -- black, studio black, neutral, and full -- laid out so that
 *      every one is hit. A gradient can miss the exact extremes, and the
 *      extremes are where a saturating instruction and a clamped table
 *      differ if they differ anywhere.
 *
 *   3. Widths that are not multiples of four. The kernel converts whole
 *      groups of four pixels and hands the remainder back to the C loop,
 *      which exists so that an ordinary window size does not fall back to
 *      scalar for the entire frame. The seam between the two is a thing that
 *      can be off by one pixel, and at 1:1 on a 638-wide picture that is a
 *      wrong column down the right-hand edge of a cutscene.
 *
 * Every case is run twice, once with the kernel on and once with it off,
 * through the real mr_yuv_rows -- not through a copy of the loop. Comparing
 * the engine's own entry point both ways is what makes this a test of what
 * ships rather than of a reimplementation of it.
 *
 * --- a note on what a pass means ------------------------------------------
 *
 * A pass says the two paths agree. It does not say either is right: that is
 * yuvtest's job, where the C path is compared against pl_mpeg's own
 * converter. The two tests are only worth anything together -- this one
 * anchors the kernel to the C path, and that one anchors the C path to the
 * vendor's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "yuv.h"
#include "cpu.h"       /* for the cpu line in the bail-out below */

/* main.c is linked by no probe. */
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(2); }
void mr_log(const char *f, ...) { (void)f; }
void mr_invalidate(void) { }
void mr_quit(void) { }
mr_surface *mr_stage(void) { return NULL; }

#define MAXW 64
#define MAXH 32

static mr_u8 plane_y[MAXW * MAXH];
static mr_u8 plane_cb[(MAXW / 2) * (MAXH / 2)];
static mr_u8 plane_cr[(MAXW / 2) * (MAXH / 2)];

static int fails;

static void frame_for(mr_yuv_frame *f, int w, int h)
{
    f->w = w;
    f->h = h;
    f->y = plane_y;
    f->cb = plane_cb;
    f->cr = plane_cr;
    f->y_pitch = MAXW;
    f->c_pitch = MAXW / 2;
}

/* Convert `f` into a fresh surface with the kernel in the given state. */
static mr_surface *convert(const mr_yuv_frame *f, int simd)
{
    mr_yuv_scaler s;
    mr_surface *out = mr_surface_new(f->w, f->h);

    if (!out)
        return NULL;
    mr_surface_fill(out, 0);

    mr_yuv_set_simd(simd);
    if (!mr_yuv_init(&s, f->w, f->h, f->w, f->h)) {
        mr_surface_free(out);
        return NULL;
    }
    mr_yuv_rows(&s, f, out, 0, 0, 0, f->h);
    mr_yuv_free(&s);
    return out;
}

static void compare(const char *label, int w, int h)
{
    mr_yuv_frame f;
    mr_surface *a, *b;
    int bad = 0, i, n;
    int bx = -1, by = -1;

    frame_for(&f, w, h);

    b = convert(&f, 0);          /* C */
    a = convert(&f, 1);          /* MMX, if it is compiled in */
    if (!a || !b) {
        printf("  %-46s ** could not allocate **\n", label);
        fails++;
        mr_surface_free(a);
        mr_surface_free(b);
        return;
    }

    n = w * h;
    for (i = 0; i < n; i++) {
        int x = i % w, y = i / w;
        mr_u32 pa = a->px[(size_t)y * a->pitch + x];
        mr_u32 pb = b->px[(size_t)y * b->pitch + x];

        if (pa != pb) {
            bad++;
            if (bx < 0) { bx = x; by = y; }
        }
    }

    printf("  %-46s %s\n", label, bad ? "** DIFFER **" : "ok");
    if (bad) {
        /* Buffer rows are bottom-up, so report the picture row as well: the
         * difference between "the top edge is wrong" and "the bottom edge is
         * wrong" is the difference between the packing and the row order. */
        mr_u32 pa = a->px[(size_t)by * a->pitch + bx];
        mr_u32 pb = b->px[(size_t)by * b->pitch + bx];

        printf("      %d of %d pixels; first at buffer (%d,%d) = picture row "
               "%d: MMX %08lX, C %08lX\n",
               bad, n, bx, by, h - 1 - by,
               (unsigned long)pa, (unsigned long)pb);
        fails++;
    }

    mr_surface_free(a);
    mr_surface_free(b);
}

/* Case 1: both inputs sweeping, so saturation is reached at both ends. */
static void fill_sweep(void)
{
    int x, y;

    for (y = 0; y < MAXH; y++)
        for (x = 0; x < MAXW; x++)
            plane_y[y * MAXW + x] = (mr_u8)(x * 255 / (MAXW - 1));

    for (y = 0; y < MAXH / 2; y++)
        for (x = 0; x < MAXW / 2; x++) {
            plane_cb[y * (MAXW / 2) + x] =
                (mr_u8)(y * 255 / (MAXH / 2 - 1));
            plane_cr[y * (MAXW / 2) + x] =
                (mr_u8)(255 - x * 255 / (MAXW / 2 - 1));
        }
}

/* Case 2: every combination of the four interesting values, exactly. */
static void fill_corners(void)
{
    static const mr_u8 v[4] = { 0, 16, 128, 255 };
    int x, y;

    for (y = 0; y < MAXH; y++)
        for (x = 0; x < MAXW; x++)
            plane_y[y * MAXW + x] = v[(x >> 1) & 3];

    for (y = 0; y < MAXH / 2; y++)
        for (x = 0; x < MAXW / 2; x++) {
            plane_cb[y * (MAXW / 2) + x] = v[y & 3];
            plane_cr[y * (MAXW / 2) + x] = v[(y >> 2) & 3];
        }
}

int main(void)
{
    printf("mmxcheck: the MMX colour conversion against the C one\n\n");

    if (!mr_yuv_have_simd()) {
        /* Not a pass and not a failure. There is nothing here to compare and
         * saying "ok" would be the vacuous answer. 77 is the skip convention
         * the harness runner uses, borrowed here for the same reason.
         *
         * Two causes since 2026-11-06, hence the cpu line: the kernel was not
         * compiled in, or CPUID says this processor cannot run it. On the
         * target the second is itself the finding; probe/cpuchk.c is the one
         * to run next. */
        printf("  the MMX kernel cannot run in this binary.\n"
               "  cpu: %s\n"
               "  Either it was built without -DMR_YUV_MMX (both Watcom\n"
               "  makefiles define it), or detection says no MMX. See\n"
               "  probe/cpuchk.c.\n", mr_cpu_desc());
        return 77;
    }

    fill_sweep();
    printf(" a full sweep of Y against Cb and Cr:\n");
    compare("64x32, a multiple of four wide", 64, 32);
    compare("62x32, two pixels of C tail", 62, 32);
    compare("58x16, and again at another width", 58, 16);

    fill_corners();
    printf("\n the exact corners -- 0, 16, 128, 255 in every combination:\n");
    compare("64x32", 64, 32);
    compare("62x32, with the tail", 62, 32);

    /* The narrowest picture mr_yuv_fit will produce. Below four pixels the
     * kernel does nothing at all and the C loop does the lot, which is worth
     * confirming rather than assuming -- a kernel that wrote one group
     * anyway would run off the end of a two-pixel row. */
    printf("\n widths the kernel must decline:\n");
    compare("2x2, narrower than one group", 2, 2);

    printf("\n%s\n", fails ? "DIFFERENCES FOUND -- set simd=0 in mrpy.ini"
                           : "the two paths agree everywhere tested");
    return fails ? 1 : 0;
}

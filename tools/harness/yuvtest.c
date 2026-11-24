/* yuv.c: YUV 4:2:0 to the stage's pixels, scaled, in one pass.
 *
 * This is the only part of video playback the engine wrote itself -- pl_mpeg
 * is the codec, movie.c is bookkeeping, and this file's subject is the one
 * loop that touches every pixel of every frame. It is also the loop that got
 * a hand-written MMX version, which makes a C reference worth pinning
 * precisely: the whole value of `simd=0` depends on the two paths agreeing,
 * and the only way to say what the MMX path *should* produce is to have the
 * C path nailed down here.
 *
 * The oracle is pl_mpeg's own plm_frame_to_bgra (libs/pl_mpeg.h:3613). That
 * is the strongest one available and it costs nothing: our conversion takes
 * its coefficients from that function on purpose (see yuv.h), so the two must
 * agree to the byte. If they ever do not, the bug is ours -- which is exactly
 * the property wanted from an oracle, and is not true of a reference computed
 * by this file, since that would agree with any misreading of the formula.
 *
 * Six claims:
 *
 *   1. The direct path is byte-identical to plm_frame_to_bgra over a frame
 *      that covers the whole input range. Both write B,G,R,A, which on
 *      little-endian x86 is our 0xAARRGGBB in a DWORD -- the same identity
 *      mr.h relies on for handing a surface to GDI.
 *   2. The comparison is not vacuous: the fixture is checked to actually
 *      drive the conversion out of 0..255 at both ends, so the clamp table is
 *      exercised rather than merely present. A ramp that never clamps would
 *      pass this file with the table removed.
 *   3. Rows come out bottom-up, which is what the presenters' buffers are.
 *      Getting this backwards is a picture that is upside down but otherwise
 *      perfect, and it is invisible in any per-pixel comparison that makes
 *      the same mistake in its reference.
 *   4. A single row can be asked for and matches the same row of a whole-
 *      frame conversion. yuv.c pays three extra multiplies per 2x2 block to
 *      make rows independent; this is what that buys.
 *   5. The scaled path samples nearest-neighbour from pixel centres, against
 *      a reference built from the mapping rather than from the code.
 *   6. mr_yuv_fit letterboxes by aspect ratio, rounds to even, and stays
 *      inside the box. This is the "optional black border" in the brief, and
 *      an off-by-one here writes a row of video outside the picture buffer.
 *
 * And one that is checked but cannot be asserted here: the MMX path. It is
 * Open Watcom inline assembly, so it is not compiled on this host at all and
 * mr_yuv_have_simd() returns 0. Said out loud at the end of the run rather
 * than left to be inferred from a green line. probe/mmxcheck.c is the test
 * that can; it runs on the target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "yuv.h"
#include "yuvmmx.h"

/* The oracle. This is the only file in the tree besides plmimpl.c that
 * includes pl_mpeg, and it only wants the one conversion function; the
 * implementation is linked in from plmimpl.c, so this is declarations. */
#include <stddef.h>
#define PLM_NO_STDIO
#include "pl_mpeg.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- the fixture ---------------------------------------------------------
 *
 * 64x64, which is 32x32 chroma. The planes are built so that every part of
 * the arithmetic is reached:
 *
 *   Y   sweeps 0..255 across the row, so both ends of the luma range appear,
 *       including the sub-16 values that make `(y-16)*76309` negative.
 *   Cb  sweeps 0..255 down the column and Cr sweeps the other way, so every
 *       pair of chroma extremes meets some luma extreme somewhere in the
 *       frame. The corners are where clamping happens: bright blue-extreme
 *       pixels overflow, dark ones underflow.
 *
 * A flat grey frame, or a luma ramp with neutral chroma, would compare equal
 * under almost any bug in the chroma terms. Claim 2 is what stops this from
 * silently becoming such a frame.
 */
#define FW 64
#define FH 64
#define CW (FW / 2)
#define CH (FH / 2)

static mr_u8 plane_y[FW * FH];
static mr_u8 plane_cb[CW * CH];
static mr_u8 plane_cr[CW * CH];

static void build_fixture(void)
{
    int x, y;

    for (y = 0; y < FH; y++)
        for (x = 0; x < FW; x++)
            plane_y[y * FW + x] = (mr_u8)(x * 255 / (FW - 1));

    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++) {
            plane_cb[y * CW + x] = (mr_u8)(y * 255 / (CH - 1));
            plane_cr[y * CW + x] = (mr_u8)(255 - y * 255 / (CH - 1));
        }
}

static void fill_frame(mr_yuv_frame *f)
{
    f->w = FW;
    f->h = FH;
    f->y = plane_y;
    f->cb = plane_cb;
    f->cr = plane_cr;
    f->y_pitch = FW;
    f->c_pitch = CW;
}

static void fill_plm(plm_frame_t *p)
{
    memset(p, 0, sizeof(*p));
    p->width = FW;
    p->height = FH;
    p->y.width = FW;  p->y.height = FH;  p->y.data = plane_y;
    p->cb.width = CW; p->cb.height = CH; p->cb.data = plane_cb;
    p->cr.width = CW; p->cr.height = CH; p->cr.data = plane_cr;
}

/* Does the fixture actually push the conversion past both ends? Computed
 * here from the coefficients, independently of yuv.c's table, so that this
 * answers "is the fixture hard enough" and not "does yuv.c clamp". */
static void clamp_reach(int *low, int *high)
{
    int x, y;

    *low = 0;
    *high = 0;

    for (y = 0; y < FH; y++)
        for (x = 0; x < FW; x++) {
            int icr = (int)plane_cr[(y >> 1) * CW + (x >> 1)] - 128;
            int icb = (int)plane_cb[(y >> 1) * CW + (x >> 1)] - 128;
            int r = (icr * 104597) >> 16;
            int g = (icb * 25674 + icr * 53278) >> 16;
            int b = (icb * 132201) >> 16;
            int yy = (((int)plane_y[y * FW + x] - 16) * 76309) >> 16;
            int v[3];
            int i;

            v[0] = yy + r; v[1] = yy - g; v[2] = yy + b;
            for (i = 0; i < 3; i++) {
                if (v[i] < 0)   (*low)++;
                if (v[i] > 255) (*high)++;
            }
        }
}

int main(void)
{
    mr_yuv_frame f;
    plm_frame_t p;
    mr_yuv_scaler s;
    mr_surface *dst;
    mr_u32 *ref;
    int low = 0, high = 0;

    printf("yuvtest: YUV 4:2:0 conversion and scaling\n\n");

    build_fixture();
    fill_frame(&f);
    fill_plm(&p);

    clamp_reach(&low, &high);
    printf("  fixture clamps %d components low, %d high\n", low, high);
    check("the fixture reaches both ends of the clamp", low > 0 && high > 0);

    /* --- 1, 3: the direct path against pl_mpeg ------------------------- */
    ref = (mr_u32 *)malloc(sizeof(mr_u32) * FW * FH);
    dst = mr_surface_new(FW, FH);
    if (!ref || !dst) {
        printf("out of memory\n");
        return 1;
    }

    /* plm_frame_to_bgra writes B, G and R and *never writes the alpha byte*
     * -- its macro is parameterised on three channel indices and 4 bytes of
     * stride, so the fourth byte of each pixel keeps whatever was there. Its
     * callers are expected to have cleared it, or to be uploading to a
     * texture that ignores it.
     *
     * So the oracle's alpha is malloc garbage, and comparing whole DWORDs
     * against it fails on every pixel for a reason that has nothing to do
     * with colour -- which is what this did on the first run. Filling with
     * 0xFF first is better than masking the alpha out of the comparison: it
     * keeps the check on all four bytes, so our writing 255 there stays
     * asserted rather than becoming untested. */
    memset(ref, 0xFF, sizeof(mr_u32) * FW * FH);
    plm_frame_to_bgra(&p, (uint8_t *)ref, FW * 4);

    if (!mr_yuv_init(&s, FW, FH, FW, FH)) {
        printf("mr_yuv_init failed\n");
        return 1;
    }
    check("a destination at the frame's size takes the direct path",
          s.kind == MR_YUV_DIRECT);

    mr_surface_fill(dst, 0);
    mr_yuv_rows(&s, &f, dst, 0, 0, 0, FH);

    {
        int bad = 0, flipped = 0, row, col;

        for (row = 0; row < FH; row++)
            for (col = 0; col < FW; col++) {
                /* Bottom-up: picture row `row` is buffer row FH-1-row. */
                mr_u32 got = dst->px[(size_t)(FH - 1 - row) * dst->pitch + col];
                if (got != ref[row * FW + col])
                    bad++;
                /* What it would be if the rows were not flipped, so a
                 * failure can say which of the two it is. */
                if (dst->px[(size_t)row * dst->pitch + col] ==
                    ref[row * FW + col])
                    flipped++;
            }

        check("direct conversion is byte-identical to plm_frame_to_bgra",
              bad == 0);
        if (bad)
            printf("      %d of %d pixels differ (%d match un-flipped, "
                   "so the row order is the suspect)\n",
                   bad, FW * FH, flipped);
    }

    /* --- 4: one row at a time ------------------------------------------ */
    {
        mr_surface *one = mr_surface_new(FW, FH);
        int bad = 0, row, col;

        mr_surface_fill(one, 0);
        for (row = 0; row < FH; row++)
            mr_yuv_rows(&s, &f, one, 0, 0, row, row + 1);

        for (row = 0; row < FH; row++)
            for (col = 0; col < FW; col++)
                if (one->px[(size_t)row * one->pitch + col] !=
                    dst->px[(size_t)row * dst->pitch + col])
                    bad++;

        check("row by row equals the whole frame at once", bad == 0);
        mr_surface_free(one);
    }

    /* --- the destination offset ---------------------------------------- */
    {
        mr_surface *big = mr_surface_new(FW + 8, FH + 6);
        int bad = 0, outside = 0, row, col;
        const int dx = 4, dy = 3;

        mr_surface_fill(big, 0xFF123456u);
        mr_yuv_rows(&s, &f, big, dx, dy, 0, FH);

        for (row = 0; row < FH; row++)
            for (col = 0; col < FW; col++) {
                size_t br = (size_t)(big->h - 1 - (dy + row));
                if (big->px[br * big->pitch + dx + col] != ref[row * FW + col])
                    bad++;
            }
        check("the picture lands at (dx, dy)", bad == 0);

        /* Nothing outside the rectangle may have been touched. This is the
         * check that matters for the letterbox: the bars are painted once
         * and a frame that writes into them leaves the border flickering. */
        for (row = 0; row < big->h; row++)
            for (col = 0; col < big->w; col++) {
                int pr = big->h - 1 - row;      /* picture row of this buffer row */
                int inside = (pr >= dy && pr < dy + FH &&
                              col >= dx && col < dx + FW);
                if (!inside &&
                    big->px[(size_t)row * big->pitch + col] != 0xFF123456u)
                    outside++;
            }
        check("nothing is written outside the picture rectangle", outside == 0);

        mr_surface_free(big);
    }

    mr_yuv_free(&s);

    /* --- 5: the scaled path -------------------------------------------- */
    {
        const int dw = 32, dh = 48;     /* down on x, down on y, not a ratio */
        mr_yuv_scaler sc;
        mr_surface *out = mr_surface_new(dw, dh);
        int bad = 0, row, col;

        if (!mr_yuv_init(&sc, FW, FH, dw, dh)) {
            printf("mr_yuv_init (scaled) failed\n");
            return 1;
        }
        check("a different destination size takes the scaled path",
              sc.kind == MR_YUV_SCALED);

        mr_surface_fill(out, 0);
        mr_yuv_rows(&sc, &f, out, 0, 0, 0, dh);

        /* The reference: nearest neighbour from pixel centres, with the
         * conversion written out again from the coefficients. Deliberately
         * not a call into yuv.c. */
        for (row = 0; row < dh; row++)
            for (col = 0; col < dw; col++) {
                int sy = (int)(((long)row * 2 + 1) * FH / ((long)dh * 2));
                int sx = (int)(((long)col * 2 + 1) * FW / ((long)dw * 2));
                int icr = (int)plane_cr[(sy >> 1) * CW + (sx >> 1)] - 128;
                int icb = (int)plane_cb[(sy >> 1) * CW + (sx >> 1)] - 128;
                int r = (icr * 104597) >> 16;
                int g = (icb * 25674 + icr * 53278) >> 16;
                int b = (icb * 132201) >> 16;
                int yy = (((int)plane_y[sy * FW + sx] - 16) * 76309) >> 16;
                mr_u32 want, got;
                int cr8, cg8, cb8;

                cr8 = yy + r; if (cr8 < 0) cr8 = 0; if (cr8 > 255) cr8 = 255;
                cg8 = yy - g; if (cg8 < 0) cg8 = 0; if (cg8 > 255) cg8 = 255;
                cb8 = yy + b; if (cb8 < 0) cb8 = 0; if (cb8 > 255) cb8 = 255;
                want = MR_ARGB(255, cr8, cg8, cb8);

                got = out->px[(size_t)(dh - 1 - row) * out->pitch + col];
                if (got != want)
                    bad++;
            }

        check("scaled conversion samples nearest-neighbour from centres",
              bad == 0);
        if (bad)
            printf("      %d of %d pixels differ\n", bad, dw * dh);

        mr_yuv_free(&sc);
        mr_surface_free(out);
    }

    /* An upscale too: the engine hits this on a 1024x768 mode, where the
     * picture rect is taller than a 2:1 video and the video is widened to
     * the full 1024. A map that clamped on the wrong side reads one pixel
     * past the plane here and nowhere else. */
    {
        mr_yuv_scaler sc;
        mr_surface *out = mr_surface_new(FW * 2, FH * 2);
        int i, bad = 0;

        if (mr_yuv_init(&sc, FW, FH, FW * 2, FH * 2)) {
            mr_surface_fill(out, 0);
            mr_yuv_rows(&sc, &f, out, 0, 0, 0, FH * 2);
            for (i = 0; i < FW * 2 * FH * 2; i++)
                if ((out->px[i] >> 24) != 0xFF)
                    bad++;
            check("a 2x upscale writes every pixel, opaque", bad == 0);
            mr_yuv_free(&sc);
        } else {
            check("a 2x upscale writes every pixel, opaque", 0);
        }
        mr_surface_free(out);
    }

    /* --- 6: the fit ----------------------------------------------------- */
    {
        int w = 0, h = 0;

        /* ending.mpg's shape, in the default window's picture rect. */
        mr_yuv_fit(640, 320, 640, 360, &w, &h);
        check("a 2:1 video in a 640x360 area is 640x320",
              w == 640 && h == 320);

        /* The same video in a 4:3 mode's 16:9 picture rect. */
        mr_yuv_fit(640, 320, 800, 450, &w, &h);
        check("fitting by width when the video is wider than the area",
              w == 800 && h == 400);

        /* A tall video: the other branch. */
        mr_yuv_fit(320, 640, 640, 360, &w, &h);
        check("fitting by height when the video is taller",
              w == 180 && h == 360);

        /* Same aspect, no bars at all. */
        mr_yuv_fit(1280, 720, 640, 360, &w, &h);
        check("a 16:9 video in a 16:9 area fills it", w == 640 && h == 360);

        /* Even on both axes, and never outside the box. 641x361 would be
         * the naive answer for a 16:9 video in this box. */
        mr_yuv_fit(1280, 720, 641, 361, &w, &h);
        check("the fit is even and inside the box",
              !(w & 1) && !(h & 1) && w <= 641 && h <= 361);

        /* Degenerate input must not divide by zero or return something a
         * caller would index with. */
        w = h = -1;
        mr_yuv_fit(0, 0, 640, 360, &w, &h);
        check("a frame with no size fits to nothing", w == 0 && h == 0);
    }

    /* --- the bars -------------------------------------------------------
     *
     * mr_yuv_bars blacks everything outside the video's rectangle. It exists
     * because the picture buffer is shared with the stage path, so the bars
     * can be painted over by a repaint while a cutscene is running --
     * reported on Win32s as a cutscene showing the previous scene and the
     * quick menu in its letterbox, after a resize or after rollback.
     *
     * Two claims, and the second is the one that was actually wrong: it is
     * easy to write this so it blacks the whole buffer, which would be
     * correct for the bars and would also erase the picture every frame.
     */
    {
        mr_surface *b = mr_surface_new(FW, FH);
        const int vx = 4, vy = 6, vw = 56, vh = 20;
        int row, col, bad_out = 0, bad_in = 0;

        mr_surface_fill(b, 0xFF123456u);
        mr_yuv_bars(b, vx, vy, vw, vh);

        for (row = 0; row < FH; row++)
            for (col = 0; col < FW; col++) {
                /* Bottom-up, as everything else here writes. */
                mr_u32 px = b->px[(size_t)(FH - 1 - row) * b->pitch + col];
                int inside = (row >= vy && row < vy + vh &&
                              col >= vx && col < vx + vw);

                if (inside && px != 0xFF123456u)
                    bad_in++;
                if (!inside && px != 0)
                    bad_out++;
            }

        check("the bars black every pixel outside the video rectangle",
              bad_out == 0);
        check("and leave every pixel inside it alone", bad_in == 0);

        /* A rectangle covering the whole surface is the common case -- a
         * 16:9 video in a 16:9 picture area -- and must do nothing at all
         * rather than, say, blacking a one-pixel edge. */
        mr_surface_fill(b, 0xFF123456u);
        mr_yuv_bars(b, 0, 0, FW, FH);
        bad_in = 0;
        for (row = 0; row < FW * FH; row++)
            if (b->px[row] != 0xFF123456u)
                bad_in++;
        check("a full-bleed video gets no bars at all", bad_in == 0);

        /* And a rectangle that has gone stale against a smaller surface must
         * still clear what is really outside it, because cleaning up after
         * exactly that is what it is for. */
        mr_surface_fill(b, 0xFF123456u);
        mr_yuv_bars(b, 0, 0, FW * 4, FH * 4);
        check("an oversized rectangle is clamped, not trusted",
              b->px[0] == 0xFF123456u);

        mr_surface_free(b);
    }

    /* --- the MMX kernel's algebra, proved without running the kernel -----
     *
     * The kernel cannot be executed here -- it is Watcom inline assembly --
     * but the one piece of novel arithmetic in it can be, and exhaustively.
     * It replaces a multiply by 76309 with an add and a multiply by 10773,
     * because 76309 does not fit the 16-bit lane `pmulhw` needs. yuvmmx.h
     * sets out why that is an identity rather than an approximation; this
     * checks it for every luma value there is.
     *
     * 256 cases is the whole input space, so this is a proof and not a
     * sample. If it ever fails, the kernel is wrong on the target and no
     * amount of staring at the assembly would have been the quicker way to
     * find out.
     */
    {
        int yv, bad = 0, first_bad = -1;

        for (yv = 0; yv < 256; yv++) {
            if (MR_YUV_LUMA_SCALAR(yv) != MR_YUV_LUMA_SPLIT(yv)) {
                bad++;
                if (first_bad < 0)
                    first_bad = yv;
            }
        }

        check("the MMX luma split equals a multiply by 76309, for all 256 Y",
              bad == 0);
        if (bad)
            printf("      %d values differ, first at Y=%d (%d vs %d)\n",
                   bad, first_bad, MR_YUV_LUMA_SCALAR(first_bad),
                   MR_YUV_LUMA_SPLIT(first_bad));

        /* And that the split is reached at all below black, which is where
         * the arithmetic-shift-of-a-negative assumption lives. A test over
         * 16..255 only would pass with a logical shift and leave the dark
         * end of every frame wrong on some future compiler. */
        check("the proof covers Y below black, where the shift goes negative",
              MR_YUV_LUMA_SCALAR(0) < 0);

        /* The coefficients the kernel's scalar preamble uses must be the ones
         * yuv.c's loop uses. They are written in two files; this is what
         * stops them drifting. */
        check("the chroma coefficients agree with the scalar path",
              MR_YUV_K_R_CR == 104597 && MR_YUV_K_G_CB == 25674 &&
              MR_YUV_K_G_CR == 53278  && MR_YUV_K_B_CB == 132201);
    }

    /* --- the fused path equals converting and then packing -------------- */
    {
        /* mr_yuv_rows_fused replaces two passes with one, so the claim that
         * matters is not "it looks right" but "it is the same bytes". Both
         * routes are run into buffers with the same pitch and compared
         * exactly.
         *
         * Four cases, because each is a way the address arithmetic could be
         * wrong while the conversion is right -- and the conversion is
         * already pinned against pl_mpeg above:
         *
         *   positive pitch   the Win32 DIB
         *   negative pitch   the DOS framebuffer, stored the other way up,
         *                    which the presenter addresses from the picture's
         *                    last row. Got this wrong and the picture is
         *                    upside down on one port only -- and that port is
         *                    the one with no debugger.
         *   an offset        (dx, dy) non-zero, i.e. a letterboxed video
         *   a row band       [r0, r1) a strip, as the DOS service loop asks
         *                    for; a band that disagrees with the whole frame
         *                    is a seam on screen.
         */
        /* The picture has to be tall enough for the whole 64x64 fixture at
         * its offset -- 64 + VY. Too small and *both* routes clip to the same
         * fewer rows, which is correct behaviour and a vacuous test: the
         * comparison then runs over rows neither of them wrote. */
        const int PW = 64, PH = 80;       /* picture buffer */
        const int VX = 0,  VY = 8;        /* where the video sits in it */
        mr_surface *pic = mr_surface_new(PW, PH);
        long stride = (long)PW * 4;
        mr_u8 *two = (mr_u8 *)malloc((size_t)stride * PH);
        mr_u8 *one = (mr_u8 *)malloc((size_t)stride * PH);
        mr_yuv_scaler sc;
        mr_yuv_frame f;
        mr_pack_fmt f32, f24;

        fill_frame(&f);
        mr_pack_fmt_std(&f32, 32);
        mr_pack_fmt_std(&f24, 24);

        if (pic && two && one && mr_yuv_init(&sc, f.w, f.h, f.w, f.h)) {
            int ok_pos, ok_neg, ok_band;

            /* positive pitch, offset picture */
            memset(two, 0xA5, (size_t)stride * PH);
            memset(one, 0xA5, (size_t)stride * PH);
            memset(pic->px, 0, (size_t)PW * PH * sizeof(mr_u32));
            mr_yuv_rows(&sc, &f, pic, VX, VY, 0, sc.dst_h);
            mr_pack_rows(&f32, pic, two, stride, 0, PH);
            ok_pos = mr_yuv_rows_fused(&sc, &f, &f32, one, stride,
                                       PW, PH, VX, VY, 0, sc.dst_h);

            /* The two routes differ outside the video: the pack writes the
             * whole picture buffer, the fused path only the video's rows. So
             * the comparison is the video's rectangle, which is all the fused
             * path claims to have written. */
            {
                int bad = 0, y, x;
                for (y = 0; y < sc.dst_h; y++) {
                    const mr_u32 *a = (const mr_u32 *)
                        (two + (long)(PH - 1 - (VY + y)) * stride) + VX;
                    const mr_u32 *b = (const mr_u32 *)
                        (one + (long)(PH - 1 - (VY + y)) * stride) + VX;
                    for (x = 0; x < sc.dst_w; x++)
                        if (a[x] != b[x])
                            bad++;
                }
                check("the fused path took the frame", ok_pos == 1);
                check("fused equals convert-then-pack, positive pitch",
                      bad == 0);
            }

            /* negative pitch: the DOS orientation. presdos.c points at the
             * picture's *last* row and steps upwards, so picture row p must
             * land at row p of the buffer -- the mirror image of the
             * positive-pitch case, which lands it at PH-1-p. `two` already
             * holds the positive-pitch answer and it has just been shown
             * equal to convert-then-pack, so it is the oracle here.
             *
             * Flipping the comparison rather than the data is the point: if
             * the sign of the pitch were ignored, both buffers would come out
             * identical row for row and this would pass while the DOS screen
             * was upside down. */
            memset(one, 0xA5, (size_t)stride * PH);
            ok_neg = mr_yuv_rows_fused(&sc, &f, &f32,
                                       one + (long)(PH - 1) * stride, -stride,
                                       PW, PH, VX, VY, 0, sc.dst_h);
            {
                int bad = 0, y, x;
                for (y = 0; y < sc.dst_h; y++) {
                    /* picture row (VY + y): bottom-up in `two`, top-down in
                     * `one` */
                    const mr_u32 *a = (const mr_u32 *)
                        (two + (long)(PH - 1 - (VY + y)) * stride) + VX;
                    const mr_u32 *b = (const mr_u32 *)
                        (one + (long)(VY + y) * stride) + VX;
                    for (x = 0; x < sc.dst_w; x++)
                        if (a[x] != b[x])
                            bad++;
                }
                check("fused equals convert-then-pack, negative pitch",
                      ok_neg && bad == 0);
            }

            /* a band, against the whole frame */
            memset(two, 0xA5, (size_t)stride * PH);
            memset(one, 0xA5, (size_t)stride * PH);
            mr_yuv_rows_fused(&sc, &f, &f32, two, stride,
                              PW, PH, VX, VY, 0, sc.dst_h);
            ok_band = 1;
            {
                int r0, r1;
                for (r0 = 0; r0 < sc.dst_h; r0 = r1) {
                    r1 = r0 + 7;            /* not a divisor of the height */
                    if (r1 > sc.dst_h)
                        r1 = sc.dst_h;
                    if (!mr_yuv_rows_fused(&sc, &f, &f32, one, stride,
                                           PW, PH, VX, VY, r0, r1))
                        ok_band = 0;
                }
            }
            check("fused in bands equals fused in one pass",
                  ok_band && memcmp(one, two, (size_t)stride * PH) == 0);

            /* and it must refuse what it cannot do, rather than write
             * 32bpp pixels into a 24bpp framebuffer */
            memset(one, 0xA5, (size_t)stride * PH);
            check("a 24bpp format is refused, not mangled",
                  mr_yuv_rows_fused(&sc, &f, &f24, one, stride,
                                    PW, PH, VX, VY, 0, sc.dst_h) == 0);
            /* Sampled *inside* the rectangle the fused path would have
             * written, not at the corners of the buffer. The first version of
             * this read one[0] and the last byte, and both are outside the
             * video -- so it passed with the format guard removed, which is
             * precisely the bug it was supposed to catch. */
            check("and nothing was written when it refused",
                  one[(size_t)((long)(PH - 1 - VY) * stride) + VX * 4]
                      == 0xA5 &&
                  one[(size_t)((long)(PH - 1 - (VY + sc.dst_h - 1)) * stride)
                      + VX * 4] == 0xA5);
            check("a picture the video does not fit in is refused",
                  mr_yuv_rows_fused(&sc, &f, &f32, one, stride,
                                    sc.dst_w - 1, PH, 0, 0, 0, 1) == 0);

            mr_yuv_free(&sc);
        } else {
            check("the fused-path fixture could be built", 0);
        }

        free(two);
        free(one);
        mr_surface_free(pic);
    }

    /* --- what this host cannot test ------------------------------------- */
    printf("\n  MMX kernel compiled in here: %s\n",
           mr_yuv_have_simd() ? "yes" : "no (expected: Watcom-only)");
    if (!mr_yuv_have_simd())
        printf("  so every claim above is about the C path. The MMX path is\n"
               "  checked against it on the target by probe/mmxcheck.c.\n");

    free(ref);
    mr_surface_free(dst);

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

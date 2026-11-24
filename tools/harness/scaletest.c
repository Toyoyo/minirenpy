/* The four scalers, against independent reference implementations.
 *
 * present.c is the one file the suite cannot reach -- HDCs and DIBs from top
 * to bottom, and not in run.sh's source list -- so until scale.c was split
 * out of it, the three scalers had no coverage at all. They are pure
 * arithmetic over two buffers, which is the most testable thing in the
 * engine, and it was untested because of what it happened to live next to.
 *
 * Two things are checked, and they are different claims:
 *
 *   1. Each scaler produces the pixels it says it does, compared against a
 *      floating-point reference written from the definition rather than from
 *      the implementation. Reimplementing the fixed-point trick in the
 *      reference would only prove it equals itself.
 *
 *   2. Scaling a *band* of rows gives bit-identical output to scaling the
 *      whole picture and taking those rows. This is the one that matters for
 *      bilinear: it reads a source row the other scalers do not, so a band
 *      can disagree with a full frame by a row at its edge, and the symptom
 *      is a faint seam across the dialogue box while a line types out --
 *      obvious once you know to look, invisible until then. bandtest covers
 *      the same idea at the stage level and cannot see this far down.
 *
 * Tolerance: the engine works in 8.8 fixed point and the reference in double,
 * so exact equality is the wrong bar. One count per channel is allowed, which
 * is tight enough that a wrong tap, a swapped weight or an off-by-one in the
 * tables all fail -- each of those moves a pixel by far more than 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mr.h"
#include "scale.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- fixtures ------------------------------------------------------------ */

/* A source picture with structure at every scale: a coarse gradient so a
 * weight error shows as a shift, plus a one-pixel checker so point sampling
 * and any kind of averaging cannot possibly agree. A flat or smooth image
 * would let a broken scaler pass. */
static mr_surface *make_src(int w, int h)
{
    mr_surface *s = mr_surface_new(w, h);
    int x, y;

    if (!s)
        return NULL;
    for (y = 0; y < h; y++) {
        mr_u32 *row = s->px + (size_t)y * s->pitch;
        for (x = 0; x < w; x++) {
            int r = x * 255 / (w - 1);
            int g = y * 255 / (h - 1);
            int b = ((x ^ y) & 1) ? 240 : 10;
            row[x] = MR_ARGB(255, r, g, b);
        }
    }
    return s;
}

static int chan(mr_u32 p, int i)
{
    return (int)((p >> (i * 8)) & 0xFF);
}

/* --- references ---------------------------------------------------------- */

/* Written from the definition of each filter, in double, with the bottom-up
 * row order applied at the end rather than baked into the loop. */
static mr_u32 ref_pixel(const mr_surface *src, int dw, int dh,
                        int x, int y, mr_scale_kind kind)
{
    double sx, sy;
    int i;
    double out[3];

    if (kind == MR_SCALE_COPY)
        return src->px[(size_t)y * src->pitch + x];

    if (kind == MR_SCALE_HALF) {
        const mr_u32 *r0 = src->px + (size_t)(y * 2) * src->pitch;
        const mr_u32 *r1 = r0 + src->pitch;
        for (i = 0; i < 3; i++)
            out[i] = (chan(r0[x * 2], i) + chan(r0[x * 2 + 1], i) +
                      chan(r1[x * 2], i) + chan(r1[x * 2 + 1], i)) / 4.0;
        return MR_ARGB(255, (int)(out[2] + 0.5), (int)(out[1] + 0.5),
                            (int)(out[0] + 0.5));
    }

    /* Centre of the destination pixel's footprint, in source coordinates. */
    sx = (x + 0.5) * src->w / (double)dw - 0.5;
    sy = (y + 0.5) * src->h / (double)dh - 0.5;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    if (kind == MR_SCALE_POINT) {
        /* Point sampling takes the containing pixel, which is the floor of
         * the un-shifted centre -- not of sx. Kept separate on purpose: the
         * half-pixel shift is a bilinear concern. */
        int px = (int)((x + 0.5) * src->w / (double)dw);
        int py = (int)((y + 0.5) * src->h / (double)dh);
        if (px >= src->w) px = src->w - 1;
        if (py >= src->h) py = src->h - 1;
        return src->px[(size_t)py * src->pitch + px];
    }

    {
        int x0 = (int)sx, y0 = (int)sy;
        int x1 = x0 + 1 < src->w ? x0 + 1 : x0;
        int y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        double fx = sx - x0, fy = sy - y0;
        const mr_u32 *r0 = src->px + (size_t)y0 * src->pitch;
        const mr_u32 *r1 = src->px + (size_t)y1 * src->pitch;

        /* The engine clamps the fraction to 0 on the last source pixel; the
         * reference reaches the same place by clamping the index, which is
         * the same picture by a different route. */
        if (x0 >= src->w - 1) { x0 = x1 = src->w - 1; fx = 0; }
        if (y0 >= src->h - 1) { y0 = y1 = src->h - 1; fy = 0; }

        for (i = 0; i < 3; i++) {
            double top = chan(r0[x0], i) * (1 - fx) + chan(r0[x1], i) * fx;
            double bot = chan(r1[x0], i) * (1 - fx) + chan(r1[x1], i) * fx;
            out[i] = top * (1 - fy) + bot * fy;
        }
        return MR_ARGB(255, (int)(out[2] + 0.5), (int)(out[1] + 0.5),
                            (int)(out[0] + 0.5));
    }
}

/* --- the checks ---------------------------------------------------------- */

static const char *kind_name(mr_scale_kind k)
{
    return k == MR_SCALE_COPY ? "copy" :
           k == MR_SCALE_HALF ? "half" :
           k == MR_SCALE_POINT ? "point" : "bilinear";
}

/* Compare a full scale against the reference, and report the worst channel
 * error so a near miss is visible rather than merely passing. */
static void against_reference(int sw, int sh, int dw, int dh, int bilinear,
                              mr_scale_kind want, int tol)
{
    mr_surface *src = make_src(sw, sh);
    mr_surface *dst = mr_surface_new(dw, dh);
    mr_scaler   s;
    int x, y, i, worst = 0;
    char label[160];

    if (!src || !dst || !mr_scale_init(&s, sw, sh, dw, dh, bilinear)) {
        check("allocation", 0);
        return;
    }

    sprintf(label, "%dx%d -> %dx%d picks %s", sw, sh, dw, dh, kind_name(want));
    check(label, s.kind == want);

    mr_scale_rows(&s, src, dst, 0, dh);

    for (y = 0; y < dh; y++) {
        /* Bottom-up: destination row y is buffer row dh-1-y. */
        const mr_u32 *got = dst->px + (size_t)(dh - 1 - y) * dst->pitch;
        for (x = 0; x < dw; x++) {
            mr_u32 want_px = ref_pixel(src, dw, dh, x, y, s.kind);
            for (i = 0; i < 3; i++) {
                int d = chan(got[x], i) - chan(want_px, i);
                if (d < 0) d = -d;
                if (d > worst) worst = d;
            }
        }
    }

    sprintf(label, "  matches the reference (worst channel error %d, tol %d)",
            worst, tol);
    check(label, worst <= tol);

    mr_scale_free(&s);
    mr_surface_free(src);
    mr_surface_free(dst);
}

/* Every band boundary, against the same rows of a full-frame scale. Bilinear
 * is the reason this exists: it reads one source row past the others, so a
 * band's first and last rows are where a seam would appear. */
static void bands_match_full(int sw, int sh, int dw, int dh, int bilinear)
{
    mr_surface *src = make_src(sw, sh);
    mr_surface *full = mr_surface_new(dw, dh);
    mr_surface *band = mr_surface_new(dw, dh);
    mr_scaler   s;
    int r0, bad = 0, checked = 0;
    char label[160];

    if (!src || !full || !band || !mr_scale_init(&s, sw, sh, dw, dh, bilinear)) {
        check("allocation", 0);
        return;
    }

    mr_scale_rows(&s, src, full, 0, dh);

    /* Every single-row band, plus a few wider ones. A single row is the
     * harshest case: both of its edges are band edges. */
    for (r0 = 0; r0 < dh; r0++) {
        int r1 = r0 + 1;
        int y;

        memset(band->px, 0xCD, (size_t)band->pitch * dh * sizeof(mr_u32));
        mr_scale_rows(&s, src, band, r0, r1);

        for (y = r0; y < r1; y++) {
            const mr_u32 *a = full->px + (size_t)(dh - 1 - y) * full->pitch;
            const mr_u32 *b = band->px + (size_t)(dh - 1 - y) * band->pitch;
            if (memcmp(a, b, (size_t)dw * sizeof(mr_u32)))
                bad++;
            checked++;
        }
    }

    sprintf(label, "%s: every one-row band equals the full frame (%d rows)",
            kind_name(s.kind), checked);
    check(label, bad == 0);

    mr_scale_free(&s);
    mr_surface_free(src);
    mr_surface_free(full);
    mr_surface_free(band);
}

/* The claim the option actually makes: setting it changes nothing at the
 * sizes that do not need it, and the pixels are identical, not merely
 * similar. This is what "no rendering change unless the option is set" means
 * at 1:1 and at exactly 2:1. */
static void bilinear_is_inert(int dw, int dh)
{
    mr_surface *src = make_src(MR_STAGE_W, MR_STAGE_H);
    mr_surface *off = mr_surface_new(dw, dh);
    mr_surface *on  = mr_surface_new(dw, dh);
    mr_scaler   a, b;
    char label[160];
    int same;

    if (!src || !off || !on) { check("allocation", 0); return; }

    mr_scale_init(&a, MR_STAGE_W, MR_STAGE_H, dw, dh, 0);
    mr_scale_init(&b, MR_STAGE_W, MR_STAGE_H, dw, dh, 1);

    sprintf(label, "%dx%d: the option does not change the scaler (%s)",
            dw, dh, kind_name(a.kind));
    check(label, a.kind == b.kind);

    mr_scale_rows(&a, src, off, 0, dh);
    mr_scale_rows(&b, src, on, 0, dh);
    same = !memcmp(off->px, on->px, (size_t)off->pitch * dh * sizeof(mr_u32));
    sprintf(label, "%dx%d: and produces identical pixels", dw, dh);
    check(label, same);

    mr_scale_free(&a);
    mr_scale_free(&b);
    mr_surface_free(src);
    mr_surface_free(off);
    mr_surface_free(on);
}

/* --- mr_blit_stretch's column map ----------------------------------------
 *
 * `sx + (x - dx) * sw / dw` was an integer divide per destination pixel, on a
 * *variable* divisor, so unlike the /255 case nothing could reduce it: 36-40
 * non-pipelined cycles each on the target, measured at 156 ns/px in the scene
 * `stretch` bucket. It is a table built once per call now.
 *
 * Which is a change to the hottest thing in the function and no change at all
 * to its output, so that is what these pin: every destination pixel must come
 * from the column the old expression picked. The reference below is that
 * expression, transcribed independently -- deliberately *not* scale.c's
 * build_point, which samples pixel centres: swapping the two would shift
 * every stretched sprite half a pixel, and a test that borrowed build_point
 * would have blessed it.
 *
 * The source encodes its own coordinates (x in red, y in green), so a pixel
 * taken from the wrong column or the wrong row is visible rather than
 * plausible -- a flat source cannot show a sampling bug, which is this file's
 * recurring lesson.
 */
static int ref_sxx(int sx, int sw, int dx, int dw, int x)
{
    int v = sx + (x - dx) * sw / dw;
    return v;
}

static void stretch_one(const char *label, int srcw, int srch,
                        int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh,
                        int dstw, int dsth, int alpha)
{
    mr_surface *src = mr_surface_new(srcw, srch);
    mr_surface *dst = mr_surface_new(dstw, dsth);
    const mr_u32 sentinel = 0xDEADBEEFUL;
    int x, y, x0, y0, x1, y1;
    long wrong = 0, spilled = 0, checked = 0;

    if (!src || !dst) {
        check("allocation for the stretch claims", 0);
        mr_surface_free(src);
        mr_surface_free(dst);
        return;
    }

    for (y = 0; y < srch; y++)
        for (x = 0; x < srcw; x++)
            src->px[y * src->pitch + x] = MR_ARGB(255, x, y, 0x5A);
    src->has_alpha = alpha;

    for (y = 0; y < dsth; y++)
        for (x = 0; x < dstw; x++)
            dst->px[y * dst->pitch + x] = sentinel;

    mr_blit_stretch(dst, src, sx, sy, sw, sh, dx, dy, dw, dh);

    /* The clipping the function does, restated: destination space, then the
     * surface's own bounds. No clip stack is pushed here, so mr_clip_rect
     * gives the whole surface. */
    x0 = dx > 0 ? dx : 0;
    y0 = dy > 0 ? dy : 0;
    x1 = (dx + dw < dstw) ? dx + dw : dstw;
    y1 = (dy + dh < dsth) ? dy + dh : dsth;

    for (y = 0; y < dsth; y++) {
        for (x = 0; x < dstw; x++) {
            mr_u32 got = dst->px[y * dst->pitch + x];
            int sxx, syy;
            mr_u32 want;

            if (x < x0 || x >= x1 || y < y0 || y >= y1) {
                if (got != sentinel)
                    spilled++;
                continue;
            }

            sxx = ref_sxx(sx, sw, dx, dw, x);
            syy = ref_sxx(sy, sh, dy, dh, y);
            if (sxx < 0) sxx = 0;
            if (sxx >= srcw) sxx = srcw - 1;
            if (syy < 0) syy = 0;
            if (syy >= srch) syy = srch - 1;

            want = src->px[syy * src->pitch + sxx];
            checked++;
            if (got != want)
                wrong++;
        }
    }

    printf("   %-46s %ld px", label, checked);
    if (wrong || spilled)
        printf(", %ld wrong, %ld spilled", wrong, spilled);
    printf("\n");

    /* `checked > 0` unless the rectangle really is off the surface: a claim
     * that compared no pixels would be the vacuous pass this file keeps
     * warning about, so the one case that legitimately writes nothing says so
     * in its own label and the rest have to have touched something. */
    if (x0 >= x1 || y0 >= y1)
        check(label, spilled == 0 && checked == 0);
    else
        check(label, wrong == 0 && spilled == 0 && checked > 0);

    mr_surface_free(src);
    mr_surface_free(dst);
}

static void stretch_claims(void)
{
    printf("\n-- mr_blit_stretch samples the column the divide picked --\n");

    /* Odd source sizes and odd ratios, so the division never comes out even
     * and an off-by-one in the map shows. Upscale, downscale and 1:1. */
    stretch_one("upscale, 37x23 -> 160x90", 37, 23, 0, 0, 37, 23,
                0, 0, 160, 90, 200, 120, 0);
    stretch_one("downscale, 160x90 -> 37x23", 160, 90, 0, 0, 160, 90,
                0, 0, 37, 23, 64, 32, 0);
    stretch_one("1:1 still goes through the map", 37, 23, 0, 0, 37, 23,
                0, 0, 37, 23, 64, 32, 0);

    /* A source sub-rectangle, which is what the nine-slice asks for: sx and
     * sy non-zero, so a map that forgot to add them lands on the wrong art. */
    stretch_one("a source sub-rectangle keeps its offset", 37, 23,
                5, 7, 20, 11, 0, 0, 90, 60, 128, 80, 0);

    /* Clipped off the left and the top: x0 > dx, so the map is built over a
     * range that does not start at the destination's origin -- the case an
     * index of `x - x0` rather than `x` would get wrong. */
    stretch_one("clipped off the left and top", 37, 23, 0, 0, 37, 23,
                -30, -20, 160, 90, 200, 120, 0);

    /* And off the right and bottom. */
    stretch_one("clipped off the right and bottom", 37, 23, 0, 0, 37, 23,
                150, 80, 160, 90, 200, 120, 0);

    /* The alpha branch takes the map too. Every alpha is 255, so the output
     * is still the source pixel and the oracle above holds -- what is being
     * checked is that the branch reads the same column, not the blend. */
    stretch_one("the alpha branch reads the same column", 37, 23, 0, 0, 37, 23,
                0, 0, 160, 90, 200, 120, 1);

    /* The fallback. A destination wider than MR_STAGE_W cannot use the static
     * table, so the inner loop keeps the divide -- and must agree with it to
     * the pixel. Without this claim the fallback is dead code that nothing
     * has ever run.
     *
     * With a *non-zero* source offset, which the first version of this did
     * not have: at sx = 0 the fallback can drop the `sx +` entirely and
     * nothing notices. That break went green and is the reason the numbers
     * below are not round. */
    stretch_one("a destination wider than the table still agrees",
                37, 23, 5, 7, 20, 11, 0, 0, MR_STAGE_W + 120, 40,
                MR_STAGE_W + 120, 40, 0);

    /* Degenerate: a rectangle entirely off the surface must write nothing,
     * and must not build a map over an empty range either. */
    stretch_one("a rectangle entirely off the surface writes nothing",
                37, 23, 0, 0, 37, 23, 400, 400, 50, 50, 64, 32, 0);
}

int main(void)
{
    setbuf(stdout, NULL);

    printf("-- which scaler a size gets --\n");
    printf("   (the option is a request; 1:1 and 2:1 decline it)\n");
    bilinear_is_inert(MR_STAGE_W, MR_STAGE_H);     /* copy */
    bilinear_is_inert(MR_STAGE_W / 2, MR_STAGE_H / 2);  /* half */

    printf("\n-- nearest, against the reference --\n");
    against_reference(MR_STAGE_W, MR_STAGE_H, 800, 450, 0, MR_SCALE_POINT, 0);
    against_reference(MR_STAGE_W, MR_STAGE_H, 1920, 1080, 0, MR_SCALE_POINT, 0);

    printf("\n-- the two that need no table --\n");
    against_reference(MR_STAGE_W, MR_STAGE_H, MR_STAGE_W, MR_STAGE_H, 0,
                      MR_SCALE_COPY, 0);
    against_reference(MR_STAGE_W, MR_STAGE_H, MR_STAGE_W / 2, MR_STAGE_H / 2, 0,
                      MR_SCALE_HALF, 1);

    printf("\n-- bilinear, against the reference --\n");
    printf("   (downscale, upscale, and the fullscreen case on a 1080p desktop)\n");
    against_reference(MR_STAGE_W, MR_STAGE_H, 800, 450, 1, MR_SCALE_BILINEAR, 1);
    against_reference(MR_STAGE_W, MR_STAGE_H, 1920, 1080, 1, MR_SCALE_BILINEAR, 1);
    against_reference(MR_STAGE_W, MR_STAGE_H, 1024, 576, 1, MR_SCALE_BILINEAR, 1);

    /* 3.8x down, at tolerance 2 rather than 1, and the reason is worth
     * stating because loosening a tolerance is usually how a bug gets
     * through. The sample position is 8.8, so it is quantised to 1/256 of a
     * source pixel; the fixture's checker puts a 230-count step between
     * adjacent source pixels; 230/256 is nearly a whole count of error before
     * the blend rounding adds its own. Measured 2.
     *
     * It is the fixture being hostile, not the scaler being wrong: with the
     * checker amplitude dropped from 230 to 10 this same size comes in at 1.
     * The harsh fixture is kept because it is what makes the other sizes
     * meaningful. */
    against_reference(MR_STAGE_W, MR_STAGE_H, 333, 187, 1, MR_SCALE_BILINEAR, 2);

    printf("\n-- bands, which is where a seam would be --\n");
    bands_match_full(MR_STAGE_W, MR_STAGE_H, 800, 450, 1);
    bands_match_full(MR_STAGE_W, MR_STAGE_H, 1920, 1080, 1);
    bands_match_full(MR_STAGE_W, MR_STAGE_H, 800, 450, 0);
    bands_match_full(MR_STAGE_W, MR_STAGE_H, MR_STAGE_W / 2, MR_STAGE_H / 2, 0);

    printf("\n-- the letterbox bars, stretched down and tiled across --\n");
    {
        mr_surface *tex;
        mr_u32 out[8];
        int i, ok;

        /* Tiling. Three distinct columns, two distinct rows, so neither axis
         * can be mistaken for the other and a wrong modulus shows at once. */
        tex = mr_surface_new(3, 2);
        for (i = 0; i < 3; i++) {
            tex->px[i] = MR_RGB(10 * (i + 1), 0, 0);
            tex->px[tex->pitch + i] = MR_RGB(0, 10 * (i + 1), 0);
        }

        for (i = 0; i < 8; i++) out[i] = 0xDEADBEEFUL;
        mr_border_row(tex, 2, 0, out, 7);
        ok = 1;
        for (i = 0; i < 7; i++)
            if (out[i] != MR_RGB(10 * (i % 3 + 1), 0, 0)) ok = 0;
        check("a bar wider than the texture repeats it horizontally", ok);
        check("and writes nothing past out_w", out[7] == 0xDEADBEEFUL);

        /* A tile is as wide as the uniform scale makes it, not as wide as
         * the texture: at twice the height it is twice the width, so the
         * repeat is every six pixels and each column is doubled. The bug
         * this pins is the one that reached the screen -- a 1672x941 border
         * in a 640-wide bar was squashed to the bar's height, tiled at 1672,
         * and so never repeated at all. */
        mr_border_row(tex, 4, 0, out, 8);
        ok = 1;
        for (i = 0; i < 8; i++)
            if (out[i] != MR_RGB(10 * ((i % 6) / 2 + 1), 0, 0)) ok = 0;
        check("the tile is scaled by the same factor on both axes", ok);

        /* 1:1 vertically is the identity on rows: row 1 is the second row of
         * the texture, not a stretched copy of the first. */
        mr_border_row(tex, 2, 1, out, 3);
        check("a bar the texture's own height takes its rows unchanged",
              out[0] == MR_RGB(0, 10, 0) && out[2] == MR_RGB(0, 30, 0));

        /* Stretched 5x. The split is where build_point puts it, which is
         * what the picture scaler does, so the two agree by construction. */
        ok = 1;
        for (i = 0; i < 10; i++) {
            mr_border_row(tex, 10, i, out, 1);
            if (out[0] != (i < 5 ? MR_RGB(10, 0, 0) : MR_RGB(0, 10, 0)))
                ok = 0;
        }
        check("a bar five times the texture's height stretches it", ok);

        /* And at a ratio where rounding is visible, the row it picks is the
         * one the picture scaler's own table picks. Stretching 2 rows over 3
         * is 0,1,1 taking row centres and 0,0,1 truncating; both are
         * defensible and only one matches mr_scale_init, so this is what
         * holds the comment in scale.c to the code next to it. */
        {
            mr_scaler s;
            int bad = 0;

            mr_scale_init(&s, 1, 2, 1, 3, 0);
            for (i = 0; i < 3; i++) {
                mr_border_row(tex, 3, i, out, 1);
                if (out[0] != (s.ymap[i] == 0 ? MR_RGB(10, 0, 0)
                                              : MR_RGB(0, 10, 0)))
                    bad = 1;
            }
            mr_scale_free(&s);
            check("the row it stretches to agrees with the scaler's ymap",
                  !bad);
        }

        /* Out of range rows clamp rather than reading off the ends: the
         * caller computes them from a bar height and a mode. */
        mr_border_row(tex, 10, -1, out, 1);
        ok = out[0] == MR_RGB(10, 0, 0);
        mr_border_row(tex, 10, 10, out, 1);
        ok = ok && out[0] == MR_RGB(0, 10, 0);
        check("rows outside the bar clamp to its first and last", ok);

        /* No alpha flag: whatever the top byte holds, the bar is opaque.
         * A PNG without an alpha channel decodes to 0xFF here anyway, but a
         * surface built some other way need not, and a bar handed to the
         * pack loops with a zero top byte is still drawn -- it would just be
         * carrying a lie. */
        tex->px[0] = 0x00123456UL;
        tex->has_alpha = 0;
        mr_border_row(tex, 2, 0, out, 1);
        check("an opaque texture keeps its colour and comes out opaque",
              out[0] == 0xFF123456UL);

        /* With alpha, composited over the black the bar already is. */
        tex->has_alpha = 1;
        tex->px[0] = MR_ARGB(128, 200, 100, 50);
        mr_border_row(tex, 2, 0, out, 1);
        check("a half-transparent texture is blended over black",
              out[0] == MR_RGB(200 * 128 / 255, 100 * 128 / 255,
                               50 * 128 / 255));
        tex->px[0] = MR_ARGB(0, 255, 255, 255);
        mr_border_row(tex, 2, 0, out, 1);
        check("a fully transparent texel leaves the bar black",
              out[0] == MR_RGB(0, 0, 0));

        /* Degenerate inputs write nothing at all rather than one pixel of
         * something: the presenter calls this for a bar that may be zero
         * rows tall when the mode is 16:9. */
        out[0] = 0xDEADBEEFUL;
        mr_border_row(tex, 0, 0, out, 1);
        mr_border_row(NULL, 4, 0, out, 1);
        mr_border_row(tex, 4, 0, out, 0);
        check("a zero-height bar, no texture and a zero width write nothing",
              out[0] == 0xDEADBEEFUL);

        mr_surface_free(tex);
    }

    printf("\n-- the edges, where a table runs out of source pixels --\n");
    {
        mr_scaler s;
        int ok;

        /* An upscale asks for a source pixel past the last one on its final
         * output column. If the table does not clamp, this reads off the end
         * of the row -- into the next row, so it would not even crash. */
        mr_scale_init(&s, 4, 4, 9, 9, 1);
        ok = s.xmap[8] == 3 && s.xfrac[8] == 0 &&
             s.ymap[8] == 3 && s.yfrac[8] == 0;
        check("the last output pixel does not blend past the source", ok);

        /* And the first clamps below zero rather than wrapping negative. */
        ok = s.xmap[0] == 0 && s.ymap[0] == 0;
        check("the first output pixel does not blend before the source", ok);
        mr_scale_free(&s);
    }

    stretch_claims();

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

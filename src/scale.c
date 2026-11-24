/* scale.c - stage to output, four ways. See scale.h for why this is its own
 * file rather than part of present.c.
 */

#include <stdlib.h>
#include <string.h>

#include "scale.h"

/* --- table building ------------------------------------------------------ */

/* Nearest neighbour: sample from the centre of each destination pixel's
 * source footprint, which is (i + 0.5) * src / dst. */
static void build_point(int *map, int src, int dst)
{
    int i;
    for (i = 0; i < dst; i++)
        map[i] = (i * 2 + 1) * src / (dst * 2);
}

/* Bilinear: the same centre, as a 8.8 fixed-point source coordinate, less the
 * half pixel that turns "centre of a pixel" into "distance from pixel 0's
 * centre". Below zero it clamps, which is what makes the first destination
 * pixel equal the first source pixel instead of blending with a pixel that is
 * not there.
 *
 * The arithmetic is kept inside a 32-bit long on purpose, since that is what
 * a long is on the target: the largest intermediate is
 * (2 * dst - 1) * src * 128, and with the 4096 cap on window size and a 1280
 * wide stage that is about 1.34e9 -- inside a signed 32-bit by a factor of
 * 1.6, but not by much, which is worth knowing before anyone raises either
 * limit.
 */
static void build_linear(int *map, int *frac, int src, int dst)
{
    int i;
    for (i = 0; i < dst; i++) {
        long f = ((long)(i * 2 + 1) * (long)src * 128L) / (long)dst - 128L;
        if (f < 0)
            f = 0;
        map[i] = (int)(f >> 8);
        frac[i] = (int)(f & 255L);
        /* The last destination pixel can land on the last source pixel with a
         * fraction; there is nothing to its right to blend with. */
        if (map[i] >= src - 1) {
            map[i] = src - 1;
            frac[i] = 0;
        }
    }
}

int mr_scale_init(mr_scaler *s, int src_w, int src_h,
                  int dst_w, int dst_h, int bilinear)
{
    memset(s, 0, sizeof(*s));

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return 0;

    s->src_w = src_w;
    s->src_h = src_h;
    s->dst_w = dst_w;
    s->dst_h = dst_h;

    if (dst_w == src_w && dst_h == src_h)
        s->kind = MR_SCALE_COPY;
    else if (dst_w * 2 == src_w && dst_h * 2 == src_h)
        s->kind = MR_SCALE_HALF;
    else
        s->kind = bilinear ? MR_SCALE_BILINEAR : MR_SCALE_POINT;

    if (s->kind == MR_SCALE_COPY || s->kind == MR_SCALE_HALF)
        return 1;                      /* neither needs a table */

    s->xmap = (int *)malloc((size_t)dst_w * sizeof(int));
    s->ymap = (int *)malloc((size_t)dst_h * sizeof(int));
    if (!s->xmap || !s->ymap) {
        mr_scale_free(s);
        return 0;
    }

    if (s->kind == MR_SCALE_POINT) {
        build_point(s->xmap, src_w, dst_w);
        build_point(s->ymap, src_h, dst_h);
        return 1;
    }

    s->xfrac = (int *)malloc((size_t)dst_w * sizeof(int));
    s->yfrac = (int *)malloc((size_t)dst_h * sizeof(int));
    if (!s->xfrac || !s->yfrac) {
        mr_scale_free(s);
        return 0;
    }
    build_linear(s->xmap, s->xfrac, src_w, dst_w);
    build_linear(s->ymap, s->yfrac, src_h, dst_h);
    return 1;
}

void mr_scale_free(mr_scaler *s)
{
    free(s->xmap);
    free(s->ymap);
    free(s->xfrac);
    free(s->yfrac);
    s->xmap = s->ymap = s->xfrac = s->yfrac = NULL;
}

/* --- the scalers --------------------------------------------------------- */

/* Blend a towards b by f (0..255), two lanes at a time.
 *
 * Red and blue sit in the even byte lanes and green in the odd one, so each
 * can take a 0..255 multiplier without running into its neighbour: the
 * largest lane value is 255 * 256 = 65280, which is 16 bits. The same trick
 * scale_half uses to average four pixels with two multiplies. Alpha is
 * dropped; the stage has no meaningful alpha and the DIB has no place to put
 * it.
 *
 * The +0x80 per lane rounds instead of truncating. It is not cosmetic: a
 * bilinear pixel is two chained blends, each >>8, so plain truncation loses
 * up to a count twice and always downwards. scaletest measured the result as
 * 2 to 3 counts dark against a double-precision reference -- a uniform
 * darkening of the whole picture, which is exactly the kind of error that
 * looks like "the filter is a bit muddy" rather than like a bug. With
 * rounding it is within 1, which is all 8.8 fixed point can promise.
 *
 * The constants are the rounding term placed in each lane: 0x00800080 for the
 * two even lanes, and 0x00008000 for green, whose operand is already shifted
 * left by 8. Headroom is fine -- 65280 + 128 is still inside 16 bits. */
#define LERP2(a, b, f)                                                      \
    (((((((a) & 0x00FF00FFUL) * (mr_u32)(256 - (f))) +                      \
        (((b) & 0x00FF00FFUL) * (mr_u32)(f)) +                              \
        0x00800080UL) >> 8) & 0x00FF00FFUL)                                 \
   | ((((((a) & 0x0000FF00UL) * (mr_u32)(256 - (f))) +                      \
        (((b) & 0x0000FF00UL) * (mr_u32)(f)) +                              \
        0x00008000UL) >> 8) & 0x0000FF00UL))

/* Exact 2x downscale with a 2x2 box filter. Averaging four pixels is both
 * better looking than point sampling and cheap; the green and red/blue
 * channels are summed in parallel using the standard two-mask trick to keep
 * this to a handful of operations per output pixel. */
static void scale_half(const mr_scaler *s, const mr_surface *src,
                       mr_surface *dst, int r0, int r1)
{
    int y, x, w, h;
    const mr_u32 *s0, *s1;
    mr_u32 *d;
    mr_u32 a, b, c, e;

    w = s->dst_w;
    h = s->dst_h;

    for (y = r0; y < r1; y++) {
        s0 = src->px + (size_t)(y * 2) * src->pitch;
        s1 = s0 + src->pitch;
        /* Reversed destination row: the DIB is bottom-up. */
        d = dst->px + (size_t)(h - 1 - y) * dst->pitch;

        for (x = 0; x < w; x++) {
            a = s0[0];
            b = s0[1];
            c = s1[0];
            e = s1[1];
            s0 += 2;
            s1 += 2;

            /* Sum red and blue in the even byte lanes, green in the odd lane,
             * then divide both by 4 and recombine. Each lane has 8 bits of
             * headroom so four values cannot overflow into a neighbour. */
            d[x] = ((((a & 0x00FF00FFUL) + (b & 0x00FF00FFUL) +
                      (c & 0x00FF00FFUL) + (e & 0x00FF00FFUL)) >> 2) & 0x00FF00FFUL)
                 | ((((a & 0x0000FF00UL) + (b & 0x0000FF00UL) +
                      (c & 0x0000FF00UL) + (e & 0x0000FF00UL)) >> 2) & 0x0000FF00UL);
        }
    }
}

static void scale_point(const mr_scaler *s, const mr_surface *src,
                        mr_surface *dst, int r0, int r1)
{
    int y, x, w, h;
    const mr_u32 *sp;
    mr_u32 *d;

    w = s->dst_w;
    h = s->dst_h;

    for (y = r0; y < r1; y++) {
        sp = src->px + (size_t)s->ymap[y] * src->pitch;
        /* Reversed destination row: the DIB is bottom-up. */
        d = dst->px + (size_t)(h - 1 - y) * dst->pitch;
        for (x = 0; x < w; x++)
            d[x] = sp[s->xmap[x]];
    }
}

/* Four taps: blend horizontally along both source rows, then vertically
 * between the two results.
 *
 * Twelve multiplies an output pixel against point sampling's one load, which
 * sounds worse than it measures: the pass is memory-bound (see "Performance"
 * in NOTES) and three of the four taps are already in cache. It is still the
 * expensive option, which is why it is off by default and why it is skipped
 * entirely for the two cases that do not need it.
 *
 * vf == 0 is worth special-casing rather than letting LERP2 do nothing twelve
 * times: build_linear clamps the fraction to 0 at the bottom edge, so this
 * fires on real rows and not only in theory. */
static void scale_linear(const mr_scaler *s, const mr_surface *src,
                         mr_surface *dst, int r0, int r1)
{
    int y, x, w, h;
    mr_u32 *d;

    w = s->dst_w;
    h = s->dst_h;

    for (y = r0; y < r1; y++) {
        const mr_u32 *row0 = src->px + (size_t)s->ymap[y] * src->pitch;
        const mr_u32 *row1 = row0;
        int vf = s->yfrac[y];

        if (s->ymap[y] + 1 < s->src_h)
            row1 = row0 + src->pitch;
        else
            vf = 0;

        /* Reversed destination row: the DIB is bottom-up. */
        d = dst->px + (size_t)(h - 1 - y) * dst->pitch;

        for (x = 0; x < w; x++) {
            int x0 = s->xmap[x];
            int hf = s->xfrac[x];
            int x1 = x0 + 1 < s->src_w ? x0 + 1 : x0;
            mr_u32 top, bot;

            top = hf ? LERP2(row0[x0], row0[x1], hf) : row0[x0];
            if (!vf) {
                d[x] = top;
                continue;
            }
            bot = hf ? LERP2(row1[x0], row1[x1], hf) : row1[x0];
            d[x] = LERP2(top, bot, vf);
        }
    }
}

/* The 1:1 case still has to reverse the row order. */
static void copy_flipped(const mr_scaler *s, const mr_surface *src,
                         mr_surface *dst, int r0, int r1)
{
    int y, h = s->dst_h;

    for (y = r0; y < r1; y++) {
        memcpy(dst->px + (size_t)(h - 1 - y) * dst->pitch,
               src->px + (size_t)y * src->pitch,
               (size_t)s->dst_w * sizeof(mr_u32));
    }
}

void mr_scale_rows(const mr_scaler *s, const mr_surface *src,
                   mr_surface *dst, int r0, int r1)
{
    if (r0 < 0)
        r0 = 0;
    if (r1 > s->dst_h)
        r1 = s->dst_h;
    if (r0 >= r1)
        return;

    switch (s->kind) {
    case MR_SCALE_COPY:     copy_flipped(s, src, dst, r0, r1); break;
    case MR_SCALE_HALF:     scale_half(s, src, dst, r0, r1);   break;
    case MR_SCALE_POINT:    scale_point(s, src, dst, r0, r1);  break;
    case MR_SCALE_BILINEAR: scale_linear(s, src, dst, r0, r1); break;
    }
}

/* --- the fit ------------------------------------------------------------- */

static int g_fx, g_fy, g_fw, g_fh;      /* the 16:9 rect, in output pixels */
static int g_ow, g_oh;                  /* the whole output */

void mr_fit_set(int out_w, int out_h)
{
    int w, h;

    /* Below this there is nothing sensible to draw, and the presenter's
     * buffers would be degenerate. Declining leaves the previous fit in
     * place, which is what a minimised window wants. */
    if (out_w < 16 || out_h < 16)
        return;

    /* Largest 16:9 rectangle that fits, centred. */
    w = out_w;
    h = w * MR_STAGE_H / MR_STAGE_W;
    if (h > out_h) {
        h = out_h;
        w = h * MR_STAGE_W / MR_STAGE_H;
    }

    g_fx = (out_w - w) / 2;
    g_fy = (out_h - h) / 2;
    g_fw = w;
    g_fh = h;
    g_ow = out_w;
    g_oh = out_h;
}

void mr_fit_get(int *x, int *y, int *w, int *h, int *out_w, int *out_h)
{
    if (x)     *x = g_fx;
    if (y)     *y = g_fy;
    if (w)     *w = g_fw;
    if (h)     *h = g_fh;
    if (out_w) *out_w = g_ow;
    if (out_h) *out_h = g_oh;
}

int mr_fit_map_point(int ox, int oy, int *sx, int *sy)
{
    if (g_fw <= 0 || g_fh <= 0)
        return 0;

    ox -= g_fx;
    oy -= g_fy;
    if (ox < 0 || oy < 0 || ox >= g_fw || oy >= g_fh)
        return 0;

    *sx = ox * MR_STAGE_W / g_fw;
    *sy = oy * MR_STAGE_H / g_fh;
    return 1;
}

/* --- the letterbox bars -------------------------------------------------- */

void mr_border_row(const mr_surface *tex, int bar_h, int row,
                   mr_u32 *out, int out_w)
{
    const mr_u32 *src;
    int sy, x, tile;

    if (!tex || !out || out_w <= 0 || bar_h <= 0 || tex->w <= 0 || tex->h <= 0)
        return;

    if (row < 0)      row = 0;
    if (row >= bar_h) row = bar_h - 1;

    /* The same mapping mr_scale_init uses for its ymap: the centre of the
     * destination row projected back, so a 1:1 bar is the identity and a
     * stretched one does not drop its first or last source row. */
    sy = ((row * 2 + 1) * tex->h) / (bar_h * 2);
    if (sy >= tex->h) sy = tex->h - 1;

    src = tex->px + (size_t)sy * tex->pitch;

    /* One tile is the texture scaled by the *same* factor both ways, so the
     * art keeps its proportions and the repeat is a repeat of the whole
     * picture rather than of a strip of it. Scaling only the height and
     * tiling the texture at its stored width is the thing this is not: a
     * 1672-wide image in a 640-wide bar then never wraps at all, and what
     * reaches the screen is its left-hand third, squashed. */
    tile = tex->w * bar_h / tex->h;
    if (tile < 1)
        tile = 1;

    if (tex->has_alpha) {
        for (x = 0; x < out_w; x++) {
            mr_u32 p = src[((x % tile) * 2 + 1) * tex->w / (tile * 2)];
            unsigned a = MR_A(p);
            /* Over black: the destination contributes nothing, so this is
             * just the source scaled by its own alpha.
             *
             * MR_FLOOR255 and not MR_DIV255: the divide here truncated, and
             * this is a bar fill where nobody is going to notice a rounding
             * change -- which is exactly why it should not be made silently.
             * MR_FLOOR255 is `x / 255` exactly, proven for every input to
             * 65535 in blendtest, so the bytes are unchanged.
             *
             * Three hardware divides a pixel otherwise, which is the thing
             * CLAUDE.md forbids per-pixel; found by reading rather than by
             * profiling, since a letterbox bar is painted on a resize and not
             * per frame. Cheap to fix and one less place for the next person
             * to copy the pattern from. */
            out[x] = MR_RGB(MR_FLOOR255((unsigned)MR_R(p) * a),
                            MR_FLOOR255((unsigned)MR_G(p) * a),
                            MR_FLOOR255((unsigned)MR_B(p) * a));
        }
    } else {
        for (x = 0; x < out_w; x++)
            out[x] = src[((x % tile) * 2 + 1) * tex->w / (tile * 2)]
                   | 0xFF000000UL;
    }
}

/* yuv.c - YUV 4:2:0 to 0xAARRGGBB, scaled, in one pass. See yuv.h.
 *
 * Two paths, and the split is by destination size rather than by quality:
 *
 *   MR_YUV_DIRECT   the destination is the frame's own size. Walks both
 *                   planes with pointer arithmetic and shares each chroma
 *                   sample between the two luma pixels beside it. This is
 *                   what runs at the default window size, and it is the path
 *                   the MMX kernel replaces.
 *
 *   MR_YUV_SCALED   anything else. Nearest neighbour through xmap/ymap, as
 *                   MR_SCALE_POINT does for the stage.
 *
 * There is no bilinear path and the omission is deliberate. mrpy.ini's
 * `bilinear` exists because the *stage* is authored at 1280x720 and a window
 * is usually a non-integer fraction of it, where point sampling shimmers on
 * text. A frame of video is already a resampled photograph with ringing in
 * it, it is replaced 25 times a second, and the one thing a viewer notices is
 * a frame arriving late. Interpolating it costs three times the arithmetic to
 * improve something nobody is reading. If this is ever wanted, the place for
 * it is here and not in a second pass -- see yuv.h on why there is only one.
 *
 * --- rows are independent, and that costs something ------------------------
 *
 * A 4:2:0 chroma sample is shared by a 2x2 luma block, so the natural loop
 * does two rows at a time and computes the chroma contribution once for four
 * pixels. This does one row at a time and computes it once for two, which is
 * three extra multiplies per 2x2 block.
 *
 * Paid on purpose. The row range is a parameter, as mr_scale_rows' is, so a
 * caller may ask for rows [7, 9) and the two-row loop cannot answer that
 * without a special case at each end. Today the only caller asks for the
 * whole picture -- a band of video makes no sense, since every pixel changes
 * every frame -- but a harness that converts one row and compares it against
 * the vendor function is worth more than those multiplies, and that is a test
 * that only exists if a single row is askable. The MMX kernel gets the
 * chroma cost back by doing eight pixels at once.
 */

#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "yuv.h"
#include "cpu.h"

/* --- the clamp table ------------------------------------------------------
 *
 * pl_mpeg clamps with two branches per component (plm_clamp, libs/pl_mpeg.h).
 * That is twelve unpredictable branches per 2x2 block, and on a deeply
 * pipelined in-order Pentium III a mispredict costs more than the multiply it
 * is guarding. A table makes it a load.
 *
 * The range it must cover is set by the coefficients, not guessed. With Y in
 * 0..255 and chroma in -128..127, the 16.16 terms in mr_yuv_rows below give
 *
 *     y   in [-19, 278]        ((0-16)*76309)>>16 .. ((255-16)*76309)>>16
 *     +r  in [-205, 202]
 *     -g  in [-154, 153]
 *     +b  in [-258, 256]
 *
 * so the widest result is y+b, in [-277, 534]. CLAMP_BIAS 320 and 1024
 * entries cover [-320, 703] with room at both ends; an input outside that
 * cannot be produced by any 8-bit frame, and the table is indexed without a
 * bounds check on exactly that argument.
 *
 * Note the reliance on `>>` of a negative int being an arithmetic shift. C89
 * leaves that implementation-defined, and pl_mpeg's own conversion relies on
 * it identically -- which is the point: the two agree or yuvtest says so.
 * Every compiler this builds under (Open Watcom, gcc) shifts arithmetically.
 */
#define CLAMP_BIAS  320
#define CLAMP_SIZE  1024

static mr_u8 g_clamp[CLAMP_SIZE];
static int   g_clamp_ready;

static void clamp_init(void)
{
    int i, v;

    if (g_clamp_ready)
        return;

    for (i = 0; i < CLAMP_SIZE; i++) {
        v = i - CLAMP_BIAS;
        if (v < 0)
            v = 0;
        else if (v > 255)
            v = 255;
        g_clamp[i] = (mr_u8)v;
    }
    g_clamp_ready = 1;
}

#define CLAMP(v) g_clamp[(v) + CLAMP_BIAS]

/* --- SIMD ----------------------------------------------------------------
 *
 * MR_YUV_MMX is defined by the makefile for the two targets that have a
 * Pentium III under them, and is never defined for the host harness build --
 * the kernel is Open Watcom inline assembly and gcc would not compile it, and
 * a kernel the suite appears to cover but does not is worse than one it
 * plainly does not. See yuv.h on probe/mmxcheck.c, which is how the kernel is
 * actually tested.
 *
 * And "have" is about the machine as well as the build: MR_YUV_MMX only says
 * the kernel was compiled in, so the processor is asked too. Both kernels ask
 * in their own have_simd() rather than at a call site, because movie.c sets
 * this one and main.c sets the blend and neither should have to know. See
 * cpu.h.
 */
static int g_simd = 1;

int mr_yuv_have_simd(void)
{
#ifdef MR_YUV_MMX
    return mr_cpu_has_mmx();
#else
    return 0;
#endif
}

int mr_yuv_simd(void)
{
    return mr_yuv_have_simd() && g_simd;
}

void mr_yuv_set_simd(int on)
{
    g_simd = on ? 1 : 0;
}

/* --- init ---------------------------------------------------------------- */

void mr_yuv_free(mr_yuv_scaler *s)
{
    if (!s)
        return;
    free(s->xmap);
    free(s->ymap);
    s->xmap = NULL;
    s->ymap = NULL;
    s->src_w = s->src_h = s->dst_w = s->dst_h = 0;
}

int mr_yuv_init(mr_yuv_scaler *s, int src_w, int src_h,
                int dst_w, int dst_h)
{
    int i;

    if (!s)
        return 0;

    memset(s, 0, sizeof(*s));

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return 0;

    clamp_init();

    s->src_w = src_w;
    s->src_h = src_h;
    s->dst_w = dst_w;
    s->dst_h = dst_h;

    if (src_w == dst_w && src_h == dst_h) {
        s->kind = MR_YUV_DIRECT;
        return 1;
    }

    s->kind = MR_YUV_SCALED;

    s->xmap = (int *)malloc((size_t)dst_w * sizeof(int));
    s->ymap = (int *)malloc((size_t)dst_h * sizeof(int));
    if (!s->xmap || !s->ymap) {
        mr_yuv_free(s);
        return 0;
    }

    /* Centre of the destination pixel mapped back, which is what
     * MR_SCALE_POINT does: (i + 0.5) * src / dst, in integers. Sampling from
     * the pixel's left edge instead biases the whole picture half a
     * destination pixel left and up, and at a 2:1 downscale that is a whole
     * source pixel. */
    for (i = 0; i < dst_w; i++) {
        int v = (int)(((long)i * 2 + 1) * src_w / ((long)dst_w * 2));
        s->xmap[i] = v < src_w ? v : src_w - 1;
    }
    for (i = 0; i < dst_h; i++) {
        int v = (int)(((long)i * 2 + 1) * src_h / ((long)dst_h * 2));
        s->ymap[i] = v < src_h ? v : src_h - 1;
    }

    return 1;
}

/* --- the fit ------------------------------------------------------------- */

void mr_yuv_fit(int src_w, int src_h, int box_w, int box_h,
                int *out_w, int *out_h)
{
    int w, h;

    if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    /* Fit by width, and if that is too tall, fit by height instead. Done in
     * longs: 1280 * 1024 overflows nothing, but a 4096-wide mode times a
     * 4096-tall frame would overflow a 16-bit int and this file is read by
     * people who have seen that happen. */
    w = box_w;
    h = (int)(((long)box_w * src_h) / src_w);
    if (h > box_h) {
        h = box_h;
        w = (int)(((long)box_h * src_w) / src_h);
    }

    /* Even on both axes; see yuv.h. Rounding down can never push the picture
     * outside the box. */
    w &= ~1;
    h &= ~1;
    if (w < 2) w = 2;
    if (h < 2) h = 2;

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* --- the bars ------------------------------------------------------------ */

void mr_yuv_bars(mr_surface *dst, int vx, int vy, int vw, int vh)
{
    int row;
    int r0, r1;                 /* picture rows the video covers */

    if (!dst || !dst->px)
        return;

    /* Clamp the rectangle into the surface before using it to decide what is
     * outside: a rectangle that has gone stale against a resized buffer must
     * not make this skip rows that are really bars, which is the exact fault
     * it exists to clean up after. */
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vw < 0) vw = 0;
    if (vh < 0) vh = 0;
    if (vx > dst->w) vx = dst->w;
    if (vy > dst->h) vy = dst->h;
    if (vx + vw > dst->w) vw = dst->w - vx;
    if (vy + vh > dst->h) vh = dst->h - vy;

    r0 = vy;
    r1 = vy + vh;

    for (row = 0; row < dst->h; row++) {
        /* Bottom-up, as mr_yuv_rows writes. */
        mr_u32 *out = dst->px + (size_t)(dst->h - 1 - row) * dst->pitch;

        if (row < r0 || row >= r1) {
            /* A bar row: all of it. */
            memset(out, 0, (size_t)dst->w * sizeof(mr_u32));
        } else {
            /* A picture row: only the columns beside the video. Usually
             * none, since a letterboxed video is normally full width. */
            if (vx > 0)
                memset(out, 0, (size_t)vx * sizeof(mr_u32));
            if (vx + vw < dst->w)
                memset(out + vx + vw, 0,
                       (size_t)(dst->w - vx - vw) * sizeof(mr_u32));
        }
    }
}

/* --- conversion ---------------------------------------------------------- */

#ifdef MR_YUV_MMX
/* Defined in yuvmmx.c. Converts the four-pixel groups of one row of the
 * MR_YUV_DIRECT case and returns how many pixels that was -- n rounded down
 * to a multiple of four. The loop below finishes the remainder. */
int mr_yuv_row_mmx(mr_u32 *dst, const mr_u8 *y,
                   const mr_u8 *cb, const mr_u8 *cr, int n);
#endif

/* One destination row, destination size == frame size. */
static void row_direct(mr_u32 *dst, const mr_u8 *y,
                       const mr_u8 *cb, const mr_u8 *cr, int n)
{
    int x = 0;

#ifdef MR_YUV_MMX
    /* The kernel takes what it can and says so, rather than the row either
     * being all MMX or all C. mr_yuv_fit rounds the picture to an even size
     * and not to a multiple of four, so an all-or-nothing kernel would fall
     * back to scalar for whole frames at perfectly ordinary window sizes.
     *
     * The loop below then runs for the last two pixels and no more, from
     * wherever the kernel stopped -- which is also what makes the C path the
     * single definition of the conversion: the tail is not a second
     * implementation, it is the same loop entered late. */
    if (mr_yuv_simd())
        x = mr_yuv_row_mmx(dst, y, cb, cr, n);
#endif

    /* Two luma pixels per chroma sample. The odd tail -- which mr_yuv_fit
     * makes impossible for the destination, but a frame handed straight in by
     * a harness may well have -- takes the last chroma sample again. */
    for (; x + 1 < n; x += 2) {
        int icr = (int)cr[x >> 1] - 128;
        int icb = (int)cb[x >> 1] - 128;
        int r = (icr * 104597) >> 16;
        int g = (icb * 25674 + icr * 53278) >> 16;
        int b = (icb * 132201) >> 16;
        int y0 = (((int)y[x]     - 16) * 76309) >> 16;
        int y1 = (((int)y[x + 1] - 16) * 76309) >> 16;

        dst[x]     = MR_ARGB(255, CLAMP(y0 + r), CLAMP(y0 - g),
                             CLAMP(y0 + b));
        dst[x + 1] = MR_ARGB(255, CLAMP(y1 + r), CLAMP(y1 - g),
                             CLAMP(y1 + b));
    }

    if (x < n) {
        int icr = (int)cr[x >> 1] - 128;
        int icb = (int)cb[x >> 1] - 128;
        int r = (icr * 104597) >> 16;
        int g = (icb * 25674 + icr * 53278) >> 16;
        int b = (icb * 132201) >> 16;
        int y0 = (((int)y[x] - 16) * 76309) >> 16;

        dst[x] = MR_ARGB(255, CLAMP(y0 + r), CLAMP(y0 - g), CLAMP(y0 + b));
    }
}

/* One destination row, scaled.
 *
 * The chroma terms are recomputed only when the source chroma column changes.
 * xmap is monotonic, so at any downscale gentler than 2:1 that is most
 * pixels, and at an upscale it is nearly all of them. Worth the two compares
 * because the three multiplies behind them are the bulk of the per-pixel
 * cost once the luma multiply is counted. */
static void row_scaled(mr_u32 *dst, const mr_u8 *y,
                       const mr_u8 *cb, const mr_u8 *cr,
                       const int *xmap, int n)
{
    int x;
    int last_c = -1;
    int r = 0, g = 0, b = 0;

    for (x = 0; x < n; x++) {
        int sx = xmap[x];
        int cx = sx >> 1;
        int y0;

        if (cx != last_c) {
            int icr = (int)cr[cx] - 128;
            int icb = (int)cb[cx] - 128;
            r = (icr * 104597) >> 16;
            g = (icb * 25674 + icr * 53278) >> 16;
            b = (icb * 132201) >> 16;
            last_c = cx;
        }

        y0 = (((int)y[sx] - 16) * 76309) >> 16;
        dst[x] = MR_ARGB(255, CLAMP(y0 + r), CLAMP(y0 - g), CLAMP(y0 + b));
    }
}

/* One destination row, whichever scaler is in force and wherever it goes.
 *
 * Extracted so that the picture-buffer path and the straight-to-display path
 * below cannot drift: they differ only in the address of the row, and the
 * conversion -- the part yuvtest pins against pl_mpeg -- stays one
 * definition with one caller per destination. */
static void convert_row(const mr_yuv_scaler *s, const mr_yuv_frame *f,
                        mr_u32 *out, int row)
{
    if (s->kind == MR_YUV_DIRECT) {
        const mr_u8 *y  = f->y  + (size_t)row * f->y_pitch;
        const mr_u8 *cb = f->cb + (size_t)(row >> 1) * f->c_pitch;
        const mr_u8 *cr = f->cr + (size_t)(row >> 1) * f->c_pitch;

        row_direct(out, y, cb, cr, s->dst_w);
    } else {
        int sy = s->ymap[row];
        const mr_u8 *y  = f->y  + (size_t)sy * f->y_pitch;
        const mr_u8 *cb = f->cb + (size_t)(sy >> 1) * f->c_pitch;
        const mr_u8 *cr = f->cr + (size_t)(sy >> 1) * f->c_pitch;

        row_scaled(out, y, cb, cr, s->xmap, s->dst_w);
    }
}

void mr_yuv_rows(const mr_yuv_scaler *s, const mr_yuv_frame *f,
                 mr_surface *dst, int dx, int dy, int r0, int r1)
{
    int row;

    if (!s || !f || !dst || !dst->px || !f->y || !f->cb || !f->cr)
        return;

    /* The scaler was built for a particular frame size and indexes the planes
     * with no further checking. A sequence header mid-stream may change that
     * size; movie.c rebuilds the scaler when it does, and this is the
     * backstop for the frame that arrives in between. */
    if (s->src_w != f->w || s->src_h != f->h)
        return;

    clamp_init();

    if (r0 < 0)
        r0 = 0;
    if (r1 > s->dst_h)
        r1 = s->dst_h;

    /* Clip to the destination surface. dy + row must land inside it, and so
     * must the whole row starting at dx. */
    if (dx < 0 || dx + s->dst_w > dst->w)
        return;
    if (dy + r0 < 0)
        r0 = -dy;
    if (dy + r1 > dst->h)
        r1 = dst->h - dy;

    for (row = r0; row < r1; row++) {
        /* Bottom-up, as mr_scale_rows writes: destination row (dy + row)
         * lands at buffer row (dst->h - 1 - (dy + row)). */
        mr_u32 *out = dst->px +
                      (size_t)(dst->h - 1 - (dy + row)) * dst->pitch + dx;

        convert_row(s, f, out, row);
    }
}

/* --- straight to the display ----------------------------------------------
 *
 * Same conversion, same row order, but the destination is the display's own
 * memory rather than the picture buffer -- so the pack pass, and the whole
 * round trip through g_buf, does not happen.
 *
 * This exists because of a measurement on the target that contradicted the
 * one this file was designed around. A frame of `ending.mpg` at 640x320 cost
 * `convert 14.98ms, pack 36.73ms`, and the pack is not arithmetic: for a
 * standard 32bpp mode mr_pack_rows is a memcpy per row. 640x360x4 bytes in
 * 36.73ms is 25MB/s, which is not a plausible memory bandwidth -- it is
 * 159ns for every four-byte store. vbedos.c maps the linear framebuffer with
 * DPMI 0800h and sets no cache attribute, so the mapping is uncached and the
 * stores never combine: the cost is one bus transaction per store
 * *instruction*, independent of how wide it is.
 *
 * The experiment that proved it was a 16bpp run, which should have halved the
 * bytes written and did not: pack went *up*, to 40.92ms, because the same
 * number of stores now also carried the pack16 arithmetic. Narrower stores
 * buy nothing against a per-transaction cost. Wider ones do, and the MMX
 * kernel already issues 8-byte movq stores -- it just issued them into main
 * memory for the pack loop to read straight back out again.
 *
 * So there is no new arithmetic here, and that is the point: for a plain
 * BGRA32 display the pixels the kernel already produces *are* the display's
 * pixels, and fusing is a question of where to point it. Only that case is
 * taken; everything else returns 0 and the caller does the two passes as
 * before. mr_pack_is_bgra32 is the predicate, and it lives in pack.c because
 * that is where the format is interpreted.
 *
 * The contract is mr_pack_rows', not mr_yuv_rows': `dst` and `pitch` are the
 * same pair the caller would hand the pack, buffer row b landing at
 * dst + b * pitch, and pitch may be negative. That is deliberate -- both
 * presenters already compute that pair for their own orientation, and making
 * this one agree with mr_yuv_rows instead would have meant a third
 * convention for whoever next reads all three.
 */
int mr_yuv_rows_fused(const mr_yuv_scaler *s, const mr_yuv_frame *f,
                      const mr_pack_fmt *fmt, mr_u8 *dst, long pitch,
                      int dst_w, int dst_h, int dx, int dy, int r0, int r1)
{
    int row;

    if (!s || !f || !fmt || !dst || !f->y || !f->cb || !f->cr)
        return 0;

    if (!mr_pack_is_bgra32(fmt))
        return 0;               /* caller converts, then packs */

    /* Same backstop as mr_yuv_rows: a sequence header mid-stream can change
     * the frame size, and the scaler indexes the planes without checking. */
    if (s->src_w != f->w || s->src_h != f->h)
        return 0;

    clamp_init();

    if (r0 < 0)
        r0 = 0;
    if (r1 > s->dst_h)
        r1 = s->dst_h;

    /* Clip to the picture buffer the caller described, exactly as mr_yuv_rows
     * clips to its surface. Refusing rather than clamping the x range: a row
     * that does not fit is a caller that has miscomputed the fit, and
     * silently writing a short row would put a stripe of stale framebuffer
     * down the side of the picture. */
    if (dx < 0 || dx + s->dst_w > dst_w)
        return 0;
    if (dy + r0 < 0)
        r0 = -dy;
    if (dy + r1 > dst_h)
        r1 = dst_h - dy;

    for (row = r0; row < r1; row++) {
        mr_u32 *out = (mr_u32 *)(dst +
                      (long)(dst_h - 1 - (dy + row)) * pitch) + dx;

        convert_row(s, f, out, row);
    }

    return 1;
}

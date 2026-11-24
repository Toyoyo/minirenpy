/* gfx.c - surface allocation and software compositing.
 *
 * Everything composites into 32bpp 0xAARRGGBB surfaces. There is no GDI in
 * this file at all; presentation lives in present.c. Keeping the compositor
 * pure software means transitions behave identically on 16, 24 and 32 bit
 * displays.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mr.h"
#include "blendmmx.h"
#include "cpu.h"

/* --- the compositor profile ----------------------------------------------
 *
 * See mr.h for what the buckets are and why the split matters. Everything
 * here is inert until mr_gfx_profile_enable is handed a clock.
 */
static unsigned int (*g_clock)(void);
static mr_gfx_prof    g_gp;

void mr_gfx_profile_enable(unsigned int (*clock)(void))
{
    g_clock = clock;
}

void mr_gfx_profile(mr_gfx_prof *out)
{
    if (out)
        *out = g_gp;
    memset(&g_gp, 0, sizeof(g_gp));
}

/* Charge elapsed time, pixels and one call to a bucket. `t0` is the reading
 * taken immediately before the work, and the unsigned subtraction is correct
 * across a wrap of the platform's tick counter.
 *
 * `px` is the clipped destination rectangle: the work the loop was offered,
 * not the pixels it chose to touch. A blend loop that skips a fully
 * transparent pixel still paid to load and test it, and a sprite's
 * transparent border is part of what blending a sprite costs -- charging only
 * the visible pixels would make a mostly-empty sprite look arbitrarily
 * expensive per pixel and invite the wrong fix. */
#define PROF(bucket, t0, px)                                                 \
    do {                                                                     \
        if (g_clock) {                                                       \
            g_gp.bucket##_ms += (long)(g_clock() - (t0));                    \
            g_gp.bucket##_px += (long)(px);                                   \
            g_gp.bucket##_n++;                                               \
        }                                                                    \
    } while (0)

mr_surface *mr_surface_new(int w, int h)
{
    mr_surface *s;

    if (w <= 0 || h <= 0)
        return NULL;

    s = (mr_surface *)malloc(sizeof(mr_surface));
    if (!s)
        return NULL;

    /* One allocation for the pixels. 1280x720x4 is 3.6MB, which Win32s
     * handles as a single VirtualAlloc-backed block without trouble. */
    s->px = (mr_u32 *)malloc((size_t)w * (size_t)h * sizeof(mr_u32));
    if (!s->px) {
        free(s);
        return NULL;
    }

    s->w = w;
    s->h = h;
    s->pitch = w;
    s->has_alpha = 0;
    return s;
}

void mr_surface_free(mr_surface *s)
{
    if (!s)
        return;
    free(s->px);
    free(s);
}

/* Declared with the clip stack below, which is further down the file only
 * because that is where its explanation lives. */
void mr_clip_rect(const mr_surface *s, int *x, int *y, int *w, int *h);

void mr_surface_fill(mr_surface *s, mr_u32 color)
{
    int y, x, cx, cy, cw, ch;
    mr_u32 *row;
    unsigned int t0 = 0;

    /* Clipped, like every other pixel writer here. render_list clears the
     * stage through this before compositing, and when the frame loop is
     * repainting only the dialogue box that clear has to stay inside it --
     * otherwise the partial frame blanks the screen and then redraws a
     * quarter of it, which is precisely what it did. */
    mr_clip_rect(s, &cx, &cy, &cw, &ch);

    if (g_clock)
        t0 = g_clock();

    for (y = cy; y < cy + ch; y++) {
        row = s->px + (size_t)y * s->pitch;
        for (x = cx; x < cx + cw; x++)
            row[x] = color;
    }

    PROF(fill, t0, (long)cw * ch);
}

void mr_surface_copy(mr_surface *dst, const mr_surface *src)
{
    int y, n, rows;
    unsigned int t0 = 0;

    n = dst->w < src->w ? dst->w : src->w;

    if (g_clock)
        t0 = g_clock();

    rows = 0;
    for (y = 0; y < dst->h && y < src->h; y++) {
        memcpy(dst->px + (size_t)y * dst->pitch,
               src->px + (size_t)y * src->pitch,
               (size_t)n * sizeof(mr_u32));
        rows++;
    }

    PROF(copy, t0, (long)n * rows);
}

/* See mr.h for why this is worth a pass over the pixels.
 *
 * ANDing whole pixels rather than testing alpha bytes: the AND of every pixel
 * has 0xFF in its top byte exactly when every alpha was 255, so the inner
 * loop is a full-width load and an AND with no branch and no comparison. A
 * byte test per pixel would be the `movzx bx, byte ptr` partial-register stall
 * this file has been bitten by twice (NOTES.md, the pack loops).
 *
 * Accumulated per row and tested per row, which buys the early exit without
 * putting a branch in the inner loop -- and a sprite's first row is almost
 * always fully transparent, so the images that cannot benefit pay for one
 * row. Not charged to a profile bucket: it is a property query, not
 * compositing, and it happens once per surface rather than once per frame. */
int mr_surface_opaque(const mr_surface *s)
{
    int y, x;
    const mr_u32 *p;
    mr_u32 all;

    if (!s || !s->px || s->w <= 0 || s->h <= 0)
        return 1;

    for (y = 0; y < s->h; y++) {
        p = s->px + (size_t)y * s->pitch;
        all = 0xFFFFFFFFu;
        for (x = 0; x < s->w; x++)
            all &= p[x];
        if ((all & 0xFF000000u) != 0xFF000000u)
            return 0;
    }

    return 1;
}

/* --- the MMX blend switch -------------------------------------------------
 *
 * Both halves of the question in one place, beside the scalar loop they
 * choose between, following yuv.c's arrangement exactly.
 *
 * MR_BLEND_MMX is defined by the two Watcom makefiles and never by the gcc
 * harness build: that one could not compile Watcom inline assembly, and a
 * kernel the suite appears to cover but does not is worse than one it plainly
 * does not. So on the host mr_blend_have_simd() is 0 and every blend takes
 * the scalar path -- which is also what every harness claim in the suite is
 * actually testing, kernel or no kernel.
 *
 * The run-time switch reads mrpy.ini's `simd`, the same key the colour
 * conversion reads, because a machine that wants one off wants the other
 * off.
 *
 * **"Have" means this machine, not this build.** The CPUID question is asked
 * here rather than at the call site on purpose: there are two call sites for
 * the two kernels and a third would be added by whoever adds the next one, so
 * the one place that cannot be forgotten is the predicate itself. A 486
 * running a Watcom build has MR_BLEND_MMX defined and no `punpcklbw`, and the
 * only honest answer to "have simd" there is no. See cpu.h. */
static int g_blend_simd = 1;

int mr_blend_have_simd(void)
{
#ifdef MR_BLEND_MMX
    return mr_cpu_has_mmx();
#else
    return 0;
#endif
}

int mr_blend_simd(void)
{
    return mr_blend_have_simd() && g_blend_simd;
}

void mr_blend_set_simd(int on)
{
    g_blend_simd = on ? 1 : 0;
}

/* Hash every pixel outside rows [y, y+h). See mr.h for what this is for and
 * why the hash is word-wise. Like mr_surface_opaque, it is a query and not
 * compositing, so it is charged to no profile bucket -- which here is also the
 * honest accounting, because nothing the player sees ever calls it. */
mr_u32 mr_surface_digest_outside(const mr_surface *s, int y, int h)
{
    mr_u32 hash = 2166136261u;
    int row, x;
    const mr_u32 *p;

    if (!s || !s->px || s->w <= 0 || s->h <= 0)
        return 0;

    for (row = 0; row < s->h; row++) {
        /* `h > 0` is redundant and deliberate: for h <= 0 the comparison that
         * follows is already false for every row, so deleting it changes
         * nothing. It is here to state the documented contract -- h <= 0
         * digests all of it -- where a reader will look for it, because the
         * dangerous misreading is the opposite one. A version that treated
         * h <= 0 as "skip everything" would make every frame's digest equal
         * and the probe would report a 100% opportunity that does not exist.
         * blendtest pins that, and pins it with a break that fails. */
        if (h > 0 && row >= y && row < y + h)
            continue;
        p = s->px + (size_t)row * s->pitch;
        for (x = 0; x < s->w; x++) {
            hash ^= p[x];
            hash *= 16777619u;
        }
    }

    return hash;
}

/* --- blur (im.Blur) -------------------------------------------------------
 *
 * im.Blur approximates a Gaussian by running several box blurs, each box blur
 * being a one-dimensional pass on x followed by one on y. This is Ren'Py's
 * own construction, transcribed from src/core.c:552-760 (blur_filters,
 * blur32_core, linblur32_core), because the point is to look the same and not
 * to be a better blur:
 *
 *   - three passes (`n = 3` in blur32_core);
 *   - box widths from Kovesi's "Fast Almost-Gaussian Filtering", so the first
 *     `m` passes use the lower width and the rest the upper one;
 *   - edges extended by repeating the first and last pixel of the line, which
 *     is what the lr/rr accumulator seeding in linblur32_core amounts to;
 *   - every channel treated alike, alpha included and not premultiplied.
 *
 * Ren'Py works on byte pointers over an SDL surface; this works on mr_u32 and
 * unpacks, which is the same arithmetic without depending on the byte order
 * of a pixel.
 */

/* The AF widths for a given standard deviation and pass count.
 * core.c:552, equations 3 and 5 of the paper. */
static void blur_filters(float sigma, int n, int *wl, int *wu, int *m)
{
    float num, den;
    int   mm;

    *wl = (int)sqrt((double)(12.0f * sigma * sigma / (float)n + 1.0f));
    if (*wl % 2 == 0)
        (*wl)--;
    if (*wl < 1)
        *wl = 1;
    *wu = *wl + 2;

    num = 12.0f * sigma * sigma - (float)(n * *wl * *wl)
          - (float)(4 * n * *wl) - (float)(3 * n);
    den = (float)(-4 * *wl - 4);

    /* C89 has no round(); these are small magnitudes and the sign can go
     * either way for a very small sigma, so round away from zero by hand. */
    num /= den;
    mm = num < 0.0f ? -(int)(-num + 0.5f) : (int)(num + 0.5f);
    if (mm < 0) mm = 0;
    if (mm > n) mm = n;
    *m = mm;
}

/* The quotient of every accumulator value a line can hold, so the inner loop
 * looks up what it would otherwise divide.
 *
 * This is the one place the transcription departs from core.c's shape, and it
 * is not cosmetic: core.c divides four times per output pixel and there are
 * six passes, so a full-screen background costs 22 million integer divisions.
 * A Pentium III takes around 40 cycles over one of those -- call it two
 * seconds of dividing, which dwarfs everything else about the blur. The
 * divisor is constant for a whole pass and an accumulator never exceeds
 * 255 * div, so the quotients fit in a table built once per pass with a few
 * thousand divisions, and the result is bit-identical by construction rather
 * than by approximation. (A reciprocal multiply would be the other way to do
 * it, and would need a 64-bit intermediate to stay exact at these divisors --
 * which on Watcom means it would not be exact.)
 *
 * 129 covers a radius of 64: far past anything blur_filters produces for a
 * plausible sigma, and the fallback below is correct anyway. */
#define MR_BLUR_MAXDIV 129

static unsigned char g_quot[255 * MR_BLUR_MAXDIV + 1];

static const unsigned char *quot_table(int div)
{
    int i, hi;

    if (div > MR_BLUR_MAXDIV)
        return NULL;
    hi = 255 * div;
    for (i = 0; i <= hi; i++)
        g_quot[i] = (unsigned char)(i / div);
    return g_quot;
}

/* One box-blur pass over a single line of `n` pixels, reading src with stride
 * `sstride` and writing dst with `dstride`. The accumulator form is linear in
 * the line length regardless of the radius, which is the whole reason Ren'Py
 * builds a Gaussian out of these.
 *
 * `radius` is already clamped to the line by the caller, and `q` is the table
 * for its divisor, or NULL to divide. */
#define MR_BQ(v) ((int)(q ? q[v] : (v) / div))

static void blur_line(const mr_u32 *src, mr_u32 *dst, int n,
                      int sstride, int dstride, int radius,
                      const unsigned char *q)
{
    int c, end, div;
    int sa, sr, sg, sb;
    int la, lr, lg, lb;      /* the leftmost pixel, repeated off the edge */
    int ra, rr, rg, rb;      /* the rightmost, likewise */
    const mr_u32 *lead, *trail;
    mr_u32 *out;
    mr_u32 p;

    div = radius * 2 + 1;
    lead = src;
    trail = src;
    out = dst;

    p = *lead;
    la = MR_A(p); lr = MR_R(p); lg = MR_G(p); lb = MR_B(p);

    sa = la * radius;
    sr = lr * radius;
    sg = lg * radius;
    sb = lb * radius;

    for (c = 0; c < radius; c++) {
        p = *lead;
        sa += MR_A(p); sr += MR_R(p); sg += MR_G(p); sb += MR_B(p);
        lead += sstride;
    }

    /* The left half of the kernel hangs off the line. */
    for (c = 0; c < radius; c++) {
        p = *lead;
        sa += MR_A(p); sr += MR_R(p); sg += MR_G(p); sb += MR_B(p);
        lead += sstride;

        *out = MR_ARGB(MR_BQ(sa), MR_BQ(sr), MR_BQ(sg), MR_BQ(sb));
        out += dstride;

        sa -= la; sr -= lr; sg -= lg; sb -= lb;
    }

    /* Fully on the line. */
    end = n - radius - 1;
    for (; c < end; c++) {
        p = *lead;
        sa += MR_A(p); sr += MR_R(p); sg += MR_G(p); sb += MR_B(p);
        lead += sstride;

        *out = MR_ARGB(MR_BQ(sa), MR_BQ(sr), MR_BQ(sg), MR_BQ(sb));
        out += dstride;

        p = *trail;
        sa -= MR_A(p); sr -= MR_R(p); sg -= MR_G(p); sb -= MR_B(p);
        trail += sstride;
    }

    p = *lead;
    ra = MR_A(p); rr = MR_R(p); rg = MR_G(p); rb = MR_B(p);

    /* And off the right end. */
    for (; c < n; c++) {
        sa += ra; sr += rr; sg += rg; sb += rb;

        *out = MR_ARGB(MR_BQ(sa), MR_BQ(sr), MR_BQ(sg), MR_BQ(sb));
        out += dstride;

        p = *trail;
        sa -= MR_A(p); sr -= MR_R(p); sg -= MR_G(p); sb -= MR_B(p);
        trail += sstride;
    }
}

/* linblur32_core: one box blur of the whole surface along one axis. */
static void linblur(const mr_surface *src, mr_surface *dst,
                    int radius, int vertical)
{
    int i;
    int cols = vertical ? dst->h : dst->w;
    const unsigned char *q;

    /* A radius wide enough to swallow the line would walk off the end of it.
     * Ren'Py never gets one, because its radii come from blur_filters on a
     * full-size image; a 2px-wide sprite is not a reason to fault, so the
     * radius is clamped here instead. Clamped once per pass rather than per
     * line, because the quotient table below depends on it. */
    if (radius * 2 + 1 > cols)
        radius = (cols - 1) / 2;
    if (radius < 0)
        radius = 0;

    q = quot_table(radius * 2 + 1);

    if (vertical) {
        for (i = 0; i < dst->w; i++)
            blur_line(src->px + i, dst->px + i, dst->h,
                      src->pitch, dst->pitch, radius, q);
    } else {
        for (i = 0; i < dst->h; i++)
            blur_line(src->px + (size_t)i * src->pitch,
                      dst->px + (size_t)i * dst->pitch, dst->w,
                      1, 1, radius, q);
    }
}

int mr_surface_blur(mr_surface *s, float xrad, float yrad)
{
    mr_surface *wrk;
    mr_surface *from;
    int xl, xu, xm, yl, yu, ym;
    int i;
    const int n = 3;        /* blur32_core's pass count */

    if (!s || xrad < 0.0f || yrad < 0.0f)
        return 0;
    if (xrad == 0.0f && yrad == 0.0f)
        return 1;

    /* Ren'Py's caller hands in a scratch surface of the same size; we own one
     * for the duration instead. It is the one real cost of doing this at
     * decode time: 3.6MB for a full-screen background, held only while the
     * blur runs. */
    wrk = mr_surface_new(s->w, s->h);
    if (!wrk) {
        mr_log("blur: out of memory for a %dx%d scratch surface", s->w, s->h);
        return 0;
    }

    blur_filters(xrad, n, &xl, &xu, &xm);
    if (xrad != yrad)
        blur_filters(yrad, n, &yl, &yu, &ym);
    else {
        yl = xl; yu = xu; ym = xm;
    }

    /* blur32_core ping-pongs src -> wrk -> dst and then aliases src to dst,
     * so from the second pass on it is bouncing between two surfaces. With
     * the result landing back in `s` every pass that is exactly s <-> wrk. */
    from = s;
    for (i = 0; i < n; i++) {
        linblur(from, wrk, i < xm ? xl : xu, 0);
        linblur(wrk, s, i < ym ? yl : yu, 1);
        from = s;
    }

    mr_surface_free(wrk);

    /* has_alpha is left alone deliberately. A blur only ever averages alphas,
     * so a surface whose every pixel was 255 still has every pixel at 255 and
     * keeps the fast opaque blit path -- which matters, because the images a
     * game blurs are backgrounds. */
    return 1;
}

/* --- clipping -------------------------------------------------------------
 *
 * A viewport shows a window onto content taller than itself, so drawing has to
 * be confined to a rectangle. Rather than thread a clip rect through every
 * drawing call, one is kept here as a stack: the renderer pushes before
 * descending into a viewport and pops afterwards. Everything that writes
 * pixels goes through mr_clip_rect, so nothing can escape it by accident.
 */

#define MR_CLIP_DEPTH 8

static struct { int x, y, w, h; } g_clip[MR_CLIP_DEPTH];
static int g_nclip;

void mr_clip_push(int x, int y, int w, int h)
{
    if (g_nclip >= MR_CLIP_DEPTH) {
        mr_log("gfx: clip stack full");
        return;
    }

    /* Nested clips intersect; a viewport inside a viewport shows only what
     * both would show. */
    if (g_nclip > 0) {
        int px = g_clip[g_nclip - 1].x, py = g_clip[g_nclip - 1].y;
        int pr = px + g_clip[g_nclip - 1].w, pb = py + g_clip[g_nclip - 1].h;
        int r = x + w, b = y + h;
        if (x < px) x = px;
        if (y < py) y = py;
        if (r > pr) r = pr;
        if (b > pb) b = pb;
        w = r - x;
        h = b - y;
    }

    if (w < 0) w = 0;
    if (h < 0) h = 0;

    g_clip[g_nclip].x = x;
    g_clip[g_nclip].y = y;
    g_clip[g_nclip].w = w;
    g_clip[g_nclip].h = h;
    g_nclip++;
}

void mr_clip_pop(void)
{
    if (g_nclip > 0)
        g_nclip--;
}

void mr_clip_reset(void)
{
    g_nclip = 0;
}

/* The active clip rectangle, intersected with the surface. */
void mr_clip_rect(const mr_surface *s, int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = 0;
    *w = s->w;
    *h = s->h;

    if (g_nclip > 0) {
        int cx = g_clip[g_nclip - 1].x, cy = g_clip[g_nclip - 1].y;
        int cr = cx + g_clip[g_nclip - 1].w, cb = cy + g_clip[g_nclip - 1].h;
        if (cx > *x) { *w -= cx - *x; *x = cx; }
        if (cy > *y) { *h -= cy - *y; *y = cy; }
        if (cr < *x + *w) *w = cr - *x;
        if (cb < *y + *h) *h = cb - *y;
        if (*w < 0) *w = 0;
        if (*h < 0) *h = 0;
    }
}

/* Clip a blit rectangle against the destination and the clip rect. Returns 0
 * if nothing is left to draw. */
static int clip_blit(const mr_surface *dst, const mr_surface *src,
                     int *dx, int *dy, int *sx, int *sy, int *w, int *h)
{
    int cx, cy, cw, ch;

    mr_clip_rect(dst, &cx, &cy, &cw, &ch);

    *sx = 0;
    *sy = 0;
    *w = src->w;
    *h = src->h;

    if (*dx < cx) { *sx = cx - *dx; *w -= *sx; *dx = cx; }
    if (*dy < cy) { *sy = cy - *dy; *h -= *sy; *dy = cy; }
    if (*dx + *w > cx + cw) *w = cx + cw - *dx;
    if (*dy + *h > cy + ch) *h = cy + ch - *dy;

    return (*w > 0 && *h > 0);
}

void mr_blit(mr_surface *dst, const mr_surface *src, int dx, int dy)
{
    int sx, sy, w, h, y, x;
    const mr_u32 *sp;
    mr_u32 *dp;
    mr_u32 s, d;
    unsigned a, ia;
    unsigned int t0 = 0;

    if (!clip_blit(dst, src, &dx, &dy, &sx, &sy, &w, &h))
        return;

    if (g_clock)
        t0 = g_clock();

    for (y = 0; y < h; y++) {
        sp = src->px + (size_t)(sy + y) * src->pitch + sx;
        dp = dst->px + (size_t)(dy + y) * dst->pitch + dx;

        if (!src->has_alpha) {
            memcpy(dp, sp, (size_t)w * sizeof(mr_u32));
            continue;
        }

#ifdef MR_BLEND_MMX
        /* The same loop in MMX, four channels at a time instead of one. It is
         * a drop-in for the body below -- same early-outs, same arithmetic,
         * same stores -- which is what lets probe/mmxcheck.c compare the two
         * pixel for pixel on the target. blendmmx.h has the lane bounds and
         * blendtest proves them; nothing in the host suite executes the
         * kernel. Asked per row rather than per call so the branch is
         * amortised over a row's pixels and the scalar path below stays the
         * one true description of what a blend is. */
        if (mr_blend_simd()) {
            mr_blend_row_mmx(dp, sp, w);
            continue;
        }
#endif

        for (x = 0; x < w; x++) {
            s = sp[x];
            a = (unsigned)MR_A(s);
            if (a == 0)
                continue;
            if (a == 255) {
                dp[x] = s;
                continue;
            }
            ia = 255u - a;
            d = dp[x];
            /* Per channel: (s*a + d*(255-a)) / 255, rounded. MR_DIV255 is
             * that divide exactly -- see mr.h, and blendtest for the proof --
             * and the reason it is not written as a divide is that Watcom
             * emits three hardware DIVs here, which measured 270ns a pixel on
             * the target. This loop is the hottest one in the engine. */
            dp[x] = MR_ARGB(255,
                MR_DIV255((unsigned)MR_R(s) * a + (unsigned)MR_R(d) * ia),
                MR_DIV255((unsigned)MR_G(s) * a + (unsigned)MR_G(d) * ia),
                MR_DIV255((unsigned)MR_B(s) * a + (unsigned)MR_B(d) * ia));
        }
    }

    /* has_alpha is a property of the source surface and so is constant for
     * the whole call: the per-row test above picks the same branch every
     * time. One bucket per call is therefore exact, not an approximation --
     * and the two branches are the memcpy and the blend, which is the split
     * the profile exists to draw. */
    if (!src->has_alpha)
        PROF(copy, t0, (long)w * h);
    else
        PROF(blend, t0, (long)w * h);
}

void mr_blit_alpha(mr_surface *dst, const mr_surface *src, int dx, int dy,
                   int alpha)
{
    int sx, sy, w, h, y, x;
    const mr_u32 *sp;
    mr_u32 *dp;
    mr_u32 s, d;
    unsigned a, ia;
    unsigned int t0 = 0;

    if (alpha <= 0)
        return;
    /* Accounted by mr_blit, not here: charging it in both places would count
     * the same pixels twice and the same milliseconds twice. */
    if (alpha >= 255) {
        mr_blit(dst, src, dx, dy);
        return;
    }

    if (!clip_blit(dst, src, &dx, &dy, &sx, &sy, &w, &h))
        return;

    if (g_clock)
        t0 = g_clock();

    for (y = 0; y < h; y++) {
        sp = src->px + (size_t)(sy + y) * src->pitch + sx;
        dp = dst->px + (size_t)(dy + y) * dst->pitch + dx;

        for (x = 0; x < w; x++) {
            s = sp[x];
            /* MR_FLOOR255 and not MR_DIV255: this one truncates, and it has
             * to go on truncating. Rounding it instead would push a source
             * alpha of 1 at a transform alpha of 128 from 0 to 1, which is
             * the difference between a ghost sprite's clear border being
             * skipped by the `a == 0` test above and being blended. */
            a = MR_FLOOR255((unsigned)MR_A(s) * (unsigned)alpha);
            if (a == 0)
                continue;
            ia = 255u - a;
            d = dp[x];
            dp[x] = MR_ARGB(255,
                MR_DIV255((unsigned)MR_R(s) * a + (unsigned)MR_R(d) * ia),
                MR_DIV255((unsigned)MR_G(s) * a + (unsigned)MR_G(d) * ia),
                MR_DIV255((unsigned)MR_B(s) * a + (unsigned)MR_B(d) * ia));
        }
    }

    PROF(blend, t0, (long)w * h);
}

void mr_lerp_surface(mr_surface *dst, const mr_surface *a,
                     const mr_surface *b, int t)
{
    int y, x, w, h;
    const mr_u32 *ap, *bp;
    mr_u32 *dp;
    mr_u32 av, bv;
    unsigned it;
    unsigned int t0 = 0;

    w = dst->w;
    h = dst->h;
    if (a->w < w) w = a->w;
    if (b->w < w) w = b->w;
    if (a->h < h) h = a->h;
    if (b->h < h) h = b->h;

    if (t <= 0) { mr_surface_copy(dst, a); return; }
    if (t >= 255) { mr_surface_copy(dst, b); return; }
    it = 255u - (unsigned)t;

    if (g_clock)
        t0 = g_clock();

    for (y = 0; y < h; y++) {
        ap = a->px + (size_t)y * a->pitch;
        bp = b->px + (size_t)y * b->pitch;
        dp = dst->px + (size_t)y * dst->pitch;
        for (x = 0; x < w; x++) {
            av = ap[x];
            bv = bp[x];
            if (av == bv) {
                dp[x] = av;
                continue;
            }
            dp[x] = MR_ARGB(255,
                ((unsigned)MR_R(av) * it + (unsigned)MR_R(bv) * (unsigned)t) >> 8,
                ((unsigned)MR_G(av) * it + (unsigned)MR_G(bv) * (unsigned)t) >> 8,
                ((unsigned)MR_B(av) * it + (unsigned)MR_B(bv) * (unsigned)t) >> 8);
        }
    }

    PROF(lerp, t0, (long)w * h);
}

/* As mr_lerp_surface, but the alpha channel is interpolated too instead of
 * being forced opaque.
 *
 * The opaque version is right for what it was written for -- two whole-stage
 * snapshots, where everything behind has already been composited in. An
 * animation's frames are not that: they are RGBA art that still has to be
 * drawn *over* the scene, so throwing their alpha away turns a firework burst
 * into an opaque rectangle covering the sprites under it.
 *
 * Mixing has to happen on premultiplied values, which is what Ren'Py's
 * dissolve shader operates on. Mixing straight RGB independently of straight
 * alpha is not the same thing: where one frame is transparent its RGB is
 * whatever the encoder left there -- usually black -- and a straight mix
 * bleeds that into the result, giving dark fringes along every edge that is
 * opaque in one frame and clear in the other. Which, in an animation whose
 * frames are mostly-transparent bursts on a clear background, is every edge
 * that matters.
 *
 * Premultiplying needs a divide by the output alpha to get back to straight,
 * and a per-pixel divide is exactly what we do not want on the target. It is
 * avoided for the two cases that cover almost every pixel: equal alphas (both
 * opaque, or both clear), where the premultiplication divides straight back
 * out and a plain mix is bit-exact; and a zero result, which is fully clear.
 * Only the genuinely mixed-alpha edge pixels pay. */
void mr_lerp_surface_a(mr_surface *dst, const mr_surface *a,
                       const mr_surface *b, int t)
{
    int y, x, w, h;
    const mr_u32 *ap, *bp;
    mr_u32 *dp;
    mr_u32 av, bv;
    unsigned it;
    unsigned int t0 = 0;

    w = dst->w;
    h = dst->h;
    if (a->w < w) w = a->w;
    if (b->w < w) w = b->w;
    if (a->h < h) h = a->h;
    if (b->h < h) h = b->h;

    if (t <= 0) { mr_surface_copy(dst, a); return; }
    if (t >= 255) { mr_surface_copy(dst, b); return; }
    it = 255u - (unsigned)t;

    if (g_clock)
        t0 = g_clock();

    for (y = 0; y < h; y++) {
        ap = a->px + (size_t)y * a->pitch;
        bp = b->px + (size_t)y * b->pitch;
        dp = dst->px + (size_t)y * dst->pitch;
        for (x = 0; x < w; x++) {
            unsigned aa, ba, oa, den;

            av = ap[x];
            bv = bp[x];
            if (av == bv) {
                dp[x] = av;
                continue;
            }

            aa = (unsigned)MR_A(av);
            ba = (unsigned)MR_A(bv);

            if (aa == ba) {
                /* Equal alpha: the premultiply cancels, so mix as-is. */
                dp[x] = MR_ARGB(aa,
                    MR_DIV255((unsigned)MR_R(av) * it + (unsigned)MR_R(bv) * (unsigned)t),
                    MR_DIV255((unsigned)MR_G(av) * it + (unsigned)MR_G(bv) * (unsigned)t),
                    MR_DIV255((unsigned)MR_B(av) * it + (unsigned)MR_B(bv) * (unsigned)t));
                continue;
            }

            /* Mixing premultiplied and converting back to straight is
             *
             *     A' = (Aa*it + Ab*t) / 255
             *     R' = (Ra*Aa*it + Rb*Ab*t) / (Aa*it + Ab*t)
             *
             * so the same denominator serves both and the 255s cancel out of
             * the colour. Writing it as premultiplied-then-divide-by-A'
             * instead loses a factor of 255 and saturates every mixed-alpha
             * pixel -- which is a quiet failure, because those pixels are a
             * thin edge and the result still looks like a cross-fade.
             * Largest numerator is 255*255*255*2, inside 32 bits.
             *
             * Rounded, not truncated, and the same for the equal-alpha
             * branch above. Truncation here is not a rounding detail: it
             * biases every blended pixel one way, and over a fade that reads
             * as the arriving frame being slightly too transparent the whole
             * way in. It is also the convention the rest of the file already
             * uses -- mr_blit_alpha rounds -- and what matching Ren'Py means,
             * since its blend is in floating point and quantises on write. */
            den = aa * it + ba * (unsigned)t;
            if (den == 0) {
                dp[x] = MR_ARGB(0, 0, 0, 0);
                continue;
            }
            oa = MR_DIV255(den);      /* den <= 255*255, see mr.h */

            dp[x] = MR_ARGB(oa,
                ((unsigned)MR_R(av) * aa * it +
                 (unsigned)MR_R(bv) * ba * (unsigned)t + den / 2u) / den,
                ((unsigned)MR_G(av) * aa * it +
                 (unsigned)MR_G(bv) * ba * (unsigned)t + den / 2u) / den,
                ((unsigned)MR_B(av) * aa * it +
                 (unsigned)MR_B(bv) * ba * (unsigned)t + den / 2u) / den);
        }
    }

    PROF(lerp, t0, (long)w * h);
}

void mr_fade_to(mr_surface *s, mr_u32 color, int t)
{
    int y, x;
    mr_u32 *p;
    mr_u32 v;
    unsigned it, cr, cg, cb;
    unsigned int t0 = 0;

    if (t <= 0)
        return;
    /* Accounted by mr_surface_fill, which is the work actually done. */
    if (t >= 255) {
        mr_surface_fill(s, color | 0xFF000000UL);
        return;
    }

    it = 255u - (unsigned)t;
    cr = (unsigned)MR_R(color) * (unsigned)t;
    cg = (unsigned)MR_G(color) * (unsigned)t;
    cb = (unsigned)MR_B(color) * (unsigned)t;

    if (g_clock)
        t0 = g_clock();

    for (y = 0; y < s->h; y++) {
        p = s->px + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) {
            v = p[x];
            p[x] = MR_ARGB(255,
                ((unsigned)MR_R(v) * it + cr) >> 8,
                ((unsigned)MR_G(v) * it + cg) >> 8,
                ((unsigned)MR_B(v) * it + cb) >> 8);
        }
    }

    PROF(lerp, t0, (long)s->w * s->h);
}

/* The destination-to-source column map for mr_blit_stretch, built once per
 * call instead of once per pixel.
 *
 * `sx + (x - dx) * sw / dw` was an `idiv` for every destination pixel -- a
 * *variable* divisor, so unlike the /255 case there was nothing a compiler
 * could have reduced even in principle, and `idiv r/m32` is 36-40 cycles on a
 * Pentium III and does not pipeline. The `stretch` bucket measured 156 ns/px
 * over 2,025kpx a batch, 10.6ms a frame on a scene with zoomed sprites
 * (log-alphafix.txt).
 *
 * The same shape as scale.c's build_point, and deliberately *not* the same
 * arithmetic: build_point samples the centre of each destination pixel,
 * `(i*2+1)*src/(dst*2)`, and this samples the top-left corner. Swapping to
 * centres would move every stretched sprite half a pixel, which is a visible
 * change wearing an optimisation's clothes. The expression below is the one
 * that was in the loop, transcribed.
 *
 * Static, not on the stack: this file's working buffers are what caused the
 * stack growth and the page fault once already (NOTES.md on stack), and 1280
 * ints is 5KB. Sized to the stage width, which bounds x1 for every caller in
 * the engine -- the clip above has already limited x to dst->w, and the only
 * surfaces wider than the stage are the zoom scratch buffers, which go
 * through mr_surface_zoom and not through here. A destination wider than the
 * table falls back to the divide rather than being refused: slow is a
 * performance bug, a truncated map is a visual one.
 */
static int g_sxmap[MR_STAGE_W];

/* Indexed by the destination x itself, not by x - x0: it saves a subtraction
 * per pixel, and `g_sxmap - x0` -- the other way to get there -- forms a
 * pointer before the start of the array, which is undefined however reliably
 * it works. */
static void build_stretch_map(int *map, int x0, int x1,
                              int sx, int sw, int dx, int dw)
{
    int x;

    for (x = x0; x < x1; x++)
        map[x] = sx + (x - dx) * sw / dw;
}

void mr_blit_stretch(mr_surface *dst, const mr_surface *src,
                     int sx, int sy, int sw, int sh,
                     int dx, int dy, int dw, int dh)
{
    int cx, cy, cw, ch;
    int x0, y0, x1, y1, x, y;
    const int *xmap = NULL;
    unsigned int t0 = 0;

    if (!dst || !src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    /* Clip in destination space, then map each surviving destination pixel
     * back into the source: clipping the source instead would need the ratio
     * applied twice and rounds differently at the edges. */
    mr_clip_rect(dst, &cx, &cy, &cw, &ch);

    x0 = dx > cx ? dx : cx;
    y0 = dy > cy ? dy : cy;
    x1 = (dx + dw < cx + cw) ? dx + dw : cx + cw;
    y1 = (dy + dh < cy + ch) ? dy + dh : cy + ch;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;

    /* Nothing to draw: build no map, and leave the row loop to do nothing
     * too. Also what keeps `x1 - x0` a sane table length below. */
    if (x0 >= x1 || y0 >= y1)
        return;

    if (x1 <= MR_STAGE_W) {
        build_stretch_map(g_sxmap, x0, x1, sx, sw, dx, dw);
        xmap = g_sxmap;
    }

    if (g_clock)
        t0 = g_clock();

    for (y = y0; y < y1; y++) {
        int   syy = sy + (y - dy) * sh / dh;
        const mr_u32 *sp;
        mr_u32 *dp;

        if (syy < 0) syy = 0;
        if (syy >= src->h) syy = src->h - 1;

        sp = src->px + (size_t)syy * src->pitch;
        dp = dst->px + (size_t)y * dst->pitch;

        for (x = x0; x < x1; x++) {
            /* The map, or the divide it replaced for a destination wider
             * than the table. Nearest, not bilinear -- see the header.
             *
             * The test costs a predicted branch, one or two cycles, in place
             * of 36 to 40: it is loop-invariant in fact and goes the same way
             * every iteration. Not worth splitting the loop in two to remove,
             * which would mean a second copy of the blend below. */
            int sxx = xmap ? xmap[x] : sx + (x - dx) * sw / dw;
            mr_u32 s, d;
            unsigned a, ia;

            if (sxx < 0) sxx = 0;
            if (sxx >= src->w) sxx = src->w - 1;

            s = sp[sxx];
            if (!src->has_alpha) {
                dp[x] = s;
                continue;
            }
            a = (unsigned)MR_A(s);
            if (a == 0)
                continue;
            if (a == 255) {
                dp[x] = s;
                continue;
            }
            ia = 255u - a;
            d = dp[x];
            dp[x] = MR_ARGB(255,
                MR_DIV255((unsigned)MR_R(s) * a + (unsigned)MR_R(d) * ia),
                MR_DIV255((unsigned)MR_G(s) * a + (unsigned)MR_G(d) * ia),
                MR_DIV255((unsigned)MR_B(s) * a + (unsigned)MR_B(d) * ia));
        }
    }

    PROF(stretch, t0, (long)(x1 - x0) * (y1 - y0));
}

/* --- bilinear resample, for Transform xzoom/yzoom ------------------------ */

/* One axis of the sampling grid: for each destination pixel, the source index
 * to its left and a 0..255 fraction towards the next one.
 *
 * Sampled at the centre of each destination pixel, less the half pixel that
 * turns "centre of a pixel" into "distance from pixel 0's centre" -- the grid
 * GL_LINEAR uses on a scaled texture, which is what Ren'Py's zoom is.
 * accelerator.pyx:555-585 only folds the zoom into the reverse matrix; the
 * filtering is the texture's, and gl2texture.pyx:298-301 sets both the
 * magnify and the minify filter to GL_LINEAR. So: bilinear, both directions,
 * and no mipmap unless the game asks for one (config.mipmap, off by default).
 *
 * Deliberately a copy of scale.c's build_linear rather than a call to it.
 * That one is the stage-to-window scaler: it is not in most harnesses' link
 * lines, and it writes bottom-up for GDI's benefit. Sharing would drag both
 * properties in here.
 *
 * Largest intermediate is (2 * dst - 1) * src * 128, which for the largest
 * surface a zoom can produce is well inside a 32-bit long. Below zero it
 * clamps, so the first destination pixel is the first source pixel rather
 * than a blend with one that is not there. */
static void zoom_axis(int *map, int *frac, int src, int dst)
{
    int i;

    for (i = 0; i < dst; i++) {
        long f = ((long)(i * 2 + 1) * (long)src * 128L) / (long)dst - 128L;

        if (f < 0)
            f = 0;
        map[i] = (int)(f >> 8);
        frac[i] = (int)(f & 255L);
        /* The last destination pixel can land on the last source pixel with a
         * fraction left over; there is nothing past it to blend with. */
        if (map[i] >= src - 1) {
            map[i] = src - 1;
            frac[i] = 0;
        }
    }
}

mr_surface *mr_surface_zoom(const mr_surface *src, int dst_w, int dst_h,
                            int flipx, int flipy)
{
    mr_surface *out;
    int *tab, *xmap, *xfrac, *ymap, *yfrac;
    int y;
    unsigned int t0 = 0;

    if (!src || src->w <= 0 || src->h <= 0 || dst_w <= 0 || dst_h <= 0)
        return NULL;

    tab = (int *)malloc(((size_t)dst_w + (size_t)dst_h) * 2 * sizeof(int));
    if (!tab)
        return NULL;
    xmap = tab;
    xfrac = xmap + dst_w;
    ymap = xfrac + dst_w;
    yfrac = ymap + dst_h;

    out = mr_surface_new(dst_w, dst_h);
    if (!out) {
        free(tab);
        return NULL;
    }
    out->has_alpha = src->has_alpha;

    zoom_axis(xmap, xfrac, src->w, dst_w);
    zoom_axis(ymap, yfrac, src->h, dst_h);

    if (g_clock)
        t0 = g_clock();

    for (y = 0; y < dst_h; y++) {
        const mr_u32 *r0 = src->px + (size_t)ymap[y] * src->pitch;
        const mr_u32 *r1 = yfrac[y] ? r0 + src->pitch : r0;
        unsigned wy = (unsigned)yfrac[y];
        unsigned iwy = 256u - wy;
        mr_u32 *dp;
        int x;

        /* Negative zoom is Ren'Py's flip -- the deprecation note on im.Flip
         * says to use a negative xzoom or yzoom instead, and
         * accelerator.pyx:563-571 takes the absolute value for the size and
         * leaves the sign in the matrix. The resample is identical; only
         * which row and column the answer lands in changes, so this costs a
         * subtraction rather than a second pass. */
        dp = out->px + (size_t)(flipy ? dst_h - 1 - y : y) * out->pitch;

        for (x = 0; x < dst_w; x++) {
            unsigned wx = (unsigned)xfrac[x];
            unsigned iwx = 256u - wx;
            unsigned w00 = iwx * iwy, w10 = wx * iwy;
            unsigned w01 = iwx * wy,  w11 = wx * wy;
            int ix = xmap[x];
            mr_u32 p00 = r0[ix], p10 = wx ? r0[ix + 1] : p00;
            mr_u32 p01 = r1[ix], p11 = wx ? r1[ix + 1] : p01;
            unsigned a00, a10, a01, a11;
            int ox = flipx ? dst_w - 1 - x : x;

            /* The four weights sum to exactly 65536, so every blend below is
             * a shift and not a divide. */
            if (p00 == p10 && p00 == p01 && p00 == p11) {
                dp[ox] = p00;           /* flat: most of a sprite, and all of
                                         * its transparent surround */
                continue;
            }

            a00 = (unsigned)MR_A(p00);
            a10 = (unsigned)MR_A(p10);
            a01 = (unsigned)MR_A(p01);
            a11 = (unsigned)MR_A(p11);

            if (a00 == a10 && a00 == a01 && a00 == a11) {
                /* Equal alpha: premultiplying would divide straight back out,
                 * so mix the channels as they stand. */
                dp[ox] = MR_ARGB(a00,
                    ((unsigned)MR_R(p00) * w00 + (unsigned)MR_R(p10) * w10 +
                     (unsigned)MR_R(p01) * w01 + (unsigned)MR_R(p11) * w11) >> 16,
                    ((unsigned)MR_G(p00) * w00 + (unsigned)MR_G(p10) * w10 +
                     (unsigned)MR_G(p01) * w01 + (unsigned)MR_G(p11) * w11) >> 16,
                    ((unsigned)MR_B(p00) * w00 + (unsigned)MR_B(p10) * w10 +
                     (unsigned)MR_B(p01) * w01 + (unsigned)MR_B(p11) * w11) >> 16);
                continue;
            }

            /* Mixed alpha: the one-pixel fringe around a sprite's outline.
             * Interpolating straight colour there pulls in whatever the
             * transparent pixels happen to hold -- usually black -- and leaves
             * a dark halo, so each colour is weighted by its own alpha and the
             * total divided back out. Same reasoning, and same arithmetic, as
             * mr_lerp_surface_a.
             *
             * Largest numerator is 255 * 255 * 65536 plus the rounding term,
             * i.e. 4,236,640,256: inside an unsigned 32-bit by about 58
             * million. The bound is fixed -- channel maxima and the weight
             * scale, neither of them a tunable -- so it cannot creep up. */
            {
                unsigned den = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;

                if (den == 0) {
                    dp[ox] = MR_ARGB(0, 0, 0, 0);
                    continue;
                }
                dp[ox] = MR_ARGB((den + 32768u) >> 16,
                    ((unsigned)MR_R(p00) * a00 * w00 +
                     (unsigned)MR_R(p10) * a10 * w10 +
                     (unsigned)MR_R(p01) * a01 * w01 +
                     (unsigned)MR_R(p11) * a11 * w11 + den / 2u) / den,
                    ((unsigned)MR_G(p00) * a00 * w00 +
                     (unsigned)MR_G(p10) * a10 * w10 +
                     (unsigned)MR_G(p01) * a01 * w01 +
                     (unsigned)MR_G(p11) * a11 * w11 + den / 2u) / den,
                    ((unsigned)MR_B(p00) * a00 * w00 +
                     (unsigned)MR_B(p10) * a10 * w10 +
                     (unsigned)MR_B(p01) * a01 * w01 +
                     (unsigned)MR_B(p11) * a11 * w11 + den / 2u) / den);
            }
        }
    }

    PROF(stretch, t0, (long)dst_w * dst_h);

    free(tab);
    return out;
}

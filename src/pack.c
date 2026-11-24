/* pack.c - 32bpp ARGB to the display's pixel format. See pack.h.
 *
 * Structure: one specialised loop per layout that is actually common, plus a
 * generic one driven by the field positions for everything else. The generic
 * loop is correct for all of them -- the specialised ones exist only because
 * this runs on every pixel of every frame on a Pentium III, where the pack
 * pass is the second most expensive thing in a frame after the scale. The
 * harness checks each specialisation against the generic loop, so a card with
 * an ordinary 5:6:5 and one with a strange layout cannot disagree.
 */

#include <string.h>

#include "pack.h"

/* --- the 8bpp palette ------------------------------------------------------
 *
 * 6x6x6 cube plus a grey ramp. These were in present.c, where only the GDI
 * presenter could reach them.
 */
#define CUBE_LEVELS 6
#define CUBE_COUNT  (CUBE_LEVELS * CUBE_LEVELS * CUBE_LEVELS)
#define GREY_COUNT  (256 - CUBE_COUNT)

/* 4x4 ordered dither, scaled at use to the gap between adjacent cube levels
 * so the error it spreads is exactly one quantisation step. */
static const int g_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

void mr_pack_palette(mr_u8 *rgb)
{
    int r, g, b, i;

    i = 0;
    for (r = 0; r < CUBE_LEVELS; r++) {
        for (g = 0; g < CUBE_LEVELS; g++) {
            for (b = 0; b < CUBE_LEVELS; b++) {
                rgb[i * 3 + 0] = (mr_u8)(r * 255 / (CUBE_LEVELS - 1));
                rgb[i * 3 + 1] = (mr_u8)(g * 255 / (CUBE_LEVELS - 1));
                rgb[i * 3 + 2] = (mr_u8)(b * 255 / (CUBE_LEVELS - 1));
                i++;
            }
        }
    }
    for (r = 0; r < GREY_COUNT; r++, i++) {
        mr_u8 v = (mr_u8)(r * 255 / (GREY_COUNT - 1));
        rgb[i * 3 + 0] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v;
    }
}

/* The dither period, in both directions. Not a parameter: the Bayer matrix
 * above is 4x4 and this exists to agree with it. */
#define DITHER 4

int mr_pack_snap_rect(int *x, int *y, int *w, int *h, int clip_w, int clip_h)
{
    int x0, y0, x1, y1;

    x0 = *x;
    y0 = *y;
    x1 = x0 + *w;
    y1 = y0 + *h;

    /* Out to the grid. The low corner rounds down and the high corner up, so
     * the rectangle only ever grows. */
    x0 -= ((x0 % DITHER) + DITHER) % DITHER;
    y0 -= ((y0 % DITHER) + DITHER) % DITHER;
    x1 += (DITHER - ((x1 % DITHER) + DITHER) % DITHER) % DITHER;
    y1 += (DITHER - ((y1 % DITHER) + DITHER) % DITHER) % DITHER;

    /* Then in to the surface. Clipping can take the corners back off the
     * grid, and that is correct: a rectangle clipped at the left edge starts
     * at column 0, which is on the grid anyway, and one clipped at the right
     * edge keeps its aligned start, which is the end that matters. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > clip_w) x1 = clip_w;
    if (y1 > clip_h) y1 = clip_h;

    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;

    return *w > 0 && *h > 0;
}

int mr_pack_fmt_std(mr_pack_fmt *f, int bpp)
{
    memset(f, 0, sizeof(*f));
    f->bpp = bpp;

    switch (bpp) {
    case 8:
        return 1;
    case 16:
        f->rpos = 11; f->rsize = 5;
        f->gpos =  5; f->gsize = 6;
        f->bpos =  0; f->bsize = 5;
        return 1;
    case 24:
    case 32:
        f->rpos = 16; f->rsize = 8;
        f->gpos =  8; f->gsize = 8;
        f->bpos =  0; f->bsize = 8;
        return 1;
    default:
        f->bpp = 0;
        return 0;
    }
}

/* --- the specialised loops ------------------------------------------------- */

/* --- quantiser tables -----------------------------------------------------
 *
 * `v * (LEVELS - 1) / 255` for every byte value, three times in the cube
 * branch of pack8 and once in the grey branch.
 *
 * Tables because Open Watcom emits a real `idiv` for a division by a
 * constant. gcc strength-reduces it to a multiply-and-shift, so the host
 * benchmark said this was worth 9% and effectively nothing -- but `wdis` on
 * the actual object showed four `idiv` live in the inner loop, and on a
 * Pentium III a 32-bit `idiv` is around forty cycles and does not pipeline.
 * That is roughly 120 cycles a pixel spent on three divisions, which at
 * 640x360 is tens of milliseconds a frame.
 *
 * This is the performance half of CLAUDE.md's "the gcc harness build hides
 * real Watcom bugs": the host cannot see this cost at all, and the only
 * reason it was found is that the generated code was read.
 *
 * Exact, not approximate: the index covers the dividend's whole range, so
 * the table *is* the division rather than a model of it, and packtest proves
 * them equal over all 256 inputs. Nothing here changes a single output byte,
 * which is what the rest of packtest's claims continue to assert.
 */
static mr_u8 g_cube_q[256];
static mr_u8 g_grey_q[256];
static int   g_quant_ready;

static void quant_init(void)
{
    int v;

    if (g_quant_ready)
        return;

    for (v = 0; v < 256; v++) {
        g_cube_q[v] = (mr_u8)(v * (CUBE_LEVELS - 1) / 255);
        g_grey_q[v] = (mr_u8)(v * (GREY_COUNT - 1) / 255);
    }
    g_quant_ready = 1;
}

/* For packtest, which checks the tables against the division they replace
 * over the whole input range. Not in pack.h: nothing in the engine has any
 * business reading these, and the harness includes pack.c's own declaration
 * rather than the header for exactly the things that should not be API. */
const mr_u8 *mr_pack_quant_tables(int grey)
{
    quant_init();
    return grey ? g_grey_q : g_cube_q;
}

/* 0xAARRGGBB to the 6x6x6 cube with 4x4 ordered dithering. Near-neutral
 * pixels go to the grey ramp instead, which the cube represents only coarsely
 * and which both games lean on heavily in their night scenes. */
static void pack8(const mr_surface *src, mr_u8 *dst, long pitch,
                  int y0, int y1)
{
    int y, x, w;
    const mr_u32 *s;
    mr_u8 *d;
    mr_u32 v;
    int r, g, b, dth, mx, mn;

    /* Spread of one quantisation step, in 1/16ths. */
    const int step = 255 / (CUBE_LEVELS - 1);

    quant_init();

    w = src->w;

    for (y = y0; y < y1; y++) {
        s = src->px + (size_t)y * src->pitch;
        d = dst + (long)y * pitch;

        for (x = 0; x < w; x++) {
            v = s[x];
            r = MR_R(v);
            g = MR_G(v);
            b = MR_B(v);

            mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            mn = r < g ? (r < b ? r : b) : (g < b ? g : b);

            dth = g_bayer[y & 3][x & 3];

            if (mx - mn < 16) {
                /* Neutral: the grey ramp is far finer than the cube here. */
                const int gstep = 255 / (GREY_COUNT - 1);
                int lum = (r * 77 + g * 151 + b * 28) >> 8;
                int gi;

                lum += (dth * gstep / 16) - gstep / 2;
                if (lum < 0) lum = 0; else if (lum > 255) lum = 255;

                gi = g_grey_q[lum];
                *d++ = (mr_u8)(CUBE_COUNT + gi);
                continue;
            }

            /* Add the dither offset before quantising, then clamp. */
            r += (dth * step / 16) - step / 2;
            g += (dth * step / 16) - step / 2;
            b += (dth * step / 16) - step / 2;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            r = g_cube_q[r];
            g = g_cube_q[g];
            b = g_cube_q[b];

            *d++ = (mr_u8)((r * CUBE_LEVELS + g) * CUBE_LEVELS + b);
        }
    }
}

/* 24bpp B,G,R per pixel: a BI_RGB DIB and the usual linear framebuffer. */
static void pack24(const mr_surface *src, mr_u8 *dst, long pitch,
                   int y0, int y1)
{
    int y, x, w;
    const mr_u32 *s;
    mr_u8 *d;
    mr_u32 v;

    w = src->w;

    for (y = y0; y < y1; y++) {
        s = src->px + (size_t)y * src->pitch;
        d = dst + (long)y * pitch;
        for (x = 0; x < w; x++) {
            v = s[x];
            *d++ = MR_B(v);
            *d++ = MR_G(v);
            *d++ = MR_R(v);
        }
    }
}

/* 32bpp x:8:8:8 in the surface's own order, so the row is a straight copy.
 * The alpha byte is written as whatever the surface held: on a framebuffer
 * the fourth byte is ignored, and on a DIB BI_RGB says the same. */
static void pack32(const mr_surface *src, mr_u8 *dst, long pitch,
                   int y0, int y1)
{
    int y;

    for (y = y0; y < y1; y++)
        memcpy(dst + (long)y * pitch,
               src->px + (size_t)y * src->pitch,
               (size_t)src->w * 4);
}

/* 16bpp with both fields contiguous from bit 0 -- 5:6:5 and 5:5:5, which is
 * every 16bpp mode anyone ships. The shifts are taken from the format rather
 * than hardcoded, so this one loop serves both. */
/* --- dithering the truncation away ----------------------------------------
 *
 * Dropping 8 bits to 5 by `v >> 3` throws away up to 7 counts, always
 * downwards, and does it the same way on every pixel -- so the error is
 * correlated across whole regions, which is what the eye sees as banding. On a
 * 5:5:5 DIB that is one step of 8 counts per channel, and it is the thing that
 * makes the dib16 path look worse than the 24bpp one even though both end up
 * on the same 5:6:5 display.
 *
 * Adding an ordered-dither offset spanning the whole step before the shift
 * fixes both halves at once: the error stops being correlated, and because the
 * offsets average half a step it *also* rounds instead of truncating. One
 * mechanism, not dither layered on top of rounding.
 *
 *     off = bayer >> (4 - sh)            0..7 for 5 bits, 0..3 for 6
 *     q   = (v + off) >> sh              clamped to the field's maximum
 *
 * No low clamp is needed because the offset is never negative, which is the
 * one place this differs from pack8 above -- that one centres its offset on
 * zero and so has to clamp both ends.
 *
 * Written arithmetically this cost +165% of the pack pass where the blit
 * saving allowed +84%, and was slower than the 24bpp path it exists to beat.
 * It is a table now; see the comment above g_q5 for why, and NOTES.md for the
 * figures.
 *
 * Measured on three of Miwa's backgrounds, downscaled to the window first
 * because that is the order the engine does it in. The figure that matters is
 * the error after a 1.2px blur, which is what the eye does when it integrates
 * neighbouring pixels, and so is the banding rather than the per-pixel noise:
 *
 *     555 truncated  (what this was)     3.91
 *     555 rounded                        1.73
 *     555 dithered   (this)              0.69
 *     565 truncated  (the 24bpp path)    3.35
 *     565 rounded    (a generous model)  1.50
 *
 * So the dithered 16bpp path is better than the 24bpp path it was losing to,
 * by a factor of two against the most charitable reading of what the driver
 * does. Per-pixel error goes up, from 2.30 to 3.20 -- that is the trade, and
 * anyone measuring raw error rather than blurred error will see this get
 * "worse". Mean signed error stays within 0.07 of a count per channel, so it
 * does not lighten or darken the picture.
 *
 * A 4x4 matrix is not a compromise here, it is sufficient: at 5 bits the step
 * is 8 counts, so `v + off` has only 8 distinguishable fractions and 16
 * entries already over-resolve them. An 8x8 Bayer was measured and is
 * byte-identical.
 *
 * Divergence from Ren'Py, deliberately: it composites and presents through
 * OpenGL at 8 bits a channel and never quantises like this, so there is no
 * behaviour to match. See NOTES.md.
 */
static int g_dither = 1;

void mr_pack_dither(int on)
{
    g_dither = on;
}

/* The matrix's 0..15 mapped onto the 0..(1<<sh)-1 span of one quantisation
 * step. Written as a left shift then a fixed >>4 rather than the obvious
 * `bay >> (4 - sh)`, because that form shifts by a negative amount -- which is
 * undefined, not merely wrong -- for any field narrower than four bits.
 * mask_to_field will hand this whatever an unusual card reports, and "no
 * display has one" is not a reason to write undefined behaviour.
 *
 * sh == 0 gives an offset of 0, which is what a channel that is not being
 * quantised at all should get. */
#define DOFF(bay, sh)  (((bay) << (sh)) >> 4)

/* One quantised channel. Written once and used by both the 16bpp loop and the
 * generic one, because packtest asserts the two agree and a second copy of
 * this arithmetic is how they would stop agreeing.
 *
 * The outer parentheses are not decoration. Without them the expansion inside
 * `((mr_u32)DITH(...) << rp)` binds the cast to the ternary's *condition* and
 * the shift to its *else branch* only, which packtest caught immediately as
 * the specialised loop disagreeing with the field-driven one.
 *
 * `v + off` is evaluated twice. Every argument passed here is a simple
 * expression with no side effects, and the compiler folds it; spelling it out
 * through a temporary would need a statement expression, which is not C89. */
#define DITH(v, sh, off, maxq)                                               \
    (((((int)(v) + (off)) >> (sh)) > (maxq))                                 \
        ? (maxq)                                                             \
        : ((((int)(v) + (off)) >> (sh))))

/* --- the dither as a table ------------------------------------------------
 *
 * Written arithmetically first, and that was measured on the Pentium III at
 * `pack 4.53ms -> 12.00ms`, which is +165% where the blit saving allowed +84%.
 * The reason was not the arithmetic but the *registers*: with rsh/gsh/bsh,
 * rp/gp/bp and a clamp bound for each channel, the inner loop had some
 * seventeen live values against x86's seven, and `wdis` showed 113 of 210
 * instructions in pack16 were `mov`. It had stopped computing and started
 * shuffling the stack.
 *
 * So the quantisation moves into a table indexed by the dither offset and the
 * channel value, which removes the add, the shift, the clamp and the clamp's
 * bound all at once -- and with them the spills. `qt_row` hands the inner loop
 * a pointer and the loop does a load.
 *
 * Only widths 5 and 6 are tabulated, because those are the only ones any 15 or
 * 16bpp display uses, and a table for every width `mask_to_field` can describe
 * would be 20KB of mostly-unused memory. Anything else falls through to the
 * arithmetic path below, which is correct, slower, and reached by no hardware
 * we have ever seen. Both compute `min((v + off) >> sh, (1 << width) - 1)`, so
 * the two agree -- packtest asserts it.
 *
 * 3KB of tables, in BSS rather than malloc'd: they are the same size on every
 * run and `quant_init` sets the precedent for the palette path above.
 */
static mr_u8 g_q5[8][256];      /* width 5, sh 3, offsets 0..7 */
static mr_u8 g_q6[4][256];      /* width 6, sh 2, offsets 0..3 */
static int   g_qt_ready;

static void qt_init(void)
{
    int off, v, q;

    if (g_qt_ready)
        return;

    for (off = 0; off < 8; off++)
        for (v = 0; v < 256; v++) {
            q = (v + off) >> 3;
            g_q5[off][v] = (mr_u8)(q > 31 ? 31 : q);
        }
    for (off = 0; off < 4; off++)
        for (v = 0; v < 256; v++) {
            q = (v + off) >> 2;
            g_q6[off][v] = (mr_u8)(q > 63 ? 63 : q);
        }

    g_qt_ready = 1;
}

/* The 256 quantised values for one dither offset, or NULL for a field width
 * this does not tabulate. `sh` is 8 - width.
 *
 * Builds the tables itself rather than leaving that to the caller: the first
 * use is inside a declaration's initialiser, which runs before any statement
 * in its block could have called an init, and a pointer into a static array is
 * non-NULL whether or not the contents have been written. So an ordering
 * mistake here would not crash, it would pack one frame from a table of
 * zeroes -- a black screen on the first frame and correct forever after, which
 * is the kind of bug that gets reported as "it flickers at startup". */
static const mr_u8 *qt_row(int sh, int bay)
{
    qt_init();

    switch (sh) {
    case 3: return g_q5[DOFF(bay, 3)];
    case 2: return g_q6[DOFF(bay, 2)];
    default: return NULL;
    }
}

/* The two layouts that actually exist, with their field positions as
 * constants.
 *
 * Splitting these out is the same argument the header makes for having
 * specialised loops at all, and the generated code is why it is needed here:
 * with `rp`/`gp`/`bp` as runtime values every channel compiled to `mov cl,
 * [esp+..]` followed by a variable `shl`, and each intermediate was spilled
 * and reloaded around it. Constants make the shifts immediates and leave
 * nothing to hold.
 *
 * 5:5:5 gets the better deal: all three fields are the same width, so all
 * three share one table row and the inner loop needs a single pointer per
 * pixel. Blue sits at bit 0 and needs no shift at all.
 */
static void pack555_d(const mr_surface *src, mr_u8 *dst, long pitch,
                      int y0, int y1)
{
    int y, x, w = src->w;

    for (y = y0; y < y1; y++) {
        /* An array and not four named pointers with a switch: the switch
         * compiled to a four-way compare-and-branch chain executed on every
         * pixel, where indexing is one load. */
        const mr_u8 *q[4];
        const mr_u32 *s = src->px + (size_t)y * src->pitch;
        mr_u16 *d = (mr_u16 *)(dst + (long)y * pitch);
        int i;

        for (i = 0; i < 4; i++)
            q[i] = qt_row(3, g_bayer[y & 3][i]);

        for (x = 0; x < w; x++) {
            const mr_u8 *t = q[x & 3];
            mr_u32 v = s[x];
            /* Full-width locals, deliberately. Folded into the expression,
             * Watcom emitted `movzx bx, byte ptr [..]` and then read `ebx` --
             * a partial register write followed by a full read, which is a
             * five-or-six cycle stall on a P6 core, three times a pixel. That
             * is most of what the dither was costing. */
            unsigned r = t[MR_R(v)];
            unsigned g = t[MR_G(v)];
            unsigned b = t[MR_B(v)];

            d[x] = (mr_u16)((r << 10) | (g << 5) | b);
        }
    }
}

static void pack565_d(const mr_surface *src, mr_u8 *dst, long pitch,
                      int y0, int y1)
{
    int y, x, w = src->w;

    for (y = y0; y < y1; y++) {
        const mr_u8 *q[4];      /* 5-bit, for red and blue */
        const mr_u8 *g[4];      /* 6-bit, for green */
        const mr_u32 *s = src->px + (size_t)y * src->pitch;
        mr_u16 *d = (mr_u16 *)(dst + (long)y * pitch);
        int i;

        for (i = 0; i < 4; i++) {
            q[i] = qt_row(3, g_bayer[y & 3][i]);
            g[i] = qt_row(2, g_bayer[y & 3][i]);
        }

        for (x = 0; x < w; x++) {
            const mr_u8 *t = q[x & 3];
            const mr_u8 *u = g[x & 3];
            mr_u32 v = s[x];
            unsigned r = t[MR_R(v)];    /* full width, see pack555_d */
            unsigned gg = u[MR_G(v)];
            unsigned b = t[MR_B(v)];

            d[x] = (mr_u16)((r << 11) | (gg << 5) | b);
        }
    }
}

static void pack16(const mr_pack_fmt *f, const mr_surface *src,
                   mr_u8 *dst, long pitch, int y0, int y1)
{
    int y, x, w;
    const mr_u32 *s;
    mr_u16 *d;
    mr_u32 v;
    const int rsh = 8 - f->rsize, gsh = 8 - f->gsize, bsh = 8 - f->bsize;
    const int rp = f->rpos, gp = f->gpos, bp = f->bpos;
    const int rmx = (1 << f->rsize) - 1;
    const int gmx = (1 << f->gsize) - 1;
    const int bmx = (1 << f->bsize) - 1;

    /* Whether the tables cover all three widths. Decided once, outside both
     * loops, because it is a property of the format and not of a pixel. */
    const int lut = g_dither &&
                    qt_row(rsh, 0) && qt_row(gsh, 0) && qt_row(bsh, 0);

    w = src->w;

    /* The two real layouts, with their positions as constants. Everything
     * below still handles them correctly; this is only about the code the
     * compiler gets to emit. */
    if (g_dither && rsh == 3 && gsh == 3 && bsh == 3 &&
        rp == 10 && gp == 5 && bp == 0) {
        pack555_d(src, dst, pitch, y0, y1);
        return;
    }
    if (g_dither && rsh == 3 && gsh == 2 && bsh == 3 &&
        rp == 11 && gp == 5 && bp == 0) {
        pack565_d(src, dst, pitch, y0, y1);
        return;
    }

    for (y = y0; y < y1; y++) {
        /* Fixed for the row, so the row lookup is hoisted by hand rather than
         * trusting the compiler to see it -- this compiler does not reduce a
         * constant divide, so it gets no benefit of the doubt. */
        const int *brow = g_bayer[y & 3];

        s = src->px + (size_t)y * src->pitch;
        d = (mr_u16 *)(dst + (long)y * pitch);

        if (!g_dither) {
            for (x = 0; x < w; x++) {
                /* Full-width locals for the same reason as pack555_d: folded
                 * into the expression this compiled to a 16-bit `movzx`
                 * followed by a 32-bit read, which stalls a P6. Nothing to do
                 * with the dither -- it was here all along. */
                unsigned r, g, b;

                v = s[x];
                r = MR_R(v) >> rsh;
                g = MR_G(v) >> gsh;
                b = MR_B(v) >> bsh;
                d[x] = (mr_u16)((r << rp) | (g << gp) | (b << bp));
            }
            continue;
        }

        if (lut) {
            /* Four columns of the matrix, so four table rows per channel.
             * Resolved once a row: the dither phase repeats every four pixels
             * and a per-pixel `DOFF` was part of what filled the registers.
             *
             * The inner loop is then three loads, three shifts and two ORs,
             * with nothing live but the pointers and the field positions --
             * no add, no clamp, no bound to hold. */
            const mr_u8 *tr[4], *tg[4], *tb[4];
            int i;

            for (i = 0; i < 4; i++) {
                tr[i] = qt_row(rsh, brow[i]);
                tg[i] = qt_row(gsh, brow[i]);
                tb[i] = qt_row(bsh, brow[i]);
            }

            for (x = 0; x < w; x++) {
                unsigned r, g, b;   /* full width, see pack555_d */

                i = x & 3;
                v = s[x];
                r = tr[i][MR_R(v)];
                g = tg[i][MR_G(v)];
                b = tb[i][MR_B(v)];
                d[x] = (mr_u16)((r << rp) | (g << gp) | (b << bp));
            }
            continue;
        }

        /* A field width the tables do not cover. Same arithmetic the tables
         * were built from, so the two produce identical bytes; reached only by
         * a card describing something no 15 or 16bpp display has described. */
        for (x = 0; x < w; x++) {
            int bay = brow[x & 3];

            v = s[x];
            d[x] = (mr_u16)(
                ((mr_u32)DITH(MR_R(v), rsh, DOFF(bay, rsh), rmx) << rp) |
                ((mr_u32)DITH(MR_G(v), gsh, DOFF(bay, gsh), gmx) << gp) |
                ((mr_u32)DITH(MR_B(v), bsh, DOFF(bay, bsh), bmx) << bp));
        }
    }
}

/* Anything else the fields can describe: assemble the pixel and store it
 * little-endian, one byte at a time. Slow and never used by a mode anybody
 * has reported, but it is what keeps an unusual card showing a picture
 * instead of nothing. */
static void pack_generic(const mr_pack_fmt *f, const mr_surface *src,
                         mr_u8 *dst, long pitch, int y0, int y1)
{
    int y, x, i, w, bytes;
    const mr_u32 *s;
    mr_u8 *d;
    mr_u32 v, p;
    const int rsh = 8 - f->rsize, gsh = 8 - f->gsize, bsh = 8 - f->bsize;
    const int rmx = (1 << f->rsize) - 1;
    const int gmx = (1 << f->gsize) - 1;
    const int bmx = (1 << f->bsize) - 1;

    w = src->w;
    bytes = f->bpp / 8;

    for (y = y0; y < y1; y++) {
        const int *brow = g_bayer[y & 3];

        s = src->px + (size_t)y * src->pitch;
        d = dst + (long)y * pitch;
        for (x = 0; x < w; x++) {
            v = s[x];

            /* The same dither as pack16, because packtest asserts this loop
             * and the specialised ones agree and an undithered generic path
             * would make that comparison fail for a reason that is not a
             * bug. Only reached by a card whose channel order pack16 cannot
             * express, but it has to produce the same picture.
             *
             * Skipped for a channel already 8 bits wide: a 24 or 32bpp format
             * comes through here too, the shift is zero, and dithering a
             * value that is not being quantised would add noise for nothing.
             * `4 - sh` would also shift by 4 and leave the offset 0, so this
             * is belt and braces -- but the intent is worth stating. */
            if (!g_dither || (rsh == 0 && gsh == 0 && bsh == 0)) {
                p = ((mr_u32)(MR_R(v) >> rsh) << f->rpos) |
                    ((mr_u32)(MR_G(v) >> gsh) << f->gpos) |
                    ((mr_u32)(MR_B(v) >> bsh) << f->bpos);
            } else {
                int bay = brow[x & 3];

                p = ((mr_u32)DITH(MR_R(v), rsh, DOFF(bay, rsh), rmx)
                        << f->rpos) |
                    ((mr_u32)DITH(MR_G(v), gsh, DOFF(bay, gsh), gmx)
                        << f->gpos) |
                    ((mr_u32)DITH(MR_B(v), bsh, DOFF(bay, bsh), bmx)
                        << f->bpos);
            }

            for (i = 0; i < bytes; i++)
                *d++ = (mr_u8)(p >> (i * 8));
        }
    }
}

/* --- dispatch -------------------------------------------------------------- */

static int is_std(const mr_pack_fmt *f)
{
    return f->rpos == 16 && f->rsize == 8 &&
           f->gpos ==  8 && f->gsize == 8 &&
           f->bpos ==  0 && f->bsize == 8;
}

/* One channel of a BI_BITFIELDS / VBE-style mask: where the field starts and
 * how wide it is. A mask with a hole in it is rejected rather than
 * approximated -- no display has one, and the loops here index by position
 * and size and could not express it. */
static int mask_to_field(mr_u32 mask, int *pos, int *size)
{
    int p = 0, n = 0;

    if (!mask)
        return 0;

    while (!(mask & 1)) {
        mask >>= 1;
        p++;
    }
    while (mask & 1) {
        mask >>= 1;
        n++;
    }

    *pos = p;
    *size = n;
    return mask == 0;           /* nothing above the run: contiguous */
}

int mr_pack_fmt_masks(mr_pack_fmt *f, int bpp,
                      mr_u32 rmask, mr_u32 gmask, mr_u32 bmask)
{
    if (!f)
        return 0;

    memset(f, 0, sizeof(*f));
    f->bpp = bpp;

    if (bpp != 16 && bpp != 24 && bpp != 32) {
        f->bpp = 0;
        return 0;
    }

    if (!mask_to_field(rmask, &f->rpos, &f->rsize) ||
        !mask_to_field(gmask, &f->gpos, &f->gsize) ||
        !mask_to_field(bmask, &f->bpos, &f->bsize)) {
        f->bpp = 0;
        return 0;
    }

    /* Overlapping channels, or a channel running past the pixel, would make
     * the pack loops write one channel's bits over another's. Checked here
     * because this is the only entry point that cannot see the layout it is
     * being handed. */
    if ((rmask & gmask) || (gmask & bmask) || (rmask & bmask) ||
        f->rpos + f->rsize > bpp ||
        f->gpos + f->gsize > bpp ||
        f->bpos + f->bsize > bpp) {
        f->bpp = 0;
        return 0;
    }

    return 1;
}

int mr_pack_is_bgra32(const mr_pack_fmt *f)
{
    return f != 0 && f->bpp == 32 && is_std(f);
}

void mr_pack_rows(const mr_pack_fmt *f, const mr_surface *src,
                  mr_u8 *dst, long pitch, int y0, int y1)
{
    if (!f || !src || !dst)
        return;
    if (y0 < 0) y0 = 0;
    if (y1 > src->h) y1 = src->h;
    if (y0 >= y1)
        return;

    switch (f->bpp) {
    case 8:
        pack8(src, dst, pitch, y0, y1);
        return;
    case 16:
        /* Contiguous from bit 0 and in R,G,B order is what pack16 assumes;
         * a card that puts blue at the top gets the generic loop. */
        if (f->bpos == 0 && f->gpos == f->bsize &&
            f->rpos == f->bsize + f->gsize &&
            f->rpos + f->rsize <= 16)
            pack16(f, src, dst, pitch, y0, y1);
        else
            pack_generic(f, src, dst, pitch, y0, y1);
        return;
    case 24:
        if (is_std(f))
            pack24(src, dst, pitch, y0, y1);
        else
            pack_generic(f, src, dst, pitch, y0, y1);
        return;
    case 32:
        if (is_std(f))
            pack32(src, dst, pitch, y0, y1);
        else
            pack_generic(f, src, dst, pitch, y0, y1);
        return;
    default:
        /* Not a whole number of bytes per pixel. mr_vbe_decode refuses those
         * modes, and mr_pack_fmt_std refuses to build one, so reaching here
         * means a caller invented a format. Draw nothing rather than walk off
         * the end of the buffer. */
        return;
    }
}

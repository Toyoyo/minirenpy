/* pack.c: 32bpp ARGB to whatever the display wants.
 *
 * The pack loops were inside present.c until the DOS port needed them, which
 * meant they were part of the one file the suite cannot reach. They are now
 * pure -- a surface and a pointer in, bytes out -- and this is their first
 * coverage, for the Win32 build as much as for DOS.
 *
 * Why it is worth testing rather than reading: on DOS the destination is a
 * linear framebuffer, and the two ways this code can be wrong are a shear
 * (bytes per pixel disagreeing with the mode) and wrong colours (fields in
 * the wrong place). Both look like a broken monitor on a machine with no
 * debugger, and neither leaves anything in a log. Here they are a byte
 * comparison.
 *
 * Four claims:
 *
 *   1. Each depth produces the bytes the format says it does, computed from
 *      the field positions rather than from the implementation.
 *   2. The specialised loops agree with the generic one. pack.c fast-paths
 *      the layouts that are common (5:6:5, B,G,R, straight copy) and falls
 *      back to a field-driven loop for the rest; an ordinary card and a
 *      strange one must not disagree about the same colour.
 *   3. Packing a band equals packing the frame and taking those rows. The
 *      8bpp dither reads the row index, so a band that renumbered its rows
 *      would shift the pattern and leave a seam across the dialogue box as a
 *      line types out -- exactly the bug scaletest describes one layer up.
 *   4. Nothing is written outside the rows and columns asked for. A VBE
 *      scanline is usually wider than the picture and a DIB row is DWORD
 *      aligned, so the padding is real and running into it is how a 24bpp
 *      mode shears.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "pack.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- fixture --------------------------------------------------------------
 *
 * Colours chosen so that a swapped channel cannot pass: the primaries, a
 * near-neutral (which the 8bpp path sends to the grey ramp and the others
 * treat as any other colour), and values whose low bits are lost at 15 and 16
 * bits so that truncation is visible.
 */
static const mr_u32 PIX[] = {
    0xFF000000u,    /* black */
    0xFFFFFFFFu,    /* white */
    0xFFFF0000u,    /* red */
    0xFF00FF00u,    /* green */
    0xFF0000FFu,    /* blue */
    0xFF123456u,    /* arbitrary, low bits set in all three */
    0xFF808182u,    /* near-neutral: max-min is 2 */
    0xFFFEDCBAu
};
#define NPIX ((int)(sizeof(PIX) / sizeof(PIX[0])))

static mr_surface *make_src(int w, int h)
{
    mr_surface *s = mr_surface_new(w, h);
    int x, y;

    if (!s) {
        printf("out of memory\n");
        exit(1);
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            s->px[y * s->pitch + x] = PIX[(x + y * 3) % NPIX];
    return s;
}

/* The reference: one pixel, packed from the field description. Written from
 * what the fields mean, not from how pack.c groups them. */
/* The dither, transcribed from pack.h's description rather than from pack.c's
 * code, and written out longhand where pack.c uses two macros.
 *
 * Deliberately a second spelling, in the manner of yuvmmx.h and yuvtest: a
 * reference that shared the engine's macros would agree with any drift in them
 * and assert nothing about the arithmetic, only about the plumbing. The spec
 * being transcribed is:
 *
 *     off = (bayer[y & 3][x & 3] << sh) >> 4      sh = 8 - field width
 *     q   = min((v + off) >> sh, (1 << width) - 1)
 *
 * The matrix has to match pack.c's exactly, since the *values* are the dither
 * and not an implementation detail; this is the one thing copied rather than
 * re-derived, and copying it is the point.
 */
static const int ref_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

static int ref_dither = 1;

static unsigned ref_chan(unsigned v, int width, int bay)
{
    int sh = 8 - width;
    int maxq = (1 << width) - 1;
    int off, q;

    if (!ref_dither)
        return v >> sh;

    off = (bay << sh) >> 4;
    q = ((int)v + off) >> sh;
    if (q > maxq)
        q = maxq;
    return (unsigned)q;
}

static mr_u32 ref_pixel_at(const mr_pack_fmt *f, mr_u32 v, int x, int y)
{
    int bay = ref_bayer[y & 3][x & 3];

    return ((mr_u32)ref_chan(MR_R(v), f->rsize, bay) << f->rpos) |
           ((mr_u32)ref_chan(MR_G(v), f->gsize, bay) << f->gpos) |
           ((mr_u32)ref_chan(MR_B(v), f->bsize, bay) << f->bpos);
}

/* The undithered packing, which is what ref_chan falls back to when
 * ref_dither is clear -- kept as its own function because `dither=0` has to
 * reproduce exactly what shipped before, and that is a claim about this
 * expression and not about a flag. */
static mr_u32 ref_pixel(const mr_pack_fmt *f, mr_u32 v)
{
    return ((mr_u32)(MR_R(v) >> (8 - f->rsize)) << f->rpos) |
           ((mr_u32)(MR_G(v) >> (8 - f->gsize)) << f->gpos) |
           ((mr_u32)(MR_B(v) >> (8 - f->bsize)) << f->bpos);
}

/* A whole buffer, packed by the reference: little-endian, bpp/8 bytes per
 * pixel, at the given pitch, leaving everything else as it found it. */
static void ref_pack(const mr_pack_fmt *f, const mr_surface *src,
                     mr_u8 *dst, long pitch, int y0, int y1)
{
    int x, y, i, bytes = f->bpp / 8;
    mr_u32 p;
    mr_u8 *d;

    for (y = y0; y < y1; y++) {
        d = dst + (size_t)y * pitch;
        for (x = 0; x < src->w; x++) {
            /* x and y are the absolute column and row of the picture, which
             * is what makes the dither phase the same for a band as for the
             * same rows of a full frame -- the invariant pack.h states and
             * the band claims below rely on. */
            p = ref_pixel_at(f, src->px[(size_t)y * src->pitch + x], x, y);
            for (i = 0; i < bytes; i++)
                *d++ = (mr_u8)(p >> (i * 8));
        }
    }
}

/* --- helpers -------------------------------------------------------------- */

#define FILL 0xA5      /* so untouched bytes are recognisable */

static mr_u8 *fresh(long bytes)
{
    mr_u8 *p = (mr_u8 *)malloc((size_t)bytes);
    if (!p) { printf("out of memory\n"); exit(1); }
    memset(p, FILL, (size_t)bytes);
    return p;
}

static void fmt_set(mr_pack_fmt *f, int bpp, int rp, int rs,
                    int gp, int gs, int bp, int bs)
{
    f->bpp = bpp;
    f->rpos = rp; f->rsize = rs;
    f->gpos = gp; f->gsize = gs;
    f->bpos = bp; f->bsize = bs;
}

/* Pack the same picture with pack.c and with the reference, and compare the
 * whole buffer including the padding. */
static int same_as_ref(const mr_pack_fmt *f, const mr_surface *src, long pitch)
{
    long bytes = pitch * src->h;
    mr_u8 *a = fresh(bytes), *b = fresh(bytes);
    int ok, x, y;

    mr_pack_rows(f, src, a, pitch, 0, src->h);
    ref_pack(f, src, b, pitch, 0, src->h);

    /* The fourth byte of a 32bpp pixel is not a colour. pack.c copies the
     * row whole, so it carries the surface's alpha; the reference assembles
     * from the colour fields and leaves it zero. Neither a framebuffer nor a
     * BI_RGB DIB reads it. Mask it rather than make the fast path write a
     * zero per pixel -- that would cost the memcpy, and this pass is the
     * second most expensive thing in a frame on the Pentium III. */
    if (f->bpp == 32 && f->rsize + f->gsize + f->bsize <= 24)
        for (y = 0; y < src->h; y++)
            for (x = 0; x < src->w; x++)
                a[y * pitch + x * 4 + 3] = b[y * pitch + x * 4 + 3] = 0;

    ok = memcmp(a, b, (size_t)bytes) == 0;
    free(a); free(b);
    return ok;
}

int main(void)
{
    mr_pack_fmt f, g;
    mr_surface *src;
    long pitch;
    int ok, i, x, y;

    printf("pack.c\n\n");

    src = make_src(17, 9);      /* odd in both directions, so no loop divides */

    /* --- the standard formats --------------------------------------------- */

    ok = mr_pack_fmt_std(&f, 24) && f.bpp == 24 &&
         f.rpos == 16 && f.gpos == 8 && f.bpos == 0 &&
         f.rsize == 8 && f.gsize == 8 && f.bsize == 8;
    check("mr_pack_fmt_std(24) is B,G,R little-endian", ok);

    ok = mr_pack_fmt_std(&f, 16) && f.rsize == 5 && f.gsize == 6 &&
         f.bsize == 5 && f.rpos == 11 && f.gpos == 5 && f.bpos == 0;
    check("mr_pack_fmt_std(16) is 5:6:5", ok);

    ok = !mr_pack_fmt_std(&f, 4) && f.bpp == 0;
    check("mr_pack_fmt_std refuses a depth it cannot pack", ok);

    /* --- each depth against the reference ---------------------------------- */

    mr_pack_fmt_std(&f, 24);
    pitch = (17L * 3 + 3) & ~3L;            /* as a DIB row is aligned */
    check("24bpp matches the field reference, DWORD-aligned pitch",
          same_as_ref(&f, src, pitch));

    mr_pack_fmt_std(&f, 32);
    check("32bpp matches the field reference",
          same_as_ref(&f, src, 17L * 4 + 12));

    mr_pack_fmt_std(&f, 16);
    check("16bpp 5:6:5 matches the field reference",
          same_as_ref(&f, src, 17L * 2 + 6));

    /* 5:5:5, which VBE reports as a 16bpp mode with 15 significant bits --
     * the distinction that makes a 32bpp mode with a blank RsvdMaskSize
     * shear if it is got wrong. */
    fmt_set(&f, 16, 10, 5, 5, 5, 0, 5);
    check("16bpp 5:5:5 matches the field reference",
          same_as_ref(&f, src, 17L * 2));

    /* --- known bytes, not just self-consistency ---------------------------- */
    {
        mr_surface *one = mr_surface_new(2, 1);
        mr_u8 *buf;

        one->px[0] = 0xFFFF0000u;           /* red */
        one->px[1] = 0xFF123456u;

        mr_pack_fmt_std(&f, 16);
        buf = fresh(8);
        mr_pack_rows(&f, one, buf, 8, 0, 1);
        /* Column 0, row 0 is where the Bayer matrix is 0, so the dither adds
         * nothing there and this byte pattern is the same either way. Which
         * is worth knowing rather than relying on: it is why only one of the
         * two claims here moved when the dither went in. */
        ok = buf[0] == 0x00 && buf[1] == 0xF8;      /* 0xF800, little-endian */
        check("5:6:5 red is 0xF800", ok);

        /* Column 1: bayer[0][1] is 8, so the offset is (8 << sh) >> 4 -- 2 for
         * the 6-bit green and 4 for the 5-bit red and blue.
         *
         *   R 0x12 + 4 = 0x16 >> 3 = 2      (unchanged, it had room)
         *   G 0x34 + 2 = 0x36 >> 2 = 13     (unchanged)
         *   B 0x56 + 4 = 0x5A >> 3 = 11     (was 10)
         *
         * Two of the three land in the same bucket and one does not, which is
         * exactly what an ordered dither is: the offset only changes the
         * answer for values near a boundary, and across a region it is the
         * proportion that lands either side that carries the extra precision.
         */
        ok = (buf[2] | (buf[3] << 8)) == ((2 << 11) | (13 << 5) | 11);
        check("5:6:5 dithers the channels it quantises", ok);
        free(buf);

        /* And `dither=0` really is the old behaviour, byte for byte, because
         * it is the escape hatch if this ever looks wrong on real hardware --
         * a switch that does not restore exactly what shipped before is not
         * an escape hatch. */
        mr_pack_dither(0);
        buf = fresh(8);
        mr_pack_rows(&f, one, buf, 8, 0, 1);
        /* 0x12 >> 3 = 2, 0x34 >> 2 = 13, 0x56 >> 3 = 10 */
        ok = (buf[2] | (buf[3] << 8)) == ((2 << 11) | (13 << 5) | 10);
        check("dither=0 truncates each channel from the top, as it always did",
              ok);
        free(buf);
        mr_pack_dither(1);

        mr_pack_fmt_std(&f, 24);
        buf = fresh(8);
        mr_pack_rows(&f, one, buf, 8, 0, 1);
        ok = buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFF &&
             buf[3] == 0x56 && buf[4] == 0x34 && buf[5] == 0x12;
        check("24bpp writes B,G,R in that order", ok);
        free(buf);

        mr_pack_fmt_std(&f, 32);
        buf = fresh(8);
        mr_pack_rows(&f, one, buf, 8, 0, 1);
        ok = buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0xFF &&
             buf[4] == 0x56 && buf[5] == 0x34 && buf[6] == 0x12;
        check("32bpp writes B,G,R,x", ok);
        free(buf);

        mr_surface_free(one);
    }

    /* --- the specialised loops against the generic one ---------------------
     *
     * pack.c fast-paths the common layouts. The generic loop is not visible
     * from here, so it is reached the way a real card would reach it: by
     * describing the same colours with fields the fast path refuses. A
     * byte-reversed 16bpp has blue at the top, and a 24bpp with red and blue
     * exchanged is a layout some framebuffers really do report.
     */
    {
        mr_surface *two = mr_surface_new(5, 2);
        mr_u8 *a, *b;

        for (y = 0; y < 2; y++)
            for (x = 0; x < 5; x++)
                two->px[y * two->pitch + x] = PIX[(x + y) % NPIX];

        /* B,G,R at 16bpp: same bit widths, reversed order. The generic loop
         * must produce exactly what the reference says. */
        fmt_set(&f, 16, 0, 5, 5, 6, 11, 5);
        check("16bpp with blue at the top goes through the generic loop",
              same_as_ref(&f, two, 10));

        fmt_set(&f, 24, 0, 8, 8, 8, 16, 8);
        check("24bpp R,G,B matches the reference too", same_as_ref(&f, two, 15));

        /* And the fast path is not merely a different answer that happens to
         * be self-consistent: 5:6:5 described as itself must equal 5:6:5
         * assembled generically. Same fields, so same bytes -- the only way
         * these differ is if the specialisation is wrong. */
        mr_pack_fmt_std(&f, 16);
        fmt_set(&g, 16, 11, 5, 5, 6, 0, 5);
        a = fresh(20); b = fresh(20);
        mr_pack_rows(&f, two, a, 10, 0, 2);
        ref_pack(&g, two, b, 10, 0, 2);
        check("the 5:6:5 fast path equals the field-driven packing",
              memcmp(a, b, 20) == 0);
        free(a); free(b);

        mr_surface_free(two);
    }

    /* --- bands equal frames ------------------------------------------------
     *
     * At every depth, and 8bpp is the one that can fail: its dither reads the
     * row index. If mr_pack_rows numbered rows from y0 instead of from the
     * top, a band would carry the wrong four-row phase and leave a seam.
     */
    {
        static const int depths[] = { 8, 16, 24, 32 };
        int d;

        for (d = 0; d < 4; d++) {
            mr_u8 *whole, *banded;
            long p;
            char label[80];

            mr_pack_fmt_std(&f, depths[d]);
            p = (long)src->w * (depths[d] / 8) + 5;
            whole  = fresh(p * src->h);
            banded = fresh(p * src->h);

            mr_pack_rows(&f, src, whole, p, 0, src->h);
            /* Three bands, none of them a multiple of the dither period. */
            mr_pack_rows(&f, src, banded, p, 0, 3);
            mr_pack_rows(&f, src, banded, p, 3, 5);
            mr_pack_rows(&f, src, banded, p, 5, src->h);

            sprintf(label, "%dbpp: three bands equal one frame", depths[d]);
            check(label, memcmp(whole, banded, (size_t)(p * src->h)) == 0);
            free(whole); free(banded);
        }
    }

    /* --- a negative pitch writes the picture bottom-up ----------------------
     *
     * How the DOS presenter gets scale.c's bottom-up output onto a top-down
     * framebuffer without flipping a megabyte a frame: point dst at the last
     * row and step backwards. Row y must land exactly where row y of a
     * positive-pitch pack would be if the buffer were then reversed --
     * dither included, since the phase follows the row index either way.
     */
    {
        mr_u8 *up, *down;
        long p = 17L * 2 + 4;
        int d;
        static const int depths[] = { 8, 16, 24, 32 };

        for (d = 0; d < 4; d++) {
            char label[80];

            mr_pack_fmt_std(&f, depths[d]);
            p = (long)src->w * (depths[d] / 8) + 4;
            up   = fresh(p * src->h);
            down = fresh(p * src->h);

            mr_pack_rows(&f, src, up, p, 0, src->h);
            mr_pack_rows(&f, src, down + (src->h - 1) * p, -p, 0, src->h);

            ok = 1;
            for (y = 0; y < src->h; y++)
                if (memcmp(up + (long)y * p,
                           down + (long)(src->h - 1 - y) * p,
                           (size_t)(src->w * (depths[d] / 8))) != 0)
                    ok = 0;
            sprintf(label, "%dbpp: a negative pitch writes bottom-up",
                    depths[d]);
            check(label, ok);
            free(up); free(down);
        }

        /* And a band of it still lands in the right place, which is the one
         * that would break if the offset were computed from y - y0 or from
         * an unsigned product. */
        mr_pack_fmt_std(&f, 24);
        p = (long)src->w * 3 + 4;
        up   = fresh(p * src->h);
        down = fresh(p * src->h);
        mr_pack_rows(&f, src, up + (src->h - 1) * p, -p, 0, src->h);
        mr_pack_rows(&f, src, down + (src->h - 1) * p, -p, 0, 4);
        mr_pack_rows(&f, src, down + (src->h - 1) * p, -p, 4, src->h);
        check("a band with a negative pitch equals the whole frame",
              memcmp(up, down, (size_t)(p * src->h)) == 0);
        free(up); free(down);
    }

    /* --- the rows and columns not asked for -------------------------------- */
    {
        mr_u8 *buf;
        long p = 17L * 3 + 7;       /* seven bytes of padding per row */

        mr_pack_fmt_std(&f, 24);
        buf = fresh(p * src->h);
        mr_pack_rows(&f, src, buf, p, 2, 5);

        ok = 1;
        for (y = 0; y < src->h; y++) {
            for (i = 0; i < (int)p; i++) {
                mr_u8 v = buf[y * p + i];
                if (y >= 2 && y < 5 && i < 17 * 3)
                    continue;               /* inside the band: packed */
                if (v != FILL)
                    ok = 0;
            }
        }
        check("only the rows asked for, and never the row padding", ok);

        /* Out of range is nothing at all, not a clamp to one row and not a
         * walk off the end. */
        memset(buf, FILL, (size_t)(p * src->h));
        mr_pack_rows(&f, src, buf, p, 7, 7);
        mr_pack_rows(&f, src, buf, p, 99, 200);
        mr_pack_rows(&f, src, buf, p, -4, 0);
        ok = 1;
        for (i = 0; i < (int)(p * src->h); i++)
            if (buf[i] != FILL) ok = 0;
        check("an empty or out-of-range row range writes nothing", ok);

        /* A range that overhangs the bottom is clamped to the surface rather
         * than read past it -- a band at the foot of the screen does this
         * every time the dialogue box is the last thing redrawn. */
        memset(buf, FILL, (size_t)(p * src->h));
        mr_pack_rows(&f, src, buf, p, src->h - 2, src->h + 40);
        ok = buf[(src->h - 1) * p] != FILL && buf[(src->h - 3) * p] == FILL;
        check("a range overhanging the last row is clamped, not refused", ok);

        free(buf);
    }

    /* --- a depth nobody can pack ------------------------------------------- */
    {
        mr_u8 *buf = fresh(64);
        fmt_set(&f, 4, 0, 1, 0, 1, 0, 1);
        mr_pack_rows(&f, src, buf, 8, 0, 2);
        ok = 1;
        for (i = 0; i < 64; i++) if (buf[i] != FILL) ok = 0;
        check("a format that is not whole bytes draws nothing", ok);
        free(buf);
    }

    /* --- the dither grid, and patching a rectangle --------------------------
     *
     * mr_pack_snap_rect exists so the DOS pointer can be repainted without
     * leaving a square of wrong-phase dither trailing behind it. The claim
     * that matters is the last one: a sub-rectangle packed on its own must
     * be byte-identical to the same pixels packed as part of the whole
     * picture.
     */
    {
        int rx, ry, rw, rh;

        rx = 5; ry = 7; rw = 3; rh = 3;
        ok = mr_pack_snap_rect(&rx, &ry, &rw, &rh, 100, 100) &&
             rx == 4 && ry == 4 && rw == 4 && rh == 8;
        check("a rectangle grows out to the 4x4 grid", ok);

        rx = 8; ry = 12; rw = 4; rh = 8;
        ok = mr_pack_snap_rect(&rx, &ry, &rw, &rh, 100, 100) &&
             rx == 8 && ry == 12 && rw == 4 && rh == 8;
        check("one already on the grid is left alone", ok);

        rx = -3; ry = -2; rw = 6; rh = 6;
        ok = mr_pack_snap_rect(&rx, &ry, &rw, &rh, 100, 100) &&
             rx == 0 && ry == 0 && rw == 4 && rh == 4;
        check("one overhanging the top left is clipped to it", ok);

        rx = 96; ry = 97; rw = 10; rh = 10;
        ok = mr_pack_snap_rect(&rx, &ry, &rw, &rh, 100, 100) &&
             rx == 96 && ry == 96 && rw == 4 && rh == 4;
        check("one overhanging the bottom right keeps its aligned start", ok);

        rx = 200; ry = 5; rw = 4; rh = 4;
        ok = !mr_pack_snap_rect(&rx, &ry, &rw, &rh, 100, 100);
        check("one entirely outside is refused", ok);

        /* The real claim. Pack the whole picture; then pack a snapped
         * sub-rectangle of it, on its own, through a view into the same
         * pixels -- which is exactly what presdos.c does under the pointer
         * -- and the bytes must match. At 8bpp this fails for any rectangle
         * not on the grid, which is the point of the function. */
        {
            mr_pack_fmt f8;
            mr_u8 *whole, *patch;
            long p = (long)src->w + 3;
            mr_surface view;
            int gy;

            mr_pack_fmt_std(&f8, 8);
            whole = fresh(p * src->h);
            patch = fresh(p * src->h);
            mr_pack_rows(&f8, src, whole, p, 0, src->h);

            rx = 5; ry = 3; rw = 6; rh = 4;
            mr_pack_snap_rect(&rx, &ry, &rw, &rh, src->w, src->h);

            view = *src;
            view.px = src->px + (size_t)ry * src->pitch + rx;
            view.w = rw;
            view.h = rh;
            mr_pack_rows(&f8, &view, patch + (long)ry * p + rx, p, 0, rh);

            ok = 1;
            for (gy = 0; gy < rh; gy++)
                if (memcmp(whole + (long)(ry + gy) * p + rx,
                           patch + (long)(ry + gy) * p + rx,
                           (size_t)rw) != 0)
                    ok = 0;
            check("a snapped sub-rectangle packs identically to the frame",
                  ok);

            /* And the negative control is in the test itself: the same
             * patch one pixel off the grid must *not* match, or the
             * alignment is doing nothing and the check above is vacuous. */
            view.px = src->px + (size_t)(ry + 1) * src->pitch + (rx + 1);
            view.w = rw - 1;
            view.h = rh - 1;
            mr_pack_rows(&f8, &view, patch + (long)(ry + 1) * p + rx + 1, p,
                         0, rh - 1);
            ok = 0;
            for (gy = 0; gy < rh - 1; gy++)
                if (memcmp(whole + (long)(ry + 1 + gy) * p + rx + 1,
                           patch + (long)(ry + 1 + gy) * p + rx + 1,
                           (size_t)(rw - 1)) != 0)
                    ok = 1;
            check("an unaligned one does not, so the grid is load-bearing",
                  ok);

            free(whole); free(patch);
        }
    }

    /* --- the palette -------------------------------------------------------
     *
     * The DOS presenter programs the DAC from this and GDI builds a
     * LOGPALETTE from it, so the two must be looking at the same 256 colours
     * or an 8bpp screen is right under Windows and wrong under DOS.
     */
    {
        mr_u8 pal[MR_PACK_PAL_BYTES];

        mr_pack_palette(pal);

        ok = pal[0] == 0 && pal[1] == 0 && pal[2] == 0;
        check("palette entry 0 is black", ok);

        /* Last of the 6x6x6 cube is white. */
        ok = pal[215 * 3] == 255 && pal[215 * 3 + 1] == 255 &&
             pal[215 * 3 + 2] == 255;
        check("the cube ends at white, entry 215", ok);

        /* Pure red is cube index (5*6+0)*6+0 = 180. */
        ok = pal[180 * 3] == 255 && pal[180 * 3 + 1] == 0 &&
             pal[180 * 3 + 2] == 0;
        check("the cube is indexed (r*6+g)*6+b", ok);

        /* The ramp above it is grey and runs to white. */
        ok = 1;
        for (i = 216; i < 256; i++)
            if (pal[i * 3] != pal[i * 3 + 1] || pal[i * 3] != pal[i * 3 + 2])
                ok = 0;
        check("entries 216..255 are neutral grey", ok);
        ok = pal[216 * 3] == 0 && pal[255 * 3] == 255;
        check("the grey ramp spans black to white", ok);
    }

    /* --- 8bpp quantisation -------------------------------------------------- */
    {
        mr_surface *flat = mr_surface_new(8, 8);
        mr_u8 *buf;
        int cube = 0, grey = 0;

        mr_pack_fmt_std(&f, 8);

        /* A near-neutral goes to the ramp, which is what the ramp is for:
         * the cube's grey steps are 51 apart and both games are mostly dark
         * interiors. */
        for (i = 0; i < 64; i++)
            flat->px[i] = 0xFF808182u;
        buf = fresh(64);
        mr_pack_rows(&f, flat, buf, 8, 0, 8);
        for (i = 0; i < 64; i++)
            if (buf[i] >= 216) grey++; else cube++;
        check("a near-neutral colour lands in the grey ramp", grey == 64);
        free(buf);

        /* A saturated one does not. */
        for (i = 0; i < 64; i++)
            flat->px[i] = 0xFFC04020u;
        buf = fresh(64);
        mr_pack_rows(&f, flat, buf, 8, 0, 8);
        grey = 0;
        for (i = 0; i < 64; i++)
            if (buf[i] >= 216) grey++;
        check("a saturated colour lands in the cube", grey == 0);

        /* And it dithers: a flat colour between two cube levels must not
         * quantise to one index everywhere, or the night scenes band. */
        ok = 0;
        for (i = 1; i < 64; i++)
            if (buf[i] != buf[0]) ok = 1;
        check("a flat colour between levels is dithered, not banded", ok);

        /* The pattern has the Bayer period: the same colour four rows apart
         * and four columns apart gets the same index. */
        ok = 1;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                if (buf[y * 8 + x] != buf[(y + 4) * 8 + x] ||
                    buf[y * 8 + x] != buf[y * 8 + x + 4])
                    ok = 0;
        check("the dither repeats every 4x4", ok);
        free(buf);

        /* Black and white survive it: a dither that pushed either off the
         * end would show as speckle on the letterbox bars and the text. */
        for (i = 0; i < 64; i++)
            flat->px[i] = 0xFF000000u;
        buf = fresh(64);
        mr_pack_rows(&f, flat, buf, 8, 0, 8);
        ok = 1;
        for (i = 0; i < 64; i++)
            if (buf[i] != 216) ok = 0;         /* grey ramp, entry 0 */
        check("black is one index everywhere", ok);
        /* White does not: an ordered dither that offsets before quantising
         * cannot reach the top step for every phase, so pure white comes out
         * as the top two ramp entries mixed -- greys 248 and 255, which is
         * the speckle an 8bpp screen shows on white text. Pinned as it is
         * rather than called a bug: it is what the Win32 8bpp path has
         * always done, and removing the dither at the ends would band every
         * near-white gradient instead. Worth revisiting when there is a real
         * 8bpp DOS screen to look at. */
        for (i = 0; i < 64; i++)
            flat->px[i] = 0xFFFFFFFFu;
        mr_pack_rows(&f, flat, buf, 8, 0, 8);
        ok = 1;
        for (i = 0; i < 64; i++)
            if (buf[i] < 254) ok = 0;
        check("white is within one step of the top of the ramp", ok);
        free(buf);

        mr_surface_free(flat);
    }

    /* --- the quantiser tables equal the division they replace ----------- */
    {
        /* pack8 used to divide by 255 four times a pixel. Open Watcom emits a
         * real idiv for that -- gcc does not, which is why the host said the
         * change was worth nothing and `wdis` on the object said it was worth
         * about 120 cycles a pixel.
         *
         * The tables are exact by construction: the index covers the
         * dividend's entire range, so the table *is* the division. That is
         * only worth asserting because "by construction" is what everyone
         * says right before an off-by-one, and because the claim is cheap --
         * the whole domain is 256 values.
         *
         * It also turned out to be the only thing covering it. Building the
         * cube table with /256 instead of /255 leaves "the cube is indexed
         * (r*6+g)*6+b" and "a saturated colour lands in the cube" both green:
         * the existing claims pin the palette's *structure* and sample a few
         * colours, neither of which notices a quantiser one step out in the
         * middle of its range.
         *
         * Declared here rather than in pack.h: nothing in the engine should
         * read these, and a test reaching past the header is the lesser evil
         * against widening the API for a test's benefit.
         */
        extern const mr_u8 *mr_pack_quant_tables(int grey);
        const mr_u8 *cube = mr_pack_quant_tables(0);
        const mr_u8 *grey = mr_pack_quant_tables(1);
        int v, bad_c = 0, bad_g = 0;

        for (v = 0; v < 256; v++) {
            if (cube[v] != (mr_u8)(v * 5 / 255))
                bad_c++;
            if (grey[v] != (mr_u8)(v * 39 / 255))
                bad_g++;
        }

        check("the cube quantiser table is v*5/255 for all 256 inputs",
              bad_c == 0);
        check("the grey quantiser table is v*39/255 for all 256 inputs",
              bad_g == 0);

        /* The two constants above are spelled out rather than taken from
         * CUBE_LEVELS and GREY_COUNT, which pack.c keeps to itself. If the
         * cube is ever resized these go red, which is the right outcome: the
         * table's bounds and the palette's layout have to be changed
         * together, and a test that silently followed one of them would hide
         * exactly that. */
        check("and those are the cube and ramp sizes the palette uses",
              cube[255] == 5 && grey[255] == 39);
    }

    /* --- formats described by bit masks --------------------------------- */
    {
        /* present.c asks GDI what a 16bpp pixel looks like on this machine
         * and hands the masks straight to mr_pack_fmt_masks. Nothing in
         * present.c can be run on this host, so the arithmetic was moved here
         * to be reachable -- the risk split CLAUDE.md asks for, with the
         * untestable half reduced to a GetDIBits call and an assignment.
         *
         * The two layouts that matter are 5:6:5 and 5:5:5. Getting them
         * confused does not fail, it tints every pixel on the screen, and on
         * Win32s there is no way to see that except by looking. */
        mr_pack_fmt f;

        check("5:6:5 masks give the same format as mr_pack_fmt_std(16)",
              mr_pack_fmt_masks(&f, 16, 0xF800, 0x07E0, 0x001F) &&
              f.bpp == 16 &&
              f.rpos == 11 && f.rsize == 5 &&
              f.gpos ==  5 && f.gsize == 6 &&
              f.bpos ==  0 && f.bsize == 5);

        check("5:5:5 masks are read as 5:5:5, not rounded to 5:6:5",
              mr_pack_fmt_masks(&f, 16, 0x7C00, 0x03E0, 0x001F) &&
              f.rpos == 10 && f.rsize == 5 &&
              f.gpos ==  5 && f.gsize == 5 &&
              f.bpos ==  0 && f.bsize == 5);

        /* A card that puts blue at the top. pack.c has a generic loop for
         * exactly this, so it must be describable rather than refused. */
        check("a reversed layout is described, not refused",
              mr_pack_fmt_masks(&f, 16, 0x001F, 0x07E0, 0xF800) &&
              f.rpos == 0 && f.bpos == 11);

        check("32bpp masks work too",
              mr_pack_fmt_masks(&f, 32, 0x00FF0000, 0x0000FF00, 0x000000FF) &&
              f.rpos == 16 && f.rsize == 8 && f.bsize == 8);

        /* And the refusals. Each of these is a layout the pack loops would
         * silently corrupt, so the answer has to be "no" and the caller's
         * fallback has to run. */
        /* 0xD800 is bits 11,12,14,15 -- a hole at 13. Chosen so that it does
         * *not* overlap green or blue: the obvious 0xF400 has a stray bit
         * inside green's range, so the overlap check below refuses it and the
         * contiguity check is never exercised. This claim went green with
         * contiguity testing removed until the mask was changed. */
        check("a mask with a hole in it is refused",
              mr_pack_fmt_masks(&f, 16, 0xD800, 0x07E0, 0x001F) == 0);
        check("overlapping channels are refused",
              mr_pack_fmt_masks(&f, 16, 0xF800, 0x0FE0, 0x001F) == 0);
        check("a channel running past the pixel is refused",
              mr_pack_fmt_masks(&f, 16, 0xF8000, 0x07E0, 0x001F) == 0);
        check("an empty mask is refused",
              mr_pack_fmt_masks(&f, 16, 0xF800, 0, 0x001F) == 0);
        check("a depth with no pack loop is refused",
              mr_pack_fmt_masks(&f, 12, 0xF00, 0x0F0, 0x00F) == 0);
        check("a refused format packs nothing",
              (mr_pack_fmt_masks(&f, 16, 0xF800, 0x0FE0, 0x001F),
               f.bpp == 0));

        /* The probed 5:6:5 must pack identically to the one we build
         * ourselves -- otherwise the fast path in mr_pack_rows and the
         * probed format disagree, and the 16bpp DIB would be subtly wrong
         * only on hardware that reports bitfields. */
        {
            mr_pack_fmt probed, std;
            mr_u8 *a = (mr_u8 *)malloc((size_t)src->w * src->h * 2);
            mr_u8 *b = (mr_u8 *)malloc((size_t)src->w * src->h * 2);

            mr_pack_fmt_masks(&probed, 16, 0xF800, 0x07E0, 0x001F);
            mr_pack_fmt_std(&std, 16);

            if (a && b) {
                long stride = (long)src->w * 2;
                memset(a, 0, (size_t)stride * src->h);
                memset(b, 0xFF, (size_t)stride * src->h);
                mr_pack_rows(&probed, src, a, stride, 0, src->h);
                mr_pack_rows(&std,    src, b, stride, 0, src->h);
                check("a probed 5:6:5 packs the same bytes as the built one",
                      memcmp(a, b, (size_t)stride * src->h) == 0);
            } else {
                check("a probed 5:6:5 packs the same bytes as the built one",
                      0);
            }
            free(a);
            free(b);
        }
    }

    /* --- what the dither is actually for -----------------------------------
     *
     * The claims above are all "the two loops agree" and "the bytes are where
     * the fields say". None of them would notice a dither that was arithmetic
     * noise, because the reference performs the same arithmetic. These are
     * about whether it does the job: quantising a smooth ramp to 5 bits should
     * come out *right on average*, which is the whole reason to add noise to a
     * picture on purpose.
     */
    {
        /* A ramp with a gentle slope, so most steps fall between two output
         * levels -- which is where dithering does its work and truncation is
         * at its worst. A flat fill could not show the difference and a steep
         * ramp would cross a level every pixel. */
        mr_surface *ramp = mr_surface_new(256, 16);
        long rp2 = 256L * 2;
        mr_u8 *a = fresh(rp2 * 16);
        long sum_d = 0, sum_t = 0;
        long n = 0;

        for (y = 0; y < 16; y++)
            for (x = 0; x < 256; x++) {
                int v = x;              /* 0..255 across, constant down */
                ramp->px[(size_t)y * ramp->pitch + x] = MR_ARGB(255, v, v, v);
            }

        mr_pack_fmt_std(&f, 16);        /* 5:6:5 */

        /* Dithered: sum the signed error of the red channel, expanded back to
         * 8 bits the way the hardware does it -- zero-fill, which is what
         * both of the user's screenshots show (they top out at 248/252, not
         * 255, in both the 16bpp and the 24bpp path). */
        mr_pack_dither(1);
        mr_pack_rows(&f, ramp, a, rp2, 0, 16);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 256; x++) {
                int px = a[y * rp2 + x * 2] | (a[y * rp2 + x * 2 + 1] << 8);
                sum_d += (long)(((px >> 11) & 31) << 3) - x;
                n++;
            }

        mr_pack_dither(0);
        mr_pack_rows(&f, ramp, a, rp2, 0, 16);
        for (y = 0; y < 16; y++)
            for (x = 0; x < 256; x++) {
                int px = a[y * rp2 + x * 2] | (a[y * rp2 + x * 2 + 1] << 8);
                sum_t += (long)(((px >> 11) & 31) << 3) - x;
            }
        mr_pack_dither(1);

        printf("   ramp mean error x1000: dithered %ld, truncated %ld"
               " (over %ld pixels)\n",
               sum_d * 1000 / n, sum_t * 1000 / n, n);

        /* Truncation is biased: it can only ever round down, so over a ramp it
         * loses about half a step -- 4 counts of a 5-bit channel. That bias is
         * the banding, and it is also a uniform darkening of the picture. */
        check("truncation is biased dark by about half a step",
              sum_t / n <= -3 && sum_t / n >= -5);

        /* The dither's offsets span the step evenly, so the error cancels:
         * -0.109 of a count against truncation's -3.500, which is 32 times
         * better and the claim that says this improves the picture rather
         * than merely scrambling it.
         *
         * Not exactly zero, and the residue is explained rather than fudged:
         * the clamp at the top of the range can only pull downwards, so a ramp
         * that reaches 255 ends slightly dark.
         *
         * The bound is set by what a *wrong* implementation would give, not by
         * what this one happens to give. An offset range off by one in either
         * direction moves the mean half a count -- 500 in these units -- and
         * pack8's centred offset would give about the same; truncation gives
         * -3500. So anything real lands an order of magnitude outside 150,
         * and the threshold catches the mistakes that look fine side by side
         * and shift every colour in the game. */
        check("dithering is unbiased, so it does not darken the picture",
              sum_d * 1000 / n <= 150 && sum_d * 1000 / n >= -150);

        free(a);
        mr_surface_free(ramp);
    }

    /* The clamp. A dither offset pushes a value up, so the top of the range is
     * where it can overflow its field and wrap into the next channel -- which
     * would not be a subtle tint, it would be red speckle in white. Checked at
     * every position in the dither cell, since only some of them add enough. */
    {
        mr_surface *w4 = mr_surface_new(4, 4);
        mr_u8 *a = fresh(4L * 4 * 2);

        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                w4->px[(size_t)y * w4->pitch + x] = 0xFFFFFFFFu;

        mr_pack_fmt_std(&f, 16);
        mr_pack_rows(&f, w4, a, 8, 0, 4);

        ok = 1;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int px = a[y * 8 + x * 2] | (a[y * 8 + x * 2 + 1] << 8);
                if (px != 0xFFFF)
                    ok = 0;
            }
        check("white stays white at every dither position, no field overflow",
              ok);

        free(a);
        mr_surface_free(w4);
    }

    /* `dither=0` against the *undithered* reference, over a whole buffer and
     * both 16bpp layouts. The single-pixel claim earlier shows the bits are
     * right; this one shows the flag turns the whole loop back into what
     * shipped, which is what makes it an escape hatch rather than a
     * near-miss. ref_dither follows it so the reference is comparing like
     * with like. */
    {
        mr_pack_dither(0);
        ref_dither = 0;

        mr_pack_fmt_std(&f, 16);
        check("dither=0 reproduces the old 5:6:5 packing over a whole buffer",
              same_as_ref(&f, src, 17L * 2));

        fmt_set(&f, 16, 10, 5, 5, 5, 0, 5);
        check("and the old 5:5:5 packing too",
              same_as_ref(&f, src, 17L * 2));

        mr_pack_dither(1);
        ref_dither = 1;
    }

    /* A field narrower than the Bayer matrix's four bits. No display reports
     * one, but mask_to_field will accept it, and the obvious spelling of the
     * offset -- `bayer >> (4 - sh)` -- shifts by a negative amount here, which
     * is undefined rather than merely wrong. So the form that avoids it is
     * worth a claim: the only thing being asserted is that this agrees with
     * the reference and stays inside the field. */
    {
        fmt_set(&f, 16, 8, 2, 4, 2, 0, 2);     /* 2:2:2, absurd but legal */
        check("a field narrower than the dither matrix packs sanely",
              same_as_ref(&f, src, 17L * 2));
    }

    mr_surface_free(src);

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

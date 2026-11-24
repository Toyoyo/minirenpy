/* presdos.c - getting the stage onto the screen through a VBE linear
 * framebuffer. The DOS implementation of present.h; present.c is the Win32s
 * one and the two share everything that is arithmetic.
 *
 * What is left here after that sharing is small, and deliberately so:
 *
 *     scale.c    1280x720 stage down to the picture size      (scaletest)
 *     scale.c    where the picture sits inside the mode, mr_fit_*
 *     vbe.c      which mode to ask for                        (vbetest)
 *     pack.c     32bpp ARGB to the mode's pixel format        (packtest)
 *
 * so this file chooses nothing and computes nothing. It asks the BIOS for a
 * mode, maps the framebuffer, points the pack loops at it and programs the
 * DAC. Everything it does that could be wrong in an interesting way is in a
 * harness somewhere else -- which matters more here than on Win32, because a
 * mistake in this path is a black or sheared screen on a machine that has no
 * debugger and whose log file is not readable until it exits.
 *
 * Three things shape it.
 *
 * First, the framebuffer is top-down and scale.c writes bottom-up, because a
 * Windows DIB is stored bottom-up and that is where the scalers came from.
 * Rather than give scale.c a second orientation, or flip a megabyte a frame,
 * the pack loops are pointed at the last row of the picture and given a
 * negative pitch. See mr_pack_rows.
 *
 * Second, there is no intermediate packed buffer. present.c needs one
 * because SetDIBitsToDevice wants a whole DIB to read from; here the pack
 * loop writes to the card. That saves a copy and a megabyte, and it is safe
 * because the pack loops only ever write -- a read back over the bus is the
 * thing that is catastrophically slow on these cards, and nothing here does
 * one.
 *
 * Third, the mode never changes while the game runs. So the letterbox bars
 * are painted once -- black at mode-set time, then from the game's own
 * texture at the first frame, which is the first moment the pak is mounted
 * -- and never again: nothing else ever writes outside the picture
 * rectangle. present.c repaints its bars on every full frame because a
 * window can be resized and uncovered under it.
 */

#include <stdlib.h>
#include <string.h>

#if defined(__DOS__) || defined(MR_TARGET_DOS)
#include <conio.h>
#endif

#include "present.h"
#include "presdos.h"
#include "pack.h"
#include "scale.h"
#include "vbe.h"
#include "cursor.h"
#include "plat.h"
#include "config.h"
#include "image.h"
#include "vfs.h"

/* At most this many modes off the BIOS. Real cards report fifty-odd; the
 * S3 this was first run against reported 53 and found 41 usable. */
#define MAX_MODES 128

static mr_vbe_mode  g_mode;
static int          g_have_mode;
static mr_u8       *g_fb;           /* the linear framebuffer */
static mr_pack_fmt  g_fmt;

static mr_surface  *g_buf;          /* the scaled picture, 32bpp */
static mr_scaler    g_scaler;
static int          g_bilinear;

/* Video playback's state. Up here rather than beside mr_present_video,
 * because mr_present_resize has to invalidate it and sits above that: g_buf
 * is freed and rebuilt on a resize, and a scaler built against its old
 * dimensions would write outside the new one. See video_setup. */
static mr_yuv_scaler g_vscale;
static int g_vs_w, g_vs_h;          /* frame size the scaler was built for */
static int g_vx, g_vy, g_vw, g_vh;  /* where the video sits inside g_buf */
static int g_vready;

/* The video's own bars need repainting: something other than a video frame
 * has written into the picture buffer since the last one.
 *
 * Set by the stage path and by a resize, cleared when the bars are painted.
 * This is the narrow question CLAUDE.md asks for -- "could the bars have
 * been overwritten?" -- rather than blacking them on every frame. See
 * mr_yuv_bars in yuv.h for what goes wrong without it, and for why the two
 * paths share one buffer in the first place. */
static int g_vbars = 1;

/* The picture rectangle inside the mode, and the mode's size. A local copy of
 * the shared fit, refreshed in mr_present_resize; scale.c holds the
 * authoritative one because input.c reads it too. */
static int g_dx, g_dy, g_dw, g_dh;
static int g_cw, g_ch;

/* MR_PROF accumulators, in milliseconds. The third is the write to the
 * framebuffer, which on Win32 is the GDI call; here the pack and the write
 * are the same loop, so it stays zero and the pack figure covers both. */
static int  g_prof;
static long g_prof_scale, g_prof_pack, g_prof_blit;

void mr_present_profile_enable(int on)
{
    g_prof = on;
}

void mr_present_profile(long *scale_ms, long *pack_ms, long *blit_ms)
{
    *scale_ms = g_prof_scale;
    *pack_ms  = g_prof_pack;
    *blit_ms  = g_prof_blit;
    g_prof_scale = g_prof_pack = g_prof_blit = 0;
}

/* --- the DAC ---------------------------------------------------------------
 *
 * Programmed through the VGA ports rather than through VBE function 4F09h or
 * int 10h AX=1012h. Three reasons, in order: the ports are the same on every
 * card that has ever existed, 4F09h is optional and some BIOSes report
 * support for it and then ignore the call, and going through the BIOS would
 * mean another real-mode buffer and another DPMI 0300h -- which is where
 * both of this port's run-time faults came from.
 *
 * The DAC takes 6 bits per channel, so the palette's 0..255 is shifted down
 * by two. VBE 3.0 added an 8-bit DAC mode (4F08h); it is not asked for,
 * because a card that quietly stays in 6-bit mode would wash the whole
 * picture out by a factor of four and the only clue would be the screen.
 */
static void set_dac(void)
{
#if defined(__DOS__) || defined(MR_TARGET_DOS)
    mr_u8 pal[MR_PACK_PAL_BYTES];
    int i;

    mr_pack_palette(pal);

    outp(0x3C8, 0);                 /* start writing at entry 0 */
    for (i = 0; i < 768; i++)
        outp(0x3C9, pal[i] >> 2);
#endif
}

/* Fill the whole mode with black, picture and bars alike. Once, after the
 * mode is set: a freshly switched mode holds whatever was in video memory
 * before, which is usually the text screen read as pixels. */
static void clear_screen(void)
{
    long y;

    if (!g_fb)
        return;

    /* Row by row rather than one memset of pitch*h, because the last
     * scanline's padding may run past the end of the mapping on a card whose
     * reported pitch is wider than it needs to be. */
    for (y = 0; y < g_mode.h; y++)
        memset(g_fb + y * (long)g_mode.pitch, 0, (size_t)g_mode.pitch);
}

/* --- the letterbox bars ----------------------------------------------------
 *
 * Black unless the game ships a texture. mrpy.ini's `border` names it and
 * defaults to gui/dosborder.png; a game without one is unchanged, which is
 * why there is no fallback art here. The strip is stretched to the height of
 * the bar and repeated across it -- mr_border_row, in scale.c, does that part
 * and scaletest covers it.
 *
 * Two things decide when this runs. The bars are painted once, because
 * nothing else ever writes outside the picture rectangle (see the file
 * header) -- but the texture lives in the pak, and the pak is mounted well
 * after mr_present_init has chosen a mode. So the paint is deferred to the
 * first frame, where the VFS is up, rather than done at mode-set time with
 * nothing to read.
 *
 * Only the top and bottom bars are textured. A 16:9 stage in a 4:3 mode is
 * letterboxed and those are the only bars there are; a mode wider than 16:9
 * would pillarbox instead, and the left and right margins stay black because
 * "stretch vertically, tile horizontally" is the wrong transform for a tall
 * thin rectangle and the right one would need the author to draw a second
 * image. No such mode has turned up on the target cards.
 */
static int g_bars_done;

/* One bar: output rows [y_top, y_top + h) across the full mode width.
 *
 * `phase` keeps the 8bpp dither aligned to the screen rather than to the
 * bar. mr_pack_rows reads the row index to pick the Bayer cell, so packing a
 * bar as rows 0..h would start its pattern at the bar's own corner; the
 * bottom bar starts at an arbitrary y and would be in a different phase from
 * the top one. Offsetting both the source rows and the destination base by
 * y_top mod 4 costs three rows of scratch and makes them agree. Same
 * reasoning as mr_pack_snap_rect, which the pointer uses.
 */
static void paint_bar(const mr_surface *tex, mr_surface *bar,
                      int y_top, int h)
{
    int phase, i;

    if (h <= 0)
        return;

    phase = y_top & 3;
    for (i = 0; i < h; i++)
        mr_border_row(tex, h, i,
                      bar->px + (size_t)(phase + i) * bar->pitch, g_cw);

    mr_pack_rows(&g_fmt, bar,
                 g_fb + (long)(y_top - phase) * g_mode.pitch,
                 (long)g_mode.pitch, phase, phase + h);
}

static void paint_borders(void)
{
    const char *path;
    mr_surface *tex, *bar;
    int top, bot, maxh;

    g_bars_done = 1;

    if (!g_fb || !g_have_mode)
        return;

    top = g_dy;
    bot = g_ch - (g_dy + g_dh);
    if (top <= 0 && bot <= 0)
        return;                 /* 16:9 mode: no bars to put anything in */

    path = mr_config_str("border", "gui/dosborder.png");
    if (!path || !*path || !mr_vfs_exists(path))
        return;

    tex = mr_png_load(path);
    if (!tex)
        return;                 /* png.c logged why */

    maxh = top > bot ? top : bot;
    bar = mr_surface_new(g_cw, maxh + 3);
    if (!bar) {
        mr_log("border: no memory for a %dx%d bar; leaving it black",
               g_cw, maxh);
        mr_surface_free(tex);
        return;
    }

    paint_bar(tex, bar, 0, top);
    paint_bar(tex, bar, g_dy + g_dh, bot);

    mr_log("border: %s, %dx%d, into bars of %d and %d rows",
           path, tex->w, tex->h, top, bot);

    mr_surface_free(bar);
    mr_surface_free(tex);
}

/* Where buffer row 0 of the picture lands, and the step between rows.
 *
 * Buffer rows run bottom-up -- buffer row j is picture row h-1-j -- so the
 * base is the *last* picture row and the step is negative. pack.c takes the
 * pitch as a signed byte count for exactly this. */
static mr_u8 *pic_base(void)
{
    return g_fb
         + (long)(g_dy + g_dh - 1) * g_mode.pitch
         + (long)g_dx * (g_mode.bpp / 8);
}

/* --- the pointer -----------------------------------------------------------
 *
 * There is no hardware cursor and no Windows to draw one, so it is composited
 * by hand. The pixels underneath come out of g_buf, which holds the scaled
 * picture in main memory: reading them back off the card would be correct and
 * ruinously slow, and this way there is no cost at all to having them.
 *
 * Moving the pointer is two patches -- repack the rectangle it was in, then
 * compose and pack the one it is in now -- and both go straight to the
 * framebuffer without disturbing the frame. That is what lets the pointer
 * move while the engine is idle, which is most of the time: the frame loop
 * only draws when something changed, and a pointer that waited for a frame
 * would stick to the screen while the player waved the mouse at a motionless
 * dialogue box.
 *
 * The rectangles are snapped to the dither grid by mr_pack_snap_rect. Without
 * that, an 8bpp patch starts its Bayer phase at its own corner instead of the
 * picture's and leaves a square of visibly wrong dithering trailing the
 * pointer around. See packtest, where the phase is pinned.
 */
static mr_surface *g_cur;        /* the baked art, flipped to bottom-up */
static mr_surface *g_scratch;    /* compose space: art plus the grid slack */
static volatile int g_cx = -1, g_cy = -1;  /* where it should be */
static int g_drawn_x = -1, g_drawn_y = -1; /* where it actually is */
static volatile int g_cur_on;    /* it is on the screen right now */
static int g_crx, g_cry, g_crw, g_crh;   /* where, in buffer coordinates */

/* Set while the framebuffer is being written, so the mouse event handler --
 * which runs at interrupt time and can land anywhere -- declines rather than
 * paints over a frame halfway out. The pointer is then drawn at the end of
 * the next pump instead, a frame late, which is what it was always.
 *
 * A flag and not a disable: an interrupt handler that waits for a 1280x720
 * pack to finish is an interrupt handler holding the machine for a third of
 * a second. It has somewhere else to be. */
static volatile int g_painting;

/* The art is baked top-down, as it was drawn; g_buf is bottom-up, as the
 * scalers write it. Flipping once here is cheaper than flipping per frame and
 * far less confusing than two orientations in the blit. */
static void cursor_build(void)
{
    int x, y;

    g_cur = mr_surface_new(mr_cursor_w, mr_cursor_h);
    g_scratch = mr_surface_new(mr_cursor_w + 2 * 4, mr_cursor_h + 2 * 4);
    if (!g_cur || !g_scratch) {
        mr_surface_free(g_cur);
        mr_surface_free(g_scratch);
        g_cur = NULL;
        g_scratch = NULL;
        mr_log("cursor: out of memory; no pointer will be drawn");
        return;
    }

    for (y = 0; y < mr_cursor_h; y++)
        for (x = 0; x < mr_cursor_w; x++)
            g_cur->px[(size_t)(mr_cursor_h - 1 - y) * g_cur->pitch + x] =
                mr_cursor_px[y * mr_cursor_w + x];
    g_cur->has_alpha = 1;
}

/* Where the pointer sits, in buffer coordinates, snapped and clipped.
 * cbx and cby receive the art's own corner, which the snapping may have
 * moved the rectangle away from. Returns 0 if none of it is on the
 * picture. */
static int cursor_box(int *bx, int *by, int *bw, int *bh, int *cbx, int *cby)
{
    if (g_cx < 0 || !g_cur)
        return 0;

    *cbx = g_cx - mr_cursor_hotx - g_dx;
    *cby = g_dh - (g_cy - mr_cursor_hoty - g_dy) - mr_cursor_h;

    *bx = *cbx;
    *by = *cby;
    *bw = mr_cursor_w;
    *bh = mr_cursor_h;
    return mr_pack_snap_rect(bx, by, bw, bh, g_dw, g_dh);
}

/* Repaint one rectangle of the picture, with or without the pointer over it.
 * Without is the erase; with is the draw. */
static void cursor_paint(int bx, int by, int bw, int bh,
                         int cbx, int cby, int with_cursor)
{
    mr_surface view, sc;
    const mr_surface *src;
    mr_u8 *dst;
    int y;

    view = *g_buf;
    view.px = g_buf->px + (size_t)by * g_buf->pitch + bx;
    view.w = bw;
    view.h = bh;
    src = &view;

    if (with_cursor) {
        sc = *g_scratch;
        sc.w = bw;
        sc.h = bh;
        sc.has_alpha = 0;
        for (y = 0; y < bh; y++)
            memcpy(sc.px + (size_t)y * sc.pitch,
                   view.px + (size_t)y * view.pitch,
                   (size_t)bw * 4);
        mr_blit(&sc, g_cur, cbx - bx, cby - by);
        src = &sc;
    }

    /* Buffer row by is picture row g_dh-1-by, so the first row of the view
     * goes to the *bottom* of the rectangle on screen and the pitch is
     * negative, as it is for a whole frame. */
    dst = g_fb
        + (long)(g_dy + g_dh - 1 - by) * g_mode.pitch
        + (long)(g_dx + bx) * (g_mode.bpp / 8);

    mr_pack_rows(&g_fmt, src, dst, -(long)g_mode.pitch, 0, bh);
}

static void cursor_draw(void)
{
    int bx, by, bw, bh, cbx, cby;

    if (g_cur_on || !g_cur || !g_fb || !g_buf)
        return;
    if (!cursor_box(&bx, &by, &bw, &bh, &cbx, &cby))
        return;

    cursor_paint(bx, by, bw, bh, cbx, cby, 1);
    g_crx = bx; g_cry = by; g_crw = bw; g_crh = bh;
    g_drawn_x = g_cx; g_drawn_y = g_cy;
    g_cur_on = 1;
}

void mr_presdos_cursor_hide(void)
{
    if (!g_cur_on)
        return;
    cursor_paint(g_crx, g_cry, g_crw, g_crh, 0, 0, 0);
    g_cur_on = 0;
}

/* Put the pointer where g_cx/g_cy say it is, if that is not where it already
 * is and if nothing else is painting.
 *
 * Wanted position and drawn position are separate variables on purpose. The
 * mouse event handler can arrive in the middle of a frame, and then the move
 * is only *recorded*; comparing the request against the last request would
 * make that recorded move look already done and the pointer would stay put
 * until the mouse happened to move again. Comparing against what is actually
 * on the screen cannot go wrong that way. */
static void cursor_sync(void)
{
    if (g_painting || !g_cur || !g_fb || !g_buf)
        return;
    if (g_cur_on && g_drawn_x == g_cx && g_drawn_y == g_cy)
        return;

    g_painting = 1;
    mr_presdos_cursor_hide();
    cursor_draw();
    g_painting = 0;
}

void mr_presdos_cursor_move(int ox, int oy)
{
    g_cx = ox;
    g_cy = oy;
    cursor_sync();
}

int mr_present_init(void *oswindow)
{
    static mr_vbe_mode modes[MAX_MODES];
    mr_vbe_want want;
    int n, i, chosen, version = 0;
    long forced;

    (void)oswindow;             /* DOS owns the whole screen; nothing names it */

    g_bilinear = (int)mr_config_int("bilinear", 0, 0, 1);

    /* mrpy.ini `depth`: when set, it outranks resolution -- `depth=32` on a
     * card whose 32bpp modes stop at 800x600 gets 800x600, and means it.
     * 0, the default, is no preference and leaves the picker's own policy
     * alone. `depth=8` outranks the colour class as well, and is the only
     * way to ask for a palettised mode on a card that has truecolour ones.
     * See the ordered list in vbe.h. */
    want.want_bpp  = (int)mr_config_int("depth", 0, 0, 32);
    want.stage_w   = MR_STAGE_W;
    want.stage_h   = MR_STAGE_H;
    /* A palettised mode is allowed but ranked last; see the policy in vbe.h.
     * `truecolour=1` refuses one outright, for someone who would rather have
     * no picture than a dithered one. */
    want.allow_8bpp = !(int)mr_config_int("truecolour", 0, 0, 1);

    n = mr_vbe_enumerate(modes, MAX_MODES, &version);
    if (n < 0) {
        mr_log("vbe: no VESA BIOS");
        return 0;
    }
    mr_log("vbe: version %x.%x, %d usable mode%s",
           (version >> 8) & 0xFF, version & 0xFF, n, n == 1 ? "" : "s");
    if (n == 0)
        return 0;

    /* mrpy.ini `vbemode`: the override for when the policy picks wrong on
     * somebody's card. Written as the number MODES.EXE prints -- which is
     * hex, `0x112`, and the ini reads that spelling as well as decimal, so
     * the number on the screen is the number to type -- without the
     * linear-framebuffer bit, and honoured only if the mode survived
     * enumeration -- forcing a mode the decoder refused would set a mode we
     * then cannot draw into, which is the one failure with no way out. */
    chosen = -1;
    forced = mr_config_int("vbemode", 0, 0, 0xFFFF);
    if (forced > 0) {
        for (i = 0; i < n; i++)
            if (modes[i].number == (int)forced)
                chosen = i;
        if (chosen < 0)
            mr_log("vbe: mrpy.ini asks for mode 0x%03x, which this card does "
                   "not offer as a usable linear mode; choosing instead",
                   (unsigned)forced);
    }

    if (chosen < 0)
        chosen = mr_vbe_pick(modes, n, &want);
    if (chosen < 0) {
        mr_log("vbe: none of the %d modes will do", n);
        return 0;
    }

    g_mode = modes[chosen];
    g_fb = (mr_u8 *)mr_vbe_set_mode(&g_mode);
    if (!g_fb)
        return 0;
    g_have_mode = 1;

    /* The pixel format comes from the mode, not from a table of depths: a
     * card that reports an unusual field layout is handled by the pack loops
     * without anything here enumerating it. A palettised mode has no fields,
     * and pack.c ignores them at 8bpp. */
    g_fmt.bpp   = g_mode.bpp;
    g_fmt.rpos  = g_mode.rpos; g_fmt.rsize = g_mode.rsize;
    g_fmt.gpos  = g_mode.gpos; g_fmt.gsize = g_mode.gsize;
    g_fmt.bpos  = g_mode.bpos; g_fmt.bsize = g_mode.bsize;

    if (g_mode.model == MR_VBE_PACKED)
        set_dac();

    clear_screen();
    cursor_build();

    /* Size everything to the mode we just got, here rather than in the
     * platform. On Win32 the platform asks the window how big its client
     * area came out and passes that in; here the presenter chose the output
     * itself and is the only thing that knows its size, so making the
     * platform ask for it back and hand it over would be ceremony. */
    mr_present_resize(g_mode.w, g_mode.h);
    return 1;
}

int mr_present_depth(void)
{
    return g_have_mode ? g_mode.bpp : 0;
}

void mr_present_shutdown(void)
{
    g_cur_on = 0;
    mr_surface_free(g_cur);
    mr_surface_free(g_scratch);
    g_cur = NULL;
    g_scratch = NULL;

    mr_surface_free(g_buf);
    g_buf = NULL;
    mr_scale_free(&g_scaler);
    mr_yuv_free(&g_vscale);
    g_vready = 0;

    if (g_have_mode) {
        mr_vbe_text_mode();
        g_have_mode = 0;
        g_fb = NULL;
    }
}

/* On Win32 this answers a window resize. Here the only caller is the startup
 * path, with the mode's own size -- but it is written as if the size could
 * change, because mr_fatal's shutdown and a future mode switch both want it
 * to be a function of its argument rather than of when it ran. */
void mr_present_resize(int out_w, int out_h)
{
    if (out_w < 16 || out_h < 16)
        return;

    /* g_buf is about to be freed and rebuilt at a new size, so a video scaler
     * built against its old dimensions is stale and would write outside the
     * new one. Dropping the flag sends the next frame of a cutscene back
     * through video_setup, which also repaints the video's own bars -- the
     * fresh buffer's contents are undefined. */
    g_vready = 0;
    g_vbars = 1;

    mr_fit_set(out_w, out_h);
    mr_fit_get(&g_dx, &g_dy, &g_dw, &g_dh, &g_cw, &g_ch);

    /* The bars just moved. Repaint them at the next frame, which is also
     * what the caller is about to draw. */
    g_bars_done = 0;

    /* Whatever the pointer was drawn over is about to be a different buffer
     * at a different size. Forget it rather than erase it: the caller is
     * repainting everything. */
    g_cur_on = 0;

    if (!g_buf || g_buf->w != g_dw || g_buf->h != g_dh) {
        mr_surface_free(g_buf);
        g_buf = mr_surface_new(g_dw, g_dh);
        if (!g_buf)
            mr_fatal("out of memory sizing the %dx%d presentation buffer",
                     g_dw, g_dh);
    }

    /* Black it out. The pointer composites the pixels underneath it out of
     * g_buf, and it can move long before the first frame is scaled into it --
     * during the pak mount, say, where mr_plat_service runs. An uninitialised
     * malloc block is somebody's freed image, so the pointer would drag a
     * rectangle of the menu background across an otherwise black screen. */
    memset(g_buf->px, 0, (size_t)g_buf->pitch * (size_t)g_buf->h *
                         sizeof(mr_u32));

    mr_scale_free(&g_scaler);
    if (!mr_scale_init(&g_scaler, MR_STAGE_W, MR_STAGE_H, g_dw, g_dh,
                       g_bilinear))
        mr_fatal("out of memory building scale tables");

    mr_log("present: %dx%d picture in a %dx%d mode, %s", g_dw, g_dh,
           g_cw, g_ch,
           g_scaler.kind == MR_SCALE_COPY     ? "1:1" :
           g_scaler.kind == MR_SCALE_HALF     ? "2x2 box" :
           g_scaler.kind == MR_SCALE_BILINEAR ? "bilinear" : "nearest");
}

/* Scale and pack picture rows [r0, r1), straight into the framebuffer.
 *
 * The row range is in picture rows counted from the top, as present.c's is,
 * and the conversion to buffer rows is the same: buffer row j is picture row
 * h-1-j, so rows [r0, r1) from the top are buffer rows [h-r1, h-r0). */
static void present_rows(const mr_surface *stage, int r0, int r1)
{
    unsigned int t0 = 0;
    int h;

    if (!g_buf || !g_fb)
        return;

    /* The first frame is also the first moment the pak is readable, so it is
     * where the letterbox gets its texture. Once; after that nothing writes
     * outside the picture. */
    if (!g_bars_done)
        paint_borders();

    h = g_buf->h;
    if (r0 < 0) r0 = 0;
    if (r1 > h) r1 = h;
    if (r0 >= r1)
        return;

    if (g_prof)
        t0 = mr_plat_ticks();

    /* Scaled and packed a band at a time, so the pointer can be moved
     * between bands. A whole frame is a third of a second on the target --
     * the single longest thing the engine does -- and a pointer that stops
     * dead for a third of a second every time the screen changes is the
     * complaint this whole arrangement exists to answer. SERVICE_ROWS is
     * picked so a band is a few milliseconds: fine enough that the pointer
     * looks continuous, coarse enough that the per-band overhead is lost.
     *
     * g_painting is dropped only across the service call, never across the
     * scale or the pack: the pointer is composited out of g_buf, which the
     * scaler is in the middle of rewriting.
     *
     * The pointer may well be drawn into rows this loop has not reached
     * yet, and then painted over. That is handled at the bottom, where the
     * frame redraws it if the rows it wrote overlapped where it was. */
#define SERVICE_ROWS 48
    {
        int y0, y1;

        for (y0 = r0; y0 < r1; y0 = y1) {
            y1 = y0 + SERVICE_ROWS;
            if (y1 > r1)
                y1 = r1;

            g_painting = 1;

            mr_scale_rows(&g_scaler, stage, g_buf, y0, y1);
            /* The stage covers the bars too; see present.c. */
            g_vbars = 1;

            if (g_prof) {
                g_prof_scale += (long)(mr_plat_ticks() - t0);
                t0 = mr_plat_ticks();
            }

            mr_pack_rows(&g_fmt, g_buf, pic_base(), -(long)g_mode.pitch,
                         h - y1, h - y0);

            if (g_prof) {
                g_prof_pack += (long)(mr_plat_ticks() - t0);
                t0 = mr_plat_ticks();
            }

            g_painting = 0;
            mr_plat_service();
        }
    }

    g_painting = 1;

    /* The frame has just been written over the pointer. Only if it actually
     * covered part of it: a band update away from the pointer leaves it
     * alone, which is the common case while a line types out. Buffer rows
     * [h-r1, h-r0) are what was repainted. */
    if (g_cur_on && h - r1 < g_cry + g_crh && h - r0 > g_cry) {
        g_cur_on = 0;
        cursor_draw();
    }

    g_painting = 0;
}

/* --- video ----------------------------------------------------------------
 *
 * A frame of video does not come through the stage. It is colour-converted
 * and scaled straight into g_buf -- the same buffer mr_present scales the
 * stage into -- and then packed and blitted by the same code. present.h has
 * the reasoning and yuv.h has the measurements; the short version is that the
 * stage is 1280x720 whatever the mode is, and a cutscene cannot afford it.
 *
 * The video's own letterbox lives *inside* g_buf, because g_buf is the
 * picture rect and the video is fitted into that. So there are two
 * independent sets of black bars on a 4:3 screen showing a 2:1 movie: the
 * ones paint_borders owns, outside the 16:9 picture, and these. Neither knows
 * about the other, which is why mr_yuv_fit is a separate function from
 * mr_fit_set.
 *
 * The state this works on is declared at the top of the file, with the rest
 * of the module's, because mr_present_resize has to invalidate it.
 */
static void video_setup(int fw, int fh)
{
    if (g_vready && g_vs_w == fw && g_vs_h == fh &&
        g_vscale.dst_w == g_vw && g_vscale.dst_h == g_vh)
        return;

    mr_yuv_free(&g_vscale);
    g_vready = 0;

    mr_yuv_fit(fw, fh, g_buf->w, g_buf->h, &g_vw, &g_vh);
    g_vx = (g_buf->w - g_vw) / 2;
    g_vy = (g_buf->h - g_vh) / 2;

    /* Even, so that the 4:2:0 chroma a 2x2 luma block shares is not split
     * across the edge of the rectangle. mr_yuv_fit already rounds the size;
     * this is the offset. */
    g_vx &= ~1;
    g_vy &= ~1;

    if (!mr_yuv_init(&g_vscale, fw, fh, g_vw, g_vh)) {
        mr_log("present: out of memory sizing the %dx%d video scaler",
               g_vw, g_vh);
        return;
    }

    /* The rectangle moved, so the bars are in the wrong place; they are
     * painted below rather than here. These are *not* the bars outside the
     * picture rect -- those belong to the 16:9 stage and are painted by
     * paint_borders / letterbox. These are the border the brief asks for
     * "given the aspect ratio" of the video, and a 2:1 movie on a 4:3 screen
     * gets both sets. */
    g_vbars = 1;

    g_vs_w = fw;
    g_vs_h = fh;
    g_vready = 1;

    mr_log("present: video %dx%d at %d,%d in a %dx%d picture, %s%s, %s",
           g_vw, g_vh, g_vx, g_vy, g_buf->w, g_buf->h,
           g_vscale.kind == MR_YUV_DIRECT ? "1:1" : "scaled",
           mr_yuv_simd() ? ", MMX" : "",
           mr_pack_is_bgra32(&g_fmt) ? "fused straight to the framebuffer"
                                     : "through the picture buffer");
}

void mr_present_video(const mr_yuv_frame *frame)
{
    unsigned int t0 = 0;
    int h, y0, y1;

    if (!g_buf || !g_fb || !frame || frame->w <= 0 || frame->h <= 0)
        return;

    if (!g_bars_done)
        paint_borders();

    video_setup(frame->w, frame->h);
    if (!g_vready)
        return;

    if (g_prof)
        t0 = mr_plat_ticks();

    /* The fused path: convert straight into the framebuffer, no picture
     * buffer and no pack pass. Banded the same way the pack below is, and for
     * the same reason -- the service call is what keeps the pointer and the
     * keyboard alive through the slowest thing the engine does.
     *
     * Not taken on a frame whose bars are stale. The bars are g_buf's, and
     * blacking them there means packing g_buf out to the card, so such a
     * frame goes the long way round and gets everything right; the flag is
     * only set by a resize or by the stage having been drawn over a running
     * cutscene, so this is rare and the frame after it is fused again. */
    if (!g_vbars &&
        mr_pack_is_bgra32(&g_fmt) && g_vscale.kind == MR_YUV_DIRECT) {
        int r0, r1;

        for (r0 = 0; r0 < g_vh; r0 = r1) {
            r1 = r0 + SERVICE_ROWS;
            if (r1 > g_vh)
                r1 = g_vh;

            g_painting = 1;
            mr_yuv_rows_fused(&g_vscale, frame, &g_fmt,
                              pic_base(), -(long)g_mode.pitch,
                              g_buf->w, g_buf->h, g_vx, g_vy, r0, r1);
            g_painting = 0;
            mr_plat_service();
        }

        /* All of the frame's cost is in this one column now; pack reads zero
         * on this path, which is how the log says which route ran. */
        if (g_prof)
            g_prof_scale += (long)(mr_plat_ticks() - t0);

        goto cursor;
    }

    /* Repaint the video's bars if anything else has been into the buffer
     * since the last frame -- a resize, or the stage being presented over
     * the top of a running cutscene from WM_PAINT. Only the strips, and only
     * when they can actually be stale. */
    if (g_vbars) {
        mr_yuv_bars(g_buf, g_vx, g_vy, g_vw, g_vh);
        g_vbars = 0;
    }

    mr_yuv_rows(&g_vscale, frame, g_buf, g_vx, g_vy, 0, g_vh);

    if (g_prof) {
        /* Counted as scale time: it is the same step in the same place, and
         * the profile line in main.c already calls that column "scale". */
        g_prof_scale += (long)(mr_plat_ticks() - t0);
        t0 = mr_plat_ticks();
    }

    /* Packed and blitted in bands with a service call between them, as
     * present_rows does and for the same reason -- but *not* by sharing its
     * loop. That loop interleaves a scale step with each band so the pointer
     * stays responsive through the slowest thing the engine does; here the
     * conversion has already happened in one pass, because it reads the YUV
     * planes rather than a surface the band could be cut out of.
     *
     * The whole picture is blitted, not a band: every pixel of a frame of
     * video is different from the last one, so there is nothing to conserve. */
    h = g_buf->h;
    for (y0 = 0; y0 < h; y0 = y1) {
        y1 = y0 + SERVICE_ROWS;
        if (y1 > h)
            y1 = h;

        g_painting = 1;
        mr_pack_rows(&g_fmt, g_buf, pic_base(), -(long)g_mode.pitch,
                     h - y1, h - y0);
        g_painting = 0;
        mr_plat_service();
    }

    if (g_prof)
        g_prof_pack += (long)(mr_plat_ticks() - t0);

cursor:
    /* The frame just covered wherever the pointer was. A cutscene is not
     * normally pointing at anything, but the mouse can still be moved during
     * one, and leaving a half-erased pointer on screen for the rest of the
     * movie is worse than redrawing it. */
    g_painting = 1;
    if (g_cur_on) {
        g_cur_on = 0;
        cursor_draw();
    }
    g_painting = 0;
}

void mr_present(const mr_surface *stage)
{
    if (!g_buf)
        return;
    /* No letterbox here: the bars are painted by the first present_rows and
     * nothing has written outside the picture since. */
    present_rows(stage, 0, g_buf->h);
}

void mr_present_band(const mr_surface *stage, int y, int h)
{
    int r0, r1;

    if (!g_buf || h <= 0)
        return;

    /* Stage rows to picture rows, rounded outwards by one at each end: two
     * rows, against any question of a seam at the band edge whichever scaler
     * is running. Same as present.c, and for the same reason. */
    r0 = y * g_buf->h / MR_STAGE_H - 1;
    r1 = (y + h) * g_buf->h / MR_STAGE_H + 1;

    present_rows(stage, r0, r1);
}

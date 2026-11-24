/* present.c - getting the stage onto the screen through GDI on Win32s.
 *
 * The Win32 implementation of present.h. Everything platform-neutral that
 * was once in here is in scale.h; everything Win32 that was once in the
 * interface -- the HDC on every call -- is now acquired and released at the
 * two entry points below, so the caller never sees one.
 *
 * Two things shape this file.
 *
 * First, CreateDIBSection does not exist on Win32s, so there is no way to get a
 * DIB that GDI and we both write to directly. Everything goes through
 * SetDIBitsToDevice from an ordinary malloc'd buffer.
 *
 * Second, StretchDIBits on Win32s is slow: it runs in software behind a 16-bit
 * thunk and would have to both scale and convert depth every frame. So we scale
 * ourselves into a buffer that is already the exact size of the destination
 * rectangle, and let GDI do nothing but a straight depth conversion.
 *
 * Third, and least obvious: Windows 3.1 GDI only understands DIBs of 1, 4, 8
 * or 24 bits per pixel. 16bpp and 32bpp DIBs are a Win32/NT addition. Whether
 * a 32bpp DIB happens to work turns out to depend on the individual display
 * driver rather than on its colour depth: two different 64k-colour setups,
 * one real and one under DOSBox-X, disagreed about it, the latter rendering
 * garbage. 24bpp sidesteps the question and is supported everywhere.
 *
 * The 8bpp path below is for a genuine 256-colour driver. It has only been
 * exercised through the MR_BPP override, never on real 8bpp hardware.
 */

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>   /* timeGetTime */

#include "present.h"
#include "presw32.h"
#include "pack.h"
#include "scale.h"
#include "config.h"

/* The window we draw into, from mr_present_init. Kept here rather than
 * reached for through the platform so that the two entry points can get a DC
 * without platw32.c having to hand one in. */
static HWND g_wnd;

static mr_surface *g_buf;       /* scaled output, client-sized, 32bpp */

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

/* Packed DIB handed to GDI: 24bpp, or 8bpp with a palette. */
static mr_u8 *g_out;
static long   g_out_stride;
static int    g_depth;          /* bits per pixel the display reports */
static int    g_pal8;           /* non-zero when the 8bpp path is in use */

/* The DIB's own depth: 8, 16 or 24. Not the same question as g_depth.
 *
 * It used to be derived from it in one line -- 8 if palettised, 24 otherwise
 * -- and on a 16bpp display that meant handing GDI a 24bpp DIB and letting
 * the display driver convert every pixel on the way to the screen. Measured
 * on the target that was the single largest cost in a frame of video:
 *
 *   convert 13.40ms + pack 9.70ms + blit 28.28ms
 *
 * where `blit` is SetDIBitsToDevice through the Win32s 16-bit thunk. A DIB
 * that already matches the display gives the driver a copy to do instead of
 * a conversion, and halves the bytes crossing the thunk.
 *
 * 24bpp remains the answer for every depth that is not 8 or 16, including
 * 32bpp displays: a 32bpp DIB would be a third more bytes through the thunk
 * to say the same thing, since the fourth byte is ignored. */
static int    g_dib_bpp;

/* BITMAPINFO plus room for a 256 entry palette. */
static struct {
    BITMAPINFOHEADER h;
    RGBQUAD          colors[256];
} g_bi;

static HPALETTE g_palette;

/* The destination pixel format, set up at init: 8bpp palettised or 24bpp
 * B,G,R. The loops that use it are in pack.c, shared with the DOS presenter.
 */
static mr_pack_fmt g_fmt;

/* MR_PROF accumulators, in milliseconds. */
static int  g_prof;
static long g_prof_scale, g_prof_pack, g_prof_gdi;

void mr_present_profile_enable(int on)
{
    g_prof = on;
}

void mr_present_profile(long *scale_ms, long *pack_ms, long *gdi_ms)
{
    *scale_ms = g_prof_scale;
    *pack_ms  = g_prof_pack;
    *gdi_ms   = g_prof_gdi;
    g_prof_scale = g_prof_pack = g_prof_gdi = 0;
}

/* The colour cube, the grey ramp and the dither that quantises to them are
 * pack.c's now -- the DOS presenter needs exactly the same palette, and two
 * copies of it would drift. mr_pack_palette hands back the 256 entries and
 * build_palette below turns them into GDI's two different spellings. */

/* Destination rect inside the client area (16:9, letterboxed), and the client
 * size it was computed against. A local copy of the shared fit, refreshed in
 * mr_present_resize: these are read on every row of every frame, and a
 * getter call per band is not worth the tidiness. scale.c holds the
 * authoritative one, because input.c needs it too. */
static int g_dx, g_dy, g_dw, g_dh;
static int g_cw, g_ch;

/* How the stage is turned into g_buf, chosen on resize. The tables it holds
 * used to be g_xmap/g_ymap here; they live in scale.c now, with the scalers,
 * so that a harness can reach them. */
static mr_scaler g_scaler;

/* mrpy.ini's `bilinear`, read once at init. A request, not a result: it is
 * ignored at 1:1 and at exactly 2:1, where it would cost time to produce the
 * same pixels. Read g_scaler.kind for what actually runs. */
static int g_bilinear;

/* Build the 6x6x6 cube plus grey ramp, both in the DIB colour table and as a
 * logical palette. Without realizing a palette, GDI would map our colours to
 * the 20 stock system colours and the result would be unrecognisable. */
static void build_palette(void)
{
    LOGPALETTE *lp;
    mr_u8 pal[MR_PACK_PAL_BYTES];
    int i;

    mr_pack_palette(pal);

    for (i = 0; i < 256; i++) {
        g_bi.colors[i].rgbRed      = (BYTE)pal[i * 3 + 0];
        g_bi.colors[i].rgbGreen    = (BYTE)pal[i * 3 + 1];
        g_bi.colors[i].rgbBlue     = (BYTE)pal[i * 3 + 2];
        g_bi.colors[i].rgbReserved = 0;
    }

    lp = (LOGPALETTE *)malloc(sizeof(LOGPALETTE) +
                              255 * sizeof(PALETTEENTRY));
    if (!lp)
        return;

    lp->palVersion = 0x300;
    lp->palNumEntries = 256;
    for (i = 0; i < 256; i++) {
        lp->palPalEntry[i].peRed   = g_bi.colors[i].rgbRed;
        lp->palPalEntry[i].peGreen = g_bi.colors[i].rgbGreen;
        lp->palPalEntry[i].peBlue  = g_bi.colors[i].rgbBlue;
        lp->palPalEntry[i].peFlags = 0;
    }

    g_palette = CreatePalette(lp);
    free(lp);
}

/* Ask the display what its 16bpp pixel actually looks like.
 *
 * It matters which one it is: a BI_RGB 16bpp DIB is 5:5:5 by Windows'
 * definition, while most 16bpp hardware of the era is 5:6:5, and guessing
 * wrong tints the whole screen or loses a bit of red.
 *
 * The documented way to ask is GetDIBits with biBitCount set to 0, which
 * makes the driver describe itself instead of converting anything. That was
 * the first version of this function and **it does not work on the target**:
 * the real machine logged `using the 24bpp path`, because the biBitCount-zero
 * query is a Win32 addition and Win32s' GDI is Win16 underneath. The fallback
 * did its job -- the screen was right and merely slow -- but the measurement
 * did not move, which is how we found out.
 *
 * So ask a question Windows 3.1 can answer. Store a known colour into a
 * device-compatible bitmap with SetPixel and read the raw device bits back
 * with GetBitmapBits: both are Win16 calls, and whatever bits come back for
 * pure red *are* the red mask, because a 5- or 6-bit channel asked for 255
 * saturates to all ones. Doing that for the three primaries derives the
 * layout with no assumption about 5:6:5, 5:5:5, or even RGB order -- a card
 * that puts blue at the top is handled without being enumerated.
 *
 * **This reports; it does not decide.** It works -- the machine logged
 * `rf800 g07e0 b001f`, correct 5:6:5 -- but knowing the hardware's layout
 * turned out not to answer the question that matters, which is which DIB
 * *GDI* will honour. Every attempt to establish that from in here failed, so
 * the format comes from `dib16` in mrpy.ini and this only logs what the
 * hardware is, for comparing against a screen that looks wrong.
 *
 * Returns 0 if the display will not answer or answers something pack.c
 * cannot express, in which case there is simply no such line in the log.
 */
static int probe_dib16(mr_pack_fmt *f, DWORD *masks)
{
    static const COLORREF probe[3] = {
        RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255)
    };
    HDC     screen, mem;
    HBITMAP bmp, old;
    int     ok = 0;
    int     i;
    WORD    bits[3];

    screen = GetDC(NULL);
    if (!screen)
        return 0;

    mem = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, 1, 1);

    if (mem && bmp) {
        for (i = 0; i < 3; i++) {
            bits[i] = 0;

            /* Selected to draw, deselected to read. GetBitmapBits requires
             * the bitmap *not* be selected into a DC -- while it is, the DC
             * owns its state and what comes back is undefined. The first
             * version of this held it selected throughout and read garbage
             * that was plausible enough to pass validation and be accepted,
             * which is how every colour on the screen came out wrong. */
            old = (HBITMAP)SelectObject(mem, bmp);
            SetPixel(mem, 0, 0, probe[i]);
            SelectObject(mem, old);

            /* Two bytes: one 16bpp pixel, and the start of the row whatever
             * the row padding is. */
            if (GetBitmapBits(bmp, 2, &bits[i]) != 2)
                break;
        }

        if (i == 3) {
            mr_log("present: 16bpp probe read r%04x g%04x b%04x",
                   (unsigned)bits[0], (unsigned)bits[1], (unsigned)bits[2]);

            /* The mask arithmetic, and the rejection of a layout pack.c
             * could not honour, is pack.c's -- so that it can be tested,
             * which nothing in this file can be. */
            ok = mr_pack_fmt_masks(f, 16, bits[0], bits[1], bits[2]);

            /* And a sanity check on top of it, because the arithmetic is
             * happy with any contiguous non-overlapping fields and a bad
             * read can still produce those. A 16bpp pixel has 5- or 6-bit
             * channels and nothing else; a 1-bit or 8-bit "channel" means
             * the probe did not measure what it thinks it did, and the
             * 24bpp fallback is always correct. */
            if (ok &&
                !(f->rsize >= 5 && f->rsize <= 6 &&
                  f->gsize >= 5 && f->gsize <= 6 &&
                  f->bsize >= 5 && f->bsize <= 6)) {
                mr_log("present: those are not 5- or 6-bit channels; "
                       "not believing the probe");
                ok = 0;
            }

            if (ok) {
                masks[0] = bits[0];
                masks[1] = bits[1];
                masks[2] = bits[2];
            }
        }
    }

    if (bmp)
        DeleteObject(bmp);
    if (mem)
        DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok;
}

int mr_present_init(void *oswindow)
{
    HDC screen;
    const char *force;
    DWORD masks[3];
    int   bitfields = 0;
    mr_pack_fmt probed;

    g_wnd = (HWND)oswindow;

    screen = GetDC(NULL);
    if (screen) {
        g_depth = GetDeviceCaps(screen, BITSPIXEL) *
                  GetDeviceCaps(screen, PLANES);
        ReleaseDC(NULL, screen);
    }
    if (g_depth <= 0)
        g_depth = 8;            /* assume the worst rather than the best */

    /* mrpy.ini's `bilinear`. Read once here rather than per resize so that
     * the setting cannot change under a running window, and because it is a
     * build-quality choice rather than something to toggle. Off by default:
     * it costs twelve multiplies an output pixel where nearest costs a load,
     * and on the Pentium III this pass is already the second most expensive
     * thing in a frame. */
    g_bilinear = (int)mr_config_int("bilinear", 0, 0, 1);

    /* The 8bpp path is hard to exercise on a modern desktop, so allow forcing
     * it. Setting MR_BPP=8 reproduces what a 256-colour Windows 3.1 sees. */
    force = getenv("MR_BPP");
    if (force && *force)
        g_depth = atoi(force);

    g_pal8 = (g_depth <= 8);

    /* Match the DIB to the display at 16bpp, so GDI copies instead of
     * converting.
     *
     * **Off unless mrpy.ini says `dib16=1`.** It defaulted on for exactly one
     * build and that build got every colour on the screen wrong -- video,
     * images, text, all of it -- because the probe below read a bitmap that
     * was still selected into a DC and believed the result. Nothing here can
     * be run on the build host, so the only evidence that it works is a run
     * on the machine, and until there is one the default has to be the
     * arrangement that has always been right. Flip this when a log line has
     * shown the probe reading sensible masks and the colours are correct. */
    g_dib_bpp = g_pal8 ? 8 : 24;

    memset(&g_bi, 0, sizeof(g_bi));
    g_bi.h.biSize = sizeof(BITMAPINFOHEADER);
    g_bi.h.biPlanes = 1;
    g_bi.h.biCompression = BI_RGB;

    /* `dib16` says which, and it is a choice rather than a detection.
     *
     *   0  off: the 24bpp DIB, whatever the display is   (default)
     *   1  5:6:5, declared BI_BITFIELDS
     *   2  5:5:5, declared BI_RGB
     *
     * It was a detection for four builds and none of them worked. The
     * display's own layout is readable -- SetPixel into a compatible bitmap
     * and GetBitmapBits reads it straight back, and it correctly reported
     * 5:6:5 -- but knowing the hardware's layout does not tell you which DIB
     * *GDI* will honour, and every attempt to establish that from inside the
     * program failed. GetDIBits' format query is Win32-only and declined. A
     * 1x1 SetDIBitsToDevice returns success and writes nothing, so the
     * round-trip read came back as the bitmap's uninitialised content --
     * (0,0,248) regardless of what was written, twice, which is what finally
     * made it obvious.
     *
     * The oracle for "does GDI honour this DIB" is the screen, and the person
     * looking at it. Two runs settle what four builds of probing did not, so
     * the probe is kept for its log line -- the hardware masks are worth
     * knowing and that part demonstrably works -- and the decision is the
     * config's. Preferring a correct manual answer to an automatic one that
     * cannot be verified from here.
     */
    if (!g_pal8 && g_depth == 16) {
        /* Default 2, on evidence rather than on it having compiled.
         *
         * Confirmed on the target with somebody looking at the screen: 2 is
         * correct and 1 is not, on a display whose own layout is 5:6:5. That
         * is the documented behaviour rather than a quirk -- 5:5:5 is what a
         * 16bpp BI_RGB DIB *is*, so it is the one 16bpp format a GDI that
         * supports them at all has to honour, and BI_BITFIELDS is the
         * optional part this driver declines. Measured there at `blit 28.28ms
         * -> 17.31ms`.
         *
         * Confirmed since on two unrelated drivers at 16bpp -- a Matrox
         * Mystique on the real Pentium III, and an S3 Trio under dosbox-x --
         * which is what moved this from "correct on the one machine we have"
         * to a default worth shipping. `dib16=0` is still one line away. 1 is
         * kept because a driver that does honour BI_BITFIELDS would get the
         * extra bit of green for free.
         *
         * That lost bit of green was then reported from the machine as the
         * 16bpp path showing visibly fewer colours than the 24bpp one, which
         * it did: 722 distinct colours against 1156, the whole difference
         * being green at step 8 instead of step 4. The answer was not to
         * change the format but to stop *truncating* when packing to it --
         * see the dither above pack16, which measures better than the 24bpp
         * path rather than merely closer to it.
         *
         * The `28.28ms -> 17.31ms` above is the **cutscene** blit, not the
         * stage one; reading it as the stage cost is misleading by a factor of
         * two and a half. Both are now in NOTES.md under "dib16". The stage
         * costs 6.56 against 11.26, so `dib16=0` is about 3.8ms a frame once
         * pack's own saving is netted off; a cutscene displays 71% of its
         * frames against 58%, which is the setting's real justification. */
        int want = (int)mr_config_int("dib16", 2, 0, 2);

        /* Logged whether or not it is acted on: it says what the hardware
         * is, which is the thing to compare a wrong-looking screen against. */
        if (want && probe_dib16(&probed, masks))
            mr_log("present: the display's own 16bpp layout is "
                   "r%d+%d g%d+%d b%d+%d",
                   probed.rpos, probed.rsize, probed.gpos, probed.gsize,
                   probed.bpos, probed.bsize);

        if (want == 1) {
            mr_pack_fmt_masks(&g_fmt, 16, 0xF800, 0x07E0, 0x001F);
            bitfields = 1;
        } else if (want == 2) {
            mr_pack_fmt_masks(&g_fmt, 16, 0x7C00, 0x03E0, 0x001F);
            bitfields = 0;
        }

        if (want) {
            g_dib_bpp = 16;
            if (bitfields) {
                g_bi.h.biCompression = BI_BITFIELDS;
                masks[0] = 0xF800; masks[1] = 0x07E0; masks[2] = 0x001F;
                memcpy(g_bi.colors, masks, 3 * sizeof(DWORD));
            }
            mr_log("present: display is %d bpp, asked for a 16bpp DIB "
                   "(r%d+%d g%d+%d b%d+%d, %s). If the colours are wrong, "
                   "try dib16=%d, or 0 for the 24bpp path",
                   g_depth, g_fmt.rpos, g_fmt.rsize, g_fmt.gpos, g_fmt.gsize,
                   g_fmt.bpos, g_fmt.bsize,
                   bitfields ? "BI_BITFIELDS 565" : "BI_RGB 555",
                   want == 1 ? 2 : 1);
        }
    }

    /* Ordered dither instead of truncating, for the 15/16bpp paths. On by
     * default: truncation loses up to 7 counts a channel, always downwards and
     * identically on every pixel, which is correlated error and reads as
     * banding -- measured at 3.91 against the dither's 0.69 on real
     * backgrounds (error after a blur, see pack.c). It is most of why the
     * dib16 path looked worse than the 24bpp one.
     *
     * The key exists because the screen is the only oracle for this file and
     * it has been wrong on real hardware before, not because there is a reason
     * to turn it off. It also has no effect at 24 or 32bpp, where nothing is
     * being quantised. */
    mr_pack_dither((int)mr_config_int("dither", 1, 0, 1));

    if (g_dib_bpp != 16) {
        mr_pack_fmt_std(&g_fmt, g_dib_bpp);
        mr_log("present: display is %d bpp, using the %s path",
               g_depth, g_pal8 ? "palettised 8bpp" : "24bpp");
    }

    g_bi.h.biBitCount = (WORD)g_dib_bpp;

    if (g_pal8) {
        g_bi.h.biClrUsed = 256;
        g_bi.h.biClrImportant = 256;
        build_palette();
    }
    return 1;
}

int mr_present_depth(void)
{
    return g_depth;
}

/* Select and realize our palette into a DC. Called from WM_PAINT and from the
 * palette messages; a background realization is what a window that does not
 * have the focus is allowed to do. */
void mr_present_realize(HDC hdc, int background)
{
    if (!g_palette || !hdc)
        return;
    SelectPalette(hdc, g_palette, background ? TRUE : FALSE);
    RealizePalette(hdc);
}

void mr_present_shutdown(void)
{
    mr_surface_free(g_buf);
    g_buf = NULL;
    free(g_out);
    g_out = NULL;
    if (g_palette) {
        DeleteObject(g_palette);
        g_palette = NULL;
    }
    mr_scale_free(&g_scaler);
    mr_yuv_free(&g_vscale);
    g_vready = 0;
}

void mr_present_resize(int client_w, int client_h)
{
    int w, h;

    /* g_buf is about to be freed and rebuilt at a new size, so the video
     * scaler that was built against its dimensions is stale. Dropping the
     * flag is what sends the next frame of a cutscene back through
     * video_setup -- which also repaints the video's own bars, since the new
     * buffer's contents are undefined. A cutscene can be running while this
     * happens: the player can resize the window during one. */
    g_vready = 0;
    g_vbars = 1;

    if (client_w < 16 || client_h < 16)
        return;

    /* The 16:9 rectangle and the letterbox around it are mr_fit_set's, not
     * ours: input.c maps clicks through the same numbers and there must be
     * exactly one set of them. */
    mr_fit_set(client_w, client_h);
    mr_fit_get(&g_dx, &g_dy, &g_dw, &g_dh, &g_cw, &g_ch);
    w = g_dw;
    h = g_dh;

    if (!g_buf || g_buf->w != w || g_buf->h != h) {
        mr_surface_free(g_buf);
        g_buf = mr_surface_new(w, h);
        if (!g_buf)
            mr_fatal("out of memory sizing the %dx%d presentation buffer", w, h);
    }

    mr_scale_free(&g_scaler);
    if (!mr_scale_init(&g_scaler, MR_STAGE_W, MR_STAGE_H, w, h, g_bilinear))
        mr_fatal("out of memory building scale tables");
    mr_log("present: %dx%d, %s", w, h,
           g_scaler.kind == MR_SCALE_COPY     ? "1:1" :
           g_scaler.kind == MR_SCALE_HALF     ? "2x2 box" :
           g_scaler.kind == MR_SCALE_BILINEAR ? "bilinear" : "nearest");

    /* DIB rows are DWORD aligned. */
    g_out_stride = (((long)w * (g_dib_bpp / 8) + 3) & ~3L);
    free(g_out);
    g_out = (mr_u8 *)malloc((size_t)(g_out_stride * h));
    if (!g_out)
        mr_fatal("out of memory sizing the %ldx%d output DIB", g_out_stride, h);

    g_bi.h.biWidth = w;
    /* Positive height: bottom-up DIB. Windows 3.1 GDI predates top-down DIBs,
     * and a negative biHeight there does not fail loudly -- it just draws
     * nothing. The scaling routines below compensate by writing their output
     * rows in reverse, which costs nothing extra. */
    g_bi.h.biHeight = h;
    /* 0 is the expected value for BI_RGB, where the size is implied by the
     * other fields. BI_BITFIELDS is documented to carry a real size, and a
     * zero there is the kind of thing a driver may read literally. */
    g_bi.h.biSizeImage = (g_bi.h.biCompression == BI_RGB)
                       ? 0 : (DWORD)(g_out_stride * h);
}

/* The two pack loops that were here -- 24bpp B,G,R and the dithered 8bpp
 * cube -- are mr_pack_rows in pack.c. They were the only arithmetic in this
 * file, and the DOS presenter needs the same thing at four more depths, so
 * they moved somewhere a harness can link. That also gave them their first
 * test coverage: this file, like main.c, is linked by no harness.
 *
 * The row range they take is in *buffer* rows, which run bottom-up: buffer
 * row j holds destination row h-1-j. Only the rows the scaler just wrote are
 * packed, and the row index reaches the dither, so a band must pass the same
 * y a full frame would or the pattern shifts at the band edge.
 *
 * Scale, pack and blit destination rows [r0, r1). Everything else in the
 * window keeps what was presented last time.
 *
 * The source rectangle is selected with xSrc/ySrc and the width and height
 * rather than with uStartScan, so the whole buffer is still described to GDI
 * and only the rows asked for are read out of it. ySrc counts from the bottom
 * because the DIB is bottom-up, which is also why the scalers write row
 * h-1-y: buffer row j is destination row h-1-j. */
static void present_rows(HDC hdc, const mr_surface *stage, int r0, int r1)
{
    DWORD t0 = 0;
    int h = g_buf->h;

    if (r0 < 0) r0 = 0;
    if (r1 > h) r1 = h;
    if (r0 >= r1)
        return;

    if (g_prof)
        t0 = timeGetTime();

    mr_scale_rows(&g_scaler, stage, g_buf, r0, r1);

    /* The stage covers the whole picture buffer, bars included. If a
     * cutscene is running -- and it can be: platw32.c's WM_PAINT calls
     * mr_present directly, without going through main.c's early return
     * -- then the video's own bars have just been painted over with
     * scenery, and the next video frame rewrites only the rectangle. */
    g_vbars = 1;

    if (g_prof) {
        g_prof_scale += (long)(timeGetTime() - t0);
        t0 = timeGetTime();
    }

    /* Realizing the palette is GDI's half of the 8bpp path and stays here;
     * the quantisation is pack.c's. */
    if (g_pal8)
        mr_present_realize(hdc, 1);
    mr_pack_rows(&g_fmt, g_buf, g_out, g_out_stride, h - r1, h - r0);

    if (g_prof) {
        g_prof_pack += (long)(timeGetTime() - t0);
        t0 = timeGetTime();
    }

    SetDIBitsToDevice(hdc,
        g_dx, g_dy + r0,                /* destination origin */
        (DWORD)g_buf->w, (DWORD)(r1 - r0),
        0, h - r1,                      /* source origin, from the bottom */
        0, (UINT)h,                     /* the whole buffer is described */
        g_out,
        (BITMAPINFO *)&g_bi,
        DIB_RGB_COLORS);

    if (g_prof)
        g_prof_gdi += (long)(timeGetTime() - t0);
}

/* Paint the letterbox bars. We claim WM_ERASEBKGND, so nothing else ever
 * clears them, and an unpainted bar shows whatever was on screen before.
 * PatBlt over the few strips outside a 16:9 rect is cheap enough to just do
 * unconditionally on a full present.
 *
 * Each bar is tested on its own extent: integer rounding can leave a
 * one-pixel strip on the bottom or right even when the opposite offset
 * rounded to zero. */
static void letterbox(HDC hdc)
{
    if (g_dy > 0)
        PatBlt(hdc, 0, 0, g_cw, g_dy, BLACKNESS);
    if (g_ch - (g_dy + g_dh) > 0)
        PatBlt(hdc, 0, g_dy + g_dh, g_cw, g_ch - (g_dy + g_dh), BLACKNESS);
    if (g_dx > 0)
        PatBlt(hdc, 0, g_dy, g_dx, g_dh, BLACKNESS);
    if (g_cw - (g_dx + g_dw) > 0)
        PatBlt(hdc, g_dx + g_dw, g_dy, g_cw - (g_dx + g_dw), g_dh, BLACKNESS);
}

/* --- video ----------------------------------------------------------------
 *
 * A frame of video does not come through the stage: it is colour-converted
 * and scaled straight into g_buf, the same buffer mr_present scales the stage
 * into, and then packed and handed to GDI by the same calls. present.h has
 * the reasoning, yuv.h the measurements. The short version is that the stage
 * is 1280x720 whatever the window is, and a cutscene cannot afford it.
 *
 * Kept deliberately parallel to presdos.c's copy of this. The two presenters
 * have always been written that way -- same names, same order -- so that a
 * change to one is visibly missing from the other.
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
    g_vx = ((g_buf->w - g_vw) / 2) & ~1;
    g_vy = ((g_buf->h - g_vh) / 2) & ~1;

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

    mr_log("present: video %dx%d at %d,%d in a %dx%d picture, %s%s",
           g_vw, g_vh, g_vx, g_vy, g_buf->w, g_buf->h,
           g_vscale.kind == MR_YUV_DIRECT ? "1:1" : "scaled",
           mr_yuv_simd() ? ", MMX" : "");
}

void mr_present_video(const mr_yuv_frame *frame)
{
    HDC   hdc;
    DWORD t0 = 0;
    int   h;

    if (!g_buf || !g_wnd || !frame || frame->w <= 0 || frame->h <= 0)
        return;

    video_setup(frame->w, frame->h);
    if (!g_vready)
        return;

    hdc = GetDC(g_wnd);
    if (!hdc)
        return;

    if (g_prof)
        t0 = timeGetTime();

    /* The stage's bars, outside the picture rect. Repainted every frame as
     * mr_present does, rather than once: we claim WM_ERASEBKGND, so nothing
     * else ever clears them, and four PatBlts over a few narrow strips is
     * cheap next to a frame of video. Doing it once would be correct until
     * something uncovered the window. */
    letterbox(hdc);

    h = g_buf->h;

    /* Convert straight into the DIB, skipping the picture buffer, where the
     * display's pixel is already the one the conversion produces.
     *
     * Less of a win here than on DOS and worth being clear about why: the DOS
     * presenter's pack writes into the framebuffer, so fusing removes a pass
     * over *uncached* memory, while here it removes a pass over ordinary RAM
     * and GDI still copies the DIB to the screen afterwards. What it saves is
     * one read and one write of the whole picture. It is done anyway, because
     * the alternative is the two ports taking different routes through the
     * same frame for no reason anybody could reconstruct later.
     *
     * Skipped when the bars are stale, as on DOS: blacking them is g_buf's
     * job, and a frame that needs it has to go the long way round. */
    if (!g_vbars && g_vscale.kind == MR_YUV_DIRECT &&
        mr_yuv_rows_fused(&g_vscale, frame, &g_fmt, g_out, g_out_stride,
                          g_buf->w, h, g_vx, g_vy, 0, g_vh)) {
        if (g_prof) {
            /* One column, as on DOS: pack reads zero on this path. */
            g_prof_scale += (long)(timeGetTime() - t0);
            t0 = timeGetTime();
        }
    } else {
        /* Repaint the video's bars if anything else has been into the buffer
         * since the last frame -- a resize, or the stage being presented over
         * the top of a running cutscene from WM_PAINT. Only the strips, and
         * only when they can actually be stale. */
        if (g_vbars) {
            mr_yuv_bars(g_buf, g_vx, g_vy, g_vw, g_vh);
            g_vbars = 0;
        }

        mr_yuv_rows(&g_vscale, frame, g_buf, g_vx, g_vy, 0, g_vh);

        if (g_prof) {
            /* The same step in the same place as the stage scaler, so it is
             * counted in the same column; main.c's profile line calls it
             * "scale". */
            g_prof_scale += (long)(timeGetTime() - t0);
            t0 = timeGetTime();
        }

        if (g_pal8)
            mr_present_realize(hdc, 1);
        mr_pack_rows(&g_fmt, g_buf, g_out, g_out_stride, 0, h);

        if (g_prof) {
            g_prof_pack += (long)(timeGetTime() - t0);
            t0 = timeGetTime();
        }
    }

    /* The stage's letterbox, outside the picture rect. Unlike mr_present this
     * is not repainted every frame -- the window cannot have been resized
     * without mr_present_resize running, which clears g_vready and brings us
     * back through video_setup. Nothing else writes outside g_buf while a
     * cutscene runs, because the cutscene is the only thing drawing. */
    SetDIBitsToDevice(hdc,
        g_dx, g_dy,
        (DWORD)g_buf->w, (DWORD)h,
        0, 0,
        0, (UINT)h,
        g_out,
        (BITMAPINFO *)&g_bi,
        DIB_RGB_COLORS);

    if (g_prof)
        g_prof_gdi += (long)(timeGetTime() - t0);

    ReleaseDC(g_wnd, hdc);
}

/* GetDC/ReleaseDC per present rather than one DC kept for the life of the
 * window. That is what the code did before the seam -- frame() acquired and
 * released around each call -- and it stays that way deliberately: a cached
 * DC held across a window style change is stale, and Windows 3.1's display
 * driver keeps a small fixed pool of them that a long-lived handle sits in.
 * The call is cheap next to a 1280x720 composite. */
void mr_present(const mr_surface *stage)
{
    HDC   hdc;
    DWORD t0 = 0;

    if (!g_buf || !g_wnd)
        return;

    hdc = GetDC(g_wnd);
    if (!hdc)
        return;

    if (g_prof)
        t0 = timeGetTime();

    letterbox(hdc);

    /* The letterbox PatBlts are GDI's work, not ours, so they are timed with
     * the blit rather than with the scaling. */
    if (g_prof)
        g_prof_gdi += (long)(timeGetTime() - t0);

    present_rows(hdc, stage, 0, g_buf->h);

    ReleaseDC(g_wnd, hdc);
}

void mr_present_band(const mr_surface *stage, int y, int h)
{
    HDC hdc;
    int r0, r1;

    if (!g_buf || !g_wnd || h <= 0)
        return;

    /* Stage rows to destination rows. Rounding outwards by one on each side
     * costs two rows and removes any question of a seam at the edge of the
     * band, whichever scaler is in use. */
    r0 = y * g_buf->h / MR_STAGE_H - 1;
    r1 = (y + h) * g_buf->h / MR_STAGE_H + 1;

    hdc = GetDC(g_wnd);
    if (!hdc)
        return;
    present_rows(hdc, stage, r0, r1);
    ReleaseDC(g_wnd, hdc);
}

/* mr_present_map_point was here. It is mr_fit_map_point in scale.c now: the
 * arithmetic is the same on any presenter, and leaving it here meant input.c
 * had to link GDI to turn a click into a stage coordinate.
 *
 * The guard it used to carry -- refuse while there is no presentation buffer
 * -- went with it, and is not missed. Before the first resize the fit is
 * zeroed and maps nothing; after mr_fatal there is no loop left to deliver a
 * click. */

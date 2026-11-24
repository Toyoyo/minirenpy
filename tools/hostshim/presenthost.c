/* presenthost.c - present.h's video entry point, for the host.
 *
 * Why this file exists at all: movie.c is portable engine code and lives in
 * tools/enginesrc.lst, so every harness that links the engine links it -- and
 * it calls mr_present_video, whose only two implementations are present.c
 * (HDCs and DIBs) and presdos.c (a VBE framebuffer). Neither can be built
 * here. Without this, adding video playback would have broken the link of
 * every full harness in the suite.
 *
 * It is the same seam as wavehost.c: a sink that satisfies the interface and
 * records enough to be asserted on. The pattern is deliberate -- the engine
 * must not grow an `#ifdef` for the harness, because then the harness is
 * testing something the target does not run.
 *
 * What it does *not* do is convert anything. mr_yuv_rows is pure and is
 * tested directly by yuvtest, which compares it against pl_mpeg's own
 * converter; repeating that work here would only give a second chance to be
 * wrong about it. What a harness wants from the presenter is the question
 * yuvtest cannot answer: did a frame get this far, how many, and were they
 * the size and at the moment the clock said they should be. So this counts
 * them and remembers the last one's shape.
 */

#include <string.h>

#include "mr.h"
#include "present.h"

static long g_frames;
static int  g_last_w, g_last_h;
static mr_u32 g_first_px;
static int  g_have_px;

void mr_present_video(const mr_yuv_frame *frame)
{
    if (!frame || frame->w <= 0 || frame->h <= 0)
        return;

    g_frames++;
    g_last_w = frame->w;
    g_last_h = frame->h;

    /* One converted pixel, so a harness can tell a decoded frame from a blank
     * one without linking a presenter. The top-left luma sample with its own
     * chroma, through the same arithmetic yuv.c uses -- which is pinned
     * against pl_mpeg by yuvtest, so this agrees with the real thing or that
     * test is already red. */
    if (frame->y && frame->cb && frame->cr) {
        int yy = (((int)frame->y[0] - 16) * 76309) >> 16;
        int icb = (int)frame->cb[0] - 128;
        int icr = (int)frame->cr[0] - 128;
        int r = yy + ((icr * 104597) >> 16);
        int g = yy - ((icb * 25674 + icr * 53278) >> 16);
        int b = yy + ((icb * 132201) >> 16);

        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;

        g_first_px = MR_ARGB(255, r, g, b);
        g_have_px = 1;
    }
}

/* mr_movie_close drains the presenter's profile accumulators, so the link
 * needs them here too. They report nothing: the figures they would carry are
 * milliseconds spent converting and blitting on the target, and this file
 * does neither. Returning zero is also what keeps mr_movie_close's log line
 * quiet in the suite's output, which is the behaviour that file documents --
 * it prints only when a real presenter has put something in them. */
void mr_present_profile_enable(int on)
{
    (void)on;
}

void mr_present_profile(long *scale_ms, long *pack_ms, long *blit_ms)
{
    if (scale_ms) *scale_ms = 0;
    if (pack_ms)  *pack_ms = 0;
    if (blit_ms)  *blit_ms = 0;
}

/* --- what a harness may ask ---------------------------------------------
 * Declared in tools/hostshim/presenthost.h, not in present.h: these do not
 * exist on the target and nothing in src/ may come to depend on them.
 */
long mr_hostpresent_video_frames(void)
{
    return g_frames;
}

void mr_hostpresent_video_last(int *w, int *h)
{
    if (w) *w = g_last_w;
    if (h) *h = g_last_h;
}

int mr_hostpresent_video_pixel(mr_u32 *px)
{
    if (px) *px = g_first_px;
    return g_have_px;
}

void mr_hostpresent_video_reset(void)
{
    g_frames = 0;
    g_last_w = g_last_h = 0;
    g_first_px = 0;
    g_have_px = 0;
}

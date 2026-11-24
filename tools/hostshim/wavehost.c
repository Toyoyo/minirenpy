/* wavehost.c - wave.h for the harnesses: a sound card a test can drive.
 *
 * This was tools/hostshim/mmsystem.h, a fake waveOut included into audio.c.
 * It is a linked implementation of the sink interface now, which is what
 * audio.c talks to since the DOS port split the device out -- and a better
 * arrangement than the header was: the two entry points below used to be
 * statics in whatever translation unit included them, so a harness could not
 * reach the copy the engine was using without a comment explaining why that
 * was safe.
 *
 * Lets the decode, resample and buffer-refill logic be driven with gcc. On
 * the target that behaviour is only verifiable by ear, and a resampling bug
 * on the five 48kHz tracks would be subtle rather than obvious.
 *
 * Two devices in one file, chosen at compile time.
 *
 *   default         consumes every buffer instantly. Nothing is ever
 *                   outstanding, so a harness can drive the stream as fast
 *                   as it likes by polling -- which is what every harness
 *                   but one is built on. The played position still trails
 *                   the written one by a fixed lag, because a card does.
 *
 *   HOSTSHIM_HOLD   keeps its buffers until the harness says they have
 *                   played, with hostshim_wave_play(). That makes the queue
 *                   depth observable, which is the only way to test
 *                   mr_audio_set_queue_ms at all.
 */

#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "wave.h"

/* The test installs this to receive every buffer the engine queues. Defined
 * by the harness, not here: every harness that links audio.c already defines
 * one, which is how it was when this was a header, and defining it here
 * would be a duplicate symbol in sixty of them. */
extern void (*hostshim_wave_sink)(const short *pcm, int samples);

static short *g_bufs[MR_WAVE_BUFS];
static int    g_busy[MR_WAVE_BUFS];
static int    g_open;
static long   g_written;        /* frames handed to the device */

/* How far the device lags what has been written to it. A real card holds
 * everything the mixer has queued -- the whole runway, over a second -- and
 * reports the position it is actually playing, well behind. Modelling that
 * is the only way the host can see a bug about the difference between the
 * two: the rhythm game's notes used to jump backwards by exactly this much,
 * once, the moment the card's position first became readable. */
#define LAG_FRAMES ((long)MR_WAVE_BUFS * MR_WAVE_FRAMES)

#ifdef HOSTSHIM_HOLD
static long   g_played;         /* frames the harness has let through */
static long   g_qend[MR_WAVE_BUFS];     /* play position each finishes at */
#endif

int mr_wave_open(void)
{
    int i;

    for (i = 0; i < MR_WAVE_BUFS; i++) {
        g_bufs[i] = (short *)malloc(MR_WAVE_BYTES);
        if (!g_bufs[i])
            return 0;
        memset(g_bufs[i], 0, MR_WAVE_BYTES);
        g_busy[i] = 0;
    }
    g_open = 1;
    g_written = 0;
    return 1;
}

void mr_wave_close(void)
{
    int i;

    for (i = 0; i < MR_WAVE_BUFS; i++) {
        free(g_bufs[i]);
        g_bufs[i] = NULL;
    }
    g_open = 0;
}

short *mr_wave_claim(void)
{
    int i;

    if (!g_open)
        return NULL;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (!g_busy[i])
            return g_bufs[i];
    return NULL;
}

int mr_wave_submit(short *buf)
{
    int i;

    if (!g_open)
        return 0;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_bufs[i] == buf)
            break;
    if (i == MR_WAVE_BUFS)
        return 0;

    if (hostshim_wave_sink)
        hostshim_wave_sink(buf, MR_WAVE_SAMPLES);

    g_written += MR_WAVE_FRAMES;

#ifdef HOSTSHIM_HOLD
    g_busy[i] = 1;
    g_qend[i] = g_written;
#else
    g_busy[i] = 0;              /* consumed instantly */
#endif
    return 1;
}

int mr_wave_outstanding(void)
{
    int i, n = 0;

    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_busy[i])
            n++;
    return n;
}

long mr_wave_played_frames(void)
{
    if (!g_open)
        return -1;
#ifdef HOSTSHIM_HOLD
    /* No modelling needed: the device really is behind by what it still
     * holds, so report what the harness has let through. */
    return g_played;
#else
    return g_written > LAG_FRAMES ? g_written - LAG_FRAMES : 0;
#endif
}

#ifdef HOSTSHIM_HOLD

/* Play `frames` of whatever is queued, releasing the buffers it finishes. */
void hostshim_wave_play(long frames)
{
    int i;

    g_played += frames;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_busy[i] && g_qend[i] <= g_played)
            g_busy[i] = 0;
}

/* Forget everything, for a harness that measures more than once. */
void hostshim_wave_reset(void)
{
    int i;

    for (i = 0; i < MR_WAVE_BUFS; i++)
        g_busy[i] = 0;
    g_played = 0;
    g_written = 0;
}

#endif /* HOSTSHIM_HOLD */

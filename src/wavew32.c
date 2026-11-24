/* wavew32.c - the Win32s sound sink: waveOut, polled.
 *
 * The implementation of wave.h for Windows 3.1x + Win32s. This was the
 * bottom of audio.c until the DOS port needed a second one; nothing in it
 * has changed in the move except that the buffers and their headers now
 * belong to this file rather than to the mixer.
 *
 * Win32s cannot be trusted to deliver waveOut completion notifications --
 * the w3vn port found callbacks unreliable -- so the device is opened with
 * CALLBACK_NULL and completion is discovered by polling WHDR_DONE. Nothing
 * here runs at interrupt time, which is also why there is no reentrancy to
 * think about on a cooperatively multitasked system.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <mmsystem.h>

#include "mr.h"
#include "wave.h"

static HWAVEOUT g_wave;
static WAVEHDR  g_hdr[MR_WAVE_BUFS];
static short   *g_bufs[MR_WAVE_BUFS];

int mr_wave_open(void)
{
    WAVEFORMATEX fmt;
    MMRESULT rc;
    int i;

    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = MR_WAVE_CH;
    fmt.nSamplesPerSec = MR_WAVE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(MR_WAVE_CH * 2);
    fmt.nAvgBytesPerSec = MR_WAVE_RATE * MR_WAVE_CH * 2;
    fmt.cbSize = 0;

    rc = waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (rc != MMSYSERR_NOERROR) {
        mr_log("audio: waveOutOpen failed (%d); running silently", (int)rc);
        g_wave = NULL;
        return 0;
    }

    for (i = 0; i < MR_WAVE_BUFS; i++) {
        g_bufs[i] = (short *)malloc(MR_WAVE_BYTES);
        if (!g_bufs[i]) {
            mr_log("audio: out of memory for mix buffers");
            return 0;
        }
        memset(g_bufs[i], 0, MR_WAVE_BYTES);

        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].lpData = (LPSTR)g_bufs[i];
        g_hdr[i].dwBufferLength = MR_WAVE_BYTES;

        if (waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR))
                != MMSYSERR_NOERROR) {
            mr_log("audio: waveOutPrepareHeader failed for buffer %d", i);
            return 0;
        }
        /* Marked finished, so the first poll fills and queues them. */
        g_hdr[i].dwFlags |= WHDR_DONE;
    }

    mr_log("audio: waveOut open at %dHz stereo, %d x %d frame buffers",
           MR_WAVE_RATE, MR_WAVE_BUFS, MR_WAVE_FRAMES);
    return 1;
}

void mr_wave_close(void)
{
    int i;

    if (g_wave) {
        waveOutReset(g_wave);
        for (i = 0; i < MR_WAVE_BUFS; i++) {
            if (g_hdr[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        }
        waveOutClose(g_wave);
        g_wave = NULL;
    }

    for (i = 0; i < MR_WAVE_BUFS; i++) {
        free(g_bufs[i]);
        g_bufs[i] = NULL;
    }
}

short *mr_wave_claim(void)
{
    int i;

    if (!g_wave)
        return NULL;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_hdr[i].dwFlags & WHDR_DONE)
            return g_bufs[i];
    return NULL;
}

int mr_wave_submit(short *buf)
{
    static int warned;
    MMRESULT rc;
    int i;

    if (!g_wave)
        return 0;

    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_bufs[i] == buf)
            break;
    if (i == MR_WAVE_BUFS)
        return 0;                       /* not one of ours */

    g_hdr[i].dwFlags &= ~WHDR_DONE;
    g_hdr[i].dwBufferLength = MR_WAVE_BYTES;

    rc = waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    if (rc != MMSYSERR_NOERROR) {
        if (!warned) {
            mr_log("audio: waveOutWrite failed (%d); sound will stutter",
                   (int)rc);
            warned = 1;
        }
        /* Put it back, so the next poll refills and retries rather than
         * losing the buffer for the rest of the session. */
        g_hdr[i].dwFlags |= WHDR_DONE;
        return 0;
    }
    return 1;
}

int mr_wave_outstanding(void)
{
    int i, n = 0;

    if (!g_wave)
        return 0;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (!(g_hdr[i].dwFlags & WHDR_DONE))
            n++;
    return n;
}

long mr_wave_played_frames(void)
{
    MMRESULT rc;
    MMTIME   t;

    if (!g_wave)
        return -1;

    /* TIME_BYTES rather than TIME_SAMPLES: what a driver counts as a
     * "sample" on a stereo device was never consistent across the Windows
     * 3.1 sound drivers, and bytes are unambiguous. The device is opened
     * once and only reset at shutdown, so this counter runs monotonically
     * for the whole session and needs no rebasing. */
    memset(&t, 0, sizeof(t));
    t.wType = TIME_BYTES;
    rc = waveOutGetPosition(g_wave, &t, sizeof(t));
    if (rc != MMSYSERR_NOERROR || t.wType != TIME_BYTES)
        return -1;

    return (long)(t.u.cb / (MR_WAVE_CH * 2));
}

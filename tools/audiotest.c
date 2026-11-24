/* audiotest.c - drive the real audio.c on the build host and dump what it
 * would have sent to the sound card.
 *
 * The point is to check the parts that are easy to get subtly wrong and hard
 * to notice by ear: the 48kHz-to-44.1kHz resample (five of the game's tracks),
 * mono-to-stereo duplication (one MP3 and all 36 effects), and the buffer
 * refill loop. The output WAV can be compared against ffmpeg's decode of the
 * same file for duration and level.
 *
 *   gcc -I src -I tools/hostshim -o tools/audiotest \
 *       tools/audiotest.c src/audio.c src/vfs.c src/mp3impl.c -lm
 *   ./tools/audiotest game audio/miwa-chichi_no_ito.mp3 out.wav 5
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

#include "mr.h"
#include "audio.h"
#include "vfs.h"

void (*hostshim_wave_sink)(const short *pcm, int samples);

void mr_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static FILE   *g_out;
static long    g_total_samples;
static double  g_sumsq;
static long    g_peak;

static void sink(const short *pcm, int samples)
{
    int i;

    for (i = 0; i < samples; i++) {
        long v = pcm[i];
        g_sumsq += (double)v * (double)v;
        if (v < 0) v = -v;
        if (v > g_peak)
            g_peak = v;
    }
    fwrite(pcm, 2, (size_t)samples, g_out);
    g_total_samples += samples;
}

static void put32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

int main(int argc, char **argv)
{
    const char *dir, *track, *outpath;
    double seconds;
    long want_samples;
    long data_bytes;

    if (argc < 4) {
        fprintf(stderr,
                "usage: audiotest <game-dir> <track> <out.wav> [seconds]\n");
        return 2;
    }
    dir = argv[1];
    track = argv[2];
    outpath = argv[3];
    seconds = (argc > 4) ? atof(argv[4]) : 5.0;

    if (!mr_vfs_init("mrpy.pak", dir)) {
        fprintf(stderr, "could not mount data\n");
        return 1;
    }

    g_out = fopen(outpath, "wb");
    if (!g_out) {
        perror(outpath);
        return 1;
    }
    /* Reserve the header; filled in once the length is known. */
    fwrite("\0", 1, 44, g_out);

    if (!mr_audio_init(dir)) {
        fprintf(stderr, "audio init failed\n");
        return 1;
    }

    hostshim_wave_sink = sink;
    mr_audio_play(MR_CH_MUSIC, track, 0, 0, 1.0f);

    want_samples = (long)(seconds * 44100.0) * 2;
    while (g_total_samples < want_samples) {
        long before = g_total_samples;
        mr_audio_poll();
        if (g_total_samples == before)
            break;                      /* stream ended */
    }

    data_bytes = g_total_samples * 2;

    rewind(g_out);
    fwrite("RIFF", 1, 4, g_out);
    put32(g_out, (unsigned long)(36 + data_bytes));
    fwrite("WAVEfmt ", 1, 8, g_out);
    put32(g_out, 16);
    put16(g_out, 1);
    put16(g_out, 2);
    put32(g_out, 44100);
    put32(g_out, 44100 * 4);
    put16(g_out, 4);
    put16(g_out, 16);
    fwrite("data", 1, 4, g_out);
    put32(g_out, (unsigned long)data_bytes);
    fclose(g_out);

    printf("%s -> %s\n", track, outpath);
    printf("  frames %ld, %.3f s at 44100Hz stereo\n",
           g_total_samples / 2, (double)(g_total_samples / 2) / 44100.0);
    printf("  peak %ld (%.1f%% FS), rms %.1f\n",
           g_peak, g_peak * 100.0 / 32767.0,
           g_total_samples ? sqrt(g_sumsq / (double)g_total_samples) : 0.0);

    mr_audio_shutdown();
    mr_vfs_shutdown();
    return 0;
}

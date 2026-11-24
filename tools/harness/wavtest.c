/* The RIFF/WAVE reader in audio.c, driven with real and hostile files.
 *
 * The rhythm game is the first thing in this engine to play WAVs in anger --
 * four hit sounds per song, one per lane -- so the loader stopped being a
 * path that only ever saw files we shipped. Two of the checks below are for
 * bugs that only bite on the target:
 *
 *  - a chunk length is an unsigned 32-bit field, and `long` is 32 bits under
 *    Watcom. Read into a signed long, a length with the top bit set comes out
 *    negative and the chunk walk steps backwards forever. On a 64-bit host
 *    the same bytes stay positive and the loop terminates, so this hangs only
 *    on Win32s. It is checked here because no amount of playing the game on a
 *    host would ever show it.
 *
 *  - the decoded buffer is malloc'd from a length field in the file. Bounded
 *    now by MR_WAV_MAX_BYTES; a header claiming 64MB used to be honoured.
 *
 * The synthetic files are written into the directory given as argv[1] (a
 * scratch directory) and the VFS is pointed at it, so nothing here depends on
 * the pak.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "vfs.h"
#include "audio.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }

static long g_frames;
static int  g_peak;

static void sink(const short *d, int n)
{
    int i;
    g_frames += n / 2;
    for (i = 0; i < n; i++) {
        int v = d[i] < 0 ? -d[i] : d[i];
        if (v > g_peak)
            g_peak = v;
    }
}
void (*hostshim_wave_sink)(const short *, int) = sink;

static int fails;

static void check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void run_ms(long ms)
{
    long want = g_frames + ms * 44100 / 1000;
    int guard = 0;
    while (g_frames < want && guard++ < 100000)
        mr_audio_poll();
}

/* --- synthetic WAVs ------------------------------------------------------ */

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

/* 16-bit mono 44100Hz. `declared` is what the data chunk *claims* its length
 * is; `actual` is how many bytes are really written after it. They differ
 * only for the hostile cases. */
static void write_wav(const char *path, unsigned long declared, long actual)
{
    FILE *f = fopen(path, "wb");
    long i;

    if (!f) {
        printf("cannot write %s\n", path);
        exit(1);
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + (unsigned long)actual);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    put32(f, 16);
    put16(f, 1);            /* PCM */
    put16(f, 1);            /* mono */
    put32(f, 44100);
    put32(f, 44100 * 2);    /* byte rate */
    put16(f, 2);            /* block align */
    put16(f, 16);           /* bits */

    fwrite("data", 1, 4, f);
    put32(f, declared);
    /* A square wave, so the sink sees something loud and unambiguous. */
    for (i = 0; i < actual / 2; i++)
        put16(f, (i / 50) % 2 ? 0x2000 : 0xE000);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char path[512];
    char rel[128];
    long dur;

    if (!mr_vfs_init("no-such.pak", dir)) {
        printf("vfs init failed on '%s'\n", dir);
        return 1;
    }
    if (!mr_audio_init(dir)) {
        printf("audio init failed\n");
        return 1;
    }

    /* --- a well-formed file ---------------------------------------------- */
    /* 22050 frames = 500ms at 44100. */
    sprintf(path, "%s/good.wav", dir);
    write_wav(path, 22050 * 2, 22050 * 2);
    strcpy(rel, "good.wav");

    g_peak = 0;
    mr_audio_play(MR_CH_SOUND, rel, 0, 0, 1.0f);
    dur = mr_audio_duration_ms(MR_CH_SOUND);
    check("a well-formed WAV reports its duration",
          dur > 480 && dur < 520);
    run_ms(200);
    check("  and it is audible", g_peak > 1000);
    mr_audio_stop(MR_CH_SOUND, 0);

    /* --- data chunk longer than the file --------------------------------- */
    /* Claims 64MB, carries 1000 bytes. The claim must be clamped to what is
     * actually there rather than read off the end of the buffer. */
    sprintf(path, "%s/over.wav", dir);
    write_wav(path, 64UL * 1024 * 1024, 1000);
    strcpy(rel, "over.wav");

    g_peak = 0;
    mr_audio_play(MR_CH_SOUND, rel, 0, 0, 1.0f);
    dur = mr_audio_duration_ms(MR_CH_SOUND);
    check("a data chunk longer than the file is clamped",
          dur > 0 && dur < 50);
    run_ms(50);
    check("  and it still plays", g_peak > 1000);
    mr_audio_stop(MR_CH_SOUND, 0);

    /* --- chunk length with the top bit set -------------------------------- */
    /* 0x80000000 is negative in a signed 32-bit long. If the walk reads it
     * that way it steps backwards and never terminates: reaching the next
     * line at all is most of this check. */
    sprintf(path, "%s/neg.wav", dir);
    write_wav(path, 0x80000000UL, 1000);
    strcpy(rel, "neg.wav");

    mr_audio_play(MR_CH_SOUND, rel, 0, 0, 1.0f);
    check("a chunk length with the top bit set terminates", 1);
    mr_audio_stop(MR_CH_SOUND, 0);

    /* --- over the decode cap ---------------------------------------------- */
    /* MR_WAV_MAX_BYTES is 500KB of 16-bit stereo = 128000 frames = 2.9s.
     * 200000 frames of mono is comfortably past it and must come back
     * truncated rather than allocating what the header asks for. */
    sprintf(path, "%s/huge.wav", dir);
    write_wav(path, 200000UL * 2, 200000L * 2);
    strcpy(rel, "huge.wav");

    g_peak = 0;
    mr_audio_play(MR_CH_SOUND, rel, 0, 0, 1.0f);
    dur = mr_audio_duration_ms(MR_CH_SOUND);
    check("a WAV over the decode cap is truncated to it",
          dur > 2800 && dur < 3000);
    run_ms(200);
    check("  and the part that was kept plays", g_peak > 1000);
    mr_audio_stop(MR_CH_SOUND, 0);

    /* --- the game's own hit sounds ---------------------------------------- */
    /* These are what the rhythm game actually loads: 16-bit mono, which the
     * loader has to widen to stereo. Skipped when the harness is pointed at a
     * scratch directory with no game data in it. */
    {
        mr_vfs_shutdown();
        if (argc > 2 && mr_vfs_init("no-such.pak", argv[2])) {
            g_peak = 0;
            mr_audio_play(MR_CH_SOUND,
                          "audio/miwa-chichi_no_ito.hit0.wav", 0, 0, 1.0f);
            dur = mr_audio_duration_ms(MR_CH_SOUND);
            check("a real hit sound loads and is ~463ms",
                  dur > 440 && dur < 490);
            run_ms(200);
            check("  and it is audible", g_peak > 1000);
            mr_audio_stop(MR_CH_SOUND, 0);
        }
    }

    mr_audio_shutdown();
    mr_vfs_shutdown();

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

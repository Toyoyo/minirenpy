/* Track lengths: mr_audio_duration_ms against the truth, for every
 * combination the game ships.
 *
 * The duration used to come from the file size and the bitrate of the first
 * frame, which is only right for a constant-bitrate file. Nine of this game's
 * MP3s are VBR, and a VBR encoder puts a quiet low-bitrate frame first, so
 * the engine read 400s for a 192s song. Four of those nine are rhythm-game
 * tracks, where the duration is not cosmetic: rhythm.c ends the run at
 * `t >= duration_ms` and fills the progress bar against it, so the summary
 * arrived minutes after the music stopped and the bar reached half.
 *
 * The expected values below are ffmpeg's (`ffprobe -show_entries
 * format=duration`), rounded to the millisecond, and were taken
 * independently of the engine -- the point is a second opinion, not a
 * restatement of what the code computes. Tolerance is 50ms: a frame is 24-26ms
 * and the Xing count excludes its own frame, so agreement to better than two
 * frames is all that is being claimed.
 *
 * Covered deliberately: 44.1kHz stereo and 48kHz joint stereo, CBR and VBR of
 * each, a mono file, and the short effects where an absolute error of a few
 * seconds would still be a large fraction of the length.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mr.h"
#include "vfs.h"
#include "audio.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }

static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static int fails;

#define MAXTRACKS 32

typedef struct {
    char path[96];
    long ms;               /* what the encoder was asked for */
    char what[32];         /* rate, channels, bitrate mode -- for the output */
} track;

static track TRACKS[MAXTRACKS];
static int   ntracks;

/* Read audio/durations.lst out of the mounted archive.
 *
 * That file is generated beside the tracks by tools/mkfixaudio.py from
 * tools/harness/fixaudio.lst, so the lengths travel inside the pak with the
 * files they describe and a regenerated fixture cannot be measured against
 * a stale table. Lines are "name<TAB>ms<TAB>rate<TAB>channels<TAB>mode".
 *
 * Returns 0 if the archive has no such file, which is what any real game's
 * pak looks like: see the note in main.
 */
static int load_matrix(void)
{
    long size = 0;
    char *text = mr_vfs_read_all("audio/durations.lst", &size);
    char *p;

    if (!text)
        return 0;

    for (p = text; *p && ntracks < MAXTRACKS; ) {
        char *eol = strchr(p, '\n');
        char name[96], mode[16];
        long ms;
        int rate, chans;

        if (eol)
            *eol = '\0';

        if (*p != '#' && *p != '\0' &&
            sscanf(p, "%95s %ld %d %d %15s",
                   name, &ms, &rate, &chans, mode) == 5) {
            track *t = &TRACKS[ntracks++];

            sprintf(t->path, "audio/%s", name);
            t->ms = ms;
            sprintf(t->what, "%dk %s %s", rate / 1000,
                    chans == 1 ? "mono" : "stereo", mode);
        }

        if (!eol)
            break;
        p = eol + 1;
    }

    free(text);
    return ntracks;
}

#define NTRACKS ntracks

#define TOL 50

static void check(const char *label, int ok)
{
    printf("%-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    int i;
    long cbr_seen = 0, vbr_seen = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: durtest <pak> <game-dir>\n");
        return 2;
    }

    setbuf(stdout, NULL);
    mr_vfs_init(argv[1], argv[2]);
    if (!mr_audio_init(argv[2])) {
        printf("no audio device\n");
        return 1;
    }

    /* The matrix comes out of the archive, so this test runs against the
     * fixture pak and not against a particular game. Handed a real game's
     * pak it finds no durations.lst and says so rather than asserting
     * about tracks that are not there. */
    if (!load_matrix()) {
        printf("no audio/durations.lst in this archive -- this test wants "
               "the fixture\npak, built by `tools/harness/mkfixpak.sh`. "
               "Nothing checked.\n");
        return 1;
    }
    printf("%d tracks from audio/durations.lst\n\n", ntracks);

    for (i = 0; i < NTRACKS; i++) {
        long got, err;
        int guard = 0;
        char label[160];

        /* The duration is only known once the decoder has looked at the head
         * of the file, which happens on the first poll that needs audio --
         * the same "the frame after it started" the cutscene code relies on
         * (interp.c:2684).
         *
         * Looping, which has nothing to do with the length and everything to
         * do with being able to read it: the host shim consumes every buffer
         * the instant it is queued, so a 400ms effect is decoded to the end
         * by the first poll and mr_audio_duration_ms then reports 0 for an
         * inactive channel. `fx photo` and `fx corde` failed that way, which
         * is a property of the shim and not of the engine. */
        mr_audio_play(MR_CH_MUSIC, TRACKS[i].path, 1, 0, 1.0f);
        while (!(got = mr_audio_duration_ms(MR_CH_MUSIC)) && guard++ < 1000)
            mr_audio_poll();

        err = got - TRACKS[i].ms;
        if (err < 0)
            err = -err;

        sprintf(label, "%-38s %-26s %ldms",
                TRACKS[i].path + 6, TRACKS[i].what, got);
        check(label, err <= TOL);
        if (err > TOL)
            printf("    expected %ldms, off by %+ld\n",
                   TRACKS[i].ms, got - TRACKS[i].ms);

        if (strstr(TRACKS[i].what, "vbr"))
            vbr_seen++;
        else
            cbr_seen++;

        mr_audio_stop(MR_CH_MUSIC, 0.0f);
        mr_audio_poll();
    }

    /* The table is the test, so assert it still spans what it claims to: a
     * suite that quietly lost its VBR rows would pass against the old bug. */
    check("the table covers both bitrate modes", vbr_seen >= 3 && cbr_seen >= 3);

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

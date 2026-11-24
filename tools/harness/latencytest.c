/* How long after it is started does a sound become audible?
 *
 * One waveOut stream software-mixed is the only option on Windows 3.1 -- the
 * device cannot be opened twice -- so every sound goes through the same
 * queue, and nothing can be heard until everything already handed to the
 * driver has played. At the default depth, twenty-four 2048-frame buffers,
 * that is 1114ms. The rhythm game's hit window is 300ms, so its hit sounds
 * arrived several windows late and the player noticed before any test did.
 *
 * mr_audio_set_queue_ms caps the outstanding audio, which is the latency.
 * This measures it the way the player experiences it: start a sound into a
 * silent stream and find how much audio plays before the first non-zero
 * sample reaches the device.
 *
 * Needs the shim's holding mode. With the default instant-consume device
 * nothing is ever outstanding, the cap never engages, and this measures 0
 * whatever the engine does -- which is exactly why the bug could ship.
 *
 * Being the only harness with a device that holds anything, it also carries
 * the claim about the *end* of a pushed stream: that a cutscene's audio is
 * not called finished until the card has played it, rather than until the
 * mixer has produced it. Same runway, far end, and no other harness can see
 * the difference. See the last block in main.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "vfs.h"
#include "audio.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }

/* The shim's holding device, compiled into audio.c's translation unit by
 * -DHOSTSHIM_HOLD (see run.sh). */
extern void hostshim_wave_play(long frames);
extern void hostshim_wave_reset(void);

/* Where, in frames since the stream began, the first audible sample sits. */
static long g_written_frames;
static long g_first_sound = -1;

static void sink(const short *d, int n)
{
    int i;

    /* Any non-zero sample at all: nothing else is playing, so the first one
     * is the hit sound arriving. */
    if (g_first_sound < 0) {
        for (i = 0; i < n; i++) {
            if (d[i]) {
                g_first_sound = g_written_frames + i / 2;
                break;
            }
        }
    }
    g_written_frames += n / 2;
}
void (*hostshim_wave_sink)(const short *, int) = sink;

static int fails;

static void check(const char *label, int ok)
{
    printf("%-54s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* A fixture WAV, not one of the game's hit sounds. The test wants a short
 * percussive sample on the WAV path -- loaded whole rather than streamed --
 * and this is a 250ms decaying tone from tools/mkfixaudio.py. */
#define HIT "audio/fix-hit.wav"

/* The hit sound's own leading silence, in frames, read from the file.
 *
 * It is not zero -- every hit WAV this game ships opens with between 50 and
 * 88ms of digital silence -- and the player hears that added to the mixer's
 * queue. Subtracted here so this harness measures the engine and not the
 * asset, and read from the file rather than written down so that trimming
 * the assets does not silently invalidate the numbers below.
 */
static long wav_onset_frames(const char *path)
{
    static unsigned char buf[262144];
    mr_file *f;
    long n, i, data = -1, len = 0;
    int ch = 1;

    f = mr_vfs_open(path);
    if (!f)
        return -1;
    n = mr_vfs_read(f, buf, (long)sizeof(buf));
    mr_vfs_close(f);
    if (n < 44)
        return -1;

    for (i = 12; i + 8 <= n; ) {
        long sz = (long)((unsigned long)buf[i + 4] |
                         ((unsigned long)buf[i + 5] << 8) |
                         ((unsigned long)buf[i + 6] << 16) |
                         ((unsigned long)buf[i + 7] << 24));
        if (!memcmp(buf + i, "fmt ", 4))
            ch = buf[i + 10] | (buf[i + 11] << 8);
        if (!memcmp(buf + i, "data", 4)) {
            data = i + 8;
            len = sz;
            break;
        }
        i += 8 + sz + (sz & 1);
    }
    if (data < 0 || ch < 1)
        return -1;
    if (len > n - data)
        len = n - data;

    for (i = 0; i + 1 < len; i += 2)
        if (buf[data + i] || buf[data + i + 1])
            return (i / 2) / ch;
    return -1;
}

static long g_onset;

/* Fill the queue with silence, start the hit sound, and return how many
 * milliseconds of audio precede it. */
static long measure(long cap_ms)
{
    long i;

    hostshim_wave_reset();
    g_written_frames = 0;
    g_first_sound = -1;

    if (!mr_audio_init("."))
        return -1;

    mr_audio_set_queue_ms(cap_ms);

    /* Nothing playing, so the queue fills with silence. Each pass releases
     * one buffer's worth and polls, which is a frame loop in miniature. */
    for (i = 0; i < 40; i++) {
        mr_audio_poll();
        hostshim_wave_play(2048);
    }

    /* The keypress. Everything queued above it has to play first. */
    mr_audio_play(MR_CH_HIT0, HIT, 0, 0, 1.0f);

    for (i = 0; i < 80 && g_first_sound < 0; i++) {
        mr_audio_poll();
        hostshim_wave_play(2048);
    }

    if (g_first_sound < 0) {
        mr_audio_shutdown();
        return -1;
    }

    /* Measured from where the player's ear was at the instant of the call:
     * the device had played exactly 40 buffers by then, so everything in the
     * stream beyond that mark is what it had to wait through. */
    {
        long lat = (g_first_sound - 40 * 2048 - g_onset) * 1000 / 44100;
        mr_audio_shutdown();
        return lat;
    }
}

int main(int argc, char **argv)
{
    long deep, shallow;

    if (argc < 3) {
        fprintf(stderr, "usage: latencytest <pak> <game-dir>\n");
        return 2;
    }

    setbuf(stdout, NULL);
    mr_vfs_init(argv[1], argv[2]);

    g_onset = wav_onset_frames(HIT);
    printf("   (%s opens with %ldms of silence of its own)\n",
           HIT, g_onset * 1000 / 44100);
    check("the hit sound's own onset was found", g_onset >= 0);
    if (g_onset < 0)
        return 1;

    /* --- the default depth, which is what the complaint was about --- */
    deep = measure(0);
    printf("   (uncapped: a sound is audible %ldms after it is started)\n",
           deep);
    /* Measured against what the engine claims, which is not circular here:
     * these were once wildly apart -- mr_audio_queue_ms reported 185 while a
     * sound took 696ms to be heard, because the cap was bookkeeping that
     * nothing enforced. One buffer of slack for where the sound lands inside
     * the buffer being filled. */
    check("uncapped, the measured latency is the runway the engine reports",
          deep > mr_audio_max_queue_ms() - 100 &&
          deep < mr_audio_max_queue_ms() + 100);
    check("uncapped, that runway is over a second",
          deep > 1000);

    /* --- capped, as the rhythm game asks for --- */
    shallow = measure(140);
    printf("   (capped at 140ms: %ldms, queue reports %ldms)\n",
           shallow, mr_audio_queue_ms());
    check("capped, a sound is audible inside 200ms",
          shallow > 0 && shallow < 200);
    check("and that is a real improvement, not a rounding one",
          deep - shallow > 400);

    /* The cap is in whole buffers, rounded up: 140ms is four of them. A
     * request finer than a buffer cannot be honoured and must not pretend. */
    check("the cap rounds up to whole buffers", mr_audio_queue_ms() == 185);

    /* Two buffers is the floor: one playing, one being filled. Asking for
     * less must not produce a queue that cannot be kept full. */
    mr_audio_set_queue_ms(1);
    check("a silly request is floored at two buffers",
          mr_audio_queue_ms() == 92);

    /* Two claims, and they need separate assertions. That zero restores the
     * full depth is checked against the engine's own figure, so it survives a
     * change to NBUF -- but on its own it is vacuous, since it would hold for
     * any runway at all, including a useless one. The magnitude is the second
     * claim: the deep default exists to ride out a burst of slow frames, and
     * under a second does not. */
    mr_audio_set_queue_ms(0);
    check("zero restores the deep default",
          mr_audio_queue_ms() == mr_audio_max_queue_ms());
    check("the deep default is deep enough to ride out slow frames",
          mr_audio_queue_ms() >= 1000 && mr_audio_queue_ms() <= 2000);

    /* --- the other end of the same runway ------------------------------
     *
     * A cutscene's audio is *pushed*: movie.c decodes it and says
     * mr_audio_push_end when the file runs out, and asks mr_audio_active
     * whether the sound has finished. Whether the channel answers that
     * honestly is the same question this harness exists for, from the far
     * end -- the mixer is a whole queue ahead of the speaker, so the instant
     * the ring runs dry there is still up to a runway of already-mixed
     * cutscene audio in front of the card.
     *
     * It used to answer "finished" there, and movie.c believed it: the
     * cutscene ended, interp.c composited the stage over the last picture,
     * and the tail was cut. Reported from a real run as the last ~0.5s of an
     * eleven-second video missing in both picture and sound, which is the
     * queue depth to the eye.
     *
     * This is the only harness that can see it. Everywhere else the shim's
     * device consumes a buffer the instant it is queued, so "mixed" and
     * "played" are the same moment and the bug is invisible -- the same
     * reason the latency it measures above could ship.
     */
    {
        static short tone[1152 * 2];
        long pushed = 0, released = 0, polls = 0;
        long active_at = -1;
        int i;

        hostshim_wave_reset();
        g_written_frames = 0;
        g_first_sound = -1;

        for (i = 0; i < 1152; i++) {
            tone[i * 2]     = (short)((i & 32) ? 8000 : -8000);
            tone[i * 2 + 1] = tone[i * 2];
        }

        if (!mr_audio_init(".")) {
            check("the shim's device opens for the push claim", 0);
        } else {
            /* movie.c's own depth, so the figures are the cutscene's. */
            mr_audio_set_queue_ms(417);

            mr_audio_push_open(MR_CH_MOVIE, 44100, 1.0f);

            /* Half a second of audio and then end of stream: less than the
             * runway, which is the case that was broken -- the whole stream
             * is still unplayed when the ring empties. */
            /* Kept as full as the ring will hold, the way feed_audio does
             * it: a ring allowed to run dry between pushes puts starvation
             * silence into the queue, and then the drain below is waiting
             * for the silence as well as for the sound. */
            while (pushed < 22050) {
                while (mr_audio_push_space(MR_CH_MOVIE) >= 1152) {
                    mr_audio_push(MR_CH_MOVIE, tone, 1152);
                    pushed += 1152;
                }
                mr_audio_poll();
                /* The card has to keep playing or the queue never frees a
                 * buffer and nothing more can be mixed -- this is the frame
                 * loop and the speaker, not just the frame loop. */
                hostshim_wave_play(2048);
                released += 2048;
            }
            mr_audio_push_end(MR_CH_MOVIE);

            /* Mix without letting the device play anything: this is a frame
             * loop during a cutscene whose sound is already decoded. */
            for (i = 0; i < 40; i++)
                mr_audio_poll();

            check("a finished push is not 'finished' before the card has"
                  " played it", mr_audio_active(MR_CH_MOVIE) != 0);

            /* Now let the card run, a buffer at a time, until the channel
             * admits it is done. */
            for (polls = 0; polls < 200; polls++) {
                if (!mr_audio_active(MR_CH_MOVIE)) {
                    active_at = released;
                    break;
                }
                mr_audio_poll();
                hostshim_wave_play(2048);
                released += 2048;
            }

            printf("   (pushed %ld frames; the channel went inactive after"
                   " the device had played %ld)\n", pushed, active_at);

            check("and it ends once the card has played what was pushed",
                  active_at >= pushed - 2048);

            /* Bounded, but not tightly, and the slack is inherent rather
             * than sloppiness: the drain is noticed when the *ring* empties,
             * and by then the last sound mixed is a queue's depth from the
             * speaker. Anything the mixer put in behind it -- starvation
             * silence from a ring that was not refilled in time -- is in
             * front of that mark and gets played too. One runway of slack,
             * which still catches the failure that matters here: a wait that
             * never ends, or one measured against the wrong counter. */
            check("and not more than a runway after it",
                  active_at >= 0 &&
                  active_at < pushed + 417 * 44 + 2 * 2048);

            mr_audio_shutdown();
        }
    }

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

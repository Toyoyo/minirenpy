/* `stop music fadeout` followed by `play music` of the same track.
 *
 * The script does this at every chapter break:
 *
 *     stop music fadeout 1.0
 *     ... title card, renpy.pause() ...
 *     play music "audio/joyful-sakura_no_ato.mp3" fadein 1.0 fadeout 1.0 loop
 *
 * and chap3_5 plays back the same file the previous chapter was playing.
 *
 * Ren'Py's play() defaults if_changed to false: it fades out whatever is on
 * the channel and enqueues the new track, so the same file does stop and
 * start again. The main menu is the one caller that asks for if_changed, so
 * that returning to it does not restart music already looping. Both are
 * checked here.
 *
 * Timing here is in samples rather than wall clock: the host shim consumes
 * every buffer the moment it is queued, so polling advances the stream.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "vfs.h"
#include "audio.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }

static long g_frames;
static int  g_peak;          /* loudest sample since the last reset */

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

/* A 60-second excerpt of real music, committed to the fixture rather than
 * generated -- the only asset in tools/harness/fixgame/ that is not.
 *
 * It has to be real. "the fade-out was allowed to finish" below looks for
 * the output dipping to near silence (`low < 200`) in the 1600ms around a
 * replay that arrives mid-fade. Two linear fades crossing over a
 * constant-amplitude signal sum to that same constant -- A*(1-t) + A*t --
 * so a generated tone never dips and the assertion cannot pass against
 * one. What makes it pass is that music has quiet moments.
 *
 * Which does mean the assertion leans on the track's amplitude envelope
 * rather than on the fade itself. A synthetic track with deep nulls cut
 * into it would satisfy the check too -- and would satisfy it whether or
 * not the fade-out ever finished, which is vacuous rather than portable.
 * So: real audio, and the dependency written down here, instead of a
 * fixture tuned until it went green.
 *
 * Its length no longer matters -- the test passes with this track cut to
 * twenty seconds. It used to: forty seconds failed where sixty passed, and
 * that was recorded here as an unexplained cliff between 42 and 45
 * seconds. It was quietest_over's clock, not the engine; see the comment
 * there and NOTES.md, "A window eighteen times longer than it read".
 *
 * Made with:
 *   ffmpeg -i joyful-sakura_no_ato.mp3 -t 60 -c:a libmp3lame -b:a 96k \
 *          -ar 44100 -ac 2 -write_xing 1 -id3v2_version 0 fix-music.mp3
 *
 * Re-encoded rather than `-c copy`: a stream copy keeps the *original's*
 * Xing frame count, so a 40-second file claims to be 180 seconds long and
 * the engine reads a duration three times the truth. Regenerating this
 * needs the game; everything else in the fixture comes from
 * tools/mkfixaudio.py.
 */
static const char *TRACK = "audio/fix-music.mp3";

/* Poll until `ms` of audio has been produced.
 *
 * The resolution is one mr_audio_poll, and a poll mixes as many buffers as
 * the queue will take -- so with the default depth a run_ms(200) produces
 * over a second, and every command issued between two calls lands somewhere
 * inside that. The fades below are measured in a few hundred milliseconds, so
 * the test's whole timeline was quantised to a unit larger than the thing it
 * measures. That stayed invisible while the depth did not change; raising
 * NBUF from 16 to 24 failed one assertion and nothing in the failure pointed
 * at the clock. main caps the queue to keep this granular. */
static void run_ms(long ms)
{
    long want = g_frames + ms * 44100 / 1000;
    int guard = 0;
    while (g_frames < want && guard++ < 100000)
        mr_audio_poll();
}

/* What the channel says it is on, which mid-fade is the incoming track
 * rather than the audible one -- see mr_audio_current. */
static const char *current(void)
{
    static char buf[128];
    int loop;
    mr_audio_current(MR_CH_MUSIC, buf, sizeof(buf), &loop);
    return buf;
}

static int playing(void)
{
    const char *buf = current();
    return buf[0] && !strcmp(buf, TRACK);
}

/* The quietest the channel gets over `ms` of output, sampled per poll. A
 * track that stops and starts again dips to silence; one that runs on does
 * not. Asserting on what comes out of the mixer rather than on its internal
 * position is both closer to what the player hears and less to maintain.
 *
 * One poll is the finest resolution there is -- 93ms with the shallow queue
 * set in main -- so the sampling window is a poll, and the *number* of polls
 * is what `ms` controls. Written as `for (step = 0; step < ms; step += 5)
 * run_ms(5)` first, which is wrong in a way that took a day to see: run_ms
 * polls until its target is passed, and one poll overshoots a 5ms target by
 * a whole buffer, so each step cost 93ms and `quietest_over(800)` consumed
 * fifteen seconds of audio. Every window in this test was eighteen times
 * longer than it reads, which is what made a 40-second fixture behave
 * differently from a 60-second one: the if_changed window started 31
 * seconds in and ran long enough to wrap a short track back round to its
 * quiet intro. See NOTES.md, "A window eighteen times longer than it
 * read". */
static int quietest_over(long ms)
{
    long want = g_frames + ms * 44100 / 1000;
    int lowest = 32767;
    int guard = 0;

    while (g_frames < want && guard++ < 10000) {
        g_peak = 0;
        mr_audio_poll();
        if (g_peak < lowest)
            lowest = g_peak;
    }
    return lowest;
}

static int looping(void)
{
    char buf[128];
    int loop = 0;
    mr_audio_current(MR_CH_MUSIC, buf, sizeof(buf), &loop);
    return loop;
}

static void check(const char *label, int ok)
{
    printf("%-52s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: audiostoptest <pak> <game-dir>\n");
        return 2;
    }

    setbuf(stdout, NULL);
    mr_vfs_init(argv[1], argv[2]);
    if (!mr_audio_init(argv[2])) {
        printf("no audio device\n");
        return 1;
    }

    /* Two buffers, the shallowest the mixer allows, so one poll produces 93ms
     * instead of a whole runway -- see run_ms. Nothing under test depends on
     * the depth: fades step per output frame, and stop/replay/queue are
     * decided when a source runs out, not when a buffer does. latencytest is
     * what covers the depth itself. */
    mr_audio_set_queue_ms(1);

    /* --- `play music X fadeout 1.0` over the same track, which is what a
     *     chapter break does when the player outruns the fade --- */
    mr_audio_play_ex(MR_CH_MUSIC, TRACK, 1, 0, 0, 1.0f, 0);
    run_ms(2000);
    g_peak = 0;
    run_ms(200);
    check("playing, and audible", playing() && g_peak > 1000);

    /* Measured on the output, not on mr_audio_current. This used to ask
     * `playing()`, which conflated two different questions: what the channel
     * is *sounding* and what the channel *is*. A stop with a fadeout is
     * still sounding and is already stopped, and the last block in this file
     * is about exactly that distinction -- so the audible half has to be
     * asked of the mixer.
     *
     * The peak is taken across the same 200ms the test already ran, not an
     * extra window: everything below is timed from here, and inserting even
     * 50ms moved the later fade far enough to fail two assertions. */
    mr_audio_stop(MR_CH_MUSIC, 1000);
    g_peak = 0;
    run_ms(200);                       /* a fifth of the way through the fade */
    check("still audible during the fade-out", g_peak > 1000);

    /* The replay, arriving mid-fade. */
    mr_audio_play_ex(MR_CH_MUSIC, TRACK, 1, 1000, 1000, 1.0f, 0);
    {
        int low = quietest_over(1600);
        printf("   (quietest across the change: %d)\n", low);
        check("the fade-out was allowed to finish", low < 200);
    }
    run_ms(1500);
    g_peak = 0;
    run_ms(200);
    check("then the same track plays again", playing() && g_peak > 1000);
    check("looping", looping());

    /* --- if_changed, which only the main menu asks for ---
     *
     * Asked of the position rather than of the quietest output, unlike the
     * fade-out above. This call passes fadeout_ms = 0, so an if_changed that
     * failed to notice the track is already playing reopens the file: the
     * decode restarts and the position drops back to zero. That is the whole
     * signature, and it is unambiguous.
     *
     * It used to look for the output never dropping below 500 across the
     * window, which this track's first seconds do not survive -- it is
     * percussive, and a 93ms window between two notes reads 28 with nothing
     * whatever wrong. The loudest over the window is kept as the guard
     * against passing on a channel that has gone silent for some other
     * reason; a floor is what the music cannot promise. */
    {
        long before, after;

        before = mr_audio_position_ms(MR_CH_MUSIC);
        mr_audio_play_ex(MR_CH_MUSIC, TRACK, 1, 0, 0, 1.0f, 1);
        g_peak = 0;
        run_ms(800);
        after = mr_audio_position_ms(MR_CH_MUSIC);
        printf("   (position %ldms -> %ldms with if_changed, loudest %d)\n",
               before, after, g_peak);
        check("if_changed does not interrupt a playing track",
              after > before && g_peak > 1000);
        check("  and it is still that track", playing());
    }

    /* --- a stop with no replay still stops --- */
    mr_audio_stop(MR_CH_MUSIC, 1000);
    run_ms(2000);
    check("a stop with no replay does stop", !playing());

    mr_audio_play_ex(MR_CH_MUSIC, TRACK, 1, 1000, 0, 1.0f, 0);
    run_ms(1500);
    check("and the track starts again afterwards", playing());

    /* --- a stop with no fade is immediate --- */
    mr_audio_stop(MR_CH_MUSIC, 0);
    check("an unfaded stop is immediate", !playing());

    /* --- the outgoing track runs out before the fade does ---
     *
     * `play X fadeout 1.0` parks X and fades the current track down. If
     * that track reaches its end during the fade, the end-of-source path
     * is where it is noticed -- and that path knew about a *queued* track
     * and not about a pending one, so it closed the channel and X was
     * never heard. The music simply stopped.
     *
     * Rare in a script (the fade has to land in the last second of a
     * track) and not rare at all in a jukebox, where Next and Previous
     * issue exactly this call whenever the player presses them near the
     * end of a song. juketest found it on a twelve-second fixture track at
     * 11.7s; this reproduces it deliberately with the 183ms one, which
     * runs out two buffers in, a long way inside a one-second fade.
     *
     * The fade-out clears the channel's loop flag, so a *looping* track
     * ends here too rather than wrapping -- which is why the short track
     * can be asked to loop and still exercise this.
     */
    {
        const char *tiny = "audio/fix-cbr-44-tiny.mp3";

        mr_audio_play_ex(MR_CH_MUSIC, tiny, 1, 0, 0, 1.0f, 0);
        run_ms(50);
        check("a very short track is playing", !strcmp(current(), tiny));

        mr_audio_play_ex(MR_CH_MUSIC, TRACK, 1, 0, 1000, 1.0f, 0);
        run_ms(3000);
        printf("   (after it ran out mid-fade: %s)\n", current());
        check("the track waiting behind the fade still starts", playing());
        check("  and it is the one that was asked for",
              !strcmp(current(), TRACK));
    }

    mr_audio_stop(MR_CH_MUSIC, 0);

    /* --- if_changed must not reopen a track that is already playing ---
     *
     * The test used to be `if_changed && active && loop && same path`, so a
     * channel started without looping fell through to a reopen every time
     * the same track was asked for again. Rollback asks for exactly that, on
     * every step back, and a reopen that fails leaves the channel dead --
     * which is how "rolling back stops the music" happened on the target and
     * not on a host where the reopen always succeeds.
     *
     * Checked by position: a reopen restarts the decode at zero.
     */
    {
        long before, after;

        mr_audio_stop(MR_CH_MUSIC, 0);
        run_ms(100);
        mr_audio_play_ex(MR_CH_MUSIC, TRACK, 0, 0, 0, 1.0f, 0);
        run_ms(2000);
        before = mr_audio_position_ms(MR_CH_MUSIC);
        check("a non-looping track is playing", before > 1000);

        /* loop=0 is the point: this is what a rollback asks for when the
         * checkpoint it restores recorded a channel that was not looping,
         * and it is the requested flag -- not the channel's -- that the old
         * test looked at. With loop=1 here the bug does not show. */
        mr_audio_play_ex(MR_CH_MUSIC, TRACK, 0, 0, 0, 1.0f, 1);
        run_ms(200);
        after = mr_audio_position_ms(MR_CH_MUSIC);
        printf("position %ldms -> %ldms across an if_changed play\n",
               before, after);
        check("if_changed on a channel that was not looping does not reopen",
              after >= before);
    }

    /* --- what a checkpoint records mid `stop music fadeout` ---
     *
     * `stop music fadeout 1.0` followed straight away by a line of dialogue:
     * the next say's checkpoint is taken while the outgoing track is still
     * audible. What the channel *is* at that moment is "no music" -- the
     * fade is only how it gets there -- so that is what has to be recorded,
     * or rolling back to that line brings the stopped track back.
     *
     * mr_audio_current already knew this for the other fadeout: mid `play X
     * fadeout`, it reports X rather than the outgoing track, via
     * has_pending. A stop has no pending track, so it fell through to
     * reporting the one on its way out.
     *
     * Reported in a real save (SAVEP1S5): roll back to just after
     * `stop music fadeout 1.0` and the track from before the stop resumes.
     */
    printf("\n-- a stop with a fadeout reads as stopped at once --\n");
    {
        char buf[128];
        int loop, peak_during;

        /* A clean channel first. The blocks above leave it part-way through
         * a non-looping track, and an exhausted channel reports no music for
         * the wrong reason -- which made the first version of this test pass
         * before the fix. */
        mr_audio_stop(MR_CH_MUSIC, 0);
        run_ms(50);
        mr_audio_play_ex(MR_CH_MUSIC, TRACK, 1, 0, 0, 1.0f, 0);
        g_peak = 0;
        run_ms(500);
        printf("   before the stop: peak %d, position %ldms\n",
               g_peak, mr_audio_position_ms(MR_CH_MUSIC));
        check("playing again, and audible", playing() && g_peak > 1000);

        /* A long fade, so "still audible" is not a timing race. */
        mr_audio_stop(MR_CH_MUSIC, 3000);
        g_peak = 0;
        run_ms(200);
        peak_during = g_peak;
        /* Still coming out of the mixer -- otherwise this tests nothing:
         * the whole difficulty is that the channel is audible and stopped at
         * the same time. */
        check("  the outgoing track is still audible", peak_during > 1000);

        mr_audio_current(MR_CH_MUSIC, buf, sizeof(buf), &loop);
        printf("   mid-fade: peak %d, channel reports '%s'\n",
               peak_during, buf[0] ? buf : "(none)");
        check("  but the channel reports no music", buf[0] == '\0');

        /* And once it has actually gone quiet, still nothing. */
        run_ms(3500);
        mr_audio_current(MR_CH_MUSIC, buf, sizeof(buf), &loop);
        check("  and still none once silent", buf[0] == '\0');
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    mr_audio_shutdown();
    mr_vfs_shutdown();
    return fails ? 1 : 0;
}

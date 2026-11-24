/* movie.c: MPEG-1 cutscene playback, from the archive to the presenter.
 *
 * yuvtest covers the conversion -- the arithmetic that turns three byte
 * planes into pixels. This covers everything around it: opening a movie
 * through the VFS, demuxing a program stream, decoding video and MP2 audio
 * out of the same file, the clock that decides when a frame is due, frame
 * dropping, and the handover to the presenter.
 *
 * pl_mpeg is somebody else's decoder and well travelled; what is new here is
 * the engine's side of it, and that is what this tests. The one thing it
 * borrows the decoder for is the answer to "did a real picture come out",
 * which is not something a fixture can assert by construction.
 *
 * --- the fixture ----------------------------------------------------------
 *
 * fixgame/audio/fixmovie.mpg: 64x32, 25fps, two seconds, with a 440Hz MP2
 * soundtrack. 28KB. Committed rather than generated, like fix-music.mp3
 * beside it, because generating it needs ffmpeg and the suite must run
 * without it. Regenerate with:
 *
 *   ffmpeg -f lavfi -i "testsrc=size=64x32:rate=25:duration=2,hue=H=6.283*t" \
 *          -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" \
 *          -c:v mpeg1video -b:v 150k -g 12 \
 *          -c:a mp2 -b:a 64k -ar 44100 -ac 2 \
 *          -f mpeg tools/harness/fixgame/audio/fixmovie.mpg
 *
 * 25fps and not something rounder because **MPEG-1 does not permit an
 * arbitrary frame rate**: the sequence header carries a four-bit code with
 * eight legal values, and 10fps is not one of them. ffmpeg refuses with
 * "MPEG-1/2 does not support 10/1 fps", which is worth recording here
 * because the obvious first attempt at this fixture was 10fps and the error
 * does not say what the legal values are. They are 23.976, 24, 25, 29.97,
 * 30, 50, 59.94 and 60.
 *
 * `testsrc` rather than a solid colour, deliberately: it moves. A fixture
 * whose every frame is identical cannot tell "the decoder is working" from
 * "the first frame is stuck on screen", which is claim 4 below and is exactly
 * the shape a reference-frame bug takes.
 *
 * The `hue` filter on top of it is not decoration either, and the first
 * version of this fixture did without it and had to be regenerated.
 * presenthost.c reports one pixel -- the top-left -- and testsrc's top-left
 * corner is inside a *static* colour bar, so claim 4 saw two changes across
 * fourteen presented frames and failed. Rotating the hue over the two seconds
 * makes every pixel of every frame differ from the last, which is what makes
 * a one-pixel probe sufficient. The lesson is the general one: a fixture has
 * to be able to express the bug at the point the test actually looks.
 *
 * --- what the host cannot test --------------------------------------------
 *
 * The clock. On the target the video follows the sound card's own playback
 * position, because that is the one thing in the machine that cannot be told
 * to hurry up (movie.c says why at length). The host shim's waveOut consumes
 * buffers the instant they are queued, so that clock runs as fast as the test
 * polls it rather than in real time.
 *
 * So this cannot assert "twenty-five frames per second". What it *can* assert
 * is that the clock is obeyed -- that frames are not presented when none is
 * due, and that when the clock runs ahead the decoder drops rather than
 * falling behind. Those are the properties the design turns on, and they are
 * testable against a clock of any speed. The frame *rate* belongs to the real
 * machine, and the log line movie.c writes at the end of every cutscene is
 * how it gets measured there.
 *
 * fixgame/audio/fixsilent.mpg is the second fixture: the same thing for one
 * second with no audio track at all (`-an`). It exists because of claim 7,
 * which the first fixture *cannot* support -- see there.
 *
 * Seven claims:
 *
 *   1. A movie in the archive opens, and reports the size, rate and duration
 *      the file actually has. The duration matters most: it is read by
 *      seeking near the end of the stream for the last timestamp, which is
 *      the only thing the VFS seek and tell callbacks exist for, and it is
 *      what makes a cutscene's length known before it starts rather than
 *      after -- the thing the poster-frame stand-in could never do.
 *   2. Something that is not a movie is refused, and refused without dying.
 *      This is the path that falls back to the stand-in, so a crash here is
 *      a crash on any game with a .webm cutscene.
 *   3. Playing it to the end presents frames, and the frames are real
 *      pictures rather than uniform grey.
 *   4. Successive frames differ. A decoder that demuxed correctly and
 *      reconstructed nothing still produces the right *number* of frames.
 *   5. The audio reaches the mixer, and is audible rather than silence.
 *   6. mr_movie_close puts the mixer's queue depth back.
 *   7. Nothing is presented before the clock says a frame is due.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "movie.h"
#include "audio.h"
#include "vfs.h"
#include "presenthost.h"
#include "interp.h"
#include "image.h"
#include "scene.h"
#include "script.h"
#include "plat.h"   /* mr_plat_ticks, for the silent movie's clock */

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_invalidate(void) { }
void mr_quit(void) { }
mr_surface *mr_stage(void) { return NULL; }

/* The host waveOut shim hands each mixed buffer here. Counting the ones with
 * something in them is how claim 5 tells "audio was pushed at the mixer" from
 * "audio came out of the mixer" -- the push could succeed and the channel
 * still be silent if the mixer's volume, mute or resampling were wrong, and
 * the movie channel's volume comes from the *music* mixer, which is exactly
 * the sort of mapping that is easy to get wrong and silent when it is. */
static long g_bufs, g_loud;
static void sink(const short *pcm, int samples)
{
    int i;

    g_bufs++;
    for (i = 0; i < samples; i++) {
        if (pcm[i] > 256 || pcm[i] < -256) {
            g_loud++;
            return;
        }
    }
}
void (*hostshim_wave_sink)(const short *, int) = sink;

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

#define MOVIE "audio/fixmovie.mpg"

int main(int argc, char **argv)
{
    const char *pak = argc > 1 ? argv[1] : NULL;
    const char *dir = argc > 2 ? argv[2] : NULL;

    printf("movietest: MPEG-1 cutscene playback\n\n");

    if (!mr_vfs_init(pak, dir)) {
        printf("could not mount %s / %s\n", pak ? pak : "(no pak)",
               dir ? dir : "(no dir)");
        return 1;
    }

    if (!mr_vfs_exists(MOVIE)) {
        /* 77, not 0: "I could not test what I am for" is not a pass. */
        printf("no %s in the archive; nothing to test.\n", MOVIE);
        printf("Run tools/harness/mkfixpak.sh.\n");
        return 77;
    }

    if (!mr_audio_init(dir)) {
        printf("no audio device from the shim; cannot test the clock.\n");
        return 77;
    }

    /* --- 1: it opens, and knows what it is ----------------------------- */
    {
        long dur;

        check("a movie in the archive opens", mr_movie_open(MOVIE) != 0);
        if (!mr_movie_active()) {
            printf("\nFAILURES (nothing else can run)\n");
            return 1;
        }

        dur = mr_movie_duration_ms();
        printf("      duration reported: %ldms\n", dur);

        /* The file is two seconds. The demuxer's figure comes from the last
         * video timestamp, so it lands a frame or two short of the nominal
         * length -- 1960ms for this one -- which is correct and is why this
         * is a window rather than an equality. A duration of 0 would mean
         * the seek and tell callbacks are not working, which is the failure
         * worth catching: everything would still play, and every cutscene's
         * length would silently come from MR_CUT_FLOOR_MS instead. */
        check("the duration is read off the stream, not guessed",
              dur > 1500 && dur < 2100);
    }

    /* --- 6, first half: nothing is due yet ----------------------------- */
    {
        /* One update immediately after opening. The clock has not advanced:
         * the shim's device has played nothing, so the movie's position is 0
         * and exactly one frame -- the first -- is due. More than one would
         * mean the decoder is running on nothing but its own eagerness. */
        mr_hostpresent_video_reset();
        mr_movie_update();
        printf("      frames presented by the first update: %ld\n",
               mr_hostpresent_video_frames());
        check("the first update presents the first frame and no more",
              mr_hostpresent_video_frames() == 1);
    }

    /* --- repainting without decoding ------------------------------------
     *
     * A repaint that does not come from the frame loop -- WM_PAINT, from an
     * uncover, an activate or a resize -- must put the *video* back on
     * screen, not the stage. platw32.c's handler painted the stage, which
     * during a cutscene is the scene from before it started; the symptom was
     * the old background flashing over the video, and a drag-resize showing
     * the scene for the whole drag, because a resize runs a modal loop in
     * which the application's message loop never gets a turn.
     *
     * The handler itself is in platw32.c and no harness links it. What is
     * testable is the half that matters: that mr_movie_repaint hands a frame
     * to the presenter and does *not* advance playback. A repaint that
     * decoded would make a cutscene run faster when the window was busy.
     */
    {
        long before_frames = mr_hostpresent_video_frames();
        long before_shown = 0, after_shown = 0;
        int w = 0, h = 0;

        mr_movie_stats(&before_shown, NULL, NULL);
        check("a repaint mid-cutscene presents a frame", mr_movie_repaint());
        mr_movie_stats(&after_shown, NULL, NULL);
        mr_hostpresent_video_last(&w, &h);

        check("  and it reached the presenter",
              mr_hostpresent_video_frames() == before_frames + 1);
        check("  at the movie's size", w == 64 && h == 32);
        check("  without advancing playback", after_shown == before_shown);
    }


    /* --- 3, 4, 5: play it out ------------------------------------------ */
    {
        long updates = 0;
        long shown = 0, dropped = 0, starved = 0;
        int w = 0, h = 0;
        mr_u32 first_px = 0, last_px = 0;
        int changed = 0;
        mr_u32 prev = 0;
        int have_prev = 0;

        mr_hostpresent_video_pixel(&first_px);

        while (mr_movie_update() && updates < 20000) {
            mr_u32 px;

            mr_audio_poll();
            updates++;

            if (mr_hostpresent_video_pixel(&px)) {
                if (have_prev && px != prev)
                    changed++;
                prev = px;
                have_prev = 1;
            }
        }

        mr_movie_stats(&shown, &dropped, &starved);
        mr_hostpresent_video_last(&w, &h);
        mr_hostpresent_video_pixel(&last_px);

        printf("      %ld updates, %ld frames presented, %ld dropped\n",
               updates, mr_hostpresent_video_frames(), dropped);
        printf("      frame size %dx%d, mixed buffers %ld (%ld audible)\n",
               w, h, g_bufs, g_loud);

        check("it reaches the end of the stream", updates < 20000);
        check("frames were presented", mr_hostpresent_video_frames() > 1);

        /* The denominator of the decode timing mr_movie_close logs. The
         * milliseconds themselves belong to the target -- the host clock is
         * finer than the work and the shims do not wait -- but every frame
         * the decoder produced was either shown or dropped, so this identity
         * has to hold, and it is what catches a decode counted in the wrong
         * place. See mr_movie_decoded. */
        printf("      decoded %ld, shown %ld + dropped %ld = %ld\n",
               mr_movie_decoded(), shown, dropped, shown + dropped);
        check("every decoded frame was either shown or dropped",
              mr_movie_decoded() == shown + dropped);
        check("the frame size is the fixture's 64x32", w == 64 && h == 32);

        /* An all-zero YUV plane set is not black -- it converts to a dark
         * green -- and a decoder that produced nothing at all would hand over
         * uniform frames of it. Requiring the top-left pixel to *change* is
         * what distinguishes a working decoder from a plausible one. */
        printf("      top-left pixel changed %d times\n", changed);
        check("successive frames differ, so pictures are being decoded",
              changed > 2);

        check("the soundtrack reached the mixer", g_bufs > 0);
        check("and came out of it audible, on the music mixer's volume",
              g_loud > 0);
    }

    /* --- 6, second half: the queue depth is restored ------------------- */
    {
        long before = mr_audio_queue_ms();
        long after;

        mr_movie_close();
        after = mr_audio_queue_ms();

        printf("      queue depth during playback %ldms, after %ldms\n",
               before, after);

        /* Playback shortens the mixer's runway so the picture does not trail
         * the sound by a whole second, and must put it back. A cutscene that
         * left the queue shallow would make every transition for the rest of
         * the session more likely to stutter -- a fault that shows up
         * nowhere near its cause. */
        check("mr_movie_close restores the mixer's queue depth",
              after > before);
        check("and the movie is no longer active", !mr_movie_active());
    }

    /* --- 2: something that is not a movie ------------------------------ */
    {
        /* An MP3 the fixture already has. It is a real file of real audio
         * that is emphatically not a program stream, which is the case that
         * matters: a cutscene whose .mpg has not been encoded yet, where the
         * engine must fall back to the poster-frame stand-in rather than
         * die. */
        check("an MP3 is refused, not played",
              !mr_movie_open("audio/fix-cbr-44-tiny.mp3"));
        check("a missing file is refused", !mr_movie_open("audio/nope.mpg"));
        check("and nothing is left playing after a refusal",
              !mr_movie_active());

        /* And a repaint with no cutscene running must say so rather than
         * presenting a stale frame, because that is what tells platw32.c's
         * WM_PAINT to fall back to the stage. Getting this wrong would
         * freeze the last frame of a cutscene over the rest of the game. */
        check("a repaint with nothing playing is refused",
              !mr_movie_repaint());

        /* Closing when nothing is open must be safe: both the end-of-movie
         * path and the dismiss path call it, and a skipped cutscene can
         * reach it twice. */
        mr_movie_close();
        mr_movie_close();
        check("closing twice is harmless", !mr_movie_active());
    }

    /* --- 7: the clock is actually obeyed -------------------------------
     *
     * This needs the *silent* fixture, and that is the whole reason it
     * exists.
     *
     * A movie with a soundtrack follows the sound card's playback position,
     * and the host shim's device consumes every buffer the instant it is
     * queued -- so that clock races ahead of real time and a frame is always
     * overdue. The gate that holds a frame back until its moment arrives is
     * therefore never reached, and deleting it from movie.c leaves every
     * assertion above still passing. Checked, not assumed: that break was
     * tried, and the suite stayed green.
     *
     * A movie with no audio track has no such clock and falls back to
     * mr_plat_ticks, which on this host is real time. So a tight update loop
     * over a one-second 25fps file must present about twenty-five frames and
     * not one per update, and must take about a second doing it. That is the
     * gate, and it is the same code path the target uses for a silent
     * cutscene.
     */
    {
        unsigned int t0, elapsed;
        long updates = 0;

        mr_hostpresent_video_reset();

        if (!mr_vfs_exists("audio/fixsilent.mpg")) {
            printf("  (no audio/fixsilent.mpg; the clock claim is skipped)\n");
            fails++;
        } else if (!mr_movie_open("audio/fixsilent.mpg")) {
            check("a silent movie opens", 0);
        } else {
            t0 = mr_plat_ticks();
            while (mr_movie_update() && updates < 2000000)
                updates++;
            elapsed = mr_plat_ticks() - t0;

            printf("      %ld updates presented %ld frames in %ums\n",
                   updates, mr_hostpresent_video_frames(),
                   (unsigned)elapsed);

            /* The gate. A one-second file at 25fps is twenty-five frames
             * however many times it is asked; a loop with the gate removed
             * presents one per update, and there are far more than
             * twenty-five updates in a second. The window is wide because
             * the last frame's timing and the loop's exit interact; what is
             * being caught is the difference between tens and thousands. */
            check("a silent movie presents about one frame per 40ms",
                  mr_hostpresent_video_frames() >= 20 &&
                  mr_hostpresent_video_frames() <= 40);

            /* And it took real time to do it, rather than running the file
             * out as fast as the decoder could go. */
            check("and it took about the file's own second to play",
                  elapsed > 700 && elapsed < 2500);

            /* Many more updates than frames is the gate doing its job: most
             * calls found nothing due and returned without decoding. */
            check("most updates found nothing due and decoded nothing",
                  updates > mr_hostpresent_video_frames() * 3);

            mr_movie_close();
        }
    }

    /* --- 8: the music comes back ---------------------------------------
     *
     * `stop_music` is True by default, so a cutscene stops the music -- and
     * Ren'Py starts it again afterwards. It *restarts* rather than resumes:
     * force_stop empties the channel's queue but keeps the loop entries
     * (audio/audio.py:508-516), so clearing the flag queues the looping
     * track again from its beginning. A track that was not looping does not
     * come back at all.
     *
     * This is the only claim here that goes through the interpreter, because
     * the behaviour lives in the statement and not in movie.c. It was also
     * the bug: the first implementation stopped the music and left the
     * restart to "the script's next `play music`". Most scripts have one.
     * Miwa's chap0 does not -- it plays its track once at the top of the
     * label and expects it to still be there afterwards -- and the symptom
     * was a chapter that fell silent for the rest of its length.
     *
     * fixture_cutscene_music is shaped after that label for exactly this
     * reason. Nothing else in the fixture plays music under a cutscene, and
     * cuttest cannot cover it: it drives the splashscreen, where there is no
     * music playing to stop.
     */
    {
        mr_script *s = mr_script_load(dir);
        char track[MR_PATH_MAX];
        int loop = 0;
        long guard;

        if (!s || mr_script_find_label(s, "fixture_cutscene_music") < 0) {
            check("the fixture has a cutscene-with-music label", 0);
        } else {
            /* Before mr_interp_init, and not optional: mr_interp_init calls
             * mr_image_autodefine_dir but does *not* initialise the image
             * module, and mr_image_init is what fills the name hash with -1.
             * Skipped, the table is zeroed BSS, so find_index starts at
             * index 0 and follows g_img[0].hnext -- which is also 0 -- round
             * for ever. The symptom is the harness hanging with no output
             * rather than failing, which cost a backtrace to find. cuttest
             * does the same call for the same reason. */
            mr_image_init(dir);
            mr_interp_init(s, dir);
            mr_interp_start("fixture_cutscene_music");

            /* Run to the cutscene: the label plays music, says a line, then
             * reaches the statement. Dismiss lines until a cutscene is
             * running. */
            for (guard = 0; guard < 500 && !mr_movie_active(); guard++) {
                mr_interp_update(mr_plat_ticks());
                mr_audio_poll();
                if (!mr_movie_active())
                    mr_interp_advance();
            }

            check("the script reaches the cutscene", mr_movie_active());

            mr_audio_current(MR_CH_MUSIC, track, (int)sizeof(track), &loop);
            printf("      music during the cutscene: '%s'\n", track);
            check("the music is stopped while it plays", track[0] == '\0');

            /* Skip it, as a click would. */
            mr_interp_advance();
            for (guard = 0; guard < 500 && mr_movie_active(); guard++) {
                mr_interp_update(mr_plat_ticks());
                mr_audio_poll();
            }

            mr_audio_current(MR_CH_MUSIC, track, (int)sizeof(track), &loop);
            printf("      music after the cutscene: '%s' (loop=%d)\n",
                   track, loop);
            check("the music is playing again afterwards", track[0] != '\0');
            check("and still looping, as it was before",  loop != 0);
        }
    }

    /* --- 9: the right-click ends the cutscene and opens nothing ---------
     *
     * The brief asked for this and it was reported as a bug: right-clicking
     * a cutscene discarded the video (right), opened the game menu (wrong),
     * and closing the menu left the story on an empty dialogue line
     * (wronger). See mr_interp_game_menu_toggle for why this diverges from
     * Ren'Py's source, which has no cutscene test in _invoke_game_menu.
     *
     * cuttest covers the splashscreen case and passed throughout, because
     * the guard there is g_in_splash -- a cutscene in an ordinary label fell
     * straight through it. That is why this claim is here and not added to
     * cuttest: the two are different states, and the one with no
     * splashscreen around it had no coverage at all.
     *
     * The last assertion is the empty-line half. Ending the cutscene is not
     * enough on its own; the story has to carry on into the next statement
     * properly, and "an empty say window" is what it looked like when it did
     * not.
     */
    {
        mr_script *s = mr_script_load(dir);
        long guard;

        if (!s || mr_script_find_label(s, "fixture_cutscene_music") < 0) {
            check("the fixture has a cutscene label to right-click", 0);
        } else {
            mr_image_init(dir);
            mr_interp_init(s, dir);
            mr_interp_start("fixture_cutscene_music");

            for (guard = 0; guard < 500 && !mr_movie_active(); guard++) {
                mr_interp_update(mr_plat_ticks());
                mr_audio_poll();
                if (!mr_movie_active())
                    mr_interp_advance();
            }
            check("a cutscene is running to right-click on",
                  mr_movie_active());

            /* The right-click. */
            mr_interp_game_menu_toggle();
            mr_interp_update(mr_plat_ticks());

            {
                /* Not "no screens at all": the quick menu legitimately comes
                 * back the moment the cutscene ends, and asserting zero
                 * screens would fail on that. cuttest can assert zero
                 * because it runs inside a splashscreen, where the overlay
                 * is suppressed -- which is the same difference that let
                 * this bug through in the first place.
                 *
                 * What must not be there is the game menu. The bug opened
                 * the save screen. */
                int i, menu = 0;

                for (i = 0; i < mr_scene_screen_count(); i++) {
                    const char *nm = mr_scene_screen_name(i);
                    printf("      screen up: %s\n", nm ? nm : "(null)");
                    if (nm && (!strcmp(nm, "save") || !strcmp(nm, "load") ||
                               !strcmp(nm, "preferences") ||
                               !strcmp(nm, "game_menu")))
                        menu = 1;
                }
                check("  it opens no game menu", !menu);
            }
            check("  and the cutscene is over", !mr_movie_active());

            /* And the story goes on. The label's next statement is a say,
             * so the interpreter must reach it and have text for it -- an
             * empty window was the reported symptom. */
            {
                const char *who = NULL, *what = NULL;
                mr_u32 col = 0;
                int shown = 0, got = 0;

                for (guard = 0; guard < 200; guard++) {
                    mr_interp_update(mr_plat_ticks());
                    got = mr_interp_dialogue(&who, &what, &col, &shown);
                    if (got && what && what[0])
                        break;
                }

                printf("      line after the cutscene: '%s'\n",
                       (got && what) ? what : "(none)");
                check("  the story continues to the next line", got != 0);
                check("  and that line is not empty",
                      got && what && what[0] != '\0');
            }
        }
    }

    printf("\n  the clock above is real time, because the silent fixture has\n"
           "  no sound card to follow. For a movie *with* audio the clock is\n"
           "  the card's own playback position, and the host shim consumes\n"
           "  audio instantly -- so the frame *rate* of a cutscene with sound\n"
           "  is not asserted here. That belongs to the target, in movie.c's\n"
           "  end-of-cutscene log line.\n");

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

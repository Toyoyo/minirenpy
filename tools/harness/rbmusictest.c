/* Rolling back must not restart the music.
 *
 * Ren'Py restores the channel's state on a rollback rather than replaying the
 * statements that set it, so a track that was playing before and should be
 * playing after simply keeps going -- from where it is, not from the top.
 * Ours replayed it, so every rollback jumped the music back to the beginning.
 *
 * The assertion is on the mixer's own position: after the rollback the track
 * must still be the same one and must be *further in* than it was before,
 * never back at zero.
 *
 * The other half matters just as much: rolling back to a point where a
 * different track was playing has to change the track. `if_changed` means
 * "leave it alone if it is already right", not "never touch it".
 *
 * Runs against tools/harness/story, a fixture with two tracks and a `stop`
 * in it. It used to walk the real script, which meant every edit to the game
 * re-fitted the test; what it is about is the mixer and the rollback log,
 * neither of which cares whose story it is.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "save.h"
#include "history.h"
#include "vfs.h"
#include "audio.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "slrender.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}

/* The game opens on a chapter-select menu, added to script.rpy after this
 * harness was written. Anything that wants dialogue has to answer it first;
 * which option hardly matters, so take the first. */
static void answer_menu(void)
{
    int count = 0;
    const char *texts[16];

    if (mr_interp_menu(&count, texts, 16) && count > 0)
        mr_interp_choose(0);
}

static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static long T;
static int  fails;

static void check(const char *label, int ok)
{
    printf("%-52s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void draw(void)
{
    mr_interp_update(T);
    T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

static const char *playing(void)
{
    static char buf[128];
    int loop;
    mr_audio_current(MR_CH_MUSIC, buf, sizeof(buf), &loop);
    return buf;
}

/* Poll until the mixer has produced `ms` more audio. */
static void run_ms(long ms)
{
    long want = mr_audio_position_ms(MR_CH_MUSIC) + ms;
    int guard = 0;

    while (mr_audio_position_ms(MR_CH_MUSIC) < want && guard++ < 40)
        mr_audio_poll();
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    char track[128];
    long pos_before, pos_after;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: rbmusictest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    if (!mr_audio_init(argv[1])) {
        printf("no audio device\n");
        return 1;
    }
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_start("start");
    mr_interp_main_menu();
    mr_interp_action("Start()");

    /* Play in until the story has its own music going and there are several
     * checkpoints behind us. */
    for (i = 0; i < 300; i++) {
        draw();
        if ((i % 5) == 4) {
            answer_menu();
            mr_interp_advance();
        }
        if (mr_rollback_depth() > 4 && playing()[0])
            break;
    }

    strcpy(track, playing());
    printf("playing: %s, %d checkpoints\n", track, mr_rollback_depth());
    check("the story has music playing", track[0] != '\0');
    check("and something to roll back to", mr_rollback_depth() > 4);
    if (!track[0])
        return 1;

    /* Let it get well past the start, so a restart is unmistakable. */
    run_ms(6000);
    pos_before = mr_audio_position_ms(MR_CH_MUSIC);
    printf("position before rollback: %ldms\n", pos_before);
    check("the track is well past its start", pos_before > 3000);

    /* --- the rollback --- */
    mr_interp_rollback();
    draw();
    run_ms(100);
    pos_after = mr_audio_position_ms(MR_CH_MUSIC);
    printf("position after rollback:  %ldms\n", pos_after);

    check("the same track is still playing", !strcmp(playing(), track));
    check("and it did not go back to the start", pos_after >= pos_before);

    /* Several more rollbacks in a row, which is how a player actually uses
     * it: each one used to knock the music back to zero again. */
    for (i = 0; i < 4; i++) {
        mr_interp_rollback();
        draw();
        run_ms(100);
    }
    printf("position after five:      %ldms\n",
           mr_audio_position_ms(MR_CH_MUSIC));
    check("five rollbacks leave it playing on",
          !strcmp(playing(), track) &&
          mr_audio_position_ms(MR_CH_MUSIC) >= pos_before);

    /* --- but a state with different music does change it --- */
    {
        mr_gamestate g;
        const char *other = "audio/two.mp3";

        mr_interp_capture(&g);
        check("the captured state names the track", !strcmp(g.music, track));

        /* Put something else on the channel, the way the `play music`
         * statement does, then restore: the saved track has to come back. */
        mr_audio_play_ex(MR_CH_MUSIC, other, 1, 0, 0, 1.0f, 0);
        run_ms(500);
        check("a different track is playing now", !strcmp(playing(), other));

        mr_interp_restore(&g);
        run_ms(200);
        printf("after restore: %s\n", playing());
        check("restoring brings the saved track back",
              !strcmp(playing(), track));
    }

    /* --- and a state with no music stops it --- */
    {
        mr_gamestate g;

        mr_interp_capture(&g);
        g.music[0] = '\0';
        mr_interp_restore(&g);
        check("restoring silence stops the music", !playing()[0]);
    }

    /* --- rolling back to just after a `stop music fadeout` ---
     *
     * The reported bug (SAVEP1S5): the script does
     *
     *     stop music fadeout 1.0
     *     m "Quand je termine, je relève lentement les yeux."
     *
     * and rolling back to that line brought back the track playing *before*
     * the stop. The line's checkpoint is taken while the outgoing track is
     * still audible, and the capture recorded what was sounding rather than
     * what the channel had been set to.
     *
     * Driven forward to those lines rather than constructed, because the
     * whole chain is what failed: capture reads mr_audio_current, the
     * checkpoint stores it, the rollback restores it.
     */
    printf("\n-- rolling back to just after a stop --\n");
    {
        int found = 0;

        /* Walk on until the line *after* the fixture's `stop music fadeout`
         * is on screen, recognised by its text.
         *
         * Deliberately not by asking whether the channel is playing: that is
         * the function under test, and with the bug present the loop would
         * simply wait out the whole fade and then capture a state that is
         * correctly silent -- passing while testing nothing. The line is the
         * one thing here that the audio code cannot influence. */
        for (i = 0; i < 2000; i++) {
            const char *who, *what;
            mr_u32 col;

            draw();
            if ((i % 5) == 4) {
                answer_menu();
                mr_interp_advance();
            }
            if (mr_interp_dialogue(&who, &what, &col, NULL) && what &&
                strstr(what, "The music is fading out")) {
                found = 1;
                break;
            }
        }
        if (!found) {
            const char *who, *what; mr_u32 col;
            if (mr_interp_dialogue(&who, &what, &col, NULL) && what)
                printf("   stuck on: \"%s\"\n", what);
        }
        check("the story reached the line after the stop", found);

        if (found) {
            mr_gamestate g;

            /* The checkpoint taken here must say "no music". This is the
             * assertion the bug failed: before the fix the channel was still
             * sounding the old track and reported it. */
            mr_interp_capture(&g);
            printf("   captured music after the stop: '%s'\n",
                   g.music[0] ? g.music : "(none)");
            check("the checkpoint after a stop records no music",
                  g.music[0] == '\0');

            /* Two more lines, then back: nothing may come on. */
            for (i = 0; i < 20; i++) {
                draw();
                if ((i % 5) == 4) {
                    answer_menu();
                    mr_interp_advance();
                }
            }
            mr_interp_rollback();
            run_ms(400);
            printf("   after rolling back: '%s'\n",
                   playing()[0] ? playing() : "(none)");
            check("  and rolling back to it brings nothing back",
                  playing()[0] == '\0');
        }
    }

    /* --- rolling *forward* has to move the music too ---------------------
     *
     * The mirror of everything above, and the reason it gets its own block:
     * the music lives on the checkpoint, not in the statements, so a
     * rollforward that restores the scene and the execution point but leaves
     * the channel alone looks completely right until the step crosses a
     * `play music` -- and then the player is looking at chapter two with
     * chapter one still playing. Rollback had exactly this bug once.
     *
     * Walk back until the track changes, then forward the same number of
     * steps, and require the track to come back.
     */
    {
        char at_start[128], after_back[128];
        int steps = 0, i;

        printf("\n-- rolling forward across a music change --\n");

        /* Get somewhere with a track playing and room to move. */
        for (i = 0; i < 400 && !(mr_rollback_depth() > 6 && playing()[0]); i++) {
            draw();
            if ((i % 5) == 4) {
                answer_menu();
                mr_interp_advance();
            }
        }
        run_ms(500);
        strcpy(at_start, playing());
        printf("   playing: '%s', %d checkpoints\n",
               at_start[0] ? at_start : "(none)", mr_rollback_depth());

        check("there is a forward step only after going back",
              mr_rollforward_depth() == 0);

        /* Rolling forward with nothing outstanding must be inert, not a
         * disguised advance: this is what makes PageDown silent in ordinary
         * play, which is the behaviour Ren'Py has. */
        {
            char before[128];
            strcpy(before, playing());
            mr_interp_rollforward();
            draw();
            run_ms(100);
            check("  and doing it anyway changes nothing",
                  !strcmp(playing(), before));
        }

        /* Back until the track changes *and* we have gone several steps.
         *
         * The "and several" half is the point. This used to stop at the
         * first change, which here is one step, so it only ever took one
         * forward step -- and a bug where rollforward moved one line and
         * then stopped dead went straight through it. A test that happens to
         * exercise the smallest possible case is not testing the loop. */
        for (i = 0; i < 40; i++) {
            mr_interp_rollback();
            draw();
            run_ms(200);
            steps++;
            if (steps >= 4 && strcmp(playing(), at_start))
                break;
        }
        strcpy(after_back, playing());
        printf("   after %d rollbacks: '%s'\n", steps,
               after_back[0] ? after_back : "(none)");
        check("rolling back far enough changed the track",
              strcmp(after_back, at_start) != 0);
        check("  and there are forward steps now",
              mr_rollforward_depth() > 0);

        /* Forward the same distance. */
        for (i = 0; i < steps; i++) {
            mr_interp_rollforward();
            draw();
            run_ms(200);
        }
        printf("   after %d rollforwards: '%s'\n", steps,
               playing()[0] ? playing() : "(none)");
        check("rolling forward puts the original track back",
              !strcmp(playing(), at_start));
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    mr_audio_shutdown();
    mr_vfs_shutdown();
    return fails ? 1 : 0;
}

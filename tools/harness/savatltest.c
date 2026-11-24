/* Saving part way through a transform block, and loading the slot again.
 *
 * Reported from play: a save taken during an animation comes back with the
 * animation frozen. For a half-second drop that is barely visible; for the
 * credits roll, which is one `linear 90`, it means the roll stops where the
 * save was taken and -- because it never reaches its end -- `fin` is drawn
 * over a half-scrolled credits list. The reporter cut the 90 to a 5 to check
 * it was not a hard engine freeze. It was not.
 *
 * The cause was in the reader rather than the writer: mr_shown's `atl` is a
 * pointer into the script's statement array, a file cannot hold a pointer, so
 * save.c nulled it and the entry kept the placement the file recorded with
 * nothing left to move it on. The other two fields are no use on their own
 * either -- `step_start` is an absolute timestamp from a process that has
 * since exited.
 *
 * So a save now carries the block by value, the step index, and the ms
 * elapsed into that step; the reader parks the block somewhere stable
 * (mr_scene_park_atl) and mr_interp_restore adds the current clock back
 * (mr_scene_resume_anim). The animation resumes where it was and runs to its
 * end, which is what Ren'Py does -- `st`, `st_offset` and `atl_state` are
 * ordinary attributes of the ATLTransform and go into the pickle
 * (transform.py:811, atl.py:386).
 *
 * Measured mid-flight, not at rest: every assertion that matters here is
 * taken while the block is still running, because a settled value cannot tell
 * "resumed from the middle" from "started again from the top" -- both end at
 * the same place.
 *
 * Runs on tools/harness/story, label save_atl.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "vfs.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T = 100000;   /* not 0: a rebase that forgot to add the clock
                           * would still look right at the start of time */
static int fails;

static void checkb(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-54s %6d want %6d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void check_between(const char *label, int got, int lo, int hi)
{
    int ok = (got >= lo && got <= hi);
    printf("%-54s %6d want %4d..%-4d %s\n", label, got, lo, hi,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* Where the `person` tag sits, as the renderer sees it. The animation is
 * advanced by mr_scene_render, so the frame has to be drawn before the answer
 * means anything -- which is also how the player sees it. */
static int person_yoffset(void)
{
    mr_scenelist sl;
    int i;

    mr_scene_render(g_st, T);
    mr_scene_capture(&sl);
    for (i = 0; i < sl.n; i++) {
        if (!strcmp(sl.v[i].tag, "person"))
            return sl.v[i].place.has_yoffset ? sl.v[i].place.yoffset : 0;
    }
    return -9999;
}

static const char *line_now(void)
{
    const char *who, *what;
    mr_u32 c;
    int shown;

    if (mr_interp_dialogue(&who, &what, &c, &shown))
        return what;
    return "(none)";
}

/* One interaction: advance until the displayed line changes. */
static int next_line(void)
{
    char was[200];
    int guard = 0;

    sprintf(was, "%.190s", line_now());
    while (guard++ < 60) {
        mr_interp_update(T);
        T += 33;
        mr_interp_advance();
        mr_interp_update(T);
        if (strcmp(line_now(), was) && strcmp(line_now(), "(none)"))
            return 1;
    }
    return 0;
}

static int advance_to(const char *line)
{
    int guard = 0;

    while (guard++ < 40) {
        if (!strcmp(line_now(), line))
            return 1;
        if (!next_line())
            return 0;
    }
    return 0;
}

/* Let the clock run without advancing the story, so a block can make
 * progress. */
static void settle(int ms)
{
    int i;

    for (i = 0; i < ms; i += 33) {
        T += 33;
        mr_interp_update(T);
        mr_scene_render(g_st, T);
    }
}

/* Save, go somewhere else, let a lot of time pass, and load it back.
 *
 * The time matters. The engine stores step_start as an absolute timestamp, so
 * a reader that took the file's number at face value -- or a rebase applied
 * to the wrong list -- gives a step that started thousands of ms ago and an
 * animation that is instantly over. Jumping the clock makes that visible
 * instead of plausible. */
static void round_trip(void)
{
    mr_interp_update(T);
    mr_save_write("1-6", g_st);

    mr_interp_start("start");
    settle(300);

    /* A minute of wall clock between the save and the load, as a real one has
     * -- and the frame that notices it, because mr_interp_restore rebases
     * onto the clock the interpreter last saw, not onto this local T. Without
     * the update the load lands a minute in the past and every animation is
     * instantly over, which is how the sign error below was found. */
    T += 60000;
    mr_interp_update(T);

    mr_save_load("1-6");
    mr_interp_update(T);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "story";
    mr_script *s;

    setbuf(stdout, NULL);
    remove("SAVEP1S6.SAV");

    mr_vfs_init(NULL, dir);
    mr_image_init(dir);
    mr_scene_init();
    s = mr_script_load(dir);
    mr_interp_init(s, dir);
    mr_ui_init(dir);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("save_atl");

    /* --- one long step, saved in the middle of it ----------------------- */

    printf("\n-- a 4-second roll, saved one second in --\n");
    if (!advance_to("The long roll.")) {
        printf("fixture: never reached the long roll\n");
        return 1;
    }
    checkb("the roll is running", mr_scene_animating(T), 1);

    settle(1000);
    {
        int at_save = person_yoffset();
        int after_load;

        check_between("one second in, it is part way down", at_save, 40, 200);

        round_trip();
        after_load = person_yoffset();

        checkb("  loading it back leaves it running",
               mr_scene_animating(T), 1);
        /* The whole report, in one assertion: the sprite comes back where it
         * was, not at the top and not at the end. A frame or two of slack
         * either side -- the load draws one. */
        check_between("  and resumes from where the save was taken",
                      after_load, at_save - 20, at_save + 20);

        /* And it ends. This is what `fin` over the credits was about: the
         * frozen roll never reached its last frame. */
        settle(3500);
        checkb("  it runs on to its end", person_yoffset(), 400);
        checkb("  and stops animating", mr_scene_animating(T), 0);
    }

    /* --- two steps, saved during the second ---------------------------- */

    /* A block is not just a start and an end: the save has to say which step
     * was running. Saved 700ms into two 500ms steps, so a reader that lost
     * the step index would restart the first one and come back below 100
     * rather than above it. */
    printf("\n-- two steps, saved during the second --\n");
    mr_interp_start("save_atl");
    if (!advance_to("Two steps.")) {
        printf("fixture: never reached the two-step block\n");
        return 1;
    }
    settle(700);
    {
        int at_save = person_yoffset();
        int after_load;

        check_between("700ms in, it is inside the second step",
                      at_save, 110, 290);

        round_trip();
        after_load = person_yoffset();

        checkb("  still running after the load", mr_scene_animating(T), 1);
        check_between("  on the second step, not back on the first",
                      after_load, at_save - 25, at_save + 25);

        settle(600);
        checkb("  and finishes on the last step's target",
               person_yoffset(), 300);
    }

    /* --- repeat, which has no end to resume to ------------------------- */

    printf("\n-- a repeating block --\n");
    mr_interp_start("save_atl");
    if (!advance_to("Bobbing.")) {
        printf("fixture: never reached the bob\n");
        return 1;
    }
    settle(300);
    round_trip();
    {
        int y;

        checkb("a loaded repeat is still running", mr_scene_animating(T), 1);
        settle(1000);
        checkb("  still running a second later", mr_scene_animating(T), 1);
        y = person_yoffset();
        check_between("  and within its range", y, 0, 80);
    }

    /* --- a block that had already finished ----------------------------- */

    /* The other direction, and the one a careless fix breaks: a finished
     * block must not come back as a running one. If it did, mr_scene_animating
     * would stay true for the rest of the session and the engine would redraw
     * the whole stage every frame -- which on the target is a third of a
     * second each. */
    printf("\n-- a block that was long since over --\n");
    mr_interp_start("save_atl");
    if (!advance_to("Long since over.")) {
        printf("fixture: never reached the finished block\n");
        return 1;
    }
    settle(600);
    checkb("it has finished", mr_scene_animating(T), 0);
    checkb("  at its target", person_yoffset(), 60);

    round_trip();
    checkb("loading it back does not restart it", mr_scene_animating(T), 0);
    checkb("  and it is still at its target", person_yoffset(), 60);

    /* --- rolling back after a load ------------------------------------- */

    /* The same bug, surviving in the rollback log. A say checkpoint is taken
     * as the line goes *up*, so the checkpoint for "The long roll." holds the
     * sprite at the top of the roll with the block just armed. In session
     * that restores fine -- the stale timestamp finishes the block on the
     * first frame -- but history.c nulled the atl pointer of every entry it
     * read from a file, so after a load the same rollback left it frozen at
     * the top.
     *
     * 0 is the value to beat here, not a near miss: pre-fix the sprite comes
     * back exactly where the checkpoint recorded it. */
    printf("\n-- rolling back after a load --\n");
    mr_interp_start("save_atl");
    if (!advance_to("The long roll.")) {
        printf("fixture: never reached the long roll\n");
        return 1;
    }
    settle(300);
    if (!advance_to("Two steps.")) {
        printf("fixture: never reached the two-step block\n");
        return 1;
    }
    settle(100);
    round_trip();

    mr_interp_rollback();
    mr_interp_update(T);
    checkb("  landed on the line the roll played over",
           !strcmp(line_now(), "The long roll."), 1);
    checkb("the rolled-back roll finishes instead of freezing",
           person_yoffset(), 400);
    checkb("  and is not left animating", mr_scene_animating(T), 0);

    /* Forward again, onto the point that was in the file too. */
    mr_interp_rollforward();
    mr_interp_update(T);
    checkb("  rolling forward again finishes that block too",
           person_yoffset(), 300);

    remove("SAVEP1S6.SAV");
    mr_surface_free(g_st);
    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();
    mr_script_free(s);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

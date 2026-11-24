/* Rolling back to a chapter title card.
 *
 * Reported from play: save and load on a title card works, but there is no way
 * back to one. A card is
 *
 *     show screen title with fade
 *     $ renpy.pause()
 *
 * and the pause took no rollback checkpoint, so the log did not know the card
 * was a place the player had been. Rolling back from the line after it went
 * straight past to the line before it, and the card -- along with everything
 * the scene held while it was up -- could not be returned to.
 *
 * Ren'Py checkpoints an untimed pause and only an untimed one: renpy.pause
 * defaults `checkpoint` to False when it was given a delay and True when it
 * was not (statementexports.py:151-154), and the call after the interaction is
 * a hard checkpoint in the untimed case (:191). Both spellings go through that
 * function, so `pause` with no argument is a stop and `pause 0.2` is not --
 * all three are in the fixture and all three are checked here.
 *
 * What is asserted, in order:
 *
 *   - the card is a stop: one step back from the line after it lands on the
 *     pause, with the screen up and the engine waiting again
 *   - and it is a real checkpoint, not just a pc: the variable the card's
 *     statements set is back to what it held *at* the card
 *   - a second step back leaves the card, rather than re-running the pause
 *     and standing still -- the shape the menu bug had
 *   - forward returns to the card
 *   - the `pause` statement spelling stops too
 *   - a timed pause does not
 *   - the history screen loses no line to a pause being crossed
 *   - and after a save and a load on the card, one step back still moves,
 *     which is the half of this that only the file path gets wrong
 *
 * Runs on tools/harness/story, label pause_back.
 *
 * Run from a scratch directory: it writes and removes SAVE*.SAV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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
void mr_log(const char *f, ...)
{
    va_list ap;
    if (!getenv("MR_VERBOSE"))
        return;
    va_start(ap, f);
    printf("    log: ");
    vprintf(f, ap);
    printf("\n");
    va_end(ap);
}
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T = 100000;
static int fails;

/* The fixture's two card titles, which differ in length so that what the
 * stage draws can be told apart by counting lit pixels. */
#define TITLE1 "One"
#define TITLE2 "The second card, with a much longer title"

static void checkb(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-54s %6d want %6d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void checks(const char *label, const char *got, const char *want)
{
    int ok = !strcmp(got, want);
    printf("%-54s %s\n   got  '%s'\n   want '%s'\n", label,
           ok ? "ok" : "** FAIL **", got, want);
    if (!ok)
        fails++;
}

static void frame(void)
{
    mr_interp_update(T);
    T += 33;
    mr_scene_render(g_st, T);
}

/* Let whatever is in flight settle: a `with fade`, a caption typing out. */
static void settle(int ms)
{
    int i;

    for (i = 0; i < ms; i += 33)
        frame();
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

/* How much light is on the stage. The card is white text on black, so this is
 * a measure of how much text got drawn -- which is the only way to tell "the
 * right title", "the other card's title" and "nothing at all" apart. Both
 * reported faults are about what the card *draws*, so the variables are not
 * enough on their own: a screen that drew from a stale cache would keep the
 * invariant and still be wrong on screen. */
static long lit(void)
{
    long n = 0;
    int x, y;

    for (y = 0; y < MR_STAGE_H; y += 2) {
        mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        for (x = 0; x < MR_STAGE_W; x += 2)
            if (MR_R(row[x]) > 100 && MR_G(row[x]) > 100 &&
                MR_B(row[x]) > 100)
                n++;
    }
    return n;
}

static int screen_up(const char *name)
{
    int i;

    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (n && !strcmp(n, name))
            return 1;
    }
    return 0;
}

/* Advance until the named line is on screen. The line is asked for by name
 * rather than counted, so an edit to the fixture that inserts a statement
 * cannot silently leave this one short. */
static int run_to(const char *want)
{
    int guard = 0;

    while (guard++ < 400) {
        frame();
        if (!strcmp(line_now(), want))
            return 1;
        /* A pause in the way is clicked through like anything else. Timed
         * pauses in the fixture are short, and the clock here runs 33ms a
         * frame, so one will also simply expire. */
        mr_interp_advance();
    }
    return 0;
}

/* Advance until a card is up and the engine is waiting on it.
 *
 * The advance is only issued when the engine is *not* waiting. Asking the
 * other way round -- advance every frame and then look -- walks straight past
 * the card: the pause is entered and dismissed in the same frame, before the
 * `with fade` has committed the screen into the display list, so the card is
 * never seen at all. */
static int run_to_card(void)
{
    int guard = 0;

    while (guard++ < 400) {
        frame();
        if (mr_interp_waiting()) {
            settle(600);                 /* let the fade finish */
            if (screen_up("title"))
                return 1;
            mr_interp_advance();         /* a pause with no card on it */
        } else {
            mr_interp_advance();
        }
    }
    return 0;
}

/* Standing at a card: waiting, with the card's screen up. */
static int at_card(void)
{
    return mr_interp_waiting() && screen_up("title");
}

static void back_once(void)
{
    mr_interp_rollback();
    settle(600);
}

static void forward_once(void)
{
    mr_interp_rollforward();
    settle(600);
}

/* The state the card leaves the engine in: the screen up, nothing being said,
 * and the interpreter waiting rather than running on. All three, because the
 * first bug this fixture ever saw was a card that came back and then ran
 * straight through `hide screen title` into the next scene. */
static void check_on_card(const char *when)
{
    char label[80];

    sprintf(label, "%s: the card's screen is up", when);
    checkb(label, screen_up("title"), 1);
    sprintf(label, "%s: and the engine is waiting on it", when);
    checkb(label, mr_interp_waiting(), 1);
    sprintf(label, "%s: with the card's own value of pb", when);
    checkb(label, mr_var_get_int("pb", -99), 1);
}

/* The story variable the card draws, or "(unset)". */
static const char *title_now(void)
{
    static char buf[128];
    mr_value v;

    if (!mr_var_get("title", &v) || v.type != MR_V_STR)
        return "(unset)";
    strncpy(buf, v.s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

/* `title` is assigned once per card and read by the card's screen, so its
 * value is a function of how far the story has got -- which `pb` also
 * reports. The two must agree at every point rollback and rollforward can
 * stop at, whichever direction arrived there:
 *
 *     pb 0        before the first card        "" (the label clears it)
 *     pb 1, 2     at and after card one        TITLE1
 *     pb 3, 4     at and after card two        TITLE2
 *
 * Asserted after every single step of the walks below rather than at the end.
 * A card draws `text "[title]"`, so a disagreement here is the reported bug
 * exactly: the second chapter's card showing the first chapter's title.
 */
static int pairs_checked, pairs_bad;

/* The last few actions, so a failure in the random walk below prints the
 * sequence that produced it instead of only the state it ended in. */
#define TRAIL 14
static char g_trail[TRAIL][24];
static int  g_trail_n;

static void trail(const char *what)
{
    strncpy(g_trail[g_trail_n % TRAIL], what, sizeof(g_trail[0]) - 1);
    g_trail[g_trail_n % TRAIL][sizeof(g_trail[0]) - 1] = '\0';
    g_trail_n++;
}

static void print_trail(void)
{
    int i, first = (g_trail_n > TRAIL) ? g_trail_n - TRAIL : 0;

    printf("     after:");
    for (i = first; i < g_trail_n; i++)
        printf(" %s", g_trail[i % TRAIL]);
    printf("\n");
}

/* What each card's text measures when it is drawn correctly, learned on the
 * first pass rather than written down: a glyph count depends on the fixture's
 * font and would be a magic number to maintain. */
static long g_ref1, g_ref2;
static int  pix_checked, pix_bad;

/* The card on the stage draws the title the point says it should.
 *
 * Only called with everything settled -- a card part way through its fade is
 * legitimately dimmer -- and only when the engine is waiting at a card, which
 * is the one state in which the stage is the card and nothing else. */
static void check_card_pixels(const char *when, int step)
{
    int pb = mr_var_get_int("pb", -99);
    long want = (pb <= 2) ? g_ref1 : g_ref2;
    long got;

    if (!screen_up("title") || !mr_interp_waiting())
        return;
    if (pb != 1 && pb != 3)
        return;                  /* not standing at a card */

    mr_scene_render(g_st, T);
    got = lit();
    pix_checked++;

    /* A tenth is far inside the gap between the two titles -- one is three
     * characters and the other forty -- and well outside any rounding in the
     * fade. Zero is the "totally black card" report. */
    if (got == 0 || got < want - want / 10 || got > want + want / 10) {
        pix_bad++;
        fails++;
        printf("  ** FAIL ** %s step %d: card %d drew %ld lit, want ~%ld"
               "   (title '%s')\n",
               when, step, pb, got, want, title_now());
        if (g_trail_n)
            print_trail();
    }
}

static void check_pair(const char *when, int step)
{
    int pb = mr_var_get_int("pb", -99);
    const char *want = (pb <= 0) ? ""
                     : (pb <= 2) ? TITLE1
                                 : TITLE2;
    const char *got = title_now();

    pairs_checked++;
    if (strcmp(got, want)) {
        pairs_bad++;
        fails++;
        printf("  ** FAIL ** %s step %d: pb=%d, title '%s', want '%s'"
               "   (%s%s)\n",
               when, step, pb, got, want,
               screen_up("title") ? "a card is up" : "no card",
               mr_interp_waiting() ? ", waiting" : "");
        if (g_trail_n)
            print_trail();
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "story";
    mr_script *s;
    int hist_at_line, hist_on_card;

    setbuf(stdout, NULL);
    remove("SAVEP1S7.SAV");

    /* The same switch main.c reads, so a sequence found here can be read in
     * the same form as one sent in from play. MR_VERBOSE has to be on too,
     * since that is what lets this harness's mr_log print at all. */
    if (getenv("MR_ROLLTRACE"))
        mr_rollback_trace(1);

    mr_vfs_init(NULL, dir);
    mr_image_init(dir);
    mr_scene_init();
    s = mr_script_load(dir);
    mr_interp_init(s, dir);
    mr_ui_init(dir);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* --- the card is somewhere to come back to --- */
    printf("-- one step back from the line after the card --\n");
    mr_interp_start("pause_back");
    if (!run_to("A line after the card.")) {
        printf("fixture: never reached the line after the card\n");
        return 1;
    }
    settle(600);
    checkb("the card is down while that line is up", screen_up("title"), 0);
    checkb("and pb has moved past it", mr_var_get_int("pb", -99), 2);
    hist_at_line = mr_history_count();

    back_once();
    check_on_card("back once");
    checks("and nothing is being said over it", line_now(), "(none)");
    hist_on_card = mr_history_count();

    /* The line that was on screen is gone from the history, and *only* it:
     * the pause writes no entry of its own, so crossing it must not pop one.
     * This is the count-by-kind in mr_rollback_back; as an exclusion list it
     * counted a pause as a line and ate the card's predecessor out of the
     * history screen. */
    checkb("the history lost exactly the line rolled off",
           hist_at_line - hist_on_card, 1);

    /* --- and leaving it again goes somewhere --- */
    printf("\n-- a second step back --\n");
    back_once();
    checks("lands on the line before the card", line_now(),
           "A line before the card.");
    checkb("with the value from before it", mr_var_get_int("pb", -99), 0);
    checkb("and the card down", screen_up("title"), 0);

    printf("\n-- forward again --\n");
    checkb("there is a future to return to", mr_rollforward_depth() > 0, 1);
    forward_once();
    check_on_card("forward once");

    /* --- the other spelling, and the kind that is not a stop --- */
    printf("\n-- the bare `pause` statement --\n");
    mr_interp_start("pause_back");
    if (!run_to("A line after the bare pause.")) {
        printf("fixture: never reached the line after the bare pause\n");
        return 1;
    }
    settle(600);
    back_once();
    checkb("one step back is waiting at it", mr_interp_waiting(), 1);
    checks("with nothing said", line_now(), "(none)");

    printf("\n-- `pause 0.2`, which Ren'Py does not checkpoint --\n");
    mr_interp_start("pause_back");
    if (!run_to("A line after the timed pause.")) {
        printf("fixture: never reached the line after the timed pause\n");
        return 1;
    }
    settle(600);
    back_once();
    checks("one step back crosses it to the line before", line_now(),
           "A line after the bare pause.");
    checkb("so it is not a stop", mr_interp_waiting(), 0);

    /* --- the file path ---
     *
     * The save half of this already worked, and titlesavetest pins it. What
     * is new is the step *after* the load: the log's newest point is the
     * card's own, and the saved pc is the statement after the pause, so a
     * rollback that compares the two naively decides the card is not what is
     * on screen, steps back one, and lands on the card again -- re-running
     * the pause and leaving the screen exactly where it was. Reported at a
     * menu first; a pause is the same shape, and this is the assertion that
     * it was fixed for both rather than for the one that was reported.
     */
    printf("\n-- after a save and a load on the card --\n");
    mr_interp_start("pause_back");
    if (!run_to("A line after the card.")) {
        printf("fixture: never reached the line after the card\n");
        return 1;
    }
    settle(600);
    back_once();
    check_on_card("before the save");

    mr_save_write("1-7", g_st);
    mr_interp_start("start");
    settle(300);
    T += 60000;
    mr_interp_update(T);
    mr_save_load("1-7");
    settle(600);
    check_on_card("after the load");

    /* No input between the load and this, deliberately: an advance is what
     * used to paper over the menu version of this. */
    back_once();
    checks("one step back leaves the card", line_now(),
           "A line before the card.");
    checkb("and is not waiting on it again", mr_interp_waiting(), 0);

    /* --- rolling back to a card that came out of the file ---
     *
     * The section above loads *on* the card, so the screen comes back through
     * the save's own display list. This one saves on the line after it and
     * rolls back, so the card has to come back out of the checkpoint -- and
     * a checkpoint's screen list was not written to the file at all. The
     * background and the sprites were, so the card rolled back to its own
     * `scene black` with nothing on it: a black screen. Reported from play.
     *
     * In-session rollback never showed this, which is the whole point of
     * doing it twice here: the live point holds the list and only the trip
     * through the file lost it.
     */
    printf("\n-- rolling back to a card restored from a slot --\n");
    mr_interp_start("pause_back");
    if (!run_to("A line after the card.")) {
        printf("fixture: never reached the line after the card\n");
        return 1;
    }
    settle(600);
    mr_save_write("1-7", g_st);
    mr_interp_start("start");
    settle(300);
    T += 60000;
    mr_interp_update(T);
    mr_save_load("1-7");
    settle(600);
    checks("the slot comes back on the line after the card", line_now(),
           "A line after the card.");
    checkb("with the card down", screen_up("title"), 0);

    back_once();
    check_on_card("back once, from the slot");

    /* --- walking back and forth across two cards ---
     *
     * Reported from play: after a few rollforwards onto a title card, a
     * rollback onto a card drew the *previous* chapter's title. Live, not
     * only from a slot, and not every time -- which is what a step whose
     * variable slice is wrong looks like from the player's side, since it
     * needs a particular back-and-forth to show.
     *
     * So this walks rather than probes: all the way back one step at a time,
     * all the way forward again, then a handful of mixed cycles, checking the
     * `pb`/`title` invariant after every single step. The title is what the
     * card draws, so a mismatch here *is* the wrong card text.
     */
    printf("\n-- back and forth across two cards --\n");
    /* A real new playthrough first. mr_interp_start runs the init phase and
     * jumps, and that is all: it keeps the variables, the scene and the
     * rollback log of whatever ran before it. Start() is the engine's own
     * reset -- clear_playthrough(1) plus mr_scene_clear -- so the log below
     * holds this section's points and nothing else, and a walk cannot
     * wander back into an earlier section's. */
    mr_interp_action("Start()");
    settle(100);
    mr_interp_start("pause_back");

    /* Learn what the first card measures, on the way past it. Both references
     * come from a plain forward playthrough, settled -- the state every later
     * reading is compared against. */
    if (!run_to_card()) {
        printf("fixture: never reached the first card\n");
        return 1;
    }
    settle(900);
    mr_scene_render(g_st, T);
    g_ref1 = lit();
    checkb("the first card draws something", g_ref1 > 0, 1);

    if (!run_to("One more line past the second card.")) {
        printf("fixture: never reached the end of the label\n");
        return 1;
    }
    settle(600);
    checkb("the second card has been passed", mr_var_get_int("pb", -99), 4);
    checks("and its title is the live one", title_now(), TITLE2);

    /* The second card, measured the same way: back onto it, settled. */
    back_once();
    back_once();
    settle(900);
    mr_scene_render(g_st, T);
    g_ref2 = lit();
    checkb("standing at the second card", mr_var_get_int("pb", -99), 3);
    checkb("which draws a wider title than the first",
           g_ref2 > g_ref1 * 2, 1);
    printf("   (card one %ld lit, card two %ld lit)\n", g_ref1, g_ref2);

    /* And back to the end, so the walk below starts where it used to. */
    while (mr_rollforward_depth() > 0)
        forward_once();

    {
        int i, j, n;

        /* All the way back. Eight steps is past the first card, which is
         * four says and two pauses from the end. */
        for (i = 0; i < 8; i++) {
            back_once();
            check_pair("back", i + 1);
        }
        /* And all the way forward again. */
        for (i = 0; i < 8; i++) {
            n = mr_rollforward_depth();
            if (n <= 0)
                break;
            forward_once();
            check_pair("forward", i + 1);
        }

        /* Mixed: back j, forward j-1, repeatedly, so a card is landed on
         * from both directions and the forward chain is re-entered after a
         * fresh rollback rather than only walked once. */
        for (j = 2; j <= 5; j++) {
            for (i = 0; i < j; i++) {
                back_once();
                check_pair("cycle back", j * 10 + i);
            }
            for (i = 0; i < j - 1; i++) {
                if (mr_rollforward_depth() <= 0)
                    break;
                forward_once();
                check_pair("cycle forward", j * 10 + i);
            }
        }

        printf("%-54s %6d want %6d  %s\n",
               "every stop had the title of its own card", pairs_bad, 0,
               pairs_bad ? "** FAIL **" : "ok");
        printf("   (%d stops checked)\n", pairs_checked);
    }

    /* --- the same thing, walked at random ---
     *
     * The ordered walk above is the sequence a person writing a test thinks
     * of. The report is not that shape: "it happens live too, and reproducing
     * is not always -- it seems to only happen if I rollforwarded a few times
     * to a title card". So this mixes the three keys in a fixed pseudo-random
     * order and, deliberately, does *not* let everything settle between them:
     * a quarter of the steps are taken one frame in, with a `with fade` still
     * running, because that is a player pressing Page Up on a title card
     * rather than a harness waiting for one.
     *
     * The invariant is the same and is checked after every step. An LCG with
     * a written-down seed, so a failure is reproducible and shrinkable rather
     * than a once-seen anomaly.
     */
    printf("\n-- and walked at random --\n");
    {
        unsigned long seed = 20261013UL;
        int step, before = pairs_bad;

        pairs_checked = 0;
        mr_interp_action("Start()");
        settle(100);
        mr_interp_start("pause_back");
        settle(600);

        for (step = 0; step < 600; step++) {
            int pick;

            seed = seed * 1103515245UL + 12345UL;
            pick = (int)((seed >> 16) % 12);

            if (pick == 10) {
                /* Skipping, which the second report names: "when skipping
                 * sometimes they appear totally black when rolling back". A
                 * skip crosses a pause by clearing the wait, so it is the one
                 * way a card is passed without an interaction of its own. */
                mr_interp_skip_toggle();
                trail(mr_interp_skipping() ? "skip-on" : "skip-off");
                settle(300);
            } else if (pick == 11) {
                mr_interp_skip_stop();
                trail("skip-stop");
            } else if (pick < 4) {
                /* Not past the end of the label: afm_pauses is next, and its
                 * hard pauses are not something a frame-stepper leaves. */
                if (strcmp(line_now(), "One more line past the second card.")) {
                    mr_interp_advance();
                    trail("adv");
                } else {
                    trail("adv-held");
                }
            } else if (pick < 7) {
                mr_interp_rollback();
                trail("back");
            } else if (pick < 9) {
                mr_interp_rollforward();
                trail("fwd");
            } else {
                /* A frame with nothing pressed, to let a transition finish
                 * and a timed pause expire. */
                trail("wait");
                settle(400);
            }

            /* One frame, then the check: the state has to be right from the
             * moment the step is taken, not once everything has settled. */
            frame();
            check_pair("random", step);

            /* And again after settling, which is where a transition's own
             * commit could put a variable back -- and the only state in which
             * the stage is worth measuring. */
            settle(500);
            check_pair("random settled", step);
            check_card_pixels("random", step);
        }

        mr_interp_skip_stop();
        printf("%-54s %6d want %6d  %s\n",
               "the random walk kept the invariant too",
               pairs_bad - before, 0,
               (pairs_bad - before) ? "** FAIL **" : "ok");
        printf("   (%d checks over 600 steps)\n", pairs_checked);
        printf("%-54s %6d want %6d  %s\n",
               "and every card it stopped at drew its own title",
               pix_bad, 0, pix_bad ? "** FAIL **" : "ok");
        printf("   (%d card renders measured)\n", pix_checked);
    }

    /* --- two presses in one frame ---
     *
     * From the log of a failing session: a rollback whose `over pc` is -1,
     * meaning nothing was recorded for the way forward, followed by a run of
     * `rollforward: 0 variables restored (nothing recorded for this step)`.
     * The same step repeated later, slowly, recorded its variable and
     * restored it.
     *
     * mr_rollback_back does not finish a step on its own: it leaves the
     * caller to resume the target's statement, and *that* statement's
     * checkpoint is what moves the head, counts the point and decrements the
     * forward count. All of that happens inside mr_interp_update. Keys do
     * not: they are handled as they arrive, so holding Page Up delivers
     * several rollbacks between two updates, and every one after the first
     * finds the log in the half-finished state the previous one left -- the
     * newest point is no longer where the player is, so the point being
     * stepped over is not recognised and nothing is captured for it, while
     * the forward count is incremented anyway.
     *
     * Ren'Py cannot hit this: its rollback raises and restarts the
     * interaction, so one press is one rollback.
     *
     * So: two presses with no update between them, then forward again. The
     * variables have to arrive with the pc either way.
     */
    printf("\n-- two rollbacks in one frame, then forward --\n");
    {
        int i, before = pairs_bad;

        mr_interp_action("Start()");
        settle(100);
        mr_interp_start("pause_back");
        if (!run_to("One more line past the second card.")) {
            printf("fixture: never reached the end of the label\n");
            return 1;
        }
        settle(600);
        g_trail_n = 0;

        /* Back across both cards, two presses per frame. */
        for (i = 0; i < 6; i++) {
            mr_interp_rollback();
            mr_interp_rollback();       /* no update in between */
            trail("back-back");
            settle(400);
            check_pair("double back", i);
        }

        /* And forward again, one press per frame, which is the direction the
         * missing capture shows up in. */
        for (i = 0; i < 12; i++) {
            if (mr_rollforward_depth() <= 0)
                break;
            forward_once();
            check_pair("forward after double back", i);
            check_card_pixels("forward after double back", i);
        }

        printf("%-54s %6d want %6d  %s\n",
               "a doubled press keeps the variables with the pc",
               pairs_bad - before, 0,
               (pairs_bad - before) ? "** FAIL **" : "ok");

        /* Held down harder, and mixed with the other two keys in the same
         * frame -- an advance and a rollforward leave the same statement
         * pending, so they are the same hazard from a different direction. */
        before = pairs_bad;
        mr_interp_action("Start()");
        settle(100);
        mr_interp_start("pause_back");
        run_to("One more line past the second card.");
        settle(600);
        g_trail_n = 0;

        for (i = 0; i < 5; i++) {
            mr_interp_rollback();
            mr_interp_rollback();
            mr_interp_rollback();
            trail("back x3");
            settle(400);
            check_pair("triple back", i);
            check_card_pixels("triple back", i);
        }
        for (i = 0; i < 10; i++) {
            if (mr_rollforward_depth() <= 0)
                break;
            mr_interp_rollforward();
            mr_interp_rollforward();
            trail("fwd x2");
            settle(400);
            check_pair("double forward", i);
            check_card_pixels("double forward", i);
        }
        for (i = 0; i < 6; i++) {
            mr_interp_advance();
            mr_interp_rollback();       /* in the same frame as the advance */
            trail("adv+back");
            settle(400);
            check_pair("advance then back", i);
            check_card_pixels("advance then back", i);
        }

        printf("%-54s %6d want %6d  %s\n",
               "and so does three, or two keys at once",
               pairs_bad - before, 0,
               (pairs_bad - before) ? "** FAIL **" : "ok");
    }

    /* --- landing on a card by every route there is ---
     *
     * The random walk above measured three card renders in six hundred
     * steps: it wanders, and a card is two statements out of twenty. Both
     * reports are specifically about *arriving* at a card, by rolling forward
     * or after skipping, so this arrives on purpose and measures every time.
     *
     * Four routes, because the reports name two of them and the other two are
     * the controls that say whether the route is what matters:
     *
     *   advance   played into normally
     *   back      rolled back onto, which is the fixed case above
     *   forward   rolled *forward* onto -- "it only happens if I
     *             rollforwarded a few times to a title card"
     *   skip      skipped across and then rolled back onto -- "when
     *             skipping sometimes they appear totally black"
     */
    printf("\n-- arriving at a card by each route --\n");
    {
        int round, before_pix = pix_bad, before_pair = pairs_bad;
        int reached[4];
        static const char *route_name[4] = { "advance", "back", "forward",
                                             "skip" };
        int i;

        for (i = 0; i < 4; i++)
            reached[i] = 0;
        pix_checked = 0;

        for (round = 0; round < 32; round++) {
            int route = round % 4;

            /* A fresh playthrough every fourth round, so a route is tried
             * both on a clean log and on one that has been walked over. */
            if (round % 8 == 0) {
                mr_interp_action("Start()");
                settle(100);
                mr_interp_start("pause_back");
                settle(300);
                g_trail_n = 0;
                trail("restart");
            }

            switch (route) {
            case 0:
                trail("adv-to-card");
                if (!run_to_card())
                    break;
                reached[route]++;
                break;

            case 1:
                /* Past both cards, then back until one is up. */
                trail("to-end");
                run_to("One more line past the second card.");
                settle(300);
                for (i = 0; i < 14 && !at_card(); i++) {
                    back_once();
                    trail("back");
                }
                if (at_card())
                    reached[route]++;
                break;

            case 2:
                /* Back past a card to build a future, then forward onto it.
                 * A few times over, which is the reported shape. */
                trail("to-end");
                run_to("One more line past the second card.");
                settle(300);
                for (i = 0; i < 14; i++) {
                    back_once();
                    trail("back");
                }
                for (i = 0; i < 14 && !at_card(); i++) {
                    if (mr_rollforward_depth() <= 0)
                        break;
                    forward_once();
                    trail("fwd");
                }
                if (at_card())
                    reached[route]++;
                break;

            default:
                /* Skip across everything, stop skipping, then roll back. */
                trail("skip");
                mr_interp_skip_toggle();
                for (i = 0; i < 120; i++) {
                    frame();
                    if (!strcmp(line_now(),
                                "One more line past the second card."))
                        break;
                }
                mr_interp_skip_stop();
                settle(300);
                for (i = 0; i < 14 && !at_card(); i++) {
                    back_once();
                    trail("back");
                }
                if (at_card())
                    reached[route]++;
                break;
            }

            if (at_card()) {
                settle(900);
                check_pair(route_name[route], round);
                check_card_pixels(route_name[route], round);
            }
        }

        mr_interp_skip_stop();
        printf("   routes reached: advance %d, back %d, forward %d, skip %d\n",
               reached[0], reached[1], reached[2], reached[3]);
        for (i = 0; i < 4; i++)
            checkb(route_name[i][0] ? "the route reached a card at all"
                                    : "", reached[i] > 0, 1);
        printf("%-54s %6d want %6d  %s\n",
               "every arrival drew the card's own title",
               pix_bad - before_pix, 0,
               (pix_bad - before_pix) ? "** FAIL **" : "ok");
        printf("%-54s %6d want %6d  %s\n",
               "and had the card's own title in the store",
               pairs_bad - before_pair, 0,
               (pairs_bad - before_pair) ? "** FAIL **" : "ok");
        printf("   (%d card renders measured)\n", pix_checked);
    }

    remove("SAVEP1S7.SAV");
    printf("\n%s\n", fails ? "** FAILURES **" : "all ok");
    return fails ? 1 : 0;
}

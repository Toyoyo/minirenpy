/* interp.c - executing the compiled statement array.
 *
 * The interpreter is a frame-driven state machine rather than a loop that
 * blocks, because Windows 3.1 multitasks cooperatively: anything that blocks
 * waiting for a click would stop the message queue and freeze the system.
 * mr_interp_update runs statements until it reaches one that has to wait, then
 * returns to the frame loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "interp.h"
#include "scene.h"
#include "image.h"
#include "expr.h"
#include "audio.h"
#include "vfs.h"       /* whether a cutscene's soundtrack was extracted */
#include "movie.h"     /* and whether the cutscene itself will decode */
#include "yuv.h"       /* mr_yuv_fit, so the stand-in sits where the movie would */
#include "style.h"
#include "save.h"
#include "history.h"
#include "stmtid.h"
#include "datefmt.h"
#include "slrender.h"
#include "tl.h"
#include "persist.h"
#include "list.h"
#include "ui.h"        /* mr_ui_dialogue_rect, for the partial-frame test */
#include "musicroom.h"
#include "pyfn.h"
#include "rhythm.h"
#include "lex.h"      /* mr_skip_ws */
#include "config.h"   /* scriptcheck */

typedef enum {
    ST_RUN = 0,       /* executing statements */
    ST_SAY,           /* dialogue on screen, waiting for a click */
    ST_TRANS,         /* a `with` transition is playing */
    ST_WAIT,          /* a timed pause */
    ST_WAITCLICK,     /* an untimed pause */
    ST_MENU,          /* waiting for a menu choice */
    ST_SCREEN,        /* a screen owns the display, e.g. the main menu */
    ST_RHYTHM,        /* the rhythm minigame owns it (rhythm.c) */
    ST_END
} runstate;

/* How deep `call` can nest. Tied to the save's stack rather than being its
 * own number: a save writes at most MR_SAVE_STACK return addresses, and the
 * capture loops stop there, so a call stack deeper than that would be
 * *silently* truncated on the way into a slot and the save would resume with
 * the wrong return path. They were two independent 32s with nothing keeping
 * them equal, which is a trap for whoever raises one. */
#define MR_CALLSTACK MR_SAVE_STACK

/* What renpy.version_only answers with. The number is the Ren'Py whose
 * script and screen language this implements -- the same release as
 * renpy-8.5.3-source, which every behaviour here was checked against -- and
 * the suffix is because the About screen renders it as "Conçu avec Ren'Py
 * [renpy.version_only]", where a bare 8.5.3 would be a claim to be Ren'Py. */
#define MR_RENPY_VERSION "8.5.3 (minirenpy)"

static mr_script *g_s;
static int        g_pc;
static runstate   g_state;
static long       g_now;

static int        g_stack[MR_CALLSTACK];
static int        g_sp;

/* Current dialogue. */
static char   g_who[MR_MAX_NAME];
static char   g_what[MR_MAX_TEXT];
/* The current line's translation identifier, so the displayed text can be
 * looked up afresh every frame: switching language has to change the line the
 * player is already looking at. The original stays in g_what. */
static char   g_tlid[MR_MAX_NAME + 24];
/* The statement index of the say that put g_what on screen. Rolling back to a
 * line means resuming at its say, so this is what a checkpoint for the current
 * line has to name; g_pc has already moved past it. */
static int    g_say_pc = -1;
/* The same thing for an untimed pause: the statement index of the `pause` or
 * the `$ renpy.pause()` the player is waiting at, or -1 when they are not
 * waiting at one. g_pc has already moved past it, exactly as it has past a
 * say, and both the pause's own checkpoint and mr_rollback_back's "is the
 * newest checkpoint what is on screen?" test need the statement rather than
 * the one after it. */
static int    g_pause_pc = -1;
static mr_u32 g_whocolor;
static int    g_has_dialogue;

/* The dialogue window, which is state of its own rather than a consequence of
 * there being a line on screen.
 *
 * `window show` puts it up and `window hide` takes it down, and it stays
 * however it was left -- through `show`, `play`, a `with dissolve`, anything.
 * A line of dialogue shows it for as long as the line is up even when it has
 * been hidden, which is Ren'Py's rule that a say statement brings its own
 * window with it.
 *
 * `window auto` is Ren'Py's default (`config.window = "auto"` in options.rpy)
 * and means the first say puts the window up by itself. An explicit `window
 * show` or `window hide` turns that off for good, which is why a `window
 * hide` is not undone by the next line of dialogue: the line shows the window
 * while it is being read and the window goes back down afterwards. Without
 * the auto flag the game would open with an empty box over its first fade,
 * since nothing shows the window before chap0's first line. */
static int    g_window_shown;
static int    g_window_auto = 1;

/* Typewriter reveal. Ren'Py reveals dialogue at preferences.text_cps
 * characters per second, with 0 meaning instant, and a click during the
 * reveal finishes the line rather than advancing past it. */
static long   g_say_start;
static int    g_say_complete;

/* Menu.
 *
 * Two numberings, because a choice can carry a condition and so a menu does
 * not always offer everything it declares:
 *
 *   - the *declared* index, 0..g_menu_count-1, which is the offset of a
 *     CHOICE statement from g_menu_first. This is the stable one: it does not
 *     move when a condition changes, so it is what the rollback log records.
 *     Ren'Py's menu returns the same thing -- display_menu's value is the
 *     item's index into the unfiltered list (ast.py:1809).
 *
 *   - the *offered* index, 0..g_menu_nvis-1, which is a row on screen.
 *     g_menu_vis maps it back. Everything outside this file speaks in offered
 *     indices, because a row is what the player clicks.
 */
static int         g_menu_first;   /* index of the first CHOICE statement */
static int         g_menu_count;   /* choices declared */
static const char *g_menu_text[16];
static int         g_menu_vis[16]; /* offered row -> declared index */
static int         g_menu_sens[16];/* and whether that row may be chosen */
static int         g_menu_nvis;

static long g_wait_until;
/* When the current untimed pause began, for the auto-forward clock. Latched
 * in mr_interp_update rather than where the pause is entered; see there. */
static long g_pause_since;
static int  g_hard_wait;   /* a hard pause ignores clicks */

/* A movie cutscene is being stood in for. Ren'Py passes suppress_overlay=True
 * for the duration (exports/mediaexports.py:72), and it matters here rather
 * than being a nicety: the splashscreen runs before the main menu, so
 * `main_menu` is 0 and nothing else would stop the quick menu being drawn
 * over the intro. Cleared by asking whether the wait it belongs to is still
 * running, rather than at each of the ways that wait can end. */
static int  g_cutscene;
static long g_cut_start;    /* when the cutscene began */
static int  g_cut_timed;    /* its length has been read off the track */

/* A real movie is decoding, rather than the poster-frame stand-in.
 *
 * The two differ in who decides when the cutscene is over. For the stand-in
 * it is the clock: the wait runs out when the soundtrack's length says it
 * should. For a movie it is movie.c -- mr_movie_update returning 0 means the
 * last frame has been shown and the last of the sound has been heard -- and
 * the clock is only a backstop against a truncated file.
 *
 * It also decides who draws. While this is set, mr_movie_update puts every
 * frame on screen itself and main.c composites nothing; see mr_interp_movie
 * and its use in the frame loop. */
static int  g_cut_movie;
static int  g_cut_loops;    /* extra repeats still to play; -1 is forever */

/* A cutscene is running, by either route. Distinct from g_cutscene, which
 * overlay_sync owns and clears on its own schedule; this one is the teardown's
 * and nothing else touches it. */
static int  g_cut_running;

/* The music that `stop_music` stopped, to be started again afterwards.
 *
 * Ren'Py does not resume it -- it *restarts* it, and the difference is
 * visible. movie_cutscene sets force_stop on the music channel and clears it
 * after (exports/mediaexports.py:56 and :79); force_stop empties the
 * channel's queue but keeps the loop entries, `self.queue =
 * self.queue[-len(self.loop):]` (audio/audio.py:508-516), so when the flag is
 * cleared the looping track is queued again from its beginning. A track that
 * was *not* looping has its queue emptied and does not come back at all.
 *
 * Both halves are copied here, which is why the loop flag is recorded and not
 * just the name. There is no seek-to-position in this mixer and this is the
 * behaviour that wants matching anyway.
 *
 * This was missed the first time round: the music was stopped and the restart
 * was left to "the script's next `play music`". Most scripts have one, and
 * Miwa's chap0 does not -- it plays its track once at the top of the label and
 * expects it to still be there after the cutscene. The symptom was a chapter
 * that fell silent for good. */
static char g_cut_music[MR_PATH_MAX];
static int  g_cut_music_loop;

/* The display list the stand-in covered up, to be put back when it ends.
 *
 * Only the stand-in needs this, and that asymmetry is the whole reason it was
 * missing. A movie never touches the stage -- its picture goes straight to the
 * presenter -- so the scene the story was on is still there underneath and
 * mr_invalidate() is the entire teardown. The stand-in is an ordinary image on
 * the stage, so putting it up *destroys* what was there, and nothing put it
 * back: the story resumed with the poster frame still as the scene until the
 * script's next `scene` statement happened to replace it. Miwa's chap0 has one
 * on the line after the cutscene, which is why this survived.
 *
 * Ren'Py's movie_cutscene does not touch the scene at all -- it displays over
 * everything and then stops displaying (exports/mediaexports.py:29) -- so
 * "whatever was there before is there after" is the behaviour being matched,
 * not an invention.
 *
 * A static rather than a local: an mr_scenelist is about 5KB and the stack is
 * committed up front on Win32s. See NOTES.md on stack. */
static mr_scenelist g_cut_scene;
static int          g_cut_scene_saved;

/* How long a cutscene lasts before its soundtrack's length is known, and how
 * long it lasts if there is no soundtrack at all. Long enough that the poster
 * frame registers as a frame rather than a flicker. */
#define MR_CUT_FLOOR_MS 2000

/* Slack on the *movie* deadline, which is a backstop and not the end
 * condition -- the end is mr_movie_update returning 0.
 *
 * Without it the deadline is the container's own duration, measured from the
 * statement rather than from the first sample leaving the card, and a
 * cutscene cannot help taking slightly longer than its duration to play:
 * opening the file and filling the mixer's runway happen first, and the sound
 * finishes a queue's depth after the mixer does. So the backstop expired
 * before the movie ended and cut the last few frames and the last words off
 * -- reported from a real run on an eleven-second test video as "maybe 0.5s"
 * of both missing, which is the queue plus the startup.
 *
 * Three seconds, because the cost of too much is a held last frame on a file
 * whose duration was wrong, and the cost of too little is the end of every
 * cutscene. */
#define MR_CUT_BACKSTOP_SLACK_MS 3000

/* The splashscreen is running. Ren'Py suppresses the overlay for the whole of
 * it, not just for a cutscene inside it: _splashscreen sets
 * suppress_overlay=True before jumping (00start.rpy:195, on by default at
 * :63). That width matters here because overlay_sync runs near the top of
 * mr_interp_update, before any statement executes -- so on the frame the
 * splashscreen begins, a flag set by the cutscene statement is set too late
 * and the quick menu is drawn over the intro for a frame. Cleared when the
 * main menu comes up, which is where a splashscreen's `return` ends. */
static int  g_in_splash;

/* Save slots. The screens name a slot by its number within the current page,
 * so the page is state of its own, exactly as it is in Ren'Py. */
static char g_file_page[16] = "1";
static char g_screen_now[MR_MAX_NAME];   /* the menu screen on display */
static int  g_on_save_screen;

/* What the story was doing when the menu opened, so closing it puts things
 * back rather than running on. Without this, dismissing the menu resumed at
 * the statement after the current line -- closing with a right-click looked
 * exactly like clicking to advance. */
static runstate g_pre_menu = ST_RUN;

/* A `modal True` screen over the menu -- the confirm dialog -- and the
 * no_action it was given, which a right-click or Escape answers with. */
static char g_modal_screen[MR_MAX_NAME];
static char g_modal_no[256];

/* Combine the current page with the slot number the screen passed. */
static void file_slot(mr_value *argv, int argc, char *out, int outsz)
{
    long n = 1;

    if (argc > 0) {
        if (argv[0].type == MR_V_INT)
            n = argv[0].i;
        else if (argv[0].type == MR_V_FLOAT)
            n = (long)argv[0].f;
    }
    sprintf(out, "%.8s-%ld", g_file_page, n);
    (void)outsz;
}

/* Take a rollback checkpoint for the line on screen. do_say calls this as it
 * displays a line; the language switch calls it again, because it has just
 * thrown the log away and without a fresh checkpoint the line the player is
 * looking at would be the one line they could never come back to. */
static void say_checkpoint(void)
{
    mr_gamestate g;

    if (g_say_pc < 0)
        return;
    if (!mr_var_get_int("config.rollback_enabled", 1))
        return;

    mr_interp_capture(&g);
    g.pc = g_say_pc;

    /* The captured scene may have the game menu in it -- the language switch
     * takes a checkpoint from inside the preferences screen. Interface
     * screens are dropped by mr_rollback_checkpoint rather than here, so no
     * future caller has to know to do it. */
    mr_rollback_checkpoint(&g, g_say_pc, MR_CP_SAY);
}

/* An untimed pause is a checkpoint too, named by the pause statement itself.
 *
 * Ren'Py checkpoints an untimed pause and not a timed one: renpy.pause
 * defaults `checkpoint` to False when it was given a delay and True when it
 * was not (statementexports.py:151-154), and the call that follows the
 * interaction is a hard checkpoint in the untimed case
 * (`hard=config.pause_after_rollback or (delay is None)`, :191). The `pause`
 * statement goes through the same function, so an untimed `pause` is a
 * checkpoint and `pause 2.0` is not.
 *
 * This is the chapter title cards: `show screen title` then `$ renpy.pause()`
 * with no delay. Saving and loading on one already worked -- the screen is
 * part of a captured state -- but the card was invisible to the log, so
 * rolling back from the line after it crossed it without stopping and there
 * was no way back to it. Reported from play.
 *
 * `pc` is passed in rather than read from g_pc because the two callers differ:
 * the `pause` statement is at g_pc when it runs, and renpy.pause() runs from
 * inside a `$` line, whose statement is what has to be resumed. Either way
 * re-running it pauses again, with the scene the checkpoint restored -- so
 * rolling back to a title card puts the card up and waits on it, with no
 * special case in apply_checkpoint.
 *
 * Taken as the pause begins, not when it ends. Ren'Py's call is after the
 * interaction, but the same is true of its say checkpoint and say_checkpoint
 * already runs when the line is displayed; see the note on the third
 * condition in mr_rollback_checkpoint for what that costs and why it is not
 * worth splitting the two apart here. The state captured is identical either
 * way: nothing executes while a pause waits. */
static void pause_checkpoint(long pc)
{
    mr_gamestate g;

    g_pause_pc = (int)pc;

    if (!mr_var_get_int("config.rollback_enabled", 1))
        return;

    mr_interp_capture(&g);
    g.pc = pc;
    mr_rollback_checkpoint(&g, pc, MR_CP_PAUSE);
}

/* A choice menu is a checkpoint too, named by the menu statement itself.
 *
 * Ren'Py's display_menu ends with `renpy.exports.checkpoint(rv)`
 * (menuexports.py:432), so the menu is a point the player can come back to
 * and the chosen option is recorded there. Without it a menu was invisible to
 * the log in both directions: rolling back crossed it without stopping, and
 * the forward log had nothing to stop at either, so rollforward jumped over
 * the menu and restored whichever branch had been taken before. Reported from
 * play as "rollback/rollforward entirely skips choices".
 *
 * The pc is the menu statement, not the option: exec_one leaves g_pc on the
 * `menu` while it waits, and mr_interp_choose is what moves it. So restoring
 * this point and resuming re-executes the menu, which offers it again -- the
 * same way restoring a say point redisplays its line, and with no extra code
 * in apply_checkpoint. */
static void menu_checkpoint(int has_caption)
{
    mr_gamestate g;

    if (!mr_var_get_int("config.rollback_enabled", 1))
        return;

    mr_interp_capture(&g);
    g.pc = g_pc;
    mr_rollback_checkpoint(&g, g_pc,
                           has_caption ? MR_CP_MENU_CAPTION : MR_CP_MENU);
}

void mr_interp_hide_modal(void);
static int show_confirm(const char *message, const char *yes, const char *no);
static void clear_playthrough(int reset_vars);
static void notify(const char *msg);

/* What FilePageNameInputValue() evaluates to. Recognised by the screen
 * language's `input`, which turns it into the current page's name. */
static char g_page_pattern[64] = "Page {}";
static char g_page_auto[64]    = "Automatic saves";
static char g_page_quick[64]   = "Quick saves";

/* --- builtin calls reachable from `$` lines --- */

/* Look up a keyword argument by name, or return NULL. */
static mr_value *kwarg(mr_value *argv, const char **argname, int argc,
                       const char *want)
{
    int i;
    for (i = 0; i < argc; i++) {
        if (argname[i] && !strcmp(argname[i], want))
            return &argv[i];
    }
    return NULL;
}

static double numarg(mr_value *argv, const char **argname, int argc,
                     int pos, const char *name, double dflt)
{
    mr_value *v = name ? kwarg(argv, argname, argc, name) : NULL;

    if (!v && pos >= 0 && pos < argc && !argname[pos])
        v = &argv[pos];
    if (!v)
        return dflt;
    if (v->type == MR_V_INT)
        return (double)v->i;
    if (v->type == MR_V_FLOAT)
        return v->f;
    return dflt;
}

/* --- preferences ----------------------------------------------------------
 *
 * Each preference is stored under its Ren'Py name in the ordinary variable
 * store, so saving picks it up for free. A bar reads and writes a *fraction*
 * of the control's range, so the range lives here, in one table, rather than
 * being implied in several places.
 */

typedef struct {
    const char *name;
    const char *var;
    double      lo, hi;
    double      dflt;
    /* Ren'Py's max_is_zero: the far right of the bar stores 0, meaning "no
     * limit". Text speed and auto-forward both work that way, so dragging
     * fully right makes text instant rather than merely very fast. */
    int         max_is_zero;
} pref_range;

static const pref_range g_prefs[] = {
    { "text speed",        "_preferences.text_cps",     0.0, 200.0, 50.0, 1 },
    { "auto-forward time", "_preferences.afm_time",     0.0,  30.0, 15.0, 1 },
    { "music volume",      "_preferences.volume.music", 0.0,   1.0,  1.0, 0 },
    { "sound volume",      "_preferences.volume.sfx",   0.0,   1.0,  1.0, 0 },
    { "voice volume",      "_preferences.volume.voice", 0.0,   1.0,  1.0, 0 },
    { NULL, NULL, 0.0, 0.0, 0.0, 0 }
};

static const pref_range *pref_find(const char *name)
{
    int i;
    for (i = 0; g_prefs[i].name; i++) {
        if (!strcmp(g_prefs[i].name, name))
            return &g_prefs[i];
    }
    return NULL;
}

/* A preference's stored value, in its own units. */
static double pref_value(const pref_range *r)
{
    mr_value v;

    if (!mr_var_get(r->var, &v))
        return r->dflt;
    if (v.type == MR_V_FLOAT)
        return v.f;
    return (double)v.i;
}

/* The current value of a preference, as a 0..1 fraction of its range. */
static double pref_get(const char *name)
{
    const pref_range *r = pref_find(name);
    double cur;

    if (!r)
        return 0.0;

    cur = pref_value(r);

    if (r->hi <= r->lo)
        return 0.0;
    if (r->max_is_zero && cur == 0.0)
        return 1.0;
    return (cur - r->lo) / (r->hi - r->lo);
}

/* Push a preference's value through to whatever it actually controls. The
 * rhythm game's four hit channels are on the sfx mixer, so the sound slider
 * moves them too -- otherwise turning sound effects down would silence the
 * story's effects and leave the minigame at full volume. */
static void pref_apply(const pref_range *r, double value)
{
    if (!strcmp(r->name, "music volume"))
        mr_audio_set_volume(MR_CH_MUSIC, (float)value);
    else if (!strcmp(r->name, "sound volume")) {
        int i;
        mr_audio_set_volume(MR_CH_SOUND, (float)value);
        for (i = 0; i < MR_CH_HIT_COUNT; i++)
            mr_audio_set_volume((mr_channel)(MR_CH_HIT0 + i), (float)value);
    }
}

/* A preference's value as a 0..1 fraction, the same units the bar uses. */
double mr_interp_pref_get_fraction(const char *name)
{
    return pref_get(name);
}

/* Set a preference from a 0..1 fraction; used by a bar being dragged. */
void mr_interp_pref_set_fraction(const char *name, double frac)
{
    const pref_range *r = pref_find(name);
    mr_value v;
    double value;

    if (!r)
        return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    value = (r->max_is_zero && frac >= 1.0) ? 0.0
                                            : r->lo + frac * (r->hi - r->lo);
    mr_value_set_float(&v, value);
    mr_var_set(r->var, &v);
    pref_apply(r, value);
}

/* `Preference(name, value)` used as an action: the toggles and the radio
 * buttons. These are stored so the screens can show the right one selected. */
/* Mute-all lives under `_preferences.` like every other preference, and not
 * because it is tidier.
 *
 * mr_persist_is_persistent exempts that prefix from the baseline, and
 * `Start()` resets every non-exempt variable. Held anywhere else, the flag
 * was wiped by starting a game while the mixer stayed muted: the button came
 * back up unticked over silent audio, and the first click "turned on" a mute
 * that was already on. The prefix is also what puts it in PERSIST.SAV, which
 * is where Ren'Py keeps it too. */
#define MR_PREF_MUTE_ALL "_preferences.all mute"

/* Every channel the game can make noise on, including the rhythm game's four
 * hit channels -- muting has to cover them for the same reason the sound
 * slider does. */
static void mute_apply(int on)
{
    int i;

    mr_audio_set_mute(MR_CH_MUSIC, on);
    mr_audio_set_mute(MR_CH_SOUND, on);
    for (i = 0; i < MR_CH_HIT_COUNT; i++)
        mr_audio_set_mute((mr_channel)(MR_CH_HIT0 + i), on);
}

/* The variable a `Preference(name, ...)` toggle lives in.
 *
 * Ren'Py's Preference() names are not always its field names, and where they
 * differ the difference has to be written down once rather than guessed at
 * each call site: `Preference("auto-forward", "toggle")` is
 * ToggleField(_preferences, "afm_enable") (00preferences.rpy:396-404), not a
 * field called "auto-forward".
 *
 * This is how the quick menu's "Auto" button did nothing at all. pref_action
 * stored `_preferences.auto-forward`, mr_interp_pref_selected read the same
 * invented name back -- so the button even lit up correctly -- while the
 * auto-forward wait below reads the real field, `_preferences.afm_enable`,
 * which nothing ever wrote. Two halves agreeing with each other and with
 * nothing else; see the note above mr_interp_pref_selected. */
static void pref_var(char *out, const char *name)
{
    const char *field = name;

    if (!strcmp(name, "auto-forward"))
        field = "afm_enable";
    else if (!strcmp(name, "auto-forward after click"))
        field = "afm_after_click";

    sprintf(out, "_preferences.%.60s", field);
}

static void pref_action(const char *name, const mr_value *val)
{
    char var[80];
    const char *set = (val->type == MR_V_STR) ? val->s : "";
    int toggle = !strcmp(set, "toggle");

    if (!strcmp(name, "all mute")) {
        int on = !mr_var_get_int(MR_PREF_MUTE_ALL, 0);
        mr_var_set_int(MR_PREF_MUTE_ALL, on);
        mute_apply(on);
        return;
    }

    /* Everything else is either a named choice (display window/fullscreen) or
     * a boolean toggle (skip, transitions). */
    pref_var(var, name);
    if (toggle)
        mr_var_set_int(var, !mr_var_get_int(var, 0));
    else
        mr_var_set(var, val);
}

/* Whether `Preference(name, value)` used as an action reports itself as
 * selected. Ren'Py asks the Action object the call returns -- ToggleField
 * answers "is the field set", SetField "does the field already equal this" --
 * and the radio and check buttons draw the answer.
 *
 * This sits next to pref_action on purpose: it reads exactly the storage that
 * writes, and a name spelled differently on the two sides is a button that
 * silently never lights up. Both go through pref_var, which is also the only
 * place that knows a Preference name can differ from the field it stands for.
 *
 * Most of those names are ours, not Ren'Py's: it keys off fields like
 * `skip_unseen`, we key off the Preference's own first argument. That is fine
 * for a preference only these two functions touch -- but not for one the
 * interpreter reads as well, which is the trap pref_var exists to close. */
int mr_interp_pref_selected(const char *name, const char *value)
{
    char var[80];
    mr_value v;

    if (!name || !value)
        return 0;

    if (!strcmp(name, "all mute"))
        return mr_var_get_int(MR_PREF_MUTE_ALL, 0) != 0;

    pref_var(var, name);

    /* A toggle is selected when it is on; a named choice when it is the one
     * that is set. */
    if (!strcmp(value, "toggle"))
        return mr_var_get_int(var, 0) != 0;

    return mr_var_get(var, &v) && v.type == MR_V_STR && !strcmp(v.s, value);
}

/* --- skipping ---
 *
 * Ren'Py's model, which the three checkboxes under "Avance rapide" describe:
 *
 *   - Ctrl held skips while it is down; Tab toggles skipping on and off.
 *   - "Texte non lu" (skip_unseen) off means skipping stops the moment it
 *     reaches a line that has never been read. This is the one that needs
 *     the seen set in persist.c.
 *   - "Apres les choix" (skip_after_choices) off means a menu stops it.
 *   - Skipping suppresses transitions regardless of the transitions
 *     preference -- displayexports.py:687 does `if config.skipping: trans =
 *     None` after checking the preference.
 *
 * Skipped dialogue is not typed out: Ren'Py sets all_at_once in
 * character.py:666 for the same reason, and on this machine the typewriter is
 * most of what a frame costs.
 */
static int g_skip;          /* Tab toggled it on */
static int g_skip_held;     /* Ctrl is down */
static int g_say_seen;      /* had the current line been read before? */
static long g_say_done_ms;  /* when the line finished revealing, 0 if not yet */
static int  g_say_painted;  /* the finished line has been drawn at least once */

static int pref_on(const char *name, int dflt)
{
    return mr_var_get_int(name, dflt) != 0;
}

/* The skip is starting or stopping, so the reveal rules have changed.
 *
 * A line drawn "complete" because skipping shows it all at once has *not*
 * been drawn complete under the typewriter, and the skip stops precisely on
 * a line the player now has to read. Leaving g_say_painted set spends the
 * one guaranteed frame before the typewriter has run, and the last
 * characters of that first line after a skip are never painted -- which is
 * the reported bug, back again by a different route. */
static void skip_state_changed(void)
{
    g_say_painted = 0;
    mr_invalidate();
}

int mr_interp_skipping(void)
{
    if (!g_skip && !g_skip_held)
        return 0;

    /* `$ _skipping = False` fences off a stretch of script that must not be
     * skipped -- the credits sequence does exactly that, and being able to
     * skip the credits by leaning on Ctrl would defeat the point. Ren'Py
     * gates on it in three places (behavior.py:345 and :814,
     * 00keymap.rpy:238) and defaults it to True in defaultstore.py:55.
     * `config.allow_skipping` is the same switch at whole-game scope.
     *
     * Gating here rather than at the key handlers is what makes it a fence
     * rather than a cancel: a player who holds Ctrl across the credits is
     * still holding it afterwards, and skipping resumes on the far side,
     * which is Ren'Py's behaviour too -- the flag suppresses the skip, it
     * does not forget that one was asked for.
     *
     * Both default to allowed, so an unset name skips. */
    if (!mr_var_get_int("_skipping", 1))
        return 0;
    if (!mr_var_get_int("config.allow_skipping", 1))
        return 0;

    return 1;
}

void mr_interp_skip_hold(int down)
{
    int was = mr_interp_skipping();
    g_skip_held = down ? 1 : 0;
    if (was != mr_interp_skipping())
        skip_state_changed();
}

void mr_interp_skip_toggle(void)
{
    int was = mr_interp_skipping();
    g_skip = !g_skip;
    if (was != mr_interp_skipping())
        skip_state_changed();
}

void mr_interp_skip_stop(void)
{
    int was = mr_interp_skipping();
    g_skip = 0;
    g_skip_held = 0;
    if (was != mr_interp_skipping())
        skip_state_changed();
}

/* May skipping carry the *current* line? Asked once the line is up. */
static int skip_allows_line(void)
{
    if (!mr_interp_skipping())
        return 0;
    /* With "unseen" on, everything goes. Otherwise only what has been read
     * before -- which is what makes skip stop when it catches up with the
     * player's progress. */
    if (pref_on("_preferences.skip", 0))
        return 1;
    return g_say_seen;
}

/* Whether transitions should play at all. Two independent reasons not to. */
static int transitions_on(void)
{
    if (mr_interp_skipping())
        return 0;
    return pref_on("_preferences.transitions", 1);
}

/* Every `with` goes through here, so neither reason can be forgotten at one
 * of the call sites. Suppressing is not "an instant dissolve": it is the
 * scene being committed, which is exactly what mr_scene_transition already
 * does when handed MR_TRANS_NONE. */
static void start_transition(const mr_transition *t, long now)
{
    if (!transitions_on()) {
        mr_scene_commit();
        return;
    }
    mr_scene_transition(t, now);
}

/* Run the action a screen bound to this keysym, if any. Returns 1 if it did,
 * so the caller can leave the default keyboard behaviour alone otherwise.
 *
 * The action runs without a focus scope: a `key` binding belongs to the
 * screen, not to a button, so there is no `for` variable to flatten the way
 * add_focus has to. It lives here rather than in main.c so that it can be
 * driven from a harness -- main.c only maps virtual keys to keysym names. */
int mr_interp_screen_key(const char *sym)
{
    const char *act = sym ? mr_sl_key_action(sym) : NULL;

    if (!act || !*act)
        return 0;

    mr_interp_action(act);
    return 1;
}

/* Take the game menu down and hand the display back to the story. Every
 * menu screen shares the tag "menu", which is how they replace each other,
 * and how all of them can be dismissed at once. */
static void close_game_menu(void)
{
    int i;

    for (i = 0; i < g_s->nscreens; i++) {
        if (!strcmp(g_s->screens[i].tag, "menu"))
            mr_scene_hide_screen(g_s->screens[i].name);
    }
    mr_scene_commit();

    /* Back to whatever was waiting, not onward. ST_RUN would execute the
     * next statement immediately, which is indistinguishable from the player
     * having clicked to advance. */
    g_state = g_pre_menu;
    g_pre_menu = ST_RUN;
}

static int builtin_call(const char *name, mr_value *argv,
                        const char **argname, int argc, mr_value *result)
{
    memset(result, 0, sizeof(*result));

    /* --- audio --- */
    if (!strcmp(name, "renpy.music.play") || !strcmp(name, "renpy.music.queue")) {
        const char *file = (argc > 0 && argv[0].type == MR_V_STR) ? argv[0].s : NULL;
        mr_value *loop = kwarg(argv, argname, argc, "loop");
        /* if_changed is a keyword here, defaulting to false as it does in
         * Ren'Py; this game's one call passes it true. */
        mr_value *ifc = kwarg(argv, argname, argc, "if_changed");
        if (file)
            mr_audio_play_ex(MR_CH_MUSIC, file,
                          loop ? mr_value_truthy(loop) : 1,
                          (long)(numarg(argv, argname, argc, -1, "fadein", 0.0) * 1000.0),
                          (long)(numarg(argv, argname, argc, -1, "fadeout", 0.0) * 1000.0),
                          1.0f,
                          ifc ? mr_value_truthy(ifc) : 0);
        return 1;
    }
    if (!strcmp(name, "renpy.music.stop")) {
        mr_audio_stop(MR_CH_MUSIC,
                      (long)(numarg(argv, argname, argc, -1, "fadeout", 0.0) * 1000.0));
        return 1;
    }
    if (!strcmp(name, "renpy.sound.play")) {
        if (argc > 0 && argv[0].type == MR_V_STR)
            mr_audio_play(MR_CH_SOUND, argv[0].s, 0, 0, 1.0f);
        return 1;
    }

    /* The audio actions from 00action_audio.rpy: Play, Queue and Stop, all of
     * which take the channel as their first argument rather than as a
     * keyword. Running the effect when the call is evaluated is the same as
     * returning an action and invoking it later, because an action expression
     * is only evaluated once a button has been clicked -- the shortcut the
     * music room methods below already take.
     *
     * `Play("sound", config.sample_sound)` behind the preferences "Test"
     * button is what this is for. The channel string gets the same
     * music/everything-else mapping the `play` statement uses in parse.c, so
     * the two cannot disagree about what "sound" means.
     *
     * Ren'Py's loop default comes from the channel rather than the call:
     * music loops, sound does not. An explicit `loop=` still wins. */
    if (!strcmp(name, "Play") || !strcmp(name, "Queue") ||
        !strcmp(name, "Stop")) {
        mr_channel ch = MR_CH_SOUND;
        const char *file = NULL;
        long fadeout;

        if (argc > 0 && argv[0].type == MR_V_STR &&
            !strcmp(argv[0].s, "music"))
            ch = MR_CH_MUSIC;
        if (argc > 1 && argv[1].type == MR_V_STR && argv[1].s[0])
            file = argv[1].s;

        fadeout = (long)(numarg(argv, argname, argc, -1, "fadeout", 0.0)
                         * 1000.0);

        if (!strcmp(name, "Stop")) {
            mr_audio_stop(ch, fadeout);
        } else if (!file) {
            mr_log("audio action: %s with no file", name);
        } else if (!strcmp(name, "Queue")) {
            mr_audio_queue(ch, file, 1.0f);
        } else {
            mr_value *loop = kwarg(argv, argname, argc, "loop");
            mr_audio_play_ex(ch, file,
                             loop ? mr_value_truthy(loop)
                                  : (ch == MR_CH_MUSIC),
                             (long)(numarg(argv, argname, argc, -1,
                                           "fadein", 0.0) * 1000.0),
                             fadeout, 1.0f, 0);
        }
        return 1;
    }

    /* --- the rhythm minigame ---
     *
     * The extras menu builds its high-score table by calling
     * rhythm_game_get_highscore() once per song, so these have to answer even
     * when no run is in progress. They are builtins rather than pyfn bodies
     * because the .rpy versions index a dict, which pyfn has no notion of;
     * the storage lives in rhythm.c. Being builtins, they also take priority
     * over the `init python` definitions the parser saw. */
    if (!strcmp(name, "rhythm_game_get_highscore")) {
        mr_value_set_int(result,
            (argc > 0 && argv[0].type == MR_V_STR)
                ? mr_rhythm_highscore_get(argv[0].s) : 0);
        return 1;
    }
    if (!strcmp(name, "rhythm_game_set_highscore")) {
        int record = 0;
        if (argc > 1 && argv[0].type == MR_V_STR)
            record = mr_rhythm_highscore_set(argv[0].s, (long)argv[1].i);
        mr_value_set_int(result, record);
        return 1;
    }

    /* `call screen rhythm_game(audio, beatmap, background, stride)`. The
     * statement handler evaluates the call text and lands here. */
    if (!strcmp(name, "rhythm_game")) {
        const char *audio = (argc > 0 && argv[0].type == MR_V_STR) ? argv[0].s : "";
        const char *bmap  = (argc > 1 && argv[1].type == MR_V_STR) ? argv[1].s : "";
        const char *bg    = (argc > 2 && argv[2].type == MR_V_STR) ? argv[2].s : "";
        mr_value *stride  = kwarg(argv, argname, argc, "beatmap_stride");

        mr_value_set_int(result,
            mr_rhythm_start(audio, bmap, bg,
                            stride ? (int)stride->i : 2));
        return 1;
    }

    /* Ren'Py stores preferences per channel; the game only mutes and sets
     * music volume, so both map onto the mixer. */
    if (!strcmp(name, "preferences.set_mute")) {
        int on = (argc > 1) ? mr_value_truthy(&argv[1]) : 1;
        mr_audio_set_mute(MR_CH_MUSIC, on);
        mr_var_set_int("_mute_music", on);
        return 1;
    }
    if (!strcmp(name, "preferences.get_mute")) {
        mr_value_set_int(result, mr_var_get_int("_mute_music", 0));
        return 1;
    }
    if (!strcmp(name, "preferences.set_volume")) {
        double v = numarg(argv, argname, argc, 1, NULL, 1.0);
        mr_audio_set_volume(MR_CH_MUSIC, (float)v);
        return 1;
    }

    /* --- preferences ---
     *
     * Preference() is both a value and an action, and which one it is depends
     * on the call: `bar value Preference("music volume")` reads it, while
     * `action Preference("skip", "toggle")` writes it. One argument reads,
     * two write, exactly as in Ren'Py.
     *
     * Reading yields a fraction of the control's range, because that is what
     * a bar wants; pref_set turns a fraction back into the stored units. */
    if (!strcmp(name, "Preference")) {
        const char *p = (argc > 0 && argv[0].type == MR_V_STR) ? argv[0].s : "";

        if (argc < 2) {
            mr_value_set_float(result, pref_get(p));
            return 1;
        }
        pref_action(p, &argv[1]);
        return 1;
    }

    /* --- flow --- */
    if (!strcmp(name, "renpy.pause")) {
        double secs = numarg(argv, argname, argc, 0, NULL, -1.0);
        mr_value *hard = kwarg(argv, argname, argc, "hard");

        /* A pause is an interaction, and an interaction is what makes the
         * pending display list the one the player has actually seen -- the
         * same reason a line of dialogue commits.
         *
         * Without this, a `show` followed by a pause and then a
         * `with dissolve` faded in from before the show: the credits are
         * exactly that shape (`show credits` / `renpy.pause(5)` /
         * `show fin` / `with dissolve`), so the closing dissolve brought the
         * whole credit roll back and faded it in a second time. */
        mr_scene_commit();

        /* A hard pause cannot be clicked through. The credits roll is one, so
         * that the ninety seconds always play out.
         *
         * `hard` applies to both kinds, which it did not: it was read on the
         * timed branch only, so `renpy.pause(hard=True)` with no delay became
         * an ordinary click-dismissable pause. Ren'Py holds it -- the keysym
         * its SayBehavior dismisses on is `dismiss_hard_pause`, and
         * 00keymap.rpy:71 binds that to the empty list, so no key and no
         * mouse button can reach it. What ends it instead is auto-forward
         * (the afm check at behavior.py:732 is *above* the dismiss loop) and
         * skipping (the check at :814 is below it); with both off it does not
         * end at all, which is deliberate and which the docstring is rude
         * about -- "tl;dr - Don't use renpy.pause with hard=True". */
        g_hard_wait = hard ? mr_value_truthy(hard) : 0;

        if (secs > 0) {
            g_wait_until = g_now + (long)(secs * 1000.0);
            g_state = ST_WAIT;
        } else {
            /* The `$` line this call is in is the statement to come back to;
             * see pause_checkpoint. A timed pause is not a checkpoint. */
            pause_checkpoint(g_pc);
            g_state = ST_WAITCLICK;
        }
        return 1;
    }

    /* A cutscene. Two implementations, and which one runs depends on what is
     * actually in the tree:
     *
     *   the movie      the file named in the statement is opened and decoded,
     *                  if it is an MPEG-1 program stream. movie.c plays it;
     *                  the picture goes straight to the presenter and never
     *                  touches the stage. This is the real thing.
     *
     *   the stand-in   otherwise, the video's first frame is held on screen
     *                  while a soundtrack extracted beside it plays. This is
     *                  what the port did before there was a decoder, and it
     *                  is kept because it still answers two cases: a cutscene
     *                  that has not been re-encoded to MPEG-1 (Miwa's
     *                  splashscreen is intro.webm, which nothing here can
     *                  decode), and a machine too slow for a particular file,
     *                  where deleting the .mpg is the way out.
     *
     * Both derive everything from the video's own filename -- the argument
     * this statement was given -- so nothing has to be registered and a
     * cutscene works in any label:
     *
     *     renpy.movie_cutscene('audio/ending.mpg')
     *         -> audio/ending.mpg   played, if it decodes
     *     renpy.movie_cutscene('audio/intro.webm')
     *         -> audio/intro.mp3    the soundtrack, if it does not
     *         -> images/intro.png   the first frame, which Ren'Py's image
     *                               auto-registration exposes as `intro`
     *
     * tools/mkcutscene.py produces the stand-in pair at build time and pads
     * the mp3 to the video's container duration, which is what makes the
     * track the single source of truth for how long *that* path lasts. The
     * movie path needs none of it: a program stream records its own duration
     * and movie.c reads it at open.
     *
     * Ren'Py's semantics, from exports/mediaexports.py:29, and they are the
     * same for both paths:
     *
     *   stop_music   True by default, so the music is stopped
     *                (set_force_stop at :56). It is *restarted* afterwards
     *                and not resumed: force_stop clears the channel queue
     *                and keeps only the loop entries (audio/audio.py:508),
     *                so clearing the flag queues a looping track again from
     *                its beginning. Matched here by stopping the music and
     *                letting the script's next `play music` restart it.
     *   delay=None   the usual call: the cutscene ends when the movie's sound
     *                does, which is soundstopbehavior at :63. For the movie
     *                path that is mr_movie_update returning 0; for the
     *                stand-in it is the mp3's length.
     *   delay<0      wait for a click, as at :44.
     *   loops        extra repeats, -1 for forever. Implemented by replaying
     *                the movie; the stand-in cannot loop a still, so it
     *                scales its wait instead, which is what (loops + 1) *
     *                length means at :43.
     *   a click      always interrupts: a saybehavior is always added (:60),
     *                so g_hard_wait stays 0, which is what makes ST_WAIT
     *                click-through.
     *   rollback     no checkpoint is taken, which the comment at :74 is
     *                explicit about -- a cutscene cannot be rolled back into.
     *                Nothing here calls pause_checkpoint, and that is the
     *                whole of it.
     *   the menu     suppressed. g_cutscene is what mr_interp_game_menu_toggle
     *                and overlay_sync read; Ren'Py gets it from
     *                suppress_overlay=True at :71. So Escape and the right
     *                button do nothing during a cutscene, which is what the
     *                real engine does too.
     */
    if (!strcmp(name, "renpy.movie_cutscene")) {
        const char *path = (argc > 0 && argv[0].type == MR_V_STR)
                           ? argv[0].s : NULL;
        char base[MR_MAX_NAME], audio[256];
        const char *slash, *dot;
        long dur = 0;
        int len;
        int loops = 0;
        int have_delay = 0;
        double delay_secs = 0.0;

        if (!path || !*path) {
            mr_log("movie_cutscene: no filename");
            return 1;
        }

        /* base = the filename with its directory and extension removed. */
        slash = strrchr(path, '/');
        slash = slash ? slash + 1 : path;
        dot = strrchr(slash, '.');
        len = dot ? (int)(dot - slash) : (int)strlen(slash);
        if (len <= 0 || len >= (int)sizeof(base)) {
            mr_log("movie_cutscene: cannot derive a name from '%s'", path);
            return 1;
        }
        memcpy(base, slash, (size_t)len);
        base[len] = '\0';

        /* The soundtrack sits beside the video, under the same name. */
        len = (int)(slash - path);
        if (len + (int)strlen(base) + 5 >= (int)sizeof(audio)) {
            mr_log("movie_cutscene: '%s' is too long", path);
            return 1;
        }
        memcpy(audio, path, (size_t)len);
        sprintf(audio + len, "%s.mp3", base);

        /* The arguments, read once: movie_cutscene(filename, delay, loops,
         * stop_music). Both of the middle two change what the wait is, so
         * they are needed before either path is chosen. */
        if (argc > 1 && (argv[1].type == MR_V_INT ||
                         argv[1].type == MR_V_FLOAT)) {
            have_delay = 1;
            delay_secs = (argv[1].type == MR_V_INT)
                         ? (double)argv[1].i : argv[1].f;
        }
        if (argc > 2 && argv[2].type == MR_V_INT)
            loops = (int)argv[2].i;

        /* stop_music defaults to True, and is argument four when given.
         *
         * What is playing is recorded before it is stopped, so the teardown
         * can start it again -- see g_cut_music. Recorded even when
         * stop_music is false, where it stays unused: the alternative is two
         * states to reason about in the teardown instead of one, and
         * mr_audio_current on an idle channel answers with an empty
         * string. */
        g_cut_music[0] = '\0';
        g_cut_music_loop = 0;
        mr_audio_current(MR_CH_MUSIC, g_cut_music, (int)sizeof(g_cut_music),
                         &g_cut_music_loop);

        if (argc < 4 || mr_value_truthy(&argv[3]))
            mr_audio_stop(MR_CH_MUSIC, 0);
        else
            g_cut_music[0] = '\0';      /* not stopped, so not restarted */

        g_cut_running = 1;

        /* --- the movie, if there is one --------------------------------- */
        if (mr_movie_open(path)) {
            g_cut_movie = 1;
            g_cut_loops = loops;

            /* A negative delay means "until the player clicks", and that is
             * true of a movie as much as of a still: the movie keeps playing
             * (mr_interp_update drives it in ST_WAITCLICK too) and the click
             * is what ends it. */
            if (have_delay && delay_secs < 0) {
                g_cut_start = g_now;
                g_hard_wait = 0;
                g_cutscene = 1;
                g_state = ST_WAITCLICK;
                return 1;
            }

            if (have_delay) {
                /* An explicit delay wins over the movie's own length, as it
                 * does in Ren'Py -- pausebehavior(delay) rather than
                 * soundstopbehavior (:65). A movie longer than the delay is
                 * cut off; a shorter one leaves the last frame up. */
                dur = (long)(delay_secs * 1000.0);
                g_cut_timed = 1;
            } else {
                /* The container's own duration, times the number of plays.
                 * This is a *backstop* and not the end condition: the real
                 * end is mr_movie_update returning 0, which is this port's
                 * soundstopbehavior. The backstop matters because a truncated
                 * file, or one whose last timestamp the demuxer could not
                 * find, would otherwise park the interpreter forever.
                 *
                 * Hence the slack: a deadline equal to the duration is not a
                 * backstop, it is a second end condition racing the real one,
                 * and it wins. See MR_CUT_BACKSTOP_SLACK_MS. */
                long one = mr_movie_duration_ms();

                if (one <= 0)
                    one = MR_CUT_FLOOR_MS;
                dur = (loops < 0)
                    ? -1
                    : one * (loops + 1) + MR_CUT_BACKSTOP_SLACK_MS;
                g_cut_timed = 1;
            }

            g_cut_start = g_now;
            /* A `loops=-1` movie has no end of its own; only a click stops
             * it, so there is no deadline to set. */
            g_wait_until = (dur < 0) ? g_now + 0x3FFFFFFFL : g_now + dur;
            g_hard_wait = 0;
            g_cutscene = 1;
            g_state = ST_WAIT;
            return 1;
        }

        /* --- the stand-in ----------------------------------------------- */

        /* Recorded for the stand-in too: it cannot loop a still, so the wait
         * is multiplied by the number of plays instead, where the movie path
         * replays. Either way the statement takes (loops + 1) times as long,
         * which is what exports/mediaexports.py:43 computes. */
        g_cut_loops = loops;

        /* The poster frame, scaled to cover what the movie would have covered.
         *
         * This was `mr_scene_scene(base)` and nothing else, which put the
         * frame on the stage as an ordinary background: no scaling, and
         * bottom-standing with x centred, which is mr_place_resolve's answer
         * for an image shown with no placement (scene.c:960). A poster is the
         * size of its video, and a video is not the size of the stage -- Miwa's
         * ending.avi is 640x320, so the stand-in appeared at a quarter of the
         * stage's area, low and centred, with black around it. It looked right
         * only for intro.webm, which happens to be 1280x720.
         *
         * The movie path does not go near the stage: it fits the picture into
         * the presenter's buffer with mr_yuv_fit, centres it, and blacks the
         * rest (present.c:667-669). The stand-in is meant to be that picture
         * held still, so it is fitted with the same function rather than with
         * a second copy of the arithmetic -- the two then cannot drift, and a
         * video whose aspect differs from the stage's gets its own bars in
         * both paths. The stage and the picture buffer are both 16:9, so the
         * fit gives proportionally the same rectangle in each.
         *
         * `mr_scene_clear` and then a `show`, rather than a `scene`, because
         * zoom lives on a display-list entry's placement and a background has
         * none (shown_image, scene.c:994). Clearing first is what supplies the
         * black the bars need.
         *
         * A script that already did `scene intro` -- which this game's
         * splashscreen does -- was relying on this being a no-op. It is no
         * longer one, and that is the point: the scene it set is the
         * unscaled version of the same image. */
        if (mr_image_exists(base)) {
            mr_surface *poster = mr_image_get(base);

            if (poster && poster->w > 0 && poster->h > 0) {
                mr_place pl;
                int fw = 0, fh = 0;

                mr_yuv_fit(poster->w, poster->h, MR_STAGE_W, MR_STAGE_H,
                           &fw, &fh);

                memset(&pl, 0, sizeof(pl));
                /* Centred both ways, which is where mr_yuv_fit's rectangle
                 * goes -- not the bottom-standing default. */
                pl.has_xalign = 1;
                pl.has_yalign = 1;
                pl.xalign = 0.5f;
                pl.yalign = 0.5f;
                /* Per-axis, not the common `zoom`: mr_yuv_fit rounds each
                 * side to an even number, so the two factors are not quite
                 * equal and one of them would leave a row short. */
                pl.has_xzoom = 1;
                pl.has_yzoom = 1;
                pl.xzoom = (float)fw / (float)poster->w;
                pl.yzoom = (float)fh / (float)poster->h;

                /* Saved before it is destroyed, not after. */
                mr_scene_capture(&g_cut_scene);
                g_cut_scene_saved = 1;

                mr_scene_clear();
                mr_scene_show(base, &pl, 1);
                mr_scene_commit();
            }
        }

        mr_audio_play(MR_CH_MUSIC, audio, 0, 0, 1.0f);

        /* An explicit delay wins, as in Ren'Py; negative means wait for a
         * click. */
        if (have_delay) {
            double secs = delay_secs;
            if (secs < 0) {
                g_hard_wait = 0;
                g_cutscene = 1;
                g_state = ST_WAITCLICK;
                return 1;
            }
            dur = (long)(secs * 1000.0);
            g_cut_timed = 1;
        } else if (mr_vfs_exists(audio)) {
            /* The length has to be read off the track, and it is not known
             * yet: an MP3 does not record it, so audio.c works it out from
             * the file size and the bitrate of the first frame -- which the
             * decoder only sees once the mixer has pulled some audio, a frame
             * or two from now. So this starts with a floor and extends it in
             * mr_interp_update as soon as the real figure appears. Asking at
             * this point returned 0 and cut every cutscene down to the
             * fallback. */
            dur = MR_CUT_FLOOR_MS;
            g_cut_timed = 0;
        } else {
            /* No track at all -- `wmake data` was not run, or ffmpeg was
             * missing when it was. Hold the frame briefly rather than
             * flashing past it, and say why. */
            mr_log("movie_cutscene: no '%s'; holding the frame briefly. "
                   "Run `wmake data` to extract it.", audio);
            dur = MR_CUT_FLOOR_MS;
            g_cut_timed = 1;
        }

        g_cut_start = g_now;
        g_wait_until = g_now + dur;
        g_hard_wait = 0;
        g_cutscene = 1;
        g_state = ST_WAIT;
        return 1;
    }

    /* --- screen actions ---
     *
     * These are evaluated only when a button is clicked, never while the
     * screen is laid out, which is exactly why property values are kept as
     * unevaluated text. */
    if (!strcmp(name, "Start")) {
        int t;
        clear_playthrough(1);
        mr_var_set_int("main_menu", 0);
        mr_scene_clear();
        t = mr_script_find_label(g_s, argc > 0 && argv[0].type == MR_V_STR
                                      ? argv[0].s : "start");
        if (t < 0) {
            mr_log("interp: Start() found no label");
            return 1;
        }
        g_pc = t;
        g_sp = 0;
        g_state = ST_RUN;
        return 1;
    }

    if (!strcmp(name, "MainMenu")) {
        mr_interp_main_menu();
        return 1;
    }

    /* --- the quick menu's own actions --- */

    if (!strcmp(name, "Rollback")) {
        mr_interp_rollback();
        return 1;
    }

    /* `Skip()` toggles, and stops if already skipping (00action_menu.rpy:303).
     * `fast=True` is the Ctrl-style skip that ignores the unseen check; the
     * quick menu reaches it through `alternate`, i.e. a right-click. */
    if (!strcmp(name, "Skip")) {
        mr_value *fast = kwarg(argv, argname, argc, "fast");

        if (mr_interp_skipping()) {
            mr_interp_skip_stop();
        } else if (fast && mr_value_truthy(fast)) {
            mr_var_set_int("_preferences.skip", 1);   /* unseen too */
            mr_interp_skip_toggle();
        } else {
            mr_interp_skip_toggle();
        }
        return 1;
    }

    /* QuickSave/QuickLoad are FileSave/FileLoad on slot 1 of the "quick"
     * page (00action_file.rpy:962, :980), and save.c already spells that
     * `quick-1` -> SAVEQ1.SAV. */
    if (!strcmp(name, "QuickSave")) {
        /* The thumbnail first, as FileTakeScreenshot does ahead of the save
         * when the call is not made from a menu (00action_file.rpy:967). */
        mr_save_screenshot(mr_stage());

        /* QuickSave is FileSave(1, page="quick", cycle=True), and the cycle
         * is the part that matters: a quick save keeps a history rather than
         * overwriting one slot. FileSave calls cycle_saves over
         * config.quicksave_slots before writing (00action_file.rpy:413), so
         * the newest is always quick-1 and the oldest drops off the end.
         * Without this every quick save landed in SAVEQ1.SAV and the previous
         * one was gone.
         *
         * config.quicksave_slots is 10 (00action_file.rpy:114), not the six a
         * save page shows -- the two are unrelated, and slots past the sixth
         * are kept but not reachable from the screen, which is Ren'Py's
         * behaviour too. */
        mr_save_cycle("quick", (int)mr_var_get_int("config.quicksave_slots",
                                                  10));

        notify(mr_tl_string(mr_save_write("quick-1", mr_stage())
                            ? "Quick save complete."
                            : "Quick save failed."));
        return 1;
    }

    if (!strcmp(name, "QuickLoad")) {
        if (mr_save_exists("quick-1"))
            mr_save_load("quick-1");
        return 1;
    }

    if (!strcmp(name, "ShowMenu")) {
        if (argc > 0 && argv[0].type == MR_V_STR)
            mr_interp_show_screen(argv[0].s);
        return 1;
    }

    if (!strcmp(name, "Hide")) {
        if (argc > 0 && argv[0].type == MR_V_STR) {
            mr_scene_hide_screen(argv[0].s);
            mr_scene_commit();
        }
        return 1;
    }

    if (!strcmp(name, "Return")) {
        /* Leaving a sub-menu goes back to whatever was underneath: the main
         * menu before the game starts, otherwise the story. */
        if (mr_var_get_int("main_menu", 0))
            mr_interp_main_menu();
        else
            close_game_menu();
        return 1;
    }

    /* Quit(confirm=None).
     *
     * Ren'Py asks before quitting unless the player is at the main menu:
     * `confirm = (not main_menu) and _confirm_quit` when the argument is left
     * out (00action_menu.rpy:265), and the prompt is layout.QUIT
     * (00action_menu.rpy:271). This game's screen passes the gate itself --
     * `action Quit(confirm=not main_menu)` (screens.rpy:337) -- so honour an
     * explicit argument, positional or keyword, and fall back to Ren'Py's
     * rule for a screen that leaves it out.
     *
     * Not done: config.autosave_on_quit's force_autosave before the prompt.
     * We have no autosave. */
    if (!strcmp(name, "Quit")) {
        mr_value *c = kwarg(argv, argname, argc, "confirm");
        int confirm;

        if (!c && argc > 0 && !argname[0])
            c = &argv[0];

        if (c && c->type != MR_V_NONE)
            confirm = mr_value_truthy(c);
        else
            confirm = !mr_var_get_int("main_menu", 0) &&
                      mr_var_get_int("_confirm_quit", 1);

        if (confirm)
            show_confirm(mr_tl_string("Are you sure you want to quit?"),
                         "_quit", "cancel");
        else
            mr_quit();
        return 1;
    }

    /* The yes half of the quit prompt. Ren'Py re-runs Quit(False); we have no
     * way to spell that as an action string, so this is the same effect. */
    if (!strcmp(name, "_quit")) {
        mr_interp_hide_modal();
        mr_quit();
        return 1;
    }

    if (!strcmp(name, "Jump")) {
        if (argc > 0 && argv[0].type == MR_V_STR) {
            int t = mr_script_find_label(g_s, argv[0].s);
            if (t >= 0) {
                g_pc = t;
                g_state = ST_RUN;
            }
        }
        return 1;
    }

    if (!strcmp(name, "SetVariable")) {
        if (argc > 1 && argv[0].type == MR_V_STR)
            mr_var_set(argv[0].s, &argv[1]);
        return 1;
    }

    if (!strcmp(name, "ToggleVariable")) {
        if (argc > 0 && argv[0].type == MR_V_STR)
            mr_var_set_int(argv[0].s, !mr_var_get_int(argv[0].s, 0));
        return 1;
    }

    if (!strcmp(name, "NullAction")) {
        return 1;
    }

    /* `reset_persistent` and `cancel` are defined in an `init python:` block
     * this engine cannot execute, so they are provided here. The script's
     * reset_persistent() clears the persistent data, puts the preferences
     * back to their defaults and relaunches; relaunching is not something a
     * Win32s process can do to itself, and returning to the main menu with
     * everything reset is the same thing without the process restart. */
    /* `persistent._clear(progress=False)`. Real, so that a script calling it
     * directly means something -- this game cannot, because its call is
     * inside the init-python def below that we intercept, but another game's
     * might. progress additionally drops the seen-text set. */
    if (!strcmp(name, "persistent._clear")) {
        mr_value *pr = kwarg(argv, argname, argc, "progress");
        int prog = pr ? mr_value_truthy(pr)
                      : (argc > 0 ? mr_value_truthy(&argv[0]) : 0);
        mr_persist_clear(prog);
        return 1;
    }

    if (!strcmp(name, "reset_persistent")) {
        int k;
        mr_interp_hide_modal();
        /* The clear itself is the script's decision, not ours: find the
         * `persistent._clear(...)` line in the script's own body and
         * evaluate it, which runs the builtin above with whatever `progress`
         * the script asked for. Ren'Py's default is False, and False really
         * does keep the read-text record (persistent.py:92) -- so a game
         * that wants it gone writes progress=True and gets it, here exactly
         * as upstream.
         *
         * Only this one line is taken from the body. The rest cannot run:
         * it ends in renpy.quit(relaunch=True), and a Win32s process cannot
         * relaunch itself. */
        {
            const char *l;
            int i, found = 0;

            for (i = 0; (l = mr_pyfn_body_line("reset_persistent", i)) != NULL;
                 i++) {
                if (strstr(l, "persistent._clear")) {
                    mr_value ignored;
                    mr_expr_eval(l, &ignored);
                    found = 1;
                    break;
                }
            }
            /* No such line, or no such function: Ren'Py's own default. */
            if (!found)
                mr_persist_clear(0);
        }
        for (k = 0; g_prefs[k].name; k++) {
            mr_value d;
            mr_value_set_float(&d, g_prefs[k].dflt);
            mr_var_set(g_prefs[k].var, &d);
            pref_apply(&g_prefs[k], g_prefs[k].dflt);
        }
        mr_var_unset("_preferences.language");
        mr_tl_sync();
        mr_persist_save();
        mr_interp_main_menu();
        return 1;
    }

    if (!strcmp(name, "cancel")) {
        mr_interp_hide_modal();
        return 1;
    }

    /* The yes half of the overwrite prompt, carrying the slot it was raised
     * for. Deliberately not inside the `File...` block above: the slot is a
     * whole slot name here ("2-4"), not the per-page number those helpers
     * take, so it must not go through file_slot.
     *
     * Not a Ren'Py action either. There the confirmation re-runs FileSave
     * with confirm=False, which needs keyword arguments on an action we do
     * not implement; this is the same effect by the shortest route. */
    if (!strcmp(name, "_overwrite_save")) {
        mr_interp_hide_modal();
        if (argc > 0 && argv[0].type == MR_V_STR && argv[0].s[0])
            mr_save_write(argv[0].s, mr_stage());
        return 1;
    }

    /* The yes half of the load prompt, carrying its slot the same way. */
    if (!strcmp(name, "_load_save")) {
        mr_interp_hide_modal();
        if (argc > 0 && argv[0].type == MR_V_STR && argv[0].s[0])
            mr_save_load(argv[0].s);
        return 1;
    }

    /* Language(None) selects the script's original language; a string picks a
     * translation. Ren'Py discards the rollback log here, because the log
     * replays statements whose text came from the other language, and the
     * player has agreed to that by switching; we do the same. The line
     * currently on screen needs nothing: it is translated as it is drawn. */
    if (!strcmp(name, "Language")) {
        if (argc > 0 && argv[0].type == MR_V_STR)
            mr_var_set("_preferences.language", &argv[0]);
        else {
            mr_value none;
            memset(&none, 0, sizeof(none));
            mr_var_set("_preferences.language", &none);
        }
        mr_tl_sync();
        mr_rollback_reset();
        /* The log is gone, so the current line needs its checkpoint back. */
        if (g_has_dialogue)
            say_checkpoint();
        return 1;
    }

    /* --- save slots ---
     *
     * The screens pass a slot *number* within the current page, which is how
     * Ren'Py's File* helpers work: the page is separate state that the page
     * buttons move around. */
    if (!strncmp(name, "File", 4)) {
        char slot[MR_SLOT_NAME];

        file_slot(argv, argc, slot, sizeof(slot));

        if (!strcmp(name, "FileSaveName")) {
            const char *nm = mr_save_name(slot);
            mr_value_set_str(result, nm ? nm : "");
            return 1;
        }

        if (!strcmp(name, "FileTime")) {
            /* Formatted here rather than at save time, as Ren'Py does
             * (00action_file.rpy:269): the slot's moment is stored raw and
             * the `format` the screen asks for is applied now. That is what
             * makes the date follow the player's *current* language -- the
             * format string is itself translatable, and the English
             * translation reorders it to put the month first.
             *
             * `format` has already been through the catalogue by the time it
             * reaches us: the screen writes `_("{#file_time}...")` and the
             * expression evaluator resolves `_()`. mr_format_time drops the
             * {#file_time} tag.
             */
            long when_t = mr_save_time_t(slot);
            const char *when = mr_save_time(slot);

            if (when_t) {
                mr_value *fv = kwarg(argv, argname, argc, "format");
                char buf[128];
                /* Ren'Py's own default for the argument. */
                const char *fmt = (fv && fv->type == MR_V_STR && fv->s[0])
                                ? fv->s : "%b %d, %H:%M";

                mr_format_time(buf, sizeof(buf), fmt, when_t);
                mr_value_set_str(result, buf);
            } else if (when) {
                /* A save from version 7 or earlier: it kept no epoch, only
                 * the stamp it was written with. Showing that is better than
                 * showing nothing, and it is the only thing it has. */
                mr_value_set_str(result, when);
            } else {
                mr_value *e = kwarg(argv, argname, argc, "empty");
                mr_value_set_str(result, (e && e->type == MR_V_STR) ? e->s : "");
            }
            return 1;
        }

        if (!strcmp(name, "FileScreenshot")) {
            /* Resolved by the renderer, which knows how to fetch it. */
            char ref[MR_SLOT_NAME + 8];
            if (mr_save_exists(slot))
                sprintf(ref, "@save:%s", slot);
            else
                ref[0] = '\0';
            mr_value_set_str(result, ref);
            return 1;
        }

        if (!strcmp(name, "FileLoadable")) {
            mr_value_set_int(result, mr_save_exists(slot));
            return 1;
        }

        if (!strcmp(name, "FileAction")) {
            /* On the save screen a slot saves; anywhere else it loads, and
             * an empty slot cannot be loaded. */
            if (g_on_save_screen) {
                /* Saving over an existing slot asks first, as Ren'Py does:
                 * FileSave puts up layout.OVERWRITE_SAVE when the slot is
                 * already taken (00action_file.rpy:406) and only writes once
                 * the player says yes. The slot rides in the yes action's
                 * text, which is how the two actions already travel.
                 *
                 * The message is Ren'Py's own string, translated through the
                 * catalogue: it is defined with _() at 00gui.rpy:450, and the
                 * game translates it in tl/None/common.rpym. */
                if (mr_save_exists(slot)) {
                    char yes[256];
                    sprintf(yes, "_overwrite_save(\"%.32s\")", slot);
                    show_confirm(mr_tl_string(
                                     "Are you sure you want to overwrite "
                                     "your save?"),
                                 yes, "cancel");
                } else {
                    mr_save_write(slot, mr_stage());
                }
            }
            else if (mr_save_exists(slot)) {
                /* Loading over a game in progress asks first, as FileLoad
                 * does: layout.LOADING when `confirm` and not at the main
                 * menu (00action_file.rpy:496). At the main menu there is no
                 * progress to lose, so it loads straight away -- which is why
                 * this gate is on main_menu and the overwrite one above is
                 * not (FileSave prompts from either menu). */
                if (!mr_var_get_int("main_menu", 0)) {
                    char yes[256];
                    sprintf(yes, "_load_save(\"%.32s\")", slot);
                    show_confirm(mr_tl_string(
                                     "Loading will lose unsaved progress.\n"
                                     "Are you sure you want to do this?"),
                                 yes, "cancel");
                } else {
                    mr_save_load(slot);
                }
            }
            return 1;
        }


        if (!strcmp(name, "FileDelete")) {
            mr_save_delete(slot);
            return 1;
        }

        if (!strcmp(name, "FilePage")) {
            if (argc > 0) {
                if (argv[0].type == MR_V_STR)
                    strncpy(g_file_page, argv[0].s, sizeof(g_file_page) - 1);
                else
                    sprintf(g_file_page, "%ld", argv[0].i);
                g_file_page[sizeof(g_file_page) - 1] = '\0';
            }
            return 1;
        }

        if (!strcmp(name, "FilePageNext") || !strcmp(name, "FilePagePrevious")) {
            long p = atol(g_file_page);
            /* The named pages sit before page 1, as they do in Ren'Py. */
            if (p <= 0)
                p = 1;
            p += !strcmp(name, "FilePageNext") ? 1 : -1;
            if (p < 1) p = 1;
            if (p > 9) p = 9;
            sprintf(g_file_page, "%ld", p);
            return 1;
        }

        /* FilePageNameInputValue(pattern=, auto=, quick=) is an object in
         * Ren'Py, and the input asks it for text every frame -- so the
         * label follows the page as the player moves between them. Here it
         * is evaluated once, by the screen's `default`, so returning the
         * text would freeze it on whatever page was open at the time.
         *
         * Returning a handle instead, the same device the music rooms and
         * lists use: the input recognises it and asks for the name when it
         * draws. The three strings are kept because they are the script's,
         * not ours -- they were hardcoded in French before, which the
         * English translation would have shown through. */
        if (!strcmp(name, "FilePageNameInputValue")) {
            mr_value *v;
            if ((v = kwarg(argv, argname, argc, "pattern")) &&
                v->type == MR_V_STR)
                strncpy(g_page_pattern, v->s, sizeof(g_page_pattern) - 1);
            if ((v = kwarg(argv, argname, argc, "auto")) &&
                v->type == MR_V_STR)
                strncpy(g_page_auto, v->s, sizeof(g_page_auto) - 1);
            if ((v = kwarg(argv, argname, argc, "quick")) &&
                v->type == MR_V_STR)
                strncpy(g_page_quick, v->s, sizeof(g_page_quick) - 1);
            mr_value_set_str(result, MR_PAGENAME_HANDLE);
            return 1;
        }

        if (!strcmp(name, "FileCurrentPage")) {
            mr_value_set_str(result, mr_interp_file_page_name());
            return 1;
        }

        mr_log("interp: %s not implemented", name);
        return 1;
    }

    if (!strcmp(name, "renpy.block_rollback")) {
        mr_rollback_block();
        return 1;
    }

    /* The history screen filters tags out of the text; ours are skipped at
     * draw time anyway, so the text passes through unchanged. */
    if (!strcmp(name, "renpy.filter_text_tags")) {
        if (argc > 0 && argv[0].type == MR_V_STR)
            mr_value_set_str(result, argv[0].s);
        return 1;
    }

    /* Screen-local variables: the help screen switches between its keyboard,
     * mouse and gamepad panels with these. */
    if (!strcmp(name, "SetScreenVariable")) {
        if (argc > 1 && argv[0].type == MR_V_STR)
            mr_sl_set_screen_var(argv[0].s, &argv[1]);
        return 1;
    }
    if (!strcmp(name, "ToggleScreenVariable")) {
        if (argc > 0 && argv[0].type == MR_V_STR) {
            mr_value cur, next;
            mr_var_get(argv[0].s, &cur);
            mr_value_set_int(&next, !mr_value_truthy(&cur));
            mr_sl_set_screen_var(argv[0].s, &next);
        }
        return 1;
    }

    /* No gamepad on a Windows 3.1 machine, so the panel for it is hidden. */
    if (!strcmp(name, "GamepadExists")) {
        mr_value_set_int(result, 0);
        return 1;
    }

    if (!strcmp(name, "CurrentScreenName")) {
        mr_value_set_str(result, g_screen_now);
        return 1;
    }

    /* A desktop Ren'Py build reports the variants ["large", "pc", None];
     * see the platform branches at the end of renpy/main.py. Screens gate
     * real content on this, so returning None here silently hides the Quit
     * and Help buttons. */
    if (!strcmp(name, "renpy.variant")) {
        int match = 0;
        if (argc > 0 && argv[0].type == MR_V_STR)
            match = (!strcmp(argv[0].s, "pc") || !strcmp(argv[0].s, "large"));
        mr_value_set_int(result, match);
        return 1;
    }

    /* --- things that are simply not needed here --- */
    /* Borders(l, t, r, b) is a padding tuple. The value model here is
     * scalar, so it is flattened to text the style layer can re-read. */
    if (!strcmp(name, "Borders")) {
        char buf[64];
        sprintf(buf, "%d %d %d %d",
                (int)numarg(argv, argname, argc, 0, NULL, 0.0),
                (int)numarg(argv, argname, argc, 1, NULL, 0.0),
                (int)numarg(argv, argname, argc, 2, NULL, 0.0),
                (int)numarg(argv, argname, argc, 3, NULL, 0.0));
        mr_value_set_str(result, buf);
        return 1;
    }

    if (!strcmp(name, "renpy.save_persistent")) {
        mr_persist_save();
        return 1;
    }

    /* --- the jukebox --- */

    if (!strcmp(name, "MusicRoom")) {
        mr_value *loop = kwarg(argv, argname, argc, "loop");
        mr_value *single = kwarg(argv, argname, argc, "single_track");
        int id = mr_room_new(MR_CH_MUSIC,
                    (long)(numarg(argv, argname, argc, -1, "fadeout", 0.0) * 1000.0),
                    (long)(numarg(argv, argname, argc, -1, "fadein", 0.0) * 1000.0),
                    loop ? mr_value_truthy(loop) : 0,
                    single ? mr_value_truthy(single) : 0);
        mr_room_handle(id, result);
        return 1;
    }

    /* `AudioPositionValue()` is a BarValue: Ren'Py asks it for a position and
     * a range, and a bar here wants the fraction of the two. */
    if (!strcmp(name, "AudioPositionValue")) {
        long dur = mr_audio_duration_ms(MR_CH_MUSIC);
        long pos = mr_audio_position_ms(MR_CH_MUSIC);

        mr_value_set_float(result,
                           dur > 0 ? (double)pos / (double)dur : 0.0);
        return 1;
    }

    /* A method on a music room: the name before the last dot is an
     * expression holding the room -- `mr.Play(...)`, where `mr` is the screen
     * variable the jukebox declared. Ren'Py's Play() returns an action and
     * the button runs it later; doing it when the call is evaluated amounts
     * to the same thing here, because a call is only evaluated when a button
     * has been clicked. */
    {
        const char *dot = strrchr(name, '.');

        if (dot && dot != name) {
            char owner[128];
            mr_value v;
            int id;

            strncpy(owner, name, (size_t)(dot - name));
            owner[dot - name] = '\0';

            mr_expr_eval(owner, &v);
            id = mr_room_from_value(&v);

            if (id >= 0) {
                const char *m = dot + 1;
                const char *file = (argc > 0 && argv[0].type == MR_V_STR)
                                   ? argv[0].s : NULL;

                if (!strcmp(m, "add")) {
                    mr_value *au = kwarg(argv, argname, argc,
                                         "always_unlocked");
                    mr_room_add(id, file, au ? mr_value_truthy(au) : 0);
                } else if (!strcmp(m, "Play") || !strcmp(m, "play")) {
                    mr_room_play(id, file, 0);
                } else if (!strcmp(m, "Next") || !strcmp(m, "next")) {
                    mr_room_next(id);
                } else if (!strcmp(m, "Previous") || !strcmp(m, "previous")) {
                    mr_room_previous(id);
                } else if (!strcmp(m, "Stop") || !strcmp(m, "stop")) {
                    mr_room_stop(id);
                } else if (!strcmp(m, "is_unlocked")) {
                    mr_value_set_int(result,
                                     mr_room_is_unlocked(id, file) ? 1 : 0);
                } else {
                    mr_log("musicroom: no method %s", m);
                }
                return 1;
            }
        }
    }

    /* Both spellings: `renpy.notify(...)` as a statement and `Notify(...)`
     * as an action. Now that there is somewhere to put the message, these
     * show it instead of discarding it. */
    if (!strcmp(name, "renpy.notify") || !strcmp(name, "Notify")) {
        if (argc > 0 && argv[0].type == MR_V_STR)
            notify(mr_tl_string(argv[0].s));
        return 1;
    }

    if (!strcmp(name, "renpy.checkpoint") ||
        !strcmp(name, "Character") ||
        !strcmp(name, "_")) {
        /* `_("x")` marks a translatable string, and returns its translation
         * in the current language -- which is what makes the menus switch. */
        if (!strcmp(name, "_") && argc > 0) {
            *result = argv[0];
            if (result->type == MR_V_STR)
                mr_value_set_str(result, mr_tl_string(argv[0].s));
        }
        return 1;
    }

    /* A function the script defined in `init python:`. Last, so that a
     * definition never shadows a builtin. */
    if (mr_pyfn_exists(name))
        return mr_pyfn_call(name, result);

    return 0;
}

/* --- setup --- */

int mr_interp_init(mr_script *s, const char *game_dir)
{
    (void)game_dir;
    g_s = s;
    mr_stmtid_init(s);
    g_pc = 0;
    g_sp = 0;
    g_state = ST_RUN;
    g_has_dialogue = 0;
    mr_expr_init(builtin_call);
    mr_expr_set_journal(mr_rollback_journal);
    mr_expr_set_listlen(mr_interp_list_len);
    mr_history_reset();
    mr_rollback_reset();
    return 1;
}

void mr_interp_shutdown(void)
{
    /* Ren'Py writes the persistent data on quit as well as on request, and
     * the preferences the player just changed are in it. */
    mr_persist_save();

    mr_expr_shutdown();
    mr_tl_shutdown();
    g_s = NULL;
}

/* Ren'Py screens carry a tag; showing one replaces any other screen with the
 * same tag, which is how the menus swap between navigation, load, prefs and
 * so on without stacking up. */
void mr_interp_show_screen(const char *name)
{
    const sl_screen *sc = mr_interp_find_screen(name);
    int i;

    if (!sc) {
        mr_log("interp: no screen '%s'", name);
        return;
    }

    /* Entering a menu part way through a line finishes that line, so the
     * player comes back to the whole of it rather than to a half-typed one. */
    g_say_complete = 1;

    /* A screen shown afresh starts with its variables at their defaults. */
    mr_sl_screen_vars_clear(name);

    /* Remember what to come back to, but only on the way in: moving between
     * menu screens must not overwrite it with ST_SCREEN. */
    if (g_state != ST_SCREEN)
        g_pre_menu = g_state;

    /* Entering the game menu from the story is when the thumbnail is taken:
     * once a menu is up, the stage shows the menu. Ren'Py does the same, in
     * _enter_menu, guarded on not already being in one. */
    if (g_state != ST_SCREEN && !mr_var_get_int("main_menu", 0)) {
        mr_save_screenshot(mr_stage());

        /* Opening the game menu ends a skip, which is the first thing
         * _enter_menu does (00gamemenu.rpy:81). Without it, clicking any
         * other quick-menu button -- Historique, Sauvegarde -- while
         * skipping left the skip running underneath the menu, so closing it
         * dropped the player back into a story still racing past. */
        mr_interp_skip_stop();
    }

    /* FileAction saves on the save screen and loads everywhere else, so the
     * screen on display is part of the state. */
    strncpy(g_screen_now, name, sizeof(g_screen_now) - 1);
    g_screen_now[sizeof(g_screen_now) - 1] = '\0';
    g_on_save_screen = !strcmp(name, "save");

    if (sc->tag[0]) {
        for (i = 0; i < g_s->nscreens; i++) {
            const sl_screen *o = &g_s->screens[i];
            if (o != sc && !strcmp(o->tag, sc->tag))
                mr_scene_hide_screen(o->name);
        }
    }

    mr_scene_show_screen(name);
    mr_scene_commit();
    g_state = ST_SCREEN;
}

/* --- notifications ---
 *
 * Ren'Py's `Notify(message)` shows a line for config.notify_delay and lets it
 * fade. There is no screen for it here; the text layer draws it, which keeps
 * it out of the display list and so out of saves and transition snapshots --
 * a transient message is not part of the scene.
 *
 * `renpy.notify` was already accepted and thrown away; the quick save is the
 * first caller that had something to say. */
#define MR_NOTIFY_MS 3200L

static char g_notify[128];
static long g_notify_until;

static void notify(const char *msg)
{
    if (!msg)
        return;
    strncpy(g_notify, msg, sizeof(g_notify) - 1);
    g_notify[sizeof(g_notify) - 1] = '\0';
    g_notify_until = g_now + MR_NOTIFY_MS;
    mr_invalidate();
}

/* --- what the quick menu's buttons may do ---
 *
 * Ren'Py greys a button whose action reports get_sensitive() false, and
 * swallows the click. These three are the ones the quick menu needs; they
 * live here because the answers are the interpreter's, not the renderer's. */
int mr_interp_can_rollback(void)
{
    /* Blocked outright by `$ renpy.block_rollback()` around the credits, and
     * empty before the first checkpoint of a playthrough. */
    if (!mr_var_get_int("config.rollback_enabled", 1))
        return 0;
    return mr_rollback_depth() > 0;
}

/* The page's display name, as FilePageNameInputValue would give it. The
 * pattern's `{}` is where the number goes -- Python's str.format, of which
 * this is the only use in the game. */
const char *mr_interp_file_page_name(void)
{
    static char buf[96];
    const char *p, *brace;

    if (!strcmp(g_file_page, "auto"))
        return mr_tl_string(g_page_auto);
    if (!strcmp(g_file_page, "quick"))
        return mr_tl_string(g_page_quick);

    p = mr_tl_string(g_page_pattern);
    brace = strstr(p, "{}");
    if (!brace) {
        sprintf(buf, "%.40s %.8s", p, g_file_page);
        return buf;
    }
    sprintf(buf, "%.*s%.8s%.40s", (int)(brace - p), p, g_file_page, brace + 2);
    return buf;
}

/* Which save page the file screens are showing, as the script spells it:
 * "1".."9", "quick", "auto". */
const char *mr_interp_file_page(void)
{
    return g_file_page;
}

int mr_interp_quicksave_exists(void)
{
    return mr_save_exists("quick-1");
}

/* Skip()'s own get_sensitive (00action_menu.rpy:338): not at the main menu,
 * and not while the script has fenced skipping off. */
int mr_interp_can_skip(void)
{
    if (mr_var_get_int("main_menu", 0))
        return 0;
    if (!mr_var_get_int("_skipping", 1))
        return 0;
    if (!mr_var_get_int("config.allow_skipping", 1))
        return 0;
    return 1;
}

/* The text layer, reporting that it has drawn the line -- and whether it
 * drew all of it. See the ST_SAY case of mr_interp_redraw_kind. */
void mr_interp_say_painted(int complete)
{
    if (complete)
        g_say_painted = 1;
}

const char *mr_interp_notify_text(void)
{
    if (!g_notify[0] || g_now >= g_notify_until)
        return NULL;
    return g_notify;
}

/* --- overlay screens ---
 *
 * `init python: config.overlay_screens.append("quick_menu")`. Ren'Py shows
 * everything in that list at each interaction and hides it again when the
 * interaction asks for it to be suppressed -- show_overlay_screens in
 * display/screen.py:1630, called from interact.
 *
 * Suppression is what the game menu sets: 00gamemenu.rpy:118 does
 * `store.suppress_overlay = True` on the way in, so the quick menu is not
 * drawn over the save screen or the main menu. Here the equivalent question
 * is whether any `tag menu` screen is up, or whether we are at the main
 * menu, which is the same thing from the player's side and needs no new
 * state to answer.
 *
 * Followed once a frame rather than hooked at every place a menu opens or
 * closes, like mr_tl_sync and the display preference: there are six ways
 * into a menu and hooking five of them is the likely outcome. */
static int a_menu_is_up(void)
{
    int i, j;

    /* Every screen, not the first one: the quick menu is itself on the
     * scene and carries no tag, so answering from whichever came first
     * meant a menu opened on top of it was never noticed. */
    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *name = mr_scene_screen_name(i);
        for (j = 0; j < g_s->nscreens; j++) {
            if (!strcmp(g_s->screens[j].name, name)) {
                if (!strcmp(g_s->screens[j].tag, "menu"))
                    return 1;
                break;
            }
        }
    }
    return 0;
}

static void overlay_sync(void)
{
    mr_value lv;
    int id, n, i;
    int want, changed = 0;

    if (!mr_var_get("config.overlay_screens", &lv))
        return;
    id = mr_list_id(&lv);
    if (id < 0)
        return;

    if (g_cutscene && g_state != ST_WAIT && g_state != ST_WAITCLICK)
        g_cutscene = 0;

    want = !mr_var_get_int("main_menu", 0) && !a_menu_is_up() &&
           !g_cutscene && !g_in_splash;
    n = mr_list_len(id);

    for (i = 0; i < n; i++) {
        mr_value v;
        const sl_screen *sc;
        int up, j;

        if (!mr_list_get(id, i, &v) || v.type != MR_V_STR || !v.s[0])
            continue;

        up = 0;
        for (j = 0; j < mr_scene_screen_count(); j++)
            if (!strcmp(mr_scene_screen_name(j), v.s))
                up = 1;

        if (!want) {
            if (up) {
                mr_scene_hide_screen(v.s);
                changed = 1;
            }
            continue;
        }

        if (!up) {
            mr_scene_show_screen(v.s);
            changed = 1;
        }

        /* `zorder 100` on the quick menu: read off the definition, so the
         * draw order is the script's rather than a special case for this
         * one screen. */
        sc = mr_interp_find_screen(v.s);
        if (sc && sc->zorder && sc->zorder[0]) {
            mr_value z;
            mr_expr_eval(sc->zorder, &z);
            mr_scene_screen_set_zorder(v.s, (int)z.i);
        }
    }

    /* An overlay appearing or disappearing changes what the player is
     * looking at, so it has to become the committed state -- otherwise the
     * next `with dissolve` fades it in from a g_prev that never had it.
     * Closing the game menu is where this bit: the close commits, the quick
     * menu comes back on the frame *after*, and the credits' closing
     * dissolve then faded the quick menu in over the top. creditstest
     * caught it.
     *
     * Only when something actually changed, and never mid-transition:
     * mr_scene_commit ends a running transition, so committing every frame
     * would abolish transitions altogether. */
    if (changed && !mr_scene_in_transition())
        mr_scene_commit();
}

/* Everything that belongs to a playthrough rather than to the game. Going to
 * the main menu abandons one, and Start() begins another; Ren'Py throws the
 * whole context away at both points, so the line on screen, the text history,
 * the rollback log and the story's variables go with it.
 *
 * Without this, `play -> right click -> main menu -> play` draws the line the
 * player was on over the new game's opening fade: the scene is cleared but
 * g_what is not, and the say window is drawn from that. The rollback log
 * survived too, so rolling back at the start of a new game walked back into
 * the abandoned one. */
static void clear_playthrough(int reset_vars)
{
    g_who[0] = '\0';
    g_what[0] = '\0';
    g_tlid[0] = '\0';
    g_has_dialogue = 0;
    g_say_pc = -1;
    g_pause_pc = -1;
    g_say_complete = 1;
    g_window_shown = 0;
    g_window_auto = 1;

    mr_history_reset();
    mr_rollback_reset();

    if (reset_vars) {
        /* Back to the init-time baseline, so a flag the last run set does not
         * greet the new one. The persistent data is not part of a
         * playthrough and has to be held aside, exactly as a load does. */
        mr_persist_snapshot();
        mr_var_reset_to_baseline();
        mr_persist_restore();
    }
}

/* Ren'Py opens on the main menu rather than the script. The screen is put on
 * the scene layer and execution simply does not run until it is dismissed. */
void mr_interp_main_menu(void)
{
    mr_value v;

    /* Whatever a splashscreen suppressed, it is over. This is the only place
     * g_in_splash is cleared, which is why a game whose splashscreen leaves
     * by `jump` instead of `return` -- a game with no main menu -- is not
     * supported: the flag would stay set and the game menu stay blocked. See
     * NOTES.md, "Not supported: a splashscreen in a game with no main menu". */
    g_in_splash = 0;
    mr_var_set_int("main_menu", 1);
    clear_playthrough(0);
    mr_scene_clear();

    /* config.main_menu_music belongs to the main menu only. Opening the game
     * menu mid-story must leave the story's music alone, which is why this
     * lives here rather than in mr_interp_show_screen. This is the one place
     * Ren'Py asks for if_changed, in 00start.rpy, so that coming back to the
     * menu does not restart music already looping there. */
    if (mr_var_get("config.main_menu_music", &v) && v.type == MR_V_STR &&
        v.s[0]) {
        mr_audio_play_ex(MR_CH_MUSIC, v.s, 1,
                      (long)(mr_var_get_int("config.main_menu_music_fadein", 0)
                             * 1000), 0, 1.0f, 1);
    } else {
        mr_audio_stop(MR_CH_MUSIC, 0);
    }

    mr_interp_show_screen("main_menu");
}

void mr_interp_start(const char *label)
{
    int i;
    int ninit;
    int check_each;

    /* What the About screen interpolates. Set *before* the init phase, so a
     * game that defines any of them in options.rpy overrides these -- which
     * is how renpy.license is meant to be filled in. Ren'Py ships it empty
     * and expects the developer to set it, and an unset name interpolates as
     * nothing at all, so the default is the empty string rather than a
     * placeholder. */
    {
        mr_value v;
        mr_value_set_str(&v, MR_RENPY_VERSION);
        mr_var_set("renpy.version_only", &v);
        mr_value_set_str(&v, "Ren'Py " MR_RENPY_VERSION);
        mr_var_set("renpy.version", &v);
        mr_value_set_str(&v, "");
        mr_var_set("renpy.license", &v);
    }

    /* Preference defaults, so the radio and check buttons start showing the
     * truth rather than showing nothing. Only the ones whose default is not
     * "off" need saying: an unset preference already reads as 0, which is
     * what Ren'Py defaults skip_unseen and skip_after_choices to.
     *
     * Both of these describe what this engine actually does. `window` is the
     * starting value, not the only one: main.c's display_sync turns this
     * variable into a window style every frame, so the preferences screen,
     * the F11/Alt+Enter/f hotkeys and a restored save all go through here.
     * And we do play transitions.
     * `transitions` reads inverted on the screen: the button is
     * InvertSelected(...), i.e. it means "skip transitions". */
    {
        mr_value v;
        if (!mr_var_get("_preferences.display", &v)) {
            mr_value_set_str(&v, "window");
            mr_var_set("_preferences.display", &v);
        }
        if (!mr_var_get("_preferences.transitions", &v))
            mr_var_set_int("_preferences.transitions", 1);
    }

    /* Ren'Py's defaultstore has `_skipping = True` and config.allow_skipping
     * is True, so a script reading either before setting it sees the right
     * thing. Not preferences: these belong to the playthrough, so Start()
     * putting them back to the baseline -- i.e. to allowed -- is correct
     * even if the last run ended inside a `$ _skipping = False` stretch. */
    {
        mr_value v;
        if (!mr_var_get("_skipping", &v))
            mr_var_set_int("_skipping", 1);
        if (!mr_var_get("config.allow_skipping", &v))
            mr_var_set_int("config.allow_skipping", 1);

        /* `config.overlay_screens = []` (config.py:700), so that the
         * script's `config.overlay_screens.append("quick_menu")` in an init
         * python block has a list to append to. Without it the append is a
         * no-op on an unset name and the quick menu never appears. */
        if (!mr_var_get("config.overlay_screens", &v)) {
            mr_value_set_str(&v, mr_list_new());
            mr_var_set("config.overlay_screens", &v);
        }

        /* Ren'Py's default; the script turns it off around the credits. */
        if (!mr_var_get("quick_menu", &v))
            mr_var_set_int("quick_menu", 1);

        /* 00gamemenu.rpy:63. It is what puts the "Q" page button on the
         * save and load screens -- `if config.has_quicksave:` in
         * screens.rpy -- so without it QuickSave writes a slot the player
         * has no way to reach.
         *
         * config.has_autosave is deliberately *not* set, though Ren'Py
         * defaults it to True (config.py:325): there it is paired with an
         * engine that actually autosaves. Here it would add an "A" page
         * that can never hold anything. */
        if (!mr_var_get("config.has_quicksave", &v))
            mr_var_set_int("config.has_quicksave", 1);

        /* How many quick saves are kept before the oldest is dropped
         * (00action_file.rpy:114). Set as a variable rather than a constant
         * so a game can override it the way it would in Ren'Py. */
        if (!mr_var_get("config.quicksave_slots", &v))
            mr_var_set_int("config.quicksave_slots", 10);
    }

    /* Init phase: register images and evaluate declarations, wherever they
     * sit in the file, before any of the story runs. */
    /* sizeof is logged from *this* file, to be read against the byte count
     * scriptcheck prints from parse.c. Two translation units disagreeing
     * about the layout of mr_stmt -- one object built before a header
     * changed and relinked against fresh ones -- would put every field at
     * the wrong offset for one of them, which is what reading a pointer
     * out of a struct and getting rubbish looks like. The makefile deletes
     * every object when a header changes precisely to prevent it; this is
     * how to confirm that worked, from the machine rather than from the
     * build. */
    mr_log("interp: init pass over %d statements, st=%p, stride %d here, "
           "MR_ST_INIT is %d here",
           g_s->n, (const void *)g_s->st, (int)sizeof(mr_stmt),
           (int)MR_ST_INIT);
    mr_script_verify(g_s, "everything before the init pass");
    check_each = (int)mr_config_int("scriptcheck", 0, 0, 1);
    ninit = 0;
    for (i = 0; i < g_s->n; i++) {
        mr_stmt *st = &g_s->st[i];
        if (st->kind == MR_ST_IMAGE) {
            if (st->anim)
                mr_image_define_anim(st->name, st->anim);
            else
                mr_image_define_manip(st->name, st->text, st->aux,
                                      st->blur_x, st->blur_y,
                                      st->scale_x, st->scale_y);
        }
        else if (st->kind == MR_ST_INIT) {
            ninit++;
            /* The text is a malloc'd copy made at parse time and cannot be
             * NULL here. The guard is for the case where it is anyway: on a
             * FreeDOS box running JEMMEX this array read back as noise and
             * the first symptom was a page fault inside mr_expr_exec, which
             * says nothing about where it came from. A line naming the
             * statement does. See NOTES on JEMMEX. */
            if (!st->text) {
                mr_log("interp: init[%d] (file %d line %d) has no text, "
                       "which should be impossible -- see NOTES on JEMMEX",
                       i, st->file, st->line);
                continue;
            }
            mr_expr_exec(st->text);

            /* After each one, under `scriptcheck=1`: the DOS fault was at
             * statement 760 of this loop, so 759 evaluations had already
             * run and any of them could have been what moved the array.
             * Checking per statement names the culprit instead of the
             * stage. Off by default and a compare-and-return then. */
            if (check_each) {
                char what[48];
                sprintf(what, "evaluating init statement %d", i);
                mr_script_verify(g_s, what);
            }
        }
    }
    mr_log("interp: init pass done, %d statements evaluated", ninit);

    /* Styles read gui.* values, so they can only be expanded once the init
     * assignments above have run. */
    mr_style_finalize();

    /* The persistent data overrides those defaults: it is where the player's
     * volumes, text speed and language actually live. Loaded before the
     * preferences are pushed to the mixer, and before the baseline is marked,
     * so that loading a save -- which resets to the baseline -- keeps it. */
    mr_persist_load();

    /* options.rpy has had its chance to set the preference defaults, so push
     * them through to the mixer now rather than waiting for the player to
     * touch a slider. */
    {
        int k;
        for (k = 0; g_prefs[k].name; k++)
            pref_apply(&g_prefs[k], pref_value(&g_prefs[k]));

        /* Mute is not one of the ranged preferences, so it needs saying
         * separately -- and it does need saying: it is in PERSIST.SAV, so a
         * player who quit muted comes back with the button ticked, and the
         * mixer has to agree. */
        mute_apply(mr_var_get_int(MR_PREF_MUTE_ALL, 0));
    }

    /* Anything in images/ that no `image` statement named. */
    mr_image_autodefine_dir();

    /* Everything set so far is the game's own configuration, not a
     * playthrough's state; a load restores to here. The persistent data is
     * excluded: it was read from the file a moment ago and is not part of
     * any playthrough, so it must not be resurrected by a reset. */
    mr_var_set_baseline_exempt(mr_persist_is_persistent);
    mr_var_mark_baseline();

    g_pc = mr_script_find_label(g_s, label);
    if (g_pc < 0) {
        mr_log("interp: no label '%s'", label);
        g_state = ST_END;
        return;
    }
    g_state = ST_RUN;
}

const sl_screen *mr_interp_find_screen(const char *name)
{
    return g_s ? mr_script_find_screen(g_s, name) : NULL;
}

int mr_interp_finished(void)
{
    return g_state == ST_END;
}

/* Number of UTF-8 characters of the current line to show, or -1 for all of
 * it. Characters rather than bytes, because cutting a multi-byte sequence in
 * half would draw a broken glyph and this game's text is full of accented
 * letters. */
/* Characters of the current line the player can see: text tags are not
 * drawn, and a UTF-8 continuation byte is not a character. Used both by the
 * typewriter and by auto-forward, which scales its delay by the length. */
/* The line as it will actually be drawn: translated, then with its `[name]`
 * interpolations substituted.
 *
 * Both steps happen here, on the way out, rather than at the say statement.
 * Translation has to, because the player can switch language while a line is
 * up. Substitution does not have to but is kept alongside it so that there is
 * one definition of "the text of this line" -- the typewriter and the
 * auto-forward delay count characters of it, and counting `[selchoice]` while
 * drawing `1` would reveal the line at the wrong rate and, worse, stop
 * revealing before the end.
 *
 * Ren'Py substitutes in the Text displayable too, which is why its history
 * entries hold the *raw* string (character.py:1211 passes `what` straight to
 * add_history) and the history screen substitutes again at display time with
 * whatever the variables hold then. mr_history_add is fed the same raw text
 * for the same reason: the history screen's `text what` goes through
 * node_text, which substitutes. */
/* The current line in the current language.
 *
 * Two catalogues, because Ren'Py has two, and a menu's caption can land in
 * either depending on how it was written:
 *
 *   h "So, was it a date?"    a say menuitem. A real ast.Say node, so it is
 *                             digested into an identifier and lives in a
 *                             `translate <lang> <id>:` block.
 *   "So, was it a date?"      a bare caption. Not a Say node at all -- it is
 *                             a Menu *item*, and Menu.get_translation_strings
 *                             hands every item caption to the string
 *                             catalogue (ast.py:1880), which is where the
 *                             choice captions go too. Looked up by its text.
 *
 * Told apart by whether there is a speaker: parse_menu only makes a say
 * menuitem once it has read a `who` (parser.py:252), so a caption with no
 * speaker is necessarily the bare form. The parser leaves such a caption
 * without an identifier, and an absent identifier is the signal here. */
static const char *say_line(void)
{
    if (!g_tlid[0])
        return mr_tl_string(g_what);
    return mr_tl_dialogue(g_tlid, g_what);
}

static const char *say_text(void)
{
    static char sub[MR_MAX_TEXT];

    mr_expr_substitute(say_line(), sub, (int)sizeof(sub));
    return sub;
}

static long visible_chars(void)
{
    const char *p = say_text();
    long total = 0;

    while (*p) {
        if (*p == '{' && p[1] != '{') {
            while (*p && *p != '}')
                p++;
            if (*p)
                p++;
            continue;
        }
        if ((*p & 0xC0) != 0x80)
            total++;
        p++;
    }
    return total;
}

static int say_shown_chars(void)
{
    long cps, chars, total;

    if (g_say_complete)
        return -1;

    /* Skipped dialogue appears all at once -- character.py:666 sets
     * all_at_once for the same reason. It also matters more here than there:
     * the typewriter is most of what a frame costs on this machine. */
    if (mr_interp_skipping())
        return -1;

    cps = mr_var_get_int("_preferences.text_cps", 0);
    if (cps <= 0)
        return -1;                      /* 0 means instant */

    chars = (g_now - g_say_start) * cps / 1000;
    if (chars < 0)
        chars = 0;

    total = visible_chars();

    if (chars >= total) {
        g_say_complete = 1;
        return -1;
    }
    return (int)chars;
}

/* Is the dialogue window on screen? `window show`/`window hide` decide, and a
 * line of dialogue shows it for as long as the line is up whatever they left
 * behind. Nothing else moves it -- in particular a transition does not, which
 * is the whole point: the effect belongs to the scene beneath the window. */
int mr_interp_window_visible(void)
{
    /* The minigame draws the whole stage; no dialogue box over it. The
     * scripts also issue `window hide` before the call, but a run started
     * any other way must not get one either. */
    if (g_state == ST_SCREEN || g_state == ST_RHYTHM)
        return 0;

    /* A cutscene owns the screen, and the same sentence is literally true of
     * Ren'Py: while a fullscreen movie is playing it draws the movie *instead
     * of the render tree* (gl2draw.pyx:1005-1008), so the scene, the window
     * and the overlay are not drawn at all. movie_cutscene gets there through
     * movie_start_fullscreen (mediaexports.py:58).
     *
     * The movie path here does that by a different route and did not need
     * asking: main.c's frame() returns early while one is decoding, so
     * nothing composites. The stand-in composites like any other frame, so
     * the empty dialogue window left over from the say before the cutscene
     * was drawn on top of the held frame -- reported from a real run with
     * movie=0, where it is a grey band across the middle of the picture.
     *
     * Asked of g_cut_running, which is "a cutscene is up, by either route",
     * because the condition being matched is Ren'Py's and Ren'Py's is about
     * the movie and not about which of our two paths is serving it. */
    if (g_cut_running)
        return 0;

    return g_window_shown || g_has_dialogue;
}

int mr_interp_dialogue(const char **who, const char **what, mr_u32 *color,
                       int *shown)
{
    if (!g_has_dialogue)
        return 0;

    /* A menu owns the display: Ren'Py takes the say window down while the
     * game menu is up and brings it back on return. */
    if (g_state == ST_SCREEN)
        return 0;
    /* Translated and interpolated on the way out rather than at the say
     * statement: the player can change language at any moment, including
     * while this line is up. See say_text.
     *
     * Order matters here. *what is read before `shown`, and say_shown_chars
     * also goes through say_text and so refills its buffer -- with the same
     * content, since nothing between these two lines can change a variable,
     * which is the only reason one static is safe for both. */
    *who = mr_tl_string(g_who);
    *what = say_text();
    *color = g_whocolor;
    if (shown)
        *shown = say_shown_chars();
    return 1;
}

int mr_interp_menu(int *count, const char **texts, int max)
{
    int i;

    if (g_state != ST_MENU)
        return 0;
    /* The offered rows, not the declared choices: an option whose condition
     * failed is not a row at all. */
    *count = g_menu_nvis;
    /* Choices marked `_("...")` in the script go through the string
     * catalogue. Ren'Py can also give a menu its own translate block, keyed
     * like a say statement; this game has no in-game menus at all, so that
     * path is not built. */
    for (i = 0; i < g_menu_nvis && i < max; i++)
        texts[i] = mr_tl_string(g_menu_text[i]);
    return 1;
}

int mr_interp_menu_sensitive(int row)
{
    if (g_state != ST_MENU || row < 0 || row >= g_menu_nvis)
        return 0;
    return g_menu_sens[row];
}

/* --- execution --- */

/* Take down every screen on display. `call screen` replaces the interaction
 * in Ren'Py, so the menu the player came from does not stay underneath. */
static void hide_all_screens(void)
{
    int i;

    if (!g_s)
        return;
    for (i = 0; i < g_s->nscreens; i++)
        mr_scene_hide_screen(g_s->screens[i].name);
    mr_scene_commit();
}

/* Publish a screen's result. Ren'Py's `_return` is a tuple here --
 * `num_hits, num_notes, rhythm_score = _return` -- and there is no tuple
 * type, so the parts are published as `_return.0`, `_return.1` and
 * `_return.2`, which is what mr_expr_exec's unpack looks for. `_return`
 * itself keeps the score, so a script that reads it whole still sees
 * something sensible. */
static void set_return_tuple(long hits, long notes, long score)
{
    mr_var_set_int("_return.0", hits);
    mr_var_set_int("_return.1", notes);
    mr_var_set_int("_return.2", score);
    mr_var_set_int("_return", score);
}

/* `call screen foo(a, b) with fade` keeps its whole argument text, transition
 * and all. Copy out just the call, cutting at a top-level `with` -- one
 * inside a string or a bracket is part of an argument, not a transition. */
static void callscreen_expr(const char *text, char *out, int outsz)
{
    int depth = 0;
    char quote = 0;
    int  n = 0;
    const char *p = text;

    for (; *p && n < outsz - 1; p++) {
        if (quote) {
            if (*p == quote)
                quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '(' || *p == '[') {
            depth++;
        } else if (*p == ')' || *p == ']') {
            depth--;
        } else if (depth == 0 && (*p == ' ' || *p == '\t')) {
            /* mr_match_word matches at exactly p, so skip the run of
             * whitespace first. */
            if (mr_match_word(mr_skip_ws(p), "with"))
                break;
        }
        out[n++] = *p;
    }

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        n--;
    out[n] = '\0';
}

/* The story finishing -- a top-level `return`, or simply running out of
 * statements -- takes the player back to the main menu, with the audio and
 * the display list reset so the menu does not inherit the ending's music or
 * its last background. */
int mr_interp_waiting(void)
{
    return g_state == ST_WAIT || g_state == ST_WAITCLICK;
}

/* Run `label splashscreen`, as Ren'Py's _start does when the game has one
 * (00start.rpy:258). Its `return` needs no handling: a RETURN at call depth
 * zero already ends in mr_interp_main_menu via end_game, which is exactly
 * where _start continues to afterwards. */
void mr_interp_splashscreen(void)
{
    g_in_splash = 1;
    mr_interp_start("splashscreen");
}

static void end_game(void)
{
    /* The music is left alone: mr_interp_main_menu either starts
     * config.main_menu_music or stops it, exactly as 00start.rpy does. Fading
     * out here first would only be undone a moment later, since the menu asks
     * for if_changed and the menu track is often the one already playing. */
    mr_audio_stop(MR_CH_SOUND, 0);
    g_has_dialogue = 0;
    g_sp = 0;
    mr_interp_main_menu();
}

/* Put a line on screen: the speaker, the text, the window, the typewriter,
 * the history entry and the seen mark.
 *
 * Split out from do_say because a menu's caption needs all of this and must
 * *not* take a checkpoint of its own. A captioned menu is one place the
 * player can come back to, and taking both a say checkpoint here and the
 * menu's own left two points at the same pc -- so the first Page Up dropped
 * both and landed on the menu it was already showing, and the history pop
 * counted the caption's entry twice. Reported from play, twice now, and the
 * second time was this. */
static void say_display(mr_stmt *st)
{
    const mr_character *c;

    g_who[0] = '\0';
    g_whocolor = MR_RGB(255, 255, 255);

    if (st->name[0]) {
        c = mr_script_find_char(g_s, st->name);
        if (c) {
            strncpy(g_who, c->who, MR_MAX_NAME - 1);
            g_whocolor = c->color;
        } else {
            strncpy(g_who, st->name, MR_MAX_NAME - 1);
        }
        /* strncpy does not terminate when the source fills the buffer. */
        g_who[MR_MAX_NAME - 1] = '\0';
    }

    strncpy(g_what, st->text ? st->text : "", MR_MAX_TEXT - 1);
    g_what[MR_MAX_TEXT - 1] = '\0';
    strncpy(g_tlid, st->tlid ? st->tlid : "", sizeof(g_tlid) - 1);
    g_tlid[sizeof(g_tlid) - 1] = '\0';
    g_has_dialogue = 1;

    /* `window auto`: until the script says otherwise, the first line puts the
     * window up and it stays up between lines. Afterwards an explicit `window
     * hide` is not undone by a line -- the line is visible because a line is
     * always drawn in its window, and the window goes back down when the line
     * does. chap12 relies on that: it hides the window for its title card and
     * never says `window show` again. */
    if (g_window_auto)
        g_window_shown = 1;
    g_say_start = g_now;
    g_say_complete = 0;
    g_say_done_ms = 0;
    g_say_painted = 0;

    /* Whether this line had already been read has to be asked *before* it is
     * marked, or every line is seen by the time anything looks. Skipping
     * consults g_say_seen; marking happens straight afterwards, because
     * Ren'Py counts a line as seen once it has been displayed rather than
     * once it has been dismissed. */
    g_say_seen = mr_persist_seen(g_tlid);
    mr_persist_seen_mark(g_tlid);

    /* The history records what was shown, in the language it was shown in,
     * which is also what Ren'Py does: a line already read does not change
     * language retroactively. */
    mr_history_add(mr_tl_string(g_who), say_line(), g_whocolor);

    /* Reaching a line of dialogue makes the pending display list the one the
     * player is looking at; a later `with` must transition from here. */
    mr_scene_commit();
}

static void do_say(mr_stmt *st)
{
    /* A line of dialogue is an interaction, which is where Ren'Py takes a
     * rollback checkpoint: rolling back lands on this statement, and
     * re-running it redisplays the line without any other effect. */
    g_say_pc = g_pc;
    say_checkpoint();
    say_display(st);
    g_state = ST_SAY;
}

static void exec_one(void)
{
    mr_stmt *st;

    /* A statement is running, so the player is no longer waiting at a pause.
     * One place rather than at each of the ways a pause can end -- a click,
     * auto-forward, skipping -- since all of them lead here. The pause
     * statements set it again below, after this. */
    g_pause_pc = -1;

    if (g_pc < 0 || g_pc >= g_s->n) {
        /* Running off the end of the script ends the game, which in Ren'Py
         * means going back to the main menu rather than quitting. */
        end_game();
        return;
    }

    st = &g_s->st[g_pc];

    switch (st->kind) {
    case MR_ST_LABEL:
    case MR_ST_NOP:
    case MR_ST_IMAGE:
    case MR_ST_INIT:
        g_pc++;
        break;

    case MR_ST_SAY:
        do_say(st);
        g_pc++;
        break;

    case MR_ST_SCENE:
        mr_scene_scene(st->name);
        if (st->has_place)
            mr_scene_show(st->name, &st->place, 1);
        g_pc++;
        break;

    case MR_ST_SHOW:
        mr_scene_show_anim(st->name, &st->place, st->has_place, st->atl, g_now,
                           st->behind);
        g_pc++;
        break;

    case MR_ST_HIDE:
        mr_scene_hide(st->name);
        g_pc++;
        break;

    case MR_ST_SHOWSCREEN:
        /* The script's own screen -- a chapter title card -- as opposed to
         * the game menu. This one is part of the story and is saved. */
        mr_scene_show_story_screen(st->name);
        g_pc++;
        break;

    case MR_ST_HIDESCREEN:
        mr_scene_hide_screen(st->name);
        g_pc++;
        break;

    case MR_ST_WITH:
        start_transition(&st->trans, g_now);
        g_pc++;
        if (mr_scene_busy(g_now))
            g_state = ST_TRANS;
        break;

    case MR_ST_JUMP: {
        int t = mr_script_find_label(g_s, st->name);
        if (t < 0) {
            mr_log("interp: jump to unknown label '%s'", st->name);
            g_state = ST_END;
        } else {
            g_pc = t;
        }
        break;
    }

    case MR_ST_CALL: {
        int t = mr_script_find_label(g_s, st->name);
        if (t < 0) {
            mr_log("interp: call to unknown label '%s'", st->name);
            g_pc++;
        } else if (g_sp >= MR_CALLSTACK) {
            mr_log("interp: call stack overflow at '%s'", st->name);
            g_pc++;
        } else {
            g_stack[g_sp++] = g_pc + 1;
            g_pc = t;
        }
        break;
    }

    case MR_ST_RETURN:
        if (g_sp > 0)
            g_pc = g_stack[--g_sp];
        else
            end_game();
        break;

    case MR_ST_IF: {
        mr_value v;
        mr_expr_eval(st->text ? st->text : "", &v);
        g_pc = mr_value_truthy(&v) ? g_pc + 1 : st->target;
        break;
    }

    case MR_ST_GOTO:
        g_pc = st->target;
        break;

    case MR_ST_MENU: {
        int i;
        int include_disabled =
            mr_var_get_int("config.menu_include_disabled", 0);

        g_menu_first = g_pc + 1;
        g_menu_count = st->aux;
        if (g_menu_count > 16)
            g_menu_count = 16;

        /* Conditions are evaluated here, on arrival, not at parse time: the
         * same menu reached twice can offer different options. */
        g_menu_nvis = 0;
        for (i = 0; i < g_menu_count; i++) {
            const mr_stmt *c = &g_s->st[g_menu_first + i];
            int ok = 1;

            if (c->cond) {
                mr_value v;
                mr_expr_eval(c->cond, &v);
                ok = mr_value_truthy(&v);
            }

            /* config.menu_include_disabled, off by default (00defaults.rpy):
             * a failed condition drops the option entirely. Turned on, the
             * option is still offered but cannot be picked, which is what
             * `sensitive=condition` does to its ChoiceReturn
             * (menuexports.py:101-110). */
            if (!ok && !include_disabled)
                continue;

            g_menu_text[g_menu_nvis] = c->text;
            g_menu_vis[g_menu_nvis] = i;
            g_menu_sens[g_menu_nvis] = ok;
            g_menu_nvis++;
        }

        /* Every option conditioned away. Ren'Py bails out of the menu rather
         * than showing an empty one: display_menu returns None and the menu
         * statement falls through to whatever follows it
         * (menuexports.py:117-121, ast.py:1828). */
        mr_scene_commit();

        if (g_menu_nvis == 0) {
            mr_log("menu: no option's condition held, skipping the menu");
            g_pc = st->target;
            break;
        }

        g_state = ST_MENU;

        /* One checkpoint, taken before the caption is displayed so that the
         * point records the state to come back *to* -- the same order the say
         * statement uses. MR_CP_MENU_CAPTION tells the log that this point is
         * a menu that nonetheless wrote a history entry. */
        menu_checkpoint(st->text != NULL);

        /* The caption, if this menu has one: a line in the dialogue window
         * with the menu up over it. Ren'Py gets there with a say node ahead
         * of the Menu carrying interact=False, which is a say in every
         * respect -- it goes in the history, marks the line seen, puts the
         * window up under `window auto` -- except that it does not wait to be
         * dismissed. So this is the display half of a say, with the menu
         * state already set rather than ST_SAY. */
        if (st->text) {
            say_display(st);
            g_say_pc = g_pc;
        }

        /* A `with` clause on the menu plays as the menu appears, which is
         * what say_menu_with does with it in Ren'Py. Started after the state
         * is set so the menu is part of the frame being transitioned to, and
         * deliberately without entering ST_TRANS: the menu has to be
         * clickable as soon as it is drawn, and the scene layer animates
         * underneath on its own. */
        if (st->trans.kind != MR_TRANS_NONE)
            start_transition(&st->trans, g_now);

        /* "Apres les choix" off means a menu ends the skip. Ren'Py does this
         * at the menu rather than after it (menuexports.py:166): the choice
         * is the player's, so the skip has to be over before the menu is
         * answered, not resumed once it has been. */
        if (mr_interp_skipping() &&
            !pref_on("_preferences.after choices", 0))
            mr_interp_skip_stop();
        break;
    }

    case MR_ST_CHOICE:
        /* Only reached if execution falls into the choice run, which the menu
         * layout prevents; skip past it. */
        g_pc++;
        break;

    case MR_ST_PLAY:
        /* if_changed off, as Ren'Py's play() defaults: `play music X` after
         * `stop music fadeout` restarts X rather than letting it run on. */
        mr_audio_play_ex((mr_channel)st->aux, st->text, st->loop,
                         st->fadein, st->fadeout, st->volume, 0);
        /* renpy.seen_audio: a track the player has heard is unlocked in the
         * jukebox even if the room did not declare it always_unlocked. */
        if (st->aux == MR_CH_MUSIC)
            mr_room_note_played(st->text);
        g_pc++;
        break;

    case MR_ST_QUEUE:
        mr_audio_queue((mr_channel)st->aux, st->text, st->volume);
        g_pc++;
        break;

    case MR_ST_STOP:
        mr_audio_stop((mr_channel)st->aux, st->fadeout);
        g_pc++;
        break;

    case MR_ST_WINDOW:
        /* An explicit window statement ends auto mode, as Ren'Py's
         * execute_window_show/hide do (`store._window_auto = False`). */
        g_window_auto = 0;
        g_window_shown = st->aux;
        if (!g_window_shown)
            g_has_dialogue = 0;
        g_pc++;
        break;

    case MR_ST_PAUSE:
        /* The `pause` statement is an interaction too; see renpy.pause. */
        mr_scene_commit();
        /* The `pause` statement has no `hard` -- that is renpy.pause()'s
         * keyword argument only -- so this is always soft. Said explicitly
         * rather than left to whatever g_hard_wait happened to hold: now that
         * an untimed pause can be hard, inheriting the flag would make the
         * next ordinary `pause` unclickable. */
        g_hard_wait = 0;
        if (st->ms > 0) {
            g_wait_until = g_now + st->ms;
            g_state = ST_WAIT;
        } else {
            /* A checkpoint, like `$ renpy.pause()`; both go through Ren'Py's
             * renpy.pause, which checkpoints the untimed kind only. */
            pause_checkpoint(g_pc);
            g_state = ST_WAITCLICK;
        }
        g_pc++;
        break;

    case MR_ST_PYTHON:
        mr_expr_exec(st->text ? st->text : "");
        g_pc++;
        break;

    case MR_ST_CALLSCREEN:
        if (!strcmp(st->name, "rhythm_game")) {
            /* Evaluate the call itself, which reaches the `rhythm_game`
             * builtin above and starts a run. The statement text still has
             * the `with fade` on the end, so it is cut off first. */
            char  call[MR_MAX_TEXT];
            mr_value r;

            callscreen_expr(st->text, call, sizeof(call));
            mr_expr_eval(call, &r);

            if (mr_rhythm_active()) {
                /* The minigame owns the display until it hands back a
                 * result. Ren'Py's `call screen` replaces the interaction, so
                 * whatever screen the player came from is taken down rather
                 * than left in the scene's list underneath. g_pc has already
                 * moved on, so finishing returns to ST_RUN at the next
                 * statement. */
                hide_all_screens();
                g_pc++;
                g_state = ST_RHYTHM;
                break;
            }
            mr_log("interp: rhythm_game would not start, skipping it");
        } else if (mr_interp_find_screen(st->name)) {
            /* `call screen X` shows X and waits for it to return a value --
             * it does not run on. Treating it as a no-op meant
             * `call screen extras_rythm` at the end of a song fell through
             * into the *next* label in the file, so finishing one song
             * started the following one a moment later. The screen is shown
             * and the interpreter parks in ST_SCREEN; the buttons'
             * Jump/ShowMenu actions are what move it on, which is what the
             * extras menu does. */
            g_pc++;
            mr_interp_show_screen(st->name);
            if (st->trans.kind != MR_TRANS_NONE)
                start_transition(&st->trans, g_now);
            break;
        } else {
            mr_log("interp: no screen '%s', skipping the call", st->name);
        }

        /* Nothing ran: leave a result the script can unpack and carry on. */
        set_return_tuple(0, 0, 0);
        if (st->trans.kind != MR_TRANS_NONE) {
            start_transition(&st->trans, g_now);
            if (mr_scene_busy(g_now))
                g_state = ST_TRANS;
        }
        g_pc++;
        break;

    default:
        g_pc++;
        break;
    }
}

/* Did this update change anything the player could see? The frame loop uses
 * this to skip rendering an idle screen entirely. */
static int g_stepped;

int mr_interp_needs_redraw(void)
{
    return mr_interp_redraw_kind() != MR_REDRAW_NONE;
}

/* A line typing itself out is the commonest frame in the game and the only
 * thing it changes is inside the dialogue box: the scene behind it is the
 * same picture it was last frame. Saying so lets the frame loop repaint and
 * present that box alone instead of all 921,600 pixels.
 *
 * Anything at all uncertain answers ALL. A missed change would leave stale
 * pixels on screen, which is a far worse failure than a slow frame. */
static int redraw_kind(int use_stepped)
{
    int no_partial = 0;
    int kind;

    if (g_stepped && use_stepped)
        return MR_REDRAW_ALL;

    /* The minigame is animating and owns the whole stage, so it is asked
     * first. This has to come above the screen test below: the player reaches
     * a song from the extras menu, whose screen is still in the scene's list
     * while the minigame runs, so that test used to answer NONE and the game
     * only redrew when a keypress set the dirty flag. Reaching a song by any
     * other route -- MR_LABEL, say -- left no screen shown and worked
     * fine, which is what made it look like a minigame bug rather than an
     * ordering one. */
    if (g_state == ST_RHYTHM)
        return MR_REDRAW_ALL;

    /* A screen -- the game menu, a dialog -- can repaint anywhere on the
     * stage, and it is drawn with the scene rather than with the dialogue,
     * so no frame with one up can be a partial one.
     *
     * Except when it demonstrably did not. The quick menu is a screen and it
     * is up for the whole game, so the blanket rule would make every typed
     * character a full 1280x720 recomposite -- hundreds of milliseconds on
     * the target, for a couple of hundred changed pixels. It paints rows
     * 699..720, inside the dialogue strip that a band redraw repaints
     * anyway, so a partial frame is still correct.
     *
     * The extent is what the screens painted *last* frame, which is sound
     * for a static overlay and is why the test is "stayed inside", not
     * "is the quick menu". Anything that moves or appears calls
     * mr_invalidate and gets a full frame that way.
     *
     * A screen painting outside the strip used to answer NONE here and
     * return at once, and that was two questions answered with one word: "a
     * partial frame is not safe" is not the same as "no frame is needed". If
     * the story's own dialogue was still typing itself out, it needed a
     * frame and did not get one -- it sat part-revealed until some input
     * forced a repaint, and then appeared complete, which from play looks
     * exactly like the text waiting for the click. Reported on a menu
     * caption, where this is reachable because a choice menu leaves the
     * state in ST_MENU with the overlay screens still up.
     *
     * So the answer is remembered rather than returned, and whatever the
     * state below asks for is upgraded to a full frame instead of being
     * suppressed. */
    if (mr_scene_screen_count() > 0) {
        int ex, ey, ew, eh, dx, dy, dw, dh;

        if (mr_sl_bars_moved())
            return MR_REDRAW_ALL;

        if (!(mr_sl_extent(&ex, &ey, &ew, &eh) &&
              mr_ui_dialogue_rect(&dx, &dy, &dw, &dh) &&
              ex >= dx && ey >= dy &&
              ex + ew <= dx + dw && ey + eh <= dy + dh))
            no_partial = 1;
    }

    switch (g_state) {
    case ST_SAY:
        /* Still typing the line out -- and on until the *finished* line has
         * actually been drawn, otherwise the last characters are computed
         * and never painted.
         *
         * "Has been drawn" is recorded by the text layer rather than
         * inferred from say_shown_chars() flipping g_say_complete. That
         * inference was the bug: the flag is a side effect of asking how
         * much to reveal, so whoever asked first consumed the one frame the
         * answer was good for, and the last glyph or two of a line stayed
         * unpainted until something forced a repaint. It only started
         * happening when the quick menu made this branch reachable during
         * ordinary dialogue. */
        kind = (say_shown_chars() >= 0 || !g_say_painted) ?
               MR_REDRAW_DIALOGUE : MR_REDRAW_NONE;
        break;

    case ST_TRANS:
    case ST_RUN:
        kind = MR_REDRAW_ALL;
        break;

    case ST_MENU:
        /* A menu itself is static, but the caption it may have opened with
         * is a line of dialogue typing itself out, and it needs frames for
         * exactly as long as an ordinary line does. Same test as ST_SAY,
         * including the "has the finished line been painted" half -- without
         * it the last characters are computed and never drawn.
         *
         * A menu with no caption has no dialogue of its own and g_has_dialogue
         * belongs to whatever line came before it, which is already complete,
         * so this answers NONE for it as it always did. */
        if (g_has_dialogue && (say_shown_chars() >= 0 || !g_say_painted))
            kind = MR_REDRAW_DIALOGUE;
        else
            kind = mr_sl_bars_moved() ? MR_REDRAW_ALL : MR_REDRAW_NONE;
        break;

    case ST_WAIT:
    case ST_WAITCLICK:
    case ST_SCREEN:
    default:
        /* Nothing of our own is moving. A bar might be: the jukebox's
         * position bar advances on its own, with no input and no animation,
         * and it is the only thing on that screen that does. It asks for a
         * frame only when one more of its pixels would be filled. The scene
         * may also still be moving, which the caller checks separately. */
        kind = mr_sl_bars_moved() ? MR_REDRAW_ALL : MR_REDRAW_NONE;
        break;
    }

    /* A screen painted outside the dialogue strip last frame, so the band is
     * not safe -- but a frame that is needed is still needed. NONE stays
     * NONE: this upgrades, it never wakes a loop that was entitled to sleep.
     */
    if (no_partial && kind == MR_REDRAW_DIALOGUE)
        kind = MR_REDRAW_ALL;

    return kind;
}

int mr_interp_redraw_kind(void)
{
    return redraw_kind(1);
}

/* What the answer would have been if `g_stepped` were not forcing ALL.
 *
 * For the MR_REDRAWPROBE measurement only, and it decides nothing: a frame
 * where this says DIALOGUE but mr_interp_redraw_kind says ALL is a frame whose
 * full-stage repaint *might* have been avoidable, and whether it actually was
 * is a question about pixels that only the digest can answer. Keeping the two
 * questions apart is the point -- "the interpreter ran a statement" and "the
 * stage above the dialogue band differs" are not the same claim, and the whole
 * probe exists because nobody has ever measured how far apart they are. */
int mr_interp_redraw_kind_nostep(void)
{
    return redraw_kind(0);
}

/* Execute until something wants to wait.
 *
 * Its own function because mr_interp_update is not the only caller: a
 * rollback or a rollforward leaves a statement pending, and the next one must
 * not begin until that statement has run. See mr_interp_rollback.
 *
 * The guard stops a malformed script from locking up the message loop;
 * 100000 statements is far more than any single interaction legitimately
 * runs. */
static void run_until_wait(void)
{
    int guard = 0;

    while (g_state == ST_RUN && guard++ < 100000) {
        exec_one();
        g_stepped = 1;
    }

    if (guard >= 100000) {
        mr_log("interp: runaway execution at statement %d, stopping", g_pc);
        g_state = ST_END;
    }
}

/* The cutscene is over, by whichever of its several routes.
 *
 * Asked rather than hooked, for the reason g_cutscene's own comment gives: a
 * cutscene's wait ends when the deadline passes, when the player clicks, when
 * a dismiss key is pressed, or when the movie itself runs out, and hooking
 * three of those four is the likely outcome. The question "is the wait this
 * belongs to still running?" has one answer and one place to ask it.
 *
 * Idempotent, and called from both ends of mr_interp_update: the click that
 * ends a cutscene is handled by input.c before the update runs, so checking
 * only at the bottom would leave a frame of dead video on screen.
 */
static void cutscene_teardown(void)
{
    if (!g_cut_running)
        return;
    if (g_state == ST_WAIT || g_state == ST_WAITCLICK)
        return;

    g_cut_running = 0;

    if (g_cut_movie) {
        mr_movie_close();
        g_cut_movie = 0;

        /* The screen is full of video, not of stage. Nothing above this line
         * knows that -- the scene graph has not changed, so the frame loop
         * would skip rendering and leave the last frame of the cutscene up
         * until something else happened to dirty the display. */
        mr_invalidate();
    }

    /* The stand-in's poster comes off the stage, and what it covered goes
     * back. See g_cut_scene.
     *
     * Unconditionally invalidated afterwards for the same reason the movie
     * path invalidates: the display list has changed back to one the frame
     * loop has already composited once, and "nothing changed since the last
     * frame I drew" is not the question -- the last frame it drew was the
     * poster. */
    if (g_cut_scene_saved) {
        mr_scene_restore(&g_cut_scene);
        mr_scene_commit();
        g_cut_scene_saved = 0;
        mr_invalidate();
    }

    g_cut_loops = 0;

    /* Start the music again, if there was any and it was looping.
     *
     * Restarted from the beginning rather than resumed, which is what Ren'Py
     * does; the reasoning and the citations are on g_cut_music. A track that
     * was not looping stays stopped, which is the other half of the same
     * rule and is why the flag is tested rather than just the name.
     *
     * No fade: clearing force_stop in Ren'Py simply lets the queue play, and
     * movie_cutscene has no fadein parameter. Volume 1.0 does not disturb the
     * channel's own volume -- mr_audio_play_ex only writes g_volume for a
     * value that is neither 0 nor 1 -- so the preference slider survives. */
    if (g_cut_music[0] && g_cut_music_loop)
        mr_audio_play(MR_CH_MUSIC, g_cut_music, 1, 0, 1.0f);
    g_cut_music[0] = '\0';
    g_cut_music_loop = 0;
}

void mr_interp_update(long now_ms)
{
    g_stepped = 0;
    g_now = now_ms;

    cutscene_teardown();

    /* The language lives in a preference, which the flag buttons write and a
     * loaded save can change, so it is followed here rather than hooked at
     * each of those places. A no-op unless it actually changed. */
    mr_tl_sync();

    /* The quick menu comes and goes with the menus, for the same reason the
     * language is followed here: too many places open one to hook them all. */
    overlay_sync();

    /* The minigame runs its own clock and owns the display until it has a
     * result to hand back. */
    if (g_state == ST_RHYTHM) {
        mr_rhythm_update(now_ms);
        if (mr_rhythm_finished()) {
            long hits = 0, notes = 0, score = 0;
            mr_rhythm_result(&hits, &notes, &score);
            mr_rhythm_stop();
            set_return_tuple(hits, notes, score);
            mr_log("interp: rhythm_game returned %ld/%ld, score %ld",
                   hits, notes, score);
            g_state = ST_RUN;
        } else {
            return;
        }
    }

    /* A movie decodes and draws from here, once per frame, for as long as the
     * wait it belongs to is running.
     *
     * This is the only thing that draws while a cutscene plays -- main.c
     * composites nothing, which is what mr_interp_movie tells it -- so the
     * call must happen whether or not anything else changed. It is also what
     * ends the cutscene: mr_movie_update returning 0 means the last frame has
     * been shown and the last of the sound has been heard, which is this
     * port's soundstopbehavior (exports/mediaexports.py:63).
     *
     * ST_WAITCLICK is included because `delay=-1` means "play until the
     * player clicks" (:44), and a movie that stopped decoding while waiting
     * for the click would be a still. */
    if (g_cut_movie && (g_state == ST_WAIT || g_state == ST_WAITCLICK)) {
        if (!mr_movie_update()) {
            if (g_cut_loops != 0) {
                /* Another time round. Ren'Py expresses loops by queueing the
                 * same filename (loops + 1) times on the movie channel
                 * (display/video.py:76), which for us is replaying it. */
                if (g_cut_loops > 0)
                    g_cut_loops--;
                if (!mr_movie_replay()) {
                    mr_log("movie_cutscene: could not replay; ending");
                    g_cut_loops = 0;
                    g_wait_until = g_now;
                }
            } else if (g_state == ST_WAIT) {
                /* Ending here rather than letting the backstop expire is the
                 * point: the deadline was computed from the container's
                 * duration, and a stream whose real length differs by a frame
                 * would otherwise hold a black screen or cut the last frame
                 * off. */
                g_wait_until = g_now;
            }
        }
    }

    /* The stand-in lasts as long as its soundtrack, and the soundtrack's
     * length only becomes known once the decoder has seen its first frame.
     * Picked up here, the frame after it started. Same end condition as
     * above, reached by the only means a still has.
     *
     * A movie never takes this branch: g_cut_timed is set at open, because a
     * program stream records its own duration. */
    if (g_cutscene && !g_cut_timed && g_state == ST_WAIT) {
        long d = mr_audio_duration_ms(MR_CH_MUSIC);
        if (d > 0) {
            /* (loops + 1) plays, as exports/mediaexports.py:43 computes the
             * default delay. A still cannot loop, so the wait is scaled
             * instead. */
            long plays = (g_cut_loops < 0) ? 1 : (long)g_cut_loops + 1;

            g_wait_until = g_cut_start + d * plays;
            g_cut_timed = 1;
            mr_log("movie_cutscene: holding the frame for %ldms", d * plays);
        }
    }

    /* Leave the waiting states when their condition clears. */
    if (g_state == ST_TRANS && !mr_scene_busy(now_ms))
        g_state = ST_RUN;
    if (g_state == ST_WAIT && now_ms >= g_wait_until) {
        g_hard_wait = 0;
        g_state = ST_RUN;
    }

    /* The wait that a cutscene was parked in may have just ended, a few lines
     * above. Tear down here as well as at the top, so the frame the story
     * resumes on is composited rather than showing one more frame of video. */
    cutscene_teardown();

    /* Skipping: advance the line the same way a click would.
     *
     * One line per frame rather than a tight loop. Ren'Py's config.skip_delay
     * is 5ms, i.e. as fast as its loop goes, but here each line still has to
     * be composited and presented, and running the interpreter ahead of the
     * screen would only build a backlog the player cannot see. A frame is
     * also what makes skipping interruptible.
     *
     * Reaching something the skip is not allowed to cross stops the skip
     * rather than merely pausing it, which is Ren'Py's behaviour too
     * (character.py:612 sets config.skipping = None); otherwise releasing and
     * re-pressing would be the only way to see the line. */
    if (g_state == ST_SAY && mr_interp_skipping()) {
        if (skip_allows_line()) {
            g_say_complete = 1;
            g_has_dialogue = 0;
            g_state = ST_RUN;
        } else {
            mr_interp_skip_stop();
        }
    }

    /* Auto-forward: once the line is fully revealed, wait a while and
     * advance by itself. Ren'Py's delay, from behavior.py:733 with the
     * defaults in config.py:223 and :226:
     *
     *     ((afm_bonus + length) / afm_characters) * afm_time  seconds
     *     ((25            + n) / 250          ) * afm_time
     *
     * so it is proportional to how much there is to read, which is the
     * point -- a fixed delay is either too short for a long line or
     * tedious for a short one. afm_time is the "Avance automatique" slider;
     * it is max_is_zero, and 0 means never, which falls out of the
     * truthiness test Ren'Py uses too.
     *
     * The clock starts when the text finished appearing, not when the line
     * began, or the delay would be eaten by the typewriter. */
    if (g_state == ST_SAY && !mr_interp_skipping()) {
        if (g_say_done_ms == 0 && say_shown_chars() < 0)
            g_say_done_ms = g_now;

        if (g_say_done_ms != 0 && mr_var_get_int("_preferences.afm_enable", 0)) {
            double afm = 0.0;
            mr_value v;

            if (mr_var_get("_preferences.afm_time", &v))
                afm = (v.type == MR_V_FLOAT) ? v.f : (double)v.i;

            if (afm > 0.0) {
                long wait = (long)(((25.0 + (double)visible_chars()) / 250.0)
                                   * afm * 1000.0);
                if (g_now - g_say_done_ms >= wait) {
                    g_has_dialogue = 0;
                    g_state = ST_RUN;
                }
            }
        }
    }

    /* A pause is crossed too. The chapter title cards are
     * `show screen title` + `$ renpy.pause()` with no delay, so a skip that
     * stopped here would wait for a click and be over in practice. Ren'Py
     * ends the pause: it is an interaction and skipping ends interactions --
     * "the pause will still end by other means - when the user clicks or
     * skips" (statementexports.py:121), and a *hard* pause is skipped too
     * (line 131), which is why g_hard_wait is cleared rather than respected.
     *
     * No seen check: a pause is not a line of dialogue and has no
     * identifier to have read before. The skip stops at the next line that
     * is new, which is the title card's own text having gone past.
     *
     * ST_SCREEN is deliberately not here. A `call screen` is waiting for the
     * player to press something on it, and Ren'Py does not skip a modal
     * interaction either (behavior.py:822 returns None for ev.modal). */
    if ((g_state == ST_WAIT || g_state == ST_WAITCLICK) &&
        mr_interp_skipping()) {
        g_hard_wait = 0;
        g_state = ST_RUN;
    }

    /* Auto-forward crosses an untimed pause too, and has to, or Auto mode
     * stops dead on the first chapter title card -- which is indistinguishable
     * from Auto being broken.
     *
     * `renpy.pause()` with no delay passes `afm = " "` to its SayBehavior and
     * one *with* a delay passes None (statementexports.py:176-179), so the
     * untimed pause auto-forwards and the timed one does not: it already has
     * a timer, and afm_length None fails the test at behavior.py:732.
     *
     * len(" ") is 1, and set_text -- which is what makes afm_length the length
     * of the line -- is only called on the say path. So the delay is the
     * formula below with a length of one character, a little under a second
     * at the default afm_time, rather than the several seconds a line of
     * dialogue gets.
     *
     * The timestamp is latched here rather than where the pause is entered,
     * the same way g_say_done_ms is: four places set ST_WAITCLICK (a `pause`
     * statement, renpy.pause(), a cutscene, and a restored save) and a clock
     * one of them forgot to start would be a pause that auto-forwards
     * instantly. */
    if (g_state == ST_WAITCLICK) {
        mr_value v;
        double afm = 0.0;

        if (g_pause_since == 0)
            g_pause_since = g_now;

        if (mr_var_get_int("_preferences.afm_enable", 0) &&
            mr_var_get("_preferences.afm_time", &v)) {
            afm = (v.type == MR_V_FLOAT) ? v.f : (double)v.i;
            if (afm > 0.0 &&
                g_now - g_pause_since >= (long)(((25.0 + 1.0) / 250.0)
                                                * afm * 1000.0)) {
                /* Cleared on the way out, as the timed path does when its
                 * clock expires: this is one of the two things that can end a
                 * *hard* untimed pause, and leaving the flag set would hand it
                 * to whatever waits next. */
                g_hard_wait = 0;
                g_state = ST_RUN;
            }
        }
    } else {
        g_pause_since = 0;
    }

    /* And once more, because two of the blocks above also end the wait a
     * cutscene was parked in: the skip (Ctrl or Tab) and auto-forward both
     * turn ST_WAIT into ST_RUN, and both sit *below* the teardown at :3547.
     * So a cutscene left that way was never torn down before the story
     * resumed -- the movie stayed open, and the stand-in's poster stayed on
     * the stage with the pre-cutscene scene still held in g_cut_scene.
     *
     * Above run_until_wait and not at the top of the next update, which is
     * where it would otherwise have been reached, because the ordering is
     * the whole bug. run_until_wait is what executes the statement after the
     * cutscene, and in a splashscreen that statement is `return`: it ends in
     * mr_interp_main_menu, which clears the scene and shows `main_menu`. The
     * teardown then arrived a frame later and restored the scene captured
     * before the cutscene *over the top of it*, taking the main menu screen
     * out of the scene's screen list with it. Nothing re-shows it, so the
     * title screen never appeared -- reported as "press Ctrl during the
     * splashscreen and the engine gets stuck". Holding Ctrl through a
     * cutscene mid-story merely lost a frame of video, which is why it
     * looked like a splashscreen fault.
     *
     * Ren'Py ends the interaction here too -- movie_cutscene's
     * `ui.saybehavior()` returns True while config.skipping
     * (behavior.py:814-821) and the `movie_stop()` on the far side of the
     * interact (mediaexports.py:78) runs for that exit exactly as it does
     * for a click. The skip ending the cutscene is right; it ending it
     * without stopping the movie was not.
     *
     * Idempotent, so the frames that reach this with nothing to do pay one
     * test of g_cut_running. */
    cutscene_teardown();

    run_until_wait();
}

void mr_interp_render(mr_surface *stage, long now_ms)
{
    /* The minigame draws the whole stage itself -- background included -- so
     * the scene is not composited underneath it. */
    if (g_state == ST_RHYTHM) {
        mr_rhythm_render(stage, now_ms);
        return;
    }

    /* Fresh each frame: the extent is "what the screens painted this time",
     * and the next frame's redraw decision reads it. */
    mr_sl_extent_reset();
    mr_scene_render(stage, now_ms);
}

void mr_interp_render_overlay(mr_surface *stage)
{
    if (g_state == ST_RHYTHM)
        return;              /* the minigame owns the whole stage */
    mr_scene_render_overlay(stage);
}

/* Player input during a run. Returns 1 if the minigame took the event, so the
 * frame loop does not also treat it as advancing the story. */
int mr_interp_rhythm_key(int key)
{
    return (g_state == ST_RHYTHM) ? mr_rhythm_key(key) : 0;
}

int mr_interp_rhythm_click(int x, int y)
{
    return (g_state == ST_RHYTHM) ? mr_rhythm_click(x, y) : 0;
}

int mr_interp_rhythm_hover(int x, int y)
{
    return (g_state == ST_RHYTHM) ? mr_rhythm_hover(x, y) : 0;
}

/* With auto-forward running, the click that would have advanced turns it off
 * and is spent doing that -- it does not advance the line, finish the
 * typewriter or end the pause. behavior.py:797-804 clears afm_enable and
 * raises IgnoreEvent. Clicking to read on manually otherwise fights the timer
 * for every line.
 *
 * Guarded by afm_after_click (default False, preferences.py:124), the "stay
 * in auto-forward after a click" preference. Ren'Py also guards on
 * using_afm_enable, true whenever config.default_afm_enable is set --
 * 00defaults.rpy:36 sets it to False, so it is true for every game that has
 * not opted out through 00compat, and there is nothing here to express the
 * other case with.
 *
 * This covers exactly the states that have a SayBehavior in Ren'Py, because
 * that is the displayable the rule lives on: dialogue, and a pause of either
 * kind (statementexports.py:181-184). A hard pause is not one of them -- its
 * dismiss keysym is "dismiss_hard_pause", so an ordinary click never reaches
 * the block at all -- and neither is a choice menu, which has no SayBehavior
 * whatsoever (menuexports.py:320 adds one only for fixed rollback). So
 * answering a menu leaves auto-forward exactly as it was. */
static int afm_took_click(void)
{
    if (!mr_var_get_int("_preferences.afm_enable", 0) ||
        mr_var_get_int("_preferences.afm_after_click", 0))
        return 0;

    mr_var_set_int("_preferences.afm_enable", 0);
    return 1;
}

void mr_interp_advance(void)
{
    switch (g_state) {
    case ST_TRANS:
        /* A click during a transition finishes it immediately, as Ren'Py
         * does. */
        mr_scene_skip();
        g_state = ST_RUN;
        break;

    case ST_SAY:
        if (afm_took_click())
            break;

        /* A click part way through a line finishes it instead of skipping
         * it, so nothing is ever missed by clicking too eagerly. */
        if (say_shown_chars() >= 0) {
            g_say_complete = 1;
            break;
        }
        g_has_dialogue = 0;
        g_state = ST_RUN;
        break;

    case ST_WAIT:
        if (g_hard_wait)
            break;              /* a hard pause plays out in full */
        if (afm_took_click())
            break;
        g_state = ST_RUN;
        break;

    case ST_WAITCLICK:
        /* Above the auto-forward cancel, for the reason given there: Ren'Py's
         * cancel is inside the dismiss loop, and a hard pause's dismiss
         * keysym is bound to nothing, so a click never reaches it. */
        if (g_hard_wait)
            break;
        if (afm_took_click())
            break;
        g_state = ST_RUN;
        break;

    case ST_MENU:
        /* A menu's caption finishes typing on a press, exactly as a line of
         * dialogue does -- and the press does nothing else: it must not
         * answer the menu, and there is nothing to advance to.
         *
         * Slow text is not SayBehavior's business in Ren'Py, which is why
         * this survives a menu at all: the Text displayable handles it
         * itself (text/text.py:2661). On `dismiss` it ends every slow text
         * on screen and raises IgnoreEvent, so the press is consumed.
         *
         * It applies to a caption because `slow_abortable` comes from the
         * Character (defaultstore.py:398, via character.py:1116), not from
         * the say's interact flag -- the *style* default is False
         * (00style.rpy:167), so reading the style alone says the opposite.
         *
         * Deliberately no afm_took_click() here, unlike ST_SAY: Ren'Py's
         * auto-forward cancel lives in SayBehavior, which is not running
         * during a menu, and afmtest pins that a menu neither auto-picks nor
         * clears Auto. */
        if (g_has_dialogue && say_shown_chars() >= 0)
            g_say_complete = 1;
        break;

    case ST_SCREEN:
        /* A screen owns the display: a click that missed every button does
         * nothing. There is no dialogue waiting to be advanced, and the menu
         * must not be dismissed by clicking beside a button. */
        break;

    case ST_RHYTHM:
        /* Handled by mr_interp_rhythm_click before it ever gets here. */
        break;

    default:
        break;
    }
}

/* --- engine lists ---------------------------------------------------------
 *
 * `for h in _history_list` iterates objects with attributes. There is no
 * object model here, but the variable store keeps dotted names whole, so an
 * item is described as a set of suffixes -- "who", "what", "who_args.color"
 * -- which the loop binds as `h.who` and friends. That is enough for the
 * history screen without inventing objects.
 */
int mr_interp_list_len(const char *name)
{
    if (!strcmp(name, "_history_list"))
        return mr_history_count();
    return -1;
}

int mr_interp_list_item(const char *name, int i, mr_list_field *out, int max)
{
    const char *who, *what;
    mr_u32 color;
    int n = 0;

    if (strcmp(name, "_history_list"))
        return 0;
    if (!mr_history_get(i, &who, &what, &color))
        return 0;

    if (n < max) {
        out[n].suffix = "who";
        mr_value_set_str(&out[n].value, who);
        n++;
    }
    if (n < max) {
        out[n].suffix = "what";
        mr_value_set_str(&out[n].value, what);
        n++;
    }
    if (n < max && who[0]) {
        /* The screen asks `if "color" in h.who_args` and then reads
         * h.who_args["color"], which the store sees as h.who_args.color. */
        char buf[16];
        sprintf(buf, "#%02x%02x%02x", MR_R(color), MR_G(color), MR_B(color));
        out[n].suffix = "who_args.color";
        mr_value_set_str(&out[n].value, buf);
        n++;
    }
    return n;
}

/* Step back one line, as Ren'Py's rollback does. Bound to Backspace, Page Up
 * and a wheel-up notch; Windows 3.1 has no wheel, so the keys are what will
 * actually be used on the target. */
/* Install a checkpoint's state and resume at the statement it names.
 *
 * Shared by rollback and rollforward rather than written twice. The music is
 * the reason: it is part of the checkpoint, and a rolled-forward scene that
 * keeps playing the track from where it was rolled back to is wrong in a way
 * that is easy to miss and has caught this engine out before. One function
 * means the two directions cannot drift apart.
 */
static void apply_checkpoint(const mr_gamestate *g, long pc, const char *what)
{
    /* Restore the scene and the call stack, then resume at the statement the
     * checkpoint named -- the say, which redraws its own line. */
    mr_scene_restore(&g->scene);

    /* A checkpoint that came out of a save file carries its animations as a
     * span rather than a timestamp; see the reader in history.c. One taken in
     * this process does not, and must not be rebased -- its stale absolute
     * timestamps are exactly what finishes the animation on the first frame. */
    if (g->anim_relative)
        mr_scene_resume_anim(g_now);

    g_window_shown = g->window_shown & 1;
    g_window_auto = (g->window_shown & 2) ? 1 : 0;

    /* if_changed: moving within a scene leaves the music alone rather than
     * restarting the track from the top, but a step that crosses a `play
     * music` does change it. See mr_interp_restore. */
    {
        char now[sizeof(g->music)];
        int loop;

        now[0] = 0;
        mr_audio_current(MR_CH_MUSIC, now, sizeof(now), &loop);
        /* Only the change is worth a line. One line per step buries the log
         * under hundreds of identical entries when the player holds the key
         * down, and the thing that has gone wrong before is the track not
         * following the step -- which is exactly what this prints. */
        if (strcmp(now, g->music))
            mr_log("%s: to pc %ld, music '%s' (was '%s')", what, pc,
                   g->music[0] ? g->music : "(none)",
                   now[0] ? now : "(none)");
    }
    if (g->music[0])
        mr_audio_play_ex(MR_CH_MUSIC, g->music, g->music_loop, 0, 0, 1.0f, 1);
    else
        mr_audio_stop(MR_CH_MUSIC, 0);

    g_sp = (int)g->sp;
    {
        int i;
        for (i = 0; i < g_sp && i < MR_SAVE_STACK; i++)
            g_stack[i] = (int)g->stack[i];
    }
    g_pc = (int)pc;
    g_has_dialogue = 0;
    g_state = ST_RUN;
}

void mr_interp_rollback(void)
{
    mr_gamestate g;
    long pc;

    if (!mr_var_get_int("config.rollback_enabled", 1))
        return;
    if (g_state == ST_SCREEN || mr_var_get_int("main_menu", 0))
        return;

    /* Finish the previous step before starting another one.
     *
     * mr_rollback_back does not complete a step by itself: it rewinds to the
     * target and leaves the caller to resume the target's statement, and it
     * is *that* statement's checkpoint which moves the head, counts the point
     * and decrements the forward count. All of that happens in
     * run_until_wait, which until now ran only from mr_interp_update -- while
     * keys are handled as they arrive. So holding Page Up delivered several
     * rollbacks between two updates, and every one after the first found the
     * log half-finished: the newest point was no longer where the player was,
     * so the point being stepped over was not recognised, nothing was
     * captured for the way forward, and the forward count was incremented
     * anyway.
     *
     * Rolling *back* still worked -- each unwind picks up where the last one
     * left off -- which is why this surfaced only on the way forward, as a
     * step that moved the pc and the scene and left the variables behind. On
     * a chapter title card, whose whole text is `text "[title]"` and whose
     * title is assigned by the step that reaches it, that is a card drawing
     * another chapter's title, or nothing at all. Reported from play twice,
     * with "reproducing is not always" both times: it needs two presses
     * inside one 33ms frame, which is exactly what key auto-repeat does.
     *
     * Ren'Py cannot reach it: its rollback restarts the interaction, so one
     * press is one rollback by construction.
     *
     * No input is dropped -- the pending statement runs now instead of a few
     * milliseconds later, and this press is then handled in full.
     * mr_interp_advance leaves the same pending state, so an advance followed
     * by a rollback inside one frame is completed here too. */
    if (g_state == ST_RUN)
        run_until_wait();

    /* While a line is displayed the newest checkpoint is that line; at a
     * pause it is the line before, which is where back should land. */
    /* The statement the player is on: the displayed line at a say, and
     * otherwise simply where execution stopped -- the menu, or the pause. The
     * log compares it with its newest checkpoint rather than being told
     * whether that checkpoint is on screen. */
    {
        long here = g_pc;

        if (g_state == ST_SAY)
            here = g_say_pc;
        else if (g_state == ST_WAITCLICK && g_pause_pc >= 0)
            /* An untimed pause is a checkpoint, and g_pc has moved past it,
             * so without this the log compared its newest point against the
             * statement *after* the pause, decided the pause was not what is
             * on screen, and stepped back one -- onto the pause's own point,
             * which re-ran the pause and left the title card exactly where it
             * was. The same shape as the menu bug above it; the fix is to
             * answer the question the log is actually asking. */
            here = g_pause_pc;

        if (!mr_rollback_back(&g, &pc, here))
            return;
    }

    apply_checkpoint(&g, pc, "rollback");
}

/* Ren'Py's `rollforward`: having gone back, go forward again. Bound to
 * PageDown and the wheel down (00keymap.rpy:66), and it does nothing at all
 * when there is no future to return to -- which is the normal case, and is
 * why these keys are silent during ordinary play rather than advancing.
 *
 * Guarded exactly as rollback is, and for the same reasons. */
void mr_interp_rollforward(void)
{
    mr_gamestate g;
    long pc;

    if (!mr_var_get_int("config.rollback_enabled", 1))
        return;
    if (g_state == ST_SCREEN || mr_var_get_int("main_menu", 0))
        return;

    /* The same as in mr_interp_rollback, and for the same reason: a step is
     * only finished once the statement it resumed has run. Two forward
     * presses in one frame did not corrupt the log the way two back presses
     * did -- the second simply re-applied the same point -- but it did lose
     * the press, and the asymmetry is not worth keeping. */
    if (g_state == ST_RUN)
        run_until_wait();

    /* A menu on screen is answered from the record rather than stepped over.
     *
     * This is Ren'Py's RollForward displayable (behavior.py:572-583): it is
     * added to the interaction whenever there is forward info for it
     * (core.py:2511), and its whole body is "on the rollforward key, return
     * the recorded value". For a menu that value is the option taken, so the
     * press answers the menu and play continues -- a menu costs one keypress
     * on the way forward and does not end the future.
     *
     * Guarded on the option still being offered, which is
     * `if roll_forward not in choices: roll_forward = None`
     * (menuexports.py:308). A menu whose options changed under an edited
     * script has no valid answer to replay, and then the key does nothing,
     * which is where a rollforward legitimately stops at a menu. */
    if (g_state == ST_MENU) {
        int n = mr_rollback_answer_here();
        int row;

        if (n < 0 || n >= g_menu_count)
            return;

        /* The record holds a declared index, so a condition that has changed
         * since cannot make it point at the wrong option -- but it can make
         * it point at one that is no longer on offer, and that is the case
         * `roll_forward not in choices` covers. Nothing to replay, so the
         * key does nothing and the rollforward stops here. */
        for (row = 0; row < g_menu_nvis; row++) {
            if (g_menu_vis[row] == n)
                break;
        }
        if (row >= g_menu_nvis || !g_menu_sens[row]) {
            mr_log("rollforward: option %d is no longer offered, stopping at "
                   "the menu", n);
            return;
        }

        /* Not mr_interp_choose: that clears the forward log, which is right
         * for a click and wrong here. Going round it leaves the log intact,
         * and the next say's checkpoint lands on the first forward point with
         * a matching pc, so the rest of the future is kept. */
        g_pc = g_s->st[g_menu_first + n].target;
        g_state = ST_RUN;
        mr_log("rollforward: answering the menu with option %d", n);
        return;
    }

    if (!mr_rollforward(&g, &pc))
        return;

    apply_checkpoint(&g, pc, "rollforward");
}

/* Right-click, as Ren'Py uses it: open the game menu during play, and close
 * it again if it is already open. It does nothing on the main menu, where
 * there is no story to go back to. */
void mr_interp_game_menu_toggle(void)
{
    /* Not during the splashscreen. Ren'Py's `label _game_menu` returns at
     * once when `_game_menu_screen` is unset (00gamemenu.rpy:166), and
     * `_start` sets it to None for exactly the length of the splashscreen,
     * restoring it afterwards (00start.rpy:249 and :260). So Escape and the
     * right-click do nothing at all while an intro is playing -- where here
     * they opened the main menu over it, which is the one divergence left
     * after the cutscene work.
     *
     * The dismiss keys are unaffected and still cut the cutscene short: those
     * go through the saybehavior, not through this. */
    if (g_in_splash)
        return;

    /* A cutscene takes the right-click as "stop the video", not as "open the
     * menu" -- exactly as a click or a dismiss key does, and through the same
     * path, so there is one way a cutscene can end and not two.
     *
     * **This is a deliberate divergence and the citation does not support
     * it.** Ren'Py's `_invoke_game_menu` (00gamemenu.rpy:125) has no test for
     * a cutscene: outside the splashscreen it calls `_game_menu` in a new
     * context whatever is on screen, and the keymap that reaches it is in
     * `config.underlay` (00keymap.rpy:466), which `movie_cutscene` does not
     * suppress -- it passes `suppress_overlay=True` only
     * (exports/mediaexports.py:71), and the underlay needs
     * `suppress_underlay`. So by the source, the menu opens.
     *
     * It is here because the behaviour was asked for and was reported from a
     * real Ren'Py run as "right click ends the video", twice. What the engine
     * did instead was worse than either reading: it opened the save screen
     * over a cutscene that the menu's own state change had already torn down,
     * and closing the menu left the story on an empty dialogue line.
     *
     * The narrower rule is also the one that can be stated: a cutscene owns
     * the screen until it ends, and there is exactly one thing a button can
     * do to it. If Ren'Py's behaviour turns out to be wanted after all, the
     * change is to delete this block -- not to add a second way out. */
    if (g_cut_running) {
        mr_interp_advance();
        return;
    }

    /* `key "game_menu" action no_action` on the confirm screen: while a modal
     * is up, the right-click belongs to it and means "no", not "close the
     * game menu". */
    if (g_modal_screen[0]) {
        char no[256];
        strcpy(no, g_modal_no);
        mr_interp_hide_modal();
        if (no[0])
            mr_interp_action(no);
        return;
    }

    /* A screen can bind the keysym itself, and `screen game_menu` does:
     * `if main_menu: key "game_menu" action ShowMenu("main_menu")`. That is
     * the only way out of Preferences or About opened from the main menu --
     * there is no story underneath to close the menu onto, so without it the
     * right-click and Escape did nothing at all and the Retour button was
     * the only exit. Honoured generally rather than special-cased, because
     * it is how a Ren'Py screen says this. */
    {
        const char *act = mr_sl_key_action("game_menu");
        if (act) {
            mr_interp_action(act);
            return;
        }
    }

    if (mr_var_get_int("main_menu", 0))
        return;

    if (g_state == ST_SCREEN)
        close_game_menu();
    else
        mr_interp_show_screen("save");
}

/* --- state capture, for saves and rollback checkpoints --- */

void mr_interp_capture(mr_gamestate *g)
{
    int i;

    memset(g, 0, sizeof(*g));
    g->pc = g_pc;
    g->sp = g_sp;
    for (i = 0; i < g_sp && i < MR_SAVE_STACK; i++)
        g->stack[i] = g_stack[i];

    strncpy(g->who, g_who, sizeof(g->who) - 1);
    strncpy(g->what, g_what, sizeof(g->what) - 1);
    strncpy(g->tlid, g_tlid, sizeof(g->tlid) - 1);
    g->has_dialogue = g_has_dialogue;
    /* Both window flags in the one saved int: bit 0 is the window, bit 1 is
     * auto mode. Packed rather than given a field of its own so that version
     * 4 saves -- which hold a plain 0 or 1 -- still load, arriving with auto
     * off, which is what a save made after any window statement would have
     * had anyway. */
    g->window_shown = g_window_shown | (g_window_auto ? 2 : 0);

    /* A save is taken with the game menu up, so the state to record is the
     * one the menu was entered from, not ST_SCREEN. */
    {
        int st = (g_state == ST_SCREEN) ? g_pre_menu : g_state;

        if (st == ST_WAITCLICK) {
            g->wait_kind = MR_WAIT_CLICK;
            /* An untimed pause can be hard too. No format change: the field
             * is already in the save, it was simply never written or read for
             * this kind of pause. An older save has it zero, which is what
             * every pause that existed when those saves were written was. */
            g->hard_wait = g_hard_wait;
        } else if (st == ST_WAIT) {
            g->wait_kind = MR_WAIT_TIMED;
            g->wait_ms = g_wait_until - g_now;
            if (g->wait_ms < 0)
                g->wait_ms = 0;
            g->hard_wait = g_hard_wait;
        }
    }

    mr_scene_capture(&g->scene);
    mr_audio_current(MR_CH_MUSIC, g->music, sizeof(g->music), &g->music_loop);

    /* The captured step_start values are absolute, on this clock. A writer
     * needs the clock to express them as an elapsed time; a checkpoint, which
     * is restored in this same process, ignores it. */
    g->now = g_now;
}

void mr_interp_restore(const mr_gamestate *g)
{
    int i;

    g_pc = (int)g->pc;
    g_sp = (int)g->sp;
    for (i = 0; i < g_sp && i < MR_SAVE_STACK; i++)
        g_stack[i] = (int)g->stack[i];

    strncpy(g_who, g->who, sizeof(g_who) - 1);
    strncpy(g_what, g->what, sizeof(g_what) - 1);
    strncpy(g_tlid, g->tlid, sizeof(g_tlid) - 1);
    g_has_dialogue = g->has_dialogue;
    g_window_shown = g->window_shown & 1;
    g_window_auto = (g->window_shown & 2) ? 1 : 0;

    /* Which say produced the restored line, in case a checkpoint has to be
     * taken for it later -- switching language does that. A save made at a
     * line of dialogue resumes just past its say, with any `with` the say
     * carried in between. Derived rather than stored so an existing save
     * stays readable. */
    g_say_pc = -1;
    if (g_has_dialogue) {
        int k = g_pc - 1;
        while (k >= 0 && g_s->st[k].kind == MR_ST_WITH)
            k--;
        if (k >= 0 && g_s->st[k].kind == MR_ST_SAY)
            g_say_pc = k;
    }

    /* The speaker's colour, which is not in the save and must not be: it
     * belongs to the Character() definition, so a save carrying it would
     * override a game that had since recoloured someone. Re-derived from the
     * say statement found above, by the same lookup do_say does -- the
     * statement names the character *variable*, which is what indexes the
     * table.
     *
     * Nothing restored this at all, and the symptom was peculiar enough to
     * be worth recording: g_whocolor simply kept whatever the last say in
     * the session had set. Loading a save straight after launching drew the
     * name in black, because the variable was still zero; loading one after
     * playing drew the new speaker's name in the *previous* speaker's
     * colour. The text and the name were both correct -- only the colour was
     * from somewhere else.
     *
     * White is do_say's own default, used here for the same cases: a save
     * taken where no line is up, and a `who` that is a bare string rather
     * than a defined character. */
    g_whocolor = MR_RGB(255, 255, 255);
    if (g_say_pc >= 0) {
        const mr_character *c = mr_script_find_char(g_s,
                                                    g_s->st[g_say_pc].name);
        if (c)
            g_whocolor = c->color;
    }

    /* A restored line is shown whole: it was already read. */
    g_say_complete = 1;
    g_say_start = g_now;

    mr_scene_restore(&g->scene);

    /* A save taken part way through a transform block comes back still
     * running it. Without this the entry kept the placement the file recorded
     * and nothing to move it on, so a long animation -- the credits roll is
     * one 90-second linear step -- loaded frozen part way through and never
     * reached its end, which is what put `fin` over a half-scrolled credits
     * list. Reported from play.
     *
     * Only a state read from a file needs this; apply_checkpoint does not
     * call it, because a checkpoint's timestamps are already on this clock. */
    if (g->anim_relative)
        mr_scene_resume_anim(g_now);

    /* if_changed, so rolling back inside a scene does not restart the track
     * that is already playing. Ren'Py restores the channel's state rather
     * than replaying the statements that set it, so a track that was playing
     * before the rollback and should be playing after it simply keeps going,
     * from where it is rather than from the top.
     *
     * This is the one call site that wants if_changed. The `play music`
     * statement must keep passing it *off*, because `stop music fadeout`
     * followed by `play music` of the same track has to stop and start again
     * -- see MR_ST_PLAY and audiostoptest. */
    if (g->music[0])
        mr_audio_play_ex(MR_CH_MUSIC, g->music, g->music_loop,
                         0, 0, 1.0f, 1);
    else
        mr_audio_stop(MR_CH_MUSIC, 0);

    /* Loading lands on a line of dialogue, waiting for the player -- or on
     * whatever pause the save was taken in. The pause has to win: a title
     * card is `show screen title` then `$ renpy.pause()`, and resuming that
     * as ST_RUN ran straight on through `hide screen title` into the next
     * scene, which looked like the card had never been restored at all. */
    mr_var_set_int("main_menu", 0);
    if (g->wait_kind == MR_WAIT_CLICK) {
        /* An untimed pause can be hard too, so this is restored rather than
         * hardcoded to 0 -- a save taken inside one came back dismissable. */
        g_hard_wait = g->hard_wait;
        g_state = ST_WAITCLICK;

        /* Which statement this pause is, for the same reason g_say_pc is
         * derived above: an untimed pause is a checkpoint, the log holds it
         * under the pause's own pc, and the saved pc is the statement after
         * it. Without this the first Page Up after loading on a title card
         * landed on the pause's own point and re-ran the pause, leaving the
         * card up -- and a second press was needed, exactly as at a menu.
         *
         * Derived rather than stored, so every existing slot gets it: a
         * `pause` statement or the `$` line that called renpy.pause() is
         * immediately before the resume point, and nothing else here can be.
         * A restored *cutscene* is also ST_WAITCLICK and also a `$` line, and
         * it takes no checkpoint -- it is named here anyway, harmlessly: the
         * log's newest point is then some earlier say, the comparison fails
         * either way, and a stale value is what would not be harmless. */
        g_pause_pc = -1;
        if (g_pc > 0 && g_pc <= g_s->n &&
            (g_s->st[g_pc - 1].kind == MR_ST_PAUSE ||
             g_s->st[g_pc - 1].kind == MR_ST_PYTHON))
            g_pause_pc = g_pc - 1;
    } else if (g->wait_kind == MR_WAIT_TIMED) {
        g_hard_wait = g->hard_wait;
        g_wait_until = g_now + g->wait_ms;
        g_state = ST_WAIT;
    } else if (g_pc >= 0 && g_pc < g_s->n &&
               g_s->st[g_pc].kind == MR_ST_MENU) {
        /* A save taken at a choice menu. Its pc is the `menu` statement
         * itself -- exec_one leaves it there while it waits, and
         * mr_interp_choose is what moves it on -- so resuming at it
         * re-executes the menu and offers it again, which is how a rollback
         * to a menu already worked.
         *
         * It has to be asked here, before the dialogue test below, because
         * at a menu there is nearly always a line on screen: the menu's own
         * caption, or the line before it that was never dismissed. Deciding
         * on g_has_dialogue alone made the load ST_SAY -- a line waiting to
         * be advanced -- so the menu statement was not reached until the
         * player supplied that advance. Reported from play as a slot that
         * had the right thumbnail but no menu, with the options appearing
         * only after a click, a space or a wheel notch.
         *
         * A caption comes back with it, since re-running the statement
         * re-runs the caption's say.
         *
         * The log has to be told, or the re-run's checkpoint lands past the
         * menu's own point instead of on it and the first rollback after the
         * load goes to the duplicate -- the same menu again. */
        mr_rollback_resume_at(g_pc);
        g_state = ST_RUN;
    } else {
        g_state = g_has_dialogue ? ST_SAY : ST_RUN;
    }
}


/* --- modal overlays -------------------------------------------------------
 *
 * Ren'Py's Confirm() shows the `confirm` screen with the message and the two
 * actions as arguments. That screen is `modal True` and `zorder 200`, so it
 * draws over whatever menu is up and takes every click.
 *
 * Two things make it awkward here. The screen layer is a list, which is what
 * lets the overlay simply be appended -- but it must not go through
 * mr_interp_show_screen, which is about *menu* screens and would take a save
 * thumbnail, rewrite g_screen_now and clobber the state to return to. And the
 * actions arrive as source text rather than as values, because a button's
 * `action` property is source text all the way down; they are bound as
 * strings and run by name when the button is clicked.
 */

void mr_interp_hide_modal(void)
{
    if (!g_modal_screen[0])
        return;
    mr_scene_hide_screen(g_modal_screen);
    mr_sl_screen_vars_clear(g_modal_screen);
    mr_scene_commit();
    g_modal_screen[0] = '\0';
    g_modal_no[0] = '\0';
}

/* Put the `confirm` screen up with a message already in hand and the two
 * actions as source text.
 *
 * Split out of do_confirm so a caller inside the engine can raise the same
 * dialog without having to build a `Confirm(...)` string and get the quoting
 * right -- the overwrite prompt's text is translated, and embedding
 * translated prose in source text to be re-parsed is a trap waiting for the
 * first language with a quote in it.
 */
static int show_confirm(const char *message, const char *yes, const char *no)
{
    mr_value v;

    if (!mr_interp_find_screen("confirm")) {
        mr_log("interp: confirm wanted but the game has no confirm screen");
        return 0;
    }

    mr_sl_screen_vars_clear("confirm");

    mr_value_set_str(&v, message);
    mr_sl_set_screen_var_for("confirm", "message", &v);

    mr_value_set_str(&v, yes);
    mr_sl_set_screen_var_for("confirm", "yes_action", &v);
    mr_value_set_str(&v, no);
    mr_sl_set_screen_var_for("confirm", "no_action", &v);

    strcpy(g_modal_screen, "confirm");
    strncpy(g_modal_no, no, sizeof(g_modal_no) - 1);
    g_modal_no[sizeof(g_modal_no) - 1] = '\0';

    mr_scene_show_screen("confirm");
    mr_scene_commit();
    return 1;
}

/* `Confirm(message, yes_action, no_action)`. The message is evaluated, the
 * two actions are kept as text. */
static int do_confirm(const char *args)
{
    char msg[MR_MAX_TEXT];
    char yes[256], no[256];
    const char *p = args;
    mr_value v;

    p = mr_read_arg(p, msg, sizeof(msg));
    p = mr_read_arg(p, yes, sizeof(yes));
    (void)mr_read_arg(p, no, sizeof(no));

    memset(&v, 0, sizeof(v));
    mr_expr_eval(msg, &v);
    if (v.type != MR_V_STR)
        mr_value_set_str(&v, msg);

    return show_confirm(v.s, yes, no);
}


void mr_interp_action(const char *expr)
{
    mr_value v;
    const char *open;

    if (!expr || !*expr)
        return;

    expr = mr_skip_ws(expr);

    /* Confirm() needs its arguments unevaluated -- two of them are actions,
     * which are source text, not values -- so it cannot go through the
     * ordinary call path. */
    if (!strncmp(expr, "Confirm", 7) &&
        (open = strchr(expr, '(')) != NULL) {
        do_confirm(open + 1);
        return;
    }

    /* A bare name, with no call syntax, is one of two things. */
    if (!strchr(expr, '(')) {
        /* Either a parameter holding another action as text --
         * `textbutton _("Oui") action yes_action` in the confirm screen -- in
         * which case run what it holds. One level only. */
        if (mr_var_get(expr, &v) && v.type == MR_V_STR && v.s[0]) {
            char inner[256];
            strncpy(inner, v.s, sizeof(inner) - 1);
            inner[sizeof(inner) - 1] = '\0';
            mr_interp_action(inner);
            return;
        }

        /* Or the name of a function the script defined in `init python:` and
         * passed by reference, which is how Confirm() is given its two
         * actions. Ren'Py calls it; so do we, with no arguments. */
        {
            mr_value argv[1];
            const char *argname[1];
            mr_value result;

            memset(argv, 0, sizeof(argv));
            argname[0] = NULL;
            memset(&result, 0, sizeof(result));
            if (builtin_call(expr, argv, argname, 0, &result))
                return;
        }
    }

    mr_expr_eval(expr, &v);
}

/* `row` is an offered row, which is what the player clicked. */
void mr_interp_choose(int row)
{
    int n;

    if (g_state != ST_MENU || row < 0 || row >= g_menu_nvis)
        return;
    /* An option left on screen by config.menu_include_disabled is not
     * pickable. The hit test already refuses it; this is the same rule at the
     * one place that acts on a choice, so no caller can get it wrong. */
    if (!g_menu_sens[row])
        return;

    /* From here on in declared indices: that is what the log records and what
     * the CHOICE statements are numbered by. */
    n = g_menu_vis[row];

    /* Answering a menu ends any future the player had.
     *
     * The general rule -- a checkpoint whose pc differs from the point it
     * lands on means the player branched, so drop the forward log -- cannot
     * see a *converging* menu, and nearly every visual novel menu converges:
     * branch for a line, set a flag, rejoin. After the join the statement is
     * the same one whichever option was taken, so the pc matches, the future
     * survives, and rolling forward re-applies variables captured on the
     * branch that was just abandoned. Measured: take the option that sets a
     * flag, roll back, take the one that does not, press Page Down once, and
     * the flag comes back. branchtest drives exactly that.
     *
     * Cleared unconditionally rather than only when the choice differs,
     * because nothing records which option was taken last time. The cost is
     * that re-making the *same* choice also loses the forward steps, which is
     * a smaller wrong than silently restoring a discarded branch's variables.
     *
     * Ren'Py *is* more generous than this, and an earlier version of this
     * comment claimed otherwise. Its checkpoint carries the interaction's
     * return value -- for a menu, the option taken (menuexports.py:432 is
     * `renpy.exports.checkpoint(rv)`) -- so on the way back through,
     * `roll_forward_info()` hands that option to display_menu, which passes
     * it to the interaction as `roll_forward` after checking it is still one
     * of the choices offered. A RollForward displayable then answers the menu
     * with it on the next press of the key. So in Ren'Py a menu does not stop
     * a rollforward: it costs one keypress like any other interaction, and
     * only a *different* answer, or an answer no longer on offer, ends the
     * future.
     *
     * Which is now what happens -- but through mr_interp_rollforward, not
     * here. Ren'Py's third condition for keeping the future is `keep_rollback
     * or self.rolled_forward` (rollback.py:787), and a click sets neither: it
     * is a fresh decision to go this way, even when it happens to match what
     * was chosen last time. So the clear below stays unconditional and is
     * right for every click, including a click on the same option. */
    mr_rollforward_clear();

    /* Recorded for the way back: a rollback onto this menu has to know what
     * was answered here so a later rollforward can replay it. After the
     * clear, because it is about the *backward* log. */
    mr_rollback_answer(n);

    g_pc = g_s->st[g_menu_first + n].target;
    g_state = ST_RUN;
}

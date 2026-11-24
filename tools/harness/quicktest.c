/* The quick menu: the overlay, its eight actions, and what greys them.
 *
 *   screen quick_menu():
 *       zorder 100
 *       if quick_menu:
 *           hbox: ... eight textbuttons ...
 *
 *   init python:
 *       config.overlay_screens.append("quick_menu")
 *
 * Ren'Py shows everything in config.overlay_screens at each interaction and
 * hides it when the interaction suppresses the overlay -- which the game
 * menu does (00gamemenu.rpy:118), so it is absent from the save screen and
 * the main menu. show_overlay_screens is display/screen.py:1630.
 *
 * The performance question is the one to watch. A screen on the scene makes
 * every frame a full-stage repaint (mr_interp_redraw_kind), and a full 1280x720
 * composite is hundreds of milliseconds on the target. An overlay that is up
 * for the whole game would therefore undo the partial-frame optimisation that
 * makes typing dialogue affordable -- so this checks the quick menu stays
 * inside the dialogue strip, which is what lets a band redraw cover it.
 *
 * Takes the game directory: the quick menu is the real screens.rpy's, and so
 * are the styles that position it.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "anygame.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "font.h"
#include "save.h"
#include "slrender.h"
#include "list.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
#include <stdarg.h>
static char g_dump[65536];
void mr_log(const char *f, ...)
{
    va_list a; char line[512];
    if (strncmp(f, "layout", 6)) return;
    va_start(a, f); vsprintf(line, f, a); va_end(a);
    if (strlen(g_dump) + strlen(line) + 2 < sizeof(g_dump)) {
        strcat(g_dump, line); strcat(g_dump, "\n");
    }
}
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;
static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-50s %-7ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static long g_t;
static void render(void)
{
    g_dump[0] = '\0';
    mr_sl_dump_next();
    mr_sl_focus_reset();
    mr_interp_render(g_st, g_t);
    mr_ui_draw(g_st);               /* as main.c does */
    mr_interp_render_overlay(g_st); /* and the zorder>0 screens over that */
}
static void step(int n)
{
    int i;
    for (i = 0; i < n; i++) { g_t += 33; mr_interp_update(g_t); }
    render();
}

/* Step until nothing is transitioning, then render.
 *
 * The layout dump is only written while the *live* list is being composited
 * (slrender.c:3395 gates on `capture`), and during a transition the screens
 * go into the two snapshots instead. Asserting on the dump mid-transition
 * therefore sees an empty one and reads as "the screen drew nothing". */
static void settle(void)
{
    int i;

    /* Until nothing is in flight, not until the *first* transition ends:
     * chap12 opens with `scene black with fade` and then
     * `show screen title with fade`, so stopping after one leaves the next
     * already running and the dump empty again. mr_scene_in_transition asks
     * without ending anything, unlike mr_scene_busy. */
    for (i = 0; i < 300; i++) {
        g_t += 33;
        mr_interp_update(g_t);
        if (!mr_scene_in_transition())
            break;
    }
    render();
}

static int on_scene(const char *name)
{
    int i;
    for (i = 0; i < mr_scene_screen_count(); i++)
        if (!strcmp(mr_scene_screen_name(i), name))
            return 1;
    return 0;
}

static int drawn(const char *text) { return strstr(g_dump, text) != NULL; }

/* The focus entry whose action mentions `want`, or -1. An insensitive
 * button is not in the list at all, which is how greying is observed. */
static int focus_of(const char *want)
{
    int i;
    for (i = 0; i < 128; i++) {
        const char *a = mr_sl_focus_action(i);
        if (a && strstr(a, want))
            return i;
    }
    return -1;
}

/* A point that hit-tests to this focus entry, found by asking mr_sl_hit
 * rather than by knowing any geometry. Coarse, because a quick-menu button is
 * tens of pixels across. */
static int hit_point(int id, int *ox, int *oy)
{
    int x, y;
    for (y = 0; y < MR_STAGE_H; y += 4)
        for (x = 0; x < MR_STAGE_W; x += 4)
            if (mr_sl_hit(x, y) == id) {
                *ox = x; *oy = y;
                return 1;
            }
    return 0;
}

/* main.c's WM_LBUTTONDOWN, in the same order it does it.
 *
 * It matters that this is the order and not just "run the action": the
 * reported bug was that clicking "Avance rapide" a second time did not stop
 * the skip, and the cause was main.c calling mr_interp_skip_stop() at the top
 * of the handler, before the click was dispatched -- so Skip() always found
 * no skip running and started a fresh one. A click() that only ran the action
 * could not see that, and this test file asserted "clicking again stops it"
 * and passed throughout. Going through mr_sl_hit and cancelling the skip only
 * on the fall-through path is what makes the assertion mean something.
 *
 * This is still a mirror of main.c rather than main.c itself, so the two can
 * drift; the window handler is not reachable from a host build.
 */
static void click_xy(int sx, int sy)
{
    int id = mr_sl_hit(sx, sy);
    const char *a = mr_sl_focus_action(id);

    if (a) {
        mr_sl_focus_scope(id);
        mr_interp_action(a);
        mr_sl_focus_scope_end();
    } else {
        /* A click on the scene ends a toggled skip; a click on a button
         * does not. */
        mr_interp_skip_stop();
        mr_interp_advance();
    }

    mr_interp_update(g_t);
    render();
}

/* What the save screen shows as a slot's label, which mr_save_screenshot
 * picks up from `save_name` on the way into a save. */
static void set_save_name(const char *s)
{
    mr_value v;
    mr_value_set_str(&v, s);
    mr_var_set("save_name", &v);
}

static int click(const char *want)
{
    int id = focus_of(want);
    int x, y;

    if (id < 0)
        return 0;
    if (!hit_point(id, &x, &y))
        return 0;
    click_xy(x, y);
    return 1;
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    (void)argc;
    remove("SAVEQ1.SAV");
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    /* Not "chap12", which is Miwa's. On a script without it nothing runs
     * at all, and the symptom is a long way from the cause: the quick menu
     * still draws, so the early checks pass, and the typing check finds no
     * dialogue in eighty frames and reports a redraw kind of -1. */
    {
        char label[MR_MAX_NAME];

        if (!anygame_story_label(s, label, sizeof(label))) {
            fprintf(stderr, "quicktest: this script has no story labels\n");
            return 1;
        }
        printf("start label: %s\n", label);
        mr_interp_start(label);
    }
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_var_set_int("main_menu", 0);
    settle();

    /* The overlay only exists because screens.rpy's `init python` line
     * `config.overlay_screens.append("quick_menu")` runs. That block used to
     * be parsed for `def`s and otherwise skipped, so the list stayed empty
     * and the menu never appeared -- with nothing in the log to say why. */
    {
        mr_value v;
        int id = mr_var_get("config.overlay_screens", &v) ? mr_list_id(&v) : -1;
        check("the script registered the overlay",
              id >= 0 && mr_list_len(id) == 1, 1);
    }

    /* --- the overlay --- */
    check("the quick menu is up during the story",
          on_scene("quick_menu"), 1);
    check("  and it drew its buttons", drawn("Historique"), 1);

    /* Where it is, because that decides whether a typing frame can stay a
     * partial one. The dialogue strip is the bottom 185 rows. */
    {
        const char *p = strstr(g_dump, "screen 'quick_menu'");
        int x = -1, y = -1, w = 0, h = 0, top = 9999, bot = -1;
        const char *q;

        (void)p;
        /* Every node of the screen, so the extent is the real one. */
        for (q = g_dump; (q = strstr(q, "textbutton [quick")) != NULL; q++) {
            const char *b = strchr(q, ']');
            if (b && sscanf(b + 1, " %d,%d %dx%d", &x, &y, &w, &h) == 4) {
                if (y < top) top = y;
                if (y + h > bot) bot = y + h;
            }
        }
        printf("  quick menu rows %d..%d of %d\n", top, bot, MR_STAGE_H);
        check("  it sits inside the dialogue strip",
              top >= MR_STAGE_H - 185 && bot <= MR_STAGE_H, 1);
    }

    { const char *q = strstr(g_dump, "'quick_menu'");
      if (q) { const char *e = strstr(q + 5, "--- screen");
               printf("%.*s\n", e ? (int)(e - q) : 700, q); } }


    /* --- the cost of having it up ---
     *
     * A screen on the scene makes every frame a full-stage repaint, because
     * a screen can draw anywhere. The quick menu is up for the whole game,
     * so if it were treated that way a line typing itself out would
     * recomposite 921600 pixels per character on a Pentium III -- the exact
     * thing the partial-frame path exists to avoid.
     *
     * It does not have to: the menu occupies rows 699..720, inside the
     * dialogue strip a band redraw already repaints. */
    {
        const char *who, *what;
        mr_u32 col; int shown, i, kind = -1;

        mr_interp_advance();            /* off the title card */

        /* Advance *until* a line is mid-reveal, rather than advancing once
         * and assuming the next frame is the typing state.
         *
         * One advance was enough against Miwa's chap12, where it dismissed
         * a title card and the say after it began typing. It is not enough
         * in general: with no card to dismiss the advance is spent
         * completing the current line's reveal instead, and the loop then
         * steps eighty frames beside a finished line -- mr_interp_dialogue
         * reporting shown = -1 throughout -- and reports a redraw kind of
         * -1 while the engine is behaving correctly. `shown < 0` is the
         * engine saying "this line is fully shown", so that is exactly when
         * to advance. */
        for (i = 0; i < 80; i++) {
            if (mr_interp_dialogue(&who, &what, &col, &shown) && shown < 0)
                mr_interp_advance();
            step(1);
            if (mr_interp_dialogue(&who, &what, &col, &shown) && shown >= 0) {
                /* Not on this frame: the say statement just ran, and a frame
                 * that executed anything is a full one by definition
                 * (g_stepped). The frame after is the steady typing state,
                 * which is the one that has to stay partial. */
                step(1);
                kind = mr_interp_redraw_kind();
                break;
            }
        }
        printf("  redraw kind while typing: %d (NONE=%d DIALOGUE=%d ALL=%d)\n",
               kind, MR_REDRAW_NONE, MR_REDRAW_DIALOGUE, MR_REDRAW_ALL);
        {   int yy, xx; long best = 0; mr_u32 bc = 0;
            for (yy = 695; yy < MR_STAGE_H; yy++)
              for (xx = 0; xx < MR_STAGE_W; xx++) {
                mr_u32 v = g_st->px[(size_t)yy*g_st->pitch+xx] & 0xFFFFFF;
                long b = (v>>16&255)+(v>>8&255)+(v&255);
                if (b > best) { best = b; bc = v; }
              }
            /* The quick menu is `zorder 100`: above the dialogue window,
             * which is painted by the text layer *after* the scene. Drawn
             * underneath instead, the textbox art composites over it and
             * gui.quick_button_text_idle_color (#aaaaaa) arrives as
             * #363636 -- which is what "the buttons are too dim" was. Only
             * visible with a line up, since that is when the box is drawn. */
            printf("  brightest quick-menu pixel with a line up: %06lX\n",
                   (unsigned long)bc);
            check("  it keeps its idle colour over the textbox",
                  (long)bc, 0xAAAAAAL);
        }
        check("a typing line still asks for a partial frame",
              kind, MR_REDRAW_DIALOGUE);
    }

    /* --- hidden wherever a menu is --- */
    mr_interp_show_screen("preferences");
    step(1);
    check("a menu screen hides it", on_scene("quick_menu"), 0);
    mr_interp_game_menu_toggle();
    step(1);
    check("  and closing the menu brings it back",
          on_scene("quick_menu"), 1);

    mr_interp_main_menu();
    step(1);
    check("the main menu hides it", on_scene("quick_menu"), 0);

    mr_interp_action("Start()");
    settle();
    check("starting a game brings it back", on_scene("quick_menu"), 1);

    /* --- `$ quick_menu = False` empties it --- */
    mr_var_set_int("quick_menu", 0);
    settle();
    check("quick_menu = False empties it", drawn("Historique"), 0);
    check("  though the screen is still shown, as in Ren'Py",
          on_scene("quick_menu"), 1);
    mr_var_set_int("quick_menu", 1);
    settle();
    check("  and back on again", drawn("Historique"), 1);

    /* --- what is greyed --- */
    check("Chargement R. is dead with no quick save",
          focus_of("QuickLoad") >= 0, 0);
    check("  Sauvegarde R. is not", focus_of("QuickSave") >= 0, 1);

    check("clicked Sauvegarde R.", click("QuickSave"), 1);
    check("  it wrote the quick save", mr_save_exists("quick-1"), 1);
    check("  and said so", mr_interp_notify_text() != NULL, 1);
    printf("  notify: %s\n", mr_interp_notify_text());
    check("  which wakes Chargement R. up", focus_of("QuickLoad") >= 0, 1);

    /* The message is transient. */
    g_t += 5000;
    mr_interp_update(g_t);
    check("  the message times out", mr_interp_notify_text() == NULL, 1);

    /* --- a quick save keeps a history ---
     *
     * QuickSave is FileSave(1, page="quick", cycle=True), and the cycle is
     * the point: before writing, Ren'Py rotates the first
     * config.quicksave_slots slots (00action_file.rpy:413), so the newest is
     * always quick-1 and the oldest falls off the end. Every quick save used
     * to land in SAVEQ1.SAV and lose the one before it.
     *
     * Checked by content rather than by existence. A rotation that copied the
     * same slot forward, or that stopped after the first step, would leave
     * the right *number* of files -- so what this follows is a single save
     * moving down the slots as further saves push it along. The save name is
     * the chapter title the story last set, so advancing the story between
     * quick saves gives each one something distinguishable in its header. */
    {
        char first[64], second[64];
        const char *n;

        /* Clear the page so the rotation starts from a known state. */
        {
            int k;
            for (k = 1; k <= 10; k++) {
                char slot[16];
                sprintf(slot, "quick-%d", k);
                mr_save_delete(slot);
            }
        }

        /* Each save is given a distinguishable header. `save_name` is what
         * mr_save_screenshot records for the slot label, and at this point in
         * the fixture the story has set neither it nor `title`, so every
         * quick save came out with an empty name -- which made the content
         * checks below pass for any rotation at all, including one that
         * wrote the new save into every slot. Setting it here is what gives
         * them something to tell apart. */
        set_save_name("first");
        mr_interp_action("QuickSave()");
        n = mr_save_name("quick-1");
        strncpy(first, n ? n : "", sizeof(first) - 1);
        first[sizeof(first) - 1] = '\0';
        check("a first quick save landed in slot 1",
              mr_save_exists("quick-1"), 1);
        check("  and nothing is in slot 2 yet",
              mr_save_exists("quick-2"), 0);

        set_save_name("second");
        mr_interp_action("QuickSave()");
        check("a second quick save also lands in slot 1",
              mr_save_exists("quick-1"), 1);
        check("  and pushed the first one into slot 2",
              mr_save_exists("quick-2"), 1);

        n = mr_save_name("quick-2");
        strncpy(second, n ? n : "", sizeof(second) - 1);
        second[sizeof(second) - 1] = '\0';

        /* Without this the content check below is vacuous: if every quick
         * save carried the same name, a rotation that wrote the *new* save
         * into slot 2 would satisfy it just as well. */
        n = mr_save_name("quick-1");
        printf("  slot 1 now '%s', slot 2 '%s', originally '%s'\n",
               n ? n : "(none)", second, first);
        check("  the two saves are distinguishable at all",
              n && strcmp(n, first) != 0, 1);

        check("  and slot 2 holds what slot 1 held before, not a copy of the "
              "new one", !strcmp(second, first), 1);

        /* A third pushes both along. */
        set_save_name("third");
        mr_interp_action("QuickSave()");
        check("a third fills slot 3", mr_save_exists("quick-3"), 1);
        n = mr_save_name("quick-3");
        check("  with the original still recognisable at the bottom",
              n && !strcmp(n, first), 1);

        /* The oldest falls off the end rather than accumulating. With the
         * default of ten slots, an eleventh save must not create quick-11. */
        {
            int k;
            for (k = 0; k < 9; k++)
                mr_interp_action("QuickSave()");
        }
        check("ten slots are kept", mr_save_exists("quick-10"), 1);
        check("  and no eleventh appears", mr_save_exists("quick-11"), 0);

        /* The count is a store variable, not a constant in the C. That is
         * what lets a game override it with `define config.quicksave_slots =
         * N`, the same way it would in Ren'Py: the default is installed
         * before the init phase runs (interp.c), so a script's define lands
         * on top of it. Checked in two parts -- that the default really is in
         * the store, and that the rotation reads it at the time of the save
         * rather than having baked 10 in. */
        check("the count is a variable, defaulted to Ren'Py's 10",
              mr_var_get_int("config.quicksave_slots", -1), 10);

        {
            int k;
            for (k = 1; k <= 11; k++) {
                char slot[16];
                sprintf(slot, "quick-%d", k);
                mr_save_delete(slot);
            }

            mr_var_set_int("config.quicksave_slots", 3);
            for (k = 0; k < 6; k++) {
                set_save_name("capped");
                mr_interp_action("QuickSave()");
            }
            check("a smaller config.quicksave_slots is honoured",
                  mr_save_exists("quick-3"), 1);
            check("  and nothing accumulates past it",
                  mr_save_exists("quick-4"), 0);
            mr_var_set_int("config.quicksave_slots", 10);
        }
    }

    /* --- the quick save has to be reachable ---
     *
     * `if config.has_quicksave:` in screens.rpy is what puts the "Q" page
     * button on the save and load screens (00gamemenu.rpy:63 defaults it
     * true). Undefined here, QuickSave wrote SAVEQ1.SAV and the player had
     * no way to get to it.
     */
    mr_interp_show_screen("load");
    settle();

    /* --- the page label, "Page 1" above the slots ---
     *
     * `input value page_name_value` inside `screen file_slots`, which load
     * and save reach through `use`. It has to survive past the frame its
     * `default` ran on, take the accent colour, and fit on one line. */
    {
        int k;
        for (k = 0; k < 3; k++) {
            settle();
            check(k == 0 ? "the page label is drawn"
                         : "  and is still there on the next frame",
                  drawn("\"Page 1\""), 1);
        }
    }
    /* gui.accent_color is '#0066cc' in this game, not the Ren'Py stock
     * blue -- read it rather than hardcoding a guess. */
    {
        mr_value v;
        long want = 0;
        if (mr_var_get("gui.accent_color", &v) && v.type == MR_V_STR)
            want = strtol(v.s + 1, NULL, 16);
        check("  in the accent colour, not white",
              (long)(mr_sl_style_color("page_label_text", "idle_",
                                       "color", 0) & 0xFFFFFF), want);
    }

    /* One line. gui.label_text_size is 24 and "Page 1" is about 70px, so
     * the box it is laid out in has to be at least that wide. */
    {
        const char *q = strstr(g_dump, "\"Page 1\"");
        const char *line = q;
        int x = 0, y = 0, w = 0, h = 0, tw;
        mr_font *f = mr_ui_font(24);

        while (line && line > g_dump && line[-1] != '\n') line--;
        if (q) sscanf(strchr(line, ']') + 1, " %d,%d %dx%d", &x, &y, &w, &h);
        tw = f ? mr_font_text_width(f, "Page 1") : -1;
        printf("  label box %dx%d, text is %d wide\n", w, h, tw);

        check("  and wide enough not to wrap", w >= tw, 1);
        check("  so it stays on one line", h < 40, 1);
    }

    check("  no A page, since nothing here autosaves",
          drawn("{#auto_page}A"), 0);
    /* The page being shown is drawn selected -- the highlight on "1". */
    check("the current page is selected",
          mr_sl_action_selected("FilePage(1)"), 1);
    check("  and another page is not",
          mr_sl_action_selected("FilePage(2)"), 0);
    check("  nor the quick page, from page 1",
          mr_sl_action_selected("FilePage(\"quick\")"), 0);

    {
        int id = focus_of("FilePage(\"quick\")");
        check("  the Q button is clickable", id >= 0, 1);
        if (id >= 0) {
            mr_sl_focus_scope(id);
            mr_interp_action(mr_sl_focus_action(id));
            mr_sl_focus_scope_end();
            settle();
            /* `slot` in the screen is just 1..6; the page is applied when
             * the engine turns that into a file name. So the question is
             * asked the way the screen asks it: slot 1 is loadable on the
             * quick page, because QuickSave wrote SAVEQ1.SAV above. */
            {
                mr_value v;
                mr_expr_eval("FileLoadable(1)", &v);
                check("  and slot 1 of that page is the quick save",
                      mr_value_truthy(&v), 1);
            }
            check("  the Q button is now the selected one",
                  mr_sl_action_selected("FilePage(\"quick\")"), 1);
            check("  and page 1 is not", mr_sl_action_selected("FilePage(1)"), 0);
            check("  the label follows the page",
                  drawn("Sauvegardes rapides"), 1);
        }
    }
    mr_interp_game_menu_toggle();
    settle();

    /* --- Skip() ---
     *
     * "Texte non lu" on first, or the skip this starts is stopped again by
     * the very next update: the line in front of it has never been read, and
     * stopping there is what that preference means. The button worked all
     * along; the first version of this check just watched the skip end
     * correctly one frame after it began. */
    mr_var_set_int("_preferences.skip", 1);
    check("not skipping", mr_interp_skipping(), 0);
    check("Avance rapide is not lit", mr_sl_action_selected("Skip()"), 0);
    check("clicked Avance rapide", click("Skip("), 1);
    check("  it started a skip", mr_interp_skipping(), 1);
    /* The button has to show it: Skip() is a toggle, and Ren'Py draws it
     * selected while skipping (00action_menu.rpy:332). */
    check("  and the button is lit", mr_sl_action_selected("Skip()"), 1);
    click("Skip(");
    check("  and clicking again stops it", mr_interp_skipping(), 0);
    check("  and the button goes out", mr_sl_action_selected("Skip()"), 0);

    /* Holding Ctrl lights it too: the held skip key sets config.skipping to
     * "slow", the same value the toggle does (behavior.py:350). */
    mr_interp_skip_hold(1);
    check("Ctrl lights it as well", mr_sl_action_selected("Skip()"), 1);
    mr_interp_skip_hold(0);
    check("  and releasing puts it out", mr_sl_action_selected("Skip()"), 0);

    /* A click on the scene stops a skip, which is the behaviour the fix had
     * to preserve while making a click on the button toggle. */
    click("Skip(");
    check("skipping again", mr_interp_skipping(), 1);
    {
        int x, y, id = -1;
        /* Somewhere with no button under it. */
        for (y = MR_STAGE_H / 2; y < MR_STAGE_H && id < 0; y += 4)
            for (x = MR_STAGE_W / 2; x < MR_STAGE_W; x += 4)
                if (mr_sl_hit(x, y) < 0 ||
                    !mr_sl_focus_action(mr_sl_hit(x, y))) {
                    click_xy(x, y);
                    id = 0;
                    break;
                }
        check("  a click on the scene stops it", mr_interp_skipping(), 0);
    }

    /* --- auto-forward --- */
    {
        const char *who = NULL, *what = NULL;
        mr_u32 col; int shown;
        char first[512];
        int i, advanced = 0;

        mr_interp_action("Start()");
        for (i = 0; i < 60 && !mr_interp_dialogue(&who, &what, &col, &shown);
             i++)
            step(1);

        /* NULL-initialised above, and checked here, because the loop can
         * end without finding a line. `what` was an uninitialised pointer
         * and the `what ? what : ""` below dereferenced whatever was on the
         * stack -- a segfault with every assertion already printed `ok`,
         * which reads as a passing test that crashed on the way out. It
         * never fired against a game whose `start` reaches dialogue inside
         * sixty frames; a script that opens with a pause does not. */
        if (!what) {
            check("Start() reaches a line of dialogue", 0, 1);
            printf("\n%d failed\n", g_fail);
            remove("SAVEQ1.SAV");
            return 1;
        }
        strcpy(first, what);

        mr_var_set_int("_preferences.afm_enable", 0);
        for (i = 0; i < 60; i++) step(1);
        mr_interp_dialogue(&who, &what, &col, &shown);
        check("with Auto off the line stays",
              what && !strcmp(what, first), 1);

        mr_var_set_int("_preferences.afm_enable", 1);
        mr_var_set_int("_preferences.afm_time", 1);   /* brisk, for the test */
        for (i = 0; i < 200; i++) {
            step(1);
            mr_interp_dialogue(&who, &what, &col, &shown);
            if (!what || strcmp(what, first)) { advanced = 1; break; }
        }
        check("  with Auto on it advances by itself", advanced, 1);
        mr_var_set_int("_preferences.afm_enable", 0);
    }

    printf("\n%d failed\n", g_fail);
    remove("SAVEQ1.SAV");
    return g_fail != 0;
}

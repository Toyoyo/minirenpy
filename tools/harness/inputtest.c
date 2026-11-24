/* What a key or a click means -- input.c, driven directly.
 *
 * This file exists because for the life of the project the answer to "is the
 * input dispatch order right?" was "read it and see". The logic lived in
 * main.c's window procedure, main.c is linked by no harness, and every fix
 * to the ordering -- and there have been several, each a real bug -- was
 * checked by eye and then *mirrored* into some other harness's helper, which
 * could drift from the real thing and did.
 *
 * The platform seam (src/plat.h) moved that logic to input.c, which knows
 * nothing about messages or scancodes. This drives it.
 *
 * What is covered:
 *
 *   the fit             output pixels to stage pixels, and the letterbox
 *   click dispatch      button before story, and where skip_stop goes
 *   hotkeys             'r' and 'f' claimed ahead of everything
 *   screen key bindings a screen's own `key` beats the default behaviour
 *   the default keys    Page Up/Down, Tab, Ctrl, Escape, the digits
 *   the wheel           scroll, else rollback and rollforward
 *
 * What is not: mr_plat_capture_mouse and mr_plat_restore_size, because the
 * harness links tools/hostshim/plathost.c and its versions do nothing. The
 * 'r' section below asserts the property that matters without them -- that
 * nothing downstream sees the key.
 *
 * Takes the story fixture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "keys.h"
#include "input.h"
#include "scale.h"
#include "expr.h"
#include "interp.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "vfs.h"
#include "ui.h"
#include "slrender.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;

static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-58s %-6ld %s\n", l, got,
           got == want ? "ok" : "** WANT DIFFERENT **");
}

static long g_t;
static void draw(void);

/* A frame: update, then draw. The drawing is not optional here.
 *
 * Both the focus list and the screen-key list are rebuilt during drawing and
 * cleared by mr_sl_focus_reset, so what a click or a key can reach is
 * whatever the last *drawn* frame put there. A harness that updates without
 * drawing is therefore aiming at a stale frame -- this one spent a while
 * reporting that Space would not advance the story, which turned out to be
 * a `key "K_SPACE"` left in the list by a screen that had been taken down
 * several sections earlier and never redrawn over.
 *
 * main.c's frame() does skip drawing when nothing has changed, and keeps the
 * lists for exactly that reason. The difference is that it only skips when
 * the screen has not changed either. */
static void step(int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        g_t += 33;
        mr_interp_update(g_t);
        draw();
    }
}

/* Drawing is what builds the focus list, so a click can only be aimed after
 * a render -- which is also true of the real frame loop, and is why a frame
 * that skips drawing must not reset the focus list.
 *
 * MR_LAYOUT's dump is how a widget's rectangle is found, and it comes out
 * through mr_log, so that is what gets captured. Same trick as bartest. */
static char g_dump[65536];

void mr_log(const char *f, ...)
{
    va_list a;
    char l[512];

    if (strncmp(f, "layout", 6))
        return;
    va_start(a, f);
    vsprintf(l, f, a);
    va_end(a);
    if (strlen(g_dump) + strlen(l) + 2 < sizeof(g_dump)) {
        strcat(g_dump, l);
        strcat(g_dump, "\n");
    }
}

static void draw(void)
{
    mr_sl_focus_reset();
    mr_interp_render(g_st, g_t);
    /* And the overlay, which is where the quick menu lives -- `zorder 100`,
     * drawn after the dialogue box. Leaving it out of a harness's render is
     * how its buttons come back un-hittable: the focus list is built during
     * drawing, so a widget that is not drawn cannot be clicked. main.c's
     * frame() calls both, in this order. */
    mr_interp_render_overlay(g_st);
}

/* One drawn frame with MR_LAYOUT's dump captured, for button_at. */
static void render(void)
{
    g_dump[0] = '\0';
    mr_sl_dump_next();
    draw();
}

/* The centre of the first button whose dumped line contains `text`. Returns
 * 0 if there is no such button, which every caller checks -- aiming a click
 * at (0,0) because the lookup failed is the classic way for one of these to
 * pass vacuously. */
static int button_at(const char *text, int *cx, int *cy)
{
    const char *p = g_dump;

    /* A dumped line is
     *   layout: <indent>textbutton [<style>] x,y WxH "caption"
     * so the rectangle is found by stepping past the style, which can hold
     * anything but a ']'. */
    while ((p = strstr(p, "textbutton [")) != NULL) {
        const char *eol = strchr(p, '\n');
        const char *rect = strchr(p, ']');
        int x, y, w, h;

        if (!eol)
            break;
        if (rect && rect < eol &&
            sscanf(rect + 1, " %d,%d %dx%d", &x, &y, &w, &h) == 4 &&
            w > 0 && h > 0) {
            char buf[512];
            size_t n = (size_t)(eol - p);
            if (n >= sizeof(buf))
                n = sizeof(buf) - 1;
            memcpy(buf, p, n);
            buf[n] = '\0';
            if (strstr(buf, text)) {
                *cx = x + w / 2;
                *cy = y + h / 2;
                return 1;
            }
        }
        p = eol;
    }
    return 0;
}

/* Read the current line, or "" when none is up. */
static const char *line(void)
{
    static char buf[1024];
    const char *who = NULL, *what = NULL;
    mr_u32 col;
    int shown;

    if (!mr_interp_dialogue(&who, &what, &col, &shown) || !what)
        return "";
    strncpy(buf, what, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

/* Step until the line has finished typing itself out.
 *
 * Load-bearing, and the first version of this harness did without it and
 * reported four engine bugs that were all this. A click or a Space on a
 * half-typed line completes the type-out instead of advancing -- correctly,
 * that is what Ren'Py does -- so "the line did not change" meant the input
 * worked, not that it was ignored. Everything here compares lines, so every
 * comparison has to start from a line that is finished. */
static void settle(void)
{
    int i, shown = 0;
    const char *who, *what;
    mr_u32 col;

    for (i = 0; i < 300; i++) {
        /* Waiting for a line to *arrive* as well as to finish, because
         * advancing off a say does not land on the next one: the fixture
         * puts a `scene ... with fade` between them and the dialogue is
         * empty for the length of the transition. Returning at the first
         * sight of no-dialogue made every comparison below read "" and pass
         * against anything. */
        if (mr_interp_dialogue(&who, &what, &col, &shown) && shown < 0)
            return;
        step(1);
    }
}

static int wait_line(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        if (line()[0]) {
            settle();
            return 1;
        }
        step(1);
    }
    return 0;
}

/* Is this screen up? */
static int showing(const char *name)
{
    int i, n = mr_scene_screen_count();
    for (i = 0; i < n; i++)
        if (!strcmp(mr_scene_screen_name(i), name))
            return 1;
    return 0;
}

/* Everything that is up, for the log. Which screen a game-menu toggle
 * actually raises is a fixture detail, and guessing it wrong is how a check
 * on "a menu opened" goes red while the engine is right. */
static void print_screens(const char *when)
{
    int i, n = mr_scene_screen_count();
    printf("  screens %s:", when);
    for (i = 0; i < n; i++)
        printf(" %s", mr_scene_screen_name(i));
    printf("%s\n", n ? "" : " (none)");
}

/* Is any screen up at all? That is the question the game-menu checks are
 * really asking -- the story has none, and a toggle raises one. */
static int any_screen_but_quick_menu(void)
{
    int i, n = mr_scene_screen_count(), c = 0;
    for (i = 0; i < n; i++)
        if (strcmp(mr_scene_screen_name(i), "quick_menu"))
            c++;
    return c;
}

/* Back to the first line of the story with nothing left over.
 *
 * mr_interp_start is not a reset -- it runs the init phase and jumps, and
 * keeps the variables, the scene and the screen layer. Start() is. Without
 * this, a screen raised by one section is still up in the next, and
 * mr_interp_rollback declines while the story is behind a screen, so three
 * checks went red for a reason that had nothing to do with them. */
static void fresh(void)
{
    mr_interp_action("Start()");
    step(2);
    wait_line();
}

/* A point guaranteed to be on the stage and on nothing else: the top-left
 * corner, well away from the dialogue box and from every screen's widgets.
 * Clicking here is "the player clicked the scene". */
#define SCENE_X 4
#define SCENE_Y 4

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    (void)argc;

    /* --- the fit: output pixels to stage pixels ---
     *
     * Before any of the engine is up, because none of it is involved. This
     * is mr_fit_*, which used to be three statics inside present.c and is
     * the reason input.c could not be linked here at all. */
    {
        int x = -1, y = -1;

        mr_fit_set(MR_STAGE_W, MR_STAGE_H);
        check("1:1 maps the origin to the origin",
              mr_fit_map_point(0, 0, &x, &y) && x == 0 && y == 0, 1);
        check("  and the far corner to the far corner",
              mr_fit_map_point(MR_STAGE_W - 1, MR_STAGE_H - 1, &x, &y) &&
              x == MR_STAGE_W - 1 && y == MR_STAGE_H - 1, 1);
        check("  one pixel past the corner is off the stage",
              mr_fit_map_point(MR_STAGE_W, 0, &x, &y), 0);

        /* Half size: exactly the default window. */
        mr_fit_set(MR_WIN_W, MR_WIN_H);
        check("at half size a click doubles",
              mr_fit_map_point(100, 50, &x, &y) && x == 200 && y == 100, 1);

        /* 4:3, which is what the real target's desktop is. 800x600 holds a
         * 800x450 stage with a 75-pixel bar above and below. */
        mr_fit_set(800, 600);
        check("4:3 output letterboxes top and bottom",
              mr_fit_map_point(400, 10, &x, &y), 0);
        check("  the row below the bar is the top of the stage",
              mr_fit_map_point(400, 75, &x, &y) && y == 0, 1);
        check("  the row above it is not",
              mr_fit_map_point(400, 74, &x, &y), 0);
        check("  and the bottom bar is off the stage too",
              mr_fit_map_point(400, 599, &x, &y), 0);

        /* A window dragged narrower than the stage's aspect gets bars at the
         * sides instead. 360x360: a 360x202 stage, 79 above. */
        mr_fit_set(360, 360);
        check("a tall output letterboxes, not stretches",
              mr_fit_map_point(180, 5, &x, &y), 0);

        /* Degenerate sizes are declined and leave the last fit standing,
         * which is what a minimised window needs. */
        mr_fit_set(4, 4);
        check("a 4x4 output is refused, the old fit stands",
              mr_fit_map_point(180, 180, &x, &y), 1);
    }

    /* --- bring the story up --- */
    remove("PERSIST.SAV");
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* 1:1 for the rest of the run, so a stage coordinate read out of the
     * layout dump can be clicked without converting. */
    mr_fit_set(MR_STAGE_W, MR_STAGE_H);

    check("a line is up", wait_line(), 1);

    /* --- a click on the scene advances the story --- */
    {
        char before[1024];
        strcpy(before, line());
        render();
        mr_input_mouse_down(MR_MB_LEFT, SCENE_X, SCENE_Y);
        mr_input_mouse_up(MR_MB_LEFT);
        step(3);
        settle();
        check("a click on the scene advances", strcmp(line(), before) != 0, 1);
    }

    /* --- a click on a button runs its action instead ---
     *
     * The quick menu is the fixture for this: it is up during the story and
     * its buttons sit over the scene, which is exactly the overlap the
     * dispatch order exists to resolve.
     */
    {
        int bx = 0, by = 0;
        char before[1024];

        mr_interp_show_screen("quick_menu");
        step(1);
        render();

        if (!button_at("Auto", &bx, &by)) {
            /* The dump is how every widget rectangle in this file is found,
             * so if it is empty nothing below can be aimed and the whole
             * section would pass vacuously. Say so loudly. */
            printf("  no Auto button in a %d-byte layout dump\n",
                   (int)strlen(g_dump));
        }
        check("the quick menu has an Auto button",
              button_at("Auto", &bx, &by), 1);

        if (bx || by) {
            int auto_before = mr_var_get_int("_preferences.afm_enable", 0);

            strcpy(before, line());
            mr_input_mouse_down(MR_MB_LEFT, bx, by);
            mr_input_mouse_up(MR_MB_LEFT);
            step(1);

            check("  clicking it runs its action",
                  mr_var_get_int("_preferences.afm_enable", 0) != auto_before,
                  1);
            check("  and does not advance the story",
                  !strcmp(line(), before), 1);

            /* Put it back; auto-forward would move the story under us. */
            mr_var_set_int("_preferences.afm_enable", auto_before);
        }
    }

    /* --- where mr_interp_skip_stop goes ---
     *
     * The bug this pins: skip_stop used to run at the *top* of the click
     * handler, before the dispatch. That made the quick menu's "Avance
     * rapide" impossible to switch off -- the click cancelled the skip, then
     * Skip() found none in progress and started another, so the button
     * toggled on for ever. Ren'Py puts it in SayBehavior.event
     * (behavior.py:773), which only acts when it is the focused thing, so a
     * click consumed by a button never reaches it.
     *
     * Checked on a button that is *not* the skip button, so that what is
     * being measured is the ordering and not Skip()'s own toggle.
     */
    {
        int bx = 0, by = 0;

        mr_var_set_int("_preferences.skip", 1);   /* may cross unread text */
        render();

        mr_interp_skip_toggle();
        check("Tab-equivalent starts a skip", mr_interp_skipping(), 1);

        if (button_at("Auto", &bx, &by)) {
            mr_input_mouse_down(MR_MB_LEFT, bx, by);
            mr_input_mouse_up(MR_MB_LEFT);
            check("  a click consumed by a button does not cancel it",
                  mr_interp_skipping(), 1);
            mr_var_set_int("_preferences.afm_enable", 0);
        } else {
            check("  a click consumed by a button does not cancel it", -1, 1);
        }

        /* ...but a click on the scene does: the player reaching for the
         * mouse is the player wanting to read again. */
        mr_input_mouse_down(MR_MB_LEFT, SCENE_X, SCENE_Y);
        mr_input_mouse_up(MR_MB_LEFT);
        check("  a click on the scene does cancel it",
              mr_interp_skipping(), 0);

        /* The same rule for keys, with the same exception list. */
        mr_interp_skip_toggle();
        check("a key that is not a hotkey cancels a skip too",
              (mr_input_key_down(MR_KEY_SPACE), mr_interp_skipping()), 0);
        step(2);
    }

    /* --- 'r' and 'f' are claimed ahead of everything ---
     *
     * They are display hotkeys, not game input. The property that can be
     * checked without a platform spy is that nothing downstream sees them:
     * 'r' must not advance the story and must not end a skip, which is the
     * exact wording of the comment in input.c.
     */
    {
        char before[1024];

        mr_var_set_int("_preferences.skip", 1);
        wait_line();
        strcpy(before, line());
        mr_interp_skip_toggle();
        check("skipping, for the hotkey checks", mr_interp_skipping(), 1);

        mr_input_key_down('R');
        check("'r' does not advance the story", !strcmp(line(), before), 1);
        check("  and does not end the skip -- it is a window key",
              mr_interp_skipping(), 1);

        mr_interp_skip_stop();
        step(2);
    }

    {
        mr_value v;
        int was;

        mr_value_set_str(&v, "window");
        mr_var_set("_preferences.display", &v);

        mr_input_key_down('F');
        was = mr_var_get("_preferences.display", &v) && v.type == MR_V_STR &&
              !strcmp(v.s, "fullscreen");
        check("'f' writes the display preference", was, 1);

        mr_input_key_down(MR_KEY_F11);
        check("  and F11 writes it back",
              mr_var_get("_preferences.display", &v) && v.type == MR_V_STR &&
              !strcmp(v.s, "window"), 1);

        /* Not the platform: the preference is the single source of truth and
         * the frame loop turns it into a call. See "Fullscreen is a
         * borderless window" in NOTES. */
    }

    /* --- a screen's own `key` beats the default behaviour ---
     *
     * screens.rpy:1792, `key "K_SPACE" action ShowMenu("extras_rythm")` on
     * rhythm_game_highscores. Space would otherwise advance the story.
     */
    {
        mr_interp_show_screen("rhythm_game_highscores");
        step(2);
        render();
        check("the high-score screen is up", showing("rhythm_game_highscores"),
              1);

        mr_input_key_down(MR_KEY_SPACE);
        step(2);
        check("  its own K_SPACE binding runs instead of advancing",
              !showing("rhythm_game_highscores"), 1);

        fresh();
    }

    /* --- the default keys --- */
    {
        char a[1024], b[1024];

        /* From a clean story, and say so if it is not: the section above
         * leaves a menu screen up, a screen suppresses the dialogue
         * entirely, and every comparison here reads the dialogue. The first
         * version of this section compared "" with "" for six checks. */
        fresh();
        print_screens("before the key checks");
        check("no screen is in the way", any_screen_but_quick_menu(), 0);

        strcpy(a, line());
        check("  and a line is up", a[0] != '\0', 1);

        /* Only a keysym some *visible* screen bound may match. The story has
         * only the quick menu up and it binds no keys, so this must decline
         * -- if it does not, a binding on a screen that is merely *defined*
         * is swallowing Space, and the four checks below would go red for a
         * reason that is nothing to do with them. */
        check("  no visible screen claims K_SPACE",
              mr_interp_screen_key("K_SPACE"), 0);
        mr_input_key_down(MR_KEY_SPACE);
        step(3);
        settle();
        strcpy(b, line());
        check("Space advances to another line", b[0] && strcmp(a, b) != 0, 1);

        /* The direct call first, as the control. If this is red the fault is
         * in the rollback log and not in anything input.c did; if it is
         * green and the key below is red, the key is not reaching it. */
        mr_interp_rollback();
        step(3);
        settle();
        check("mr_interp_rollback goes back to it", !strcmp(line(), a), 1);
        mr_interp_rollforward();
        step(3);
        settle();

        mr_input_key_down(MR_KEY_PAGEUP);
        step(3);
        settle();
        check("Page Up rolls back to it", !strcmp(line(), a), 1);

        mr_input_key_down(MR_KEY_PAGEDOWN);
        step(3);
        settle();
        check("Page Down rolls forward again", !strcmp(line(), b), 1);

        /* Backspace is the second rollback binding, and the one the real
         * target uses -- Windows 3.1 has no wheel. */
        mr_input_key_down(MR_KEY_BACKSPACE);
        step(3);
        settle();
        check("Backspace rolls back as well", !strcmp(line(), a), 1);
        mr_input_key_down(MR_KEY_PAGEDOWN);
        step(3);
        settle();
    }

    {
        check("Ctrl down holds a skip",
              (mr_input_key_down(MR_KEY_CTRL), mr_interp_skipping()), 1);
        check("  Ctrl up releases it",
              (mr_input_key_up(MR_KEY_CTRL), mr_interp_skipping()), 0);

        /* Without this, losing the keyboard with Ctrl held leaves the skip
         * held forever: no key-up is coming, because the key goes up in
         * another window. */
        mr_input_key_down(MR_KEY_CTRL);
        check("losing the keyboard releases a held skip",
              (mr_input_focus_lost(), mr_interp_skipping()), 0);

        check("Tab toggles a skip on",
              (mr_input_key_down(MR_KEY_TAB), mr_interp_skipping()), 1);
        check("  and off again",
              (mr_input_key_down(MR_KEY_TAB), mr_interp_skipping()), 0);
        step(2);
    }

    /* Escape opens the game menu and closes it, the same binding as the
     * right-click. It used to close the window. */
    {
        fresh();
        print_screens("in the story");
        check("the story is not behind a screen",
              any_screen_but_quick_menu(), 0);

        mr_input_key_down(MR_KEY_ESCAPE);
        step(2);
        print_screens("after Escape");
        check("Escape opens the game menu", any_screen_but_quick_menu() > 0, 1);

        mr_input_key_down(MR_KEY_ESCAPE);
        step(2);
        check("  and closes it again", any_screen_but_quick_menu(), 0);
    }

    /* The right button is the same binding: Ren'Py gives Escape and the
     * right-click the same "game_menu" keysym. */
    {
        mr_input_mouse_down(MR_MB_RIGHT, SCENE_X, SCENE_Y);
        step(2);
        check("right-click opens it too", any_screen_but_quick_menu() > 0, 1);
        mr_input_mouse_down(MR_MB_RIGHT, SCENE_X, SCENE_Y);
        step(2);
        check("  and closes it", any_screen_but_quick_menu(), 0);
    }

    /* --- the wheel ---
     *
     * Up is rollback and down is rollforward: `rollforward` includes
     * 'mousedown_5' (00keymap.rpy:66). Down is *not* a second way to
     * advance -- with no rollback outstanding it does nothing.
     */
    {
        char a[1024], b[1024];

        fresh();
        strcpy(a, line());
        mr_input_key_down(MR_KEY_SPACE);
        step(3);
        settle();
        strcpy(b, line());

        mr_input_wheel(1, SCENE_X, SCENE_Y);
        step(3);
        settle();
        check("wheel up rolls back", !strcmp(line(), a), 1);

        mr_input_wheel(-1, SCENE_X, SCENE_Y);
        step(3);
        settle();
        check("wheel down rolls forward", !strcmp(line(), b), 1);

        mr_input_wheel(-1, SCENE_X, SCENE_Y);
        step(3);
        settle();
        check("  wheel down with nothing outstanding does not advance",
              !strcmp(line(), b), 1);

        /* A wheel in the letterbox is not a wheel on the stage. 1:1 here, so
         * anything past the stage width is outside it. */
        strcpy(a, line());
        mr_input_wheel(1, MR_STAGE_W + 10, 10);
        step(3);
        settle();
        check("a wheel outside the stage is ignored", !strcmp(line(), a), 1);
    }

    /* A click in the letterbox is ignored for the same reason. */
    {
        char before[1024];

        wait_line();
        strcpy(before, line());
        mr_input_mouse_down(MR_MB_LEFT, MR_STAGE_W + 10, 10);
        mr_input_mouse_up(MR_MB_LEFT);
        step(3);
        settle();
        /* Still advances: a click that hits nothing advances the story, and
         * the letterbox hits nothing. This is the behaviour, not a bug --
         * what must not happen is it being read as a hit on whatever widget
         * sits at the clamped coordinate. */
        if (!strcmp(line(), before))
            printf("  letterbox click: still on \"%.40s\"\n", before);
        check("a click in the letterbox hits no widget",
              strcmp(line(), before) != 0, 1);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

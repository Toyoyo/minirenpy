/* Clicking "Souris" on the help screen must swap the keyboard panel for the
 * mouse one, and the choice must survive the next frame. */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "anygame.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "slrender.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
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
static void dump_reset(void) { g_dump[0] = '\0'; mr_sl_dump_next(); }
static int dump_has(const char *t) { return strstr(g_dump, t) != NULL; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

/* Counts, and main returns it. Six harnesses printed their verdict and
 * exited 0 regardless, which run.sh reads as a pass; see NOTES, "A test
 * that cannot go red". */
static int fails;

static void check(const char *l, long got, long want)
{
    printf("%-38s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

/* Find the focus entry whose action mentions `want`, and click it. */
static int click_action(const char *want)
{
    int i;
    for (i = 0; i < 64; i++) {
        const char *a = mr_sl_focus_action(i);
        if (a && strstr(a, want)) {
            mr_sl_focus_scope(i);
            mr_interp_action(a);
            mr_sl_focus_scope_end();
            return 1;
        }
    }
    return 0;
}

/* Does the rendered screen contain this text anywhere? */
static int g_found;
static const char *g_needle;

int main(int argc, char **argv)
{
    mr_script *s;
    mr_value v;

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_show_screen("help");
    mr_interp_update(0);

    /* What the screen actually draws is the test: "Entrée" belongs to the
     * keyboard panel, "Bouton gauche" to the mouse one. */
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 0);
    check("opens on the keyboard panel", dump_has("Entr"), 1);
    check("  mouse panel not shown", dump_has("Bouton gauche"), 0);

    check("found the Souris button", click_action("\"mouse\""), 1);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 33);
    check("clicking shows the mouse panel", dump_has("Bouton gauche"), 1);
    check("  keyboard panel gone", dump_has("Entr\u00e9e"), 0);

    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 66);
    check("still the mouse panel next frame", dump_has("Bouton gauche"), 1);

    check("found the Clavier button", click_action("\"keyboard\""), 1);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 99);
    check("back to the keyboard panel", dump_has("Bouton gauche"), 0);

    /* Re-showing the screen resets it to the default. */
    click_action("\"mouse\"");
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 132);
    check("  on mouse before reopening", dump_has("Bouton gauche"), 1);
    mr_interp_show_screen("preferences");
    mr_interp_show_screen("help");
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 165);
    check("reopening resets to keyboard", dump_has("Bouton gauche"), 0);

    /* --- the way out of a sub-menu opened from the main menu ---
     *
     * `screen game_menu` ends with `if main_menu: key "game_menu" action
     * ShowMenu("main_menu")`. In game there is a story underneath and the
     * right-click closes the menu onto it; before the game starts there is
     * nothing to close onto, so that binding is the only exit besides the
     * Retour button -- and until it was honoured, Escape and the right-click
     * simply did nothing in Preferences or About.
     *
     * Both halves are checked here because the risk is symmetrical: a
     * binding read without regard to `if main_menu` would send the in-game
     * right-click to the main menu, which is much worse than the bug.
     */
    mr_interp_main_menu();
    mr_interp_update(200);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 200);
    check("at the main menu", dump_has("Lecture"), 1);

    mr_interp_show_screen("preferences");
    mr_interp_update(233);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 233);
    check("in Preferences", dump_has("Volume de la musique"), 1);
    check("  and it binds the game_menu keysym",
          mr_sl_key_action("game_menu") != NULL, 1);

    mr_interp_game_menu_toggle();          /* Escape, or the right-click */
    mr_interp_update(266);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 266);
    /* "Lecture" alone would not do: the navigation column is drawn on the
     * sub-screen too, so it is there either way and the check would pass
     * against the bug. What tells them apart is Preferences going away, and
     * the game_menu binding going with it. */
    check("Escape leaves Preferences", dump_has("Volume de la musique"), 0);
    check("  for the main menu", dump_has("Lecture"), 1);
    check("  which binds nothing itself",
          mr_sl_key_action("game_menu") == NULL, 1);

    /* In game the same keysym must close the menu onto the story instead.
     * `main_menu` is the flag the screen's `if` reads, and it is Start()
     * that clears it -- mr_interp_start only jumps, so setting it here is
     * what makes this the in-game case rather than another main-menu one. */
    anygame_start_story(s);
    mr_var_set_int("main_menu", 0);
    mr_interp_update(300);
    mr_interp_show_screen("preferences");
    mr_interp_update(333);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 333);
    check("in game, Preferences binds nothing",
          mr_sl_key_action("game_menu") == NULL, 1);
    mr_interp_game_menu_toggle();
    mr_interp_update(366);
    dump_reset(); mr_sl_focus_reset(); mr_interp_render(g_st, 366);
    /* Not "no screens at all": the quick menu is up during the story, which
     * is why this asks whether Preferences went away rather than counting. */
    check("  and Escape closes the menu onto the story",
          dump_has("Volume de la musique"), 0);
    check("  without going to the main menu", dump_has("Lecture"), 0);

    (void)g_found; (void)g_needle; (void)v;

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

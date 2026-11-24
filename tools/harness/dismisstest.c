/* `dismiss action X` -- a click anywhere that nothing else took.
 *
 * The high-scores screen uses it to close on a tap:
 *
 *     screen rhythm_game_highscores():
 *         key "K_SPACE" action ShowMenu("extras_rythm")
 *         dismiss        action ShowMenu("extras_rythm")
 *         use game_menu(_("Meilleurs scores")):
 *
 * `dismiss` was in slang.c's keyword list -- so it parsed, and no error was
 * ever reported -- but no renderer case existed, so it was silently dropped
 * and clicking did nothing.
 *
 * The half that is easy to get wrong is priority. dismiss must be the
 * *lowest*, or it eats the clicks meant for the navigation column and the
 * scrollbar, and the screen becomes impossible to use rather than merely
 * inert. Both directions are checked here.
 *
 * Takes the game directory.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
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
    printf("%-48s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static void render(void)
{
    g_dump[0] = '\0';
    mr_sl_dump_next();
    mr_sl_focus_reset();
    mr_interp_render(g_st, 0);
}

/* Which screen is up, judged by what it draws -- there is no accessor for
 * the scene's screen names, and what the player sees is the better test
 * anyway. Re-renders, so the focus list is rebuilt too.
 *
 * The needle has to be unique to one screen: "Meilleurs scores" is the high
 * scores screen's title *and* the label of the tile on extras_rythm that
 * opens it, so it reads as present on both and the first version of this
 * test reported the screen had not closed when it had. */
static int showing(const char *title)
{
    render();
    return strstr(g_dump, title) != NULL;
}

/* The rectangle of the textbutton whose label contains `needle`. */
static int rect_of(const char *needle, int *x, int *y, int *w, int *h)
{
    const char *p = g_dump;
    while ((p = strstr(p, "textbutton [")) != NULL) {
        const char *eol = strchr(p, '\n');
        char line[512];
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p += 12;
        if (!strstr(line, needle))
            continue;
        if (sscanf(strchr(line, ']') + 1, " %d,%d %dx%d", x, y, w, h) == 4)
            return 1;
    }
    return 0;
}

/* Click at a stage coordinate through the real hit test. */
static const char *click_at(int sx, int sy)
{
    int id = mr_sl_hit(sx, sy);
    const char *a;

    if (id < 0)
        return NULL;
    a = mr_sl_focus_action(id);
    if (!a)
        return NULL;
    mr_sl_focus_scope(id);
    mr_interp_action(a);
    mr_sl_focus_scope_end();
    mr_interp_update(0);
    return a;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int bx, by, bw, bh;

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_show_screen("rhythm_game_highscores");
    mr_interp_update(0);
    render();
    check("the high scores screen is up",
          showing("Appuyez sur Espace"), 1);

    /* --- the navigation must win, so check it first --- */
    check("found the Retour button", rect_of("Retour", &bx, &by, &bw, &bh), 1);
    {
        const char *a = click_at(bx + bw / 2, by + bh / 2);
        printf("  clicking Retour ran: %s\n", a ? a : "(nothing)");
        check("  a click on Retour is not the dismiss",
              a && !strstr(a, "extras_rythm"), 1);
    }

    /* --- and now the empty space --- */
    mr_interp_show_screen("rhythm_game_highscores");
    mr_interp_update(0);
    render();
    check("back on the high scores screen",
          showing("Appuyez sur Espace"), 1);

    /* Top-left: inside the screen, on nothing. Before dismiss existed this
     * hit nothing at all and the click was swallowed. */
    {
        const char *a = click_at(4, 4);
        printf("  clicking empty space ran: %s\n", a ? a : "(nothing)");
        check("  a click on nothing dismisses", a != NULL, 1);
        check("  and it is the dismiss action",
              a && strstr(a, "extras_rythm") != NULL, 1);
    }
    mr_interp_update(0);
    check("  the high scores screen is gone",
          showing("Appuyez sur Espace"), 0);
    check("  replaced by the extras menu", showing("Jeu de rythme"), 1);

    /* --- the same screen's `key "K_SPACE"` --- */
    mr_interp_show_screen("rhythm_game_highscores");
    mr_interp_update(0);
    check("on the high scores screen again",
          showing("Appuyez sur Espace"), 1);
    check("  it binds K_SPACE", mr_sl_key_action("K_SPACE") != NULL, 1);
    check("  and pressing it is handled",
          mr_interp_screen_key("K_SPACE"), 1);
    mr_interp_update(0);
    check("  which closes the screen", showing("Appuyez sur Espace"), 0);

    /* A keysym nobody bound must fall through, or the default keyboard
     * handling in main.c would be shadowed. */
    check("an unbound keysym is not handled",
          mr_interp_screen_key("K_HOME"), 0);

    /* --- a screen with no dismiss must not grow one --- */
    mr_interp_show_screen("preferences");
    mr_interp_update(0);
    render();
    check("preferences is up", showing("Volume de la musique"), 1);
    check("  clicking its empty corner does nothing",
          click_at(4, 4) == NULL, 1);
    check("  and it is still up", showing("Volume de la musique"), 1);

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

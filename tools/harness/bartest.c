/* A bar's style comes from its *value*, not from the `bar` keyword.
 *
 * sldisplayables.sl2bar asks value.get_style() whenever the screen gives no
 * explicit style, so `bar value Preference("music volume")` is a "slider" and
 * picks up `style slider_slider: xsize 350`, while the jukebox's
 * `bar value AudioPositionValue()` keeps the base "bar" and no width at all.
 *
 * Read as plain `bar`, the preference sliders inherited no width and stretched
 * to fill their row -- visibly wider than Ren'Py's. The temptation is to give
 * bars a default width instead, which is why the jukebox half is checked here
 * too: that one is *supposed* to fill its viewport, and a width default would
 * shrink it back to the 350px it used to be wrong at.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
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
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

/* Counts, and main returns it. Six harnesses printed their verdict and
 * exited 0 regardless, which run.sh reads as a pass; see NOTES, "A test
 * that cannot go red". */
static int fails;

static void check(const char *l, long got, long want)
{
    printf("%-44s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

/* Pull the first "bar [<style>] x,y WxH" out of the dump. */
static int bar_line(char *style, int *w, int *h)
{
    const char *p = g_dump;
    while ((p = strstr(p, "bar [")) != NULL) {
        int x, y;
        /* "vbar [" and "scrollbar [" also end in "bar [", so require a space
         * in front: the dump separates the type from its indent with one. */
        if (p == g_dump || p[-1] != ' ') { p += 5; continue; }
        if (sscanf(p, "bar [%63[^]]] %d,%d %dx%d", style, &x, &y, w, h) == 5)
            return 1;
        p += 5;
    }
    return 0;
}

static void render(void)
{
    g_dump[0] = '\0';
    mr_sl_dump_next();
    mr_sl_focus_reset();
    mr_interp_render(g_st, 0);
}

int main(int argc, char **argv)
{
    mr_script *s;
    char style[64];
    int w = 0, h = 0;

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* --- a preference bar is a slider --- */
    mr_interp_show_screen("preferences");
    mr_interp_update(0);
    render();

    check("found a bar on the preferences screen", bar_line(style, &w, &h), 1);
    /* style_prefix "slider" on the hbox + the value's own "slider". */
    check("  its style is slider_slider", !strcmp(style, "slider_slider"), 1);
    /* `style slider_slider: xsize 350` in screens.rpy. Read as plain `bar`
     * this came out at the full width of the row instead. */
    check("  350 wide, as the style asks", w, 350);
    /* `style slider: ysize gui.slider_size`, which is 25. This was already
     * right by luck: gui.bar_size is 25 too, so the wrong style gave the
     * right thickness and only the width showed the bug. */
    check("  and 25 thick", h, 25);

    /* --- the jukebox bar is not --- */
    mr_interp_show_screen("music_room");
    mr_interp_update(33);
    render();

    if (!bar_line(style, &w, &h)) {
        printf("%-44s %-6s %s\n", "found the jukebox bar", "no",
               "** WANT DIFFERENT **");
    } else {
        /* Not a bare "bar": music_room lives inside `use game_menu(...)`,
         * which puts style_prefix "game_menu" in force, so the name is
         * game_menu_bar -- and the implicit-parent rule walks that back to
         * `bar`. What matters is the last component, i.e. that the value did
         * not turn this into a slider. */
        size_t n = strlen(style);
        check("jukebox bar keeps the base style",
              n >= 3 && !strcmp(style + n - 3, "bar") &&
              !strstr(style, "slider") && !strstr(style, "scroll"), 1);
        /* AudioPositionValue does not override get_style, so no xsize applies
         * and the bar fills what it is offered -- far more than 350. */
        check("  and still fills its viewport", w > 600, 1);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

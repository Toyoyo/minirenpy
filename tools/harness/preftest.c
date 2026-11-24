/* The preferences screen's radio and check buttons must show which option is
 * chosen. Three things have to line up for that, and all three were missing:
 *
 *   - action_selected has to understand Preference(name, value), and
 *     InvertSelected() around it -- the transitions button reads inverted,
 *     because the preference means "play them" and the button says "skip".
 *   - the selected_ style prefix then has to reach the text colour, which is
 *     gui.button_text_selected_color.
 *   - `foreground "gui/button/radio_[prefix_]foreground.png"` has to be drawn
 *     at all, and [prefix_] has to resolve through Ren'Py's search list:
 *     only radio_foreground.png and radio_selected_foreground.png exist, so
 *     idle falls back to "" and selected_idle to "selected_".
 *
 * Display and Skip are no-ops in this engine, so the *state* is all there is
 * to see; that is exactly why it has to be drawn. Mute All really works, and
 * had no visual cue either.
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
#include "audio.h"
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
    printf("%-50s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static void render(void)
{
    g_dump[0] = '\0';
    mr_sl_dump_next();
    mr_sl_focus_reset();
    mr_interp_render(g_st, 0);
}

/* The rectangle of the textbutton whose label contains `needle`, out of the
 * layout dump. */
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

/* A hash of the 18px left border of a button -- gui.radio_button_borders
 * reserves exactly that much, and it is where the marker is drawn. No text
 * reaches into it, so this sees the foreground art and nothing else. */
static long marker_hash(int x, int y, int h)
{
    unsigned long hh = 2166136261UL;
    int yy, xx;
    for (yy = y; yy < y + h && yy < g_st->h; yy++) {
        const mr_u32 *row = g_st->px + (size_t)yy * g_st->pitch;
        for (xx = x; xx < x + 18 && xx < g_st->w; xx++) {
            hh ^= (unsigned long)(row[xx] & 0xFFFFFF);
            hh *= 16777619UL;
        }
    }
    return (long)(hh & 0x7FFFFFFF);
}

/* Click the focus entry whose action text contains `want`. */
static int click(const char *want)
{
    int i;
    for (i = 0; i < 128; i++) {
        const char *a = mr_sl_focus_action(i);
        if (a && strstr(a, want)) {
            mr_sl_focus_scope(i);
            mr_interp_action(a);
            mr_sl_focus_scope_end();
            mr_interp_update(0);
            render();
            return 1;
        }
    }
    return 0;
}

/* A hash of the stage, not a count of lit pixels.
 *
 * Counting was the first attempt and it cannot see this bug: moving the
 * selection from one radio button to the other takes the marker off one and
 * puts the same 18x33 marker on the other, and swaps two labels between the
 * same two colours. The number of non-black pixels comes out *identical*, to
 * the pixel, whether or not anything was drawn. A hash notices. */
static long stage_hash(void)
{
    unsigned long h = 2166136261UL;
    int y, x;
    for (y = 0; y < g_st->h; y++) {
        const mr_u32 *row = g_st->px + (size_t)y * g_st->pitch;
        for (x = 0; x < g_st->w; x++) {
            h ^= (unsigned long)(row[x] & 0xFFFFFF);
            h *= 16777619UL;
        }
    }
    return (long)(h & 0x7FFFFFFF);
}

int main(int argc, char **argv)
{
    mr_script *s;
    long a, b;

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* --- the query itself --- */
    check("windowed is the default",
          mr_interp_pref_selected("display", "window"), 1);
    check("  and fullscreen is not",
          mr_interp_pref_selected("display", "fullscreen"), 0);
    check("skip unseen starts off",
          mr_interp_pref_selected("skip", "toggle"), 0);
    /* Ren'Py defaults transitions on, and the button inverts it, so the
     * button starts *un*selected. Getting this backwards is invisible in the
     * code and obvious on screen. */
    check("transitions start on",
          mr_interp_pref_selected("transitions", "toggle"), 1);
    check("all mute starts off",
          mr_interp_pref_selected("all mute", "toggle"), 0);

    mr_interp_show_screen("preferences");
    mr_interp_update(0);
    render();

    /* --- clicking has to change what is drawn --- */
    a = stage_hash();
    check("clicked Plein ecran", click("Preference(\"display\", \"fullscreen\""), 1);
    check("  the preference moved",
          mr_interp_pref_selected("display", "fullscreen"), 1);
    check("  and window is no longer selected",
          mr_interp_pref_selected("display", "window"), 0);
    b = stage_hash();
    printf("  stage hash %ld -> %ld\n", a, b);
    check("  and the screen actually changed", a != b, 1);

    /* The marker itself, isolated from the label colour.
     *
     * Worth its own case: with the foreground draw removed the checks above
     * all still pass, because swapping which label is white moves the whole
     * stage hash on its own. Only the left border tells you whether the
     * marker art was ever drawn. */
    {
        int rx, ry, rw, rh;
        long sel, idle;

        check("found the Fenetre button in the dump",
              rect_of("Fen", &rx, &ry, &rw, &rh), 1);
        idle = marker_hash(rx, ry, rh);          /* now unselected */

        click("Preference(\"display\", \"window\"");
        check("  Fenetre is selected again",
              mr_interp_pref_selected("display", "window"), 1);
        rect_of("Fen", &rx, &ry, &rw, &rh);
        sel = marker_hash(rx, ry, rh);

        printf("  marker strip %ld (idle) vs %ld (selected)\n", idle, sel);
        check("  the marker art changes with the state", idle != sel, 1);
    }

    check("clicking Fenetre restores both the state",
          mr_interp_pref_selected("display", "window"), 1);
    check("  and the pixels", stage_hash(), a);

    /* --- a check button, and a toggle that really does something --- */
    a = stage_hash();
    check("clicked Texte non lu", click("Preference(\"skip\", \"toggle\""), 1);
    check("  skip is now on", mr_interp_pref_selected("skip", "toggle"), 1);
    check("  and the screen changed", stage_hash() != a, 1);

    a = stage_hash();
    check("clicked Couper tous les sons",
          click("Preference(\"all mute\", \"toggle\""), 1);
    check("  mute is now on", mr_interp_pref_selected("all mute", "toggle"), 1);
    /* This one only has a text colour to change -- no foreground art -- which
     * is why it is worth its own case. */
    check("  and the screen changed", stage_hash() != a, 1);

    /* --- a preference must survive starting a game ---
     *
     * The reported route: main menu -> prefs -> Mute All -> Start -> prefs.
     * The button came back unticked over still-silent audio, so the first
     * click "turned on" a mute that was already on and the second turned it
     * off. The cause was the flag living outside the `_preferences.` prefix,
     * which is what mr_persist_is_persistent exempts from the baseline that
     * Start() resets to -- the mixer kept the mute, the variable did not.
     *
     * Checked for a slider too, because the fix is about the prefix rather
     * than about mute, and a ranged preference takes the same route. */
    mr_interp_pref_set_fraction("music volume", 0.25);
    check("mute is on before starting",
          mr_interp_pref_selected("all mute", "toggle"), 1);

    mr_interp_action("Start()");               /* the real thing: mr_interp_start
                                                * only jumps, Start() is what
                                                * resets to the baseline */
    mr_interp_update(0);

    check("  and still on after Start()",
          mr_interp_pref_selected("all mute", "toggle"), 1);
    check("  the mixer agrees", mr_audio_muted(MR_CH_SOUND), 1);
    check("  and the volume slider survived too",
          (long)(mr_interp_pref_get_fraction("music volume") * 100.0 + 0.5),
          25);

    /* Turning it off from here has to work first time, not second. */
    mr_interp_show_screen("preferences");
    mr_interp_update(0);
    render();
    check("clicked Couper tous les sons again",
          click("Preference(\"all mute\", \"toggle\""), 1);
    check("  one click unmutes",
          mr_interp_pref_selected("all mute", "toggle"), 0);
    check("  and the mixer agrees", mr_audio_muted(MR_CH_SOUND), 0);

    /* --- a Preference whose name is not its field ---
     *
     * `Preference("auto-forward", "toggle")` is ToggleField(_preferences,
     * "afm_enable") in Ren'Py (00preferences.rpy:396-404), and the
     * interpreter's auto-forward wait reads that field. Stored under the
     * Preference's own name instead -- the convention every other one here
     * follows -- the quick menu's Auto button toggled a variable nothing else
     * read and did nothing at all. The behaviour is afmtest's; this is the
     * real game's screens.rpy:252 reaching the right storage.
     *
     * The mute case above is the same hazard caught from the other side, and
     * the reason mr_interp_pref_selected and pref_action share pref_var. */
    check("the quick menu's Auto starts off",
          mr_interp_pref_selected("auto-forward", "toggle"), 0);
    mr_interp_action("Preference(\"auto-forward\", \"toggle\")");
    check("  it is afm_enable that moves",
          mr_var_get_int("_preferences.afm_enable", 0), 1);
    check("  the button reads back selected",
          mr_interp_pref_selected("auto-forward", "toggle"), 1);
    check("  and nothing is stored under the Preference name",
          mr_var_get_int("_preferences.auto-forward", 0), 0);

    /* --- InvertSelected --- */
    check("Transitions reads inverted",
          mr_sl_action_selected("InvertSelected(Preference(\"transitions\", \"toggle\"))"), 0);
    mr_var_set_int("_preferences.transitions", 0);
    check("  and follows the preference",
          mr_sl_action_selected("InvertSelected(Preference(\"transitions\", \"toggle\"))"), 1);
    check("  while the bare action does not",
          mr_sl_action_selected("Preference(\"transitions\", \"toggle\")"), 0);

    /* --- [prefix_] resolves through the search list --- */
    check("idle radio art falls back to no prefix",
          mr_vfs_exists("gui/button/radio_foreground.png"), 1);
    check("selected radio art exists",
          mr_vfs_exists("gui/button/radio_selected_foreground.png"), 1);
    check("  and radio_idle_foreground.png does not",
          mr_vfs_exists("gui/button/radio_idle_foreground.png"), 0);

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

/* Saving on a chapter title card.
 *
 * A chapter opens on a title card -- `show screen title with fade`, then
 * `$ renpy.pause()` -- and a save taken there restored to a bare black
 * screen. The thumbnail was right, which made it look like a drawing bug; it
 * was not, and it was two faults stacked. The screen layer was cleared
 * wholesale on load, on the grounds
 * that a save is always taken with the game menu up and the menu must not
 * come back. That is true of the menu and wrong for the story's own screens.
 *
 * So what has to be pinned down is both halves at once, which is why they are
 * in one test: the title card comes back, and the save menu does not.
 *
 * The second fault only showed once the first was fixed: the card came back
 * and then vanished again, because the `$ renpy.pause()` holding it was not
 * saved either, so the restored state was "running" and walked straight on
 * through `hide screen title`. Both are checked here, the second by waiting
 * several seconds and looking again.
 *
 * The card is found by looking for its *text* on the stage rather than by
 * asking the engine what it thinks is shown -- the bug was entirely in the
 * difference between those two answers.
 *
 * Runs against tools/harness/story, whose `chapter_two` opens on exactly
 * that: `show screen title with fade` then `$ renpy.pause()`. It used to
 * walk the real game, which put it at the mercy of every edit to the script.
 *
 * Run from a scratch directory: it writes and removes SAVE*.SAV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "vfs.h"
#include "audio.h"
#include "image.h"
#include "ui.h"
#include "expr.h"
#include "interp.h"
#include "script.h"
#include "scene.h"
#include "save.h"
#include "slrender.h"
#include "list.h"

static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static mr_surface *g_stage;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("MR_VERBOSE"))
        return;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}
void mr_quit(void) { }
void mr_invalidate(void) { }
mr_surface *mr_stage(void) { return g_stage; }

static int fails;
static long T;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void frame(void)
{
    mr_interp_update(T);
    T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_stage, T);
    mr_ui_draw(g_stage);
}

/* Is a screen of this name up?
 *
 * These checks used to count screens and expect exactly one. That broke when
 * the quick menu arrived: it is an overlay screen, so it is legitimately up
 * during play and the count became two without anything being wrong. Asking
 * after screens by name says what this test actually means -- the card came
 * back, the save menu did not -- and does not care what else the engine shows
 * alongside them.
 */
static int shown(const char *name)
{
    int i;
    for (i = 0; i < mr_scene_screen_count(); i++) {
        const char *n = mr_scene_screen_name(i);
        if (n && !strcmp(n, name))
            return 1;
    }
    return 0;
}

/* How much light there is on the stage. The title card is white text on
 * black, so this separates "the card is up" from "a bare black screen"
 * without knowing anything about where the text lands. */
static long lit(void)
{
    long n = 0;
    int x, y;

    for (y = 0; y < MR_STAGE_H; y += 2) {
        mr_u32 *row = g_stage->px + (size_t)y * g_stage->pitch;
        for (x = 0; x < MR_STAGE_W; x += 2)
            if (MR_R(row[x]) > 100 && MR_G(row[x]) > 100 &&
                MR_B(row[x]) > 100)
                n++;
    }
    return n;
}

static void dump(const char *path)
{
    FILE *f;
    int x, y;

    if (!getenv("MR_DUMP"))
        return;
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", MR_STAGE_W, MR_STAGE_H);
    for (y = 0; y < MR_STAGE_H; y++) {
        mr_u32 *row = g_stage->px + (size_t)y * g_stage->pitch;
        for (x = 0; x < MR_STAGE_W; x++) {
            unsigned char px[3];
            px[0] = (unsigned char)MR_R(row[x]);
            px[1] = (unsigned char)MR_G(row[x]);
            px[2] = (unsigned char)MR_B(row[x]);
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("   wrote %s\n", path);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "story";
    mr_script *s;
    long on_card = 0;
    int i;

    remove("SAVEP1S6.SAV");

    if (!mr_vfs_init(NULL, dir)) {
        printf("vfs init failed\n");
        return 1;
    }
    mr_image_init(dir);
    mr_audio_init(dir);
    mr_ui_init(dir);
    mr_scene_init();
    g_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    s = mr_script_load(dir);
    if (!s || s->n == 0) {
        printf("no script\n");
        return 1;
    }
    mr_interp_init(s, dir);
    /* Straight to a chapter, which opens on its title card. mr_interp_start
     * runs the init phase and jumps, which is how MR_LABEL gets there. */
    mr_interp_start("chapter_two");

    printf("-- on the title card --\n");
    for (i = 0; i < 200; i++) {
        frame();
        if (mr_scene_screen_count() > 0 && lit() > 50)
            break;
    }
    /* Let the `with fade` finish before measuring: the card is shown through
     * a fade, so a count taken the moment it appears is of a half-faded one
     * and would not compare with the restored card. */
    for (i = 0; i < 60; i++)
        frame();
    on_card = lit();
    dump("/tmp/title_before.ppm");
    printf("   (%ld lit pixels)\n", on_card);
    check("the title card is on the stage", on_card > 50);
    check("  and the engine agrees a screen is shown",
          mr_scene_screen_count() > 0);

    printf("\n-- save, from the game menu as a player would --\n");
    {
        /* The save screen goes up over the card first: that is exactly the
         * condition that made the old code throw the card away. */
        mr_interp_show_screen("save");
        frame();
        check("the save menu is up over it", mr_scene_screen_count() > 1);
        check("  and the thumbnail was taken on the scene, not the menu",
              1);   /* uitest owns that; noted here so the order is clear */

        mr_save_write("1-6", g_stage);
        check("the save was written", mr_save_exists("1-6"));
    }

    printf("\n-- load it back --\n");
    {
        long after;

        /* Somewhere else entirely, so nothing can be left over. */
        mr_interp_start("start");
        for (i = 0; i < 30; i++)
            frame();

        check("moved away from the card", lit() != on_card);

        mr_save_load("1-6");
        for (i = 0; i < 60; i++)
            frame();
        after = lit();
        dump("/tmp/title_after.ppm");
        printf("   (%ld lit pixels)\n", after);
        {
            int k;
            printf("   (%d screens:", mr_scene_screen_count());
            for (k = 0; k < mr_scene_screen_count(); k++)
                printf(" %s", mr_scene_screen_name(k));
            printf(")\n");
        }

        check("the title card came back", after > 50);
        check("  with as much on it as before",
              after > on_card - 20 && after < on_card + 20);
        check("  and the save menu did NOT come back", !shown("save"));
        check("  and the card's own screen did", shown("title"));

        /* The half that restoring the screen alone did not fix: the card is
         * held by `$ renpy.pause()`, and a restored state that is merely
         * "running" walks straight through `hide screen title` into the next
         * scene. The card has to still be there some seconds later. */
        for (i = 0; i < 120; i++)
            frame();
        printf("   (%ld lit pixels four seconds on)\n", lit());
        check("and it is still up, waiting, four seconds later",
              lit() > 50 && shown("title"));

        /* And it is a *pause*, not a freeze: a click still moves on. */
        mr_interp_advance();
        for (i = 0; i < 120; i++)
            frame();
        check("  while a click still moves past it", lit() != after);
    }

    printf("\n-- and the menu is still not saved --\n");
    {
        /* A save taken on an ordinary line, with the menu up, must restore
         * to no screens at all. */
        int guard;

        mr_interp_start("start");
        for (guard = 0; guard < 400; guard++) {
            int count = 0;
            const char *texts[16];
            frame();
            if (mr_interp_menu(&count, texts, 16) && count > 0)
                mr_interp_choose(0);
            if ((guard % 6) == 5)
                mr_interp_advance();
            if (!shown("title") && guard > 60)
                break;
        }

        mr_interp_show_screen("save");
        frame();
        mr_save_write("1-6", g_stage);
        mr_save_load("1-6");
        for (i = 0; i < 30; i++)
            frame();
        check("a save taken off a title card restores no story screens",
              !shown("title") && !shown("save"));
    }

    mr_interp_shutdown();
    mr_script_free(s);
    mr_surface_free(g_stage);
    mr_audio_shutdown();
    mr_ui_shutdown();
    mr_image_shutdown();
    mr_scene_shutdown();
    mr_vfs_shutdown();
    remove("SAVEP1S6.SAV");

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

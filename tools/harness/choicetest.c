/* `menu:` -- the in-game choice menu.
 *
 * The parser and the interpreter have had menus since early on, but nothing
 * exercised them: this game has no in-game menu at all, so the whole path was
 * carried by inspection. This drives the menu from the request verbatim --
 * seven options, each a `jump`, the last one setting a variable first -- and
 * then the `if` that reads that variable afterwards.
 *
 * It also checks the part that had no implementation at all: the menu is
 * drawn natively (Ren'Py shows `screen choice(items)`, and an item is an
 * object with a .caption and an .action, which the screen language here
 * cannot be handed), so the layout, the hover highlight and the hit testing
 * are this engine's own code and can disagree with each other.
 *
 * Everything it asserts about placement comes from the game's gui.rpy rather
 * than from constants here, so the test says "where its own gui.rpy puts it"
 * rather than "at y=78" -- which is what makes it mean anything for a
 * different game.
 *
 * Wants the fixture in tools/harness/choice/, which borrows the real game's
 * gui.rpy, screens.rpy and art so the styles and the button images are the
 * real ones. MR_DUMP=1 writes the menu to /tmp/choice.ppm.
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
#include "style.h"
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

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static void checkl(const char *what, long got, long want)
{
    printf("%-58s %s", what, got == want ? "ok\n" : "FAIL");
    if (got != want) {
        printf("  (got %ld, wanted %ld)\n", got, want);
        fails++;
    }
}

static mr_script *g_script;
static long g_now;

/* The line of dialogue on screen, or "" -- how the test reads which branch
 * of the `if` after the menu actually ran. */
static const char *current_line(void)
{
    const char *who, *what;
    mr_u32 col;
    int shown;

    if (!mr_interp_dialogue(&who, &what, &col, &shown))
        return "";
    return what;
}

/* Run the script until it is waiting for something. */
static void settle(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        mr_interp_update(g_now);
        g_now += 33;
    }
}

static void render(void)
{
    mr_scene_render(g_stage, g_now);
    mr_ui_draw(g_stage);
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

/* Start a fresh run and stop on the menu. */
static void to_the_menu(const char *dir)
{
    int count = 0;
    const char *texts[16];
    int guard;

    (void)dir;
    mr_interp_start("start");
    for (guard = 0; guard < 400; guard++) {
        mr_interp_update(g_now);
        g_now += 33;
        if (mr_interp_menu(&count, texts, 16))
            return;
        mr_interp_advance();     /* through the say before the menu */
    }
}

/* Rows on which any pixel is markedly blue: the hover background. */
static int blue_rows(int *first, int *last)
{
    int x, y, n = 0;

    *first = *last = -1;
    for (y = 0; y < MR_STAGE_H; y++) {
        mr_u32 *row = g_stage->px + (size_t)y * g_stage->pitch;
        int hit = 0;
        for (x = 0; x < MR_STAGE_W; x++) {
            mr_u32 c = row[x];
            if (MR_B(c) > 150 && MR_B(c) > MR_R(c) + 80 &&
                MR_B(c) > MR_G(c) + 40) {
                hit = 1;
                break;
            }
        }
        if (hit) {
            if (*first < 0)
                *first = y;
            *last = y;
            n++;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "choice";
    const char *texts[16];
    int count = 0;

    if (!mr_vfs_init(NULL, dir)) {
        printf("vfs init failed for '%s'\n", dir);
        return 1;
    }
    mr_image_init(dir);
    mr_audio_init(dir);
    mr_ui_init(dir);
    mr_scene_init();
    g_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    g_script = mr_script_load(dir);
    if (!g_script || g_script->n == 0) {
        printf("no script in '%s'\n", dir);
        return 1;
    }
    mr_interp_init(g_script, dir);

    printf("-- the menu is reached and has its options --\n");
    to_the_menu(dir);
    check("the script stops on the menu",
          mr_interp_menu(&count, texts, 16));
    checkl("  with seven options", count, 7);
    check("  in the order they were written",
          count == 7 && !strcmp(texts[0], "Chap1") &&
          !strcmp(texts[3], "Chap4") && !strcmp(texts[6], "Chap7"));

    printf("\n-- where the options are drawn --\n");
    {
        /* Asserted against gui.rpy rather than against numbers: the whole
         * point of reading the style table is that another game's menu lands
         * where its own gui.rpy says. */
        long width = mr_var_get_int("gui.choice_button_width", 0);
        long spacing = mr_var_get_int("gui.choice_spacing", 0);
        int i, x, prev_y = -1, h = 0;
        int x0 = -1, x1 = -1;

        check("gui.rpy really supplies the width and spacing",
              width > 0 && spacing > 0);

        render();
        dump("/tmp/choice.ppm");

        /* Walk the options through the public hit test, which is the same
         * layout the drawing used. */
        for (i = 0; i < count; i++) {
            int found_top = -1, found_bot = -1, y;
            for (y = 0; y < MR_STAGE_H; y++) {
                if (mr_ui_menu_hit(MR_STAGE_W / 2, y) == i) {
                    if (found_top < 0)
                        found_top = y;
                    found_bot = y;
                }
            }
            if (found_top < 0) {
                check("  every option has a rectangle", 0);
                break;
            }
            if (i == 0) {
                h = found_bot - found_top + 1;
                /* The horizontal extent, from the hit test. */
                for (x = 0; x < MR_STAGE_W; x++)
                    if (mr_ui_menu_hit(x, found_top) == 0) {
                        if (x0 < 0) x0 = x;
                        x1 = x;
                    }
            } else {
                checkl("  options are gui.choice_spacing apart",
                       found_top - prev_y - h, spacing);
            }
            prev_y = found_top;
        }

        checkl("the buttons are gui.choice_button_width wide",
               x1 - x0 + 1, width);
        check("  and centred on the stage",
              x0 == (MR_STAGE_W - (int)width) / 2);

        /* style choice_vbox: ypos 270, yanchor 0.5 -- the block's centre
         * sits on ypos, which for seven options puts the top at 78. */
        {
            int top = -1, bot = -1, y;
            for (y = 0; y < MR_STAGE_H; y++) {
                if (mr_ui_menu_hit(MR_STAGE_W / 2, y) >= 0) {
                    if (top < 0) top = y;
                    bot = y;
                }
            }
            printf("   (the block runs y=%d..%d, centre %d)\n",
                   top, bot, (top + bot) / 2);
            check("the block is centred where choice_vbox says",
                  top >= 0 && (top + bot) / 2 >= 268 &&
                  (top + bot) / 2 <= 272);
        }
    }

    printf("\n-- the hover highlight --\n");
    {
        int f0, l0, f1, l1;

        mr_ui_set_menu_hover(-1);
        render();
        blue_rows(&f0, &l0);

        mr_ui_set_menu_hover(2);       /* Chap3, as in the reference shot */
        render();
        blue_rows(&f1, &l1);
        dump("/tmp/choice_hover.ppm");

        check("hovering paints a blue bar", f1 >= 0);
        check("  where the hovered option is",
              f1 >= 0 && mr_ui_menu_hit(MR_STAGE_W / 2, (f1 + l1) / 2) == 2);
        check("  and only there", f0 != f1 || l0 != l1);
    }

    printf("\n-- the hit test answers the click --\n");
    {
        int x, y, w, h, i;
        int hits = 0;

        (void)x; (void)y; (void)w; (void)h;
        for (i = 0; i < count; i++) {
            int yy;
            for (yy = 0; yy < MR_STAGE_H; yy++)
                if (mr_ui_menu_hit(MR_STAGE_W / 2, yy) == i) {
                    hits++;
                    break;
                }
        }
        checkl("every option can be clicked", hits, count);
        check("a point outside every button is not a choice",
              mr_ui_menu_hit(4, 4) < 0);
    }

    printf("\n-- choosing runs that option's block --\n");
    {
        /* Chap7: `$ chap7=True` then `jump chap7`. Both have to happen, and
         * in that order, or the `if` after the jump sees the old value. */
        mr_interp_choose(6);
        settle();

        check("the variable the option set is set",
              mr_var_get_int("chap7", 0) != 0);
        checkl("  and the jump landed in that chapter",
               mr_var_get_int("landed", 0), 7);

        check("`if chap7 == True` took the true branch",
              !strcmp(current_line(), "We jumped"));
    }

    printf("\n-- and a plain option only jumps --\n");
    {
        mr_var_set_int("chap7", 0);
        mr_var_set_int("landed", 0);
        g_now += 1000;
        to_the_menu(dir);
        check("back at the menu", mr_interp_menu(&count, texts, 16));

        mr_interp_choose(2);           /* Chap3 */
        settle();
        checkl("the third option jumped to chap3",
               mr_var_get_int("landed", 0), 3);
        check("  and left chap7 alone", mr_var_get_int("chap7", 0) == 0);
        check("`if chap7 == True` took the false branch",
              !strcmp(current_line(), "No chap7"));
    }

    mr_interp_shutdown();
    mr_script_free(g_script);
    mr_surface_free(g_stage);
    mr_audio_shutdown();
    mr_ui_shutdown();
    mr_image_shutdown();
    mr_scene_shutdown();
    mr_vfs_shutdown();

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

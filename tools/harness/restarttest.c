/* Starting a new game must not inherit the last one.
 *
 * `play -> right click -> main menu -> play` drew the line the player had been
 * on over the new game's opening fade: Start() cleared the scene but not the
 * dialogue, and the say window is drawn from g_what regardless of where the
 * story is. The rollback log and the story's variables survived too, so a
 * rollback at the start of a new game walked back into the abandoned one.
 *
 * Ren'Py discards the whole context at both points, which is what
 * clear_playthrough reproduces.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
#include "history.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "slrender.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}

/* The game opens on a chapter-select menu, added to script.rpy after this
 * harness was written. Anything that wants dialogue has to answer it first;
 * which option hardly matters, so take the first. */
static void answer_menu(void)
{
    int count = 0;
    const char *texts[16];

    if (mr_interp_menu(&count, texts, 16) && count > 0)
        mr_interp_choose(0);
}

static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static long T;
static int  fails;

static void draw(void)
{
    mr_interp_update(T);
    T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

static void check(const char *label, int ok)
{
    printf("%-50s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static const char *line_now(void)
{
    static char buf[200];
    const char *who, *what;
    mr_u32 c;
    int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(buf, "%.150s", what);
    else
        strcpy(buf, "(none)");
    return buf;
}

/* How much of the stage is not black. A fade-out from an empty scene should
 * reach zero; anything drawn over it shows up here. */
static long lit(void)
{
    long n = 0;
    int x, y;
    for (y = 0; y < MR_STAGE_H; y += 9)
        for (x = 0; x < MR_STAGE_W; x += 9)
            if ((g_st->px[y * g_st->pitch + x] & 0xFFFFFFUL) > 0x101010UL)
                n++;
    return n;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    char first[200];

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: restarttest <game-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_start("start");
    mr_interp_main_menu();
    draw();

    /* --- play a few lines --- */
    mr_interp_action("Start()");
    for (i = 0; i < 90; i++) {
        draw();
        answer_menu();
        if (strcmp(line_now(), "(none)"))
            break;
    }
    for (i = 0; i < 6; i++) {
        answer_menu();
        mr_interp_advance();
        draw();
        answer_menu();
        mr_interp_advance();
        draw();
    }
    strcpy(first, line_now());
    printf("played as far as: %s\n", first);
    check("on a line", strcmp(first, "(none)") != 0);
    check("with a rollback log", mr_rollback_depth() > 0);
    check("and some history", mr_history_count() > 1);

    mr_expr_exec("probe_flag = True");
    check("a story flag is set", mr_var_get_int("probe_flag", 0) == 1);

    /* --- right click, then Menu principal --- */
    mr_interp_game_menu_toggle();
    draw();
    mr_interp_action("MainMenu()");
    draw();
    check("no line while the menu is up", !strcmp(line_now(), "(none)"));

    /* --- play again --- */
    mr_interp_action("Start()");
    {
        long worst = 0;
        int leaked = 0;
        for (i = 0; i < 16; i++) {      /* the fade-out half of `with fade` */
            draw();
            if (strcmp(line_now(), "(none)"))
                leaked = 1;
            if (lit() > worst)
                worst = lit();
        }
        printf("brightest frame during the opening fade-out: %ld\n", worst);
        check("the old line is not drawn over the new fade", !leaked);
        check("and the stage really is black", worst == 0);
    }

    check("the rollback log went with it", mr_rollback_depth() == 0);
    check("so did the history", mr_history_count() == 0);
    check("and the story flag is back to unset",
          mr_var_get_int("probe_flag", 0) == 0);

    /* The new game still runs. Clicking through, because the chapter the
     * menu leads to opens on a title card held by `$ renpy.pause()`. */
    for (i = 0; i < 300; i++) {
        draw();
        answer_menu();
        if ((i % 6) == 5)
            mr_interp_advance();
        if (strcmp(line_now(), "(none)"))
            break;
    }
    printf("new game opens on : %s\n", line_now());
    check("the new game reaches its first line",
          strcmp(line_now(), "(none)") != 0);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

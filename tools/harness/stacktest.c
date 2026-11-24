/* How much stack does drawing a screen take?
 *
 * This matters more here than on a normal target. mrpy.exe is linked with
 * wlink's default 1MB reserve but only **64KB committed** (`commit stack=` is
 * accepted and silently ignored for `format windows nt` -- see mrpy.lnk), so
 * everything past 64KB depends on guard-page growth, which Win32s emulates
 * least well. A render path that wants more than that is a page fault with a
 * null instruction pointer, not a tidy error.
 *
 * Two things on the stack scale with MR_VSTR_MAX, which is 1024:
 *
 *   - `bind_use_args` (slrender.c) holds `mr_value pos[8]` and
 *     `mr_value kw[8]`, so ~16KB per nested `use`. The game menu is
 *     `save` -> `use file_slots` -> `use game_menu` -> `use navigation`.
 *   - `eval_atom` (expr.c) holds `mr_value argv[8]`, ~8KB per nested call.
 *
 * Measured rather than reasoned about: the body runs on a thread whose stack
 * is a region filled with a known pattern, and the high-water mark is however
 * much of that pattern is gone afterwards. The number is what decides whether
 * this fits, so it is printed whether the test passes or fails.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "mr.h"
#include "save.h"
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
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static int fails;
static const char *g_game;
static char *g_pak;

/* What the target actually commits. Not an assertion threshold: this is a
 * host build at -O0 with a different compiler, so the absolute figure here
 * is not the target's. It is printed for scale. */
#define TARGET_COMMIT  (64 * 1024)

/* A regression ceiling, not a budget. The measured peak was 127KB when this
 * was written (and 97KB before MR_VSTR_MAX went to 1024, so the path has
 * always been over the commitment and has always relied on guard-page
 * growth). The point of the number is to catch a change that *doubles* the
 * stack -- another array of mr_value on the stack, or a new level of
 * nesting -- rather than to certify that it fits. */
#define CEILING        (160 * 1024)

/* The thread stack. Generous, so that measuring never itself overflows; the
 * pattern is what gives the figure, not the size. */
#define STACK_BYTES    (4 * 1024 * 1024)
#define PATTERN        0xA5

static void check(const char *label, int ok)
{
    printf("%-54s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static long T;

static void draw(void)
{
    mr_interp_update(T); T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

/* The work whose stack we want: the game menu's save and load pages, which
 * are the most deeply nested screens in the game -- a screen with parameters
 * that `use`s two others. */
static void *body(void *arg)
{
    (void)arg;

    mr_interp_start("start");
    mr_interp_main_menu();
    draw();

    mr_interp_action("Start()");
    draw();

    /* In-game, which is where the navigation chain is deepest. */
    mr_interp_show_screen("save");
    draw();
    draw();
    mr_interp_show_screen("load");
    draw();
    draw();

    mr_interp_show_screen("preferences");
    draw();
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t th;
    char *stack;
    size_t i, used;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: stacktest <pak> <game-dir>\n");
        return 2;
    }
    g_pak = argv[1];
    g_game = argv[2];

    mr_vfs_init(g_pak, g_game);
    mr_image_init(g_game);
    mr_scene_init();
    mr_interp_init(mr_script_load(g_game), g_game);
    mr_ui_init(g_game);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    stack = (char *)malloc(STACK_BYTES);
    if (!stack) {
        printf("no memory for a test stack\n");
        return 1;
    }
    memset(stack, PATTERN, STACK_BYTES);

    pthread_attr_init(&attr);
    if (pthread_attr_setstack(&attr, stack, STACK_BYTES) != 0) {
        printf("cannot set a thread stack\n");
        return 1;
    }
    if (pthread_create(&th, &attr, body, NULL) != 0) {
        printf("cannot create the thread\n");
        return 1;
    }
    pthread_join(th, NULL);

    /* The stack grows down, so the untouched pattern is at the low end. */
    for (i = 0; i < STACK_BYTES; i++)
        if ((unsigned char)stack[i] != PATTERN)
            break;
    used = STACK_BYTES - i;

    printf("\n   peak stack drawing the menu screens: %lu bytes (%.1f KB)\n",
           (unsigned long)used, used / 1024.0);
    printf("   the target commits %d KB\n", TARGET_COMMIT / 1024);

    if (used > (size_t)TARGET_COMMIT)
        printf("   (over the commitment, so it relies on guard-page growth --\n"
               "    true of this path before any of this was changed too)\n");

    check("the render path has not grown another stack frame",
          used < (size_t)CEILING);

    free(stack);
    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

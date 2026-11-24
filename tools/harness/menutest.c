/* Opening and closing the game menu must leave the story exactly where it
 * was: closing is not an advance. */
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

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static char cur[220];
static void snap(void)
{
    const char *who, *what; mr_u32 c; int shown;
    if (mr_interp_dialogue(&who, &what, &c, &shown)) sprintf(cur, "%.46s", what);
    else strcpy(cur, "(no text)");
}
/* Counts, and main returns it. Six harnesses printed their verdict and
 * exited 0 regardless, which run.sh reads as a pass; see NOTES, "A test
 * that cannot go red". */
static int fails;

static void check(const char *l, long got, long want)
{
    printf("%-36s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

int main(int argc, char **argv)
{
    mr_script *s; int i; char before[220];

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]); mr_image_init(argv[1]); mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    for (i = 0; i < 40; i++) {
        mr_interp_update(T); T += 33; snap();
        if (strcmp(cur, "(no text)")) break;
        mr_interp_advance();
    }
    snap(); strcpy(before, cur);
    printf("on line          : %s\n", before);

    /* Right-click in, right-click out. */
    mr_interp_game_menu_toggle();
    mr_interp_update(T); T += 33; snap();
    check("menu hides the text", strcmp(cur, "(no text)") == 0, 1);

    mr_interp_game_menu_toggle();
    mr_interp_update(T); T += 33; snap();
    printf("after closing    : %s\n", cur);
    check("same line, did not advance", strcmp(cur, before) == 0, 1);

    /* And the Retour button, which is the same path. */
    mr_interp_show_screen("preferences");
    mr_interp_update(T); T += 33;
    mr_interp_action("Return()");
    mr_interp_update(T); T += 33; snap();
    printf("after Retour     : %s\n", cur);
    check("Retour did not advance either", strcmp(cur, before) == 0, 1);

    /* A real click still advances. */
    mr_interp_advance();
    mr_interp_update(T); snap();
    check("clicking still advances", strcmp(cur, before) != 0, 1);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

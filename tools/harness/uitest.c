/* Drives the save/load path the way the UI does: run the story a while, open
 * the game menu (which takes the thumbnail), click a slot to save, then load
 * it back and check we land where we were. */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
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

/* Counted, and returned by main. This harness printed six lines and
 * exited 0 regardless, so run.sh reported a pass whatever the thumbnail
 * held; see NOTES, "A test that cannot go red". */
static int fails;

static void check(const char *l, long got, long want)
{
    printf("%-44s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

/* The line on screen, or "" -- so the test can compare where it was with
 * where it ends up, which is the thing a load is for. */
static char g_line[200];

static void show_dialogue(const char *tag)
{
    const char *who = "", *what = "";
    mr_u32 c; int shown;

    g_line[0] = '\0';
    if (mr_interp_dialogue(&who, &what, &c, &shown) && what)
        sprintf(g_line, "%.150s", what);
    if (mr_interp_dialogue(&who, &what, &c, &shown))
        printf("%s: who='%s' what='%.44s...'\n", tag, who, what);
    else
        printf("%s: (no dialogue)\n", tag);
}

int main(int argc, char **argv)
{
    mr_script *s;
    char before[200];
    long t = 0;
    int i;

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    anygame_start_story(s);

    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_surface_fill(g_st, MR_RGB(10, 200, 30));   /* "the scene": green */

    /* Advance through a few lines of dialogue. */
    for (i = 0; i < 6; i++) {
        mr_interp_update(t); t += 33;
        mr_interp_advance();
    }
    mr_interp_update(t);
    show_dialogue("before save");
    strcpy(before, g_line);
    check("there is a line to save on", before[0] != '\0', 1);

    /* Open the game menu: this is when the thumbnail is taken. */
    mr_interp_show_screen("save");
    /* The stage now shows the menu, as it would in the real loop. */
    mr_surface_fill(g_st, MR_RGB(200, 0, 0));     /* "the menu": red */

    check("the save was written", mr_save_write("2-4", mr_stage()), 1);

    /* The thumbnail is the *scene*, not the menu that was on the stage
     * when the slot was clicked. That is the subject of this harness --
     * the shot is taken when the game menu opens, before the menu is
     * drawn -- and it was printed and never checked: green means the
     * scene, red means the menu got into the picture. */
    {
        mr_surface *th = mr_save_thumbnail("2-4");
        mr_u32 p = th ? th->px[(size_t)(th->h/2) * th->pitch + th->w/2] : 0;
        printf("thumbnail centre = %02x%02x%02x (green = scene, red = menu)\n",
               MR_R(p), MR_G(p), MR_B(p));
        check("there is a thumbnail", th != NULL, 1);
        check("  it is the scene, not the menu",
              th && MR_G(p) > MR_R(p) && MR_G(p) > 100, 1);
    }

    /* Wander off somewhere else entirely, then load.
     *
     * A derived label: this said `mr_interp_start("credits")`, and in a
     * script without one nothing moved -- so "after load" compared a
     * line with itself and the load proved nothing. */
    {
        char elsewhere[MR_MAX_NAME];

        if (anygame_other_label(s, anygame_label,
                                elsewhere, sizeof(elsewhere))) {
            printf("wandering off to %s\n", elsewhere);
            mr_interp_start(elsewhere);
        }
        for (i = 0; i < 8; i++) {
            mr_interp_update(t); t += 33;
            mr_interp_advance();
        }
        mr_interp_update(t);
        show_dialogue("after wandering");
        check("the story moved away from the saved line",
              strcmp(g_line, before) != 0, 1);
    }

    check("the save loads", mr_save_load("2-4"), 1);
    mr_interp_update(t);
    show_dialogue("after load");
    check("  and lands back on the line it was saved at",
          strcmp(g_line, before) == 0, 1);

    mr_save_delete("2-4");

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

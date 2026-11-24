/* The cutscene stand-in: where the held frame goes, and what is left behind.
 *
 * A `renpy.movie_cutscene()` whose video the engine cannot open falls back to
 * holding the video's first frame on the stage while its soundtrack plays.
 * cuttest covers the build convention that produces those two assets and the
 * wait that times them, against a real game's pak. This covers the two things
 * cuttest structurally cannot see, both of which were wrong:
 *
 *   - **the frame was not scaled.** It went up with `mr_scene_scene(base)`,
 *     which puts it on the 1280x720 stage at its own size, bottom-standing
 *     with x centred -- mr_place_resolve's answer for an image shown with no
 *     placement (scene.c:960). A poster is the size of its video. Miwa's
 *     ending.avi is 640x320, so the stand-in came up a quarter of the stage's
 *     area, low and centred. The movie path fits the picture into the
 *     presenter's buffer and centres it (present.c:667-669), and the stand-in
 *     is meant to be that picture held still.
 *
 *   - **the dialogue window was drawn over it.** Ren'Py draws a fullscreen
 *     movie *instead of* the render tree (gl2draw.pyx:1005-1008), so nothing
 *     is above it. The movie path here gets that free -- main.c's frame()
 *     returns early and nothing composites -- and the stand-in, which does
 *     composite, kept the empty say window from the line before.
 *
 *   - **the scene was not put back.** The stand-in *is* a display-list entry,
 *     so raising it destroys what the story was looking at, and nothing
 *     restored it; the story resumed with the poster still on the stage until
 *     the script's next `scene` happened to replace it. A movie has no such
 *     problem -- it never touches the stage -- which is why the two paths
 *     needed different teardowns and only one of them had any.
 *
 * Why neither was visible before. cuttest runs against whatever game is
 * mounted, and the only cutscene either shipped game has is Miwa's
 * intro.webm, which is 1280x720: its poster is exactly stage-sized, so the
 * missing scale is the identity and the missing restore is hidden by the
 * `scene` statement on the line after. A fixture that cannot express the bug
 * is the usual way an assertion comes out vacuous, so the fixture here is
 * built the other way round: images/fixstandin.png is 640x320 *because* the
 * stage is not, and the label does not clear the scene afterwards.
 *
 * What it does not cover: the duration. The stand-in's length comes off its
 * soundtrack, and the host waveOut shim consumes every buffer the instant it
 * is queued, so a claim about how long it lasts would be clocked off a lie.
 * The fixture label therefore has no soundtrack at all and takes the
 * fixed-length branch. cuttest owns the duration, against a real track.
 *
 * Takes the fixture pak first and the fixture directory second. Nothing here
 * is about the archive, but the fixture declares no `image` statements -- its
 * names come from Ren'Py's auto-registration over the images directory, and
 * mr_vfs_enum only enumerates an archive on this host. Mounted loose, the
 * poster does not resolve and every claim below goes red for a reason that
 * has nothing to do with the stand-in.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "interp.h"
#include "vfs.h"
#include "ui.h"
#include "audio.h"
#include "yuv.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
/* A pointer and not a function, which is how wavehost.c calls it -- the
 * harnesses that spell it as a function are the ones that never reach
 * mr_audio_poll. This one does: the cutscene stops and restarts the music. */
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static mr_u32 px(const mr_surface *s, int x, int y)
{
    return s->px[(size_t)y * s->pitch + x];
}

static void render(void)
{
    mr_image_begin_frame();
    mr_image_set_time(0);
    mr_scene_render(g_st, 0);
}

static long T = 1000;

static void frame(void)
{
    mr_audio_poll();
    mr_interp_update(T);
    T += 33;
}

/* The display list, read directly rather than inferred from pixels.
 *
 * Both are used below and they answer different questions: the pixels say
 * where the frame ended up, which is the bug a player saw, and the list says
 * what the engine believes is on screen, which is what the next statement and
 * the next save will act on. A test that only looked at pixels would pass on
 * an implementation that drew the right thing and left the wrong display
 * list behind. */
static mr_scenelist g_list;

static const char *bg_now(void)
{
    mr_scene_capture(&g_list);
    return g_list.bg[0] ? g_list.bg : "(none)";
}

static int shown_count(void)
{
    mr_scene_capture(&g_list);
    return g_list.n;
}

static const char *shown_name(int i)
{
    mr_scene_capture(&g_list);
    if (i < 0 || i >= g_list.n)
        return "(none)";
    return g_list.v[i].name;
}

/* Walk to the next line of dialogue, so a block starts somewhere definite. */
static void to_dialogue(void)
{
    const char *who, *what;
    mr_u32 c;
    int sh, i;

    for (i = 0; i < 60; i++) {
        frame();
        if (mr_interp_dialogue(&who, &what, &c, &sh))
            return;
        mr_interp_advance();
    }
}

int main(int argc, char **argv)
{
    mr_script *s;
    mr_surface *poster;
    int fw = 0, fh = 0, top, bot;

    setbuf(stdout, NULL);
    if (argc < 3) {
        fprintf(stderr, "usage: standintest <pak> <game-dir>\n");
        return 2;
    }

    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]);
    mr_scene_init();
    s = mr_script_load(argv[2]);
    if (!s) {
        fprintf(stderr, "standintest: no script in %s\n", argv[2]);
        return 2;
    }
    mr_interp_init(s, argv[2]);
    mr_ui_init(argv[2]);
    mr_audio_init(argv[2]);

    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    if (!g_st) {
        fprintf(stderr, "standintest: could not allocate the stage\n");
        return 2;
    }

    /* The init pass that registers the image names runs inside
     * mr_interp_start, not mr_interp_init. */
    if (mr_script_find_label(s, "fixture_standin") < 0) {
        fprintf(stderr, "standintest: the fixture has no fixture_standin "
                        "label; this is our fixture, not a game's\n");
        return 1;
    }
    mr_interp_start("fixture_standin");

    /* --- the fixture can express the bug ---------------------------------
     *
     * Asserted off the surface rather than assumed from the filename, and
     * asserted first, because every claim below is vacuous if the poster
     * turns out to be stage-sized. This is the check that would catch
     * somebody "fixing" the fixture by regenerating it at 1280x720. */
    printf("-- the fixture --\n");
    mr_image_begin_frame();
    poster = mr_image_get("fixstandin");
    check("the poster frame resolves", poster != NULL);
    if (!poster) {
        printf("\nFAILURES\n");
        return 1;
    }
    printf("   poster %dx%d, stage %dx%d\n",
           poster->w, poster->h, MR_STAGE_W, MR_STAGE_H);
    check("  and it is not stage-sized, so a missing scale would show",
          poster->w != MR_STAGE_W || poster->h != MR_STAGE_H);

    /* Where the movie would have put it. mr_yuv_fit is the function the
     * presenter uses, called here for the same reason interp.c calls it
     * rather than computing a scale of its own: if the two ever disagree
     * this test is what says so. */
    mr_yuv_fit(poster->w, poster->h, MR_STAGE_W, MR_STAGE_H, &fw, &fh);
    printf("   the movie's rectangle would be %dx%d\n", fw, fh);
    check("  and the fit is wider than the poster, so a scale is needed",
          fw > poster->w);

    top = (MR_STAGE_H - fh) / 2;
    bot = top + fh;

    /* --- before the cutscene --------------------------------------------- */
    printf("\n-- before --\n");
    to_dialogue();
    printf("   bg=%s, %d shown, window=%d\n",
           bg_now(), shown_count(), mr_interp_window_visible());
    check("the story's scene is up", !strcmp(bg_now(), "fix-room"));
    /* The precondition for the window claim below, and it is not a detail:
     * if the window were already down here, "the window is down during the
     * cutscene" would hold for a reason that has nothing to do with the
     * cutscene. The fixture has a say immediately before, which is what puts
     * it up. */
    check("  with the dialogue window up, as the say before it left it",
          mr_interp_window_visible());

    /* --- the stand-in ----------------------------------------------------- */
    printf("\n-- the stand-in --\n");
    /* Advance until the cutscene is up rather than once: a say has to be
     * dismissed and the `$` line after it executed, and how many frames that
     * takes is the typewriter's business, not this test's. Bounded, so a
     * cutscene that never starts fails here instead of hanging. */
    {
        int i;
        for (i = 0; i < 60 && !mr_interp_waiting(); i++) {
            mr_interp_advance();
            frame();
        }
        printf("   the cutscene came up after %d frames\n", i);
    }

    check("the cutscene is waiting", mr_interp_waiting());
    printf("   bg=%s, %d shown: %s\n",
           bg_now(), shown_count(), shown_name(0));
    check("the poster is on the display list",
          shown_count() == 1 && !strcmp(shown_name(0), "fixstandin"));
    check("  and the story's scene is not under it",
          strcmp(bg_now(), "fix-room") != 0);

    /* Ren'Py draws the movie *instead of* the render tree while a fullscreen
     * movie is up (gl2draw.pyx:1005-1008), so nothing is over it -- not the
     * scene, not the overlay, and not the dialogue window. The window is the
     * one this engine could still get wrong, because the stand-in composites
     * like an ordinary frame where the movie path never composites at all. */
    printf("   window=%d\n", mr_interp_window_visible());
    check("  and the dialogue window is down over the cutscene",
          !mr_interp_window_visible());

    render();
    {
        mr_u32 want = px(poster, poster->w / 2, poster->h / 2);
        mr_u32 mid  = px(g_st, MR_STAGE_W / 2, MR_STAGE_H / 2);
        mr_u32 left = px(g_st, 0, MR_STAGE_H / 2);
        mr_u32 right = px(g_st, MR_STAGE_W - 1, MR_STAGE_H / 2);
        mr_u32 above = px(g_st, MR_STAGE_W / 2, top / 2);
        mr_u32 below = px(g_st, MR_STAGE_W / 2, (bot + MR_STAGE_H) / 2);

        printf("   stage rows: bar %08lX, picture %08lX, bar %08lX\n",
               (unsigned long)above, (unsigned long)mid,
               (unsigned long)below);

        /* The centre is the poster. Read off the poster's own pixel rather
         * than written here as a constant, so regenerating the fixture in
         * another colour does not quietly turn this green. */
        check("the centre of the stage is the poster", mid == want);

        /* Against the poster's pixel and not against `mid`, which is the
         * difference between a claim and a tautology: with the old placement
         * the centre of the stage was black and so were both edges, so
         * `left == mid` held while the picture was somewhere else entirely.
         * Caught by running this test against the old code, which is the only
         * way that kind of agreement shows up. */
        check("  it reaches the left edge of the stage", left == want);
        check("  and the right edge", right == want);

        /* And the two that a stretch-to-fill would fail. The fit keeps the
         * video's aspect, so a 2:1 poster on a 16:9 stage has bars, exactly
         * as the presenter's letterbox gives it. */
        check("  with a black bar above", above == MR_RGB(0, 0, 0));
        check("  and below", below == MR_RGB(0, 0, 0));
    }

    /* --- after it ends ---------------------------------------------------- */
    printf("\n-- after --\n");
    {
        int i;
        for (i = 0; i < 400 && mr_interp_waiting(); i++)
            frame();
        printf("   it ended after %d frames\n", i);
        check("the cutscene ended", !mr_interp_waiting());
    }

    printf("   bg=%s, %d shown, window=%d\n",
           bg_now(), shown_count(), mr_interp_window_visible());
    check("the story's scene is back", !strcmp(bg_now(), "fix-room"));
    /* Suppressed for the cutscene and not destroyed by it: the say after the
     * cutscene has to have a window to appear in. A guard that cleared the
     * state instead of hiding it would pass the check above and fail here. */
    check("  and the dialogue window is back", mr_interp_window_visible());
    check("  and the poster is off the display list", shown_count() == 0);

    render();
    check("  so the stage is no longer the poster",
          px(g_st, MR_STAGE_W / 2, MR_STAGE_H / 2) !=
          px(poster, poster->w / 2, poster->h / 2));

    /* The line after the cutscene still runs, which is the cheap guard
     * against a teardown that restores the scene by breaking the
     * interpreter. */
    to_dialogue();
    {
        const char *who, *what;
        mr_u32 c;
        int sh;
        check("the story carried on past the cutscene",
              mr_interp_dialogue(&who, &what, &c, &sh));
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

/* The dialogue window: when it is up, and that an effect never moves it.
 *
 * The window is state of its own, not a consequence of there being a line to
 * read. `window show` puts it up, `window hide` takes it down, a line of
 * dialogue shows it for as long as the line is up whatever `window hide` left
 * behind, and *nothing else touches it*. In particular a dissolve, a fade or
 * a move leaves it exactly where it was: the effect belongs to the scene
 * underneath, and the window sits above it.
 *
 * Both halves were wrong at different times. First the box was drawn only
 * while a line was displayed, so it blinked out between lines and for the
 * whole of every transition. Then it was put into the transition snapshots,
 * which made it cross-fade in and out of effects instead of staying put.
 *
 * What is measured is the box on the stage: the textbox strip against the
 * same strip of the scene rendered without it, which is exactly the window
 * and nothing else. Through an effect that figure has to hold still.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
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

#define BOX_TOP (MR_STAGE_H - 185)

static void check(const char *label, int ok)
{
    printf("%-54s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* One frame, exactly as main.c draws it. */
static void frame(void)
{
    mr_interp_update(T);
    T += 33;
    mr_sl_focus_reset();
    mr_interp_render(g_st, T);
    mr_ui_draw(g_st);
}

/* How much of the textbox strip is the window rather than the scene.
 *
 * The reference is the scene composited on its own. During a transition that
 * is the blend -- which is the point: the window is not in the blend, so the
 * difference is still the window alone, and it stays constant while the scene
 * underneath cross-fades. */
static long window_strength(void)
{
    static mr_surface *bare;
    long total = 0;
    int x, y, n = 0;

    if (!bare)
        bare = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_scene_render(bare, T);

    for (y = BOX_TOP; y < MR_STAGE_H; y += 4) {
        for (x = 0; x < MR_STAGE_W; x += 4) {
            mr_u32 a = g_st->px[y * g_st->pitch + x];
            mr_u32 b = bare->px[y * bare->pitch + x];
            long d = (long)MR_R(a) - (long)MR_R(b);
            total += d < 0 ? -d : d;
            n++;
        }
    }
    return n ? total / n : 0;
}

/* Step until a transition is running, or give up. */
static int run_to_transition(int limit)
{
    int i;

    for (i = 0; i < limit; i++) {
        if (mr_scene_in_transition())
            return 1;
        if ((i % 6) == 5) {
            answer_menu();
            mr_interp_advance();
        }
        frame();
    }
    return 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: windowtest <game-dir>\n");
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
    mr_expr_exec("_preferences.text_cps = 30");

    /* --- the opening: nothing has shown the window yet --- */
    mr_interp_action("Start()");
    frame();
    printf("window over the opening fade: %ld\n", window_strength());
    check("no window before the first line", window_strength() == 0);

    /* --- the first line puts it up --- */
    for (i = 0; i < 200; i++) {
        const char *who, *what;
        mr_u32 c;
        int shown;
        frame();
        answer_menu();
        /* The chapter behind the menu opens on a title card waiting for a
         * click, so getting to a line of dialogue means clicking. */
        if ((i % 6) == 5)
            mr_interp_advance();
        if (mr_interp_dialogue(&who, &what, &c, &shown))
            break;
    }
    check("a line of dialogue puts the window up", window_strength() > 5);

    /* --- and it stays up between lines --- */
    {
        long worst = 9999;
        int  blinked = 0;

        for (i = 0; i < 120; i++) {
            long w;
            if ((i % 6) == 5) {
                answer_menu();
                mr_interp_advance();
            }
            frame();
            if (mr_scene_in_transition())
                continue;
            w = window_strength();
            if (w < worst)
                worst = w;
            if (w == 0)
                blinked = 1;
        }
        printf("weakest the window got between lines: %ld\n", worst);
        check("it does not blink out between lines", !blinked);
    }

    /* --- an effect must not move it ---
     *
     * Sampled every frame of the transition. `window_strength` says whether
     * the window is *there*; where it is has to be read off its rectangle.
     * The strength is a difference against a render without the window, and
     * the window art is semi-transparent, so the figure tracks whatever is
     * dissolving behind it -- it says nothing about the window moving. A
     * window caught inside the effect shows up as both: it fades (strength
     * drops to nothing at one end) and its rectangle is wrong. */
    {
        long lo = 99999, hi = -1;
        int  n = 0;
        int  rx = -1, ry = -1, rw = -1, rh = -1;
        int  moved = 0, got_rect = 0;

        check("found an effect to watch", run_to_transition(400));

        while (mr_scene_in_transition() && n < 40) {
            long w = window_strength();
            int x, y, bw, bh;

            if (w < lo) lo = w;
            if (w > hi) hi = w;
            if (mr_ui_dialogue_rect(&x, &y, &bw, &bh)) {
                if (got_rect && (x != rx || y != ry || bw != rw || bh != rh))
                    moved = 1;
                rx = x; ry = y; rw = bw; rh = bh;
                got_rect = 1;
            }
            n++;
            frame();
        }

        printf("window through the effect: %d frames, %ld..%ld,"
               " rect %d,%d %dx%d\n", n, lo, hi, rx, ry, rw, rh);
        check("the effect lasted several frames", n >= 4);
        check("the window was up throughout", lo > 5);
        check("  and its rectangle never moved", got_rect && !moved);
    }

    /* --- the same for a move, which composites differently ---
     *
     * dissolve and fade blend two whole-screen snapshots; the move family
     * re-renders both display lists every frame with per-image offsets. The
     * window has to be equally untouched by either, and `moveinright` was
     * where the double fade showed up. */
    {
        long lo = 99999, hi = -1;
        int  n = 0, found = 0;

        for (i = 0; i < 4000 && !found; i++) {
            if ((i % 6) == 5) {
                answer_menu();
                mr_interp_advance();
            }
            frame();
            if (mr_scene_in_transition() && mr_scene_move_transition())
                found = 1;
        }

        check("found a move effect", found);
        {
            /* "It never moved" was read off the strength metric, which is a
             * proxy: a translucent window over a scene that is itself moving
             * jitters by a unit or two for reasons that have nothing to do
             * with the window's position. Ask for the rectangle instead --
             * that is the thing the test is actually about, and it does not
             * drift with whatever happens to be behind it. */
            int rx = -1, ry = -1, rw = -1, rh = -1;
            int moved = 0, got_rect = 0;

            while (found && mr_scene_in_transition() && n < 40) {
                long w = window_strength();
                int x, y, bw, bh;

                if (w < lo) lo = w;
                if (w > hi) hi = w;
                if (mr_ui_dialogue_rect(&x, &y, &bw, &bh)) {
                    if (got_rect &&
                        (x != rx || y != ry || bw != rw || bh != rh))
                        moved = 1;
                    rx = x; ry = y; rw = bw; rh = bh;
                    got_rect = 1;
                }
                n++;
                frame();
            }
            if (found) {
                printf("window through the move: %d frames, %ld..%ld,"
                       " rect %d,%d %dx%d\n", n, lo, hi, rx, ry, rw, rh);
                check("the window was up throughout the move", lo > 5);
                check("  and its rectangle never moved",
                      got_rect && !moved);
            }
        }
    }

    /* --- `window hide` takes it down, `window show` brings it back --- */
    {
        int hidden = 0, shown_again = 0;

        for (i = 0; i < 4000; i++) {
            if ((i % 6) == 5) {
                answer_menu();
                mr_interp_advance();
            }
            frame();

            if (!hidden) {
                /* A title card: the window is down and no line is up. */
                const char *who, *what;
                mr_u32 c;
                int sh;
                if (!mr_interp_dialogue(&who, &what, &c, &sh) &&
                    !mr_interp_window_visible())
                    hidden = 1;
            } else if (window_strength() > 5) {
                shown_again = 1;
                break;
            }
        }
        check("`window hide` takes the window down", hidden);
        check("and it comes back afterwards", shown_again);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

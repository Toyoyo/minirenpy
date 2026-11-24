/* The move family of transitions, and MoveTransition written longhand.
 *
 * Reported from play on a second game:
 *
 *     show bg parknight:
 *         yoffset 740
 *     with MoveTransition(2.0, enter=None, leave=None, old=False,
 *                         layers=['master'], time_warp=None, ...)
 *
 * showed as a dissolve. The name was not recognised, and an unknown
 * transition becomes a dissolve on the grounds that a dissolve is always
 * visually safe -- which is right as a fallback and wrong here, because every
 * keyword in that call is Ren'Py's own default (movetransition.py:457). It is
 * `MoveTransition(2.0)`: a move, with a 2 second delay instead of the stock
 * 0.5. The delay was already being read from the first argument; only the
 * kind was wrong, so it dissolved over the correct duration.
 *
 * The same sweep of that game's scripts turned up `moveinbottom`,
 * `moveoutbottom` and `hpunch`. The first two are the vertical half of a
 * family whose horizontal half was implemented, and are here. `hpunch` is a
 * different thing -- `Move((15, 0), (-15, 0), .10, bounce=True, repeat=True,
 * delay=.275)` (00definitions.rpy:364), a shake rather than a transition
 * between two scenes -- and is not implemented.
 *
 * Runs on tools/harness/story, label move_trans.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...)
{
    va_list a;
    if (strncmp(f, "DIAG", 4))
        return;
    va_start(a, f);
    printf("   ");
    vprintf(f, a);
    printf("\n");
    va_end(a);
}
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static long T;
static int  fails;

static void check(const char *label, int ok)
{
    printf("  %-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* One frame: update, then compose the stage so the pixels below are the ones
 * the player would be looking at. */
static long g_t;        /* the time of the last update, not the next one */

static void step(void)
{
    g_t = T;
    mr_interp_update(T);
    mr_interp_render(g_st, T);
    T += 33;
}

static int busy(void) { return mr_scene_busy(g_t); }

/* Advance until a transition is actually running.
 *
 * Two traps, and between them they cost this file two rounds of failures that
 * were entirely its own:
 *
 *  - one `mr_interp_advance` does not get past a line of dialogue. On a line
 *    still typing it only *completes* the reveal; it takes another to dismiss
 *    it. Advancing once and then stepping leaves the story parked on the say
 *    with nothing happening, which reads exactly like an engine that does not
 *    animate -- it reported "0ms" for a 1000ms transition.
 *
 *  - busy() has to be tested *before* each advance, never after. Once the
 *    state is ST_TRANS an advance means `mr_scene_skip()`: the harness would
 *    cut short the very effect it is trying to measure. */
static int into_transition(void)
{
    int i;
    for (i = 0; i < 30; i++) {
        if (busy())
            return 1;
        mr_interp_advance();
        step();
    }
    return 0;
}

static const char *line(void)
{
    static char cur[400];
    const char *who, *what;
    mr_u32 c;
    int shown;

    if (mr_interp_dialogue(&who, &what, &c, &shown))
        sprintf(cur, "%.200s", what);
    else
        strcpy(cur, "(no text)");
    return cur;
}

/* Walk until the line on screen contains `needle`. */
static int reach(const char *needle)
{
    int i;
    for (i = 0; i < 400; i++) {
        if (strstr(line(), needle))
            return 1;
        mr_interp_advance();
        step();
    }
    return 0;
}

/* --- finding the image ---
 *
 * `person` is a 300x600 flat colour over a flat background, so down the
 * middle of the stage its rows are a single run of one value. The top of that
 * run is the image's y position, which is what a move changes and a dissolve
 * does not.
 *
 * Measured rather than assumed: the background is whatever `scene dark` is
 * and the image is whatever colour it was generated as, so the top row of the
 * stage supplies the background and anything unlike it is the image. */
#define COL (MR_STAGE_W / 2)

static mr_u32 bg(void)
{
    return g_st->px[0 * g_st->pitch + COL];
}

/* The first row, from the top, whose pixel differs from the background; -1 if
 * the column is uniform. `exact` counts only pixels that are fully one colour
 * -- which is how a half-dissolved image is excluded. */
static int top_edge(int *height)
{
    mr_u32 back = bg();
    int y, first = -1, n = 0;

    for (y = 0; y < MR_STAGE_H; y++) {
        mr_u32 p = g_st->px[y * g_st->pitch + COL];
        if (p != back) {
            if (first < 0)
                first = y;
            n++;
        }
    }
    if (height)
        *height = n;
    return first;
}

int main(int argc, char **argv)
{
    mr_script *s;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: movetest <story-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("move_trans");

    /* What the parser made of each `with`, before anything is rendered.
     * Separating the two questions matters: a transition that never starts
     * and one that starts but does not move look the same on screen, and the
     * first time this ran it was the parse. */
    printf("\n-- what the parser made of each `with` --\n");
    {
        static const char *kindname[] = {
            "none", "dissolve", "fade", "move",
            "moveinleft", "moveinright", "moveoutleft", "moveoutright",
            "moveintop", "moveinbottom", "moveouttop", "moveoutbottom"
        };
        int i, n = 0;

        for (i = 0; i < s->n; i++) {
            if (s->st[i].kind != MR_ST_WITH)
                continue;
            if (strcmp(s->files[s->st[i].file], "script.rpy"))
                continue;
            if (n < 12)
                printf("   line %4d: %-14s %ldms\n", s->st[i].line,
                       (int)s->st[i].trans.kind <= MR_TRANS_MOVEOUTBOTTOM ?
                       kindname[s->st[i].trans.kind] : "?",
                       s->st[i].trans.ms);
            n++;
        }
        printf("   %d `with` statements in the fixture\n", n);
    }

    printf("\n-- MoveTransition(1.0, ...) longhand --\n");
    {
        int y0, h0, y_mid, h_mid, y1, h1;
        long started, ended;
        int moved_report;

        check("the line before the move arrives", reach("Before the move"));
        step();
        y0 = top_edge(&h0);
        printf("   at rest: top edge %d, %d rows of it\n", y0, h0);
        check("  the image is on screen", y0 >= 0 && h0 > 0);

        /* Into the transition. */
        check("  the transition starts", into_transition());
        started = g_t;
        moved_report = mr_scene_move_transition();
        printf("   the engine calls it a %s\n",
               moved_report ? "move" : "dissolve or other");
        /* The direct question. An unrecognised name becomes a dissolve, so
         * this alone is the reported bug. */
        check("  it is a move, not a dissolve", moved_report);

        /* A trace through the effect, because "it jumped" and "it glided"
         * differ only in the middle. */
        /* Midway is captured *inside* the loop. Reading it afterwards is
         * reading the end state, which of course has arrived -- that was
         * this check's own bug, left over from a version that stepped a
         * fixed number of frames.
         *
         * A move has the image at one intermediate position; a dissolve
         * would leave it at its old position fading out and the new one
         * fading in -- two runs in this column, not one, and neither at full
         * strength. Hence the row count as well as the position. */
        y_mid = -1;
        h_mid = 0;
        printf("   trace:");
        {
            int n = 0;
            while (busy()) {
                int yy = top_edge(n == 12 ? &h_mid : NULL);
                if (n == 12)
                    y_mid = yy;
                printf(" %d", yy);
                n++;
                step();
            }
        }
        printf("\n");
        ended = g_t;
        printf("   midway: top edge %d, %d rows\n", y_mid, h_mid);
        check("  the image has moved from where it was", y_mid > y0);
        check("  but has not arrived yet", y_mid < y0 + 300);
        /* 600 rows of image, not 900 of two half-faded copies. */
        check("  and there is one image, not two half-drawn ones",
              h_mid > 0 && h_mid <= 620);

        y1 = top_edge(&h1);
        printf("   after %ldms: top edge %d (started %d, yoffset 300)\n",
               ended - started, y1, y0);
        check("  it ends 300px lower, as the yoffset says",
              y1 >= y0 + 295 && y1 <= y0 + 305);
        /* The duration comes from the first argument. The stock `move` is
         * 500ms; this call says 1.0s. Generous bounds: the frame step is
         * 33ms and the loop above overshoots by up to one frame. */
        printf("   duration %ldms (stock move is 500, this says 1000)\n",
               ended - started);
        check("  and took the duration the call asked for, not 500ms",
              ended - started >= 950 && ended - started <= 1120);
    }

    /* The vertical four, by the direction the image travels.
     *
     * Direction rather than absolute positions: an entering image starts off
     * the stage and a leaving one ends off it, so the endpoints are not
     * always measurable, but the sign of the movement always is. The four
     * are mirror images, so a sign test that passes for all of them with the
     * same code is also a check that none of them was wired to its
     * opposite. */
    {
        static const struct { const char *after; const char *what; int down; }
        want[] = {
            { "After the MoveTransition", "moveoutbottom", +1 },
            { "After moveoutbottom",      "moveinbottom",  -1 },
            { "After moveinbottom",       "moveouttop",    -1 },
            { "After moveouttop",         "moveintop",     +1 }
        };
        int k;

        for (k = 0; k < 4; k++) {
            int first = -1, last = -1, n = 0, valid = 0;
            char label[80];

            printf("\n-- %s --\n", want[k].what);
            sprintf(label, "the line before %s arrives", want[k].what);
            check(label, reach(want[k].after));

            if (!into_transition()) {
                check("  the transition starts", 0);
                continue;
            }
            check("  it is a move", mr_scene_move_transition());

            printf("   trace:");
            while (busy()) {
                int y = top_edge(NULL);
                if (n < 34)
                    printf(" %d", y);
                if (y >= 0) {
                    if (!valid)
                        first = y;
                    last = y;
                    valid++;
                }
                n++;
                step();
            }
            printf("\n   %d samples, %d -> %d\n", valid, first, last);
            check("  the image is visible during the effect", valid > 1);
            if (want[k].down > 0)
                check("  and travels downwards", last > first);
            else
                check("  and travels upwards", last < first);
        }
    }

    /* The reported case, statement for statement. The label above re-shows a
     * tag with only a transform block; this one re-shows it with a new `at`
     * position *and* a transform block, which is what the game does. */
    /* There was a phase here that re-showed a tag with a new `at` position
     * and traced it sideways. It is gone, and the reason is worth recording:
     * the fixture's backdrop is not a flat colour, so scanning a row for
     * "the first pixel unlike x=0" returned a constant 81 whatever the
     * engine did -- a measurement that cannot fail. Three rewrites of that
     * scanner later, the question was settled instead by logging
     * render_move's own numbers for one run: old=(490,180) new=(980,180)
     * with t sweeping 0..252, i.e. correct. A vertical trace down column 640
     * is measurable and is what the phases above use; a horizontal one over
     * this backdrop is not, so it is not pretended. */

    printf("\n-- panning a `scene` background --\n");
    /* The reported bug, and the one the name mapping above did *not* fix.
     *
     * A script pans a backdrop by re-showing it with an offset. `scene X`
     * does not create a shown image here -- it fills a slot of its own that
     * render_move blits at a fixed centred position -- so the `show X:` used
     * to add a second copy beside it, with no previous entry for the tag.
     * render_move counted that as newly shown, which under plain `move` means
     * "draw it at its final position for the whole transition", while the
     * slot kept painting the backdrop where it had always been. The end state
     * looked right and nothing animated.
     *
     * Measured where it is reliable: the backdrop is full-stage, so its own
     * top edge is not findable by scanning, but the *interpolated* position
     * is what was wrong and the renderer reports it through the only thing
     * that moves on screen -- so the person is panned by the same offset in
     * the fixture and stands in for the pair. The backdrop's entry existing
     * at all is checked directly. */
    {
        int i, y0, y_mid = -1, y1, h, frames = 0;
        mr_scenelist sl;

        mr_interp_start("move_bg");
        check("the line before the pan arrives", reach("Before the pan"));
        step();
        y0 = top_edge(&h);

        check("the transition starts", into_transition());
        check("  it is a move", mr_scene_move_transition());

        /* The handover: after the `show room:` the backdrop is a shown entry
         * with tag `room` and the slot is empty. Both halves matter -- an
         * entry *beside* a still-filled slot is the old behaviour, and it
         * draws the backdrop twice. */
        mr_scene_capture(&sl);
        {
            int at = -1, person_at = -1;

            for (i = 0; i < sl.n; i++) {
                if (!strcmp(sl.v[i].tag, "room"))
                    at = i;
                if (!strcmp(sl.v[i].tag, "person"))
                    person_at = i;
            }
            printf("   slot = \"%s\", room at index %d, person at %d of %d\n",
                   sl.bg, at, person_at, sl.n);
            check("  the backdrop became a shown entry", at >= 0);
            check("  and the slot was handed over, not duplicated",
                  sl.bg[0] == '\0');
            /* The z-order, which is the half that broke the sprites. The
             * list is drawn in order, so an appended backdrop draws *over*
             * everything: the reported scene lost takio and hinata behind a
             * 1280-wide background the moment the pan began. It has to take
             * the slot's place at the bottom. */
            check("  at the bottom of the z-order, where the slot drew it",
                  at == 0);
            check("  with the sprite still above it",
                  person_at > at);
        }

        printf("   trace:");
        while (busy()) {
            int yy = top_edge(NULL);
            if (frames == 12)
                y_mid = yy;
            if (frames < 34)
                printf(" %d", yy);
            frames++;
            step();
        }
        y1 = top_edge(&h);
        printf("\n   before the pan %d (the person), after %d (the "
               "backdrop's top edge), %d frames, midway %d\n",
               y0, y1, frames, y_mid);
        /* Deliberately not measured against y0. Before the pan the backdrop
         * fills the stage, so it has no visible top edge and top_edge finds
         * the *person* instead -- 140, which is not a baseline for anything
         * here. Once the pan starts, black appears above the backdrop and the
         * edge being traced is the backdrop's own, from 0 to 300. Comparing
         * the two was this check's first mistake and it failed for a reason
         * that had nothing to do with the engine. */
        check("  the backdrop ends 300px down, as the yoffset says",
              y1 >= 295 && y1 <= 305);
        check("  and was part way there in the middle",
              y_mid > 0 && y_mid < y1 - 50);
    }

    printf("\n-- panning a backdrop taller than the stage --\n");
    /* The end position of the pan, which was wrong twice while every other
     * check here stayed green -- because `room` is 1280x720 and a pan over a
     * backdrop that is not taller than the stage cannot tell the three
     * candidate placements apart. They differ only in where a 1460-tall
     * image sits:
     *
     *   seeded with the default transform  y = 720-1460 = -740 -> +740 = 0
     *   seeded with yalign 0.5 (centring)  y = (720-1460)/2 = -370 -> +370
     *   seeded with nothing (zeros)        y = 0 -> +740, below the stage
     *
     * Only the first shows the backdrop's upper band at the end; the other
     * two leave the sampled row black. The fixture image is in two colours
     * for exactly this reason: on a flat one all three look identical,
     * because a tall backdrop covers the whole stage either way. */
    {
        mr_u32 before, after;
        int i;
        const mr_u32 upper = MR_RGB(0x33, 0x66, 0xcc);
        const mr_u32 lower = MR_RGB(0xcc, 0x66, 0x33);

        mr_interp_start("move_tallbg");
        check("the line before the tall pan arrives",
              reach("Before the tall pan"));
        step();
        before = g_st->px[(MR_STAGE_H / 2) * g_st->pitch + COL];
        printf("   at rest the middle of the stage is %06lx\n",
               (unsigned long)(before & 0xffffff));
        /* `scene tall` has no placement, so the default transform applies:
         * bottom-standing, which for a 1460-tall image shows its lower 720
         * rows -- the ground. */
        check("  the backdrop starts on its lower band",
              (before & 0xffffff) == (lower & 0xffffff));

        check("  the transition starts", into_transition());
        check("  it is a move", mr_scene_move_transition());
        for (i = 0; i < 200 && busy(); i++)
            step();
        step();
        after = g_st->px[(MR_STAGE_H / 2) * g_st->pitch + COL];
        printf("   after the pan it is %06lx (upper %06lx, black means the "
               "image left the stage)\n", (unsigned long)(after & 0xffffff),
               (unsigned long)(upper & 0xffffff));
        check("  and `yoffset 740` lands it on its upper band",
              (after & 0xffffff) == (upper & 0xffffff));
        check("  rather than sliding off the stage entirely",
              (after & 0xffffff) != 0);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

/* Partial frames: while a line types out, only the dialogue box is repainted.
 *
 * The saving is large -- a full composite is a few hundred milliseconds on the
 * target and the box is a quarter of the stage -- but the failure mode is
 * stale pixels left on screen, which is worse than a slow frame. So this does
 * not test the optimisation, it tests that it is invisible: every partial
 * frame is compared against what a full frame would have produced, pixel for
 * pixel, over a long walk through the story.
 *
 * The comparison is against a *reference stage* that is always drawn in full,
 * fed the same statements at the same times. If the two ever differ, the
 * partial path missed something.
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
#include "bandcache.h"

static mr_surface *g_st;
static int base_by;
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

static mr_surface *g_ref;
static long T;
static int  fails;

static void check(const char *label, int ok)
{
    printf("%-52s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void dump_ppm(const mr_surface *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;

    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        for (x = 0; x < s->w; x++) {
            mr_u32 v = s->px[y * s->pitch + x];
            fputc((int)MR_R(v), f);
            fputc((int)MR_G(v), f);
            fputc((int)MR_B(v), f);
        }
    }
    fclose(f);
}

/* --- the frame, as the engine draws it -----------------------------------
 *
 * main.c's frame() as far as the pixels go: the band clip, the band cache, the
 * two halves of mr_ui_draw and the overlay, in that order. Both walks below go
 * through here, which is what makes every pixel comparison in this file a test
 * of the cache as well as of the band -- a strip that went stale shows up as
 * the same divergence a missed repaint does, and in the same oracle.
 *
 * It is a *transcription*: main.c is linked by no harness, so the policy in
 * `keep` below exists twice and the two copies have to be read against each
 * other. The comment there is the one that explains it.
 *
 * `moving` is passed in rather than asked for again because
 * mr_scene_animating is a question with a side effect -- it compares the
 * animation frame against the one last drawn -- so asking twice in one frame
 * gets a different answer the second time.
 */
static long g_cache_hits, g_cache_offers;

static void draw_live(int band, int bx, int by, int bw, int bh,
                      long t, int moving)
{
    int cached = 0;

    mr_clip_reset();
    if (band) {
        mr_clip_push(bx, by, bw, bh);
        cached = mr_band_cache_try(g_st, by, bh);
    }

    if (cached) {
        g_cache_hits++;
        mr_sl_extent_reset();
    } else {
        mr_interp_render(g_st, t);
    }

    if (!cached) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        int keep;

        mr_ui_draw_panel(g_st);

        keep = !moving &&
               !mr_scene_in_transition() &&
               mr_scene_screen_count_under_window() == 0 &&
               mr_ui_dialogue_rect(&ox, &oy, &ow, &oh);
        (void)ox;
        (void)ow;
        mr_band_cache_offer(g_st, oy, oh, keep);
        if (keep)
            g_cache_offers++;
    }

    mr_ui_draw_front(g_st);
    mr_interp_render_overlay(g_st);

    if (band)
        mr_clip_pop();
}

/* The same moment drawn in full, for the oracle. Deliberately *not* split and
 * deliberately cacheless: mr_ui_draw is the two halves back to back, so a
 * reference drawn this way also pins that equivalence. */
static void draw_ref(long t)
{
    mr_clip_reset();
    mr_interp_render(g_ref, t);
    mr_ui_draw(g_ref);
    mr_interp_render_overlay(g_ref);
}

/* --- dialogue over an animated background --------------------------------
 *
 * The walk above runs on static backgrounds, so it cannot answer the question
 * an animated one raises: the partial-frame path repaints only the dialogue
 * box, and that is safe exactly when nothing outside the box has changed. An
 * animated background is the case where something has, and Hinata ships one,
 * so "static between clicks" is a fact about one game's art rather than about
 * the engine.
 *
 * The engine's answer is mr_scene_animating, which asks mr_image_anim_changed
 * about g_cur.bg and so reports movement only on the frames where the
 * animation's frame actually advances -- a held frame is not movement. A band
 * frame is taken only when that says no. If it ever said no while the
 * background had moved, the band frame would repaint the box over a stale
 * stage and the rows *above* the box would keep the previous animation frame,
 * which is why the oracle has to be the whole stage and not the box.
 *
 * Two ways this could be green without testing anything, both guarded below:
 * the animation might never advance (the clock), and the walk might never
 * produce a band frame at all (nothing to compare).
 */
static long first_difference(int *ox, int *oy);

static long bg_pass(void)
{
    long bands = 0, fulls = 0, changes = 0;
    int  diverged = 0, dx = 0, dy = 0;
    long divergent_frame = -1;
    mr_u32 seen = 0;
    int  have_seen = 0;
    int  i;

    /* Above the band (the dialogue window starts at y=535 for this fixture's
     * gui.textbox_height) and clear of the textbox art, so it is background
     * and nothing else. This is the pixel a band frame does *not* repaint,
     * which makes it both the witness that the animation moved and the place
     * a stale frame would show. */
    const int px = 100, py = 100;

    mr_interp_start("anim_bg");

    for (i = 0; i < 900; i++) {
        int kind, moving, band = 0;
        int bx, by, bw, bh;
        mr_u32 now_bg;

        if ((i % 6) == 5) {
            answer_menu();
            mr_interp_advance();
        }

        mr_interp_update(T);
        T += 33;
        mr_image_set_time(T);

        moving = mr_scene_animating(T);
        kind = mr_interp_redraw_kind();

        if (kind == MR_REDRAW_NONE && !moving)
            continue;

        if (!moving && kind == MR_REDRAW_DIALOGUE &&
            mr_ui_dialogue_rect(&bx, &by, &bw, &bh))
            band = 1;

        mr_sl_focus_reset();

        draw_live(band, bx, by, bw, bh, T, moving);
        draw_ref(T);

        if (band)
            bands++;
        else
            fulls++;

        /* Counted off the reference, which is always drawn in full, so this
         * measures the animation rather than the thing under test. */
        now_bg = g_ref->px[py * g_ref->pitch + px];
        if (!have_seen) {
            have_seen = 1;
            seen = now_bg;
        } else if (now_bg != seen) {
            changes++;
            seen = now_bg;
        }

        if (!diverged && first_difference(&dx, &dy)) {
            diverged = 1;
            divergent_frame = i;
            printf("   diverged: band=%d moving=%d kind=%d at %d,%d\n",
                   band, moving, kind, dx, dy);
            printf("   stage %08lx ref %08lx (band starts at y=%d)\n",
                   (unsigned long)g_st->px[dy * g_st->pitch + dx],
                   (unsigned long)g_ref->px[dy * g_ref->pitch + dx],
                   base_by);
            dump_ppm(g_st, "/tmp/bandanim_stage.ppm");
            dump_ppm(g_ref, "/tmp/bandanim_ref.ppm");
            printf("   wrote /tmp/bandanim_stage.ppm and"
                   " /tmp/bandanim_ref.ppm\n");
        }
    }

    printf("animated background: %ld band frames, %ld full, %ld animation"
           " frame changes\n", bands, fulls, changes);

    /* The animation moved, for the whole pass and not just the start of it.
     * The expected figure is arithmetic, not a sample: 900 iterations at 33ms
     * is 29.7s, the cycle is 100+200+400 = 700ms, so 42.4 cycles of 3 frames
     * is about 127 changes -- and 127 is what it reports. Written as a
     * threshold with margin rather than an equality because the walk's exact
     * frame count is not the claim, but a figure far below it means the pass
     * left the animated background partway through and spent the rest back in
     * the static story. That is what it did at first, scoring 53: `anim_bg`
     * ended in `jump the_end`, which goes to `start`. A figure of 0 means the
     * clock is not being driven at all (mr_image_set_time). */
    check("the background animation advanced throughout", changes > 100);

    /* Both paths were exercised. A pass with no band frames compares a full
     * frame against a full frame, which is true of any implementation ever
     * written, and a pass with no full frames never saw the animation move. */
    check("and dialogue still took the partial path while it animated",
          bands > 200);
    check("and full frames happened too, on the frames it moved",
          fulls > 50);

    if (diverged)
        printf("   (first difference at frame %ld, pixel %d,%d)\n",
               divergent_frame, dx, dy);
    check("a partial frame matches a full one over an animated background",
          !diverged);

    return bands;
}

/* The first pixel where the two stages differ, or -1. */
static long first_difference(int *ox, int *oy)
{
    int x, y;

    for (y = 0; y < MR_STAGE_H; y++) {
        for (x = 0; x < MR_STAGE_W; x++) {
            if (g_st->px[y * g_st->pitch + x] !=
                g_ref->px[y * g_ref->pitch + x]) {
                *ox = x;
                *oy = y;
                return 1;
            }
        }
    }
    return 0;
}


/* --- the geometry comes from gui.*, not from constants in ui.c ----------- */

/* The topmost stage row the dialogue box paints, found by differencing a
 * scene-only render against a scene-plus-dialogue one. That is where the say
 * window's top edge actually is, as opposed to where mr_ui_dialogue_rect
 * claims it is -- and keeping those two separate is the whole point. */
static int dialogue_top_drawn(long t)
{
    int x, y;

    mr_clip_reset();
    mr_interp_render(g_ref, t);          /* scene only */
    mr_interp_render(g_st, t);
    mr_ui_draw(g_st);                    /* scene plus dialogue */

    for (y = 0; y < MR_STAGE_H; y++) {
        for (x = 0; x < MR_STAGE_W; x++) {
            if (g_st->px[y * g_st->pitch + x] !=
                g_ref->px[y * g_ref->pitch + x])
                return y;
        }
    }
    return -1;
}

/* ui.c used to hardcode the say window's geometry -- textbox height, the
 * dialogue text's offsets, the name position -- to the values both games
 * happen to share. The file comment claimed they came from gui.rpy; they had
 * been transcribed by hand.
 *
 * Nothing had gone wrong, because Miwa and Hinata agree on all of them. A
 * third game with a different gui.textbox_height would have had its dialogue
 * drawn at the right place and its partial-redraw band computed at the wrong
 * one, leaving stale pixels along the top of the box while a line types out
 * -- visible only during typing, which is the hardest moment to catch.
 *
 * This fixture's gui.rpy is a symlink to the game's, so it cannot carry a
 * different height to prove the point. Instead the variables are set
 * directly and mr_ui_sync_geometry re-read: that is exactly the contract --
 * whatever is in the store when it runs is what the UI uses.
 *
 * Both halves are checked, because each alone passes with the other broken:
 * the band has to follow (or it repaints the wrong strip) *and* the drawing
 * has to follow (or the band is merely a bigger box around an unmoved one,
 * and the containment check below cannot tell). */
static void geometry_follows_gui(long t)
{
    int bx, by, bw, bh;
    int base_top, base_h, drawn_top, base_drawn;

    printf("\n-- the say window's geometry comes from gui.* --\n");

    if (!mr_ui_dialogue_rect(&bx, &by, &bw, &bh)) {
        check("a line is up to measure the geometry against", 0);
        return;
    }
    base_by = by;
    base_h  = bh;
    base_top = by;
    drawn_top = dialogue_top_drawn(t);
    base_drawn = drawn_top;

    printf("   as declared: rect y=%d h=%d, box drawn from y=%d\n",
           by, bh, drawn_top);
    check("the band's height is the game's gui.textbox_height (185)",
          bh == 185);
    /* Containment, not equality. The textbox art has yalign 1.0 inside the
     * window, so with a window taller than the art the top of the band is
     * legitimately empty -- the band is what gets *repainted*, and it has to
     * be a superset of what is drawn, not a tight fit. */
    check("and the drawing starts inside the band",
          drawn_top >= by && drawn_top < by + bh);

    /* Taller. 240 is the value both games' own gui.rpy carries in its
     * commented-out variant block, so it is the realistic third-game case
     * rather than an invented number. */
    mr_var_set_int("gui.textbox_height", 240);
    mr_ui_sync_geometry();

    if (!mr_ui_dialogue_rect(&bx, &by, &bw, &bh)) {
        check("the rect survives a geometry change", 0);
        return;
    }
    drawn_top = dialogue_top_drawn(t);
    printf("   at 240:      rect y=%d h=%d, box drawn from y=%d\n",
           by, bh, drawn_top);

    check("the band follows gui.textbox_height", bh == 240);
    check("the band's top moves up with it", by == MR_STAGE_H - 240);
    check("the drawing is still inside it",
          drawn_top >= by && drawn_top < by + bh);
    /* The drawing has to have *moved*, or the band could simply be a bigger
     * box drawn around an unmoved one -- which containment alone cannot
     * tell apart. The namebox sits at the window's top, so it tracks it. */
    check("and the drawing moved with it", drawn_top != base_drawn);
    check("which is a real change, not the old value", by != base_top);

    /* Shorter, so a test that merely grew everything cannot pass. */
    mr_var_set_int("gui.textbox_height", 120);
    mr_ui_sync_geometry();
    mr_ui_dialogue_rect(&bx, &by, &bw, &bh);
    drawn_top = dialogue_top_drawn(t);
    printf("   at 120:      rect y=%d h=%d, box drawn from y=%d\n",
           by, bh, drawn_top);
    check("a shorter textbox shortens the band too",
          bh >= 120);
    /* 120 is shorter than the 185-tall textbox art, so the art hangs above
     * the window's top. The band must stretch to cover it: this is the case
     * that used to draw outside the band and leave stale rows on screen
     * while a line typed out. */
    check("a window shorter than its art still contains the art",
          drawn_top >= by && drawn_top < by + bh);

    /* yalign is a fraction, not an assumption that the box is at the bottom.
     * 0.0 puts it at the top of the stage. */
    mr_var_set_int("gui.textbox_height", 185);
    {
        mr_value v;
        v.type = MR_V_FLOAT;
        v.i = 0;
        v.f = 0.0;
        v.s[0] = '\0';
        mr_var_set("gui.textbox_yalign", &v);
    }
    mr_ui_sync_geometry();
    mr_ui_dialogue_rect(&bx, &by, &bw, &bh);
    drawn_top = dialogue_top_drawn(t);
    printf("   yalign 0.0:  rect y=%d h=%d, box drawn from y=%d\n",
           by, bh, drawn_top);
    check("gui.textbox_yalign 0.0 puts the window at the top", by == 0);
    check("and the drawing goes with it",
          drawn_top >= 0 && drawn_top < bh && drawn_top != base_drawn);

    /* An out-of-range height must not put the band off the surface: the rect
     * is used to clip a blit. */
    mr_var_set_int("gui.textbox_height", 99999);
    mr_ui_sync_geometry();
    mr_ui_dialogue_rect(&bx, &by, &bw, &bh);
    check("an absurd textbox height is clamped to the stage",
          bh <= MR_STAGE_H && by >= 0 && by + bh <= MR_STAGE_H);

    /* Put it all back, so anything after this sees the real game again. */
    mr_var_set_int("gui.textbox_height", 185);
    {
        mr_value v;
        v.type = MR_V_FLOAT;
        v.i = 0;
        v.f = 1.0;
        v.s[0] = '\0';
        mr_var_set("gui.textbox_yalign", &v);
    }
    mr_ui_sync_geometry();
    mr_ui_dialogue_rect(&bx, &by, &bw, &bh);
    check("and restoring the real values restores the real band",
          bh == base_h && by == base_by);
}

/* --- dialogue with a screen inside the strip ------------------------------
 *
 * The one thing the band cache must refuse, and the one the pixel oracle
 * cannot see. A screen at zorder 0 is drawn by draw_screens, inside
 * mr_interp_render -- which is precisely the pass a cached frame skips. Its
 * pixels would come back from the strip and look perfect; what would not come
 * back is its entry in the focus list, which is rebuilt from that same pass
 * and is what the next click reads. The button would be visible and dead.
 *
 * So the oracle here is mr_sl_hit, not a pixel: on every drawn frame, the
 * point the fixture's button occupies must still be focusable. And the second
 * claim is that no strip was kept at all while it was up, which is how the
 * engine arranges the first.
 */
static void screen_pass(void)
{
    long bands = 0, drawn = 0, dead = 0;
    long hits_before = g_cache_hits;
    int  i;

    /* Inside `screen band_screen`'s textbutton: xpos 40 ypos 560, and the
     * glyphs are at least a few pixels tall and wide from there. */
    const int hx = 48, hy = 568;

    mr_interp_start("band_with_screen");

    for (i = 0; i < 600; i++) {
        int kind, moving, band = 0;
        int bx, by, bw, bh;

        if ((i % 6) == 5) {
            answer_menu();
            mr_interp_advance();
        }

        mr_interp_update(T);
        T += 33;
        mr_image_set_time(T);

        moving = mr_scene_animating(T);
        kind = mr_interp_redraw_kind();

        if (kind == MR_REDRAW_NONE && !moving)
            continue;

        if (!moving && kind == MR_REDRAW_DIALOGUE &&
            mr_ui_dialogue_rect(&bx, &by, &bw, &bh))
            band = 1;

        mr_sl_focus_reset();
        draw_live(band, bx, by, bw, bh, T, moving);

        /* Before the reference frame, and that is not a detail. The reference
         * is a second full render of the same moment, and rendering a screen
         * is what registers its focusables -- so drawing it would repopulate
         * the list that the live frame may just have left empty, and this
         * claim would pass with the cache misbehaving. Asked here, the list
         * is exactly what a click arriving after the live frame would read.
         *
         * "Focusable at that point" would not be enough either: the quick
         * menu is also up and also inside the strip, so a hit could be one of
         * its buttons with band_screen missing entirely. The action is what
         * identifies it. */
        {
            int id = mr_sl_hit(hx, hy);
            const char *act = id >= 0 ? mr_sl_focus_action(id) : NULL;

            if (!act || strcmp(act, "Return()") != 0)
                dead++;
        }

        draw_ref(T);

        drawn++;
        if (band)
            bands++;
    }

    printf("screen in the strip: %ld drawn, %ld band, %ld frames with the"
           " button unclickable\n", drawn, bands, dead);

    /* Vacuity, both ways. No band frames and the pass is about nothing; no
     * frame that ever found the button and mr_sl_hit is being asked about the
     * wrong pixel, which would make `dead == 0` unreachable rather than
     * true. */
    check("a screen inside the strip still allows band frames", bands > 50);
    check("and the fixture's button is where the test looks for it",
          dead < drawn);

    check("a screen under the window keeps its place in the focus list",
          dead == 0);
    check("which it does by keeping no strip while one is up",
          g_cache_hits == hits_before);
}

/* --- the cache as a mechanism --------------------------------------------
 *
 * The walks above prove the cache is *invisible*, which is the claim that
 * matters, but they prove it over whatever rectangles and sequences the
 * fixture's story happens to produce. These are the module's own edges, on a
 * surface small enough to state exactly: the rectangle test, the invalidation,
 * the reallocation, and the bytes it costs.
 *
 * Run after the walks and ending in a drop, because it leaves a strip of its
 * own size in the cache and the real sizes must not be left behind.
 */
static void cache_unit(void)
{
    mr_surface *s = mr_surface_new(64, 32);
    const int y = 8, h = 8;
    int i;
    int outside_ok = 1, inside_ok = 1;

    if (!s) {
        check("a surface to test the band cache on", 0);
        return;
    }

    mr_band_cache_drop();
    check("nothing is held before the first offer",
          !mr_band_cache_try(s, y, h) && mr_band_cache_bytes() == 0);

    /* Each pixel its own value, so a copy from the wrong row or a stale
     * column cannot look like a correct one. */
    for (i = 0; i < 64 * 32; i++)
        s->px[i] = MR_RGB((i * 7) & 255, (i * 13) & 255, (i * 29) & 255);

    mr_band_cache_offer(s, y, h, 1);
    check("a strip offered is a strip held", mr_band_cache_bytes() > 0);
    check("and it costs exactly the band it covers",
          mr_band_cache_bytes() == (long)s->w * h * 4);

    /* Scribble over the whole surface: the band must come back and the rest
     * must not be touched, which is the one thing a caller relies on -- the
     * rows outside the band are the previous frame's and stay that way. */
    for (i = 0; i < 64 * 32; i++)
        s->px[i] = MR_RGB(1, 2, 3);

    check("a held strip is restored", mr_band_cache_try(s, y, h) == 1);

    for (i = 0; i < 64 * 32; i++) {
        int row = i / 64;
        mr_u32 want = MR_RGB((i * 7) & 255, (i * 13) & 255, (i * 29) & 255);

        if (row >= y && row < y + h) {
            if (s->px[i] != want)
                inside_ok = 0;
        } else if (s->px[i] != MR_RGB(1, 2, 3)) {
            outside_ok = 0;
        }
    }
    check("restoring puts back the band's own pixels", inside_ok);
    check("and writes nothing outside the band", outside_ok);

    /* The rectangle is part of the key. A strip taken at one position is not
     * an answer about another, and the band moves whenever
     * gui.textbox_height does. */
    check("a strip for another position is not offered up",
          !mr_band_cache_try(s, y + 1, h));
    check("nor one for another height", !mr_band_cache_try(s, y, h + 1));
    check("and the original is still good after both",
          mr_band_cache_try(s, y, h) == 1);

    /* keep == 0 invalidates rather than merely declining to store, because
     * the reason to decline is always that what is held has gone stale. */
    mr_band_cache_offer(s, y, h, 0);
    check("declining to keep a strip drops the one in hand",
          !mr_band_cache_try(s, y, h));

    /* A rectangle off the surface must store nothing: the caller clips a copy
     * with these numbers, and mr_ui_dialogue_rect's come from a script. */
    mr_band_cache_offer(s, 28, 8, 1);
    check("a band running off the surface is refused",
          !mr_band_cache_try(s, 28, 8));
    mr_band_cache_offer(s, -1, 8, 1);
    check("and so is one starting above it",
          !mr_band_cache_try(s, -1, 8));

    /* A different height reallocates rather than keeping the old buffer and
     * copying part of it. */
    mr_band_cache_offer(s, 4, 16, 1);
    check("a taller band resizes the strip",
          mr_band_cache_try(s, 4, 16) == 1 &&
          mr_band_cache_bytes() == (long)s->w * 16 * 4);

    /* The change `keep` cannot see: the window's geometry being re-read. The
     * rectangle may not move, but the art at it can. */
    mr_band_cache_offer(s, 4, 16, 1);
    mr_ui_sync_geometry();
    check("re-reading the window geometry drops the strip",
          !mr_band_cache_try(s, 4, 16) && mr_band_cache_bytes() == 0);

    mr_band_cache_drop();
    check("and a drop releases the buffer", mr_band_cache_bytes() == 0);

    mr_surface_free(s);
}

int main(int argc, char **argv)
{
    mr_script *s;
    int i;
    long bands = 0, fulls = 0;
    int  diverged = 0, dx = 0, dy = 0;
    long divergent_frame = -1;

    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: bandtest <game-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    mr_scene_init();
    s = mr_script_load(argv[1]);
    mr_interp_init(s, argv[1]);
    mr_ui_init(argv[1]);
    g_st  = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_ref = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    mr_interp_start("start");
    mr_interp_main_menu();

    /* A text speed, so lines type out. Unset means instant in Ren'Py, and an
     * instantly-revealed line never asks for a second frame -- which would
     * make this test pass by drawing nothing. */
    mr_expr_exec("_preferences.text_cps = 30");

    mr_interp_action("Start()");

    /* Play for a while, clicking through lines. Long enough to cross scene
     * changes, transitions, `window hide`, and the chapter title cards. */
    for (i = 0; i < 1200; i++) {
        int kind, moving;
        int bx, by, bw, bh;
        int band = 0;
        static struct { int i, kind, moving, band, drawn; } hist[8];
        static int nhist;

        /* Click through at a steady rate. This comes first because a frame
         * the loop would skip still has to be able to advance the story --
         * the story spends most of its time waiting for exactly this. */
        if ((i % 6) == 5) {
            answer_menu();
            mr_interp_advance();
        }

        mr_interp_update(T);
        T += 33;

        /* main.c's frame() calls this before mr_interp_update, so that every
         * mr_image_get in a frame agrees on which frame of an animation is
         * showing. This harness advances T *between* the update and the
         * render, so setting it here instead is what makes the two line up:
         * after the increment, g_now equals the timestamp the renders and
         * mr_scene_animating below both use, and equals the argument the next
         * iteration's mr_interp_update will be given.
         *
         * Without this call at all, g_now stays 0 and no animation ever
         * advances -- which is how the animated-background pass below could
         * have come out green while testing nothing. */
        mr_image_set_time(T);

        moving = mr_scene_animating(T);
        kind = mr_interp_redraw_kind();

        hist[nhist & 7].i = i;
        hist[nhist & 7].kind = kind;
        hist[nhist & 7].moving = moving;
        hist[nhist & 7].drawn = !(kind == MR_REDRAW_NONE && !moving);
        hist[nhist & 7].band = 0;

        if (kind == MR_REDRAW_NONE && !moving) {
            nhist++;
            continue;                      /* the frame loop would skip it */
        }

        if (!moving && kind == MR_REDRAW_DIALOGUE &&
            mr_ui_dialogue_rect(&bx, &by, &bw, &bh))
            band = 1;

        mr_sl_focus_reset();

        /* The frame as the engine would draw it, and then as it would look
         * drawn in full. Note the second runs the renderer again at the same
         * timestamp, which is why the scene layer has to be free of per-call
         * side effects for this to mean anything -- and it is: stepping a
         * transform is driven by the timestamp. */
        draw_live(band, bx, by, bw, bh, T, moving);
        draw_ref(T);

        hist[nhist & 7].band = band;
        nhist++;

        if (band)
            bands++;
        else
            fulls++;

        if (!diverged && first_difference(&dx, &dy)) {
            const char *who, *what;
            mr_u32 color;
            int shown;
            int k;

            diverged = 1;
            divergent_frame = i;

            /* Everything needed to find the cause, because "frame 156 differs
             * at 1151,180" on its own is not enough to act on: the run-up of
             * frames says whether it followed a transition, and the two
             * images say what was left behind. The first time this fired, the
             * stage was black above the dialogue box and the reference was
             * not -- render_list clearing the stage through the one pixel
             * writer that ignored the clip. */
            printf("   diverged: band=%d moving=%d kind=%d screens=%d\n",
                   band, moving, kind, mr_scene_screen_count());
            for (k = 7; k >= 0; k--) {
                int j = (nhist - k) & 7;
                printf("   frame %4d kind=%d moving=%d band=%d drawn=%d\n",
                       hist[j].i, hist[j].kind, hist[j].moving,
                       hist[j].band, hist[j].drawn);
            }
            printf("   stage %08lx ref %08lx\n",
                   (unsigned long)g_st->px[dy * g_st->pitch + dx],
                   (unsigned long)g_ref->px[dy * g_ref->pitch + dx]);
            if (mr_interp_dialogue(&who, &what, &color, &shown))
                printf("   line: %.60s\n", what);
            dump_ppm(g_st, "/tmp/band_stage.ppm");
            dump_ppm(g_ref, "/tmp/band_ref.ppm");
            printf("   wrote /tmp/band_stage.ppm and /tmp/band_ref.ppm\n");
        }
    }

    printf("%ld dialogue-only frames, %ld full frames\n", bands, fulls);

    check("partial frames happen at all", bands > 50);
    check("and they are the common case", bands > fulls / 2);

    if (diverged)
        printf("   (first difference at frame %ld, pixel %d,%d)\n",
               divergent_frame, dx, dy);
    check("a partial frame draws what a full frame would", !diverged);

    /* How much of the above was actually about the cache. The pixel
     * comparison tests it only on the frames that restored a strip, so a run
     * with no hits is the old code path passing the old test: it would stay
     * green with the cache disabled, which is exactly the vacuous outcome
     * worth guarding against here. */
    printf("band cache: %ld frames restored a strip, %ld strips taken\n",
           g_cache_hits, g_cache_offers);
    check("band frames restored a cached strip", g_cache_hits > 50);
    check("and that is what most band frames did",
          g_cache_hits > bands / 2);

    /* The rectangle really does contain the dialogue: blank the stage outside
     * it, redraw a full frame, and the difference must be confined to the
     * box. This is the assumption the whole thing rests on. */
    {
        int bx, by, bw, bh;

        for (i = 0; i < 400 && !mr_ui_dialogue_rect(&bx, &by, &bw, &bh); i++) {
            mr_interp_update(T);
            T += 33;
            mr_sl_focus_reset();
            mr_interp_render(g_st, T);
            mr_ui_draw(g_st);
            if ((i % 6) == 5) {
                answer_menu();
                mr_interp_advance();
            }
        }

        if (mr_ui_dialogue_rect(&bx, &by, &bw, &bh)) {
            int x, y, outside = 0;

            printf("dialogue rect: %d,%d %dx%d\n", bx, by, bw, bh);

            mr_clip_reset();
            mr_interp_render(g_ref, T);          /* scene only, no dialogue */

            mr_interp_render(g_st, T);
            mr_ui_draw(g_st);                    /* scene plus dialogue */

            for (y = 0; y < MR_STAGE_H; y++) {
                for (x = 0; x < MR_STAGE_W; x++) {
                    if (g_st->px[y * g_st->pitch + x] ==
                        g_ref->px[y * g_ref->pitch + x])
                        continue;
                    if (x >= bx && x < bx + bw && y >= by && y < by + bh)
                        continue;
                    outside++;
                }
            }
            printf("   (%d pixels of dialogue outside its rect)\n", outside);
            check("the dialogue draws nothing outside its rect", !outside);

            /* The same invariant is what makes the geometry section below
             * meaningful: if the band and the drawing ever stop agreeing,
             * this is the check that notices. */
            geometry_follows_gui(T);
        } else {
            check("found a line to check the rect against", 0);
        }
    }

    /* Last, because it restarts the interpreter at its own label and the
     * geometry checks above want the walk's state. */
    printf("\n-- dialogue over an animated background --\n");
    bg_pass();

    printf("\n-- dialogue with a screen inside the strip --\n");
    screen_pass();

    printf("\n-- the band cache as a mechanism --\n");
    cache_unit();

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

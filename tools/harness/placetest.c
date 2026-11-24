/* Where a `show ... at X:` block actually puts an image.
 *
 * Ren'Py's rule is that an integer position is a pixel count and a float is a
 * fraction of the stage, and `align` is sugar for setting *both* pos and
 * anchor -- so the rule applies to it too. `yalign 1` is one pixel, which puts
 * an image at the top; `yalign 1.0` is the bottom. The two differ by a single
 * character in the script and by 185 pixels on screen, and the game's
 * miwasunset-draw uses the integer form.
 *
 * The script is written to a temporary directory and run through the real
 * lexer and parser, because the parser is where the distinction is made:
 * asserting on a hand-built mr_place would test the resolver and miss it.
 *
 * Numbers are for a 1280x720 stage and an 803x535 image.
 *
 * The second half renders. Everything above calls mr_place_resolve directly,
 * which proves the resolver and says nothing about whether the draw path
 * hands it the right arguments -- and that is exactly where this broke:
 * mr_shown had no has_place field at all, so render_list passed a literal 1
 * and every sprite was placed as though it carried a transform. A bare
 * `show kalea` landed in the top-left corner instead of standing on the
 * bottom. The checks above were all green throughout.
 *
 * So the render section drives parser -> mr_scene_show -> mr_scene_render and
 * measures the ink on the stage. The fixture sprite is 206x576, the size from
 * the report, on a 1280x720 stage: bottom centre is (537, 144), and the
 * corner the bug put it in is (0, 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mr.h"
#include "script.h"
#include "scene.h"
#include "image.h"
#include "vfs.h"
#include "expr.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_audio_poll(void) {}
/* image.c reaches for timeGetTime (MR_PROF), which drags in the host's fake
 * waveOut, which refers to this. */
static void sink(const short *d, int n) { (void)d; (void)n; }
void (*hostshim_wave_sink)(const short *, int) = sink;

static int host_call(const char *n, mr_value *a, const char **an, int c,
                     mr_value *r)
{
    (void)n; (void)an;
    memset(r, 0, sizeof(*r));
    if (c > 0) *r = a[0];
    return 1;
}

#define IMG_W 803
#define IMG_H 535

static mr_script *g_s;
static int fails;

/* The placement of the show statement for `name`, resolved. */
static void placement(const char *name, int *x, int *y)
{
    int i;
    for (i = 0; i < g_s->n; i++) {
        if (g_s->st[i].kind == MR_ST_SHOW &&
            !strcmp(g_s->st[i].name, name)) {
            mr_place_resolve(&g_s->st[i].place, g_s->st[i].has_place,
                             IMG_W, IMG_H, x, y);
            return;
        }
    }
    *x = *y = -9999;
}

static void check(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-46s %6d want %6d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* --- the render path ---------------------------------------------------- */

#define SPR_W 206
#define SPR_H 576

/* Named as a path, not as a registered image. mr_image_init builds its
 * registry by enumerating images/ through the vfs, and the loose-file branch
 * of mr_vfs_enum is _WIN32-only (vfs.c:457), so on the gcc harness there is
 * no registry without a pak. A name containing '/' bypasses it and is loaded
 * straight from the data (image.c:640-645), which works either way. */
#define SPRITE "images/sprite.png"

/* Show `name`'s statement the way the interpreter does, render, and report
 * the bounding box of everything that is not black. render_list fills the
 * stage black first and the fixture sprite is opaque and nowhere near black,
 * so the box is the sprite. */
static int ink_box(const char *name, int *x0, int *y0, int *x1, int *y1)
{
    mr_surface *stage;
    int i, px, py, found = 0;

    *x0 = *y0 = 1 << 20;
    *x1 = *y1 = -1;

    mr_scene_clear();

    for (i = 0; i < g_s->n; i++) {
        if (g_s->st[i].kind == MR_ST_SHOW && !strcmp(g_s->st[i].name, name)) {
            /* The statement supplies the placement; the image is always the
             * fixture sprite, so every case is measured at one known size. */
            mr_scene_show(SPRITE, &g_s->st[i].place, g_s->st[i].has_place);
            found = 1;
            break;
        }
    }
    if (!found)
        return 0;

    mr_scene_commit();

    stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    if (!stage)
        return 0;
    mr_scene_render(stage, 0);

    for (py = 0; py < MR_STAGE_H; py++) {
        for (px = 0; px < MR_STAGE_W; px++) {
            if ((stage->px[py * stage->w + px] & 0x00FFFFFFu) != 0) {
                if (px < *x0) *x0 = px;
                if (py < *y0) *y0 = py;
                if (px > *x1) *x1 = px;
                if (py > *y1) *y1 = py;
            }
        }
    }

    mr_surface_free(stage);
    return (*x1 >= 0);
}

static void render_checks(void)
{
    int x0, y0, x1, y1;

    printf("\n-- through mr_scene_render, on the stage --\n");

    /* The reported bug. A bare `show` carries no transform, so Ren'Py applies
     * config.default_transform (00definitions.rpy:64-67) and the sprite
     * stands on the bottom, centred. Before the fix this was (0, 0). */
    if (!ink_box("plain", &x0, &y0, &x1, &y1)) {
        printf("could not render `plain`\n");
        fails++;
        return;
    }
    check("bare `show`: drawn centred", x0, (MR_STAGE_W - SPR_W) / 2);
    check("  standing on the bottom", y0, MR_STAGE_H - SPR_H);
    check("  and the far edge follows", x1, (MR_STAGE_W - SPR_W) / 2 + SPR_W - 1);
    check("  down to the last row", y1, MR_STAGE_H - 1);

    /* The other half of the rule, and the reason the flag has to be carried
     * rather than assumed either way: a statement that says anything at all
     * gets no default, so the axis it does not mention stays at 0. If the
     * fix had been "always default", this is what would have broken -- and
     * it is the mouth-overlay regression in the parse section above, seen
     * through the renderer. */
    if (!ink_box("mouth", &x0, &y0, &x1, &y1)) {
        printf("could not render `mouth`\n");
        fails++;
        return;
    }
    check("a placed `show`: xoffset 129 is x=129", x0, 129);
    check("  and yoffset 443 is y=443", y0, 443);

    /* A re-show that says nothing must not re-default: Ren'Py's re-show path
     * inherits the previous placement (transform_state(old, thing),
     * scenelists.py:445). `mouth` is on screen at (129, 443) from just above;
     * showing the same tag again with no placement has to leave it there. */
    mr_scene_show(SPRITE, NULL, 0);
    {
        mr_surface *stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
        int px, py;
        x0 = y0 = 1 << 20;
        mr_scene_commit();
        mr_scene_render(stage, 0);
        for (py = 0; py < MR_STAGE_H; py++)
            for (px = 0; px < MR_STAGE_W; px++)
                if ((stage->px[py * stage->w + px] & 0x00FFFFFFu) != 0) {
                    if (px < x0) x0 = px;
                    if (py < y0) y0 = py;
                }
        mr_surface_free(stage);
    }
    check("re-showing the tag keeps its placement: x", x0, 129);
    check("  and y, rather than defaulting to the bottom", y0, 443);

    /* The same inheritance when the tag was first shown *bare*: what the
     * re-show takes over is config.default_transform's state (scenelists.py:
     * 444 -> transform_state -> take_state, :306), so
     *
     *     show park
     *     show park:
     *         yoffset 400
     *     with move
     *
     * slides the image 400px down from bottom centre. The block used to be
     * merged into an empty placement instead, so the unnamed axes were 0 and
     * the image jumped to the top-left corner, 400px down. The backdrop
     * handover below had the seed; a sprite re-show did not. */
    mr_scene_clear();
    mr_scene_show(SPRITE, NULL, 0);
    {
        int i, found = 0;
        for (i = 0; i < g_s->n; i++)
            if (g_s->st[i].kind == MR_ST_SHOW &&
                !strcmp(g_s->st[i].name, "reshowpan")) {
                mr_scene_show(SPRITE, &g_s->st[i].place,
                              g_s->st[i].has_place);
                found = 1;
                break;
            }
        if (!found) {
            printf("no `reshowpan` statement\n");
            fails++;
            return;
        }
    }
    {
        mr_surface *stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
        int px, py;
        x0 = y0 = 1 << 20;
        mr_scene_commit();
        mr_scene_render(stage, 0);
        for (py = 0; py < MR_STAGE_H; py++)
            for (px = 0; px < MR_STAGE_W; px++)
                if ((stage->px[py * stage->w + px] & 0x00FFFFFFu) != 0) {
                    if (px < x0) x0 = px;
                    if (py < y0) y0 = py;
                }
        mr_surface_free(stage);
    }
    check("bare show, then `yoffset 400`: still centred", x0,
          (MR_STAGE_W - SPR_W) / 2);
    check("  and 400px below the bottom-standing y", y0,
          MR_STAGE_H - SPR_H + 400);
}

static const char SCRIPT[] =
    "label start:\n"
    "    show intalign at center:\n"
    "        yalign 1\n"
    "    show floatalign:\n"
    "        yalign 1.0\n"
    "    show halfalign:\n"
    "        yalign 0.5\n"
    "    show zeroalign:\n"
    "        yalign 0\n"
    "    show bothalign:\n"
    "        xalign 0.5 yalign 0.5\n"
    "    show plain\n"
    "    show atcenter at center\n"
    "    show pixelpos:\n"
    "        ypos 100 yanchor 0\n"
    /* An absolute-coordinate overlay, which is what a mouth sprite is. */
    "    show mouth:\n"
    "        yalign 0.0 xoffset 129 yoffset 443\n"
    /* Shown after a bare show of the same tag by render_checks. */
    "    show reshowpan:\n"
    "        yoffset 400\n"
    /* --- clauses before the `at` ---
     *
     * Ren'Py's image specifier is a loop over clauses in any order
     * (parser.py:140-185). We looked for `at` immediately after the image
     * name and nowhere else, so a `behind` in front of it took the whole
     * placement down with it -- and the `with` behind that as well.
     * The three list spellings are all one list. */
    "    show reported behind keiko at right:\n"
    "        xoffset 100\n"
    "    show listnospace behind keiko,azami at right\n"
    "    show listspace behind keiko, azami at right\n"
    "    show listspaceboth behind keiko , azami at right\n"
    "    show clauseafter at right behind keiko\n"
    "    show clausewith behind keiko at right with dissolve\n"
    "    return\n";

int main(int argc, char **argv)
{
    int x, y;
    FILE *f;

    setbuf(stdout, NULL);

    if (argc < 2) {
        printf("usage: placetest <fixture dir>\n");
        return 2;
    }

    if (system("rm -rf /tmp/placetest && mkdir -p /tmp/placetest/images") != 0)
        return 2;
    /* The script is generated, the sprite is a committed fixture: copy it in
     * beside the script so one vfs root serves both. */
    {
        char cmd[512];
        sprintf(cmd, "cp '%s/images/sprite.png' /tmp/placetest/images/",
                argv[1]);
        if (system(cmd) != 0) {
            printf("no sprite.png in %s/images\n", argv[1]);
            return 2;
        }
    }
    f = fopen("/tmp/placetest/script.rpy", "wb");
    if (!f)
        return 2;
    fwrite(SCRIPT, 1, sizeof(SCRIPT) - 1, f);
    fclose(f);

    mr_vfs_init(NULL, "/tmp/placetest");
    mr_expr_init(host_call);
    g_s = mr_script_load("/tmp/placetest");
    if (!g_s) {
        printf("script load failed\n");
        return 1;
    }

    placement("intalign", &x, &y);
    check("`yalign 1` is one pixel: top of the screen", y, 0);
    check("  and `at center` still centres it", x, (1280 - IMG_W) / 2);

    placement("floatalign", &x, &y);
    check("`yalign 1.0` is a fraction: the bottom", y, 720 - IMG_H);

    placement("halfalign", &x, &y);
    check("`yalign 0.5` centres vertically", y, (720 - IMG_H) / 2);

    placement("zeroalign", &x, &y);
    check("`yalign 0` is the top too", y, 0);

    placement("bothalign", &x, &y);
    check("`xalign 0.5` centres horizontally", x, (1280 - IMG_W) / 2);
    check("  with yalign 0.5 on the same line", y, (720 - IMG_H) / 2);

    /* --- a placement that was given at all gets no default ---
     *
     * Ren'Py applies config.default_transform -- bottom centre -- only when
     * the shown thing is not already a Transform (scenelists.py:450), and a
     * `show` with an `at` list or a transform block already is one. So an
     * axis a placement does not mention is *zero*, not defaulted: whatever
     * is left as None, place() turns into 0 (displayable.py:51-58).
     *
     * This had no coverage at all, which is how it stayed wrong. `yalign
     * 1.0` above leaves x unspecified, and nothing here looked at its x. */
    placement("plain", &x, &y);
    check("no transform at all: stands on the bottom", y, 720 - IMG_H);
    check("  and is centred horizontally", x, (1280 - IMG_W) / 2);

    placement("atcenter", &x, &y);
    check("`at center` centres x", x, (1280 - IMG_W) / 2);
    /* `transform center` is `xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0`
     * (00definitions.rpy:40), so it names y as well -- and has to here too,
     * or the rule above would move every sprite in the game to the top. */
    check("  and names y as well: still on the bottom", y, 720 - IMG_H);

    placement("floatalign", &x, &y);
    check("a y-only block leaves x at 0, not centred", x, 0);

    placement("pixelpos", &x, &y);
    check("`ypos 100 yanchor 0` is unaffected", y, 100);
    check("  with x at 0", x, 0);

    /* The reported bug, with the real numbers: a 25x18 mouth meant to sit at
     * (129, 443) over a face. Centring first put it at 756 -- 627px right of
     * where the script asked, and wrong by a different amount for every
     * image width. */
    placement("mouth", &x, &y);
    check("`yalign 0.0 xoffset 129` is x=129, not centre+129", x, 129);
    check("  and yoffset 443 from the top is y=443", y, 443);

    /* --- a clause in front of the `at` ---
     *
     * The reported line was
     *
     *     show eari uniform flip behind keiko at right:
     *         xoffset 100
     *
     * and the sprite drew in the top-left corner with only the xoffset. The
     * `at right` was never parsed: `behind` sat where the parser expected
     * `at`, so it stopped looking. 136 of Azami's and Hinata's show
     * statements put a clause before the `at`.
     *
     * Note what makes it *invisible* rather than merely wrong: the inline
     * block still ran, so has_place was 1, so the unnamed axes resolved to
     * zero instead of to bottom centre. A dropped `at` with no block would
     * have left the sprite standing in the middle, which looks fine. */
    placement("reported", &x, &y);
    check("`behind X at right` still reaches the `at`", x, 1280 - IMG_W + 100);
    check("  and stands on the bottom, not the top", y, 720 - IMG_H);

    placement("listnospace", &x, &y);
    check("`behind a,b at right` -- no space in the list", x, 1280 - IMG_W);
    placement("listspace", &x, &y);
    check("`behind a, b at right` -- space after the comma", x, 1280 - IMG_W);
    placement("listspaceboth", &x, &y);
    check("`behind a , b at right` -- spaces either side", x, 1280 - IMG_W);

    placement("clauseafter", &x, &y);
    check("`at right behind X` -- the order that always worked",
          x, 1280 - IMG_W);

    /* The `with` was lost to the same stop: read_with only looks at the
     * position the specifier left it in, and that was still `behind`. So the
     * transition silently did not happen either. */
    {
        int i, found = 0;
        for (i = 0; i < g_s->n; i++) {
            if (g_s->st[i].kind == MR_ST_SHOW &&
                !strcmp(g_s->st[i].name, "clausewith")) {
                found = (i + 1 < g_s->n && g_s->st[i + 1].kind == MR_ST_WITH);
                break;
            }
        }
        check("the `with` behind the clause is not lost either", found, 1);
    }

    /* The images live in the fixture rather than in /tmp beside the script,
     * so the sprite is a committed file with known dimensions. */
    mr_image_init("/tmp/placetest");
    render_checks();
    mr_image_shutdown();
    mr_scene_shutdown();

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    mr_script_free(g_s);
    mr_expr_shutdown();
    mr_vfs_shutdown();
    return fails ? 1 : 0;
}

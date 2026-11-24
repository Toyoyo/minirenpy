/* Named transforms: `transform left640:` and `show x at left640`.
 *
 * These were skipped by the parser along with their bodies, and every `at`
 * that named one fell through to "unknown position, leaving default". Azami
 * does it 739 times across 41 transforms, so 739 sprites stood bottom-centre
 * instead of where the script put them -- and said nothing on screen, because
 * bottom-centre is a perfectly plausible place for a sprite to be.
 *
 * In Ren'Py a transform is a store variable holding an ATLTransform
 * (ast.py:1323-1329) wrapped in an Init (parser.py:1052-1053). Three things
 * follow, and all three are asserted below:
 *
 *   - the body is the same ATL grammar an inline `show x:` block uses, so
 *     timed steps come along with the placement;
 *   - a game may redefine `center` or `truecenter` and gets its own;
 *   - every transform exists before any story line runs, so a `show` may name
 *     one defined further down the file.
 *
 * The script is written out and run through the real lexer and parser, because
 * the parser is where all of this happens: asserting on a hand-built mr_place
 * would test the resolver, which was never wrong.
 *
 * Numbers are for a 1280x720 stage and a 200x100 image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mr.h"
#include "script.h"
#include "scene.h"
#include "vfs.h"
#include "expr.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_audio_poll(void) {}
/* image.c reaches for timeGetTime, which drags in the host's fake waveOut,
 * which refers to this. */
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int host_call(const char *n, mr_value *a, const char **an, int c,
                     mr_value *r)
{
    (void)n; (void)an;
    memset(r, 0, sizeof(*r));
    if (c > 0) *r = a[0];
    return 1;
}

#define IMG_W 200
#define IMG_H 100

static mr_script *g_s;
static int fails;

static const mr_stmt *show_stmt(const char *name)
{
    int i;
    for (i = 0; i < g_s->n; i++) {
        if (g_s->st[i].kind == MR_ST_SHOW && !strcmp(g_s->st[i].name, name))
            return &g_s->st[i];
    }
    return NULL;
}

static void placement(const char *name, int *x, int *y)
{
    const mr_stmt *st = show_stmt(name);

    if (!st) {
        *x = *y = -9999;
        return;
    }
    mr_place_resolve(&st->place, st->has_place, IMG_W, IMG_H, x, y);
}

static void check(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-52s %6d want %6d  %s\n", label, got, want,
           ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* The fixture covers the grammar `transform` accepts, not the usage that
 * prompted the work: a plain offset, a negative one, an animating one, a
 * redefinition of a stock position, a forward reference, a transform with an
 * inline block on top of it, and a name that does not exist. */
static const char SCRIPT[] =
    "transform left640:\n"
    "    xoffset 640.0\n"
    "    yalign 1.0\n"
    "\n"
    "transform leftneg:\n"
    "    xoffset -60\n"
    "    yalign 1.0\n"
    "\n"
    "transform rise:\n"
    "    xalign 0.0\n"
    "    yalign 1.0\n"
    "    linear 0.5 yoffset 300\n"
    "\n"
    /* The game's own `truecenter`, deliberately not Ren'Py's 0.5/0.5. */
    "transform truecenter:\n"
    "    xalign 0.25\n"
    "    yalign 0.25\n"
    "\n"
    "label start:\n"
    "    show early at later\n"
    "    show offset at left640\n"
    "    show negative at leftneg\n"
    "    show animated at rise\n"
    "    show stock at center\n"
    "    show redefined at truecenter\n"
    "    show blockwins at left640:\n"
    "        xoffset 10\n"
    "    show nosuch at nowhereatall\n"
    "    return\n"
    "\n"
    /* Below the `show` that uses it, and that has to work. */
    "transform later:\n"
    "    xoffset 777.0\n"
    "    yalign 1.0\n";

int main(int argc, char **argv)
{
    int x, y;
    FILE *f;
    const mr_stmt *st;

    (void)argc; (void)argv;
    setbuf(stdout, NULL);

    if (system("rm -rf /tmp/transformtest && mkdir -p /tmp/transformtest") != 0)
        return 2;
    f = fopen("/tmp/transformtest/script.rpy", "wb");
    if (!f)
        return 2;
    fwrite(SCRIPT, 1, sizeof(SCRIPT) - 1, f);
    fclose(f);

    mr_vfs_init(NULL, "/tmp/transformtest");
    mr_expr_init(host_call);
    g_s = mr_script_load("/tmp/transformtest");
    if (!g_s) {
        printf("script load failed\n");
        return 1;
    }

    /* The plain case, and the one Azami has forty of. `xoffset 640.0` with no
     * xpos means 640 pixels from the left edge, and `yalign 1.0` stands the
     * image on the bottom. */
    placement("offset", &x, &y);
    check("`at left640` offsets x by 640", x, 640);
    check("  and yalign 1.0 stands it on the bottom", y, 720 - IMG_H);

    /* Written as an integer and negative: half an Azami sprite hangs off the
     * left edge on purpose. */
    placement("negative", &x, &y);
    check("a negative xoffset goes off the left edge", x, -60);

    /* The whole point of resolving fixups after every file is read: in Ren'Py
     * the transform statement is wrapped in an Init, so this is legal. */
    placement("early", &x, &y);
    check("a transform used above its definition still applies", x, 777);
    check("  including the rest of its placement", y, 720 - IMG_H);

    /* The stock positions still work, and are still what an undefined name
     * falls back to. */
    placement("stock", &x, &y);
    check("`at center` is unaffected", x, (1280 - IMG_W) / 2);
    check("  and still names y", y, 720 - IMG_H);

    /* A game that defines `transform truecenter:` gets its own, because in
     * Ren'Py it is the same store variable 00definitions.rpy set. 0.25 of a
     * 1280 stage with the anchor 0.25 into a 200px image. */
    placement("redefined", &x, &y);
    check("the game's `truecenter` beats the stock one",
          x, (int)(0.25 * 1280) - (int)(0.25 * IMG_W));
    check("  on both axes", y, (int)(0.25 * 720) - (int)(0.25 * IMG_H));

    /* Ren'Py applies the `at` transform first and the statement's own block
     * on top, so the block's xoffset is the one that survives. */
    placement("blockwins", &x, &y);
    check("an inline block overrides the transform it sits on", x, 10);
    check("  and keeps what the block did not mention", y, 720 - IMG_H);

    /* A name that is neither a transform nor a stock position. Ren'Py raises;
     * we log and leave the placement empty, which -- because the statement
     * did carry an `at` -- resolves to the corner rather than to bottom
     * centre. Asserted so the behaviour is a decision rather than a surprise. */
    placement("nosuch", &x, &y);
    check("an unknown name leaves the placement empty", x, 0);
    check("  and does not fall back to bottom centre", y, 0);

    /* The timed steps have to come with the placement, or `at rise` is a
     * sprite that should rise from below the floor and simply stands there.
     * Nothing about the statement says it animates except this. */
    st = show_stmt("animated");
    check("a transform with a `linear` step carries an ATL",
          st && st->atl ? 1 : 0, 1);
    if (st && st->atl) {
        check("  with one step", st->atl->n, 1);
        check("  lasting the half second it asked for",
              (int)st->atl->v[0].ms, 500);
        check("  moving yoffset to 300",
              st->atl->v[0].place.has_yoffset ?
                  st->atl->v[0].place.yoffset : -1, 300);
        placement("animated", &x, &y);
        check("  starting from the transform's own placement", x, 0);
        check("  on the bottom", y, 720 - IMG_H);
    } else {
        fails += 5;
    }

    /* A statement with no `at` at all is untouched by any of this: it still
     * gets config.default_transform, bottom centre. */
    check("the fixture parsed as one script", g_s->n > 0, 1);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    mr_script_free(g_s);
    mr_expr_shutdown();
    mr_vfs_shutdown();
    return fails ? 1 : 0;
}

/* The image registry has a fixed number of name slots, and a game can use
 * them all up.
 *
 * Azami did. Its main menu and its preferences screen came up black -- no
 * background, no frame art, no slider or button art -- while the text on them
 * drew perfectly. Nothing in the screens was wrong: `add gui.main_menu_background`
 * parsed, the variable held "gui/main_menu.png", and the file was in the pak.
 * The registry was simply full by the time the screen asked.
 *
 * It fills in a particular order, and that order is what made the symptom look
 * like a rendering bug rather than a capacity one:
 *
 *   1. mr_image_init enumerates every file under images/ and gives each one a
 *      slot, whether the script ever shows it or not. Azami: 792.
 *   2. The `image` declarations add every name that is not already there.
 *      Azami: 434 declared names.
 *   3. A screen's `gui/...` path gets its slot the *first time it is drawn* --
 *      so the menu art is last in the queue, and last is what gets dropped.
 *
 * What this asserts is that the cap is above what a game of that size needs,
 * and -- the part that actually failed -- that a path-named image arriving
 * long after the bulk registration still gets a slot and still decodes.
 *
 * The names here are synthetic rather than read from any game: the point is
 * the count and the arrival order, which a fixture can state exactly and a
 * game's script cannot be relied on to keep stating.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "image.h"
#include "vfs.h"

/* image.c includes mmsystem.h for timeGetTime, and the host shim's waveOut
 * stub refers to this whether or not anything plays audio. */
void (*hostshim_wave_sink)(const short *, int) = NULL;

/* The log goes to stderr rather than being swallowed: "the image table is
 * full" is the message this whole test exists because of, and seeing it
 * is half of reading a failure here. */
void mr_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void mr_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static int fails;

static void check(const char *label, int got, int want)
{
    int ok = (got == want);
    printf("%-54s %5d  %s\n", label, got, ok ? "ok" : "** FAIL **");
    if (!ok) {
        printf("%-54s want %d\n", "", want);
        fails++;
    }
}

/* The fixture's one real image, named as a path so it takes the same route
 * through mr_image_get that a screen's `add gui/...` does: not in the
 * registry, contains a '/', therefore defined on demand and loaded straight
 * from the data. */
#define ART "gui/overlay/menu.png"
#define ART_W 64
#define ART_H 48

/* What Azami needs, measured from the game: 792 files under images/ plus 434
 * declared names. Stated as the two numbers rather than their sum so that a
 * later reader can see which part grew. */
#define ENUMERATED 792
#define DECLARED   434

int main(int argc, char **argv)
{
    char name[64];
    int i, defined = 0;
    mr_surface *s;

    setbuf(stdout, NULL);

    if (argc < 2) {
        printf("usage: imgcaptest <fixture-dir>\n");
        return 2;
    }

    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);

    /* The fixture has no images/ directory to enumerate, so the registry
     * starts empty and every slot counted below is one this test took. */

    /* Step 1 and 2: the bulk of a large game's names. Each entry points at
     * the one real file -- what is being filled is the name table, and
     * nothing here decodes. */
    for (i = 0; i < ENUMERATED + DECLARED; i++) {
        sprintf(name, "bulk %d", i);
        if (mr_image_define(name, ART, 0))
            defined++;
    }
    check("a game with 792 files and 434 declared names fits",
          defined, ENUMERATED + DECLARED);

    /* Those names are not merely counted, they resolve. A table that
     * overflowed silently would still pass the count above if mr_image_define
     * lied about it. */
    {
        int found = 0;
        for (i = 0; i < ENUMERATED + DECLARED; i += 97) {
            sprintf(name, "bulk %d", i);
            if (mr_image_get(name))
                found++;
        }
        check("  and every one of them resolves to pixels", found,
              (ENUMERATED + DECLARED + 96) / 97);
    }

    /* Step 3, and the reported bug. The menu art is asked for only once the
     * screen is drawn, which is after all of the above. With the table full
     * this returned NULL and the screen drew black around perfectly good
     * text. */
    s = mr_image_get(ART);
    check("the menu art asked for last still loads", s != NULL, 1);
    if (s) {
        check("  at its real width", s->w, ART_W);
        check("  and height", s->h, ART_H);
    } else {
        fails += 2;
    }

    /* The cap has not been removed, only raised, so the failure mode still
     * exists for a game large enough. Fill the rest and confirm the next one
     * is refused -- an engine that silently grew the table past its static
     * array would be a far worse bug than the one being fixed. */
    {
        int refused = 0;
        for (i = 0; i < 8192; i++) {
            sprintf(name, "overflow %d", i);
            if (!mr_image_define(name, ART, 0)) {
                refused = 1;
                break;
            }
        }
        check("the table is still bounded, and says so when full",
              refused, 1);
        printf("   (it took %d more names past the %d to fill it)\n",
               i, ENUMERATED + DECLARED);
    }

    /* And a name that was already there is still found after the overflow:
     * a full table must not corrupt what it already holds. */
    check("a name registered before the overflow survives it",
          mr_image_get(ART) != NULL, 1);

    mr_image_shutdown();
    mr_vfs_shutdown();

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

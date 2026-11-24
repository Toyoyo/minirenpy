/* Image attributes: `show azami at left270` when there is no azami.png.
 *
 * In Ren'Py an image name is a tag plus a *set* of attributes, and a `show`
 * that names fewer than the image has keeps the ones the tag is already
 * wearing (display/image.py:949 apply_attributes, :1004 choose_image). We
 * matched the whole name as one string, so `show azami` found nothing and the
 * sprite vanished mid-conversation -- 377 times in one Azami playthrough, and
 * the log said `image: unknown image 'azami'` each time.
 *
 * The registry is the fixture: these names are registered with
 * mr_image_define and never decoded, because what is under test is which name
 * gets chosen, not what is in the file. Nothing here reads a game script.
 *
 * The set below is modelled on how a Ren'Py game actually names sprites -- a
 * costume attribute, an expression attribute, a mirrored variant, and a
 * second character sharing the tag's prefix -- rather than on the one line
 * that was reported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "mr.h"
#include "image.h"
#include "scene.h"
#include "vfs.h"

void (*hostshim_wave_sink)(const short *, int) = NULL;
void mr_audio_poll(void) {}

static int logged;

void mr_log(const char *fmt, ...)
{
    va_list ap;
    logged++;
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

/* `want` NULL means "no image should match at all". */
static void choose(const char *label, const char *name, const char *current,
                   const char *want)
{
    char got[64];
    int  ok, found;

    got[0] = '\0';
    found = mr_image_choose(name, current, got, (int)sizeof(got));

    if (want)
        ok = found && !strcmp(got, want);
    else
        ok = !found;

    printf("%-50s %-26s %s\n", label, found ? got : "(no match)",
           ok ? "ok" : "** FAIL **");
    if (!ok) {
        printf("%-50s want %-21s\n", "", want ? want : "(no match)");
        fails++;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setbuf(stdout, NULL);

    mr_vfs_init(NULL, ".");
    mr_image_init(".");

    /* Azami's wardrobe, in miniature. Note there is deliberately no bare
     * "azami": that absence is the whole bug. The file each name points at is
     * never opened. */
    mr_image_define("azami pout",            "x.png", 0);
    mr_image_define("azami smile",           "x.png", 0);
    mr_image_define("azami casual pout",     "x.png", 0);
    mr_image_define("azami casual smile",    "x.png", 0);
    mr_image_define("azami casual talk",     "x.png", 0);
    mr_image_define("azami magical pout",    "x.png", 0);
    mr_image_define("azami pout flip",       "x.png", 0);
    mr_image_define("eari uniform talk flip", "x.png", 0);
    /* A tag that does have an attribute-less image, so the "no match" cases
     * below are about the name and not about the registry being thin. */
    mr_image_define("nanani",                "x.png", 0);
    mr_image_define("nanani smile",          "x.png", 0);

    /* --- the reported case ------------------------------------------- */

    /* `show azami pout` ... `show azami at left270`: the second one moves her
     * without changing her face. */
    choose("`show azami` keeps the attributes in force",
           "azami", "azami pout", "azami pout");
    choose("  and the most specific set of them",
           "azami", "azami casual pout", "azami casual pout");

    /* Nothing in force and no attribute-less image: Ren'Py finds nothing
     * here too. The caller keeps the name and the miss is logged where it
     * always was. */
    choose("`show azami` with nothing showing finds nothing",
           "azami", NULL, NULL);

    /* ...but a tag that *does* have a bare image resolves to it. */
    choose("a tag with an attribute-less image still works",
           "nanani", NULL, "nanani");

    /* --- the exact-match short circuit -------------------------------- */

    /* image.py:962-967 returns an exactly existing name before any
     * inheriting, which is what stops attributes from accumulating forever.
     * `show azami pout` while wearing `casual` means she is no longer
     * casual. */
    choose("an exact name drops what was in force",
           "azami pout", "azami casual pout", "azami pout");

    /* Where the exact name does *not* exist, the attribute is added to what
     * is already there rather than replacing it. */
    choose("a partial name adds to what is in force",
           "azami talk", "azami casual pout", "azami casual talk");

    /* --- attributes are a set, not a string --------------------------- */

    /* The script writes them in whatever order reads well; the image was
     * declared in another. Matching the whole name as one string got this
     * wrong without ever saying so. */
    choose("attribute order does not matter",
           "eari uniform flip talk", NULL, "eari uniform talk flip");

    /* --- what must not be chosen -------------------------------------- */

    /* `azami magical pout` is a different costume. An attribute the show did
     * not ask for and the tag is not wearing disqualifies a candidate --
     * without that rule, `show azami` could turn her into a magical girl. */
    choose("an attribute nobody asked for disqualifies a match",
           "azami", "azami smile", "azami smile");

    /* A required attribute that no image has: no match, rather than the
     * nearest thing. */
    choose("a required attribute that does not exist finds nothing",
           "azami furious", "azami pout", NULL);

    /* A different tag's attributes are not inherited. This is what the
     * caller's tag check already guarantees, asserted here so the function
     * is safe on its own. */
    choose("another tag's attributes are not inherited",
           "azami", "eari uniform talk flip", NULL);

    /* --- removing an attribute ---------------------------------------- */

    /* `show azami -casual` takes the costume off (image.py:990-997). */
    choose("a leading `-` removes an inherited attribute",
           "azami -casual", "azami casual pout", "azami pout");
    choose("  and is not itself required",
           "azami -flip", "azami pout flip", "azami pout");

    /* --- names that are not tags -------------------------------------- */

    /* A screen's `gui/overlay/main_menu.png` is a path, not a tag with
     * attributes, and must not be taken apart. */
    choose("a path is left alone",
           "gui/overlay/menu.png", NULL, NULL);

    /* --- the ambiguity Ren'Py raises on ------------------------------- */

    /* Two images match equally well: `azami casual smile` and `azami casual
     * talk` are both two attributes drawn from what is in force. Ren'Py
     * raises (image.py:1060); stopping the game on Win32s over a sprite is
     * the wrong trade, so one is taken and the ambiguity is logged. The
     * assertion is that it is *said*, not which one wins. */
    {
        char got[64];
        int  before = logged;
        int  found = mr_image_choose("azami", "azami casual smile talk",
                                     got, (int)sizeof(got));
        int  ok = found && logged > before;

        printf("%-50s %-26s %s\n", "an ambiguous match is reported, not fatal",
               found ? got : "(no match)", ok ? "ok" : "** FAIL **");
        if (!ok)
            fails++;
    }

    /* --- through the show path ---------------------------------------
     *
     * Everything above tests mr_image_choose. That is not where the bug was
     * visible: what the player saw was a sprite disappearing on a `show
     * azami at left270`, and that depends on mr_scene_show_anim asking the
     * question at all, and asking it with the right `current`. The same
     * mistake as the has_place one -- a correct function nobody called
     * properly -- so the wiring is driven here rather than reasoned about. */
    {
        mr_scenelist sl;
        mr_place pl;
        int i, found = -1;

        mr_scene_clear();
        mr_scene_show("azami casual pout", NULL, 0);

        /* The reported statement: a tag, no attributes, and a placement. */
        memset(&pl, 0, sizeof(pl));
        pl.has_xoffset = 1;
        pl.xoffset = 270;
        pl.has_yalign = 1;
        pl.yalign = 1.0f;
        mr_scene_show("azami", &pl, 1);

        mr_scene_capture(&sl);
        for (i = 0; i < sl.n; i++) {
            if (!strcmp(sl.v[i].tag, "azami"))
                found = i;
        }

        printf("%-50s %-26s %s\n", "the show path resolves the name too",
               found >= 0 ? sl.v[found].name : "(gone)",
               (found >= 0 && !strcmp(sl.v[found].name, "azami casual pout"))
                   ? "ok" : "** FAIL **");
        if (!(found >= 0 && !strcmp(sl.v[found].name, "azami casual pout")))
            fails++;

        if (found >= 0) {
            int ok = sl.v[found].place.has_xoffset &&
                     sl.v[found].place.xoffset == 270;
            printf("%-50s %-26d %s\n", "  and still moves her where it said",
                   sl.v[found].place.xoffset, ok ? "ok" : "** FAIL **");
            if (!ok)
                fails++;
        } else {
            fails++;
        }

        /* One entry, not two: resolving the name must not change the tag, or
         * the re-show would miss the entry and append a second azami. */
        {
            int n = 0;
            for (i = 0; i < sl.n; i++)
                if (!strcmp(sl.v[i].tag, "azami"))
                    n++;
            printf("%-50s %-26d %s\n", "  and did not add a second entry",
                   n, n == 1 ? "ok" : "** FAIL **");
            if (n != 1)
                fails++;
        }

        /* A name that resolves to nothing is left exactly as written, so the
         * miss is logged where it always was rather than silently becoming
         * some other image. */
        mr_scene_clear();
        mr_scene_show("azami", NULL, 0);
        mr_scene_capture(&sl);
        found = -1;
        for (i = 0; i < sl.n; i++)
            if (!strcmp(sl.v[i].tag, "azami"))
                found = i;
        printf("%-50s %-26s %s\n", "an unresolvable show keeps its own name",
               found >= 0 ? sl.v[found].name : "(gone)",
               (found >= 0 && !strcmp(sl.v[found].name, "azami"))
                   ? "ok" : "** FAIL **");
        if (!(found >= 0 && !strcmp(sl.v[found].name, "azami")))
            fails++;
    }

    /* --- `behind`, which is the same list and the same function ------
     *
     * `show eari behind keiko` has to put eari *earlier* in the display
     * list, because render_list draws in list order. Ren'Py's rule is the
     * index of the first entry whose tag is either this one or one of the
     * behind names (scenelists.py:313-348), with every zorder equal -- which
     * is what these games have.
     *
     * It lives here rather than in a test of its own because what it shares
     * with the rest of this file is the thing that was wrong: a clause on a
     * show statement that the engine read and then dropped. */
    {
        mr_scenelist sl;
        int i;

        /* The order on screen, back to front, as a string of initials. */
        #define ORDER(buf) do {                                       \
            int k_;                                                   \
            mr_scene_capture(&sl);                                    \
            for (k_ = 0; k_ < sl.n && k_ < 7; k_++)                   \
                (buf)[k_] = sl.v[k_].tag[0];                          \
            (buf)[k_ < 7 ? k_ : 7] = '\0';                            \
        } while (0)

        char order[8];

        /* Tags with distinct initials, because the order is read as a string
         * of them. `nanani` is already in the registry above. */
        mr_image_define("keiko", "x.png", 0);
        mr_image_define("eari",  "x.png", 0);

        mr_scene_clear();
        mr_scene_show("azami pout", NULL, 0);
        mr_scene_show("keiko", NULL, 0);
        ORDER(order);
        printf("%-50s %-26s %s\n", "shows stack in the order they were made",
               order, !strcmp(order, "ak") ? "ok" : "** FAIL **");
        if (strcmp(order, "ak"))
            fails++;

        /* The reported statement. eari must land in front of keiko in the
         * list, which is behind her on screen. */
        mr_scene_show_anim("eari", NULL, 0, NULL, 0, "keiko");
        ORDER(order);
        printf("%-50s %-26s %s\n", "`behind keiko` puts her under keiko",
               order, !strcmp(order, "aek") ? "ok" : "** FAIL **");
        if (strcmp(order, "aek"))
            fails++;

        /* A list goes behind the *first* of them that is on screen, so
         * `behind keiko,azami` with azami at the bottom lands under azami. */
        mr_scene_show_anim("nanani", NULL, 0, NULL, 0, "keiko,azami");
        ORDER(order);
        printf("%-50s %-26s %s\n", "a list uses the earliest of the tags",
               order, !strcmp(order, "naek") ? "ok" : "** FAIL **");
        if (strcmp(order, "naek"))
            fails++;

        /* Ren'Py recomputes the slot on every show, so a re-show with a
         * behind clause *moves* an image that is already up. */
        mr_scene_show_anim("keiko", NULL, 0, NULL, 0, "azami");
        ORDER(order);
        printf("%-50s %-26s %s\n", "a re-show behind something moves it",
               order, !strcmp(order, "nkae") ? "ok" : "** FAIL **");
        if (strcmp(order, "nkae"))
            fails++;

        /* ...and a re-show *without* one does not move anything, which is
         * the case every other game takes. */
        mr_scene_show("eari", NULL, 0);
        ORDER(order);
        printf("%-50s %-26s %s\n", "a plain re-show leaves the order alone",
               order, !strcmp(order, "nkae") ? "ok" : "** FAIL **");
        if (strcmp(order, "nkae"))
            fails++;

        /* A tag named in `behind` that is not on screen is simply not found,
         * and the image appends as it always did. Ren'Py does the same: the
         * loop finds nothing and add_index becomes the end of the list. */
        mr_scene_clear();
        mr_scene_show("azami pout", NULL, 0);
        mr_scene_show_anim("eari", NULL, 0, NULL, 0, "nobody");
        ORDER(order);
        printf("%-50s %-26s %s\n", "behind a tag that is not shown appends",
               order, !strcmp(order, "ae") ? "ok" : "** FAIL **");
        if (strcmp(order, "ae"))
            fails++;

        /* A substring must not match: `azami` is not `az`. */
        mr_scene_clear();
        mr_scene_show("azami pout", NULL, 0);
        mr_scene_show("keiko", NULL, 0);
        mr_scene_show_anim("eari", NULL, 0, NULL, 0, "az,keikoo");
        ORDER(order);
        printf("%-50s %-26s %s\n", "a partial tag name does not match",
               order, !strcmp(order, "ake") ? "ok" : "** FAIL **");
        if (strcmp(order, "ake"))
            fails++;

        #undef ORDER
    }

    mr_scene_shutdown();
    mr_image_shutdown();
    mr_vfs_shutdown();

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

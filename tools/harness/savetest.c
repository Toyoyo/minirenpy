#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "save.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"

static mr_surface *g_fake_stage;
mr_surface *mr_stage(void) { return g_fake_stage; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}

static int fails;

static void check(const char *label, int ok)
{
    printf("%-56s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* Visiting a third page on the save screen showed every slot empty.
 *
 * The screen asks FileTime, FileSaveName and FileScreenshot for all six slots
 * of the page every time it lays out, so save.c caches the headers. The cache
 * was fixed at twelve entries and, when full, `slot_lookup` returned NULL
 * instead of evicting anything -- and nothing but a save or a delete ever
 * cleared it. Six slots a page: the first page filled half, the second filled
 * it, and every page after that reported no name, no date and no thumbnail
 * for every slot.
 *
 * It looked like a multi-game problem because that is where it was noticed,
 * with Miwa saves on one page and Hinata's on another. It is not: one game
 * and three pages is enough, which is what this does.
 *
 * FileLoadable kept working throughout, because mr_save_exists reads the file
 * rather than the cache -- so the slots stayed clickable while showing
 * nothing, which is a stranger symptom than a plainly empty page.
 */
static int more_slots_than_the_cache_holds(void)
{
    static const char *pages[] = { "1", "quick", "2" };
    char slot[MR_SLOT_NAME];
    int p, i, blank = 0;

    printf("\n-- more slots than the header cache holds --\n");

    for (p = 0; p < 3; p++) {
        for (i = 1; i <= 6; i++) {
            sprintf(slot, "%s-%d", pages[p], i);
            mr_save_write(slot, g_fake_stage);
        }
    }

    /* Read them in the order the player would meet them: all of one page,
     * then all of the next. A write flushes the cache, so this walk is what
     * fills it, exactly as laying the screen out three times would. */
    for (p = 0; p < 3; p++) {
        int page_blank = 0;

        for (i = 1; i <= 6; i++) {
            sprintf(slot, "%s-%d", pages[p], i);
            if (!mr_save_time(slot) || !mr_save_name(slot))
                page_blank++;
        }
        printf("   page %-5s: %d of 6 slots have no header\n",
               pages[p], page_blank);
        blank += page_blank;
    }

    check("every written slot reports its header, on every page", blank == 0);

    /* And the thumbnails, which are the other thing the cache holds and the
     * part the player actually sees as "empty". */
    blank = 0;
    for (p = 0; p < 3; p++) {
        for (i = 1; i <= 6; i++) {
            sprintf(slot, "%s-%d", pages[p], i);
            if (!mr_save_thumbnail(slot))
                blank++;
        }
    }
    printf("   %d of 18 slots have no thumbnail\n", blank);
    check("and its thumbnail", blank == 0);

    /* Going back to the first page must still work: an eviction policy that
     * only ever keeps the newest page would pass everything above. */
    for (i = 1, blank = 0; i <= 6; i++) {
        sprintf(slot, "1-%d", i);
        if (!mr_save_time("1-1") || !mr_save_thumbnail(slot))
            blank++;
    }
    check("and going back to the first page still shows them", blank == 0);

    for (p = 0; p < 3; p++) {
        for (i = 1; i <= 6; i++) {
            sprintf(slot, "%s-%d", pages[p], i);
            mr_save_delete(slot);
        }
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all ok",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    mr_script *s;
    mr_value v;
    int x, y;

    setbuf(stdout, NULL);
    mr_vfs_init(NULL, argv[1]);
    mr_image_init(argv[1]);
    s = mr_script_load(argv[1]);
    if (!s) { printf("no script\n"); return 1; }
    mr_interp_init(s, argv[1]);
    mr_interp_start("start");

    /* A stage with a recognisable gradient, to check the thumbnail. */
    g_fake_stage = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    for (y = 0; y < MR_STAGE_H; y++)
        for (x = 0; x < MR_STAGE_W; x++)
            g_fake_stage->px[(size_t)y * g_fake_stage->pitch + x] =
                MR_RGB(x * 255 / MR_STAGE_W, y * 255 / MR_STAGE_H, 128);

    /* Put some state in. */
    mr_value_set_str(&v, "Les cloches");
    mr_var_set("title", &v);
    mr_var_set_int("miwa_affection", 42);
    mr_value_set_str(&v, "english");
    mr_var_set("_preferences.language", &v);

    printf("exists before: %d\n", mr_save_exists("1-3"));
    printf("write: %d\n", mr_save_write("1-3", g_fake_stage));
    printf("exists after:  %d\n", mr_save_exists("1-3"));
    printf("name: '%s'  time: '%s'\n",
           mr_save_name("1-3"), mr_save_time("1-3"));

    /* Clobber the state, then load it back. */
    mr_var_set_int("miwa_affection", 999);
    mr_value_set_str(&v, "wrong");
    mr_var_set("title", &v);

    printf("load: %d\n", mr_save_load("1-3"));
    printf("affection restored: %ld (expect 42)\n",
           mr_var_get_int("miwa_affection", -1));
    mr_var_get("title", &v);
    printf("title restored: '%s' (expect Les cloches)\n", v.s);
    mr_var_get("_preferences.language", &v);
    printf("language restored: '%s' (expect english)\n", v.s);

    {
        mr_surface *t = mr_save_thumbnail("1-3");
        printf("thumbnail: %s", t ? "" : "MISSING\n");
        if (t) {
            mr_u32 p = t->px[(size_t)(t->h/2) * t->pitch + t->w/2];
            printf("%dx%d centre=%02x%02x%02x (expect ~7f7f80)\n",
                   t->w, t->h, MR_R(p), MR_G(p), MR_B(p));
        }
    }

    mr_save_delete("1-3");
    printf("exists after delete: %d\n", mr_save_exists("1-3"));

    return more_slots_than_the_cache_holds();
}

/* The host waveOut shim needs a sink; this test plays nothing. */
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

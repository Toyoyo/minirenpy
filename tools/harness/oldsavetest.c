/* Saves written by older versions still load.
 *
 * The variable records changed twice in quick succession. Version 5 wrote
 * each string as a fixed 128-byte field; version 6 widened that to 256 so a
 * value could hold a whole sentence (the French confirm prompt is 128 bytes
 * and was losing its last character); version 7 length-prefixes instead, so
 * that the cap -- now 1024 -- stops driving a save's size.
 *
 * Throwing away a player's saves over a field's encoding would not be
 * acceptable, so 5 and 6 are still read. This checks that, against *real*
 * files written by the engine as it stood at each version, kept in
 * tools/harness/fixtures. A synthesised fixture would only prove the reader
 * agrees with the test's idea of the old layout, which is the thing in doubt.
 *
 * Takes the pak and the game directory: the save names its execution point by
 * statement id, so the script it was taken from has to be loadable for the
 * load to mean anything.
 *
 * Runs on the fixture pak. The fixtures are real Miwa saves and stay that
 * way -- a synthesised old save proves nothing -- but what they *name* is
 * now in the fixture: `tools/harness/fixgame/script_game.rpy` carries the
 * twenty statements these three files point at, copied verbatim from Miwa
 * because the id is an md5 of the line. Before that this test needed
 * Miwa's own pak, and against any other game all three failed "it loads"
 * while the variable section read fine -- the desynced-save guard doing
 * its job, which looks exactly like a reader regression and cost an
 * afternoon.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "save.h"
#include "expr.h"
#include "interp.h"
#include "persist.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "slrender.h"
#include "ui.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); }
/* Printed, unlike in most harnesses, because every refusal in save.c says why
 * through this and nowhere else -- and the reason is the one thing worth
 * knowing when this test fails. With it swallowed there was no way to tell a
 * statement id that no longer resolves (the wrong pak, see the header) from a
 * bad call depth or a truncated file. Prefixed so the engine's lines are
 * obvious among the test's. */
void mr_log(const char *f, ...)
{
    va_list ap;

    printf("  [log] ");
    va_start(ap, f);
    vprintf(f, ap);
    va_end(ap);
    printf("\n");
}
void mr_quit(void) {}
void mr_invalidate(void) {}
void hostshim_wave_sink(const short *d, int n) { (void)d; (void)n; }

static int fails;

static void check(const char *label, int ok)
{
    printf("%-52s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

/* The version field is the second 32-bit word of the file. */
static long file_version(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char b[8];
    long v;

    if (!f)
        return -1;
    if (fread(b, 1, 8, f) != 8) { fclose(f); return -1; }
    fclose(f);
    v = (long)b[4] | ((long)b[5] << 8) | ((long)b[6] << 16) |
        ((long)b[7] << 24);
    return v;
}

static int copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[4096];
    size_t n;

    if (!a)
        return 0;
    b = fopen(to, "wb");
    if (!b) { fclose(a); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
        fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 1;
}

/* Everything that has to hold for one old save. */
static void check_fixture(const char *path, long want_version)
{
    char label[128];

    printf("\n-- %s --\n", path);

    remove("SAVEP1S1.SAV");          /* slot 1-1 */
    if (!copy(path, "SAVEP1S1.SAV")) {
        printf("cannot read fixture %s\n", path);
        fails++;
        return;
    }

    /* It really is the old format. Without this the test could be about the
     * current writer reading its own output, which proves nothing about
     * migration -- and that is easy to end up with, because the fixtures are
     * captured from a working tree that moves on. */
    sprintf(label, "the fixture is a version %ld save", want_version);
    check(label, file_version("SAVEP1S1.SAV") == want_version);

    /* The header path, which the save screen uses to decide a slot is
     * occupied. It has its own version gate. */
    mr_save_flush();
    check("an old slot still shows as occupied", mr_save_exists("1-1"));
    check("  with its timestamp", mr_save_time("1-1") != NULL);

    /* And the full load, which is where the variable records live. */
    mr_interp_main_menu();
    if (!mr_save_load("1-1")) {
        check("it loads", 0);
        /* The one explanation that is not a bug in the reader, and the one
         * that has actually happened. See the header. */
        printf("      if the [log] line above says the script no longer has "
               "the statement\n"
               "      id, this is the desynced-save guard, not the reader: "
               "the lines these\n"
               "      fixtures name live in fixgame/script_game.rpy and are "
               "verbatim, so an\n"
               "      edit to one of them -- or the wrong pak -- is enough. "
               "Rebuild\n"
               "      fixture.pak (tools/harness/mkfixpak.sh).\n");
    } else {
        check("it loads", 1);
    }
    check("  and left the main menu", mr_var_get_int("main_menu", 1) == 0);

    /* The variable section, the part whose encoding changed.
     *
     * Counting the variables proves nothing -- the baseline already has
     * about as many, so the count barely moves whether the section was read
     * correctly, read wrongly, or not read at all. What a mismatch actually
     * does is desynchronise the stream: every record after the first string
     * is read from the wrong offset, so the *names* come back as fragments
     * of the previous record's bytes. That is what to look for. */
    {
        const char *name;
        const mr_value *v;
        int i, strings = 0, longest = 0, junk = 0;

        for (i = 0; mr_var_enumerate(i, &name, &v); i++) {
            const unsigned char *p = (const unsigned char *)name;

            if (!*p) {
                junk++;
                continue;
            }
            for (; *p; p++) {
                /* A variable name is ASCII: letters, digits, '_' and the
                 * '.' of a dotted name. Anything else is misread bytes. */
                if (*p < 0x20 || *p > 0x7E) { junk++; break; }
            }
            if (v->type == MR_V_STR && v->s[0]) {
                strings++;
                if ((int)strlen(v->s) > longest)
                    longest = (int)strlen(v->s);
            }
        }
        printf("   %d variables, %d with strings, longest %d bytes\n",
               i, strings, longest);
        check("  every variable name came back intact", junk == 0);
        check("  and strings came with them", strings > 0);
    }

    remove("SAVEP1S1.SAV");
}

int main(int argc, char **argv)
{
    mr_script *s;
    const char *game;

    setbuf(stdout, NULL);
    if (argc < 6) {
        fprintf(stderr,
                "usage: oldsavetest <pak> <game-dir> <v5-fixture> "
                "<v6-fixture> <v7-fixture>\n");
        return 2;
    }
    game = argv[2];

    mr_vfs_init(argv[1], game);
    mr_image_init(game);
    mr_scene_init();
    s = mr_script_load(game);
    mr_interp_init(s, game);
    mr_ui_init(game);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    mr_interp_start("start");

    check_fixture(argv[3], 5);
    check_fixture(argv[4], 6);
    check_fixture(argv[5], 7);

    /* And the current writer's own output, so that whatever version is
     * current is covered by the same checks rather than only by the rest of
     * the suite. */
    {
        mr_interp_main_menu();
        mr_save_write("1-2", g_st);
        mr_save_flush();
        printf("\n-- freshly written --\n");
        /* >= 7 rather than an exact number: MR_SAVE_VERSION is private to
         * save.c, and a test that has to be edited on every bump gets
         * edited carelessly. 7 is where length-prefixed strings start. */
        check("a new save is at least the length-prefixed version",
              file_version("SAVEP1S2.SAV") >= 7);
        check("  and loads", mr_save_load("1-2"));
        mr_save_delete("1-2");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

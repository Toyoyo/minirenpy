/* A save must survive an edit to the scripts.
 *
 * The bug this covers: a save stored the execution point as a raw statement
 * index, so anything that changed the number of statements before the save
 * point -- editing a .rpy, or the parser change that made top-level python
 * lines emit MR_ST_INIT -- silently moved where an old slot resumed. The
 * symptom was peculiar and easy to misread: the line of dialogue came back
 * *correctly*, because a save stores `what` verbatim, while the line after it
 * and the rollback history belonged to somewhere else entirely.
 *
 * So the check is not "does the save load" -- it always did. It is "does the
 * story continue from the right place", which means asserting on the *next*
 * line and on the *previous* one, not on the visible one.
 *
 * The two script variants are written here rather than kept as fixture
 * directories so the edit between them is legible in one place, and so the
 * test can be sure the edit really shifts indices -- which it asserts, since
 * an edit that happened not to shift anything would make everything below
 * pass without testing a thing.
 *
 * Takes nothing; it builds what it reads.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "save.h"
#include "history.h"
#include "stmtid.h"

mr_surface *mr_stage(void) { return NULL; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int g_fail;

static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-52s %-8ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
}

static void check_str(const char *l, const char *got, const char *want)
{
    int ok = got && !strcmp(got, want);
    if (!ok) g_fail++;
    printf("%-52s %-28s %s\n", l, got ? got : "(null)",
           ok ? "ok" : "** WANT DIFFERENT **");
    if (!ok)
        printf("%-52s wanted: %s\n", "", want);
}

/* --- the two scripts -----------------------------------------------------
 *
 * Only the dialogue matters to the assertions. The rest is what the
 * interpreter needs in order to run at all.
 */

static const char *COMMON =
    "define n = Character(None)\n"
    "define a = Character(\"Ada\", color=\"#c8ffc8\")\n"
    "\n";

static const char *LINES =
    "    n \"Line one.\"\n"
    "    a \"Line two.\"\n"
    "    n \"Line three.\"\n"
    "    a \"Line four.\"\n"
    "    n \"Line five.\"\n"
    "    a \"Line six.\"\n"
    "    return\n";

/* `before` is the script the save is written against. */
static void write_before(const char *dir)
{
    char path[256];
    FILE *f;

    sprintf(path, "%s/script.rpy", dir);
    f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(1); }
    fputs(COMMON, f);
    fputs("label start:\n"
          "    $ config.rollback_enabled = True\n", f);
    fputs(LINES, f);
    fclose(f);
}

/* `after` is the same story, edited the way a script gets edited: new
 * declarations at the top, a whole new label before the one being played, and
 * new statements inside that label ahead of the save point. Every statement
 * index from `start` onwards moves. None of the *dialogue* changes, which is
 * what the translation identifiers key on. */
static void write_after(const char *dir)
{
    char path[256];
    FILE *f;

    sprintf(path, "%s/script.rpy", dir);
    f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(1); }
    fputs(COMMON, f);
    fputs("define inserted_decl = 1\n"
          "\n"
          "label prologue:\n"
          "    n \"A new line at the top of the file.\"\n"
          "    return\n"
          "\n", f);
    fputs("label start:\n"
          "    $ config.rollback_enabled = True\n"
          "    $ inserted_one = 1\n"
          "    $ inserted_two = 2\n", f);
    fputs(LINES, f);
    fclose(f);
}

/* `deleted` removes the line the save *resumes at*, which is the case that
 * cannot be migrated and must be refused.
 *
 * Note which line that is. A save does not resume at the line on screen --
 * mr_interp_capture records g_pc, which has already moved past the say that
 * displayed it -- so a slot showing "Line four." resumes at "Line five.".
 * Deleting "Line four." therefore does *not* invalidate it, and an earlier
 * version of this test asserted otherwise and failed for that reason rather
 * than for a fault in the engine. */
static void write_deleted(const char *dir)
{
    char path[256];
    FILE *f;

    sprintf(path, "%s/script.rpy", dir);
    f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(1); }
    fputs(COMMON, f);
    fputs("label start:\n"
          "    $ config.rollback_enabled = True\n"
          "    n \"Line one.\"\n"
          "    a \"Line two.\"\n"
          "    n \"Line three.\"\n"
          "    a \"Line four.\"\n"
          "    a \"Line six.\"\n"
          "    return\n", f);
    fclose(f);
}

/* `changed` rewords the exact line the save was taken on, leaving everything
 * else alone. This is the documented divergence from Ren'Py (see NOTES.md):
 * the resume point still resolves, so the story continues correctly, but the
 * *displayed* line comes back as the text that was saved rather than the text
 * that is now in the script. Pinned here so that behaviour is a decision
 * under test rather than something that drifts either way unnoticed. */
static void write_changed(const char *dir)
{
    char path[256];
    FILE *f;

    sprintf(path, "%s/script.rpy", dir);
    f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(1); }
    fputs(COMMON, f);
    fputs("label start:\n"
          "    $ config.rollback_enabled = True\n"
          "    n \"Line one.\"\n"
          "    a \"Line two.\"\n"
          "    n \"Line three.\"\n"
          "    a \"Line four, reworded.\"\n"
          "    n \"Line five.\"\n"
          "    a \"Line six.\"\n"
          "    return\n", f);
    fclose(f);
}

/* --- the case the label fallback does not cover ---------------------------
 *
 * Everything above happens to resume at a say, because a line of dialogue is
 * usually followed by another. When it is followed by something else there is
 * no translation identifier at the resume point, and naming it by its label
 * and offset breaks on any insertion earlier in that label -- which is
 * precisely the edit being tested. So these two variants put a `$` line
 * between the dialogue, and the save lands on it.
 */

static const char *PY_LINES =
    "    n \"Alpha.\"\n"
    "    $ marker = 1\n"
    "    a \"Beta.\"\n"
    "    $ marker = 2\n"
    "    n \"Gamma.\"\n"
    "    return\n";

static void write_py(const char *dir, int edited)
{
    char path[256];
    FILE *f;

    sprintf(path, "%s/script.rpy", dir);
    f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(1); }
    fputs(COMMON, f);
    if (edited)
        fputs("define another_decl = 7\n\n"
              "label extra_label:\n"
              "    n \"Yet another new line.\"\n"
              "    return\n\n", f);
    fputs("label start:\n"
          "    $ config.rollback_enabled = True\n", f);
    if (edited)
        fputs("    $ inserted_three = 3\n"
              "    $ inserted_four = 4\n", f);
    fputs(PY_LINES, f);
    fclose(f);
}

/* --- driving --------------------------------------------------------------
 *
 * Text is instant so a line is complete as soon as it appears; these tests
 * are about which line it is, not about the typewriter.
 */

static long g_T = 1000;

static mr_script *open_script(const char *dir)
{
    mr_script *s;

    mr_vfs_init(NULL, dir);
    s = mr_script_load(dir);
    if (!s) { printf("cannot parse %s\n", dir); exit(1); }
    mr_interp_init(s, dir);
    mr_var_set_int("_preferences.text_cps", 0);
    return s;
}

static void settle(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        mr_interp_update(g_T);
        g_T += 33;
    }
}

static const char *now_what(void)
{
    static mr_gamestate g;
    mr_interp_capture(&g);
    return g.what;
}

static long now_pc(void)
{
    mr_gamestate g;
    mr_interp_capture(&g);
    return g.pc;
}

/* Advance until the line on screen is `want`, or give up. */
static int advance_to(const char *want)
{
    int i;
    for (i = 0; i < 40; i++) {
        settle();
        if (!strcmp(now_what(), want))
            return 1;
        mr_interp_advance();
    }
    return 0;
}

int main(int argc, char **argv)
{
    char dir_before[256], dir_after[256], dir_deleted[256];
    const char *tmp = (argc > 1) ? argv[1] : "/tmp";
    long pc_before, pc_after;

    setbuf(stdout, NULL);

    sprintf(dir_before, "%s/mig_before", tmp);
    sprintf(dir_after, "%s/mig_after", tmp);
    sprintf(dir_deleted, "%s/mig_deleted", tmp);
    {
        char cmd[1024];
        sprintf(cmd, "mkdir -p %s %s %s", dir_before, dir_after, dir_deleted);
        if (system(cmd) != 0) { printf("mkdir failed\n"); return 1; }
    }
    write_before(dir_before);
    write_after(dir_after);
    write_deleted(dir_deleted);

    /* The save is written into the working directory, which is where the
     * engine puts slots. */
    if (chdir(tmp) != 0) { printf("cannot chdir %s\n", tmp); return 1; }
    remove("SAVEP1S1.SAV");

    /* --- play the original script up to a known line, and save --- */
    open_script(dir_before);
    mr_interp_start("start");
    check("reached the save point", advance_to("Line four."), 1);
    pc_before = now_pc();
    check("wrote the slot", mr_save_write("1-1", NULL), 1);

    /* --- now load it against the edited script --- */
    open_script(dir_after);

    /* If the edit did not move this statement, nothing below is a test. */
    {
        long resolved;
        mr_interp_start("start");
        advance_to("Line four.");
        resolved = now_pc();
        pc_after = resolved;
        check("the edit really moved the statement",
              pc_after != pc_before, 1);
    }

    open_script(dir_after);
    check("the old slot still loads", mr_save_load("1-1"), 1);
    settle();

    /* The visible line was always right, even with the bug -- a save stores
     * the text. Checked anyway, because a migration that resolved to the
     * wrong statement would break it too. */
    check_str("the line on screen", now_what(), "Line four.");
    check("resumed at the edited script's index", now_pc(), pc_after);

    /* This is the assertion the bug failed. */
    mr_interp_advance();
    settle();
    check_str("the next line", now_what(), "Line five.");

    /* And the migrated rollback log: back twice from "Line five." is
     * "Line three.", and both of those checkpoints came out of the save. */
    mr_interp_rollback();
    settle();
    check_str("one step back", now_what(), "Line four.");
    mr_interp_rollback();
    settle();
    check_str("two steps back, from the save's own log", now_what(),
              "Line three.");

    /* --- the resume point is gone: refuse, do not guess --- */
    open_script(dir_deleted);
    check("a slot whose resume point was deleted is refused",
          mr_save_load("1-1"), 0);

    /* --- the exact saved line reworded: the documented divergence --- */
    {
        char dir_changed[256];
        char cmd[1024];

        sprintf(dir_changed, "%s/mig_changed", tmp);
        sprintf(cmd, "mkdir -p %s", dir_changed);
        if (system(cmd) != 0) { printf("mkdir failed\n"); return 1; }
        write_changed(dir_changed);

        open_script(dir_changed);
        check("a slot whose line was reworded still loads",
              mr_save_load("1-1"), 1);
        settle();

        /* Ren'Py would show "Line four, reworded." here: its load re-executes
         * the say (rollback_core(0, on_load=True)). We restore the text from
         * the file instead. Deliberate -- NOTES.md says why -- so the old
         * text is what this asserts. */
        check_str("shows the text the save was taken with", now_what(),
                  "Line four.");

        /* The story itself is not stale: the resume point was named after the
         * *next* say, which the edit did not touch. */
        mr_interp_advance();
        settle();
        check_str("but the story continues from the new script", now_what(),
                  "Line five.");

        /* The checkpoint for the reworded line was named by its translation
         * identifier, which the rewording changed, so it and everything older
         * is dropped: rollback cannot cross the edit. */
        check("rollback does not reach across the edit",
              mr_interp_can_rollback(), 0);
    }

    /* --- the speaker's colour comes back with the line ---
     *
     * It is not in the save, and should not be: the colour belongs to the
     * Character() definition, so a stored one would override a game that had
     * recoloured someone since. It is re-derived in mr_interp_restore from
     * the say the save resumes just past.
     *
     * Nothing restored it at all before, and the symptom was odd: the name
     * and the text were both right, only the colour was from somewhere else
     * -- whatever the last say in the session had set. Loading straight
     * after launch drew the name black (the variable still zero); loading
     * after playing drew the new speaker in the *previous* speaker's colour.
     */
    {
        const char *who, *what;
        mr_u32 col_before, col_after;
        int shown;

        remove("SAVEP1S3.SAV");
        open_script(dir_before);
        mr_interp_start("start");

        /* "Line four." is an `a` line -- Ada, defined with
         * color="#c8ffc8" -- so there is a specific colour to expect. */
        check("reached Ada's line", advance_to("Line four."), 1);
        check("wrote the third slot", mr_save_write("1-3", NULL), 1);

        /* Leave the session sitting on a line by someone else, so the live
         * colour is not already the one being checked for. Without this the
         * assertion below passes whether or not anything restores it, which
         * is exactly how the bug survived: a test that loads straight after
         * saving never notices, because the right colour is still in place
         * from the save itself. */
        check("moved to a line by someone else", advance_to("Line five."), 1);
        settle();
        mr_interp_dialogue(&who, &what, &col_before, &shown);
        check("  whose colour differs from Ada's",
              (col_before & 0xFFFFFF) != 0xc8ffc8UL, 1);

        check("the slot loads", mr_save_load("1-3"), 1);
        settle();
        mr_interp_dialogue(&who, &what, &col_after, &shown);
        check_str("the restored line", what, "Line four.");
        check("  and Ada's colour came back with it",
              (long)(col_after & 0xFFFFFF), 0xc8ffc8L);
    }

    /* --- resuming at a statement that is not a say --- */
    {
        char dir_py[256], dir_py2[256];

        sprintf(dir_py, "%s/mig_py", tmp);
        sprintf(dir_py2, "%s/mig_py_edited", tmp);
        {
            char cmd[1024];
            sprintf(cmd, "mkdir -p %s %s", dir_py, dir_py2);
            if (system(cmd) != 0) { printf("mkdir failed\n"); return 1; }
        }
        write_py(dir_py, 0);
        write_py(dir_py2, 1);

        remove("SAVEP1S2.SAV");
        open_script(dir_py);
        mr_interp_start("start");
        check("reached the line before a $", advance_to("Beta."), 1);
        check("wrote the second slot", mr_save_write("1-2", NULL), 1);

        open_script(dir_py2);
        check("the slot loads against the edited script",
              mr_save_load("1-2"), 1);
        settle();
        check_str("the line on screen", now_what(), "Beta.");

        /* The resume point is the `$ marker = 2` between Beta and Gamma, so
         * the statement after the save must run and then Gamma must show. If
         * the name had shifted, this lands somewhere else in the label. */
        mr_interp_advance();
        settle();
        check_str("the next line, resuming from a non-say", now_what(),
                  "Gamma.");
        check("the $ between them ran", mr_var_get_int("marker", -1), 2);
    }

    printf("\n%d failed\n", g_fail);
    return g_fail != 0;
}

/* Setting one `persistent.<name>` must not set any other.
 *
 * The two unlockable songs are gated on `persistent.watashi` and
 * `persistent.ending`, set by separate `$` lines in the script and read from
 * three different places: a screen-language `if`, a pyfn `if` inside
 * build_music_room / build_rhythm_highscores, and an `== True` comparison.
 * Unlocking the first was unlocking the second as well.
 *
 * Nothing here is specific to those two names -- the point is that the
 * variable store, the persistent file and the readers are generic, because
 * the engine is meant for other games too.
 *
 * Run from a scratch directory: it writes and removes PERSIST.SAV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "vfs.h"
#include "expr.h"
#include "persist.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("MR_VERBOSE"))
        return;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int fails;

static void check(const char *what, int ok)
{
    printf("%-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* Truthiness as every reader sees it. */
static int truthy(const char *name)
{
    mr_value v;
    if (!mr_var_get(name, &v))
        return 0;
    return mr_value_truthy(&v);
}

static void forget(const char *name)
{
    mr_var_unset(name);
}

int main(void)
{
    remove("PERSIST.SAV");

    printf("-- one flag at a time --\n");

    forget("persistent.watashi");
    forget("persistent.ending");
    check("both flags start unset",
          !truthy("persistent.watashi") && !truthy("persistent.ending"));

    /* Exactly what the script runs. */
    mr_expr_exec("persistent.watashi = True");
    check("setting watashi sets it", truthy("persistent.watashi"));
    check("  and leaves ending alone", !truthy("persistent.ending"));

    /* ...and through the file, which is the other half of the script line. */
    mr_persist_save();
    check("after save_persistent, watashi is still set",
          truthy("persistent.watashi"));
    check("  and ending is still unset", !truthy("persistent.ending"));

    forget("persistent.watashi");
    forget("persistent.ending");
    mr_persist_load();
    check("after a restart, watashi comes back",
          truthy("persistent.watashi"));
    check("  and ending is still unset", !truthy("persistent.ending"));

    /* The other order, in case it is the name rather than the order. */
    printf("\n-- the other one first --\n");
    remove("PERSIST.SAV");
    forget("persistent.watashi");
    forget("persistent.ending");
    mr_expr_exec("persistent.ending = True");
    check("setting ending sets it", truthy("persistent.ending"));
    check("  and leaves watashi alone", !truthy("persistent.watashi"));

    mr_persist_save();
    forget("persistent.watashi");
    forget("persistent.ending");
    mr_persist_load();
    check("after a restart, ending comes back", truthy("persistent.ending"));
    check("  and watashi is still unset", !truthy("persistent.watashi"));

    /* `== True`, which is how one of the screens tests them. */
    printf("\n-- compared against True --\n");
    {
        mr_value v;
        mr_expr_eval("persistent.ending == True", &v);
        check("a set flag compares equal to True", mr_value_truthy(&v));
        mr_expr_eval("persistent.watashi == True", &v);
        check("an unset one does not", !mr_value_truthy(&v));
    }

    /* Generic: names that share a prefix must stay distinct. */
    printf("\n-- names that look alike --\n");
    mr_expr_exec("persistent.foo = True");
    check("setting foo leaves foobar unset", !truthy("persistent.foobar"));
    mr_expr_exec("persistent.foobar = True");
    check("setting foobar keeps foo set", truthy("persistent.foo"));
    forget("persistent.foo");
    check("  and forgetting foo leaves foobar set",
          truthy("persistent.foobar"));

    /* --- a value holds a sentence, not just a word ---
     *
     * MR_VSTR_MAX was 128. The French `layout.LOADING` is exactly 128 bytes,
     * so a value stored 127 of them and the message lost its final "?" --
     * invisible in English (68 bytes) and invisible in the "erase save"
     * prompt (55 bytes), which is why it looked like a rendering bug.
     *
     * Tested here rather than through the dialog because this is where it
     * went wrong: anything that passes prose through a variable or a screen
     * parameter is exposed, and 26 strings in this game's catalogue alone
     * are 128 bytes or longer.
     */
    printf("\n-- a value holds a whole sentence --\n");
    {
        mr_value v;
        /* The real string, byte for byte, including the no-break space
         * before the "?" that French typography wants. */
        const char *fr =
            "Le chargement entra\xc3\xaenera la perte de votre progression "
            "depuis la derni\xc3\xa8re sauvegarde.\n"
            "\xc3\x8ates-vous s\xc3\xbbr de vouloir faire cela\xc2\xa0?";

        check("the message is the 128 bytes that broke it",
              strlen(fr) == 128);

        mr_value_set_str(&v, fr);
        check("  and a value keeps all of it", strlen(v.s) == 128);
        check("  including the last character",
              v.s[strlen(v.s) - 1] == '?');
        check("  byte for byte", !strcmp(v.s, fr));

        /* Through a variable, which is the path the confirm message takes. */
        mr_var_set("_long", &v);
        memset(&v, 0, sizeof(v));
        check("  and it survives a round trip",
              mr_var_get("_long", &v) && !strcmp(v.s, fr));

        /* Longer than that still has to work, or the cap has merely moved
         * somewhere else unannounced. */
        {
            char big[MR_VSTR_MAX];
            memset(big, 'x', sizeof(big) - 1);
            big[sizeof(big) - 1] = '\0';
            mr_value_set_str(&v, big);
            check("a value fills to MR_VSTR_MAX - 1",
                  (int)strlen(v.s) == MR_VSTR_MAX - 1);
        }
    }

    remove("PERSIST.SAV");
    printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}

/* initest.c - mrpy.ini: the reader, and the number parser underneath it.
 *
 * config.c had no coverage at all, which was tolerable while it was "split
 * a line on =" and stopped being so when mr_config_int grew a second number
 * base. The base is the point of this file: MODES.EXE prints VBE mode
 * numbers in hex because the VBE standard does, and `vbemode` read them as
 * decimal, so the number printed for the ini could not be typed into it and
 * nothing said so -- 0x112 quietly meant mode 112. Both spellings now work.
 *
 * What is checked beyond that is the behaviour the engine depends on
 * everywhere it calls a getter: a bad value is logged and the *default*
 * comes back, because a window 0 pixels wide is worse than a window that
 * ignored the file.
 *
 * It writes its own ini in the working directory and removes it, so it
 * needs no game and no fixture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "config.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

static void check_int(const char *label, long got, long want)
{
    if (got != want)
        printf("  %-62s ** FAIL ** (got %ld, wanted %ld)\n", label, got, want);
    else
        printf("  %-62s ok\n", label);
    if (got != want)
        fails++;
}

#define INI "initest.ini"

static void write_ini(void)
{
    FILE *f = fopen(INI, "w");

    if (!f) {
        printf("FATAL: cannot write %s\n", INI);
        exit(1);
    }

    fprintf(f, "; a comment\n");
    fprintf(f, "[Minirenpy]\n");
    fprintf(f, "vbemode=0x112\n");
    fprintf(f, "capsmode=0X112\n");
    fprintf(f, "decmode=274\n");
    fprintf(f, "leadzero=0640\n");
    fprintf(f, "negative=-5\n");
    fprintf(f, "spaced =  42  \n");
    fprintf(f, "VBEMODEUPPER=0x1b\n");
    fprintf(f, "notanumber=wide\n");
    fprintf(f, "emptyhex=0x\n");
    fprintf(f, "trailing=12ab\n");
    fprintf(f, "toobig=99999\n");
    fprintf(f, "pak=mrpy.pak\n");
    fprintf(f, "[Other]\n");
    fprintf(f, "vbemode=0x999\n");
    fclose(f);
}

int main(void)
{
    write_ini();
    mr_config_load(INI);

    printf("-- hex and decimal, which is what this file exists for --\n");
    check_int("0x112 is read as hex", mr_config_int("vbemode", 0, 0, 0xFFFF),
              0x112);
    check_int("0X112 too, since the file is not case sensitive either",
              mr_config_int("capsmode", 0, 0, 0xFFFF), 0x112);
    check_int("274 is still decimal, as every existing ini has it",
              mr_config_int("decmode", 0, 0, 0xFFFF), 274);
    check_int("0x112 and 274 are the same mode",
              mr_config_int("vbemode", 0, 0, 0xFFFF),
              mr_config_int("decmode", 0, 0, 0xFFFF));

    /* The reason this is not strtol's base 0. A leading zero reading as
     * octal would turn a plausible `width=0640` into 416 silently, which is
     * a worse failure than the one the hex form fixes. */
    check_int("a leading zero is decimal, not octal",
              mr_config_int("leadzero", 0, 0, 10000), 640);

    check_int("a negative value survives", mr_config_int("negative", 0, -10, 10),
              -5);
    check_int("surrounding space is trimmed", mr_config_int("spaced", 0, 0, 100),
              42);
    check_int("keys match without regard to case",
              mr_config_int("vbemodeupper", 0, 0, 0xFFFF), 0x1b);

    printf("\n-- a bad value gives the default back, and says so --\n");
    check_int("a word is not a number", mr_config_int("notanumber", 7, 0, 100),
              7);
    /* "0x" with no digits consumes nothing. The parser compares against the
     * start of the *digits*, not of the string, or this would read as 0. */
    check_int("a bare 0x is refused rather than read as zero",
              mr_config_int("emptyhex", 7, 0, 100), 7);
    check_int("trailing rubbish is refused, not ignored",
              mr_config_int("trailing", 7, 0, 100), 7);
    check_int("out of range gives the default, not the clamp",
              mr_config_int("toobig", 7, 0, 100), 7);
    check_int("a key that is not there gives the default",
              mr_config_int("absent", 7, 0, 100), 7);

    printf("\n-- the rest of the reader --\n");
    check("a string value comes back",
          strcmp(mr_config_str("pak", ""), "mrpy.pak") == 0);
    check("a missing string gives the default",
          strcmp(mr_config_str("absent", "fallback"), "fallback") == 0);
    /* Another section's keys are parsed and ignored, so a game may keep its
     * own settings in the same file without colliding with the engine's. */
    check_int("another section does not override [Minirenpy]",
              mr_config_int("vbemode", 0, 0, 0xFFFF), 0x112);

    /* A missing file is not an error: every getter returns its default,
     * which is a working configuration. */
    mr_config_load("initest-nosuchfile.ini");
    check_int("an absent ini leaves every getter on its default",
              mr_config_int("vbemode", 99, 0, 0xFFFF), 99);

    remove(INI);

    printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}

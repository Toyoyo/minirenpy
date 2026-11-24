/* UTF-8 to CP1252, for the window caption.
 *
 * The script is UTF-8 and SetWindowText on Win32s is the -A entry point, so
 * the caption has to be converted. It was not, and `Le départ de Kalea`
 * reached the title bar as `Le dÃ©part de Kalea` -- CP1252's reading of the
 * two bytes C3 A9.
 *
 * What this covers is mr_utf8_to_ansi. It does *not* cover the call site:
 * main.c is linked by no harness, so that one line is verified only by
 * running the game. Said here rather than left to be assumed.
 *
 * Expected bytes are written as hex escapes rather than as literal CP1252,
 * because this file is itself UTF-8 and a literal `é` in it would be the two
 * bytes we are converting away from.
 */
#include <stdio.h>
#include <string.h>
#include "mr.h"
#include "ansi.h"

static int fails;

static void check(const char *label, const char *in, const char *want)
{
    char got[256];
    int n, ok;

    n = mr_utf8_to_ansi(in, got, (int)sizeof(got));
    ok = (strcmp(got, want) == 0) && (n == (int)strlen(want));

    printf("%-44s ", label);
    {
        int i;
        for (i = 0; got[i]; i++)
            printf("%02X ", (unsigned char)got[i]);
        printf(" %s\n", ok ? "ok" : "** FAIL **");
        if (!ok) {
            printf("%-44s want ", "");
            for (i = 0; want[i]; i++)
                printf("%02X ", (unsigned char)want[i]);
            printf("\n");
            fails++;
        }
    }
}

int main(void)
{
    char out[8];

    setbuf(stdout, NULL);

    /* The reported case, and the whole point. */
    check("the reported title",
          "Le d\xC3\xA9part de Kalea", "Le d\xE9part de Kalea");

    /* ASCII is untouched, including the empty string. */
    check("ASCII passes through", "Miwa no Utsuroi", "Miwa no Utsuroi");
    check("the empty string", "", "");

    /* The Latin-1 range is a straight copy, which is the easy half. A title
     * that is only this would have worked with a two-line converter. */
    check("Latin-1: accented vowels",
          "\xC3\xA0\xC3\xA8\xC3\xAC\xC3\xB2\xC3\xB9", "\xE0\xE8\xEC\xF2\xF9");
    check("Latin-1: cedilla and tilde",
          "Fran\xC3\xA7ois ni\xC3\xB1o", "Fran\xE7ois ni\xF1o");

    /* The hard half: CP1252's 0x80-0x9F block, which Latin-1 leaves as
     * control codes. A converter that handled only Latin-1 would turn every
     * one of these into '?', and a title written in any editor that
     * substitutes quotes is full of them. */
    check("curly apostrophe (U+2019)",
          "L\xE2\x80\x99\xC3\xA9t\xC3\xA9", "L\x92\xE9t\xE9");
    check("curly double quotes (U+201C/D)",
          "\xE2\x80\x9CHinata\xE2\x80\x9D", "\x93Hinata\x94");
    check("em dash (U+2014)", "a\xE2\x80\x94""b", "a\x97""b");
    check("en dash (U+2013)", "a\xE2\x80\x93""b", "a\x96""b");
    check("ellipsis (U+2026)", "wait\xE2\x80\xA6", "wait\x85");
    check("oe ligature (U+0153)", "c\xC5\x93ur", "c\x9Cur");
    check("euro sign (U+20AC)", "5\xE2\x82\xAC", "5\x80");
    check("Y diaeresis (U+0178)", "\xC5\xB8", "\x9F");

    /* A zero-width character is dropped, not substituted. kalea's script.rpy
     * has a U+FEFF immediately after an opening quote, invisible in the
     * editor that produced it; turning it into a visible '?' would report a
     * problem whose cause the author cannot see. */
    check("U+FEFF is dropped, not '?'",
          /* Split: a hex escape is greedy, so "\xBFAssise" would parse the
           * 'A' as part of the escape rather than as the first letter. */
          "\xEF\xBB\xBF" "Assise", "Assise");

    /* No CP1252 byte, no obvious ASCII stand-in: '?' is visible and says so,
     * where dropping it would silently shorten the title. */
    check("an unrepresentable character is '?'",
          "star \xE2\x98\x85", "star ?");
    check("a CJK character is '?'", "\xE6\x97\xA5", "?");

    /* But where there is an obvious stand-in, it is used rather than '?'. */
    check("U+2010 hyphen falls back to '-'", "a\xE2\x80\x90""b", "a-b");
    check("U+2009 thin space falls back to ' '", "a\xE2\x80\x89""b", "a b");

    /* Malformed input must terminate and must not run off the buffer. A lone
     * continuation byte is consumed one at a time as U+FFFD, which has no
     * CP1252 form, so it comes out as '?'. */
    check("a stray continuation byte", "a\x80""b", "a?b");
    check("a truncated sequence", "a\xC3", "a?");

    /* Truncation is by output bytes and still terminates. "abcdefghij" into
     * eight bytes is seven characters and a NUL. */
    {
        int n = mr_utf8_to_ansi("abcdefghij", out, (int)sizeof(out));
        int ok = (n == 7) && !strcmp(out, "abcdefg");
        printf("%-44s %d '%s'  %s\n", "truncates to the buffer, terminated",
               n, out, ok ? "ok" : "** FAIL **");
        if (!ok)
            fails++;
    }

    /* A one-byte buffer has room for the terminator and nothing else. */
    {
        char tiny[1];
        int n = mr_utf8_to_ansi("abc", tiny, 1);
        int ok = (n == 0) && (tiny[0] == '\0');
        printf("%-44s %d        %s\n", "a 1-byte buffer writes only the NUL",
               n, ok ? "ok" : "** FAIL **");
        if (!ok)
            fails++;
    }

    /* A NULL source terminates the buffer rather than leaving it undefined:
     * the caller passes a pointer that may not have been found. */
    {
        char buf[4];
        int n;
        memset(buf, 'x', sizeof(buf));
        n = mr_utf8_to_ansi(NULL, buf, (int)sizeof(buf));
        printf("%-44s %d '%s'  %s\n", "a NULL source gives an empty string",
               n, buf, (n == 0 && buf[0] == '\0') ? "ok" : "** FAIL **");
        if (!(n == 0 && buf[0] == '\0'))
            fails++;
    }

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

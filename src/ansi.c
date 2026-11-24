/* UTF-8 to CP1252. See ansi.h for why this exists.
 *
 * CP1252 is Latin-1 plus a block at 0x80-0x9F that Latin-1 leaves as control
 * codes, and that block is where the typographic characters live: the curly
 * quotes, the en and em dash, the ellipsis, the euro sign, oe and OE. A VN
 * title is exactly the kind of string that has them -- options.rpy is written
 * in a text editor that substitutes quotes -- so a converter that handled
 * only Latin-1 would turn half of them into '?'.
 */

#include "mr.h"
#include "ansi.h"

/* The 0x80-0x9F block, in byte order with the five unassigned slots as 0.
 * Table rather than a switch so the reverse lookup is a single loop and the
 * assignment is readable against the published code page. */
static const mr_u32 g_c1[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178
};

/* Decode one UTF-8 character, advancing *p. Invalid bytes are consumed one at
 * a time and reported as U+FFFD, so a truncated or mis-encoded file cannot
 * stall the loop.
 *
 * font.c has its own copy of this. They are not shared on purpose: this file
 * is meant to link on its own, and pulling in font.c would drag the atlas
 * loader and the vfs behind it for the sake of eighteen lines. */
static mr_u32 utf8_next(const char **p)
{
    const mr_u8 *s = (const mr_u8 *)*p;
    mr_u32 c = *s;
    int extra, i;

    if (c < 0x80)                { extra = 0; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { (*p)++; return 0xFFFD; }

    s++;
    for (i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) {
            (*p)++;
            return 0xFFFD;
        }
        c = (c << 6) | (*s & 0x3F);
        s++;
    }

    *p = (const char *)s;
    return c;
}

/* An ASCII stand-in for a character CP1252 cannot hold, or 0 for "no obvious
 * one". Only the characters a Latin-script title plausibly contains: the
 * remaining dash and quote variants, the non-breaking and typographic spaces,
 * and the ellipsis forms. Everything else falls through to '?'. */
static char fallback(mr_u32 c)
{
    switch (c) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2015:
        return '-';
    case 0x2032: case 0x201B:
        return '\'';
    case 0x2033:
        return '"';
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x202F: case 0x205F: case 0x3000:
        return ' ';
    default:
        return 0;
    }
}

int mr_utf8_to_ansi(const char *in, char *out, int outsz)
{
    int n = 0;
    int i;

    if (!out || outsz <= 0)
        return 0;
    if (!in) {
        out[0] = '\0';
        return 0;
    }

    while (*in && n < outsz - 1) {
        mr_u32 c = utf8_next(&in);
        int b = -1;

        /* The zero-width characters are dropped rather than substituted. A
         * BOM or a word joiner pasted into a title is invisible in the editor
         * that produced it, and turning it into a visible '?' would report a
         * problem the author cannot see the cause of. kalea's script.rpy has
         * a U+FEFF immediately after a quote, which is how this came up. */
        if (c == 0xFEFF || c == 0x200B || c == 0x200C || c == 0x200D ||
            c == 0x2060)
            continue;

        if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
            b = (int)c;
        } else {
            for (i = 0; i < 32; i++) {
                if (g_c1[i] && g_c1[i] == c) {
                    b = 0x80 + i;
                    break;
                }
            }
        }

        if (b < 0) {
            char f = fallback(c);
            b = f ? (int)(unsigned char)f : '?';
        }

        out[n++] = (char)b;
    }

    out[n] = '\0';
    return n;
}

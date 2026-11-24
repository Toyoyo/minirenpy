/* datefmt.c - strftime with the day and month names taken from the
 * translation catalogue rather than from the C locale.
 *
 * See datefmt.h. The tables below are Ren'Py's own, copied exactly from
 * 00action_file.rpy:22-75, because they are the *lookup keys*: a catalogue
 * translates `{#month}September`, so anything else misses and the English
 * comes out. The `{#...}` prefix is a disambiguation tag -- it keeps "May"
 * the month apart from "may" the verb for translators -- and is stripped
 * before the name is emitted.
 *
 * The weekday tables start at **Monday**, because Ren'Py indexes them with
 * Python's tm_wday where Monday is 0. C's struct tm numbers from Sunday, so
 * the index is converted; getting that wrong shifts every date by one day,
 * which is the sort of thing that looks like a timezone bug for an hour.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "datefmt.h"
#include "tl.h"

static const char *const WEEKDAY_LONG[7] = {
    "{#weekday}Monday", "{#weekday}Tuesday", "{#weekday}Wednesday",
    "{#weekday}Thursday", "{#weekday}Friday", "{#weekday}Saturday",
    "{#weekday}Sunday"
};

static const char *const WEEKDAY_SHORT[7] = {
    "{#weekday_short}Mon", "{#weekday_short}Tue", "{#weekday_short}Wed",
    "{#weekday_short}Thu", "{#weekday_short}Fri", "{#weekday_short}Sat",
    "{#weekday_short}Sun"
};

static const char *const MONTH_LONG[12] = {
    "{#month}January", "{#month}February", "{#month}March",
    "{#month}April", "{#month}May", "{#month}June",
    "{#month}July", "{#month}August", "{#month}September",
    "{#month}October", "{#month}November", "{#month}December"
};

static const char *const MONTH_SHORT[12] = {
    "{#month_short}Jan", "{#month_short}Feb", "{#month_short}Mar",
    "{#month_short}Apr", "{#month_short}May", "{#month_short}Jun",
    "{#month_short}Jul", "{#month_short}Aug", "{#month_short}Sep",
    "{#month_short}Oct", "{#month_short}Nov", "{#month_short}Dec"
};

/* Past a leading {#...}, if there is one. The catalogue keeps the tag on the
 * translation too ("{#weekday}Lundi"), so this runs on the way out as well
 * as being the reason the key has one. */
static const char *past_tag(const char *s)
{
    if (s && s[0] == '{' && s[1] == '#') {
        const char *e = strchr(s, '}');
        if (e)
            return e + 1;
    }
    return s;
}

/* Append, stopping at the end of the buffer. */
static int put(char *out, int outsz, int n, const char *s)
{
    while (*s && n < outsz - 1)
        out[n++] = *s++;
    return n;
}

static int put_num(char *out, int outsz, int n, int value, int width)
{
    char buf[16];
    sprintf(buf, width == 2 ? "%02d" : "%d", value);
    return put(out, outsz, n, buf);
}

int mr_format_time(char *out, int outsz, const char *fmt, long epoch)
{
    time_t t = (time_t)epoch;
    struct tm *lt;
    int n = 0;
    int wday;

    if (!out || outsz < 1)
        return 0;
    out[0] = '\0';
    if (!fmt)
        return 0;

    lt = localtime(&t);
    if (!lt)
        return 0;

    /* Python counts weekdays from Monday, C from Sunday. */
    wday = (lt->tm_wday + 6) % 7;

    while (*fmt && n < outsz - 1) {
        /* The format is a translatable string and carries its tag. */
        if (fmt[0] == '{' && fmt[1] == '#') {
            const char *e = strchr(fmt, '}');
            if (e) {
                fmt = e + 1;
                continue;
            }
        }

        if (*fmt != '%') {
            out[n++] = *fmt++;
            continue;
        }

        fmt++;
        if (!*fmt)
            break;

        /* Ren'Py's split allows flags and a width between the % and the
         * letter (`%[-_^#]?[0-9]*[a-zA-Z]`). We accept and ignore them
         * rather than mis-parsing a format that uses one. */
        while (*fmt == '-' || *fmt == '_' || *fmt == '^' || *fmt == '#')
            fmt++;
        while (*fmt >= '0' && *fmt <= '9')
            fmt++;
        if (!*fmt)
            break;

        switch (*fmt) {
        case 'a': n = put(out, outsz, n, past_tag(
                      mr_tl_string(WEEKDAY_SHORT[wday]))); break;
        case 'A': n = put(out, outsz, n, past_tag(
                      mr_tl_string(WEEKDAY_LONG[wday]))); break;
        case 'b': n = put(out, outsz, n, past_tag(
                      mr_tl_string(MONTH_SHORT[lt->tm_mon]))); break;
        case 'B': n = put(out, outsz, n, past_tag(
                      mr_tl_string(MONTH_LONG[lt->tm_mon]))); break;

        /* The rest are numbers, done here rather than handed to strftime:
         * only a handful are ever used, and Watcom's strftime would want the
         * locale we are deliberately not using. */
        case 'd': n = put_num(out, outsz, n, lt->tm_mday, 2); break;
        case 'e': n = put_num(out, outsz, n, lt->tm_mday, 0); break;
        case 'm': n = put_num(out, outsz, n, lt->tm_mon + 1, 2); break;
        case 'Y': n = put_num(out, outsz, n, lt->tm_year + 1900, 0); break;
        case 'y': n = put_num(out, outsz, n, (lt->tm_year + 1900) % 100, 2);
                  break;
        case 'H': n = put_num(out, outsz, n, lt->tm_hour, 2); break;
        case 'M': n = put_num(out, outsz, n, lt->tm_min, 2); break;
        case 'S': n = put_num(out, outsz, n, lt->tm_sec, 2); break;
        case 'I': {
            int h = lt->tm_hour % 12;
            n = put_num(out, outsz, n, h ? h : 12, 2);
            break;
        }
        case 'p': n = put(out, outsz, n, lt->tm_hour < 12 ? "AM" : "PM");
                  break;
        case 'j': n = put_num(out, outsz, n, lt->tm_yday + 1, 0); break;
        case '%': if (n < outsz - 1) out[n++] = '%'; break;

        /* Anything else is passed through as it was written, so an unknown
         * specifier is visible in the slot rather than silently eaten. */
        default:
            if (n < outsz - 1) out[n++] = '%';
            if (n < outsz - 1) out[n++] = *fmt;
            break;
        }
        fmt++;
    }

    out[n] = '\0';
    return n;
}

/* font.c - drawing text from a prebaked coverage atlas.
 *
 * Glyphs arrive as 8-bit coverage from tools/mkfont.py, so drawing is an
 * alpha blend per pixel with the text colour. That reproduces Ren'Py's
 * antialiased text, which GDI on Windows 3.1 cannot do for TrueType.
 *
 * Ren'Py's inline text tags are recognised. {b}, {i} and {u} are rendered:
 * the first two select one of four baked variants of each glyph (regular,
 * bold, italic, bold italic) and the third is drawn as a rule under the
 * advance width, since an underline needs no glyph data. Every other tag
 * ({w}, {cps} and so on) is still consumed and ignored.
 *
 * The tags are counted per flag rather than kept on a stack, so the closes
 * need not be in the order the opens were: {i}{b}x{/i}{/b} does what it
 * looks like. Ren'Py is stricter, but being lenient here costs nothing and
 * the alternative is silently dropping a style for the rest of the line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "font.h"
#include "vfs.h"

/* Longest single wrapped line we will assemble. Dialogue lines in this game
 * top out well under this. */
#define MR_FONT_LINE_MAX 1024

#define MR_FONT_MAGIC 0x4E46524DUL
#define MR_FONT_VER   2

/* Glyphs are keyed by codepoint and style together, so one file per size
 * still holds every variant. Bit 0 of the style is bold, bit 1 italic;
 * mkfont.py packs the key the same way. */
#define MR_STYLE_B     1
#define MR_STYLE_I     2
#define MR_GLYPH_KEY(cp, style) (((mr_u32)(cp) << 2) | (mr_u32)(style))

typedef struct {
    mr_u32 key;
    short  x, y, w, h;
    short  ox, oy, adv, pad;
} mr_glyph;

/* How many of each tag are currently open. Threaded through a whole block of
 * wrapped text so that a style opened on one line survives onto the next. */
typedef struct {
    int b, i, u;
} mr_tstyle;

struct mr_font {
    int       size;
    int       ascent;
    int       descent;
    int       line_height;
    int       atlas_w;
    int       atlas_h;
    int       nglyphs;
    mr_glyph *glyphs;
    mr_u8    *atlas;
    mr_font  *fallback;       /* borrowed; see mr_font_set_fallback */
};

/* How far a fallback chain is followed. One link is all either game needs
 * (named face -> default face); the limit is only here so that a cycle cannot
 * hang the renderer. */
#define MR_FONT_MAX_FALLBACK 4

mr_font *mr_font_load(const char *path)
{
    mr_file *fp;
    mr_font *f;
    mr_u32 hdr[9];
    long need;

    fp = mr_vfs_open(path);
    if (!fp) {
        mr_log("font: cannot open %s", path);
        return NULL;
    }

    if (mr_vfs_read(fp, hdr, (long)sizeof(hdr)) != (long)sizeof(hdr)) {
        mr_log("font: %s is truncated", path);
        mr_vfs_close(fp);
        return NULL;
    }
    if (hdr[0] != MR_FONT_MAGIC || hdr[1] != MR_FONT_VER) {
        mr_log("font: %s has a bad header (%08lX v%lu)", path,
               (unsigned long)hdr[0], (unsigned long)hdr[1]);
        mr_vfs_close(fp);
        return NULL;
    }

    f = (mr_font *)calloc(1, sizeof(mr_font));
    if (!f) {
        mr_vfs_close(fp);
        return NULL;
    }

    f->size        = (int)hdr[2];
    f->ascent      = (int)hdr[3];
    f->descent     = (int)hdr[4];
    f->line_height = (int)hdr[5];
    f->atlas_w     = (int)hdr[6];
    f->atlas_h     = (int)hdr[7];
    f->nglyphs     = (int)hdr[8];

    if (f->nglyphs <= 0 || f->atlas_w <= 0 || f->atlas_h <= 0) {
        mr_log("font: %s has implausible dimensions", path);
        free(f);
        mr_vfs_close(fp);
        return NULL;
    }

    f->glyphs = (mr_glyph *)malloc((size_t)f->nglyphs * sizeof(mr_glyph));
    need = (long)f->atlas_w * (long)f->atlas_h;
    f->atlas = (mr_u8 *)malloc((size_t)need);
    if (!f->glyphs || !f->atlas) {
        mr_log("font: out of memory loading %s", path);
        mr_font_free(f);
        mr_vfs_close(fp);
        return NULL;
    }

    if (mr_vfs_read(fp, f->glyphs,
                    (long)f->nglyphs * (long)sizeof(mr_glyph))
            != (long)f->nglyphs * (long)sizeof(mr_glyph) ||
        mr_vfs_read(fp, f->atlas, need) != need) {
        mr_log("font: %s ended early", path);
        mr_font_free(f);
        mr_vfs_close(fp);
        return NULL;
    }

    mr_vfs_close(fp);
    return f;
}

void mr_font_set_fallback(mr_font *f, mr_font *fb)
{
    if (f && f != fb)
        f->fallback = fb;
}

void mr_font_free(mr_font *f)
{
    if (!f)
        return;
    /* f->fallback is borrowed and belongs to ui.c; freeing it here would
     * double-free the default atlas every named face points at. */
    free(f->glyphs);
    free(f->atlas);
    free(f);
}

int mr_font_line_height(const mr_font *f)
{
    return f ? f->line_height : 0;
}

/* The glyph table is sorted by key at bake time. */
static const mr_glyph *lookup(const mr_font *f, mr_u32 key)
{
    int lo = 0, hi = f->nglyphs - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].key == key)
            return &f->glyphs[mid];
        if (f->glyphs[mid].key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* A variant the baker left out -- every CJK glyph, which has one weight --
 * falls back to the regular face rather than vanishing. */
static const mr_glyph *find_in(const mr_font *f, mr_u32 cp, int style)
{
    const mr_glyph *g = lookup(f, MR_GLYPH_KEY(cp, style));

    if (!g && style)
        g = lookup(f, MR_GLYPH_KEY(cp, 0));
    return g;
}

/* As above, then along the fallback chain. `owner` receives the font whose
 * atlas holds the glyph, which is what draw_glyph needs: the coordinates in a
 * mr_glyph index its own atlas, not the one that was asked. Pass NULL when
 * only the advance matters. */
static const mr_glyph *find_glyph(const mr_font *f, mr_u32 cp, int style,
                                  const mr_font **owner)
{
    int depth;

    for (depth = 0; f && depth < MR_FONT_MAX_FALLBACK; depth++) {
        const mr_glyph *g = find_in(f, cp, style);
        if (g) {
            if (owner)
                *owner = f;
            return g;
        }
        f = f->fallback;
    }
    return NULL;
}

/* Decode one UTF-8 character, advancing *p. Invalid bytes are consumed one at
 * a time and reported as U+FFFD so a bad byte cannot stall the loop. */
static mr_u32 utf8_next(const char **p)
{
    const mr_u8 *s = (const mr_u8 *)*p;
    mr_u32 c = *s;
    int extra, i;

    if (c < 0x80)       { extra = 0; }
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

/* Consume a Ren'Py text tag at *p, which points at '{', applying it to *st if
 * it is one we render. A doubled '{{' is a literal brace and is not a tag.
 * Returns 1 if a tag was consumed. Passing a NULL st just skips it.
 *
 * A stray close is clamped at zero rather than going negative, so a line with
 * an unmatched {/i} in it does not turn everything after it italic. */
static int take_tag(const char **p, mr_tstyle *st)
{
    const char *q = *p;
    int close = 0;
    char name;

    if (*q != '{' || q[1] == '{')
        return 0;

    q++;
    if (*q == '/') {
        close = 1;
        q++;
    }
    name = *q;

    /* Only a bare one-letter tag counts: {b} but not {bold} or {b=2}. */
    if (q[0] && q[1] == '}' && st) {
        int *n = (name == 'b') ? &st->b :
                 (name == 'i') ? &st->i :
                 (name == 'u') ? &st->u : NULL;
        if (n) {
            if (close)
                *n = (*n > 0) ? *n - 1 : 0;
            else
                (*n)++;
        }
    }

    while (*q && *q != '}')
        q++;
    if (*q == '}')
        q++;
    *p = q;
    return 1;
}

static int style_of(const mr_tstyle *st)
{
    return (st->b > 0 ? MR_STYLE_B : 0) | (st->i > 0 ? MR_STYLE_I : 0);
}

/* Width of one line of a run, advancing the style state across it. Stops at a
 * newline: every other caller builds its argument with next_word, which never
 * crosses one, so this only matters to mr_font_text_width below. */
static int width_run(const mr_font *f, const char *utf8, mr_tstyle *st)
{
    int w = 0;
    const mr_glyph *g;
    mr_u32 cp;

    if (!f || !utf8)
        return 0;

    while (*utf8 && *utf8 != '\n') {
        if (take_tag(&utf8, st))
            continue;
        if (utf8[0] == '{' && utf8[1] == '{')
            utf8++;                       /* emit one of the two braces */
        cp = utf8_next(&utf8);
        g = find_glyph(f, cp, style_of(st), NULL);
        if (g)
            w += g->adv;
    }
    return w;
}

/* The natural width of a piece of text: the widest of its lines, not the
 * length of the whole thing laid end to end.
 *
 * An embedded newline is a line break -- Ren'Py's confirm messages are
 * written with one (`layout.LOADING`) -- and a caller measuring one wants the
 * box it will occupy. Returning the sum made slrender size the load prompt's
 * frame to the whole screen. */
int mr_font_text_width(const mr_font *f, const char *utf8)
{
    mr_tstyle st;
    int best = 0;

    if (!f || !utf8)
        return 0;

    st.b = st.i = st.u = 0;

    for (;;) {
        int w = width_run(f, utf8, &st);
        const char *nl;

        if (w > best)
            best = w;

        nl = strchr(utf8, '\n');
        if (!nl)
            break;
        utf8 = nl + 1;      /* style state carries across the break */
    }
    return best;
}

int mr_font_text_lines(const char *utf8)
{
    int n = 1;

    if (!utf8)
        return 0;
    while ((utf8 = strchr(utf8, '\n')) != NULL) {
        n++;
        utf8++;
    }
    return n;
}

/* Blend one glyph's coverage into the destination. */
static void draw_glyph(mr_surface *dst, const mr_font *f, const mr_glyph *g,
                       int x, int y, mr_u32 color)
{
    int gy, gx, dx, dy;
    int cx, cy, cw, ch;
    const mr_u8 *src;
    mr_u32 *row;
    unsigned a, ia, cr, cg, cb;

    if (!g->w || !g->h)
        return;

    /* Text inside a viewport has to be cut off at its edge like anything
     * else, so glyphs are clipped rather than merely bounded by the
     * surface. */
    mr_clip_rect(dst, &cx, &cy, &cw, &ch);

    cr = MR_R(color);
    cg = MR_G(color);
    cb = MR_B(color);

    for (gy = 0; gy < g->h; gy++) {
        dy = y + g->oy + gy;
        if (dy < cy || dy >= cy + ch)
            continue;

        src = f->atlas + (size_t)(g->y + gy) * f->atlas_w + g->x;
        row = dst->px + (size_t)dy * dst->pitch;

        for (gx = 0; gx < g->w; gx++) {
            a = src[gx];
            if (!a)
                continue;

            dx = x + g->ox + gx;
            if (dx < cx || dx >= cx + cw)
                continue;

            if (a == 255) {
                row[dx] = MR_ARGB(255, cr, cg, cb);
                continue;
            }
            ia = 255u - a;
            {
                mr_u32 d = row[dx];
                row[dx] = MR_ARGB(255,
                    MR_DIV255(cr * a + (unsigned)MR_R(d) * ia),
                    MR_DIV255(cg * a + (unsigned)MR_G(d) * ia),
                    MR_DIV255(cb * a + (unsigned)MR_B(d) * ia));
            }
        }
    }
}

/* A solid horizontal bar, clipped like a glyph: the underline. */
static void draw_rule(mr_surface *dst, int x, int y, int w, int h,
                      mr_u32 color)
{
    int cx, cy, cw, ch, px, py;

    mr_clip_rect(dst, &cx, &cy, &cw, &ch);

    for (py = y; py < y + h; py++) {
        mr_u32 *row;
        if (py < cy || py >= cy + ch)
            continue;
        row = dst->px + (size_t)py * dst->pitch;
        for (px = x; px < x + w; px++) {
            if (px < cx || px >= cx + cw)
                continue;
            row[px] = MR_ARGB(255, MR_R(color), MR_G(color), MR_B(color));
        }
    }
}

/* Where an underline sits and how thick it is. Both scale with the size so
 * that the 50px title does not get the same hairline as 14px body text. */
static int rule_thickness(const mr_font *f)
{
    int t = f->size / 16;
    return t < 1 ? 1 : t;
}

static int draw_run(mr_surface *dst, const mr_font *f, int x, int y,
                    const char *utf8, mr_u32 color, mr_tstyle *st)
{
    const mr_glyph *g;
    const mr_font *owner;
    mr_u32 cp;

    if (!f || !utf8)
        return x;

    while (*utf8) {
        if (take_tag(&utf8, st))
            continue;
        if (utf8[0] == '{' && utf8[1] == '{')
            utf8++;
        cp = utf8_next(&utf8);
        owner = f;
        g = find_glyph(f, cp, style_of(st), &owner);
        if (!g)
            continue;
        if (dst) {
            /* `owner`, not f: the glyph may have come from the fallback
             * atlas. The baseline and the rule stay on f, because the line
             * metrics are the asked-for face's. */
            draw_glyph(dst, owner, g, x, y, color);
            /* Per glyph rather than per run: consecutive glyphs cover the
             * whole advance, so the rule comes out continuous anyway, and a
             * style change mid-word needs no bookkeeping. */
            if (st->u > 0 && g->adv > 0)
                draw_rule(dst, x, y + f->ascent + rule_thickness(f),
                          g->adv, rule_thickness(f), color);
        }
        x += g->adv;
    }
    return x;
}

int mr_font_draw(mr_surface *dst, const mr_font *f, int x, int y,
                 const char *utf8, mr_u32 color)
{
    mr_tstyle st;

    st.b = st.i = st.u = 0;
    return draw_run(dst, f, x, y, utf8, color, &st);
}

/* Four-way rather than eight-way: at a one-pixel radius the diagonals add
 * almost nothing visually and would cost another two passes over every
 * glyph. Each pass starts from the same entry style; only the last one is
 * allowed to advance it. */
static int outline_run(mr_surface *dst, const mr_font *f, int x, int y,
                       const char *utf8, mr_u32 color, mr_u32 outline,
                       mr_tstyle *st)
{
    if (dst) {
        static const int ox[4] = { -1, 1, 0, 0 };
        static const int oy[4] = { 0, 0, -1, 1 };
        int i;
        for (i = 0; i < 4; i++) {
            mr_tstyle pass = *st;
            draw_run(dst, f, x + ox[i], y + oy[i], utf8, outline, &pass);
        }
    }
    return draw_run(dst, f, x, y, utf8, color, st);
}

int mr_font_draw_outlined(mr_surface *dst, const mr_font *f, int x, int y,
                          const char *utf8, mr_u32 color, mr_u32 outline)
{
    mr_tstyle st;

    st.b = st.i = st.u = 0;
    return outline_run(dst, f, x, y, utf8, color, outline, &st);
}

/* The width of `s`, not counting whitespace at its end.
 *
 * A line break happens *at* a space, so the space that ends a line costs
 * nothing: it is the break. Ren'Py arrives at this from the other direction
 * -- it breaks at the space rather than deciding word by word, and sets a
 * line-ending glyph's advance to its width, which for a space is nothing
 * (textsupport.pyx:697). Measuring the trailing space made us wrap a word
 * early whenever a line fit with less than a space to spare: the textbox at
 * gui.text_size 22 has about 3px left at the end of the first line of
 * "Il y a plusieurs annees, cela m'aurait probablement paniquee, mais" and a
 * space is about 6, so "mais" went to the next line where Ren'Py keeps it.
 *
 * Takes a writable buffer rather than a const one, because it measures by
 * cutting the string short and putting the byte back: both callers hold
 * their own line buffer, and saying so in the signature is better than
 * casting the const away. The style state needs no special care -- trailing
 * whitespace carries no tags, so cutting it off cannot drop one.
 */
static int width_run_rtrim(const mr_font *f, char *s, mr_tstyle *st)
{
    int n = (int)strlen(s);
    int w;
    char save;

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;

    save = s[n];
    s[n] = '\0';
    w = width_run(f, s, st);
    s[n] = save;
    return w;
}

/* Copy the next word, plus any trailing spaces, out of *p. */
static int next_word(const char **p, char *out, int outsz)
{
    const char *q = *p;
    int n = 0;

    while (*q && *q != ' ' && *q != '\n') {
        if (n < outsz - 1)
            out[n++] = *q;
        q++;
    }
    while (*q == ' ') {
        if (n < outsz - 1)
            out[n++] = *q;
        q++;
    }
    out[n] = '\0';
    *p = q;
    return n;
}

int mr_font_draw_wrapped(mr_surface *dst, const mr_font *f, int x, int y,
                         int width, const char *utf8, mr_u32 color,
                         int outlined, mr_u32 outline)
{
    return mr_font_draw_revealed(dst, f, x, y, width, utf8, color,
                                 outlined, outline, -1);
}

/* Count visible characters in a byte range. Text tags are skipped, because
 * they are not drawn either -- otherwise a reveal would stall for three
 * characters' worth of time on every {i}. */
static int utf8_chars(const char *s, int bytes)
{
    int i = 0, n = 0;

    while (i < bytes) {
        if (s[i] == '{' && s[i + 1] != '{') {
            while (i < bytes && s[i] != '}')
                i++;
            if (i < bytes)
                i++;
            continue;
        }
        if ((s[i] & 0xC0) != 0x80)
            n++;
        i++;
    }
    return n;
}

/* Byte offset just past the nth visible character. A tag that follows the cut
 * stays on the far side of it, so a closing {/i} never appears alone. */
static int utf8_offset(const char *s, int bytes, int nth)
{
    int i = 0, n = 0;

    while (i < bytes) {
        if (s[i] == '{' && s[i + 1] != '{') {
            int start = i;
            while (i < bytes && s[i] != '}')
                i++;
            if (i < bytes)
                i++;
            if (n >= nth)
                return start;
            continue;
        }
        if ((s[i] & 0xC0) != 0x80) {
            if (n == nth)
                return i;
            n++;
        }
        i++;
    }
    return bytes;
}

int mr_font_draw_revealed(mr_surface *dst, const mr_font *f, int x, int y,
                          int width, const char *utf8, mr_u32 color,
                          int outlined, mr_u32 outline, int limit)
{
    return mr_font_draw_aligned(dst, f, x, y, width, utf8, color,
                                outlined, outline, limit, 0.0f);
}

int mr_font_draw_aligned(mr_surface *dst, const mr_font *f, int x, int y,
                         int width, const char *utf8, mr_u32 color,
                         int outlined, mr_u32 outline, int limit, float align)
{
    char line[MR_FONT_LINE_MAX];
    char probe[MR_FONT_LINE_MAX];
    char word[256];
    int  lines = 0;
    int  linelen = 0;
    int  drawn = 0;     /* characters already revealed on earlier lines */
    mr_tstyle entry;    /* style in force at the start of the current line */

    if (!f || !utf8)
        return 0;

    entry.b = entry.i = entry.u = 0;
    line[0] = '\0';

/* Emit the line built so far, cut short if the reveal has not reached the end
 * of it yet. The layout is always done on the whole text, so a line never
 * reflows as it appears -- only fewer of its characters get painted.
 *
 * `entry` then advances over the whole line, not the part that was painted,
 * so that a style opened on one wrapped line is still in force on the next
 * and the reveal does not change which words are bold. */
#define FLUSH_LINE()                                                        \
    do {                                                                    \
        int nchars_ = utf8_chars(line, linelen);                            \
        int show_ = nchars_;                                                \
        if (limit >= 0) {                                                   \
            show_ = limit - drawn;                                          \
            if (show_ < 0) show_ = 0;                                       \
            if (show_ > nchars_) show_ = nchars_;                           \
        }                                                                   \
        if (dst && show_ > 0) {                                             \
            int  cut_ = utf8_offset(line, linelen, show_);                  \
            char save_ = line[cut_];                                        \
            int  lx_ = x;                                                   \
            mr_tstyle pass_ = entry;                                        \
            line[cut_] = '\0';                                              \
            /* textalign shifts each line within the wrap width: 1.0 is the \
             * right-aligned key column on the help screen. Measured        \
             * without the trailing space, which is the break rather than   \
             * part of the line -- counting it pushed a centred line a few  \
             * pixels left of where it belongs. */                          \
            if (align > 0.0f) {                                             \
                mr_tstyle m_ = entry;                                       \
                lx_ += (int)((float)(width - width_run_rtrim(f, line, &m_)) \
                             * align);                                      \
            }                                                               \
            if (outlined)                                                   \
                outline_run(dst, f, lx_, y, line, color, outline, &pass_);  \
            else                                                            \
                draw_run(dst, f, lx_, y, line, color, &pass_);              \
            line[cut_] = save_;                                             \
        }                                                                   \
        width_run(f, line, &entry);                                         \
        drawn += nchars_;                                                   \
    } while (0)

    for (;;) {
        int wlen, pw;

        /* Flush at an explicit newline or at the end of the text. */
        if (!*utf8 || *utf8 == '\n') {
            if (linelen) {
                FLUSH_LINE();
                y += f->line_height;
                lines++;
                line[0] = '\0';
                linelen = 0;
            }
            if (!*utf8)
                break;
            utf8++;             /* consume the newline */
            continue;
        }

        wlen = next_word(&utf8, word, sizeof(word));
        if (!wlen)
            continue;

        /* Try the word on the current line. */
        if (linelen + wlen < (int)sizeof(probe)) {
            memcpy(probe, line, (size_t)linelen);
            memcpy(probe + linelen, word, (size_t)wlen + 1);
        } else {
            continue;           /* absurdly long line; drop the word */
        }

        {
            /* Measured from the style in force at the start of the line:
             * bold is wider, and wrapping has to agree with drawing.
             *
             * And without the word's trailing space, which would be the
             * break itself and so costs nothing -- see width_run_rtrim. */
            mr_tstyle m = entry;
            pw = width_run_rtrim(f, probe, &m);
        }

        /* A word too wide for a line of its own has to be broken inside,
         * because the alternative is drawing past `width` -- and the caller
         * sized something to that width. A save slot is the case that bit:
         * the label comes from the game's `save_name`, and one without a
         * space in it ran clean across the neighbouring slots and the
         * navigation menu.
         *
         * Ren'Py's linebreak_greedy has the same hole -- with no split point
         * it lets x exceed the width (textsupport.pyx:578) -- so this is a
         * deliberate divergence, taken because painting on the next save is
         * worse than a break in an odd place.
         *
         * Only when the line is empty: a word that merely does not fit the
         * *remainder* of a line still moves down whole, below. */
        if (linelen == 0 && pw > width) {
            int nch = utf8_chars(word, wlen);
            int k, cut = 0;

            for (k = 1; k <= nch; k++) {
                int off = utf8_offset(word, wlen, k);
                char save = word[off];
                mr_tstyle m = entry;
                int w;

                word[off] = '\0';
                w = width_run(f, word, &m);
                word[off] = save;
                if (w > width)
                    break;
                cut = off;
            }
            /* At least one character, or a width narrower than a single
             * glyph would loop here for ever. */
            if (cut <= 0)
                cut = utf8_offset(word, wlen, 1);

            if (cut > 0 && cut < wlen) {
                memcpy(line, word, (size_t)cut);
                line[cut] = '\0';
                linelen = cut;
                utf8 -= wlen - cut;    /* `word` is a verbatim copy, so the
                                        * remainder is still in the source */
                FLUSH_LINE();
                y += f->line_height;
                lines++;
                line[0] = '\0';
                linelen = 0;
                continue;
            }
        }

        if (linelen == 0 || pw <= width) {
            memcpy(line, probe, (size_t)(linelen + wlen) + 1);
            linelen += wlen;
            continue;
        }

        /* It does not fit: emit the line, then start a new one with the
         * word that overflowed. */
        FLUSH_LINE();
        y += f->line_height;
        lines++;

        memcpy(line, word, (size_t)wlen + 1);
        linelen = wlen;
    }

#undef FLUSH_LINE

    return lines;
}

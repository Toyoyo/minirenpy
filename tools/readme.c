/*
 * README.EXE -- the DOS readme: a CP437 picture followed by readme.txt, in
 * VGA 80x50 text mode, through a pager.
 *
 * 16-bit real-mode DOS, small model, 8086 code. Build on Linux with the
 * Linux-hosted Open Watcom tools (INCLUDE=$WATCOM/h, 16-bit DOS libraries
 * installed):
 *
 *     python3 -I tools/mkreadme.py miwacol.bin miwacol.pal miwados/readme.txt \
 *         tools/readmdat.h
 *     wcl -zq -bt=dos -l=dos -ms -0 -os -w4 -fe=readme.exe tools/readme.c
 *
 * The picture is made by cp437.py with the adapt16 palette: blink is turned
 * off so backgrounds get all 16 colours, the attribute controller is set to
 * map colour i to DAC entry i, and the DAC is loaded with the picture's own
 * 16 colours. The text is drawn in that palette's lightest and darkest
 * colours, so one palette serves the whole document.
 *
 * The character clock is switched from 9 to 8 dots (640x400). cp437.py lays
 * the picture out for 640x400, and in 9-dot mode the VGA leaves the ninth
 * column blank for everything outside C0-DF, which puts a vertical seam
 * through every shade glyph (B0-B2).
 *
 * Without a colour VGA it prints the text to stdout and stops.
 *
 * Keys: Space/PgDn page, Enter/Down line, b/PgUp page back, Up line back,
 * Home/End, q/Esc quit. Space or Enter at the end quits, as more does.
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#include "readmdat.h"

#define COLS 80
#define MAX_LINES 2000

static unsigned line_off[MAX_LINES];
static unsigned char line_len[MAX_LINES];
static int nlines;
static int rows;

static unsigned char __far *vram;

static void bios_video(unsigned ax, unsigned bx, unsigned cx)
{
    union REGS r;

    r.w.ax = ax;
    r.w.bx = bx;
    r.w.cx = cx;
    int86(0x10, &r, &r);
}

static int have_colour_vga(void)
{
    union REGS r;

    r.w.ax = 0x1A00;                /* read display combination code */
    int86(0x10, &r, &r);
    return r.h.al == 0x1A && r.h.bl == 0x08;
}

static unsigned read_key(void)
{
    union REGS r;

    r.h.ah = 0x00;
    int86(0x16, &r, &r);
    return r.w.ax;
}

/* Split the text into screen lines, word-wrapping anything over 80. */
static void split_lines(void)
{
    unsigned i, start, brk;

    nlines = 0;
    start = 0;
    brk = 0;
    for (i = 0; i <= TXT_LEN && nlines < MAX_LINES; i++) {
        if (i == TXT_LEN || txt[i] == '\n') {
            if (i == TXT_LEN && i == start)
                break;              /* no empty line after the final newline */
            line_off[nlines] = start;
            line_len[nlines] = (unsigned char)(i - start);
            nlines++;
            start = i + 1;
            brk = 0;
        } else if (i - start == COLS) {
            if (brk == 0)
                brk = i;            /* no space in the line: cut it hard */
            line_off[nlines] = start;
            line_len[nlines] = (unsigned char)(brk - start);
            nlines++;
            start = brk;
            while (start < i && txt[start] == ' ')
                start++;
            brk = 0;
        } else if (txt[i] == ' ') {
            brk = i;
        }
    }
}

/* Document line n: the picture, one blank line, then the text. */
static void draw_line(int y, int n)
{
    unsigned char __far *p = vram + (unsigned)y * (COLS * 2);
    const unsigned char *s;
    int x, len;

    if (n < IMG_ROWS) {
        s = img_cells + (unsigned)n * (COLS * 2);
        for (x = 0; x < COLS * 2; x++)
            p[x] = s[x];
        return;
    }
    n -= IMG_ROWS + 1;
    len = 0;
    s = 0;
    if (n >= 0 && n < nlines) {
        s = txt + line_off[n];
        len = line_len[n];
    }
    for (x = 0; x < COLS; x++) {
        p[x * 2] = x < len ? s[x] : ' ';
        p[x * 2 + 1] = TXT_ATTR;
    }
}

static void draw_bar(int top, int total, int page)
{
    unsigned char __far *p = vram + (unsigned)(rows - 1) * (COLS * 2);
    char buf[128];                  /* longest message is 80; the loop clips */
    long pct;
    int x, n;

    pct = (long)(top + page) * 100L / total;
    if (pct > 100)
        pct = 100;
    if (top + page >= total)
        n = sprintf(buf, " -- End --  Space: quit  b/PgUp/Up: back  q: quit");
    else
        n = sprintf(buf, " -- More (%ld%%) --  Space/PgDn: page  Enter/Down: line"
                    "  b/PgUp/Up: back  q: quit", pct);
    for (x = 0; x < COLS; x++) {
        p[x * 2] = x < n ? buf[x] : ' ';
        p[x * 2 + 1] = BAR_ATTR;
    }
}

static void setup_video(void)
{
    unsigned char v;
    int i;

    bios_video(0x0003, 0, 0);       /* 80x25 colour text */
    bios_video(0x1112, 0x0000, 0);  /* 8x8 ROM font: 50 rows */
    bios_video(0x1003, 0x0000, 0);  /* blink off: 16 background colours */
    bios_video(0x0100, 0, 0x2000);  /* hide the cursor */

    /* 8-dot characters at 25.175 MHz instead of 9 at 28.322: the line rate
     * is unchanged, since the CRTC counts in characters. */
    outp(0x3C4, 0x00); outp(0x3C5, 0x01);           /* sequencer reset */
    outp(0x3C4, 0x01); v = (unsigned char)inp(0x3C5);
    outp(0x3C5, v | 0x01);                          /* 8-dot clocking */
    v = (unsigned char)inp(0x3CC);
    outp(0x3C2, v & ~0x0C);                         /* clock select 0 */
    outp(0x3C4, 0x00); outp(0x3C5, 0x03);           /* end reset */

    /* Attribute controller: colour i -> DAC i. Writing an index without
     * bit 5 blanks the screen until the final 0x20.
     *
     * Pel panning (AR13) is deliberately left at mode 3's 8. In 9-dot mode
     * 8 means no shift and 0 means one pixel; in 8-dot mode 0 is no shift
     * and 8 is undefined. Writing 0 was right on real hardware but DOSBox-X
     * still reads it the 9-dot way: the picture moved a pixel left and the
     * next row's first pixel column showed at x=639. Leaving 8 is clean on
     * both, tested on DOSBox-X and a real machine. */
    (void)inp(0x3DA);
    for (i = 0; i < 16; i++) {
        outp(0x3C0, i);
        outp(0x3C0, i);
    }
    outp(0x3C0, 0x20);

    outp(0x3C8, 0);
    for (i = 0; i < 48; i++)
        outp(0x3C9, img_pal[i]);

    rows = *(unsigned char __far *)MK_FP(0x40, 0x84) + 1;
    if (rows < 2)
        rows = 50;
}

static void restore_video(unsigned char mode, unsigned char old_rows)
{
    /* A mode set restores the font, the palette, blink and the 9-dot clock. */
    bios_video(mode, 0, 0);
    if (old_rows > 25)
        bios_video(0x1112, 0x0000, 0);
}

static void plain_dump(void)
{
    unsigned i;

    for (i = 0; i < TXT_LEN; i++)
        putchar(txt[i]);
    if (TXT_LEN && txt[TXT_LEN - 1] != '\n')
        putchar('\n');
}

int main(void)
{
    union REGS r;
    unsigned char old_mode, old_rows;
    unsigned key;
    int top, total, page, y, quit, last;

    split_lines();
    if (!have_colour_vga()) {
        plain_dump();
        return 0;
    }

    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    old_mode = r.h.al & 0x7F;
    old_rows = *(unsigned char __far *)MK_FP(0x40, 0x84) + 1;

    vram = (unsigned char __far *)MK_FP(0xB800, 0);
    setup_video();

    total = IMG_ROWS + 1 + nlines;
    page = rows - 1;
    last = total > page ? total - page : 0;
    top = 0;
    quit = 0;
    while (!quit) {
        for (y = 0; y < page; y++)
            draw_line(y, top + y);
        draw_bar(top, total, page);

        key = read_key();
        switch (key & 0xFF) {
        case 'q': case 'Q': case 0x1B:
            quit = 1;
            break;
        case ' ':
            if (top >= last)
                quit = 1;
            else
                top += page;
            break;
        case '\r':
            if (top >= last)
                quit = 1;
            else
                top++;
            break;
        case 'b': case 'B':
            top -= page;
            break;
        case 0x00: case 0xE0:       /* extended key: look at the scan code */
            switch (key >> 8) {
            case 0x50: top++; break;            /* Down */
            case 0x48: top--; break;            /* Up */
            case 0x51: top += page; break;      /* PgDn */
            case 0x49: top -= page; break;      /* PgUp */
            case 0x47: top = 0; break;          /* Home */
            case 0x4F: top = last; break;       /* End */
            }
            break;
        }
        if (top > last)
            top = last;
        if (top < 0)
            top = 0;
    }

    restore_video(old_mode, old_rows);
    return 0;
}

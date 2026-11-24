/* The VBE mode picker: decoding a ModeInfoBlock, and choosing from a list.
 *
 * Every mode here is a 256-byte block built field by field, the way a BIOS
 * would fill one in, and handed to the real mr_vbe_decode. There is no VESA
 * BIOS on this host and there never will be, which is exactly why the
 * decision was split out of the int 10h code: on the real machine this
 * policy runs once, before anything can draw, and a wrong answer is a black
 * screen with no log on it.
 *
 * The card tables below are not invented from the spec. They are the mode
 * lists real hardware reports, which is the only kind of input that matters:
 * a picker that handles the standard set and falls over on an S3's duplicate
 * entries is a picker that works here and not there.
 *
 * Takes nothing at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mr.h"
#include "vbe.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int g_fail;

/* mr_vbe_decode's refusal reason, which MODES.EXE prints on the target.
 * Checked below rather than ignored: a mode count of zero with no reason
 * is indistinguishable from reading the block wrong, and these strings are
 * the only thing that will be on screen when that happens. */
static const char *g_why;

static void check(const char *l, long got, long want)
{
    if (got != want) g_fail++;
    printf("%-56s %-8ld %s\n", l, got,
           got == want ? "ok" : "** WANT DIFFERENT **");
}

/* The string form. Prints the same "** WANT DIFFERENT **" marker as check()
 * rather than just the expected value: the suite is read by grepping for
 * that marker, and a failure that does not carry it is a failure that gets
 * counted and not seen. Two of the negative controls below went past
 * unnoticed that way. */
static void checks(const char *l, const char *got, const char *want)
{
    int ok;

    /* NULL-safe: g_why is NULL on success, and a refusal check that fired
     * when it should not have would otherwise take the whole harness down
     * inside strcmp rather than reporting a failure. */
    if (!got)
        got = "(none)";
    ok = !strcmp(got, want);
    if (!ok) g_fail++;
    printf("%-56s %-14s %s\n", l, got,
           ok ? "ok" : "** WANT DIFFERENT **");
    if (!ok)
        printf("%-56s %-14s (wanted)\n", "", want);
}

/* --- building a ModeInfoBlock ------------------------------------------- */

static unsigned char g_blk[MR_VBE_BLOCK];

static void put16(int off, int v)
{
    g_blk[off]     = (unsigned char)(v & 0xFF);
    g_blk[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(int off, unsigned long v)
{
    g_blk[off]     = (unsigned char)(v & 0xFF);
    g_blk[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    g_blk[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    g_blk[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

/* A direct-colour LFB mode, filled in the way a well-behaved VBE 2.0 BIOS
 * does. The sizes/positions are the standard layouts: 5:6:5 for 16, 5:5:5
 * for 15, 8:8:8 for 24 and 32. */
static void blk_direct(int w, int h, int bpp)
{
    int rs, gs, bs, xs;

    memset(g_blk, 0, sizeof(g_blk));
    put16(0x00, 0x0001 | 0x0008 | 0x0010 | 0x0080);  /* sup|col|gfx|lfb */
    put16(0x12, w);
    put16(0x14, h);
    g_blk[0x18] = 1;                                  /* planes */
    g_blk[0x19] = (unsigned char)bpp;
    g_blk[0x1B] = MR_VBE_DIRECT;
    put16(0x10, w * ((bpp + 7) / 8));                 /* bytes per scanline */
    put32(0x28, 0xE0000000UL);                        /* PhysBasePtr */

    switch (bpp) {
    case 15: rs = 5; gs = 5; bs = 5; xs = 1; break;
    case 16: rs = 5; gs = 6; bs = 5; xs = 0; break;
    case 24: rs = 8; gs = 8; bs = 8; xs = 0; break;
    default: rs = 8; gs = 8; bs = 8; xs = 8; break;   /* 32 */
    }
    g_blk[0x1F] = (unsigned char)rs;
    g_blk[0x20] = (unsigned char)(gs + bs);
    g_blk[0x21] = (unsigned char)gs;
    g_blk[0x22] = (unsigned char)bs;
    g_blk[0x23] = (unsigned char)bs;
    g_blk[0x24] = 0;
    g_blk[0x25] = (unsigned char)xs;
}

/* A palettised 8bpp LFB mode. */
static void blk_packed(int w, int h)
{
    memset(g_blk, 0, sizeof(g_blk));
    put16(0x00, 0x0001 | 0x0008 | 0x0010 | 0x0080);
    put16(0x12, w);
    put16(0x14, h);
    g_blk[0x18] = 1;
    g_blk[0x19] = 8;
    g_blk[0x1B] = MR_VBE_PACKED;
    put16(0x10, w);
    put32(0x28, 0xE0000000UL);
}

/* --- card mode lists ----------------------------------------------------- */

typedef struct { int number, w, h, bpp, packed; } card_entry;

/* Decode a whole card's worth into a list, the way vbedos.c will. */
static int build(const card_entry *e, int n, int version, mr_vbe_mode *out)
{
    int i, c = 0;

    for (i = 0; i < n; i++) {
        if (e[i].packed)
            blk_packed(e[i].w, e[i].h);
        else
            blk_direct(e[i].w, e[i].h, e[i].bpp);
        if (mr_vbe_decode(g_blk, e[i].number, version, &out[c], NULL))
            c++;
    }
    return c;
}

/* The standard VBE 2.0 set, which is what almost everything reports: the
 * 0x10x block at 8bpp and the 0x11x block at 15/16/24/32. No 16:9 anywhere
 * -- this is the case the letterbox policy exists for. */
static const card_entry CARD_STANDARD[] = {
    { 0x101, 640,  480,  8, 1 },
    { 0x103, 800,  600,  8, 1 },
    { 0x105, 1024, 768,  8, 1 },
    { 0x107, 1280, 1024, 8, 1 },
    { 0x110, 640,  480, 15, 0 },
    { 0x111, 640,  480, 16, 0 },
    { 0x112, 640,  480, 24, 0 },
    { 0x113, 800,  600, 15, 0 },
    { 0x114, 800,  600, 16, 0 },
    { 0x115, 800,  600, 24, 0 },
    { 0x116, 1024, 768, 15, 0 },
    { 0x117, 1024, 768, 16, 0 },
    { 0x118, 1024, 768, 24, 0 },
    { 0x119, 1280, 1024, 15, 0 },
    { 0x11A, 1280, 1024, 16, 0 },
    { 0x11B, 1280, 1024, 24, 0 }
};

/* The same card with a patched BIOS that adds the widescreen modes, which
 * is the configuration worth having and the one step 3 of the policy is
 * for. Numbers in the 0x14x range, where cards put their OEM modes. */
static const card_entry CARD_WIDE[] = {
    { 0x101, 640,  480,  8, 1 },
    { 0x111, 640,  480, 16, 0 },
    { 0x114, 800,  600, 16, 0 },
    { 0x117, 1024, 768, 16, 0 },
    { 0x11A, 1280, 1024, 16, 0 },
    { 0x143, 1280, 720, 16, 0 },
    { 0x144, 1280, 720, 32, 0 },
    { 0x145, 640,  360, 16, 0 }
};

int main(void)
{
    mr_vbe_mode list[64];
    mr_vbe_mode m;
    mr_vbe_want want;
    int n, i;

    setbuf(stdout, NULL);

    memset(&want, 0, sizeof(want));
    want.stage_w = MR_STAGE_W;
    want.stage_h = MR_STAGE_H;
    want.want_bpp = 16;
    want.allow_8bpp = 1;

    /* --- decoding one block --- */
    blk_direct(1024, 768, 16);
    check("a plain 1024x768x16 LFB mode decodes",
          mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why), 1);
    check("  width", m.w, 1024);
    check("  height", m.h, 768);
    check("  bpp", m.bpp, 16);
    check("  significant bpp", m.sigbpp, 16);
    check("  pitch", m.pitch, 2048);
    check("  model is direct colour", m.model, MR_VBE_DIRECT);
    check("  5:6:5 green is 6 bits at position 5",
          m.gsize == 6 && m.gpos == 5, 1);
    check("  physical base survives", m.phys == 0xE0000000UL, 1);

    /* 32bpp: 24 significant in 32 total. Both numbers are used -- one to
     * judge the picture, the other to stride the pack loop. */
    blk_direct(1280, 720, 32);
    mr_vbe_decode(g_blk, 0x144, 0x0200, &m, &g_why);
    check("32bpp is 24 significant bits", m.sigbpp, 24);
    check("  in a 32-bit pixel", m.bpp, 32);

    blk_direct(640, 480, 15);
    mr_vbe_decode(g_blk, 0x110, 0x0200, &m, &g_why);
    check("15bpp is 15 significant bits", m.sigbpp, 15);
    check("  in a 16-bit pixel", m.bpp, 16);

    /* --- the blocks that must be refused --- */
    blk_direct(1024, 768, 16);
    put16(0x00, 0x0008 | 0x0010 | 0x0080);          /* drop "supported" */
    check("a mode the hardware cannot do is refused",
          mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why), 0);

    blk_direct(1024, 768, 16);
    put16(0x00, 0x0001 | 0x0008 | 0x0010);          /* drop the LFB bit */
    check("a banked-only mode is refused",
          mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why), 0);

    blk_direct(1024, 768, 16);
    put16(0x00, 0x0001 | 0x0008 | 0x0080);          /* drop "graphics" */
    check("a text mode is refused",
          mr_vbe_decode(g_blk, 0x108, 0x0200, &m, &g_why), 0);

    blk_direct(1024, 768, 16);
    put16(0x00, 0x0001 | 0x0010 | 0x0080);          /* drop "colour" */
    check("a monochrome mode is refused",
          mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why), 0);

    blk_direct(1024, 768, 16);
    g_blk[0x1B] = 3;                                 /* planar */
    check("a planar mode is refused",
          mr_vbe_decode(g_blk, 0x102, 0x0200, &m, &g_why), 0);

    blk_direct(1024, 768, 16);
    g_blk[0x18] = 4;                                 /* 4 planes */
    check("  and so is one claiming packed with four planes",
          mr_vbe_decode(g_blk, 0x102, 0x0200, &m, &g_why), 0);

    /* VBE 1.2 has no PhysBasePtr: those four bytes are something else, and
     * believing them hands the presenter an address to scribble on. */
    blk_direct(1024, 768, 16);
    check("a VBE 1.2 BIOS yields nothing, whatever the bits say",
          mr_vbe_decode(g_blk, 0x117, 0x0102, &m, &g_why), 0);

    blk_direct(1024, 768, 16);
    put32(0x28, 0);
    check("a mode with no framebuffer address is refused",
          mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why), 0);

    /* A 4bpp pixel is not a whole number of bytes and the pack loop has no
     * path for it. Refused rather than mis-decoded. */
    blk_direct(640, 480, 4);
    check("a sub-byte pixel is refused",
          mr_vbe_decode(g_blk, 0x012, 0x0200, &m, &g_why), 0);

    /* A pitch that cannot hold a row is firmware nonsense. */
    blk_direct(1024, 768, 16);
    put16(0x10, 1000);
    check("a pitch too small for a row is refused",
          mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why), 0);

    /* --- every refusal names itself ---
     *
     * These strings are what MODES.EXE prints beside a skipped mode, and on
     * the target they are the whole diagnostic: "0 of 47 modes usable" says
     * nothing, "banked only, no linear framebuffer" on all 47 says the card
     * is VBE 1.2 and "pitch too small for a row" on all 47 says we are
     * reading the block at the wrong offsets. Checked so that a reason
     * cannot silently become the wrong one, and so that a new refusal
     * cannot be added without one. */
    blk_direct(1024, 768, 16);
    put16(0x00, 0x0001 | 0x0008 | 0x0010);
    mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why);
    checks("a refusal says why", g_why, "banked only, no linear framebuffer");

    blk_direct(1024, 768, 16);
    mr_vbe_decode(g_blk, 0x117, 0x0102, &m, &g_why);
    checks("  VBE 1.2", g_why, "VBE below 2.0, no PhysBasePtr");

    blk_direct(1024, 768, 16);
    put16(0x10, 1000);
    mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why);
    checks("  a short pitch", g_why, "pitch too small for a row");

    blk_direct(1024, 768, 16);
    g_blk[0x1B] = 3;
    mr_vbe_decode(g_blk, 0x102, 0x0200, &m, &g_why);
    checks("  a planar mode", g_why,
           "memory model is neither packed nor direct");

    /* And a mode that is fine leaves no reason behind, so a caller that
     * prints *why unconditionally cannot print a stale one. */
    blk_direct(1024, 768, 16);
    mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why);
    check("a mode that is accepted leaves no reason", g_why == NULL, 1);

    /* --- the two firmware quirks --- */

    /* VIDMODES.ASM's: RsvdMaskSize left at zero on a mode that is really
     * 32bpp. sig+rsvd says 24, BitsPerPixel says 32, and the larger is
     * right -- stride by 24 bits through a 32-bit framebuffer and the
     * picture shears diagonally. */
    blk_direct(1024, 768, 32);
    g_blk[0x25] = 0;
    check("a blank reserved field does not shrink the pixel",
          mr_vbe_decode(g_blk, 0x118, 0x0200, &m, &g_why) && m.bpp == 32, 1);
    check("  while the significant depth stays 24", m.sigbpp, 24);

    /* VBE 3.0's separate linear pitch. A card can pad differently in the
     * two addressing modes; using the banked figure shears every row after
     * the first. */
    blk_direct(1024, 768, 16);
    put16(0x10, 2048);          /* banked */
    put16(0x32, 2560);          /* linear, padded */
    mr_vbe_decode(g_blk, 0x117, 0x0300, &m, &g_why);
    check("VBE 3.0 uses the linear pitch", m.pitch, 2560);
    mr_vbe_decode(g_blk, 0x117, 0x0200, &m, &g_why);
    check("  and 2.0 has only the one", m.pitch, 2048);

    /* A 3.0 card that leaves the linear pitch blank falls back rather than
     * ending up with a zero stride. */
    blk_direct(1024, 768, 16);
    put16(0x10, 2048);
    put16(0x32, 0);
    mr_vbe_decode(g_blk, 0x117, 0x0300, &m, &g_why);
    check("  a blank linear pitch falls back to the banked one",
          m.pitch, 2048);

    /* --- picking, on the standard 4:3-only card --- */
    {
        char name[32];

        n = build(CARD_STANDARD, (int)(sizeof(CARD_STANDARD) /
                                       sizeof(CARD_STANDARD[0])),
                  0x0200, list);
        check("the standard card yields every mode", n, 16);

        i = mr_vbe_pick(list, n, &want);
        check("something was chosen", i >= 0, 1);

        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        /* 1280x1024 shows the stage at 1280x720, which is the full picture
         * and 1:1 -- no scaling at all. 1024x768 would only manage
         * 1024x576. The letterbox is 304 wasted rows and worth it. */
        checks("  the biggest picture wins, not the biggest mode",
               name, "1280x1024x16");

        /* The depth preference is real but subordinate. */
        want.want_bpp = 24;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("  asking for 24bpp moves the depth, not the size",
               name, "1280x1024x24");

        want.want_bpp = 16;
    }

    /* --- the same card with the widescreen modes patched in --- */
    {
        char name[32];

        n = build(CARD_WIDE, (int)(sizeof(CARD_WIDE) / sizeof(CARD_WIDE[0])),
                  0x0200, list);
        check("the patched card yields every mode", n, 8);

        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        /* 1280x720 and 1280x1024 show the identical picture. The 16:9 mode
         * wins on step 3: the other spends 389,120 pixels a frame painting
         * bars. This is the check that makes patching a BIOS worthwhile. */
        checks("a 16:9 mode beats a 4:3 one showing the same picture",
               name, "1280x720x16");

        want.want_bpp = 32;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("  and the depth preference still chooses between them",
               name, "1280x720x32");
        want.want_bpp = 16;
    }

    /* --- colour class outranks resolution --- */
    {
        static const card_entry CARD_MIXED[] = {
            { 0x105, 1024, 768,  8, 1 },   /* big, palettised */
            { 0x111, 640,  480, 16, 0 }    /* small, truecolour */
        };
        char name[32];

        n = build(CARD_MIXED, 2, 0x0200, list);
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("truecolour beats a larger palettised mode",
               name, "640x480x16");

        /* ...unless the ini asked for a palettised depth. This is the one
         * key above the colour class, and without it `depth=8` picks the
         * 16bpp mode here -- the same one no setting at all picks, which
         * is to say it does nothing. */
        want.want_bpp = 8;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("  but depth=8 asks for palettised and gets it",
               name, "1024x768x8");

        /* And refusing palettised modes still beats asking for one: they
         * are removed from the list rather than ranked last. */
        want.allow_8bpp = 0;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("  with truecolour=1 the request has nothing to choose",
               name, "640x480x16");
        want.allow_8bpp = 1;
        want.want_bpp = 16;

        /* ...and with truecolour refused outright, the 8bpp mode is taken
         * rather than nothing. A dithered picture beats no picture. */
        {
            static const card_entry CARD_8ONLY[] = {
                { 0x105, 1024, 768, 8, 1 },
                { 0x101, 640,  480, 8, 1 }
            };
            n = build(CARD_8ONLY, 2, 0x0200, list);
            i = mr_vbe_pick(list, n, &want);
            sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
            checks("  an 8bpp-only card still gets its best mode",
                   name, "1024x768x8");

            want.allow_8bpp = 0;
            check("  unless palettised modes are refused",
                  mr_vbe_pick(list, n, &want), -1);
            want.allow_8bpp = 1;
        }
    }

    /* --- the S3 under DOSBox-X, from a real run of MODES.EXE ---
     *
     * Trimmed to the modes that can win, but the shape is the one that
     * matters: the card offers 1152x864 at 16bpp *twice*, as 5:5:5 (209)
     * and as 5:6:5 (20a), and its only 1:1 mode is palettised.
     *
     * This is where the first version of the policy went wrong. 209 and
     * 20a tie on every key through depth, so the choice fell to the lower
     * mode number and took the 5:5:5 one -- half the colours, for free.
     */
    {
        static const card_entry CARD_S3[] = {
            { 0x107, 1280, 1024,  8, 1 },   /* the only 1:1 mode, 8bpp */
            { 0x117, 1024, 768,  16, 0 },
            { 0x115, 800,  600,  32, 0 },
            { 0x209, 1152, 864,  15, 0 },   /* 5:5:5 */
            { 0x20A, 1152, 864,  16, 0 }    /* 5:6:5, same cost */
        };
        char name[40];

        n = build(CARD_S3, 5, 0x0200, list);
        check("the S3 list decodes", n, 5);

        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%03x %dx%dx%d/%d", list[i].number, list[i].w,
                list[i].h, list[i].bpp, list[i].sigbpp);
        checks("5:6:5 beats 5:5:5 at the same size and cost",
               name, "20a 1152x864x16/16");

        /* And the judgement call, pinned so that changing it is deliberate:
         * 1280x1024x8 is the only mode on this card that shows the stage
         * 1:1 with no scaling at all, and it is still not chosen, because
         * dithering into a 6x6x6 cube costs more than scaling does. */
        check("  the 1:1 palettised mode is still refused",
              list[i].number != 0x107, 1);

        want.allow_8bpp = 0;
        i = mr_vbe_pick(list, n, &want);
        check("  and refusing 8bpp outright changes nothing here",
              list[i].number, 0x20A);
        want.allow_8bpp = 1;
    }

    /* --- the Matrox Mystique, from a real run on the hardware ---
     *
     * The shape that matters: this card's truecolour modes stop at
     * 800x600, while 1024x768 is available only at 16bpp. So the two
     * candidates are 1024x576 of picture at 16bpp and 800x450 at 32bpp,
     * and the default policy takes the first -- more picture for fewer
     * bytes a frame, 1.18MB against 1.44MB, which is the right default on
     * a Pentium III.
     *
     * The second half is why `depth` moved above resolution. While it was
     * only a tiebreak, *every* value of it chose the same mode here,
     * because the keys above it were already decided. A setting that
     * cannot change the outcome on a real card is not a setting.
     */
    {
        static const card_entry CARD_MYSTIQUE[] = {
            { 0x101, 640,  480,  8, 1 },
            { 0x103, 800,  600,  8, 1 },
            { 0x105, 1024, 768,  8, 1 },
            { 0x107, 1280, 1024, 8, 1 },
            { 0x110, 640,  480, 15, 0 },
            { 0x111, 640,  480, 16, 0 },
            { 0x112, 640,  480, 32, 0 },
            { 0x113, 800,  600, 15, 0 },
            { 0x114, 800,  600, 16, 0 },
            { 0x115, 800,  600, 32, 0 },
            { 0x116, 1024, 768, 15, 0 },
            { 0x117, 1024, 768, 16, 0 }    /* and no 1024x768x32 */
        };
        char name[32];

        n = build(CARD_MYSTIQUE, 12, 0x0200, list);
        check("the Mystique list decodes", n, 12);

        want.want_bpp = 0;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("with no depth asked, the bigger picture wins over the "
               "deeper pixel", name, "1024x768x16");

        /* 5:5:5 is available at the same size and loses on significant
         * bits, which is the S3 check again on different hardware. */
        check("  and 5:6:5 beats 5:5:5 there too", list[i].number, 0x117);

        want.want_bpp = 32;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("  asking for 32bpp gives up the resolution to get it",
               name, "800x600x32");

        /* 24 is not on offer at all: 16 and 32 are equally far from it, so
         * the tie falls through to picture and the bigger mode wins. That
         * is the intended reading of "nearest", not a special case. */
        want.want_bpp = 24;
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        checks("  a depth the card lacks falls back to the policy",
               name, "1024x768x16");

        want.want_bpp = 16;
    }

    /* --- a mode larger than the stage is worth no more than the stage --- */
    {
        static const card_entry CARD_HUGE[] = {
            { 0x143, 1280, 720,  16, 0 },
            { 0x180, 1600, 900,  16, 0 },
            { 0x182, 1920, 1080, 16, 0 }
        };
        char name[32];

        n = build(CARD_HUGE, 3, 0x0200, list);
        i = mr_vbe_pick(list, n, &want);
        sprintf(name, "%dx%dx%d", list[i].w, list[i].h, list[i].bpp);
        /* All three show the whole stage. None can show more of it, and the
         * larger ones cost more bus and more scaling for an identical
         * image, so the clamp has to stop the policy chasing resolution. */
        checks("a mode past the stage gains nothing and is not chosen",
               name, "1280x720x16");
    }

    /* --- the degenerate cases --- */
    check("an empty list picks nothing", mr_vbe_pick(list, 0, &want), -1);
    check("a null list picks nothing", mr_vbe_pick(NULL, 4, &want), -1);
    {
        mr_vbe_want bad = want;
        bad.stage_w = 0;
        n = build(CARD_WIDE, 8, 0x0200, list);
        check("a zero stage picks nothing", mr_vbe_pick(list, n, &bad), -1);
    }
    check("a null block does not decode",
          mr_vbe_decode(NULL, 0x117, 0x0200, &m, &g_why), 0);

    /* --- a duplicated mode is resolved the same way every run --- */
    {
        static const card_entry CARD_DUP[] = {
            { 0x144, 1280, 720, 16, 0 },
            { 0x143, 1280, 720, 16, 0 }   /* same mode, lower number */
        };
        n = build(CARD_DUP, 2, 0x0200, list);
        i = mr_vbe_pick(list, n, &want);
        check("a duplicate mode resolves to the lower number",
              list[i].number, 0x143);
    }

    /* --- the fit the picker scored is the fit the presenter will use --- */
    {
        int pw = 0, ph = 0;

        mr_vbe_fit(1024, 768, MR_STAGE_W, MR_STAGE_H, &pw, &ph);
        check("1024x768 shows a 1024x576 picture", pw == 1024 && ph == 576, 1);
        mr_vbe_fit(1280, 1024, MR_STAGE_W, MR_STAGE_H, &pw, &ph);
        check("1280x1024 shows it 1:1", pw == 1280 && ph == 720, 1);
        mr_vbe_fit(640, 480, MR_STAGE_W, MR_STAGE_H, &pw, &ph);
        check("640x480 shows a 640x360 picture", pw == 640 && ph == 360, 1);
        mr_vbe_fit(1280, 720, MR_STAGE_W, MR_STAGE_H, &pw, &ph);
        check("1280x720 wastes nothing", pw == 1280 && ph == 720, 1);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

/* vbe.c - decoding a VBE ModeInfoBlock, and choosing a mode.
 *
 * Pure: bytes in, a decision out. See vbe.h for the policy and why it is
 * here rather than in the DOS half.
 */

#include <string.h>

#include "mr.h"
#include "vbe.h"

/* ModeInfoBlock field offsets, VBE 3.0 section 4.4. Named rather than
 * written as literals at the point of use, because every one of them is a
 * magic number and a transposed pair would decode a plausible-looking mode
 * with the wrong geometry. */
#define OFF_ATTRIBUTES   0x00   /* word */
#define OFF_BYTES_PER_SL 0x10   /* word */
#define OFF_XRES         0x12   /* word */
#define OFF_YRES         0x14   /* word */
#define OFF_PLANES       0x18   /* byte */
#define OFF_BPP          0x19   /* byte */
#define OFF_MEMORY_MODEL 0x1B   /* byte */
#define OFF_RED_SIZE     0x1F   /* byte */
#define OFF_RED_POS      0x20   /* byte */
#define OFF_GREEN_SIZE   0x21   /* byte */
#define OFF_GREEN_POS    0x22   /* byte */
#define OFF_BLUE_SIZE    0x23   /* byte */
#define OFF_BLUE_POS     0x24   /* byte */
#define OFF_RSVD_SIZE    0x25   /* byte */
#define OFF_PHYS_BASE    0x28   /* dword, VBE 2.0+ */
#define OFF_LIN_BYTES_SL 0x32   /* word,  VBE 3.0+ */

/* ModeAttributes bits, table 4.9. */
#define ATTR_SUPPORTED   0x0001  /* mode supported by this hardware */
#define ATTR_COLOUR      0x0008  /* colour, not monochrome */
#define ATTR_GRAPHICS    0x0010  /* graphics, not text */
#define ATTR_LFB         0x0080  /* a linear framebuffer is available */

static int rd16(const unsigned char *p, int off)
{
    return (int)p[off] | ((int)p[off + 1] << 8);
}

static unsigned long rd32(const unsigned char *p, int off)
{
    return (unsigned long)p[off]
         | ((unsigned long)p[off + 1] << 8)
         | ((unsigned long)p[off + 2] << 16)
         | ((unsigned long)p[off + 3] << 24);
}

/* One place to set *why and return, so a new check cannot forget to. */
#define REFUSE(reason) do { if (why) *why = (reason); return 0; } while (0)

int mr_vbe_decode(const unsigned char *block, int number, int vbe_version,
                  mr_vbe_mode *out, const char **why)
{
    mr_vbe_mode m;
    int attr, sig;

    if (why)
        *why = NULL;

    if (!block || !out)
        REFUSE("no block");

    memset(&m, 0, sizeof(m));
    m.number = number;

    attr = rd16(block, OFF_ATTRIBUTES);

    /* The three checks VIDMODES.ASM makes, in its order. "Supported" is not
     * redundant with being in the list: the mode list is what the BIOS
     * knows about, and this bit is whether the card in the machine can
     * actually do it -- the same BIOS image ships on boards with different
     * amounts of memory. */
    if (!(attr & ATTR_SUPPORTED))
        REFUSE("hardware does not support it");
    if (!(attr & ATTR_GRAPHICS))
        REFUSE("text mode");
    if (!(attr & ATTR_COLOUR))
        REFUSE("monochrome");

    /* And the one it does not make, because a Windows 3.1 display driver
     * can bank-switch and we will not. Banked output means recomputing the
     * window for every scanline that crosses a 64K boundary, in the middle
     * of the one loop in the engine that has to be fast. VBE 2.0 is from
     * 1994 and the target is a Pentium III. */
    if (!(attr & ATTR_LFB))
        REFUSE("banked only, no linear framebuffer");

    /* PhysBasePtr is a VBE 2.0 addition. A 1.2 BIOS leaves those four bytes
     * as something else entirely, so believing them would hand the
     * presenter a garbage address -- and a write to a garbage physical
     * address on a machine with no memory protection is not a crash, it is
     * a corrupted something-else. */
    if (vbe_version < 0x0200)
        REFUSE("VBE below 2.0, no PhysBasePtr");

    m.model = block[OFF_MEMORY_MODEL];
    if (m.model != MR_VBE_PACKED && m.model != MR_VBE_DIRECT)
        REFUSE("memory model is neither packed nor direct");

    /* Planar modes report NumberOfPlanes > 1 and are rejected above by the
     * memory model; this catches a BIOS that says "packed pixel" and then
     * contradicts itself, which is cheaper to check than to debug. */
    if (block[OFF_PLANES] != 1)
        REFUSE("more than one plane");

    m.w = rd16(block, OFF_XRES);
    m.h = rd16(block, OFF_YRES);
    if (m.w <= 0 || m.h <= 0)
        REFUSE("no resolution");

    m.bpp = block[OFF_BPP];

    /* Significant depth, and the quirk worth stealing from VIDMODES.ASM.
     *
     * For a packed-pixel mode the pixel is a DAC index and the masks mean
     * nothing, so the depth is the depth. For a direct-colour mode the
     * significant depth is red+green+blue and the total adds the reserved
     * field -- except that some firmware leaves the reserved size at zero
     * and expects the total to be read from BitsPerPixel instead. Taking
     * whichever is larger covers both, and is what VIDMODES.ASM does
     * (`cmp bl,al / jae`).
     *
     * This is not a theoretical tidy-up: get it wrong on a 32bpp mode that
     * reports RsvdMaskSize 0 and the pack loop strides by 24 bits through a
     * 32-bit framebuffer, which produces a recognisable diagonal smear and
     * nothing in the log. */
    if (m.model == MR_VBE_PACKED) {
        m.sigbpp = m.bpp;
    } else {
        int total;

        m.rsize = block[OFF_RED_SIZE];
        m.rpos  = block[OFF_RED_POS];
        m.gsize = block[OFF_GREEN_SIZE];
        m.gpos  = block[OFF_GREEN_POS];
        m.bsize = block[OFF_BLUE_SIZE];
        m.bpos  = block[OFF_BLUE_POS];

        sig = m.rsize + m.gsize + m.bsize;
        if (sig <= 0)
            REFUSE("direct colour with no colour fields");
        m.sigbpp = sig;

        total = sig + block[OFF_RSVD_SIZE];
        if (m.bpp > total)
            total = m.bpp;
        m.bpp = total;
    }

    /* What the pack loop can actually write. 15bpp lives in a 16-bit pixel
     * and is fine; anything whose pixel is not a whole number of bytes is
     * not, and there is no reason to grow the presenter a bit-banging path
     * for a mode no card has offered since the ISA era. */
    if (m.bpp != 8 && m.bpp != 16 && m.bpp != 24 && m.bpp != 32)
        REFUSE("pixel is not a whole number of bytes");
    if (m.model == MR_VBE_PACKED && m.bpp != 8)
        REFUSE("packed pixel but not 8bpp");

    /* The pitch in *linear* mode, which is not always the banked one: a
     * card can pad a scanline differently in the two. VBE 3.0 added a
     * separate field for it; before that there is only the one. Trusting
     * the banked figure on a 3.0 card is how every row after the first
     * lands a few pixels to the left of where it should, and the picture
     * shears. */
    if (vbe_version >= 0x0300 && rd16(block, OFF_LIN_BYTES_SL) > 0)
        m.pitch = rd16(block, OFF_LIN_BYTES_SL);
    else
        m.pitch = rd16(block, OFF_BYTES_PER_SL);

    /* A BIOS that reports no pitch at all gets the unpadded one, which is
     * right far more often than zero is. */
    if (m.pitch <= 0)
        m.pitch = m.w * (m.bpp / 8);
    if (m.pitch < m.w * (m.bpp / 8))
        REFUSE("pitch too small for a row");

    m.phys = rd32(block, OFF_PHYS_BASE);
    if (m.phys == 0)
        REFUSE("no framebuffer address");

    *out = m;
    return 1;
}

void mr_vbe_fit(int mode_w, int mode_h, int stage_w, int stage_h,
                int *pic_w, int *pic_h)
{
    int w, h;

    /* The same arithmetic as mr_fit_set, and it has to stay the same: the
     * picker scores a mode on the picture it can show, and the presenter
     * then has to show exactly that. Not a call to mr_fit_set, because that
     * one *sets* the live fit and this is a question about a mode we may
     * not choose. */
    w = mode_w;
    h = (int)((long)w * stage_h / stage_w);
    if (h > mode_h) {
        h = mode_h;
        w = (int)((long)h * stage_w / stage_h);
    }

    *pic_w = w;
    *pic_h = h;
}

/* The score for one mode, as the keys in vbe.h. Compared
 * lexicographically by better_than below rather than squashed into a single
 * number -- a weighted sum needs weights, the weights need to not overflow
 * against each other, and getting that wrong produces a policy nobody
 * intended and nobody can read back out. */
typedef struct {
    int palettised;   /* key 1: 0 truecolour, 1 palettised. Lower wins. */
    int  depth_asked; /* was a depth named? If so depth_miss moves to key 2 */
    int  shallow_asked; /* ...and if that depth was a palettised one, key 0 */
    int  depth_miss;  /* key 2: |bpp - want|, only when one was asked for. */
    long picture;     /* key 3: fitted area, clamped. Higher wins. */
    long wasted;      /* key 4: letterbox pixels. Lower wins. */
    int  sigbpp;      /* key 5: significant bits. Higher wins. */
    int  number;      /* key 6: mode number. Lower wins. */
} vbe_score;

static void score_of(const mr_vbe_mode *m, const mr_vbe_want *w, vbe_score *s)
{
    int pw, ph;
    long area, cap;

    s->palettised = (m->sigbpp < 15);

    mr_vbe_fit(m->w, m->h, w->stage_w, w->stage_h, &pw, &ph);
    area = (long)pw * ph;

    /* Clamped at the stage: a 1600x900 mode shows the stage 1:1 with room
     * to spare and is worth no more than 1280x720 is, because there is no
     * more picture to show. Without the clamp the policy would chase
     * resolution forever and pick the largest mode the card has, which on
     * the target means a slower frame for an identical image. */
    cap = (long)w->stage_w * w->stage_h;
    if (area > cap)
        area = cap;
    s->picture = area;

    s->wasted = (long)m->w * m->h - (long)pw * ph;

    /* An explicit `depth` outranks resolution; the default does not.
     *
     * Left to itself the policy puts picture size above depth, which is
     * the right default -- but not for the reason this comment used to
     * give, and the correction is worth keeping.
     *
     * The old argument was bandwidth: a card with 1024x768 at 16bpp but
     * only 800x600 at 32 shows more picture for fewer bytes a frame in the
     * shallower mode. True of that pair, and not true of the mode the
     * policy actually picks on the S3 this engine is tested against. There
     * it takes 1152x864x16 -- a 1152x648 picture, 1,492,992 bytes a frame
     * against 800x600x32's 1,440,000. Slightly *more* bus traffic, and
     * 746,496 pixels through the scaler and the pack loop against 360,000,
     * so a little over twice the per-pixel work. The shallow mode is not
     * cheaper here; it is dearer.
     *
     * What makes it right anyway is the scale factor, which is an argument
     * about text rather than about bytes. The stage is 1280x720, so those
     * two modes are a 0.9 downscale and a 0.625 one, and a visual novel is
     * mostly anti-aliased text. 0.625 is where text starts to mush. Twice
     * the picture at 0.9 is worth the frame time; 65536 colours is enough
     * for a painted background.
     *
     * The cost that does not show up in either count: pack16 truncates and
     * does not dither (pack.c), so 5:6:5 can band on a gradient where
     * 32bpp would not. That is what `depth=32` is for.
     *
     * But someone who writes
     * `depth=32` in the ini has said what they want, and a setting that
     * only breaks ties nothing else decided does nothing at all on most
     * real mode lists -- which is what it did on the Matrox: every value
     * of `depth` chose the same mode. A key that silently has no effect
     * is worse than one that costs resolution on purpose. */
    /* And a depth below the truecolour threshold outranks the colour class
     * as well, which is the only way to ask for a palettised mode on a card
     * that also offers truecolour ones. Without this `depth=8` is inert:
     * key 1 has already eliminated every 8bpp mode before the depth key is
     * read, so it quietly selects the *shallowest truecolour* mode instead
     * -- on the Mystique's list, the same mode no setting at all chooses.
     * A key that works for 16, 24 and 32 and silently does nothing for 8 is
     * worse than one that costs colour on purpose, which is the same
     * argument that put depth above resolution one line below.
     *
     * 15 rather than 8 so the threshold is the one `palettised` uses; they
     * have to agree or this reorders a comparison it cannot then win. */
    s->depth_asked = (w->want_bpp > 0);
    s->shallow_asked = (w->want_bpp > 0 && w->want_bpp < 15);
    s->depth_miss = w->want_bpp > 0 ? m->bpp - w->want_bpp : 0;
    if (s->depth_miss < 0)
        s->depth_miss = -s->depth_miss;

    /* Among modes that cost the same, more colour.
     *
     * This exists because of a real mode list. An S3 under DOSBox-X offers
     * 1152x864 at 16bpp twice: mode 209 is 5:5:5 with a wasted bit and 20a
     * is 5:6:5. Identical size, identical pixel, identical bandwidth -- so
     * every key above ties, and the choice fell through to "lower mode
     * number", which picked 209. Thirty-two thousand colours instead of
     * sixty-five thousand, for nothing.
     *
     * Below depth_miss rather than above it: that one compares the pixel
     * the pack loop has to write against what the ini asked for, which is a
     * bandwidth decision, and this only refines between equals. */
    s->sigbpp = m->sigbpp;

    s->number = m->number;
}

static int better_than(const vbe_score *a, const vbe_score *b)
{
    /* Key 0, and it exists only for `depth=8`: an explicitly palettised
     * depth is decided before the colour class, so a mode that matches it
     * is not thrown out for being palettised. Unset -- which is every run
     * that does not say so in mrpy.ini -- and the order below is exactly
     * what ran before. */
    if (a->shallow_asked && a->depth_miss != b->depth_miss)
        return a->depth_miss < b->depth_miss;

    if (a->palettised != b->palettised) return a->palettised < b->palettised;

    /* Both scores come from the same `want`, so one flag answers for the
     * pair. When a depth was named it is decided here, above resolution;
     * when it was not, depth_miss is zero everywhere and this step does
     * nothing.
     *
     * There is no second depth step further down. There used to be: the
     * original policy ranked depth *below* resolution, and when `depth`
     * was promoted to here the lower one was left in place. It cannot fire
     * either way -- with no depth asked every depth_miss is zero, and with
     * one asked this line has already returned on any difference -- so it
     * was two comparisons that read like policy and decided nothing. */
    if (a->depth_asked && a->depth_miss != b->depth_miss)
        return a->depth_miss < b->depth_miss;

    if (a->picture    != b->picture)    return a->picture    > b->picture;
    if (a->wasted     != b->wasted)     return a->wasted     < b->wasted;
    if (a->sigbpp     != b->sigbpp)     return a->sigbpp     > b->sigbpp;
    return a->number < b->number;
}

int mr_vbe_pick(const mr_vbe_mode *list, int n, const mr_vbe_want *want)
{
    vbe_score best;
    int besti = -1;
    int i;

    if (!list || n <= 0 || !want)
        return -1;
    if (want->stage_w <= 0 || want->stage_h <= 0)
        return -1;

    memset(&best, 0, sizeof(best));

    for (i = 0; i < n; i++) {
        vbe_score s;

        if (!want->allow_8bpp && list[i].sigbpp < 15)
            continue;

        score_of(&list[i], want, &s);

        if (besti < 0 || better_than(&s, &best)) {
            best = s;
            besti = i;
        }
    }

    return besti;
}

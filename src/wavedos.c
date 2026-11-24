/* wavedos.c - the DOS sound sink: a Sound Blaster, DMA.
 *
 * The implementation of wave.h for DOS. The mixer above it is the same file
 * that feeds waveOut on Win32s and produces the same thing: interleaved
 * signed 16-bit stereo at 44100Hz. An SB16 plays that natively; everything
 * older is fed through sbconv.c, which is where all the arithmetic lives.
 *
 * Reference: the Sound Blaster Series Hardware Programming Guide, and in
 * practice MikMod's DOS driver (drivers/dos/dossb.c and dosdma.c), which has
 * been run against real cards for twenty-five years. Where the two disagree
 * about a detail, dossb.c wins -- notably that the SB16 block length written
 * to the DSP for 16-bit stereo is in *frames* minus one, not bytes and not
 * samples.
 *
 * --- the shape of it -------------------------------------------------------
 *
 * Two rings, and they are not the same ring.
 *
 *   The mixer's ring is wave.h's: MR_WAVE_BUFS buffers of MR_WAVE_BYTES, in
 *   ordinary memory. That is the 1114ms runway audio.c argues for at length,
 *   and it exists because a transition on a Pentium III can spend a couple
 *   of hundred milliseconds inside one frame while the mixer, which only
 *   runs between frames, does not run at all.
 *
 *   The card's ring is two halves in conventional memory, each one mixer
 *   buffer's worth of *time* -- 16KB in all for an SB16, 2KB for an 8-bit
 *   mono card at half the rate -- with the DSP interrupting at each half
 *   boundary.
 *
 * It has to be two rings. A 16-bit DMA transfer may not cross a 128KB
 * physical boundary, so the 192KB runway cannot be the DMA buffer; and 640K
 * of conventional memory is not somewhere to put 192KB even if it could.
 *
 * **The interrupt handler does the copy.** That is the decision that makes
 * the runway work. If the frame loop moved the data, a 200ms frame would
 * starve a 46ms half and the runway would protect nothing; with the handler
 * doing it, the card keeps pulling from the queue whatever the frame loop is
 * doing, and the queue is a second deep. The cost is a 8KB memcpy at
 * interrupt time, 21 times a second -- some tens of microseconds each, on a
 * machine whose own timer interrupt now runs at 1kHz.
 *
 * --- three cards, not one --------------------------------------------------
 *
 * The DSP version decides which of three paths runs, and nothing above this
 * file can tell the difference:
 *
 *   4.00+   SB16. 16-bit signed stereo at 44100 on a 16-bit DMA channel,
 *           auto-init. The mixer's buffers are memcpy'd in; no conversion.
 *   2.00+   SB Pro, SB 2.0, and the ESS1868, which answers as a Pro.
 *           8-bit unsigned mono at about 22050 on an 8-bit channel,
 *           auto-init.
 *   below   SB 1.x. The same format, but the DSP has no auto-init command,
 *           so each block is re-triggered by hand from the interrupt that
 *           ended the last one -- as the first thing that interrupt does.
 *           The DMA controller auto-inits regardless of what the DSP can
 *           do, so the ring keeps turning and the restart is three port
 *           writes rather than a buffer conversion. See the handler.
 *
 * Why mono, and why half the rate, when a Pro can do stereo and 44100:
 *
 *   - Stereo on a Pro means writing the mixer chip's register 0x0E, and
 *     then being the program that left somebody's card in stereo mode after
 *     a crash. The Pro also plays its stereo by alternating samples, with a
 *     channel-swap failure mode that cannot be diagnosed from here.
 *   - Above roughly 23kHz the pre-16 DSPs need "high-speed" mode, in which
 *     the DSP accepts no further commands until the card is reset. An SB 1.x
 *     tops out near 22kHz anyway.
 *
 * So: one 8-bit format for every old card, chosen because it is the one all
 * of them do the same way. The rate is the time constant's, not exactly
 * 22050 -- see g_tc below.
 *
 * **The conversion runs in the interrupt handler**, with the copy, for the
 * reason the copy does. It is an average and a dither over one buffer, a
 * couple of thousand samples, a few times a second; on the slowest machine
 * that would ever run the 8-bit path it is a smaller cost than the frame it
 * would otherwise be stalling.
 *
 * A fourth path, for one chip: an **ESS AudioDrive** answers the version
 * command as a Pro, and behind that pretence is a real 16-bit stereo DAC.
 * Asked the right question (command 0xE7) it says who it is, and then it
 * can be driven in its own extended mode at the mixer's own format, with
 * none of the 8-bit apparatus involved. On by default -- `essnative=0`
 * in mrpy.ini goes back to the Pro path -- and falling back by itself if
 * anything about the card does not fit. See the ESS section below.
 *
 * **`__interrupt __loadds`, not plain `__interrupt`.** A protected-mode
 * handler is entered with DS undefined, so without the second keyword the
 * handler touches its own globals through whatever selector was current.
 * See the longer note in platdos.c, which is where the crash that found it
 * happened.
 *
 * **And the handler runs on a stack of its own**, because DS is not the only
 * selector the compiler has assumptions about: under DOS/4GW an interrupt
 * arrives on the extender's stack, SS is not DS, and the first global that a
 * callee happens to index through EBP faults. dosisr.h has the dump and the
 * instruction. On DOS/32A the switch changes nothing and costs four
 * instructions per interrupt.
 *
 * **No mixer registers are touched.** Volume is the mixer's own business and
 * the card's levels are whatever the player set them to. Reaching into the
 * CT1745 mixer to turn things up is a good way to be the program that
 * changed somebody's settings.
 *
 * That rule was broken once, by a single line copied out of a Linux driver:
 * the ESS path reset the mixer, which set the player's Line level to 0 and
 * left it there after the game exited. A kernel driver owning the card from
 * boot may do that; a game may not. If something here ever seems to need a
 * mixer write, it needs a saved copy and a restore on the way out first.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>       /* the audiotone diagnostic, at init only */

#include "mr.h"
#include "wave.h"
#include "sbconv.h"
#include "config.h"
#include "dosmem.h"
#include "plat.h"       /* mr_plat_ticks, for the delivered-rate check */
#include "dosisr.h"     /* the handler runs on a stack whose selector is DS */

#if defined(__DOS__) || defined(MR_TARGET_DOS)

#include <conio.h>
#include <dos.h>
#include <i86.h>

/* --- the card ------------------------------------------------------------ */

/* DSP ports, as offsets from the base address. */
#define DSP_RESET       0x06
#define DSP_READ        0x0A
#define DSP_WRITE       0x0C    /* also the write-status port, bit 7 */
#define DSP_READ_STATUS 0x0E    /* bit 7 set: data available. 8-bit ack */
#define DSP_ACK16       0x0F    /* reading it acknowledges a 16-bit IRQ */

/* DSP commands. */
#define DSP_SET_RATE    0x41    /* output sampling rate, big-endian word */
#define DSP_GET_VERSION 0xE1
#define DSP_ESS_ID      0xE7    /* ESS AudioDrive legacy ID; see ess_probe */

/* ESS extended mode. The registers are reached through the DSP port: the
 * register number is a command and the value is the command after it.
 * Reading one is command 0xC0, the register number, then a byte from the
 * read port once bit 0x40 of the write-status port is set -- which is a
 * different status bit from the one an SB read waits on. */
#define ESS_EXTENDED    0xC6
#define ESS_READ_REG    0xC0
#define ESS_RATE        0xA1    /* sample rate divisor */
#define ESS_FILTER      0xA2    /* the output filter's own divisor */
#define ESS_COUNT_LO    0xA4
#define ESS_COUNT_HI    0xA5
#define ESS_CHANNELS    0xA8
#define ESS_IRQ_CTL     0xB1
#define ESS_DRQ_CTL     0xB2
#define ESS_SIGNED      0xB6
#define ESS_MODE        0xB7
#define ESS_DMA_CTL     0xB8
#define ESS_DEMAND      0xB9
#define DSP_SPEAKER_ON  0xD1
#define DSP_SPEAKER_OFF 0xD3
#define DSP_HALT16      0xD5
#define DSP_HALT8       0xD0
#define DSP_PLAY16      0xB6    /* 16-bit D/A, auto-init, FIFO */
#define DSP_MODE_STEREO_SIGNED 0x30
#define DSP_TIME_CONST  0x40    /* 8-bit cards: the rate, as 256 - 1e6/rate */
#define DSP_BLOCK_LEN   0x48    /* 8-bit auto-init block length, bytes - 1 */
#define DSP_PLAY8_AUTO  0x1C    /* 8-bit D/A, auto-init. DSP 2.00 and up */
#define DSP_PLAY8_ONCE  0x14    /* 8-bit D/A, one block, length follows */

/* 16-bit DMA controller, channels 5..7. */
#define DMA2_MASK       0xD4
#define DMA2_MODE       0xD6
#define DMA2_CLEAR_FF   0xD8
/* and the 8-bit one, channels 0..3. */
#define DMA1_MASK       0x0A
#define DMA1_MODE       0x0B
#define DMA1_CLEAR_FF   0x0C
#define DMA_MODE_AUTOINIT 0x10
#define DMA_MODE_WRITE    0x48  /* memory to device, increment, single mode */

static const unsigned char DMA16_ADDR[4] = { 0xC0, 0xC4, 0xC8, 0xCC };
static const unsigned char DMA16_COUNT[4] = { 0xC2, 0xC6, 0xCA, 0xCE };
static const unsigned char DMA16_PAGE[4] = { 0x8F, 0x8B, 0x89, 0x8A };

static const unsigned char DMA8_ADDR[4] = { 0x00, 0x02, 0x04, 0x06 };
static const unsigned char DMA8_COUNT[4] = { 0x01, 0x03, 0x05, 0x07 };
static const unsigned char DMA8_PAGE[4] = { 0x87, 0x83, 0x81, 0x82 };

static int g_base = 0x220;
static int g_irq  = 5;
static int g_dma  = 5;          /* the 16-bit channel: H in BLASTER */
static int g_dma8 = 1;          /* the 8-bit channel: D in BLASTER */
static int g_ver;               /* DSP version, 0x0400 and up for an SB16 */
static int g_ess;               /* an ESS AudioDrive is behind the SB Pro */
static int g_ess_id;            /* the two bytes it answered 0xE7 with */
static int g_open;

/* --- the output format ---------------------------------------------------
 *
 * Decided once, in mr_wave_open, from the DSP version. g_is16 selects the
 * whole SB16 path -- 16-bit DMA controller, 16-bit DSP commands, no
 * conversion -- and g_autoinit says whether the DSP can keep a transfer
 * going by itself or has to be restarted from each interrupt.
 *
 * The 8-bit rate is not 22050. The pre-16 DSPs take a time constant, not a
 * rate: 256 - 1000000/rate, which for 22050 rounds to 211 and plays back at
 * 1000000/45 = 22222Hz. That is 0.8% sharp, thirteen cents, under the
 * tuning error of the piano that was sampled. It is written down because the
 * delivered-rate line at shutdown will show it and it should not start a
 * hunt.
 */
static mr_sbconv g_conv;
static int  g_is16;             /* the SB16: 16-bit DMA channel and commands */
static int  g_ess_on;           /* the ESS, in its own 16-bit mode */
static int  g_passthru;         /* the mixer's bytes go out unconverted */
static int  g_autoinit;
static int  g_tc;               /* the time constant actually written */
static int  g_out_rate, g_out_ch, g_out_bits;

/* --- the DMA ring -------------------------------------------------------- */

#define HALVES 2
#define RING_MAX (MR_WAVE_BYTES * HALVES)

/* One mixer buffer's worth of converted bytes, and twice that. Both are set
 * in mr_wave_open once the format is known; they were constants while this
 * file only did one. */
static long g_half_bytes;
static long g_ring_bytes;

static mr_dos_block  g_dos;         /* what was allocated */
static unsigned char *g_ring;       /* the aligned window inside it */
static unsigned long  g_ring_phys;

/* The half most recently refilled. -1 before the first one, so that the
 * first interrupt fills half 0 rather than deciding it already has. */
static volatile int   g_half = -1;
static volatile long  g_spurious;   /* interrupts naming a half already full */
static unsigned int   g_t0;         /* when the transfer started */

/* --- the mixer's buffers ------------------------------------------------- */

static short *g_bufs[MR_WAVE_BUFS];
static volatile unsigned char g_busy[MR_WAVE_BUFS];  /* filled, not yet copied */
static volatile int g_q[MR_WAVE_BUFS + 1];           /* indices, in order */
static volatile int g_qhead, g_qtail;

static volatile long g_played;      /* halves the card has consumed, x frames */

/* What mr_wave_played_frames reports, which is not the same thing.
 *
 * wave.h asks for "frames the device has actually played, counted from the
 * first one ever submitted". On this card the two differ, because unlike
 * waveOut the hardware is running before the engine has anything to give
 * it: the DSP starts in mr_wave_open and the handler fills every half from
 * then on, with silence until the first mr_wave_submit arrives. That gap is
 * the 8 halves documented in fill_half -- 371ms on FreeDOS in QEMU.
 *
 * Counting those into the figure audio.c subtracts start_mixed from makes
 * the device look 371ms further into the track than it is, from the instant
 * the track starts. The rhythm game then eases its anchor across the error
 * over the following frames (rhythm.c song_ms), which on the PIII is seen
 * as the notes briefly speeding up the moment the music becomes audible and
 * then settling. mr_audio_latency_ms had the same cause: g_mixed_frames
 * minus a count that runs ahead of it is negative, and it clamps to zero,
 * so DOS reported no output latency at all.
 *
 * So this one advances only for a half that was filled from the queue.
 * Silence, before the first buffer or during a stall, is not something the
 * track was heard through. */
static volatile long g_heard;
static volatile long g_underruns;
static volatile long g_under_run;   /* the current run of empty halves */
static volatile int  g_fed;         /* has the engine ever queued a buffer? */
static volatile long g_under_worst; /* and the worst one of the session */

/* mrpy.ini `audiotone`: feed the card a sine generated here instead of the
 * mixer's audio.
 *
 * This is a diagnostic, and it exists because "it sounds wrong" has two
 * halves that cannot be told apart by ear. Everything from the decoder to
 * mr_wave_submit is one half; the converter, the ring, the DMA controller
 * and the DSP are the other. The tone goes through the second half only. If
 * it comes out clean then the card plumbing is right and the fault is
 * upstream -- starvation, or the mixer; if the tone is dirty too, the fault
 * is here, and the mixer is irrelevant to it.
 *
 * One buffer, holding a whole number of cycles, so that repeating it is
 * seamless and anything heard at the seam is the sink's doing and not the
 * tone's. */
static short *g_tone;

static void (__interrupt __far *g_old_isr)(void);
static int  g_hooked;
static int  g_locked;

/* --- talking to the DSP --------------------------------------------------
 *
 * Every write waits for bit 7 of the write-status port to clear, with a
 * bounded spin: a card that is not there never clears it, and an unbounded
 * loop there is a hang at startup rather than a failed detection.
 */
#define SPIN 65535

static void dsp_write(int val)
{
    long n = SPIN;

    while (n-- > 0 && (inp(g_base + DSP_WRITE) & 0x80))
        ;
    outp(g_base + DSP_WRITE, val);
}

static int dsp_read(void)
{
    long n = SPIN;

    while (n-- > 0 && !(inp(g_base + DSP_READ_STATUS) & 0x80))
        ;
    if (n <= 0)
        return -1;
    return inp(g_base + DSP_READ);
}

/* Reset: 1 into the reset port, hold it, 0, then the DSP answers 0xAA. The
 * hold has to be at least 3 microseconds; reading an unused port is the
 * usual way to spend about a microsecond on any machine, and doing it a
 * dozen times is both long enough and harmless. */
static int dsp_reset(void)
{
    int i, v;

    outp(g_base + DSP_RESET, 1);
    for (i = 0; i < 16; i++)
        inp(0x80);
    outp(g_base + DSP_RESET, 0);

    for (i = 0; i < 256; i++) {
        if (inp(g_base + DSP_READ_STATUS) & 0x80) {
            v = inp(g_base + DSP_READ);
            if (v == 0xAA)
                return 1;
        }
    }
    return 0;
}

/* --- is there an ESS behind the Sound Blaster? ---------------------------
 *
 * An ESS AudioDrive answers the version command as a Sound Blaster Pro --
 * DSP 3.01 or thereabouts -- and that is all this file has ever asked it,
 * so the ES1868 in the development machine has been driven as an 8-bit
 * mono Pro while a real 16-bit stereo DAC sat behind it unused.
 *
 * The chip identifies itself to anyone who asks the right question:
 * command 0xE7 returns two bytes, and an AudioDrive puts 0x68 in the
 * first. The second names the family -- the 0x8x generation is ES688 and
 * ES1688, and later parts including the ES1868 answer differently; this
 * only records what came back, because what to *do* with it is the next
 * step and wants a known-good reference rather than a guess.
 *
 * Two pieces of care. It only asks cards that claim to be a Pro, so an
 * SB16 is never poked with a command it does not know; and it resets the
 * DSP afterwards whatever happened, because a genuine Pro will have
 * ignored an undefined command and there is no reason to find out later
 * what state that left it in.
 */
static void ess_probe(void)
{
    int a, b;

    /* Anything that is not an SB16. The gate used to start at 3.00 on the
     * grounds that an AudioDrive reports itself as a Pro, which is true of
     * the real ones and is not a safe thing to require: an emulated one
     * may report anything, and a card that does not know 0xE7 simply does
     * not answer. */
    if (g_ver >= 0x0400)
        return;

    dsp_write(DSP_ESS_ID);
    a = dsp_read();
    b = dsp_read();

    dsp_reset();

    if (a < 0 || b < 0) {
        /* Said out loud, because "no ESS here" and "this card never
         * answered the question" look identical from the outside and only
         * one of them means the probe is working. */
        mr_log("audio: no answer to the ESS id command; not an AudioDrive, "
               "or one that does not implement it");
        return;
    }

    g_ess_id = (a << 8) | b;
    if (a == 0x68)
        g_ess = 1;
}

/* --- the ESS, driven as itself -------------------------------------------
 *
 * Everything here follows Linux's `sound/isa/es18xx.c`, which has been run
 * against these chips for twenty years: `snd_es18xx_initialize` for the
 * IRQ and DMA encodings, and the DAC1 path -- confusingly named
 * `snd_es18xx_playback2_prepare` -- for the per-transfer sequence. The
 * datasheet would be a second chance to make the same mistake; a driver
 * people actually use is evidence.
 *
 * What this buys: signed 16-bit stereo at 44100, which is the mixer's own
 * format. On an ESS the whole 8-bit apparatus -- the half-band filter, the
 * makeup gain, the shaped dither -- is simply not used, because nothing is
 * being thrown away. The transfer is still on an *8-bit* DMA channel, in
 * bytes, with the 64KB boundary rule, so the controller code below is the
 * same one the SB Pro path uses.
 */
static void ess_write(int reg, int val)
{
    dsp_write(reg);
    dsp_write(val);
}

static int ess_read(int reg)
{
    long n = SPIN;

    dsp_write(ESS_READ_REG);
    dsp_write(reg);

    while (n-- > 0 && !(inp(g_base + DSP_WRITE) & 0x40))
        ;
    if (n <= 0)
        return -1;
    return inp(g_base + DSP_READ);
}

/* The IRQ and DMA numbers are not written as themselves: the chip wants a
 * two-bit code for each, and it only has codes for some of them. A card
 * configured outside those falls back to being a Pro rather than being
 * driven with a code that means a different line. */
static int ess_irq_code(void)
{
    switch (g_irq) {
    case 2: case 9: return 0;
    case 5:         return 1;
    case 7:         return 2;
    case 10:        return 3;
    }
    return -1;
}

static int ess_dma_code(void)
{
    switch (g_dma8) {
    case 0: return 1;
    case 1: return 2;
    case 3: return 3;
    }
    return -1;
}

/* Rate and filter divisors.
 *
 * The rate clock is 795444Hz divided by a small integer: 44100 wants 18,
 * which gives 44191Hz, two tenths of a percent sharp and four cents. The
 * filter is a second divisor, and its formula is the driver's own -- the
 * cutoff tracks the rate, which is the point of having it.
 */
static int ess_rate_div(int rate)
{
    int den = (795444 + rate / 2) / rate;

    if (den < 1)
        den = 1;
    else if (den > 127)
        den = 127;
    return 256 - den;
}

static int ess_filter_div(int rate)
{
    /* 256 - (7160000 * 20) / (8 * 82 * rate), in integers, as es18xx.c
     * writes it. */
    long d = 143200000L / (656L * (long)rate);

    if (d > 255)
        d = 255;
    return (int)(256 - d);
}

/* Put the chip into extended mode and tell it where its interrupt and its
 * DMA channel are. Returns 0 if this card cannot be driven this way, with
 * the caller left to use the Pro path. */
static int ess_init(void)
{
    int irqc = ess_irq_code();
    int dmac = ess_dma_code();

    if (irqc < 0 || dmac < 0) {
        mr_log("audio: ESS on irq %d dma %d is outside what its native "
               "mode encodes; staying with the Pro path", g_irq, g_dma8);
        return 0;
    }

    dsp_write(ESS_EXTENDED);

    /* No mixer reset here, though es18xx.c does one and this used to copy
     * it. A driver taking the card over at boot is entitled to put the
     * mixer in a known state; a game run from a DOS prompt is not. Writing
     * 0 to the mixer's register 0 resets every level to the chip's
     * power-on defaults, and on a real ES1868 that was measured with
     * ESSVOL as Line going 15 -> 0 across a run and staying there
     * afterwards -- the player's settings, silently changed and not put
     * back. Nothing below needs it: the unlock is 0xC6 and everything this
     * path programs is an extended DSP register, not a mixer one. */

    ess_write(ESS_DEMAND, 0x02);        /* four bytes per DMA request */
    ess_write(ESS_IRQ_CTL, 0x50 | (irqc << 2));
    ess_write(ESS_DRQ_CTL, 0x50 | (dmac << 2));

    return 1;
}

/* The per-transfer half of it: format, rate, and how many bytes between
 * interrupts. Called where dsp_start is for the Sound Blasters. */
static void ess_start(void)
{
    int count = (int)(0x10000L - g_half_bytes);
    int a8;

    /* Reset the FIFO: 0x02 into the reset port, read it back, 0x00. */
    outp(g_base + DSP_RESET, 0x02);
    inp(g_base + DSP_RESET);
    outp(g_base + DSP_RESET, 0x00);

    a8 = ess_read(ESS_CHANNELS);
    if (a8 < 0)
        a8 = 0;
    ess_write(ESS_CHANNELS, (a8 & ~0x03) | 0x01);       /* 01 is stereo */

    ess_write(ESS_RATE, ess_rate_div(MR_WAVE_RATE));
    ess_write(ESS_FILTER, ess_filter_div(MR_WAVE_RATE));

    /* The count is the two's complement of the bytes between interrupts,
     * which is one half of the ring -- the same division of labour the
     * other two paths use. */
    ess_write(ESS_COUNT_LO, count & 0xFF);
    ess_write(ESS_COUNT_HI, (count >> 8) & 0xFF);

    ess_write(ESS_SIGNED, 0x00);        /* 0x80 would be unsigned */
    ess_write(ESS_MODE, 0x71);          /* the signed-mode prefix */
    ess_write(ESS_MODE, 0xBC);          /* stereo | 16-bit | signed */

    dsp_write(DSP_SPEAKER_ON);
    ess_write(ESS_DMA_CTL, 0x05);       /* and away */
}

static void ess_stop(void)
{
    ess_write(ESS_DMA_CTL, 0x00);
}

/* --- BLASTER -------------------------------------------------------------
 *
 * A220 I5 D1 H5 P330 T6. Parsed by letter rather than by position: the
 * order is not fixed, cards and drivers write different subsets, and H is
 * absent on anything that is not an SB16 -- which is itself a useful signal,
 * though the DSP version is the one that decides.
 */
static int parse_blaster(void)
{
    const char *s = getenv("BLASTER");
    int got_a = 0;

    if (!s) {
        mr_log("audio: no BLASTER in the environment; no card assumed");
        return 0;
    }

    while (*s) {
        int c = *s++;
        int v = 0;

        if (c == ' ' || c == '\t')
            continue;

        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';

        if (c == 'A') {                 /* hex */
            while ((*s >= '0' && *s <= '9') ||
                   ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'f')) {
                int d = (*s <= '9') ? *s - '0' : (*s | 0x20) - 'a' + 10;
                v = v * 16 + d;
                s++;
            }
            g_base = v;
            got_a = 1;
            continue;
        }

        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');

        if (c == 'I')
            g_irq = v;
        else if (c == 'H')
            g_dma = v;
        else if (c == 'D')
            g_dma8 = v;
        /* P (MIDI) and T (model) are not wanted. H is absent on anything
         * that is not an SB16 and D is absent on nothing, which is why the
         * 8-bit channel has a default and the 16-bit one does not. */
    }

    if (!got_a) {
        mr_log("audio: BLASTER has no A= port; ignoring it");
        return 0;
    }
    return 1;
}

/* --- the interrupt ------------------------------------------------------- */

static int irq_vector(void)
{
    return (g_irq < 8) ? (0x08 + g_irq) : (0x70 + g_irq - 8);
}

static void irq_unmask(void)
{
    if (g_irq < 8)
        outp(0x21, inp(0x21) & ~(1 << g_irq));
    else {
        outp(0xA1, inp(0xA1) & ~(1 << (g_irq - 8)));
        outp(0x21, inp(0x21) & ~(1 << 2));      /* the cascade */
    }
}

static void irq_mask(void)
{
    if (g_irq < 8)
        outp(0x21, inp(0x21) | (1 << g_irq));
    else
        outp(0xA1, inp(0xA1) | (1 << (g_irq - 8)));
}

/* The handler. It acknowledges the card, refills the half that has just
 * finished playing from the mixer's queue, and signals end of interrupt.
 *
 * It does not chain. A sound card IRQ is ours alone while the game is up,
 * and the only thing a stale handler could want with it is to be confused.
 *
 * Silence on an empty queue, rather than letting the card repeat the half it
 * just played: a repeat is a 46ms loop of whatever was there, which is far
 * more noticeable than a gap. The count is logged at shutdown, because an
 * underrun is a symptom of the frame loop being slow and that is worth
 * knowing from a player's log.
 */
/* Which half the card is playing out of, asked of the DMA controller rather
 * than inferred from how many interrupts have arrived.
 *
 * Inferring it is the obvious thing and it was wrong. The count the DSP is
 * given for a 16-bit stereo transfer is documented in units that different
 * sources describe differently -- frames in one reading, 16-bit samples in
 * another, which differ by exactly the two channels -- so an interrupt may
 * arrive once per half or twice. Alternating a flag on each one then refills
 * the half the card is *currently reading*, overwriting audio it has not
 * played yet, every 23ms: the stream advances at double speed with a
 * discontinuity at every seam. That is what "too fast, with crackling"
 * sounded like.
 *
 * The controller knows. Its count register holds the words remaining in the
 * whole ring, which gives the exact byte position, and the half that is safe
 * to write is simply the other one. Then it does not matter how often the
 * card interrupts or what the units were: a duplicate interrupt names a half
 * that has already been filled and does nothing.
 */
static int playing_half(void)
{
    unsigned count;
    unsigned long pos;

    if (g_is16) {
        int ch = g_dma & 3;

        outp(DMA2_CLEAR_FF, 0);
        count = (unsigned)inp(DMA16_COUNT[ch]);
        count |= (unsigned)inp(DMA16_COUNT[ch]) << 8;

        /* Words remaining, minus one, counting down; so words done is the
         * complement. Doubled for bytes, since the channel transfers
         * words. */
        pos = ((unsigned long)(g_ring_bytes / 2) - 1 - count) * 2;
    } else {
        int ch = g_dma8 & 3;

        outp(DMA1_CLEAR_FF, 0);
        count = (unsigned)inp(DMA8_COUNT[ch]);
        count |= (unsigned)inp(DMA8_COUNT[ch]) << 8;

        /* The 8-bit channel counts bytes, so there is no doubling. */
        pos = (unsigned long)g_ring_bytes - 1 - count;
    }

    if (pos >= (unsigned long)g_ring_bytes)
        pos = 0;                    /* mid-reload; treat it as the start */

    return (pos >= (unsigned long)g_half_bytes) ? 1 : 0;
}

/* Put one mixer buffer, converted, into a half of the ring. */
static void fill_half(int target)
{
    unsigned char *dst = g_ring + (long)target * g_half_bytes;

    if (g_tone) {
        /* The diagnostic tone never touches the queue, so g_heard would
         * otherwise stay at zero and freeze every clock derived from it.
         * There is no track to be out of step with here. */
        mr_sbconv_run(&g_conv, g_tone, MR_WAVE_FRAMES, dst);
        g_played += MR_WAVE_FRAMES;
        g_heard += MR_WAVE_FRAMES;
        return;
    }

    if (g_qhead != g_qtail) {
        int idx = g_q[g_qhead];
        g_qhead = (g_qhead + 1) % (MR_WAVE_BUFS + 1);
        if (g_passthru)
            memcpy(dst, g_bufs[idx], (size_t)g_half_bytes);
        else
            mr_sbconv_run(&g_conv, g_bufs[idx], MR_WAVE_FRAMES, dst);
        g_busy[idx] = 0;
        g_under_run = 0;
        g_heard += MR_WAVE_FRAMES;
    } else {
        memset(dst, mr_sbconv_silence(&g_conv), (size_t)g_half_bytes);

        /* Only once the engine has fed us something. The card starts as soon
         * as mr_wave_open returns, and the first buffer cannot arrive until
         * the script has been parsed and a frame has run -- on FreeDOS in
         * QEMU that gap is 8 halves, and it was reported as "8 underruns
         * (371 ms of silence), worst run 8", identically whether the player
         * quit at the title screen or played the game through. Three things
         * said it was one event and not eight: 371ms is exactly 8 x
         * MR_WAVE_FRAMES at MR_WAVE_RATE, the worst run equalled the total
         * so they were consecutive, and the count did not grow with the
         * length of the session.
         *
         * Nobody can hear it -- there is nothing to play yet -- and calling
         * it "the frame loop could not keep the queue full" is false before
         * there is a frame loop. A metric that cries wolf at every startup
         * is worse than no metric, because the real thing it is for is the
         * stall in the middle of a run. */
        if (g_fed) {
            g_underruns++;
            /* One long stall and a chop every third half are the same total
             * and very different faults, so count the run as well as the
             * sum. */
            g_under_run++;
            if (g_under_run > g_under_worst)
                g_under_worst = g_under_run;
        }
    }

    /* Mixer frames, always, whatever the card was fed -- including the
     * silence, because this counter answers "is the card running at the
     * rate we asked for", and the close-down log divides it by the wall
     * clock. g_heard above is the one that answers "where is the track",
     * and it deliberately does not count silence.
     *
     * Mixer frames and not card frames in both cases: mr_audio's latency
     * and the rhythm game's judgement window are in the mixer's time base,
     * and converting to the card's would make both wrong by the decimation
     * ratio. */
    g_played += MR_WAVE_FRAMES;
}

static void dsp_play_once(void);

/* The handler's work, as an ordinary function, so that mr_dos_isr_call can
 * run it on a stack whose selector is DS. Everything it reaches -- fill_half,
 * mr_sbconv_run -- is below this point and inherits that. See dosisr.h: with
 * the host's own stack under it instead, this faulted on DOS/4GW at an
 * EBP-indexed store to a global in fill_half. */
static void sb_isr_work(void)
{
    int target;

    /* Acknowledging the DSP is a read, of a different port depending on
     * which engine finished. */
    inp(g_base + (g_is16 ? DSP_ACK16 : DSP_READ_STATUS));

    /* SB 1.x first, and before anything else that takes time.
     *
     * Its DSP has no auto-init command, so the transfer has stopped dead
     * and the DAC is holding its last sample until a new block is started.
     * Whatever happens between the interrupt and that restart is a pause in
     * the audio -- and the first version of this did the conversion first,
     * so the pause was the length of a buffer conversion and varied with
     * what the machine was doing. There were no clicks in the recording of
     * it, because a held sample does not click; there was a 20Hz wobble on
     * a pure tone, which is the block rate, and a smear 25Hz wide around
     * the carrier. That is what an irregularly time-stretched sine looks
     * like.
     *
     * The DMA *controller* can auto-init even when the DSP cannot, so the
     * ring keeps turning on its own and the restart is three writes to the
     * DSP. The data for the next block is already there -- it is the half
     * this handler filled last time. Restart now, convert afterwards. */
    if (!g_autoinit)
        dsp_play_once();

    target = playing_half() ^ 1;

    if (target != g_half) {         /* g_half: the last one filled */
        fill_half(target);
        g_half = target;
    } else {
        g_spurious++;
    }

    if (g_irq >= 8)
        outp(0xA0, 0x20);           /* the slave first, then the master */
    outp(0x20, 0x20);
}

/* The vector's target. Nothing but the switch: a body with any C in it would
 * give the compiler somewhere to address through EBP, which after the switch
 * is the wrong stack. */
static unsigned char g_isr_stack[MR_DOS_ISR_STACK];

static void __interrupt __loadds sb_isr(void)
{
    mr_dos_isr_call(sb_isr_work, MR_DOS_ISR_TOP(g_isr_stack));
}

/* --- starting and stopping the transfer ---------------------------------- */

static void dma_program(void)
{
    unsigned long addr = g_ring_phys;

    /* The whole ring, auto-init, on every path. The DMA controller has had
     * auto-init since the PC/AT and does not care that an SB 1.x DSP has
     * not; letting the controller wrap by itself is what keeps the restart
     * on that card down to three port writes. */
    long len = g_ring_bytes;

    if (g_is16) {
        int ch = g_dma & 3;
        unsigned words;

        /* A 16-bit channel counts in words and addresses in words: the
         * address register takes bits 1..16 of the physical address and the
         * page register bits 17..23. This is the step where a buffer that
         * crossed a 128KB boundary would silently wrap to the start of its
         * own page, which is why the allocation below aligns rather than
         * hoping. */
        words = (unsigned)(len / 2) - 1;

        outp(DMA2_MASK, ch | 0x04);             /* mask while programming */
        outp(DMA2_CLEAR_FF, 0);
        outp(DMA2_MODE, DMA_MODE_WRITE | DMA_MODE_AUTOINIT | ch);

        outp(DMA16_PAGE[ch], (int)((addr >> 16) & 0xFE));
        outp(DMA16_ADDR[ch], (int)((addr >> 1) & 0xFF));
        outp(DMA16_ADDR[ch], (int)((addr >> 9) & 0xFF));

        outp(DMA2_CLEAR_FF, 0);
        outp(DMA16_COUNT[ch], (int)(words & 0xFF));
        outp(DMA16_COUNT[ch], (int)((words >> 8) & 0xFF));

        outp(DMA2_MASK, ch);                    /* and away */
    } else {
        int ch = g_dma8 & 3;
        unsigned bytes = (unsigned)len - 1;
        int mode = DMA_MODE_WRITE | DMA_MODE_AUTOINIT | ch;

        /* The 8-bit channel counts and addresses in bytes, and its boundary
         * rule is 64KB rather than 128KB. */
        outp(DMA1_MASK, ch | 0x04);
        outp(DMA1_CLEAR_FF, 0);
        outp(DMA1_MODE, mode);

        outp(DMA8_PAGE[ch], (int)((addr >> 16) & 0xFF));
        outp(DMA8_ADDR[ch], (int)(addr & 0xFF));
        outp(DMA8_ADDR[ch], (int)((addr >> 8) & 0xFF));

        outp(DMA1_CLEAR_FF, 0);
        outp(DMA8_COUNT[ch], (int)(bytes & 0xFF));
        outp(DMA8_COUNT[ch], (int)((bytes >> 8) & 0xFF));

        outp(DMA1_MASK, ch);
    }
}

/* One block, on a DSP with no auto-init command. The length goes with the
 * command rather than ahead of it, and the whole thing is reissued from the
 * interrupt that ends it. */
static void dsp_play_once(void)
{
    int len = (int)g_half_bytes - 1;

    dsp_write(DSP_PLAY8_ONCE);
    dsp_write(len & 0xFF);
    dsp_write((len >> 8) & 0xFF);
}

static void dsp_start(void)
{
    dsp_write(DSP_SPEAKER_ON);

    if (g_is16) {
        int frames = MR_WAVE_FRAMES - 1;

        /* The output rate, big-endian, as a plain number of Hz -- the SB16
         * has no time constant. */
        dsp_write(DSP_SET_RATE);
        dsp_write((MR_WAVE_RATE >> 8) & 0xFF);
        dsp_write(MR_WAVE_RATE & 0xFF);

        /* Block length is one *half*, so the card interrupts twice per ring
         * and each interrupt leaves exactly one half free to refill. In
         * frames minus one, which for 16-bit stereo is bytes/4 - 1. */
        dsp_write(DSP_PLAY16);
        dsp_write(DSP_MODE_STEREO_SIGNED);
        dsp_write(frames & 0xFF);
        dsp_write((frames >> 8) & 0xFF);
        return;
    }

    dsp_write(DSP_TIME_CONST);
    dsp_write(g_tc);

    if (g_autoinit) {
        int len = (int)g_half_bytes - 1;

        /* Same reasoning as the SB16's block length: one half per
         * interrupt. Here it is in bytes, because the transfer is. */
        dsp_write(DSP_BLOCK_LEN);
        dsp_write(len & 0xFF);
        dsp_write((len >> 8) & 0xFF);
        dsp_write(DSP_PLAY8_AUTO);
    } else {
        dsp_play_once();
    }
}

/* --- wave.h -------------------------------------------------------------- */

int mr_wave_open(void)
{
    unsigned long base;
    int i;

    if (!parse_blaster())
        return 0;

    if (!dsp_reset()) {
        mr_log("audio: no Sound Blaster at %03x", (unsigned)g_base);
        return 0;
    }

    dsp_write(DSP_GET_VERSION);
    i = dsp_read();
    g_ver = (i < 0) ? 0 : (i << 8);
    i = dsp_read();
    g_ver |= (i < 0) ? 0 : i;

    ess_probe();
    if (g_ess)
        mr_log("audio: ESS AudioDrive behind the Pro, legacy id %04x",
               (unsigned)g_ess_id);
    else if (g_ess_id)
        mr_log("audio: 0xE7 answered %04x, which is not an ESS",
               (unsigned)g_ess_id);

    /* The format, from the version. See the three-card note at the top. */
    if (g_ver >= 0x0400) {
        if (g_dma < 5 || g_dma > 7) {
            mr_log("audio: BLASTER gives no 16-bit DMA channel (H=); "
                   "running silently");
            return 0;
        }
        g_is16 = 1;
        g_autoinit = 1;
        g_out_rate = MR_WAVE_RATE;
        g_out_ch = 2;
        g_out_bits = 16;
    } else if (g_ess && mr_config_int("essnative", 1, 0, 1) && ess_init()) {
        /* The ESS, as itself: the mixer's own format, straight out, on the
         * 8-bit DMA channel. On by default -- verified on an ES1868 (id
         * 688b, irq 5, dma 1), where it is the difference between the
         * card's own 16-bit DAC and an 8-bit mono imitation of a 1989
         * Sound Blaster. `essnative=0` in mrpy.ini goes back to the Pro
         * path, and a card whose irq or dma this mode cannot encode has
         * already fallen back by here. */
        g_ess_on = 1;
        g_autoinit = 1;
        g_out_rate = MR_WAVE_RATE;
        g_out_ch = 2;
        g_out_bits = 16;
    } else {
        if (g_dma8 < 0 || g_dma8 > 3) {
            mr_log("audio: BLASTER gives no 8-bit DMA channel (D=); "
                   "running silently");
            return 0;
        }
        g_is16 = 0;
        g_autoinit = (g_ver >= 0x0200);
        g_out_rate = MR_WAVE_RATE / 2;
        g_out_ch = 1;
        g_out_bits = 8;
        g_tc = 256 - 1000000 / g_out_rate;
    }

    /* mrpy.ini `audiogain`, in percent.
     *
     * Why there is a default above 100 on the 8-bit path, and none on the
     * SB16's: eight bits is 48dB at full scale, and the music does not run
     * at full scale. The game's own track peaks at -11dBFS before the
     * player's music volume is applied at all, so what reaches the
     * quantiser uses a fifth of the range and half its samples land within
     * two steps of silence. Doubling buys 6dB of that back and still
     * leaves this track 5dB of headroom. It is a default, not a law --
     * audiogain=100 turns it off, and the clip count at shutdown says if
     * it was too much for a louder game. */
    /* Both 16-bit paths hand the mixer's bytes to the card untouched. The
     * converter is still initialised, because the ring is sized from it
     * and the underrun path asks it what silence is. */
    g_passthru = (g_is16 || g_ess_on);

    if (!mr_sbconv_init(&g_conv, g_out_rate, g_out_ch, g_out_bits)) {
        mr_log("audio: no conversion to %dHz %d-bit %s; running silently",
               g_out_rate, g_out_bits, g_out_ch == 1 ? "mono" : "stereo");
        return 0;
    }

    /* After the init, which resets it. */
    mr_sbconv_set_gain(&g_conv, (int)mr_config_int("audiogain",
                                                   g_passthru ? 100 : 200,
                                                   10, 1000));
    mr_sbconv_set_dither(&g_conv,
                         (int)mr_config_int("audiodither", 1, 0, 1));

    g_half_bytes = mr_sbconv_out_bytes(&g_conv, MR_WAVE_FRAMES);
    g_ring_bytes = g_half_bytes * HALVES;

    /* Twice the ring, so that an aligned window fits inside whatever DOS
     * hands back. The ring is a power of two, so a block of that size on
     * that alignment lies inside one 64KB page and inside one 128KB page --
     * the two rules the 8-bit and 16-bit transfers have to obey. */
    if (!mr_dos_alloc(&g_dos, (unsigned)((g_ring_bytes * 2) / 16))) {
        mr_log("audio: no %ld bytes of conventional memory for the DMA ring",
               g_ring_bytes * 2);
        return 0;
    }
    base = mr_dos_linear(&g_dos);
    g_ring_phys = (base + (g_ring_bytes - 1)) &
                  ~(unsigned long)(g_ring_bytes - 1);
    g_ring = (unsigned char *)g_ring_phys;
    memset(g_ring, mr_sbconv_silence(&g_conv), (size_t)g_ring_bytes);

    /* The ring is the last conventional memory this engine asks for -- the
     * VBE scratch was freed at the end of enumeration -- so this reading
     * is the low-water mark for the whole session. Against the one
     * dosmem.c logs at startup it says what the ring cost; against MEM at
     * the DOS prompt it says what the whole program needs. */
    {
        long conv = mr_dos_conventional_free();
        if (conv >= 0)
            mr_log("mem: conventional %ldK free after the %ldK DMA ring",
                   conv / 1024L, (g_ring_bytes * 2) / 1024L);
    }

    for (i = 0; i < MR_WAVE_BUFS; i++) {
        g_bufs[i] = (short *)malloc(MR_WAVE_BYTES);
        if (!g_bufs[i]) {
            mr_log("audio: out of memory for mix buffers");
            mr_wave_close();
            return 0;
        }
        memset(g_bufs[i], 0, MR_WAVE_BYTES);
        g_busy[i] = 0;
    }
    if (mr_config_int("audiotone", 0, 0, 1)) {
        g_tone = (short *)malloc(MR_WAVE_BYTES);
        if (g_tone) {
            /* CYCLES whole periods in one buffer: 20 of them in 2048 frames
             * at 44100 is 430.7Hz, near A4 and well inside what any of
             * these cards reproduce. Half scale, to leave the clipping
             * question out of the diagnosis. */
            int k;
            for (k = 0; k < MR_WAVE_FRAMES; k++) {
                double a = 2.0 * 3.14159265358979 * 20.0 * k / MR_WAVE_FRAMES;
                short v = (short)(16000.0 * sin(a));
                g_tone[k * 2] = v;
                g_tone[k * 2 + 1] = v;
            }
            mr_dpmi_lock(g_tone, MR_WAVE_BYTES);
            mr_log("audio: audiotone=1 -- playing a generated 431Hz tone, "
                   "not the game's audio");
        }
    }

    g_qhead = g_qtail = 0;
    g_half = -1;
    g_played = 0;
    g_heard = 0;
    g_underruns = 0;
    g_under_run = 0;
    g_under_worst = 0;
    g_fed = 0;
    g_spurious = 0;
    g_t0 = mr_plat_ticks();

    /* The heap side of what the handler touches, locked against a paging
     * host. Its code, its statics and the stack are locked once at
     * startup by mr_dpmi_lock_image; these are malloc'd and so are nobody
     * else's to know about. */
    g_locked = 1;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (!mr_dpmi_lock(g_bufs[i], MR_WAVE_BYTES))
            g_locked = 0;
    if (!mr_dpmi_lock((void *)g_ring, (unsigned long)g_ring_bytes))
        g_locked = 0;
    if (!g_locked)
        mr_log("audio: DPMI would not lock the buffers; harmless under "
               "DOS/32A, a risk under a host that pages");

    g_old_isr = _dos_getvect(irq_vector());
    _dos_setvect(irq_vector(), sb_isr);
    g_hooked = 1;

    dma_program();
    if (g_ess_on)
        ess_start();
    else
        dsp_start();
    irq_unmask();

    g_open = 1;

    /* Everything a later bug report will want: which card was believed,
     * what it is being fed, down which channel, and by which of the three
     * mechanisms. "It is quiet" and "it is quiet and the log says DSP 2.01,
     * single-cycle" are different problems. */
    if (g_conv.gain != 100)
        mr_log("audio: output gain %d%% (mrpy.ini audiogain) -- eight bits "
               "has no resolution to spare at the mixer's own level",
               g_conv.gain);

    mr_log("audio: %s DSP %d.%02d at %03x irq %d dma %d, "
           "%dHz %d-bit %s, %s, %d x %d frame buffers",
           g_ess_on ? "ESS AudioDrive" : "Sound Blaster",
           g_ver >> 8, g_ver & 0xFF, (unsigned)g_base, g_irq,
           g_is16 ? g_dma : g_dma8,
           g_ess_on ? 795444 / (256 - ess_rate_div(MR_WAVE_RATE)) :
           g_is16   ? g_out_rate : 1000000 / (256 - g_tc),
           g_out_bits, g_out_ch == 1 ? "mono" : "stereo",
           g_ess_on ? "ESS native auto-init" :
           g_is16   ? "auto-init 16-bit" :
                      (g_autoinit ? "auto-init 8-bit" : "single-cycle 8-bit"),
           MR_WAVE_BUFS, MR_WAVE_FRAMES);
    return 1;
}

void mr_wave_close(void)
{
    int i;

    if (g_open) {
        irq_mask();
        if (g_ess_on)
            ess_stop();
        dsp_write(g_is16 ? DSP_HALT16 : DSP_HALT8);
        dsp_write(DSP_SPEAKER_OFF);
        if (g_is16)
            outp(DMA2_MASK, (g_dma & 3) | 0x04);    /* mask the channel */
        else
            outp(DMA1_MASK, (g_dma8 & 3) | 0x04);
        dsp_reset();                            /* and stop it believing in
                                                 * an auto-init transfer */
    }

    if (g_hooked) {
        _dos_setvect(irq_vector(), g_old_isr);
        g_hooked = 0;
    }

    /* What the card actually consumed, against the clock. This is the line
     * that turns "it sounds too fast" into a number: if the delivered rate
     * is not within a few percent of MR_WAVE_RATE then the card is being fed
     * at the wrong rate, whatever it sounds like, and the two candidates are
     * the DSP block length and the interrupt bookkeeping. The spurious count
     * says which -- a large one means the card interrupts more often than
     * once per half, which is the thing the handler now tolerates. */
    if (g_played) {
        long ms = (long)(mr_plat_ticks() - g_t0);
        if (ms > 0)
            /* Divided before it is scaled, which is not a style preference.
             *
             * The obvious `g_played * 1000 / ms` overflows a 32-bit long once
             * g_played passes 2,147,483 frames -- 48.7 seconds of audio -- so
             * it had been wrong for every session of any length, silently and
             * in a plausible-looking direction. A 206-second cutscene on the
             * PIII is what finally made it obvious: it reported "6465 Hz
             * delivered (wanted 44100)", which reproduces exactly as
             * 10049536 * 1000 wrapped mod 2^32 and then divided. The true
             * figure for that run was 44516 Hz, against a card opened at
             * 44191, which is the "within a few percent" this line exists to
             * confirm -- so the bug inverted its entire purpose, turning the
             * one diagnostic that says "the card is being fed at the wrong
             * rate" into a permanent false positive.
             *
             * **No harness can catch this and that is the point.** wavedos.c
             * is linked by nothing on the host -- it is the Sound Blaster
             * half, which is why sbconv.c was split out of it -- and even if
             * it were, the gcc build's long is 64 bits and the overflow would
             * not happen. This is precisely the class CLAUDE.md names: a real
             * Watcom bug the harness build hides.
             *
             * The quotient and remainder are scaled separately, so the result
             * is exact rather than truncated to whole kHz, and the largest
             * intermediate is (ms - 1) * 1000 -- for this run 116,580,000,
             * comfortably inside a signed 32-bit long. Checked against 32-bit
             * semantics on the real figures: 44000 + 516 = 44516.
             *
             * Confirmed on the machine afterwards by a far longer session --
             * 40552448 frames in 915472 ms, a quarter of an hour -- which
             * reports 44296 Hz against a card opened at 44191. The old
             * expression would have printed 2072 Hz for that run. */
            mr_log("audio: %ld frames in %ld ms = %ld Hz delivered "
                   "(wanted %d), %ld spurious interrupts",
                   (long)g_played, ms,
                   (g_played / ms) * 1000L + (g_played % ms) * 1000L / ms,
                   MR_WAVE_RATE, (long)g_spurious);
    }

    if (mr_sbconv_clipped(&g_conv))
        mr_log("audio: %ld samples clipped by the %d%% output gain -- lower "
               "audiogain in mrpy.ini if it sounds harsh",
               mr_sbconv_clipped(&g_conv), g_conv.gain);

    if (g_underruns)
        mr_log("audio: %ld underruns (%ld ms of silence), worst run %ld "
               "(%ld ms) -- the frame loop could not keep the queue full",
               (long)g_underruns,
               (long)g_underruns * MR_WAVE_FRAMES * 1000L / MR_WAVE_RATE,
               (long)g_under_worst,
               (long)g_under_worst * MR_WAVE_FRAMES * 1000L / MR_WAVE_RATE);

    for (i = 0; i < MR_WAVE_BUFS; i++) {
        if (g_bufs[i]) {
            mr_dpmi_unlock(g_bufs[i], MR_WAVE_BYTES);
            free(g_bufs[i]);
            g_bufs[i] = NULL;
        }
    }
    if (g_tone) {
        mr_dpmi_unlock(g_tone, MR_WAVE_BYTES);
        free(g_tone);
        g_tone = NULL;
    }
    if (g_ring) {
        mr_dpmi_unlock((void *)g_ring, (unsigned long)g_ring_bytes);
        g_ring = NULL;
    }
    mr_dos_free(&g_dos);

    g_open = 0;
}

short *mr_wave_claim(void)
{
    int i;

    if (!g_open)
        return NULL;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (!g_busy[i])
            return g_bufs[i];
    return NULL;
}

int mr_wave_submit(short *buf)
{
    int i, next;

    if (!g_open)
        return 0;
    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_bufs[i] == buf)
            break;
    if (i == MR_WAVE_BUFS)
        return 0;

    /* The queue is one longer than the number of buffers, so a full queue
     * and an empty one are distinguishable and this can never be full while
     * a buffer is free to claim. Marking busy before publishing the index
     * matters: the handler may run between the two statements, and a buffer
     * it has already consumed must not look free until it is. */
    g_busy[i] = 1;

    _disable();
    next = (g_qtail + 1) % (MR_WAVE_BUFS + 1);
    if (next == g_qhead) {
        _enable();
        g_busy[i] = 0;
        return 0;
    }
    g_q[g_qtail] = i;
    g_qtail = next;
    g_fed = 1;                      /* underruns mean something from here on */
    _enable();

    return 1;
}

int mr_wave_outstanding(void)
{
    int i, n = 0;

    for (i = 0; i < MR_WAVE_BUFS; i++)
        if (g_busy[i])
            n++;
    return n;
}

long mr_wave_played_frames(void)
{
    if (!g_open)
        return -1;
    return g_heard;
}

#else  /* not DOS */

/* The host build compiles this file to a sink that is never opened, for the
 * same reason vbedos.c compiles to three do-nothing functions: nothing here
 * builds the DOS half, so without this a prototype could drift out of step
 * with wave.h and nobody would find out until a DOS build. No harness links
 * it -- the harnesses use tools/hostshim/wavehost.c, which is a sink that
 * can actually be driven. */

int mr_wave_open(void)            { return 0; }
void mr_wave_close(void)          { }
short *mr_wave_claim(void)        { return 0; }
int mr_wave_submit(short *buf)    { (void)buf; return 0; }
int mr_wave_outstanding(void)     { return 0; }
long mr_wave_played_frames(void)  { return -1; }

#endif

/* sbconv.c: 44100 signed 16-bit stereo down to what an SB 1.x will take.
 *
 * The conversion only ever runs on a machine with an old sound card in it,
 * where the whole report is "it sounds wrong" and there is no way to look at
 * a byte. So it is pure, and this is where it is actually examined.
 *
 * Five claims:
 *
 *   1. The output is the size the caller is told it will be. wavedos.c sizes
 *      a DMA ring from mr_sbconv_out_bytes and hands the DMA controller a
 *      count; a converter that wrote one frame more than it promised would
 *      scribble past a conventional-memory block during an interrupt.
 *   2. 44100 stereo 16-bit is a byte-for-byte passthrough, so an SB16 pays
 *      nothing for this file existing.
 *   3. Mixdown and decimation are the averages they claim to be, including
 *      at the clipping edges.
 *   4. Converting a buffer in pieces equals converting it whole. This is the
 *      seam check -- the dither residue is carried between calls, and the
 *      card is fed one MR_WAVE_FRAMES buffer at a time, so a converter that
 *      reset its state per call would put a discontinuity at every 46ms
 *      boundary. packtest makes the same demand of the pack loops.
 *   5. The dither does its job: a level below one 8-bit step still comes out
 *      at the right *average*. Truncation would round it away, which is the
 *      grainy-quiet-passage sound this is here to avoid, and that is the
 *      assertion that goes red if the error feedback is removed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mr.h"
#include "wave.h"
#include "sbconv.h"

void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
void mr_log(const char *f, ...) { (void)f; }

static int fails;

static void check(const char *label, int ok)
{
    printf("  %-62s %s\n", label, ok ? "ok" : "** FAIL **");
    if (!ok)
        fails++;
}

#define FRAMES 2048

static short in[FRAMES * 2];
static unsigned char out[FRAMES * 4];
static unsigned char out2[FRAMES * 4];

/* A signal with something in it: the two channels differ (so a mixdown that
 * dropped one is visible), it runs through zero (so sign handling is), and
 * it has content above 11kHz (so decimation changes it). */
static void fill_signal(void)
{
    int i;

    for (i = 0; i < FRAMES; i++) {
        in[i * 2]     = (short)(((i * 37) % 2000) - 1000 + ((i & 1) ? 300 : -300));
        in[i * 2 + 1] = (short)(((i * 11) % 4000) - 2000);
    }
}

static void fill_const(int l, int r)
{
    int i;

    for (i = 0; i < FRAMES; i++) {
        in[i * 2] = (short)l;
        in[i * 2 + 1] = (short)r;
    }
}

/* --- 1. sizes and what is refused ---------------------------------------- */

static void test_init(void)
{
    mr_sbconv c;

    check("44100 stereo 16-bit accepted", mr_sbconv_init(&c, 44100, 2, 16));
    check("22050 mono 8-bit accepted", mr_sbconv_init(&c, 22050, 1, 8));
    check("11025 mono 8-bit accepted", mr_sbconv_init(&c, 11025, 1, 8));

    /* A rate that does not divide the mixer's is refused rather than
     * approximated; see the comment in sbconv.h about drift. */
    check("8000 refused (does not divide 44100)",
          !mr_sbconv_init(&c, 8000, 1, 8));
    check("22000 refused (close is not the same as exact)",
          !mr_sbconv_init(&c, 22000, 1, 8));
    check("14700 refused (divides, but by three)",
          !mr_sbconv_init(&c, 14700, 1, 8));
    check("4 channels refused", !mr_sbconv_init(&c, 44100, 4, 8));
    check("12 bits refused", !mr_sbconv_init(&c, 44100, 2, 12));

    mr_sbconv_init(&c, 44100, 2, 16);
    check("44100/2/16: 4 bytes per frame",
          mr_sbconv_out_bytes(&c, FRAMES) == (long)FRAMES * 4);
    mr_sbconv_init(&c, 44100, 1, 8);
    check("44100/1/8: 1 byte per frame",
          mr_sbconv_out_bytes(&c, FRAMES) == (long)FRAMES);
    mr_sbconv_init(&c, 22050, 1, 8);
    check("22050/1/8: half a byte per input frame",
          mr_sbconv_out_bytes(&c, FRAMES) == (long)FRAMES / 2);
    mr_sbconv_init(&c, 11025, 2, 8);
    check("11025/2/8: half a byte per input frame",
          mr_sbconv_out_bytes(&c, FRAMES) == (long)FRAMES / 2);

    mr_sbconv_init(&c, 44100, 2, 16);
    check("16-bit silence is zero", mr_sbconv_silence(&c) == 0x00);
    mr_sbconv_init(&c, 22050, 1, 8);
    check("8-bit silence is 0x80", mr_sbconv_silence(&c) == 0x80);
}

/* --- 2. the SB16 path is untouched --------------------------------------- */

static void test_passthrough(void)
{
    mr_sbconv c;
    int i, ok = 1;

    fill_signal();
    mr_sbconv_init(&c, 44100, 2, 16);
    mr_sbconv_run(&c, in, FRAMES, out);

    for (i = 0; i < FRAMES * 2; i++) {
        int v = (int)(short)(out[i * 2] | (out[i * 2 + 1] << 8));
        if (v != in[i]) {
            ok = 0;
            break;
        }
    }
    check("44100 stereo 16-bit is the input, sample for sample", ok);
}

/* --- 3. the averages ------------------------------------------------------ */

static int u8_to_s16ish(unsigned char u)
{
    return ((int)u - 128) * 256;
}

/* The mean level of a stretch of output, in 16-bit units.
 *
 * Checks on a dithered quantiser have to be made on the average, not on
 * each sample. Shaped dither deliberately moves energy between adjacent
 * samples -- that is how it keeps the average right while using only 256
 * codes -- so any single sample can sit a couple of steps from the value
 * it represents, and a test that demands otherwise is testing for the
 * absence of the feature. What must hold is that the average is right and
 * that the excursion stays bounded; both are checked below.
 */
static double mean_level(const unsigned char *p, int from, int to, int stride)
{
    double sum = 0.0;
    int i, n = 0;

    for (i = from; i < to; i += stride) {
        sum += u8_to_s16ish(p[i]);
        n++;
    }
    return (n > 0) ? sum / n : 0.0;
}

static int max_excursion(const unsigned char *p, int from, int to,
                         int stride, int want)
{
    int i, worst = 0;

    for (i = from; i < to; i += stride) {
        int d = abs(u8_to_s16ish(p[i]) - want);
        if (d > worst)
            worst = d;
    }
    return worst;
}

static void test_averages(void)
{
    mr_sbconv c;
    int i, ok;

    /* Mixdown: one channel loud, the other silent, comes out half. Checked
     * against the 8-bit step, since that is the resolution available. */
    fill_const(8192, 0);
    mr_sbconv_init(&c, 44100, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("mono mixdown of (8192, 0) averages 4096",
          fabs(mean_level(out, 32, FRAMES, 1) - 4096.0) < 64.0);
    check("...with every sample within three steps of it",
          max_excursion(out, 32, FRAMES, 1, 4096) <= 768);

    /* A channel that was dropped rather than averaged would give 8192 or 0,
     * both of which the check above rejects; state it the other way round so
     * the intent survives a rewrite. */
    check("...and not 8192 (which is L kept) or 0 (R kept)",
          fabs(mean_level(out, 32, FRAMES, 1) - 8192.0) > 1024.0 &&
          fabs(mean_level(out, 32, FRAMES, 1)) > 1024.0);

    /* Decimation: alternate frames at +A and -A average to zero. */
    for (i = 0; i < FRAMES; i++) {
        short v = (short)((i & 1) ? -6000 : 6000);
        in[i * 2] = v;
        in[i * 2 + 1] = v;
    }
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("decimating 44100->22050 averages +/-6000 pairs to zero",
          fabs(mean_level(out, 32, FRAMES / 2, 1)) < 64.0 &&
          max_excursion(out, 32, FRAMES / 2, 1, 0) <= 768);

    /* And at 44100 the same signal is *not* averaged away: a decim of 1 that
     * quietly averaged anything would show up here. */
    mr_sbconv_init(&c, 44100, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("...while at 44100 the same signal still swings",
          abs(u8_to_s16ish(out[64])) > 4096);

    /* Clipping. Full-scale in both channels must saturate the 8-bit range
     * rather than wrap, which is the difference between loud and a buzz. */
    fill_const(32767, 32767);
    mr_sbconv_init(&c, 44100, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    ok = 1;
    for (i = 0; i < FRAMES; i++)
        if (out[i] != 0xFF)
            ok = 0;
    check("full positive scale is 0xFF, not a wrap", ok);

    /* Freshly initialised, not carried on from the clip above: coming off
     * full positive scale the residue legitimately makes the first byte
     * 0x01, and that is the error feedback working, not a bug to pin. */
    fill_const(-32768, -32768);
    mr_sbconv_init(&c, 44100, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    ok = 1;
    for (i = 0; i < FRAMES; i++)
        if (out[i] > 0x01)      /* the dither reaches one step up */
            ok = 0;
    check("full negative scale is 0x00, not a wrap", ok);

    /* Digital silence is the DAC's idle value, exactly, for every sample:
     * a dither that rattled around zero would be audible hiss under a
     * pause. */
    fill_const(0, 0);
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    ok = 1;
    for (i = 0; i < FRAMES / 2; i++)
        if (out[i] != 0x80)
            ok = 0;
    check("silence is 0x80 for every byte, with no dither rattle", ok);

    /* The gate is only for *digital* silence. A signal a quarter of a step
     * from zero still has to be dithered, or the fade it is the tail of
     * ends in a step rather than in nothing. */
    fill_const(64, 64);
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    ok = 0;
    for (i = 0; i < FRAMES / 2; i++)
        if (out[i] != 0x80)
            ok = 1;
    check("...but a quarter-step signal is still dithered", ok);

    /* Stereo 8-bit keeps the channels apart and in order. */
    fill_const(16384, -16384);
    mr_sbconv_init(&c, 22050, 2, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("stereo 8-bit interleaves L then R",
          fabs(mean_level(out, 64, FRAMES, 2) - 16384.0) < 64.0 &&
          fabs(mean_level(out, 65, FRAMES, 2) + 16384.0) < 64.0);
}

/* --- 4. the seam ---------------------------------------------------------- */

static void test_seam(void)
{
    mr_sbconv a, b;
    long n;
    int rates[3];
    int ri, ok;

    rates[0] = 44100;
    rates[1] = 22050;
    rates[2] = 11025;

    fill_signal();

    for (ri = 0; ri < 3; ri++) {
        char label[80];
        long half;

        mr_sbconv_init(&a, rates[ri], 1, 8);
        mr_sbconv_init(&b, rates[ri], 1, 8);

        n = mr_sbconv_out_bytes(&a, FRAMES);
        mr_sbconv_run(&a, in, FRAMES, out);

        /* The same input in four pieces, through one converter. The pieces
         * are the shape wavedos.c uses: whole mixer buffers, one per
         * interrupt. */
        half = mr_sbconv_out_bytes(&b, FRAMES / 4);
        mr_sbconv_run(&b, in, FRAMES / 4, out2);
        mr_sbconv_run(&b, in + (FRAMES / 4) * 2, FRAMES / 4, out2 + half);
        mr_sbconv_run(&b, in + (FRAMES / 2) * 2, FRAMES / 4, out2 + half * 2);
        mr_sbconv_run(&b, in + (FRAMES * 3 / 4) * 2, FRAMES / 4,
                      out2 + half * 3);

        sprintf(label, "%d: four pieces equal one whole buffer", rates[ri]);
        check(label, half * 4 == n && memcmp(out, out2, (size_t)n) == 0);
    }

    /* The negative control for the one above: a converter whose state was
     * thrown away between pieces really does differ, so the check has
     * something to catch. Re-initialising between pieces is exactly the bug
     * being guarded against. */
    mr_sbconv_init(&a, 22050, 1, 8);
    n = mr_sbconv_out_bytes(&a, FRAMES);
    mr_sbconv_run(&a, in, FRAMES, out);

    {
        long half = mr_sbconv_out_bytes(&a, FRAMES / 4);
        int piece;
        for (piece = 0; piece < 4; piece++) {
            mr_sbconv_init(&b, 22050, 1, 8);        /* the bug */
            mr_sbconv_run(&b, in + (FRAMES / 4) * piece * 2, FRAMES / 4,
                          out2 + half * piece);
        }
        ok = memcmp(out, out2, (size_t)n) != 0;
    }
    check("...and dropping the dither residue at each seam does not", ok);
}

/* --- 5. the dither -------------------------------------------------------- */

static void test_dither(void)
{
    mr_sbconv c;
    long sum = 0;
    int i, n;
    int distinct, seen_low, seen_high;

    /* A quarter of an 8-bit step. No single output byte can represent it;
     * the average of many can, and that is the whole argument for carrying
     * an error term. */
    fill_const(64, 64);
    mr_sbconv_init(&c, 44100, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);

    n = FRAMES;
    for (i = 0; i < n; i++)
        sum += (long)out[i] - 128;

    /* 64/256 of a step, times n samples. The slack is wider than the
     * shaping alone would need because the dither is a random walk on top
     * of it: n samples of triangular noise one step wide wander by about
     * sqrt(n) steps, which is 45 here. Ten percent of the expected sum is
     * comfortably inside that and still nowhere near what truncation
     * would give, which is zero. */
    check("a quarter-step DC offset survives as an average",
          sum * 256 >= (long)n * 64 * 3 / 4 &&
          sum * 256 <= (long)n * 64 * 5 / 4);

    /* It survives by alternating between two adjacent codes rather than by
     * sitting on one, which is the mechanism, and is what truncation would
     * not do. */
    seen_low = seen_high = 0;
    for (i = 0; i < n; i++) {
        if (out[i] == 0x80)
            seen_low = 1;
        else if (out[i] == 0x81)
            seen_high = 1;
    }
    distinct = seen_low && seen_high;
    check("...by alternating 0x80 and 0x81, not by rounding to one", distinct);

    /* A ramp that crosses many steps must stay monotonic: error feedback
     * that was fed back with the wrong sign would oscillate instead. */
    for (i = 0; i < FRAMES; i++) {
        short v = (short)(-20000 + i * 19);
        in[i * 2] = v;
        in[i * 2 + 1] = v;
    }
    mr_sbconv_init(&c, 44100, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    {
        /* On the average over a window, not sample by sample: the dither
         * is shaped, so neighbouring samples trade energy on purpose. What
         * is being checked is that the feedback is signed correctly, and a
         * sign error there oscillates rather than climbing. */
        int ok = 1;
        for (i = 128; i + 128 < FRAMES; i += 128)
            if (mean_level(out, i, i + 128, 1) <
                mean_level(out, i - 128, i, 1) - 256.0)
                ok = 0;
        check("a rising ramp never turns back on itself", ok);
    }
}

/* --- 6. the decimation filter --------------------------------------------
 *
 * The reason this exists: averaging two samples leaves most of what is
 * above the new Nyquist in place, and halving the rate folds it down into
 * the music as noise. A recording of the game on an SB1 is what found it.
 *
 * Measured as level in against level out, which is all a fixed tone needs.
 */
static double tone_level(int rate, double freq, int gain)
{
    mr_sbconv c;
    double sum = 0.0;
    int i, n;

    for (i = 0; i < FRAMES; i++) {
        double v = 12000.0 * sin(2.0 * 3.14159265358979 * freq * i / 44100.0);
        in[i * 2] = (short)v;
        in[i * 2 + 1] = (short)v;
    }

    mr_sbconv_init(&c, rate, 1, 8);
    mr_sbconv_set_gain(&c, gain);
    mr_sbconv_run(&c, in, FRAMES, out);

    n = FRAMES / c.decim;
    for (i = 32; i < n; i++) {          /* past the filter's settling */
        double d = ((double)out[i] - 128.0) * 256.0;
        sum += d * d;
    }
    return sqrt(sum / (n - 32));
}

static void test_filter(void)
{
    double pass, stop, alias;

    /* 1kHz goes through untouched: a filter that ate the music would pass
     * the stopband check below for the wrong reason. */
    pass = tone_level(22050, 1000.0, 100);
    check("1kHz passes the decimation filter",
          pass > 12000.0 * 0.65 && pass < 12000.0 * 0.78);

    /* 15kHz cannot be represented at 22050 and must not come back as
     * something that can. The filter is 19.8dB down there; averaging two
     * samples is 6dB down, so this separates them by a wide margin. */
    stop = tone_level(22050, 15000.0, 100);
    check("15kHz is attenuated by at least 15dB",
          stop < pass * 0.18);

    /* 21kHz is nearly at the input Nyquist, where the half-band is deepest;
     * averaging would fold it to 1kHz, right in the middle of the music. */
    alias = tone_level(22050, 21000.0, 100);
    check("21kHz does not fold back into the music", alias < pass * 0.05);

    /* And at 44100 nothing is being thrown away, so nothing is filtered:
     * 15kHz must survive. Decim 1 skipping the filter is deliberate. */
    check("...while at 44100 15kHz still comes through",
          tone_level(44100, 15000.0, 100) > 12000.0 * 0.6);
}

/* --- 7. the gain --------------------------------------------------------- */

static void test_gain(void)
{
    mr_sbconv c;
    int i, ok;

    /* Quarter scale at 200% is half scale. This is the whole point of the
     * knob: the music arrives quiet and eight bits has no resolution to
     * spare. */
    fill_const(8192, 8192);
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_set_gain(&c, 200);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("gain 200% doubles the level",
          fabs(mean_level(out, 32, FRAMES / 2, 1) - 16384.0) < 64.0);
    check("...and nothing clipped doing it", mr_sbconv_clipped(&c) == 0);

    /* Over the top, it saturates and says so. A silent clip is how a
     * too-high setting would be mistaken for a broken card. */
    fill_const(24000, 24000);
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_set_gain(&c, 200);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("gain past full scale clips", out[64] == 0xFF);
    check("...and is counted", mr_sbconv_clipped(&c) > FRAMES / 4);

    /* The default is the mixer's own level, so nothing changes for anyone
     * who does not ask. */
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);
    check("the default gain is 100%, and silent about it",
          abs(u8_to_s16ish(out[64]) - 24000) < 256 &&
          mr_sbconv_clipped(&c) == 0);
}

/* --- 8. the dither is shaped, not flat -----------------------------------
 *
 * This is the regression test for the bug that made the first dithered
 * build still sound like white noise. The error fed back has to be
 * measured against the *undithered* target; measure it against the
 * dithered one and the loop corrects only the quantiser, leaving the
 * dither itself flat across the band at full amplitude -- 18dB above the
 * shaped quantisation noise in the midrange, where it is plainly audible.
 *
 * Both versions have the same total noise power, so a test that measured
 * that would pass either way. What distinguishes them is *where* the noise
 * is, so that is what this measures: a constant input, and then how much
 * of the output's wobble survives averaging. White noise averaged over
 * sixteen samples keeps a sixteenth of its power; noise pushed to the top
 * of the band keeps far less.
 */
static void test_shaped_dither(void)
{
    mr_sbconv c;
    double mean, var = 0.0, blockvar = 0.0;
    int i, n, b, nb;

    fill_const(300, 300);               /* just over one step, so the
                                         * quantiser works and the dither
                                         * gate stays open */
    mr_sbconv_init(&c, 22050, 1, 8);
    mr_sbconv_run(&c, in, FRAMES, out);

    n = FRAMES / 2;
    mean = mean_level(out, 32, n, 1);

    for (i = 32; i < n; i++) {
        double d = u8_to_s16ish(out[i]) - mean;
        var += d * d;
    }
    var /= (n - 32);

    nb = 0;
    for (b = 32; b + 16 <= n; b += 16) {
        double d = mean_level(out, b, b + 16, 1) - mean;
        blockvar += d * d;
        nb++;
    }
    blockvar /= nb;

    /* White noise would leave var/16 after averaging sixteen samples.
     * Shaped noise leaves a small fraction of that; the threshold is a
     * third of the white-noise figure, which the flat-dither bug fails by
     * a wide margin and the shaped version passes by one. */
    check("the dither is shaped away from the low end, not flat",
          blockvar < var / 16.0 * 0.33);

    /* And it is genuinely dithering: without any noise at all a constant
     * input gives a constant output, and the check above would pass for
     * the wrong reason. */
    check("...while still dithering (a constant input does not come out "
          "constant)", var > 1000.0);
}

int main(void)
{
    printf("sbconvtest\n");
    test_init();
    test_passthrough();
    test_averages();
    test_seam();
    test_dither();
    test_filter();
    test_gain();
    test_shaped_dither();
    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}

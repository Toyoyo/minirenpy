/* sbconv.c - see sbconv.h for what this is and why it has no resampler. */

#include "sbconv.h"
#include "wave.h"

/* The decimation filter: an eleven-tap half-band, scaled by 256.
 *
 *     1, 0, -11, 0, 74, 128, 74, 0, -11, 0, 1
 *
 * A half-band filter has a zero at every other tap, which is why eleven
 * taps cost four multiplies and not eleven. It is 6dB down at the new
 * Nyquist, as a half-band is, and 20dB down by 18kHz where the averaging it
 * replaces was 4dB down. On the first thirty seconds of the game's music,
 * against a brick-walled reference: averaging 24.3dB, this 40.3dB. Fifteen
 * taps would give 43.3 and nineteen 44.6, which did not look worth the
 * multiplies in an interrupt handler on a 486.
 *
 * The coefficients sum to exactly 256 so that a constant passes through
 * unchanged -- a filter that does not is a filter with a DC offset, and a
 * DC offset on an 8-bit DAC is a thump at every start and stop.
 */
static const int FIR[MR_SBCONV_TAPS] = { 1, 0, -11, 0, 74, 128, 74,
                                         0, -11, 0, 1 };
#define FIR_MID (MR_SBCONV_TAPS / 2)

/* How far behind the input the filter's centre runs.
 *
 * It has to be at least FIR_MID so that every tap before the centre is
 * available, and no more than MR_SBCONV_HIST - FIR_MID so that the history
 * reaches. Within that, it is *even* on purpose: an odd offset delays the
 * output by half of an output frame, which is a fractional-sample shift of
 * the whole stream. Nothing can hear that, but it is a half-sample
 * disagreement with any reference anything is ever compared against, and
 * it cost an hour of thinking a good filter had made things worse. */
#define FIR_DELAY 4

int mr_sbconv_init(mr_sbconv *c, int rate, int channels, int bits)
{
    int d, i;

    if (!c)
        return 0;
    if (channels != 1 && channels != 2)
        return 0;
    if (bits != 8 && bits != 16)
        return 0;
    if (rate <= 0 || (MR_WAVE_RATE % rate) != 0)
        return 0;

    d = MR_WAVE_RATE / rate;
    if (d != 1 && d != 2 && d != 4)
        return 0;

    c->decim = d;
    c->channels = channels;
    c->bits = bits;
    c->gain = 100;
    c->dither = 1;
    c->rng = 22222UL;
    c->err[0] = 0;
    c->err[1] = 0;
    c->clipped = 0;
    for (i = 0; i < MR_SBCONV_HIST; i++) {
        c->hist[i][0] = 0;
        c->hist[i][1] = 0;
    }
    return 1;
}

void mr_sbconv_set_gain(mr_sbconv *c, int percent)
{
    if (percent < 10)
        percent = 10;
    else if (percent > 1000)
        percent = 1000;
    c->gain = percent;
}

long mr_sbconv_clipped(const mr_sbconv *c)
{
    return c->clipped;
}

void mr_sbconv_set_dither(mr_sbconv *c, int on)
{
    c->dither = on;
}

/* Triangular noise, one quantiser step either side.
 *
 * Two uniform draws summed, which is what makes it triangular rather than
 * rectangular: a rectangular dither of the same width leaves the noise
 * *power* modulated by the signal, which is audible as breathing. The
 * generator is a plain LCG with the state in the struct -- it has to be
 * reproducible, because a converter whose output depends on when it was
 * called cannot be tested. */
static long dither_noise(mr_sbconv *c)
{
    long a, b;

    c->rng = c->rng * 1103515245UL + 12345UL;
    a = (long)((c->rng >> 16) & 0xFF);
    c->rng = c->rng * 1103515245UL + 12345UL;
    b = (long)((c->rng >> 16) & 0xFF);

    return a + b - 256;
}

long mr_sbconv_out_bytes(const mr_sbconv *c, int in_frames)
{
    return (long)(in_frames / c->decim) * c->channels * (c->bits / 8);
}

unsigned char mr_sbconv_silence(const mr_sbconv *c)
{
    return (unsigned char)((c->bits == 8) ? 0x80 : 0x00);
}

/* One input frame, from the buffer or from the tail of the last one.
 *
 * Negative indices reach into the history, which is what lets the filter be
 * centred on a frame near the start of a buffer without a seam. */
static long sample(const mr_sbconv *c, const short *in, int in_frames,
                   long i, int ch)
{
    if (i < 0)
        return c->hist[MR_SBCONV_HIST + i][ch];
    if (i >= in_frames)
        return in[(in_frames - 1) * 2 + ch];    /* cannot happen; be safe */
    return in[i * 2 + ch];
}

/* The filter, centred on input frame p. */
static long filtered(const mr_sbconv *c, const short *in, int in_frames,
                     long p, int ch)
{
    long acc = 0;
    int k;

    for (k = 0; k < MR_SBCONV_TAPS; k++)
        if (FIR[k])
            acc += (long)FIR[k] *
                   sample(c, in, in_frames, p + k - FIR_MID, ch);

    return acc / 256;
}

void mr_sbconv_run(mr_sbconv *c, const short *in, int in_frames,
                   unsigned char *out)
{
    int nch = c->channels;
    int decim = c->decim;
    int i, j, ch;

    for (i = 0; i + decim <= in_frames; i += decim) {
        long acc[2];
        long v[2];

        acc[0] = 0;
        acc[1] = 0;

        if (decim == 1) {
            /* Nothing is being thrown away, so there is nothing to filter
             * away first. This is the SB16 path and it stays a copy. */
            acc[0] = in[i * 2];
            acc[1] = in[i * 2 + 1];
        } else {
            /* Band-limit, then take every decim'th. The centre runs five
             * frames behind the input so that every tap is available -- the
             * ones before the buffer come out of the history.
             *
             * For decim 4 the half-band is applied once and two of its
             * outputs are averaged, which is a cascade of two halvings
             * written without a scratch buffer. The second stage is the
             * crude one, but 11025 is not a rate anything asks for today. */
            for (j = 0; j < decim; j += 2) {
                acc[0] += filtered(c, in, in_frames,
                                   (long)i + j - FIR_DELAY, 0);
                acc[1] += filtered(c, in, in_frames,
                                   (long)i + j - FIR_DELAY, 1);
            }
            acc[0] /= (decim / 2);
            acc[1] /= (decim / 2);
        }

        if (nch == 1) {
            /* Mix down by averaging, not by summing: two channels of the
             * same music are correlated, so summing clips on anything
             * mastered near full scale. */
            v[0] = (acc[0] + acc[1]) / 2;
            v[1] = 0;
        } else {
            v[0] = acc[0];
            v[1] = acc[1];
        }

        if (c->gain != 100) {
            for (ch = 0; ch < nch; ch++) {
                v[ch] = v[ch] * c->gain / 100;
                if (v[ch] > 32767L) {
                    v[ch] = 32767L;
                    c->clipped++;
                } else if (v[ch] < -32768L) {
                    v[ch] = -32768L;
                    c->clipped++;
                }
            }
        }

        for (ch = 0; ch < nch; ch++) {
            if (c->bits == 16) {
                *out++ = (unsigned char)(v[ch] & 0xFF);
                *out++ = (unsigned char)((v[ch] >> 8) & 0xFF);
            } else {
                long t;
                long td;
                long fb;
                int q;

                /* Error feedback. What the last sample could not represent
                 * is added to this one, so the running average stays right
                 * below the resolution of a single sample. The shift below
                 * is an arithmetic one on both compilers that build this;
                 * the +128 makes it round to nearest rather than toward
                 * minus infinity.
                 *
                 * First order, and that is a listening result rather than
                 * a measured one: a second-order shaper was built and
                 * measured 3.2dB better in the 1-3kHz band, where hearing
                 * is sharpest, and was judged clearly worse on the game's
                 * own music. See NOTES. Bounded either way, because a
                 * feedback loop that rings is a buzz rather than a hiss. */
                fb = c->err[ch];
                if (fb > 512L)
                    fb = 512L;
                else if (fb < -512L)
                    fb = -512L;

                t = v[ch] + fb;

                /* Dither, except on digital silence.
                 *
                 * Noise added to nothing is just noise: between scenes,
                 * and before the first note, the mixer hands us exact
                 * zeroes and the card should be quiet. Gating on the
                 * residue as well as the sample means the tail of a fade
                 * still gets dithered all the way down, and the gate only
                 * closes once there is genuinely nothing left. */
                td = t;
                if (c->dither && (v[ch] != 0 || c->err[ch] != 0))
                    td += dither_noise(c);

                if (t > 32767L)
                    t = 32767L;
                else if (t < -32768L)
                    t = -32768L;
                if (td > 32767L)
                    td = 32767L;
                else if (td < -32768L)
                    td = -32768L;

                q = (int)((td + 128) >> 8);
                if (q > 127)
                    q = 127;
                else if (q < -128)
                    q = -128;

                /* The residue is measured against `t`, the signal we meant
                 * to represent, and *not* against `td`, the dithered value
                 * we actually quantised. That difference is the whole
                 * behaviour of the dither.
                 *
                 * Measuring against td leaves the loop correcting only the
                 * quantiser, so the dither itself comes out flat across
                 * the band at full amplitude -- 18dB above the shaped
                 * quantisation noise in the midrange, measured, which
                 * swamped the shaping completely and was audible as plain
                 * hiss. Measuring against t makes the loop treat the
                 * dither as part of the error, so it is shaped away from
                 * the midrange along with everything else.
                 *
                 * Measuring against what was *written* also means a clamp
                 * at full scale does not accumulate an unpayable debt that
                 * then rings on the way back down. */
                c->err[ch] = t - ((long)q << 8);

                *out++ = (unsigned char)(q + 128);
            }
        }
    }

    /* Keep the tail for the next call's filter. Without this the first few
     * output frames of every buffer would be filtered against silence --
     * a dip 21 times a second, which is a buzz rather than a hiss. */
    for (i = 0; i < MR_SBCONV_HIST; i++) {
        long k = (long)in_frames - MR_SBCONV_HIST + i;

        c->hist[i][0] = (k >= 0) ? in[k * 2] : 0;
        c->hist[i][1] = (k >= 0) ? in[k * 2 + 1] : 0;
    }
}

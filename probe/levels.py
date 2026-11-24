"""Where the noise in the 8-bit path actually comes from, in order of size.

Three candidates, measured together on the real music rather than argued
about:

  aliasing      the two-sample average is a poor decimation filter
  resolution    eight bits of a signal that only uses part of the range
  shaping       where the quantiser puts the noise it cannot avoid

The third column is the one that decides what to implement.
"""
import numpy as np

RATE = 44100
src = np.fromfile('probe/src.raw', dtype='<i2').astype(np.float64)
src = src.reshape(-1, 2).mean(axis=1)[:44100 * 15]

n = len(src)
sp = np.fft.rfft(src)
f = np.fft.rfftfreq(n, 1.0 / RATE)
sp[f > 11025 * 0.95] = 0
ideal = np.fft.irfft(sp)[::2]

print("source: peak %.0f (%.1f dBFS), rms %.0f (%.1f dBFS)"
      % (np.abs(src).max(), 20 * np.log10(np.abs(src).max() / 32768.0),
         np.sqrt((src ** 2).mean()),
         20 * np.log10(np.sqrt((src ** 2).mean()) / 32768.0)))
print("the engine then applies the player's music volume on top of that.\n")


def quant(x, order):
    out = np.empty(len(x))
    e1 = e2 = 0.0
    for i in range(len(x)):
        fb = e1 if order == 1 else 2.0 * e1 - e2
        t = min(max(x[i] + fb, -32768), 32767)
        q = np.floor((t + 128) / 256.0)
        q = min(max(q, -128), 127)
        e2, e1 = e1, t - q * 256
        out[i] = q * 256
    return out


def halfband(ntaps):
    m = (ntaps - 1) / 2.0
    i = np.arange(ntaps) - m
    h = np.sinc(i / 2.0) * np.hamming(ntaps)
    return h / h.sum()


def snr(y, gain):
    k = min(len(y), len(ideal))
    b = ideal[:k] * gain
    d = y[:k] - b
    return 10 * np.log10((b ** 2).sum() / max((d ** 2).sum(), 1e-9))


print("%-34s %6s %6s" % ("", "SNR", "clipped"))
for label, taps in (("average two (today)", None), ("7-tap sinc", 7),
                    ("15-tap sinc", 15)):
    for gain in (1.0, 2.0, 3.0):
        for order in (1, 2):
            if taps is None:
                y = (src[0::2] + src[1::2]) / 2.0
            else:
                y = np.convolve(src, halfband(taps), mode='same')[::2]
            y = y * gain
            clip = float((np.abs(y) > 32767).mean() * 100.0)
            y = quant(y, order)
            print("%-34s %6.1f %6.2f%%"
                  % ("%s, gain %.0fx, order %d" % (label, gain, order),
                     snr(y, gain), clip))

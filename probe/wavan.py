"""Look at a recording of the DOS build's audio output.

Not a test -- a measurement, to tell two faults apart that sound alike:

  starvation   periodic gaps or repeats at the 46ms buffer boundary
  plumbing     broadband noise spread evenly through the signal

For the generated tone the answer is quantitative: everything that is not
430.66Hz is error, and where it sits in time says which fault it is.
"""
import sys
import wave

import numpy as np


def load(path):
    w = wave.open(path)
    n = w.getnframes()
    a = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float64)
    a = a.reshape(-1, w.getnchannels())
    return a, w.getframerate()


def blocks(x, rate, ms=46.4):
    n = int(rate * ms / 1000.0)
    k = len(x) // n
    return x[:k * n].reshape(k, n)


def report_tone(path, freq=44100.0 * 20.0 / 2048.0):
    x, rate = load(path)
    mono = x.mean(axis=1)
    print("%s: %d ch, %d Hz, %.1f s" % (path, x.shape[1], rate, len(x) / rate))
    print("  expecting a %.2f Hz tone" % freq)

    # Where the signal actually is, so leading silence is not measured.
    b = blocks(mono, rate)
    rms = np.sqrt((b ** 2).mean(axis=1))
    loud = rms > rms.max() * 0.2
    print("  %d blocks of %.1f ms, %d of them above a fifth of peak"
          % (len(b), 1000.0 * b.shape[1] / rate, loud.sum()))

    # Gaps: blocks that are near-silent in the middle of loud ones.
    first, last = np.argmax(loud), len(loud) - 1 - np.argmax(loud[::-1])
    inner = rms[first:last + 1]
    quiet = inner < rms.max() * 0.05
    print("  inside the tone: %d blocks, %d of them near silent (%.1f%%)"
          % (len(inner), quiet.sum(), 100.0 * quiet.sum() / max(1, len(inner))))

    # Spectrum of the loud part: tone power against everything else.
    seg = mono[first * b.shape[1]:(last + 1) * b.shape[1]]
    n = 1 << int(np.floor(np.log2(len(seg))))
    seg = seg[:n] * np.hanning(n)
    sp = np.abs(np.fft.rfft(seg)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / rate)

    peak = int(np.argmax(sp[1:])) + 1
    print("  loudest component at %.1f Hz" % f[peak])

    band = (np.abs(f - freq) < 15.0)
    tone = sp[band].sum()
    total = sp[1:].sum()
    print("  tone / (everything else) = %.1f dB"
          % (10.0 * np.log10(tone / max(total - tone, 1e-30))))

    # Where the rest of the energy is: harmonics (distortion) or spread
    # (noise), and whether it clusters at low frequency (chop).
    for lo, hi in [(0, 300), (300, 600), (600, 2000), (2000, 6000),
                   (6000, 11000), (11000, 22050)]:
        m = (f >= lo) & (f < hi)
        print("    %5d-%5d Hz  %6.1f dB rel tone"
              % (lo, hi, 10.0 * np.log10(max(sp[m].sum(), 1e-30) / tone)))

    # Amplitude stability block to block: starvation shows up here.
    inner_rms = rms[first:last + 1]
    print("  block RMS: mean %.0f, min %.0f, max %.0f, sd/mean %.3f"
          % (inner_rms.mean(), inner_rms.min(), inner_rms.max(),
             inner_rms.std() / inner_rms.mean()))

    # Runs of blocks that are much quieter than their neighbours.
    drop = inner_rms < inner_rms.mean() * 0.5
    runs, cur = [], 0
    for d in drop:
        if d:
            cur += 1
        elif cur:
            runs.append(cur)
            cur = 0
    if cur:
        runs.append(cur)
    print("  %d dropout runs, longest %d blocks"
          % (len(runs), max(runs) if runs else 0))


def report_music(path):
    x, rate = load(path)
    mono = x.mean(axis=1)
    b = blocks(mono, rate)
    rms = np.sqrt((b ** 2).mean(axis=1))
    print("%s: %.1f s, %d blocks of 46.4 ms" % (path, len(x) / rate, len(b)))
    loud = rms > rms.max() * 0.2
    first = np.argmax(loud)
    inner = rms[first:]
    quiet = inner < inner.mean() * 0.1
    print("  after the first loud block: %d blocks, %d under a tenth of the "
          "mean (%.1f%%)" % (len(inner), quiet.sum(),
                             100.0 * quiet.sum() / max(1, len(inner))))
    print("  block RMS: mean %.0f, sd/mean %.3f"
          % (inner.mean(), inner.std() / inner.mean()))

    # Exact-zero and exact-0x80-equivalent runs: an inserted silent half is
    # a constant, which music never is.
    d = np.abs(np.diff(mono))
    flat = d == 0
    runs, cur, out = [], 0, 0
    for fl in flat:
        if fl:
            cur += 1
        elif cur:
            runs.append(cur)
            cur = 0
    runs = np.array(runs) if runs else np.array([0])
    print("  longest run of identical consecutive samples: %d (%.1f ms)"
          % (runs.max(), 1000.0 * runs.max() / rate))
    print("  runs over 200 samples (4.5ms of hold): %d"
          % (runs > 200).sum())


if __name__ == '__main__':
    report_tone(sys.argv[1])
    print()
    report_music(sys.argv[2])

"""Find the glitches in the recorded tone and measure how far apart they are.

A spacing names the culprit directly:

  46.4 ms   one mixer buffer / one DMA half  -- the refill is the fault
  23.2 ms   twice per half
  something else  -- not the sink's bookkeeping

Method: the tone is a known sine, so fit frequency and phase over a short
window and look at where the residual jumps. No filtering, so a click shows
up where it happens rather than smeared over the filter's length.
"""
import sys
import wave

import numpy as np


def load(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2')
    a = a.astype(np.float64).reshape(-1, w.getnchannels()).mean(axis=1)
    return a, w.getframerate()


x, rate = load(sys.argv[1])

# Work on a loud stretch.
n = len(x)
x = x[n // 3: 2 * n // 3]

# Sample-to-sample difference of a sine is bounded; a seam is not.
d = np.diff(x)
thresh = np.percentile(np.abs(d), 99.5) * 4
spikes = np.where(np.abs(d) > thresh)[0]

# Group adjacent samples into one event.
events = []
for s in spikes:
    if not events or s - events[-1] > 20:
        events.append(s)
print("%d glitch events in %.2f s (%.1f per second), threshold %.0f"
      % (len(events), len(x) / rate, len(events) * rate / len(x), thresh))

if len(events) > 2:
    gaps = np.diff(events) / rate * 1000.0
    print("gaps between them, ms: median %.2f, mean %.2f, min %.2f, max %.2f"
          % (np.median(gaps), gaps.mean(), gaps.min(), gaps.max()))
    hist, edges = np.histogram(gaps, bins=20)
    for h, e0, e1 in zip(hist, edges[:-1], edges[1:]):
        if h:
            print("   %6.1f - %6.1f ms : %s" % (e0, e1, "#" * min(h, 60)))

# And what one looks like: the samples around the first few.
print("\nsamples around three of them (card output is held across 2 "
      "recorded samples at 22kHz):")
for e in events[1:4]:
    seg = x[e - 8: e + 9].astype(int)
    print("  " + " ".join("%6d" % v for v in seg))

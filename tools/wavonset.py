#!/usr/bin/env python3
"""Report how much silence each WAV begins with.

Leading silence in a hit sound is latency the engine cannot do anything
about: it is added to whatever the mixer queue costs, and the player hears
the sum. Worth knowing before blaming the mixer.
"""
import array
import struct
import sys


def onset(path):
    d = open(path, "rb").read()
    i, ch, rate, off, n = 12, 0, 0, 0, 0
    while i + 8 <= len(d):
        cid, sz = d[i:i + 4], struct.unpack("<I", d[i + 4:i + 8])[0]
        if cid == b"fmt ":
            ch, rate = struct.unpack("<HI", d[i + 10:i + 16])
        if cid == b"data":
            off, n = i + 8, sz
            break
        i += 8 + sz + (sz & 1)
    if not rate or not n:
        return None

    a = array.array("h")
    a.frombytes(d[off:off + (min(n, len(d) - off) & ~1)])
    first = next((j for j, v in enumerate(a) if v), None)
    if first is None:
        return ch, rate, len(a) // max(ch, 1), None
    return ch, rate, len(a) // max(ch, 1), (first // max(ch, 1)) * 1000.0 / rate


for p in sys.argv[1:]:
    r = onset(p)
    if not r:
        print("%-44s unreadable" % p)
        continue
    ch, rate, frames, ms = r
    print("%-44s %dch %dHz %6.0fms long, silence first: %s"
          % (p, ch, rate, frames * 1000.0 / rate,
             "all of it" if ms is None else "%.1fms" % ms))

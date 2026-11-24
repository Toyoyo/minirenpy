#!/usr/bin/env python3
"""Write the default mouse pointer as a PNG.

    python3 tools/mkarrow.py tools/cursor.png

The DOS build has to draw its own pointer -- there is no Windows to ask for
one -- and the artwork for it comes from a PNG at build time, through
tools/mkcursor.py. This writes the default one.

The art lives here as ASCII rather than as a committed binary for one
reason: a 12x19 PNG in the repository is a file nobody can read, review or
patch, and the whole point of the pointer coming from a PNG is that it is
meant to be replaced. Replacing it does not mean editing this script -- it
means pointing CURSOR_PNG at your own file. This is just the fallback, and
keeping its source legible costs twenty lines.

The glyph is the usual arrow: a white body with a black outline, so it is
visible against both a dark background and a light one, which a plain white
or plain black pointer is not.
"""

import struct
import sys
import zlib

#   .  transparent
#   #  black outline
#   *  white body
ARROW = [
    "#...........",
    "##..........",
    "#*#.........",
    "#**#........",
    "#***#.......",
    "#****#......",
    "#*****#.....",
    "#******#....",
    "#*******#...",
    "#********#..",
    "#*****####..",
    "#**#**#.....",
    "#*#.#**#....",
    "##..#**#....",
    "#....#**#...",
    ".....#**#...",
    "......#**#..",
    "......#**#..",
    ".......##...",
]

# Where the hot spot is, in pixels from the top left. The tip of the arrow,
# which for this glyph is its first pixel. mkcursor.py reads it from the
# command line or defaults to 0,0; it is written here as a comment for the
# person who replaces the art and wonders where it went.
HOTSPOT = (0, 0)


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: mkarrow.py <out.png>\n")
        return 1

    h = len(ARROW)
    w = max(len(row) for row in ARROW)

    raw = bytearray()
    for row in ARROW:
        raw.append(0)                       # filter type 0, none
        for x in range(w):
            c = row[x] if x < len(row) else "."
            if c == "#":
                raw += bytes((0, 0, 0, 255))
            elif c == "*":
                raw += bytes((255, 255, 255, 255))
            else:
                raw += bytes((0, 0, 0, 0))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           chunk(b"IEND", b""))

    with open(sys.argv[1], "wb") as f:
        f.write(png)

    print("%s: %dx%d, hotspot %d,%d" % (sys.argv[1], w, h,
                                        HOTSPOT[0], HOTSPOT[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

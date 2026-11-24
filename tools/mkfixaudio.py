#!/usr/bin/env python3
"""Bake the story fixture's two music tracks, so no test needs the game's.

Why this exists
---------------

`tools/harness/story/audio/one.mp3` and `two.mp3` used to be symlinks into
the game's own `audio/` directory -- 6.3MB of somebody's music, needed by the
harness only as "two MP3s that decode, sound different from each other, and
do not run out".  That is a build-time dependency on a directory that is not
part of this repository and cannot be published with it.

The output is committed, rather than generated when the suite runs, so that a
fresh clone can run the tests without an encoder installed.  This script is
here for provenance and for when the requirements below change.

What the fixture actually needs
-------------------------------

Three things, and the third one is the one that is easy to miss:

  * **Decodable by minimp3**, which is what the engine uses.
  * **Distinguishable**, because `rbmusictest` switches between them and
    asserts which one is playing.  Different pitches do that.
  * **Long enough not to loop during a test.**  The fixture's script.rpy
    plays both with `loop`, and `chan_rewind` in audio.c resets
    `out_frames` to zero on every loop (audio.c:526) -- which is correct,
    since the position of a looping track is its position within the current
    pass.  But `rbmusictest` waits for the position to *reach* a target
    (`run_ms`, a `while` with a 40-iteration guard), and a track that loops
    underneath that wait sends the counter it is watching backwards.  The
    test would not fail cleanly; it would run out of guard and then assert
    on a position that means something else.

CBR and not VBR, 44100Hz stereo: that is what the game's tracks are, so it is
what the engine's MP3 path sees in production.  A VBR encode of a sine wave
would be a twentieth of the size, but it would also carry a Xing tag and send
`chan_open` down the `xing_tried` branch instead -- a different code path
from the one the fixture is standing in for.  The size is worth it.

Usage
-----

    tools/mkfixaudio.py [outdir]

Default outdir is tools/harness/story/audio.  Needs ffmpeg on PATH.
"""

import os
import subprocess
import sys

# 44100Hz stereo CBR, as the game's tracks are. 64k rather than their 128k:
# this is a sine wave, nobody is listening to it, and it halves the bytes
# that go into the repository.
RATE = 44100
BITRATE = "64k"

# Long enough that no test reaches the end. The longest single wait in the
# suite is rbmusictest's run_ms(6000), and a test may make several in a row
# on one playback; 90 seconds is an order of magnitude over that, and still
# a fifteenth of what the real tracks cost.
SECONDS = 90

# One tone each, a fifth apart so that a spectrum tells them apart at a
# glance, with a slow tremolo so neither is a pure steady tone -- a constant
# sine is the one signal a decoder bug can get right by accident.
TRACKS = [
    ("one.mp3", 440.0, 0.25),
    ("two.mp3", 660.0, 0.33),
]


def build(path, freq, tremolo):
    """Write one track, or raise CalledProcessError."""
    expr = ("sin(2*PI*%f*t) * (0.55 + 0.35*sin(2*PI*%f*t))" % (freq, tremolo))
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi",
        "-i", "aevalsrc=%s|%s:s=%d:d=%d" % (expr, expr, RATE, SECONDS),
        "-c:a", "libmp3lame",
        "-b:a", BITRATE,
        "-ar", str(RATE),
        "-ac", "2",
        # No Xing/LAME tag: see the docstring. `-write_xing 0` keeps this a
        # plain CBR stream, which is the path the real tracks take.
        "-write_xing", "0",
        "-id3v2_version", "0",
        path,
    ]
    subprocess.run(cmd, check=True)


MATRIX = os.path.join("tools", "harness", "fixaudio.lst")


def read_matrix(path):
    """The rows of fixaudio.lst, or raise ValueError saying which line."""
    rows = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 5:
                raise ValueError("%s:%d: want 5 fields, got %d"
                                 % (path, n, len(parts)))
            name, rate, chans, mode, ms = parts
            if mode not in ("cbr", "vbr"):
                raise ValueError("%s:%d: mode is cbr or vbr, not %r"
                                 % (path, n, mode))
            rows.append((name, int(rate), int(chans), mode, int(ms)))
    return rows


def build_matrix(path, rate, chans, mode, ms):
    """One matrix track: a tone of the requested length and format.

    A different pitch per sample rate, so a spectrum says which file is
    playing. The duration is given to ffmpeg in seconds with three
    decimals, which is milliseconds.
    """
    freq = 330.0 if rate == 44100 else 495.0
    expr = "sin(2*PI*%f*t) * 0.6" % freq
    src = expr if chans == 1 else "%s|%s" % (expr, expr)

    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi",
        "-i", "aevalsrc=%s:s=%d:d=%.3f" % (src, rate, ms / 1000.0),
        "-c:a", "libmp3lame",
        "-ar", str(rate),
        "-ac", str(chans),
    ]
    if mode == "vbr":
        # -q:a selects VBR, which is what makes lame write the Xing frame
        # the duration code has to read.
        cmd += ["-q:a", "4", "-write_xing", "1"]
    else:
        cmd += ["-b:a", "64k", "-write_xing", "0"]
    cmd += ["-id3v2_version", "0", path]

    subprocess.run(cmd, check=True)


def probe_ms(path):
    """ffprobe's duration for a file, in milliseconds.

    A second opinion, which is the whole point: durtest compares the
    engine's answer against this, and a figure chosen by this script would
    only be a restatement of what the encoder was told.
    """
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        check=True, capture_output=True, text=True)
    return int(round(float(out.stdout.strip()) * 1000.0))


def main(argv):
    outdir = argv[1] if len(argv) > 1 else os.path.join(
        "tools", "harness", "story", "audio")

    if not os.path.isdir(outdir):
        sys.stderr.write("%s: not a directory\n" % outdir)
        return 1

    for name, freq, tremolo in TRACKS:
        path = os.path.join(outdir, name)
        if os.path.islink(path):
            # The thing this script exists to replace. Removing the link
            # rather than writing through it, which would edit the game's
            # own file.
            os.unlink(path)
        build(path, freq, tremolo)
        print("%s: %.0fHz, %ds, %d bytes" %
              (path, freq, SECONDS, os.path.getsize(path)))

    # And the duration matrix, when a second directory is given for it.
    # Separate because it goes into the fixture *game* directory, which is
    # packed into an archive, while the two above sit in the loose story
    # fixture.
    if len(argv) > 2:
        mdir = argv[2]
        if not os.path.isdir(mdir):
            sys.stderr.write("%s: not a directory\n" % mdir)
            return 1
        rows = read_matrix(MATRIX)
        measured = []
        for name, rate, chans, mode, ms in rows:
            path = os.path.join(mdir, name)
            build_matrix(path, rate, chans, mode, ms)
            got = probe_ms(path)
            measured.append((name, got, rate, chans, mode))
            print("%s: %dHz %dch %s, asked %dms got %dms, %d bytes" %
                  (path, rate, chans, mode, ms, got, os.path.getsize(path)))

        # The matrix again, beside the tracks, so it goes into the archive
        # with them and durtest can read its expectations out of the pak it
        # is given rather than needing a path to this repository. The tracks
        # and the figures they are checked against then travel together: a
        # regenerated fixture cannot be measured against a stale table.
        spec = os.path.join(mdir, "durations.lst")
        with open(spec, "w") as f:
            f.write("# Generated by tools/mkfixaudio.py from "
                    "tools/harness/fixaudio.lst -- do not edit.\n"
                    "# The length is ffprobe's, measured from the encoded "
                    "file, not the\n"
                    "# figure the encoder was asked for: lame pads, so the "
                    "two differ by\n"
                    "# 30-45ms and durtest's tolerance is 50ms. More to the "
                    "point, the\n"
                    "# test wants a second opinion about the duration, which "
                    "a number\n"
                    "# this script chose would not be.\n"
                    "# name <tab> ms <tab> rate <tab> channels <tab> mode\n")
            for name, got, rate, chans, mode in measured:
                f.write("%s\t%d\t%d\t%d\t%s\n" % (name, got, rate, chans, mode))
        print("%s: %d tracks" % (spec, len(measured)))

        # A WAV as well as the MP3s. latencytest wants one -- the rhythm
        # game's hit sounds are WAVs, loaded whole rather than streamed, and
        # that is a different path through audio.c. A quarter-second
        # exponentially-decaying tone: percussive, so a latency measurement
        # has a sharp onset to find.
        hit = os.path.join(mdir, "fix-hit.wav")
        env = "sin(2*PI*880*t)*exp(-14*t)"
        subprocess.run([
            "ffmpeg", "-y", "-loglevel", "error",
            "-f", "lavfi",
            "-i", "aevalsrc=%s|%s:s=44100:d=0.25" % (env, env),
            "-c:a", "pcm_s16le", "-ar", "44100", "-ac", "2", hit,
        ], check=True)
        print("%s: 44100Hz stereo s16, 250ms, %d bytes"
              % (hit, os.path.getsize(hit)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

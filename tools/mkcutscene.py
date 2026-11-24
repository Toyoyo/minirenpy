#!/usr/bin/env python3
"""Derive the stand-in assets for every `renpy.movie_cutscene()` in a game.

A cutscene's stand-in is its first frame held on screen while its soundtrack
plays, for exactly as long as the video ran.

There *is* a video decoder on the target now -- two, in fact: MPEG-1 with MP2
in a program stream, and MPEG-4 Simple Profile with MP3 in an AVI, picked by
sniffing the file (src/viddec.h). This docstring used to say "there is no
video decoder on the target and there will not be one", which was true when
it was written and is not now. What remains true is the narrower claim: VP8
and H.264 at 1280x720 are out of reach of a Pentium III, so a `.webm` still
cannot be played and the MCI route w3vn used would still mean shipping codecs
Windows 3.1 does not have.

So the stand-in is not a historical fallback, it is the live answer in three
cases: a format neither decoder takes, `movie=0` in mrpy.ini for a machine
that cannot keep up with a particular file, and any cutscene a port has not
got around to converting yet.

**Converting a video is not this script's job and deliberately never will
be.** Its job is deriving the two stand-ins from whatever the script names.
Porting a VN whose cutscenes are in an unsupported format is a manual
exercise: convert the video by hand, then edit the `renpy.movie_cutscene()`
call to name the converted file. Both halves are manual on purpose -- the
second one especially, because the script is the only thing that decides
which file is played, and a build step that silently rewrote formats behind
the script's back would make "which video is this actually playing" a
question nobody could answer from the source.

That division costs nothing here, because this script is format-agnostic on
input: it splits the extension off whatever path the statement contains and
runs ffmpeg on it. So a call edited to `audio/ending.avi` yields
`audio/ending.mp3` and `images/ending.png` exactly as a `.webm` one does, and
the stand-in keeps working for a cutscene that is also playable.

Both stand-ins are *derived*, and both are named after the video, because that
is what lets the engine find them at runtime without being told anything:

    audio/intro.webm  ->  audio/intro.mp3     the soundtrack
                      ->  images/intro.png    the first frame

The engine re-derives those two names from the filename in the statement it is
executing (interp.c, `renpy.movie_cutscene`), so nothing has to be registered
anywhere and a cutscene works in any label, not just `splashscreen`. The frame
lands in images/ so that Ren'Py's own image auto-registration picks it up and
`scene intro` in the script resolves to it.

The mp3 is padded or trimmed to the *container's* duration, not the audio
stream's. That makes the mp3 the single source of truth for how long the
cutscene lasts, so the engine can simply ask how long the track is instead of
carrying a duration anywhere. A video with no audio at all gets a silent track
of the right length rather than a special case at runtime.

Games with no cutscene are the normal case: this exits quietly having done
nothing. A missing ffmpeg is a warning, not an error, so a tree that already
has the derived files still builds on a host without it.

Usage:
    python3 tools/mkcutscene.py <game-dir>
"""

import os
import re
import shutil
import subprocess
import sys

# Both quote styles, and an optional leading `$`/whitespace. Only the first
# argument is wanted; delay and loops do not affect what is extracted.
CALL_RE = re.compile(r"""renpy\.movie_cutscene\s*\(\s*(['"])(.+?)\1""")

AUDIO_RATE = "48000"
AUDIO_BITRATE = "192k"          # the game's own tracks are 48kHz stereo


def find_cutscenes(game_dir):
    """Every distinct path passed to renpy.movie_cutscene, in file order."""
    found = []
    seen = set()

    paths = []
    for root, _dirs, names in os.walk(game_dir):
        for name in names:
            if name.endswith(".rpy") or name.endswith(".rpym"):
                paths.append(os.path.join(root, name))

    for path in sorted(paths):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                # A commented-out call is not a call.
                if line.lstrip().startswith("#"):
                    continue
                for m in CALL_RE.finditer(line):
                    rel = m.group(2)
                    if rel not in seen:
                        seen.add(rel)
                        found.append(rel)
    return found


def probe_duration(path):
    """The container's duration in seconds, or None."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True)
    if out.returncode != 0:
        return None
    try:
        return float(out.stdout.strip())
    except ValueError:
        return None


def has_audio(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a",
         "-show_entries", "stream=index",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True)
    return out.returncode == 0 and out.stdout.strip() != ""


def newer(out_path, src_path):
    """True when out_path exists and is not older than src_path."""
    return (os.path.exists(out_path)
            and os.path.getmtime(out_path) >= os.path.getmtime(src_path))


def run(cmd):
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stderr[-2000:])
        raise SystemExit("ffmpeg failed: %s" % " ".join(cmd))


def extract_audio(src, dst, duration):
    """The soundtrack, padded or trimmed to the container's duration.

    `apad` then `-t` is what pins the length: apad appends silence
    indefinitely and -t cuts the result, so a stream shorter than the video
    comes out the video's length and a longer one is trimmed to it.
    """
    if has_audio(src):
        run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vn",
             "-af", "apad", "-t", "%.3f" % duration,
             "-ac", "2", "-ar", AUDIO_RATE, "-b:a", AUDIO_BITRATE, dst])
        return "soundtrack"

    # No audio stream: a silent track of the right length, so that the engine
    # still has one thing to time the cutscene by.
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi",
         "-i", "anullsrc=r=%s:cl=stereo" % AUDIO_RATE,
         "-t", "%.3f" % duration, "-b:a", "64k", dst])
    return "silence"


def extract_frame(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    run(["ffmpeg", "-v", "error", "-y", "-i", src, "-frames:v", "1", dst])


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    game_dir = sys.argv[1]
    cutscenes = find_cutscenes(game_dir)

    if not cutscenes:
        print("cutscenes: none in %s" % game_dir)
        return

    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        print("cutscenes: ffmpeg/ffprobe not found, leaving %d cutscene "
              "asset set(s) alone" % len(cutscenes))
        return

    for rel in cutscenes:
        src = os.path.join(game_dir, rel)
        if not os.path.exists(src):
            print("cutscenes: %s is referenced but missing, skipped" % rel)
            continue

        base = os.path.splitext(os.path.basename(rel))[0]
        mp3 = os.path.join(game_dir, os.path.dirname(rel), base + ".mp3")
        png = os.path.join(game_dir, "images", base + ".png")

        duration = probe_duration(src)
        if duration is None or duration <= 0:
            print("cutscenes: cannot read a duration from %s, skipped" % rel)
            continue

        if newer(mp3, src):
            print("cutscenes: %s up to date" % os.path.relpath(mp3, game_dir))
        else:
            kind = extract_audio(src, mp3, duration)
            print("cutscenes: %s <- %s (%s, %.3fs)"
                  % (os.path.relpath(mp3, game_dir), rel, kind, duration))

        if newer(png, src):
            print("cutscenes: %s up to date" % os.path.relpath(png, game_dir))
        else:
            extract_frame(src, png)
            print("cutscenes: %s <- %s (first frame)"
                  % (os.path.relpath(png, game_dir), rel))


if __name__ == "__main__":
    main()

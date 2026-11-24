#!/bin/bash
#
# Build the fixture pak: an MRPK archive with no game in it.
#
#   tools/harness/mkfixpak.sh [out.pak]
#
# Default output is fixture.pak at the top of the tree, which is where
# run.sh looks for it.
#
# Why a pak at all, when most harnesses take a loose fixture directory:
# some of them are *about* the archive. mr_vfs_enum only enumerates an
# archive on this host, so a catalogue in a loose tl/ directory is invisible
# to a gcc build -- which is why langtest mounts one. And durtest's whole
# subject is reading a duration out of a file, which it does through the VFS.
#
# The audio is generated rather than copied: tools/mkfixaudio.py writes the
# duration matrix from tools/harness/fixaudio.lst, and drops durations.lst
# beside the tracks so durtest reads its expectations out of the archive it
# is handed. See the comments in both of those.
#
# The fixture game directory symlinks fonts/, gui/ and the three template
# .rpy files out of tools/harness/common/, so this script dereferences when
# it packs -- mkpak.py walks the directory and an archive cannot hold a
# symlink.
#
# One asset is committed rather than generated: audio/fix-music.mp3, a
# 60-second excerpt of real music that audiostoptest needs because its fade
# assertion cannot be satisfied by a synthetic tone without being made
# vacuous. The comment above TRACK in that test has the reasoning and the
# ffmpeg command. mkfixaudio.py does not touch it.

set -e

cd "$(dirname "$0")/../.."

OUT=${1:-fixture.pak}
FIX=tools/harness/fixgame

if [ ! -d "$FIX" ]; then
    echo "no fixture game directory at $FIX" >&2
    exit 1
fi

# Regenerate the audio only when the matrix is newer than what is there, so
# a plain run of the suite does not need ffmpeg installed.
if [ ! -f "$FIX/audio/durations.lst" ] ||
   [ tools/harness/fixaudio.lst -nt "$FIX/audio/durations.lst" ]; then
    echo "regenerating fixture audio from tools/harness/fixaudio.lst"
    python3 tools/mkfixaudio.py tools/harness/story/audio "$FIX/audio"
fi

python3 tools/mkpak.py "$FIX" "$OUT"
echo "built $OUT"

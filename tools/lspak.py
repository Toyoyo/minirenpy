#!/usr/bin/env python3
"""List the contents of an MRPK archive.

The counterpart to mkpak.py: it reads the directory only, so it is fast even
on a 300MB archive and never touches the payload unless --extract is given.

The name field width is part of the format (the runtime reads the directory as
one fread into a struct array), so the version decides how to walk it: v1 is
64 bytes a name, v2 is 256. See the layout comment in tools/mkpak.py.

Usage:
    python3 tools/lspak.py <archive.pak> [glob] [-l] [--extract DIR]

    glob        shell pattern matched against the stored name, e.g.
                'images/bg_*' or '*.ogg'
    -l          long form: offset, size and padding per entry
    --extract   write the matching entries out under DIR, recreating the
                stored directory structure
"""

import fnmatch
import os
import struct
import sys

MAGIC = b"MRPK"
NAME_LEN = {1: 64, 2: 256}
ALIGN = 2048


def read_dir(path):
    """Return (version, [(name, offset, size)], archive_size)."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(16)
        if len(head) < 16 or head[:4] != MAGIC:
            raise SystemExit("%s: not an MRPK archive" % path)
        version, count, dir_off = struct.unpack("<III", head[4:])
        if version not in NAME_LEN:
            raise SystemExit("%s: unknown MRPK version %d (known: %s)"
                             % (path, version,
                                ", ".join(str(v) for v in sorted(NAME_LEN))))

        name_len = NAME_LEN[version]
        rec = name_len + 8
        f.seek(dir_off)
        blob = f.read(count * rec)
        if len(blob) < count * rec:
            raise SystemExit("%s: directory is truncated (%d of %d bytes)"
                             % (path, len(blob), count * rec))

    entries = []
    for i in range(count):
        base = i * rec
        raw = blob[base:base + name_len]
        name = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        off, sz = struct.unpack("<II", blob[base + name_len:base + rec])
        entries.append((name, off, sz))
    return version, entries, size


def main():
    args = sys.argv[1:]
    long_form = False
    extract = None
    rest = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-l":
            long_form = True
        elif a == "--extract":
            i += 1
            if i >= len(args):
                raise SystemExit("--extract needs a directory")
            extract = args[i]
        elif a in ("-h", "--help"):
            raise SystemExit(__doc__)
        else:
            rest.append(a)
        i += 1

    if not rest or len(rest) > 2:
        raise SystemExit(__doc__)
    path = rest[0]
    pattern = rest[1] if len(rest) > 1 else None

    version, entries, arc_size = read_dir(path)
    sel = [(pos, e) for pos, e in enumerate(entries)
           if pattern is None or fnmatch.fnmatch(e[0], pattern)]

    for pos, (name, off, sz) in sel:
        if long_form:
            # Padding is the gap to the next entry's offset, which is what the
            # archive actually spends; the last entry pads to the file end.
            nxt = entries[pos + 1][1] if pos + 1 < len(entries) else arc_size
            pad = max(nxt - (off + sz), 0)
            print("%10d %10d %6d  %s" % (off, sz, pad, name))
        else:
            print(name)

        if extract is not None:
            out = os.path.join(extract, *name.split("/"))
            d = os.path.dirname(out)
            if d:
                os.makedirs(d, exist_ok=True)
            with open(path, "rb") as f, open(out, "wb") as o:
                f.seek(off)
                left = sz
                while left > 0:
                    chunk = f.read(min(left, 1 << 16))
                    if not chunk:
                        raise SystemExit("%s: %s runs past the end of the "
                                         "archive" % (path, name))
                    o.write(chunk)
                    left -= len(chunk)

    # Sanity checks worth having in a listing tool: the runtime binary searches
    # the directory, so an unsorted or overrunning archive fails as a missing
    # file rather than as a bad pak.
    raw = sum(s for _n, _o, s in entries)
    print("-- %s: v%d, %d entries (%d shown), %.1f MB payload, %.1f MB archive"
          % (os.path.basename(path), version, len(entries), len(sel),
             raw / 1048576.0, arc_size / 1048576.0), file=sys.stderr)

    names = [n for n, _o, _s in entries]
    enc = [n.encode("utf-8") for n in names]
    if enc != sorted(enc):
        print("   warning: directory is not sorted by name bytes; the "
              "runtime's binary search will miss entries", file=sys.stderr)
    for n, o, s in entries:
        if o + s > arc_size:
            print("   warning: %s (offset %d, size %d) runs past the %d-byte "
                  "archive" % (n, o, s, arc_size), file=sys.stderr)
        if o % ALIGN:
            print("   warning: %s is not %d-aligned (offset %d)"
                  % (n, ALIGN, o), file=sys.stderr)


if __name__ == "__main__":
    main()

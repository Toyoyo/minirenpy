#!/usr/bin/env python3
"""Make a copy of DOS/32A that looks for a DPMI host before a VCPI server.

Why this exists
---------------

DOS/32A 9.1.2 detects VCPI first and uses it even when a DPMI host is
present.  Under FreeDOS with JEMMEX that is the wrong choice for us: VSBHDA
can only trap I/O ports for a protected-mode program through HDPMI32i, so a
VCPI client never sees the Sound Blaster it is told is at 220h.

Corroborated since, by two other extenders rather than by argument: DOS/4GW
and CauseWay (Makefile.cw, built 2026-10-28) both run the engine fine and
both get no sound under VSBHDA, exactly as stock DOS/32A does.  And the
CauseWay build *does* get sound under `jemmex novcpi`, which closes the
mechanism from the other end: removing VCPI from the host and making the
extender prefer DPMI are two independent routes to the same fix.  Three
stock extenders behave identically and both routes work, so this is a
mechanism rather than a hypothesis that fit one case.  See "A second
extender: CauseWay" in NOTES.

The comparison also sharpens what this tool is working around.  CauseWay
defaults to VCPI too -- `SetProtectedType` tries RAW, then VCPI, then DPMI
(cw32.asm:4610 in the source at /home/fosco/src/claude/cw) -- but it ships a
documented environment option that *works*, `SET CAUSEWAY=DPMI`, which sets
one flag and takes the DPMI branch.  DOS/32A has the same idea in
`/DPMITST:ON` and the option is dead code in 9.1.2, which is the whole
reason this file patches bytes instead of setting a variable.  Same default,
same fix needed, and the difference is only whether the vendor's switch
still does anything.

The
documented way out -- `SET DOS32A=/DPMITST:ON`, or the "VCPI/DPMI Detection
Mode" field in ss.exe -- does not work, because that option no longer exists
in 9.1.2:

  * `text/client/config.asm:345`, cfg_env_test, is `call get_env_swc` / `ret`
    with its whole body commented out.  (And the commented body is inverted
    against its own manual: get_env_swc returns ZF=0 for ON, which falls
    through to `or pm32_data[0],1`, and `kernel.asm:81` says bit 0 set means
    VCPI first.  The manual says ON means DPMI first.)
  * Nothing reads that bit.  Every `pm32_mode` test in the tree is bit 2, 3
    or 7.
  * `text/kernel/detect.asm:119` is hardcoded, and says so:

        @@1:  call  @@detect_VCPI    ; check for VCPI first
              call  @@detect_DPMI    ; check for DPMI second

The documentation that describes the option is v8.00 documentation; the
source is v9.12 unzipped over it (`readme.md:5` in the DOS/32A tree).  Both
halves of the feature are gone.

The alternative to this tool is `JEMMEX NOVCPI` before each run and `JEMMEX
VCPI` after, which toggles global state that a crash can leave switched off.

What it changes
---------------

The two calls above, swapped.  Nothing else.  They are safe to reorder:
both routines pop their own return address into BP on entry, write nothing
on the not-found path and return through `jmp bp`, and never return at all
on the found path (`jmp @@done`).  detect_VCPI's side effects -- the PIC
mappings, the XMS allocation, the page tables -- all happen after VCPI has
been confirmed present.

**On a machine with no DPMI host this is a no-op**: detect_DPMI bails at
`int 2Fh/1687h` having touched nothing, and detect_VCPI then runs exactly as
it does today.  That is the PIII with himem+emm386.

The one behaviour it does change, beyond the intended one: a *16-bit only*
DPMI host.  detect_DPMI tests `bl,1` and jumps to @@fail with error 3 rather
than returning, so such a host would now fail the extender where today VCPI
is found first and the question never arises.  HDPMI32i is 32-bit, and so is
the DPMI in Windows and in DOSBox-X, so this is narrow -- but it is real,
and it is a candidate for why the option was withdrawn in the first place.

How it finds them
-----------------

By the instructions that follow, which are distinctive enough to be unique
in the file:

    E8 ?? ??   call detect_VCPI
    E8 ?? ??   call detect_DPMI
    0F 01 E0   smsw ax
    24 01      and  al,1
    B8 02 00   mov  ax,2

Both call displacements have to be recomputed, because swapping two 3-byte
instructions moves each of them three bytes relative to its target.

Anything unexpected is an error rather than a guess: no match, more than one
match, a backwards call, two calls to the same place.  A wrong patch here is
a machine that does not boot the game, with no log and no message, so this
would rather produce nothing than produce something plausible.
"""

import re
import struct
import sys

# call rel16, call rel16, smsw ax, and al,1, mov ax,2
SIG = re.compile(rb"\xe8(..)\xe8(..)\x0f\x01\xe0\x24\x01\xb8\x02\x00", re.S)


def patch(data):
    """Return the patched image, or raise ValueError saying what was wrong."""
    hits = list(SIG.finditer(data))
    if len(hits) != 1:
        raise ValueError(
            "expected exactly one VCPI/DPMI detection site, found %d. This is "
            "not the DOS/32A this tool was written against (9.1.2, 27504 "
            "bytes); check the extender before trusting the patch." % len(hits))

    m = hits[0]
    at = m.start()
    first = struct.unpack("<h", m.group(1))[0]     # call detect_VCPI
    second = struct.unpack("<h", m.group(2))[0]    # call detect_DPMI

    vcpi = at + 3 + first
    dpmi = at + 6 + second

    # Both routines sit after the call site in detect.asm, and they are not
    # the same routine. If either of those stops being true the signature has
    # matched something else.
    if vcpi <= at or dpmi <= at:
        raise ValueError("a detection call points backwards; wrong match")
    if vcpi == dpmi:
        raise ValueError("both detection calls point at the same place; "
                         "wrong match")

    # Swapped: DPMI's target is now reached from three bytes earlier, VCPI's
    # from three bytes later.
    new = bytearray(data)
    new[at:at + 6] = (b"\xe8" + struct.pack("<h", dpmi - (at + 3)) +
                      b"\xe8" + struct.pack("<h", vcpi - (at + 6)))

    # Read it back the way it will be read in anger, rather than trusting the
    # arithmetic above.
    check = SIG.search(bytes(new), at)
    if not check or check.start() != at:
        raise ValueError("the patched bytes no longer match the signature")
    a = struct.unpack("<h", check.group(1))[0]
    b = struct.unpack("<h", check.group(2))[0]
    if at + 3 + a != dpmi or at + 6 + b != vcpi:
        raise ValueError("the patched calls do not reach the right routines")

    return bytes(new), at, dpmi, vcpi


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: mkdpmifirst.py <dos32a.exe> <output.exe>\n")
        return 2

    with open(argv[1], "rb") as f:
        data = f.read()

    if data[:2] not in (b"MZ", b"ZM"):
        sys.stderr.write("%s: not a DOS executable\n" % argv[1])
        return 1

    try:
        out, at, dpmi, vcpi = patch(data)
    except ValueError as e:
        sys.stderr.write("%s: %s\n" % (argv[1], e))
        return 1

    with open(argv[2], "wb") as f:
        f.write(out)

    print("%s: DPMI tested before VCPI (site 0x%04x, dpmi 0x%04x, "
          "vcpi 0x%04x) -> %s" % (argv[1], at, dpmi, vcpi, argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

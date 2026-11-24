# The DOS build, linked against CauseWay instead of DOS/32A.
#
#     wmake -f Makefile.cw              both of the below (the default)
#     wmake -f Makefile.cw game         mrpycw.exe
#     wmake -f Makefile.cw modes        modescw.exe
#     wmake -f Makefile.cw clean
#
# Everything comes from Makefile.dos: the flags, the object lists, the header
# stamp, the libraries, every target. Five values differ and they are all
# below. Not a copy of that file -- the three per-game Win32 makefiles were
# copies, and what they did with their freedom was drift until they named a
# linker file that no longer existed.
#
# This is the override that Makefile.dos's own GAME_MAP comment describes as a
# one-liner, written down and kept, because an extender swap is worth being
# able to repeat exactly.
#
# --- what is shared, and what that means -----------------------------------
#
# The *objects* are shared with the DOS/32A build, deliberately. The compile
# is identical -- same -bt=dos, same GCFLAGS, same MR_TARGET_DOS -- so there
# is nothing to separate, and only the link differs. Two consequences:
#
#   - `wmake -f Makefile.cw game` after a DOS/32A build relinks rather than
#     recompiling, and vice versa. That is the point: it makes comparing the
#     two extenders cheap, and guarantees they are the same code.
#   - `wmake -f Makefile.cw clean` therefore deletes the DOS/32A build's
#     objects too, and dos32adp.exe with them. Both come back on the next
#     build (the stub from tools/mkdpmifirst.py), so it costs time, not work.
#
# This is the opposite of the .o/.do split against the Win32 Makefile. There
# the compile genuinely differs and sharing objects would produce a wrong
# binary that links without complaint; here it would produce the same binary.
#
# --- why a second extender, and what came of it ----------------------------
#
# **It does not fix the sound on its own, which was the main reason for
# trying it.** Built and run: the engine works under DOSBox-X and under
# QEMU, and gets no sound with VSBHDA -- the same as DOS/4GW, and the same as
# DOS/32A before the patched stub.
#
# This file originally guessed the other way, from CauseWay's reputation for
# preferring DPMI. It takes VCPI when VCPI is on offer, like the other two,
# and a VCPI client is invisible to VSBHDA's port trapping: "no Sound Blaster
# at 220" from a machine displaying "SB emulation: Addr=220". See
# tools/mkdpmifirst.py.
#
# **But `jemmex novcpi` under FreeDOS gives this build sound.** Which is the
# interesting half: take VCPI away from the host and CauseWay falls back to
# DPMI and works, exactly as patching DOS/32A to prefer DPMI makes *it* work.
# The same outcome from two independent directions, so CauseWay's silence is
# the same bug rather than a second one wearing its clothes.
#
# What is missing is only a per-program way to do it. `jemmex novcpi` is
# global state -- every program on the machine loses VCPI, and the `jemmex
# vcpi` that is supposed to follow is the thing a crash leaves switched off.
# That is why dos32adp.exe exists instead of two batch lines, and the
# reasoning carries over here unchanged.
#
# cw32.exe carries both "$DPMI failed to switch to protected mode." and
# "$VCPI failed to switch into protected mode.", so it can do either; what is
# not readable out of the shipped files is the order it tries them in, and
# cwstub.exe is compressed so its option table cannot be read either.
#
# So for now **Makefile.dos stays the one that ships**, and this file is for
# comparing extenders. That could change -- see the first item below.
#
# --- the switch exists, and it is spelled DPMI -----------------------------
#
#     SET CAUSEWAY=DPMI
#
# `CAUSEWAY=NOVCPI` was tried first and did nothing, which was a null test
# rather than an answer: there is no NOVCPI option, and the parser walks past
# a token it does not know (cw32.asm:4001, `inc si` / `jmp @@3`), so an
# invented name is silently ignored. The real one is in CauseWay's own manual,
# doc/page_10.htm:
#
#     DPMI  Force DPMI rather than default VCPI usage whenever possible
#           (recommended for 386^Max and BlueMax users). The memory manager
#           must support DPMI or else this setting is ignored.
#
# and the source confirms both halves of that, which is worth more than the
# manual on its own (source at /home/fosco/src/claude/cw):
#
#   src/cw32.asm:3981   the option table matches "IMPD" -- the four bytes of
#                       DPMI, compared as a dword, so it must be uppercase
#   src/cw32.asm:4194   @@dpmi sets ProtectedForce = 1, and nothing else
#   src/cw32.asm:4610   SetProtectedType. With ProtectedForce clear the order
#                       is RAW, then VCPI, then DPMI -- so with any memory
#                       manager loaded, VCPI wins and DPMI is never reached.
#                       With it set and a DPMI server present, it goes
#                       straight to ProtectedType = 2, DPMI.
#
# So "CauseWay defaults to VCPI" is not an inference from the silence any
# more; it is the documented default and the three branches that implement it.
# That is also why `jemmex novcpi` fixed the sound: it clears ProtectedFlags
# bit 1 and the same code falls through to DPMI.
#
# **Confirmed on FreeDOS: with CAUSEWAY=DPMI set, this build has sound**,
# VCPI still enabled, nothing patched. Three ways to reach DPMI have now been
# tried and all three play; every VCPI configuration is silent. The question
# is closed.
#
# It still needs a DPMI server present or the setting is ignored by design --
# HDPMI32i, which VSBHDA wants anyway.
#
# --- so why does Makefile.dos still ship ------------------------------------
#
# Not correctness: both builds play sound. The difference is what the
# enduser's machine must have.
#
#   dos32adp.exe needs *nothing* at run time. One file, no environment, no
#   launcher, and it behaves the same on a machine with a real Sound Blaster
#   (where VCPI was never a problem) as on a VSBHDA one. Its cost is in the
#   build: a vendor binary patched by six bytes, fragile across Open Watcom
#   updates, which is why mkdpmifirst.py fails loudly instead of quietly.
#
#   CAUSEWAY=DPMI patches nothing and is the vendor's own documented option,
#   which is the more maintainable half -- but it has to be set on every
#   machine, so "one file that needs nothing on the path" is gone, and that
#   was the whole reason for binding the extender.
#
# WARPWRAP would get both -- `WW !CAUSEWAY=DPMI mrpycw.exe` burns the variable
# into the exe's environment block (doc/page_90.htm on) -- but ww.exe is a DOS
# program and this build runs on Linux, and while its source is here
# (src/wl/ww.c) it reads *its own image* for the code it injects
# (strcpy(DataFileName, argv[0]), line 204), so a host port would need
# bin/ww.exe as a data file too: a third-party blob plus a port of somebody
# else's tool, for nothing the first option does not already give.
#
# Building CauseWay with the flag compiled in is a dead end on this host:
# variants are `ml /DPERMNOVM=1` (src/novmmak) -- MASM, linkold and cwc.exe,
# all DOS tools -- and there is no PERM-style define for forcing DPMI anyway.
#
# **CauseWay is not a shipping candidate -- decided, not pending.** This file
# is a compatibility check, which is what it was built as. Keep it: it is the
# only second opinion the DOS port has, and it has paid for itself twice.
#
#   It settled the VCPI question. Three extenders agreeing, plus CAUSEWAY=DPMI
#   working, is what turned mkdpmifirst.py's six bytes from a hypothesis that
#   fit one case into a documented mechanism.
#
#   It narrowed the Jemm realloc fault. memwalkcw.exe keeps every byte under
#   JEMMEX where the DOS/32A build loses the whole block, which moved that
#   diagnosis from "this host's resize" to "this extender and this host
#   together". See "It is the extender's resize, not Jemm's alone" in NOTES.
#   That is also why the memwalk target below is worth its twenty lines.
#
# And it is the fallback if a future Open Watcom breaks the patch -- already
# tested rather than hoped for.
#
# Still open, and not answered by the run above: **realloc across one
# megabyte.** On FreeDOS with JEMMEX or HIMEMX+JEMM386 the DOS/32A build
# page faulted before the first frame -- the Watcom runtime's resize of a
# block from 557,056 to 1,114,112 bytes came back with new pages and none of
# the old contents, which is the statement array doubling 2048 -> 4096. See
# "realloc does not keep what it was given" in NOTES. That is the host's DPMI
# resize reached through the extender, and CauseWay has its own memory
# manager. "Works under QEMU" does not settle it, because the engine already
# steps around the bug; what would is probe/memwalk.c linked against this
# file and run under Jemm.
#
# --- the stub --------------------------------------------------------------
#
# `system causeway` is in $WATCOM/binl/wlink.lnk (460-466) and ends with
# `op stub=cwstub.exe`, carrying no path -- exactly the trap Makefile.dos
# documents for dos32a.exe. On a DOS or Windows host the stub sits beside
# wlink and the bare name resolves; cross-building from Linux, wlink is in
# binl and cwstub.exe is in binw, so it does not:
#
#     Warning! W1093: cannot open cwstub.exe
#
# and **wlink does not fail on a missing stub** -- it links the default
# DOS/4GW stub instead and exits 0, so a bad path yields a binary that wants
# DOS4GW.EXE at run time from a build that looked fine. Makefile.dos tests for
# the file before linking for that reason, and the test covers this file too
# because STUB is what it checks.
#
# Worth knowing while reading a sound bug here: that silent fallback produces
# a DOS/4GW binary, and DOS/4GW has no VSBHDA sound either. So "no sound" on
# its own does not distinguish a CauseWay build from a build that quietly
# is not one. The test above is what rules it out; `wmake` printing
# "Built mrpycw.exe for 'causeway' with .../cwstub.exe bound in" is the
# other half.
#
# cwstub.exe is 47KB against dos32a.exe's 27KB, so the exe grows by about
# 20KB. The 88-byte cwdstub.exe beside it is the other variant -- it loads
# CW32.EXE from disk rather than carrying the extender -- and is not wanted
# here for the same reason stub32a.exe is not: what ships should be one file.
# (That reading of the two is from their sizes and names. The check that
# matters is whether mrpycw.exe runs on a machine with no CW32.EXE anywhere.)

STUB        = $(%WATCOM)/binw/cwstub.exe
LSYSTEM     = causeway
GAME_TARGET = mrpycw.exe
GAME_MAP    = mrpycw.map
MODES       = modescw.exe
MEMWALK     = memwalkcw.exe
MMXCHECK    = mmxchkcw.exe
BLENDCHECK  = blndchkcw.exe
CPUCHECK    = cpuchkcw.exe

!include Makefile.dos

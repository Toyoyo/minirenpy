#!/bin/bash
#
# Build and run every harness.
#
# This exists because the argument conventions are not uniform and getting one
# wrong looks exactly like a broken engine rather than a broken command line --
# a suite run once came back with four "failures" that were only that. The
# conventions are documented in README; this is them in executable form, so
# they stop being something to re-derive by hand.
#
# Binaries and output go to /tmp; the harnesses write save files and persistent
# data into the working directory, so each runs with /tmp as its cwd.
#
#   tools/harness/run.sh                 # everything
#   tools/harness/run.sh migratetest rbtest   # just these
#   MR_PAK=$PWD/azami.pak tools/harness/run.sh     # against another game
#
# **A pak is a game.** mr_vfs_init mounts the archive and returns (vfs.c:186)
# -- the loose directory is a fallback for when there is no archive, not a
# second place to look -- so with a pak built, the directory a harness is
# handed decides nothing. There used to be an MR_GAME pointing at somebody's
# game folder beside this one, and all it did was make it possible to run one
# game's script against another game's archive: playtest spent months
# reporting a pass that way.
#
# So: MR_PAK picks the game, and nothing else does. Harnesses that take a
# directory and mount no archive get a *fixture* directory, always.
#
# Needs a pak built (`wmake data`) for the five that read a real game's.

cd "$(dirname "$0")/../.." || exit 1

PAK=${MR_PAK:-"$PWD/mrpy.pak"}
H=tools/harness

# The fixture game and its archive: no game's content, built by
# tools/harness/mkfixpak.sh. Used by the harnesses that need a *pak* but not
# a story -- see args_for below.
FIXGAME="$PWD/$H/fixgame"
FIXPAK="$PWD/fixture.pak"

# Built here rather than left to the reader, so a fresh clone runs the suite
# with one command. It is small (under half a megabyte) and mkfixpak.sh
# regenerates the audio only when tools/harness/fixaudio.lst is newer than
# what is already there, so ffmpeg is needed to *change* the fixture, not to
# run the tests.
#
# Worth building even though mr_vfs_init falls back to the loose directory
# when the archive is missing: that fallback means a forgotten pak does not
# fail, it quietly tests the loose path instead, and the archive is the path
# the target actually uses.
# Anything newer than the archive, anywhere in the fixture or in the shared
# files it links to, rebuilds it. Named files were listed here before and
# the list went stale the first time the fixture grew one: script_game.rpy
# and its catalogue were added, the pak was not rebuilt, and four harnesses
# failed on content that was sitting right there in the tree.
if [ ! -f "$FIXPAK" ] ||
   [ -n "$(find -L "$H/fixgame" "$H/common" "$H/fixaudio.lst" \
                -newer "$FIXPAK" -print -quit 2>/dev/null)" ]; then
    echo "building $FIXPAK"
    "$H/mkfixpak.sh" "$FIXPAK" || exit 1
fi


# The engine's portable modules, from tools/enginesrc.lst -- which probe/
# Makefile reads too, so the two cannot drift. What is in it and what is
# deliberately left out (the platform seam) is documented there. Keep it in
# step with the Makefile's OBJS; a stale list fails to link rather than
# misbehaving.
FULL=$(for f in $(sed 's/#.*//' tools/enginesrc.lst); do
    echo -n "src/$f.c "
done)
if [ -z "$FULL" ]; then
    echo "tools/enginesrc.lst is empty or missing" >&2
    exit 1
fi

# The six that stub out parts of the engine themselves, or need far less of it:
# linking the full list is a duplicate-symbol error.
srcs_for() {
    case $1 in
    audiostoptest|wavtest|durtest|latencytest) echo "src/audio.c src/vfs.c src/mp3impl.c";;
    enumtest)              echo "src/vfs.c";;
    ansitest)              echo "src/ansi.c";;
    scaletest)             echo "src/scale.c src/gfx.c";;
    # gfx.c on its own, which is the point: it accounts the compositor's work
    # without naming mr_plat_ticks, so a harness with no platform layer can
    # link it and pass a clock of its own.
    gfxproftest)           echo "src/gfx.c";;
    # gfx.c for the real mr_blit; the macros come from mr.h and need nothing.
    blendtest)             echo "src/gfx.c";;
    # config.c on its own: it reads a file and hands back numbers and
    # strings, and depends on nothing else in the engine.
    initest)               echo "src/config.c";;
    # cpu.c on its own too, and it needs nothing: CPUID in, a flag word out.
    # Note it does not pull in gfx.c or yuv.c, which are the engine's two
    # callers -- their have_simd() is compiled with the MMX macros undefined
    # here, so it returns 0 without asking, and linking them would prove
    # nothing about the gate.
    cputest)               echo "src/cpu.c";;
    # pack.c is pure the same way scale.c is: a surface in, bytes out. It
    # came out of present.c for the DOS presenter's sake and this is the
    # only thing that links it on the host.
    packtest)              echo "src/pack.c src/gfx.c";;
    # yuv.c is pure in the same way, one layer earlier: three byte planes in,
    # a surface out. plmimpl.c comes along as the *oracle* and not as a
    # dependency -- yuv.c does not include pl_mpeg and must not -- so that
    # the conversion can be compared against the vendor function it borrowed
    # its coefficients from. See the header of yuvtest.c.
    # pack.c is here because yuvtest checks mr_yuv_rows_fused, which produces
    # the display's bytes directly and so has to be compared against the
    # convert-then-pack route it replaces.
    yuvtest)               echo "src/yuv.c src/gfx.c src/plmimpl.c src/pack.c";;
    # The AVI/xvid pair, one layer below movie.c: a container and a decoder,
    # a byte range in and three planes out. No mixer and no presenter -- the
    # clock and the handover are movie.c's and are movietest's subject, and
    # neither is testable here anyway (the host shim fakes the audio clock).
    # mp3impl.c comes along because xviddec.c decodes the soundtrack with
    # minimp3, the same instantiation audio.c uses.
    avitest)               echo "src/avi.c src/xviddec.c src/vfs.c src/mp3impl.c";;
    # Same shape again for the audio side: mixer frames in, a card's bytes
    # out, with the Sound Blaster left in wavedos.c.
    sbconvtest)            echo "src/sbconv.c";;
    # The VBE mode picker is pure: bytes in, an index out. vbedos.c, the
    # int 10h half, cannot be *driven* here -- there is no VESA BIOS on a
    # host, which is the reason the two are separate files -- but it is
    # linked anyway. It compiles to three do-nothing functions off DOS, and
    # linking it is what catches a prototype drifting out of step with
    # vbe.h; no makefile on this machine builds the DOS half otherwise.
    vbetest)               echo "src/vbe.c src/vbedos.c src/dosmem.c";;
    imgcaptest)            echo "src/image.c src/png.c src/vfs.c src/gfx.c src/scale.c";;
    # Also drives mr_scene_show, which is where the resolution has to be
    # asked for -- hence scene.c and what it drags behind it.
    attrtest)  for f in image png vfs gfx scale scene expr lex parse style \
                        slang slparse mp3impl tl md5 persist pyfn serial \
                        list; do echo -n "src/$f.c "; done;;
    texttest)              echo "src/font.c src/vfs.c src/gfx.c";;
    # tl.c is here for expr.c's sake, not flagtest's: the `!t` conversion in
    # mr_expr_substitute translates through the string table.
    flagtest)  for f in expr list persist vfs pyfn md5 lex serial tl; do
                   echo -n "src/$f.c "; done;;
    # The substitution conversion flags: expr.c, and tl.c for `!t`. It writes
    # and mounts its own one-entry archive, so it needs no game.
    convtest)  for f in expr list persist vfs pyfn md5 lex serial tl; do
                   echo -n "src/$f.c "; done;;
    datetest)  for f in datefmt tl vfs lex md5 expr list pyfn persist serial; do
                   echo -n "src/$f.c "; done;;
    tltest)    for f in parse lex expr vfs style slang slparse tl md5 pyfn \
                        list; do echo -n "src/$f.c "; done;;
    # blurtest links Ren'Py's own blur alongside the engine's, as its oracle.
    blurtest)  echo "$FULL tools/harness/blurref.c";;
    # Parser-side, but mr_place_resolve lives in scene.c, which drags the
    # image cache and the blitter behind it.
    transformtest|placetest) for f in expr scene vfs lex parse style slang slparse image \
                        png gfx mp3impl tl md5 persist pyfn serial list; do
                   echo -n "src/$f.c "; done;;
    *)                     echo "$FULL";;
    esac
}

# Most take the game directory. The exceptions are why this script exists.
args_for() {
    case $1 in
    # On a real game, and each for its own reason. cuttest needs a cutscene
    # the fixture has not got; stacktest, seamtest and memreport are
    # measurements against a real game's real size, which is the point of
    # them; playtest and slotfittest want a real script's volume of
    # content. All of these work on *any* game's pak rather than on
    # Miwa's -- verified against Hinata.
    #
    # The four that used to be here and are not any more -- juketest,
    # tltest, unlocktest, oldsavetest -- were the last harnesses that
    # needed somebody's game, and three of them needed *Miwa's*. What they
    # reached for moved into the fixture instead: a jukebox and a high
    # score table over the fixture's own generated tones
    # (common/screens.rpy), two persistent unlock flags, and twenty say
    # statements copied verbatim from Miwa with the translation blocks
    # Ren'Py generated for them (fixgame/script_game.rpy and
    # tl/english/script_game.rpy). The verbatim lines are there because
    # both an old save's statement id and a tlid are hashes of the line, so
    # they cannot be invented without inventing the hash too -- which would
    # make the test agree with whatever the engine did.
    #
    # cuttest stays on a real game: the cutscene assets it needs are named
    # by our own build convention (audio/intro.mp3, images/intro.png from
    # `wmake data`) and it wants a `splashscreen` label, so it works on any
    # game but not on a fixture that has no cutscene.
    # langtest stays on a real game, and this one is a boundary rather than
    # an oversight: its subject is *dialogue* translation -- the line on
    # screen changing when the language does -- which needs a translated
    # script. Those are `translate <lang> <identifier>:` blocks keyed on a
    # tlid computed from the label, the speaker and the text
    # (mr_tl_identifier), so writing them for a fixture by hand means
    # hand-computing hashes. The fixture now has twenty lines that *came*
    # with their hashes, borrowed for oldsavetest and tltest, so this one
    # could follow them -- it needs the story to walk through those lines,
    # which nothing currently does. Not done. langtest works on any game --
    # verified against Miwa and Hinata, whose base languages are opposite.
    cuttest|langtest)
        echo "$PAK|$FIXGAME";;
    # The fixture carries a catalogue now -- tl/None in its own language and
    # tl/english to switch into -- so these two no longer need a game.
    datetest|confirmtest)
        echo "$FIXPAK|$FIXGAME";;
    # standintest wants the *fixture* pak and not a game's. Its subject is
    # where the cutscene stand-in lands on the stage, and the only shape that
    # can show that is a poster which is not stage-sized -- which is a
    # property of the fixture's 640x320 images/fixstandin.png and of no
    # shipped game's cutscene, Miwa's being 1280x720. The pak rather than the
    # loose directory because the fixture declares no `image` statements and
    # mr_vfs_enum only enumerates an archive here.
    standintest)
        echo "$FIXPAK|$FIXGAME";;
    # cutskiptest, the same pair and for the same two reasons: its label is
    # the fixture's (fixture_splash, a cutscene followed by a top-level
    # `return`, which no shipped game's script is obliged to contain) and it
    # reaches the stand-in, so it needs the poster the archive enumerates.
    cutskiptest)
        echo "$FIXPAK|$FIXGAME";;
    # movietest needs a *pak* -- its subject is a movie read through the VFS,
    # and the seek/tell callbacks that let the demuxer find the stream's
    # duration are exercised against the archive, not a loose file. The
    # fixture carries its own two-second 64x32 .mpg, so no game is involved;
    # see the header of movietest.c for how to regenerate it.
    movietest)
        echo "$FIXPAK|$FIXGAME";;
    # avitest, the same: its fixture .avi is in the fixture pak, and the
    # seek callback does more work for the AVI header walk than for the
    # MPEG one -- avi.c skips chunk payloads rather than reading them.
    avitest)
        echo "$FIXPAK|$FIXGAME";;
    # rgtest's assets are all in the fixture now: a real beatmap, a
    # generated background, the music excerpt, and a `fixture_rhythm` label
    # shaped after the game's own for the one check that goes through the
    # interpreter.
    #
    # juketest and unlocktest read the jukebox and the high score table in
    # common/screens.rpy, which are built over the fixture's own tones and
    # gated on two fixture persistent flags; tltest reads the borrowed
    # lines and the catalogue beside them.
    #
    # playtest's subject is the channel mapping -- Play("sound", ...) must
    # reach the sound channel and not loop, Play("music", ...) must reach
    # the music channel and loop. Neither half was portable: it played a
    # file named in the harness, which was one game's effect, and the half
    # that did read config.sample_sound depends on the game's *format* --
    # Hinata's is an .opus, which this engine cannot decode, so the Test
    # button correctly plays nothing and four assertions correctly fail,
    # about a game's audio format rather than about channels.
    #
    # It reported `pass` throughout, on every game, because its check()
    # only printed and main ended in `return 0`. run.sh judges by exit
    # code. See the comment on check() in playtest.c.
    rgtest|juketest|unlocktest|tltest|playtest)
        echo "$FIXPAK|$FIXGAME";;
    slotfittest|stacktest|seamtest|memreport)
        echo "$PAK|$FIXGAME";;
    # These read assets and no script, so they take the fixture pak rather
    # than a game: durtest's duration matrix is generated with measured
    # lengths (tools/harness/fixaudio.lst), latencytest wants one short WAV,
    # and audiostoptest wants real music -- see the comment above TRACK in
    # it. Built automatically above; by hand it is mkfixpak.sh.
    durtest|latencytest|audiostoptest)
        echo "$FIXPAK|$FIXGAME";;
    restarttest|bandtest|filltest|windowtest|rbmusictest|titlesavetest|stalltest|bartest|skiptest|branchtest|afmtest|substtest|prefixtest|condtest|movetest|blurtest|animtest|rbatltest|atlreshowtest|savatltest|savmenutest|pausebacktest|tfatltest|alphatest|zoomtest|fscaletest|inputtest|wintest)
        echo "$PWD/$H/story";;
    choicetest)  echo "$PWD/$H/choice";;
    creditstest) echo "$PWD/$H/credits";;
    # Its own fixture, because it needs a gui.rpy that names a second font
    # face and its own baked atlases to go with it. Rebake with
    #   python3 tools/mkfont.py tools/harness/faces tools/harness/faces/fonts
    facetest)    echo "$PWD/$H/faces";;
    # The save fixtures are real Miwa saves and have to be; the statements
    # they name are in the fixture's script_game.rpy, so the pak is ours.
    oldsavetest) echo "$FIXPAK|$FIXGAME|$PWD/$H/fixtures/v5-slot.sav|$PWD/$H/fixtures/v6-slot.sav|$PWD/$H/fixtures/v7-slot.sav";;
    # Its own fixture: a 206x576 sprite, the size from the kalea report, so
    # the render half can measure where a bare `show` actually lands.
    placetest)   echo "$PWD/$H/place";;
    # Its own fixture: one small image named as a gui/ path, so the name the
    # registry runs out of room for is the same shape as the real one.
    imgcaptest)  echo "$PWD/$H/imgcap";;
    # These take a loose game directory and mount no archive -- they assert
    # on screens, their buttons and their labels, which come from
    # screens.rpy rather than from a catalogue. The fixture carries that
    # file (tools/harness/common/screens.rpy) and the baked fonts with it,
    # so they get the fixture instead of whatever game is installed. Before
    # this they read Miwa's screens and failed against any other game on
    # French button labels -- "Retour", "Fenetre" -- that are not the
    # engine's business.
    dismisstest|preftest|quicktest|texttest) echo "$FIXGAME";;
    flagtest|enumtest|scaletest|ansitest|transformtest|attrtest|convtest|vbetest|packtest|sbconvtest|initest|gfxproftest|blendtest|cputest) echo "";;
    wavtest|migratetest) echo "/tmp";;
    # The default is the fixture directory, and it is a *directory* run:
    # these harnesses mount no archive, so what they see is the fixture's
    # loose files. None of them is about a particular game -- they drive
    # the save screen, the game menu, rollback, the history screen -- and
    # the ones that used to name a game's label now ask anygame.h.
    *)           echo "$FIXGAME";;
    esac
}

if [ $# -gt 0 ]; then
    tests="$*"
else
    tests=$(for f in $H/*test.c; do basename "$f" .c; done)
fi

pass=0
fail=0
skip=0
failed=""
skipped=""

# The harnesses that need a real game's archive, and the only ones left
# that need a real game at all. Each is here on purpose: cuttest wants a
# cutscene and a `splashscreen` (and skips with 77 when the game has
# none); langtest wants translated *dialogue*; slotfittest, stacktest and
# seamtest are measurements against a real script's real size, which is
# the point of them.
#
# "Any game's pak will do" is a claim worth being careful with: all five
# were run against three paks -- Miwa's, Hinata's and Azami's. Three games
# is better evidence than two -- the first two share a template and an
# author, Azami does not -- but it is still not proof, so what each of
# them actually rests on is written down above it.
#
# They are skipped rather than failed when there is no pak. mr_vfs_enum only
# enumerates an archive on this host, so a translation catalogue in a loose
# tl/ directory is invisible to a gcc build; with no pak, mr_vfs_init
# silently falls back to the loose directory and the failures read like
# engine bugs -- "no english catalogue loaded", "switching language changes
# the dialog FAIL". That happened on a fresh clone and cost a round of
# investigation, which is the failure mode this whole script exists to
# prevent.
NEEDS_ARCHIVE="cuttest langtest slotfittest stacktest seamtest"

if [ ! -f "$PAK" ]; then
    echo "note: no archive at $PAK"
    echo "      $NEEDS_ARCHIVE need one -- build it with \`wmake data\`,"
    echo "      or point MR_PAK at one. Skipping those; everything else"
    echo "      runs against the fixture or a loose directory as usual."
    echo
fi

for t in $tests; do
    if [ ! -f "$H/$t.c" ]; then
        echo "  no such harness: $t"
        fail=$((fail + 1)); failed="$failed $t(missing)"
        continue
    fi

    if [ ! -f "$PAK" ]; then
        case " $NEEDS_ARCHIVE " in
        *" $t "*)
            echo "  SKIP  $t    (needs the pak; see the note above)"
            skip=$((skip + 1)); skipped="$skipped $t"
            continue;;
        esac
    fi

    # latencytest needs the shim's device to hold its buffers instead of
    # consuming them instantly, which is what makes a queue depth observable.
    # Every other harness depends on the instant device and must not get this.
    extra=""
    extralib=""
    [ "$t" = latencytest ] && extra="-DHOSTSHIM_HOLD"
    # memreport intercepts every allocation to report where the heap goes.
    [ "$t" = memreport ] && extra="-rdynamic -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"

    # wavehost.c goes with audio.c and only with it: it is wave.h's host
    # implementation -- the sound card a harness can drive -- and the one
    # file that used to be tools/hostshim/mmsystem.h, included rather than
    # linked. Harnesses that do not link the mixer have no sink to supply.
    srcs=$(srcs_for "$t")
    case " $srcs " in
    *" src/audio.c "*) srcs="$srcs tools/hostshim/wavehost.c";;
    esac

    # Same seam, same rule, for video: movie.c calls mr_present_video, whose
    # only implementations are the two presenters this machine cannot build.
    # presenthost.c is the sink, and goes with movie.c and only with it.
    case " $srcs " in
    *" src/movie.c "*) srcs="$srcs tools/hostshim/presenthost.c";;
    esac

    # avitest's subject is xvidcore's decoder, so it needs xvidcore. Built
    # once as a cached static library at -O2 rather than named in srcs_for and
    # recompiled at -O0 on every run -- mkxvidhost.sh explains why, and says
    # plainly that what it builds is the pure C half and not the MMX one the
    # target gets.
    case " $srcs " in
    *" src/xviddec.c "*)
        if ! "$H/mkxvidhost.sh" >"/tmp/xvidlib_$t" 2>&1; then
            echo "  BUILD $t    (xvidcore host library; see /tmp/xvidlib_$t)"
            head -3 "/tmp/xvidlib_$t"
            fail=$((fail + 1)); failed="$failed $t(xvidlib)"
            continue
        fi
        extra="$extra -I libs/xvidcore-1.3.7/src"
        # After the objects, not among them: a static library only resolves
        # what is already undefined when the linker reaches it, so an archive
        # placed ahead of the sources that need it contributes nothing and the
        # link fails on every xvid symbol.
        extralib="libs/xvidcore-1.3.7/build/host/libxvidhost.a";;
    esac

    # plathost.c is linked into everything: plat.h is the seam between the
    # engine and the machine, and the harnesses run on a machine with no
    # window and no clock worth the name. image.c needs mr_plat_ticks for its
    # MR_PROF decode timer, which is enough to make it universal. A harness
    # that wants to assert on a platform call replaces this file; it must not
    # define one of these itself, or the link fails on the duplicate.
    if ! gcc -O0 -g -I src -I tools/hostshim -I libs $extra -o "/tmp/h_$t" \
            "$H/$t.c" tools/hostshim/plathost.c $srcs \
            $extralib -lpng -lm 2>"/tmp/build_$t"; then
        echo "  BUILD $t    (see /tmp/build_$t)"
        head -3 "/tmp/build_$t"
        fail=$((fail + 1)); failed="$failed $t(build)"
        continue
    fi

    # vbedos.c's DOS branch is behind `#if defined(__DOS__)`, so the build
    # above compiles three do-nothing stubs and the real int 10h code is
    # seen by no compiler on this machine -- a typo in it survives until
    # someone runs wmake on a DOS box, which is a slow way to find one. A
    # syntax-only pass against tools/hostshim/dos closes that.
    #
    # It checks syntax and nothing else. The shim's declarations come from
    # the same reading of Watcom's API as the code does, so it agrees with
    # any misunderstanding rather than catching it: it would not have found
    # the selector-versus-segment fault that made it necessary. See the
    # comment at the top of tools/hostshim/dos/i86.h.
    #
    # Every DOS-only file rides along with it: presdos.c, platdos.c,
    # wavedos.c and dosmem.c. Some of those are mostly host-compilable --
    # presdos.c calls pure code and vbedos.c's three entry points, which
    # have host stubs -- but the parts that matter are all behind the same
    # DOS guard, and this is the only thing on this machine that looks at
    # them.
    #
    # Be clear about what it is worth for the two interrupt handlers. The
    # shim defines __interrupt and __far away, so this pass cannot see the
    # one property that makes a handler a handler. It finds typos. The real
    # check is `wmake -f Makefile.dos check`, which needs the Watcom tools.
    if [ "$t" = vbetest ]; then
        if ! gcc -fsyntax-only -std=c89 -Wall -Wextra -I src \
                -I tools/hostshim/dos -DMR_TARGET_DOS \
                src/vbedos.c src/presdos.c src/platdos.c src/wavedos.c src/dosmem.c \
                2>"/tmp/dossyn_$t"; then
            echo "  DOSSYN $t    (see /tmp/dossyn_$t)"
            head -5 "/tmp/dossyn_$t"
            fail=$((fail + 1)); failed="$failed $t(dos-syntax)"
            continue
        fi
    fi

    IFS='|' read -r -a a <<< "$(args_for "$t")"
    (cd /tmp && "/tmp/h_$t" "${a[@]}" >"/tmp/out_$t" 2>&1)
    rc=$?
    # 77 is "I could not test what I am for", not "all my assertions held".
    # The autotools convention, borrowed because the alternative is worse: a
    # harness that finds the game has nothing for it to check -- no second
    # language, no catalogue that renames a month -- used to return 0 and be
    # counted as a pass, which is a green line for a test that did nothing.
    # Harnesses that use it print a SKIP line saying what was missing.
    if [ "$rc" = 0 ]; then
        echo "  pass  $t"
        pass=$((pass + 1))
    elif [ "$rc" = 77 ]; then
        echo "  SKIP  $t    (see /tmp/out_$t)"
        skip=$((skip + 1)); skipped="$skipped $t"
    else
        echo "  FAIL  $t    (see /tmp/out_$t)"
        fail=$((fail + 1)); failed="$failed $t"
    fi
done

echo
if [ "$skip" -gt 0 ]; then
    echo "pass=$pass fail=$fail skip=$skip"
    # Not "no archive": a skip is either this script declining to run a
    # harness with no pak, *or* the harness itself returning 77 because the
    # game has nothing for it to look at -- Azami has no cutscene and ships
    # one language, so cuttest and langtest skip with a pak present and
    # correct. Saying "no archive" for those sent someone to rebuild a pak
    # that was already there. Each harness prints its own reason.
    echo "skipped:$skipped  (each says why in /tmp/out_<name>)"
else
    echo "pass=$pass fail=$fail"
fi
[ -n "$failed" ] && echo "failed:$failed"
exit $((fail != 0))

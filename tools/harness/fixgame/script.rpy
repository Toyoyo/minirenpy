# The fixture game's script: the smallest thing that is a game.
#
# This exists so that the harnesses which need a *pak* rather than a loose
# directory stop needing somebody's actual game. It is not trying to be
# interesting; it is trying to contain one of everything those tests reach
# for, and to be readable when one of them fails.
#
# Deliberately not a copy of a real script. A fixture built from the grammar
# the parser accepts covers more than one built from the usage that happened
# to prompt the work -- and a fixture that is somebody's story cannot be
# published with the engine.

define n = Character("Narrator")
define a = Character("Ada", color="#c8a05a")

# A title card, because a chapter in a real script opens with one and at
# least one harness relies on that.
#
# quicktest spends a single mr_interp_advance() "off the title card" before
# looking for a line being typed. With no card to dismiss, that advance is
# spent *completing the first line's reveal* instead -- so the only line the
# test ever sees is already finished, mr_interp_dialogue reports shown=-1
# forever, and the redraw-kind assertion fails with the engine behaving
# correctly. Miwa's chap12 opens `scene black with fade` / `show screen
# title with fade` / `renpy.pause()`; this is the same shape, trimmed.
screen title:
    text "[title!t]" xalign 0.5 yalign 0.5 text_align 0.5

# A story variable, so a harness can check that starting a new game resets
# one. unlocktest sets it to 7 and asks the baseline to take it back.
default chapter = 1

label start:
    # One of the two unlock flags, set a few statements into the story.
    #
    # This is unlocktest's subject: a game unlocks an extra song, an extra
    # ending, a gallery image, by setting `persistent.<something>` as the
    # player reaches it, and setting one must not set the other. Reaching
    # it has to happen by *running the script*, because the bug was in the
    # baseline the interpreter captures at init -- which no amount of
    # poking the variable store from a test would have shown.
    #
    # unlock_b is set nowhere on this path; `fixture_unlock_b` below is
    # reachable only by jumping to it deliberately, which stands in for
    # the credits label a real game gates its second flag behind.
    $ persistent.unlock_a = True
    jump chapter_one

# Never reached from `start`. See above.
label fixture_unlock_b:
    $ persistent.unlock_b = True
    return

# A plain run of dialogue, long enough for a harness to step through it,
# switch language in the middle and roll back. anygame_story_label picks the
# label with the longest unbroken run of says, which is this one.
label chapter_one:
    # A scene with a transition, so a harness waiting for one has something
    # to wait for -- quicktest's settle() loops until nothing is in flight.
    #
    # No title card and no `renpy.pause()` here, though `screen title` is
    # defined above for a harness that wants one. A pause at the top of the
    # opening label blocks `Start()` from ever reaching a line, which is
    # not what a fixture should do to a test that steps the clock and waits.
    scene fix-room with fade

    # Long on purpose. quicktest checks that a line *still being typed*
    # asks for a partial redraw rather than a whole frame, and a short line
    # has finished revealing by the time it looks -- the redraw kind came
    # back -1, "nothing is typing", and the assertion failed with the
    # engine behaving correctly. At the game's default characters-per-second
    # this takes several seconds to reveal, which is what makes the
    # mid-typing state observable.
    n "The first line, which exists to be read, and which runs on for a good while longer than it needs to so that something is still being revealed when a harness looks at the screen partway through it."
    n "The second line, which exists to be rolled back to, and which is also long enough to spend a few seconds arriving one character at a time like the one before it."
    a "A line from a character with a colour, so a name box has something to draw."
    # Thirty-odd lines, not eight.
    #
    # Harnesses advance through this label a lot and they do not all start
    # from the same place: quicktest's earlier checks had consumed every
    # line by the time it looked for one being typed, so it searched eighty
    # frames for dialogue that was not there and reported a redraw kind of
    # -1. A fixture that runs out mid-test fails in whatever way the test
    # happens to notice, which is never the way that points at the cause.
    #
    # They are also not all the same length, because a line's length is the
    # thing some of them measure -- how long it takes to reveal, whether it
    # wraps, how wide the box ends up.
    n "A fourth line."
    n "A fifth line, so there is somewhere to advance to."
    n "A sixth."
    n "A seventh, a little longer than the one before it, for variety in the widths."
    n "An eighth."
    a "A ninth, this one spoken by the character with a colour."
    n "A tenth."
    n "An eleventh, which is another of the longer ones so that a reveal in progress is easy to catch partway through."
    n "A twelfth."
    n "A thirteenth."
    a "A fourteenth, spoken again."
    n "A fifteenth."
    n "A sixteenth."
    n "A seventeenth, long enough to wrap in a narrow box and to take a moment to type itself out at the usual rate."
    n "An eighteenth."
    n "A nineteenth."
    n "A twentieth."
    a "A twenty-first."
    n "A twenty-second."
    n "A twenty-third."
    n "A twenty-fourth."
    n "A twenty-fifth."
    n "A twenty-sixth."
    n "A twenty-seventh."
    n "A twenty-eighth."
    n "A twenty-ninth."
    n "A thirtieth, and there is still more after this one."
    n "A thirty-first."
    n "A thirty-second."
    jump chapter_two

label chapter_two:
    n "A second chapter, so the label table has more than one story label."
    n "Another line here."
    n "And one more."
    # Back to the start, so the story cannot run out.
    #
    # This is not decoration. Auto-forward advances the script as virtual
    # time accumulates, and a harness that steps the clock accumulates a
    # lot of it -- quicktest's settle() alone can spend ten seconds per
    # call. Against a real game that is invisible, because a chapter is
    # thousands of statements deep; against thirty-two lines the script
    # reaches its end mid-test and every later assertion is about an engine
    # with nothing left to run. quicktest's typing check found itself in
    # chapter_two with no line revealing and reported a redraw kind of -1.
    #
    # A cycle costs nothing here: no harness on this fixture asserts that
    # the story ends, and every stepping loop in the suite is bounded, so a
    # test that did would fail rather than hang.
    jump chapter_one

# The rhythm minigame, reached the way a script reaches it. rgtest drives
# mr_rhythm_* directly for most of its checks, but one of them goes through
# the interpreter, because `call screen rhythm_game(...)` handing the
# display over is its own step and used to be where it broke.
#
# Shaped after the real thing, trimmed to what the test needs: the mute
# dance, the volume save and the checkpoint around it are the game's
# business, not the minigame's.
label fixture_rhythm:
    window hide
    $ quick_menu = False
    $ config.rollback_enabled = False
    stop music
    call screen rhythm_game('audio/fix-music.mp3', 'audio/fix-music.beatmap.txt', 'images/fix-room.png', beatmap_stride=2) with fade
    # Three targets from one tuple, which is the form the real script uses
    # and the thing rgtest's interpreter path checks: the minigame hands
    # back hits, notes and score together and the script has to unpack them
    # into three separate variables.
    $ num_hits, num_notes, rhythm_score = _return
    $ config.rollback_enabled = True
    return

# A cutscene with music playing underneath it, which is the shape
# movietest's music claim needs and no other fixture label has.
#
# Shaped after Miwa's chap0, which is where the bug it covers was found: the
# track is started once at the top of the label with loop=True, and the
# script never plays it again. Ren'Py's movie_cutscene stops the music for
# the duration and starts it back afterwards -- force_stop keeps the loop
# entries in the channel's queue (audio/audio.py:508) -- so the line after
# the cutscene still has music under it. The first implementation here left
# the restart to "the script's next play music", and a script like this one
# has not got a next play music.
# The *stand-in* path: a cutscene whose video the engine cannot open, so the
# poster frame is held on the stage instead of a picture being decoded.
#
# `audio/fixstandin.webm` is not in the fixture and is not meant to be. The
# branch under test is "mr_movie_open failed", and interp.c only looks at that
# return -- a file that is absent and one that is present in a format neither
# decoder takes reach it by the same line. Absent is the cheaper of the two to
# keep honest, since a committed .webm would need a decoder nobody has to
# verify it still fails to decode.
#
# There is no audio/fixstandin.mp3 either, which puts the cutscene on the
# fixed-length branch rather than the soundtrack's-length one. That is
# deliberate: the host waveOut shim consumes buffers the instant they are
# queued, so a track-timed wait here would be clocked off a lie. standintest's
# subject is the stage and the display list, not the duration -- cuttest owns
# the duration, against a real pak.
#
# images/fixstandin.png is 640x320, and the size is the fixture's whole point:
# it is a poster that is *not* stage-sized, which is the only shape that can
# show whether the stand-in is scaled to cover what the movie would have
# covered. fix-room is 1280x720 and cannot express the bug. Regenerate with
#   ffmpeg -f lavfi -i "color=c=0xFF00FF:s=640x320" -frames:v 1 \
#          tools/harness/fixgame/images/fixstandin.png
label fixture_standin:
    scene fix-room
    "Before the stand-in."
    $ renpy.movie_cutscene('audio/fixstandin.webm')
    "After the stand-in."
    return

# The shape a splashscreen has, and the shape is the whole fixture: a cutscene
# with a top-level `return` immediately after it and nothing in between. That
# `return` is what makes it different from fixture_standin above -- it ends the
# story, so the engine goes to the main menu on the very frame the cutscene
# ends, which is where a teardown that arrives a frame late lands on top of the
# menu instead of on top of the cutscene. A say statement after the cutscene,
# as fixture_standin has, hides that completely.
#
# Reuses fixstandin.webm, i.e. a file that does not exist, so this runs the
# stand-in path. The fault being tested is in the wait, not in the decoder, and
# both paths park in the same ST_WAIT; the stand-in is simply the half that can
# be driven on this host.
label fixture_splash:
    scene fix-room
    $ renpy.movie_cutscene('audio/fixstandin.webm')
    return

label fixture_cutscene_music:
    $ renpy.music.play("audio/fix-music.mp3", loop=True)
    "Before the cutscene."
    $ renpy.movie_cutscene('audio/fixmovie.mpg')
    "After the cutscene."
    return

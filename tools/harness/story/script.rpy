# A stable story for the harnesses that walk one.
#
# These tests are about the engine, not about this game, and they used to
# assert against the real script.rpy -- so every edit to the game broke them
# and they had to be re-fitted. Nothing here is a scene from anything; it is
# the shortest story that still contains each structural feature the tests
# need, and the point is that it does not change.
#
# What is deliberately in it, and which test wants it:
#
#   lines that type out .................. bandtest (partial frames)
#   a long run of them, window up ........ windowtest (no blinking out)
#   `scene ... with dissolve` ............ bandtest, windowtest
#   a move transition .................... windowtest (composites differently)
#   `window hide` / `window show` ........ windowtest
#   two music tracks, and a stop ......... rbmusictest
#   a title card: `show screen` + pause .. titlesavetest
#   a title card with lines either side .. pausebacktest (label pause_back,
#                                          both pause spellings and a timed one)
#   a `menu` with a `jump` and a `$` ..... choice handling in any of them
#   a flag to watch across a restart ..... restarttest
#   a pause of each kind, off to one side . afmtest (label afm_pauses,
#                                          including two untimed hard ones)
#   `[name]` in a say, off to one side ... substtest (label interp_say)
#   a gui colour written u'#rrggbb' ...... prefixtest
#   `"caption" if <expr>:` choices ....... condtest (label cond_menu)
#   a menu with a caption, both forms .... condtest (label caption_menu)
#   three menus to save in ............... savmenutest (label save_menu)
#   the move family, all eight ........... movetest (label move_trans)
#   a panned `scene` background .......... movetest (move_bg, move_tallbg)
#   im.Blur declarations, three forms .... blurtest (no label; see images)
#   `image <name>:` animations, 3 forms .. animtest (no label; see images)
#
# The order matters as much as the contents: each harness walks forward
# looking for the next thing it needs, so the features are laid out in the
# order they are looked for, with enough dialogue between them that a test
# stepping six frames at a time does not skip one.
#
# Images are flat colours generated into images/, declared explicitly rather
# than discovered: mr_vfs_enum only walks loose files on Windows, so a host
# build finds nothing by scanning and an `image` statement is what works
# everywhere.

# A gui colour spelled with Python's u'' prefix, which another game's gui.rpy
# uses for every colour it defines. The expression parser read that as the name
# `u` followed by a string, so the define evaluated to an unset variable -- and
# an unset colour is indistinguishable from an absent one, so nothing drew at
# all differently except that buttons stopped lighting up under the pointer.
#
# Defined here rather than in gui.rpy because this fixture symlinks the game's
# gui.rpy and screens.rpy on purpose: the gui under test is the real one. This
# is the one gui variable the fixture owns, and style navigation_button_text
# picks it up through gui.button_text_properties("navigation_button").
define gui.navigation_button_text_hover_color = u'#cc3366'

image room = "images/room.png"
image street = "images/street.png"
image dark = "images/dark.png"
# 1280x1460, upper half #3366cc and lower half #cc6633, from
# tools/mkfixtureimg.py. Taller than the stage *and* in two bands on purpose:
# a vertical pan over a backdrop shorter than the stage is a no-op, and over a
# flat taller one it is invisible -- the image covers the whole stage at both
# ends, so a pixel check reads the same thing whatever the engine does. Which
# is exactly why the pan's end position was wrong twice with every harness
# green. One pixel now says which part of the backdrop is on screen.
image tall = "images/tall.png"
image person = "images/person.png"

# 1280x720 in four quadrants, #20c080 and #f04010 on the diagonals, from
# tools/mkfixtureimg.py -m quad. The blur fixtures need an image with a hard
# edge on *both* axes: blurring a flat image gives the flat image back, so on
# any of the fixtures above a blur test passes whether the blur runs or not.
image quad = "images/quad.png"
# A second attribute set on the `person` tag, so a re-show can change the
# image without saying anything about where it goes -- which is the shape
# `show azami magical pout: ... / show azami magical fear` has.
image person other = "images/person.png"

# The three forms of im.Blur the engine accepts. blurtest compares the pixels
# these resolve to against Ren'Py's own blur (tools/harness/blurref.c), so
# what is being checked here is the parse and the wiring: that a radius
# arrives, that a second radius is a separate axis rather than being ignored,
# and that a nested im.Flip still flips.
# 64x64 RGBA from tools/mkfixtureimg.py -m vband: the left half opaque
# #20c080, the right half #f04010 at alpha 0. For zoomtest.
#
# The transparent half is a *bright and differently hued* colour on purpose.
# A bilinear resample of an image with an alpha edge has to weight each colour
# by its own alpha, or the colour of the invisible pixels bleeds into the
# visible edge. With the usual transparent-black that bleed only darkens the
# edge, which on a flat sprite over a flat background is indistinguishable
# from a slightly lower alpha -- so a fixture whose clear half is black cannot
# fail the check. Orange behind green can: the bleed shows up as red, and the
# assertion is that green stays the larger channel.
image fringe = "images/fringe.png"
# A second attribute set on the same tag, so a re-show can change the picture
# without saying anything about the zoom -- the shape
# `show azami magical pout: xzoom 0.5` / `show azami magical fear` has.
image fringe other = "images/fringe.png"

# 64x64 opaque, upper half #c02080 and lower half #2080c0, from
# tools/mkfixtureimg.py -m hband. A vertical flip is only observable on an
# image whose rows differ, and `fringe` above differs by column: xzoom -1.0 on
# it is visible, yzoom -1.0 is not.
image band = "images/zoomband.png"

# 64x64 opaque in four quadrants, the same two colours. This one is for the
# both-axes-negative case: flipping it on one axis swaps the diagonals and
# flipping it on both puts them back, so it fails if either sign is dropped
# -- which neither of the two single-axis fixtures can catch.
image quadsmall = "images/zoomquad.png"

# Eight tags for the cache test, more than the zoomed-copy cache has slots,
# alternating between the two pictures. zc5 and zc6 resolve to the same zoomed
# *size* as each other and to different pictures, which is what says the cache
# key is the name as well as the size.
image zc1 = "images/fringe.png"
image zc2 = "images/zoomband.png"
image zc3 = "images/fringe.png"
image zc4 = "images/zoomband.png"
image zc5 = "images/fringe.png"
image zc6 = "images/zoomband.png"
image zc7 = "images/fringe.png"
image zc8 = "images/zoomband.png"

# im.FactorScale, for fscaletest. The forms the grammar accepts, not just the
# one Azami writes: one factor (the second defaults to it, im.py:1313-1314),
# two factors, a factor of exactly 1.0, a nested im.Flip, the bare filename
# spelling without the images/ prefix, and `bilinear=False`, which is read and
# ignored because the resampler has no nearest path.
#
# zoomband.png is 64x64, so these are 32x32, 128x64, 64x64 and 32x32.
image fs half = im.FactorScale("images/zoomband.png", 0.5)
image fs wide = im.FactorScale("images/zoomband.png", 2.0, 1.0)
image fs dup = im.FactorScale("images/zoomband.png", 1.0)
image fs bare = im.FactorScale("zoomband.png", 0.5)
image fs nearest = im.FactorScale("images/zoomband.png", 0.5, 0.5, bilinear=False)

# 64 * 0.51 is 32.64. Ren'Py truncates (im.py:1334), so this is 32 and not 33
# -- the one case that says which way the size is rounded.
image fs trunc = im.FactorScale("images/zoomband.png", 0.51)

# Nested, which the grammar allows even though no game writes it. fringe.png
# has its opaque half on the left, so flipped it is on the right.
image fs flip = im.FactorScale(im.Flip("images/fringe.png", horizontal=True), 0.5)

# A factor of zero has no image to produce, so the declaration is dropped and
# the name does not exist. Asserted, because the alternative -- a one-pixel
# sprite -- would hide a typo.
image fs zero = im.FactorScale("images/zoomband.png", 0.0)

# One zoomed copy larger than the cache's whole byte budget, which is the
# other half of the eviction path: it is served and simply does not stay
# resident, and the eight above have to come back correct once it goes.
image bigtall = "images/tall.png"

image quad blurred = im.Blur("images/quad.png", 2.0)
image quad ellipse = im.Blur("images/quad.png", 1.0, 6.0)
image quad flipblur = im.Blur(im.Flip("images/quad.png", horizontal=True), 2.0)

# Animations, for animtest. 64x64 RGBA from tools/mkfixtureimg.py -m vband,
# so each frame is a left half and a right half:
#
#   anim0   left ff0000 opaque   right fully clear
#   anim1   left 0000ff opaque   right 0000ff at alpha 128
#   anim2   left 00ff00 opaque   right 00ff00 at alpha 64
#
# The left halves are opaque and differ in colour, which is what makes "which
# frame is showing" a single pixel read. The right halves differ in *alpha*,
# which is the only way to see whether the cross-fade interpolates alpha
# correctly -- two opaque frames blend identically whether it does or not, so
# an all-opaque fixture cannot fail that check.
#
# Durations are deliberately unequal: 100/200/400ms. With equal ones a reader
# that divided the elapsed time by the frame count instead of walking the
# frames would still land on the right frame, and the test would pass on an
# implementation that only works for the uniform case.
image anim cut:
    "images/anim0.png"
    pause 0.1
    "images/anim1.png"
    pause 0.2
    "images/anim2.png"
    pause 0.4
    repeat

# The same thing with a cross-fade into each frame, which is the form the
# scripts actually use. Note what the stock `dissolve` does here: it is 500ms
# and every frame below is shorter, so each fade gets clamped to fill its own
# frame exactly. That is the clamp under test as much as the blend is -- an
# unclamped fade longer than its frame would never reach the frame it is
# fading into, leaving the animation permanently half-way.
image anim fade:
    "images/anim0.png" with dissolve
    pause 0.1
    "images/anim1.png" with dissolve
    pause 0.2
    "images/anim2.png" with dissolve
    pause 0.4
    repeat

# One-shot: no `repeat`, so it holds its last frame forever.
image anim once:
    "images/anim0.png"
    pause 0.1
    "images/anim1.png"
    pause 0.2

# An animated *background*, for bandtest (label anim_bg).
#
# The partial-frame path repaints only the dialogue box while a line types out,
# and whether that is safe depends on nothing outside the box changing. An
# animated background is the case where something does, and Hinata has one --
# so "the background is static between clicks" is an assumption about one
# game's art, not about the engine.
#
# Deliberately built from the three full-stage backgrounds rather than from
# anim0..2: those are 64x64 sprites, and a 64x64 scene image never touches the
# bottom 185 rows of the stage, so an animation made of them could not change
# a single pixel the band covers and the test would pass without testing
# anything. room/street/dark are 1280x720 and differ sharply inside the band
# -- (180,150,120) against (100,140,190) against (42,42,46) at y=640 -- which
# is what makes a stale band visible.
#
# Durations unequal and short relative to a 33ms frame step, so a walk crosses
# several frame changes and gets a mix of banded and full frames. `repeat`,
# because the interesting state is the steady one: a one-shot animation would
# settle and the rest of the walk would be the static case again.
image anim bg:
    "images/room.png"
    pause 0.1
    "images/street.png"
    pause 0.2
    "images/dark.png"
    pause 0.4
    repeat

# Named transforms, for tfatltest (label tf_atl). The shapes that matter are
# the three Azami has: a placement with no animation, a placement followed by
# a timed step, and a body that is *only* a timed step. The last one is the
# one that was reported broken -- there is nothing static in it at all, so if
# the step is lost the statement does nothing whatsoever.
transform here940:
    xoffset 940
    yalign 1.0

transform groundrise:
    xoffset 0
    yalign 1.0
    linear 0.5 yoffset 300

transform slideleft:
    linear 0.5 xoffset 100

# Azami's `moveleft`, verbatim in shape. `xalign` inside a transform body is
# not the same stored property as the `xalign` a stock position carries, which
# is the whole point of the test.
transform slideleftalign:
    linear 0.5 xalign 0.0

# And the other direction: a step whose target is a bare stock position.
transform slidetoright:
    linear 0.5 right

# For zoomtest. An absolute spot well clear of the dialogue window, so the
# test can take the bounding box of everything that differs from the empty
# stage without the say box or the speaker's name landing in it.
#
# Every property it depends on is named, which matters: a re-show keeps
# whatever the previous one set. Without the `xoffset`, the `xoffset 370` case
# below leaks into every statement after it; without the `xalign 0.0`, the
# `zoomcenter` case does, and the sprite lands 608px to the right of where the
# transform says. Both were caught by this test rather than reasoned out.
transform zoomspot:
    xalign 0.0
    xoffset 400
    yoffset 100

# The same spot by alignment rather than by offset. This is the one that says
# whether the placement is resolved from the *scaled* size: at xzoom 0.5 a
# 64-wide image is 32 wide, so `xalign 0.5` puts its left edge at 624 rather
# than at 608.
transform zoomcenter:
    xalign 0.5
    xoffset 0
    yoffset 100

# A row of its own for the cache test: each show adds its own xoffset.
transform zoomrow:
    xalign 0.0
    yoffset 100

define n = Character(None)
define a = Character("Ada", color="#c8ffc8")

screen title:
    add "images/dark.png"
    text "[title]" xalign 0.5 yalign 0.5 text_align 0.5

# A screen that is drawn *with the scene* -- no zorder, so zorder 0 -- and that
# paints only inside the dialogue strip, for bandtest (label band_screen).
#
# Both halves of that matter. Inside the strip, because a screen painting
# outside it makes every frame a full one (interp.c's extent test) and there
# would be no band frame to say anything about. At zorder 0, because that is
# the layer draw_screens paints and mr_scene_render_overlay does not: the band
# cache stands in for exactly that drawing, so a strip kept while this is up
# would hold the button's pixels and then skip the pass that registers it as
# focusable -- the button would still be *visible* and no longer *clickable*,
# which no pixel comparison can see. The quick menu cannot stand in for this:
# it is `zorder 100` and so an overlay, redrawn every frame either way.
#
# ypos 560 puts it in the strip for this fixture's gui.textbox_height of 185
# (the window runs 535..720) and clear of the quick menu's own rows at 699.
screen band_screen():
    textbutton "clickable" xpos 40 ypos 560 action Return()

label start:
    $ config.rollback_enabled = True
    $ probe_flag = 0
    $ visited = 0

    scene room
    with fade

    play music "audio/one.mp3" fadein 1.0 loop

    # --- a long run of plain dialogue, window up throughout ---
    n "The first line, long enough that it takes a moment to type itself out."
    a "A second line, from someone with a name."
    $ probe_flag = 1
    n "A third, so there is something to roll back to."
    a "A fourth."
    n "A fifth, and this one runs on a good deal further than the others do."
    a "A sixth."
    n "A seventh."
    a "An eighth, with a name again."
    n "A ninth."
    a "A tenth."
    n "An eleventh, longer once more so that it types for a while."
    a "A twelfth."
    n "A thirteenth."
    a "A fourteenth."
    n "A fifteenth."
    a "A sixteenth."
    n "A seventeenth, and the window has not gone anywhere."
    a "An eighteenth."
    n "A nineteenth."
    a "A twentieth."
    n "A twenty-first."
    a "A twenty-second."
    n "A twenty-third."
    a "A twenty-fourth."
    n "A twenty-fifth, and that is enough of those."

    # --- an effect, with the window up over it ---
    scene street
    with dissolve

    a "After a dissolve."
    n "Still after it."
    a "And once more."

    # --- a move, which composites differently ---
    show person at center
    with moveinright

    a "Now with someone on screen, after a move."
    n "And another line while they are there."
    a "And a third."

    # --- the window taken down and put back ---
    window hide
    n "A line with the window taken down beforehand."
    window show

    a "And the window is back."
    n "With another line after it."
    a "And another."

label the_menu:
    menu:
        "Carry on":
            jump chapter_two
        "Carry on, but set a flag":
            $ visited = 1
            jump chapter_two

label chapter_two:
    stop music fadeout 1.0
    window hide
    $ title = "Chapter Two"

    scene dark with fade
    show screen title with fade
    $ renpy.pause()
    hide screen title with fade

    scene room
    with fade
    play music "audio/two.mp3" fadein 1.0 loop
    window show

    a "The second chapter, on a different track."
    n "One more line so the chapter is not over at once."
    a "A third."
    n "A fourth."
    a "A fifth."
    n "A sixth."
    a "A seventh."
    n "An eighth."
    a "A ninth."
    n "And the last."

    # A `stop music fadeout` with dialogue straight after it, rather than the
    # title card chapter_two opens with. The difference matters: these lines
    # take their checkpoints while the outgoing track is still audible, which
    # is where a checkpoint used to record the track the script had just
    # stopped -- and so rolling back to here brought it back. The real script
    # does this after the concert piece.
    stop music fadeout 1.0
    a "The music is fading out as this line appears."
    n "And it must not come back when we roll back to here."
    a "A third line, to roll back from."

    # A pause of each kind, for afmtest: auto-forward crosses an untimed one
    # and leaves a timed one to its own timer, which is the difference between
    # `afm = " "` and `afm = None` in statementexports.py:176-179.
    #
    # In a label of its own, reached only by a harness that asks for it by
    # name. Appended to the walk instead, these would be the last thing every
    # test that reaches the end of the story runs into -- and a hard pause is
    # not something a frame-stepping harness can click its way out of.
    jump the_end

# A title card to roll back *to*, for pausebacktest.
#
# The shape is chapter_two's -- `show screen title` then `$ renpy.pause()` --
# which titlesavetest already saves and loads on. What this label adds is
# something on either side of the card: a line before it, lines after it, and
# a variable that changes across it, so a rollback that lands on the pause can
# be told from one that lands anywhere else.
#
# Both spellings of an untimed pause are here, and a timed one, because Ren'Py
# checkpoints the untimed kind only (statementexports.py:151-154): the `pause`
# statement and `$ renpy.pause()` are stops, and `pause 0.2` is not.
label pause_back:
    window hide
    # Both of these make the label self-contained, because a harness enters it
    # more than once in a process: `scene` clears the image layer and not the
    # screens (Ren'Py's rule), so a card left up by an earlier run would still
    # be up, and `title` would still hold that run's value. A test whose
    # variables depend on what ran before it cannot tell a real fault from its
    # own leftovers -- this one did, once.
    hide screen title
    scene dark
    $ pb = 0
    $ title = ""
    a "A line before the card."

    # The title is assigned immediately before the card and nowhere else,
    # which is the shape the game has it in -- so the value the card draws
    # belongs to the step that reaches the card, and a rollback or a
    # rollforward that mishandles that step shows the *other* card's title.
    #
    # The two titles are deliberately very different lengths. The reported
    # faults are both about what the card *draws* -- the other card's text, or
    # nothing at all -- so the test measures lit pixels on the stage, and two
    # titles of the same width would be indistinguishable there.
    $ pb = 1
    $ title = "One"
    show screen title with fade
    $ renpy.pause()
    hide screen title with fade

    $ pb = 2
    a "A line after the card."

    pause
    a "A line after the bare pause."

    pause 0.2
    a "A line after the timed pause."

    # A second card, so that a step can land on a card whose title differs
    # from the one before it. A chapter change in the game is exactly this:
    # stop the music, hide the window, set the title, show the card.
    $ pb = 3
    $ title = "The second card, with a much longer title"
    show screen title with fade
    $ renpy.pause()
    hide screen title with fade

    $ pb = 4
    a "A line after the second card."
    a "One more line past the second card."

label afm_pauses:
    window hide
    scene dark
    a "A line before the pauses."
    $ renpy.pause(3.0)
    a "After a timed pause."
    $ renpy.pause(3.0, hard=True)
    a "After a hard pause."
    $ renpy.pause()
    a "After an untimed pause."

    # An untimed *hard* pause, which nothing the player presses can end: the
    # keysym its SayBehavior dismisses on is `dismiss_hard_pause`, bound to
    # the empty list in 00keymap.rpy:71. Two of them, because it has exactly
    # two exits -- auto-forward and skipping -- and each consumes one.
    a "A line before the hard pauses."
    $ renpy.pause(hard=True)
    a "Between the two."
    $ renpy.pause(hard=True)
    a "After both."

    # `[name]` substitution in *dialogue*, which is where it was missing: it
    # worked in screen text all along, so the one place a writer reaches for
    # it first was the one place it did nothing. In its own label for the same
    # reason the pauses are.
    #
    # The long line is deliberate: the typewriter and the auto-forward delay
    # count characters of the line, and counting the brackets while drawing
    # the value gets both wrong.
label interp_say:
    window hide
    scene dark
    $ sub_num = 2
    $ sub_str = "Ada"
    # The substituted line is *first* so a harness can measure it without
    # navigating to it. substtest's timing block got that wrong and silently
    # measured the plain line instead.
    n "Number [sub_num], name [sub_str], and [sub_num!t] with a flag."
    n "A plain line, no brackets in it at all."

    # Menu choices with conditions, which is `"caption" if <expr>:`. In a
    # label of its own because a harness has to be able to set the flags
    # *before* the menu is reached -- the conditions are evaluated on arrival,
    # so a menu in the middle of the walk would already have been decided.
label cond_menu:
    window hide
    scene dark
    $ cond_a = False
    $ cond_picked = 0

    # The third option is gated on *persistent* data, which is what a game
    # gating an ending actually does -- and it is the only kind of condition
    # whose answer can stop being on offer between a rollback and the
    # rollforward that follows it, because the rollback log restores ordinary
    # variables and deliberately does not restore this (history.c:212).
    $ persistent.cond_ok = True

    # Something to roll back *to*: the menu is a checkpoint, and a rollback
    # from the menu needs a point before it or it has nowhere to go.
    a "A line before the conditional menu."

    # Three declared, the middle one conditioned away. So the second row on
    # screen is declared option 2, and anything that confuses a row with a
    # declared index picks the wrong branch here rather than merely drawing
    # the wrong number of rows.
    menu:
        "First, always":
            $ cond_picked = 1
        "Second, never" if cond_a:
            $ cond_picked = 2
        "Third, when unlocked" if persistent.cond_ok == True:
            $ cond_picked = 3

    a "After the conditional menu."

    # Nothing left to offer. Ren'Py does not show an empty menu: display_menu
    # returns None and the statement falls through (menuexports.py:117-121).
    menu:
        "Not this one" if cond_a:
            $ cond_picked = 4
        "Nor this one" if False:
            $ cond_picked = 5

    a "After the menu that was never offered."
    jump the_end

    # A menu that opens with a line of dialogue -- its caption. Reported from
    # play on another game, in this exact shape: the caption was not displayed
    # at all, and every option ran the block of the one above it because the
    # caption line was counted as an option.
    #
    # So the options here deliberately have *several* statements each, and the
    # line after the menu is distinct from both: an off-by-one lands on a real
    # line either way, and only checking which line comes up tells them apart.
label caption_menu:
    window hide
    scene dark
    $ wasadate = -1

    # Something to roll back *to*, as in cond_menu: without a checkpoint
    # before it the menu is the oldest one and a rollback there has nowhere
    # to go, which looks exactly like a rollback that refuses to leave a menu.
    a "A line before the captioned menu."

    menu:
        a "So, was it a date or not?"
        "Yes, probably... maybe...":
            $ wasadate = 1
            a "Maybe it was, after all."
            a "One can always dream, at least!"
        "No...":
            $ wasadate = 0
            a "To be honest, I don't think it was."
            a "But who knows, maybe one day?"

    a "Without a definitive answer, I shake my head before heading home."

    # The other caption form: a bare string on a line of its own, with no
    # colon and no block. Ren'Py narrates it, so it comes up with no speaker.
    menu:
        "A caption with nobody saying it"
        "Left":
            $ wasadate = 10
        "Right":
            $ wasadate = 11

    a "After the narrated caption."
    jump the_end

    # The move family, off to one side because each of these is measured by
    # pixel position and needs a known starting point.
    #
    # `person` is a 300x600 flat colour on a flat background, so the image's
    # top edge is findable by scanning a column -- which is how a *move* is
    # told from a dissolve without trusting the engine's own report: a move
    # puts the image at one intermediate position, a dissolve leaves it at
    # both ends at half strength.
label move_trans:
    window hide
    scene dark
    show person at center
    with None
    a "Before the move."

    # A MoveTransition written longhand, which is how a second game spells a
    # move with a duration other than the stock 0.5s. Every keyword in it is
    # Ren'Py's own default (movetransition.py:457), so this is
    # MoveTransition(1.0): a move, and 1000ms rather than 500.
    show person:
        yoffset 300
    with MoveTransition(1.0, enter=None, leave=None, old=False, layers=['master'], time_warp=None, enter_time_warp=None, leave_time_warp=None)
    a "After the MoveTransition."

    # The vertical half of the family. Ren'Py builds all eight from
    # MoveTransition plus an enter_factory or a leave_factory
    # (00definitions.rpy:250-300); only the horizontal four existed here.
    hide person
    with moveoutbottom
    a "After moveoutbottom."

    show person at center
    with moveinbottom
    a "After moveinbottom."

    hide person
    with moveouttop
    a "After moveouttop."

    show person at center
    with moveintop
    a "After moveintop."
    jump the_end

    # The reported shape, copied statement for statement from the game that
    # showed it: the *same tag* re-shown with a new `at` position *and* a
    # transform block, then moved. The label above uses a bare `show person:
    # yoffset 300`, which is not the same thing -- it glided correctly while
    # the real case did not.
    # Two variants, differing in one thing only: whether the moving `show`
    # carries a transform block as well as a new `at` position. `person` is
    # 300 wide, so `at center` puts its left edge at 490 and `at right` at
    # 980 -- both fully on screen, which is what makes the left edge a usable
    # measurement throughout.
label move_atl:
    window hide
    scene dark
    show person at center:
        yoffset 60
    with dissolve
    a "At center, to begin with."

    # Variant A: a new `at`, no transform block.
    show person at right
    with MoveTransition(1.0, enter=None, leave=None, old=False, layers=['master'], time_warp=None, enter_time_warp=None, leave_time_warp=None)
    a "Moved with no block."

    show person at center:
        yoffset 60
    with dissolve
    a "Back at center."

    # Variant B: a new `at` *and* a transform block. The reported shape.
    show person at right:
        yoffset 60
    with MoveTransition(1.0, enter=None, leave=None, old=False, layers=['master'], time_warp=None, enter_time_warp=None, leave_time_warp=None)
    a "Moved with a block."
    jump the_end

    # The third shape, and the one the reported scene actually has: a `scene`
    # background re-shown as an image so it can be panned.
    #
    #     scene bg parknight          <- goes into the list's `bg` slot
    #     ...
    #     show bg parknight:          <- becomes a *shown entry*, tag `bg`
    #         yoffset 740
    #     with MoveTransition(2.0, ...)
label move_bg:
    window hide
    scene room
    show person at center
    with None
    a "Before the pan."

    show room:
        yoffset 300
    show person:
        yoffset 300
    with MoveTransition(1.0, enter=None, leave=None, old=False, layers=['master'], time_warp=None, enter_time_warp=None, leave_time_warp=None)
    a "After the pan."
    jump the_end

    # The reported scene's real shape: a backdrop *taller than the stage*,
    # panned by re-showing it. This is a camera tilt -- the ground slides out
    # of frame and the sky comes into it -- and it is the only shape in which
    # the pan's end position is observable at all.
    #
    #     scene tall              -> y = 720-1460 = -740, lower band showing
    #     show tall: yoffset 740  -> y = 0,               upper band showing
label move_tallbg:
    window hide
    scene tall
    a "Before the tall pan."

    show tall:
        yoffset 740
    with MoveTransition(1.0, enter=None, leave=None, old=False, layers=['master'], time_warp=None, enter_time_warp=None, leave_time_warp=None)
    a "After the tall pan."
    jump the_end

# An ATL that moves the image and stops there, with dialogue on either side.
# The point is the rollback: the checkpoint for the first line is taken as it
# goes up, which is before the half second has run, so what it records is the
# image at the *start* of the drop. Rolling back onto that line must not put
# it back up there.
#
# `repeating` is the other kind -- an ATL with `repeat` is never finished, so
# a rollback onto it must leave it running rather than freezing it.
label rb_atl:
    window hide
    scene dark
    show person at center
    a "Before the drop."

    show person:
        linear 0.5 yoffset 200
    a "During the drop."
    a "After the drop."

    show person at center:
        yoffset 0
        linear 0.25 yoffset 80
        linear 0.25 yoffset 0
        repeat
    a "Bobbing."
    a "Still bobbing."
    jump the_end

# Saving part way through a transform block and loading the slot again.
#
# The reported case is the credits roll, which is one `linear 90` -- long
# enough that a save taken during it is loaded during it too, and long enough
# that freezing it is the difference between the credits ending and `fin`
# appearing over a half-scrolled list. Four shapes, because the save has to
# carry the step index and the elapsed time and not just the fact of an
# animation:
#
#   - one long step, saved in the middle of it
#   - two steps, saved during the second, so the step index matters
#   - `repeat`, which has no end to resume to
#   - a block that has already finished, which must not come back as running
label save_atl:
    window hide
    scene dark
    show person at center
    a "Before the roll."

    show person:
        yoffset 0
        linear 4.0 yoffset 400
    a "The long roll."

    show person at center:
        yoffset 0
        linear 0.5 yoffset 100
        linear 0.5 yoffset 300
    a "Two steps."

    show person at center:
        yoffset 0
        linear 0.25 yoffset 80
        linear 0.25 yoffset 0
        repeat
    a "Bobbing."

    show person at center:
        yoffset 0
        linear 0.2 yoffset 60
    a "A short one."
    a "Long since over."
    jump the_end

# Three menus to save in, differing only in what dialogue is on screen when
# the menu opens -- which is the whole of the bug: a save taken at a menu is
# restored as "a line waiting to be dismissed" whenever there is a line, so
# the menu stayed off screen until the player pressed something.
#
# Each option sets `wasadate` to its own value, so the test can tell that the
# restored menu answers to the right block rather than merely looking right.
label save_menu:
    window hide
    scene dark
    $ wasadate = -1

    # 1. Nothing said yet, so no dialogue is up: the case that already worked,
    #    kept as the regression guard.
    menu:
        "Cold, left":
            $ wasadate = 1
        "Cold, right":
            $ wasadate = 2

    # 2. A bare menu with the line before it still on screen. The menu has no
    #    caption of its own, but there is dialogue up, which is enough.
    a "A line that stays up under the bare menu."

    menu:
        "Warm, left":
            $ wasadate = 3
        "Warm, right":
            $ wasadate = 4

    # 3. A menu with a caption of its own, which is the shape it was reported
    #    in.
    menu:
        a "A caption the menu brought with it."
        "Captioned, left":
            $ wasadate = 5
        "Captioned, right":
            $ wasadate = 6

    a "After the saved menus."
    jump the_end

# A re-show immediately after an ATL, with nothing between the two to draw a
# frame. The second statement says nothing about where the image goes.
label atl_reshow:
    window hide
    scene dark
    show person at center
    a "Before the move."

    show person:
        linear 0.5 xoffset 500
    show person other with dissolve
    a "Replaced before it could run."

    # The same pair with the story stopping in between, so the move really
    # does happen before it is replaced.
    show person at center:
        xoffset 0
    a "Back at centre."
    show person:
        linear 0.5 xoffset 500
    a "Moving."
    show person other with dissolve
    a "Replaced after it ran."

    # The shape the report actually has: an earlier ATL that *did* run and
    # left an offset behind, and then the pair above on top of it. The
    # question is what the second move's starting point is.
    # Explicitly back to zero: an `at` does not clear an xoffset that is
    # already in force, in Ren'Py either -- take_state copies it across
    # (transform.py's take_state, `self.xoffset = ts.xoffset`).
    show person at center:
        xoffset 0
    a "Centre again."
    show person with move:
        linear 0.5 xoffset 830
    a "Moved a long way."
    show person:
        linear 0.5 xoffset 500
    show person other with dissolve
    a "Replaced while a second move was pending."
    jump the_end

# A named transform whose body animates, reached through `at`. Reported from
# Azami: `show saza at moveleft`, where moveleft is nothing but
# `linear 0.5 xalign 0.0`, landed on the right coordinates without ever
# travelling to them.
label tf_atl:
    window hide
    scene dark
    show person at here940
    a "Parked on the right."

    # The reported statement, in isolation and with the story stopping right
    # after it so the move has frames to run in.
    show person at slideleft
    a "Sliding left."

    # The reported *shape*: nothing draws a frame between the `at` and the
    # dissolve that follows it.
    show person at here940
    a "Parked again."
    show person at slideleft
    show person other with dissolve
    a "Sliding left under a dissolve."

    # A transform that places and then animates. The placement must land at
    # once and the step must still run from it.
    show person at groundrise
    a "Rising from the ground."

    # The report, with the statement that precedes it in Azami. `at right`
    # puts her x under one name and `linear 0.5 xalign 0.0` reads it under
    # the other, so the travel was lost while the destination was kept.
    show person at right
    a "Over at the right."
    show person at slideleftalign
    a "Sliding in from the right edge."

    # The mirror: the step's target is the stock position, and the placement
    # it starts from is the pos/anchor pair.
    show person at truecenter
    a "Dead centre."
    show person at slidetoright
    a "Sliding back out to the right."

    # The ghosting case. A move armed by one statement, and a dissolve
    # carried by the *next* one. The dissolve must transition only its own
    # change: the mover is already on the old side, so there must be exactly
    # one of her on the stage, not a half-transparent copy at each end.
    # The baseline has to be pinned by a transition of its own, or the "old"
    # side of the dissolve below is whatever the last commit happened to
    # leave behind and the ghost lands somewhere the test is not looking.
    # Azami pins it the same way: a `with dissolve` a few lines earlier.
    # `scene` first: the sections above leave an align in force on the tag,
    # and an align plus this section's xoffset put her off the right edge
    # where there is nothing to ghost. Clearing the list is what makes the
    # starting position this section's own business.
    scene dark
    show person at here940
    show person other with dissolve
    a "Parked for the ghost test."
    show person at slideleft
    show person other with dissolve
    a "Moving while something else dissolves."
    jump the_end

# Transform `alpha`, for alphatest. The forms are the ones the games write:
# a static value on a show block, a value of 0 and of 1 at the ends of the
# range, a timed fade, and an alpha that has to survive a re-show that says
# nothing about it.
label alpha_show:
    window hide
    scene dark
    show person at here940
    a "Opaque."

    show person at here940:
        alpha 0.5
    a "Half transparent."

    # Written with a leading dot, which is how Azami writes it, and at a
    # value that is not a half -- the blend has to be computed from the
    # number, not chosen from a handful of cases.
    show person at here940:
        alpha .3
    a "Nearly gone."

    show person at here940:
        alpha 0.0
    a "Invisible."

    show person at here940:
        alpha 1.0
    a "Opaque again."

    # A re-show that changes only the picture must not throw the alpha away:
    # it is transform state like any other.
    show person at here940:
        alpha 0.5
    a "Half transparent again."
    show person other
    a "Still half transparent."

    # A timed fade. `alpha` is interpolated by the same code that interpolates
    # a position, which is the whole reason it lives in mr_place.
    show person at here940:
        alpha 1.0
        linear 0.5 alpha 0.0
    a "Fading out."
    jump the_end

# Transform xzoom/yzoom, for zoomtest. Every form the parser accepts: one
# axis, both axes, a factor above 1, a negative factor (which mirrors), a zoom
# combined with offsets and with an alignment, a zoom that survives a re-show,
# and a zoom reached by a timed step.
label zoom_show:
    window hide
    scene dark
    # The reference frame. Everything after this is measured as "what differs
    # from the empty stage", which is immune to dark.png's two shades and to
    # anything a screen draws.
    a "Empty stage."

    show fringe at zoomspot
    a "Unzoomed."

    show fringe at zoomspot:
        xzoom 0.5
    a "Half as wide."

    # Spelled out rather than left to be forgotten: an unmentioned property
    # keeps its value, so without the `xzoom 1.0` this would be a quarter of
    # the area rather than half the height.
    show fringe at zoomspot:
        xzoom 1.0
        yzoom 0.5
    a "Half as tall."

    # Azami's shape, verbatim. The offsets are screen pixels and are not
    # scaled by the zoom: Ren'Py resamples first and places afterwards.
    show fringe at zoomspot:
        xzoom 0.5
        yzoom 0.5
        xoffset 370.0
        yoffset 300.0
    a "Half size, offset."

    # Above 1.0. accelerator.pyx has no clamp, so this is twice the size.
    show fringe at zoomspot:
        xzoom 2.0
        yzoom 1.0
    a "Twice as wide."

    show fringe other
    a "Still twice as wide."

    show fringe at zoomcenter:
        xzoom 0.5
        yzoom 1.0
    a "Centred and halved."

    # Negative mirrors, which is what the im.Flip deprecation note points at.
    # The fixture's opaque half is on the left, so this moves it to the right.
    show fringe at zoomspot:
        xzoom -1.0
        yzoom 1.0
    a "Mirrored."

    # Upscaled, which is where the interpolation shows: a nearest-neighbour
    # resample of an alpha edge produces no partly transparent pixels at all,
    # and a resample that ignores premultiplication pulls the clear half's
    # orange into the green edge.
    show fringe at zoomspot:
        xzoom 2.0
        yzoom 2.0
    a "Doubled."

    # Timed, so the zoom is interpolated by the same code that interpolates a
    # position. Measured in flight: the endpoints alone cannot tell a shrink
    # from a jump.
    show fringe at zoomspot:
        xzoom 1.0
        yzoom 1.0
        linear 0.5 xzoom 0.5
    a "Shrinking."

    # The combined `zoom`, which multiplies into both axes and is separate
    # state from the two above (transform.py:1382, accelerator.pyx:555-557).
    #
    # Most of these name all three factors, because a re-show keeps what it
    # does not mention and the point of several of them is a *specific*
    # combination. The two that deliberately leave one out are marked.
    show fringe at zoomspot:
        zoom 1.0
        xzoom 1.0
        yzoom 1.0
    a "Zoom reset."

    show fringe at zoomspot:
        zoom 0.5
    a "Zoom halved."

    show fringe at zoomspot:
        zoom 2.0
    a "Zoom doubled."

    show fringe other
    a "Zoom still doubled."

    # Deliberately mentions only xzoom. The result is the *product* -- x is
    # 2.0 * 0.5 and y is still 2.0 -- which is the case that fails if `zoom`
    # is folded into the per-axis factors at parse time: the folded version
    # has this statement replace the 2.0 instead of multiplying with it.
    show fringe at zoomspot:
        xzoom 0.5
    a "Zoom times xzoom."

    # And the same two numbers in the opposite order, which has to give the
    # same answer. The second statement here is the one that mentions only
    # `zoom`.
    show fringe at zoomspot:
        zoom 1.0
        xzoom 0.5
        yzoom 1.0
    a "Only xzoom."
    show fringe at zoomspot:
        zoom 2.0
    a "Then zoom."

    show fringe at zoomspot:
        zoom 0.0
        xzoom 1.0
        yzoom 1.0
    a "Zoom zero."

    # Aligned, so the placement has to be resolved from the scaled size the
    # same way it is for xzoom.
    show fringe at zoomcenter:
        zoom 0.5
        xzoom 1.0
        yzoom 1.0
    a "Zoom centred."

    # Timed, with an xzoom standing. The interpolation is on `zoom` itself,
    # so x passes through 0.75 * 0.5 and not through 0.75.
    show fringe at zoomspot:
        zoom 1.0
        xzoom 0.5
        yzoom 1.0
        linear 0.5 zoom 0.5
    a "Zoom shrinking."

    # Negative factors, checked as pixels rather than as a bounding box. Each
    # fixture is asymmetric on exactly the axis it is used for.
    hide fringe
    show band at zoomspot
    a "Band upright."

    show band at zoomspot:
        yzoom -1.0
    a "Band flipped."

    hide band
    show quadsmall at zoomspot
    a "Quad upright."

    show quadsmall at zoomspot:
        xzoom -1.0
    a "Quad mirrored."

    # Both at once, which puts the diagonals back where they started. This is
    # the case that fails if one of the two signs is dropped.
    show quadsmall at zoomspot:
        xzoom -1.0
        yzoom -1.0
    a "Quad flipped twice."
    hide quadsmall

    # `zoom` carries a sign too, and the signs multiply. These two statements
    # are the same three numbers on two fixtures, and between them they say
    # which axis ended up mirrored: zoom * xzoom is +1 and zoom * yzoom is
    # -1, so y flips and x does not.
    #
    # An implementation where any negative factor mirrors both axes -- or one
    # that takes the absolute value too early -- passes one of these and
    # fails the other.
    show band at zoomspot:
        zoom -1.0
        xzoom -1.0
        yzoom 1.0
    a "Band two negatives."
    hide band

    show fringe at zoomspot:
        zoom -1.0
        xzoom -1.0
        yzoom 1.0
    a "Fringe two negatives."
    hide fringe

    # Eight distinct zoomed copies in one frame, which is more than the cache
    # has slots: every slot is pinned and the next arrival has to take one
    # anyway. Measured twice, with a bigger image in between that is on its
    # own larger than the cache's byte budget, so that the eight have to be
    # rebuilt after it goes.
    show zc1 at zoomrow:
        xoffset 50
        xzoom 0.25
        yzoom 0.25
    show zc2 at zoomrow:
        xoffset 200
        xzoom 0.5
        yzoom 0.5
    show zc3 at zoomrow:
        xoffset 350
        xzoom 0.75
        yzoom 0.75
    show zc4 at zoomrow:
        xoffset 500
        xzoom 1.25
        yzoom 1.25
    show zc5 at zoomrow:
        xoffset 650
        xzoom 1.5
        yzoom 1.5
    show zc6 at zoomrow:
        xoffset 800
        xzoom 1.5
        yzoom 1.5
    show zc7 at zoomrow:
        xoffset 950
        xzoom 1.75
        yzoom 1.75
    show zc8 at zoomrow:
        xoffset 1100
        xzoom 2.0
        yzoom 2.0
    a "Eight at once."

    # 1280x1460 at 0.7 x 2.0 is 896x2920, which is 10MB of zoomed copy
    # against an 8MB budget. Shown last, so it covers the row.
    show bigtall:
        xalign 0.0
        yalign 0.0
        xzoom 0.7
        yzoom 2.0
    a "One huge one."

    hide bigtall
    a "Eight again."
    jump the_end

# im.FactorScale, for fscaletest. The registry half is asserted off the
# decoded surfaces; this label exists so that one of them is also shown, which
# is what says the scaled pixels are the ones that reach the stage.
label fscale_show:
    window hide
    scene dark
    a "Empty stage."
    show fs half at zoomspot
    a "Scaled by a half."
    show fs flip at zoomspot
    a "Scaled and flipped."
    jump the_end

# Dialogue over an animated background, for bandtest's second pass.
#
# Several lines rather than one, because the walk has to keep typing for long
# enough to cross many animation frame changes -- the question is whether a
# banded frame and a full frame agree *while* the background is moving, and
# one line would sample only a moment of that.
#
# No transition on the scene: `with dissolve` would make the opening frames
# transition frames, which are never banded, and the pass would spend its
# early iterations testing the path it is not about.
label anim_bg:
    window hide
    scene anim bg
    with None
    a "The background is animating while this line types itself out."
    a "And this one, which has to be long enough to span several frames."
    a "A third, because the animation's cycle is 700ms and a line is shorter."
    a "A fourth, to be sure the steady state is what is being measured."
    # Back to itself, not to the_end. the_end jumps to `start`, whose
    # backgrounds are static, so a walk long enough to be worth running spent
    # most of itself back in the static case -- 53 animation frame changes
    # over 900 frames where a 700ms cycle at 33ms should give about 127. The
    # loop keeps the animated background up for the whole pass, which is the
    # only state this label exists to provide.
    jump anim_bg

# Dialogue with a zorder-0 screen up inside the strip, for bandtest. See
# `screen band_screen` above for why that layer is the one that matters: the
# band cache must not keep a strip while such a screen is up, and whether it
# did is visible in mr_sl_hit rather than in any pixel.
#
# A static background, so this pass is about the screen and not about movement.
label band_with_screen:
    scene room
    with None
    show screen band_screen
    a "A line typing itself out with a clickable screen inside the strip."
    a "And a second, so the pass spends most of itself in the banded case."
    a "A third, long enough that the typing crosses a good many frames."
    jump band_with_screen

label the_end:
    # Back to the beginning rather than `return`. A harness that steps for a
    # while -- bandtest runs 1200 frames and then wants a line of dialogue to
    # measure -- must not run out of story and sit on an empty stage. The
    # real script never ran out because it is a whole game; this is how a
    # fixture gets the same property in one line.
    jump start

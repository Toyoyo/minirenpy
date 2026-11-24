# A scrolling credit roll, for the one bug it exposes.
#
# The shape is the game's own ending: an image given a timed transform that
# runs for several seconds, a hard pause covering it, and then a `with
# dissolve` onto something else. Nothing else in the harness fixtures has a
# transform that is still running -- or has only just finished -- when a
# transition starts, and that is exactly the case that broke.
#
# Images are the story fixture's flat colours, declared explicitly for the
# same reason as there: mr_vfs_enum only walks loose files on Windows, so a
# host build discovers nothing by scanning.

image roll = "images/street.png"
image fin = "images/person.png"
image room = "images/room.png"
image rolltall = "images/rolltall.png"

label start:
    scene room
    n "A line before the credits, so there is something committed."

    window hide

    show roll:
        ypos 720 yanchor 0.0 xalign 0.5
        linear 5 yanchor 1.0 ypos 0

    $ renpy.pause(5, hard=True)

    show fin
    with dissolve

    $ renpy.pause()

    n "After the end."
    jump start

# The same roll in the shape Miwa's own ending has, because `roll` above
# differs from it in three ways that each hide something:
#
#   - street.png is 1280x720. A roll image is *taller than the stage*:
#     Miwa's credits.png is 840x2140. A stage-sized image covers the screen at
#     the midpoint of the pan, so "is the roll on screen" reads true for most
#     of the run whatever the placement arithmetic does. rolltall.png matches
#     840x2140 exactly.
#   - street.png is opaque RGB. A credit roll is RGBA -- text over
#     transparency, composited onto whatever is behind. That takes a
#     different blit path (mr_blit consults has_alpha), and the opaque one
#     cannot fail the way the alpha one can.
#   - the `show` is at the top level of its label. Miwa's is nested inside
#     `if _preferences.language == None:`, so the transform block sits two
#     levels deep and the block is consumed relative to the *show's* indent,
#     not the label's. Nothing else in these fixtures has a transform block
#     inside a conditional.
#
# rolltall.png is 840x2140 RGBA from tools/mkfixtureimg.py -m hband: an
# opaque upper half and a half-transparent lower half, so which part of it is
# on screen is a single pixel read and the alpha is exercised.
label tallroll:
    scene room
    n "A line before the tall roll."

    window hide

    $ lang = None

    if lang == None:
        show rolltall:
            ypos 720 yanchor 0.0 xalign 0.5
            linear 5 yanchor 1.0 ypos 0

    if lang == "english":
        show rolltall:
            ypos 720 yanchor 0.0 xalign 0.5
            linear 5 yanchor 1.0 ypos 0

    $ renpy.pause(5, hard=True)

    show fin
    with dissolve

    $ renpy.pause()

    n "After the tall roll."
    jump tallroll

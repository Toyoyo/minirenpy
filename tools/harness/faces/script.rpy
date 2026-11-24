# A fixture for the second-face support: see gui.rpy and screens.rpy.
#
# The word is the same in every row on purpose. The test compares renderings
# of identical text at an identical size, so the only thing that can make two
# rows differ is which face was selected. "Hamburgefonstiv" is the usual
# type-design pangram for exactly this reason: it exercises the shapes that
# differ most between a serif and a sans.
#
# Row by row, and what each is for:
#
#   explicit ...... `font` written straight on the node
#   styled ........ the face reached through gui.interface_text_font
#   inherited ..... a style with no font of its own, parented to one that has
#   default ....... no font anywhere; must come out in gui.text_font
#   uncovered ..... a codepoint the named face does *not* have, which has to
#                   be found in the default atlas instead of not drawn at all
#
# The last pair is the one that justifies the whole fallback mechanism.
# DejaVuSerif has no U+2605 BLACK STAR and DejaVuSans does, exactly as Pecita
# has no kanji and NotoSansJP does, so the two star rows must come out
# identical: one asks the named face and gets the glyph through its fallback,
# the other asks the default face directly.

define c = Character("C")

screen faces():
    text "Hamburgefonstiv" size 26 xpos 0 ypos 0 \
        font gui.interface_text_font
    text "Hamburgefonstiv" size 26 xpos 0 ypos 40 \
        style "fixture_interface_text"
    text "Hamburgefonstiv" size 26 xpos 0 ypos 80 \
        style "fixture_label_text"
    text "Hamburgefonstiv" size 26 xpos 0 ypos 120 \
        style "fixture_default_text"
    text "★★★" size 26 xpos 0 ypos 160 \
        font gui.interface_text_font
    text "★★★" size 26 xpos 0 ypos 200 \
        style "fixture_default_text"

label start:
    c "A line, so the window exists."
    show screen faces
    c "And another, with the screen up."
    c "Done."
    return

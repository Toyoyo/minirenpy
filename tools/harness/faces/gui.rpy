## A fixture GUI that names a second font face.
##
## Hinata does this for real:
##
##     define gui.interface_text_font = "Pecita-0owz.otf"
##
## but Pecita is not something a harness can assume is installed, so this uses
## DejaVuSerif, which ships beside DejaVuSans everywhere DejaVu does. What
## matters for the test is only that the two faces are *visibly different*
## shapes at the same size -- a serif against a sans is about as different as
## two faces get, and a fixture where both faces rendered alike could not show
## the selection working at all.
##
## The commented-out line is deliberate: Hinata has three of these and only
## the last is live, and mkfont.py's scrape has to agree. A fixture that only
## had the live one could not tell a correct scrape from one that takes the
## first match.

define gui.text_font = "DejaVuSans.ttf"
#define gui.interface_text_font = "DejaVuSansMono.ttf"
define gui.interface_text_font = "DejaVuSerif.ttf"
define gui.name_text_font = "DejaVuSans.ttf"

define gui.text_size = 22
define gui.name_text_size = 30
define gui.interface_text_size = 26

## Inherited by button_text, as Ren'Py's own template does it. This is the
## path that makes a size reach a face without a `gui.<kind>_text_font` of its
## own, which is why mkfont.py bakes a named face at every size.
define gui.button_text_font = gui.interface_text_font
define gui.button_text_size = 26
define gui.quick_button_text_size = 14

## The per-device override block, in the older spelling: these sizes must
## *not* be baked. mkfont.py only understood the @gui.variant decorator at
## first, and Hinata uses this form, so four atlases it never read were baked,
## packed and loaded resident.
init python:

    if renpy.variant("small"):

        gui.text_size = 34
        gui.interface_text_size = 36

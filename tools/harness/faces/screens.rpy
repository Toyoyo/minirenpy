## The three ways a `text` can end up with a face, kept apart so a test can
## tell which one broke.
##
## This is the shape Ren'Py's own template has, trimmed to what the test
## needs: `gui.text_properties(kind)` sets the `font` property only when
## `gui.<kind>_text_font` exists, and there is no fallback to
## gui.interface_text_font (00gui.rpy:396). So `fixture_label_text` below gets
## no font of its own and has to inherit one from its parent -- which is the
## case the engine has to get right for Hinata's quick buttons.

style fixture_default_text:
    properties gui.text_properties()

style fixture_interface_text:
    properties gui.text_properties("interface")

style fixture_button_text:
    properties gui.text_properties("button")

style fixture_label_text is fixture_button_text:
    properties gui.text_properties("label")

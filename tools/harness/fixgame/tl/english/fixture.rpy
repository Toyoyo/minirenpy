# The fixture's English translation: the other direction from tl/None.
#
# The fixture is treated as a French-written game (see tl/None/fixture.rpym
# for why), so this is the language a player switches *into*, and the keys
# here are French because Ren'Py keys a string translation on the text the
# script wrote.
#
# Two kinds of entry:
#
#   * the rhythm minigame's strings, whose keys really are French literals
#     in the engine -- RG_DLG_TEXT and friends, src/rhythm.c:1307 -- because
#     the minigame module they came from is French. rgtest switches to this
#     language and asserts the dialog redraws differently and that its box
#     is resized to the new text, so these have to be a different length
#     from the French.
#
#   * nothing else, deliberately. Ren'Py's own strings are keyed in English
#     already, so under this language they fall through to the key and come
#     out English, which is the correct answer and is what makes "switching
#     language changes the engine's strings" observable: tl/None turns them
#     French, this turns them back.

translate english strings:

    # The give-up dialog: src/rhythm.c:1307-1309.
    old "Quitter le jeu de rythme ?"
    new "Quit the rhythm game?"

    old "Oui"
    new "Yes"

    old "Non"
    new "No"

    # The end-of-run text: src/rhythm.c:1272-1284.
    old "Full combo !"
    new "Full combo!"

    old "Nouveau record !"
    new "New record!"

    old "Appuyez sur Espace ou cliquez pour continuer."
    new "Press Space or click to continue."

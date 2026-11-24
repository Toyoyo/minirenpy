# script_game.rpy -- borrowed content, kept apart from the invented kind.
#
# The parser reads a fixed list of filenames (parse.c, g_script_files), of
# which this is the last, so an extra fixture file has to be one of those
# names rather than any name. Everything here is copied verbatim from a
# real game and the verbatim-ness is load-bearing; script.rpy next door is
# invented and free to change.
#
# Real lines from a real script, here for one reason: oldsavetest's save
# fixtures name their execution point by statement id.
#
# A save does not record "line 3190". It records the *translation
# identifier* of the statement the player was on -- `chap6_83afebf2` --
# which is the label plus an md5 of the reconstructed source line (tl.c,
# mr_tl_identifier, after Ren'Py's ast.py:941). On load, that id is looked
# up in the script that is mounted now. If it is not there, mr_save_load
# refuses the slot: the desynced-save guard, and correct -- resuming at a
# guessed index is the thing it exists to prevent.
#
# So a save fixture can only load against a script that contains the
# statements it names. The fixtures in tools/harness/fixtures are version
# 5, 6 and 7 files captured from real builds, and they cannot be
# regenerated for a made-up script: writing them by hand would only prove
# the reader agrees with the test's idea of the old layout, which is the
# thing in doubt. The captured bytes stay; what moves into the fixture is
# the handful of statements they point at.
#
# Hence: these twenty lines are copied verbatim from "Miwa no Utsuroi",
# labels and speaker and text exactly as written there, because the id is
# a hash of exactly that. Change a character -- a space, an accent, the
# speaker tag -- and the id changes and oldsavetest goes red on "it
# loads". They are the only lines in this fixture that are somebody's
# story rather than invented, and they are here as *hash preimages*, not
# as content: nothing jumps to these labels and nothing reads them.
#
# The three fixtures resume in two of them:
#   v5-slot.sav, v6-slot.sav -> chap0   (Miwa's script.rpy:22-28)
#   v7-slot.sav              -> chap6   (Miwa's script.rpy:3151-3210)
# The other ids each fixture carries are its rollback log, which is read
# through the same table.

define m = Character("Miwa")

label chap0:
    m "La journée de cours est terminée, les devoirs sont faits, je peux enfin me laisser aller."
    m "Je suis assise dans ma chambre, le vieil instrument de mon père sur les genoux."
    m "Je joue."
    return

label chap6:
    m "Je ris un peu."
    m "\"Merci maman.\""
    m "Je rejoins les autres élèves qui attendent leur tour, tandis que les visiteurs prennent place dans la salle."
    m "Je pose mon biwa près de moi et regarde la scène."
    m "Quelques élèves parlent à voix basse."
    m "D'autres vérifient une dernière fois leurs affaires."
    m "J'écoute distraitement les prestations qui défilent."
    m "Une voix annonce la prochaine."
    m "Je prends mon instrument et serre mon plectre dans ma main."
    m "C'est à moi."
    m "Je monte sur scène."
    m "Je regarde les personnes assises dans le public."
    m "Je sais que maman et Soraha sont quelque part devant moi, mais je ne les cherche pas."
    m "Je m'assieds sur le tapis rouge et pose mon biwa contre moi."
    m "Mon cœur bat beaucoup trop vite, alors je prends une grande inspiration."
    m "Puis une autre."
    m "Mon ventre se calme légèrement et je laisse maman et Soraha derrière moi puis entre dans l'auditorium."
    return

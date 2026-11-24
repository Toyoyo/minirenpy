# The menu from the request, verbatim, plus what is needed to reach it and
# to observe the result.
define m = Character("M")

label start:
    $ chap7 = False
    scene room
    m "Choose."
    menu:
        "Chap1":
            jump chap1
        "Chap2":
            jump chap2
        "Chap3":
            jump chap3
        "Chap4":
            jump chap4
        "Chap5":
            jump chap5
        "Chap6":
            jump chap6
        "Chap7":
            $ chap7=True
            jump chap7

label chap1:
    $ landed = 1
    jump after
label chap2:
    $ landed = 2
    jump after
label chap3:
    $ landed = 3
    jump after
label chap4:
    $ landed = 4
    jump after
label chap5:
    $ landed = 5
    jump after
label chap6:
    $ landed = 6
    jump after
label chap7:
    $ landed = 7
    jump after

label after:
    if chap7 == True:
        m "We jumped"
    else:
        m "No chap7"
    return

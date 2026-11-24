# Note: the terms onset and beatmap are used interchangeably in this script
#
# Visuals and scoring ported back from the C (Win32s) version of the game:
#  - white track lines, hit-zone bar that fills up as the song progresses
#  - one colour per track, notes grow when they enter the hit window
#  - score (10 + combo per note) and combo system, combo lost on a missed note

define THIS_PATH = '00-renpythm/'

# XXX: using os.path.join here will actually break because Ren'Py somehow doesn't recognize it
define IMG_DIR = 'images/'
define IMG_UP = THIS_PATH + IMG_DIR + 'note_up.png'
define IMG_LEFT = THIS_PATH + IMG_DIR + 'note_left.png'
define IMG_RIGHT = THIS_PATH + IMG_DIR + 'note_right.png'
define IMG_DOWN = THIS_PATH + IMG_DIR + 'note_down.png'
# quit button, for touch screens which have no Escape key
define IMG_QUIT_IDLE = THIS_PATH + IMG_DIR + 'quit_idle.png'
define IMG_QUIT_HOVER = THIS_PATH + IMG_DIR + 'quit_hover.png'

# sound played when a note is hit (the C version dings a MIDI cymbal)
# one note per track, left to right: do, mi, sol, la (C3, E3, G3, A3), all
# resampled from 'audio/fx corde.mp3'
define RHYTHM_HIT_SOUND = [
    'audio/fx corde do.mp3',
    'audio/fx corde mi.mp3',
    'audio/fx corde sol.mp3',
    'audio/fx corde la.mp3',
    ]
# health bar: every missed note costs RHYTHM_MISS_DAMAGE, and the run is over
# when it reaches zero (20 missed notes with these values)
define RHYTHM_MAX_HEALTH = 100
define RHYTHM_MISS_DAMAGE = 5

# a hit landing within this many seconds of the onset is a PERFECT and scores
# RHYTHM_PERFECT_BONUS times as much; anything else inside hit_threshold is a
# GREAT. There is no life bar here, so there is nothing below GREAT: a note is
# either hit or missed.
define RHYTHM_PERFECT_WINDOW = 0.2
define RHYTHM_PERFECT_BONUS = 1.5

# how long a note takes to cross the screen, in seconds: this is both the
# scrolling speed and the reading time, and it sets how many notes are on
# screen at once. At 3.0 a third of the notes of a track visually overlapped
# the next one; 2.0 brings that down to a tenth.
define RHYTHM_NOTE_OFFSET = 2.0

# a hit sound is ~0.4s long and notes can follow each other faster than that,
# so the hits are spread over several channels instead of cutting each other
define RHYTHM_HIT_CHANNELS = ['rhythm_hit_0', 'rhythm_hit_1', 'rhythm_hit_2', 'rhythm_hit_3']

init python:
    for _rhythm_channel in RHYTHM_HIT_CHANNELS:
        renpy.music.register_channel(_rhythm_channel, mixer='sfx', loop=False)

style score:
    outlines [ (absolute(1), "#000", absolute(0), absolute(0)) ]

# High scores, keyed by beatmap file name: {'foo.beatmap.txt': 1234}
# `default` only creates it the first time, so existing persistent data is kept.
default persistent.rhythm_game_highscores = {}

init -1 python:

    def rhythm_game_score_key(beatmap_path):
        # only the bare file name is stored, so moving the files around or
        # renaming a directory doesn't lose the high scores
        return beatmap_path.replace('\\', '/').rsplit('/', 1)[-1]

    class RhythmGameRandom(object):
        """The C version's PRNG, reproduced bit for bit.

        Track assignment is seeded with the beatmap name, so a given song
        always has the same arrow pattern: patterns can be learnt and the high
        scores of two runs are comparable.
        """

        def __init__(self, seed_string):
            # djb2 hash, as rg_hash_string() does
            h = 5381
            for char in seed_string.encode('utf-8'):
                h = (h * 33 + char) & 0xFFFFFFFF
            self.seed = h

        def randint(self, lo, hi):
            # rg_rand(): 32 bit LCG, top bits used for the modulo
            self.seed = (self.seed * 1103515245 + 12345) & 0xFFFFFFFF
            return lo + ((self.seed >> 16) % (hi - lo + 1))

    def rhythm_game_song_sounds(beatmap_path, num_tracks):
        """Hit sounds tuned to one song, or None when they weren't generated.

        generate_beatmap-pentatonique.py writes 'foo.hit0.wav'..'foo.hit3.wav'
        next to 'foo.beatmap.txt'; all of them must be there to be used.
        """
        base = beatmap_path
        if base.endswith('.beatmap.txt'):
            base = base[:-len('.beatmap.txt')]
        sounds = ['%s.hit%d.wav' % (base, track_idx) for track_idx in range(num_tracks)]
        if all(renpy.loadable(sound) for sound in sounds):
            return sounds
        return None

    def rhythm_game_get_highscore(beatmap_path):
        # 0 when the song has never been played (or persistent data is missing)
        scores = persistent.rhythm_game_highscores
        if not scores:
            return 0
        return scores.get(rhythm_game_score_key(beatmap_path), 0)

    def rhythm_game_set_highscore(beatmap_path, score):
        # returns True when this run is a new record
        scores = dict(persistent.rhythm_game_highscores or {})
        key = rhythm_game_score_key(beatmap_path)
        if score <= scores.get(key, 0):
            return False
        scores[key] = score
        # rebind so Ren'Py always notices the change
        persistent.rhythm_game_highscores = scores
        return True

# screen definition
screen rhythm_game(audio_path, beatmap_path, background_path, beatmap_stride=None, hit_sound=None):
    # audio_path (str): file path relative to renpy.config.gamedir
    # hit_sound (str or list of one file name per track): defaults to
    # RHYTHM_HIT_SOUND, None-safe if a file is missing
    default rhythm_game_displayable = RhythmGameDisplayable(
        audio_path, beatmap_path, beatmap_stride=beatmap_stride, hit_sound=hit_sound)

    add background_path
    add rhythm_game_displayable

    # Escape (and whatever else is bound to the game menu) pauses the song and
    # asks whether the player wants to give up
    key "game_menu" action Function(rhythm_game_displayable.pause_game)
    key "K_ESCAPE" action Function(rhythm_game_displayable.pause_game)

    # same thing as Escape, for touch screens; hidden during the countdown and
    # once the song is over, like the rest of the HUD
    showif not rhythm_game_displayable.is_paused and not rhythm_game_displayable.has_ended and rhythm_game_displayable.countdown_text is None:
        imagebutton:
            align (1.0, 0.0)
            xoffset -10
            yoffset 10
            idle IMG_QUIT_IDLE
            hover IMG_QUIT_HOVER
            focus_mask None
            action Function(rhythm_game_displayable.pause_game)

    if rhythm_game_displayable.is_paused:
        use rhythm_game_quit_confirm(
            yes_action=[
                Function(rhythm_game_displayable.quit_game),
                Return(rhythm_game_displayable.result())],
            no_action=Function(rhythm_game_displayable.resume_game))

    # the end of song summary stays up until the player dismisses it, so there
    # is time to read it
    default end_ready = False

    if rhythm_game_displayable.has_ended:
        # short grace period: the last note is often tapped right as the song
        # ends, and that tap must not dismiss the summary straight away
        timer 0.8 action SetScreenVariable('end_ready', True)

        if end_ready:
            key "K_SPACE" action Return(
                rhythm_game_displayable.result())
            key "K_RETURN" action Return(
                rhythm_game_displayable.result())
            # any click or tap not caught by something else
            dismiss action Return(
                rhythm_game_displayable.result())

            text _("Appuyez sur Espace ou cliquez pour continuer."):
                align (0.5, 0.68)
                color '#fff'
                style "score"
                size 28

    showif rhythm_game_displayable.is_game_over:
        text _("GAME OVER"):
            align (0.5, 0.35)
            color '#ff8080'
            style "score"
            size 72

    # End of song banners. They can both happen on the same run, so they get
    # their own line.
    showif rhythm_game_displayable.is_full_combo:
        text _("Full combo !"):
            align (0.5, 0.33)
            color '#43d35a'
            style "score"
            size 48

    showif rhythm_game_displayable.is_new_highscore:
        text _("Nouveau record !"):
            align (0.5, 0.45)
            color '#ffc882'
            style "score"
            size 48

    showif rhythm_game_displayable.has_ended:
        text ('PERFECT %d    GREAT %d' % (
                rhythm_game_displayable.num_perfect,
                rhythm_game_displayable.num_great)):
            align (0.5, 0.57)
            color '#fff'
            style "score"
            size 32

    # health bar, top left, with the score to its right
    showif rhythm_game_displayable.has_started and rhythm_game_displayable.countdown_text is None:
        fixed:
            xpos 10
            ypos 10
            xysize (228, 26)
            add Solid('#000000a0')
            # a zero-width Solid would be pointless, and the bar is empty anyway
            if rhythm_game_displayable.health > 0:
                add Solid(rhythm_game_displayable.health_color()):
                    xysize (int(224 * rhythm_game_displayable.health_fraction()), 22)
                    xpos 2
                    ypos 2

    showif rhythm_game_displayable.has_started and rhythm_game_displayable.countdown_text is None:
        vbox xpos 250 ypos 5 spacing 2:
            text 'Score: ' + str(rhythm_game_displayable.score):
                color '#fff'
                style "score"
                size 28
            showif rhythm_game_displayable.combo > 1:
                text ('Combo x%d (+%d%%)' % (
                        rhythm_game_displayable.combo,
                        rhythm_game_displayable.combo * 10)):
                    color '#ffc882'
                    style "score"
                    size 28

# Confirmation dialog shown when the player presses Escape during the song.
# All the strings go through _() so they are picked up by Ren'Py's translation
# system (see "Generate Translations" in the launcher).
screen rhythm_game_quit_confirm(yes_action, no_action):
    modal True
    zorder 200

    add Solid('#000000a0')

    frame:
        align (0.5, 0.5)
        padding (40, 30)

        vbox:
            spacing 30
            xalign 0.5

            text _("Quitter le jeu de rythme ?"):
                xalign 0.5
                text_align 0.5

            hbox:
                spacing 40
                xalign 0.5

                textbutton _("Oui") action yes_action
                textbutton _("Non") action no_action

    # Escape / Enter as shortcuts for the two answers
    key "game_menu" action no_action
    key "K_ESCAPE" action no_action

init python:
    import pygame

    class RhythmGameDisplayable(renpy.Displayable):

        def __init__(self, audio_path, beatmap_path, beatmap_stride=None, hit_sound=None):
            """
            beatmap_stride (int): Default to 2. Use onset_times[::beatmap_stride] so that the tracks don't get too crowded. Can be used to set difficulty level
            """
            super(RhythmGameDisplayable, self).__init__()

            self.audio_path = audio_path
            self.beatmap_path = beatmap_path

            # the hit sounds themselves are resolved further down, once
            # num_track_bars is known
            self.hit_sound_channel = 0
            # set when the finished run beat the stored high score
            self.is_new_highscore = False
            # set when the finished run didn't miss a single note
            self.is_full_combo = False

            self.has_started = False
            self.has_ended = False
            # game time the run ended at, so the progress bar stops there
            self.end_time = None
            # health, lost on missed notes; the run ends when it hits zero
            self.max_health = RHYTHM_MAX_HEALTH
            self.health = RHYTHM_MAX_HEALTH
            self.is_game_over = False

            # pausing: st keeps running while the confirmation dialog is up, so
            # the elapsed paused time is accumulated and subtracted from st
            self.is_paused = False
            self.pause_offset = 0.0
            self.pause_start_st = None
            self.resume_requested = False

            # zoom the note when it is within the hit threshold
            self.zoom_scale = 1.25

            # offset for rendering
            # leave some offset from the left side of the screen
            self.x_offset = 100
            self.track_width = 4
            # hit-zone bar sits near the very bottom of the screen
            self.horizontal_bar_height = 28
            self.track_bar_height = config.screen_height - self.horizontal_bar_height - 10
            self.note_width = 108 # width of the note image
            self.note_xoffset = -self.note_width / 2
            self.note_xoffset_large = -self.note_width * self.zoom_scale / 2

            # note appear on the tracks prior to the actual onset
            # which is also the note's entire lifetime to travel the entire screen
            # can be used to set difficulty level of the game
            self.note_offset = RHYTHM_NOTE_OFFSET # seconds

            # an offset is necessary because there might be a delay between when the
            # displayable first appears on screen and the time the music starts playing
            # seconds, same unit as st, shown time
            self.time_offset = None

            # number of track bars
            self.num_track_bars = 4

            # one hit sound per track (do, mi, sol, la from left to right), or a
            # single file name used by every track
            sounds = hit_sound
            if sounds is None:
                # sounds tuned to this very song, if the beatmap generator made
                # them ('foo.beatmap.txt' -> 'foo.hit0.wav'..'foo.hit3.wav')
                sounds = rhythm_game_song_sounds(beatmap_path, self.num_track_bars)
            if sounds is None:
                sounds = RHYTHM_HIT_SOUND
            if not isinstance(sounds, (list, tuple)):
                sounds = [sounds] * self.num_track_bars
            # a missing file simply means no sound, rather than a crash
            self.hit_sounds = [
            sound if sound and renpy.loadable(sound) else None
            for sound in list(sounds)[:self.num_track_bars]
            ]
            # pad, in case fewer sounds than tracks were given
            self.hit_sounds += [None] * (self.num_track_bars - len(self.hit_sounds))

            # count down before the notes start falling
            self.countdown = 3.0
            self.countdown_text = '3'
            # silence before the music plays: the countdown, then exactly one
            # note lifetime, so the first note reaches the hit-zone bar at the
            # very moment the music starts (this is what the C version does)
            self.silence_offset_start = self.countdown + self.note_offset
            self.silence_start = '<silence %s>' % str(self.silence_offset_start)

            # onset timestamps in the beatmap file given audio file, plus the
            # track of each onset when the beatmap carries them
            onset_times, onset_tracks = self.read_beatmap_file(beatmap_path)
            # take strides throught onset_times so that the tracks don't get too crowded
            if beatmap_stride is None:
                beatmap_stride = 2
            self.onset_times = onset_times[::beatmap_stride]

            # whether an onset been hit determines whether it will be rendered
            # 0 = pending, 1 = hit, -1 = missed
            self.onset_status = [0 for _ in self.onset_times]
            self.num_notes = len(self.onset_times)
            self.song_duration = self.onset_times[-1] if self.num_notes else 1.0
            # assign tracks in advance since generating on the fly is too slow
            if onset_tracks is not None:
                # the beatmap follows the music: keep its tracks
                self.random_track_indices = onset_tracks[::beatmap_stride]
            else:
                # old format: the PRNG is seeded with the beatmap name, so the
                # pattern is the same on every run of a given song (as in C)
                track_random = RhythmGameRandom(rhythm_game_score_key(beatmap_path))
                self.random_track_indices = [
                track_random.randint(0, self.num_track_bars - 1) for _ in range(self.num_notes)
                ]

            # a list of active note indices on each track
            self.active_notes_per_track = {track_idx: [] for track_idx in range(self.num_track_bars)}

            # track number of hits for scoring
            self.num_hits = 0
            self.score = 0
            self.combo = 0
            self.full_combo = True

            # the threshold for declaring a note as active when computing onset - (st - self.time_offset)
            self.time_difference_threshold = 0.01
            # the threshold for considering a note as hit
            self.hit_threshold = 0.3
            # inside this tighter window the hit is a PERFECT
            self.perfect_threshold = RHYTHM_PERFECT_WINDOW
            # judgement shown for a short while after each hit
            self.judgement = None
            self.judgement_at = None
            self.judgement_duration = 0.4
            # dissolve in and out over this long, at each end
            self.judgement_fade = 0.1
            # how many of each, for the end of song summary
            self.num_perfect = 0
            self.num_great = 0

            # map pygame key code to track idx
            # the alternate keys are the ones the C version accepts: A/S for the
            # left and up tracks, numpad 1/2 for the down and right ones
            # Q is accepted too: these are SDL keycodes, which follow the layout,
            # so on an AZERTY keyboard Q sits where A is on a QWERTY one (S is at
            # the same physical spot on both layouts, hence no alias for it)
            self.keycode_to_track_idx = {}
            alternate_keys = {
            0: ('K_LEFT', 'K_a', 'K_q'),
            1: ('K_UP', 'K_s'),
            2: ('K_DOWN', 'K_KP1', 'K_KP_1'),
            3: ('K_RIGHT', 'K_KP2', 'K_KP_2'),
            }
            for track_idx, key_names in alternate_keys.items():
                for key_name in key_names:
                    # the numpad constants aren't spelled the same in every
                    # pygame / pygame_sdl2 version, so only keep what exists
                    keycode = getattr(pygame, key_name, None)
                    if keycode is not None:
                        self.keycode_to_track_idx[keycode] = track_idx

            # drawables
            self.countdown_drawables = {
            text: Text(text, color='#fff', size=120,
                outlines=[(absolute(2), '#000', absolute(0), absolute(0))])
            for text in ('3', '2', '1')
            }

            # PERFECT / GREAT flashed above the hit-zone bar after each hit
            # (kept in English, as rhythm games usually do)
            self.judgement_drawables = {
            'perfect': Text('PERFECT', color='#ffc882', size=44,
                outlines=[(absolute(2), '#000', absolute(0), absolute(0))]),
            'great': Text('GREAT', color='#4dc8ff', size=44,
                outlines=[(absolute(2), '#000', absolute(0), absolute(0))]),
            }

            self.track_bar_drawable = Solid('#fff', xsize=self.track_width, ysize=config.screen_height)
            # hit-zone bar, unfilled part and the part already played
            self.bar_width = (config.screen_width - self.x_offset * 2) + self.track_width
            self.horizontal_bar_drawable = Solid('#4dc8ff',
                xsize=self.bar_width, ysize=self.horizontal_bar_height)
            # the filled part is drawn cropped to the current progress
            self.horizontal_bar_filled = Solid('#2a6e99',
                xsize=self.bar_width, ysize=self.horizontal_bar_height)

            self.note_drawables = {
            0: Image(IMG_LEFT),
            1: Image(IMG_UP),
            2: Image(IMG_DOWN),
            3: Image(IMG_RIGHT),
            }

            self.note_drawables_large = {
            0: Transform(self.note_drawables[0], zoom=self.zoom_scale),
            1: Transform(self.note_drawables[1], zoom=self.zoom_scale),
            2: Transform(self.note_drawables[2], zoom=self.zoom_scale),
            3: Transform(self.note_drawables[3], zoom=self.zoom_scale),
            }

            # for self.visit method
            self.drawables = [
            self.track_bar_drawable,
            self.horizontal_bar_drawable,
            self.horizontal_bar_filled,
            ]
            self.drawables.extend(list(self.countdown_drawables.values()))
            self.drawables.extend(list(self.judgement_drawables.values()))
            self.drawables.extend(list(self.note_drawables.values()))
            self.drawables.extend(list(self.note_drawables_large.values()))

            # variables for drawing positions
            self.track_bar_spacing = (config.screen_width - self.x_offset * 2) / (self.num_track_bars - 1)
            # cache computation for looking up on the fly for better performance
            self.track_xoffsets = {
            track_idx: self.x_offset + track_idx * self.track_bar_spacing
            for track_idx in range(self.num_track_bars)
            }
            # the y coordinate the notes travel to, middle of the hit-zone bar
            self.bar_mid_y = self.track_bar_height + self.horizontal_bar_height / 2
            self.note_travel = self.bar_mid_y + self.note_width / 2

            # start playing music
            self.play_music()

        def play_hit_sound(self, track_idx):
            sound = self.hit_sounds[track_idx]
            if not sound:
                return
            channel = RHYTHM_HIT_CHANNELS[self.hit_sound_channel]
            self.hit_sound_channel = (self.hit_sound_channel + 1) % len(RHYTHM_HIT_CHANNELS)
            renpy.sound.play(sound, channel=channel)

        def pause_game(self):
            if self.has_ended or self.is_paused:
                return
            self.is_paused = True
            self.pause_start_st = None
            renpy.music.set_pause(True, channel='music')
            renpy.restart_interaction()

        def resume_game(self):
            if not self.is_paused:
                return
            self.is_paused = False
            # the paused duration is measured in render(), which has st
            self.resume_requested = True
            renpy.music.set_pause(False, channel='music')
            renpy.restart_interaction()

        def quit_game(self):
            self.is_paused = False
            self.has_ended = True
            renpy.music.set_pause(False, channel='music')
            renpy.music.stop(channel='music')
            # don't let a hit sound ring on into the menu
            for channel in RHYTHM_HIT_CHANNELS:
                renpy.sound.stop(channel=channel)

        def render(self, width, height, st, at):
            # freeze the game clock while the quit dialog is up
            if self.is_paused:
                if self.pause_start_st is None:
                    self.pause_start_st = st
                st = self.pause_start_st - self.pause_offset
            else:
                if self.resume_requested:
                    if self.pause_start_st is not None:
                        self.pause_offset += st - self.pause_start_st
                    self.pause_start_st = None
                    self.resume_requested = False
                st -= self.pause_offset

            # cache the shown time offset
            if self.has_started and self.time_offset is None:
                self.time_offset = self.silence_offset_start + st

            render = renpy.Render(width, height)

            # count down silence_offset_start, 3 seconds, while silence
            countdown_text = None
            time_before_music = self.countdown - st
            if time_before_music > 2.0:
                countdown_text = '3'
            elif time_before_music > 1.0:
                countdown_text = '2'
            elif time_before_music > 0.0:
                countdown_text = '1'
            if countdown_text != self.countdown_text:
                self.countdown_text = countdown_text
                renpy.restart_interaction() # show/hide the score overlay
            if countdown_text is not None:
                # only the big centered digit is shown during the countdown
                drawable = self.countdown_drawables[countdown_text]
                dw, dh = renpy.render(drawable, width, height, st, at).get_size()
                render.place(drawable,
                    x=(config.screen_width - dw) / 2, y=(config.screen_height - dh) / 2)
                renpy.redraw(self, 0)
                return render

            # draw the tracks
            for track_idx in range(self.num_track_bars):
                x_offset = self.track_xoffsets[track_idx] # look up to save computation
                render.place(self.track_bar_drawable, x=x_offset - self.track_width / 2, y=0)

            curr_time = st - self.time_offset if self.time_offset is not None else 0.0

            # once the run is over the clock stops: the progress bar below must
            # not keep filling up while the summary is on screen
            if self.has_ended:
                if self.end_time is None:
                    self.end_time = curr_time
                curr_time = self.end_time

            # hit-zone bar spanning all the tracks, fills up as the song progresses
            bar_x = self.track_xoffsets[0] - self.track_width / 2
            progress = max(0.0, min(1.0, curr_time / self.song_duration))
            filled_width = int(progress * self.bar_width)
            render.place(self.horizontal_bar_drawable, x=bar_x, y=self.track_bar_height)
            if filled_width > 0:
                filled = renpy.render(self.horizontal_bar_filled,
                    filled_width, self.horizontal_bar_height, st, at)
                render.blit(filled.subsurface(
                    (0, 0, filled_width, self.horizontal_bar_height)),
                    (int(bar_x), int(self.track_bar_height)))

            if self.has_started:
                # the run is over (song finished, or game over): nothing left
                # to draw, the screen shows the summary
                if self.has_ended:
                    renpy.timeout(0) # raise event
                    renpy.redraw(self, 0)
                    return render

                # the song played to the end: record the run (never while
                # paused, the channel may report nothing playing then)
                if not self.is_paused and renpy.music.get_playing() is None:
                    self.has_ended = True
                    self.is_new_highscore = rhythm_game_set_highscore(
                        self.beatmap_path, self.score)
                    # full combo: the song went to the end without a single
                    # missed note (g_fullcombo in the C version)
                    self.is_full_combo = self.full_combo and self.num_hits > 0
                    # the screen has to be re-evaluated right now for the
                    # summary to show up: nothing else would trigger it,
                    # since the player has no reason to touch anything
                    renpy.restart_interaction()
                    renpy.timeout(0) # raise event
                    renpy.redraw(self, 0)
                    return render

                # a note that went past the hit window without being hit breaks the combo
                if self.detect_misses(curr_time):
                    renpy.restart_interaction()

                # draw notes
                self.active_notes_per_track = self.get_active_notes(st)
                for track_idx in self.active_notes_per_track:
                    x_offset = self.track_xoffsets[track_idx] # look up to save computation

                    for note_idx, time_before_onset in self.active_notes_per_track[track_idx]:
                        if self.onset_status[note_idx] != 0: # already hit or missed
                            continue
                        # enlarge the note if it is now within the hit threshold
                        if abs(time_before_onset) <= self.hit_threshold:
                            note_drawable = self.note_drawables_large[track_idx]
                            note_xoffset = x_offset + self.note_xoffset_large
                        else:
                            note_drawable = self.note_drawables[track_idx]
                            note_xoffset = x_offset + self.note_xoffset

                        # 1 = just appeared at the top, 0 = at the hit-zone bar
                        frac = time_before_onset / self.note_offset
                        y_center = self.bar_mid_y - frac * self.note_travel
                        render.place(note_drawable,
                            x=note_xoffset, y=y_center - self.note_width / 2)

                # PERFECT / GREAT, fading in and out on its own
                if self.judgement is not None:
                    elapsed = curr_time - self.judgement_at
                    if elapsed > self.judgement_duration:
                        self.judgement = None
                    else:
                        drawable = self.judgement_drawables[self.judgement]
                        jw, jh = renpy.render(drawable, width, height, st, at).get_size()
                        # short dissolve at both ends, so the text doesn't pop
                        fade = self.judgement_fade
                        alpha = min(elapsed / fade,
                                    (self.judgement_duration - elapsed) / fade, 1.0)
                        render.place(Transform(drawable, alpha=max(0.0, alpha)),
                            x=(config.screen_width - jw) / 2,
                            y=self.track_bar_height - jh - 20)

            renpy.redraw(self, 0)
            return render

        def event(self, ev, x, y, st):
            if self.has_ended:
                # the summary is up: let the screen handle the events, it is
                # what dismisses it (render() already forced a refresh through
                # renpy.timeout when the song ended)
                return
            if self.is_paused or self.time_offset is None or self.countdown_text is not None:
                return
            curr_time = st - self.pause_offset - self.time_offset

            if ev.type == pygame.KEYDOWN:
                if not ev.key in self.keycode_to_track_idx:
                    return
                self.try_hit(self.keycode_to_track_idx[ev.key], curr_time)

            # a click / touch on a track counts as pressing that track's key, so
            # the game is playable with a mouse and on a touch screen
            elif ev.type == pygame.MOUSEBUTTONDOWN and getattr(ev, 'button', 1) == 1:
                track_idx = self.track_at(x)
                if track_idx is not None:
                    self.try_hit(track_idx, curr_time)

        def track_at(self, x):
            # nearest track, as long as the click is inside its half-lane
            track_idx = int(round(
                (x - self.x_offset) / float(self.track_bar_spacing)))
            if track_idx < 0 or track_idx >= self.num_track_bars:
                return None
            if abs(x - self.track_xoffsets[track_idx]) > self.track_bar_spacing / 2:
                return None
            return track_idx

        def try_hit(self, track_idx, curr_time):
            # validates the first pending note of the track that is inside the
            # hit window; same window as the keyboard, only the input differs
            for note_idx, _ in self.active_notes_per_track[track_idx]:
                if self.onset_status[note_idx] != 0:
                    continue
                # time when player attempts to hit the note
                # difference between the time the note is hittable and actually hit
                delta = abs(curr_time - self.onset_times[note_idx])
                if delta <= self.hit_threshold:
                    self.onset_status[note_idx] = 1
                    self.num_hits += 1
                    # a longer combo makes every note worth more, and landing
                    # close to the beat is worth more still
                    points = 10 + self.combo
                    if delta <= self.perfect_threshold:
                        points = int(round(points * RHYTHM_PERFECT_BONUS))
                        self.num_perfect += 1
                        self.judgement = 'perfect'
                    else:
                        self.num_great += 1
                        self.judgement = 'great'
                    self.judgement_at = curr_time
                    self.score += points
                    self.combo += 1
                    self.play_hit_sound(track_idx)
                    renpy.redraw(self, 0)
                    renpy.restart_interaction() # force refresh the screen for score to show
                    return True
            return False

        def detect_misses(self, curr_time):
            # returns True if at least one note was just missed
            missed = False
            for note_idx, onset in enumerate(self.onset_times):
                if self.onset_status[note_idx] == 0 and curr_time > onset + self.hit_threshold:
                    self.onset_status[note_idx] = -1
                    self.combo = 0
                    self.full_combo = False
                    self.health = max(0, self.health - RHYTHM_MISS_DAMAGE)
                    missed = True
                elif onset > curr_time:
                    break
            if missed and self.health <= 0 and not self.is_game_over:
                self.game_over()
            return missed

        def game_over(self):
            self.is_game_over = True
            # ends the run then and there: no high score, no full combo, the
            # one-shot block in render() is guarded by has_ended
            self.has_ended = True
            renpy.music.stop(channel='music', fadeout=0.5)
            renpy.restart_interaction()

        def health_fraction(self):
            return self.health / float(self.max_health)

        def health_color(self):
            # green, then amber, then red as the run gets dangerous
            fraction = self.health_fraction()
            if fraction > 0.5:
                return '#43d35a'
            if fraction > 0.25:
                return '#ffc882'
            return '#ff8080'

        def result(self):
            # what the screen returns to the script: (hits, notes, score).
            # A game over reports zeroes, so the script can tell a lost run
            # from a finished one; giving up through the Escape dialog keeps
            # whatever was achieved up to that point.
            if self.is_game_over:
                return (0, self.num_notes, 0)
            return (self.num_hits, self.num_notes, self.score)

        def visit(self):
            # visit all drawables
            return self.drawables

        def play_music(self):
            renpy.music.queue([self.silence_start, self.audio_path], loop=False)
            self.has_started = True

        def get_active_notes(self, st):
            active_notes = {track_idx: [] for track_idx in range(self.num_track_bars)}
            curr_time = st - self.time_offset
            for note_idx, (onset, track_idx) in enumerate(zip(self.onset_times, self.random_track_indices)):
                # determine if this note is active
                time_before_appearance = onset - curr_time
                # a note stays active until it leaves the hit window, so that a
                # late tap counts just as much as an early one; it is dropped
                # at the very moment detect_misses() declares it missed
                if time_before_appearance < -self.hit_threshold:
                    continue
                # should be on screen already
                elif time_before_appearance <= self.note_offset:
                    active_notes[track_idx].append((note_idx, time_before_appearance))
                # within threshold
                elif abs(time_before_appearance - self.note_offset) < self.time_difference_threshold:
                    active_notes[track_idx].append((note_idx, time_before_appearance))
                elif time_before_appearance > self.note_offset: # still time before it should appear
                    break

            return active_notes

        def read_beatmap_file(self, beatmap_path):
            # one onset per line, either "seconds" (old format, the track is
            # then drawn by the game) or "seconds<TAB>track" (the beatmap was
            # generated from the music and knows which note to play)
            text=renpy.file(beatmap_path).read()
            if isinstance(text, bytes):
                text = text.decode('utf-8')
            onset_times = []
            onset_tracks = []
            for line in text.split('\n'):
                line = line.strip()
                if line == '':
                    continue
                fields = line.split()
                onset_times.append(float(fields[0]))
                if len(fields) > 1:
                    onset_tracks.append(int(fields[1]) % self.num_track_bars)
            # tracks are used only when every onset has one
            if len(onset_tracks) != len(onset_times):
                onset_tracks = None
            return onset_times, onset_tracks

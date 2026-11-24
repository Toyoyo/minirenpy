/* The preferences "Test" button, and the audio actions behind it.
 *
 * `textbutton _("Test") action Play("sound", config.sample_sound)`. Only the
 * music-room form `mr.Play(...)` was implemented, so the bare Play() action
 * evaluated to nothing at all and the button was silent -- while still being
 * drawn, because its presence keys off `config.sample_sound` being set, which
 * is a different question and worked.
 *
 * This drives the real button through the focus list rather than calling the
 * action text directly, so the screen, the config variable and the action all
 * have to line up the way they do in the game.
 *
 * Takes the pak first and the game directory second: the sample sound has to
 * be decodable, so it needs the archive.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mr.h"
#include "expr.h"
#include "interp.h"
#include "vfs.h"
#include "script.h"
#include "image.h"
#include "scene.h"
#include "ui.h"
#include "audio.h"
#include "slrender.h"

static mr_surface *g_st;
mr_surface *mr_stage(void) { return g_st; }
void mr_fatal(const char *f, ...) { (void)f; printf("FATAL\n"); exit(1); }
#include <stdarg.h>
void mr_log(const char *f, ...) { (void)f; }
void mr_quit(void) {}
void mr_invalidate(void) {}
/* A pointer, not a function -- defining it as a function links fine and then
 * crashes the first time the mixer writes a buffer. */
void (*hostshim_wave_sink)(const short *, int) = NULL;

static int fails;

/* Counts. It did not, and `main` ended in a bare `return 0`, so every
 * assertion in this file was decoration: run.sh judges by exit code, so
 * four lines reading ** WANT DIFFERENT ** were reported as `pass
 * playtest`. That is how the hardcoded effect path below survived being
 * run against a game whose sample sound this engine cannot decode. */
static void check(const char *l, long got, long want)
{
    printf("%-46s %-6ld %s\n", l, got, got == want ? "ok" : "** WANT DIFFERENT **");
    if (got != want)
        fails++;
}

/* Click the focus entry whose action mentions `want`. */
static int click_action(const char *want)
{
    int i;
    for (i = 0; i < 128; i++) {
        const char *a = mr_sl_focus_action(i);
        if (a && strstr(a, want)) {
            mr_sl_focus_scope(i);
            mr_interp_action(a);
            mr_sl_focus_scope_end();
            return 1;
        }
    }
    return 0;
}

/* What a channel is playing, "" when idle, plus its loop flag. */
static const char *cur(mr_channel ch, int *loop)
{
    static char buf[256];
    mr_audio_current(ch, buf, sizeof(buf), loop);
    return buf;
}

static void pump(long t)
{
    int i;
    for (i = 0; i < 20; i++) {
        mr_audio_poll();
        mr_interp_update(t + i * 33);
    }
}

int main(int argc, char **argv)
{
    mr_script *s;
    mr_value v;
    char sample[256];

    setbuf(stdout, NULL);
    (void)argc;
    mr_vfs_init(argv[1], argv[2]);
    mr_image_init(argv[2]); mr_scene_init();
    s = mr_script_load(argv[2]);
    mr_interp_init(s, argv[2]);
    mr_interp_start("start");
    mr_ui_init(argv[2]);
    mr_audio_init(argv[2]);
    g_st = mr_surface_new(MR_STAGE_W, MR_STAGE_H);

    /* options.rpy sets this; the button only exists because it is set. */
    sample[0] = '\0';
    if (mr_var_get("config.sample_sound", &v) && v.type == MR_V_STR)
        strcpy(sample, v.s);
    check("config.sample_sound is set", sample[0] != '\0', 1);
    printf("  it is '%s'\n", sample);

    mr_interp_show_screen("preferences");
    mr_interp_update(0);
    mr_sl_focus_reset();
    mr_interp_render(g_st, 0);

    check("nothing on the sound channel yet", cur(MR_CH_SOUND, NULL)[0], 0);

    check("found the Test button", click_action("Play(\"sound\""), 1);
    /* Deliberately no pump here. The host shim pretends the device consumes
     * every buffer instantly, so a short non-looping effect is drained and
     * the channel idle again within a few polls -- the music check below
     * only survives its pump because it loops. */

    /* The whole point: the click has to reach the mixer. */
    {
        int loop = -1;
        const char *c = cur(MR_CH_SOUND, &loop);
        printf("  sound channel now '%s'\n", c);
        check("clicking Test plays on the sound channel", c[0] != 0, 1);
        check("  and it is the sample file", !strcmp(c, sample), 1);
        /* A sound effect must not loop, whatever the music channel does. */
        check("  playing once, not looping", loop, 0);
    }

    /* Music is the other half of the channel mapping, and loops by default.
     *
     * The same file the script named for config.sample_sound, built into
     * the action text, rather than a path written here. This said
     * "audio/fx corde.mp3" -- which is one game's effect, and passed on
     * the second game only because the two share a generated options.rpy.
     * Against a game that named anything else the file would not open, the
     * channel would stay inactive, and the failure would be "Play(music)
     * uses the music channel ** FAIL **": a channel-mapping bug, by the
     * look of it. What matters here is the channel and the loop flag, not
     * which file. */
    {
        char action[MR_PATH_MAX + 32];
        sprintf(action, "Play(\"music\", \"%s\")", sample);
        mr_interp_action(action);
    }
    pump(1000);
    {
        int loop = -1;
        const char *c = cur(MR_CH_MUSIC, &loop);
        check("Play(\"music\", ...) uses the music channel", c[0] != 0, 1);
        check("  and loops by default", loop, 1);
    }

    mr_interp_action("Stop(\"music\")");
    pump(2000);
    check("Stop(\"music\") stops it", cur(MR_CH_MUSIC, NULL)[0], 0);

    printf("\n%s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}

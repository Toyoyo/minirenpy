/* musicroom.c - MusicRoom, from renpy/common/00musicroom.rpy.
 *
 * The parts of the class the jukebox uses: a playlist, the unlocked subset of
 * it, and play/next/previous, which all go through the same routine in Ren'Py
 * -- play(filename, offset) -- so they do here too.
 *
 * Rooms are handed out of a fixed pool rather than allocated, because the
 * screen rebuilds its room from scratch every time it opens (`default mr =
 * build_music_room()`) and nothing ever destroys one. The pool wraps, so a
 * player who opens the jukebox nine times is using the same slot as their
 * first visit -- harmless, because the room is rebuilt identically.
 */

#include <stdio.h>
#include <string.h>

#include "mr.h"
#include "musicroom.h"
#include "audio.h"

#define MAX_ROOMS   8
#define MAX_TRACKS  32
#define MAX_SEEN    32

typedef struct {
    int  used;
    mr_channel ch;
    long fadeout_ms;
    long fadein_ms;
    int  loop;
    int  single_track;

    char file[MAX_TRACKS][MR_PATH_MAX];
    int  always_unlocked[MAX_TRACKS];
    int  n;
} mr_room;

static mr_room g_room[MAX_ROOMS];
static int     g_next;

static char g_seen[MAX_SEEN][128];
static int  g_nseen;

int mr_room_new(mr_channel ch, long fadeout_ms, long fadein_ms,
                int loop, int single_track)
{
    int id = g_next;
    mr_room *r;

    g_next = (g_next + 1) % MAX_ROOMS;

    r = &g_room[id];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->ch = ch;
    r->fadeout_ms = fadeout_ms;
    r->fadein_ms = fadein_ms;
    r->loop = loop;
    r->single_track = single_track;
    return id;
}

/* A handle is a string no script could produce, so a value either is one or
 * is something else entirely; there is no ambiguity to resolve. */
void mr_room_handle(int id, mr_value *out)
{
    char buf[32];

    sprintf(buf, "\001musicroom:%d", id);
    mr_value_set_str(out, buf);
}

int mr_room_from_value(const mr_value *v)
{
    int id;

    if (!v || v->type != MR_V_STR || v->s[0] != '\001')
        return -1;
    if (sscanf(v->s, "\001musicroom:%d", &id) != 1)
        return -1;
    if (id < 0 || id >= MAX_ROOMS || !g_room[id].used)
        return -1;
    return id;
}

void mr_room_add(int id, const char *file, int always_unlocked)
{
    mr_room *r;

    if (id < 0 || id >= MAX_ROOMS || !file || !*file)
        return;
    r = &g_room[id];
    if (r->n >= MAX_TRACKS) {
        mr_log("musicroom: more than %d tracks, ignoring %s", MAX_TRACKS, file);
        return;
    }
    strncpy(r->file[r->n], file, sizeof(r->file[0]) - 1);
    r->file[r->n][sizeof(r->file[0]) - 1] = '\0';
    r->always_unlocked[r->n] = always_unlocked;
    r->n++;
}

void mr_room_note_played(const char *file)
{
    int i;

    if (!file || !*file)
        return;
    for (i = 0; i < g_nseen; i++)
        if (!strcmp(g_seen[i], file))
            return;
    if (g_nseen >= MAX_SEEN)
        return;
    strncpy(g_seen[g_nseen], file, sizeof(g_seen[0]) - 1);
    g_seen[g_nseen][sizeof(g_seen[0]) - 1] = '\0';
    g_nseen++;
}

int mr_room_is_unlocked(int id, const char *file)
{
    const mr_room *r;
    int i;

    if (id < 0 || id >= MAX_ROOMS || !file)
        return 0;
    r = &g_room[id];

    for (i = 0; i < r->n; i++)
        if (!strcmp(r->file[i], file) && r->always_unlocked[i])
            return 1;

    for (i = 0; i < g_nseen; i++)
        if (!strcmp(g_seen[i], file))
            return 1;

    return 0;
}

int mr_room_is_playing(int id, const char *file)
{
    char playing[128];
    int loop;

    if (id < 0 || id >= MAX_ROOMS || !g_room[id].used || !file || !*file)
        return 0;

    mr_audio_current(g_room[id].ch, playing, sizeof(playing), &loop);
    return playing[0] && !strcmp(playing, file);
}

/* The unlocked playlist, as indices into the room. */
static int unlocked(const mr_room *r, int id, int *out)
{
    int i, n = 0;

    for (i = 0; i < r->n; i++)
        if (mr_room_is_unlocked(id, r->file[i]))
            out[n++] = i;
    return n;
}

void mr_room_play(int id, const char *file, int offset)
{
    mr_room *r;
    int list[MAX_TRACKS];
    int n, idx, i;
    char playing[128];
    int loop;

    if (id < 0 || id >= MAX_ROOMS || !g_room[id].used)
        return;
    r = &g_room[id];

    n = unlocked(r, id, list);
    if (!n)
        return;

    /* With no file named, start from whatever the channel is on: that is what
     * makes Next() advance from the current track rather than from the top. */
    if (!file || !*file) {
        mr_audio_current(r->ch, playing, sizeof(playing), &loop);
        file = playing;
    }

    idx = 0;
    for (i = 0; i < n; i++) {
        if (!strcmp(r->file[list[i]], file)) {
            idx = i;
            break;
        }
    }

    idx = (idx + offset) % n;
    if (idx < 0)
        idx += n;

    if (r->single_track) {
        mr_audio_play_ex(r->ch, r->file[list[idx]], r->loop,
                         r->fadein_ms, r->fadeout_ms, 1.0f, 0);
    } else {
        /* The playlist form. Ren'Py hands the whole rest of the list to
         * play(); the mixer here takes one track and one queued behind it,
         * which is as far as a jukebox with a Next button needs to look. */
        int next = (idx + 1) % n;

        mr_audio_play_ex(r->ch, r->file[list[idx]], 0,
                         r->fadein_ms, r->fadeout_ms, 1.0f, 0);
        if (n > 1 && (r->loop || next > idx))
            mr_audio_queue(r->ch, r->file[list[next]], 1.0f);
    }

    mr_room_note_played(r->file[list[idx]]);
}

void mr_room_next(int id)
{
    mr_room *r;
    char playing[128];
    int loop;

    if (id < 0 || id >= MAX_ROOMS || !g_room[id].used)
        return;
    r = &g_room[id];

    /* Nothing playing means Next starts the playlist rather than stepping
     * past its first entry. */
    mr_audio_current(r->ch, playing, sizeof(playing), &loop);
    mr_room_play(id, NULL, playing[0] ? 1 : 0);
}

void mr_room_previous(int id)
{
    mr_room_play(id, NULL, -1);
}

void mr_room_stop(int id)
{
    if (id < 0 || id >= MAX_ROOMS || !g_room[id].used)
        return;
    mr_audio_stop(g_room[id].ch, g_room[id].fadeout_ms);
}

void mr_room_reset(void)
{
    memset(g_room, 0, sizeof(g_room));
    g_next = 0;
}

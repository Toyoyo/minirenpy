/* scene.c - display list management, placement, and transitions.
 *
 * Two display lists are kept: `g_prev`, the state the player can currently
 * see, and `g_cur`, the state the script has built up. A transition animates
 * between them; when it finishes, g_prev becomes g_cur.
 *
 * Dissolve and fade work on whole-screen snapshots, because they blend
 * everything uniformly. The move family cannot, since it has to animate each
 * image's position independently, so it re-renders both lists every frame with
 * per-image offsets. That costs more, but at 1280x720 on a Pentium III it is
 * a handful of blits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "scene.h"
#include "image.h"
#include "audio.h"

static mr_scenelist g_prev;   /* what is on screen */
static mr_scenelist g_cur;    /* what the script has asked for */

static mr_surface *g_from;    /* snapshot of g_prev, for dissolve/fade */
static mr_surface *g_to;      /* snapshot of g_cur */
static int         g_snapped; /* the snapshots match the current lists */

static mr_screen_draw_fn g_draw_screen;

static mr_transition g_trans;
static long          g_trans_start;
static int           g_trans_active;

static void draw_screens(mr_surface *dst, const mr_scenelist *l);

int mr_scene_init(void)
{
    memset(&g_prev, 0, sizeof(g_prev));
    memset(&g_cur, 0, sizeof(g_cur));

    g_from = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    g_to = mr_surface_new(MR_STAGE_W, MR_STAGE_H);
    if (!g_from || !g_to) {
        mr_log("scene: could not allocate the two transition buffers");
        return 0;
    }
    return 1;
}

void mr_scene_shutdown(void)
{
    mr_surface_free(g_from);
    mr_surface_free(g_to);
    g_from = g_to = NULL;
}

/* The tag is the first word of the image name: `miwa biwa talk` has tag
 * `miwa`, so showing any `miwa ...` image replaces the one on screen. */
static void tag_of(const char *name, char *out, int outsz)
{
    int n = 0;
    while (name[n] && name[n] != ' ' && n < outsz - 1) {
        out[n] = name[n];
        n++;
    }
    out[n] = '\0';
}

void mr_scene_scene(const char *bg)
{
    /* `scene` clears the image layer but not the screen layer, which in
     * Ren'Py is separate and survives. */
    char keep[MR_MAX_SCREENS][MR_MAX_NAME];
    int  keep_story[MR_MAX_SCREENS];
    int  keep_z[MR_MAX_SCREENS];
    int  nkeep = g_cur.nscreens;

    /* The story/interface flags and the zorders travel with the names. They
     * did not before, so a `scene` statement quietly demoted every story
     * screen to an interface one -- which decides whether a save restores
     * it. Nothing had noticed because a title card is shown after the scene
     * rather than before. */
    memcpy(keep, g_cur.screens, sizeof(keep));
    memcpy(keep_story, g_cur.screen_story, sizeof(keep_story));
    memcpy(keep_z, g_cur.screen_z, sizeof(keep_z));
    memset(&g_cur, 0, sizeof(g_cur));
    memcpy(g_cur.screens, keep, sizeof(keep));
    memcpy(g_cur.screen_story, keep_story, sizeof(keep_story));
    memcpy(g_cur.screen_z, keep_z, sizeof(keep_z));
    g_cur.nscreens = nkeep;

    if (bg)
        strncpy(g_cur.bg, bg, MR_MAX_NAME - 1);
    g_snapped = 0;
}

void mr_scene_clear(void)
{
    memset(&g_cur, 0, sizeof(g_cur));
    memset(&g_prev, 0, sizeof(g_prev));
    g_trans_active = 0;
    g_snapped = 0;
}

/* config.default_transform's placement, `xpos 0.5 xanchor 0.5 ypos 1.0
 * yanchor 1.0` (00definitions.rpy:64-67): what a tag shown with no transform
 * stands at, written out so that a later block can be merged on top of it. */
static void place_seed_default(mr_place *pl)
{
    pl->has_xpos = 1;
    pl->xpos = 0.5f;
    pl->xpos_px = 0;
    pl->has_xanchor = 1;
    pl->xanchor = 0.5f;
    pl->xanchor_px = 0;
    pl->has_ypos = 1;
    pl->ypos = 1.0f;
    pl->ypos_px = 0;
    pl->has_yanchor = 1;
    pl->yanchor = 1.0f;
    pl->yanchor_px = 0;
}

/* Apply the properties a show statement actually specified on top of the
 * transform the image already has. Ren'Py updates rather than replaces, so an
 * earlier `yoffset 150` survives a later `at left: xoffset 300`.
 *
 * Alignment and absolute position are alternatives for the same axis, so
 * setting one clears the other. */
static void place_merge(mr_place *dst, const mr_place *src)
{
    if (src->has_xalign) {
        dst->has_xalign = 1;
        dst->xalign = src->xalign;
        dst->has_xpos = 0;
    }
    if (src->has_xpos) {
        dst->has_xpos = 1;
        dst->xpos = src->xpos;
        dst->xpos_px = src->xpos_px;
        dst->has_xalign = 0;
    }
    if (src->has_yalign) {
        dst->has_yalign = 1;
        dst->yalign = src->yalign;
        dst->has_ypos = 0;
    }
    if (src->has_ypos) {
        dst->has_ypos = 1;
        dst->ypos = src->ypos;
        dst->ypos_px = src->ypos_px;
        dst->has_yalign = 0;
    }
    if (src->has_xoffset) {
        dst->has_xoffset = 1;
        dst->xoffset = src->xoffset;
    }
    if (src->has_yoffset) {
        dst->has_yoffset = 1;
        dst->yoffset = src->yoffset;
    }
    if (src->has_xanchor) {
        dst->has_xanchor = 1;
        dst->xanchor = src->xanchor;
        dst->xanchor_px = src->xanchor_px;
    }
    if (src->has_yanchor) {
        dst->has_yanchor = 1;
        dst->yanchor = src->yanchor;
        dst->yanchor_px = src->yanchor_px;
    }
    if (src->has_alpha) {
        dst->has_alpha = 1;
        dst->alpha = src->alpha;
    }
    if (src->has_xzoom) {
        dst->has_xzoom = 1;
        dst->xzoom = src->xzoom;
    }
    if (src->has_yzoom) {
        dst->has_yzoom = 1;
        dst->yzoom = src->yzoom;
    }
    if (src->has_zoom) {
        dst->has_zoom = 1;
        dst->zoom = src->zoom;
    }
}

/* --- ATL animation --------------------------------------------------------
 *
 * A timed step interpolates every property it names from wherever the image
 * currently is toward the step's own placement. Ren'Py interpolates the
 * properties themselves, not the resolved pixel position, which matters here:
 * the credits move `ypos 720 yanchor 0.0` to `ypos 0 yanchor 1.0`, and both
 * halves have to travel together for the roll to be even.
 */

static float warp(mr_warper w, float t)
{
    if (w == MR_WARP_EASE)
        return t * t * (3.0f - 2.0f * t);   /* smoothstep */
    return t;
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Where an interpolation toward a pos/anchor starts, when the placement it
 * starts from does not name that property.
 *
 * An mr_place can say the same thing two ways: `has_xalign` as a single
 * fraction, or `has_xpos` plus `has_xanchor`. Ren'Py has only the second --
 * `xalign` is a property setter that writes both (transform.py:268-269) and
 * there is no align in the state at all -- and set_place_prop expands it the
 * same way, so a transform body's `xalign 0.0` arrives here as xpos 0.0 and
 * xanchor 0.0. The *stock positions* are the exception: place_from_name
 * stores `at right` as xalign 1.0.
 *
 * So `show saza at right` followed by `show saza at moveleft`, where
 *
 *     transform moveleft:
 *         linear 0.5 xalign 0.0
 *
 * had the step interpolating xpos from nothing toward 0.0. `from` carried no
 * xpos -- her x was under the other name -- and Ren'Py's rule for a start
 * value that is absent is `if a is None: a = 0` (atl.py's interpolate). Zero
 * to zero: she arrived at the left edge, correctly, without travelling. The
 * placement half of the feature worked and the animation half did not, which
 * is exactly how it was reported.
 *
 * Only across matching units. An align is always a fraction of the stage, so
 * it can stand in for a fractional pos and not for a pixel one; `linear 0.5
 * xpos 500` from `at right` keeps the old answer, which is Ren'Py's too
 * unless config.mixed_position is on. */
static float lerp_from(int has, float v, int want_px, int alias_has,
                       float alias, float none)
{
    if (has)
        return v;
    if (!want_px && alias_has)
        return alias;
    return none;
}

/* Interpolate `from` toward the properties `to` names, into `out`. */
static void place_lerp(mr_place *out, const mr_place *from,
                       const mr_place *to, float t)
{
    *out = *from;

    if (to->has_xpos) {
        float a = lerp_from(from->has_xpos, from->xpos, to->xpos_px,
                            from->has_xalign, from->xalign, 0.0f);
        out->has_xpos = 1;
        out->xpos_px = to->xpos_px;
        out->xpos = lerpf(a, to->xpos, t);
        out->has_xalign = 0;
    }
    if (to->has_ypos) {
        float a = lerp_from(from->has_ypos, from->ypos, to->ypos_px,
                            from->has_yalign, from->yalign, 0.0f);
        out->has_ypos = 1;
        out->ypos_px = to->ypos_px;
        out->ypos = lerpf(a, to->ypos, t);
        out->has_yalign = 0;
    }
    if (to->has_xanchor) {
        float a = lerp_from(from->has_xanchor, from->xanchor, to->xanchor_px,
                            from->has_xalign, from->xalign, 0.0f);
        out->has_xanchor = 1;
        out->xanchor_px = to->xanchor_px;
        out->xanchor = lerpf(a, to->xanchor, t);
    }
    if (to->has_yanchor) {
        float a = lerp_from(from->has_yanchor, from->yanchor, to->yanchor_px,
                            from->has_yalign, from->yalign, 0.0f);
        out->has_yanchor = 1;
        out->yanchor_px = to->yanchor_px;
        out->yanchor = lerpf(a, to->yanchor, t);
    }
    /* The other direction -- `linear 0.5 center` is legal ATL, and reaches
     * place_from_name with a step's placement as its target. Clearing the pos
     * form is the mirror of what the two branches above do to the align one:
     * mr_place_resolve prefers has_xpos, so an align left sitting beside a
     * stale pos would not be read at all. */
    if (to->has_xalign) {
        float a = lerp_from(from->has_xalign, from->xalign, 0,
                            from->has_xpos && !from->xpos_px, from->xpos,
                            0.5f);
        out->has_xalign = 1;
        out->xalign = lerpf(a, to->xalign, t);
        out->has_xpos = 0;
        out->has_xanchor = 0;
    }
    if (to->has_yalign) {
        float a = lerp_from(from->has_yalign, from->yalign, 0,
                            from->has_ypos && !from->ypos_px, from->ypos,
                            0.0f);
        out->has_yalign = 1;
        out->yalign = lerpf(a, to->yalign, t);
        out->has_ypos = 0;
        out->has_yanchor = 0;
    }
    if (to->has_xoffset) {
        out->has_xoffset = 1;
        out->xoffset = (int)lerpf((float)from->xoffset, (float)to->xoffset, t);
    }
    if (to->has_yoffset) {
        out->has_yoffset = 1;
        out->yoffset = (int)lerpf((float)from->yoffset, (float)to->yoffset, t);
    }
    /* From 1.0 when the image had no alpha of its own, because that is
     * Ren'Py's default for the property -- so `linear .5 alpha 0.0` on a
     * plain sprite is a fade-out, not a no-op. */
    if (to->has_alpha) {
        out->has_alpha = 1;
        out->alpha = lerpf(from->has_alpha ? from->alpha : 1.0f,
                           to->alpha, t);
    }
    /* From 1.0 for the same reason, so `linear 1.0 xzoom 2.0` on a plain
     * sprite grows it rather than starting from nothing. */
    if (to->has_xzoom) {
        out->has_xzoom = 1;
        out->xzoom = lerpf(from->has_xzoom ? from->xzoom : 1.0f,
                           to->xzoom, t);
    }
    if (to->has_yzoom) {
        out->has_yzoom = 1;
        out->yzoom = lerpf(from->has_yzoom ? from->yzoom : 1.0f,
                           to->yzoom, t);
    }
    /* Interpolated on its own, not as the product. `linear 1.0 zoom 2.0` on
     * an image already at `xzoom 0.5` has to pass through zoom 1.5 -- which
     * is x 0.75 -- and not through x 1.5. */
    if (to->has_zoom) {
        out->has_zoom = 1;
        out->zoom = lerpf(from->has_zoom ? from->zoom : 1.0f,
                          to->zoom, t);
    }
}

/* Advance every animating image in a list. Returns 1 if any is still
 * running, so the frame loop knows to keep drawing. */
static int atl_update(mr_scenelist *l, long now_ms)
{
    int i, busy = 0;

    for (i = 0; i < l->n; i++) {
        mr_shown *s = &l->v[i];
        const mr_atl *a = s->atl;

        if (!a)
            continue;

        while (s->step < a->n) {
            const mr_atl_step *st = &a->v[s->step];
            long elapsed = now_ms - s->step_start;
            float t;

            if (st->ms > 0 && elapsed < st->ms) {
                t = warp(st->warp, (float)elapsed / (float)st->ms);
                place_lerp(&s->place, &s->step_from, &st->place, t);
                busy = 1;
                break;
            }

            /* Step done: land exactly on it and start the next one. */
            place_lerp(&s->place, &s->step_from, &st->place, 1.0f);
            s->step++;
            s->step_from = s->place;
            s->step_start += st->ms;

            if (s->step >= a->n && a->repeat) {
                s->step = 0;
                s->place = s->atl_base;
                s->step_from = s->place;
                s->step_start = now_ms;
                busy = 1;
                break;
            }
        }
    }

    return busy;
}

/* Arm (or disarm) an image's animation. A show statement without a transform
 * block clears any animation the tag was running, which is how the story
 * stops a roll by showing something else under the same tag. */
static void start_atl(mr_shown *s, const mr_atl *atl, long now_ms)
{
    s->atl = atl;
    s->step = 0;
    s->step_start = now_ms;
    s->atl_base = s->place;
    s->step_from = s->place;
}

/* Is `tag` one of the comma-separated names in `list`? Whole entries only, so
 * `azami` does not match inside `azamine`. */
static int in_taglist(const char *list, const char *tag)
{
    int n;

    if (!list || !*list)
        return 0;
    n = (int)strlen(tag);

    while (*list) {
        const char *e = list;

        while (*e && *e != ',')
            e++;
        if (e - list == n && !strncmp(list, tag, (size_t)n))
            return 1;
        list = *e ? e + 1 : e;
    }
    return 0;
}

/* Where a show lands in the z-order. Ren'Py's find_index (scenelists.py:313)
 * with every zorder equal, which is what these games have: the index of the
 * *first* entry whose tag is either this one or one of the `behind` names,
 * and the end of the list if there is none.
 *
 * With no behind clause that is just the entry's own slot, so an ordinary
 * re-show does not move -- which is why this can be applied unconditionally
 * rather than only when a behind clause is present. */
static int find_add_index(const char *tag, const char *behind)
{
    int i;

    for (i = 0; i < g_cur.n; i++) {
        if (!strcmp(g_cur.v[i].tag, tag) || in_taglist(behind, g_cur.v[i].tag))
            return i;
    }
    return g_cur.n;
}

void mr_scene_show(const char *name, const mr_place *place, int has_place)
{
    mr_scene_show_anim(name, place, has_place, NULL, 0, NULL);
}

void mr_scene_show_anim(const char *name, const mr_place *place, int has_place,
                        const mr_atl *atl, long now_ms, const char *behind)
{
    char tag[MR_MAX_NAME];
    char resolved[MR_MAX_NAME];
    int i;
    int is_bg = 0;

    tag_of(name, tag, sizeof(tag));

    /* `show azami at left270` names a tag and no attributes, and there is no
     * azami.png. In Ren'Py an image name is a tag plus a set of attributes,
     * and a show that names fewer than the image has keeps the ones the tag
     * is wearing (display/image.py:949). Resolved here, against the entry
     * already on the list, because this is the only place that knows both.
     *
     * The tag is taken from what the script wrote and is not re-derived from
     * the result: resolution never changes the first word, and the list
     * lookup below has to find the same entry whether it succeeded or not. */
    for (i = 0; i < g_cur.n; i++) {
        if (!strcmp(g_cur.v[i].tag, tag))
            break;
    }
    if (mr_image_choose(name, i < g_cur.n ? g_cur.v[i].name : NULL,
                        resolved, sizeof(resolved)))
        name = resolved;

    /* Replace an existing image with the same tag, merging in whatever the
     * new statement specified. This is what makes `show miwa biwa` followed
     * by `show miwa biwa talk` stay put, and what keeps an earlier offset
     * alive across a later repositioning. */
    for (i = 0; i < g_cur.n; i++) {
        if (!strcmp(g_cur.v[i].tag, tag)) {
            strncpy(g_cur.v[i].name, name, MR_MAX_NAME - 1);
            /* A re-show that says nothing keeps what the tag already had --
             * Ren'Py's re-show path inherits the old placement rather than
             * re-applying the default (transform_state(old, thing),
             * scenelists.py:445) -- so has_place is sticky, never cleared.
             *
             * A re-show that says something inherits too: take_state
             * (scenelists.py:306) copies the old transform's state and the
             * new block applies on top. If the tag was shown bare, that
             * state is config.default_transform's, bottom centre, so it is
             * seeded before the merge. Merging into the empty placement
             * instead left the unnamed axes at 0: `show park` then
             * `show park:` + `yoffset 400` put the image in the top-left
             * corner, 400px down, where Ren'Py slides it 400px down from
             * where it stood. */
            if (has_place) {
                if (!g_cur.v[i].has_place)
                    place_seed_default(&g_cur.v[i].place);
                place_merge(&g_cur.v[i].place, place);
                g_cur.v[i].has_place = 1;
            }

            /* A re-show that says nothing about where the image goes leaves
             * the running animation alone. Ren'Py's rule, and it is a clean
             * three-way one (scenelists.py:435-444):
             *
             *   - an ATL block        -> the new block replaces the old
             *   - an `at` list        -> transform_state with execution=False,
             *                            so the state carries over and the
             *                            animation does not
             *   - neither             -> _change_transform_child, which keeps
             *                            the old transform running and swaps
             *                            only the picture
             *                            (config.keep_running_transform, True)
             *
             * The third case is the one this used to get wrong -- every
             * re-show cleared the animation -- and it cost:
             *
             *     show azami magical pout:
             *         linear 0.5 xoffset 500
             *     show azami magical fear with dissolve
             *
             * Nothing draws a frame between those two, so the move was
             * cancelled before it could run and she stayed at the 830 an
             * earlier move had left her at, 330 pixels right of where the
             * script put her.
             *
             * `has_place || atl` is exactly Ren'Py's `at_list or atl`: a
             * block sets has_place whether or not it had timed steps, and a
             * block with no timed steps is an ATL to Ren'Py too, so both
             * land in the same branch. */
            if (atl || has_place)
                start_atl(&g_cur.v[i], atl, now_ms);

            /* Ren'Py recomputes the slot on every show, not only on the
             * first: the old entry is removed and the new one inserted at
             * find_index (scenelists.py:424-455). So
             *
             *     show eari at right          # eari is in front
             *     show eari behind keiko      # and now she is not
             *
             * moves her. With no behind clause the two indices are the same
             * and nothing happens, which is the common case.
             *
             * The target can only be at or before the entry's own slot: its
             * predicate is a superset of "same tag", so the first match
             * cannot come later. That is why this moves one way only. */
            {
                int to = find_add_index(tag, behind);

                if (to < i) {
                    mr_shown moved = g_cur.v[i];
                    int k;

                    for (k = i; k > to; k--)
                        g_cur.v[k] = g_cur.v[k - 1];
                    g_cur.v[to] = moved;
                }
            }

            g_snapped = 0;
            return;
        }
    }

    if (g_cur.n >= MR_MAX_SHOWN) {
        /* The limit is named in the message because a dropped image is
         * invisible by definition: the sprite the script asked for simply is
         * not there, which reads as a missing file or a bad name rather than
         * a full list. Azami's summoning scene wants 16. */
        mr_log("scene: %d images already shown, which is MR_MAX_SHOWN; "
               "dropping '%s'", MR_MAX_SHOWN, name);
        return;
    }

    /* Is this tag the scene backdrop? Then the new entry takes the slot's
     * place in the list as well as its contents -- at the *bottom* of the
     * z-order, which is where the slot drew it.
     *
     * Appending instead put the backdrop last, so it drew over every sprite:
     * takio and hinata vanished behind a 1280-wide background the moment the
     * pan began. Reported from play, and caused by this fix rather than by
     * the bug it was fixing. */
    {
        if (g_cur.bg[0]) {
            char bgtag[MR_MAX_NAME];
            tag_of(g_cur.bg, bgtag, sizeof(bgtag));
            is_bg = !strcmp(bgtag, tag);
        }

        if (is_bg) {
            int k;
            for (k = g_cur.n; k > 0; k--)
                g_cur.v[k] = g_cur.v[k - 1];
            g_cur.n++;
            i = 0;
            g_cur.bg[0] = '\0';
        } else {
            /* `show eari behind keiko` goes in front of keiko in the list,
             * which is behind her on the screen -- render_list draws in list
             * order. Without a behind clause this is the end of the list,
             * i.e. the append it has always been. */
            int k;

            i = find_add_index(tag, behind);
            for (k = g_cur.n; k > i; k--)
                g_cur.v[k] = g_cur.v[k - 1];
            g_cur.n++;
        }
    }

    memset(&g_cur.v[i], 0, sizeof(g_cur.v[i]));
    strncpy(g_cur.v[i].name, name, MR_MAX_NAME - 1);
    strncpy(g_cur.v[i].tag, tag, MR_MAX_NAME - 1);

    /* The handover above took the slot. What is deliberately *not* done here
     * is to seed a placement for it: an empty place resolves to exactly where
     * bg_pos puts the slot, so the entry starts where the backdrop already
     * was and the transition has no jump in it.
     *
     * Seeding `yalign 0.5` was the first attempt, to match what the slot used
     * to do -- and it baked in the very divergence being removed. On the
     * 1280x1460 backdrop that is y=-370 instead of -740, so the pan ended
     * with the image's top edge 370px down the screen. Reported as "the end
     * position is bad".
     *
     * `scene X` does not create a shown image here: it puts X in a slot of
     * its own, which render_list and render_move blit with no place and no
     * interpolation. Ren'Py has no such slot --
     * `scene X` is "clear the layer, then show X", so the backdrop is an
     * ordinary displayable with tag `bg` that a transition moves like any
     * other.
     *
     * The divergence showed up as a MoveTransition with no animation. A
     * script pans a backdrop by re-showing it:
     *
     *     scene bg parknight
     *     ...
     *     show bg parknight:
     *         yoffset 740
     *     with MoveTransition(2.0)
     *
     * and that `show` used to *add* an entry beside the slot, leaving two
     * copies of the same image on screen. render_move then found no previous
     * entry for the tag, treated it as newly shown -- which under plain
     * `move` is drawn at its final position for the whole transition -- while
     * the slot kept painting the backdrop where it had always been. Nothing
     * appeared to move, and the end state looked right, which is what made it
     * hard to see.
     *
     * Handing the slot over gives one backdrop, placed, and interpolated.
     *
     * The entry is seeded with the placement the slot was drawing with --
     * config.default_transform, `xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0`
     * (00definitions.rpy:64-67) -- because this is a *re-show* of a tag that
     * is already on screen, and Ren'Py's re-show path inherits the previous
     * placement and applies the new block on top of it
     * (`transform_state(old, thing)`, scenelists.py:445). Only a *fresh* tag
     * gets no default and starts from zero.
     *
     * Both halves of that matter, and both were got wrong in turn:
     *
     *   - seeding `yalign 0.5` (centring) put the 1280x1460 backdrop's
     *     middle on screen, so a `yoffset 740` pan ended with its top edge
     *     370px down the stage.
     *   - seeding nothing left it at zero, so the same pan ended at y=740 --
     *     the whole image below the stage, a black window.
     *
     * Seeded properly, the backdrop starts at y = 720-1460 = -740 (its lower
     * 720 rows, the ground) and `yoffset 740` takes it to y=0 (its upper 720
     * rows, the sky). That is the pan the scene is asking for. */
    if (is_bg) {
        place_seed_default(&g_cur.v[i].place);
        /* The seed names all four, so this entry does carry a placement now
         * and must not be defaulted a second time. */
        g_cur.v[i].has_place = 1;
    }

    /* Merged rather than assigned, so the seed above survives whatever the
     * statement itself specified -- which is the whole point of it. */
    if (has_place) {
        place_merge(&g_cur.v[i].place, place);
        g_cur.v[i].has_place = 1;
    }
    start_atl(&g_cur.v[i], atl, now_ms);
    g_snapped = 0;
}

/* The display list, for a save or a rollback checkpoint. The atl pointers are
 * dropped: they point into the script's statement array, and a restored state
 * should not resume half way through someone else's animation. */
/* The display list as it stands, including each entry's running animation.
 *
 * The animation used to be stripped here -- atl, step and step_start cleared
 * -- which was safe for a save, where the pointer cannot be written anyway,
 * and wrong for a rollback checkpoint, which lives in memory beside the
 * script that owns the mr_atl.
 *
 * What it cost:
 *
 *     show azami magical cry:
 *         linear 0.5 yoffset 200
 *     "..."
 *
 * The checkpoint for the say is taken as the line goes up, which is *before*
 * the half second has run, so the entry it recorded had yoffset 0 and no
 * animation to get it anywhere else. Rolling back onto that line put her back
 * at the top of the drop and left her there.
 *
 * Keeping the three fields fixes it with no extra machinery, because
 * step_start is an absolute timestamp. By the time a rollback restores it the
 * clock has moved well past the end of the step, so advance_atl finishes the
 * interpolation on the first frame and the sprite is simply where she ended
 * up. That is what Ren'Py shows too: the SceneListEntry's show_time is rolled
 * back with everything else, so the transform's st is large and the ATL
 * renders past its end rather than replaying.
 *
 * A pointer is not something a file can hold, so both readers -- save.c for
 * the live scene, history.c for the checkpoints -- write the block itself and
 * point the rebuilt entry at a parked copy. They used to null it instead,
 * which is what made a save taken mid-animation load frozen; see
 * mr_scene_resume_anim. */
void mr_scene_capture(mr_scenelist *out)
{
    *out = g_cur;
}

void mr_scene_restore(const mr_scenelist *in)
{
    g_cur = *in;
    g_prev = *in;
    g_trans_active = 0;
    g_snapped = 0;
}

/* One parked block per display list slot. About 9KB, written only by a load.
 *
 * Indexed by slot rather than allocated, so a second load reuses the same
 * storage. Nothing outlives a load that could still be pointing at the old
 * contents: reading a save replaces the rollback log with the one in the file
 * (mr_history_read), and history.c nulls the atl pointer of every entry it
 * reads, so no checkpoint from before the load survives to be animated. */
static mr_atl g_parked[MR_MAX_SHOWN];

const mr_atl *mr_scene_park_atl(int slot, const mr_atl *a)
{
    if (slot < 0 || slot >= MR_MAX_SHOWN)
        slot = 0;
    g_parked[slot] = *a;
    return &g_parked[slot];
}

void mr_scene_resume_anim(long now_ms)
{
    int i;

    /* now - elapsed, which is the timestamp the step would have had if it had
     * started that long ago on this clock. Not now + elapsed: that puts the
     * step's start in the future, and atl_update's `now - step_start` then
     * goes negative and extrapolates backwards past the step's own
     * beginning. */
    for (i = 0; i < g_cur.n; i++) {
        if (!g_cur.v[i].atl)
            continue;
        g_cur.v[i].step_start = now_ms - g_cur.v[i].step_start;
    }
    /* mr_scene_restore makes both lists the same, and the render advances
     * each of them separately (see the note in mr_scene_render on keeping
     * them in step). Rebasing only one would leave the visible list frozen
     * at the moment of the save while the pending one animated. */
    for (i = 0; i < g_prev.n; i++) {
        if (!g_prev.v[i].atl)
            continue;
        g_prev.v[i].step_start = now_ms - g_prev.v[i].step_start;
    }
    g_snapped = 0;
}

void mr_scene_set_screen_drawer(mr_screen_draw_fn fn)
{
    g_draw_screen = fn;
}

static void show_screen(const char *name, int story)
{
    int i;

    for (i = 0; i < g_cur.nscreens; i++) {
        if (!strcmp(g_cur.screens[i], name)) {
            /* Already up. `show screen` over an interface screen of the same
             * name still makes it the story's. */
            if (story)
                g_cur.screen_story[i] = 1;
            return;
        }
    }
    if (g_cur.nscreens >= MR_MAX_SCREENS) {
        mr_log("scene: too many screens shown, dropping '%s'", name);
        return;
    }
    strncpy(g_cur.screens[g_cur.nscreens], name, MR_MAX_NAME - 1);
    g_cur.screens[g_cur.nscreens][MR_MAX_NAME - 1] = '\0';
    g_cur.screen_story[g_cur.nscreens] = story;
    g_cur.screen_z[g_cur.nscreens] = 0;
    g_cur.nscreens++;
    g_snapped = 0;
}

void mr_scene_show_screen(const char *name)
{
    show_screen(name, 0);
}

void mr_scene_show_story_screen(const char *name)
{
    show_screen(name, 1);
}

void mr_scene_screen_set_zorder(const char *name, int z)
{
    int i;
    for (i = 0; i < g_cur.nscreens; i++) {
        if (!strcmp(g_cur.screens[i], name)) {
            if (g_cur.screen_z[i] != z) {
                g_cur.screen_z[i] = z;
                g_snapped = 0;
            }
            return;
        }
    }
}

int mr_scene_screen_count(void)
{
    return g_cur.nscreens;
}

int mr_scene_screen_count_under_window(void)
{
    int i, n = 0;

    for (i = 0; i < g_cur.nscreens; i++)
        if (g_cur.screen_z[i] <= 0)
            n++;

    return n;
}

const char *mr_scene_screen_name(int i)
{
    if (i < 0 || i >= g_cur.nscreens)
        return NULL;
    return g_cur.screens[i];
}

void mr_scene_hide_screen(const char *name)
{
    int i;

    for (i = 0; i < g_cur.nscreens; i++) {
        if (!strcmp(g_cur.screens[i], name)) {
            memmove(g_cur.screens[i], g_cur.screens[i + 1],
                    (size_t)(g_cur.nscreens - i - 1) * MR_MAX_NAME);
            memmove(&g_cur.screen_story[i], &g_cur.screen_story[i + 1],
                    (size_t)(g_cur.nscreens - i - 1) *
                    sizeof(g_cur.screen_story[0]));
            memmove(&g_cur.screen_z[i], &g_cur.screen_z[i + 1],
                    (size_t)(g_cur.nscreens - i - 1) *
                    sizeof(g_cur.screen_z[0]));
            g_cur.nscreens--;
            g_snapped = 0;
            return;
        }
    }
}

void mr_scene_hide(const char *name)
{
    char tag[MR_MAX_NAME];
    int i;

    tag_of(name, tag, sizeof(tag));

    for (i = 0; i < g_cur.n; i++) {
        if (!strcmp(g_cur.v[i].tag, tag)) {
            memmove(&g_cur.v[i], &g_cur.v[i + 1],
                    (size_t)(g_cur.n - i - 1) * sizeof(mr_shown));
            g_cur.n--;
            g_snapped = 0;
            return;
        }
    }
}

/* Ren'Py places images bottom-centre by default: the stock transform is
 * xpos 0.5 / xanchor 0.5 / ypos 1.0 / yanchor 1.0, which is why sprites stand
 * on the bottom edge. `at left` and `at right` change only the x pair. */
void mr_place_resolve(const mr_place *pl, int has_place,
                      int img_w, int img_h, int *x, int *y)
{
    /* The default placement, for an image shown with no placement at all.
     *
     * Ren'Py applies `config.default_transform` in that case, which is
     * `transform default: xpos 0.5 xanchor 0.5 ypos 1.0 yanchor 1.0`
     * (00definitions.rpy:64-67) -- bottom centre, the familiar place a
     * sprite stands.
     *
     * But *only* in that case. The default transform is applied at
     * scenelists.py:450 under `if not isinstance(thing, Transform)`, and a
     * `show` carrying an `at` list or a transform block already is one. So a
     * placement that says anything at all gets no default: whatever it does
     * not mention stays None, and place() turns None into 0
     * (displayable.py:51-58). Top left, anchored at the corner.
     *
     * That distinction is the whole of this function's x and y fallbacks,
     * and getting it wrong is how
     *
     *     show mouthhinata:
     *         yalign 0.0 xoffset 129 yoffset 443
     *
     * landed 627px to the right of where it belongs. A 25px mouth meant to
     * sit at x=129 was centred first and then offset, to 756. Ren'Py gives
     * 0 + 129. Reported from play as misplaced mouths, and it would do the
     * same to every absolute-coordinate overlay a script positions this way.
     */
    float xa = has_place ? 0.0f : 0.5f;
    float ya = has_place ? 0.0f : 1.0f;

    if (has_place && pl->has_xalign)
        xa = pl->xalign;
    if (has_place && pl->has_yalign)
        ya = pl->yalign;

    /* An explicit pos names where the image's anchor point goes; an align is
     * the shorthand that sets pos and anchor to the same fraction. */
    /* Accumulated as a float and truncated once, not term by term: at
     * xalign 0.5 with an 803-wide image that is 640 - 401.5, and truncating
     * each half separately would land a pixel to the right of where the same
     * placement written `at center` lands. Ren'Py resolves the whole
     * expression before it becomes a coordinate. */
    if (has_place && pl->has_xpos) {
        float fx = pl->xpos_px ? pl->xpos
                               : pl->xpos * (float)MR_STAGE_W;
        if (pl->has_xanchor)
            fx -= pl->xanchor_px ? pl->xanchor
                                 : pl->xanchor * (float)img_w;
        else if (pl->has_xalign)
            fx -= pl->xalign * (float)img_w;
        *x = (int)fx;
    } else {
        *x = (int)(xa * (float)(MR_STAGE_W - img_w));
    }

    if (has_place && pl->has_ypos) {
        float fy = pl->ypos_px ? pl->ypos
                               : pl->ypos * (float)MR_STAGE_H;
        if (pl->has_yanchor)
            fy -= pl->yanchor_px ? pl->yanchor
                                 : pl->yanchor * (float)img_h;
        else if (pl->has_yalign)
            fy -= pl->yalign * (float)img_h;
        *y = (int)fy;
    } else {
        /* `ya` carries the fallback: 1.0 (standing on the bottom) when no
         * placement was given at all, 0.0 when one was and simply did not
         * mention y. */
        *y = (int)(ya * (float)(MR_STAGE_H - img_h));
    }

    if (has_place) {
        *x += pl->xoffset;
        *y += pl->yoffset;
    }
}

/* Draw one display list. dx/dy shift every non-background image, which the
 * move transitions use. */
/* Set while painting the live display list rather than a transition
 * snapshot; only then are the buttons worth hit-testing. */
static int g_live;

/* Where the scene backdrop goes.
 *
 * The answer mr_place_resolve gives for an image shown with no placement at
 * all -- x centred, y standing on the bottom -- and *not* the "centre it both
 * ways" this used to do. For a full-stage 1280x720 backdrop the two agree,
 * which is why centring survived; for a taller one they do not, and a taller
 * one is exactly what a script pans. `bg parknight.png` is 1280x1460, so
 * centring showed its middle, and a `yoffset 740` pan from there landed the
 * image's top edge 370px down the screen with black above and below.
 *
 * Bottom-standing is also Ren'Py's default -- its placement for a shown image
 * is bottom-centre -- so a 1460-tall background shows its lower 720 rows
 * until something moves it, and `yoffset 740` then brings the top 720 into
 * view. That is the pan the script is asking for.
 *
 * Checked before changing: the original game has nothing but 1280x720
 * full-width images, so this is a no-op there. */
static void bg_pos(const mr_surface *s, int *x, int *y)
{
    mr_place none;

    memset(&none, 0, sizeof(none));
    mr_place_resolve(&none, 0, s->w, s->h, x, y);
}

/* The pixels to draw for one display-list entry: the image, or a resampled
 * copy of it when the entry carries a zoom, an xzoom or a yzoom.
 *
 * Every caller that wants an entry's surface goes through this, because the
 * zoomed size is also the size the placement has to be resolved from --
 * accelerator.pyx:564-566 scales width and height before the placement runs,
 * so `xzoom 0.5` on a centred sprite moves it as well as shrinking it. Asking
 * mr_image_get directly here would place at the full size and draw at half.
 *
 * Returns NULL for a zoom of zero, which is nothing to draw -- every caller
 * already skips a NULL surface, since an undecodable image produces one.
 *
 * The scene background is not covered: it has no mr_shown and no placement of
 * its own (see bg_pos). `scene bg at <transform>` is not something the parser
 * builds today, so there is nothing to apply.
 */
static mr_surface *shown_image(const mr_shown *v)
{
    mr_surface *s;
    float zx, zy, z;
    int w, h;

    if (!v->has_place ||
        (!v->place.has_xzoom && !v->place.has_yzoom && !v->place.has_zoom))
        return mr_image_get(v->name);

    s = mr_image_get(v->name);
    if (!s)
        return NULL;

    /* The product of the per-axis factor and the common one, signs included:
     * accelerator.pyx:555-557. `zoom -1.0` with `xzoom -1.0` is +1 on x and
     * -1 on y, so this is a vertical flip and not a horizontal one. */
    z = v->place.has_zoom ? v->place.zoom : 1.0f;
    zx = z * (v->place.has_xzoom ? v->place.xzoom : 1.0f);
    zy = z * (v->place.has_yzoom ? v->place.yzoom : 1.0f);

    /* Absolute value for the size, sign for the mirroring: Ren'Py's own split
     * (accelerator.pyx:563-571). */
    w = (int)((zx < 0.0f ? -zx : zx) * (float)s->w + 0.5f);
    h = (int)((zy < 0.0f ? -zy : zy) * (float)s->h + 0.5f);
    if (w <= 0 || h <= 0)
        return NULL;

    if (w == s->w && h == s->h && zx > 0.0f && zy > 0.0f)
        return s;           /* `xzoom 1.0`, or a zoom too small to show */

    return mr_image_get_zoom(v->name, w, h, zx < 0.0f, zy < 0.0f);
}

/* Draw one display-list entry, honouring its `alpha`.
 *
 * mr_blit_alpha already short-circuits to mr_blit at 255 and returns at 0, so
 * the common opaque sprite keeps the memcpy fast path and a fully
 * transparent one costs nothing. The cost of a real blend is the same loop
 * the dissolve runs, over a sprite instead of the whole stage -- about a
 * fifth of the pixels of one dissolve frame, and only for the entries that
 * ask for it. */
static void blit_shown(mr_surface *dst, const mr_surface *s,
                       const mr_shown *v, int x, int y)
{
    int a;

    if (!v->place.has_alpha) {
        mr_blit(dst, s, x, y);
        return;
    }
    a = (int)(v->place.alpha * 255.0f + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    mr_blit_alpha(dst, s, x, y, a);
}

/* Will the background, placed here, write every pixel the black fill would?
 *
 * MR_PROF is what asked the question: on every full-frame batch the `copy` and
 * `fill` buckets carried *identical* pixel and call counts -- 27,648kpx over
 * 30 calls each, 8,294kpx over 9 each -- because the stage was being cleared
 * to black and then covered, pixel for pixel, by an opaque full-stage
 * background. 921,600 px at 20.7 ns/px is **19.1ms of a ~151ms still frame**
 * spent on pixels nobody ever sees (log-divfix.txt).
 *
 * Against the *clip rectangle* and not the surface, because that is what both
 * writers actually touch: mr_surface_fill clips (gfx.c, and it has to -- a
 * band frame's clear must stay inside the band or the partial frame blanks the
 * screen) and so does mr_blit. Comparing against dst->w/h instead would be
 * merely conservative; comparing against the clip is exact, and it lets the
 * first frame of a line benefit too.
 *
 * Three ways this is false and the fill is still needed, which is the whole
 * reason it is a test and not a deletion: no background at all (a `scene` with
 * only sprites, or an image that would not decode); one smaller than the
 * stage, or placed at an offset, so there are edges to show through; and one
 * that is genuinely translucent, where black is the thing it composites over.
 *
 * `!has_alpha` is only true for the full-stage backgrounds because of the
 * alpha scan in png.c -- before that they all claimed an alpha channel they
 * did not use, and this test would have failed for every one of them. The two
 * changes compose, and that is the order they had to be made in.
 *
 * The *extent* half looks redundant and is not. A background is placed by
 * mr_place_resolve with has_place = 0, which is bottom centre, so
 * x = (stage_w - w) / 2 -- and that truncates. For a background one pixel
 * *narrower* than the stage, x = (int)(0.5f * 1) = 0: the origin test passes
 * and column 1279 is covered by nothing at all. Drop the extent test and that
 * column keeps the previous frame, a one-pixel stale stripe down the right
 * edge. filltest pins it by shrinking a cached surface to 1279 and checking
 * the far column is not left unwritten; patching the extent test out takes it
 * red, and before that claim existed it stayed green.
 */
static int bg_covers_clip(const mr_surface *dst, const mr_surface *bg,
                          int x, int y)
{
    int cx, cy, cw, ch;

    if (!bg || bg->has_alpha)
        return 0;

    mr_clip_rect(dst, &cx, &cy, &cw, &ch);
    if (cw <= 0 || ch <= 0)
        return 1;              /* nothing to fill either way */

    return x <= cx && y <= cy &&
           x + bg->w >= cx + cw &&
           y + bg->h >= cy + ch;
}

static void render_list(mr_surface *dst, const mr_scenelist *l)
{
    int i, x = 0, y = 0;
    mr_surface *s;
    mr_surface *bg = NULL;

    /* The background is resolved before the fill rather than after it, so the
     * fill can be skipped when the background is about to cover it. Moving
     * the mr_image_get earlier is not a visible change: it decodes and pins,
     * and draws nothing. */
    if (l->bg[0]) {
        bg = mr_image_get(l->bg);
        if (bg)
            bg_pos(bg, &x, &y);
    }

    if (!bg_covers_clip(dst, bg, x, y))
        mr_surface_fill(dst, MR_RGB(0, 0, 0));

    if (bg)
        mr_blit(dst, bg, x, y);

    for (i = 0; i < l->n; i++) {
        s = shown_image(&l->v[i]);
        if (!s)
            continue;
        mr_place_resolve(&l->v[i].place, l->v[i].has_place, s->w, s->h, &x, &y);
        blit_shown(dst, s, &l->v[i], x, y);
    }

    draw_screens(dst, l);
}

/* Screens, in ascending zorder. A stable insertion pass rather than a sort:
 * there are at most a handful, and two screens with the same zorder must
 * keep the order they were shown in -- a confirm dialog over a menu screen
 * both sit at 0 and the dialog has to stay on top.
 *
 * This exists for `screen quick_menu(): zorder 100`, which has to draw over
 * a chapter title card that was shown after it. */
/* Screens at or below zorder 0. Anything above it is drawn later, over the
 * dialogue window, by mr_scene_render_overlay.
 *
 * The split exists because the say window is not a screen here -- the text
 * layer paints it after the scene -- so a screen that Ren'Py would put above
 * it by zorder would otherwise be painted under it. The quick menu is
 * `zorder 100` and sits along the bottom edge, right where the textbox art
 * is: drawn underneath, its #aaaaaa text came out #363636.
 *
 * Being outside the scene render also keeps an overlay out of transition
 * snapshots, which is right for the same reason the dialogue window is
 * excluded: it is persistent furniture above the effect, not part of it. */
static void draw_screens(mr_surface *dst, const mr_scenelist *l)
{
    int order[MR_MAX_SCREENS];
    int n = 0, i, j;

    if (!g_draw_screen)
        return;

    for (i = 0; i < l->nscreens; i++) {
        if (l->screen_z[i] > 0)
            continue;
        for (j = n; j > 0 && l->screen_z[order[j - 1]] > l->screen_z[i]; j--)
            order[j] = order[j - 1];
        order[j] = i;
        n++;
    }

    for (i = 0; i < n; i++)
        g_draw_screen(dst, l->screens[order[i]], g_live);
}

void mr_scene_render_overlay(mr_surface *dst)
{
    int order[MR_MAX_SCREENS];
    int n = 0, i, j;

    if (!g_draw_screen)
        return;

    for (i = 0; i < g_cur.nscreens; i++) {
        if (g_cur.screen_z[i] <= 0)
            continue;
        for (j = n; j > 0 &&
             g_cur.screen_z[order[j - 1]] > g_cur.screen_z[i]; j--)
            order[j] = order[j - 1];
        order[j] = i;
        n++;
    }

    /* Always the live list: an overlay is not part of a transition, so it is
     * drawn once over the finished composite rather than blended. */
    for (i = 0; i < n; i++)
        g_draw_screen(dst, g_cur.screens[order[i]], 1);
}

/* Find the entry with this tag, or -1. */
static int find_tag(const mr_scenelist *l, const char *tag)
{
    int i;
    for (i = 0; i < l->n; i++) {
        if (!strcmp(l->v[i].tag, tag))
            return i;
    }
    return -1;
}

static void resolve_entry(const mr_shown *e, int *x, int *y, int *w, int *h)
{
    mr_surface *s = shown_image(e);

    if (!s) {
        *x = *y = *w = *h = 0;
        return;
    }
    *w = s->w;
    *h = s->h;
    mr_place_resolve(&e->place, e->has_place, s->w, s->h, x, y);
}

/* The move family. Images present in both lists glide between their two
 * positions; images that only appear in one slide in from, or out to, the
 * edge the transition names.
 *
 * Geometry for every image is worked out before anything is drawn. That
 * matters because mr_image_get can evict, and free, a surface handed out
 * earlier: holding one pointer across a fetch of a different image is a
 * use-after-free. Each surface is therefore fetched immediately before its
 * own blit and never held across another lookup.
 */
/* Geometry only. The image to draw is looked up from the display-list entry
 * the slot corresponds to -- out_items[i] to g_prev.v[i], cur_items[i] to
 * g_cur.v[i] -- because a zoomed entry needs its placement as well as its
 * name to find the right pixels; see shown_image. */
typedef struct {
    int draw;
    int x, y;
} move_item;

static void render_move(mr_surface *dst, int t)
{
    int i, x, y, w, h, px, py, pw, ph, j;
    mr_surface *s;
    const mr_scenelist *l = &g_cur;
    move_item out_items[MR_MAX_SHOWN];
    move_item cur_items[MR_MAX_SHOWN];

    mr_surface_fill(dst, MR_RGB(0, 0, 0));

    /* Pass one: outgoing images, present before and gone now. */
    for (i = 0; i < g_prev.n; i++) {
        out_items[i].draw = 0;

        if (find_tag(l, g_prev.v[i].tag) >= 0)
            continue;

        resolve_entry(&g_prev.v[i], &px, &py, &pw, &ph);
        if (pw == 0)
            continue;

        switch (g_trans.kind) {
        case MR_TRANS_MOVEOUTRIGHT:
            px += (MR_STAGE_W - px) * t / 255;
            break;
        case MR_TRANS_MOVEOUTLEFT:
        case MR_TRANS_MOVEINLEFT:
            px -= (px + pw) * t / 255;
            break;
        case MR_TRANS_MOVEINRIGHT:
            px += (MR_STAGE_W - px) * t / 255;
            break;
        /* The vertical pair. `outbottom` is a leave_factory of
         * (None, 1.0, None, 0.0) -- ypos at the bottom of the screen with
         * the image's top edge as the anchor, so it ends at y = stage
         * height. `outtop` is (None, 0.0, None, 1.0): bottom edge at the top,
         * so it ends at -h. */
        case MR_TRANS_MOVEOUTBOTTOM:
            py += (MR_STAGE_H - py) * t / 255;
            break;
        case MR_TRANS_MOVEOUTTOP:
        case MR_TRANS_MOVEINTOP:
            py -= (py + ph) * t / 255;
            break;
        case MR_TRANS_MOVEINBOTTOM:
            py += (MR_STAGE_H - py) * t / 255;
            break;
        default:
            /* Plain `move` does not animate removals; drop it at the end. */
            if (t >= 255)
                continue;
            break;
        }

        out_items[i].draw = 1;
        out_items[i].x = px;
        out_items[i].y = py;
    }

    /* Pass two: current images. */
    for (i = 0; i < l->n; i++) {
        cur_items[i].draw = 0;

        resolve_entry(&l->v[i], &x, &y, &w, &h);
        if (w == 0)
            continue;

        j = find_tag(&g_prev, l->v[i].tag);

        /* The entry may have been the backdrop until this very statement:
         * the show that panned it took the slot over, so g_cur has an entry
         * where g_prev still has only its `bg`. Start from where the slot was
         * drawing it, which is the position the player is looking at.
         *
         * Without this the panned backdrop counts as newly shown and does
         * not animate at all -- the reported bug. The size is read straight
         * out of the image and not held: mr_image_get can evict and free a
         * surface handed out earlier, which is why nothing in this function
         * keeps one across another lookup. */
        if (j < 0 && g_prev.bg[0]) {
            char bgtag[MR_MAX_NAME];

            tag_of(g_prev.bg, bgtag, sizeof(bgtag));
            if (!strcmp(bgtag, l->v[i].tag)) {
                mr_surface *bs = mr_image_get(g_prev.bg);
                if (bs) {
                    /* Through bg_pos, the same function that draws the slot,
                     * so the glide starts exactly where the player is
                     * looking. Centring here instead would open the pan with
                     * a jump of (img_h - stage_h)/2 -- 370px on the tall
                     * backdrop this exists for. */
                    bg_pos(bs, &px, &py);
                    x = px + (x - px) * t / 255;
                    y = py + (y - py) * t / 255;
                    cur_items[i].draw = 1;
                    cur_items[i].x = x;
                    cur_items[i].y = y;
                    continue;
                }
            }
        }

        if (j >= 0) {
            /* Present in both: glide from the old position to the new. */
            resolve_entry(&g_prev.v[j], &px, &py, &pw, &ph);
            x = px + (x - px) * t / 255;
            y = py + (y - py) * t / 255;
        } else {
            /* Newly shown: slide in from the named edge. */
            switch (g_trans.kind) {
            case MR_TRANS_MOVEINLEFT:
                x -= (x + w) * (255 - t) / 255;
                break;
            case MR_TRANS_MOVEINRIGHT:
                x += (MR_STAGE_W - x) * (255 - t) / 255;
                break;
            /* `inbottom` enters from below: enter_factory
             * (None, 1.0, None, 0.0) starts the image's top edge at the
             * bottom of the screen. `intop` is the mirror, starting at -h. */
            case MR_TRANS_MOVEINBOTTOM:
                y += (MR_STAGE_H - y) * (255 - t) / 255;
                break;
            case MR_TRANS_MOVEINTOP:
                y -= (y + h) * (255 - t) / 255;
                break;
            default:
                break;
            }
        }

        cur_items[i].draw = 1;
        cur_items[i].x = x;
        cur_items[i].y = y;
    }

    /* Now draw, fetching each surface only as it is needed. */
    if (l->bg[0]) {
        s = mr_image_get(l->bg);
        if (s) {
            int bx, by;
            bg_pos(s, &bx, &by);
            mr_blit(dst, s, bx, by);
        }
    }

    for (i = 0; i < g_prev.n; i++) {
        if (!out_items[i].draw)
            continue;
        s = shown_image(&g_prev.v[i]);
        if (s)
            blit_shown(dst, s, &g_prev.v[i], out_items[i].x, out_items[i].y);
    }

    for (i = 0; i < l->n; i++) {
        if (!cur_items[i].draw)
            continue;
        s = shown_image(&l->v[i]);
        if (s)
            blit_shown(dst, s, &l->v[i], cur_items[i].x, cur_items[i].y);
    }

    draw_screens(dst, l);
}

/* Both sides of a dissolve or a fade.
 *
 * The dialogue window is deliberately *not* in either of them. It is not part
 * of the scene: it is a persistent element that sits above the effect and
 * carries straight through it, which is why the frame loop paints it after
 * the blend on every frame. Putting it in the snapshots made it cross-fade,
 * which is wrong -- the window's own coming and going is governed by `window
 * show` and `window hide`, not by whatever the scene is doing. */
static void take_snapshots(void)
{
    mr_image_begin_frame();

    render_list(g_from, &g_prev);
    mr_audio_poll();

    render_list(g_to, &g_cur);
    mr_audio_poll();

    g_snapped = 1;
}

void mr_scene_transition(const mr_transition *t, long now_ms)
{
    if (!t || t->kind == MR_TRANS_NONE || t->ms <= 0) {
        mr_scene_commit();
        return;
    }

    g_trans = *t;
    g_trans_start = now_ms;
    g_trans_active = 1;

    /* Snapshot the outgoing screen now, while g_prev still describes it.
     *
     * Deliberately *without* stepping the transforms to now_ms first. That
     * looks like the careful thing to do -- a transition starts from a
     * statement, so the last atl_update was up to a frame ago -- and it is
     * wrong: it puts the snapshot a partial step ahead of the frame the
     * player is actually looking at, and the dissolve opens with a visible
     * jump of exactly that much. Measured, it was the difference between
     * the first dissolve frame matching its predecessor to the pixel and
     * differing by 6400 of them. The render-time atl_update advances both
     * lists together just before compositing, which is what keeps them
     * honest. */
    if (g_trans.kind == MR_TRANS_DISSOLVE || g_trans.kind == MR_TRANS_FADE)
        take_snapshots();
}

void mr_scene_commit(void)
{
    g_prev = g_cur;
    g_trans_active = 0;
    g_snapped = 0;
}

/* Is anything on the scene still moving -- a transition, or a transform? */
int mr_scene_animating(long now_ms)
{
    int i;

    if (g_trans_active)
        return 1;

    for (i = 0; i < g_cur.n; i++) {
        const mr_shown *s = &g_cur.v[i];
        if (s->atl && (s->step < s->atl->n || s->atl->repeat))
            return 1;
    }

    /* An animated image moves without anything on the display list moving:
     * the name stands for a different set of pixels over time, and the
     * registry resolves that from the clock. Nothing else would ever ask for
     * a redraw, so without this the animation advances only when something
     * else happens to repaint -- which looks like an animation that plays
     * one frame per line of dialogue.
     *
     * The question is "would it look different now", not "is it animated".
     * The two are far apart: a frame is held for seconds and resolves to the
     * same cached surface the whole time, so the broad question has the loop
     * recompositing the stage to produce an identical image. See
     * mr_image_anim_changed. */
    if (mr_image_any_anim()) {
        if (g_cur.bg[0] && mr_image_anim_changed(g_cur.bg))
            return 1;
        for (i = 0; i < g_cur.n; i++) {
            if (mr_image_anim_changed(g_cur.v[i].name))
                return 1;
        }
    }

    (void)now_ms;
    return 0;
}

int mr_scene_in_transition(void)
{
    return g_trans_active;
}

/* Is the running transition one of the move family? They composite quite
 * differently from dissolve and fade -- both display lists re-rendered every
 * frame with per-image offsets, rather than two snapshots blended -- so
 * anything that has to hold true of every effect is worth checking against
 * one of each. */
int mr_scene_move_transition(void)
{
    if (!g_trans_active)
        return 0;
    return g_trans.kind == MR_TRANS_MOVE ||
           g_trans.kind == MR_TRANS_MOVEINLEFT ||
           g_trans.kind == MR_TRANS_MOVEINRIGHT ||
           g_trans.kind == MR_TRANS_MOVEOUTLEFT ||
           g_trans.kind == MR_TRANS_MOVEOUTRIGHT ||
           g_trans.kind == MR_TRANS_MOVEINTOP ||
           g_trans.kind == MR_TRANS_MOVEINBOTTOM ||
           g_trans.kind == MR_TRANS_MOVEOUTTOP ||
           g_trans.kind == MR_TRANS_MOVEOUTBOTTOM;
}

int mr_scene_busy(long now_ms)
{
    if (!g_trans_active)
        return 0;
    if (now_ms - g_trans_start >= g_trans.ms) {
        mr_scene_commit();
        return 0;
    }
    return 1;
}

void mr_scene_skip(void)
{
    if (g_trans_active)
        mr_scene_commit();
}

void mr_scene_render(mr_surface *stage, long now_ms)
{
    long elapsed;
    int t;

    /* Pin everything this pass touches, so a surface fetched early cannot be
     * freed by a later fetch of a different image. */
    mr_image_begin_frame();

    /* Step any running transform before anything is composited, so both the
     * live list and a transition snapshot see the same positions.
     *
     * Both lists, and that is the point. g_prev is a struct copy of g_cur
     * taken at the last commit, animation state included, but only g_cur
     * used to be advanced -- so an entry still moving sat frozen in g_prev
     * at wherever it had been when the commit happened. take_snapshots
     * renders g_prev as the outgoing image, so a dissolve starting while
     * something was moving faded *from* a position the player had left
     * behind seconds earlier.
     *
     * The credit roll showed it: open the game menu mid-scroll and close it
     * (both commit), and the dissolve onto the final card replayed the
     * credits from where the menu had covered them. Advancing g_prev keeps
     * the two in step -- same atl, same step_start, so an entry neither
     * statement touched lands on the same placement in both lists and the
     * transition does not move it. */
    if (atl_update(&g_cur, now_ms))
        g_snapped = 0;
    if (atl_update(&g_prev, now_ms))
        g_snapped = 0;

    if (!g_trans_active) {
        g_live = 1;
        render_list(stage, &g_cur);
        g_live = 0;
        return;
    }

    elapsed = now_ms - g_trans_start;
    if (elapsed >= g_trans.ms) {
        mr_scene_commit();
        g_live = 1;
        render_list(stage, &g_cur);
        g_live = 0;
        return;
    }

    t = (int)(elapsed * 255 / g_trans.ms);
    if (t < 0) t = 0;
    if (t > 255) t = 255;

    /* Compositing a transition is the most expensive thing this engine does:
     * two full-stage renders plus a blend over 921,600 pixels. The mixer is
     * normally refilled once per frame from the frame loop, which is not often
     * enough when one frame takes this long, so it is topped up between the
     * phases here. Interleaving is safe: mr_vfs_read re-seeks before every
     * read, so an audio read cannot disturb an in-flight image decode. */
    switch (g_trans.kind) {
    case MR_TRANS_DISSOLVE:
        if (!g_snapped)
            take_snapshots();
        mr_lerp_surface(stage, g_from, g_to, t);
        break;

    case MR_TRANS_FADE:
        /* Out to black over the first half, in from black over the second. */
        if (!g_snapped)
            take_snapshots();
        if (t < 128) {
            mr_surface_copy(stage, g_from);
            mr_fade_to(stage, g_trans.color, t * 2);
        } else {
            mr_surface_copy(stage, g_to);
            mr_fade_to(stage, g_trans.color, (255 - t) * 2);
        }
        break;

    case MR_TRANS_MOVE:
    case MR_TRANS_MOVEINLEFT:
    case MR_TRANS_MOVEINRIGHT:
    case MR_TRANS_MOVEOUTLEFT:
    case MR_TRANS_MOVEOUTRIGHT:
    case MR_TRANS_MOVEINTOP:
    case MR_TRANS_MOVEINBOTTOM:
    case MR_TRANS_MOVEOUTTOP:
    case MR_TRANS_MOVEOUTBOTTOM:
        render_move(stage, t);
        break;

    default:
        render_list(stage, &g_cur);
        break;
    }
}

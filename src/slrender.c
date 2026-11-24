/* slrender.c - laying out and drawing a screen language tree.
 *
 * Ren'Py sizes a displayable by rendering it into an available area and
 * letting it report back how much it used, then the parent places that result
 * according to the child's position properties. The same two passes happen
 * here: measure() reports a natural size, draw() places and paints.
 *
 * The one rule worth stating because everything depends on it: a position or
 * size given as a float is a fraction of the parent, while an integer is a
 * pixel count. `xalign 1.0` is the right-hand edge; `xoffset -20` is twenty
 * pixels left. That is why the value model keeps ints and floats distinct.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mr.h"
#include "slrender.h"
#include "list.h"
#include "slang.h"
#include "style.h"
#include "expr.h"
#include "image.h"
#include "vfs.h"      /* mr_vfs_exists, for the [prefix_] search */
#include "script.h"   /* MR_MAX_TEXT, MR_MAX_NAME */
#include "plat.h"     /* mr_plat_service, between widgets */
#include "lex.h"      /* mr_skip_ws, mr_read_number */
#include "musicroom.h"
#include "lex.h"      /* mr_read_string */
#include "tl.h"
#include "interp.h"   /* preference values behind a bar */
#include "save.h"     /* save-slot thumbnails behind FileScreenshot */

#define SL_MAX_DEPTH 24

/* The height a viewport offers its child: large enough that nothing wraps or
 * fills against it, small enough that a fraction of it stays sane. */
#define SL_UNBOUNDED 100000

static mr_font_provider g_font_for;
static mr_screen_lookup g_find_screen;

/* --- local scopes ---------------------------------------------------------
 *
 * A screen's locals are its `use` parameters, its `for` loop variables, and
 * whatever a `$` line inside it assigns. In Ren'Py all three are scope-local
 * rather than global, and that matters here rather than being a nicety: the
 * save screen writes `$ slot = i + 1` once per grid cell, and every cell's
 * FileAction must see its own value.
 *
 * Bindings nest, and the same subtree is walked twice (measure, then draw) and
 * once per loop iteration, so they cannot just be pushed and popped around a
 * single walk: the child list collect() produces outlives the call that built
 * it. So bindings live on a stack that the *caller* releases once it has
 * finished drawing the children it collected, and every collected child
 * records the environment it was collected in.
 */

#define SL_MAX_BIND 512
#define SL_MAX_XC   16

typedef struct {
    int bind;       /* head of the binding chain, or -1 */
    int xclude;     /* innermost transclude body, or -1 */
    /* `use` splices a screen's children into this one's child list, which
     * drops the root node and with it any style_prefix set at the top of the
     * used screen. `screen game_menu` opens with `style_prefix "game_menu"`
     * and every label and button in it depends on that, so the prefix travels
     * with the spliced children and is applied once, at the boundary. */
    char prefix[64];
    int  has_prefix;
} sl_env;

typedef struct {
    char     name[SL_MAX_PROPNAME];
    mr_value val;
    int      next;  /* the binding this one shadows, or -1 */
} sl_bind;

typedef struct {
    const sl_node *body;  /* a `use` node; its children are the body */
    sl_env         env;   /* the environment that body belongs to */
} sl_xc;

static sl_bind g_bind[SL_MAX_BIND];
static int     g_nbind;
static sl_xc   g_xc[SL_MAX_XC];
static int     g_nxc;
static sl_env  g_env;
static int     g_rendering;

static int bind_find(int head, const char *name)
{
    while (head >= 0) {
        if (!strcmp(g_bind[head].name, name))
            return head;
        head = g_bind[head].next;
    }
    return -1;
}

/* Push a binding, shadowing anything of the same name further out. */
static int bind_push(int *head, const char *name, const mr_value *v)
{
    if (g_nbind >= SL_MAX_BIND) {
        mr_log("slrender: scope stack full, dropping '%s'", name);
        return -1;
    }
    memset(&g_bind[g_nbind], 0, sizeof(g_bind[0]));
    strncpy(g_bind[g_nbind].name, name, SL_MAX_PROPNAME - 1);
    g_bind[g_nbind].val = *v;
    g_bind[g_nbind].next = *head;
    *head = g_nbind;
    return g_nbind++;
}

static int scope_get(const char *name, mr_value *out)
{
    int i = bind_find(g_env.bind, name);

    if (i < 0)
        return 0;
    *out = g_bind[i].val;
    return 1;
}

static int scope_set(const char *name, const mr_value *v)
{
    int i;

    /* Outside a screen walk there is no scope, so assignments are global. */
    if (!g_rendering)
        return 0;

    i = bind_find(g_env.bind, name);
    if (i >= 0) {
        g_bind[i].val = *v;
        return 1;
    }
    return bind_push(&g_env.bind, name, v) >= 0;
}

/* Set from MR_LAYOUT; see dump_node. */
static int g_dump_request;
static int g_dump;

/* The rectangle the screens actually painted this frame.
 *
 * Not the boxes they were *given*: a screen's root is a `fixed` covering the
 * whole stage and painting none of it, so allocated boxes would always say
 * "everywhere" and be useless. Only the places that mark pixels extend this.
 *
 * It exists for one decision. A screen on the scene normally forces a full
 * 1280x720 recomposite every frame, because a screen may draw anywhere -- and
 * the quick menu is up for the whole game, which would make every typed
 * character cost a full frame on a Pentium III. Knowing it painted rows
 * 699..720 is what lets the dialogue band keep covering it. */
static int g_ext_x0, g_ext_y0, g_ext_x1, g_ext_y1;

void mr_sl_extent_reset(void)
{
    g_ext_x0 = g_ext_y0 = 0;
    g_ext_x1 = g_ext_y1 = -1;
}

static void ext_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (g_ext_x1 < g_ext_x0) {          /* first painted thing */
        g_ext_x0 = x; g_ext_y0 = y;
        g_ext_x1 = x + w; g_ext_y1 = y + h;
        return;
    }
    if (x < g_ext_x0) g_ext_x0 = x;
    if (y < g_ext_y0) g_ext_y0 = y;
    if (x + w > g_ext_x1) g_ext_x1 = x + w;
    if (y + h > g_ext_y1) g_ext_y1 = y + h;
}

int mr_sl_extent(int *x, int *y, int *w, int *h)
{
    if (g_ext_x1 < g_ext_x0)
        return 0;
    *x = g_ext_x0; *y = g_ext_y0;
    *w = g_ext_x1 - g_ext_x0;
    *h = g_ext_y1 - g_ext_y0;
    return 1;
}
static int g_stats;

void mr_sl_stats(int on)
{
    g_stats = on;
}

void mr_sl_dump_next(void)
{
    g_dump_request = 1;
}

void mr_sl_init(void)
{
    memset(&g_env, 0, sizeof(g_env));
    g_env.bind = -1;
    g_env.xclude = -1;
    mr_expr_set_scope(scope_get, scope_set);
}

/* --- screen variables ------------------------------------------------------
 *
 * `default device = "keyboard"` and `SetScreenVariable("device", "mouse")`
 * are per-screen state that has to live between frames. The ordinary scope
 * cannot hold it: that is rebuilt from nothing on every render, so a
 * `default` would reset the value each frame and clicking "Souris" would
 * never stick. These persist until the screen is shown afresh.
 */

#define SL_MAX_SCREENVAR 24

typedef struct {
    char     screen[64];
    char     name[SL_MAX_PROPNAME];
    mr_value value;
    int      used;
} sl_screenvar;

static sl_screenvar g_svars[SL_MAX_SCREENVAR];
static char         g_screen_name[64];   /* the screen being rendered */

static sl_screenvar *screenvar_find(const char *name)
{
    int i;

    for (i = 0; i < SL_MAX_SCREENVAR; i++) {
        if (g_svars[i].used && !strcmp(g_svars[i].screen, g_screen_name) &&
            !strcmp(g_svars[i].name, name))
            return &g_svars[i];
    }
    return NULL;
}

static sl_screenvar *screenvar_add(const char *name)
{
    int i;

    for (i = 0; i < SL_MAX_SCREENVAR; i++) {
        if (!g_svars[i].used) {
            g_svars[i].used = 1;
            strncpy(g_svars[i].screen, g_screen_name, sizeof(g_svars[i].screen) - 1);
            g_svars[i].screen[sizeof(g_svars[i].screen) - 1] = '\0';
            strncpy(g_svars[i].name, name, SL_MAX_PROPNAME - 1);
            g_svars[i].name[SL_MAX_PROPNAME - 1] = '\0';
            memset(&g_svars[i].value, 0, sizeof(g_svars[i].value));
            return &g_svars[i];
        }
    }
    mr_log("slrender: no room for screen variable '%s'", name);
    return NULL;
}

void mr_sl_screen_vars_clear(const char *screen)
{
    int i;

    for (i = 0; i < SL_MAX_SCREENVAR; i++) {
        if (g_svars[i].used && !strcmp(g_svars[i].screen, screen))
            g_svars[i].used = 0;
    }
}

/* Set a variable on a screen that is not the one being rendered. Screen
 * *arguments* arrive this way: Confirm() has to bind `message`, `yes_action`
 * and `no_action` before the confirm screen is drawn for the first time. */
int mr_sl_set_screen_var_for(const char *screen, const char *name,
                             const mr_value *v)
{
    char saved[64];
    int rv;

    strcpy(saved, g_screen_name);
    strncpy(g_screen_name, screen, sizeof(g_screen_name) - 1);
    g_screen_name[sizeof(g_screen_name) - 1] = '\0';
    rv = mr_sl_set_screen_var(name, v);
    strcpy(g_screen_name, saved);
    return rv;
}

int mr_sl_set_screen_var(const char *name, const mr_value *v)
{
    sl_screenvar *sv;

    if (!g_screen_name[0])
        return 0;

    sv = screenvar_find(name);
    if (!sv)
        sv = screenvar_add(name);
    if (!sv)
        return 0;

    sv->value = *v;
    return 1;
}

/* Focusable rectangles recorded during the last interactive render. */
#define SL_MAX_FOCUS 64

/* How much of a button's scope is remembered for its action. Actions read at
 * most a loop variable or two (`FileAction(slot)`, `FilePage(page)`). */
#define SL_FOCUS_BIND 4

/* Not everything focusable runs an action: a bar sets a preference from where
 * it was clicked, and a scrollbar moves a viewport. */
#define SL_FOCUS_BUTTON 0
#define SL_FOCUS_BAR    1
#define SL_FOCUS_SCROLL 2

typedef struct {
    int  kind;              /* SL_FOCUS_* */
    int  x, y, w, h;
    char action[224];       /* BUTTON: the action expression */

    /* BAR: the preference it drives, and whether it runs top to bottom. */
    char pref[64];
    int  vertical;

    /* SCROLL: which viewport, the numbers the thumb was sized from, and the
     * viewport's own horizontal extent so the wheel can work over all of it
     * rather than only over the bar. */
    const sl_node *vp;
    int  content, view;
    int  vx, vw;

    /* The scope the action must be evaluated in, innermost first. */
    char     bname[SL_FOCUS_BIND][SL_MAX_PROPNAME];
    mr_value bval[SL_FOCUS_BIND];
    int      nb;
} sl_focus;

static sl_focus g_focus[SL_MAX_FOCUS];
static int      g_nfocus;
static int      g_capture;
static int      g_hover = -1;
static sl_env   g_focus_save;
static int      g_focus_mark;

/* Defined with the rest of the viewport machinery further down. */
typedef struct sl_scroll sl_scroll;
static sl_scroll *scroll_for(const sl_node *n);
static int scroll_clamp(int pos, int content, int view);
static void scroll_set(sl_scroll *sc, int pos);
static int scroll_get(const sl_scroll *sc);

/* A `modal True` screen swallows everything under it. The focus list is
 * built in drawing order, so the modal screen's own entries are all at or
 * after the point where it started drawing -- remembering that index is
 * enough to stop a hit test from reaching the screen behind, which is what
 * modality means. Reset per frame along with the list itself. */
static int g_modal_floor;

/* Defined with the bar machinery further down. */
static void livebar_reset(void);

/* --- key bindings ---------------------------------------------------------
 *
 * A `key` node is not drawn and is not focusable; it binds a keysym for as
 * long as its screen is on display. The table is rebuilt every frame beside
 * the focus list, for the same reason: what is bound is whatever was laid
 * out last, and a screen that has gone away binds nothing.
 */
#define SL_MAX_KEYS 16

typedef struct {
    char sym[32];
    char action[160];
} sl_keybind;

static sl_keybind g_keys[SL_MAX_KEYS];
static int        g_nkeys;

static void key_bind(const char *sym, const char *action)
{
    int i;

    /* Later wins: a modal dialog's binding sits on top of the menu's, which
     * is the order they are drawn in. */
    for (i = 0; i < g_nkeys; i++) {
        if (!strcmp(g_keys[i].sym, sym))
            break;
    }
    if (i == g_nkeys) {
        if (g_nkeys >= SL_MAX_KEYS)
            return;
        g_nkeys++;
    }
    strncpy(g_keys[i].sym, sym, sizeof(g_keys[i].sym) - 1);
    g_keys[i].sym[sizeof(g_keys[i].sym) - 1] = '\0';
    strncpy(g_keys[i].action, action, sizeof(g_keys[i].action) - 1);
    g_keys[i].action[sizeof(g_keys[i].action) - 1] = '\0';
}

const char *mr_sl_key_action(const char *sym)
{
    int i;

    if (!sym)
        return NULL;
    for (i = 0; i < g_nkeys; i++)
        if (!strcmp(g_keys[i].sym, sym))
            return g_keys[i].action;
    return NULL;
}

void mr_sl_focus_reset(void)
{
    g_nfocus = 0;
    g_nkeys = 0;
    g_modal_floor = 0;

    /* The bar list describes the frame that is about to be drawn, so it is
     * cleared here and not when a frame is skipped: a skipped frame leaves
     * the last drawn frame's bars in place, which is exactly what the
     * comparison in mr_sl_bars_moved is against. */
    livebar_reset();
}

int mr_sl_modal_active(void)
{
    return g_modal_floor > 0;
}

int mr_sl_hit(int sx, int sy)
{
    int i;

    /* Back to front: the last thing drawn is on top. */
    for (i = g_nfocus - 1; i >= g_modal_floor; i--) {
        if (sx >= g_focus[i].x && sx < g_focus[i].x + g_focus[i].w &&
            sy >= g_focus[i].y && sy < g_focus[i].y + g_focus[i].h)
            return i;
    }
    return -1;
}

const char *mr_sl_focus_action(int id)
{
    if (id < 0 || id >= g_nfocus)
        return NULL;
    if (g_focus[id].kind != SL_FOCUS_BUTTON || !g_focus[id].action[0])
        return NULL;
    return g_focus[id].action;
}

int mr_sl_focus_draggable(int id)
{
    if (id < 0 || id >= g_nfocus)
        return 0;
    return g_focus[id].kind == SL_FOCUS_BAR ||
           g_focus[id].kind == SL_FOCUS_SCROLL;
}

/* A press or a drag on a bar or a scrollbar. Both work the same way: the
 * pointer's position along the control decides the value. */
void mr_sl_focus_drag(int id, int sx, int sy)
{
    sl_focus *f;

    if (id < 0 || id >= g_nfocus)
        return;
    f = &g_focus[id];

    if (f->kind == SL_FOCUS_BAR) {
        float frac;
        if (f->vertical)
            frac = f->h > 0 ? (float)(f->y + f->h - sy) / (float)f->h : 0.0f;
        else
            frac = f->w > 0 ? (float)(sx - f->x) / (float)f->w : 0.0f;
        mr_interp_pref_set_fraction(f->pref, (double)frac);
        return;
    }

    if (f->kind == SL_FOCUS_SCROLL) {
        sl_scroll *sc = scroll_for(f->vp);
        float frac = f->h > 0 ? (float)(sy - f->y) / (float)f->h : 0.0f;
        if (!sc)
            return;
        /* Centre the thumb on the pointer rather than putting its top
         * there, which is what makes dragging feel right. */
        scroll_set(sc, scroll_clamp((int)(frac * (float)f->content) -
                                    f->view / 2, f->content, f->view));
    }
}

/* Scroll the viewport under a point, for a mouse wheel. Returns 1 if one was
 * found, so the caller knows the wheel was consumed. */
int mr_sl_scroll_at(int sx, int sy, int delta)
{
    int i;

    for (i = g_nfocus - 1; i >= 0; i--) {
        sl_focus *f = &g_focus[i];
        if (f->kind != SL_FOCUS_SCROLL)
            continue;
        /* The wheel works over the whole viewport, not just its bar. */
        if (sx >= f->vx && sx < f->vx + f->vw &&
            sy >= f->y && sy < f->y + f->h) {
            sl_scroll *sc = scroll_for(f->vp);
            if (sc) {
                scroll_set(sc, scroll_clamp(scroll_get(sc) - delta * 40,
                                            f->content, f->view));
                return 1;
            }
        }
    }
    return 0;
}

/* Record a button, returning its focus id (or -1 when not capturing). */
static int action_sensitive(const sl_node *n);

static int add_focus(const sl_node *n, int x, int y, int w, int h)
{
    const char *act;
    sl_focus *f;
    int b;

    if (!g_capture || g_nfocus >= SL_MAX_FOCUS)
        return -1;

    act = sl_node_prop(n, "action");
    if (!act || !*act)
        return -1;

    /* An insensitive button is still drawn -- greyed -- but is not in the
     * focus list at all, so it cannot be hovered and a click on it falls
     * through to whatever is underneath. That is Ren'Py's behaviour and it
     * is also what keeps `dismiss` working: a click on a dead quick-menu
     * button should do nothing rather than be quietly eaten. */
    if (!action_sensitive(n))
        return -1;

    f = &g_focus[g_nfocus];
    memset(f, 0, sizeof(*f));
    f->kind = SL_FOCUS_BUTTON;
    f->x = x;
    f->y = y;
    f->w = w;
    f->h = h;
    strncpy(f->action, act, sizeof(f->action) - 1);
    f->action[sizeof(f->action) - 1] = '\0';

    /* The action text is evaluated on click, long after the binding stack that
     * gave it meaning has been released, so flatten the scope into the entry. */
    f->nb = 0;
    for (b = g_env.bind; b >= 0 && f->nb < SL_FOCUS_BIND; b = g_bind[b].next) {
        strcpy(f->bname[f->nb], g_bind[b].name);
        f->bval[f->nb] = g_bind[b].val;
        f->nb++;
    }

    return g_nfocus++;
}

/* Make a button's captured scope current, so its action expression sees the
 * loop variables it was written against. Paired with mr_sl_focus_scope_end. */
void mr_sl_focus_scope(int id)
{
    int i;

    g_focus_save = g_env;
    g_focus_mark = g_nbind;

    if (id < 0 || id >= g_nfocus)
        return;

    /* Innermost was stored first, so push in reverse to rebuild the chain. */
    g_env.bind = -1;
    for (i = g_focus[id].nb - 1; i >= 0; i--)
        bind_push(&g_env.bind, g_focus[id].bname[i], &g_focus[id].bval[i]);
}

void mr_sl_focus_scope_end(void)
{
    g_env = g_focus_save;
    g_nbind = g_focus_mark;
}

void mr_sl_set_font_provider(mr_font_provider fn) { g_font_for = fn; }
void mr_sl_set_screen_lookup(mr_screen_lookup fn) { g_find_screen = fn; }

/* A resolved numeric property: either a pixel count or a fraction. */
typedef struct {
    int   present;
    int   is_frac;
    float f;
    long  i;
} sl_num;

/* Context handed down the tree. */
typedef struct {
    char prefix[64];     /* style_prefix in force */
    int  depth;
} sl_ctx;

/* --- property access --- */

/* The default style for each displayable, from sldisplayables.py. */
static const char *default_style(const char *type)
{
    if (!strcmp(type, "text"))        return "text";
    if (!strcmp(type, "hbox"))        return "hbox";
    if (!strcmp(type, "vbox"))        return "vbox";
    if (!strcmp(type, "fixed"))       return "fixed";
    if (!strcmp(type, "grid"))        return "grid";
    if (!strcmp(type, "side"))        return "side";
    if (!strcmp(type, "window"))      return "window";
    if (!strcmp(type, "frame"))       return "frame";
    if (!strcmp(type, "button"))      return "button";
    if (!strcmp(type, "textbutton"))  return "button";
    if (!strcmp(type, "imagebutton")) return "image_button";
    if (!strcmp(type, "label"))       return "label";
    if (!strcmp(type, "viewport"))    return "viewport";
    if (!strcmp(type, "vpgrid"))      return "vpgrid";
    if (!strcmp(type, "input"))       return "input";
    if (!strcmp(type, "bar"))         return "bar";
    if (!strcmp(type, "vbar"))        return "vbar";
    if (!strcmp(type, "slider"))      return "slider";
    if (!strcmp(type, "vslider"))     return "vslider";
    if (!strcmp(type, "scrollbar"))   return "scrollbar";
    if (!strcmp(type, "vscrollbar"))  return "vscrollbar";
    return "default";
}

/* A bar is the one displayable whose default style does not come from its
 * keyword. `sl2bar` in sldisplayables.py asks `value.get_style()` whenever the
 * screen gives no explicit style, so what a bar looks like depends on what it
 * is bound to:
 *
 *   BarValue        -> "bar"       (the base class, and __GenericValue's
 *                                   default, so FieldValue and friends)
 *   MixerValue      -> "slider"    (the volume bars)
 *   XScrollValue    -> "scrollbar"
 *   __GenericValue  -> whatever its `style=` argument said
 *
 * Every Preference() that yields a bar value ends up on "slider", either as a
 * MixerValue or as FieldValue(..., style="slider"). That matters because
 * `style slider_slider: xsize 350` is where the preference sliders get their
 * width: read as plain `bar` they inherit no width at all and stretch to fill
 * the row, which is what made ours noticeably wider than Ren'Py's.
 *
 * The jukebox's AudioPositionValue does not override get_style, so it stays on
 * "bar" and keeps filling its viewport -- which is the behaviour the sizing
 * comment in measure() is protecting, and the reason this is fixed here rather
 * than by giving bars a default width.
 *
 * We cannot call get_style, so the value's constructor name stands in for its
 * class. An explicit `style=` inside the value wins first, since that is the
 * one case where the script states the answer outright. */
static const char *bar_base_style(const sl_node *n)
{
    static const struct { const char *fn; const char *style; } map[] = {
        { "Preference",   "slider"    },
        { "MixerValue",   "slider"    },
        { "XScrollValue", "scrollbar" },
        { "YScrollValue", "scrollbar" }
    };
    static char buf[MR_MAX_NAME];
    const char *v = sl_node_prop(n, "value");
    const char *s;
    int i;

    if (!v)
        return "bar";

    /* `style="..."` anywhere in the argument list. */
    s = strstr(v, "style=");
    if (!s)
        s = strstr(v, "style =");
    if (s) {
        s = strchr(s, '"');
        if (s && mr_read_string(s, buf, sizeof(buf)) && buf[0])
            return buf;
    }

    while (*v == ' ' || *v == '\t')
        v++;
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        size_t len = strlen(map[i].fn);
        if (!strncmp(v, map[i].fn, len) && v[len] == '(')
            return map[i].style;
    }
    return "bar";
}

/* Work out the style name for a node: an explicit `style` wins, otherwise the
 * displayable's default style, prefixed by any style_prefix in force. */
static void node_style(const sl_node *n, const sl_ctx *ctx,
                       char *out, int outsz)
{
    const char *e = sl_node_prop(n, "style");
    const char *base;

    if (e) {
        mr_value v;
        mr_expr_eval(e, &v);
        if (v.type == MR_V_STR && v.s[0]) {
            strncpy(out, v.s, (size_t)outsz - 1);
            out[outsz - 1] = '\0';
            return;
        }
    }

    base = default_style(n->type);

    /* Ren'Py builds the vertical name by prefixing a "v", which is why the
     * lookup above answers for both orientations. */
    if (!strcmp(n->type, "bar")) {
        base = bar_base_style(n);
    } else if (!strcmp(n->type, "vbar")) {
        static char vbuf[MR_MAX_NAME];
        sprintf(vbuf, "v%s", bar_base_style(n));
        base = vbuf;
    }

    /* A style_prefix applies to the displayable that carries it as well as to
     * its children, which is why `style_prefix "radio"` on a vbox is what
     * gives that vbox the radio_vbox style and its xsize. */
    {
        const char *own = sl_node_prop(n, "style_prefix");
        if (own) {
            mr_value v;
            mr_expr_eval(own, &v);
            if (v.type == MR_V_STR && v.s[0]) {
                sprintf(out, "%.30s_%.28s", v.s, base);
                return;
            }
        }
    }

    if (ctx->prefix[0])
        sprintf(out, "%.30s_%.28s", ctx->prefix, base);
    else {
        strncpy(out, base, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
    }
}

/* Look a property up on the node first, then on its style. */
static const char *prop_expr(const sl_node *n, const char *style,
                             const char *state, const char *name)
{
    const char *e = n ? sl_node_prop(n, name) : NULL;

    if (e)
        return e;
    return mr_style_get_state(style, state, name);
}

static sl_num num_of(const char *e)
{
    sl_num r;
    mr_value v;

    memset(&r, 0, sizeof(r));
    if (!e || !*e)
        return r;

    mr_expr_eval(e, &v);
    if (v.type == MR_V_INT) {
        r.present = 1;
        r.i = v.i;
        r.f = (float)v.i;
    } else if (v.type == MR_V_FLOAT) {
        r.present = 1;
        r.is_frac = 1;
        r.f = (float)v.f;
        r.i = (long)v.f;
    }
    return r;
}

static sl_num get_num(const sl_node *n, const char *style, const char *state,
                      const char *name)
{
    return num_of(prop_expr(n, style, state, name));
}

/* A string-valued property, copied into the caller's buffer. Returns 1 if one
 * was found and it evaluated to a string.
 *
 * The mr_value is static rather than automatic on purpose: it is MR_VSTR_MAX
 * plus change, over a kilobyte, and this is called from inside the per-node
 * render path where the stack is already deep. See NOTES on the stack. The
 * result is copied out before anything else can run, and there is no thread
 * and no recursion through here. */
static int get_str(const sl_node *n, const char *style, const char *state,
                   const char *name, char *out, int outsz)
{
    static mr_value v;
    const char *e = prop_expr(n, style, state, name);

    out[0] = '\0';
    if (!e || !*e)
        return 0;

    mr_expr_eval(e, &v);
    if (v.type != MR_V_STR || !v.s[0])
        return 0;

    strncpy(out, v.s, (size_t)outsz - 1);
    out[outsz - 1] = '\0';
    return 1;
}

/* Resolve a size-like property against the space available. */
static int num_px(sl_num v, int avail, int dflt)
{
    if (!v.present)
        return dflt;
    if (v.is_frac)
        return (int)(v.f * (float)avail);
    return (int)v.i;
}

/* `area (0, 42, 921, 478)`, which Ren'Py describes as setting xpos, ypos,
 * both anchors, and the minimum and maximum sizes all at once: the
 * displayable's top-left corner goes where it says and it is exactly that
 * big. The jukebox lays its three viewports over one another with it.
 *
 * The tuple is read from the property's source text rather than evaluated,
 * because the expression evaluator has no tuples. Every use of `area` is four
 * literals, and anything else is logged and ignored. */
static int node_area(const sl_node *n, const char *style, int out[4])
{
    const char *e = prop_expr(n, style, NULL, "area");
    int i;

    if (!e || !*e)
        return 0;

    e = mr_skip_ws(e);
    if (*e == '(')
        e++;

    for (i = 0; i < 4; i++) {
        double v;
        const char *next = mr_read_number(e, &v);

        if (!next) {
            mr_log("screen: area needs four numbers, got %.40s",
                   prop_expr(n, style, NULL, "area"));
            return 0;
        }
        out[i] = (int)v;
        e = mr_skip_ws(next);
        if (*e == ',')
            e = mr_skip_ws(e + 1);
    }
    return 1;
}

static int truthy_prop(const sl_node *n, const char *style, const char *name)
{
    const char *e = prop_expr(n, style, NULL, name);
    mr_value v;

    if (!e || !*e)
        return 0;
    mr_expr_eval(e, &v);
    return mr_value_truthy(&v);
}

static void str_prop(const sl_node *n, const char *style, const char *state,
                     const char *name, char *out, int outsz)
{
    const char *e = prop_expr(n, style, state, name);
    mr_value v;

    out[0] = '\0';
    if (!e || !*e)
        return;
    mr_expr_eval(e, &v);
    if (v.type == MR_V_STR)
        strncpy(out, v.s, (size_t)outsz - 1);
    out[outsz - 1] = '\0';
}

/* An image-valued property. These are written as `Frame("path", borders)` or
 * `Image("path")` or a bare string, and the constructors evaluate to nothing
 * here, so the path is lifted out of the expression text. `[prefix_]` inside
 * it stands for the style prefix search state, as in
 * `gui/slider/horizontal_[prefix_]bar.png`. */
/* `Frame(`, `Image(`, `Solid(` ... -- a capitalised identifier applied to
 * arguments, which is how every Ren'Py displayable constructor is written and
 * how no plain expression is. */
static int is_ctor_call(const char *e)
{
    const char *p = mr_skip_ws(e);

    if (*p < 'A' || *p > 'Z')
        return 0;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9') || *p == '_')
        p++;
    p = mr_skip_ws(p);
    return *p == '(';
}

static void image_prop(const sl_node *n, const char *style, const char *state,
                       const char *name, char *out, int outsz)
{
    const char *e = prop_expr(n, style, state, name);
    const char *tag;
    mr_value v;

    out[0] = '\0';
    if (!e || !*e)
        return;

    /* A displayable constructor is not evaluated. `Frame`, `Image` and the
     * rest are not functions the evaluator knows, so evaluating one only ever
     * returned None and then the path was lifted out of the text below
     * anyway -- but it also logged "unhandled call Frame(), result None"
     * every time, and mr_log reopens log.txt per line. Opening the main menu
     * wrote 230 of those, which on the target is 230 open-write-close cycles
     * on FAT for a result that was discarded. Skipping the call changes
     * nothing about what is drawn.
     *
     * The test is "a capitalised identifier followed by (", which is every
     * Ren'Py displayable constructor and no ordinary expression; a bare
     * "path.png" or a variable still goes through the evaluator as before. */
    if (!is_ctor_call(e)) {
        mr_expr_eval(e, &v);
        if (v.type == MR_V_STR && v.s[0]) {
            strncpy(out, v.s, (size_t)outsz - 1);
            out[outsz - 1] = '\0';
        }
    }

    if (!out[0]) {
        /* Not a plain string: take the first string literal in the call. */
        const char *q = strchr(e, '"');
        int n2 = 0;
        if (!q)
            q = strchr(e, '\'');
        if (!q)
            return;
        q++;
        while (*q && *q != '"' && *q != '\'' && n2 < outsz - 1)
            out[n2++] = *q++;
        out[n2] = '\0';
    }

    /* Substitute [prefix_]. Ren'Py does not simply drop the state in: it
     * tries a *list* of prefixes and takes the first that names a file that
     * exists (PREFIX_SEARCH in scripts/generate_styles.py, used by
     * easy.py:displayable). That is what lets one file serve several states.
     * The radio button art is exactly this case -- `radio_[prefix_]
     * foreground.png` with only radio_foreground.png and
     * radio_selected_foreground.png on disk, so idle and hover share the
     * first and both selected states share the second.
     *
     * The exact state is always first in its list, so art that does spell
     * every state out -- the slider's horizontal_idle_bar.png and
     * horizontal_hover_bar.png -- is picked up as before. */
    tag = strstr(out, "[prefix_]");
    if (tag) {
        static const struct { const char *state; const char *search[6]; } pfx[] = {
            { "idle_",           { "idle_", "", NULL } },
            { "hover_",          { "hover_", "", NULL } },
            { "insensitive_",    { "insensitive_", "", "idle_", NULL } },
            { "selected_idle_",  { "selected_idle_", "selected_", "",
                                   "idle_", NULL } },
            { "selected_hover_", { "selected_hover_", "hover_", "selected_",
                                   "", NULL } },
            { "",                { "", NULL } }
        };
        const char *st = state ? state : "";
        char head[MR_MAX_NAME];
        char tail[MR_MAX_NAME];
        char buf[MR_MAX_NAME];
        const char *const *search = NULL;
        int i;

        if ((int)(tag - out) >= (int)sizeof(head))
            return;
        memcpy(head, out, (size_t)(tag - out));
        head[tag - out] = '\0';
        strncpy(tail, tag + 9, sizeof(tail) - 1);
        tail[sizeof(tail) - 1] = '\0';

        for (i = 0; i < (int)(sizeof(pfx) / sizeof(pfx[0])); i++) {
            if (!strcmp(pfx[i].state, st)) {
                search = pfx[i].search;
                break;
            }
        }

        /* Unknown state: behave as before and just substitute it. */
        if (!search) {
            sprintf(buf, "%s%s%s", head, st, tail);
            strncpy(out, buf, (size_t)outsz - 1);
            out[outsz - 1] = '\0';
            return;
        }

        for (i = 0; search[i]; i++) {
            sprintf(buf, "%s%s%s", head, search[i], tail);
            if (mr_vfs_exists(buf)) {
                strncpy(out, buf, (size_t)outsz - 1);
                out[outsz - 1] = '\0';
                return;
            }
        }

        /* Nothing on disk. Answer the plain state, so a missing file is
         * reported against the name the style actually asked for. */
        sprintf(buf, "%s%s%s", head, st, tail);
        strncpy(out, buf, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
    }
}

/* Repeat an image to cover a rectangle. The bar and scrollbar art is a single
 * strip meant to be stretched; tiling covers the same ground without a
 * resampler, and the pieces are solid colour so it looks identical. */
static void draw_tiled(mr_surface *dst, mr_surface *img,
                       int x, int y, int w, int h)
{
    int yy, xx;

    if (!img || w <= 0 || h <= 0)
        return;

    mr_clip_push(x, y, w, h);
    for (yy = 0; yy < h; yy += img->h)
        for (xx = 0; xx < w; xx += img->w)
            mr_blit(dst, img, x + xx, y + yy);
    mr_clip_pop();
}

static mr_u32 color_prop(const sl_node *n, const char *style,
                         const char *state, const char *name, mr_u32 dflt)
{
    char buf[32];
    unsigned r = 0, g = 0, b = 0;

    str_prop(n, style, state, name, buf, sizeof(buf));
    if (buf[0] != '#' || strlen(buf) < 7)
        return dflt;
    sscanf(buf + 1, "%2x%2x%2x", &r, &g, &b);
    return MR_RGB(r, g, b);
}

/* Padding, from `padding` ("l t r b" as flattened by Borders) or the
 * individual properties. */
static void get_padding(const sl_node *n, const char *style, int *pad)
{
    const char *e;
    sl_num v;

    pad[0] = pad[1] = pad[2] = pad[3] = 0;

    e = prop_expr(n, style, NULL, "padding");
    if (e) {
        mr_value pv;
        char base[160];
        int n_e = (int)strlen(e);

        /* `padding gui.confirm_frame_borders.padding`: a Borders object's
         * .padding is the same four numbers as the borders themselves, and
         * Borders() leaves those in a string, so drop the suffix and read
         * the object. */
        if (n_e > 8 && !strcmp(e + n_e - 8, ".padding") &&
            n_e - 8 < (int)sizeof(base)) {
            memcpy(base, e, (size_t)(n_e - 8));
            base[n_e - 8] = '\0';
            e = base;
        }

        mr_expr_eval(e, &pv);
        if (pv.type == MR_V_STR)
            sscanf(pv.s, "%d %d %d %d", &pad[0], &pad[1], &pad[2], &pad[3]);
    }

    /* xpadding/ypadding set a pair at once, as xmargin/ymargin do. The save
     * screen's page label is sized entirely by `xpadding 50`. */
    v = get_num(n, style, NULL, "xpadding");
    if (v.present) { pad[0] = (int)v.i; pad[2] = (int)v.i; }
    v = get_num(n, style, NULL, "ypadding");
    if (v.present) { pad[1] = (int)v.i; pad[3] = (int)v.i; }

    v = get_num(n, style, NULL, "left_padding");   if (v.present) pad[0] = (int)v.i;
    v = get_num(n, style, NULL, "top_padding");    if (v.present) pad[1] = (int)v.i;
    v = get_num(n, style, NULL, "right_padding");  if (v.present) pad[2] = (int)v.i;
    v = get_num(n, style, NULL, "bottom_padding"); if (v.present) pad[3] = (int)v.i;
}

/* Margin is the space *outside* a displayable's background, where padding is
 * the space inside it. The distinction matters: game_menu_content_frame's
 * `left_margin 80` is what puts the preferences content at x=360 rather than
 * hard against the 280px navigation column, and `pref_label`'s top and bottom
 * margins are the gap between a slider's caption and the slider. */
static void get_margin(const sl_node *n, const char *style, int *mg)
{
    const char *e;
    sl_num v;

    mg[0] = mg[1] = mg[2] = mg[3] = 0;

    e = prop_expr(n, style, NULL, "margin");
    if (e) {
        mr_value pv;
        mr_expr_eval(e, &pv);
        if (pv.type == MR_V_STR)
            sscanf(pv.s, "%d %d %d %d", &mg[0], &mg[1], &mg[2], &mg[3]);
    }

    v = get_num(n, style, NULL, "xmargin");
    if (v.present) { mg[0] = (int)v.i; mg[2] = (int)v.i; }
    v = get_num(n, style, NULL, "ymargin");
    if (v.present) { mg[1] = (int)v.i; mg[3] = (int)v.i; }

    v = get_num(n, style, NULL, "left_margin");   if (v.present) mg[0] = (int)v.i;
    v = get_num(n, style, NULL, "top_margin");    if (v.present) mg[1] = (int)v.i;
    v = get_num(n, style, NULL, "right_margin");  if (v.present) mg[2] = (int)v.i;
    v = get_num(n, style, NULL, "bottom_margin"); if (v.present) mg[3] = (int)v.i;
}

/* --- text --- */

static const mr_font *font_for_node(const sl_node *n, const char *style)
{
    sl_num sz = get_num(n, style, NULL, "size");
    int px = sz.present ? (int)sz.i : 22;
    char face[64];

    /* The `font` property is a string expression -- style.c's expand_text
     * stores gui.<kind>_text_font's *value*, already quoted. Evaluating it
     * rather than taking the expression text verbatim keeps a `font
     * gui.text_font` written directly on a node working too. */
    get_str(n, style, NULL, "font", face, (int)sizeof(face));

    return g_font_for ? g_font_for(face[0] ? face : NULL, px) : NULL;
}

/* Resolve a node's text content. */
static void node_text(const sl_node *n, char *out, int outsz)
{
    char raw[MR_MAX_TEXT];
    const char *src = n->arg;
    mr_value v;

    out[0] = '\0';

    /* An `input` has no positional argument: what it shows is its `value`,
     * which on the save screen is the page name. */
    if (!src && !strcmp(n->type, "input"))
        src = sl_node_prop(n, "value");
    if (!src)
        return;

    mr_expr_eval(src, &v);
    /* FilePageNameInputValue() answers a handle rather than text, so that
     * the label tracks the page instead of freezing on whichever one was
     * open when the screen's `default` ran. */
    if (v.type == MR_V_STR && !strcmp(v.s, MR_PAGENAME_HANDLE))
        mr_value_set_str(&v, mr_interp_file_page_name());
    if (v.type == MR_V_STR)
        strncpy(raw, v.s, sizeof(raw) - 1);
    else if (v.type == MR_V_INT)
        sprintf(raw, "%ld", v.i);
    else
        raw[0] = '\0';
    raw[sizeof(raw) - 1] = '\0';

    /* Translated before interpolation, and whether or not the script wrote
     * `_()`. Ren'Py does this in substitutions.substitute(), which every Text
     * displayable runs its string through, so the marker only makes a string
     * *extractable* by the translation generator -- it is not what enables
     * the lookup. A screen that says `text "Meilleurs scores"` is translated
     * in Ren'Py exactly as `text _("Meilleurs scores")` is. */
    mr_expr_substitute(mr_tl_string(raw), out, outsz);
}

/* Whether a button's action reports itself as selected -- Ren'Py asks the
 * action object, and a selected button draws in the selected_ style prefix.
 * Two actions here answer yes.
 *
 * Language, where the current language decides; Language(None) means the
 * script's original language, which is the unset state.
 *
 * MusicRoom's Play(file), whose get_selected is `get_playing(channel) ==
 * filename`: the jukebox draws the track it is playing in white and the rest
 * in the idle grey. */
/* Whether a button's action can be pressed at all. Ren'Py asks the action's
 * get_sensitive(); a false answer both greys the button and swallows the
 * click.
 *
 * The quick menu is where this shows: `Retour` is dead until there is
 * something to roll back to and `Chargement R.` until a quick save exists,
 * and qm.png shows both greyed. An explicit `sensitive` property on the
 * button wins over the action, as it does in Ren'Py.
 *
 * Unknown actions are sensitive. The failure mode of guessing wrong the
 * other way is a button nobody can press and no message saying why. */
static int action_sensitive(const sl_node *n)
{
    const char *e = sl_node_prop(n, "sensitive");
    const char *act;

    if (e && *e) {
        mr_value v;
        mr_expr_eval(e, &v);
        return mr_value_truthy(&v);
    }

    act = sl_node_prop(n, "action");
    if (!act)
        return 1;

    while (*act == ' ' || *act == '\t')
        act++;

    if (!strncmp(act, "Rollback(", 9))
        return mr_interp_can_rollback();
    if (!strncmp(act, "QuickLoad(", 10))
        return mr_interp_quicksave_exists();
    if (!strncmp(act, "Skip(", 5))
        return mr_interp_can_skip();

    return 1;
}

static int act_selected(const char *act);

/* The same question asked of an action's text rather than of a node, so the
 * selected rules can be tested without building a screen around them. */
int mr_sl_action_selected(const char *action)
{
    return act_selected(action);
}

static int action_selected(const sl_node *n)
{
    const char *act = sl_node_prop(n, "action");

    return act ? act_selected(act) : 0;
}

static int act_selected(const char *act)
{
    const char *open;
    char want[128];
    mr_value cur;
    int have;

    if (!act)
        return 0;

    while (*act == ' ' || *act == '\t')
        act++;

    open = strchr(act, '(');
    if (!open)
        return 0;

    /* `InvertSelected(X)` is selected exactly when X is not. The preferences
     * screen wraps the transitions toggle in it, so the button reads "skip
     * transitions" while the preference underneath means "play them". */
    if (!strncmp(act, "InvertSelected", 14) && act + 14 == open) {
        char inner[256];
        int len = (int)strlen(open + 1);

        /* Trailing space, then exactly one ')' -- the wrapper's own. Peeling
         * every ')' would take the inner call's with it and leave
         * `Preference("transitions", "toggle"` behind. */
        while (len > 0 && (open[len] == ' ' || open[len] == '\t'))
            len--;
        if (len <= 0 || open[len] != ')')
            return 0;
        len--;
        if (len <= 0 || len >= (int)sizeof(inner))
            return 0;
        memcpy(inner, open + 1, (size_t)len);
        inner[len] = '\0';
        return !act_selected(inner);
    }

    /* `Preference(name, value)` with two arguments is an action, and the
     * radio and check buttons ask it which one is chosen. One argument is a
     * bar value instead, and has no selected state. */
    if (!strncmp(act, "Preference", 10) && act + 10 == open) {
        char pname[80];
        const char *p = mr_read_string(open + 1, pname, sizeof(pname));

        if (!p)
            return 0;
        p = mr_skip_ws(p);
        if (*p != ',')
            return 0;
        p = mr_skip_ws(p + 1);
        if (!mr_read_string(p, want, sizeof(want)))
            return 0;
        return mr_interp_pref_selected(pname, want);
    }

    /* `Skip()` is a toggle, so the quick menu's button has to show it is on.
     *
     * Ren'Py's Skip.get_selected (00action_menu.rpy:332) is
     * `config.skipping and config.skipping != "fast"`, and both the Tab
     * toggle (00keymap.rpy:241) and the held skip key (behavior.py:350) set
     * it to "slow" -- so the button is lit while Ctrl is down as well, not
     * only after a click. mr_interp_skipping covers both, and applies the
     * `_skipping` and `config.allow_skipping` fences, which is right: a
     * button claiming to be skipping through the credits, where the fence
     * suppresses it, would be lying.
     *
     * The fast/slow distinction is not reproduced because there is one skip
     * state here; nothing sets Ren'Py's "fast", which only comes from warp
     * and `_return_fast_skipping`. The quick menu takes its selected state
     * from `action Skip()`, the slow one, so this is what it asks for. */
    if (!strncmp(act, "Skip", 4) && act + 4 == open)
        return mr_interp_skipping();

    if (!strncmp(act, "Language", 8)) {
        have = mr_var_get("_preferences.language", &cur) && cur.type == MR_V_STR;

        if (!mr_read_string(open + 1, want, sizeof(want)))
            return !have;          /* Language(None) */

        return have && !strcmp(cur.s, want);
    }

    /* `FilePage(1)` / `FilePage("quick")`: the button for the page being
     * shown is drawn selected, which is the highlight on "1" in the row of
     * page numbers. The argument is often the loop variable `page`, so it
     * is evaluated rather than read -- and this runs inside the button's
     * scope, where that variable is bound. */
    if (!strncmp(act, "FilePage", 8) && act + 8 == open) {
        const char *cur = mr_interp_file_page();
        mr_value v;

        mr_expr_eval(open + 1, &v);
        if (!cur || !*cur)
            return 0;
        if (v.type == MR_V_STR)
            return !strcmp(v.s, cur);
        if (v.type == MR_V_INT)
            return v.i == atol(cur);
        return 0;
    }

    /* `<expr>.Play("file")`, where <expr> holds a music room. The expression
     * is evaluated in the scope the button is being drawn in, which is where
     * the screen variable holding the room lives. */
    if (open - act > 5 && !strncmp(open - 5, ".Play", 5)) {
        char owner[128];
        int len = (int)(open - 5 - act);
        mr_value v;
        int id;

        if (len <= 0 || len >= (int)sizeof(owner))
            return 0;
        memcpy(owner, act, (size_t)len);
        owner[len] = '\0';

        mr_expr_eval(owner, &v);
        id = mr_room_from_value(&v);
        if (id < 0)
            return 0;

        if (!mr_read_string(open + 1, want, sizeof(want)))
            return 0;
        return mr_room_is_playing(id, want);
    }

    return 0;
}

/* The style prefix a button draws in. Ren'Py's own search order then fills in
 * what the prefix does not define: selected_idle_ falls back through
 * selected_ and idle_, which is how `gui.button_text_selected_color` reaches
 * a button that only ever sets idle and hover colours. */
static const char *button_state(const sl_node *n, int hovered)
{
    /* Insensitive beats both of the others: a button that cannot be pressed
     * must not look pressable, hovered or selected. */
    if (!action_sensitive(n))
        return "insensitive_";
    if (action_selected(n))
        return hovered ? "selected_hover_" : "selected_idle_";
    return hovered ? "hover_" : "idle_";
}

/* Build an imagebutton's image name from `auto "english %s.png"`. */
static void auto_image(const sl_node *n, const char *state,
                       char *out, int outsz)
{
    const char *e = sl_node_prop(n, "auto");
    mr_value v;
    const char *pct;
    int len;

    out[0] = '\0';
    if (!e)
        return;

    mr_expr_eval(e, &v);
    if (v.type != MR_V_STR)
        return;

    pct = strstr(v.s, "%s");
    if (!pct) {
        strncpy(out, v.s, (size_t)outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }

    len = (int)(pct - v.s);
    if (len + (int)strlen(state) + (int)strlen(pct + 2) >= outsz)
        return;
    memcpy(out, v.s, (size_t)len);
    out[len] = '\0';
    strcat(out, state);
    strcat(out, pct + 2);
}

/* Resolve an imagebutton's surface for its current state, falling back the
 * way Ren'Py's style prefix search does. */
static mr_surface *imagebutton_surface(const sl_node *n, int hovered)
{
    char name[MR_MAX_NAME];
    mr_surface *img;
    int sel = action_selected(n);
    const char *states[4];
    int i, ns = 0;

    if (sel) {
        states[ns++] = hovered ? "selected_hover" : "selected_idle";
        states[ns++] = "selected_idle";
    }
    states[ns++] = hovered ? "hover" : "idle";
    states[ns++] = "idle";

    for (i = 0; i < ns; i++) {
        auto_image(n, states[i], name, sizeof(name));
        if (!name[0])
            continue;
        img = mr_image_get(name);
        if (img)
            return img;
    }

    /* An explicit `idle`/`image` property, for buttons not using auto. */
    {
        char buf[MR_MAX_NAME];
        str_prop(n, "", NULL, hovered ? "hover" : "idle", buf, sizeof(buf));
        if (!buf[0])
            str_prop(n, "", NULL, "image", buf, sizeof(buf));
        if (buf[0])
            return mr_image_get(buf);
    }
    return NULL;
}

/* --- viewport scrolling ---------------------------------------------------
 *
 * A viewport's scroll position has to survive between frames, but the screen
 * tree is rebuilt on every render, so it is kept beside the tree and keyed on
 * the node. There are only ever a handful of viewports on screen.
 */

#define SL_MAX_SCROLL 8

struct sl_scroll {
    const sl_node *n;
    int            pos;       /* pixels scrolled down */
    int            inited;    /* yinitial has been applied */
};

static sl_scroll g_scroll[SL_MAX_SCROLL];
static int       g_nscroll;

static void scroll_set(sl_scroll *sc, int pos) { sc->pos = pos; }
static int  scroll_get(const sl_scroll *sc) { return sc->pos; }

static sl_scroll *scroll_for(const sl_node *n)
{
    int i;

    for (i = 0; i < g_nscroll; i++) {
        if (g_scroll[i].n == n)
            return &g_scroll[i];
    }
    if (g_nscroll >= SL_MAX_SCROLL)
        return NULL;

    i = g_nscroll++;
    g_scroll[i].n = n;
    g_scroll[i].pos = 0;
    g_scroll[i].inited = 0;
    return &g_scroll[i];
}

/* Clamp a scroll position to the content that actually exists. */
static int scroll_clamp(int pos, int content, int view)
{
    int max = content - view;

    if (max < 0)
        max = 0;
    if (pos > max)
        pos = max;
    if (pos < 0)
        pos = 0;
    return pos;
}

/* --- the tree walk --- */

static void measure(const sl_node *n, const sl_ctx *ctx,
                    int availw, int availh, int *w, int *h);
static void draw(mr_surface *dst, const sl_node *n, const sl_ctx *ctx,
                 int x, int y, int availw, int availh);

/* Children of control-flow nodes are spliced into the parent's child list, so
 * both passes need to walk them uniformly. This yields the effective child
 * list by resolving if/for/use/transclude into concrete nodes, each paired
 * with the environment it must be measured and drawn in. */
#define SL_MAX_EFFECTIVE 64

typedef struct {
    const sl_node *v[SL_MAX_EFFECTIVE];
    sl_env         e[SL_MAX_EFFECTIVE];
    int n;
} sl_kidlist;

static void collect(const sl_node *n, sl_kidlist *out, int depth);

/* --- iterables ------------------------------------------------------------
 *
 * `for` in a screen iterates a Python sequence. The value model here has no
 * sequence type, so rather than bolt one on, the two forms screens actually
 * use are walked directly: a range() call and a literal list. Anything else
 * yields nothing and says so, which loses a loop but never invents items.
 */

typedef struct {
    int         kind;       /* 0 none, 1 range, 2 literal list, 3 engine,
                             * 4 a list value (list.c) */
    long        cur, end, step;
    const char *p;          /* next element, for a list */
    char        list[64];   /* engine list name */
    int         index, count;
    int         lid;        /* kind 4: the list's id */
} sl_iter;

/* End of the current top-level element of a comma-separated list. */
static const char *split_comma(const char *p, int *len)
{
    int depth = 0;
    int i = 0;

    while (p[i]) {
        char c = p[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) break;
            depth--;
        } else if (c == '"' || c == '\'') {
            char q = c;
            i++;
            while (p[i] && p[i] != q) {
                if (p[i] == '\\' && p[i + 1]) i++;
                i++;
            }
            if (!p[i]) break;
        } else if (c == ',' && depth == 0) {
            break;
        }
        i++;
    }
    *len = i;
    return p + i;
}

static long iter_int(const char *p, int len)
{
    char buf[64];
    mr_value v;

    if (len <= 0 || len >= (int)sizeof(buf))
        return 0;
    memcpy(buf, p, (size_t)len);
    buf[len] = '\0';
    mr_expr_eval(buf, &v);
    if (v.type == MR_V_FLOAT)
        return (long)v.f;
    return v.i;
}

static void iter_begin(const char *expr, sl_iter *it)
{
    const char *p;

    memset(it, 0, sizeof(*it));
    if (!expr)
        return;
    p = mr_skip_ws(expr);

    if (!strncmp(p, "range", 5) && *mr_skip_ws(p + 5) == '(') {
        long a[3];
        int na = 0;
        p = mr_skip_ws(mr_skip_ws(p + 5) + 1);
        while (*p && *p != ')' && na < 3) {
            int len;
            const char *end = split_comma(p, &len);
            a[na++] = iter_int(p, len);
            p = end;
            if (*p == ',')
                p = mr_skip_ws(p + 1);
        }
        it->kind = 1;
        it->step = 1;
        if (na == 1) {
            it->cur = 0;
            it->end = a[0];
        } else if (na >= 2) {
            it->cur = a[0];
            it->end = a[1];
            if (na == 3 && a[2] != 0)
                it->step = a[2];
        }
        return;
    }

    if (*p == '[') {
        it->kind = 2;
        it->p = mr_skip_ws(p + 1);
        return;
    }

    /* A list the engine keeps, such as _history_list. */
    {
        int n = mr_interp_list_len(p);
        if (n >= 0) {
            it->kind = 3;
            it->count = n;
            it->index = 0;
            strncpy(it->list, p, sizeof(it->list) - 1);
            it->list[sizeof(it->list) - 1] = '\0';
            return;
        }
    }

    /* Anything else is evaluated, and iterated if it turns out to be a list
     * value -- `default scores = build_rhythm_highscores()` puts one in a
     * screen variable and the table below walks it. */
    {
        mr_value v;
        int id;

        mr_expr_eval(p, &v);
        id = mr_list_id(&v);
        if (id >= 0) {
            it->kind = 4;
            it->lid = id;
            it->count = mr_list_len(id);
            it->index = 0;
            return;
        }
    }

    mr_log("slrender: cannot iterate '%s'", expr);
}

/* Bind one item of an engine list beneath the loop variable: the item's
 * fields become `h.who`, `h.what` and so on, which the dotted-name store can
 * resolve without any notion of an object. */
/* `for title, score in ...`: bind the targets to the elements of the item,
 * which for a list of tuples is each tuple's contents. A target with nothing
 * to take is bound to None rather than left over from the previous turn of
 * the loop. */
static void bind_unpack(int *head, const char *targets,
                        const mr_value *item)
{
    int sub = mr_list_id(item);
    const char *p = targets;
    int col = 0;

    while (*p) {
        char name[64];
        int n = 0;
        mr_value v;

        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && n < 63)
            name[n++] = *p++;
        name[n] = '\0';
        if (!n)
            break;

        memset(&v, 0, sizeof(v));
        if (sub >= 0)
            mr_list_get(sub, col, &v);
        else if (col == 0)
            v = *item;
        bind_push(head, name, &v);
        col++;
    }
}

static void iter_bind_fields(sl_iter *it, const char *var)
{
    mr_list_field f[MR_LIST_FIELDS];
    int n, i;

    n = mr_interp_list_item(it->list, it->index - 1, f, MR_LIST_FIELDS);
    for (i = 0; i < n; i++) {
        char name[SL_MAX_PROPNAME];
        if ((int)(strlen(var) + strlen(f[i].suffix) + 2) > SL_MAX_PROPNAME)
            continue;
        sprintf(name, "%s.%s", var, f[i].suffix);
        bind_push(&g_env.bind, name, &f[i].value);
    }
}

static int iter_next(sl_iter *it, mr_value *out)
{
    memset(out, 0, sizeof(*out));

    if (it->kind == 4) {
        if (it->index >= it->count)
            return 0;
        mr_list_get(it->lid, it->index, out);
        it->index++;
        return 1;
    }

    if (it->kind == 3) {
        if (it->index >= it->count)
            return 0;
        it->index++;
        return 1;
    }

    if (it->kind == 1) {
        if (it->step > 0 ? it->cur >= it->end : it->cur <= it->end)
            return 0;
        mr_value_set_int(out, it->cur);
        it->cur += it->step;
        return 1;
    }

    if (it->kind == 2) {
        char buf[256];
        int len;
        const char *end;

        if (!it->p || !*it->p || *it->p == ']')
            return 0;
        end = split_comma(it->p, &len);
        while (len > 0 && it->p[len - 1] == ' ')
            len--;
        if (len > 0 && len < (int)sizeof(buf)) {
            memcpy(buf, it->p, (size_t)len);
            buf[len] = '\0';
            mr_expr_eval(buf, out);
        }
        it->p = (*end == ',') ? mr_skip_ws(end + 1) : end;
        return 1;
    }

    return 0;
}

/* --- `use` argument binding ----------------------------------------------- */

/* Bind a `use` node's arguments to the used screen's parameters, producing the
 * environment its body runs in. A used screen sees its parameters and the
 * globals, not the using screen's locals, so the chain starts empty. */
static sl_env bind_use_args(const sl_node *k, const sl_screen *sc)
{
    mr_value  pos[SL_MAX_PARAMS];
    int       npos = 0;
    mr_value  kw[SL_MAX_PARAMS];
    int       haskw[SL_MAX_PARAMS];
    sl_env    out;
    const char *p;
    int i;

    memset(&out, 0, sizeof(out));
    out.bind = -1;
    out.xclude = -1;
    memset(haskw, 0, sizeof(haskw));

    /* Arguments are evaluated in the caller's scope, before we switch. */
    p = k->arg ? strchr(k->arg, '(') : NULL;
    if (p) {
        p = mr_skip_ws(p + 1);
        while (*p && *p != ')') {
            int len;
            const char *end = split_comma(p, &len);
            char buf[256];
            int elen = len;

            while (elen > 0 && p[elen - 1] == ' ')
                elen--;
            if (elen > 0 && elen < (int)sizeof(buf)) {
                char name[SL_MAX_PROPNAME];
                const char *eq;
                int ni = 0;

                memcpy(buf, p, (size_t)elen);
                buf[elen] = '\0';

                /* A keyword argument is `name=expr` with a bare identifier on
                 * the left; `a == b` and `a <= b` are not. */
                eq = strchr(buf, '=');
                if (eq && eq > buf && eq[1] != '=' &&
                    eq[-1] != '=' && eq[-1] != '!' &&
                    eq[-1] != '<' && eq[-1] != '>') {
                    const char *q = mr_skip_ws(buf);
                    while (*q && (q[0] == '_' || (q[0] >= 'a' && q[0] <= 'z') ||
                                  (q[0] >= 'A' && q[0] <= 'Z') ||
                                  (q[0] >= '0' && q[0] <= '9'))) {
                        if (ni < SL_MAX_PROPNAME - 1)
                            name[ni++] = *q;
                        q++;
                    }
                    name[ni] = '\0';
                    if (mr_skip_ws(q) != eq)
                        ni = 0;    /* not a plain identifier after all */
                }

                if (ni > 0) {
                    for (i = 0; i < sc->nparams; i++) {
                        if (!strcmp(sc->params[i], name)) {
                            mr_expr_eval(mr_skip_ws(eq + 1), &kw[i]);
                            haskw[i] = 1;
                            break;
                        }
                    }
                    if (i == sc->nparams)
                        mr_log("slrender: `use %s` has no parameter '%s'",
                               sc->name, name);
                } else if (npos < SL_MAX_PARAMS) {
                    mr_expr_eval(buf, &pos[npos++]);
                }
            }
            p = (*end == ',') ? mr_skip_ws(end + 1) : end;
        }
    }

    /* Now switch to the callee's scope and fill it in. Defaults are evaluated
     * there, since a default may refer to an earlier parameter. */
    {
        sl_env save = g_env;
        g_env = out;

        for (i = 0; i < sc->nparams; i++) {
            mr_value v;
            if (haskw[i])
                v = kw[i];
            else if (i < npos)
                v = pos[i];
            else if (sc->defaults[i])
                mr_expr_eval(sc->defaults[i], &v);
            else
                memset(&v, 0, sizeof(v));
            bind_push(&g_env.bind, sc->params[i], &v);
        }

        out = g_env;
        g_env = save;
    }

    return out;
}

/* Apply and consume any style prefix that travelled across a `use` splice.
 * Consumed rather than left set, so that a container further in with its own
 * style_prefix is not overwritten by the one from the boundary. */
static void enter_child(sl_ctx *ctx)
{
    if (!g_env.has_prefix)
        return;
    strncpy(ctx->prefix, g_env.prefix, sizeof(ctx->prefix) - 1);
    ctx->prefix[sizeof(ctx->prefix) - 1] = '\0';
    g_env.has_prefix = 0;
}

/* Bindings and transclude bodies created while collecting a child list stay
 * alive until the caller has finished measuring and drawing that list. */
typedef struct { int bind, xc; } sl_mark;

static sl_mark scope_mark(void)
{
    sl_mark m;
    m.bind = g_nbind;
    m.xc = g_nxc;
    return m;
}

static void scope_release(sl_mark m)
{
    g_nbind = m.bind;
    g_nxc = m.xc;
}

static void collect_one(const sl_node *k, sl_kidlist *out, int depth)
{
    if (out->n >= SL_MAX_EFFECTIVE || depth > SL_MAX_DEPTH)
        return;

    switch (k->kind) {
    case SL_IF: {
        mr_value v;
        if (!k->cond) {                 /* else */
            collect(k, out, depth + 1);
            return;
        }
        mr_expr_eval(k->cond, &v);
        if (mr_value_truthy(&v))
            collect(k, out, depth + 1);
        else if (k->orelse)
            collect_one(k->orelse, out, depth + 1);
        return;
    }

    case SL_PYTHON:
        if (k->arg)
            mr_expr_exec(k->arg);
        return;

    case SL_DEFAULT:
        /* `default x = expr` sets x once per showing of the screen, not once
         * per frame, so it is backed by the persistent store. */
        if (k->arg) {
            char name[SL_MAX_PROPNAME];
            const char *eq = strchr(k->arg, '=');
            if (eq) {
                const char *s0 = mr_skip_ws(k->arg);
                int len = (int)(eq - s0);
                while (len > 0 && s0[len - 1] == ' ')
                    len--;
                if (len > 0 && len < SL_MAX_PROPNAME) {
                    sl_screenvar *sv;

                    memcpy(name, s0, (size_t)len);
                    name[len] = '\0';

                    sv = screenvar_find(name);
                    if (!sv) {
                        sv = screenvar_add(name);
                        if (sv)
                            mr_expr_eval(eq + 1, &sv->value);
                    }

                    /* Bind it every frame, not only the frame it was
                     * created on.
                     *
                     * mr_sl_render binds the screen's variables up front,
                     * but `use` *replaces* the environment
                     * (g_env = bind_use_args), so a default declared inside
                     * a used screen loses that binding as soon as the
                     * splice happens. The save screen's page label is
                     * exactly this -- `default page_name_value` lives in
                     * file_slots, which load and save reach through `use` --
                     * and it showed "Page 1" on the frame its default ran
                     * and nothing on every frame after. */
                    if (sv)
                        bind_push(&g_env.bind, name, &sv->value);
                }
            }
        }
        return;

    case SL_USE: {
        const sl_screen *sc = g_find_screen ? g_find_screen(k->type) : NULL;
        sl_env save;

        if (!sc || !sc->root)
            return;

        save = g_env;
        g_env = bind_use_args(k, sc);

        /* Carry the used screen's own style_prefix across the splice. */
        {
            const char *sp = sl_node_prop(sc->root, "style_prefix");
            if (sp) {
                mr_value v;
                mr_expr_eval(sp, &v);
                if (v.type == MR_V_STR && v.s[0]) {
                    strncpy(g_env.prefix, v.s, sizeof(g_env.prefix) - 1);
                    g_env.has_prefix = 1;
                }
            }
        }

        /* If the `use` opened a block, that block is the body a `transclude`
         * inside the used screen expands to -- and it belongs to the calling
         * screen's environment, not the callee's. */
        if (k->nkids > 0 && g_nxc < SL_MAX_XC) {
            g_xc[g_nxc].body = k;
            g_xc[g_nxc].env = save;
            g_env.xclude = g_nxc++;
        }

        collect(sc->root, out, depth + 1);
        g_env = save;
        return;
    }

    case SL_FOR: {
        sl_iter it;
        mr_value v;
        sl_env save = g_env;

        if (!k->var[0]) {
            mr_log("slrender: for with no target");
            return;
        }

        iter_begin(k->cond, &it);
        while (iter_next(&it, &v)) {
            g_env = save;
            if (it.kind == 3)
                iter_bind_fields(&it, k->var);
            else if (strchr(k->var, ','))
                bind_unpack(&g_env.bind, k->var, &v);
            else
                bind_push(&g_env.bind, k->var, &v);
            collect(k, out, depth + 1);
            if (out->n >= SL_MAX_EFFECTIVE)
                break;
        }
        g_env = save;
        return;
    }

    case SL_DISPLAYABLE:
        /* `transclude` stands for the block attached to the `use` that brought
         * us here, evaluated back in the calling screen's environment. */
        if (!strcmp(k->type, "transclude")) {
            if (g_env.xclude >= 0) {
                sl_xc *xc = &g_xc[g_env.xclude];
                sl_env save = g_env;
                const char *sp;

                g_env = xc->env;

                /* A style_prefix written at the top of the `use` block
                 * belongs to the transcluded body: the history screen opens
                 * its block with `style_prefix "history"`, and without this
                 * every window in it resolves against game_menu instead. */
                sp = sl_node_prop(xc->body, "style_prefix");
                if (sp) {
                    mr_value v;
                    mr_expr_eval(sp, &v);
                    if (v.type == MR_V_STR && v.s[0]) {
                        strncpy(g_env.prefix, v.s, sizeof(g_env.prefix) - 1);
                        g_env.prefix[sizeof(g_env.prefix) - 1] = '\0';
                        g_env.has_prefix = 1;
                    }
                }

                collect(xc->body, out, depth + 1);
                g_env = save;
            }
            return;
        }
        /* fall through */

    default:
        out->e[out->n] = g_env;
        out->v[out->n++] = k;
        return;
    }
}

static void collect(const sl_node *n, sl_kidlist *out, int depth)
{
    int i;
    for (i = 0; i < n->nkids; i++)
        collect_one(n->kids[i], out, depth);
}

/* Place a child inside a parent box according to its position properties. */
static void place(const sl_node *k, const sl_ctx *ctx,
                  int box_x, int box_y, int box_w, int box_h,
                  int cw, int ch, int *ox, int *oy)
{
    char style[64];
    sl_num p;
    int px, py;
    int area[4];

    node_style(k, ctx, style, sizeof(style));

    /* `area` pins the corner outright, anchors at zero. */
    if (node_area(k, style, area)) {
        *ox = box_x + area[0];
        *oy = box_y + area[1];
        return;
    }

    /* xalign sets both position and anchor. */
    px = 0;
    p = get_num(k, style, NULL, "xpos");
    if (p.present) {
        px = p.is_frac ? (int)(p.f * (float)box_w) : (int)p.i;
        p = get_num(k, style, NULL, "xanchor");
        if (p.present)
            px -= p.is_frac ? (int)(p.f * (float)cw) : (int)p.i;
    } else {
        p = get_num(k, style, NULL, "xalign");
        if (p.present)
            px = (int)(p.f * (float)(box_w - cw));
        else {
            p = get_num(k, style, NULL, "xcenter");
            if (p.present)
                px = (p.is_frac ? (int)(p.f * (float)box_w) : (int)p.i) - cw / 2;
        }
    }

    py = 0;
    p = get_num(k, style, NULL, "ypos");
    if (p.present) {
        py = p.is_frac ? (int)(p.f * (float)box_h) : (int)p.i;
        p = get_num(k, style, NULL, "yanchor");
        if (p.present)
            py -= p.is_frac ? (int)(p.f * (float)ch) : (int)p.i;
    } else {
        p = get_num(k, style, NULL, "yalign");
        if (p.present)
            py = (int)(p.f * (float)(box_h - ch));
        else {
            p = get_num(k, style, NULL, "ycenter");
            if (p.present)
                py = (p.is_frac ? (int)(p.f * (float)box_h) : (int)p.i) - ch / 2;
        }
    }

    px += num_px(get_num(k, style, NULL, "xoffset"), box_w, 0);
    py += num_px(get_num(k, style, NULL, "yoffset"), box_h, 0);

    *ox = box_x + px;
    *oy = box_y + py;
}

/* The text a textbutton or label shows uses the `_text` companion style. */
static void text_child_style(const char *style, char *out, int outsz)
{
    sprintf(out, "%.55s_text", style);
    (void)outsz;
}

/* Resolve an image name from a screen. FileScreenshot yields "@save:<slot>"
 * rather than a path, because a thumbnail lives inside a save file and not in
 * the archive. Everything else is an ordinary image name. */
static mr_surface *screen_image(const char *name)
{
    if (!name || !name[0])
        return NULL;
    if (!strncmp(name, "@save:", 6))
        return mr_save_thumbnail(name + 6);
    return mr_image_get(name);
}

/* A `grid cols rows` carries its shape as two positional arguments. Anything
 * else has no grid shape, which is how both passes tell them apart. */
static void grid_shape(const sl_node *n, int *cols, int *rows)
{
    mr_value v;

    *cols = 0;
    *rows = 0;
    if (strcmp(n->type, "grid") || !n->arg || !n->arg2)
        return;

    mr_expr_eval(n->arg, &v);
    *cols = (int)v.i;
    mr_expr_eval(n->arg2, &v);
    *rows = (int)v.i;
    if (*cols < 1 || *rows < 1) {
        *cols = 0;
        *rows = 0;
    }
}

/* How wide the text inside a displayable may run before it wraps.
 *
 * Ren'Py wraps against the box the text sits in, not against the text style:
 * the help screen's `style help_label: xsize 250` is what makes "Flèches
 * directionnelles" break across two lines. The text style's own xmaximum
 * wins when it has one. */
static int text_wrap_width(const sl_node *n, const char *style,
                           const char *tstyle, int availw)
{
    sl_num mx;
    int pad[4];

    mx = get_num(n, tstyle, NULL, "xmaximum");
    if (!mx.present)
        mx = get_num(n, tstyle, NULL, "xsize");
    if (mx.present)
        return num_px(mx, availw, 0);

    /* `text` is the text itself and has no box of its own to be bound by. */
    if (!strcmp(n->type, "text"))
        return 0;

    mx = get_num(n, style, NULL, "xsize");
    if (!mx.present)
        mx = get_num(n, style, NULL, "xmaximum");
    if (!mx.present)
        return 0;

    get_padding(n, style, pad);
    return num_px(mx, availw, 0) - pad[0] - pad[2];
}

/* Horizontal alignment of wrapped text within its width, from textalign. */
static float text_align_of(const sl_node *n, const char *tstyle)
{
    sl_num a = get_num(n, tstyle, NULL, "textalign");

    if (!a.present)
        a = get_num(n, tstyle, NULL, "text_align");
    return a.present ? a.f : 0.0f;
}

static void measure_text(const char *text, const mr_font *f, int wrap,
                         int *w, int *h)
{
    int lh = mr_font_line_height(f);

    if (wrap > 0) {
        int lines = mr_font_draw_wrapped(NULL, f, 0, 0, wrap, text, 0, 0, 0);
        if (lines < 1)
            lines = 1;
        *w = wrap;
        *h = lines * lh;
    } else {
        /* Unwrapped text still breaks at its own newlines, so it is as wide
         * as its widest line and as tall as it has lines. */
        *w = mr_font_text_width(f, text);
        *h = mr_font_text_lines(text) * lh;
    }
}

static void measure_inner(const sl_node *n, const sl_ctx *ctx,
                          int availw, int availh, int *w, int *h)
{
    char style[64];
    sl_kidlist kids;
    sl_num sz;
    int i;

    *w = 0;
    *h = 0;

    if (!n || ctx->depth > SL_MAX_DEPTH)
        return;

    node_style(n, ctx, style, sizeof(style));

    if (!strcmp(n->type, "text") ||
        !strcmp(n->type, "textbutton") ||
        !strcmp(n->type, "label") ||
        !strcmp(n->type, "input")) {
        char buf[MR_MAX_TEXT];
        char tstyle[64];
        const mr_font *f;
        int pad[4];
        int wrap = 0;

        /* An `input` carries its own text style, exactly as a `text` does --
         * the save screen writes `input: style "page_label_text"`. The draw
         * path has always known that; measuring went looking for a child
         * style instead and so sized the text in the default font.
         *
         * The two disagreeing is worse than either being wrong: the page
         * label measured 75px in the fallback font, the button reserved
         * that plus its 100px of padding, and then the draw laid the real
         * 82px text out in the 75px it had been given -- so "Page 1" wrapped
         * onto two lines with room to spare on either side. */
        if (!strcmp(n->type, "text") || !strcmp(n->type, "input"))
            strncpy(tstyle, style, sizeof(tstyle) - 1);
        else
            text_child_style(style, tstyle, sizeof(tstyle));
        tstyle[sizeof(tstyle) - 1] = '\0';

        node_text(n, buf, sizeof(buf));
        f = font_for_node(n, tstyle);
        if (!f)
            return;

        wrap = text_wrap_width(n, style, tstyle, availw);

        /* Nothing in the styles pins the width, so the text wraps at what it
         * was offered -- but only if it does not already fit, or every label
         * would claim the whole row and an hbox could pack nothing after
         * it. */
        if (wrap <= 0) {
            get_padding(n, style, pad);
            {
                int room = availw - pad[0] - pad[2];
                if (room > 0 && mr_font_text_width(f, buf) > room)
                    wrap = room;
            }
        }

        measure_text(buf, f, wrap, w, h);

        if (strcmp(n->type, "text") && strcmp(n->type, "input")) {
            get_padding(n, style, pad);
            *w += pad[0] + pad[2];
            *h += pad[1] + pad[3];
        }
    } else if (!strcmp(n->type, "add")) {
        char name[MR_MAX_NAME];
        mr_surface *img;
        node_text(n, name, sizeof(name));
        img = screen_image(name);
        if (img) {
            *w = img->w;
            *h = img->h;
        }
    } else if (!strcmp(n->type, "imagebutton")) {
        mr_surface *img = imagebutton_surface(n, 0);
        if (img) {
            *w = img->w;
            *h = img->h;
        }
    } else if (!strcmp(n->type, "null")) {
        sl_num v = get_num(n, style, NULL, "width");
        *w = v.present ? (int)v.i : 0;
        v = get_num(n, style, NULL, "height");
        *h = v.present ? (int)v.i : 0;
    } else if (!strcmp(n->type, "viewport") || !strcmp(n->type, "vpgrid")) {
        /* A viewport is a window onto content that may be far taller than it
         * is. Its own size is whatever it was given -- Ren'Py's builtin
         * viewport style is xfill/yfill True -- never the content's size. */
        *w = availw;
        *h = availh;
    } else if (!strcmp(n->type, "fixed") && !truthy_prop(n, style, "xfit") &&
                                            !truthy_prop(n, style, "yfit")) {
        /* Ren'Py's Fixed takes the whole area it is offered and places its
         * children inside it; it shrinks to its content only when asked with
         * xfit/yfit. Measuring it as an ordinary container made the save
         * screen's fixed exactly as tall as the slot grid, so the page label
         * above it had nowhere to go and landed on top of the slots. */
        *w = availw;
        *h = availh;
    } else if (!strcmp(n->type, "bar") || !strcmp(n->type, "vbar")) {
        /* Ren'Py's Bar takes the whole width it is offered and gets its
         * thickness from the style -- `style bar: ysize gui.bar_size`. The
         * track art only decides the thickness when the style says nothing,
         * which is why it is still read here. Sizing a bar from its art
         * instead left the jukebox's position bar 350px wide inside a 901px
         * viewport. */
        int vertical = !strcmp(n->type, "vbar") ||
                       truthy_prop(n, style, "bar_vertical");
        char path[MR_MAX_NAME];
        mr_surface *img;

        if (vertical)
            *h = availh;
        else
            *w = availw;

        image_prop(n, style, "idle_", "left_bar", path, sizeof(path));
        if (!path[0])
            image_prop(n, style, "idle_", "base_bar", path, sizeof(path));
        if (!path[0])
            image_prop(n, style, "idle_", "top_bar", path, sizeof(path));

        img = path[0] ? mr_image_get(path) : NULL;
        if (img) {
            if (vertical)
                *w = img->w;
            else
                *h = img->h;
        }
    } else {
        /* A container: measure the children. */
        int isv = !strcmp(n->type, "vbox");
        int ish = !strcmp(n->type, "hbox");
        int spacing = num_px(get_num(n, style, NULL, "spacing"), 0, 0);
        int pad[4];
        int cols = 0, rows = 0;
        sl_ctx sub = *ctx;
        const char *sp = sl_node_prop(n, "style_prefix");
        sl_mark mark;
        sl_env  save;

        if (sp) {
            mr_value v;
            mr_expr_eval(sp, &v);
            if (v.type == MR_V_STR)
                strncpy(sub.prefix, v.s, sizeof(sub.prefix) - 1);
        }
        sub.depth++;

        get_padding(n, style, pad);
        grid_shape(n, &cols, &rows);

        mark = scope_mark();
        save = g_env;
        kids.n = 0;
        collect(n, &kids, 0);

        for (i = 0; i < kids.n; i++) {
            int cw, ch;
            int aw = availw - pad[0] - pad[2];
            int ah = availh - pad[1] - pad[3];
            sl_ctx kctx = sub;

            /* A box offers each child what is left along its own axis and the
             * whole of the other, as Ren'Py's MultiBox does. Offering every
             * child the full width instead lets a child that fills its area
             * -- a `fixed` -- claim the lot and push the box past the
             * screen. */
            if (ish) aw -= *w;
            if (isv) ah -= *h;
            if (aw < 0) aw = 0;
            if (ah < 0) ah = 0;

            g_env = kids.e[i];
            enter_child(&kctx);
            measure(kids.v[i], &kctx, aw, ah, &cw, &ch);
            if (cols > 0) {
                /* A grid's cells are uniform, so the widest and tallest
                 * child sets the cell size. */
                if (cw > *w) *w = cw;
                if (ch > *h) *h = ch;
            } else if (isv) {
                if (cw > *w) *w = cw;
                *h += ch + (i ? spacing : 0);
            } else if (ish) {
                *w += cw + (i ? spacing : 0);
                if (ch > *h) *h = ch;
            } else {
                if (cw > *w) *w = cw;
                if (ch > *h) *h = ch;
            }
        }
        g_env = save;
        scope_release(mark);

        if (cols > 0) {
            *w = *w * cols + spacing * (cols - 1);
            *h = *h * rows + spacing * (rows - 1);
        }

        *w += pad[0] + pad[2];
        *h += pad[1] + pad[3];
    }

    /* An `area` is the last word on size, as it is on position. */
    {
        int area[4];
        if (node_area(n, style, area)) {
            *w = area[2];
            *h = area[3];
            return;
        }
    }

    /* Explicit sizes and fills override what the content wanted. */
    sz = get_num(n, style, NULL, "xsize");
    if (sz.present)
        *w = num_px(sz, availw, *w);
    sz = get_num(n, style, NULL, "ysize");
    if (sz.present)
        *h = num_px(sz, availh, *h);

    if (truthy_prop(n, style, "xfill"))
        *w = availw;
    if (truthy_prop(n, style, "yfill"))
        *h = availh;

    sz = get_num(n, style, NULL, "xmaximum");
    if (sz.present) {
        int m = num_px(sz, availw, *w);
        if (*w > m) *w = m;
    }
    sz = get_num(n, style, NULL, "ymaximum");
    if (sz.present) {
        int m = num_px(sz, availh, *h);
        if (*h > m) *h = m;
    }
}

/* Margins wrap everything: a displayable occupies its content plus its
 * margins, and only the content gets the background. Applying them here, once,
 * keeps every displayable type honest rather than repeating the arithmetic in
 * each branch. */
/* Layout work done for one screen, for MR_STATS. */
static long g_n_measure, g_n_draw, g_n_hit;

/* Measuring a subtree is not cheap: every property read parses an expression
 * string, and a container measures each child before drawing it, which then
 * measures that child's children again. The preferences screen measured 53
 * nodes 212 times. Sizes cannot change within a frame, so they are cached for
 * the duration of one.
 *
 * The key has to include the scope: `text FileTime(slot)` measures differently
 * for each save slot, and those differ only by their bindings. */
#define SL_MEASURE_CACHE 192

typedef struct {
    const sl_node *n;
    int   availw, availh;
    int   bind, xclude;
    char  prefix[32];
    int   w, h;
} sl_measured;

static sl_measured g_mcache[SL_MEASURE_CACHE];
static int         g_nmcache;
/* The most any one frame has needed. measure_store gives up silently when the
 * table is full, so a screen bigger than the table degrades into re-measuring
 * rather than failing, and nothing says so. This is how to tell whether 192
 * is still enough -- at the cap, the cache has stopped working. */
static int         g_mcache_hi;

/* For memreport. Static, so the allocation report cannot see these bytes. */
void mr_sl_measure_cache_stats(long *bytes, int *high_water, int *capacity)
{
    *bytes = (long)sizeof(g_mcache);
    *high_water = g_mcache_hi;
    *capacity = SL_MEASURE_CACHE;
}

static sl_measured *measure_cached(const sl_node *n, const sl_ctx *ctx,
                                   int availw, int availh)
{
    int i;

    for (i = 0; i < g_nmcache; i++) {
        sl_measured *m = &g_mcache[i];
        if (m->n == n && m->availw == availw && m->availh == availh &&
            m->bind == g_env.bind && m->xclude == g_env.xclude &&
            !strcmp(m->prefix, ctx->prefix))
            return m;
    }
    return NULL;
}

static void measure_store(const sl_node *n, const sl_ctx *ctx,
                          int availw, int availh, int w, int h)
{
    sl_measured *m;

    if (g_nmcache >= SL_MEASURE_CACHE)
        return;

    m = &g_mcache[g_nmcache++];
    if (g_nmcache > g_mcache_hi)
        g_mcache_hi = g_nmcache;
    m->n = n;
    m->availw = availw;
    m->availh = availh;
    m->bind = g_env.bind;
    m->xclude = g_env.xclude;
    strncpy(m->prefix, ctx->prefix, sizeof(m->prefix) - 1);
    m->prefix[sizeof(m->prefix) - 1] = '\0';
    m->w = w;
    m->h = h;
}

static void measure(const sl_node *n, const sl_ctx *ctx,
                    int availw, int availh, int *w, int *h)
{
    char style[64];
    int mg[4];

    *w = 0;
    *h = 0;
    if (!n || ctx->depth > SL_MAX_DEPTH)
        return;

    {
        sl_measured *hit = measure_cached(n, ctx, availw, availh);
        if (hit) {
            g_n_hit++;
            *w = hit->w;
            *h = hit->h;
            return;
        }
    }

    g_n_measure++;

    node_style(n, ctx, style, sizeof(style));
    get_margin(n, style, mg);

    measure_inner(n, ctx, availw - mg[0] - mg[2], availh - mg[1] - mg[3], w, h);

    *w += mg[0] + mg[2];
    *h += mg[1] + mg[3];

    measure_store(n, ctx, availw, availh, *w, *h);
}

/* Paint a background, which may be an image path or a colour. */
/* --- Frame backgrounds -----------------------------------------------------
 *
 *     background Frame(["gui/confirm_frame.png", "gui/frame.png"],
 *                      gui.confirm_frame_borders, tile=gui.frame_tile)
 *
 * A nine-slice: the four corners are drawn at their own size, the four edges
 * are stretched along their length, and the centre is stretched both ways.
 * That is what puts a border on a box of any size -- gui/frame.png is a blue
 * rule around black, and the confirm dialog is that art at the size the text
 * needs.
 *
 * Parsed from the property's *source*, not from its value: the first argument
 * is a list of candidate images, and this engine's values have no list type.
 * The list is a fallback chain, so the first one the VFS has wins.
 */

typedef struct {
    mr_surface *img;
    int l, t, r, b;
    int tile;
} sl_frame;

static int parse_frame(const char *expr, sl_frame *out)
{
    char arg[256];
    const char *p;

    memset(out, 0, sizeof(*out));

    p = mr_skip_ws(expr);
    if (strncmp(p, "Frame", 5))
        return 0;
    p = mr_skip_ws(p + 5);
    if (*p != '(')
        return 0;
    p = mr_read_arg(p + 1, arg, sizeof(arg));

    /* The image: one name, or a bracketed list of them to try in order. */
    {
        const char *q = mr_skip_ws(arg);
        char name[160];

        if (*q == '[') {
            /* A fallback chain, and the earlier entries are *expected* to be
             * missing. The stock GUI writes
             *
             *     Frame(["gui/confirm_frame.png", "gui/frame.png"], ...)
             *
             * where confirm_frame.png is an override a game may drop in to
             * give the confirm dialog its own art; Ren'Py does not ship one
             * either (gui/game/screens.rpy:1189 lists it, gui/game/gui/ does
             * not contain it). So a miss here is the normal case and must be
             * silent: asking the image layer to load it logged "cannot open"
             * and "failed to decode" every time the dialog was drawn.
             *
             * Existence is checked first for anything that looks like a file
             * -- a name with a dot in it. A bare name is a declared image,
             * which the VFS knows nothing about, so that still goes to
             * mr_image_get. */
            q = mr_skip_ws(q + 1);
            while (*q && *q != ']') {
                char item[160];
                q = mr_read_arg(q, item, sizeof(item));
                if (mr_read_string(item, name, sizeof(name))) {
                    if (!strchr(name, '.') || mr_vfs_exists(name))
                        out->img = mr_image_get(name);
                }
                if (out->img)
                    break;
                q = mr_skip_ws(q);
            }
            if (!out->img)
                mr_log("slrender: no image in %.70s", expr);
        } else if (mr_read_string(q, name, sizeof(name))) {
            out->img = mr_image_get(name);
        }
    }
    if (!out->img)
        return 0;

    /* The borders, which Borders() leaves as four numbers in a string. */
    p = mr_read_arg(p, arg, sizeof(arg));
    {
        mr_value v;
        mr_expr_eval(arg, &v);
        if (v.type == MR_V_STR)
            sscanf(v.s, "%d %d %d %d", &out->l, &out->t, &out->r, &out->b);
    }

    /* tile=, which this game defines as False; tiling is the fallback for an
     * image with no borders at all, so the flag only has to be read. */
    while (*p && *p != ')') {
        p = mr_read_arg(p, arg, sizeof(arg));
        if (!strncmp(arg, "tile", 4)) {
            const char *eq = strchr(arg, '=');
            if (eq) {
                mr_value v;
                mr_expr_eval(eq + 1, &v);
                out->tile = mr_value_truthy(&v);
            }
        }
    }
    return 1;
}

/* One slice. A zero-sized slice is skipped rather than drawn degenerate --
 * a border of 0 is legal and means that edge has no art. */
static void slice(mr_surface *dst, mr_surface *src,
                  int sx, int sy, int sw, int sh,
                  int dx, int dy, int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    /* At 1:1 the nearest-neighbour map is the identity, so the corners go
     * through the same path as the stretched edges. */
    mr_blit_stretch(dst, src, sx, sy, sw, sh, dx, dy, dw, dh);
}

static void draw_frame(mr_surface *dst, const sl_frame *fr,
                       int x, int y, int w, int h)
{
    mr_surface *s = fr->img;
    int l = fr->l, t = fr->t, r = fr->r, b = fr->b;
    int smw, smh, dmw, dmh;

    /* Borders larger than the box would overlap; Ren'Py shrinks them to fit,
     * and so must we or the corners would draw over each other. */
    if (l + r > w) { l = l * w / (l + r); r = w - l; }
    if (t + b > h) { t = t * h / (t + b); b = h - t; }

    smw = s->w - l - r;          /* source middle */
    smh = s->h - t - b;
    dmw = w - l - r;             /* destination middle */
    dmh = h - t - b;

    /* corners */
    slice(dst, s, 0,          0,          l, t, x,           y,           l, t);
    slice(dst, s, s->w - r,   0,          r, t, x + w - r,   y,           r, t);
    slice(dst, s, 0,          s->h - b,   l, b, x,           y + h - b,   l, b);
    slice(dst, s, s->w - r,   s->h - b,   r, b, x + w - r,   y + h - b,   r, b);

    /* edges */
    slice(dst, s, l,          0,        smw, t, x + l,       y,         dmw, t);
    slice(dst, s, l,          s->h - b, smw, b, x + l,       y + h - b, dmw, b);
    slice(dst, s, 0,          t,        l, smh, x,           y + t,     l, dmh);
    slice(dst, s, s->w - r,   t,        r, smh, x + w - r,   y + t,     r, dmh);

    /* centre */
    slice(dst, s, l, t, smw, smh, x + l, y + t, dmw, dmh);
}

void mr_sl_draw_frame_image(mr_surface *dst, const char *image,
                            int l, int t, int r, int b,
                            int x, int y, int w, int h)
{
    sl_frame fr;

    memset(&fr, 0, sizeof(fr));
    fr.img = mr_image_get(image);
    if (!fr.img || w <= 0 || h <= 0)
        return;
    fr.l = l;
    fr.t = t;
    fr.r = r;
    fr.b = b;
    draw_frame(dst, &fr, x, y, w, h);
}

static void draw_background(mr_surface *dst, const sl_node *n,
                            const char *style, const char *state,
                            int x, int y, int w, int h);

static mr_u32 color_prop(const sl_node *n, const char *style,
                         const char *state, const char *name, mr_u32 dflt);

mr_u32 mr_sl_style_color(const char *style, const char *state,
                         const char *name, mr_u32 dflt)
{
    return color_prop(NULL, style, state, name, dflt);
}

void mr_sl_draw_style_background(mr_surface *dst, const char *style,
                                 const char *state, int x, int y,
                                 int w, int h)
{
    /* No node: every property comes from the style table. The whole point is
     * that a caller outside the screen language -- the choice menu, which is
     * drawn natively -- gets pixel-identical backgrounds to a real button. */
    draw_background(dst, NULL, style, state, x, y, w, h);
}

static void draw_background(mr_surface *dst, const sl_node *n,
                            const char *style, const char *state,
                            int x, int y, int w, int h)
{
    char bg[160];
    mr_surface *img;
    const char *e;
    sl_frame fr;

    /* Read the source first: a Frame has to be taken apart rather than
     * evaluated, because its first argument is a list. */
    e = prop_expr(n, style, state, "background");
    if (e && parse_frame(e, &fr)) {
        draw_frame(dst, &fr, x, y, w, h);
        ext_add(x, y, w, h);
        return;
    }

    str_prop(n, style, state, "background", bg, sizeof(bg));
    if (!bg[0])
        return;
    ext_add(x, y, w, h);

    if (bg[0] == '#') {
        unsigned r = 0, g = 0, b = 0;
        int yy, xx, cx, cy, cw, ch;
        if (strlen(bg) < 7)
            return;
        sscanf(bg + 1, "%2x%2x%2x", &r, &g, &b);
        mr_clip_rect(dst, &cx, &cy, &cw, &ch);
        for (yy = y; yy < y + h; yy++) {
            mr_u32 *row;
            if (yy < cy || yy >= cy + ch) continue;
            row = dst->px + (size_t)yy * dst->pitch;
            for (xx = x; xx < x + w; xx++) {
                if (xx < cx || xx >= cx + cw) continue;
                row[xx] = MR_RGB(r, g, b);
            }
        }
        return;
    }

    img = mr_image_get(bg);
    if (!img)
        return;

    /* A background is a Frame in Ren'Py terms; without border information
     * here, tile it by simple repetition so a small plate still covers the
     * whole area rather than leaving gaps. */
    {
        int yy, xx;
        for (yy = 0; yy < h; yy += img->h)
            for (xx = 0; xx < w; xx += img->w)
                mr_blit(dst, img, x + xx, y + yy);
    }
}

/* The preference a bar drives, lifted out of `value Preference("name")`. The
 * expression is not evaluated for this: we want the name, not the number. */
static void bar_pref(const sl_node *n, char *out, int outsz)
{
    const char *e = sl_node_prop(n, "value");
    const char *q;
    int i = 0;

    out[0] = '\0';
    if (!e)
        return;
    q = strchr(e, '"');
    if (!q)
        q = strchr(e, '\'');
    if (!q)
        return;
    q++;
    while (*q && *q != '"' && *q != '\'' && i < outsz - 1)
        out[i++] = *q++;
    out[i] = '\0';
}

/* Bars drawn this frame, and how far along each one was drawn *in pixels*.
 *
 * A bar can move without anything else on the screen moving -- the jukebox's
 * position bar does it once a second or so -- and the frame loop skips
 * rendering entirely when nothing has changed. Rather than repaint the screen
 * on a timer, each bar's value is re-read between frames and compared against
 * the pixel it was last drawn at: a redraw is asked for exactly when one more
 * pixel of the bar would be filled, and never otherwise. A four-minute track
 * across a 901px bar comes to about three redraws a second while the jukebox
 * is open, and none at all when nothing is playing.
 *
 * The value is quantised because that is what the player can see. The
 * position changes continuously; the picture does not. */
#define SL_MAX_LIVEBARS 8

typedef struct {
    const sl_node *n;
    int w, h;
    int vertical;
    int filled;            /* pixels, as drawn */
} sl_livebar;

static sl_livebar g_livebar[SL_MAX_LIVEBARS];
static int        g_nlivebar;

/* The 0..1 value of a bar node, clamped. */
static float bar_value_of(const sl_node *n)
{
    const char *e = sl_node_prop(n, "value");
    mr_value v;
    float value = 0.0f;

    if (e) {
        mr_expr_eval(e, &v);
        if (v.type == MR_V_FLOAT)
            value = (float)v.f;
        else if (v.type == MR_V_INT)
            value = (float)v.i;
    }
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value;
}

static int bar_filled(const sl_livebar *b, float value)
{
    return (int)(value * (float)(b->vertical ? b->h : b->w));
}

static void livebar_reset(void)
{
    g_nlivebar = 0;
}

static void livebar_record(const sl_node *n, int w, int h, int vertical,
                           int filled)
{
    sl_livebar *b;

    if (!g_capture || g_nlivebar >= SL_MAX_LIVEBARS)
        return;
    b = &g_livebar[g_nlivebar++];
    b->n = n;
    b->w = w;
    b->h = h;
    b->vertical = vertical;
    b->filled = filled;
}

int mr_sl_bars_moved(void)
{
    int i;

    for (i = 0; i < g_nlivebar; i++) {
        if (bar_filled(&g_livebar[i], bar_value_of(g_livebar[i].n))
            != g_livebar[i].filled)
            return 1;
    }
    return 0;
}

/* Draw a bar, which Ren'Py builds one of two ways: either a pair of images
 * meeting at the value (left_bar/right_bar, what the preference sliders use),
 * or a track with a thumb riding along it (base_bar/thumb, the scrollbars). */
static void draw_bar(mr_surface *dst, const sl_node *n, const char *style,
                     int x, int y, int w, int h)
{
    int vertical = !strcmp(n->type, "vbar") ||
                   truthy_prop(n, style, "bar_vertical");
    int hovered, id;
    const char *state;
    char path[MR_MAX_NAME];
    mr_surface *img;
    float value = 0.0f;
    int filled;

    /* Record the bar before drawing, so its hover state is known. */
    id = -1;
    if (g_capture && g_nfocus < SL_MAX_FOCUS) {
        sl_focus *f = &g_focus[g_nfocus];
        char pref[64];

        bar_pref(n, pref, sizeof(pref));
        if (pref[0]) {
            memset(f, 0, sizeof(*f));
            f->kind = SL_FOCUS_BAR;
            f->x = x; f->y = y; f->w = w; f->h = h;
            f->vertical = vertical;
            strncpy(f->pref, pref, sizeof(f->pref) - 1);
            id = g_nfocus++;
        }
    }

    hovered = (id >= 0 && id == g_hover);
    state = hovered ? "hover_" : "idle_";

    value = bar_value_of(n);

    /* Two-image form: the filled part and the empty part meet at the value. */
    image_prop(n, style, state, vertical ? "top_bar" : "left_bar",
               path, sizeof(path));
    if (path[0]) {
        char path2[MR_MAX_NAME];
        mr_surface *a = mr_image_get(path);
        mr_surface *b;

        image_prop(n, style, state, vertical ? "bottom_bar" : "right_bar",
                   path2, sizeof(path2));
        b = path2[0] ? mr_image_get(path2) : NULL;

        if (vertical) {
            /* A vertical bar fills from the bottom, so the value is the
             * height of the lower piece. */
            filled = (int)(value * (float)h);
            draw_tiled(dst, b, x, y, w, h - filled);
            draw_tiled(dst, a, x, y + h - filled, w, filled);
        } else {
            filled = (int)(value * (float)w);
            draw_tiled(dst, a, x, y, filled, h);
            draw_tiled(dst, b, x + filled, y, w - filled, h);
        }
        livebar_record(n, w, h, vertical, filled);
        return;
    }

    /* Track-and-thumb form. */
    image_prop(n, style, state, "base_bar", path, sizeof(path));
    img = path[0] ? mr_image_get(path) : NULL;
    draw_tiled(dst, img, x, y, w, h);

    image_prop(n, style, state, "thumb", path, sizeof(path));
    img = path[0] ? mr_image_get(path) : NULL;
    if (img) {
        if (vertical) {
            int th = img->h < h ? img->h : h / 4;
            if (th < 8) th = 8;
            draw_tiled(dst, img, x, y + (int)(value * (float)(h - th)), w, th);
        } else {
            int tw = img->w < w ? img->w : w / 4;
            if (tw < 8) tw = 8;
            draw_tiled(dst, img, x + (int)(value * (float)(w - tw)), y, tw, h);
        }
    }

    livebar_record(n, w, h, vertical, (int)(value * (float)(vertical ? h : w)));
}

/* Draw a viewport: measure the child freely, then show a window onto it at
 * the current scroll position, with a scrollbar beside it if one was asked
 * for and the content is actually taller than the window. */
static void draw_viewport(mr_surface *dst, const sl_node *n, const sl_ctx *ctx,
                          const char *style, int x, int y, int w, int h)
{
    sl_kidlist kids;
    sl_ctx sub = *ctx;
    sl_mark mark;
    sl_env saveenv;
    sl_scroll *sc = scroll_for(n);
    char bars[32];
    int has_bar, barw = 0;
    int content = 0, cw = 0;
    int spacing, vpcols = 0;
    int i;

    sub.depth++;
    spacing = num_px(get_num(n, style, NULL, "spacing"), 0, 0);

    /* A vpgrid is a viewport whose children are laid out in a grid rather
     * than stacked on top of each other; `cols 1` makes it a scrolling
     * column, which is what the history screen uses. */
    if (!strcmp(n->type, "vpgrid")) {
        sl_num c = get_num(n, style, NULL, "cols");
        vpcols = c.present && c.i > 0 ? (int)c.i : 1;
    }
    draw_background(dst, n, style, NULL, x, y, w, h);

    str_prop(n, style, NULL, "scrollbars", bars, sizeof(bars));
    has_bar = (bars[0] && strcmp(bars, "None") != 0);

    mark = scope_mark();
    saveenv = g_env;
    kids.n = 0;
    collect(n, &kids, 0);

    /* Measure the content against an effectively unbounded height: that is
     * what makes a viewport a viewport rather than a squashed box. */
    if (has_bar) {
        char bstyle[64];
        mr_surface *img;
        char path[MR_MAX_NAME];

        sprintf(bstyle, "%.55s_vscrollbar", ctx->prefix[0] ? ctx->prefix : "gui");
        image_prop(NULL, "vscrollbar", "idle_", "base_bar", path, sizeof(path));
        img = path[0] ? mr_image_get(path) : NULL;
        barw = img ? img->w : 12;
        (void)bstyle;
    }

    cw = w - barw;
    {
        int col = 0, rowh = 0;
        for (i = 0; i < kids.n; i++) {
            int kw, kh;
            sl_ctx kctx = sub;
            g_env = kids.e[i];
            enter_child(&kctx);
            measure(kids.v[i], &kctx, vpcols > 0 ? cw / vpcols : cw,
                    SL_UNBOUNDED, &kw, &kh);
            if (vpcols > 0) {
                if (kh > rowh)
                    rowh = kh;
                if (++col >= vpcols) {
                    col = 0;
                    content += rowh + spacing;
                    rowh = 0;
                }
            } else if (kh > content) {
                content = kh;
            }
        }
        if (vpcols > 0)
            content += rowh;
    }

    if (sc) {
        if (!sc->inited) {
            sl_num yi = get_num(n, style, NULL, "yinitial");
            sc->inited = 1;
            if (yi.present)
                sc->pos = (int)(yi.f * (float)(content - h));
        }
        sc->pos = scroll_clamp(sc->pos, content, h);
    }

    /* Now draw the content, shifted up by the scroll position and cut off at
     * the viewport's edge. A vpgrid stacks its children into `cols` columns;
     * a plain viewport has one child that it simply shows. */
    mr_clip_push(x, y, cw, h);
    {
        int run = 0;
        int col = 0;
        int rowh = 0;

        for (i = 0; i < kids.n; i++) {
            int kw, kh, ox, oy;
            sl_ctx kctx = sub;

            g_env = kids.e[i];
            enter_child(&kctx);
            measure(kids.v[i], &kctx, vpcols > 0 ? cw / vpcols : cw,
                    SL_UNBOUNDED, &kw, &kh);

            if (vpcols > 0) {
                ox = x + col * (cw / vpcols);
                oy = y + run - (sc ? sc->pos : 0);
                draw(dst, kids.v[i], &kctx, ox, oy, cw / vpcols, kh);
                if (kh > rowh)
                    rowh = kh;
                if (++col >= vpcols) {
                    col = 0;
                    run += rowh + spacing;
                    rowh = 0;
                }
            } else {
                place(kids.v[i], &kctx, x, y, cw, kh, kw, kh, &ox, &oy);
                draw(dst, kids.v[i], &kctx, ox, y - (sc ? sc->pos : 0),
                     kw, kh);
            }
        }

        /* The content is as tall as the stack, not as tall as one child. */
        if (vpcols > 0)
            content = run + rowh;
    }
    mr_clip_pop();

    g_env = saveenv;
    scope_release(mark);

    /* The scrollbar. Drawn directly rather than as a `side` child, because it
     * is the viewport that knows the content size. */
    if (has_bar && content > h && barw > 0) {
        char path[MR_MAX_NAME];
        mr_surface *img;
        int bx = x + w - barw;
        int thumb_h = (int)((float)h * (float)h / (float)content);
        int thumb_y;
        int id = -1;

        if (thumb_h < 16)
            thumb_h = 16;
        thumb_y = (int)((float)(h - thumb_h) * (float)sc->pos /
                        (float)(content - h));

        if (g_capture && g_nfocus < SL_MAX_FOCUS) {
            sl_focus *f = &g_focus[g_nfocus];
            memset(f, 0, sizeof(*f));
            f->kind = SL_FOCUS_SCROLL;
            f->x = bx; f->y = y; f->w = barw; f->h = h;
            f->vp = n;
            f->content = content;
            f->view = h;
            f->vx = x;
            f->vw = w;
            id = g_nfocus++;
        }

        image_prop(NULL, "vscrollbar", (id >= 0 && id == g_hover)
                   ? "hover_" : "idle_", "base_bar", path, sizeof(path));
        img = path[0] ? mr_image_get(path) : NULL;
        draw_tiled(dst, img, bx, y, barw, h);

        image_prop(NULL, "vscrollbar", (id >= 0 && id == g_hover)
                   ? "hover_" : "idle_", "thumb", path, sizeof(path));
        img = path[0] ? mr_image_get(path) : NULL;
        draw_tiled(dst, img, bx, y + thumb_y, barw, thumb_h);
    }
}

/* MR_LAYOUT=1 dumps one frame's layout to the log: every node with the
 * style it resolved to and the rectangle it was given. Alignment questions
 * are otherwise answered by squinting at screenshots. */
static void dump_node(const sl_node *n, const char *style,
                      int x, int y, int w, int h, int depth)
{
    char indent[64];
    char text[64];
    int i, d = depth < 20 ? depth : 20;

    for (i = 0; i < d * 2; i++)
        indent[i] = ' ';
    indent[d * 2] = '\0';

    /* The text too: which panel a screen is showing is otherwise invisible
     * in a dump of boxes. */
    text[0] = '\0';
    if (!strcmp(n->type, "text") || !strcmp(n->type, "label") ||
        !strcmp(n->type, "textbutton") || !strcmp(n->type, "input")) {
        char buf[MR_MAX_TEXT];
        node_text(n, buf, sizeof(buf));
        sprintf(text, " \"%.40s\"", buf);
    }

    mr_log("layout: %s%s [%s] %d,%d %dx%d%s",
           indent, n->type, style, x, y, w, h, text);
}

static void draw_inner(mr_surface *dst, const sl_node *n, const sl_ctx *ctx,
                       int x, int y, int availw, int availh)
{
    char style[64];
    sl_kidlist kids;
    int i;

    if (!n || ctx->depth > SL_MAX_DEPTH)
        return;

    /* Let the platform come up for air between widgets. On DOS that is what
     * keeps the mouse pointer moving while a screen is being drawn, which
     * on the target is long enough to notice; everywhere else it does
     * nothing. It is rate-gated on the platform side, so the cost here is a
     * call and a compare per node, and the right number of nodes to do it
     * on is all of them.
     *
     * See mr_plat_service in plat.h -- and note that it is forbidden to
     * dispatch input, which is the only reason it can sit in the middle of
     * a render pass at all. */
    mr_plat_service();

    node_style(n, ctx, style, sizeof(style));

    if (g_dump)
        dump_node(n, style, x, y, availw, availh, ctx->depth);

    if (!strcmp(n->type, "text") ||
        !strcmp(n->type, "textbutton") ||
        !strcmp(n->type, "label") ||
        !strcmp(n->type, "input")) {
        char buf[MR_MAX_TEXT];
        char tstyle[64];
        const mr_font *f;
        int pad[4] = { 0, 0, 0, 0 };
        mr_u32 col;
        int wrap = 0;
        const char *state = "idle_";

        if (!strcmp(n->type, "text") || !strcmp(n->type, "input"))
            strncpy(tstyle, style, sizeof(tstyle) - 1);
        else
            text_child_style(style, tstyle, sizeof(tstyle));
        tstyle[sizeof(tstyle) - 1] = '\0';

        if (strcmp(n->type, "text") && strcmp(n->type, "input")) {
            int id = add_focus(n, x, y, availw, availh);
            state = button_state(n, id >= 0 && id == g_hover);
            get_padding(n, style, pad);
            draw_background(dst, n, style, state, x, y, availw, availh);
        }
        ext_add(x, y, availw, availh);

        node_text(n, buf, sizeof(buf));
        f = font_for_node(n, tstyle);
        if (!f)
            return;

        col = color_prop(n, tstyle, state, "color", MR_RGB(255, 255, 255));

        wrap = text_wrap_width(n, style, tstyle, availw);
        if (wrap <= 0) {
            int room = availw - pad[0] - pad[2];
            if (room > 0 && mr_font_text_width(f, buf) > room)
                wrap = room;
        }

        /* Text with its own line breaks has to go through the wrapped path
         * even when nothing wraps it, because that is the only one that
         * honours a newline: mr_font_draw lays a single run and emits no
         * glyph for '\n', so the two lines ran together into one that then
         * overflowed the frame. Its natural width is the widest line, so
         * laying it out at that width breaks exactly where the text says and
         * nowhere else -- and the short line gets centred under the long one
         * the way Ren'Py draws it. */
        if (wrap <= 0 && strchr(buf, '\n'))
            wrap = mr_font_text_width(f, buf);

        if (wrap > 0)
            mr_font_draw_aligned(dst, f, x + pad[0], y + pad[1], wrap,
                                 buf, col, 0, 0, -1,
                                 text_align_of(n, tstyle));
        else
            mr_font_draw(dst, f, x + pad[0], y + pad[1], buf, col);

        /* The foreground goes over the text, which is the whole point of it
         * being a foreground: it is how a radio or check button draws its
         * marker. `style radio_button: foreground
         * "gui/button/radio_[prefix_]foreground.png"`, and the 18px left
         * border in gui.radio_button_borders is the room the marker sits in,
         * so it is drawn at the button's own corner at its natural size --
         * the same as Ren'Py rendering a plain Image into the window. */
        if (strcmp(n->type, "text") && strcmp(n->type, "input")) {
            char fg[MR_MAX_NAME];
            image_prop(n, style, state, "foreground", fg, sizeof(fg));
            if (fg[0]) {
                mr_surface *img = mr_image_get(fg);
                if (img)
                    mr_blit(dst, img, x, y);
            }
        }
        return;
    }

    if (!strcmp(n->type, "add")) {
        char name[MR_MAX_NAME];
        mr_surface *img;
        node_text(n, name, sizeof(name));
        img = screen_image(name);
        if (img) {
            mr_blit(dst, img, x, y);
            ext_add(x, y, img->w, img->h);
        }
        return;
    }

    if (!strcmp(n->type, "imagebutton")) {
        int id = add_focus(n, x, y, availw, availh);
        mr_surface *img = imagebutton_surface(n, id >= 0 && id == g_hover);
        if (img) {
            mr_blit(dst, img, x, y);
            ext_add(x, y, img->w, img->h);
        }
        return;
    }

    if (!strcmp(n->type, "null"))
        return;

    /* `key "game_menu" action ShowMenu("main_menu")` draws nothing; it binds
     * a keysym for as long as the screen is up. Recorded here, while the
     * screen is being laid out, because that is what knows the screen is on
     * display -- and because a screen can only bind a key while it is. */
    if (!strcmp(n->type, "key")) {
        const char *act = sl_node_prop(n, "action");
        const char *sym = n->arg;
        char name[64];

        if (act && *act && sym && mr_read_string(sym, name, sizeof(name)))
            key_bind(name, act);
        return;
    }

    /* `dismiss action X` is a click anywhere that nothing else took. It draws
     * nothing and takes no space -- it is purely a hit area, and a
     * full-screen one, because "anywhere" includes the parts of the screen no
     * displayable covers.
     *
     * It ends up lowest priority for free: mr_sl_hit runs back to front, and
     * a screen declares its dismiss before the content, so every button and
     * scrollbar declared afterwards is tested first. That is exactly the
     * ordering screens.rpy counts on when it notes the navigation buttons and
     * the scrollbar still receive clicks. */
    if (!strcmp(n->type, "dismiss")) {
        add_focus(n, 0, 0, MR_STAGE_W, MR_STAGE_H);
        return;
    }

    if (!strcmp(n->type, "bar") || !strcmp(n->type, "vbar")) {
        draw_bar(dst, n, style, x, y, availw, availh);
        ext_add(x, y, availw, availh);
        return;
    }

    if (!strcmp(n->type, "viewport") || !strcmp(n->type, "vpgrid")) {
        draw_viewport(dst, n, ctx, style, x, y, availw, availh);
        return;
    }

    /* Containers. */
    {
        int isv = !strcmp(n->type, "vbox");
        int ish = !strcmp(n->type, "hbox");
        int spacing = num_px(get_num(n, style, NULL, "spacing"), 0, 0);
        int pad[4];
        int cx, cy, cw, ch;
        int cols = 0, rows = 0;
        sl_ctx sub = *ctx;
        const char *sp = sl_node_prop(n, "style_prefix");
        sl_mark mark;
        sl_env  saveenv;
        int clipped = 0;

        if (sp) {
            mr_value v;
            mr_expr_eval(sp, &v);
            if (v.type == MR_V_STR)
                strncpy(sub.prefix, v.s, sizeof(sub.prefix) - 1);
        }
        sub.depth++;

        get_padding(n, style, pad);
        grid_shape(n, &cols, &rows);

        /* A plain `button` is a container, not one of the text displayables,
         * so it needs registering here or it cannot be clicked at all. The
         * save slots are exactly this: `button:` with `has vbox` inside. */
        {
            int id = add_focus(n, x, y, availw, availh);
            const char *state = button_state(n, id >= 0 && id == g_hover);
            draw_background(dst, n, style,
                            sl_node_prop(n, "action") ? state : NULL,
                            x, y, availw, availh);
        }

        cx = x + pad[0];
        cy = y + pad[1];
        cw = availw - pad[0] - pad[2];
        ch = availh - pad[1] - pad[3];

        /* A container whose style pins its size keeps its contents inside
         * it. Without this, text longer than the box was simply drawn past
         * the edge and over whatever was next to it: a save slot's label
         * comes from the game's `save_name`, and a long one wrapped to more
         * lines than the slot is tall and ran down into the row below --
         * over the next save's thumbnail.
         *
         * Only for a pinned size, which is the case where something really
         * did say how big this is (`gui.button_properties` sets xsize and
         * ysize from gui.<kind>_width/height). A box that merely got a small
         * `availw` from its parent is not making a claim and must not clip,
         * or every tight layout starts losing glyphs silently.
         *
         * Ren'Py does not clip here either; see the note in font.c about the
         * same trade. Truncating is the lesser harm. */
        if (!cols && (get_num(n, style, NULL, "xsize").present ||
                      get_num(n, style, NULL, "ysize").present)) {
            mr_clip_push(x, y, availw, availh);
            clipped = 1;
        }

        mark = scope_mark();
        saveenv = g_env;
        kids.n = 0;
        collect(n, &kids, 0);

        if (cols > 0) {
            /* Uniform cells, filled left to right then top to bottom. */
            int cellw = (cw - spacing * (cols - 1)) / cols;
            int cellh = (ch - spacing * (rows - 1)) / rows;
            if (cellw < 0) cellw = 0;
            if (cellh < 0) cellh = 0;
            for (i = 0; i < kids.n && i < cols * rows; i++) {
                int gx = cx + (i % cols) * (cellw + spacing);
                int gy = cy + (i / cols) * (cellh + spacing);
                sl_ctx kctx = sub;
                g_env = kids.e[i];
                enter_child(&kctx);
                draw(dst, kids.v[i], &kctx, gx, gy, cellw, cellh);
            }
        } else if (isv || ish) {
            /* Boxes stack; the cross axis is still aligned per child. */
            int run = 0;
            for (i = 0; i < kids.n; i++) {
                int kw, kh, ox, oy;
                int aw = ish ? cw - run : cw;
                int ah = isv ? ch - run : ch;
                sl_ctx kctx = sub;

                if (aw < 0) aw = 0;
                if (ah < 0) ah = 0;

                g_env = kids.e[i];
                enter_child(&kctx);
                measure(kids.v[i], &kctx, aw, ah, &kw, &kh);

                if (isv) {
                    place(kids.v[i], &kctx, cx, cy, cw, kh, kw, kh, &ox, &oy);
                    oy = cy + run;
                    draw(dst, kids.v[i], &kctx, ox, oy, kw, kh);
                    run += kh + spacing;
                } else {
                    place(kids.v[i], &kctx, cx, cy, kw, ch, kw, kh, &ox, &oy);
                    ox = cx + run;
                    draw(dst, kids.v[i], &kctx, ox, oy, kw, kh);
                    run += kw + spacing;
                }
            }
        } else {
            for (i = 0; i < kids.n; i++) {
                int kw, kh, ox, oy;
                sl_ctx kctx = sub;
                g_env = kids.e[i];
                enter_child(&kctx);
                measure(kids.v[i], &kctx, cw, ch, &kw, &kh);
                place(kids.v[i], &kctx, cx, cy, cw, ch, kw, kh, &ox, &oy);
                draw(dst, kids.v[i], &kctx, ox, oy, kw, kh);
            }
        }

        g_env = saveenv;
        scope_release(mark);
        if (clipped)
            mr_clip_pop();
    }
}

static void draw(mr_surface *dst, const sl_node *n, const sl_ctx *ctx,
                 int x, int y, int availw, int availh)
{
    char style[64];
    int mg[4];

    if (!n || ctx->depth > SL_MAX_DEPTH)
        return;

    g_n_draw++;

    node_style(n, ctx, style, sizeof(style));
    get_margin(n, style, mg);

    draw_inner(dst, n, ctx, x + mg[0], y + mg[1],
               availw - mg[0] - mg[2], availh - mg[1] - mg[3]);
}

void mr_sl_render(mr_surface *dst, const sl_screen *sc, int capture, int hover)
{
    sl_ctx ctx;

    if (!sc || !sc->root)
        return;

    g_capture = capture;
    g_hover = hover;

    if (sc->modal && capture)
        g_modal_floor = g_nfocus;

    strncpy(g_screen_name, sc->name, sizeof(g_screen_name) - 1);
    g_screen_name[sizeof(g_screen_name) - 1] = '\0';

    /* Each screen gets a fresh scope, holding its own parameters. */
    g_nbind = 0;
    g_nxc = 0;
    g_env.bind = -1;
    g_env.xclude = -1;
    g_rendering = 1;

    /* The screen's own variables come first, so `default` and
     * SetScreenVariable are visible to everything in it. */
    {
        int i;
        for (i = 0; i < SL_MAX_SCREENVAR; i++) {
            if (g_svars[i].used && !strcmp(g_svars[i].screen, sc->name))
                bind_push(&g_env.bind, g_svars[i].name, &g_svars[i].value);
        }
    }

    if (sc->nparams > 0) {
        int i;
        for (i = 0; i < sc->nparams; i++) {
            mr_value v;
            /* An argument pushed in from outside is already bound above, and
             * binding the default on top of it would shadow it -- the later
             * push is the one a lookup finds. */
            if (screenvar_find(sc->params[i]))
                continue;
            memset(&v, 0, sizeof(v));
            if (sc->defaults[i])
                mr_expr_eval(sc->defaults[i], &v);
            bind_push(&g_env.bind, sc->params[i], &v);
        }
    }

    memset(&ctx, 0, sizeof(ctx));

    /* Dump one frame only; a per-frame dump would bury the log. */
    if (g_dump_request && capture) {
        g_dump = 1;
        mr_log("layout: --- screen '%s' ---", sc->name);
    }

    g_n_measure = 0;
    g_n_draw = 0;
    g_n_hit = 0;
    g_nmcache = 0;

    draw(dst, sc->root, &ctx, 0, 0, MR_STAGE_W, MR_STAGE_H);
    g_dump = 0;

    if (g_stats && capture) {
        extern long g_style_lookups, g_style_steps;
        mr_log("stats: screen '%s': %ld measure (%ld cached), %ld draw, "
               "%ld style lookups / %ld chain steps",
               sc->name, g_n_measure, g_n_hit, g_n_draw,
               g_style_lookups, g_style_steps);
        g_style_lookups = 0;
        g_style_steps = 0;
    }

    g_rendering = 0;
    g_capture = 0;
}

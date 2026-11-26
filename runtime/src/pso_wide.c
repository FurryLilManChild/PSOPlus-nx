/* 16:9 widescreen, build A (docs/widescreen-research.md sections 1-3 and 6): the three game hooks.
 *
 * 1. set_perspective_projection_from_fov (0x803D676C): every camera of the game builds its projection here,
 *    with the aspect 1.1333 (0.85 x 4:3). With the 16:9 screen on, the game's own function runs with the
 *    aspect k times wider (k = the EFB width factor, 854 / 640 = 1.334375) and the field of view angle
 *    adjusted so the vertical angle is unchanged (pso_wide.h wide_fov_for). Everything the function derives
 *    follows: the projection the GX library keeps (which the game's visibility tests project through, so
 *    objects at the new sides are not culled), screen_projection.width (3D-anchored sprites and particles),
 *    and the screen space matrix (2D drawn through the perspective projection fills the wide picture).
 *    Not for the radar's own overhead view (called from 0x801F93F0): the radar is HUD. In 4:3 the game's
 *    function runs unchanged (the picture is then bit for bit as in 1.0.0).
 * 2. TObject::render_nodes (0x8022A87C): the walk over the task tree that calls each object's render,
 *    done natively (the same loop: children at +0x14, next at +0x0C, flag 0x10 at +4 skips, render method at
 *    vtable +0x18 slot +0x10). In 16:9 it also knows the layer being drawn and the object's class name
 *    (the string at +0) and tells the GP thread, in order with the GX stream, where that object's 2D goes
 *    (pso_wide.h WIDE_*): HUD layers and UI classes 4:3 in the middle, speech bubbles and lens flare in
 *    "follow" mode (their place kept, their width as in 4:3), everything else as drawn.
 * 3. load_screen_space_matrix (0x803E20B0): runs the game's function, then tells the GP thread that
 *    position matrix 0 holds a screen space matrix, so it can tell the 2D draws (HUD, menus, sprites)
 *    from 3D ones (gx_render.c).
 *
 * Nothing here changes what the game sends online: only its local projection and the local placement of
 * the picture's parts (docs/widescreen-research.md section 4). */
#include <stdint.h>
#include <string.h>

#include "ppc.h"
#include "pso_wide.h"
#include "runtime.h"
#include "settings.h"

void f_803D676C_orig(Cpu* c);
void f_803E20B0_orig(Cpu* c);
void gx_host_mark(const uint8_t* b, int n);  /* gx_fifo.c */

#define RADAR_PROJ_RETURN 0x801F93F4u  /* TRaderMapCore::setup_overhead_projection's call 0x801F93F0 + 4 */
#define FOV_ANGLE_SCALE   0x805E0E18u  /* 0.85f, read by 0x803D67C4 */

static int wide_on(void) { return settings_wide_k() > 1.0f; }

/* ---- statistics for the [wide] line (game thread) ---- */
static unsigned long long g_proj_calls, g_proj_widened, g_ss_loads, g_marks, g_objects;

static void mark(int kind, int place, int layer, uint32_t name) {
    uint8_t b[8] = {GX_HOST_MARK, (uint8_t)kind, (uint8_t)place, (uint8_t)layer,
                    (uint8_t)(name >> 24), (uint8_t)(name >> 16), (uint8_t)(name >> 8), (uint8_t)name};
    gx_host_mark(b, 8);
    g_marks++;
}

/* ---- 1. the projection ---- */
void hle_wide_perspective(Cpu* c) {
    g_proj_calls++;
    float k = settings_wide_k();
    if (!(k > 1.0f) || c->lr == RADAR_PROJ_RETURN) { f_803D676C_orig(c); return; }
    float s = u32_as_f32(ld32r(FOV_ANGLE_SCALE));
    int32_t fov = (int32_t)c->r[3];
    float aspect = (float)c->f[1].ps0;
    c->r[3] = (uint32_t)wide_fov_for(fov, k, s);
    c->f[1].ps0 = (double)(aspect * k);
    f_803D676C_orig(c);
    g_proj_widened++;
    mark(WIDE_MARK_PROJ, 0, 0, 0);
}

/* ---- 3. the screen space matrix ---- */
void hle_screen_space_matrix(Cpu* c) {
    f_803E20B0_orig(c);
    if (!wide_on()) return;
    g_ss_loads++;
    /* the matrix depends only on the current projection's aspect and screen scale and the frame buffer
     * size (0x803E20CC-0x803E2104): tell the GP thread when those change; between changes it recognises the
     * same matrix in position matrix 0 by its values */
    static uint32_t last[4];
    uint32_t key[4] = {ld32r(0x8058CA60u + 0x54u), ld32r(0x8058CA60u + 0x4Cu), 0, 0};
    uint32_t fb = ld32r(0x805D7734u);
    if (fb) { key[2] = ld32r(fb + 4u); key[3] = 1; }
    static unsigned since;
    if (memcmp(key, last, sizeof key) || ++since >= 256) {  /* and now and then anyway (a mark is dropped
                                                             * while the game records a display list) */
        since = 0;
        memcpy(last, key, sizeof key);
        mark(WIDE_MARK_SS, 0, 0, 0);
    }
}

/* ---- 2. the task tree walk ---- */

/* class names whose objects are UI wherever they sit in the tree (docs/widescreen-research.md section 2,
 * checked against the decomp's class names); prefixes */
static const char* const k_ui_prefix[] = {
    "TWindow", "TAd", "TTitle", "TLogo", "TChCr", "TObjCM", "TNowLoading", "TLoadingCur", "TLevelSelect",
    "TCharSave", "TEndingPic", "TFontSmall", "TShop", "TQuestList", "TKeyBoard", "TRaderMap", "TCheckMemoryCard",
};
/* fixed-size 2D placed from a 3D point: kept at its place, as narrow as in 4:3 */
static const char* const k_follow_unit[] = {"TWindowFukidasi"};  /* speech bubbles: one piece */
static const char* const k_follow_draw[] = {"TEF_LensFlare"};    /* separate sprites, each placed on its own */

static int has_prefix(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

/* the class rule for one name: WIDE_FOLLOW_* / WIDE_HUD, or -1 (inherit from the parent) */
static int name_rule(const char* s) {
    for (size_t i = 0; i < sizeof k_follow_unit / sizeof *k_follow_unit; i++)
        if (!strcmp(s, k_follow_unit[i])) return WIDE_FOLLOW_UNIT;
    for (size_t i = 0; i < sizeof k_follow_draw / sizeof *k_follow_draw; i++)
        if (!strcmp(s, k_follow_draw[i])) return WIDE_FOLLOW_DRAW;
    for (size_t i = 0; i < sizeof k_ui_prefix / sizeof *k_ui_prefix; i++)
        if (has_prefix(s, k_ui_prefix[i])) return WIDE_HUD;
    if (has_prefix(s, "TObj") && strstr(s, "Win")) return WIDE_HUD;  /* TObjResultWin, TObjNewFloorWin, ... */
    return -1;
}

/* class names are static strings: a small cache from the name's address to its rule */
#define NAME_CACHE 1024
static struct { uint32_t name; int8_t rule; } g_name_cache[NAME_CACHE];
static int rule_of(uint32_t name) {
    if (!name) return -1;
    uint32_t h = (name >> 2) * 2654435761u >> 22;
    for (int k = 0; k < 8; k++) {
        uint32_t i = (h + (uint32_t)k) & (NAME_CACHE - 1);
        if (g_name_cache[i].name == name) return g_name_cache[i].rule;
        if (!g_name_cache[i].name) {
            int rule = -1;
            if (guest_span_ok(name, 1)) {
                char s[64];
                uint32_t n = 0;
                while (n < sizeof s - 1 && guest_span_ok(name + n, 1)) {
                    char ch = (char)ld8(name + n);
                    if (!ch) break;
                    s[n++] = ch;
                }
                s[n] = 0;
                rule = name_rule(s);
            }
            g_name_cache[i].name = name;
            g_name_cache[i].rule = (int8_t)rule;
            return rule;
        }
    }
    uint32_t i = h & (NAME_CACHE - 1);  /* full around here: replace */
    g_name_cache[i].name = 0;
    return rule_of(name);
}

typedef struct { uint8_t place, layer; uint32_t name; } WideCtx;
static WideCtx g_cur = {WIDE_OUTSIDE, WIDE_NO_LAYER, 0};  /* the object drawing now (or outside the tree) */
static WideCtx g_sent = {0xFF, 0xFF, 0};                    /* the last context the GP thread was told */
static int g_depth;                                          /* nested walks */
static uint32_t g_layer[WIDE_LAYERS];                        /* the layer objects (re-read at each outer walk) */

static int layer_index(uint32_t obj) {
    for (int i = 0; i < WIDE_LAYERS; i++)
        if (g_layer[i] == obj) return i;
    return -1;
}

static void send_ctx(const WideCtx* x) {
    /* every follow-unit object starts a unit of its own; otherwise only changes are sent */
    if (x->place != WIDE_FOLLOW_UNIT && x->place == g_sent.place && x->layer == g_sent.layer && x->name == g_sent.name)
        return;
    g_sent = *x;
    mark(WIDE_MARK_CTX, x->place, x->layer, x->name);
}

static WideCtx classify(uint32_t obj, const WideCtx* parent) {
    WideCtx me = *parent;
    int li = layer_index(obj);
    if (li >= 0) {  /* a layer object (render_geometry walks the root task, whose children are the layers) */
        me.layer = (uint8_t)li;
        me.place = wide_layer_is_hud(li) ? WIDE_HUD : WIDE_WORLD;
        me.name = 0;
        return me;
    }
    uint32_t name = ld32(obj);
    me.name = name;
    if (parent->place == WIDE_FOLLOW_UNIT || parent->place == WIDE_FOLLOW_DRAW) return me;  /* parts of it */
    int rule = rule_of(name);
    if (rule >= 0) me.place = (uint8_t)rule;
    else if (me.place == WIDE_OUTSIDE) me.place = WIDE_WORLD;
    return me;
}

/* the game's loop (0x8022A87C), with the same stack frame: stwu r1,-0x10; lr at +0x14, r31 at +0x0C */
static void walk(Cpu* c, uint32_t parent, const WideCtx* base, int wide) {
    uint32_t sp = c->r[1], lr = c->lr, r31 = c->r[31];
    uint32_t fr = sp - 0x10u;
    st32r(fr, sp);
    st32r(fr + 0x14u, lr);
    st32r(fr + 0x0Cu, r31);
    c->r[1] = fr;
    for (uint32_t o = ld32(parent + 0x14u); o; o = ld32(o + 0x0Cu)) {
        PPC_LOOP();
        c->r[31] = o;
        if (ld16(o + 0x04u) & 0x10u) continue;
        WideCtx me;
        if (wide) {
            me = classify(o, base);
            g_cur = me;
            send_ctx(&me);
            g_objects++;
        }
        uint32_t fn = ld32(ld32(o + 0x18u) + 0x10u);
        c->r[3] = o;
        c->r[12] = fn;
        c->ctr = fn;
        c->lr = 0x8022A8B4u;
        c->pc = fn;
        ppc_dispatch(c);
        c->r[3] = o;
        c->lr = 0x8022A8BCu;
        walk(c, o, wide ? &me : NULL, wide);
    }
    c->r[0] = lr;
    c->r[31] = r31;
    c->lr = lr;
    c->r[1] = sp;
}

void hle_render_nodes(Cpu* c) {
    uint32_t parent = c->r[3];
    int wide = wide_on();
    if (!wide) {
        g_depth++;
        walk(c, parent, NULL, 0);
        g_depth--;
        return;
    }
    if (g_depth == 0)
        for (int i = 0; i < WIDE_LAYERS; i++) g_layer[i] = ld32r(WIDE_LAYER_BASE + 4u * (uint32_t)i);
    WideCtx saved = g_cur, base;
    int li = layer_index(parent);
    if (li >= 0) {  /* called with a layer: render_ui (tl_window, tl_radermap), render_clipout_and_fade, ... */
        base.layer = (uint8_t)li;
        base.place = wide_layer_is_hud(li) ? WIDE_HUD : WIDE_WORLD;
        base.name = 0;
    } else if (g_depth > 0) {
        base = g_cur;  /* a walk inside an object's render: its children are its parts */
    } else {
        base.layer = WIDE_NO_LAYER;  /* the root task: its children are the layers */
        base.place = WIDE_WORLD;
        base.name = ld32(parent);
    }
    g_depth++;
    walk(c, parent, &base, 1);
    g_depth--;
    /* back in the caller: the enclosing object's context, or outside the tree */
    if (g_depth == 0) g_cur = (WideCtx){WIDE_OUTSIDE, WIDE_NO_LAYER, 0};
    else g_cur = saved;
    send_ctx(&g_cur);
}

/* every 10 s with the [pace] line (frame_pacing.c) */
void pso_wide_report(void) {
    if (!wide_on() && !g_proj_widened) return;
    rt_log("[wide] game side: projections %llu (%llu widened), screen space matrix loads %llu, objects %llu, marks %llu",
           g_proj_calls, g_proj_widened, g_ss_loads, g_objects, g_marks);
    g_proj_calls = g_proj_widened = g_ss_loads = g_objects = g_marks = 0;
}

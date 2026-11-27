/* Host test for the 16:9 hooks (runtime/src/pso_wide.c, docs/widescreen-research.md build A).
 *
 * 1. The projection maths (pso_wide.h wide_fov_for): for every field of view the game could pass, the
 *    widened call keeps the vertical angle the game's function works out (to a small fraction of a degree)
 *    and widens the horizontal one by k.
 * 2. The class rules: names from the decomp's UI, world and follow classes give the placement the plan says.
 * 3. The native task tree walk against the game's own (f_8022A87C_orig, extracted from build/gen by
 *    run_pso_wide_test.sh; nothing from the game is in this file): on random trees in the guest window, the
 *    same render calls in the same order with the same r1 / lr, the same stack words, and the same registers
 *    after. Also, with 16:9 on, the marks the walk sends for a tree with HUD layers, UI, follow and world
 *    objects.
 * 4. The backend arithmetic (render_deko.c / render_gl.c apply_state with gx.vert's anchor): a HUD draw lands
 *    at x + 320 (k - 1) in the wide EFB, the scissor offset (the radar) included.
 * Build B (pso_hud.h, HUD windows at the screen edges):
 * 5. hud_anchor.txt: the shipped file (embedded by run_pso_wide_test.sh as the program does) reads without
 *    errors and gives the placements planned; the parser's rules (comments, prefixes, precedence, bad lines).
 * 6. One placement per window from the x range of all its draws: windows whose draws alone would split
 *    (the status window's icons sit in the middle third) stay in one piece; the radar's modes; split screen.
 * 7. The edge shift: a pinned window keeps its distance from the 4:3 edge, measured from the 16:9 edge, in
 *    gx.vert's clip arithmetic and the backends' scissor, for full and offset viewports.
 * 8. The walk's windows: with containers (TWindowCtrlScreen walking a window root), each window and its parts
 *    share one window id, the class's own hud_anchor.txt line and the split screen bit are in the marks.
 *
 * Usage: pso_wide_test [trees] [seed] */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc.h"
#include "runtime.h"

/* ---- stand-ins for the runtime pieces pso_wide.c touches ---- */
volatile int g_core_preempt[3];
volatile uint32_t g_ppc_cur_fn, g_ppc_cur_lr;
int g_ppc_trace;
uint8_t* g_mod_hook_flags;
void ppc_trace_enter(uint32_t a) { (void)a; }
void ppc_preempt(Cpu* c) { (void)c; }
void gs_store(void) {}
void rt_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
_Noreturn void rt_fatal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(2); }
uint32_t mmio_read(uint32_t ea, int size) { (void)ea; (void)size; return 0; }
void mmio_write(uint32_t ea, uint32_t v, int size) { (void)ea; (void)v; (void)size; }
void mmio_write64(uint32_t ea, uint64_t v) { (void)ea; (void)v; }
void gx_pipe_write(uint32_t v, int size) { (void)v; (void)size; }
uint8_t g_wg[GX_WG_BATCH];
int g_wg_n, g_prev_bp_prefix, g_gxstat;
unsigned long long g_pipe_stores, g_pipe_bytes;

static float g_k = 1.0f;
float settings_wide_k(void) { return g_k; }
void f_803D676C_orig(Cpu* c) { (void)c; }
const char* platform_settings_path(void) { return "build/tests/no-such-folder/settings.ini"; }  /* built-in table */
void f_803E20B0_orig(Cpu* c) { (void)c; }

static uint8_t g_tmarks[1 << 16];
static int g_ntmarks;
void gx_host_mark(const uint8_t* b, int n) { memcpy(g_tmarks + g_ntmarks, b, (size_t)n); g_ntmarks += n; }

/* the render methods: every one records the object, r1 and lr; some nest a walk of their own */
#define RENDER_FN 0x80001000u
#define NEST_FN   0x80001100u
#define VT_RENDER 0x80002100u  /* vtables: render method at +0x10 */
#define VT_NEST   0x80002200u
static struct { uint32_t obj, r1, lr; } g_calls[4096];
static int g_ncalls;
static int g_use_hle;
void hle_render_nodes(Cpu* c);
void f_8022A87C_orig(Cpu* c);
void f_8022A87C(Cpu* c) { if (g_use_hle) hle_render_nodes(c); else f_8022A87C_orig(c); }
void ppc_dispatch(Cpu* c) {
    if (g_ncalls < 4096) { g_calls[g_ncalls].obj = c->r[3]; g_calls[g_ncalls].r1 = c->r[1]; g_calls[g_ncalls].lr = c->lr; }
    g_ncalls++;
    c->r[0] = 0xDEAD0000u; c->r[3] = 0x12345678u; c->r[4] = 0x9ABCDEF0u; c->r[12] = 0x55555555u;  /* clobbered, as callees may */
    if (c->pc == NEST_FN) {  /* a render that walks another list itself (e.g. 0x8036A230) */
        uint32_t lr = c->lr, sp = c->r[1], r31 = c->r[31];
        c->r[1] = sp - 0x20u;
        c->r[3] = ld32(r31 + 0x20u);  /* the extra list's owner */
        c->lr = NEST_FN + 4u;
        if (c->r[3]) f_8022A87C(c);
        c->r[1] = sp; c->lr = lr; c->r[31] = r31;
    }
}

#include "../../runtime/src/pso_wide.c"

static uint64_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }

/* ---- 1. projection maths ---- */
static int test_projection(void) {
    const double unit = 6.283185307179586 / 65536.0;
    const float s = 0.85f, ks[] = {854.0f / 640.0f, 1.25f, 1.5f};
    double worst_v = 0, worst_h = 0;
    int fails = 0;
    for (int ki = 0; ki < 3; ki++)
        for (int32_t fov = 0x800; fov <= 0x7000; fov += 7) {
            float k = ks[ki];
            int32_t f2 = wide_fov_for(fov, k, s);
            int32_t h1 = (int32_t)((float)fov * s) / 2, h2 = (int32_t)((float)f2 * s) / 2;
            double a = 1.1333334;
            double v1 = 2 * atan(tan(h1 * unit) / a), v2 = 2 * atan(tan(h2 * unit) / (a * (float)k));
            double dv = fabs(v1 - v2) * 180 / 3.141592653589793;
            double hr = tan(h2 * unit) / tan(h1 * unit) / k;  /* horizontal: tan ratio should be k */
            if (dv > worst_v) worst_v = dv;
            if (fabs(hr - 1) > worst_h) worst_h = fabs(hr - 1);
            if (dv > 0.02 || fabs(hr - 1) > 0.003) {
                if (fails++ < 10) printf("  fov %d k %.4f -> %d: vertical off by %.4f deg, horizontal ratio %.5f  FAIL\n", fov, k, f2, dv, hr);
            }
        }
    /* the game's own camera: 0x2AAA-ish and the movie / lobby ones (0x4B is not an angle the function can widen) */
    int32_t f = wide_fov_for(0x3555, 854.0f / 640.0f, s);
    printf("  fov 0x3555 (lobby) -> 0x%X; worst vertical change %.4f deg, worst horizontal error %.4f%%\n", f, worst_v, 100 * worst_h);
    if (wide_fov_for(0x4B, 1.334375f, s) <= 0) { printf("  tiny fov FAIL\n"); fails++; }
    return fails;
}

/* ---- 2. class rules ---- */
static int test_names(void) {
    struct { const char* n; int want; } t[] = {
        {"TWindowPartyStatus", WIDE_HUD}, {"TWindowActionPalette", WIDE_HUD}, {"TWindowFukidasi", WIDE_FOLLOW_UNIT},
        {"TEF_LensFlare", WIDE_FOLLOW_DRAW}, {"TEffStarLight2D", -1}, {"TEffStarLight2D_Base", -1},
        {"TAdTitleGC", WIDE_HUD}, {"TAdSinglePlyChrSelectGC", WIDE_HUD}, {"TObjResultWin", WIDE_HUD},
        {"TObjNewFloorWin", WIDE_HUD}, {"TObjItemOKWindow", WIDE_HUD}, {"TChCrTagName", WIDE_HUD},
        {"TObjCMMainMenu", WIDE_HUD}, {"TKeyBoardControl", WIDE_HUD}, {"TShopItemListWindow", WIDE_HUD},
        {"TRaderMapCore", WIDE_HUD}, {"TNowLoading", WIDE_HUD}, {"TLogo", WIDE_HUD}, {"TEndingPic", WIDE_HUD},
        {"TLockOnCursor", -1}, {"TParticle", -1}, {"TQuadSpriteNormal", -1}, {"TObjWarpString", -1},
        {"TObjEnePanarms", -1}, {"TObjContainerItem", -1}, {"TEnding", -1}, {"TScreenFadeIn", -1},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        int got = name_rule(t[i].n);
        if (got != t[i].want) { printf("  %s: %d (want %d)  FAIL\n", t[i].n, got, t[i].want); fails++; }
    }
    printf("  %zu class names checked\n", sizeof t / sizeof *t);
    return fails;
}

/* ---- 3. the walk ---- */
#define POOL 0x80100000u
#define OBJ_SIZE 0x40u
#define STACK_TOP 0x80400000u
static uint32_t g_nobj;
static uint32_t new_obj(uint32_t name, int nest) {
    uint32_t o = POOL + OBJ_SIZE * g_nobj++;
    for (uint32_t i = 0; i < OBJ_SIZE; i += 4) st32(o + i, 0);
    st32(o + 0x00u, name);
    st16(o + 0x04u, (rnd() % 6 == 0) ? 0x10 : (uint16_t)(rnd() & 0x2F));  /* sometimes hidden (0x10) */
    st32(o + 0x18u, nest ? VT_NEST : VT_RENDER);
    return o;
}
static void add_child(uint32_t parent, uint32_t child) {  /* appended: children at +0x14, next at +0x0C */
    uint32_t c = ld32(parent + 0x14u);
    if (!c) { st32(parent + 0x14u, child); return; }
    while (ld32(c + 0x0Cu)) c = ld32(c + 0x0Cu);
    st32(c + 0x0Cu, child);
}
static void build_random(uint32_t parent, int depth) {
    int n = (int)(rnd() % (depth ? 4 : 6));
    for (int i = 0; i < n; i++) {
        uint32_t o = new_obj(0x80003000u + 0x20u * (rnd() % 8), depth < 3 && rnd() % 9 == 0);
        add_child(parent, o);
        if (depth < 4) build_random(o, depth + 1);
        if (ld32(o + 0x18u) == VT_NEST) {  /* a nesting render: a side list */
            uint32_t side = new_obj(0x80003000u, 0);
            st16(side + 4u, 0);
            st32(o + 0x20u, side);
            build_random(side, 4);
        }
    }
}
typedef struct { int ncalls; uint32_t calls[4096][3]; uint32_t r[32], lr, ctr; uint8_t stack[0x400]; } Run;
static void run_walk(uint32_t root, int hle, Run* out) {
    Cpu c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 32; i++) c.r[i] = 0x1000u * (uint32_t)i + 7u;
    c.r[1] = STACK_TOP;
    c.r[3] = root;
    c.lr = 0x80229AC0u;
    for (uint32_t a = STACK_TOP - 0x400u; a < STACK_TOP + 0x20u; a += 4) st32(a, 0xCCCCCCCCu);
    g_use_hle = hle;
    g_ncalls = 0;
    f_8022A87C(&c);
    out->ncalls = g_ncalls;
    for (int i = 0; i < g_ncalls && i < 4096; i++) {
        out->calls[i][0] = g_calls[i].obj; out->calls[i][1] = g_calls[i].r1; out->calls[i][2] = g_calls[i].lr;
    }
    memcpy(out->r, c.r, sizeof out->r);
    out->lr = c.lr;
    out->ctr = c.ctr;
    for (uint32_t i = 0; i < 0x400u; i++) out->stack[i] = ld8(STACK_TOP - 0x400u + i);
}
#define NAME_AT(i) (0x80003000u + 0x20u * (uint32_t)(i))
enum { N_CTRLSCREEN = 8, N_TOBJECT, N_SIMPLESTATUS, N_PALETTE, N_MAINMENU, N_RADAR, N_NAMES };
static void set_names(void) {
    static const char* const names[N_NAMES] = {"TWindowPartyStatus", "TObjEnePanarms", "TWindowFukidasi", "TEF_LensFlare",
                                               "TParticle", "TAdTitleGC", "TObjResultWin", "TQuadSpriteNormal",
                                               "TWindowCtrlScreen", "TObject", "TWindowSimpleStatus",
                                               "TWindowActionPalette", "TWindowMainMenu1P", "TRaderMapCore"};
    for (int i = 0; i < N_NAMES; i++) {
        uint32_t a = 0x80003000u + 0x20u * (uint32_t)i;
        for (size_t j = 0; j <= strlen(names[i]); j++) st8(a + (uint32_t)j, (uint8_t)names[i][j]);
    }
    st32(VT_RENDER + 0x10u, RENDER_FN);
    st32(VT_NEST + 0x10u, NEST_FN);
}
static int test_walk(long trees) {
    static Run a, b;
    int fails = 0;
    for (long t = 0; t < trees; t++) {
        g_nobj = 0;
        uint32_t root = new_obj(0x80003000u, 0);
        st16(root + 4u, 0);
        build_random(root, 0);
        for (int wide = 0; wide < 2; wide++) {
            g_k = wide ? 854.0f / 640.0f : 1.0f;
            run_walk(root, 0, &a);
            g_ntmarks = 0;
            run_walk(root, 1, &b);
            int bad = a.ncalls != b.ncalls || memcmp(a.calls, b.calls, sizeof a.calls[0] * (size_t)(a.ncalls < 4096 ? a.ncalls : 4096)) ||
                      a.r[1] != b.r[1] || a.r[31] != b.r[31] || a.lr != b.lr || memcmp(a.stack, b.stack, sizeof a.stack);
            for (int i = 13; i < 31; i++) bad |= a.r[i] != b.r[i];  /* callee-saved (r0 and r3-r12 are volatile: the generated code leaves the last callee's) */
            if (bad) {
                if (fails++ < 5)
                    printf("  tree %ld (16:9 %d): calls %d / %d, r1 %08X / %08X, r31 %08X / %08X, lr %08X / %08X, r0 %08X / %08X  FAIL\n",
                           t, wide, a.ncalls, b.ncalls, a.r[1], b.r[1], a.r[31], b.r[31], a.lr, b.lr, a.r[0], b.r[0]);
            }
            if (!wide && g_ntmarks) { printf("  4:3 sent marks  FAIL\n"); fails++; }
        }
    }
    printf("  %ld random trees, walk native vs game's own, 4:3 and 16:9: %d failures\n", trees, fails);
    return fails;
}

/* a fixed tree in the game's shape: root task -> layers -> objects; checks the marks */
static int test_marks(void) {
    g_nobj = 0;
    g_k = 854.0f / 640.0f;
    uint32_t root = new_obj(0x80003000u + 0x20u * 1, 0);
    st16(root + 4u, 0);
    uint32_t layers[WIDE_LAYERS];
    for (int i = 0; i < WIDE_LAYERS; i++) {
        layers[i] = new_obj(0, 0);
        st16(layers[i] + 4u, 0);
        st32(WIDE_LAYER_BASE + 4u * (uint32_t)i, layers[i]);
        add_child(root, layers[i]);
    }
#define OBJ(parent, ni) ({ uint32_t o_ = new_obj(0x80003000u + 0x20u * (ni), 0); st16(o_ + 4u, 0); add_child(parent, o_); o_; })
    uint32_t enemy = OBJ(layers[5], 1);        /* tl_03: TObjEnePanarms -> world */
    uint32_t part = OBJ(enemy, 4);             /* its TParticle child -> world */
    uint32_t party = OBJ(layers[13], 0);       /* tl_window: TWindowPartyStatus -> HUD */
    uint32_t bubble = OBJ(party, 2);           /* a TWindowFukidasi under it -> follow, one piece */
    uint32_t bubble_text = OBJ(bubble, 4);     /* its parts -> follow too */
    uint32_t flare = OBJ(layers[9], 3);        /* tl_05: TEF_LensFlare -> follow, per sprite */
    uint32_t title = OBJ(layers[3], 5);        /* tl_01: TAdTitleGC -> HUD by its class */
    uint32_t radar_part = OBJ(layers[17], 7);  /* tl_clipout: any class -> HUD by its layer */
    uint32_t fade = OBJ(layers[18], 7);        /* tl_fade: -> world (stretched) */
    (void)part; (void)bubble_text;
    g_ntmarks = 0;
    run_walk(root, 1, &(Run){0});
    struct { uint32_t obj; int place; } want[] = {
        {enemy, WIDE_WORLD}, {part, WIDE_WORLD}, {party, WIDE_HUD}, {bubble, WIDE_FOLLOW_UNIT}, {bubble_text, WIDE_FOLLOW_UNIT},
        {flare, WIDE_FOLLOW_DRAW}, {title, WIDE_HUD}, {radar_part, WIDE_HUD}, {fade, WIDE_WORLD},
    };
    int fails = 0, ctx = 0, last_place = -1;
    for (int i = 0; i < g_ntmarks; i += WIDE_MARK_BYTES) {
        if (g_tmarks[i] != GX_HOST_MARK || g_tmarks[i + 1] != WIDE_MARK_CTX) { printf("  bad mark  FAIL\n"); fails++; continue; }
        ctx++;
        last_place = g_tmarks[i + 2] & 7;
    }
    /* replay: the walk's per-object context, as classify gives it */
    WideCtx base = {WIDE_WORLD, WIDE_NO_LAYER, HUD_NONE, 1, 0, 0};
    for (int i = 0; i < WIDE_LAYERS; i++) g_layer[i] = layers[i];
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        uint32_t o = want[i].obj;
        /* the parent chain: find it by search */
        uint32_t chain[8]; int n = 0;
        for (uint32_t x = o; x && n < 8;) {
            chain[n++] = x;
            uint32_t par = 0;
            for (uint32_t p = POOL; p < POOL + OBJ_SIZE * g_nobj; p += OBJ_SIZE)
                for (uint32_t ch = ld32(p + 0x14u); ch; ch = ld32(ch + 0x0Cu))
                    if (ch == x) par = p;
            x = par == root ? 0 : par;
        }
        WideCtx cur = base;
        for (int j = n - 1; j >= 0; j--) cur = classify(chain[j], &cur);
        if (cur.place != want[i].place) { printf("  object %zu: placement %d (want %d)  FAIL\n", i, cur.place, want[i].place); fails++; }
    }
    /* the marks sent: at least one per change, ending with "outside the tree" */
    if (ctx < 8 || last_place != WIDE_OUTSIDE) { printf("  %d context marks, last %d  FAIL\n", ctx, last_place); fails++; }
    printf("  game-shaped tree: %d context marks sent, %zu placements checked\n", ctx, sizeof want / sizeof *want);
    return fails;
}

/* ---- 4. where a HUD draw lands (backend viewport + gx.vert anchor) ---- */
static int test_mapping(void) {
    int fails = 0;
    float k = 854.0f / 640.0f;
    /* game viewport: centre xo (bias cancelled against the scissor offset sox), half width ax */
    struct { float xo, ax, sox; } vps[] = {{320, 320, 0}, {320, 320, -224}, {160, 160, 0}, {480, 160, 0}, {320, 320, 100}};
    for (size_t i = 0; i < sizeof vps / sizeof *vps; i++)
        for (float ndc = -1.0f; ndc <= 1.0f; ndc += 0.25f) {
            float p43 = vps[i].xo - vps[i].sox + ndc * vps[i].ax;                 /* the 4:3 EFB pixel */
            float want = p43 + 320.0f * (k - 1.0f);                               /* centred in the wide EFB */
            float cx = vps[i].xo - vps[i].sox + 320.0f * (k - 1.0f), half = vps[i].ax * k;  /* HUD viewport */
            float got = cx + (ndc / k) * half;                                     /* gx.vert: x / k (middle) */
            float full = cx + ndc * half;                                          /* stretch, full viewport */
            if (fabsf(got - want) > 1e-3f) { printf("  viewport %zu ndc %.2f: %.3f want %.3f  FAIL\n", i, ndc, got, want); fails++; }
            if (i == 0 && fabsf(full - (ndc + 1.0f) * 320.0f * k) > 1e-3f) { printf("  stretch %.2f  FAIL\n", ndc); fails++; }
        }
    /* follow: the middle c keeps its stretched place, the width is k times narrower */
    for (float c0 = -0.8f; c0 <= 0.8f; c0 += 0.4f) {
        float x = c0 + 0.1f;
        float out = x / k + c0 * (1.0f - 1.0f / k);
        if (fabsf(out - c0 - 0.1f / k) > 1e-5f) { printf("  follow %.2f  FAIL\n", c0); fails++; }
    }
    printf("  viewport, scissor offset and follow arithmetic checked\n");
    return fails;
}

/* ---- 5. hud_anchor.txt ---- */
static int test_anchor_file(void) {
    int fails = 0;
    const HudRules* r = pso_hud_rules();  /* the built-in copy of config/GPOE8P/hud_anchor.txt */
    if (r->errors) { printf("  shipped hud_anchor.txt: %d bad lines (first %d)  FAIL\n", r->errors, r->first_error_line); fails++; }
    if (r->dflt != HUD_CENTRE) { printf("  shipped default %d  FAIL\n", r->dflt); fails++; }
    struct { const char* n; int want; } t[] = {
        {"TWindowSimpleStatus", HUD_LEFT}, {"TWindowEffectParty", HUD_LEFT}, {"TWindowPartyStatus", HUD_LEFT},
        {"TWindowLockOn", HUD_LEFT}, {"TWindowActionPalette", HUD_RIGHT}, {"TWindowEffectLight", HUD_RIGHT},
        {"TWindowMessageInfomation", HUD_RIGHT}, {"TRaderMapCore", HUD_AUTO}, {"TWindowTradeRequest", HUD_AUTO},
        /* no line of their own: the default (centre) */
        {"TWindowMainMenu1P", HUD_NONE}, {"TShopItemListWindow", HUD_NONE}, {"TWindowDeposit", HUD_NONE},
        {"TWindowMessageYesNo", HUD_NONE}, {"TWindowDD_WS_Word", HUD_NONE}, {"TWindowSubQuestBoard", HUD_NONE},
        {"TAdTitleGC", HUD_NONE}, {"TWindowOptionCtrl", HUD_NONE}, {"TNowLoading", HUD_NONE}, {"TWindowFukidasi", HUD_NONE},
        {"TWindowSimpleStatusX", HUD_NONE},  /* exact names do not match longer ones */
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        int got = hud_rules_lookup(r, t[i].n);
        if (got != t[i].want) { printf("  shipped: %s -> %s (want %s)  FAIL\n", t[i].n, k_hud_place_names[got], k_hud_place_names[t[i].want]); fails++; }
    }
    /* a class named in the file is HUD wherever it sits; follow lines make follow classes */
    if (name_rule("TWindowLockOn") != WIDE_HUD) { printf("  TWindowLockOn not HUD  FAIL\n"); fails++; }
    /* the parser */
    static HudRules p;
    const char* text =
        "# comment line\r\n"
        "   \r\n"
        "*            auto      # default\r\n"
        "TWin*        right\n"
        "TWindow*     left\n"
        "TWindowSub*  center\n"
        "TWindowSubItem stretch\n"
        "TWindowSubItem middle\n"     /* the later line wins */
        "TBubble      follow\n"
        "TBad         sideways\n"     /* bad word */
        "TBad2\n"                    /* no placement */
        "TBad3 left extra\n"          /* trailing word */
        "* follow\n"                  /* not a default */
        "TLast        right";         /* no newline at the end */
    hud_rules_parse(&p, text);
    struct { const char* n; int want; } q[] = {
        {"TWindowSubItem", HUD_CENTRE}, {"TWindowSubItemList", HUD_CENTRE}, {"TWindowLockOn", HUD_LEFT},
        {"TWinX", HUD_RIGHT}, {"TBubble", HUD_FOLLOW}, {"TBad", HUD_NONE}, {"TLast", HUD_RIGHT}, {"TOther", HUD_NONE},
    };
    for (size_t i = 0; i < sizeof q / sizeof *q; i++) {
        int got = hud_rules_lookup(&p, q[i].n);
        if (got != q[i].want) { printf("  parser: %s -> %s (want %s)  FAIL\n", q[i].n, k_hud_place_names[got], k_hud_place_names[q[i].want]); fails++; }
    }
    if (p.dflt != HUD_AUTO || p.errors != 4 || p.first_error_line != 10 || p.n != 6) {
        printf("  parser: default %d, %d errors (first line %d), %d rules  FAIL\n", p.dflt, p.errors, p.first_error_line, p.n);
        fails++;
    }
    printf("  shipped file: %d classes, default %s; %zu lookups and the parser's rules checked\n", r->n,
           k_hud_place_names[r->dflt], sizeof t / sizeof *t + sizeof q / sizeof *q);
    return fails;
}

/* ---- 6. one placement per window ---- */
typedef struct { float lo, hi; } Rng;
static int window_place(const Rng* d, int n, int force, int split, int* why) {  /* gx_render.c: the union of its draws */
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < n; i++) { if (d[i].lo < lo) lo = d[i].lo; if (d[i].hi > hi) hi = d[i].hi; }
    return hud_decide(force, HUD_CENTRE, lo, hi, split, why);
}
static int test_windows(void) {
    int fails = 0, why;
    /* the player status window measured: frame and bars 28..181, status icons from 226 (the middle third) */
    Rng status[] = {{28, 181}, {67, 152}, {47, 60}, {226, 242}, {244, 260}};
    for (size_t i = 0; i < sizeof status / sizeof *status; i++)  /* drawn alone, the icons would stay centred */
        if (i >= 3 && hud_measured(status[i].lo, status[i].hi) != HUD_CENTRE) { printf("  icon alone  FAIL\n"); fails++; }
    if (window_place(status, 5, HUD_AUTO, 0, &why) != HUD_LEFT || why != HUD_WHY_MEASURED) { printf("  status window (auto)  FAIL\n"); fails++; }
    Rng palette[] = {{471, 560}, {500, 620}, {600, 621}};
    if (window_place(palette, 3, HUD_AUTO, 0, &why) != HUD_RIGHT) { printf("  action palette  FAIL\n"); fails++; }
    struct { Rng r; int want; const char* what; } m[] = {
        {{480, 608}, HUD_RIGHT, "radar, corner"}, {{32, 480}, HUD_CENTRE, "radar, wide map"},
        {{96, 544}, HUD_CENTRE, "radar, middle map"}, {{373, 628}, HUD_RIGHT, "information popup"},
        {{30, 180}, HUD_LEFT, "party status"}, {{254, 386}, HUD_CENTRE, "timer"}, {{0, 640}, HUD_CENTRE, "full width"},
        {{13, 400}, HUD_CENTRE, "a wide window from the left"}, {{214, 300}, HUD_CENTRE, "left half, middle third"},
        {{212, 300}, HUD_LEFT, "just into the left third"}, {{100, 324}, HUD_LEFT, "4 columns of slack"},
        {{100, 325}, HUD_CENTRE, "past the slack"}, {{316, 500}, HUD_RIGHT, "right, with slack"},
        {{315, 500}, HUD_CENTRE, "right, past the slack"},
    };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++)
        if (hud_measured(m[i].r.lo, m[i].r.hi) != m[i].want) { printf("  %s  FAIL\n", m[i].what); fails++; }
    /* own lines win over the measurement; the default; nothing measured */
    if (hud_decide(HUD_RIGHT, HUD_CENTRE, 28, 181, 0, &why) != HUD_RIGHT || why != HUD_WHY_FILE) { printf("  forced right  FAIL\n"); fails++; }
    if (hud_decide(HUD_NONE, HUD_CENTRE, 28, 181, 0, &why) != HUD_CENTRE || why != HUD_WHY_DEFAULT) { printf("  default centre  FAIL\n"); fails++; }
    if (hud_decide(HUD_NONE, HUD_AUTO, 28, 181, 0, &why) != HUD_LEFT || why != HUD_WHY_MEASURED) { printf("  default auto  FAIL\n"); fails++; }
    if (hud_decide(HUD_AUTO, HUD_CENTRE, 1, 0, 0, &why) != HUD_CENTRE || why != HUD_WHY_NO_DRAWS) { printf("  no draws  FAIL\n"); fails++; }
    if (hud_decide(HUD_STRETCH, HUD_CENTRE, 28, 181, 0, &why) != HUD_STRETCH) { printf("  stretch  FAIL\n"); fails++; }
    if (hud_decide(HUD_FOLLOW, HUD_CENTRE, 28, 181, 0, &why) != HUD_CENTRE) { printf("  follow is no window placement  FAIL\n"); fails++; }
    /* split screen: everything stays in the middle of its view */
    int forces[] = {HUD_NONE, HUD_AUTO, HUD_LEFT, HUD_RIGHT, HUD_STRETCH, HUD_CENTRE};
    for (size_t i = 0; i < sizeof forces / sizeof *forces; i++)
        if (hud_decide(forces[i], HUD_AUTO, 28, 181, 1, &why) != HUD_CENTRE || why != HUD_WHY_SPLIT) { printf("  split %d  FAIL\n", forces[i]); fails++; }
    printf("  per-window union, %zu measured ranges, own lines, default and split screen checked\n", sizeof m / sizeof *m);
    return fails;
}

/* ---- 7. the edge shift ---- */
static int test_shift(void) {
    int fails = 0;
    float k = 854.0f / 640.0f, W = 640.0f * k;
    if (fabsf(hud_shift_px(HUD_LEFT, k) + 320.0f * (k - 1)) > 1e-4f || fabsf(hud_shift_px(HUD_RIGHT, k) - 320.0f * (k - 1)) > 1e-4f ||
        hud_shift_px(HUD_CENTRE, k) != 0.0f || hud_shift_px(HUD_STRETCH, k) != 0.0f || hud_shift_px(HUD_LEFT, 1.0f) != 0.0f) {
        printf("  shift sizes  FAIL\n");
        fails++;
    }
    /* viewports: full, the radar's (centre moved by the scissor offset), and smaller ones */
    struct { float xo, ax, sox; } vps[] = {{320, 320, 0}, {320, 320, -224}, {320, 320, 64}, {160, 160, 0}, {480, 160, 0}};
    for (size_t i = 0; i < sizeof vps / sizeof *vps; i++)
        for (float ndc = -1.0f; ndc <= 1.0f; ndc += 0.125f) {
            float p43 = vps[i].xo - vps[i].sox + ndc * vps[i].ax;  /* the 4:3 frame's pixel */
            float cx = vps[i].xo - vps[i].sox + 320.0f * (k - 1.0f), half = vps[i].ax * k;  /* the backend's HUD viewport */
            for (int pl = HUD_LEFT; pl <= HUD_RIGHT; pl++) {
                /* gx.vert: clip / k + sh, sh = (320 / ax) (1 - 1 / k) */
                float sh = (1.0f - 1.0f / k) * 320.0f / vps[i].ax;
                float clip = ndc / k + (pl == HUD_RIGHT ? sh : -sh);
                float got = cx + clip * half;
                float want = pl == HUD_LEFT ? p43 : p43 + 640.0f * (k - 1.0f);  /* same distance from its own edge */
                float dist = pl == HUD_LEFT ? got : W - got, dist43 = pl == HUD_LEFT ? p43 : 640.0f - p43;
                if (fabsf(got - want) > 2e-3f || fabsf(dist - dist43) > 2e-3f ||
                    fabsf(clip - (ndc / k + hud_shift_clip(pl, k, vps[i].ax))) > 1e-5f) {
                    printf("  viewport %zu ndc %.3f %s: %.3f want %.3f  FAIL\n", i, ndc, k_hud_place_names[pl], got, want);
                    fails++;
                }
            }
        }
    /* the scissor (render_deko.c / render_gl.c): x + 320 (k - 1) + the shift, the radar's corner box 480..608 */
    float hx = 320.0f * (k - 1.0f);
    float r0 = 480.0f + hx + hud_shift_px(HUD_RIGHT, k), r1 = 608.0f + hx + hud_shift_px(HUD_RIGHT, k);
    if (fabsf((W - r1) - (640.0f - 608.0f)) > 1e-3f || fabsf(r1 - r0 - 128.0f) > 1e-3f || r1 > W + 1e-3f) {
        printf("  radar scissor %.2f..%.2f of %.2f  FAIL\n", r0, r1, W);
        fails++;
    }
    float l0 = 28.0f + hx + hud_shift_px(HUD_LEFT, k);
    if (fabsf(l0 - 28.0f) > 1e-3f) { printf("  left scissor %.2f  FAIL\n", l0); fails++; }
    printf("  edge shift for %zu viewports, both sides, and the scissor checked\n", sizeof vps / sizeof *vps);
    return fails;
}

/* ---- 8. windows in the walk ---- */
static int test_units(void) {
    int fails = 0;
    g_nobj = 0;
    g_k = 854.0f / 640.0f;
    uint32_t root = new_obj(NAME_AT(1), 0);
    st16(root + 4u, 0);
    uint32_t layers[WIDE_LAYERS];
    for (int i = 0; i < WIDE_LAYERS; i++) {
        layers[i] = new_obj(0, 0);
        st16(layers[i] + 4u, 0);
        st32(WIDE_LAYER_BASE + 4u * (uint32_t)i, layers[i]);
        add_child(root, layers[i]);
    }
    /* tl_window -> TWindowCtrlScreen, whose render walks a window root (a plain TObject) holding the windows */
    uint32_t screen = new_obj(NAME_AT(N_CTRLSCREEN), 1);
    st16(screen + 4u, 0);
    add_child(layers[13], screen);
    uint32_t wroot = new_obj(NAME_AT(N_TOBJECT), 0);
    st16(wroot + 4u, 0);
    st32(screen + 0x20u, wroot);
    uint32_t status = OBJ(wroot, N_SIMPLESTATUS);
    uint32_t status_part = OBJ(status, 4);    /* a part of it (any class) */
    uint32_t status_group = OBJ(status, N_TOBJECT);    /* a plain TObject of its own: not a window root */
    uint32_t status_sub = OBJ(status_group, N_RADAR);  /* below it: still the status window's, no line */
    uint32_t palette = OBJ(wroot, N_PALETTE);
    uint32_t menu = OBJ(wroot, N_MAINMENU);
    uint32_t menu_part = OBJ(menu, N_PALETTE);  /* a part named like a pinned class: still the menu's, no line */
    uint32_t radar = OBJ(layers[17], N_RADAR);  /* tl_clipout */
    uint32_t radar_part = OBJ(radar, 7);
    struct { uint32_t obj, unit; int force; } want[] = {
        {screen, screen, HUD_NONE}, {status, status, HUD_LEFT}, {status_part, status, HUD_NONE}, {status_group, status, HUD_NONE}, {status_sub, status, HUD_NONE},
        {palette, palette, HUD_RIGHT}, {menu, menu, HUD_NONE}, {menu_part, menu, HUD_NONE},
        {radar, radar, HUD_AUTO}, {radar_part, radar, HUD_NONE},
    };
    for (int split = 0; split < 2; split++) {
        st32(VIEW_COUNT, split ? 2u : 1u);
        g_ntmarks = 0;
        run_walk(root, 1, &(Run){0});
        /* the render calls in order give the object of each context mark (the walk marks before each render) */
        int seen[sizeof want / sizeof *want] = {0}, bad_split = 0;
        for (int i = 0; i < g_ntmarks; i += WIDE_MARK_BYTES) {
            const uint8_t* b = g_tmarks + i;
            if (b[1] != WIDE_MARK_CTX) continue;
            uint32_t name = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
            uint32_t unit = (uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
            if (((b[2] >> 6) & 1) != split) bad_split++;
            for (size_t j = 0; j < sizeof want / sizeof *want; j++)
                if (unit == want[j].unit && name == ld32(want[j].obj)) {
                    int force = (b[2] >> 3) & 7;
                    if (force == want[j].force && (b[2] & 7) == WIDE_HUD) seen[j] = 1;
                }
        }
        for (size_t j = 0; j < sizeof want / sizeof *want; j++)
            if (!seen[j]) { printf("  split %d: object %zu: no mark with window %08X and own line %s  FAIL\n", split, j, want[j].unit, k_hud_place_names[want[j].force]); fails++; }
        if (bad_split) { printf("  split %d: %d marks with the wrong split bit  FAIL\n", split, bad_split); fails++; }
    }
    st32(VIEW_COUNT, 0);
    g_split = 0;
    printf("  windows under a container, their parts, own lines and the split screen bit checked\n");
    return fails;
}

int main(int argc, char** argv) {
    long trees = argc > 1 ? atol(argv[1]) : 3000;
    g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ull;
    mem_init();
    set_names();
    int f1, f2, f3, f4, f5, f6, f7, f8, f9;
    printf("projection:\n"); f1 = test_projection();
    printf("class rules:\n"); f2 = test_names();
    printf("walk:\n"); f3 = test_walk(trees);
    printf("marks:\n"); f4 = test_marks();
    printf("mapping:\n"); f5 = test_mapping();
    printf("hud_anchor.txt:\n"); f6 = test_anchor_file();
    printf("windows:\n"); f7 = test_windows();
    printf("edge shift:\n"); f8 = test_shift();
    printf("walk windows:\n"); f9 = test_units();
    int total = f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9;
    printf("RESULT: %s (%d failures)\n", total ? "FAIL" : "PASS", total);
    return total != 0;
}

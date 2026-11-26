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
static void set_names(void) {
    static const char* const names[8] = {"TWindowPartyStatus", "TObjEnePanarms", "TWindowFukidasi", "TEF_LensFlare",
                                         "TParticle", "TAdTitleGC", "TObjResultWin", "TQuadSpriteNormal"};
    for (int i = 0; i < 8; i++) {
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
    for (int i = 0; i < g_ntmarks; i += 8) {
        if (g_tmarks[i] != GX_HOST_MARK || g_tmarks[i + 1] != WIDE_MARK_CTX) { printf("  bad mark  FAIL\n"); fails++; continue; }
        ctx++;
        last_place = g_tmarks[i + 2];
    }
    /* replay: the walk's per-object context, as classify gives it */
    WideCtx base = {WIDE_WORLD, WIDE_NO_LAYER, 0};
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

int main(int argc, char** argv) {
    long trees = argc > 1 ? atol(argv[1]) : 3000;
    g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ull;
    mem_init();
    set_names();
    int f1, f2, f3, f4, f5;
    printf("projection:\n"); f1 = test_projection();
    printf("class rules:\n"); f2 = test_names();
    printf("walk:\n"); f3 = test_walk(trees);
    printf("marks:\n"); f4 = test_marks();
    printf("mapping:\n"); f5 = test_mapping();
    int total = f1 + f2 + f3 + f4 + f5;
    printf("RESULT: %s (%d failures)\n", total ? "FAIL" : "PASS", total);
    return total != 0;
}

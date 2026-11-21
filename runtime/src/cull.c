/* Skip models that are wholly outside the view (PSO test 7).
 *
 * The game draws whole groups of models around the player and leaves the clipping to the GameCube's
 * graphics chip: in the Forest's busy spots 50-64% of the 3D draws landed entirely outside the view
 * (test 6's SR_GXSTAT), each still costing the game thread its state setup and display list calls.
 * Nearly every scenery and object model goes through the game's model routine 0x803DB510 (r3 = the
 * model): 803DC76C loads the current matrix (pointer at r13-0x2E8C) into position matrix 0, 803DC600
 * sets state, 803DC460 sets each mesh's state and calls its display list (mesh tables at model+0x08
 * and +0x0C, 16-byte entries, display list address at +8, counts as halfwords at model+0x10/+0x12),
 * 803DC6F4 restores state.
 *
 * Bounds: the GP thread keeps, per display list address, the box around the object-space positions
 * it decoded the last time that list ran (cull_store, from gx_fifo.c's call_display_list), and the
 * last perspective projection. A list whose vertices use any position matrix but 0 is never culled
 * (skinned characters).
 * Test: hle_ModelDraw takes the union of the boxes of all the model's display lists, puts its corners
 * through the current matrix and the projection, and skips 803DC460 when every corner is outside the
 * same plane of a view volume widened by a margin (and by the wide screen factor sideways). The
 * matrix load and the state set and restore still run, so later draws see the same GX state.
 * A model with any list not seen yet is drawn. SR_CULL=0 (env.txt) turns this off. */
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ppc.h"
#include "runtime.h"
#include "settings.h"

void f_803DB510_orig(Cpu* c);
void f_803DC76C(Cpu* c);
void f_803DC600(Cpu* c);
void f_803DC6F4(Cpu* c);

#define CULL_SLOTS 16384  /* power of two */
typedef struct {
    _Atomic uint32_t seq;   /* odd while being written */
    _Atomic uint32_t addr;  /* display list address, 0 = empty */
    float bb[6];            /* min xyz, max xyz (object space) */
    uint32_t ok;            /* all vertices on position matrix 0 */
} CullEntry;
static CullEntry g_tab[CULL_SLOTS];
static _Atomic uint32_t g_proj_seq;
static float g_proj[4];  /* P0..P3 of the last perspective projection */
static _Atomic int g_proj_valid;

static int g_cull_on = -1;
unsigned long long g_cull_models, g_cull_skipped, g_cull_unknown;
static int g_sample_due = 1;

int cull_enabled(void) {
    if (g_cull_on < 0) { const char* e = getenv("SR_CULL"); g_cull_on = !(e && *e == '0'); }
    return g_cull_on;
}

static inline uint32_t slot_of(uint32_t a) { return (a * 2654435761u) >> 18; }  /* 14 bits */

/* GP thread */
void cull_store(uint32_t addr, const float bb[6], int ok) {
    uint32_t h = slot_of(addr) & (CULL_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (CULL_SLOTS - 1)) {
        CullEntry* e = &g_tab[h];
        uint32_t a = atomic_load_explicit(&e->addr, memory_order_relaxed);
        if (a != addr && a != 0) continue;
        uint32_t s = atomic_load_explicit(&e->seq, memory_order_relaxed);
        atomic_store_explicit(&e->seq, s + 1, memory_order_relaxed);
        atomic_thread_fence(memory_order_release);
        memcpy(e->bb, bb, sizeof e->bb);
        e->ok = (uint32_t)ok;
        if (!a) atomic_store_explicit(&e->addr, addr, memory_order_relaxed);
        atomic_store_explicit(&e->seq, s + 2, memory_order_release);
        return;
    }
}
void cull_set_proj(const float p[4]) {
    uint32_t s = atomic_load_explicit(&g_proj_seq, memory_order_relaxed);
    atomic_store_explicit(&g_proj_seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(g_proj, p, sizeof g_proj);
    atomic_store_explicit(&g_proj_seq, s + 2, memory_order_release);
    atomic_store_explicit(&g_proj_valid, 1, memory_order_release);
}

/* game thread */
static int lookup(uint32_t addr, float bb[6]) {
    uint32_t h = slot_of(addr) & (CULL_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (CULL_SLOTS - 1)) {
        CullEntry* e = &g_tab[h];
        uint32_t a = atomic_load_explicit(&e->addr, memory_order_acquire);
        if (!a) return 0;
        if (a != addr) continue;
        for (int tries = 0; tries < 4; tries++) {
            uint32_t s0 = atomic_load_explicit(&e->seq, memory_order_acquire);
            if (s0 & 1) continue;
            float t[6];
            memcpy(t, e->bb, sizeof t);
            uint32_t ok = e->ok;
            atomic_thread_fence(memory_order_acquire);
            if (atomic_load_explicit(&e->seq, memory_order_relaxed) != s0) continue;
            if (!ok) return 0;
            memcpy(bb, t, sizeof t);
            return 1;
        }
        return 0;
    }
    return 0;
}
static int get_proj(float p[4]) {
    if (!atomic_load_explicit(&g_proj_valid, memory_order_acquire)) return 0;
    for (int tries = 0; tries < 4; tries++) {
        uint32_t s0 = atomic_load_explicit(&g_proj_seq, memory_order_acquire);
        if (s0 & 1) continue;
        memcpy(p, g_proj, sizeof g_proj);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_proj_seq, memory_order_relaxed) == s0) return 1;
    }
    return 0;
}

static inline float gf(uint32_t ea) { return u32_as_f32(ld32r(ea)); }

/* the union of the boxes of the model's display lists; 0 when any list is unknown or skinned */
static int model_bounds(uint32_t model, float bb[6]) {
    int any = 0;
    for (int list = 0; list < 2; list++) {
        uint32_t tab = ld32r(model + 0x08u + 4u * (uint32_t)list);
        uint32_t n = ld16r(model + 0x10u + 2u * (uint32_t)list);
        if (!n) continue;
        if (n > 256 || PPC_IS_MMIO(tab) || (tab & 0x3FFFFFFFu) + 16u * n > GUEST_RAM_SIZE) return 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t dl = ld32r(tab + 16u * i + 8u);
            float t[6];
            if (!lookup(dl, t)) return 0;
            if (!any) { memcpy(bb, t, sizeof t); any = 1; continue; }
            for (int k = 0; k < 3; k++) {
                if (t[k] < bb[k]) bb[k] = t[k];
                if (t[k + 3] > bb[k + 3]) bb[k + 3] = t[k + 3];
            }
        }
    }
    return any;
}

static int model_hidden(Cpu* c, uint32_t model) {
    if (PPC_IS_MMIO(model) || (model & 0x3FFFFFFFu) + 0x14u > GUEST_RAM_SIZE) return 0;
    float bb[6], p[4];
    if (!model_bounds(model, bb)) { g_cull_unknown++; return 0; }
    /* the projection in force now: the GX library's own copy (GXSetProjection, the code before 0x8038E978, keeps it in
     * __GXData, pointer at r13-0x6078: +0x420 type, 0 = perspective, +0x424.. P0-P5). Test 8 used the
     * last perspective projection the GP thread saw, which is another pass's, and culled nothing. */
    uint32_t gx = ld32r(c->r[13] - 0x6078u);
    if (PPC_IS_MMIO(gx) || (gx & 0x3FFFFFFFu) + 0x43Cu > GUEST_RAM_SIZE) return 0;
    if (ld32r(gx + 0x420u) != 0) return 0;  /* orthographic: 2D, never culled */
    for (int k = 0; k < 4; k++) p[k] = gf(gx + 0x424u + 4u * (uint32_t)k);
    (void)get_proj;
    uint32_t mp = ld32r(c->r[13] - 0x2E8Cu);  /* the current matrix: 3x4, row-major floats */
    if (PPC_IS_MMIO(mp) || (mp & 0x3FFFFFFFu) + 48u > GUEST_RAM_SIZE) return 0;
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = gf(mp + 4u * (uint32_t)i);
    const float margin = 1.15f;
    float kx = margin * fmaxf(1.0f, settings_wide_k());  /* wide screen modes show more to the sides */
    if (g_sample_due) {
        g_sample_due = 0;
        rt_log("[cull] sample: model %08X box %.1f %.1f %.1f .. %.1f %.1f %.1f, matrix row z %.3f %.3f %.3f %.1f, "
               "projection %.3f %.3f %.3f %.3f", model, bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], m[8], m[9], m[10], m[11],
               p[0], p[1], p[2], p[3]);
    }
    unsigned out_l = 1, out_r = 1, out_b = 1, out_t = 1, behind = 1;
    for (int k = 0; k < 8; k++) {
        float x = (k & 1) ? bb[3] : bb[0], y = (k & 2) ? bb[4] : bb[1], z = (k & 4) ? bb[5] : bb[2];
        float vx = m[0] * x + m[1] * y + m[2] * z + m[3];
        float vy = m[4] * x + m[5] * y + m[6] * z + m[7];
        float vz = m[8] * x + m[9] * y + m[10] * z + m[11];
        float cx = p[0] * vx + p[1] * vz, cy = p[2] * vy + p[3] * vz, cw = -vz;  /* GX perspective, w = -z */
        if (cw > 0.0f) behind = 0;
        if (!(cx < -kx * cw)) out_l = 0;
        if (!(cx > kx * cw)) out_r = 0;
        if (!(cy < -margin * cw)) out_b = 0;
        if (!(cy > margin * cw)) out_t = 0;
        if (!(behind | out_l | out_r | out_b | out_t)) return 0;
        if (!isfinite(cx) || !isfinite(cy) || !isfinite(cw)) return 0;
    }
    return 1;
}

void hle_ModelDraw(Cpu* c) {
    if (!cull_enabled()) { f_803DB510_orig(c); return; }
    g_cull_models++;
    if (!model_hidden(c, c->r[3])) { f_803DB510_orig(c); return; }
    g_cull_skipped++;
    /* 803DB510 without 803DC460 (the meshes): the matrix load, state set and restore stay */
    c->r[3] = 0;
    c->lr = 0x803DB52Cu; f_803DC76C(c);
    c->lr = 0x803DB530u; f_803DC600(c);
    c->lr = 0x803DB53Cu; f_803DC6F4(c);
}

void cull_report(void) {
    if (!cull_enabled()) { rt_log("[cull] off (SR_CULL=0)"); return; }
    if (g_cull_models)
        rt_log("[cull] models %llu, skipped as wholly outside the view %llu (%.1f%%), drawn because a display list was not "
               "seen yet or is skinned %llu", g_cull_models, g_cull_skipped, 100.0 * g_cull_skipped / g_cull_models, g_cull_unknown);
    g_cull_models = g_cull_skipped = g_cull_unknown = 0;
    g_sample_due = 1;
}

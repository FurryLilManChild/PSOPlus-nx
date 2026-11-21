/* The GameCube graphics pipe, from the CPU side:
 *
 *  - PI FIFO (0xCC00300C base, 0x3010 end, 0x3014 write pointer): every byte the game writes to the
 *    write-gather pipe (0xCC008000) is stored in RAM at the write pointer, which advances and wraps.
 *    That is how GXBeginDisplayList/GXEndDisplayList record display lists.
 *  - When the GP is linked to the CPU FIFO (CP control: read enable + GP link, immediate mode), the
 *    same bytes go to the command parser below, which decodes CP/XF/BP register loads, indexed
 *    loads, display-list calls and draws. Draws go to the renderer (gx_draw hook).
 *  - PE: the "draw done" (BP 0x45) and token (BP 0x47/0x48) commands raise the PE finish/token
 *    interrupts, so GXDrawDone, GXSetDrawSync and draw-sync callbacks run through the game's own
 *    handlers, when the parser reaches them.
 *  - The parser runs on its own thread (the "GP thread", core 2 on the Switch), as the real GP works
 *    in parallel with the CPU: the game thread only stores the pipe's bytes and appends them to a
 *    ring. Everything after (decoding, draw processing, frame building) happens on the GP thread.
 *    SR_GX_SYNC=1 parses on the game thread instead, as before (comparisons, debugging).
 *
 * Command formats follow YAGCD / Dolphin's OpcodeDecoding and VertexLoader (vertex sizes). */
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "gx.h"
#include "gx_render.h"
#include "runtime.h"

#define OS_INTERRUPT_PI_PE_TOKEN  18
#define OS_INTERRUPT_PI_PE_FINISH 19

GxState g_gx;

/* ---- PI FIFO ---- */
static uint32_t g_pi_base, g_pi_end, g_pi_wptr;  /* physical; wptr bit 26 = wrapped */
static uint32_t g_pi_regs[0x40];

/* ---- CP / PE registers (16-bit) ---- */
static uint16_t g_cp_mmio[0x40];
static uint16_t g_pe_mmio[0x80];
#define CP_CR      (g_cp_mmio[0x02 >> 1])
#define PE_INTRSR  (g_pe_mmio[0x0A >> 1])
#define PE_TOKEN   (g_pe_mmio[0x0E >> 1])
static mtx_t g_pe_lock;  /* PE status and token: the GP thread sets them, the CPU reads and clears */

/* SR_GPTL=<VI field>: a timeline of game/GP synchronisation for a few frames from that field (debugging
 * the multiplayer frame rate): game thread idle begin/end, PE finish/token raised, queued bytes */
size_t gx_gp_backlog(void);
static struct { uint64_t t; uint32_t field; char type; uint32_t a, b; } g_tl[4096];
static atomic_int g_tl_n;
static uint32_t g_tl_from;
static int g_tl_on = -1;
void gx_tl(char type, uint32_t a) {
    if (g_tl_on < 0) {
        const char* e = getenv("SR_GPTL");
        g_tl_from = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        g_tl_on = e != NULL;
    }
    if (!g_tl_on) return;
    uint32_t f = vi_field_count();
    if (f < g_tl_from) return;
    int i = atomic_fetch_add(&g_tl_n, 1);
    if (i < 4096) {
        g_tl[i].t = rt_ticks(); g_tl[i].field = f; g_tl[i].type = type; g_tl[i].a = a; g_tl[i].b = (uint32_t)gx_gp_backlog();
    } else if (i == 4096) {
        uint64_t t0 = g_tl[0].t;
        rt_log("[gptl] timeline from field %u (us since first event; type: I idle begin, i idle end, F PE finish, T token, V retrace; a; queued bytes)", g_tl_from);
        for (int k = 0; k < 4096; k++)
            rt_log("[gptl] %8.1f f%u %c %u q%u", (g_tl[k].t - t0) / 40.5, g_tl[k].field, g_tl[k].type, g_tl[k].a, g_tl[k].b);
        g_tl_on = 0;
    }
}

/* ---- the GP thread's own variables, written for every batch it parses, kept on cache lines of their
 * own: next to the game thread's write-gather state (written on every pipe store), they moved a
 * cache line between the two cores on every 32-byte burst. ---- */
static struct {
    _Alignas(64) _Atomic size_t ring_tail;  /* ring bytes consumed, ever */
    _Atomic uint64_t gp_busy;               /* ticks spent parsing (draw processing included) */
    atomic_int gp_sleeping;
    uint8_t* pend;                          /* bytes of the command being parsed */
    size_t pend_len, pend_cap;
    size_t need, parse_need;                /* see gp_feed / parse */
    int parse_depth;
} g_gpside;
#define g_ring_tail g_gpside.ring_tail
#define g_gp_busy g_gpside.gp_busy
#define g_gp_sleeping g_gpside.gp_sleeping
#define g_pend g_gpside.pend
#define g_pend_len g_gpside.pend_len
#define g_pend_cap g_gpside.pend_cap
#define g_need g_gpside.need
#define g_parse_need g_gpside.parse_need
#define g_parse_depth g_gpside.parse_depth

/* ---- command parser ---- */

static void pend_push(const uint8_t* p, size_t n) {
    if (g_pend_len + n > g_pend_cap) {
        g_pend_cap = (g_pend_len + n) * 2 + 4096;
        g_pend = realloc(g_pend, g_pend_cap);
        if (!g_pend) rt_fatal("out of memory (GX FIFO)");
    }
    memcpy(g_pend + g_pend_len, p, n);
    g_pend_len += n;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* size of one vertex for VAT `vat`, from the current VCD/VAT CP registers */
static const int k_comp_size[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const int k_color_size[8] = {2, 3, 4, 2, 3, 4, 0, 0};

int gx_vertex_size(int vat) {
    uint32_t lo = g_gx.cp[0x50], hi = g_gx.cp[0x60];
    uint32_t a = g_gx.cp[0x70 + vat], b = g_gx.cp[0x80 + vat], cc = g_gx.cp[0x90 + vat];
    int size = 0;
    for (int i = 0; i < 9; i++) size += (lo >> i) & 1;  /* PNMTXIDX, TEX0..7 MTXIDX: one byte each */
    static const int idx_size[4] = {0, 0, 1, 2};
    /* position */
    int t = (lo >> 9) & 3;
    if (t == 1) size += ((a & 1) ? 3 : 2) * k_comp_size[(a >> 1) & 7];
    else size += idx_size[t];
    /* normal */
    t = (lo >> 11) & 3;
    if (t == 1) size += ((a >> 9) & 1 ? 9 : 3) * k_comp_size[(a >> 10) & 7];
    else if (t >= 2) size += idx_size[t] * (((a >> 9) & 1) && ((a >> 31) & 1) ? 3 : 1);
    /* colors */
    for (int c = 0; c < 2; c++) {
        t = (lo >> (13 + 2 * c)) & 3;
        int fmt = c == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        if (t == 1) size += k_color_size[fmt];
        else size += idx_size[t];
    }
    /* texture coordinates */
    static const struct { int word, cnt, fmt; } tex[8] = {
        {0, 21, 22}, {1, 0, 1}, {1, 9, 10}, {1, 18, 19}, {1, 27, 28}, {2, 5, 6}, {2, 14, 15}, {2, 23, 24}};
    for (int i = 0; i < 8; i++) {
        t = (hi >> (2 * i)) & 3;
        uint32_t w = tex[i].word == 0 ? a : tex[i].word == 1 ? b : cc;
        if (t == 1) size += (((w >> tex[i].cnt) & 1) ? 2 : 1) * k_comp_size[(w >> tex[i].fmt) & 7];
        else size += idx_size[t];
    }
    return size;
}

/* [perf] "draw done": how long after the game issued it (BP 0x45 stored into the pipe) the GP thread
 * reached it. The game waits for it at the end of each frame (GXDrawDone), so this is the GP thread's
 * share of the frame's critical path. */
static _Atomic uint64_t g_dd_issued, g_dd_wait, g_dd_max, g_dd_n;
void gx_drawdone_stats(uint64_t* total, uint64_t* max, uint64_t* n) {
    *total = atomic_exchange(&g_dd_wait, 0); *max = atomic_exchange(&g_dd_max, 0); *n = atomic_exchange(&g_dd_n, 0);
}
void gx_decode_sync(void);  /* gx_render.c: queued vertex decodes finished */
static _Atomic int g_pe_det_pending;  /* SR_DETCLOCK: PE updates waiting (gx_det_deliver) */
static struct { int finish, token, tok_irq; uint16_t tok; } g_pe_det;  /* under g_pe_lock */
static void pe_finish(void) {
    gx_decode_sync();  /* the game may reuse vertex arrays once it sees draw done */
    {
        uint64_t t0 = atomic_exchange(&g_dd_issued, 0);
        if (t0) {
            uint64_t d = rt_ticks() - t0;
            atomic_fetch_add(&g_dd_wait, d);
            atomic_fetch_add(&g_dd_n, 1);
            if (d > atomic_load(&g_dd_max)) atomic_store(&g_dd_max, d);
        }
    }
    mtx_lock(&g_pe_lock);
    gx_tl('F', 0);
    int ppc_detclock(void);
    if (ppc_detclock()) {  /* SR_DETCLOCK: the status and the interrupt wait for gx_det_deliver */
        g_pe_det.finish = 1;
        mtx_unlock(&g_pe_lock);
        atomic_store(&g_pe_det_pending, 1);
        return;
    }
    PE_INTRSR |= 0x8;  /* finish status */
    int irq = (PE_INTRSR & 0x2) != 0;
    mtx_unlock(&g_pe_lock);
    if (irq) os_raise_interrupt(OS_INTERRUPT_PI_PE_FINISH);
}

static void pe_token(uint16_t token, int irq) {
    gx_decode_sync();
    mtx_lock(&g_pe_lock);
    gx_tl('T', token | (unsigned)irq << 16);
    int ppc_detclock(void);
    if (ppc_detclock()) {  /* SR_DETCLOCK: the register, status and interrupt wait for gx_det_deliver */
        g_pe_det.token = 1;
        g_pe_det.tok = token;
        g_pe_det.tok_irq |= irq;
        mtx_unlock(&g_pe_lock);
        atomic_store(&g_pe_det_pending, 1);
        return;
    }
    PE_TOKEN = token;
    int raise = 0;
    if (irq) {
        PE_INTRSR |= 0x4;  /* token status */
        raise = (PE_INTRSR & 0x1) != 0;
    }
    mtx_unlock(&g_pe_lock);
    if (raise) os_raise_interrupt(OS_INTERRUPT_PI_PE_TOKEN);
}

/* SR_DETCLOCK: the GP thread reaches draw-done and token commands at times set by the host. What they
 * change (the token register, the status bits) and their interrupts wait here and are applied at a point
 * where no game thread is running (the CPU idles, or only a thread parked in a known wait loop runs:
 * vi.c, sched.c), or when the game reads a PE register (after the GP has caught up), so the game sees
 * them at the same place in its work every run. */
extern int g_wg_n;  /* (defined below with the write-gather pipe) */
/* GPU work that could still wake a game thread: held PE updates, or commands the game thread wrote but
 * has not pushed to the GP yet (flushed when it idles, gx_pipe_idle) */
int gx_det_pending(void) { return atomic_load(&g_pe_det_pending) != 0 || *(volatile int*)&g_wg_n != 0; }
int gx_det_wg(void) { return *(volatile int*)&g_wg_n != 0; }  /* commands in the gather buffer, not pushed yet */
int gx_det_parts(void) { return (atomic_load(&g_pe_det_pending) != 0) | (*(volatile int*)&g_wg_n != 0) << 1; }  /* diagnostics */
int gx_det_deliver(void) {
    if (!atomic_exchange(&g_pe_det_pending, 0)) return 0;
    mtx_lock(&g_pe_lock);
    int raise_f = 0, raise_t = 0;
    if (g_pe_det.finish) {
        PE_INTRSR |= 0x8;
        raise_f = (PE_INTRSR & 0x2) != 0;
    }
    if (g_pe_det.token) {
        PE_TOKEN = g_pe_det.tok;
        if (g_pe_det.tok_irq) {
            PE_INTRSR |= 0x4;
            raise_t = (PE_INTRSR & 0x1) != 0;
        }
    }
    memset(&g_pe_det, 0, sizeof g_pe_det);
    mtx_unlock(&g_pe_lock);
    if (raise_f) os_raise_interrupt(OS_INTERRUPT_PI_PE_FINISH);
    if (raise_t) os_raise_interrupt(OS_INTERRUPT_PI_PE_TOKEN);
    return 1;
}

/* BP registers whose write is an action (draw done, tokens, EFB copy, TLUT load), not only state */
static int bp_is_action(uint32_t reg) { return reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52 || reg == 0x65; }

static void load_bp(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    if (reg >= 0xE0 && reg <= 0xE7 && (val >> 23)) {  /* konstant color: same address, type bit set */
        if (g_gx.konst[reg - 0xE0] == val) return;  /* games rewrite unchanged state: nothing to snapshot */
        g_gx.konst[reg - 0xE0] = val;
        g_gx.regs_dirty = 1;
        gx_bp_written(reg, val);
        return;
    }
    if (reg != 0xFE) {  /* BP mask register: the next write only changes the masked bits */
        uint32_t m = g_gx.bp_mask, nv = (g_gx.bp[reg] & ~m) | (val & m);
        g_gx.bp_mask = 0xFFFFFF;
        if (nv == g_gx.bp[reg] && !bp_is_action(reg)) return;
        g_gx.bp[reg] = nv;
        g_gx.regs_dirty = 1;
    } else {
        g_gx.bp_mask = val;
        return;
    }
    switch (reg) {
    case 0x45: pe_finish(); break;                          /* PE_DONE: draw done */
    case 0x47: pe_token((uint16_t)val, 0); break;           /* PE_TOKEN */
    case 0x48: pe_token((uint16_t)val, 1); break;           /* PE_TOKEN_INT */
    default: break;
    }
    gx_bp_written(reg, g_gx.bp[reg]);
}

void gx_state_dirty(void);

static void load_xf(uint32_t addr, const uint8_t* data, uint32_t n) {
    int changed = 0;
    for (uint32_t i = 0; i < n; i++, addr++) {
        uint32_t v = rd32(data + 4 * i);
        if (addr < 0x1000) {
            if (g_gx.xf_mem[addr] == v) continue;
            g_gx.xf_mem[addr] = v;
            g_gx.xfm_dirty |= 1u << ((addr >> 6) & 31);
            changed = 1;
        } else if (addr < 0x1100 && (addr < 0x1000 + GX_XR_ASPECT || addr > 0x1000 + GX_XR_WIDE_FLAGS)) {
            if (g_gx.xf_reg[addr - 0x1000] == v) continue;
            g_gx.xf_reg[addr - 0x1000] = v;
            g_gx.regs_dirty = 1;
            changed = 1;
        }
    }
    if (changed) gx_state_dirty();
}

/* LOAD_INDX_A..D: copy n words from an indexed array element into XF memory */
/* SR_DFF (gx_dff.c): memory the GP reads, recorded with the frame */
void gx_dff_memory(uint32_t addr, uint32_t size);
extern int g_dff_active;
static void load_indexed(int array, uint32_t v) {
    uint32_t index = v >> 16, n = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    uint32_t base = g_gx.cp[0xA0 + 12 + array] & 0x3FFFFFFF, stride = g_gx.cp[0xB0 + 12 + array] & 0xFF;
    uint32_t src = 0x80000000u | (base + index * stride);
    if (g_dff_active) gx_dff_memory(src, n * 4);
    for (uint32_t i = 0; i < n && addr + i < 0x1000; i++) {
        uint32_t w = ld32(src + 4 * i);
        if (g_gx.xf_mem[addr + i] == w) continue;
        g_gx.xf_mem[addr + i] = w;
        g_gx.xfm_dirty |= 1u << (((addr + i) >> 6) & 31);
        gx_state_dirty();
    }
}

static size_t parse(const uint8_t* p, size_t len, int depth);

/* SR_GXSTAT=1 (env.txt on the Switch): what the GX command stream is made of, for the native GX plan
 * (docs/design.md): bytes per command kind, immediate stream vs display lists, vertex attribute modes,
 * and which game functions store into the write-gather pipe. Logged every 10 s by gx_stat_report. */
int g_gxstat = -1;
static unsigned long long gs_bytes[2][8], gs_cmds[2][8], gs_dl_calls, gs_dl_bytes, gs_verts[2], gs_draws[2];
static unsigned long long gs_attr[4][4];  /* position, normal, colour 0, texcoord 0: none/direct/index8/index16, per vertex */
enum { GS_CP, GS_XF, GS_INDX, GS_CALL, GS_BP, GS_DRAW, GS_OTHER };
#define GS_FN_SLOTS 512
static uint32_t gs_fn[GS_FN_SLOTS], gs_fn_n[GS_FN_SLOTS];
static unsigned long long gs_fn_lost;
static inline int gxstat_on(void) {
    if (g_gxstat < 0) { const char* e = getenv("SR_GXSTAT"); g_gxstat = e && *e == '1'; }
    return g_gxstat;
}
void gs_store(void) {  /* game thread: a pipe store, charged to the last function entered */
    uint32_t fn = g_ppc_cur_fn, h = (fn * 2654435761u) >> 23;
    for (int k = 0; k < 16; k++, h = (h + 1) & (GS_FN_SLOTS - 1)) {
        if (gs_fn[h] == fn) { gs_fn_n[h]++; return; }
        if (!gs_fn[h]) { gs_fn[h] = fn; gs_fn_n[h] = 1; return; }
    }
    gs_fn_lost++;
}
static void gs_dl_report(void);
void gx_stat_report(void) {
    if (!gxstat_on()) return;
    gs_dl_report();
    unsigned long long tot = 0;
    for (int d = 0; d < 2; d++) for (int k = 0; k < 8; k++) tot += gs_bytes[d][k];
    if (!tot) return;
    static const char* const nm[] = {"CP regs", "XF regs", "indexed XF", "display list calls", "BP regs", "draws", "other"};
    for (int d = 0; d < 2; d++) {
        char line[512]; int o = 0;
        for (int k = 0; k < 7; k++)
            o += snprintf(line + o, sizeof line - o, "%s%s %llu KB (%llu)", k ? ", " : "", nm[k], gs_bytes[d][k] >> 10, gs_cmds[d][k]);
        rt_log("[gxstat] %s: %s; %llu draws, %llu vertices", d ? "inside display lists" : "immediate stream", line, gs_draws[d], gs_verts[d]);
    }
    rt_log("[gxstat] display list calls %llu, %llu KB run from them", gs_dl_calls, gs_dl_bytes >> 10);
    {
        extern unsigned long long g_cull_draws, g_cull_off, g_cull_behind, g_cull_verts, g_cull_off_verts;
        if (g_cull_draws)
            rt_log("[gxstat] 3D draws wholly outside the view: %llu of %llu (%.1f%%; %llu of them behind the camera), "
                   "%.1f%% of their index count", g_cull_off, g_cull_draws, 100.0 * g_cull_off / g_cull_draws, g_cull_behind,
                   g_cull_verts ? 100.0 * g_cull_off_verts / g_cull_verts : 0.0);
        g_cull_draws = g_cull_off = g_cull_behind = g_cull_verts = g_cull_off_verts = 0;
    }
    static const char* const an[] = {"position", "normal", "colour 0", "texcoord 0"};
    for (int a = 0; a < 4; a++)
        rt_log("[gxstat] vertices with %s: none %llu, direct %llu, 8-bit index %llu, 16-bit index %llu", an[a],
               gs_attr[a][0], gs_attr[a][1], gs_attr[a][2], gs_attr[a][3]);
    unsigned long long st = 0;
    for (int i = 0; i < GS_FN_SLOTS; i++) st += gs_fn_n[i];
    rt_log("[gxstat] pipe stores on the game thread: %llu (%llu not charged); by last function entered:", st, gs_fn_lost);
    for (int k = 0; k < 25 && st; k++) {
        uint32_t best = 0; int bi = -1;
        for (int i = 0; i < GS_FN_SLOTS; i++) if (gs_fn_n[i] > best) { best = gs_fn_n[i]; bi = i; }
        if (bi < 0) break;
        rt_log("[gxstat]   %5.1f%%  %08X", 100.0 * best / st, gs_fn[bi]);
        gs_fn_n[bi] = 0;
    }
    memset(gs_bytes, 0, sizeof gs_bytes); memset(gs_cmds, 0, sizeof gs_cmds); memset(gs_attr, 0, sizeof gs_attr);
    memset(gs_verts, 0, sizeof gs_verts); memset(gs_draws, 0, sizeof gs_draws);
    memset(gs_fn, 0, sizeof gs_fn); memset(gs_fn_n, 0, sizeof gs_fn_n);
    gs_dl_calls = gs_dl_bytes = gs_fn_lost = 0;
}
/* per display list (the models' drawing data): calls and vertices, to name what is drawn most */
#define GS_DL_SLOTS 2048
static uint32_t gs_dla[GS_DL_SLOTS], gs_dls[GS_DL_SLOTS], gs_dlc[GS_DL_SLOTS];
static unsigned long long gs_dlv[GS_DL_SLOTS];
static void gs_dl(uint32_t addr, uint32_t size, unsigned long long verts) {
    uint32_t h = (addr * 2654435761u) >> 21;
    for (int k = 0; k < 32; k++, h = (h + 1) & (GS_DL_SLOTS - 1)) {
        if (gs_dla[h] == addr || !gs_dla[h]) { gs_dla[h] = addr; gs_dls[h] = size; gs_dlc[h]++; gs_dlv[h] += verts; return; }
    }
}
static void gs_dl_report(void) {
    unsigned long long calls = 0;
    int distinct = 0;
    for (int i = 0; i < GS_DL_SLOTS; i++) if (gs_dla[i]) { calls += gs_dlc[i]; distinct++; }
    if (!calls) return;
    rt_log("[gxstat] %d different display lists; the 20 called most (address, size, calls, vertices):", distinct);
    for (int k = 0; k < 20; k++) {
        uint32_t best = 0; int bi = -1;
        for (int i = 0; i < GS_DL_SLOTS; i++) if (gs_dlc[i] > best) { best = gs_dlc[i]; bi = i; }
        if (bi < 0) break;
        rt_log("[gxstat]   %08X %6u B  %5.1f%% of calls  %llu vertices", gs_dla[bi], gs_dls[bi], 100.0 * best / calls, gs_dlv[bi]);
        gs_dlc[bi] = 0;
    }
    memset(gs_dla, 0, sizeof gs_dla); memset(gs_dlc, 0, sizeof gs_dlc); memset(gs_dlv, 0, sizeof gs_dlv);
}
static inline void gs_cmd(int depth, int kind, size_t n) {
    if (g_gxstat == 1) { gs_bytes[depth > 0][kind] += n; gs_cmds[depth > 0][kind]++; }
}

static void call_display_list(uint32_t addr, uint32_t size, int depth) {
    if (depth > 4 || size > 0x4000000) return;
    if (g_dff_active) gx_dff_memory(addr, size);
    const uint8_t* p = mem_phys(addr & 0x03FFFFFF);
    if (g_gxstat == 1) {
        gs_dl_calls++;
        gs_dl_bytes += size;
        unsigned long long v0 = gs_verts[1];
        parse(p, size, depth + 1);
        gs_dl(addr, size, gs_verts[1] - v0);
        return;
    }
    parse(p, size, depth + 1);
}

/* Parse complete commands from p[0..len); returns the bytes consumed (a partial command stays). */
/* at depth 0: the bytes the incomplete command at the end of the buffer needs in total (0: unknown),
 * so the write-gather pipe does not re-parse it on every store until it is complete */
/* g_parse_need; g_parse_depth: 0 = the immediate stream, more inside display lists (g_gpside) */

static size_t parse(const uint8_t* p, size_t len, int depth) {
    size_t i = 0;
    if (depth == 0) g_parse_need = 0;
    int saved_depth = g_parse_depth;
    g_parse_depth = depth;
    void gx_dff_cmd(const uint8_t* p, size_t n);
    size_t mark = 0;  /* SR_DFF: each whole command of the immediate stream is recorded after it ran */
    while (i < len) {
        if (g_dff_active && depth == 0 && i > mark) gx_dff_cmd(p + mark, i - mark);
        mark = i;
        uint8_t cmd = p[i];
        size_t left = len - i;
        if (cmd == 0x00) { i++; continue; }                             /* NOP */
        if (cmd == 0x08) {                                              /* LOAD_CP_REG */
            if (left < 6) break;
            g_gx.cp[p[i + 1]] = rd32(p + i + 2);
            gs_cmd(depth, GS_CP, 6);
            i += 6;
            continue;
        }
        if (cmd == 0x10) {                                              /* LOAD_XF_REG */
            if (left < 5) break;
            uint32_t h = rd32(p + i + 1), n = (h >> 16) + 1;
            if (left < 5 + 4 * n) {
                if (depth == 0) g_parse_need = 5 + 4 * n;
                break;
            }
            load_xf(h & 0xFFFF, p + i + 5, n);
            gs_cmd(depth, GS_XF, 5 + 4 * n);
            i += 5 + 4 * n;
            continue;
        }
        if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) { /* LOAD_INDX_A..D */
            if (left < 5) break;
            load_indexed((cmd >> 3) & 3, rd32(p + i + 1));
            gs_cmd(depth, GS_INDX, 5);
            i += 5;
            continue;
        }
        if (cmd == 0x40) {                                              /* CALL_DL */
            if (left < 9) break;
            gs_cmd(depth, GS_CALL, 9);
            call_display_list(rd32(p + i + 1), rd32(p + i + 5), depth);
            i += 9;
            continue;
        }
        if (cmd == 0x44 || cmd == 0x48) { i++; continue; }              /* vertex cache invalidate */
        if (cmd == 0x61) {                                              /* LOAD_BP_REG */
            if (left < 5) break;
            load_bp(rd32(p + i + 1));
            gs_cmd(depth, GS_BP, 5);
            i += 5;
            continue;
        }
        if ((cmd & 0xC0) == 0x80) {                                     /* draw primitive */
            if (left < 3) break;
            int vat = cmd & 7, prim = (cmd >> 3) & 7;
            uint32_t count = rd16(p + i + 1);
            size_t vsize = (size_t)gx_vertex_size(vat), total = 3 + vsize * count;
            if (left < total) {
                if (depth == 0) g_parse_need = total;
                break;
            }
            gx_draw(prim, vat, count, p + i + 3, (uint32_t)vsize);
            g_gx.draws++;
            if (g_gxstat == 1) {
                gs_cmd(depth, GS_DRAW, total);
                gs_draws[depth > 0]++;
                gs_verts[depth > 0] += count;
                uint32_t lo = g_gx.cp[0x50], hi = g_gx.cp[0x60];
                gs_attr[0][(lo >> 9) & 3] += count;
                gs_attr[1][(lo >> 11) & 3] += count;
                gs_attr[2][(lo >> 13) & 3] += count;
                gs_attr[3][hi & 3] += count;
            }
            i += total;
            continue;
        }
        gs_cmd(depth, GS_OTHER, 1);
        if (!g_gx.bad_logged++)
            rt_log("[gx] unknown FIFO command %02X (depth %d); skipping a byte", cmd, depth);
        i++;
    }
    if (g_dff_active && depth == 0 && i > mark) gx_dff_cmd(p + mark, i - mark);
    g_parse_depth = saved_depth;
    return i;
}

/* g_need (g_gpside): pending bytes needed before parsing again (the incomplete command's size) */
unsigned long long g_pipe_stores, g_pipe_bytes;  /* for the [perf] line */

static void gp_feed(const uint8_t* p, size_t n) {
    pend_push(p, n);
    if (g_pend_len < g_need) return;  /* the command being written is not complete yet */
    size_t used = parse(g_pend, g_pend_len, 0);
    if (used) {
        memmove(g_pend, g_pend + used, g_pend_len - used);
        g_pend_len -= used;
    }
    g_need = g_parse_need;
}

/* ---- the GP thread: a single-producer (game thread), single-consumer ring of pipe bytes ---- */
#define RING_SIZE (16u << 20)
static uint8_t* g_ring;
static _Atomic size_t g_ring_head;  /* bytes written, ever (consumed: g_ring_tail, in g_gpside) */
static size_t g_tail_seen;          /* the game thread's last look at g_ring_tail */
static mtx_t g_gp_lock;
static cnd_t g_gp_cv, g_space_cv;
static int g_gx_async = 1;
uint64_t gx_render_wait_total(void);  /* gx_render.c: GP-thread ticks spent waiting for the render thread, ever */
static _Atomic uint64_t g_gp_wait;     /* those waits, since the last gx_gp_wait_ticks() */
uint64_t gx_gp_busy_ticks(void) { return atomic_exchange(&g_gp_busy, 0); }
uint64_t gx_gp_wait_ticks(void) { return atomic_exchange(&g_gp_wait, 0); }
static _Atomic uint64_t g_gp_busy_all;  /* never reset (frame_pacing.c measures it per frame) */
uint64_t gx_gp_busy_total(void) { return atomic_load_explicit(&g_gp_busy_all, memory_order_relaxed); }
/* FIFO bytes written but not yet processed by the GP thread (0 in synchronous mode) */
size_t gx_gp_backlog(void) {
    return atomic_load_explicit(&g_ring_head, memory_order_relaxed) - atomic_load_explicit(&g_ring_tail, memory_order_relaxed);
}

#define KICK_BYTES 32768u
static size_t g_kicked_at;  /* ring head at the last wake check (game thread only) */
static void ring_kick(void);
static void ring_push(const uint8_t* b, size_t n) {
    size_t head = atomic_load_explicit(&g_ring_head, memory_order_relaxed);
    /* the GP thread's position is read only when the ring looks full from the last look */
    if (head + n - g_tail_seen > RING_SIZE) g_tail_seen = atomic_load_explicit(&g_ring_tail, memory_order_acquire);
    while (head + n - g_tail_seen > RING_SIZE) {
        /* full: the GP is a whole ring behind; wait for it */
        mtx_lock(&g_gp_lock);
        cnd_signal(&g_gp_cv);
        if (head + n - atomic_load(&g_ring_tail) > RING_SIZE) {
            struct timespec ts;
            timespec_get(&ts, TIME_UTC);
            ts.tv_nsec += 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            cnd_timedwait(&g_space_cv, &g_gp_lock, &ts);
        }
        mtx_unlock(&g_gp_lock);
        g_tail_seen = atomic_load_explicit(&g_ring_tail, memory_order_acquire);
    }
    size_t off = head & (RING_SIZE - 1), first = n < RING_SIZE - off ? n : RING_SIZE - off;
    memcpy(g_ring + off, b, first);
    memcpy(g_ring, b + first, n - first);
    atomic_store_explicit(&g_ring_head, head + n, memory_order_release);
    /* wake a sleeping GP thread only once a batch has built up: waking it per 32-byte burst cost the
     * game thread thousands of futex calls per frame (~7 ms of a 3-4 player frame on the Switch).
     * Sync points (ring_kick) and the GP thread's own short timed wait cover the rest. */
    if (head + n - g_kicked_at >= KICK_BYTES) ring_kick();
}

/* wake the GP thread if it sleeps with work queued (sync points: the game is about to wait on it) */
static void ring_kick(void) {
    size_t head = atomic_load_explicit(&g_ring_head, memory_order_relaxed);
    g_kicked_at = head;
    if (atomic_load_explicit(&g_gp_sleeping, memory_order_acquire) &&
        head != atomic_load_explicit(&g_ring_tail, memory_order_acquire)) {
        mtx_lock(&g_gp_lock);
        cnd_signal(&g_gp_cv);
        mtx_unlock(&g_gp_lock);
    }
}

static int gp_thread(void* arg) {
    (void)arg;
    rt_thread_pin(2);
    rt_thread_lower_priority();
    profile_register_thread();
    const char* slow = getenv("SR_GPSLOW");
    double gp_slow = slow ? atof(slow) : 1.0;
    if (gp_slow > 1.0) rt_log("[gx] SR_GPSLOW: the GP thread takes %.1fx as long", gp_slow);
    for (;;) {
        size_t tail = atomic_load_explicit(&g_ring_tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&g_ring_head, memory_order_acquire);
        if (head == tail) {
            mtx_lock(&g_gp_lock);
            atomic_store(&g_gp_sleeping, 1);
            /* a short timed wait: the producer wakes this thread only per batch and at sync points */
            while (atomic_load(&g_ring_head) == tail) {
                struct timespec ts;
                timespec_get(&ts, TIME_UTC);
                ts.tv_nsec += 500000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                cnd_timedwait(&g_gp_cv, &g_gp_lock, &ts);
            }
            atomic_store(&g_gp_sleeping, 0);
            mtx_unlock(&g_gp_lock);
            continue;
        }
        size_t off = tail & (RING_SIZE - 1), n = head - tail;
        if (n > RING_SIZE - off) n = RING_SIZE - off;  /* up to the end of the ring; the rest next time */
        if (n > 65536) n = 65536;
        uint64_t t0 = rt_ticks(), w0 = gx_render_wait_total();
        gp_feed(g_ring + off, n);
        if (gp_slow > 1.0) {  /* SR_GPSLOW: emulate a slower CPU (the Switch) on the PC */
            uint64_t until = t0 + (uint64_t)((rt_ticks() - t0) * gp_slow);
            while (rt_ticks() < until) {}
        }
        {
            /* work only: the time gx_render.c spent waiting for the render thread (a free frame, or the
             * previous frame still not taken) is back-pressure from the GPU, counted on its own */
            uint64_t w = gx_render_wait_total() - w0, d = rt_ticks() - t0;
            d = d > w ? d - w : 0;
            atomic_fetch_add_explicit(&g_gp_wait, w, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_gp_busy, d, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_gp_busy_all, d, memory_order_relaxed);
        }
        atomic_store_explicit(&g_ring_tail, tail + n, memory_order_release);
        if (head + 65536 - (tail + n) > RING_SIZE) {  /* the producer may be waiting for space */
            mtx_lock(&g_gp_lock);
            cnd_signal(&g_space_cv);
            mtx_unlock(&g_gp_lock);
        }
    }
    return 0;
}

/* ---- palette (TLUT) snapshots ----
 * A TLUT load (BP 0x64 source address, BP 0x65 TMEM address + size) copies the palette from RAM when
 * the GP processes it. The real GP does that within microseconds of the store; our GP thread can be
 * most of a frame behind, and by then the game may have rewritten that RAM (garbled UI colours,
 * worst on the Switch). So the game thread copies the palette at the moment it issues the load (the
 * SDK's GXLoadTlut writes BP registers as a 1-byte 0x61 store and a 4-byte store) and the GP thread
 * uses that copy for the matching immediate-mode load. Each copy carries the register values, so a
 * mismatch falls back to reading RAM. */
#define TLUT_SNAPS 4096
#define TLUT_DATA (4u << 20)
typedef struct { uint32_t v64, v65, off, len, end; } TlutSnap;  /* end: data-ring position after it */
static TlutSnap g_tsnap[TLUT_SNAPS];
static uint8_t* g_tdata;
static _Atomic uint32_t g_tsnap_head, g_tsnap_tail;  /* entries written / consumed, ever */
static uint32_t g_tdata_head;                         /* data bytes written, ever (game thread) */
static _Atomic uint32_t g_tdata_tail;                 /* data bytes released, ever (GP thread) */
int g_prev_bp_prefix;                                 /* the previous pipe store was the 1-byte 0x61 (ppc.h reads it) */
static uint32_t g_store_v64;                          /* the last BP 0x64 value stored by the game */
unsigned long long g_tsnap_used, g_tsnap_missed;      /* [perf] */

static void tlut_snapshot(uint32_t v65) {
    uint32_t src = ((g_store_v64 & 0xFFFFFF) << 5) & GX_PHYS_MASK, tmem = (v65 & 0x3FF) << 9, len = ((v65 >> 10) & 0x7FF) * 32;
    if (tmem + len > 0x100000) len = tmem < 0x100000 ? 0x100000 - tmem : 0;  /* gx_texture.c's TMEM */
    if (src + len > GUEST_RAM_SIZE || len > TLUT_DATA / 4) len = 0;
    uint32_t off = g_tdata_head % TLUT_DATA, skip = off + len > TLUT_DATA ? TLUT_DATA - off : 0;  /* no wrap inside one */
    /* wait for room: the GP thread is a whole ring behind (keeps the pairing in order) */
    while (atomic_load_explicit(&g_tsnap_head, memory_order_relaxed) - atomic_load_explicit(&g_tsnap_tail, memory_order_acquire) >= TLUT_SNAPS ||
           g_tdata_head + skip + len - atomic_load_explicit(&g_tdata_tail, memory_order_acquire) > TLUT_DATA) {
        void gx_pipe_idle(void);
        gx_pipe_idle();  /* the GP thread may need commands still in the batch (pipe_flush) */
    }
    g_tdata_head += skip;
    off = g_tdata_head % TLUT_DATA;
    uint32_t h = atomic_load_explicit(&g_tsnap_head, memory_order_relaxed);
    TlutSnap* sn = &g_tsnap[h % TLUT_SNAPS];
    sn->v64 = g_store_v64; sn->v65 = v65; sn->off = off; sn->len = len;
    if (len) memcpy(g_tdata + off, mem_phys(src), len);
    g_tdata_head += len;
    sn->end = g_tdata_head;
    atomic_store_explicit(&g_tsnap_head, h + 1, memory_order_release);
}

/* the game thread's view of each pipe store (before it is gathered into bursts) */
static void tlut_watch(uint32_t v, int size) {
    if (size == 1) { g_prev_bp_prefix = (v & 0xFF) == 0x61; return; }
    if (size == 4 && g_prev_bp_prefix) {
        uint32_t reg = v >> 24;
        if (reg == 0x45 && (CP_CR & 0x11) == 0x11) atomic_store(&g_dd_issued, rt_ticks());  /* draw done, for [perf] */
        else if (reg == 0x64) g_store_v64 = v;
        else if (reg == 0x65 && (CP_CR & 0x11) == 0x11) tlut_snapshot(v);  /* immediate mode only: not recorded into a display list */
    }
    g_prev_bp_prefix = 0;
}

/* GP thread, BP 0x65 from the immediate stream: copy the palette the game had when it issued the
 * load into dst; returns 0 (read RAM instead) when there is no matching copy */
int gx_tlut_snapshot_copy(uint32_t v64, uint32_t v65, uint8_t* dst, uint32_t len) {
    if (!g_tdata || g_parse_depth) return 0;
    uint32_t t = atomic_load_explicit(&g_tsnap_tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&g_tsnap_head, memory_order_acquire);
    /* the next copy, or one of the next few (skipping stores that only looked like a TLUT load) */
    for (uint32_t k = 0; k < 4 && t + k != h; k++) {
        const TlutSnap* sn = &g_tsnap[(t + k) % TLUT_SNAPS];
        if (((sn->v65 ^ v65) & 0xFFFFFF) || ((sn->v64 ^ v64) & 0xFFFFFF)) continue;  /* bp[] keeps 24 bits */
        memcpy(dst, g_tdata + sn->off, len < sn->len ? len : sn->len);
        atomic_store_explicit(&g_tdata_tail, sn->end, memory_order_release);
        atomic_store_explicit(&g_tsnap_tail, t + k + 1, memory_order_release);
        g_tsnap_used++;
        return 1;
    }
    g_tsnap_missed++;
    return 0;
}

/* ---- write-gather pipe ---- */
static void pipe_write(uint32_t v, int size);
/* the write-gather buffer (see pipe_flush below) */
/* PSO test 5: the stores collect into a 4 KB batch (GX_WG_BATCH, ppc.h) that goes to the GP thread
 * in one piece, instead of a 32-byte hardware burst copied into the game's FIFO in RAM and pushed on
 * its own. Nothing reads that RAM copy (the GP thread takes the stream from its ring); SR_FIFO_RAM=1
 * puts it back. Register reads, sync points and the idle loop flush a partial batch (pipe_flush). */
uint8_t g_wg[GX_WG_BATCH + 8];
int g_wg_n;
static int g_fifo_ram = -1;
void gx_pipe_write(uint32_t v, int size) {
    /* the common case first (most of the ~50,000 stores a busy frame makes): a word that neither
     * follows a BP prefix byte (tlut_watch has nothing to do) nor completes a burst */
    if (__builtin_expect(g_gxstat == 1, 0)) gs_store();
    if (size == 4 && !g_prev_bp_prefix && g_wg_n <= GX_WG_BATCH - 8) {
        uint32_t be = __builtin_bswap32(v);
        memcpy(g_wg + g_wg_n, &be, 4);
        g_wg_n += 4;
        g_pipe_stores++;
        g_pipe_bytes += 4;
        return;
    }
    uint32_t prev = g_ppc_cur_fn;
    g_ppc_cur_fn = PROF_PIPE;
    pipe_write(v, size);
    g_ppc_cur_fn = prev;
}
/* The write-gather pipe collects stores into 32-byte bursts, as the hardware's does (the SDK's GXFlush
 * pads with 32 zero bytes to push the last one out); a burst goes to RAM and the GP at once. Accesses
 * to the PI and CP registers flush a partial one first (the game may be reading the FIFO pointers). */
static void burst_out(const uint8_t* b, int size);

static void pipe_flush(void) {
    if (g_wg_n) {
        int n = g_wg_n;
        g_wg_n = 0;
        burst_out(g_wg, n);
    }
    if (g_gx_async) ring_kick();
}

static void pipe_write(uint32_t v, int size) {
    if (g_tdata) tlut_watch(v, size);
    g_pipe_stores++;
    g_pipe_bytes += (unsigned)size;
    if (size == 4) {  /* the common case: one big-endian word */
        uint32_t be = __builtin_bswap32(v);
        memcpy(g_wg + g_wg_n, &be, 4);
        g_wg_n += 4;
    } else {
        for (int k = 0; k < size; k++) g_wg[g_wg_n++] = (uint8_t)(v >> (8 * (size - 1 - k)));
    }
    if (g_wg_n >= GX_WG_BATCH - 4) {  /* a full batch */
        int n = g_wg_n;
        g_wg_n = 0;
        burst_out(g_wg, n);
    }
}

static void burst_out(const uint8_t* b, int size) {
    if (g_fifo_ram < 0) { const char* e = getenv("SR_FIFO_RAM"); g_fifo_ram = e && *e == '1'; }
    uint32_t w = g_pi_wptr & 0x03FFFFFF;
    uint32_t end = g_pi_end ? (g_pi_end & 0x03FFFFFF) + 32 : 0;  /* end is inclusive of the last 32 bytes */
    if (!g_fifo_ram && (CP_CR & 0x11) == 0x11) {  /* linked (immediate) mode: the GP takes the stream from the
                                                  * ring; only the PI write pointer moves, wrapping as the FIFO does */
        if (g_pi_end) {
            uint32_t base = g_pi_base & 0x03FFFFFF, len = end > base ? end - base : 0;
            uint32_t nw = w + (uint32_t)size;
            if (nw >= end && len) {
                nw = base + (nw - end) % len;
                g_pi_wptr = nw | 0x04000000;
            } else {
                g_pi_wptr = (g_pi_wptr & 0x04000000) | nw;
            }
        }
    } else
    /* into RAM at the PI write pointer: in one piece unless the FIFO wraps inside this burst */
    if (g_pi_end && w + (uint32_t)size < end && w + (uint32_t)size <= GUEST_RAM_SIZE) {
        memcpy(mem_phys(w), b, (size_t)size);
        g_pi_wptr = (g_pi_wptr & 0x04000000) | (w + (uint32_t)size);
    } else {
        for (int k = 0; k < size; k++) {
            w = g_pi_wptr & 0x03FFFFFF;
            if (g_pi_end && w < GUEST_RAM_SIZE) *mem_phys(w) = b[k];
            w++;
            if (g_pi_end && w >= end) {
                w = g_pi_base & 0x03FFFFFF;
                g_pi_wptr = w | 0x04000000;
            } else {
                g_pi_wptr = (g_pi_wptr & 0x04000000) | w;
            }
        }
    }
    if ((CP_CR & 0x11) == 0x11) {  /* GP read enable + linked: immediate mode */
        if (g_gx_async) ring_push(b, (size_t)size);
        else gp_feed(b, (size_t)size);
    }
}

/* ---- register blocks ---- */
unsigned g_cp_reads, g_pe_reads;  /* [perf]: polling of GP status by the game */
uint32_t gx_cp_read(uint32_t off, int size) {
    g_cp_reads++;
    pipe_flush();
    off &= 0x7F;
    if (off == 0x00) return 0x000C;  /* SR: GP read idle + command idle */
    if (size == 4) return (uint32_t)g_cp_mmio[off >> 1] << 16 | g_cp_mmio[(off >> 1) + 1];
    return g_cp_mmio[off >> 1];
}
void gx_cp_write(uint32_t off, uint32_t v, int size) {
    pipe_flush();
    off &= 0x7F;
    if (size == 4) { g_cp_mmio[off >> 1] = (uint16_t)(v >> 16); g_cp_mmio[(off >> 1) + 1] = (uint16_t)v; }
    else g_cp_mmio[off >> 1] = (uint16_t)v;
}

uint32_t gx_pe_read(uint32_t off, int size) {
    g_pe_reads++;
    pipe_flush();  /* a game polling draw sync tokens waits for commands still in the gather buffer */
    {   /* SR_DETCLOCK: the GP finishes what was sent, then its held updates apply: the game reads the
         * same value every run */
        int ppc_detclock(void);
        if (ppc_detclock()) {
            while (gx_gp_backlog()) platform_sleep_ns(50000);
            gx_det_deliver();
        }
    }
    off &= 0xFF;
    mtx_lock(&g_pe_lock);
    uint32_t v = size == 4 ? (uint32_t)g_pe_mmio[off >> 1] << 16 | g_pe_mmio[(off >> 1) + 1] : g_pe_mmio[off >> 1];
    mtx_unlock(&g_pe_lock);
    return v;
}
void gx_pe_write(uint32_t off, uint32_t v, int size) {
    pipe_flush();
    off &= 0xFF;
    if (off == 0x0A) {  /* writing a 1 to a status bit clears it; the enable bits are stored */
        mtx_lock(&g_pe_lock);
        PE_INTRSR = (uint16_t)((PE_INTRSR & 0xC & ~v) | (v & 0x3));
        mtx_unlock(&g_pe_lock);
        return;
    }
    if (size == 4) { g_pe_mmio[off >> 1] = (uint16_t)(v >> 16); g_pe_mmio[(off >> 1) + 1] = (uint16_t)v; }
    else g_pe_mmio[off >> 1] = (uint16_t)v;
}

/* PI: interrupt cause/mask (0x00/0x04), FIFO base/end/write pointer (0x0C/0x10/0x14) */
uint32_t gx_pi_read(uint32_t off, int size) {
    (void)size;
    pipe_flush();
    off &= 0xFF;
    switch (off) {
    case 0x0C: return g_pi_base;
    case 0x10: return g_pi_end;
    case 0x14: return g_pi_wptr;
    case 0x2C: return 0x20000000u | 0x2;  /* console type: retail, Flipper rev */
    default: return g_pi_regs[off >> 2];
    }
}
void gx_pi_write(uint32_t off, uint32_t v, int size) {
    (void)size;
    pipe_flush();
    off &= 0xFF;
    switch (off) {
    case 0x0C: g_pi_base = v & 0x03FFFFE0; break;
    case 0x10: g_pi_end = v & 0x03FFFFE0; break;
    case 0x14: g_pi_wptr = v & 0x07FFFFE0; break;
    default: g_pi_regs[off >> 2] = v; break;
    }
}

int gx_async(void) { return g_gx_async; }
/* the game thread has nothing to run (sched.c): what it wrote reaches the GP now */
void gx_pipe_idle(void) { pipe_flush(); }

/* the CPU FIFO's bounds (PI FIFO base / end, physical): gx_dff.c */
void gx_pi_fifo_range(uint32_t* base, uint32_t* end) { *base = g_pi_base & 0x03FFFFFF; *end = g_pi_end & 0x03FFFFFF; }
void gx_dff_init(void);
void gx_init(void) {
    gx_dff_init();
    g_gx.bp_mask = 0xFFFFFF;
    mtx_init(&g_pe_lock, mtx_plain);
    if (getenv("SR_GX_SYNC")) {
        g_gx_async = 0;
        rt_log("[gx] SR_GX_SYNC: the FIFO is parsed on the game thread");
        return;
    }
    if (!getenv("SR_NOTLUTSNAP")) g_tdata = malloc(TLUT_DATA);  /* SR_NOTLUTSNAP=1: read palettes late, as before */
    g_ring = malloc(RING_SIZE);
    if (!g_ring) rt_fatal("cannot allocate the GP ring");
    mtx_init(&g_gp_lock, mtx_plain);
    cnd_init(&g_gp_cv);
    cnd_init(&g_space_cv);
    rt_thread_start(gp_thread, NULL);
}

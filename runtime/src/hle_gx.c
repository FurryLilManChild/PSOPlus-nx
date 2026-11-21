/* Native replacements for the GX library's matrix writers (config/GPOE8P/hle.txt).
 *
 * 0x8038EA3C (the SDK's WriteMTXPS4x3) and 0x8038EA70 (WriteMTXPS3x3) copy a matrix from memory
 * (r3) into the write-gather pipe (r4) with paired-single loads and stores: 12 and 9 floats. Every
 * position, normal and texture matrix load goes through them (GXLoadPosMtxImm 0x8038EAC8,
 * GXLoadNrmMtxImm 0x8038EB04, GXLoadTexMtxImm 0x8038EB80): about 11,000 matrices a frame in the
 * Forest's busy spots, a fifth of all pipe stores (test 4's SR_GXSTAT).
 *
 * The data goes to the pipe in the order and format the game's code writes it: memory holds the floats
 * big-endian, as the command stream does, so the matrix is one copy into the batch buffer. The
 * registers the original leaves behind (f0-f5 loaded, r3 and r4 unchanged) are set the same way.
 * When the target is not the pipe, or GQR0 is not the plain-float setting the SDK relies on, the
 * game's own code runs. */
#include <stdint.h>
#include <string.h>

#include "ppc.h"
#include "runtime.h"

void f_8038EA3C_orig(Cpu* c);
void f_8038EA70_orig(Cpu* c);

static inline double f32_at(uint32_t ea) { return (double)u32_as_f32(ld32r(ea)); }

/* append n big-endian words from guest memory at src to the pipe batch */
static inline void pipe_copy(uint32_t src, int n) {
    if (!g_prev_bp_prefix && g_wg_n + 4 * n <= GX_WG_BATCH - 8 && g_gxstat != 1) {
        memcpy(g_wg + g_wg_n, ppc_ptr(src), (size_t)(4 * n));
        g_wg_n += 4 * n;
        g_pipe_stores += (unsigned long long)n;
        g_pipe_bytes += (unsigned long long)(4 * n);
        return;
    }
    for (int i = 0; i < n; i++) ppc_pipe32(ld32r(src + 4u * (uint32_t)i));  /* batch boundary, statistics on */
}

/* the matrix source is ordinary RAM (cached or uncached mirror), never a device */
static inline int ram_src(uint32_t a, uint32_t len) {
    uint32_t p = a & 0x3FFFFFFFu;
    return !PPC_IS_MMIO(a) && p + len <= GUEST_RAM_SIZE;
}

void hle_WriteMTXPS4x3(Cpu* c) {
    uint32_t src = c->r[3], dst = c->r[4];
    if (c->gqr[0] != 0 || !ppc_is_pipe(dst) || !ram_src(src, 48)) { f_8038EA3C_orig(c); return; }
    pipe_copy(src, 12);
    for (int i = 0; i < 6; i++) {  /* f0-f5 = the six float pairs */
        c->f[i].ps0 = f32_at(src + 8u * (uint32_t)i);
        c->f[i].ps1 = f32_at(src + 8u * (uint32_t)i + 4u);
    }
}

void hle_WriteMTXPS3x3(Cpu* c) {
    uint32_t src = c->r[3], dst = c->r[4];
    if (c->gqr[0] != 0 || !ppc_is_pipe(dst) || !ram_src(src, 48)) { f_8038EA70_orig(c); return; }
    for (int row = 0; row < 3; row++) {  /* each row of the 3x4 source: its first three floats */
        uint32_t r = src + 16u * (uint32_t)row;
        pipe_copy(r, 3);
        c->f[2 * row].ps0 = f32_at(r);         /* psq_l: a pair */
        c->f[2 * row].ps1 = f32_at(r + 4u);
        double v = f32_at(r + 8u);             /* lfs: the single into both halves */
        c->f[2 * row + 1].ps0 = v;
        c->f[2 * row + 1].ps1 = v;
    }
}

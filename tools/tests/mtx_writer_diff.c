/* Differential test: the native matrix writers (runtime/src/hle_gx.c) against the game's own code.
 *
 * Links the recompiled originals f_8038EA3C_orig (WriteMTXPS4x3) and f_8038EA70_orig (WriteMTXPS3x3),
 * extracted from build/gen at test time by run_mtx_writer_diff.sh (nothing from the game is in this
 * file), with hle_gx.c and the runtime's guest window (runtime/src/memory.c). Each case puts a random
 * matrix in RAM or in the locked cache, runs both with r4 = 0xCC008000 (the GX pipe) from the same
 * starting batch fill, and compares byte for byte the pipe stream (bytes flushed at a batch boundary
 * plus the batch buffer g_wg / g_wg_n), f0-f5 (ps0 and ps1, as bits), r3, r4 and the pipe counters.
 * Also the guest-span helper (runtime.h guest_span_ok) at the edges of both windows.
 *
 * Usage: mtx_writer_diff [cases] [seed] */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc.h"
#include "runtime.h"

/* ---- stand-ins for the runtime pieces these functions touch ---- */
uint8_t g_wg[GX_WG_BATCH];
int g_wg_n, g_prev_bp_prefix, g_gxstat;
unsigned long long g_pipe_stores, g_pipe_bytes;
volatile int g_core_preempt[3];
volatile uint32_t g_ppc_cur_fn, g_ppc_cur_lr;
int g_ppc_trace;
uint8_t* g_mod_hook_flags;
void ppc_trace_enter(uint32_t a) { (void)a; }
void ppc_preempt(Cpu* c) { (void)c; }
void gs_store(void) {}
void rt_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
_Noreturn void rt_fatal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(2); }
static int g_bad_mmio;
uint32_t mmio_read(uint32_t ea, int size) { (void)ea; (void)size; g_bad_mmio++; return 0; }
void mmio_write(uint32_t ea, uint32_t v, int size) { (void)ea; (void)v; (void)size; g_bad_mmio++; }
void mmio_write64(uint32_t ea, uint64_t v) { (void)ea; (void)v; g_bad_mmio++; }
void f_8038EAA4(Cpu* c) { (void)c; fprintf(stderr, "unreachable tail reached\n"); exit(2); }

/* the flushed part of the stream: what gx_pipe_write would hand on (a full batch, then the word) */
static uint8_t g_flushed[1 << 16];
static size_t g_nflushed;
void gx_pipe_write(uint32_t v, int size) {
    memcpy(g_flushed + g_nflushed, g_wg, (size_t)g_wg_n);
    g_nflushed += (size_t)g_wg_n;
    g_wg_n = 0;
    for (int k = size - 1; k >= 0; k--) g_flushed[g_nflushed++] = (uint8_t)(v >> (8 * k));
}

void f_8038EA3C_orig(Cpu* c);
void f_8038EA70_orig(Cpu* c);
/* the script links with --wrap on the originals: a fall back from the native code to the game's is
 * counted, so a run where the native path never ran cannot pass */
void __real_f_8038EA3C_orig(Cpu* c);
void __real_f_8038EA70_orig(Cpu* c);
static int g_in_hle;
static long g_fallbacks;
void __wrap_f_8038EA3C_orig(Cpu* c) { g_fallbacks += g_in_hle; __real_f_8038EA3C_orig(c); }
void __wrap_f_8038EA70_orig(Cpu* c) { g_fallbacks += g_in_hle; __real_f_8038EA70_orig(c); }
void hle_WriteMTXPS4x3(Cpu* c);
void hle_WriteMTXPS3x3(Cpu* c);

/* ---- xorshift ---- */
static uint64_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }
static float rndf(void) { return ((int32_t)rnd() / 2147483648.0f) * (float)(1u << (rnd() % 12)); }

typedef struct {
    uint8_t stream[1 << 16]; size_t n;
    uint64_t f[6][2]; uint32_t r3, r4;
    unsigned long long stores, bytes;
} Result;

static void run(int which, int hle, uint32_t src, int fill, Result* out) {
    Cpu c; memset(&c, 0, sizeof c);
    for (int i = 0; i < 32; i++) c.f[i].ps0 = c.f[i].ps1 = -12345.0;
    c.r[3] = src; c.r[4] = 0xCC008000u; c.gqr[0] = 0;
    g_nflushed = 0; g_wg_n = fill; memset(g_wg, 0xA5, (size_t)fill);
    g_pipe_stores = g_pipe_bytes = 0;
    g_in_hle = hle;
    if (which == 0) { if (hle) hle_WriteMTXPS4x3(&c); else f_8038EA3C_orig(&c); }
    else { if (hle) hle_WriteMTXPS3x3(&c); else f_8038EA70_orig(&c); }
    g_in_hle = 0;
    memcpy(out->stream, g_flushed, g_nflushed);
    memcpy(out->stream + g_nflushed, g_wg, (size_t)g_wg_n);
    out->n = g_nflushed + (size_t)g_wg_n;
    for (int i = 0; i < 6; i++) { memcpy(&out->f[i][0], &c.f[i].ps0, 8); memcpy(&out->f[i][1], &c.f[i].ps1, 8); }
    out->r3 = c.r[3]; out->r4 = c.r[4];
    out->stores = g_pipe_stores; out->bytes = g_pipe_bytes;
}

static int span_test(void) {
    struct { uint32_t a; int want; } t[] = {
        {0x80000000u, 1}, {0x817FFFD0u, 1}, {0x81BFFFD0u, 1}, {0xC1BFFFD0u, 1}, {0x01BFFFD0u, 1},
        {0xE0000000u, 1}, {0xE0003FD0u, 1},
        {0xE0003FE0u, 0}, {0xE0003FFCu, 0}, {0xE0004000u, 0}, {0xCC008000u, 0}, {0x81C00000u, 0}, {0x81BFFFD4u, 0},
        {0xDFFFFFF0u, 0}, {0xC8000000u, 0}, {0x90000000u, 0},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int got = guest_span_ok(t[i].a, 48);
        printf("  guest_span_ok(%08X, 48) = %d (want %d)%s\n", t[i].a, got, t[i].want, got == t[i].want ? "" : "  FAIL");
        fails += got != t[i].want;
    }
    return fails;
}

int main(int argc, char** argv) {
    long cases = argc > 1 ? atol(argv[1]) : 20000;
    g_rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ull;
    mem_init();
    printf("guest span helper (GUEST_RAM_SIZE %08X, locked cache %08X + %X):\n", GUEST_RAM_SIZE, LOCKED_CACHE_PHYS, LOCKED_CACHE_SIZE);
    int span_fail = span_test();
    printf("span helper: %d failures\n", span_fail);

    static Result a, b;
    long pass[2][2] = {{0}}, fail[2][2] = {{0}}, boundary = 0, nan_cases = 0, nan_diff = 0, nan_unexplained = 0;
    for (long k = 0; k < cases; k++) {
        int which = (int)(k & 1);
        int lc = (int)((k >> 1) & 1);  /* half the cases in the locked cache */
        uint32_t src;
        if (lc) src = 0xE0000000u + 4u * (rnd() % ((LOCKED_CACHE_SIZE - 48) / 4 + 1));
        else {
            uint32_t base = (rnd() & 1) ? 0x80000000u : 0xC0000000u;
            src = base + 4u * (rnd() % ((GUEST_RAM_SIZE - 48) / 4 + 1));
        }
        int raw = rnd() % 10 == 0;  /* one case in ten: raw random bit patterns (NaN, inf, denormals) */
        uint32_t w[12];
        for (int i = 0; i < 12; i++) {
            if (raw) w[i] = rnd() ^ (rnd() << 16);
            else { float f = rndf(); memcpy(&w[i], &f, 4); }
        }
        for (int i = 0; i < 12; i++) st32(src + 4u * (uint32_t)i, w[i]);
        int snan = 0;
        for (int i = 0; i < 12; i++) snan |= (w[i] & 0x7FC00000u) == 0x7F800000u && (w[i] & 0x003FFFFFu);
        /* the starting fill: mostly mid-batch, sometimes right at the boundary so the stream splits */
        int fill = (rnd() % 8) ? (int)(4 * (rnd() % 64)) : (int)(GX_WG_BATCH - 8 - 4 * (rnd() % 16));
        if (fill > GX_WG_BATCH - 8 - 44) boundary++;
        run(which, 0, src, fill, &a);
        run(which, 1, src, fill, &b);
        int same = a.n == b.n && !memcmp(a.stream, b.stream, a.n) && !memcmp(a.f, b.f, sizeof a.f) && a.r3 == b.r3 &&
                   a.r4 == b.r4 && a.stores == b.stores && a.bytes == b.bytes;
        if (snan) {
            /* a signalling NaN: the original goes float -> double -> float through the host FPU, which
             * quiets it; the native copy keeps the bits, as the GameCube's paired-single store does */
            nan_cases++;
            if (!same) nan_diff++;
            /* everything else must match, and the stream only in the quiet bit of those words */
            int expl = a.n == b.n && !memcmp(a.f, b.f, sizeof a.f) && a.r3 == b.r3 && a.r4 == b.r4 && a.stores == b.stores;
            for (size_t i = 0; expl && i < a.n; i++) {
                uint8_t x = a.stream[i] ^ b.stream[i];
                if (x && !(x == 0x40 && (i & 3) == 1)) expl = 0;  /* the quiet bit: byte 1 of a big-endian float */
            }
            if (!expl) nan_unexplained++;
            continue;
        }
        if (same) pass[which][lc]++;
        else {
            if (fail[which][lc]++ < 5)
                printf("MISMATCH case %ld: %s src %08X fill %d: stream %zu vs %zu bytes, r3 %08X/%08X r4 %08X/%08X stores %llu/%llu\n",
                       k, which ? "3x3" : "4x3", src, fill, a.n, b.n, a.r3, b.r3, a.r4, b.r4, a.stores, b.stores);
        }
    }
    printf("cases %ld (boundary-straddling fills %ld)\n", cases, boundary);
    printf("  4x3 RAM: pass %ld fail %ld\n", pass[0][0], fail[0][0]);
    printf("  4x3 locked cache: pass %ld fail %ld\n", pass[0][1], fail[0][1]);
    printf("  3x3 RAM: pass %ld fail %ld\n", pass[1][0], fail[1][0]);
    printf("  3x3 locked cache: pass %ld fail %ld\n", pass[1][1], fail[1][1]);
    printf("  signalling-NaN cases set aside: %ld (differing only in the quiet bit: %ld, any other difference: %ld)\n",
           nan_cases, nan_diff - nan_unexplained, nan_unexplained);
    printf("  native calls that fell back to the game's code (must be 0): %ld\n", g_fallbacks);
    printf("  device accesses (must be 0): %d\n", g_bad_mmio);
    long fails = fail[0][0] + fail[0][1] + fail[1][0] + fail[1][1];
    int ok = !fails && !span_fail && !g_bad_mmio && !g_fallbacks && !nan_unexplained;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

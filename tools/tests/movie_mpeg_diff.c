/* Differential test: the native MPEG-1 slice decoder (runtime/src/movie_mpeg.c) against the game's own
 * Sofdec decoder, on a real movie.
 *
 * Links movie.rel's recompiled functions, extracted from build/gen by run_movie_mpeg_diff.sh (nothing
 * from the game is in this file), with movie_mpeg.c and the runtime's guest window. It lays movie.rel
 * out at its fixed address, initialises Sofdec's MPV decoder as the game does (MPV init 0x81810394,
 * create 0x8180FEB0, parameters 0x8180FA80: output mode 3), demuxes the video stream from an .sfd
 * (MPEG-1 program stream) and then, per picture, runs the game's header decoder (0x8180F8E0) and
 * picture decoder (0x8180E0A4) with reference frames chosen as the game's 0x81827DCC chooses them.
 *
 * compare mode: every picture is decoded twice from the same guest memory, once with the game's slice
 * decoder (0x8180E498's original code) and once with the native one, and all of guest RAM (MEM1 and
 * the REL area, 28 MiB) is compared byte for byte, except the stack below the caller and the decoder's
 * per-macroblock scratch (listed in SCRATCH below, each with the code that writes it). Also the return
 * value and the data pointer/length/consumed words. Any difference is reported.
 * native / orig modes: one decoder only, timed (ms per picture), optional I420 output in display order.
 *
 * usage: movie_mpeg_diff MOVIE_REL SFD compare|native|orig [max_pictures] [out.yuv]
 *        movie_mpeg_diff MOVIE_REL - idct [blocks/6]   (the transform alone, random blocks) */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ppc.h"
#include "runtime.h"

/* ---- stand-ins for the runtime ---- */
volatile int g_core_preempt[3];
volatile uint32_t g_ppc_cur_fn, g_ppc_cur_lr;
int g_ppc_trace;
uint8_t* g_mod_hook_flags;
void ppc_trace_enter(uint32_t a) { (void)a; }
void ppc_preempt(Cpu* c) { (void)c; }
static int g_quiet_log;
void rt_log(const char* fmt, ...) {
    if (g_quiet_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
_Noreturn void rt_fatal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(2); }
uint32_t mmio_read(uint32_t ea, int size) { rt_fatal("device read %08X (%d)", ea, size); }
void mmio_write(uint32_t ea, uint32_t v, int size) { (void)v; rt_fatal("device write %08X (%d)", ea, size); }
void mmio_write64(uint32_t ea, uint64_t v) { (void)v; rt_fatal("device write64 %08X", ea); }
/* the GX pipe: none of the decoder's code writes it (g_prev_bp_prefix set: any write reaches gx_pipe_write) */
uint8_t g_wg[GX_WG_BATCH];
int g_wg_n, g_prev_bp_prefix = 1, g_gxstat;
unsigned long long g_pipe_stores, g_pipe_bytes;
void gx_pipe_write(uint32_t v, int size) { (void)v; rt_fatal("GX pipe write (%d bytes)", size); }
void gs_store(void) { rt_fatal("GX pipe store"); }
void ppc_unimplemented(Cpu* c, uint32_t addr, uint32_t insn) { (void)c; rt_fatal("unimplemented %08X at %08X", insn, addr); }
uint32_t ppc_mfspr(Cpu* c, int spr) {
    (void)c;
    if (spr == 920) return 0;  /* HID2: no locked cache (the game's MPV work area then stays in RAM) */
    rt_fatal("mfspr %d", spr);
}
void ppc_mtspr(Cpu* c, int spr, uint32_t v) { (void)c; (void)v; rt_fatal("mtspr %d", spr); }
uint64_t platform_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t ppc_timebase(void) { return platform_now_ns() * 81 / 2000; }
uint64_t rt_ticks(void) { return ppc_timebase(); }

/* dispatch over movie.rel's functions (movie_rel_table.c, generated) */
typedef struct { uint32_t addr; PpcFunc fn; } FnEnt;
extern const FnEnt g_movie_fns[];
extern const unsigned g_movie_fn_count;
void ppc_dispatch(Cpu* c) {
    unsigned lo = 0, hi = g_movie_fn_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (g_movie_fns[m].addr < c->pc) lo = m + 1; else hi = m;
    }
    if (lo < g_movie_fn_count && g_movie_fns[lo].addr == c->pc) { g_movie_fns[lo].fn(c); return; }
    rt_fatal("indirect call to %08X (lr %08X): not a movie.rel function", c->pc, c->lr);
}

/* ---- the native decoder and the game's, switchable ---- */
void native_movie_slice(Cpu* c);   /* movie_mpeg.c, built with -Dhle_movie_slice=native_movie_slice */
void f_8180E498_orig(Cpu* c);
static int g_use_native;
static unsigned long long g_fallback_calls, g_native_calls;
void test_slice_orig(Cpu* c) { g_fallback_calls += g_use_native; f_8180E498_orig(c); }  /* -Df_8180E498_orig=... */
void hle_movie_slice(Cpu* c) {
    if (g_use_native) { g_native_calls++; native_movie_slice(c); }
    else f_8180E498_orig(c);
}

void f_81810394(Cpu* c);  /* MPV init (count, work) */
void f_8180FEB0(Cpu* c);  /* MPV create -> handle */
void f_8180FA80(Cpu* c);  /* MPV parameter (handle, id, value) */
void f_8180F8E0(Cpu* c);  /* headers up to a picture (handle, &{ptr, len}, &consumed) */
void f_8180E3D8(Cpu* c);  /* picture attributes (handle, out) */
void f_8180E0A4(Cpu* c);  /* picture body (handle, &{ptr, len}, &consumed, frames) */

/* ---- the IDCT alone: random coefficient blocks through the game's 0x8180822C and the native one ---- */
void f_8180822C(Cpu* c);
void movie_mpeg_test_idct(const float* c, int16_t* out);
static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)g_rng; }
static int idct_test(int n) {
    uint32_t base = 0x80800000u, coef = base + 0x1000, outp = base + 0x2000, tmp = base + 0x3000, list = base + 0x100;
    long bad = 0;
    for (int it = 0; it < n; it++) {
        float cf[6][64];
        for (int b = 0; b < 6; b++) {
            int nnz = 1 + (int)(rnd() % 24);
            memset(cf[b], 0, sizeof cf[b]);
            for (int k = 0; k < nnz; k++) {  /* integer levels times prescale-like factors, as the decoder makes */
                int idx = (int)(rnd() % 64), v = (int)(rnd() % 2001) - 1000;
                if (rnd() % 8 == 0) v = (int)(rnd() % 200001) - 100000;
                float pre = 0.01f + (float)(rnd() % 100000) / 40000.0f;
                cf[b][idx] = (float)v * pre;
            }
            if (rnd() % 3 == 0) cf[b][0] = (float)(rnd() % 2048) * 0.125f;
            for (int k = 0; k < 64; k++) { uint32_t u; memcpy(&u, &cf[b][k], 4); st32(coef + 0x100u * (uint32_t)b + 4u * (uint32_t)k, u); }
            st8(base + (uint32_t)b, 1);  /* "has AC": the full transform for every block */
            st32(list + 4u * (uint32_t)b, outp + 0x80u * (uint32_t)b);
        }
        st32(base + 0x2C, coef); st32(base + 0x30, list); st32(base + 0x48, tmp);
        Cpu c; memset(&c, 0, sizeof c); c.r[1] = 0x80F00000u; c.r[3] = base; c.gqr[7] = 0x00070007u;  /* s16, as 0x818085C4 sets */
        f_8180822C(&c);
        for (int b = 0; b < 6; b++) {
            int16_t mine[64];
            movie_mpeg_test_idct(cf[b], mine);
            for (int k = 0; k < 64; k++) {
                int16_t g = (int16_t)ld16(outp + 0x80u * (uint32_t)b + 2u * (uint32_t)k);
                if (g != mine[k] && bad++ < 10) printf("  IDCT case %d block %d value %d: game %d, native %d\n", it, b, k, g, mine[k]);
            }
        }
    }
    printf("IDCT: %d blocks through both transforms, %ld values differ\n", n * 6, bad);
    printf("RESULT: %s\n", bad ? "DIFFERENT" : "IDENTICAL");
    return bad ? 1 : 0;
}

/* ---- guest layout ---- */
#define STACK_TOP 0x80F00000u
#define IO        0x80F80000u  /* {ptr, len} at +0, consumed +8, consumed2 +0xC, attr +0x40, frames +0x80, out attr +0x100 */
#define WORK      0x80600000u  /* the MPV work area (the game passes a bss buffer) */
#define WORK_SIZE 0x00030000u
#define ESBUF     0x80A00000u
#define ESBUF_SIZE 0x00400000u
#define FRAMES    0x80100000u

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t* q, uint32_t v) { q[0] = (uint8_t)(v >> 24); q[1] = (uint8_t)(v >> 16); q[2] = (uint8_t)(v >> 8); q[3] = (uint8_t)v; }
static void put16(uint8_t* q, uint16_t v) { q[0] = (uint8_t)(v >> 8); q[1] = (uint8_t)v; }

static uint8_t* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(*n + 16);
    if (fread(b, 1, *n, f) != *n) rt_fatal("cannot read %s", path);
    fclose(f);
    return b;
}

/* movie.rel at its fixed address (runtime/src/loader.c link_rel) */
extern const uint32_t g_rel_base, g_rel_bss, g_rel_bss_size;
static void link_rel(const uint8_t* rel, uint32_t rel_size) {
    uint32_t base = g_rel_base, nsec = be32(rel + 0x0C), secinfo = be32(rel + 0x10), id = be32(rel + 0x00);
    uint32_t imp_off = be32(rel + 0x28), imp_size = be32(rel + 0x2C), sec_addr[32] = {0};
    memcpy(mem_phys(base), rel, rel_size);
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t o = be32(rel + secinfo + 8 * i) & ~3u, s = be32(rel + secinfo + 8 * i + 4);
        sec_addr[i] = o ? base + o : (s ? g_rel_bss : 0);
    }
    memset(mem_phys(g_rel_bss), 0, g_rel_bss_size);
    for (uint32_t k = 0; k < imp_size / 8; k++) {
        uint32_t mod = be32(rel + imp_off + 8 * k), p = be32(rel + imp_off + 8 * k + 4), cur = 0, sec = 0;
        for (;; p += 8) {
            uint16_t o = be16(rel + p);
            uint8_t t = rel[p + 2], ts = rel[p + 3];
            uint32_t add = be32(rel + p + 4);
            if (t == 203) break;
            if (t == 202) { sec = ts; cur = 0; continue; }
            cur += o;
            if (t == 201) continue;
            uint32_t where = sec_addr[sec] + cur, target = mod == id ? sec_addr[ts] + add : add;
            uint8_t* q = mem_phys(where);
            switch (t) {
            case 1: put32(q, target); break;
            case 2: put32(q, (be32(q) & 0xFC000003u) | (target & 0x03FFFFFCu)); break;
            case 3: case 4: put16(q, (uint16_t)target); break;
            case 5: put16(q, (uint16_t)(target >> 16)); break;
            case 6: put16(q, (uint16_t)((target + 0x8000) >> 16)); break;
            case 7: put32(q, (be32(q) & 0xFFFF0003u) | (target & 0xFFFCu)); break;
            case 10: put32(q, (be32(q) & 0xFC000003u) | ((target - where) & 0x03FFFFFCu)); break;
            case 11: put32(q, (be32(q) & 0xFFFF0003u) | ((target - where) & 0xFFFCu)); break;
            default: rt_fatal("REL relocation type %u", t);
            }
        }
    }
}

/* the video elementary stream (stream 0xE0) of an MPEG-1 program stream */
static uint8_t* demux(const uint8_t* d, size_t n, size_t* out_n) {
    uint8_t* es = malloc(n);
    size_t o = 0, i = 0;
    while (i + 4 <= n) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) { i++; continue; }
        uint8_t id = d[i + 3];
        if (id == 0xBA) {  /* pack: MPEG-1 (0010) 12 bytes, MPEG-2 (01) 14 + stuffing */
            if ((d[i + 4] >> 6) == 1) i += 14 + (d[i + 13] & 7); else i += 12;
            continue;
        }
        if (id == 0xB9) { i += 4; continue; }
        if (id < 0xBB) { i++; continue; }
        if (i + 6 > n) break;
        size_t len = be16(d + i + 4), p = i + 6, end = p + len;
        if (end > n) end = n;
        if (id == 0xE0) {
            while (p < end && d[p] == 0xFF) p++;
            if (p < end && (d[p] & 0xC0) == 0x40) p += 2;
            if (p < end && (d[p] & 0xF0) == 0x20) p += 5;
            else if (p < end && (d[p] & 0xF0) == 0x30) p += 10;
            else if (p < end && d[p] == 0x0F) p += 1;
            else if (p < end && (d[p] & 0xC0) == 0x80) {  /* MPEG-2 PES header */
                p += 3 + d[p + 2];
            }
            if (p < end) { memcpy(es + o, d + p, end - p); o += end - p; }
        }
        i = end;
    }
    *out_n = o;
    return es;
}

/* ---- comparison ---- */
#define RAM_LEN GUEST_RAM_SIZE
typedef struct { uint32_t lo, hi; const char* why; } Range;  /* ctx-relative, [lo, hi) */
/* written by the game's code while it decodes macroblocks and not read again before the next slice
 * header or picture rewrites them (the native decoder keeps these in host variables) */
static const Range SCRATCH[] = {
    {0x03C, 0x070, "block parameters 0x3C-0x6F (0x8180AE50, 0x8180ADA4, 0x81819EB0, 0x81815954)"},
    {0x070, 0x076, "per-block 'has AC' bytes (0x8180AE50, 0x8180ADA4)"},
    {0x098, 0x09C, "coded block mask for the IDCT (0x8180ADA4)"},
    {0x0CC, 0x0E0, "prediction fetch parameters (0x81814ECC)"},
    {0x0F8, 0x128, "store destination table (0x8180C68C, 0x818157CC, 0x8181520C, ...)"},
    {0x2A4, 0x2AC, "macroblock type and coded block pattern words (slice loops)"},
    {0x2E0, 0x5E0, "IDCT output, s16 (0x8180822C, 0x81807E94)"},
    {0x5E0, 0xBE0, "coefficients, float (0x8180AE50, 0x81815954)"},
    {0xC60, 0xF60, "prediction and IDCT temporary (0x81814ECC, 0x8180822C)"},
};
static uint8_t *g_snap, *g_ref;
static unsigned long long g_diff_pictures, g_diff_bytes;

static const char* region(uint32_t pa, uint32_t ctx_pa, uint32_t* rel) {
    if (pa >= ctx_pa && pa < ctx_pa + 0x2000) {
        *rel = pa - ctx_pa;
        for (unsigned k = 0; k < sizeof SCRATCH / sizeof SCRATCH[0]; k++)
            if (*rel >= SCRATCH[k].lo && *rel < SCRATCH[k].hi) return NULL;
        return "decoder state";
    }
    *rel = pa;
    if (pa >= (STACK_TOP & PPC_PHYS_MASK) - 0x10000 && pa < (STACK_TOP & PPC_PHYS_MASK)) return NULL;  /* stack */
    if (pa >= (FRAMES & PPC_PHYS_MASK) && pa < (WORK & PPC_PHYS_MASK)) return "frames";
    return "other memory";
}
static void compare(uint32_t ctx, int pic, int type) {
    const uint8_t* now = mem_phys(0);
    uint32_t ctx_pa = ctx & PPC_PHYS_MASK;
    int reported = 0, bad = 0;
    for (uint32_t i = 0; i < RAM_LEN; i++) {
        if (now[i] == g_ref[i]) continue;
        uint32_t rel;
        const char* r = region(i, ctx_pa, &rel);
        if (!r) continue;
        bad = 1;
        g_diff_bytes++;
        if (reported < 12) {
            printf("  picture %d (type %d): %s +0x%X: game %02X, native %02X\n", pic, type, r, rel, g_ref[i], now[i]);
            reported++;
        }
    }
    if (bad) g_diff_pictures++;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s MOVIE_REL SFD compare|native|orig [max] [out.yuv]\n", argv[0]); return 2; }
    const char* mode = argv[3];
    int cmp = !strcmp(mode, "compare");
    g_use_native = !strcmp(mode, "native");
    int maxpic = argc > 4 ? atoi(argv[4]) : 1 << 30;
    FILE* yuv = argc > 5 ? fopen(argv[5], "wb") : NULL;
    FILE* times = getenv("MOVIE_TIMES") ? fopen(getenv("MOVIE_TIMES"), "w") : NULL;  /* picture, type, ms */
    mem_init();
    size_t rn, sn, en;
    uint8_t* rel = read_file(argv[1], &rn);
    link_rel(rel, (uint32_t)rn);
    if (!strcmp(mode, "idct")) return idct_test(argc > 4 ? atoi(argv[4]) : 100000);
    uint8_t* sfd = read_file(argv[2], &sn);
    uint8_t* es;
    if (sn > 4 && !sfd[0] && !sfd[1] && sfd[2] == 1 && sfd[3] == 0xB3) {  /* a bare video stream (psogc_j*.sfd) */
        es = sfd;
        en = sn;
    } else {
        es = demux(sfd, sn, &en);
        free(sfd);
    }
    printf("%s: %zu bytes of MPEG-1 video\n", argv[2], en);
    if (getenv("MOVIE_CORRUPT")) {  /* damaged data: N random bytes changed per MiB, the first 4 KiB kept */
        unsigned seed = 1, per_mib = 4;
        sscanf(getenv("MOVIE_CORRUPT"), "%u,%u", &seed, &per_mib);
        g_rng = 0x9E3779B97F4A7C15ull * (seed + 1);
        size_t n = en / (1 << 20) * per_mib + per_mib;
        for (size_t k = 0; k < n; k++) {
            size_t at = 4096 + (size_t)(((uint64_t)rnd() << 32 | rnd()) % (en - 4096));
            es[at] ^= (uint8_t)(1u << (rnd() % 8));
        }
        printf("changed %zu bits of the stream (seed %u)\n", n, seed);
    }

    Cpu c;
    memset(&c, 0, sizeof c);
    c.r[1] = STACK_TOP;
    memset(mem_phys(WORK), 0, WORK_SIZE);
    c.r[3] = 8; c.r[4] = WORK;
    f_81810394(&c);
    if (c.r[3]) rt_fatal("MPV init: %08X", c.r[3]);
    f_8180FEB0(&c);
    uint32_t ctx = c.r[3];
    if (!ctx) rt_fatal("MPV create failed");
    static const uint32_t params[][2] = {{1, 1}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 3}};
    for (unsigned k = 0; k < 6; k++) { c.r[3] = ctx; c.r[4] = params[k][0]; c.r[5] = params[k][1]; f_8180FA80(&c); }
    printf("MPV handle at %08X\n", ctx);

    if (cmp) { g_snap = malloc(RAM_LEN); g_ref = malloc(RAM_LEN); }
    size_t es_pos = 0;
    uint32_t win = ESBUF, win_len = 0;
    int pic = 0, w = 0, h = 0, sy = 0, sc = 0, nfr = 4;
    uint32_t frame[4], fsize = 0;
    int fwd = -1, bwd = -1, held = -1;  /* held: the reference shown after the next I/P (display order) */
    double t_sum = 0, t_max = 0;
    unsigned long long counts[4] = {0};
    for (; pic < maxpic; pic++) {
        /* keep 2 MiB of data ahead in the window */
        if (win_len < 0x200000 && es_pos < en) {
            memmove(mem_phys(ESBUF), mem_phys(win), win_len);
            win = ESBUF;
            size_t add = ESBUF_SIZE - 64 - win_len;
            if (add > en - es_pos) add = en - es_pos;
            memcpy(mem_phys(win + win_len), es + es_pos, add);
            es_pos += add;
            win_len += (uint32_t)add;
            memset(mem_phys(win + win_len), 0, 64);
        }
        st32(IO, win); st32(IO + 4, win_len); st32(IO + 8, 0);
        c.r[3] = ctx; c.r[4] = IO; c.r[5] = IO + 8;
        f_8180F8E0(&c);
        int r = (int)c.r[3];
        uint32_t used = ld32(IO + 8);
        win += used; win_len -= used;
        if (r != 0) { printf("headers: %d after %d pictures (end of data)\n", r, pic); break; }
        c.r[3] = ctx; c.r[4] = IO + 0x40;
        f_8180E3D8(&c);
        int type = (int)ld32(IO + 0x40 + 0x18);
        if (!fsize) {
            w = (int)ld32(IO + 0x40); h = (int)ld32(IO + 0x44);
            int w16 = (w + 15) & ~15, h16 = (h + 15) & ~15;
            sy = (w16 + 31) & ~31; sc = (w16 / 2 + 31) & ~31;
            fsize = (uint32_t)(sy * h16 + 2 * sc * (h16 / 2) + 64 + 31) & ~31u;
            for (int k = 0; k < nfr; k++) frame[k] = FRAMES + (uint32_t)k * fsize;
            if (FRAMES + 4 * fsize > WORK) rt_fatal("frames do not fit");
            memset(mem_phys(FRAMES), 0, 4 * fsize);
            printf("picture size %dx%d, strides %d/%d\n", w, h, sy, sc);
        }
        int h16 = (h + 15) & ~15;
        int cur;
        if (type == 1 || type == 2) {
            int nf = 0;
            while (nf == fwd || nf == bwd || nf == held) nf++;
            fwd = bwd; bwd = nf; cur = bwd;
        } else {
            cur = 0;
            while (cur == fwd || cur == bwd || cur == held) cur++;
        }
        /* frames: {Cb, Cr, Y, -, stride C, stride Y} x2, current Y, attribute out, 0, 0 */
        uint32_t d = IO + 0x80;
        for (int k = 0; k < 2; k++) {
            int f = k ? bwd : fwd;
            uint32_t y = f < 0 ? 0 : frame[f], cb = y + (uint32_t)(sy * h16), cr = cb + (uint32_t)(sc * h16 / 2);
            st32(d + 16 * k, f < 0 ? 0 : cb); st32(d + 16 * k + 4, f < 0 ? 0 : cr); st32(d + 16 * k + 8, y);
            st16(d + 16 * k + 12, (uint16_t)sc); st16(d + 16 * k + 14, (uint16_t)sy);
        }
        st32(d + 0x20, frame[cur]); st32(d + 0x24, IO + 0x100); st32(d + 0x28, 0); st32(d + 0x2C, 0);
        st32(IO + 0x10, win); st32(IO + 0x14, win_len); st32(IO + 0xC, 0);
        Cpu c0 = c, c1;
        uint32_t ref_used = 0;
        int ref_ret = 0;
        if (cmp) {
            memcpy(g_snap, mem_phys(0), RAM_LEN);
            g_use_native = 0;
            c.r[3] = ctx; c.r[4] = IO + 0x10; c.r[5] = IO + 0xC; c.r[6] = d;
            f_8180E0A4(&c);
            ref_ret = (int)c.r[3];
            ref_used = ld32(IO + 0xC);
            memcpy(g_ref, mem_phys(0), RAM_LEN);
            memcpy(mem_phys(0), g_snap, RAM_LEN);
            c1 = c;
            c = c0;
            g_use_native = 1;
        }
        uint64_t t0 = platform_now_ns();
        c.r[3] = ctx; c.r[4] = IO + 0x10; c.r[5] = IO + 0xC; c.r[6] = d;
        f_8180E0A4(&c);
        double ms = (platform_now_ns() - t0) / 1e6;
        t_sum += ms;
        if (ms > t_max) t_max = ms;
        int ret = (int)c.r[3];
        uint32_t used2 = ld32(IO + 0xC);
        if (times) fprintf(times, "%d,%d,%.4f\n", pic, type, ms);
        if (cmp) {
            if (ret != ref_ret || used2 != ref_used || c.r[1] != c1.r[1])
                printf("  picture %d: return %d / %d, consumed %u / %u (game / native)\n", pic, ref_ret, ret, ref_used, used2);
            compare(ctx, pic, type);
        }
        if (type >= 1 && type <= 3) counts[type]++;
        win += used2; win_len -= used2;
        if (ret != 0) printf("picture %d: decode returned %d\n", pic, ret);
        if (yuv) {  /* display order: an I/P is shown when the next I/P arrives; B at once */
            int show = type == 3 ? cur : held;
            if (type != 3) held = cur;
            if (show >= 0) {
                const uint8_t* y = mem_phys(frame[show]);
                const uint8_t* cb = y + sy * h16;
                const uint8_t* cr = cb + sc * h16 / 2;
                for (int r2 = 0; r2 < h; r2++) fwrite(y + r2 * sy, 1, (size_t)w, yuv);
                for (int r2 = 0; r2 < h / 2; r2++) fwrite(cb + r2 * sc, 1, (size_t)w / 2, yuv);
                for (int r2 = 0; r2 < h / 2; r2++) fwrite(cr + r2 * sc, 1, (size_t)w / 2, yuv);
            }
        } else if (type != 3) {
            held = -1;
        }
        if (pic % 500 == 0) { printf("  %d pictures...\n", pic); fflush(stdout); }
    }
    if (yuv && held >= 0) {
        const uint8_t* y = mem_phys(frame[held]);
        const uint8_t* cb = y + sy * ((h + 15) & ~15);
        const uint8_t* cr = cb + sc * ((h + 15) & ~15) / 2;
        for (int r2 = 0; r2 < h; r2++) fwrite(y + r2 * sy, 1, (size_t)w, yuv);
        for (int r2 = 0; r2 < h / 2; r2++) fwrite(cb + r2 * sc, 1, (size_t)w / 2, yuv);
        for (int r2 = 0; r2 < h / 2; r2++) fwrite(cr + r2 * sc, 1, (size_t)w / 2, yuv);
    }
    if (yuv) fclose(yuv);
    if (times) fclose(times);
    printf("%d pictures (I %llu, P %llu, B %llu); %s decoder: %.3f ms/picture (longest %.3f)\n", pic, counts[1], counts[2], counts[3],
           g_use_native ? "native" : "game's", pic ? t_sum / pic : 0, t_max);
    if (g_use_native) printf("native slice calls %llu, of them run by the game's decoder %llu\n", g_native_calls, g_fallback_calls);
    if (cmp) {
        printf("RESULT: %s (%llu pictures differ, %llu bytes)\n", g_diff_pictures ? "DIFFERENT" : "IDENTICAL", g_diff_pictures, g_diff_bytes);
        return g_diff_pictures || g_fallback_calls ? 1 : 0;
    }
    return 0;
}

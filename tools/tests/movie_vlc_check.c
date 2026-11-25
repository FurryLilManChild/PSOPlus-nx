/* Exhaustive check of runtime/src/movie_mpeg.c's VLC lookup tables (typed from ISO/IEC 11172-2 annex B)
 * against an independent decoder's tables: pl_mpeg (phoboslab/pl_mpeg, MIT), not part of this repository.
 * Every bit pattern of each table's width is decoded both ways (pl_mpeg walks its code tree bit by bit) and
 * the value and code length must agree, invalid codes included.
 *
 * build: cc -std=gnu11 -O1 -Iruntime/include -Iruntime/src -I<folder with pl_mpeg.h> tools/tests/movie_vlc_check.c
 * (run_movie_vlc_check.sh fetches pl_mpeg.h at a pinned commit into build/tests) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

/* the decoder itself, for its tables (its runtime calls are never made here) */
#include "../../runtime/src/movie_mpeg.c"
uint8_t* ppc_mem_base_var;
volatile int g_core_preempt[3];
volatile uint32_t g_ppc_cur_fn, g_ppc_cur_lr;
int g_ppc_trace;
uint8_t* g_mod_hook_flags;
void ppc_trace_enter(uint32_t a) { (void)a; }
void ppc_preempt(Cpu* c) { (void)c; }
void rt_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
uint32_t mmio_read(uint32_t ea, int size) { (void)ea; (void)size; abort(); }
void mmio_write(uint32_t ea, uint32_t v, int size) { (void)ea; (void)v; (void)size; abort(); }
void mmio_write64(uint32_t ea, uint64_t v) { (void)ea; (void)v; abort(); }
uint8_t g_wg[GX_WG_BATCH];
int g_wg_n, g_prev_bp_prefix, g_gxstat;
unsigned long long g_pipe_stores, g_pipe_bytes;
void gx_pipe_write(uint32_t v, int size) { (void)v; (void)size; abort(); }
void gs_store(void) { abort(); }
uint64_t platform_now_ns(void) { return 0; }
uint64_t rt_ticks(void) { return 0; }
void f_8180E498_orig(Cpu* c) { (void)c; abort(); }
void f_8180E87C(Cpu* c) { (void)c; abort(); }

/* pl_mpeg's tree walk over the top `bits` bits of w: value, or -1000 if the code is invalid or longer */
static int walk(const plm_vlc_t* t, unsigned w, int bits, int* len) {
    plm_vlc_t s = {0, 0};
    for (int n = 0; n < bits; n++) {
        s = t[s.index + ((w >> (bits - 1 - n)) & 1)];
        if (s.index == -1) return -1000;
        if (s.index <= 0) { *len = n + 1; return s.value; }
    }
    return -1000;
}
static long g_bad;
static void check16(const char* name, const uint16_t* mine, int bits, const plm_vlc_t* ref, int value_offset) {
    long n = 0;
    for (unsigned w = 0; w < (1u << bits); w++) {
        int len = 0, v = walk(ref, w, bits, &len);
        int mlen = mine[w] & 0xFF, mv = (mine[w] >> 8) - value_offset;
        int ok = v == -1000 ? mlen == 0 : (mlen == len && mv == v);
        if (!ok && n++ < 5) printf("  %s %0*X: pl_mpeg %d (%d bits), here %d (%d bits)\n", name, (bits + 3) / 4, w, v, len, mv, mlen);
    }
    printf("%-26s %5u patterns, %ld differ\n", name, 1u << bits, n);
    g_bad += n;
}
static void check_type(const char* name, uint8_t (*mine)[2], int bits, const plm_vlc_t* ref) {
    long n = 0;
    for (unsigned w = 0; w < (1u << bits); w++) {
        int len = 0, v = walk(ref, w, bits, &len);
        int ok = v == -1000 ? mine[w][1] == 0 : (mine[w][1] == len && mine[w][0] == v);
        if (!ok && n++ < 5) printf("  %s %X: pl_mpeg %d (%d bits), here %d (%d bits)\n", name, w, v, len, mine[w][0], mine[w][1]);
    }
    printf("%-26s %5u patterns, %ld differ\n", name, 1u << bits, n);
    g_bad += n;
}

int main(void) {
    build_tables();
    check16("address increment", t_mbai, 11, PLM_VIDEO_MACROBLOCK_ADDRESS_INCREMENT, 0);
    check_type("macroblock type I", t_type_i, 2, PLM_VIDEO_MACROBLOCK_TYPE_INTRA);
    check_type("macroblock type P", t_type_p, 6, PLM_VIDEO_MACROBLOCK_TYPE_PREDICTIVE);
    check_type("macroblock type B", t_type_b, 6, PLM_VIDEO_MACROBLOCK_TYPE_B);
    check16("coded block pattern", t_cbp, 9, PLM_VIDEO_CODE_BLOCK_PATTERN, 0);
    check16("motion code", t_motion, 11, PLM_VIDEO_MOTION, 16);
    check16("DC size, luminance", t_dcy, 7, PLM_VIDEO_DCT_SIZE_LUMINANCE, 0);
    check16("DC size, chrominance", t_dcc, 8, PLM_VIDEO_DCT_SIZE_CHROMINANCE, 0);
    /* DCT coefficients (dct_coeff_next): pl_mpeg's tree ends "1" at one bit, its caller reads the next
     * bit: 0 = end of block, 1 = run 0 level 1; its values are run << 8 | level, 0xFFFF = escape */
    long n = 0;
    for (unsigned w = 0; w < (1u << 16); w++) {
        int len = 0, v = walk((const plm_vlc_t*)PLM_VIDEO_DCT_COEFF, w, 16, &len);
        int run, level;
        if (v == -1000) { run = -1; level = 0; len = 0; }
        else if ((uint16_t)v == 0x0001 && len == 1) { len = 2; if (w & 0x4000) { run = 0; level = 1; } else { run = DCT_EOB; level = 0; } }
        else if ((uint16_t)v == 0xFFFF) { run = DCT_ESC; level = 0; }
        else { run = (uint16_t)v >> 8; level = v & 0xFF; }
        uint32_t d = (w >> 9) ? t_dct1[w >> 6] : t_dct2[w & 0x1FF];
        int mlen = (int)(d >> 12), mrun = mlen ? (int)(d & 63) : -1, mlevel = mlen && mrun < DCT_EOB ? (int)(d >> 6 & 63) : 0;
        if (run >= DCT_EOB) level = 0;
        if (!(mlen == len && mrun == run && mlevel == level) && n++ < 5)
            printf("  DCT %04X: pl_mpeg run %d level %d (%d bits), here run %d level %d (%d bits)\n", w, run, level, len, mrun, mlevel, mlen);
    }
    printf("%-26s %5u patterns, %ld differ\n", "DCT coefficients", 1u << 16, n);
    g_bad += n;
    printf("RESULT: %s\n", g_bad ? "DIFFERENT" : "IDENTICAL");
    return g_bad ? 1 : 0;
}

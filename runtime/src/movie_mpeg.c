/* Native MPEG-1 video decoding for the Sofdec movies (movie.rel, CRI's MPV decoder).
 *
 * The recompiled MPV decoder is too slow for the attract movie (psogc_j.sfd fell to 11-16 fps on the
 * Switch with the game core 100% busy in its block and motion compensation code). Here the slice
 * decoder 0x8180E498 is replaced (config/GPOE8P/hle.txt, movie_slice): one call decodes one slice, from
 * its start code to the next start code. Everything around it stays the game's own code: the sequence,
 * GOP and picture headers, the reference frame choice, the start code search between slices
 * (0x8180E7E0, 0x8180E67C), frame skipping, ADX audio and sync.
 *
 * What one call does, as the game's code does it (movie.rel addresses; ctx = the MPV handle in r3):
 *   slice header (0x8180E498): vertical position -> ctx+0x29C (macroblock address - 1), quantiser
 *     scale -> +0x250, motion vector predictors (+0x264..0x270, +0x288..0x294) and DC predictors
 *     (+0x2AC..0x2B4 = 1024) reset, extra_bit_slice skipped, then the picture type's slice loop
 *     (ctx+0x22C: I 0x8180D4A4, P 0x8180D088, B 0x8180CA3C).
 *   per macroblock: address increment (stuffing, escape +33), type, quantiser, motion vectors (0x8180CEC4),
 *     coded block pattern; intra blocks (0x81819EB0) and coded non-intra blocks (0x81815954) give
 *     float coefficients: (float)dequantised * prescale[i] (ctx+0x34, 64 floats in the decoder's own
 *     coefficient order, the scan table at ctx+0x2C maps zig-zag to that order), the DC of an intra
 *     block (float)(pred) * 0.125; then the paired-single AAN IDCT (0x8180822C, 0x81807E94) rounded
 *     +-0.5 and saturated to s16, a DC-only block filled with its rounded DC.
 *   store (r12 = 0 set: ctx+0x180 == 3): intra 0x818152C0 (clamp to 0..255), forward 0x8181520C,
 *     backward 0x81815158, both 0x81815080 (half-pel prediction 0x81814ECC: integer averages rounding
 *     up; both directions (f + b + 1) >> 1), the residual added and clamped; skipped macroblocks
 *     (P: 0x81814528 copies from the forward reference into the frame at ctx+0x1E0, B: 0x81813F1C
 *     repeats the previous prediction). Every 8x8 store whose destination is 32-byte aligned first
 *     clears each row's cache line with dcbz, which is reproduced here (it zeroes bytes of the
 *     following blocks and of the stride padding exactly as the game does).
 * All arithmetic is reproduced exactly (the floating point steps in float, in the game's order,
 * without contraction), so the picture is bit for bit the one the recompiled decoder makes;
 * tools/tests/run_movie_mpeg_diff.sh checks that against the game's own code on the movies.
 *
 * Anything unexpected (another output mode, an unknown function in the decoder's tables, a bad
 * code, a macroblock address out of range, data that runs out) runs the game's own slice decoder
 * instead: the macroblocks decoded so far are rewritten identically by it. Logged once per reason. */
#include <stdint.h>
#include <string.h>

#include "runtime.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

void f_8180E498_orig(Cpu* c);  /* the game's slice decoder */
void f_8180E87C(Cpu* c);       /* movie.rel: skip to the next start code (*r3 ptr, *r4 len, *r5 consumed) */

/* ---- movie.rel addresses (fixed image at 0x81800000) ---- */
#define FN_SLICE_I    0x8180D4A4u
#define FN_SLICE_P    0x8180D088u
#define FN_SLICE_B    0x8180CA3Cu
#define FN_MB_INTRA   0x8180AE50u
#define FN_MB_RESID   0x8180ADA4u
#define FN_ST_INTRA   0x818157CCu
#define FN_SKIP_P     0x81814528u
#define FN_SKIP_B     0x81813F1Cu
#define FN_ST_FWD     0x8181520Cu
#define FN_ST_BWD     0x81815158u
#define FN_ST_BI      0x81815080u
#define IDCT_CONSTS   0x8183F648u  /* lbl_1_data_180: sqrt2, 2.613126, 1.0823922, 0.76536685, 0.5, -0.5 (pairs) */

/* ---- the decoder's state (offsets into the MPV handle) ---- */
#define C_PICTYPE   0x1A0
#define C_MBW       0x190
#define C_SCAN      0x02C
#define C_PRESCALE  0x034
#define C_MC_HALF   0x174  /* param 3: 0 = full half-pel prediction */
#define C_FWDREF    0x1D0  /* {Cb, Cr, Y, -, s16 stride C, s16 stride Y} */
#define C_BWDREF    0x1E0  /* P pictures: the current frame (the skipped-macroblock copy writes here) */
#define C_CUR       0x200
#define C_SLICEFN   0x22C
#define C_SKIPFN    0x230
#define C_MBINTRA   0x234
#define C_MBRESID   0x238
#define C_STINTRA   0x23C
#define C_LASTPRED  0x240
#define C_STBWD     0x244
#define C_STFWD     0x248
#define C_STBI      0x24C
#define C_QSCALE    0x250
#define C_FMV       0x254  /* full_pel, r_size, 27 - r_size, f, pred x, pred y, recon x, recon y */
#define C_BMV       0x278
#define C_MBADDR    0x29C
#define C_MBMAX     0x2A0
#define C_DCPRED    0x2AC  /* Y, Cb, Cr (value * 8) */
#define C_MBPOS     0x1268 /* per macroblock address: signed bytes row, column */
#define C_QINTRA    0xBE0  /* 64 bytes, in the decoder's coefficient order */
#define C_QNONINTRA 0xC20

/* ---- statistics, logging ---- */
static unsigned long long g_pics, g_slices, g_fallbacks, g_pic_ns_sum, g_pic_ns_max, g_pic_ns_cur;
static uint64_t g_report_t0;
static int g_last_vpos = 1 << 30;
static uint32_t g_announced_ctx, g_announced_size;
static unsigned g_seen_reasons;

static void fallback_log(int reason, const char* what, uint32_t a, uint32_t b) {
    if (reason < 32 && !(g_seen_reasons & (1u << reason))) {
        g_seen_reasons |= 1u << reason;
        rt_log("[movie] native MPEG-1 decode: %s (%08X %08X); this slice runs the game's own decoder", what, a, b);
    }
}

/* ---- VLC tables (ISO/IEC 11172-2 annex B), built into direct lookups at first use ---- */
typedef struct { const char* code; int value; } VlcDef;
/* lookup entry: value << 8 | length; length 0 = invalid code */
static uint16_t t_mbai[1 << 11];          /* value 1..33, 34 stuffing, 35 escape */
static uint8_t t_type_i[1 << 2][2], t_type_p[1 << 6][2], t_type_b[1 << 6][2];  /* [flags, length] */
static uint16_t t_cbp[1 << 9];
static uint16_t t_motion[1 << 11];        /* value + 16 */
static uint16_t t_dcy[1 << 7], t_dcc[1 << 8];
/* DCT coefficients: run (6 bits) | level << 6 (6 bits) | len << 12 (4 bits); special run 62 = EOB, 63 = escape */
static uint32_t t_dct1[1 << 10];          /* codes not starting with seven zeros, by their first 10 bits */
static uint32_t t_dct2[1 << 9];           /* codes starting with seven zeros, by the 9 bits after them */
#define DCT_EOB 62
#define DCT_ESC 63

static const VlcDef d_mbai[] = {
    {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"00011", 6}, {"00010", 7}, {"0000111", 8},
    {"0000110", 9}, {"00001011", 10}, {"00001010", 11}, {"00001001", 12}, {"00001000", 13}, {"00000111", 14},
    {"00000110", 15}, {"0000010111", 16}, {"0000010110", 17}, {"0000010101", 18}, {"0000010100", 19},
    {"0000010011", 20}, {"0000010010", 21}, {"00000100011", 22}, {"00000100010", 23}, {"00000100001", 24},
    {"00000100000", 25}, {"00000011111", 26}, {"00000011110", 27}, {"00000011101", 28}, {"00000011100", 29},
    {"00000011011", 30}, {"00000011010", 31}, {"00000011001", 32}, {"00000011000", 33},
    {"00000001111", 34}, {"00000001000", 35}, {0, 0}};
/* macroblock_type flags: 0x10 quant, 0x08 forward, 0x04 backward, 0x02 pattern, 0x01 intra */
static const VlcDef d_type_i[] = {{"1", 0x01}, {"01", 0x11}, {0, 0}};
static const VlcDef d_type_p[] = {{"1", 0x0A}, {"01", 0x02}, {"001", 0x08}, {"00011", 0x01}, {"00010", 0x1A},
                                  {"00001", 0x12}, {"000001", 0x11}, {0, 0}};
static const VlcDef d_type_b[] = {{"10", 0x0C}, {"11", 0x0E}, {"010", 0x04}, {"011", 0x06}, {"0010", 0x08},
                                  {"0011", 0x0A}, {"00011", 0x01}, {"00010", 0x1E}, {"000011", 0x1A},
                                  {"000010", 0x16}, {"000001", 0x11}, {0, 0}};
static const VlcDef d_cbp[] = {
    {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"10011", 12}, {"10010", 48},
    {"10001", 20}, {"10000", 40}, {"01111", 28}, {"01110", 44}, {"01101", 52}, {"01100", 56}, {"01011", 1},
    {"01010", 61}, {"01001", 2}, {"01000", 62}, {"001111", 24}, {"001110", 36}, {"001101", 3}, {"001100", 63},
    {"0010111", 5}, {"0010110", 9}, {"0010101", 17}, {"0010100", 33}, {"0010011", 6}, {"0010010", 10},
    {"0010001", 18}, {"0010000", 34}, {"00011111", 7}, {"00011110", 11}, {"00011101", 19}, {"00011100", 35},
    {"00011011", 13}, {"00011010", 49}, {"00011001", 21}, {"00011000", 41}, {"00010111", 14}, {"00010110", 50},
    {"00010101", 22}, {"00010100", 42}, {"00010011", 15}, {"00010010", 51}, {"00010001", 23}, {"00010000", 43},
    {"00001111", 25}, {"00001110", 37}, {"00001101", 26}, {"00001100", 38}, {"00001011", 29}, {"00001010", 45},
    {"00001001", 53}, {"00001000", 57}, {"00000111", 30}, {"00000110", 46}, {"00000101", 54}, {"00000100", 58},
    {"000000111", 31}, {"000000110", 47}, {"000000101", 55}, {"000000100", 59}, {"000000011", 27},
    {"000000010", 39}, {0, 0}};
/* motion_code magnitudes; the sign bit follows (1 = negative) */
static const VlcDef d_motion[] = {
    {"01", 1}, {"001", 2}, {"0001", 3}, {"000011", 4}, {"0000101", 5}, {"0000100", 6}, {"0000011", 7},
    {"000001011", 8}, {"000001010", 9}, {"000001001", 10}, {"0000010001", 11}, {"0000010000", 12},
    {"0000001111", 13}, {"0000001110", 14}, {"0000001101", 15}, {"0000001100", 16}, {0, 0}};
static const VlcDef d_dcy[] = {{"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5},
                               {"11110", 6}, {"111110", 7}, {"1111110", 8}, {0, 0}};
static const VlcDef d_dcc[] = {{"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"11110", 5},
                               {"111110", 6}, {"1111110", 7}, {"11111110", 8}, {0, 0}};
/* dct_coeff_next: code without its sign bit, run, level */
static const struct { const char* code; int run, level; } d_dct[] = {
    {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
    {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1},
    {"0000110", 0, 4}, {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1}, {"00100110", 0, 5},
    {"00100001", 0, 6}, {"00100101", 1, 3}, {"00100100", 3, 2}, {"00100111", 10, 1}, {"00100011", 11, 1},
    {"00100010", 12, 1}, {"00100000", 13, 1}, {"0000001010", 0, 7}, {"0000001100", 1, 4},
    {"0000001011", 2, 3}, {"0000001111", 4, 2}, {"0000001001", 5, 2}, {"0000001110", 14, 1},
    {"0000001101", 15, 1}, {"0000001000", 16, 1}, {"000000011101", 0, 8}, {"000000011000", 0, 9},
    {"000000010011", 0, 10}, {"000000010000", 0, 11}, {"000000011011", 1, 5}, {"000000010100", 2, 4},
    {"000000011100", 3, 3}, {"000000010010", 4, 3}, {"000000011110", 6, 2}, {"000000010101", 7, 2},
    {"000000010001", 8, 2}, {"000000011111", 17, 1}, {"000000011010", 18, 1}, {"000000011001", 19, 1},
    {"000000010111", 20, 1}, {"000000010110", 21, 1}, {"0000000011010", 0, 12}, {"0000000011001", 0, 13},
    {"0000000011000", 0, 14}, {"0000000010111", 0, 15}, {"0000000010110", 1, 6}, {"0000000010101", 1, 7},
    {"0000000010100", 2, 5}, {"0000000010011", 3, 4}, {"0000000010010", 5, 3}, {"0000000010001", 9, 2},
    {"0000000010000", 10, 2}, {"0000000011111", 22, 1}, {"0000000011110", 23, 1}, {"0000000011101", 24, 1},
    {"0000000011100", 25, 1}, {"0000000011011", 26, 1}, {"00000000011111", 0, 16}, {"00000000011110", 0, 17},
    {"00000000011101", 0, 18}, {"00000000011100", 0, 19}, {"00000000011011", 0, 20}, {"00000000011010", 0, 21},
    {"00000000011001", 0, 22}, {"00000000011000", 0, 23}, {"00000000010111", 0, 24}, {"00000000010110", 0, 25},
    {"00000000010101", 0, 26}, {"00000000010100", 0, 27}, {"00000000010011", 0, 28}, {"00000000010010", 0, 29},
    {"00000000010001", 0, 30}, {"00000000010000", 0, 31}, {"000000000011000", 0, 32},
    {"000000000010111", 0, 33}, {"000000000010110", 0, 34}, {"000000000010101", 0, 35},
    {"000000000010100", 0, 36}, {"000000000010011", 0, 37}, {"000000000010010", 0, 38},
    {"000000000010001", 0, 39}, {"000000000010000", 0, 40}, {"000000000011111", 1, 8},
    {"000000000011110", 1, 9}, {"000000000011101", 1, 10}, {"000000000011100", 1, 11},
    {"000000000011011", 1, 12}, {"000000000011010", 1, 13}, {"000000000011001", 1, 14},
    {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17},
    {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2},
    {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2}, {"0000000000010111", 14, 2},
    {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1},
    {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1},
    {"0000000000011011", 31, 1}, {0, 0, 0}};

static unsigned code_bits(const char* s, int* len) {
    unsigned v = 0;
    int n = 0;
    for (; *s; s++, n++) v = v << 1 | (unsigned)(*s - '0');
    *len = n;
    return v;
}
/* fill every index of a `bits`-wide table whose top bits are the code */
static void fill16(uint16_t* t, int bits, const VlcDef* d) {
    for (; d->code; d++) {
        int len;
        unsigned v = code_bits(d->code, &len), sh = (unsigned)(bits - len);
        for (unsigned i = 0; i < (1u << sh); i++) t[(v << sh) | i] = (uint16_t)(d->value << 8 | len);
    }
}
static void fill_type(uint8_t (*t)[2], int bits, const VlcDef* d) {
    for (; d->code; d++) {
        int len;
        unsigned v = code_bits(d->code, &len), sh = (unsigned)(bits - len);
        for (unsigned i = 0; i < (1u << sh); i++) { t[(v << sh) | i][0] = (uint8_t)d->value; t[(v << sh) | i][1] = (uint8_t)len; }
    }
}
static void dct_put(unsigned v, int len, uint32_t e) {
    /* v: code of length len (<= 16), placed at the top of a 16-bit window */
    unsigned w = v << (16 - len);
    if (w >> 9) {  /* not seven leading zeros: decided by the first 10 bits (len <= 10) */
        unsigned top = w >> 6, n = 1u << (10 - len);
        for (unsigned i = 0; i < n; i++) t_dct1[top + i] = e;
    } else {
        unsigned low = w & 0x1FF, n = 1u << (16 - len);
        for (unsigned i = 0; i < n; i++) t_dct2[low + i] = e;
    }
}
static int g_tables_ready;
static void build_tables(void) {
    fill16(t_mbai, 11, d_mbai);
    fill_type(t_type_i, 2, d_type_i);
    fill_type(t_type_p, 6, d_type_p);
    fill_type(t_type_b, 6, d_type_b);
    fill16(t_cbp, 9, d_cbp);
    for (const VlcDef* d = d_motion; d->code; d++) {  /* with the sign bit: 1 = negative */
        int len;
        unsigned v = code_bits(d->code, &len);
        for (int s = 0; s < 2; s++) {
            unsigned code = v << 1 | (unsigned)s, sh = (unsigned)(11 - len - 1);
            int val = (s ? -d->value : d->value) + 16;
            for (unsigned i = 0; i < (1u << sh); i++) t_motion[(code << sh) | i] = (uint16_t)(val << 8 | (len + 1));
        }
    }
    for (unsigned i = 1u << 10; i < (1u << 11); i++) t_motion[i] = (uint16_t)(16 << 8 | 1);  /* "1": 0 */
    fill16(t_dcy, 7, d_dcy);
    fill16(t_dcc, 8, d_dcc);
    for (int i = 0; d_dct[i].code; i++) {
        int len;
        unsigned v = code_bits(d_dct[i].code, &len);
        dct_put(v, len, (uint32_t)d_dct[i].run | (uint32_t)d_dct[i].level << 6 | (uint32_t)len << 12);
    }
    dct_put(2, 2, DCT_EOB | 2u << 12);   /* "10" */
    dct_put(1, 6, DCT_ESC | 6u << 12);   /* "000001" */
    g_tables_ready = 1;
}

/* ---- bit reader over guest memory: bit position from the word-aligned start A ---- */
typedef struct {
    const uint8_t* base;  /* host pointer of A */
    uint32_t pos;         /* bits consumed from A */
    uint32_t limit;       /* bits that may be read (the slice data plus the bytes after it the game reads too) */
    int bad;              /* read past the limit */
} Bits;
static inline uint32_t peek32(Bits* b) {
    if (b->pos > b->limit) return 0;  /* past the data: zeros, which no code accepts */
    uint32_t byte = b->pos >> 3;
    uint64_t w;
    memcpy(&w, b->base + byte, 8);
    w = __builtin_bswap64(w);
    return (uint32_t)((w << (b->pos & 7)) >> 32);
}
static inline uint32_t peek(Bits* b, int n) { return peek32(b) >> (32 - n); }
static inline void skip(Bits* b, int n) {
    b->pos += (uint32_t)n;
    if (b->pos > b->limit) b->bad = 1;
}
static inline uint32_t get(Bits* b, int n) {
    uint32_t v = peek(b, n);
    skip(b, n);
    return v;
}

/* ---- the slice's environment, read from the decoder state ---- */
typedef struct {
    uint8_t *y, *cb, *cr;
    uint32_t yea, cbea, crea;  /* guest addresses */
    int sy, sc;
} Planes;
typedef struct {
    int full_pel, r_size, f;
    int pred[2], recon[2];  /* x, y */
} MvState;
typedef struct {
    uint8_t* ctx;  /* host pointer of the MPV handle */
    uint32_t ctx_ea;
    int pictype, mbw, mbmax;
    int8_t scan[64];
    float prescale[64];
    uint8_t qintra[64], qnon[64];
    Planes cur, fwd, bwd;
    MvState mv[2];
    int qscale;
    int dcpred[3];
    int lastpred;  /* B: 1 forward, 2 backward, 3 both */
    const uint8_t* mbpos;
} Slice;

static inline uint32_t be32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline void wbe32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline int16_t be16s(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return (int16_t)__builtin_bswap16(v); }
static inline float bef(const uint8_t* p) { uint32_t v = be32(p); float f; memcpy(&f, &v, 4); return f; }

/* ---- IDCT: the game's paired-single AAN transform (0x8180822C / 0x81807E94), lane for lane ---- */
typedef float v2f __attribute__((vector_size(8)));
#define V2(a, b) ((v2f){(a), (b)})
/* the game's constants, by their bits (lbl_1_data_180, checked against guest memory at every slice) */
static const uint32_t K_BITS[12] = {0x3FB504F3, 0x3FB504F3, 0x40273D75, 0x40273D75, 0x3F8A8BD4, 0x3F8A8BD4,
                                    0x3F43EF15, 0x3F43EF15, 0x3F000000, 0x3F000000, 0xBF000000, 0xBF000000};

static inline int16_t sat16(float v) {  /* psq_st, GQR type s16, scale 0: clamp, then truncate */
    v = v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v;
    return (int16_t)(int32_t)v;
}
static inline float round_half(float v) { return v + (v >= 0.0f ? 0.5f : -0.5f); }  /* ps_sel + ps_add */

static inline float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static void idct(const float* c, int16_t* out) {
    const float k31 = fbits(K_BITS[0]), k13 = fbits(K_BITS[2]), k12 = fbits(K_BITS[4]), k11 = fbits(K_BITS[6]);
    const v2f c31 = V2(k31, k31), c13 = V2(k13, k13), c12 = V2(k12, k12), c11 = V2(k11, k11);
    float t[64];
    for (int i = 0; i < 4; i++) {
        const float* p = c + 16 * i;
        float* o = t + 16 * i;
        int nz = 0;
        for (int k = 0; k < 16; k++) nz |= p[k] != 0.0f;
        if (!nz) {  /* the transform of zeros is zero (signs of zero do not reach the result) */
            memset(o, 0, 16 * sizeof(float));
            continue;
        }
        v2f a8 = V2(p[0], p[1]), a7 = V2(p[2], p[3]), a6 = V2(p[4], p[5]), a5 = V2(p[6], p[7]);
        v2f a4 = V2(p[8], p[9]), a3 = V2(p[10], p[11]), a2 = V2(p[12], p[13]), a1 = V2(p[14], p[15]);
        v2f f0 = a6 - a2;
        v2f f2 = a6 + a2;
        f0 = f0 * c31;
        v2f f6 = a8 - a4;
        f0 = f0 - f2;
        v2f f4 = a8 + a4;
        v2f f8 = f4 - f2;
        f2 = f4 + f2;
        f4 = f6 - f0;
        f0 = f6 + f0;
        f6 = a3 - a5;
        v2f f5 = a3 + a5;
        v2f f3 = a7 - a1;
        v2f f1 = a7 + a1;
        v2f f7 = f1 - f5;
        f5 = f1 + f5;
        f1 = f6 - f3;
        f1 = f1 * c11;
        f3 = f3 * c12;
        f3 = f3 - f1;
        f6 = f6 * c13;
        f3 = f3 - f5;
        f6 = f6 - f1;
        f7 = f7 * c31;
        f1 = f2 - f5;
        f7 = f7 - f3;
        f5 = f2 + f5;
        f2 = f0 - f3;
        f6 = f6 - f7;
        f3 = f0 + f3;
        f0 = f8 - f6;
        f6 = f8 + f6;
        f8 = f4 - f7;
        f7 = f4 + f7;
        o[0] = f5[0]; o[1] = f3[0]; o[2] = f7[0]; o[3] = f6[0]; o[4] = f0[0]; o[5] = f8[0]; o[6] = f2[0]; o[7] = f1[0];
        o[8] = f5[1]; o[9] = f3[1]; o[10] = f7[1]; o[11] = f6[1]; o[12] = f0[1]; o[13] = f8[1]; o[14] = f2[1]; o[15] = f1[1];
    }
    for (int j = 0; j < 4; j++) {
#define TP(k) V2(t[2 * (j + 4 * (k))], t[2 * (j + 4 * (k)) + 1])
        v2f b8 = TP(0), b3 = TP(1), b2 = TP(2), b5 = TP(3), b0 = TP(4), b6 = TP(5), b1 = TP(6), b7 = TP(7);
#undef TP
        v2f f4 = b2 - b1;
        v2f f1 = b2 + b1;
        f4 = f4 * c31;
        v2f f2 = b8 - b0;
        f4 = f4 - f1;
        v2f f0 = b8 + b0;
        v2f f8 = f0 - f1;
        f1 = f0 + f1;
        f0 = f2 - f4;
        f4 = f2 + f4;
        f2 = b6 - b5;
        v2f f5 = b6 + b5;
        v2f f6 = b3 - b7;
        v2f f7 = b3 + b7;
        v2f f3 = f7 - f5;
        f5 = f7 + f5;
        f7 = f2 - f6;
        f7 = f7 * c11;
        f6 = f6 * c12;
        f6 = f6 - f7;
        f2 = f2 * c13;
        f6 = f6 - f5;
        f2 = f2 - f7;
        f3 = f3 * c31;
        f7 = f1 - f5;
        f3 = f3 - f6;
        f5 = f1 + f5;
        f1 = f4 - f6;
        f2 = f2 - f3;
        f6 = f4 + f6;
        f4 = f8 - f2;
        f2 = f8 + f2;
        f8 = f0 - f3;
        f3 = f0 + f3;
        const v2f row[8] = {f5, f6, f3, f2, f4, f8, f1, f7};
        for (int k = 0; k < 8; k++) {
            out[8 * k + 2 * j] = sat16(round_half(row[k][0]));
            out[8 * k + 2 * j + 1] = sat16(round_half(row[k][1]));
        }
    }
}
#ifdef MOVIE_MPEG_TEST
void movie_mpeg_test_idct(const float* c, int16_t* out) { idct(c, out); }  /* tools/tests/movie_mpeg_diff.c */
#endif
/* a block with only its DC: fctiwz of DC +- 0.5, the low 16 bits */
static void idct_dc(float dc, int16_t* out) {
    float v = dc >= 0.0f ? 0.5f + dc : dc - 0.5f;
    int32_t i = v >= 2147483647.0f ? 0x7FFFFFFF : v <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)v;
    int16_t s = (int16_t)(uint16_t)(uint32_t)i;
    for (int k = 0; k < 64; k++) out[k] = s;
}

/* ---- blocks ---- */
enum { ERR_NONE, ERR_VLC, ERR_POS, ERR_DATA, ERR_ADDR, ERR_MV, ERR_TYPE };
static inline float i2f(int v) { return (float)v; }

/* intra block (0x81819EB0): DC from its predictor, AC to EOB. Returns ERR_*, *ac = an AC was decoded */
static int block_intra(Slice* s, Bits* b, int comp, float* coef, int* ac) {
    uint32_t w = peek(b, 8);
    uint16_t e = comp == 0 ? t_dcy[w >> 1] : t_dcc[w];
    if (!(e & 0xFF)) return ERR_VLC;
    int size = e >> 8;
    skip(b, e & 0xFF);
    int diff = 0;
    if (size) {
        diff = (int)get(b, size);
        if (!(diff & (1 << (size - 1)))) diff += 1 - (1 << size);
    }
    int* pred = &s->dcpred[comp];
    *pred += diff * 8;
    memset(coef, 0, 64 * sizeof(float));
    coef[0] = i2f(*pred) * 0.125f;
    const int qs2 = s->qscale * 2;
    int pos = 0, last = 0;
    for (;;) {
        uint32_t v = peek(b, 16);
        uint32_t d = (v >> 9) ? t_dct1[v >> 6] : t_dct2[v & 0x1FF];
        int len = (int)(d >> 12), run = (int)(d & 63), level, neg;
        if (!len) return ERR_VLC;
        if (run == DCT_EOB) { skip(b, len); break; }
        if (run == DCT_ESC) {
            skip(b, 6);
            run = (int)get(b, 6);
            level = (int8_t)get(b, 8);
            if (!(level & 0x7F)) level = level * 2 | (int)get(b, 8);
        } else {
            skip(b, len);
            level = (int)(d >> 6 & 63);
            if (get(b, 1)) level = -level;
        }
        neg = level < 0;
        if (neg) level = -level;
        pos += run + 1;
        if (pos > 63) return ERR_POS;
        int idx = s->scan[pos];
        int val = (((level * qs2 * s->qintra[idx]) >> 4) - 1) | 1;
        if (neg) val = -val;
        coef[idx] = i2f(val) * s->prescale[idx];
        last = idx;
    }
    *ac = last != 0;
    return b->bad ? ERR_DATA : ERR_NONE;
}
/* coded non-intra block (0x81815954). *full = not just one coefficient at position 0 */
static int block_nonintra(Slice* s, Bits* b, float* coef, int* full) {
    memset(coef, 0, 64 * sizeof(float));
    int pos = -1, first = -1, last = 0;
    for (;;) {
        uint32_t v = peek(b, 16);
        int run, level, len;
        if (pos < 0 && (v >> 15)) {  /* first coefficient "1s": run 0, level 1 */
            run = 0;
            level = (v >> 14) & 1 ? -1 : 1;
            skip(b, 2);
        } else {
            uint32_t d = (v >> 9) ? t_dct1[v >> 6] : t_dct2[v & 0x1FF];
            len = (int)(d >> 12);
            run = (int)(d & 63);
            if (!len) return ERR_VLC;
            if (run == DCT_EOB) {
                if (pos < 0) return ERR_VLC;
                skip(b, len);
                break;
            }
            if (run == DCT_ESC) {
                skip(b, 6);
                run = (int)get(b, 6);
                level = (int8_t)get(b, 8);
                if (!(level & 0x7F)) level = level * 2 | (int)get(b, 8);
            } else {
                skip(b, len);
                level = (int)(d >> 6 & 63);
                if (get(b, 1)) level = -level;
            }
        }
        int neg = level < 0;
        if (neg) level = -level;
        pos += run + 1;
        if (pos > 63) return ERR_POS;
        int idx = s->scan[pos];
        int val = ((((2 * level + 1) * s->qscale * s->qnon[idx]) >> 4) - 1) | 1;
        if (neg) val = -val;
        coef[idx] = i2f(val) * s->prescale[idx];
        if (first < 0) first = idx;
        last = idx;
    }
    *full = (last == first ? last : -last) != 0;
    return b->bad ? ERR_DATA : ERR_NONE;
}

/* ---- stores (dcbz: a destination row on a 32-byte boundary gets its cache line zeroed first) ---- */
#define INLINE static inline __attribute__((always_inline))
INLINE void zero_line(uint8_t* p) { memset((uint8_t*)((uintptr_t)p & ~(uintptr_t)31), 0, 32); }
INLINE uint8_t clamp8(int v) { v = v < 0 ? 0 : v; return (uint8_t)(v > 255 ? 255 : v); }

/* one 8-pixel row each; plain loops the compiler vectorises */
INLINE void row_res(uint8_t* restrict d, const int16_t* restrict r) {
    for (int x = 0; x < 8; x++) d[x] = clamp8(r[x]);
}
INLINE void row_add(uint8_t* restrict d, const uint8_t* restrict p, const int16_t* restrict r) {
    for (int x = 0; x < 8; x++) d[x] = clamp8(p[x] + r[x]);
}
INLINE void row_avg(uint8_t* restrict d, const uint8_t* restrict f, const uint8_t* restrict g) {
    for (int x = 0; x < 8; x++) d[x] = (uint8_t)((f[x] + g[x] + 1) >> 1);
}
INLINE void row_avg_add(uint8_t* restrict d, const uint8_t* restrict f, const uint8_t* restrict g, const int16_t* restrict r) {
    for (int x = 0; x < 8; x++) d[x] = clamp8(((f[x] + g[x] + 1) >> 1) + r[x]);
}

static void store_intra(uint8_t* d, int stride, const int16_t* r) {
    int z = !((uintptr_t)d & 31);
    for (int y = 0; y < 8; y++, d += stride, r += 8) {
        if (z) zero_line(d);
        row_res(d, r);
    }
}
static void store_pred(uint8_t* d, int stride, const uint8_t* p, int ps, const int16_t* r) {
    int z = !((uintptr_t)d & 31);
    for (int y = 0; y < 8; y++, d += stride, p += ps) {
        if (z) zero_line(d);
        if (r) {
            row_add(d, p, r);
            r += 8;
        } else {
            memcpy(d, p, 8);
        }
    }
}
static void store_bi(uint8_t* d, int stride, const uint8_t* f, const uint8_t* g, int ps, const int16_t* r) {
    int z = !((uintptr_t)d & 31);
    for (int y = 0; y < 8; y++, d += stride, f += ps, g += ps) {
        if (z) zero_line(d);
        if (r) {
            row_avg_add(d, f, g, r);
            r += 8;
        } else {
            row_avg(d, f, g);
        }
    }
}

/* half-pel prediction of a w x h area into dst (stride w); w and h are constants where it is inlined */
INLINE void predict(uint8_t* restrict dst, const uint8_t* restrict src, int stride, int w, int h, int hx, int hy) {
    if (!hx && !hy) {
        for (int y = 0; y < h; y++, src += stride, dst += w) memcpy(dst, src, (size_t)w);
    } else if (hx && !hy) {
        for (int y = 0; y < h; y++, src += stride, dst += w)
            for (int x = 0; x < w; x++) dst[x] = (uint8_t)((src[x] + src[x + 1] + 1) >> 1);
    } else if (!hx) {
        for (int y = 0; y < h; y++, src += stride, dst += w)
            for (int x = 0; x < w; x++) dst[x] = (uint8_t)((src[x] + src[x + stride] + 1) >> 1);
    } else {
        for (int y = 0; y < h; y++, src += stride, dst += w)
            for (int x = 0; x < w; x++)
                dst[x] = (uint8_t)((src[x] + src[x + 1] + src[x + stride] + src[x + stride + 1] + 2) >> 2);
    }
}
static void predict16(uint8_t* dst, const uint8_t* src, int stride, int hx, int hy) { predict(dst, src, stride, 16, 16, hx, hy); }
static void predict8(uint8_t* dst, const uint8_t* src, int stride, int hx, int hy) { predict(dst, src, stride, 8, 8, hx, hy); }
typedef struct { uint8_t y[256], cb[64], cr[64]; } Pred;
/* the area a prediction reads lies in game memory (the game reads wherever a vector points: a vector
 * outside the picture reads the neighbouring memory, as here, but never outside RAM) */
static int src_ok(uint32_t base, int off, int stride, int w, int h) {
    int64_t first = (int64_t)base + off, len = (int64_t)(h - 1) * stride + w;
    return first >= 0 && first <= 0xFFFFFFFFll && guest_span_ok((uint32_t)first, (uint32_t)len);
}
/* 0x81814ECC: the prediction for the macroblock at (row, col) from ref with motion vector mv (half-pels) */
static int predict_mb(Pred* p, const Planes* ref, int row, int col, int mvx, int mvy) {
    int yoff = row * 16 * ref->sy + col * 16, coff = row * 8 * ref->sc + col * 8;
    int yo = yoff + (mvy >> 1) * ref->sy + (mvx >> 1);
    int cx = mvx / 2, cy = mvy / 2;  /* truncating, as srawi + addze */
    int co = coff + (cy >> 1) * ref->sc + (cx >> 1);
    if (!src_ok(ref->yea, yo, ref->sy, 17, 17) || !src_ok(ref->cbea, co, ref->sc, 9, 9) || !src_ok(ref->crea, co, ref->sc, 9, 9))
        return 0;
    predict16(p->y, ref->y + yo, ref->sy, mvx & 1, mvy & 1);
    predict8(p->cb, ref->cb + co, ref->sc, cx & 1, cy & 1);
    predict8(p->cr, ref->cr + co, ref->sc, cx & 1, cy & 1);
    return 1;
}

/* store a predicted macroblock: order Cb, Cr, Y0, Y1, Y2, Y3 as the game's store loop; res[b] or NULL */
static void store_mb_pred(const Slice* s, int row, int col, const Pred* f, const Pred* g, int16_t (*res)[64], const int* coded) {
    const Planes* c = &s->cur;
    /* the destination offsets come from the reference's strides (0x81814ECC), equal by the entry check */
    int yo = row * 16 * c->sy + col * 16, co = row * 8 * c->sc + col * 8;
    uint8_t* dst[6] = {c->cb + co, c->cr + co, c->y + yo, c->y + yo + 8, c->y + yo + 8 * c->sy, c->y + yo + 8 * c->sy + 8};
    static const int blk[6] = {4, 5, 0, 1, 2, 3};
    for (int i = 0; i < 6; i++) {
        int bk = blk[i];
        const int16_t* r = coded[bk] ? res[bk] : NULL;
        int stride = i < 2 ? c->sc : c->sy, ps;
        const uint8_t *fp, *gp = NULL;
        if (i == 0) { fp = f->cb; if (g) gp = g->cb; ps = 8; }
        else if (i == 1) { fp = f->cr; if (g) gp = g->cr; ps = 8; }
        else {
            int ox = (bk & 1) * 8, oy = (bk >> 1) * 8 * 16;
            fp = f->y + oy + ox;
            if (g) gp = g->y + oy + ox;
            ps = 16;
        }
        if (g) store_bi(dst[i], stride, fp, gp, ps, r);
        else store_pred(dst[i], stride, fp, ps, r);
    }
}

/* P skipped macroblock (0x81813F9C): copy from the forward reference into the frame at ctx+0x1E0, the dcbz
 * condition on the offsets */
static void copy_skip(const Slice* s, int row, int col) {
    const Planes *src = &s->fwd, *dst = &s->bwd;
    int coff = row * 8 * src->sc + col * 8, yoff = row * 16 * src->sy + col * 16;
    int zc = !(coff & 31), zy = !(yoff & 31);
    for (int y = 0; y < 8; y++) {
        uint8_t* d = dst->cb + coff + y * dst->sc;
        if (zc) zero_line(d);
        memcpy(d, src->cb + coff + y * dst->sc, 8);
    }
    for (int y = 0; y < 8; y++) {
        uint8_t* d = dst->cr + coff + y * dst->sc;
        if (zc) zero_line(d);
        memcpy(d, src->cr + coff + y * dst->sc, 8);
    }
    for (int y = 0; y < 16; y++) {
        uint8_t* d = dst->y + yoff + y * dst->sy;
        if (zy) zero_line(d);
        memcpy(d, src->y + yoff + y * dst->sy, 16);
    }
}

/* ---- motion vectors (0x8180CEC4) ---- */
static int motion(Bits* b, MvState* m, int k) {
    uint16_t e = t_motion[peek(b, 11)];
    if (!(e & 0xFF)) return ERR_MV;
    skip(b, e & 0xFF);
    int code = (e >> 8) - 16;
    if (code == 0) {
        m->recon[k] = m->pred[k];
    } else {
        int delta = code;
        if (m->r_size) {
            int r = (int)get(b, m->r_size);
            delta = code > 0 ? (code << m->r_size) - (m->f - 1 - r) : (code << m->r_size) + (m->f - 1 - r);
        }
        int v = m->pred[k] + delta;
        int sh = 27 - m->r_size;
        v = (int)((uint32_t)v << sh) >> sh;
        m->recon[k] = v;
        m->pred[k] = v;
    }
    if (m->full_pel) m->recon[k] <<= 1;
    return ERR_NONE;
}

/* ---- one slice ---- */
static int mb_position(const Slice* s, int addr, int* row, int* col) {
    if (addr < 0 || addr > s->mbmax) return 0;
    *row = (int8_t)s->mbpos[2 * addr];
    *col = (int8_t)s->mbpos[2 * addr + 1];
    return 1;
}
static int mb_inside(const Slice* s, int row, int col) {
    return row >= 0 && col >= 0 && col < s->mbw && (row + 1) * s->mbw <= s->mbmax + 1;
}

static int decode_intra_mb(Slice* s, Bits* b, int row, int col) {
    float coef[64];
    int16_t res[6][64];
    for (int k = 0; k < 6; k++) {
        int ac = 0, comp = k < 4 ? 0 : k - 3;
        int e = block_intra(s, b, comp, coef, &ac);
        if (e) return e;
        if (ac) idct(coef, res[k]);
        else idct_dc(coef[0], res[k]);
    }
    const Planes* c = &s->cur;
    int yo = row * 16 * c->sy + col * 16, co = row * 8 * c->sc + col * 8;
    store_intra(c->cb + co, c->sc, res[4]);
    store_intra(c->cr + co, c->sc, res[5]);
    store_intra(c->y + yo, c->sy, res[0]);
    store_intra(c->y + yo + 8, c->sy, res[1]);
    store_intra(c->y + yo + 8 * c->sy, c->sy, res[2]);
    store_intra(c->y + yo + 8 * c->sy + 8, c->sy, res[3]);
    return ERR_NONE;
}
static int decode_resid(Slice* s, Bits* b, int cbp, int16_t (*res)[64], int* coded) {
    float coef[64];
    for (int k = 0; k < 6; k++) {
        coded[k] = (cbp >> (5 - k)) & 1;
        if (!coded[k]) continue;
        int full = 0;
        int e = block_nonintra(s, b, coef, &full);
        if (e) return e;
        if (full) idct(coef, res[k]);
        else idct_dc(coef[0], res[k]);
    }
    return ERR_NONE;
}
/* the prediction kinds of B (and P: forward) macroblocks: 1 forward, 2 backward, 3 both */
static int predicted_mb(Slice* s, int kind, int row, int col, int16_t (*res)[64], const int* coded) {
    Pred f, g;
    if (kind == 1) {
        if (!predict_mb(&f, &s->fwd, row, col, s->mv[0].recon[0], s->mv[0].recon[1])) return ERR_MV;
        store_mb_pred(s, row, col, &f, NULL, res, coded);
    } else if (kind == 2) {
        if (!predict_mb(&f, &s->bwd, row, col, s->mv[1].recon[0], s->mv[1].recon[1])) return ERR_MV;
        store_mb_pred(s, row, col, &f, NULL, res, coded);
    } else {
        if (!predict_mb(&f, &s->fwd, row, col, s->mv[0].recon[0], s->mv[0].recon[1]) ||
            !predict_mb(&g, &s->bwd, row, col, s->mv[1].recon[0], s->mv[1].recon[1]))
            return ERR_MV;
        store_mb_pred(s, row, col, &f, &g, res, coded);
    }
    return ERR_NONE;
}

static int decode_slice(Cpu* c, Slice* s, Bits* b, int* addr_io) {
    int addr = *addr_io;
    int first = 1;
    for (;;) {
        /* pending interrupts (audio, retrace, disc) are delivered between macroblocks, as the game's code
         * delivers them at its function entries: the decoder's state is in host variables, guest memory
         * and c->r are as at the call */
        if (__builtin_expect(g_core_preempt[0], 0)) ppc_preempt(c);
        if (peek(b, 23) == 0) break;  /* next start code: end of slice */
        int inc = 0;
        for (;;) {
            uint16_t e = t_mbai[peek(b, 11)];
            if (!(e & 0xFF)) return ERR_VLC;
            skip(b, e & 0xFF);
            int v = e >> 8;
            if (v == 34) continue;
            if (v == 35) { inc += 33; continue; }
            inc += v;
            break;
        }
        int prev = addr;
        addr += inc;
        if (addr > s->mbmax) return ERR_ADDR;
        int row, col;
        /* skipped macroblocks between the previous one and this one (not before the first) */
        if (!first && inc > 1) {
            for (int a = prev + 1; a < addr; a++) {
                if (!mb_position(s, a, &row, &col) || !mb_inside(s, row, col)) return ERR_ADDR;
                if (s->pictype == 2) {
                    copy_skip(s, row, col);
                } else if (s->pictype == 3) {
                    int coded[6] = {0};
                    int e = predicted_mb(s, s->lastpred, row, col, NULL, coded);
                    if (e) return e;
                }
            }
            if (s->pictype == 2) {
                s->mv[0].pred[0] = s->mv[0].pred[1] = s->mv[0].recon[0] = s->mv[0].recon[1] = 0;
            }
            if (s->pictype != 1) s->dcpred[0] = s->dcpred[1] = s->dcpred[2] = 1024;
        }
        first = 0;
        if (!mb_position(s, addr, &row, &col) || !mb_inside(s, row, col)) return ERR_ADDR;
        int type;
        if (s->pictype == 1) {
            const uint8_t* t = t_type_i[peek(b, 2)];
            if (!t[1]) return ERR_TYPE;
            type = t[0]; skip(b, t[1]);
        } else {
            const uint8_t* t = (s->pictype == 2 ? t_type_p : t_type_b)[peek(b, 6)];
            if (!t[1]) return ERR_TYPE;
            type = t[0]; skip(b, t[1]);
        }
        if (type & 0x10) s->qscale = (int)get(b, 5);
        if (type & 0x08) {
            if (motion(b, &s->mv[0], 0) || motion(b, &s->mv[0], 1)) return ERR_MV;
        } else if (s->pictype == 2) {
            s->mv[0].pred[0] = s->mv[0].pred[1] = s->mv[0].recon[0] = s->mv[0].recon[1] = 0;
        }
        if (type & 0x04) {
            if (motion(b, &s->mv[1], 0) || motion(b, &s->mv[1], 1)) return ERR_MV;
        }
        int cbp = 0;
        if (type & 0x02) {
            uint16_t e = t_cbp[peek(b, 9)];
            if (!(e & 0xFF)) return ERR_VLC;
            skip(b, e & 0xFF);
            cbp = e >> 8;
        }
        if (b->bad) return ERR_DATA;
        if (type & 0x01) {
            int e = decode_intra_mb(s, b, row, col);
            if (e) return e;
            if (s->pictype == 3) {
                s->mv[0].pred[0] = s->mv[0].pred[1] = s->mv[0].recon[0] = s->mv[0].recon[1] = 0;
                s->mv[1].pred[0] = s->mv[1].pred[1] = s->mv[1].recon[0] = s->mv[1].recon[1] = 0;
            }
        } else {
            int16_t res[6][64];
            int coded[6] = {0};
            if (cbp) {
                int e = decode_resid(s, b, cbp, res, coded);
                if (e) return e;
            }
            int kind = s->pictype == 2 ? 1 : (type >> 2 & 3) == 2 ? 1 : (type >> 2 & 3) == 1 ? 2 : (type >> 2 & 3) == 3 ? 3 : s->lastpred;
            if (s->pictype == 3) s->lastpred = kind;
            int e = predicted_mb(s, kind, row, col, res, coded);
            if (e) return e;
            s->dcpred[0] = s->dcpred[1] = s->dcpred[2] = 1024;
        }
        if (b->bad) return ERR_DATA;
        *addr_io = addr;
    }
    *addr_io = addr;
    return b->bad ? ERR_DATA : ERR_NONE;
}

/* ---- entry: 0x8180E498 (r3 ctx, r4 &data pointer, r5 &bytes left, r6 &bytes consumed) ---- */
static int read_planes(const uint8_t* ctx, int off, Planes* p, int mbh, uint32_t* bad) {
    uint32_t cb = be32(ctx + off), cr = be32(ctx + off + 4), y = be32(ctx + off + 8);
    int sc = be16s(ctx + off + 0xC), sy = be16s(ctx + off + 0xE);
    if (sc <= 0 || sy <= 0 || sc > 4096 || sy > 4096) { *bad = (uint32_t)sc << 16 | (uint16_t)sy; return 0; }
    /* rows read or written: the picture plus the half-pel row below and the dcbz line after a row end */
    uint32_t ylen = (uint32_t)sy * (uint32_t)(mbh * 16 + 1) + 32, clen = (uint32_t)sc * (uint32_t)(mbh * 8 + 1) + 32;
    if (!guest_span_ok(y & ~31u, ylen) || !guest_span_ok(cb & ~31u, clen) || !guest_span_ok(cr & ~31u, clen)) {
        *bad = y;
        return 0;
    }
    p->y = (uint8_t*)ppc_ptr(y);
    p->cb = (uint8_t*)ppc_ptr(cb);
    p->cr = (uint8_t*)ppc_ptr(cr);
    p->yea = y;
    p->cbea = cb;
    p->crea = cr;
    p->sy = sy;
    p->sc = sc;
    return 1;
}
static int read_mv(const uint8_t* m, MvState* v, int used) {
    v->full_pel = (int)be32(m);
    v->r_size = (int)be32(m + 4);
    v->f = (int)be32(m + 0xC);
    v->pred[0] = v->pred[1] = v->recon[0] = v->recon[1] = 0;
    if (!used) return 1;
    if (v->full_pel > 1 || v->r_size < 0 || v->r_size > 6 || (int)be32(m + 8) != 27 - v->r_size || v->f != 1 << v->r_size) return 0;
    v->pred[0] = v->pred[1] = v->recon[0] = v->recon[1] = 0;  /* the slice header resets them */
    return 1;
}
static void write_mv(uint8_t* m, const MvState* v) {
    wbe32(m + 0x10, (uint32_t)v->pred[0]);
    wbe32(m + 0x14, (uint32_t)v->pred[1]);
    wbe32(m + 0x18, (uint32_t)v->recon[0]);
    wbe32(m + 0x1C, (uint32_t)v->recon[1]);
}

static int setup(Slice* s, Cpu* c) {
    uint32_t ctx = c->r[3];
    if (!guest_span_ok(ctx, C_MBPOS)) { fallback_log(1, "decoder state outside game memory", ctx, 0); return 0; }
    uint8_t* x = (uint8_t*)ppc_ptr(ctx);
    s->ctx = x;
    s->ctx_ea = ctx;
    s->pictype = (int)be32(x + C_PICTYPE);
    static const uint32_t slice_fn[4] = {0, FN_SLICE_I, FN_SLICE_P, FN_SLICE_B};
    if (s->pictype < 1 || s->pictype > 3 || be32(x + C_SLICEFN) != slice_fn[s->pictype]) {
        fallback_log(2, "picture type or slice function not handled", (uint32_t)s->pictype, be32(x + C_SLICEFN));
        return 0;
    }
    /* the macroblock functions the picture header chose: the paired-single set (ctx+0x180 == 3), mode 0 */
    if (be32(x + C_MBINTRA) != FN_MB_INTRA || be32(x + C_MBRESID) != FN_MB_RESID || be32(x + C_STINTRA) != FN_ST_INTRA ||
        (s->pictype == 2 && (be32(x + C_SKIPFN) != FN_SKIP_P || be32(x + C_STFWD) != FN_ST_FWD)) ||
        (s->pictype == 3 && (be32(x + C_SKIPFN) != FN_SKIP_B || be32(x + C_STFWD) != FN_ST_FWD ||
                             be32(x + C_STBWD) != FN_ST_BWD || be32(x + C_STBI) != FN_ST_BI))) {
        fallback_log(3, "another output mode (macroblock functions)", be32(x + C_STINTRA), be32(x + C_STFWD));
        return 0;
    }
    if (be32(x + C_MC_HALF) != 0) { fallback_log(4, "simplified half-pel mode", be32(x + C_MC_HALF), 0); return 0; }
    if (c->gqr[3] != 0x00070004u || c->gqr[4] != 0x00040004u || c->gqr[5] != 0x3F073F04u || c->gqr[7] != 0x00070007u) {
        fallback_log(5, "unexpected GQR set-up", c->gqr[3], c->gqr[5]);
        return 0;
    }
    if (!guest_span_ok(IDCT_CONSTS, 48)) return 0;
    for (int i = 0; i < 12; i++)
        if (ld32(IDCT_CONSTS + 4u * (uint32_t)i) != K_BITS[i]) { fallback_log(6, "IDCT constants differ", IDCT_CONSTS, ld32(IDCT_CONSTS + 4u * (uint32_t)i)); return 0; }
    s->mbw = (int)be32(x + C_MBW);
    s->mbmax = (int)be32(x + C_MBMAX);
    if (s->mbw < 1 || s->mbw > 64 || s->mbmax < 0 || s->mbmax >= 64 * 64 || (s->mbmax + 1) % s->mbw ||
        !guest_span_ok(ctx + C_MBPOS, 2u * (uint32_t)(s->mbmax + 1))) {
        fallback_log(7, "picture size", (uint32_t)s->mbw, (uint32_t)s->mbmax);
        return 0;
    }
    s->mbpos = x + C_MBPOS;
    uint32_t scan = be32(x + C_SCAN), pre = be32(x + C_PRESCALE);
    if (!guest_span_ok(scan, 64) || !guest_span_ok(pre, 256)) { fallback_log(8, "coefficient tables", scan, pre); return 0; }
    const uint8_t* sp = (const uint8_t*)ppc_ptr(scan);
    const uint8_t* pp = (const uint8_t*)ppc_ptr(pre);
    uint64_t seen = 0;
    for (int i = 0; i < 64; i++) {
        s->scan[i] = (int8_t)sp[i];
        if (s->scan[i] < 0 || s->scan[i] > 63 || (seen >> s->scan[i] & 1) || (i == 0 && s->scan[0] != 0)) {
            fallback_log(8, "scan table", scan, (uint32_t)i);
            return 0;
        }
        seen |= 1ull << s->scan[i];
        s->prescale[i] = bef(pp + 4 * i);
    }
    memcpy(s->qintra, x + C_QINTRA, 64);
    memcpy(s->qnon, x + C_QNONINTRA, 64);
    int mbh = (s->mbmax + 1) / s->mbw;
    uint32_t bad = 0;
    if (!read_planes(x, C_CUR, &s->cur, mbh, &bad) ||
        (s->pictype >= 2 && (!read_planes(x, C_FWDREF, &s->fwd, mbh, &bad) || !read_planes(x, C_BWDREF, &s->bwd, mbh, &bad)))) {
        fallback_log(9, "frame planes", bad, 0);
        return 0;
    }
    if (s->pictype >= 2 && (s->fwd.sy != s->cur.sy || s->fwd.sc != s->cur.sc || s->bwd.sy != s->cur.sy || s->bwd.sc != s->cur.sc)) {
        fallback_log(10, "frames with different strides", (uint32_t)s->fwd.sy, (uint32_t)s->cur.sy);
        return 0;
    }
    /* the f_code fields of a direction the picture does not use are left over from earlier pictures */
    if (!read_mv(x + C_FMV, &s->mv[0], s->pictype >= 2) || !read_mv(x + C_BMV, &s->mv[1], s->pictype == 3)) {
        fallback_log(11, "motion vector set-up", be32(x + C_FMV + 4), be32(x + C_BMV + 4));
        return 0;
    }
    s->lastpred = 1;
    if (s->pictype == 3) {
        uint32_t lp = be32(x + C_LASTPRED);
        s->lastpred = lp == FN_ST_FWD ? 1 : lp == FN_ST_BWD ? 2 : lp == FN_ST_BI ? 3 : 0;
        if (!s->lastpred) { fallback_log(12, "previous prediction function", lp, 0); return 0; }
    }
    return 1;
}

static const uint8_t k_default_intra[64] = {  /* zig-zag order */
    8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29,
    34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48,
    46, 46, 56, 56, 58, 69, 69, 83};
static void announce(const Slice* s) {
    int def_i = 1, def_n = 1;
    for (int i = 1; i < 64; i++) def_i &= s->qintra[s->scan[i]] == k_default_intra[i];
    for (int i = 0; i < 64; i++) def_n &= s->qnon[i] == 16;
    rt_log("[movie] native MPEG-1 video decoding (movie.rel slice decoder 0x8180E498 replaced): %dx%d macroblocks "
           "(%dx%d), quantiser matrices: intra %s, non-intra %s; Y stride %d, chroma stride %d",
           s->mbw, (s->mbmax + 1) / s->mbw, s->mbw * 16, (s->mbmax + 1) / s->mbw * 16, def_i ? "default" : "from the stream",
           def_n ? "default" : "from the stream", s->cur.sy, s->cur.sc);
}

/* rel.c: movie.rel was linked again (a new movie): announce the next one, statistics from zero */
void movie_mpeg_linked(void) {
    g_announced_ctx = g_announced_size = 0;
    g_pics = g_slices = g_fallbacks = g_pic_ns_sum = g_pic_ns_max = g_pic_ns_cur = 0;
    g_report_t0 = 0;
    g_last_vpos = 1 << 30;
}

static void perf_account(uint64_t ns, int vpos) {
    if (vpos <= g_last_vpos) {  /* a new picture */
        if (g_pic_ns_cur) {
            g_pics++;
            g_pic_ns_sum += g_pic_ns_cur;
            if (g_pic_ns_cur > g_pic_ns_max) g_pic_ns_max = g_pic_ns_cur;
        }
        g_pic_ns_cur = 0;
    }
    g_last_vpos = vpos;
    g_pic_ns_cur += ns;
    g_slices++;
    uint64_t t = rt_ticks();
    if (!g_report_t0) g_report_t0 = t;
    else if (t - g_report_t0 >= 10ull * 40500000ull) {
        if (g_pics)
            rt_log("[perf] movie: native MPEG-1 decode %.2f ms/picture (longest %.2f), %llu pictures, %llu slices, "
                   "%llu slices by the game's decoder", g_pic_ns_sum / 1e6 / (double)g_pics, g_pic_ns_max / 1e6, g_pics,
                   g_slices, g_fallbacks);
        g_pics = g_slices = g_fallbacks = g_pic_ns_sum = g_pic_ns_max = 0;
        g_report_t0 = t;
    }
}

void hle_movie_slice(Cpu* c) {
    g_ppc_cur_fn = 0x8180E498u;
    uint64_t t0 = platform_now_ns();
    if (!g_tables_ready) build_tables();
    if (((uintptr_t)ppc_mem_base_var & 31) != 0) {  /* dcbz is reproduced on host addresses */
        fallback_log(0, "guest memory not 32-byte aligned", 0, 0);
        goto orig;
    }
    Slice s;
    if (!setup(&s, c)) goto orig;
    uint32_t pa = c->r[4], la = c->r[5], ca = c->r[6];
    if (!guest_span_ok(pa, 4) || !guest_span_ok(la, 4) || !guest_span_ok(ca, 4)) goto orig;
    uint32_t p = ld32(pa), len = ld32(la);
    uint32_t a = p & ~3u, lead = p & 3u;
    /* the data, and the 16 bytes after it the bit reader may load */
    if (len < 5 || len > 0x00800000u || !guest_span_ok(a, lead + len + 16)) { fallback_log(13, "slice data", p, len); goto orig; }
    Bits b = {(const uint8_t*)ppc_ptr(a), lead * 8, (lead + len) * 8, 0};
    /* slice header */
    uint32_t sc = get(&b, 32);
    int vpos = (int)(sc & 0xFF);
    int addr = (vpos - 1) * s.mbw - 1;
    s.qscale = (int)get(&b, 5);
    while (get(&b, 1)) skip(&b, 8);  /* extra_bit_slice, extra_information_slice */
    if (b.bad) { fallback_log(13, "slice data", p, len); goto orig; }
    s.dcpred[0] = s.dcpred[1] = s.dcpred[2] = 1024;
    if (g_announced_ctx != c->r[3] || g_announced_size != (uint32_t)(s.mbw << 16 | s.mbmax)) {  /* once per movie */
        g_announced_ctx = c->r[3];
        g_announced_size = (uint32_t)(s.mbw << 16 | s.mbmax);
        announce(&s);
    }
    int err = decode_slice(c, &s, &b, &addr);
    if (err) {
        static const char* what[] = {"", "bad code", "coefficient past the block", "data ran out", "macroblock address",
                                     "motion vector", "macroblock type"};
        fallback_log(16 + err, what[err], p, b.pos);
        goto orig;
    }
    /* the decoder state as the game's loop leaves it */
    uint8_t* x = s.ctx;
    wbe32(x + C_MBADDR, (uint32_t)addr);
    wbe32(x + C_QSCALE, (uint32_t)s.qscale);
    write_mv(x + C_FMV, &s.mv[0]);
    write_mv(x + C_BMV, &s.mv[1]);
    for (int i = 0; i < 3; i++) wbe32(x + C_DCPRED + 4 * i, (uint32_t)s.dcpred[i]);
    if (s.pictype == 3) wbe32(x + C_LASTPRED, s.lastpred == 1 ? FN_ST_FWD : s.lastpred == 2 ? FN_ST_BWD : FN_ST_BI);
    {   /* bit reader: the word at the position shifted, the next word, the bit, the word after next */
        uint32_t wp = a + 4 * (b.pos >> 5), bit = b.pos & 31;
        wbe32(x + 0x0, bit ? ld32(wp) << bit : ld32(wp));
        wbe32(x + 0x4, ld32(wp + 4));
        wbe32(x + 0x8, bit);
        wbe32(x + 0xC, wp + 8);
    }
    /* advance past the slice (byte aligned), then the game's own skip to the next start code */
    uint32_t np = a + (b.pos + 7) / 8, d = np - p;
    st32(pa, np);
    st32(la, len - d);
    st32(ca, ld32(ca) + d);
    c->r[3] = pa;
    c->r[4] = la;
    c->r[5] = ca;
    f_8180E87C(c);
    perf_account(platform_now_ns() - t0, vpos);
    return;
orig:
    g_fallbacks++;
    f_8180E498_orig(c);
}

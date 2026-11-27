/* Test of the PowerPC interpreter fallback (runtime/src/interp.c).
 *
 * Runs small hand-assembled PowerPC sequences, written into a guest RAM of our own at 0x80004000 (where
 * PSO servers put their code), through ppc_interp, with stand-ins for the rest of the runtime: two
 * "recompiled functions" inside a made-up game code range 0x8000C000-0x8000E000, and the log and fatal
 * captured. No game data is involved. Checks integer arithmetic and record forms, OE overflow, rotates,
 * loads and stores with update and indexed and byte-reversed forms, lmw/stmw, compares into CR fields and
 * CR logic, a bdnz loop, bl into a recompiled function and back (with a stack frame), a tail call, bctrl,
 * return detection (blr to lr only counts with r1 as at entry), floating point, the game-code store log,
 * the first-entry RAM dump, the unknown-instruction fatal, the step limit and ppc_interp_can_run.
 *
 * Built and run by run_interp_test.sh. Prints RESULT: PASS or RESULT: FAIL. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interp.h"
#include "recomp_table.h"
#include "runtime.h"

/* ---- stand-ins for the runtime ---- */
uint8_t* ppc_mem_base_var;
uint8_t g_wg[GX_WG_BATCH + 64];
int g_wg_n, g_prev_bp_prefix, g_gxstat;
unsigned long long g_pipe_stores, g_pipe_bytes;
void gx_pipe_write(uint32_t v, int size) { (void)v; (void)size; }
void gs_store(void) {}
uint32_t mmio_read(uint32_t ea, int size) { (void)ea; (void)size; return 0; }
void mmio_write(uint32_t ea, uint32_t v, int size) { (void)ea; (void)v; (void)size; }
void mmio_write64(uint32_t ea, uint64_t v) { (void)ea; (void)v; }
volatile int g_core_preempt[3];
void ppc_preempt(Cpu* c) { (void)c; }
volatile uint32_t g_ppc_cur_fn, g_ppc_cur_lr;
uint32_t ppc_mfmsr(Cpu* c) { return c->msr; }
void ppc_mtmsr(Cpu* c, uint32_t v) { c->msr = v; }
uint32_t ppc_mfspr(Cpu* c, int spr) { (void)c; return 0x1000u + (uint32_t)spr; }
void ppc_mtspr(Cpu* c, int spr, uint32_t v) { (void)c; (void)spr; (void)v; }
void ppc_sc(Cpu* c, uint32_t addr) { (void)c; (void)addr; }
uint64_t ppc_timebase(void) { return 0x123456789ull; }
double ppc_fres(double x) { return 1.0 / x; }
double ppc_frsqrte(double x) { return 1.0 / sqrt(x); }

static char g_log[1 << 16];
static size_t g_log_n;
void rt_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_log_n, sizeof g_log - g_log_n, fmt, ap);
    va_end(ap);
    if (n > 0 && g_log_n + n + 1 < sizeof g_log) { g_log_n += n; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0; }
    if (getenv("INTERP_TEST_VERBOSE")) {
        va_start(ap, fmt);
        vfprintf(stdout, fmt, ap);
        va_end(ap);
        fputc('\n', stdout);
    }
}
static jmp_buf g_fatal_jmp;
static int g_fatal_armed;
static char g_fatal_msg[512];
_Noreturn void rt_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fatal_msg, sizeof g_fatal_msg, fmt, ap);
    va_end(ap);
    if (g_fatal_armed) longjmp(g_fatal_jmp, 1);
    fprintf(stderr, "unexpected FATAL: %s\n", g_fatal_msg);
    exit(2);
}
void ppc_trap(Cpu* c, uint32_t addr) { (void)c; rt_fatal("trap at %08X", addr); }

/* the game's code range and two recompiled functions in it */
const RecompRange g_code_ranges[] = {{0x8000C000u, 0x8000E000u}};
const unsigned g_code_range_count = 1;
#define FN_DOUBLE 0x8000C100u  /* r3 = 2 * r3 + 1; records the lr it saw */
#define FN_ADD10 0x8000C200u   /* r3 += 10 */
static uint32_t g_seen_lr;
static int g_calls;
static void fn_double(Cpu* c) { g_calls++; g_seen_lr = c->lr; c->r[3] = 2 * c->r[3] + 1; }
static void fn_add10(Cpu* c) { g_calls++; c->r[3] += 10; }
PpcFunc ppc_dispatch_lookup(uint32_t a) {
    if (a == FN_DOUBLE) return fn_double;
    if (a == FN_ADD10) return fn_add10;
    return 0;
}

/* ---- an assembler for the forms used ---- */
#define D(op, d, a, imm) (((uint32_t)(op) << 26) | ((uint32_t)(d) << 21) | ((uint32_t)(a) << 16) | ((uint32_t)(imm) & 0xFFFF))
#define X(d, a, b, xo, rc) ((31u << 26) | ((uint32_t)(d) << 21) | ((uint32_t)(a) << 16) | ((uint32_t)(b) << 11) | ((uint32_t)(xo) << 1) | (rc))
#define XO(d, a, b, oe, xo, rc) (X(d, a, b, 0, rc) | ((uint32_t)(oe) << 10) | ((uint32_t)(xo) << 1))
#define LI(d, v) D(14, d, 0, v)
#define LIS(d, v) D(15, d, 0, v)
#define ADDI(d, a, v) D(14, d, a, v)
#define ORI(a, s, v) D(24, s, a, v)
#define RLWINM(a, s, sh, mb, me, rc) ((21u << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(a) << 16) | ((uint32_t)(sh) << 11) | ((uint32_t)(mb) << 6) | ((uint32_t)(me) << 1) | (rc))
#define RLWIMI(a, s, sh, mb, me) ((20u << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(a) << 16) | ((uint32_t)(sh) << 11) | ((uint32_t)(mb) << 6) | ((uint32_t)(me) << 1))
#define BLR 0x4E800020u
#define BCTR 0x4E800420u
#define BCTRL 0x4E800421u
#define MTLR(s) (0x7C0803A6u | ((uint32_t)(s) << 21))
#define MFLR(d) (0x7C0802A6u | ((uint32_t)(d) << 21))
#define MTCTR(s) (0x7C0903A6u | ((uint32_t)(s) << 21))
#define B(from, to, lk) ((18u << 26) | (((uint32_t)(to) - (uint32_t)(from)) & 0x03FFFFFCu) | (lk))
#define BC(bo, bi, from, to) ((16u << 26) | ((uint32_t)(bo) << 21) | ((uint32_t)(bi) << 16) | (((uint32_t)(to) - (uint32_t)(from)) & 0xFFFC))
#define CMPW(crf, a, b) X((crf) << 2, a, b, 0, 0)
#define CMPLW(crf, a, b) X((crf) << 2, a, b, 32, 0)
#define CMPWI(crf, a, v) D(11, (crf) << 2, a, v)

#define CODE 0x80004000u
#define RET_LR 0x80001000u   /* the recompiled caller's return address (never interpreted here) */
#define SP 0x80100000u
#define DATA 0x80008000u

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(uint32_t a, const uint32_t* w, unsigned n) { for (unsigned i = 0; i < n; i++) st32(a + 4 * i, w[i]); }
static void fresh(Cpu* c, uint32_t pc) {
    memset(c, 0, sizeof *c);
    c->pc = pc;
    c->lr = RET_LR;
    c->r[1] = SP;
}
#define RUN(c, code) do { put(CODE, code, sizeof(code) / 4); fresh(c, CODE); } while (0)

int main(void) {
    ppc_mem_base_var = calloc(1, GUEST_RAM_SIZE);
    if (!ppc_mem_base_var) return 2;
    Cpu c;

    /* 1. arithmetic, record forms, carries, overflow */
    {
        uint32_t code[] = {
            LI(3, 5), LI(4, 7), XO(5, 3, 4, 0, 266, 0),       /* add r5 = 12 */
            XO(6, 4, 3, 0, 40, 1),                            /* subf. r6 = r3 - r4 = -2, cr0 lt */
            D(12, 7, 3, -6),                                  /* addic r7 = 5 - 6 = -1, no carry */
            D(12, 8, 4, -6),                                  /* addic r8 = 1, carry */
            XO(9, 3, 4, 0, 235, 0),                           /* mullw r9 = 35 */
            LI(10, -35), XO(11, 10, 4, 0, 491, 0),            /* divw r11 = -5 */
            LIS(12, 0x7FFF), ORI(12, 12, 0xFFFF),
            XO(13, 12, 12, 1, 266, 1),                        /* addo. overflow: ov, so, cr0.so */
            XO(14, 3, 0, 0, 104, 0),                          /* neg r14 = -5 */
            D(7, 15, 3, -3),                                  /* mulli r15 = -15 */
            D(8, 16, 3, 100),                                 /* subfic r16 = 95 */
            X(4, 17, 0, 26, 0),                               /* cntlzw r17 = clz(7) = 29 */
            LI(18, 0x80), X(18, 19, 0, 954, 0),               /* extsb r19 = 0xFFFFFF80 */
            LI(20, -16), X(20, 21, 0, 824, 0) | (2 << 11),    /* srawi r21 = -4, ca 0 */
            BLR,
        };
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(c.r[5] == 12, "add %u", c.r[5]);
        CHECK(c.r[6] == (uint32_t)-2, "subf. %d", (int)c.r[6]);  /* its cr0 is overwritten by addo. below */
        CHECK(c.r[7] == 0xFFFFFFFFu, "addic %08X", c.r[7]);
        CHECK(c.r[8] == 1, "addic carry %u", c.r[8]);
        CHECK(c.r[9] == 35, "mullw %u", c.r[9]);
        CHECK((int32_t)c.r[11] == -5, "divw %d", (int)c.r[11]);
        CHECK(c.r[13] == 0xFFFFFFFEu && c.xer_ov == 1 && c.xer_so == 1 && c.cr[3] == 1 && c.cr[0] == 1, "addo. %08X ov %d so %d cr0 %d%d%d%d",
              c.r[13], c.xer_ov, c.xer_so, c.cr[0], c.cr[1], c.cr[2], c.cr[3]);
        CHECK((int32_t)c.r[14] == -5, "neg %d", (int)c.r[14]);
        CHECK((int32_t)c.r[15] == -15, "mulli %d", (int)c.r[15]);
        CHECK(c.r[16] == 95, "subfic %u", c.r[16]);
        CHECK(c.r[17] == 29, "cntlzw %u", c.r[17]);
        CHECK(c.r[19] == 0xFFFFFF80u, "extsb %08X", c.r[19]);
        CHECK((int32_t)c.r[21] == -4 && c.xer_ca == 0, "srawi %d ca %d", (int)c.r[21], c.xer_ca);
        CHECK(c.pc == CODE + 4 * (sizeof code / 4 - 1), "pc at the blr %08X", c.pc);
    }

    /* 2. rlwinm, rlwimi, rlwinm. giving 0 */
    {
        uint32_t code[] = {
            LIS(3, 0x1234), ORI(3, 3, 0x5678),
            RLWINM(4, 3, 8, 24, 31, 0),                       /* r4 = 0x12 */
            LIS(5, 0xAAAA), ORI(5, 5, 0xBBBB),
            RLWIMI(5, 3, 16, 0, 15),                          /* r5 = 0x5678BBBB */
            RLWINM(6, 3, 0, 28, 27, 0),                       /* wrapping mask (all bits): r6 = r3 */
            RLWINM(7, 3, 0, 0, 0, 1),                         /* r7 = 0, cr0 eq */
            RLWINM(8, 3, 4, 0, 27, 0),                        /* slwi 4: 0x23456780 */
            BLR,
        };
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(c.r[4] == 0x12, "rlwinm %08X", c.r[4]);
        CHECK(c.r[5] == 0x5678BBBBu, "rlwimi %08X", c.r[5]);
        CHECK(c.r[6] == 0x12345678u, "rlwinm wrap %08X", c.r[6]);
        CHECK(c.r[7] == 0 && c.cr[2] == 1 && c.cr[0] == 0, "rlwinm. %08X eq %d", c.r[7], c.cr[2]);
        CHECK(c.r[8] == 0x23456780u, "slwi %08X", c.r[8]);
    }

    /* 3. loads and stores: update, indexed, sign extension, byte-reversed */
    {
        uint32_t code[] = {
            LIS(10, 0x8000), ORI(10, 10, 0x8000),             /* r10 = DATA */
            LIS(3, 0xCAFE), ORI(3, 3, 0xF00D),
            D(37, 3, 10, 4),                                  /* stwu r3, 4(r10): r10 = DATA + 4 */
            D(33, 4, 10, -4),                                 /* lwzu r4, -4(r10): r10 = DATA, r4 = 0 */
            D(32, 5, 10, 4),                                  /* lwz r5 = CAFEF00D */
            D(35, 6, 10, 4),                                  /* lbzu r6 = CA, r10 = DATA + 4 */
            LI(11, 2), X(7, 10, 11, 343, 0),                  /* lhax r7 = (int16)0xF00D */
            X(8, 10, 0, 534, 0),                              /* lwbrx r8 = 0DF0FECA */
            LI(12, 8), X(3, 10, 12, 151, 0),                  /* stwx r3 -> DATA + 12 */
            X(3, 10, 12, 662, 0), ADDI(12, 12, 4), X(3, 10, 12, 662, 0),  /* stwbrx at DATA + 12 and + 16 */
            D(44, 3, 10, 20),                                 /* sth r3 -> DATA + 24 */
            D(43, 9, 10, 20),                                 /* lhau r9 = (int16)F00D, r10 = DATA + 24 */
            BLR,
        };
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(ld32(DATA + 4) == 0xCAFEF00Du, "stwu %08X", ld32(DATA + 4));
        CHECK(c.r[4] == 0, "lwzu %08X", c.r[4]);
        CHECK(c.r[5] == 0xCAFEF00Du, "lwz %08X", c.r[5]);
        CHECK(c.r[6] == 0xCA, "lbzu %08X", c.r[6]);
        CHECK(c.r[7] == 0xFFFFF00Du, "lhax %08X", c.r[7]);
        CHECK(c.r[8] == 0x0DF0FECAu, "lwbrx %08X", c.r[8]);
        CHECK(ld32(DATA + 12) == 0x0DF0FECAu && ld32(DATA + 16) == 0x0DF0FECAu, "stwbrx %08X %08X", ld32(DATA + 12), ld32(DATA + 16));
        CHECK(c.r[9] == 0xFFFFF00Du && c.r[10] == DATA + 24, "lhau %08X r10 %08X", c.r[9], c.r[10]);
    }

    /* 4. a bdnz loop */
    {
        uint32_t code[] = {LI(3, 0), LI(4, 10), MTCTR(4), ADDI(3, 3, 3), BC(16, 0, CODE + 16, CODE + 12), BLR};
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(c.r[3] == 30 && c.ctr == 0, "bdnz loop r3 %u ctr %u", c.r[3], c.ctr);
    }

    /* 5. bl into a recompiled function and back, inside a stack frame of the interpreted code */
    {
        uint32_t code[] = {
            MFLR(0), D(36, 0, 1, 4), D(37, 1, 1, -16),        /* mflr r0; stw r0, 4(r1); stwu r1, -16(r1) */
            LI(3, 4),
            B(CODE + 16, FN_DOUBLE, 1),                       /* bl FN_DOUBLE: r3 = 9 */
            ADDI(3, 3, 100),                                  /* r3 = 109 */
            D(32, 0, 1, 20), ADDI(1, 1, 16), MTLR(0), BLR,
        };
        RUN(&c, code);
        g_calls = 0;
        ppc_interp(&c);
        CHECK(c.r[3] == 109, "bl and back r3 %u", c.r[3]);
        CHECK(g_seen_lr == CODE + 20, "callee saw lr %08X", g_seen_lr);
        CHECK(g_calls == 1 && c.r[1] == SP && c.lr == RET_LR, "calls %d r1 %08X lr %08X", g_calls, c.r[1], c.lr);
    }

    /* 6. return detection: a blr to lr with another r1 is not a return */
    {
        uint32_t code[] = {
            LIS(4, 0x8000), ORI(4, 4, 0x5000), MTLR(4),        /* lr = 0x80005000 */
            D(37, 1, 1, -16),                                  /* stwu r1, -16(r1) */
            BLR,                                               /* to 0x80005000, but r1 differs: no return */
        };
        uint32_t more[] = {
            ADDI(1, 1, 16), LI(30, 77),
            B(0x80005008u, 0x80005000u, 0),                    /* b 0x80005000: lr and r1 now match */
        };
        put(0x80005000u, more, 3);
        RUN(&c, code);
        c.lr = 0x80005000u;  /* the caller's return address is the code above */
        ppc_interp(&c);
        CHECK(c.r[30] == 77 && c.r[1] == SP, "return detection r30 %u r1 %08X", c.r[30], c.r[1]);
    }

    /* 7. lmw / stmw */
    {
        uint32_t code[] = {
            LIS(10, 0x8000), ORI(10, 10, 0x8100),
            LI(27, 1), LI(28, 2), LI(29, 3), LI(30, 4), LI(31, 5),
            D(47, 27, 10, 0),                                  /* stmw r27, 0(r10) */
            LI(27, 0), LI(28, 0), LI(29, 0), LI(30, 0), LI(31, 0),
            D(46, 28, 10, 4),                                  /* lmw r28, 4(r10): r28..r31 = 2..5 */
            BLR,
        };
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(ld32(0x80008100u) == 1 && ld32(0x80008110u) == 5, "stmw %u %u", ld32(0x80008100u), ld32(0x80008110u));
        CHECK(c.r[27] == 0 && c.r[28] == 2 && c.r[29] == 3 && c.r[30] == 4 && c.r[31] == 5, "lmw %u %u %u %u %u",
              c.r[27], c.r[28], c.r[29], c.r[30], c.r[31]);
    }

    /* 8. compares into CR fields, CR logic, mfcr */
    {
        uint32_t code[] = {
            LI(3, -1), LI(4, 1),
            CMPW(3, 3, 4),                                     /* cr3: lt */
            CMPLW(5, 3, 4),                                    /* cr5: gt (unsigned) */
            CMPWI(7, 3, -1),                                   /* cr7: eq */
            (19u << 26) | (1u << 21) | (12u << 16) | (21u << 11) | (449u << 1),  /* cror 1, 12, 21: cr0.gt = cr3.lt | cr5.gt */
            X(5, 0, 0, 19, 0),                                 /* mfcr r5 */
            BLR,
        };
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(c.cr[12] == 1 && c.cr[13] == 0 && c.cr[14] == 0, "cmpw cr3 %d%d%d", c.cr[12], c.cr[13], c.cr[14]);
        CHECK(c.cr[20] == 0 && c.cr[21] == 1 && c.cr[22] == 0, "cmplw cr5 %d%d%d", c.cr[20], c.cr[21], c.cr[22]);
        CHECK(c.cr[28] == 0 && c.cr[29] == 0 && c.cr[30] == 1, "cmpwi cr7 %d%d%d", c.cr[28], c.cr[29], c.cr[30]);
        CHECK(c.cr[1] == 1, "cror %d", c.cr[1]);
        CHECK(c.r[5] == 0x40080402u, "mfcr %08X", c.r[5]);
    }

    /* 9. tail call (b to a recompiled function with lr and r1 as at entry), and bctrl */
    {
        uint32_t code[] = {
            LIS(9, 0x8000), ORI(9, 9, 0xC200), MTCTR(9),
            MFLR(0), D(37, 1, 1, -16), D(36, 0, 1, 20),
            LI(3, 1),
            BCTRL,                                             /* FN_ADD10: r3 = 11 */
            D(32, 0, 1, 20), MTLR(0), ADDI(1, 1, 16),
            B(CODE + 44, FN_DOUBLE, 0),                        /* tail call: r3 = 23, then back to the caller */
            LI(3, 999),                                        /* never reached */
        };
        RUN(&c, code);
        g_calls = 0;
        ppc_interp(&c);
        CHECK(c.r[3] == 23 && g_calls == 2, "bctrl and tail call r3 %u calls %d", c.r[3], g_calls);
        CHECK(g_seen_lr == RET_LR, "tail callee saw lr %08X", g_seen_lr);
    }

    /* 9b. a jump to a recompiled function with lr set by the code itself: after it, control goes to c->lr
     * (more interpreted code), not back to the caller */
    {
        uint32_t code[] = {
            MFLR(31), LIS(4, 0x8000), ORI(4, 4, 0x5100), MTLR(4),   /* r31 = entry lr; lr = 0x80005100 */
            LI(3, 5),
            B(CODE + 20, FN_ADD10, 0),                              /* r3 = 15, then on at 0x80005100 */
        };
        uint32_t more[] = {ADDI(3, 3, 1), MTLR(31), BLR};          /* r3 = 16, back to the caller */
        put(0x80005100u, more, 3);
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(c.r[3] == 16 && c.lr == RET_LR, "jump with own lr r3 %u lr %08X", c.r[3], c.lr);
    }

    /* 10. floating point and paired singles */
    {
        uint32_t code[] = {
            LIS(10, 0x8000), ORI(10, 10, 0x8200),
            D(48, 1, 10, 0), D(48, 2, 10, 4),                  /* lfs f1 = 1.5, f2 = 2.25 */
            (59u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (21u << 1),    /* fadds f3 = 3.75 */
            (63u << 26) | (4u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (29u << 1),  /* fmadd f4 = f1*f3+f2 */
            D(52, 3, 10, 8),                                   /* stfs f3 -> +8 */
            (63u << 26) | (1u << 23) | (1u << 16) | (2u << 11) | (0u << 1),     /* fcmpu cr1, f1, f2: lt */
            (63u << 26) | (5u << 21) | (4u << 11) | (15u << 1), /* fctiwz f5 = 7 */
            X(5, 0, 12, 983, 0),                               /* stfiwx f5 -> 0 + r12 (set below) */
            BLR,
        };
        RUN(&c, code);
        st32(0x80008200u, 0x3FC00000u);  /* 1.5f */
        st32(0x80008204u, 0x40100000u);  /* 2.25f */
        c.r[12] = 0x80008210u;
        ppc_interp(&c);
        CHECK(c.f[3].ps0 == 3.75 && c.f[3].ps1 == 3.75, "fadds %g %g", c.f[3].ps0, c.f[3].ps1);
        CHECK(c.f[4].ps0 == 1.5 * 3.75 + 2.25, "fmadd %g", c.f[4].ps0);
        CHECK(ld32(0x80008208u) == 0x40700000u, "stfs %08X", ld32(0x80008208u));
        CHECK(c.cr[4] == 1 && c.cr[5] == 0 && c.cr[6] == 0 && c.cr[7] == 0, "fcmpu cr1 %d%d%d%d", c.cr[4], c.cr[5], c.cr[6], c.cr[7]);
        CHECK(ld32(0x80008210u) == 7, "fctiwz + stfiwx %u", ld32(0x80008210u));
    }

    /* 11. a store into the game's code is logged with old and new value */
    {
        st32(0x8000D000u, 0x7C0802A6u);
        g_log_n = 0; g_log[0] = 0;
        uint32_t code[] = {LIS(10, 0x8000), ORI(10, 10, 0xD000), LIS(3, 0x4E80), ORI(3, 3, 0x0020), D(36, 3, 10, 0), BLR};
        RUN(&c, code);
        ppc_interp(&c);
        CHECK(ld32(0x8000D000u) == 0x4E800020u, "patched word %08X", ld32(0x8000D000u));
        CHECK(strstr(g_log, "store to the game's code at 8000D000 (4 bytes): 7C0802A6 -> 4E800020") != NULL, "store log missing:\n%s", g_log);
    }

    /* 12. first entry: the hex dump line and the RAM dump file */
    {
        g_log_n = 0; g_log[0] = 0;
        uint32_t code[] = {LI(3, 1), BLR};
        put(0x80006000u, code, 2);
        fresh(&c, 0x80006000u);
        ppc_interp(&c);
        CHECK(strstr(g_log, "[interp] first entry at 80006000 (lr 80001000, r1 80100000)") != NULL, "first entry log missing:\n%s", g_log);
        CHECK(strstr(g_log, "[interp]   80006000: 38600001 4E800020 00000000") != NULL, "hex dump missing:\n%s", g_log);
        FILE* f = fopen(INTERP_DUMP_DIR "server_code_80006000.bin", "rb");
        long size = -1;
        uint8_t w[4] = {0};
        if (f) {
            fseek(f, 0x1000, SEEK_SET);  /* the dump starts 4 KiB before the entry */
            CHECK(fread(w, 1, 4, f) == 4, "dump read");
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fclose(f);
        }
        CHECK(size == 0x7000 && w[0] == 0x38 && w[1] == 0x60 && w[3] == 0x01, "RAM dump size %ld first word %02X%02X%02X%02X", size, w[0], w[1], w[2], w[3]);
        g_log_n = 0; g_log[0] = 0;
        fresh(&c, 0x80006000u);
        ppc_interp(&c);
        CHECK(strstr(g_log, "first entry") == NULL, "second entry logged as first:\n%s", g_log);
    }

    /* 13. an unknown instruction is a clean fatal naming address and word */
    {
        uint32_t code[] = {LI(3, 1), X(3, 4, 5, 533, 0) /* lswx: not translated by ppc2c.py either */};
        RUN(&c, code);
        g_fatal_armed = 1;
        if (!setjmp(g_fatal_jmp)) {
            ppc_interp(&c);
            CHECK(0, "no fatal for an unknown instruction");
        } else {
            char want[128];
            snprintf(want, sizeof want, "interpreted code at %08X: instruction %08X not handled by the interpreter", CODE + 4, code[1]);
            CHECK(strstr(g_fatal_msg, want) != NULL, "fatal message: %s", g_fatal_msg);
        }
        g_fatal_armed = 0;
    }

    /* 14. the step limit: an endless loop ends in a fatal */
    {
        uint32_t code[] = {B(CODE, CODE, 0)};
        RUN(&c, code);
        g_fatal_armed = 1;
        if (!setjmp(g_fatal_jmp)) {
            ppc_interp(&c);
            CHECK(0, "no fatal for an endless loop");
        } else {
            CHECK(strstr(g_fatal_msg, "or returning") != NULL, "fatal message: %s", g_fatal_msg);
        }
        g_fatal_armed = 0;
    }

    /* 15. ppc_interp_can_run */
    {
        st32(0x80007000u, 0x60000000u);
        st32(0x80007004u, 0);
        CHECK(ppc_interp_can_run(0x80007000u), "can run aligned code");
        CHECK(ppc_interp_can_run(0xC0007000u), "can run through the uncached mirror");
        CHECK(!ppc_interp_can_run(0x80007002u), "unaligned");
        CHECK(!ppc_interp_can_run(0x80100000u), "inside the DOL image: a bad pointer, not server code");
        CHECK(!ppc_interp_can_run(0x80500000u), "inside the DOL bss");
        CHECK(!ppc_interp_can_run(0x81810000u), "inside the REL image");
        CHECK(!ppc_interp_can_run(0x80007004u), "zero word");
        CHECK(!ppc_interp_can_run(0x00007000u), "physical address");
        CHECK(!ppc_interp_can_run(0xCC003000u), "device range");
        CHECK(!ppc_interp_can_run(0x80000040u), "boot info");
        CHECK(!ppc_interp_can_run(0x80000000u + GUEST_RAM_SIZE), "past guest RAM");
    }

    printf("RESULT: %s (%d failed checks)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}

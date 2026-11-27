/* PowerPC (Gekko) interpreter fallback for guest code that has no recompiled function.
 *
 * Why: PSO servers (Schtserv and others; the technique is documented in newserv) send GameCube clients a
 * download quest whose script makes the game's quest interpreter (0x801F2E64, its handler table at
 * 0x804C69D8, bctrl at 0x801F2ED0) call code the server wrote into unused low memory (0x80004000). That
 * code can install further handlers the game later calls through function pointers, and it can patch the
 * game's own code. A GameCube just runs it. This port only has the functions recompiled from the DOL, so
 * ppc_dispatch (dispatch.c) used to stop with "not a function entry". Now, when the target is not a
 * recompiled function but looks like code (ppc_interp_can_run), ppc_dispatch hands it to ppc_interp, which
 * interprets it from guest memory. Normal game paths never get here: the recompiled lookup comes first.
 *
 * The execution model (also in docs/design.md):
 *   - Entry: c->pc is the code, c->lr the return address the recompiled caller set, c->r[1] its stack
 *     pointer. Both are recorded (entry_lr, entry_sp).
 *   - Return: any branch without link (blr, bctr, b, conditional forms) to entry_lr while r1 equals
 *     entry_sp ends the interpretation: ppc_interp returns and the recompiled caller goes on, exactly as
 *     when a recompiled callee returns. A branch to entry_lr with another r1 is not a return (the code is
 *     still inside a frame of its own): it goes on interpreting there.
 *   - Calls (bl, bctrl, blrl, bcl...) to an address that is a recompiled function entry: lr is set to the
 *     next instruction, as the instruction does, the recompiled function runs (it may come back here
 *     through ppc_dispatch: every bit of interpreter state is local), and interpretation goes on at the
 *     instruction after the call. Only the game's code ranges are looked up (no function lives elsewhere).
 *   - Jumps without link (b, bctr, bclr not matching the return) to a recompiled function entry: a tail
 *     call. The function runs; it returns the way every recompiled function returns, which on the
 *     GameCube is a blr to the lr it sees, so control then goes to c->lr as the callee left it. That goes
 *     through the same rules again: when it is entry_lr with r1 at entry_sp (the usual tail call, lr
 *     untouched) the interpretation ends; otherwise it goes on interpreting at c->lr (code that set lr
 *     itself before the jump, a hand-made call). So a tail call is never assumed to end the frame; the
 *     guest state decides, as on the hardware.
 *   - Any other target (more server code, or the middle of game code: a hook that resumes a game function
 *     after its patched instruction) is interpreted from guest memory, which holds the DOL text (loader.c).
 *     Code entered in the middle of a recompiled function stays in the interpreter until a return matches
 *     entry_lr and entry_sp; its own epilogue normally returns further up than our entry, which is logged
 *     ("went past its entry frame") and then runs on in the interpreter (correct, slower) until the step
 *     limit stops it with a clean fatal.
 *   - Not covered: instructions the recompiler does not translate either (lswx/stswx, eciwx/ecowx, fsqrts,
 *     bcctr with a CTR decrement, rfi), and patches the server writes into the game's code do not change
 *     the recompiled functions (every such store is logged). Each unknown instruction is a clean rt_fatal
 *     with its address and word.
 *
 * Semantics follow tools/recomp/ppc2c.py instruction by instruction (the same ppc.h helpers: loads and
 * stores with their device test, cr_set_*, round25 and to_single for single precision, psq_load/store,
 * ppc_mfspr/ppc_mtspr, ppc_timebase, lwarx/stwcx. reservations). Interrupts are delivered at taken branches
 * as the recompiled code does at loop back-edges (PPC_LOOP). */
#include <stdio.h>
#include <string.h>

#include "interp.h"
#include "recomp_table.h"
#include "runtime.h"

PpcFunc ppc_dispatch_lookup(uint32_t a);  /* dispatch.c */

#ifndef INTERP_STEP_LIMIT
#define INTERP_STEP_LIMIT 50000000ull     /* per entry: about a second of interpreting; a hang ends in a fatal */
#endif
#ifndef INTERP_DUMP_DIR                    /* the first-entry guest RAM dump goes next to the log */
#ifdef __SWITCH__
#define INTERP_DUMP_DIR "sdmc:/switch/psoplus/"
#else
#define INTERP_DUMP_DIR ""
#endif
#endif
#define DUMP_LO 0x80000000u
#define DUMP_SIZE 0x10000u
#define LOG_ENTRIES 40          /* entries logged one by one; then a summary every SUMMARY_EVERY entries */
#define SUMMARY_EVERY 1000u
#define LOG_CODE_STORES 200
#define MAX_STARTS 64
#define MAX_CALLED 1024

typedef struct {
    Cpu* c;
    uint32_t start, entry_lr, entry_sp;
    unsigned long long steps;
    int past_frame_logged;
} Interp;

/* statistics (one guest thread runs at a time; nested entries on the same host thread are fine) */
static unsigned g_entries;
static unsigned long long g_steps_total;
static unsigned g_code_stores;
static uint32_t g_starts[MAX_STARTS];
static unsigned g_nstarts;
static uint32_t g_called[MAX_CALLED];  /* open addressing, 0 = empty */
static unsigned g_ncalled;
static unsigned g_past_frame_logs;

static int is_backed_code(uint32_t a) {
    uint32_t seg = a >> 28, p = a & PPC_PHYS_MASK;
    if (a & 3) return 0;
    if (seg != 0x8 && seg != 0xC) return 0;
    return p >= 0x100 && p <= GUEST_RAM_SIZE - 4;
}

int ppc_interp_can_run(uint32_t a) {
    return is_backed_code(a) && ld32(a) != 0;
}

/* the game's own code: the DOL's (and REL's) text sections, as the recompiler saw them (build/gen/table.c) */
static int in_game_code(uint32_t ea, uint32_t n) {
    if (PPC_IS_MMIO(ea)) return 0;
    uint32_t p = ea & PPC_PHYS_MASK;
    for (unsigned i = 0; i < g_code_range_count; i++) {
        uint32_t lo = g_code_ranges[i].lo & PPC_PHYS_MASK, hi = g_code_ranges[i].hi & PPC_PHYS_MASK;
        if (p < hi && p + n > lo) return 1;
    }
    return 0;
}
static int in_code_ranges(uint32_t a) { return in_game_code(a, 4); }

/* ---- logging ---- */

static void log_code_store(const Interp* I, uint32_t pc, uint32_t ea, unsigned n, const uint8_t* old) {
    unsigned k = ++g_code_stores;
    if (k > LOG_CODE_STORES) {
        if (k == LOG_CODE_STORES + 1)
            rt_log("[interp] more stores to the game's code: counted from here on, not logged (see the summary lines)");
        return;
    }
    const uint8_t* now = ppc_ptr(ea);
    char a[80], b[80];
    unsigned m = n > 32 ? 32 : n;
    for (unsigned i = 0; i < m; i++) {
        snprintf(a + 2 * i, sizeof a - 2 * i, "%02X", old[i]);
        snprintf(b + 2 * i, sizeof b - 2 * i, "%02X", now[i]);
    }
    rt_log("[interp] store to the game's code at %08X (%u bytes): %s -> %s, by the instruction at %08X (code entered at %08X); "
           "the recompiled code does not see this change", ea, n, a, b, pc, I->start);
}

/* every guest store of the interpreter: the ppc.h store, plus the log when it lands in the game's code */
#define WATCHED(ea, n, STORE) do {                                                    \
        uint32_t ea_ = (ea);                                                          \
        if (__builtin_expect(in_game_code(ea_, (n)), 0)) {                            \
            uint8_t old_[32];                                                         \
            memcpy(old_, ppc_ptr(ea_), (n));                                          \
            STORE;                                                                    \
            log_code_store(I, pc, ea_, (n), old_);                                    \
        } else {                                                                      \
            STORE;                                                                    \
        }                                                                             \
    } while (0)

static void first_entry(const Interp* I) {
    static int full_logged;
    for (unsigned i = 0; i < g_nstarts; i++)
        if (g_starts[i] == I->start) return;
    if (g_nstarts == MAX_STARTS) {
        if (!full_logged) rt_log("[interp] more than %d start addresses: no more first-entry logs", MAX_STARTS);
        full_logged = 1;
        return;
    }
    g_starts[g_nstarts++] = I->start;
    rt_log("[interp] first entry at %08X (lr %08X, r1 %08X): not a recompiled function, so the interpreter runs it. "
           "The first 64 words there:", I->start, I->entry_lr, I->entry_sp);
    for (unsigned row = 0; row < 8; row++) {
        uint32_t a = I->start + 32 * row;
        char line[8 * 9 + 1];
        int len = 0;
        for (unsigned i = 0; i < 8; i++)
            len += snprintf(line + len, sizeof line - len, i ? " %08X" : "%08X",
                            is_backed_code(a + 4 * i) ? ld32(a + 4 * i) : 0u);
        rt_log("[interp]   %08X: %s", a, line);
    }
    char path[256];
    snprintf(path, sizeof path, "%sserver_code_%08X.bin", INTERP_DUMP_DIR, I->start);
    FILE* f = fopen(path, "wb");
    size_t w = f ? fwrite(ppc_ptr(DUMP_LO), 1, DUMP_SIZE, f) : 0;
    if (f && fclose(f) == 0 && w == DUMP_SIZE)
        rt_log("[interp] wrote guest RAM %08X-%08X to %s", DUMP_LO, DUMP_LO + DUMP_SIZE - 1, path);
    else
        rt_log("[interp] could not write the guest RAM dump %s", path);
}

static void note_recomp_call(const Interp* I, uint32_t pc, uint32_t t, int link) {
    if (g_ncalled >= MAX_CALLED / 2) return;  /* the table stays half empty; later new targets go unlogged */
    unsigned h = (t >> 2) * 2654435761u % MAX_CALLED;
    while (g_called[h]) {
        if (g_called[h] == t) return;
        h = (h + 1) % MAX_CALLED;
    }
    g_called[h] = t;
    g_ncalled++;
    rt_log("[interp] code entered at %08X calls the recompiled function %08X (%s at %08X)", I->start, t,
           link ? "call" : "jump", pc);
    if (g_ncalled == MAX_CALLED / 2) rt_log("[interp] %u recompiled functions called: no more of these lines", g_ncalled);
}

static _Noreturn void unhandled(const Interp* I, uint32_t pc, uint32_t w) {
    rt_fatal("interpreted code at %08X: instruction %08X not handled by the interpreter (code entered at %08X, lr %08X)",
             pc, w, I->start, I->entry_lr);
}

/* ---- helpers matching ppc2c.py ---- */

static inline uint32_t ppc_mask(unsigned mb, unsigned me) {
    uint32_t m1 = 0xFFFFFFFFu >> mb, m2 = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (m1 & m2) : (m1 | m2);
}
static inline uint32_t sext16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
static inline void rc0(Cpu* c, uint32_t w, uint32_t v) { if (w & 1) cr0_rc(c, v); }
static inline void fp_rc(Cpu* c, uint32_t w) { if (w & 1) c->cr[4] = c->cr[5] = c->cr[6] = c->cr[7] = 0; }
/* signed overflow of x + y (+ carry in) giving r */
static inline void set_ov(Cpu* c, uint32_t x, uint32_t y, uint32_t r) {
    uint8_t o = (uint8_t)((~(x ^ y) & (x ^ r)) >> 31);
    c->xer_ov = o;
    c->xer_so |= o;
}
/* x + y + ci into the carry and, with OE, the overflow (addc, adde, subfc, subfe, addme, addze, subfme, subfze) */
static inline uint32_t add_ca(Cpu* c, uint32_t x, uint32_t y, uint32_t ci, int oe) {
    uint64_t t = (uint64_t)x + y + ci;
    uint32_t r = (uint32_t)t;
    c->xer_ca = (uint8_t)(t >> 32);
    if (oe) set_ov(c, x, y, r);
    return r;
}
static inline int trap_hit(uint32_t to, uint32_t x, uint32_t y) {
    return ((to & 16) && (int32_t)x < (int32_t)y) || ((to & 8) && (int32_t)x > (int32_t)y) || ((to & 4) && x == y) ||
           ((to & 2) && x < y) || ((to & 1) && x > y);
}
static inline int bc_taken(Cpu* c, uint32_t bo, uint32_t bi) {
    if (!(bo & 0x04)) {
        c->ctr--;
        if ((c->ctr == 0) != ((bo >> 1) & 1)) return 0;
    }
    if (!(bo & 0x10) && (!!c->cr[bi]) != ((bo >> 3) & 1)) return 0;
    return 1;
}

/* A control transfer from the instruction at pc to t (lr already set for a link branch). Returns 1 when
 * the interpreted frame is finished (back to the recompiled caller), else 0 with the next pc in *npc. */
static int transfer(Interp* I, uint32_t pc, uint32_t t, int link, uint32_t* npc) {
    Cpu* c = I->c;
    /* interrupts, as at the recompiled code's loop back-edges (PPC_LOOP) */
    __asm__ __volatile__("" ::: "memory");
    if (__builtin_expect(g_core_preempt[0], 0)) ppc_preempt(c);
    for (;;) {
        if (!link && t == I->entry_lr && c->r[1] == I->entry_sp) return 1;
        PpcFunc fn = in_code_ranges(t) ? ppc_dispatch_lookup(t) : (PpcFunc)0;
        if (!fn) {
            if (!is_backed_code(t))
                rt_fatal("interpreted code at %08X branches to %08X, which is not code in guest RAM (code entered at %08X, lr %08X)",
                         pc, t, I->start, I->entry_lr);
            if (!link && !I->past_frame_logged && c->r[1] > I->entry_sp) {
                I->past_frame_logged = 1;
                if (g_past_frame_logs++ < 20)
                    rt_log("[interp] code entered at %08X went past its entry frame (r1 %08X, at entry %08X) to %08X, "
                           "not back to %08X: it runs on in the interpreter", I->start, c->r[1], I->entry_sp, t, I->entry_lr);
            }
            *npc = t;
            return 0;
        }
        note_recomp_call(I, pc, t, link);
        c->pc = t;
        fn(c);
        g_ppc_cur_fn = I->start;
        if (link) {  /* the call returned: on at the instruction after it */
            *npc = pc + 4;
            return 0;
        }
        /* a tail call: the callee returned through lr, as a blr to c->lr; the same rules decide where to */
        t = c->lr & ~3u;
        if (++I->steps > INTERP_STEP_LIMIT)
            rt_fatal("interpreted code entered at %08X: %llu steps without returning (tail calls at %08X): stopped",
                     I->start, I->steps, pc);
    }
}

/* ---- the interpreter loop ---- */

static void run(Interp* I) {
    Cpu* c = I->c;
    uint32_t pc = c->pc;
    for (;;) {
        if (__builtin_expect(++I->steps > INTERP_STEP_LIMIT, 0))
            rt_fatal("interpreted code entered at %08X ran %llu instructions without returning (now at %08X, lr %08X): stopped",
                     I->start, I->steps - 1, pc, I->entry_lr);
        if (__builtin_expect(!is_backed_code(pc), 0))
            rt_fatal("interpreted code ran on to %08X, which is not code in guest RAM (code entered at %08X, lr %08X)",
                     pc, I->start, I->entry_lr);
        uint32_t w = ld32(pc);
        uint32_t op = w >> 26, d = (w >> 21) & 31, a = (w >> 16) & 31, b = (w >> 11) & 31, cc = (w >> 6) & 31;
        uint32_t simm = sext16(w & 0xFFFF), uimm = w & 0xFFFF;
        uint32_t ra0 = a ? c->r[a] : 0;
        uint32_t npc = pc + 4;
        c->pc = pc;
        switch (op) {
        case 18: {  /* b ba bl bla */
            uint32_t off = w & 0x03FFFFFCu;
            if (off & 0x02000000u) off |= 0xFC000000u;
            uint32_t t = off + ((w & 2) ? 0 : pc);
            if (w & 1) c->lr = pc + 4;
            if (transfer(I, pc, t, w & 1, &npc)) return;
            break;
        }
        case 16: {  /* bc */
            uint32_t t = sext16(w & 0xFFFC) + ((w & 2) ? 0 : pc);
            if (bc_taken(c, d, a)) {
                if (w & 1) c->lr = pc + 4;
                if (transfer(I, pc, t, w & 1, &npc)) return;
            }
            break;
        }
        case 19: {
            uint32_t xo = (w >> 1) & 0x3FF;
            switch (xo) {
            case 16:    /* bclr */
            case 528: { /* bcctr */
                if (xo == 528 && !(d & 0x04)) unhandled(I, pc, w);  /* bcctr with a CTR decrement: invalid */
                uint32_t t = (xo == 16 ? c->lr : c->ctr) & ~3u;
                if (bc_taken(c, d, a)) {
                    if (w & 1) c->lr = pc + 4;
                    if (transfer(I, pc, t, w & 1, &npc)) return;
                }
                break;
            }
            case 257: c->cr[d] = c->cr[a] & c->cr[b]; break;     /* crand */
            case 449: c->cr[d] = c->cr[a] | c->cr[b]; break;     /* cror */
            case 193: c->cr[d] = c->cr[a] ^ c->cr[b]; break;     /* crxor */
            case 225: c->cr[d] = !(c->cr[a] & c->cr[b]); break;  /* crnand */
            case 33: c->cr[d] = !(c->cr[a] | c->cr[b]); break;   /* crnor */
            case 289: c->cr[d] = !(c->cr[a] ^ c->cr[b]); break;  /* creqv */
            case 129: c->cr[d] = c->cr[a] & !c->cr[b]; break;    /* crandc */
            case 417: c->cr[d] = c->cr[a] | !c->cr[b]; break;    /* crorc */
            case 0: memmove(&c->cr[4 * (d >> 2)], &c->cr[4 * (a >> 2)], 4); break;  /* mcrf */
            case 150: __atomic_thread_fence(__ATOMIC_ACQUIRE); break;              /* isync */
            default: unhandled(I, pc, w);  /* rfi and the rest */
            }
            break;
        }
        /* integer immediate */
        case 14: c->r[d] = ra0 + simm; break;                 /* addi */
        case 15: c->r[d] = ra0 + (uimm << 16); break;         /* addis */
        case 12:
        case 13: {                                            /* addic addic. */
            uint64_t t = (uint64_t)c->r[a] + simm;
            c->r[d] = (uint32_t)t;
            c->xer_ca = (uint8_t)(t >> 32);
            if (op == 13) cr0_rc(c, c->r[d]);
            break;
        }
        case 8: {                                             /* subfic */
            uint64_t t = (uint64_t)(uint32_t)~c->r[a] + simm + 1;
            c->r[d] = (uint32_t)t;
            c->xer_ca = (uint8_t)(t >> 32);
            break;
        }
        case 7: c->r[d] = c->r[a] * simm; break;              /* mulli */
        case 10: cr_set_u(c, d >> 2, c->r[a], uimm); break;   /* cmpli */
        case 11: cr_set_s(c, d >> 2, (int32_t)c->r[a], (int32_t)simm); break;  /* cmpi */
        case 24: c->r[a] = c->r[d] | uimm; break;             /* ori */
        case 25: c->r[a] = c->r[d] | (uimm << 16); break;     /* oris */
        case 26: c->r[a] = c->r[d] ^ uimm; break;             /* xori */
        case 27: c->r[a] = c->r[d] ^ (uimm << 16); break;     /* xoris */
        case 28: c->r[a] = c->r[d] & uimm; cr0_rc(c, c->r[a]); break;          /* andi. */
        case 29: c->r[a] = c->r[d] & (uimm << 16); cr0_rc(c, c->r[a]); break;  /* andis. */
        /* rotates */
        case 20: {                                            /* rlwimi */
            uint32_t m = ppc_mask(cc, (w >> 1) & 31);
            c->r[a] = (rotl32(c->r[d], b) & m) | (c->r[a] & ~m);
            rc0(c, w, c->r[a]);
            break;
        }
        case 21:                                              /* rlwinm */
            c->r[a] = rotl32(c->r[d], b) & ppc_mask(cc, (w >> 1) & 31);
            rc0(c, w, c->r[a]);
            break;
        case 23:                                              /* rlwnm */
            c->r[a] = rotl32(c->r[d], c->r[b] & 31) & ppc_mask(cc, (w >> 1) & 31);
            rc0(c, w, c->r[a]);
            break;
        /* loads and stores, D-form */
        case 32: c->r[d] = ld32(ra0 + simm); break;                               /* lwz */
        case 34: c->r[d] = ld8(ra0 + simm); break;                                /* lbz */
        case 40: c->r[d] = ld16(ra0 + simm); break;                               /* lhz */
        case 42: c->r[d] = (uint32_t)(int32_t)(int16_t)ld16(ra0 + simm); break;   /* lha */
        case 33: { uint32_t ea = c->r[a] + simm; c->r[d] = ld32(ea); c->r[a] = ea; break; }  /* lwzu */
        case 35: { uint32_t ea = c->r[a] + simm; c->r[d] = ld8(ea); c->r[a] = ea; break; }   /* lbzu */
        case 41: { uint32_t ea = c->r[a] + simm; c->r[d] = ld16(ea); c->r[a] = ea; break; }  /* lhzu */
        case 43: { uint32_t ea = c->r[a] + simm; c->r[d] = (uint32_t)(int32_t)(int16_t)ld16(ea); c->r[a] = ea; break; }  /* lhau */
        case 36: WATCHED(ra0 + simm, 4, st32(ea_, c->r[d])); break;                          /* stw */
        case 38: WATCHED(ra0 + simm, 1, st8(ea_, (uint8_t)c->r[d])); break;                  /* stb */
        case 44: WATCHED(ra0 + simm, 2, st16(ea_, (uint16_t)c->r[d])); break;                /* sth */
        case 37: { uint32_t ea = c->r[a] + simm; WATCHED(ea, 4, st32(ea_, c->r[d])); c->r[a] = ea; break; }            /* stwu */
        case 39: { uint32_t ea = c->r[a] + simm; WATCHED(ea, 1, st8(ea_, (uint8_t)c->r[d])); c->r[a] = ea; break; }    /* stbu */
        case 45: { uint32_t ea = c->r[a] + simm; WATCHED(ea, 2, st16(ea_, (uint16_t)c->r[d])); c->r[a] = ea; break; }  /* sthu */
        case 46: {                                                                /* lmw */
            uint32_t ea = ra0 + simm;
            for (uint32_t r = d; r < 32; r++) c->r[r] = ld32(ea + 4 * (r - d));
            break;
        }
        case 47: {                                                                /* stmw */
            uint32_t ea = ra0 + simm;
            for (uint32_t r = d; r < 32; r++) WATCHED(ea + 4 * (r - d), 4, st32(ea_, c->r[r]));
            break;
        }
        /* floating-point loads and stores */
        case 48: case 49: case 50: case 51: {                                     /* lfs lfsu lfd lfdu */
            int upd = op & 1;
            uint32_t ea = (upd ? c->r[a] : ra0) + simm;
            if (op < 50) { double v = ldf32(ea); c->f[d].ps0 = v; c->f[d].ps1 = v; }
            else c->f[d].ps0 = ldf64(ea);
            if (upd) c->r[a] = ea;
            break;
        }
        case 52: case 53: case 54: case 55: {                                     /* stfs stfsu stfd stfdu */
            int upd = op & 1;
            uint32_t ea = (upd ? c->r[a] : ra0) + simm;
            if (op < 54) WATCHED(ea, 4, stf32(ea_, c->f[d].ps0));
            else WATCHED(ea, 8, stf64(ea_, c->f[d].ps0));
            if (upd) c->r[a] = ea;
            break;
        }
        case 56: case 57: case 60: case 61: {                                     /* psq_l psq_lu psq_st psq_stu */
            uint32_t off = (uint32_t)((int32_t)(w << 20) >> 20);
            int pw = (w >> 15) & 1, pi = (w >> 12) & 7, upd = op & 1;
            uint32_t ea = (upd ? c->r[a] : ra0) + off;
            if (op < 60) psq_load(c, d, ea, pw, pi);
            else WATCHED(ea, 8, psq_store(c, d, ea_, pw, pi));
            if (upd) c->r[a] = ea;
            break;
        }
        case 3:                                                                   /* twi */
            if (trap_hit(d, c->r[a], simm)) ppc_trap(c, pc);
            break;
        case 17: ppc_sc(c, pc); break;                                            /* sc */
        case 31: {
            uint32_t xo = (w >> 1) & 0x3FF, xo9 = (w >> 1) & 0x1FF, oe = (w >> 10) & 1;
            uint32_t rA = c->r[a], rB = c->r[b], rS = c->r[d];
            /* XO-form arithmetic (9-bit XO and OE), first as in ppc2c.py */
            switch (xo9) {
            case 266: { uint32_t r = rA + rB; if (oe) set_ov(c, rA, rB, r); c->r[d] = r; rc0(c, w, r); goto next; }      /* add */
            case 40: { uint32_t r = rB - rA; if (oe) set_ov(c, ~rA, rB, r); c->r[d] = r; rc0(c, w, r); goto next; }     /* subf */
            case 10: c->r[d] = add_ca(c, rA, rB, 0, oe); rc0(c, w, c->r[d]); goto next;                                  /* addc */
            case 138: c->r[d] = add_ca(c, rA, rB, c->xer_ca, oe); rc0(c, w, c->r[d]); goto next;                         /* adde */
            case 8: c->r[d] = add_ca(c, ~rA, rB, 1, oe); rc0(c, w, c->r[d]); goto next;                                  /* subfc */
            case 136: c->r[d] = add_ca(c, ~rA, rB, c->xer_ca, oe); rc0(c, w, c->r[d]); goto next;                        /* subfe */
            case 234: c->r[d] = add_ca(c, rA, 0xFFFFFFFFu, c->xer_ca, oe); rc0(c, w, c->r[d]); goto next;                /* addme */
            case 202: c->r[d] = add_ca(c, rA, 0, c->xer_ca, oe); rc0(c, w, c->r[d]); goto next;                          /* addze */
            case 232: c->r[d] = add_ca(c, ~rA, 0xFFFFFFFFu, c->xer_ca, oe); rc0(c, w, c->r[d]); goto next;               /* subfme */
            case 200: c->r[d] = add_ca(c, ~rA, 0, c->xer_ca, oe); rc0(c, w, c->r[d]); goto next;                         /* subfze */
            case 104:                                                                                                    /* neg */
                if (oe) { uint8_t o = rA == 0x80000000u; c->xer_ov = o; c->xer_so |= o; }
                c->r[d] = 0u - rA; rc0(c, w, c->r[d]); goto next;
            case 235: {                                                                                                  /* mullw */
                int64_t t = (int64_t)(int32_t)rA * (int32_t)rB;
                if (oe) { uint8_t o = t != (int32_t)t; c->xer_ov = o; c->xer_so |= o; }
                c->r[d] = (uint32_t)t; rc0(c, w, c->r[d]); goto next;
            }
            case 75: c->r[d] = (uint32_t)(((int64_t)(int32_t)rA * (int32_t)rB) >> 32); rc0(c, w, c->r[d]); goto next;   /* mulhw */
            case 11: c->r[d] = (uint32_t)(((uint64_t)rA * rB) >> 32); rc0(c, w, c->r[d]); goto next;                    /* mulhwu */
            case 491:                                                                                                    /* divw */
                if (oe) { uint8_t o = rB == 0 || (rA == 0x80000000u && rB == 0xFFFFFFFFu); c->xer_ov = o; c->xer_so |= o; }
                c->r[d] = ppc_divw(rA, rB); rc0(c, w, c->r[d]); goto next;
            case 459:                                                                                                    /* divwu */
                if (oe) { uint8_t o = rB == 0; c->xer_ov = o; c->xer_so |= o; }
                c->r[d] = ppc_divwu(rA, rB); rc0(c, w, c->r[d]); goto next;
            }
            uint32_t ea = ra0 + rB;  /* indexed forms */
            uint32_t spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5);
            switch (xo) {
            /* logical (rS = d) */
            case 28: c->r[a] = rS & rB; rc0(c, w, c->r[a]); break;       /* and */
            case 444: c->r[a] = rS | rB; rc0(c, w, c->r[a]); break;      /* or, mr */
            case 316: c->r[a] = rS ^ rB; rc0(c, w, c->r[a]); break;      /* xor */
            case 60: c->r[a] = rS & ~rB; rc0(c, w, c->r[a]); break;      /* andc */
            case 412: c->r[a] = rS | ~rB; rc0(c, w, c->r[a]); break;     /* orc */
            case 476: c->r[a] = ~(rS & rB); rc0(c, w, c->r[a]); break;   /* nand */
            case 124: c->r[a] = ~(rS | rB); rc0(c, w, c->r[a]); break;   /* nor */
            case 284: c->r[a] = ~(rS ^ rB); rc0(c, w, c->r[a]); break;   /* eqv */
            case 24: c->r[a] = (rB & 0x20) ? 0 : rS << (rB & 31); rc0(c, w, c->r[a]); break;  /* slw */
            case 536: c->r[a] = (rB & 0x20) ? 0 : rS >> (rB & 31); rc0(c, w, c->r[a]); break; /* srw */
            case 792: {                                                  /* sraw */
                uint32_t n = rB & 0x3F;
                int32_t s = (int32_t)rS;
                if (n > 31) { c->r[a] = (uint32_t)(s >> 31); c->xer_ca = s < 0; }
                else { c->r[a] = (uint32_t)(s >> n); c->xer_ca = s < 0 && n && ((uint32_t)s << (32 - n)) != 0; }
                rc0(c, w, c->r[a]);
                break;
            }
            case 824: {                                                  /* srawi */
                int32_t s = (int32_t)rS;
                if (b == 0) { c->r[a] = rS; c->xer_ca = 0; }
                else { c->xer_ca = s < 0 && (s & ((1u << b) - 1)); c->r[a] = (uint32_t)(s >> b); }
                rc0(c, w, c->r[a]);
                break;
            }
            case 26: c->r[a] = rS ? (uint32_t)__builtin_clz(rS) : 32; rc0(c, w, c->r[a]); break;   /* cntlzw */
            case 954: c->r[a] = (uint32_t)(int32_t)(int8_t)rS; rc0(c, w, c->r[a]); break;          /* extsb */
            case 922: c->r[a] = (uint32_t)(int32_t)(int16_t)rS; rc0(c, w, c->r[a]); break;         /* extsh */
            /* compares */
            case 0: cr_set_s(c, d >> 2, (int32_t)rA, (int32_t)rB); break;   /* cmp */
            case 32: cr_set_u(c, d >> 2, rA, rB); break;                    /* cmpl */
            /* indexed loads and stores */
            case 23: c->r[d] = ld32(ea); break;                                         /* lwzx */
            case 87: c->r[d] = ld8(ea); break;                                          /* lbzx */
            case 279: c->r[d] = ld16(ea); break;                                        /* lhzx */
            case 343: c->r[d] = (uint32_t)(int32_t)(int16_t)ld16(ea); break;            /* lhax */
            case 55: ea = rA + rB; c->r[d] = ld32(ea); c->r[a] = ea; break;             /* lwzux */
            case 119: ea = rA + rB; c->r[d] = ld8(ea); c->r[a] = ea; break;             /* lbzux */
            case 311: ea = rA + rB; c->r[d] = ld16(ea); c->r[a] = ea; break;            /* lhzux */
            case 375: ea = rA + rB; c->r[d] = (uint32_t)(int32_t)(int16_t)ld16(ea); c->r[a] = ea; break;  /* lhaux */
            case 151: WATCHED(ea, 4, st32(ea_, rS)); break;                             /* stwx */
            case 215: WATCHED(ea, 1, st8(ea_, (uint8_t)rS)); break;                     /* stbx */
            case 407: WATCHED(ea, 2, st16(ea_, (uint16_t)rS)); break;                   /* sthx */
            case 183: ea = rA + rB; WATCHED(ea, 4, st32(ea_, rS)); c->r[a] = ea; break; /* stwux */
            case 247: ea = rA + rB; WATCHED(ea, 1, st8(ea_, (uint8_t)rS)); c->r[a] = ea; break;    /* stbux */
            case 439: ea = rA + rB; WATCHED(ea, 2, st16(ea_, (uint16_t)rS)); c->r[a] = ea; break;  /* sthux */
            case 534: c->r[d] = __builtin_bswap32(ld32(ea)); break;                     /* lwbrx */
            case 790: c->r[d] = __builtin_bswap16(ld16(ea)); break;                     /* lhbrx */
            case 662: WATCHED(ea, 4, st32(ea_, __builtin_bswap32(rS))); break;          /* stwbrx */
            case 918: WATCHED(ea, 2, st16(ea_, __builtin_bswap16((uint16_t)rS))); break; /* sthbrx */
            case 20: c->r[d] = ppc_lwarx(c, ea); break;                                 /* lwarx */
            case 150:                                                                   /* stwcx. */
                if (in_game_code(ea, 4)) { uint8_t old_[4]; memcpy(old_, ppc_ptr(ea), 4); ppc_stwcx(c, ea, rS); log_code_store(I, pc, ea, 4, old_); }
                else ppc_stwcx(c, ea, rS);
                break;
            case 597: {                                                                 /* lswi */
                uint32_t n = b ? b : 32, r = (d - 1) & 31, base = ra0;
                for (uint32_t i = 0; i < n; i++) {
                    if (i % 4 == 0) { r = (r + 1) & 31; c->r[r] = 0; }
                    c->r[r] |= (uint32_t)ld8(base + i) << (24 - 8 * (i % 4));
                }
                break;
            }
            case 725: {                                                                 /* stswi */
                uint32_t n = b ? b : 32, r = (d - 1) & 31, base = ra0;
                for (uint32_t i = 0; i < n; i++) {
                    if (i % 4 == 0) r = (r + 1) & 31;
                    WATCHED(base + i, 1, st8(ea_, (uint8_t)(c->r[r] >> (24 - 8 * (i % 4)))));
                }
                break;
            }
            /* floating point, indexed */
            case 535: case 567: {                                                       /* lfsx lfsux */
                if (xo == 567) ea = rA + rB;
                double v = ldf32(ea); c->f[d].ps0 = v; c->f[d].ps1 = v;
                if (xo == 567) c->r[a] = ea;
                break;
            }
            case 599: case 631:                                                         /* lfdx lfdux */
                if (xo == 631) ea = rA + rB;
                c->f[d].ps0 = ldf64(ea);
                if (xo == 631) c->r[a] = ea;
                break;
            case 663: case 695:                                                         /* stfsx stfsux */
                if (xo == 695) ea = rA + rB;
                WATCHED(ea, 4, stf32(ea_, c->f[d].ps0));
                if (xo == 695) c->r[a] = ea;
                break;
            case 727: case 759:                                                         /* stfdx stfdux */
                if (xo == 759) ea = rA + rB;
                WATCHED(ea, 8, stf64(ea_, c->f[d].ps0));
                if (xo == 759) c->r[a] = ea;
                break;
            case 983: WATCHED(ea, 4, st32(ea_, (uint32_t)f64_as_u64(c->f[d].ps0))); break;  /* stfiwx */
            /* cache and ordering */
            case 1014:                                                                  /* dcbz */
                if (in_game_code(ea & ~31u, 32)) {
                    uint8_t old_[32];
                    memcpy(old_, ppc_ptr(ea & ~31u), 32);
                    ppc_dcbz(ea);
                    log_code_store(I, pc, ea & ~31u, 32, old_);
                } else ppc_dcbz(ea);
                break;
            case 598: case 854: __atomic_thread_fence(__ATOMIC_SEQ_CST); break;        /* sync eieio */
            case 86: case 54: case 278: case 246: case 470: case 982: break;            /* dcbf dcbst dcbt dcbtst dcbi icbi */
            /* supervisor state */
            case 83: c->r[d] = ppc_mfmsr(c); break;                                     /* mfmsr */
            case 146: ppc_mtmsr(c, rS); break;                                          /* mtmsr */
            case 595: case 659: c->r[d] = 0; break;                                     /* mfsr mfsrin */
            case 210: case 242: case 306: case 566: break;                              /* mtsr mtsrin tlbie tlbsync */
            case 339:                                                                   /* mfspr */
                if (spr == 8) c->r[d] = c->lr;
                else if (spr == 9) c->r[d] = c->ctr;
                else if (spr == 1) c->r[d] = ppc_mfxer(c);
                else if (spr >= 912 && spr <= 919) c->r[d] = c->gqr[spr - 912];
                else if (spr >= 896 && spr <= 903) c->r[d] = c->gqr[spr - 896];
                else if (spr == 268 || spr == 269) c->r[d] = (uint32_t)(ppc_timebase() >> (spr == 268 ? 0 : 32));
                else c->r[d] = ppc_mfspr(c, (int)spr);
                break;
            case 467:                                                                   /* mtspr */
                if (spr == 8) c->lr = rS;
                else if (spr == 9) c->ctr = rS;
                else if (spr == 1) ppc_mtxer(c, rS);
                else if (spr >= 912 && spr <= 919) c->gqr[spr - 912] = rS;
                else if (spr >= 896 && spr <= 903) c->gqr[spr - 896] = rS;
                else ppc_mtspr(c, (int)spr, rS);
                break;
            case 371: c->r[d] = (uint32_t)(ppc_timebase() >> (spr == 268 ? 0 : 32)); break;  /* mftb */
            case 19: c->r[d] = ppc_mfcr(c); break;                                      /* mfcr */
            case 144: ppc_mtcrf(c, (w >> 12) & 0xFF, rS); break;                        /* mtcrf */
            case 512: {                                                                 /* mcrxr */
                uint32_t f = 4 * (d >> 2);
                c->cr[f] = c->xer_so; c->cr[f + 1] = c->xer_ov; c->cr[f + 2] = c->xer_ca; c->cr[f + 3] = 0;
                c->xer_so = c->xer_ov = c->xer_ca = 0;
                break;
            }
            case 4: if (trap_hit(d, rA, rB)) ppc_trap(c, pc); break;                    /* tw */
            default: unhandled(I, pc, w);
            }
            break;
        }
        case 59: {  /* single-precision arithmetic */
            double fa = c->f[a].ps0, fb = c->f[b].ps0, fc = c->f[cc].ps0, v;
            switch ((w >> 1) & 31) {
            case 18: v = to_single(ppc_nan2(fa / fb, fa, fb)); break;                          /* fdivs */
            case 20: v = to_single(ppc_nan2(fa - fb, fa, fb)); break;                          /* fsubs */
            case 21: v = to_single(ppc_nan2(fa + fb, fa, fb)); break;                          /* fadds */
            case 25: v = to_single(ppc_nan2(fa * round25(fc), fa, fc)); break;                 /* fmuls */
            case 28: v = to_single(ppc_nan3(fa * round25(fc) - fb, fa, fc, fb)); break;        /* fmsubs */
            case 29: v = to_single(ppc_nan3(fa * round25(fc) + fb, fa, fc, fb)); break;        /* fmadds */
            case 30: v = to_single(-ppc_nan3(fa * round25(fc) - fb, fa, fc, fb)); break;       /* fnmsubs */
            case 31: v = to_single(-ppc_nan3(fa * round25(fc) + fb, fa, fc, fb)); break;       /* fnmadds */
            case 24: v = to_single(ppc_fres(fb)); break;                                       /* fres */
            default: unhandled(I, pc, w);
            }
            c->f[d].ps0 = v;
            c->f[d].ps1 = v;
            fp_rc(c, w);
            break;
        }
        case 63: {  /* double-precision arithmetic and FPSCR */
            double fa = c->f[a].ps0, fb = c->f[b].ps0, fc = c->f[cc].ps0;
            uint32_t xo5 = (w >> 1) & 31, xo = (w >> 1) & 0x3FF;
            switch (xo5) {
            case 18: c->f[d].ps0 = ppc_nan2(fa / fb, fa, fb); fp_rc(c, w); goto next;          /* fdiv */
            case 20: c->f[d].ps0 = ppc_nan2(fa - fb, fa, fb); fp_rc(c, w); goto next;          /* fsub */
            case 21: c->f[d].ps0 = ppc_nan2(fa + fb, fa, fb); fp_rc(c, w); goto next;          /* fadd */
            case 22: c->f[d].ps0 = ppc_nan2(sqrt(fb), fb, 0.0); fp_rc(c, w); goto next;        /* fsqrt */
            case 23: c->f[d].ps0 = ppc_fsel(fa, fb, fc); fp_rc(c, w); goto next;               /* fsel */
            case 25: c->f[d].ps0 = ppc_nan2(fa * fc, fa, fc); fp_rc(c, w); goto next;          /* fmul */
            case 26: c->f[d].ps0 = ppc_frsqrte(fb); fp_rc(c, w); goto next;                    /* frsqrte */
            case 28: c->f[d].ps0 = ppc_nan3(fma(fa, fc, -fb), fa, fc, fb); fp_rc(c, w); goto next;    /* fmsub */
            case 29: c->f[d].ps0 = ppc_nan3(fma(fa, fc, fb), fa, fc, fb); fp_rc(c, w); goto next;     /* fmadd */
            case 30: c->f[d].ps0 = -ppc_nan3(fma(fa, fc, -fb), fa, fc, fb); fp_rc(c, w); goto next;   /* fnmsub */
            case 31: c->f[d].ps0 = -ppc_nan3(fma(fa, fc, fb), fa, fc, fb); fp_rc(c, w); goto next;    /* fnmadd */
            }
            switch (xo) {
            case 0: case 32: cr_set_f(c, d >> 2, fa, fb); break;                              /* fcmpu fcmpo */
            case 12: { double v = to_single(fb); c->f[d].ps0 = v; c->f[d].ps1 = v; fp_rc(c, w); break; }  /* frsp */
            case 15: c->f[d].ps0 = u64_as_f64(ppc_fctiwz(fb)); fp_rc(c, w); break;            /* fctiwz */
            case 14: c->f[d].ps0 = u64_as_f64(ppc_fctiw(c, fb)); fp_rc(c, w); break;          /* fctiw */
            case 72: c->f[d].ps0 = fb; fp_rc(c, w); break;                                    /* fmr */
            case 40: c->f[d].ps0 = -fb; fp_rc(c, w); break;                                   /* fneg */
            case 264: c->f[d].ps0 = fabs(fb); fp_rc(c, w); break;                             /* fabs */
            case 136: c->f[d].ps0 = -fabs(fb); fp_rc(c, w); break;                            /* fnabs */
            case 583: c->f[d].ps0 = u64_as_f64(0xFFF8000000000000ull | c->fpscr); break;      /* mffs */
            case 711: {                                                                       /* mtfsf */
                uint32_t fm = (w >> 17) & 0xFF, m = 0;
                for (int i = 0; i < 8; i++)
                    if (fm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
                c->fpscr = (c->fpscr & ~m) | ((uint32_t)f64_as_u64(fb) & m);
                break;
            }
            case 134: {                                                                       /* mtfsfi */
                uint32_t sh = 28 - 4 * (d >> 2);
                c->fpscr = (c->fpscr & ~(0xFu << sh)) | (((w >> 12) & 0xF) << sh);
                break;
            }
            case 38: c->fpscr |= 0x80000000u >> d; break;                                     /* mtfsb1 */
            case 70: c->fpscr &= ~(0x80000000u >> d); break;                                  /* mtfsb0 */
            case 64: {                                                                        /* mcrfs */
                uint32_t v = (c->fpscr >> (28 - 4 * (a >> 2))) & 0xF, f = 4 * (d >> 2);
                c->cr[f] = v >> 3; c->cr[f + 1] = (v >> 2) & 1; c->cr[f + 2] = (v >> 1) & 1; c->cr[f + 3] = v & 1;
                break;
            }
            default: unhandled(I, pc, w);
            }
            break;
        }
        case 4: {  /* paired singles */
            double A0 = c->f[a].ps0, A1 = c->f[a].ps1, B0 = c->f[b].ps0, B1 = c->f[b].ps1;
            double C0 = c->f[cc].ps0, C1 = c->f[cc].ps1, v0, v1;
            uint32_t xo5 = (w >> 1) & 31, xo = (w >> 1) & 0x3FF;
#define MUL(x, y) ppc_nan2((x) * round25(y), x, y)
#define MAD(x, y, o, z) ppc_nan3((x) * round25(y) o (z), x, y, z)
            switch (xo5) {
            case 10: c->f[d].ps0 = to_single(ppc_nan2(A0 + B1, A0, B1)); c->f[d].ps1 = C1; goto next;  /* ps_sum0 */
            case 11: v1 = to_single(ppc_nan2(A0 + B1, A0, B1)); c->f[d].ps0 = C0; c->f[d].ps1 = v1; goto next;  /* ps_sum1 */
            case 12: v0 = MUL(A0, C0); v1 = MUL(A1, C0); goto pair;                         /* ps_muls0 */
            case 13: v0 = MUL(A0, C1); v1 = MUL(A1, C1); goto pair;                         /* ps_muls1 */
            case 14: v0 = MAD(A0, C0, +, B0); v1 = MAD(A1, C0, +, B1); goto pair;           /* ps_madds0 */
            case 15: v0 = MAD(A0, C1, +, B0); v1 = MAD(A1, C1, +, B1); goto pair;           /* ps_madds1 */
            case 18: v0 = ppc_nan2(A0 / B0, A0, B0); v1 = ppc_nan2(A1 / B1, A1, B1); goto pair;  /* ps_div */
            case 20: v0 = ppc_nan2(A0 - B0, A0, B0); v1 = ppc_nan2(A1 - B1, A1, B1); goto pair;  /* ps_sub */
            case 21: v0 = ppc_nan2(A0 + B0, A0, B0); v1 = ppc_nan2(A1 + B1, A1, B1); goto pair;  /* ps_add */
            case 23: c->f[d].ps0 = ppc_fsel(A0, B0, C0); c->f[d].ps1 = ppc_fsel(A1, B1, C1); goto next;  /* ps_sel */
            case 24: v0 = ppc_fres(B0); v1 = ppc_fres(B1); goto pair;                        /* ps_res */
            case 25: v0 = MUL(A0, C0); v1 = MUL(A1, C1); goto pair;                         /* ps_mul */
            case 26: v0 = ppc_frsqrte(B0); v1 = ppc_frsqrte(B1); goto pair;                  /* ps_rsqrte */
            case 28: v0 = MAD(A0, C0, -, B0); v1 = MAD(A1, C1, -, B1); goto pair;           /* ps_msub */
            case 29: v0 = MAD(A0, C0, +, B0); v1 = MAD(A1, C1, +, B1); goto pair;           /* ps_madd */
            case 30: v0 = -MAD(A0, C0, -, B0); v1 = -MAD(A1, C1, -, B1); goto pair;         /* ps_nmsub */
            case 31: v0 = -MAD(A0, C0, +, B0); v1 = -MAD(A1, C1, +, B1); goto pair;         /* ps_nmadd */
            case 6: case 7: {                                                               /* psq_lx psq_stx psq_lux psq_stux */
                uint32_t xo6 = (w >> 1) & 0x3F;
                int upd = xo6 == 38 || xo6 == 39, pw = (w >> 10) & 1, pi = (w >> 7) & 7;
                uint32_t ea = (upd ? c->r[a] : ra0) + c->r[b];
                if (xo5 == 6) psq_load(c, d, ea, pw, pi);
                else WATCHED(ea, 8, psq_store(c, d, ea_, pw, pi));
                if (upd) c->r[a] = ea;
                goto next;
            }
            }
#undef MUL
#undef MAD
            switch (xo) {
            case 0: case 32: cr_set_f(c, d >> 2, A0, B0); break;                             /* ps_cmpu0 ps_cmpo0 */
            case 64: case 96: cr_set_f(c, d >> 2, A1, B1); break;                            /* ps_cmpu1 ps_cmpo1 */
            case 40: c->f[d].ps0 = -B0; c->f[d].ps1 = -B1; fp_rc(c, w); break;               /* ps_neg */
            case 72: c->f[d].ps0 = B0; c->f[d].ps1 = B1; fp_rc(c, w); break;                 /* ps_mr */
            case 136: c->f[d].ps0 = -fabs(B0); c->f[d].ps1 = -fabs(B1); fp_rc(c, w); break;  /* ps_nabs */
            case 264: c->f[d].ps0 = fabs(B0); c->f[d].ps1 = fabs(B1); fp_rc(c, w); break;    /* ps_abs */
            case 528: c->f[d].ps0 = A0; c->f[d].ps1 = B0; fp_rc(c, w); break;                /* ps_merge00 */
            case 560: c->f[d].ps0 = A0; c->f[d].ps1 = B1; fp_rc(c, w); break;                /* ps_merge01 */
            case 592: c->f[d].ps0 = A1; c->f[d].ps1 = B0; fp_rc(c, w); break;                /* ps_merge10 */
            case 624: c->f[d].ps0 = A1; c->f[d].ps1 = B1; fp_rc(c, w); break;                /* ps_merge11 */
            case 1014: {                                                                     /* dcbz_l */
                uint32_t ea = ra0 + c->r[b];
                WATCHED(ea & ~31u, 32, ppc_dcbz(ea_));
                break;
            }
            default: unhandled(I, pc, w);
            }
            break;
        pair:
            v0 = to_single(v0);
            v1 = to_single(v1);
            c->f[d].ps0 = v0;
            c->f[d].ps1 = v1;
            fp_rc(c, w);
            break;
        }
        default:
            unhandled(I, pc, w);
        }
    next:
        pc = npc;
    }
}

void ppc_interp(Cpu* c) {
    Interp I = {c, c->pc, c->lr, c->r[1], 0, 0};
    unsigned n = ++g_entries;
    uint32_t prev_fn = g_ppc_cur_fn;
    first_entry(&I);
    g_ppc_cur_fn = I.start;
    run(&I);
    g_ppc_cur_fn = prev_fn;
    g_steps_total += I.steps;
    if (n <= LOG_ENTRIES)
        rt_log("[interp] entry %u at %08X: %llu instructions, back to %08X", n, I.start, I.steps, I.entry_lr);
    else if (n % SUMMARY_EVERY == 0)
        rt_log("[interp] %u entries so far, %llu instructions in all, %u stores to the game's code", n, g_steps_total,
               g_code_stores);
}

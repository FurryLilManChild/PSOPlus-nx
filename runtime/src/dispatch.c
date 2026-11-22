/* Indirect calls and jumps of the recompiled code (bctrl, bctr, blrl): guest address -> recompiled function.
 *
 * The table (build/gen/table.c, g_recomp_funcs) is sorted by address: about 29,000 entries of 16 bytes,
 * 460 KB. A binary search over it is about 15 dependent loads and as many hard-to-predict branches, and
 * the generated code has about 5,900 indirect call sites (PSO is C++: virtual calls, and state tables
 * such as the material loop in 0x803DBE1C). The Switch's sampling profiler charges that search to the
 * function entered last (test 10: 4.3% for 0x803DB560, the tiny function called just before the
 * material loop's indirect call).
 *
 * So a direct-mapped cache of 4096 recent targets sits in front of the search. An entry is one 64-bit
 * word: the guest address in the high half, the host function as a 32-bit offset from ppc_dispatch in
 * the low half (0 = empty: no table entry points at ppc_dispatch itself). One word, read and written
 * with relaxed atomics, so a reader on another host thread sees either a whole old entry or a whole new
 * one; both are valid, because the table never changes. The function called for an address is the one
 * the binary search finds, always: the cache only remembers results of that search, and an address
 * that is not a function entry is never cached (it ends in rt_fatal as before). */
#include <stdint.h>

#include "recomp_table.h"
#include "runtime.h"

#define DISPATCH_CACHE_BITS 12
static uint64_t g_dispatch_cache[1u << DISPATCH_CACHE_BITS];
static unsigned long long g_dispatch_calls, g_dispatch_misses;  /* statistics for the [dispatch] line */

static inline unsigned dispatch_slot(uint32_t a) {
    return ((a >> 2) ^ (a >> (2 + DISPATCH_CACHE_BITS))) & ((1u << DISPATCH_CACHE_BITS) - 1);
}

/* the binary search, as ppc_dispatch did it before the cache: NULL when a is not a function entry */
static PpcFunc dispatch_search(uint32_t a) {
    unsigned lo = 0, hi = g_recomp_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_recomp_funcs[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return lo < g_recomp_func_count && g_recomp_funcs[lo].addr == a ? g_recomp_funcs[lo].fn : (PpcFunc)0;
}

PpcFunc ppc_dispatch_lookup(uint32_t a);  /* also used by tools/tests/dispatch_cache_test.c */
PpcFunc ppc_dispatch_lookup(uint32_t a) {
    uint64_t* slot = &g_dispatch_cache[dispatch_slot(a)];
    uint64_t e = __atomic_load_n(slot, __ATOMIC_RELAXED);
    uintptr_t base = (uintptr_t)&ppc_dispatch;
    if (__builtin_expect((++g_dispatch_calls & ((1ull << 24) - 1)) == 0, 0))
        rt_log("[dispatch] %llu indirect calls so far, %llu of them not in the cache", g_dispatch_calls, g_dispatch_misses);
    if (__builtin_expect((uint32_t)(e >> 32) == a && (uint32_t)e != 0, 1))
        return (PpcFunc)(base + (uintptr_t)(intptr_t)(int32_t)(uint32_t)e);
    g_dispatch_misses++;
    PpcFunc fn = dispatch_search(a);
    if (fn) {
        intptr_t off = (intptr_t)((uintptr_t)fn - base);
        if (off != 0 && off == (intptr_t)(int32_t)off)  /* always, for one executable image */
            __atomic_store_n(slot, ((uint64_t)a << 32) | (uint32_t)(int32_t)off, __ATOMIC_RELAXED);
    }
    return fn;
}

void ppc_dispatch(Cpu* c) {
    uint32_t a = c->pc;
    PpcFunc fn = ppc_dispatch_lookup(a);
    if (__builtin_expect(fn != 0, 1)) {
        fn(c);
        return;
    }
    rt_fatal("dispatch to %08X: not a function entry (lr %08X)", a, c->lr);
}

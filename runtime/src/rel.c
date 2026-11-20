/* OSLink / OSUnlink for a module recompiled at a fixed address (gcrecomp.py REL_BASE).
 *
 * PSO: PsoV3.dol reads movie.rel (CRI's Sofdec player) into a heap buffer and calls OSLink on it before
 * each movie (0x80320478), then OSUnlink after it (0x80320398). The recompiled module code lives at the
 * fixed base it was translated for instead. So OSLink lays the fixed image out fresh (rel_relink: data
 * back to its first state, bss zeroed), finishes the game's header the way Link would (prolog, epilog
 * and unresolved become absolute addresses: here, those of the fixed image) and reports success; the
 * game then calls module->prolog, which reaches the recompiled code. OSUnlink has nothing to undo.
 * Based on sonic-riders-recomp's rel.c (MPL-2.0). */
#include "recomp_table.h"
#include "runtime.h"

void rel_relink(void);  /* loader.c */

static int g_linked;

void hle_OSLink(Cpu* c) {
    uint32_t m = c->r[3], bss = c->r[4];
    uint32_t id = ld32(m + 0x00), version = ld32(m + 0x1C);
    if (!g_rel_base || id != ld32(g_rel_base + 0x00) || version != ld32(g_rel_base + 0x1C))
        rt_fatal("OSLink of an unexpected module (id %u version %u at %08X); only %s is precompiled", id, version, m,
                 g_rel_file[0] ? g_rel_file : "(none)");
    if (g_linked) rt_log("[rel] OSLink while %s is still linked (no OSUnlink in between)", g_rel_file);
    rel_relink();
    st32(m + 0x34, g_rel_prolog);
    st32(m + 0x38, g_rel_epilog);
    st32(m + 0x3C, g_rel_unresolved);
    rt_log("[rel] OSLink(%08X, bss %08X): %s (module %u) runs from its fixed image at %08X (prolog %08X, epilog %08X)",
           m, bss, g_rel_file, id, g_rel_base, g_rel_prolog, g_rel_epilog);
    g_linked = 1;
    c->r[3] = 1;
}

void hle_OSUnlink(Cpu* c) {
    rt_log("[rel] OSUnlink(%08X): %s", c->r[3], g_rel_file);
    g_linked = 0;
    c->r[3] = 1;
}

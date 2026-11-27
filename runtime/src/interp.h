/* PowerPC interpreter fallback (interp.c): runs guest code that has no recompiled function, such as code a
 * PSO server writes into low memory and makes the game call. Only ppc_dispatch (dispatch.c) enters it. */
#pragma once
#include <stdint.h>
#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when addr looks like code the interpreter can run: 4-byte aligned, in backed guest RAM (the cached
 * 0x8 or uncached 0xC mirror, above the first 0x100 bytes of boot info), and the word there is not 0 */
int ppc_interp_can_run(uint32_t addr);
/* run the guest code at c->pc until it returns to c->lr with r1 as it was (the normal function return) */
void ppc_interp(Cpu* c);

#ifdef __cplusplus
}
#endif

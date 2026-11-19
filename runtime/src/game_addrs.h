/* Addresses in Phantasy Star Online Episode I & II Plus (USA, GPOE8P rev 2) PsoV3.dol that the native
 * runtime uses. Found with decomp-toolkit 1.8.4 (config/GPOE8P), Dolphin's signature database
 * (analysis/sig.txt) and the disassembly of __OSThreadInit (0x80374D64). Low-memory globals are the
 * Dolphin OS ABI. Layout based on sonic-riders-recomp's game_addrs.h (MPL-2.0). */
#pragma once

/* OS globals (low memory) */
#define OS_CURRENT_CONTEXT  0x800000D4u  /* OSContext* */
#define OS_CURRENT_THREAD   0x800000E4u  /* OSThread*  */
#define OS_INTERRUPT_MASK   0x800000C4u  /* __OSInterruptMask (OS) */
#define OS_USER_MASK        0x800000C8u  /* user interrupt mask */

/* OSThread.c statics. This SDK's OSThread is 0x310 bytes (Sonic Riders' newer SDK: 0x318), so the
 * default thread and the idle context sit 8 bytes lower relative to RunQueue than in Riders.
 * __OSThreadInit: DefaultThread = RunQueue + 0x410, IdleContext = RunQueue + 0x720. */
#define OS_RUNQUEUE         0x80543BB8u
#define OS_IDLE_CONTEXT     (OS_RUNQUEUE + 0x720)
#define OS_DEFAULT_THREAD   (OS_RUNQUEUE + 0x410)
#define OS_RUNQUEUE_BITS    0x805D70D0u
#define OS_RUNQUEUE_HINT    0x805D70D4u
#define OS_RESCHEDULE       0x805D70D8u

/* dvd.c statics (hang dumps only): not located yet in PsoV3.dol; 0 skips the dump */
#define DVD_EXECUTING        0u
#define DVD_PAUSE_FLAG       0u
#define DVD_PAUSING_FLAG     0u
#define DVD_FATAL_ERROR_FLAG 0u
#define DVD_CURR_COMMAND     0u
#define DVD_RESUME_FROM_HERE 0u
#define DVD_DUMMY_BLOCK      0u

/* OSThread.c's code (__OSThreadInit 0x80374D64 to the end of OSWakeupThread; OSGetTime follows at 0x80375F08):
 * sched.c's profiler charges time to the first return address outside it */
#define OS_THREAD_CODE_LO 0x80374D64u
#define OS_THREAD_CODE_HI 0x80375F08u

/* functions */
#define FN_OSSaveContext                 0x80371844u
#define FN_OSLoadContext                 0x803718C4u
#define FN_DecrementerExceptionCallback  0x803707CCu
#define FN_OSReschedule                  0x8037542Cu  /* __OSReschedule: SelectThread(0) when RunQueueHint */

/* the generated originals of the C library routines replaced in hle.txt (hle_misc.c falls back to them) */
#define ORIG_memcpy  f_8000E41C_orig
#define ORIG_memset  f_8000E334_orig
#define ORIG_memmove f_8039A568_orig

/* the boot block the switcher hands to PsoV3.dol (main.dol 0x80035558 copies it to 0x80003100). The game
 * checks for "SEGA" at +0x110 and builds its own defaults when it is missing; the port boots PsoV3.dol
 * directly and leaves it empty. */
#define PSO_BOOT_BLOCK       0x80003100u

/* OSContext layout */
#define CTX_GPR   0x000
#define CTX_CR    0x080
#define CTX_LR    0x084
#define CTX_CTR   0x088
#define CTX_XER   0x08C
#define CTX_FPR   0x090
#define CTX_SRR0  0x198
#define CTX_SRR1  0x19C
#define CTX_STATE 0x1A2
#define CTX_GQR   0x1A4

#define MSR_EE 0x8000u

/* Switch platform layer: SD-card paths and log, threads, the guest memory window.
 *
 * Files (sdmc:/switch/psoplus/):
 *   psoplus.nro     the program
 *   *.iso               the user's own disc image (GPOE8P rev 2, the first .iso or .gcm found)
 *   saves/card_a.raw    memory card in slot A (created formatted on first start)
 *   psoplus.log     log of the last session
 *
 * Guest memory follows SwitchWakerHD (runtime/src/platform/core_switch.cpp): Horizon commits every
 * mapping, so a code-memory region is reserved for the whole physical window and only MEM1 (+ the
 * REL area) and the locked cache are backed, by mapping heap pages there. That needs
 * svcMapProcessCodeMemory, available when the Homebrew Menu runs in title mode (hold R while
 * starting a game); otherwise svcMapMemory into the stack region is used. */
#include <dirent.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "chat_kbd.h"
#include "net_native.h"
#include "runtime.h"

#define BASE_DIR "sdmc:/switch/psoplus"

/* newlib declares C11 timespec_get but libnx does not provide it; CLOCK_REALTIME is also the clock
 * of cnd_timedwait's absolute deadlines */
int timespec_get(struct timespec* ts, int base) {
    if (base != TIME_UTC || clock_gettime(CLOCK_REALTIME, ts)) return 0;
    return base;
}

/* Sleep mode, HOME menu: the system's applet messages are logged (with how far the CPU tick counter
 * moved since the last one), and a monitor reports a stall: no new frame for 5 s while the game has
 * focus -> where every thread is (once per stall). It also logs a jump of the tick counter (which
 * shows whether the counter runs on while the console sleeps). */
static AppletHookCookie g_hook;
_Atomic int g_switch_log_input;  /* input_hid.c logs the next button press */
static void clk_on_applet(AppletHookType type);  /* the CPU clock setting, below */
static void applet_hook(AppletHookType type, void* param) {
    (void)param;
    static uint64_t last;
    uint64_t now = armGetSystemTick();
    static const char* const names[] = {"focus state", "operation mode", "performance mode", "exit request",
                                        "resume (after sleep)", "capture button", "screenshot", "request to display"};
    rt_log("[applet] %s; focus %d, %.1f s since the previous message", type < 8 ? names[type] : "?",
           (int)appletGetFocusState(), last ? armTicksToNs(now - last) / 1e9 : 0.0);
    last = now;
    if (type == AppletHookType_OnResume || (type == AppletHookType_OnFocusState && appletGetFocusState() == AppletFocusState_InFocus))
        g_switch_log_input = 1;
    /* online (net_native.c): back after 15 s or more away (HOME or sleep) the connections are ended */
    if (type == AppletHookType_OnResume) net_platform_event(NET_EV_RESUME);
    if (type == AppletHookType_OnFocusState) {  /* time away counts only in the background (HOME, sleep), not while
                                                    an applet such as the chat keyboard is in front (OutOfFocus) */
        AppletFocusState fs = appletGetFocusState();
        net_platform_event(fs == AppletFocusState_InFocus ? NET_EV_FOCUS_BACK
                           : fs == AppletFocusState_Background ? NET_EV_FOCUS_LOST : NET_EV_FOCUS_OUT);
    }
    if (type == AppletHookType_OnFocusState || type == AppletHookType_OnResume || type == AppletHookType_OnOperationMode ||
        type == AppletHookType_OnPerformanceMode)
        clk_on_applet(type);
    fflush(stderr);  /* rare; the log must hold it if the game hangs right after */
}

/* Handheld GPU clock: Nintendo's own 384 MHz configuration for games (the system default in handheld
 * is 307.2 MHz; CPU 1020 and memory 1331 MHz stay), as SwitchWakerHD does (its round 29: +5 fps where
 * the GPU was the limit). Not an overclock: an official profile, selected through apm, which needs
 * title mode. Only the handheld ("Normal") mode is changed; docked keeps the system's 768 MHz. The
 * previous configuration is restored at exit. SR_HANDHELD_GPU=0 (env.txt) leaves the clocks alone. */
#define APM_CONF_HANDHELD_GPU384 0x00020004u
static u32 g_apm_prev;
static int g_apm_set;
static void apm_restore(void) {
    if (g_apm_set) apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, g_apm_prev);
}
static int g_apm_ready = -1;  /* -1 not tried, 0 unavailable, 1 ready (previous configuration saved) */
void platform_set_handheld_gpu(int on) {
    const char* e = getenv("SR_HANDHELD_GPU");
    if (e && atoi(e) == 0) on = 0;  /* env.txt override: leave the clocks alone */
    if (g_apm_ready < 0) {
        if (!on) return;  /* nothing to change and nothing to restore yet */
        g_apm_ready = 0;
        Result rc = apmInitialize();
        if (R_FAILED(rc)) {
            rt_log("[switch] apm unavailable (0x%X; applet mode?): handheld GPU stays at 307 MHz", rc);
            return;
        }
        if (R_FAILED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &g_apm_prev))) return;
        g_apm_ready = 1;
        atexit(apm_restore);
    }
    if (g_apm_ready != 1) return;
    u32 want = on ? APM_CONF_HANDHELD_GPU384 : g_apm_prev;
    Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, want);
    g_apm_set = R_SUCCEEDED(rc);
    rt_log("[switch] handheld performance configuration -> 0x%08X (%s): rc 0x%X", want, on ? "GPU 384 MHz" : "system default", rc);
}

/* ---- clocks: reading them ([pace] lines, the menu) and the menu's CPU clock setting ----
 * clkrst (system 8.0+) or pcv (older systems), one session per clock, shared by every caller under
 * g_clk_lock (the game thread's [pace] line, the render thread's menu and applet messages, the watch on
 * the stall thread).
 *
 * The CPU setting (Settings.cpu_clock, 0 = stock): set with clkrstSetClockRate / pcvSetClockRate only while
 * the game has focus. Focus lost (HOME menu, sleep, a system screen such as the controller setup) or exit:
 * the CPU is set back to 1020 MHz, the clock every game profile uses; focus back, a resume or a docked /
 * handheld change: set again. While an override is set, clock_override.txt exists, so a launch after a
 * crash sets the CPU back to stock first and logs it. Another tool (sys-clk with a profile) can set the
 * clock after us: the watch (once a second, clock_watch) reads it back, sets it once more after a mismatch
 * (a docked / handheld change can land after our message), and when it still differs reports another tool
 * in the log and the menu. Nothing here talks to sys-clk. */
#define CPU_STOCK_HZ 1020000000u
#define CLOCK_MARKER BASE_DIR "/clock_override.txt"
static Mutex g_clk_lock;
static int g_clk_mode = -1;  /* -1 not tried, 0 none, 1 clkrst, 2 pcv */
static ClkrstSession g_clk_ses[3];
static u32 g_cpu_want_hz;     /* the menu's choice, 0 = stock (nothing set) */
static volatile int g_cpu_applied;  /* our rate is set now */
static int g_cpu_reset_done;  /* the watch set it again once since the last event */
static int g_cpu_mismatch;    /* reads in a row that differ */
static _Atomic int g_clk_other;  /* another tool sets the CPU clock */
static _Atomic int g_clk_failed;  /* the last attempt to set the CPU clock was refused */
static _Atomic u32 g_clk_now[3];  /* last read: CPU, GPU, memory (Hz; 0 unknown) */
static _Atomic u64 g_clk_asked;  /* tick of the last platform_clock_info call (the menu wants reads) */

static void clk_init_locked(void) {
    if (g_clk_mode >= 0) return;
    g_clk_mode = 0;
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) {
        if (R_SUCCEEDED(clkrstOpenSession(&g_clk_ses[0], PcvModuleId_CpuBus, 3)) &&
            R_SUCCEEDED(clkrstOpenSession(&g_clk_ses[1], PcvModuleId_GPU, 3)) &&
            R_SUCCEEDED(clkrstOpenSession(&g_clk_ses[2], PcvModuleId_EMC, 3)))
            g_clk_mode = 1;
    }
    if (!g_clk_mode && R_SUCCEEDED(pcvInitialize())) g_clk_mode = 2;
    rt_log("[clocks] read and set through %s", g_clk_mode == 1 ? "clkrst" : g_clk_mode == 2 ? "pcv" : "nothing (unavailable)");
}

static void clk_read_locked(u32 hz[3]) {
    hz[0] = hz[1] = hz[2] = 0;
    if (g_clk_mode == 1) {
        for (int i = 0; i < 3; i++) clkrstGetClockRate(&g_clk_ses[i], &hz[i]);
    } else if (g_clk_mode == 2) {
        pcvGetClockRate(PcvModule_CpuBus, &hz[0]);
        pcvGetClockRate(PcvModule_GPU, &hz[1]);
        pcvGetClockRate(PcvModule_EMC, &hz[2]);
    }
    for (int i = 0; i < 3; i++) g_clk_now[i] = hz[i];
}

static Result clk_set_cpu_locked(u32 hz) {
    if (g_clk_mode == 1) return clkrstSetClockRate(&g_clk_ses[0], hz);
    if (g_clk_mode == 2) return pcvSetClockRate(PcvModule_CpuBus, hz);
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
}

static int clk_has_focus(void) {
    AppletFocusState st = appletGetFocusState();
    return st != AppletFocusState_OutOfFocus && st != AppletFocusState_Background;
}

static int mhz_close(u32 a, u32 b) { return (a > b ? a - b : b - a) < 2000000u; }

/* sets or releases the CPU clock as the setting and the focus ask; force: set again even if set */
static void clk_apply_locked(const char* why, int force) {
    clk_init_locked();
    if (!g_clk_mode) return;
    g_cpu_reset_done = 0;
    g_cpu_mismatch = 0;
    u32 hz[3];
    if (g_cpu_want_hz && clk_has_focus()) {
        if (g_cpu_applied && !force) return;
        if (!g_cpu_applied) {  /* before the change: a crash from here on leaves the marker */
            FILE* m = fopen(CLOCK_MARKER, "w");
            if (m) {
                fprintf(m, "CPU %u MHz set by PSO Episode I and II Plus; removed when set back to stock\n", g_cpu_want_hz / 1000000);
                fclose(m);
            }
        }
        Result rc = clk_set_cpu_locked(g_cpu_want_hz);
        g_clk_failed = R_FAILED(rc);
        g_cpu_applied = R_SUCCEEDED(rc);
        if (!g_cpu_applied) remove(CLOCK_MARKER);
        clk_read_locked(hz);
        rt_log("[clocks] CPU %u MHz requested (%s): rc 0x%X%s, reads back %u MHz", g_cpu_want_hz / 1000000, why, rc,
               g_cpu_applied ? "" : " (refused: the clock stays as it is)", hz[0] / 1000000);
    } else if (g_cpu_applied) {
        Result rc = clk_set_cpu_locked(CPU_STOCK_HZ);
        g_cpu_applied = 0;
        remove(CLOCK_MARKER);
        clk_read_locked(hz);
        rt_log("[clocks] CPU set back to stock %u MHz (%s): rc 0x%X, reads back %u MHz", CPU_STOCK_HZ / 1000000, why, rc,
               hz[0] / 1000000);
    }
}

void platform_set_cpu_clock(int mhz) {
    mutexLock(&g_clk_lock);
    u32 want = mhz > 0 ? (u32)mhz * 1000000u : 0;
    int changed = want != g_cpu_want_hz;
    g_cpu_want_hz = want;
    if (changed) {
        g_clk_other = 0;
        g_clk_failed = 0;
    }
    /* forced: apply() selects the apm configuration just before, which can set the CPU back to 1020 MHz */
    clk_apply_locked(changed ? "menu setting" : "settings applied", 1);
    mutexUnlock(&g_clk_lock);
}

/* applet messages (render thread, inside appletMainLoop) */
static void clk_on_applet(AppletHookType type) {
    const char* why = type == AppletHookType_OnFocusState     ? (clk_has_focus() ? "focus back" : "focus lost")
                      : type == AppletHookType_OnResume        ? "resume"
                      : type == AppletHookType_OnOperationMode ? "docked / handheld change"
                                                               : "performance mode change";
    mutexLock(&g_clk_lock);
    if (g_cpu_want_hz || g_cpu_applied) clk_apply_locked(why, type != AppletHookType_OnFocusState);
    mutexUnlock(&g_clk_lock);
}

/* exit (atexit) and rt_fatal: set back without waiting for the lock (a thread may hold it while it dies) */
void platform_clock_release_at_exit(void) {
    if (!g_cpu_applied || g_clk_mode <= 0) return;
    g_cpu_applied = 0;
    Result rc = clk_set_cpu_locked(CPU_STOCK_HZ);
    remove(CLOCK_MARKER);
    rt_log("[clocks] exit: CPU set back to stock %u MHz: rc 0x%X", CPU_STOCK_HZ / 1000000, rc);
}

/* ---- a crash (data abort, bad instruction, ...): the CPU clock back to stock first ----
 * The kernel hands a CPU exception to the program's entry point; nx-hbloader forwards it to the NRO, whose
 * libnx start code jumps to __libnx_exception_entry. libnx's own version (no __libnx_exception_handler
 * defined, as in this port until now) passes it straight back with svcReturnFromException(0xF801,
 * "unhandled"), and the system then handles the crash as usual (Atmosphere: the error screen and the crash
 * report). The version below does the same, after one thing: if the menu's CPU clock is set, one IPC
 * message to clkrst (or pcv) sets it back to 1020 MHz.
 *
 * Why not libnx's __libnx_exception_handler: libnx runs that after telling the kernel the exception was
 * handled, on another stack, and then calls svcBreak; the crash would then be reported as a break at that
 * point, not as the original fault with its registers. Here the fault is still the one reported: the
 * kernel's saved frame (x0-x8, lr, sp, pc) is never touched, and every other register (x1-x6, x8-x30,
 * sp, q0-q31, fpcr, fpsr) is saved before the call and put back before svcReturnFromException. Only x0
 * (the result code, as in libnx) and x7 (nx-hbloader's forwarding already overwrites it) differ, and the
 * kernel keeps both in the saved frame.
 *
 * The call runs on a stack of its own (the crashing thread's stack may be the fault), takes no lock, does
 * no allocation, file or log output: it reads two variables and sends one message through the crashing
 * thread's own IPC buffer. The kernel lets one thread at a time handle an exception, so one static stack
 * and save area are enough. clock_override.txt is left in place (no file access here), so the next start
 * logs that the last session ended with the override set and sets stock again. If the call itself faulted,
 * the kernel would treat that second fault as unhandled: still a crash report, of the second fault. */
void psoplus_exc_release_clock(void) {
    if (!g_cpu_applied || g_clk_mode <= 0) return;
    g_cpu_applied = 0;
    clk_set_cpu_locked(CPU_STOCK_HZ);
}
__attribute__((aligned(16))) u8 psoplus_exc_stack[16 * 1024];
__attribute__((aligned(16))) u64 psoplus_exc_save[96];  /* x1-x6, x8-x30, sp, fpcr, fpsr; q0-q31 from byte 256 */
__asm__(
    ".section .text.__libnx_exception_entry, \"ax\", %progbits\n"
    ".align 2\n"
    ".global __libnx_exception_entry\n"
    ".type __libnx_exception_entry, %function\n"
    "__libnx_exception_entry:\n"
    "    adrp x7, psoplus_exc_save\n"
    "    add  x7, x7, #:lo12:psoplus_exc_save\n"
    "    stp  x1, x2, [x7, #0]\n"
    "    stp  x3, x4, [x7, #16]\n"
    "    stp  x5, x6, [x7, #32]\n"
    "    stp  x8, x9, [x7, #48]\n"
    "    stp  x10, x11, [x7, #64]\n"
    "    stp  x12, x13, [x7, #80]\n"
    "    stp  x14, x15, [x7, #96]\n"
    "    stp  x16, x17, [x7, #112]\n"
    "    stp  x18, x19, [x7, #128]\n"
    "    stp  x20, x21, [x7, #144]\n"
    "    stp  x22, x23, [x7, #160]\n"
    "    stp  x24, x25, [x7, #176]\n"
    "    stp  x26, x27, [x7, #192]\n"
    "    stp  x28, x29, [x7, #208]\n"
    "    mov  x8, sp\n"
    "    stp  x30, x8, [x7, #224]\n"
    "    mrs  x8, fpcr\n"
    "    mrs  x9, fpsr\n"
    "    stp  x8, x9, [x7, #240]\n"
    "    add  x8, x7, #256\n"
    "    stp  q0, q1, [x8, #0]\n"
    "    stp  q2, q3, [x8, #32]\n"
    "    stp  q4, q5, [x8, #64]\n"
    "    stp  q6, q7, [x8, #96]\n"
    "    stp  q8, q9, [x8, #128]\n"
    "    stp  q10, q11, [x8, #160]\n"
    "    stp  q12, q13, [x8, #192]\n"
    "    stp  q14, q15, [x8, #224]\n"
    "    stp  q16, q17, [x8, #256]\n"
    "    stp  q18, q19, [x8, #288]\n"
    "    stp  q20, q21, [x8, #320]\n"
    "    stp  q22, q23, [x8, #352]\n"
    "    stp  q24, q25, [x8, #384]\n"
    "    stp  q26, q27, [x8, #416]\n"
    "    stp  q28, q29, [x8, #448]\n"
    "    stp  q30, q31, [x8, #480]\n"
    "    adrp x8, psoplus_exc_stack\n"
    "    add  x8, x8, #:lo12:psoplus_exc_stack\n"
    "    add  x8, x8, #0x4000\n"  /* the top of psoplus_exc_stack (16 KB) */
    "    mov  sp, x8\n"
    "    bl   psoplus_exc_release_clock\n"
    "    adrp x7, psoplus_exc_save\n"
    "    add  x7, x7, #:lo12:psoplus_exc_save\n"
    "    add  x8, x7, #256\n"
    "    ldp  q0, q1, [x8, #0]\n"
    "    ldp  q2, q3, [x8, #32]\n"
    "    ldp  q4, q5, [x8, #64]\n"
    "    ldp  q6, q7, [x8, #96]\n"
    "    ldp  q8, q9, [x8, #128]\n"
    "    ldp  q10, q11, [x8, #160]\n"
    "    ldp  q12, q13, [x8, #192]\n"
    "    ldp  q14, q15, [x8, #224]\n"
    "    ldp  q16, q17, [x8, #256]\n"
    "    ldp  q18, q19, [x8, #288]\n"
    "    ldp  q20, q21, [x8, #320]\n"
    "    ldp  q22, q23, [x8, #352]\n"
    "    ldp  q24, q25, [x8, #384]\n"
    "    ldp  q26, q27, [x8, #416]\n"
    "    ldp  q28, q29, [x8, #448]\n"
    "    ldp  q30, q31, [x8, #480]\n"
    "    ldp  x8, x9, [x7, #240]\n"
    "    msr  fpcr, x8\n"
    "    msr  fpsr, x9\n"
    "    ldp  x30, x8, [x7, #224]\n"
    "    mov  sp, x8\n"
    "    ldp  x28, x29, [x7, #208]\n"
    "    ldp  x26, x27, [x7, #192]\n"
    "    ldp  x24, x25, [x7, #176]\n"
    "    ldp  x22, x23, [x7, #160]\n"
    "    ldp  x20, x21, [x7, #144]\n"
    "    ldp  x18, x19, [x7, #128]\n"
    "    ldp  x16, x17, [x7, #112]\n"
    "    ldp  x14, x15, [x7, #96]\n"
    "    ldp  x12, x13, [x7, #80]\n"
    "    ldp  x10, x11, [x7, #64]\n"
    "    ldp  x8, x9, [x7, #48]\n"
    "    ldp  x5, x6, [x7, #32]\n"
    "    ldp  x3, x4, [x7, #16]\n"
    "    ldp  x1, x2, [x7, #0]\n"
    "    mov  w0, #0xf801\n"  /* KERNELRESULT(UnhandledUserInterrupt), as libnx and nx-hbloader */
    "    svc  0x28\n"          /* svcReturnFromException: does not come back */
    "    b    .\n"
    ".size __libnx_exception_entry, . - __libnx_exception_entry\n"
    ".previous\n");

/* start-up: an override left by a session that did not end normally */
static void clk_startup_check(void) {
    FILE* m = fopen(CLOCK_MARKER, "r");
    if (!m) return;
    char line[128] = "";
    if (!fgets(line, sizeof line, m)) line[0] = 0;
    fclose(m);
    line[strcspn(line, "\r\n")] = 0;
    mutexLock(&g_clk_lock);
    clk_init_locked();
    Result rc = clk_set_cpu_locked(CPU_STOCK_HZ);
    u32 hz[3];
    clk_read_locked(hz);
    mutexUnlock(&g_clk_lock);
    remove(CLOCK_MARKER);
    rt_log("[clocks] the last session ended with a CPU override still set (%s): set back to stock %u MHz first: rc 0x%X, reads back %u MHz",
           line, CPU_STOCK_HZ / 1000000, rc, hz[0] / 1000000);
}

/* once a second (stall thread): reads the clocks while the menu shows them or an override
 * is set; checks the override held */
static void clock_watch(void) {
    int asked = armTicksToNs(armGetSystemTick() - g_clk_asked) < 3000000000ull;
    if (!asked && !g_cpu_want_hz) return;
    mutexLock(&g_clk_lock);
    clk_init_locked();
    if (!g_clk_mode) {
        mutexUnlock(&g_clk_lock);
        return;
    }
    u32 hz[3];
    clk_read_locked(hz);
    if (clk_has_focus()) {
        u32 expect = g_cpu_applied ? g_cpu_want_hz : CPU_STOCK_HZ;
        if (g_cpu_want_hz && !g_cpu_applied) expect = 0;  /* not set yet (no focus message yet): nothing to check */
        if (expect && hz[0] && !mhz_close(hz[0], expect)) {
            if (++g_cpu_mismatch >= 2) {
                if (g_cpu_applied && !g_cpu_reset_done) {
                    g_cpu_reset_done = 1;
                    g_cpu_mismatch = 0;
                    Result rc = clk_set_cpu_locked(g_cpu_want_hz);
                    rt_log("[clocks] CPU reads %u MHz, wanted %u MHz: set again: rc 0x%X", hz[0] / 1000000,
                           g_cpu_want_hz / 1000000, rc);
                } else if (!g_clk_other) {
                    g_clk_other = 1;
                    rt_log("[clocks] CPU reads %u MHz, the menu's setting is %u MHz (%s): another tool sets the clock (sys-clk?)",
                           hz[0] / 1000000, expect / 1000000, g_cpu_applied ? "set by the port" : "stock");
                }
            }
        } else if (expect && hz[0]) {
            if (g_clk_other) rt_log("[clocks] CPU reads %u MHz again, as the menu's setting", hz[0] / 1000000);
            g_clk_other = 0;
            g_cpu_mismatch = 0;
        }
    }
    mutexUnlock(&g_clk_lock);
}

void platform_clock_info(PlatformClockInfo* out) {
    g_clk_asked = armGetSystemTick();
    out->available = g_clk_mode != 0;  /* -1 (not tried yet) counts as available */
    out->cpu_mhz = g_clk_now[0] / 1000000;
    out->gpu_mhz = g_clk_now[1] / 1000000;
    out->mem_mhz = g_clk_now[2] / 1000000;
    out->docked = appletGetOperationMode() == AppletOperationMode_Console;
    out->other_tool = g_clk_other;
    out->overridden = g_cpu_applied;
    out->set_failed = g_clk_failed;
}

/* the clocks the console actually runs at (sys-clk or other overclock tools included), for the [pace]
 * lines: "" when neither clkrst nor pcv answers */
void platform_clocks_str(char* out, size_t n) {
    u32 hz[3];
    mutexLock(&g_clk_lock);
    clk_init_locked();
    int mode = g_clk_mode;
    clk_read_locked(hz);
    mutexUnlock(&g_clk_lock);
    if (!mode) { if (n) out[0] = 0; return; }
    snprintf(out, n, ", CPU %u / GPU %u / memory %u MHz %s", hz[0] / 1000000, hz[1] / 1000000, hz[2] / 1000000,
             appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
}

void platform_init(void) {
    appletHook(&g_hook, applet_hook, NULL);  /* before the render thread runs appletMainLoop */
    mkdir(BASE_DIR, 0777);
    mkdir(BASE_DIR "/saves", 0777);
    /* the previous session's log is kept (a relaunch would otherwise erase it) */
    remove(BASE_DIR "/psoplus.prev.log");
    rename(BASE_DIR "/psoplus.log", BASE_DIR "/psoplus.prev.log");
    if (!freopen(BASE_DIR "/psoplus.log", "w", stderr)) return;
    static char logbuf[64 * 1024];
    setvbuf(stderr, logbuf, _IOFBF, sizeof logbuf);  /* flushed by rt_log at most once a second */
    extern const char g_build_version[], g_port_version[];  /* build/<target>/gen/version.c (tools/write_version.py) */
    rt_log("[switch] PSO Episode I and II Plus native port %s (build %s); log started", g_port_version, g_build_version);
    /* debugging switches (SR_WATCHDOG, ...) as KEY=VALUE lines in env.txt, there being no environment */
    FILE* f = fopen(BASE_DIR "/env.txt", "r");
    if (f) {
        static char line[8192];  /* SR_INPUT scripts get long */
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            rt_log("[switch] env %s=%s", line, eq + 1);
        }
        fclose(f);
    }
    clk_startup_check();
    atexit(platform_clock_release_at_exit);
}

const char* platform_disc_path(void) {
    static char path[512] = BASE_DIR "/game.iso";
    DIR* d = opendir(BASE_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > 4 && (!strcasecmp(e->d_name + n - 4, ".iso") || !strcasecmp(e->d_name + n - 4, ".gcm"))) {
                snprintf(path, sizeof path, BASE_DIR "/%s", e->d_name);
                break;
            }
        }
        closedir(d);
    }
    return path;
}

/* SR_CARD=<file> (env.txt): another memory card image (determinism tests run every machine on the same save) */
/* online play (net_native.c): libnx's BSD sockets (bsd:u) and nifm (nifm:u) for the connection's settings.
 * Both are services every homebrew NRO may use (no extra permissions); started once, ended at exit. */
static int g_nifm_ok;
int platform_net_init(void) {
    Result r = socketInitializeDefault();
    Result rn = nifmInitialize(NifmServiceType_User);
    g_nifm_ok = R_SUCCEEDED(rn);
    rt_log("[switch] sockets: socketInitializeDefault 0x%X, nifmInitialize 0x%X", r, rn);
    return R_SUCCEEDED(r) ? 0 : -1;
}
void platform_net_exit(void) {
    if (g_nifm_ok) nifmExit();
    g_nifm_ok = 0;
    socketExit();
}
int platform_net_ipconfig(NetIpConfig* out) {
    if (!g_nifm_ok) return 0;
    u32 ip = 0, mask = 0, gw = 0, d1 = 0, d2 = 0;  /* network byte order, as in_addr.s_addr */
    Result r = nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &d1, &d2);
    if (R_FAILED(r)) {
        rt_log("[switch] nifmGetCurrentIpConfigInfo 0x%X (no connection?)", r);
        return 0;
    }
    memcpy(out->ip, &ip, 4);
    memcpy(out->mask, &mask, 4);
    memcpy(out->gateway, &gw, 4);
    memcpy(out->dns1, &d1, 4);
    memcpy(out->dns2, &d2, 4);
    return 1;
}
/* linked = the console has an address. Not nifm's internet status: that also needs Nintendo's connection test
 * to pass, which a DNS that blocks Nintendo (90DNS and the like) makes fail on a working network */
int platform_net_link(void) {
    if (!g_nifm_ok) return -1;
    u32 ip = 0;
    return R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0;
}

/* chat (chat_kbd.c): the system keyboard. Called on chat_kbd.c's own thread; the game, its network and the
 * render thread run on behind it */
int platform_text_input(const char* header, char* out, size_t n) {
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) {
        rt_log("[switch] swkbdCreate 0x%X", rc);
        return -1;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_Latin);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, "Chat line (letters, digits and ASCII symbols)");
    swkbdConfigSetStringLenMax(&kbd, 80);
    out[0] = 0;
    rc = swkbdShow(&kbd, out, n);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) {
        rt_log("[switch] swkbdShow 0x%X (cancelled?)", rc);
        return 0;
    }
    return 1;
}

const char* platform_save_path(void) { return getenv("SR_CARD") ? getenv("SR_CARD") : BASE_DIR "/saves/card_a.raw"; }
const char* platform_settings_path(void) { return BASE_DIR "/settings.ini"; }
const char* platform_mods_dir(void) { return BASE_DIR "/mods"; }

uint64_t platform_now_ns(void) { return armTicksToNs(armGetSystemTick()); }  /* the CPU's tick counter */
void platform_sleep_ns(uint64_t ns) { svcSleepThread((s64)ns); }

/* the console's local clock (time service), for the GameCube RTC */
void platform_local_time(int* year, int* month, int* day, int* hour, int* minute, int* second) {
    u64 now = 0;
    TimeCalendarTime ct = {0};
    TimeCalendarAdditionalInfo info;
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &now)) || R_FAILED(timeToCalendarTimeWithMyRule(now, &ct, &info))) {
        static int warned;
        if (!warned++) rt_log("[switch] the time service failed: the game's clock starts at 2000-01-01");
        ct.year = 2000; ct.month = 1; ct.day = 1;
    }
    *year = ct.year; *month = ct.month; *day = ct.day;
    *hour = ct.hour; *minute = ct.minute; *second = ct.second;
}

/* ---- threads: libnx gives threads created without attributes a 128 KiB stack ---- */
typedef struct { int (*fn)(void*); void* arg; } Start;
static void* trampoline(void* p) {
    Start s = *(Start*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
void rt_thread_start(int (*fn)(void*), void* arg) {
    Start* s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 4 << 20);
    pthread_t t;
    if (pthread_create(&t, &a, trampoline, s)) rt_fatal("cannot create a thread");
    pthread_attr_destroy(&a);
}

/* Horizon runs the highest-priority ready thread of a core: a busy thread one step below the others
 * yields to them whenever they wake. But threads start at 0x3B (libnx's pthread_create), and 0x3B is the
 * lowest priority this NRO may use: the console logs of tests 12 and 13 show every call failing with
 * 0xE001 (invalid priority 0x3C), so the "lower" threads (the GP thread, the vertex decode helper, the
 * menu's picture thread) have always run at 0x3B like the others on their cores. Since 0.5.0 nothing is
 * asked of the kernel (the result is the same) and one line says so; threads that must give way do it
 * themselves (menu.cpp's picture thread yields between pieces of work). */
void rt_thread_lower_priority(void) {
    s32 prio = 0x3B;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (prio >= 0x3B) {
        static _Atomic int logged;
        if (!atomic_exchange(&logged, 1))
            rt_log("[switch] thread priority 0x%X is already the lowest this program may use: threads asked to run "
                   "lower stay there (equal to the others on their core)", prio);
        return;
    }
    Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)prio + 1);
    if (R_FAILED(rc)) rt_log("[switch] svcSetThreadPriority(%d) failed (0x%X)", prio + 1, rc);
}

/* time the calling thread has run on a CPU (all cores), in ns; 0 when the kernel cannot tell. Against the
 * wall clock it shows how long a thread was ready but not running (another thread had its core). */
uint64_t rt_thread_cpu_ns(void) {
    u64 ticks = 0;
    Result rc = hosversionAtLeast(13, 0, 0) ? svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total)
                                            : svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, TickCountInfo_Total);
    return R_SUCCEEDED(rc) ? armTicksToNs(ticks) : 0;
}

/* let another ready thread of the same priority on this core run (none ready: returns at once) */
void rt_thread_yield(void) { svcSleepThread(YieldType_WithoutCoreMigration); }

/* one step above: preempts the render thread on its core whenever it wakes (the audio mixer must
 * finish each 5 ms frame in time; the render thread can run for a whole video frame) */
void rt_thread_raise_priority(void) {
    s32 prio = 0x2C;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)prio - 1);
    if (R_FAILED(rc)) rt_log("[switch] svcSetThreadPriority(%d) failed (0x%X)", prio - 1, rc);
}

void rt_thread_pin(int core) {
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc)) rt_log("[switch] svcSetThreadCoreMask(%d) failed (0x%X)", core, rc);
}

/* ---- guest memory window (ppc_mem_base_var is defined in memory.c) ---- */

static const struct { uint32_t start, size; } k_ranges[] = {
    {0x00000000, GUEST_RAM_SIZE},              /* MEM1 + the REL area above it */
    {LOCKED_CACHE_PHYS, 0x10000},              /* the 16 KiB locked cache (mapped in 64 KiB) */
};

/* Map heap pages over [window + start, +size) for each guest range. Code memory (title mode) is
 * tried first, as on SwitchWakerHD; svcMapMemory into the stack region is the fallback (also what
 * emulators allow: they do not give homebrew its own process handle). */
static int map_ranges(uint8_t* window, int code) {
    Handle self = envGetOwnProcessHandle();
    for (size_t i = 0; i < sizeof k_ranges / sizeof k_ranges[0]; i++) {
        void* backing = aligned_alloc(0x1000, k_ranges[i].size);
        if (!backing) rt_fatal("out of memory for guest RAM");
        memset(backing, 0, k_ranges[i].size);
        uint8_t* dst = window + k_ranges[i].start;
        Result rc;
        if (code) {
            rc = svcMapProcessCodeMemory(self, (u64)dst, (u64)backing, k_ranges[i].size);
            if (R_SUCCEEDED(rc)) {
                rc = svcSetProcessMemoryPermission(self, (u64)dst, k_ranges[i].size, Perm_Rw);
                if (R_FAILED(rc)) rt_fatal("svcSetProcessMemoryPermission %08X failed (0x%X)", k_ranges[i].start, rc);
            }
        } else {
            rc = svcMapMemory(dst, backing, k_ranges[i].size);
        }
        if (R_FAILED(rc)) {
            if (code && i == 0) {
                free(backing);
                rt_log("[mem] code memory mapping failed (0x%X); using svcMapMemory", rc);
                return 0;
            }
            rt_fatal("%s %08X failed (0x%X)", code ? "svcMapProcessCodeMemory" : "svcMapMemory", k_ranges[i].start, rc);
        }
    }
    return 1;
}

static int try_window(int code) {
    const uint64_t window_size = 0x20010000ull;
    virtmemLock();
    void* window = code ? virtmemFindCodeMemory(window_size, 0x10000) : virtmemFindStack(window_size, 0x10000);
    VirtmemReservation* rv = window ? virtmemAddReservation(window, window_size) : NULL;
    virtmemUnlock();
    if (!rv) rt_fatal("cannot reserve the guest address window");
    if (!map_ranges((uint8_t*)window, code)) {
        virtmemLock();
        virtmemRemoveReservation(rv);
        virtmemUnlock();
        return 0;
    }
    ppc_mem_base_var = (uint8_t*)window;
    rt_log("[mem] guest window at %p (%s)", window, code ? "code memory" : "svcMapMemory");
    return 1;
}

void mem_init(void) {
    int code = envIsSyscallHinted(0x77) && envIsSyscallHinted(0x73) && envGetOwnProcessHandle() != INVALID_HANDLE;
    if (!(code && try_window(1)) && !try_window(0)) rt_fatal("cannot map guest memory");
}

/* Sampling profiler (SR_PROFILE=1 in env.txt): a thread on core 2 reads g_ppc_cur_fn, the guest
 * function entered last on the game core or a PROF_* tag for runtime work, every millisecond, and
 * logs the 30 most frequent every 10 seconds. A sample is charged to the last function entered, so a
 * caller's own code after a call returns counts for the callee: a leaf-heavy approximation, enough
 * to see where the time goes on the hardware (the desktop build samples real instruction
 * pointers, platform/host/profile_win.c). */
#define PROF_BUCKETS 16384u
static struct { uint32_t fn, lr, count; } g_prof[PROF_BUCKETS];
static uint32_t g_prof_samples, g_prof_overflow;

static void prof_report(void) {
    uint32_t n = g_prof_samples;
    if (!n) return;
    rt_log("[profile] %u samples (1 ms) over the last 10 s: last function entered on the game core, and its return address:", n);
    /* the same samples per function, whatever called it (the pairs below split a busy function by caller) */
    {
        static uint32_t fns[PROF_BUCKETS], cnt[PROF_BUCKETS];
        uint32_t nf = 0;
        for (uint32_t i = 0; i < PROF_BUCKETS; i++) {
            if (!g_prof[i].count && !g_prof[i].fn) continue;
            uint32_t c = g_prof[i].count;
            for (uint32_t j = 0; j < nf; j++) if (fns[j] == g_prof[i].fn) { cnt[j] += c; c = 0; break; }
            if (c && nf < PROF_BUCKETS) { fns[nf] = g_prof[i].fn; cnt[nf++] = c; }
        }
        rt_log("[profile] by function:");
        for (int k = 0; k < 40; k++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t j = 0; j < nf; j++) if (cnt[j] > best) { best = cnt[j]; bi = j; }
            if (!best) break;
            rt_log("[profile]   %5.1f%%  %08X", 100.0 * best / n, fns[bi]);
            cnt[bi] = 0;
        }
    }
    rt_log("[profile] by function and return address:");
    for (int k = 0; k < 30; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < PROF_BUCKETS; i++)
            if (g_prof[i].count > best) { best = g_prof[i].count; bi = i; }
        if (!best) break;
        uint32_t fn = g_prof[bi].fn, lr = g_prof[bi].lr;
        const char* tag = fn == PROF_IDLE ? " (idle)" : fn == PROF_DRAW ? " (draw processing)" : fn == PROF_PIPE ? " (GX FIFO)" : fn == PROF_SWITCH ? " (thread switch)" : fn == PROF_RESUMED ? " (resumed thread)" : "";
        if (*tag) rt_log("[profile]   %5.1f%%  %08X%s", 100.0 * best / n, fn, tag);
        else rt_log("[profile]   %5.1f%%  %08X  from %08X", 100.0 * best / n, fn, lr);
        g_prof[bi].count = 0;
    }
    if (g_prof_overflow) rt_log("[profile]   (%u samples lost: table full)", g_prof_overflow);
    memset(g_prof, 0, sizeof g_prof);
    g_prof_samples = g_prof_overflow = 0;
}

static int prof_thread(void* arg) {
    (void)arg;
    rt_thread_pin(2);
    uint64_t next_report = armGetSystemTick() + armNsToTicks(10000000000ull);
    for (;;) {
        svcSleepThread(1000000);
        uint32_t fn = g_ppc_cur_fn, lr = fn >= 0xF0000000u ? 0 : g_ppc_cur_lr;
        uint32_t h = ((fn ^ (lr * 31u)) * 2654435761u) >> 18;
        for (uint32_t probe = 0;; probe++) {
            uint32_t i = (h + probe) & (PROF_BUCKETS - 1);
            if (g_prof[i].fn == fn && g_prof[i].lr == lr && g_prof[i].count) { g_prof[i].count++; break; }
            if (!g_prof[i].count) { g_prof[i].fn = fn; g_prof[i].lr = lr; g_prof[i].count = 1; break; }
            if (probe == 64) { g_prof_overflow++; break; }
        }
        g_prof_samples++;
        if (armGetSystemTick() >= next_report) {
            prof_report();
            next_report = armGetSystemTick() + armNsToTicks(10000000000ull);
        }
    }
    return 0;
}

void sched_dump(void);
void mmio_dump(void);
unsigned long long gx_frames_presented(void);
static int stall_thread(void* arg) {
    (void)arg;
    rt_thread_pin(2);
    unsigned long long last_frames = gx_frames_presented();
    unsigned still = 0;
    int reported = 0;
    uint64_t prev = armGetSystemTick();
    for (;;) {
        svcSleepThread(1000000000);
        clock_watch();
        uint64_t now = armGetSystemTick();
        double gap = armTicksToNs(now - prev) / 1e9;
        if (gap > 3.0) rt_log("[clock] the tick counter moved %.1f s during a 1 s wait (sleep or suspension)", gap);
        prev = now;
        unsigned long long f = gx_frames_presented();
        int rel_is_linked(void);  /* rel.c: a Sofdec movie (movie.rel) writes the XFB directly, no new frames */
        if (f != last_frames || appletGetFocusState() != AppletFocusState_InFocus || rel_is_linked()) {
            if (reported && f != last_frames) rt_log("[stall] frames again after %u s", still);
            last_frames = f;
            still = 0;
            reported = 0;
            continue;
        }
        if (++still == 5 && !reported) {
            reported = 1;
            rt_log("[stall] no new frame for 5 s with focus (frame %llu, retrace %u): state follows", f, vi_field_count());
            mmio_dump();
            sched_dump();
            fflush(stderr);
        }
    }
    return 0;
}

void profile_register_thread(void) {}
void profile_start(void) {
    rt_thread_start(stall_thread, NULL);
    if (!getenv("SR_PROFILE")) return;
    rt_log("[profile] sampling the game core every 1 ms (SR_PROFILE); reports every 10 s");
    rt_thread_start(prof_thread, NULL);
}
void profile_dump(void) {}

void platform_sample_threads(void) {}  /* desktop debugging only */

/* Frame pacing for PSO Episode I & II Plus (PsoV3.dol).
 *
 * The game paces itself: its display flip (Sega's Ninja layer, 0x803DCF2C) sets the next frame buffer,
 * waits until at least two retraces have passed since the previous flip (the interval is set once at
 * boot, 0x8000FB2C via 0x803DD080), then VIFlush and VIWaitForRetrace. With the runtime's VI model
 * raising retraces at 59.94 Hz that gives the original 30 fps cap, so this file does not change the
 * timing. It hooks the flip (config/GPOE8P/hle.txt, "frame_end") only to:
 *   - hold the game thread while the host pauses it (rt_host_pause),
 *   - mark one game frame for perftest.c,
 *   - log the frame rate every 10 s ([pace] lines): frames flipped per second and the game thread's
 *     work per frame (the flip's own wait left out);
 *   - watch for a stall: no flip for 5 s while the host is not pausing the game logs [stall] with the last
 *     function entries (the trace ring, on from the first flip), the hardware registers and every guest
 *     thread with where it is parked, once per stall, and the game keeps running (test 2's black screen
 *     after movie.rel was linked left no trace at all).
 * Based on sonic-riders-recomp's frame_pacing.c (MPL-2.0); Riders' 30/60 fps modes are not used here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "game_addrs.h"
#include "settings.h"

void f_803DCF2C_orig(Cpu* c);  /* the game's display flip (generated) */

/* the checksum of 1 MB block `blk` of guest RAM (SR_RAMHASH; netplay.c's per-frame state checksum) */
uint32_t ram_block_hash(uint32_t blk) {
    const uint64_t* p = (const uint64_t*)(mem_phys(0) + ((size_t)blk << 20));
    uint64_t h0 = 0x9E3779B97F4A7C15ull, h1 = 0, h2 = 0, h3 = 0;  /* cheap: 4 lanes, add/xor/rotate */
    for (size_t i = 0; i < (1u << 20) / 8; i += 4) {
        h0 = ((h0 << 5) | (h0 >> 59)) ^ p[i];
        h1 = ((h1 << 7) | (h1 >> 57)) + p[i + 1];
        h2 = ((h2 << 11) | (h2 >> 53)) ^ p[i + 2];
        h3 = ((h3 << 13) | (h3 >> 51)) + p[i + 3];
    }
    uint64_t h = (h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7)) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 32));
}

/* the host pauses the game (window minimized, HOME menu): the game thread waits at its next flip */
static volatile int g_host_pause;
void rt_host_pause(int on) { g_host_pause = on; rt_clock_hold(on); }  /* timers stop too */

void cpu_dump_trace(void);
void mmio_dump(void);
void sched_dump(void);
extern int g_ppc_trace;

static volatile uint64_t g_last_flip_ns;
static uint64_t g_t0_ns;  /* the first flip: [pace] lines give the time since then */
static int stall_watch(void* arg) {
    (void)arg;
    int reported = 0;
    for (;;) {
        platform_sleep_ns(1000000000ull);
        uint64_t last = g_last_flip_ns, now = platform_now_ns();
        if (g_host_pause || !last) continue;
        { int rel_is_linked(void);  /* a Sofdec movie (movie.rel) writes its frames straight to the XFB: no flips */
          if (rel_is_linked()) { reported = 0; continue; } }
        if (now - last < 5000000000ull) { reported = 0; continue; }
        if (reported) continue;
        reported = 1;
        rt_log("[stall] no frame for %.1f s (field %u): where the game is", (now - last) / 1e9, vi_field_count());
        cpu_dump_trace();
        mmio_dump();
        sched_dump();
        platform_sample_threads();  /* every host thread's PC (exe+offset): the generated function a busy loop is in */
        { void sched_irq_state_dump(void);  /* the same counters 1 s later: which ones are still climbing */
          platform_sleep_ns(1000000000ull); rt_log("[stall] one second later:"); sched_irq_state_dump();
          platform_sample_threads(); }
        rt_log("[stall] end of report; the game keeps running");
        fflush(stderr);
    }
    return 0;
}

static uint32_t g_stat_field, g_n_frames;
static uint64_t g_work_ns, g_last_return_ns;

void hle_frame_end(Cpu* c) {
    while (g_host_pause) platform_sleep_ns(10000000);
    void perftest_frame_begin(void); void perftest_frame_done(void);
    perftest_frame_begin();
    uint64_t now = platform_now_ns();
    if (g_last_return_ns) g_work_ns += now - g_last_return_ns;  /* game work since the last flip returned */
    f_803DCF2C_orig(c);  /* the game's own flip: waits for its retraces */
    g_last_return_ns = platform_now_ns();
    if (!g_last_flip_ns) g_t0_ns = g_last_return_ns;
    if (!g_last_flip_ns) {  /* first flip: start the stall watch and the function-entry trace it reports */
#ifdef __SWITCH__
        g_ppc_trace = 0;  /* the function-entry trace costs speed; the console report has the rest */
#else
        g_ppc_trace = getenv("SR_NO_STALL_TRACE") == NULL;
#endif
        rt_thread_start(stall_watch, NULL);
    }
    g_last_flip_ns = g_last_return_ns;
    perftest_frame_done();
    g_n_frames++;
    uint32_t f = vi_field_count();
    if (!g_stat_field) g_stat_field = f;
    if (f - g_stat_field >= 600) {
        double s = (f - g_stat_field) / 59.94;
        char clk[96];
        platform_clocks_str(clk, sizeof clk);
        rt_log("[pace] %.1f frames/s (game target 30), game work %.1f ms per frame, GPU %.1f ms per frame%s, %u s since the first frame",
               g_n_frames / s, g_n_frames ? g_work_ns / 1e6 / g_n_frames : 0.0, platform_gpu_frame_ms(), clk,
               (unsigned)((platform_now_ns() - g_t0_ns) / 1000000000ull));
        g_stat_field = f; g_n_frames = 0; g_work_ns = 0;
    }
}

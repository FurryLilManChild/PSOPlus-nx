# Progress log (newest first)

## 2026-11-23: Build 0.5.0 (the GP thread's long frames at stock clocks, [pace] GP and menu lines, version 0.5.0)

- Why: tests 12 and 13 at stock CPU 1020 MHz: stretches of 28-29 fps with single 57-70 ms frames whose [hitch]
  "GX draws" (the GP thread's draw processing) was 20-60 ms, while that work averages about 8 ms a frame.
- Cause found (code and logs): the GP thread, the vertex decode helper and the menu's picture thread were meant to
  run one priority step below the other threads on their cores, but they never did on the Switch. libnx starts
  every thread at priority 0x3B (its pthread_create, read from libnx.a), 0x3B is the lowest this NRO may use, and
  every rt_thread_lower_priority call failed: both logs have "[switch] svcSetThreadPriority(60) failed (0xE001)"
  four times (Build D lines 12, 26, 27, 30; Build C 11, 26, 1334, 1335), 0xE001 being the kernel's invalid
  priority. So the menu's picture thread (pinned to core 2 with the GP thread) had the GP thread's priority, and a
  ready thread of equal priority does not take a core from a running one. A picture takes 25-38 ms ("[menu]
  picture drawn in 35.42 / 36.51 / 37.68 ms", Build D lines 61, 62, 136; 25.84-28.26 ms in Build C) and is drawn
  again whenever the menu's look changes (each button press).
- Log evidence that it is the menu: Build D's two 10 s windows ending lines 955 and 1088 had 39 and 29 slow
  frames, 35 and 28 of them with GX draws over 15 ms; the menu was open for most of them (its settings were saved
  at line 997, when it closed; 2 slow frames came after that). The windows around the button map changes (ending
  lines 1421-1799) had 3-24 (3-14 over 15 ms). The seven windows after the last menu use (ending lines
  1901-2548, 70 s, game work 16.2-23.0 ms a frame, the FPS counter on) had none. Not explained by this: Build D
  frames 4081-4316 (after the last logged settings change; the menu may have been open without a change, which
  logs nothing) and 6631-6661 (after the last [pace] line, just before the log ends); Build C's 2-12 slow frames
  with GX draws over 15 ms per 10 s (windows ending lines 2476-3408) with its bigger performance readout on
  (redrawn about once a second: the same mechanism is possible but not shown). Build C's minutes before the menu
  was first opened were at 1428 MHz, so they do not separate menu and no menu.
- Change 1 (menu.cpp, menu_raster.cpp): the picture thread gives way. Every 16 shapes and every 32 rows of a big
  one it checks the clock; after 0.25 ms of work it yields (svcSleepThread(0): any ready thread of its priority on
  core 2 runs first, the GP thread included; none ready: it goes on at once). The GP thread should then wait at
  most about 0.25 ms for it; a picture takes longer when core 2 is busy. Pixels drawn: unchanged (the yield
  touches nothing). The device threads on core 2 (retrace, disc and audio timers) gain the same.
- Change 2 (platform_switch.c): rt_thread_lower_priority no longer asks the kernel for 0x3C; one log line says
  the thread stays at 0x3B. Behaviour unchanged (the call failed before). The decode helper still shares core 1
  with the render thread at equal priority, as in tests 10-13.
- Not changed: vertex decode. A version with a template vertex and inline readers instead of function pointers
  was written and gave the same output bit for bit (a host differential test: 10.6 million vertices, random
  registers and 16 common GX layouts, 0 mismatches), but host timing was mixed (-30% to +20% per layout), so it is
  not in this build: no shown gain for a change in the hottest path before a release.
- Logging ([pace], every 10 s, from counters kept anyway plus one kernel call per frame): "[pace] GP thread: N
  frames; per frame busy X ms (on the CPU Y), draw processing Z; decode helper W" and "[pace]   worst frame F:
  draw processing, busy, on the CPU, vertex decode (of it waiting for the helper's job in progress), state
  snapshots, texture lookups, waiting for the render thread". CPU time is the GP thread's own (svcGetInfo thread
  tick count): a worst frame with much more busy time than CPU time waited for its core. "[pace]   menu open /
  closed now: pictures drawn, helper thread ms in all, longest, on the CPU; ImGui frames on the render thread"
  (only while the menu is open or pictures were drawn, e.g. the FPS counter's about once a second). The
  "waiting for the helper" part is new: in tests 12 and 13 it was inside "vertex decode".
- Version 0.5.0: tools/write_version.py PORT_VERSION feeds the NRO's NACP version (nacptool), the first log line
  ("[switch] PSO Episode I and II Plus native port 0.5.0 (build <stamp>); log started"), the [build] line
  ("[build] <stamp> (version 0.5.0)"; tools/read_log.py still finds the commit) and the menu's System page
  ("Version 0.5.0 (build <stamp>)"); the PC bug report header too. The NACP holds the version alone (16 bytes,
  made when configure.py runs).
- Evidence (cloud): the changed files compile for the Switch with -Wall, no warnings; gx_render.c, frame_pacing.c,
  main.c, platform_host.c, menu.cpp and menu_raster.cpp pass a PC (llvm-mingw) syntax check (menu.cpp's older
  unused-function warning for warn() on PC stays). Not verifiable in the cloud: everything on the console (whether
  the slow frames with the menu open are gone, the yield's effect, the thread tick count call's result).

## 2026-11-23: Switch tests 12 and 13 (Build C a90788b, Build D 989bc96) results

- the owner: FPS counter fine; button map, Reset and saving all work; the CPU menu setting works (his test; the
  session where he set it is not in the two logs he sent); FSR at 50% looked sharper and seemed to hold 30.
- Build C log (a90788ba): sys-clk held 1428 MHz while the menu said Stock, and the watch logged "another tool
  sets the clock (sys-clk?)", then "reads 1020 MHz again" when sys-clk was turned off. Detection works.
- Build D log (989bc966): button map changes logged as expected; FSR 640x480 -> 960x720, measured GPU cost
  5.3 ms a frame at 50% (EASU 4.0, RCAS 1.3); Forest GPU 12-16 ms a frame with it on.
- At stock clocks both logs show stretches of 28-29 fps with frequent single 57-70 ms frames while game work
  averaged only 15-19 ms. In most of those frames "GX draws" (GP thread draw processing) was 20-60 ms, so the
  GP thread, not game code, missed the frame. The worst run (frames 2220-2349, Build D) was with the port menu
  open. GP thread average 10.7 ms a frame (vertex decode 4.75 ms plus 3.69 on the helper thread). None of this
  at 1428 MHz.

## 2026-11-23: Build D (FPS counter, a button map, the clock set back on a crash)

- Why: the owner's review of Build C: the performance readout was too big and too busy; he asked for the frame rate
  alone, a way to change the buttons, and a clear note that FSR costs speed. Build C's review also asked for
  the CPU clock to be set back when the game crashes, and for the Riders names left on PC.
- FPS counter (menu.cpp): the Performance page's "Frame rate counter" (off by default, saved as perf_overlay
  as before) shows "30 FPS" in 15 px text in a small box at the top left; on the Switch that is the black bar
  beside the 4:3 picture. The figure is gx_render.c's existing one (every 30 game frames); the text is laid out
  again only when the whole number changes. Removed: the game work, GPU and CPU figures and the per-frame
  sum in frame_pacing.c that fed them (frame_pacing.c is back to its form before Build C); [pace] lines are
  unchanged. Off: one settings check per frame, nothing drawn.
- Button map (Controls page, Switch): a list for each GameCube input (A, B, X, Y, Z, L, R, START, D-pad up,
  down, left, right), choices None, A, B, X, Y, L, R, ZL, ZR, +, either stick click, the four D-pad
  directions. A opens a list, the D-pad picks, A chooses, B closes it unchanged (B now also reaches ImGui as
  "cancel"). Reset to default restores the old fixed layout (A B X Y = A B X Y, + = START, ZL / ZR = L / R,
  R = Z, D-pad = D-pad). One Switch button on several inputs is allowed and shown in amber ("ZR presses R and
  Z together"); so is an input with no button. L / R mapped to a button press fully (0xFF), as before.
  Minus is not offered: holding it 0.5 s opens the menu with any map, and the menu reads A, B, the D-pad and
  L / R / ZL / ZR from the buttons themselves, not through the map. Applies to the Pro Controller, a Joy-Con
  pair and handheld mode, all players; a single sideways Joy-Con keeps its fixed rotated layout (said on the
  page). Per poll: 12 table lookups (input_hid.c). The PC keeps its own controls.ini map (keyboard and SDL
  pad names) and its Controls table; the new keys are not used there.
- settings.ini: map_a ... map_right, values none / a / b / x / y / l / r / zl / zr / plus / lstick / rstick /
  up / down / left / right. A missing key keeps the default; an unknown input or button is ignored and
  logged. The log's [settings] lines list the inputs that differ from the default.
- FSR: the help text now says FSR costs graphics time every frame and can lower the frame rate in busy
  areas. fsr_present (render_deko.c) read; no change.
- Crash: the libnx exception entry is replaced (platform_switch.c). Before, an exception went straight back
  to the system as unhandled (libnx's own entry with no handler). Now the same happens after one step: if
  the menu's CPU clock is set, one clkrst (or pcv) message sets 1020 MHz. It runs on a stack of its own,
  takes no lock, writes no file or log, saves and restores every register it could change and returns the
  exception as unhandled (0xF801), so Atmosphere's error screen and crash report should show the original
  fault as before. libnx's __libnx_exception_handler was not used: libnx calls it after marking the
  exception handled and then calls svcBreak, which would turn crash reports into a break at that point.
  clock_override.txt stays after a crash, so the next start logs it and sets stock again.
- PC: window title "PSO Episode I and II Plus (native port)", bug report header and the mods.ini header no
  longer say Sonic Riders.
- Evidence (cloud): the changed files compile for the Switch with -Wall, no new warnings (mods.c keeps its 4
  older ones); menu.cpp, render_gl.c, platform_host.c, settings.c pass a PC (llvm-mingw) syntax check. A host
  test of settings.c: map_ keys load, unknown keys and buttons are ignored with a log line, CRLF and trailing
  spaces are handled, the file is written back and read again. Not verifiable in the cloud: the counter's
  look and place on the console, the button lists with the pad (scrolling on the Controls page), whether the
  mapped buttons reach the game, and the crash path (needs a crash on the console with the CPU setting on).

## 2026-11-23: Build C (the port menu overhauled, a CPU clock setting)

- Why: test 11 showed Episode II at 24-30 fps at stock CPU 1020 MHz and 29-30 fps at 1428 MHz (sys-clk), and
  the menu had no clock choice. The project goal stays stock clocks: Stock is the default.
- Menu before: Display (4:3 / "Fit to screen", field of view, resolution with Riders race wording, FSR
  sharpness, bloom, colour presets and sliders), Performance (a frame rate note, the FPS counter, the 384 MHz
  handheld graphics profile), Controls, About (version, screenshot); Sonic colours (navy, blue, gold) and a
  "Sonic Riders - Settings" title; dead Riders code (Gameplay and Mods pages, phone and iOS text, update check).
- Menu now (runtime/src/menu.cpp): Performance (CPU clock: Stock 1020 / 1224 / 1428 / 1785 MHz, the clocks
  read now, a warning when another tool sets the clock, the handheld graphics profile, the performance
  readout), Display (field of view, resolution, sharpness, colour), Controls (buttons, controller setup,
  how to work the menu), System (build stamp, the clocks now, the CPU setting, where settings are saved,
  screenshot). Plain dark grey with a muted cyan accent; title "PSO Episode I and II Plus: port settings".
  Opened as before (hold Minus 0.5 s; a single Joy-Con: + or -), pad only.
- Removed as Riders leftovers: "Fit to screen" (PSO has no camera patch, so it only stretched the 3D picture;
  a saved wide mode now loads as 4:3, logged), bloom (never ran in PSO: no game-state probe), the PC
  in-between picture rate (assumes a 60-picture game; a saved rate is reset to the game's own, logged), the
  Chaos Emerald and ring cap settings.
- CPU clock (platform_switch.c): clkrstSetClockRate on a PcvModuleId_CpuBus session (system 8.0+), else
  pcvSetClockRate. Set only while the game has focus; focus lost (HOME, sleep, a system applet), exit
  (atexit) and rt_fatal set 1020 MHz back; focus back, resume and docked / handheld changes set it again.
  clock_override.txt exists while an override is set; a start that finds it sets 1020 MHz first and logs it.
  Each change logs the request, the result code and the clock read back. A once-a-second watch (stall
  thread, only while an override is set or the menu or readout shows clocks) sets it once more after a
  mismatch, then reports "another tool sets the clock (sys-clk?)" in the log and the menu. No sys-clk IPC.
- Performance readout (off by default): fps, game work per frame (30-frame average from frame_pacing.c,
  computed only while on), GPU ms, CPU MHz; laid out again only when its text changes (about once a second).
- settings.ini: new keys cpu_clock and perf_overlay (old show_fps is ignored); written to settings.ini.tmp,
  then the old file removed and the new one renamed (FAT refuses a rename onto a file); a start that finds
  only the .tmp reads it.
- Evidence (cloud): the changed files compile for the Switch with -Wall, no warnings; menu.cpp also passes a
  host syntax check. Not verifiable in the cloud: anything on the console (whether clkrst accepts the rates
  from a homebrew NRO, the focus messages, sys-clk interplay, the menu's look and pad navigation).

## 2026-11-23: Switch test 11 (Build B, 82495e2) results

- the owner played Forest 1 and then Episode II, part of it with sys-clk at CPU 1428 MHz (the log's [pace] lines show
  the clocks, so the two are separated below). Log stamp 82495e29, about 550 s.
- Stock clocks (1020 MHz), Forest 1 busy stretch: 29.9-30.0 fps, game work 26.2-27.9 ms a frame (test 10:
  19.6-26.3 ms; a different play-through, so not a like-for-like comparison).
- Stock clocks, Episode II (from about 330 s): 23.8-30 fps, game work up to 32.7 ms in the worst 10 s windows,
  GPU 21-24 ms a frame (Forest: 7-16). The stock-clock goal is not met there yet.
- CPU 1428 MHz, Episode II: 29.2-30 fps, game work 13.9-24.4 ms.
- [dispatch]: 167,772,160 indirect calls in about 550 s (about 10,000 a frame), 341,361 (0.2%) missed the cache.
- Profile: 803DB560's bucket fell from 4.3% (test 10) to 1.4-2.0% of samples in matching windows, consistent with
  the search no longer running; the game-work figure did not show a clear drop. Episode II's top game buckets:
  803D64C0 3.8-4.6% (from 802A8DA0 and 802A8F2C), 802A9478 1.8-2.5%, 803BEEFC 2.1-2.4%, 80376420 1.7-2.3%,
  8038C50C 2.0-2.2%. Apart from idle the profile is flat: no single function above 5%.
- Render wait 0.01 ms a frame and draw done 0.3-0.6 ms throughout, as in test 10.

## 2026-11-22: Build B (an indirect-call cache); the matrix functions measured and left as they are

- Reading test 10's profile: the Switch profiler charges each sample to the function entered last, so a
  caller's code after a call returns counts for the callee. 803DB560 (4.3%, from 803DBE50) is a 15-instruction
  state compare; what follows it in the material loop of 803DBE1C is the indirect call to the state handler
  (table at 0x804FC540) through ppc_dispatch, a binary search over the 28,864-entry function table (460 KB,
  about 15 dependent loads and unpredictable branches). The generated code has 5,911 such indirect call sites.
- Build B: runtime/src/dispatch.c (ppc_dispatch moved out of cpu.c) puts a 4096-entry direct-mapped cache in
  front of the search: one 64-bit word per entry (guest address, host function as a 32-bit offset), relaxed
  atomics, only results of the search are cached, non-entries are never cached and still end in rt_fatal.
  No guest-visible change: the same function runs for every address. A [dispatch] line (every 16.7 million
  indirect calls) logs the count and the cache misses, so test 11 shows how often the game calls indirectly.
- Evidence (cloud): tools/tests/run_dispatch_cache_test.sh links dispatch.c with the real table and one stub
  per function: 20,000,000 lookups (90% from 600 hot targets, the rest the whole table and 391,482 non-entries)
  returned what the binary search returns, 0 wrong; 1,225,433 calls through ppc_dispatch ran the right stub;
  4 threads at once, 20,000,000 lookups, 0 wrong. Host speed per lookup: binary search 90.7 ns, cache 12.1 ns.
- The matrix functions were written natively and not kept. PSMTXConcat 80376420, PSMTXInverse 8037653C and the
  transpose 803764EC passed a differential test (300,000 cases, the whole Cpu struct and memory windows,
  locked cache and RAM, aliasing, against the game's code in both generated forms; NaN cases apart), but an
  hle.txt entry makes the recompiler emit the original in its plain form and compile every caller to read
  back any register, so the native code must write every register the original changes (17 register pairs
  for Concat) where the optimised generated code wrote 5. Host timing against the optimised form that ran in
  test 10: Concat 51.0 vs 43.1 ns (slower), Inverse 83.8 vs 68.5 ns (slower; 21.9 ns when the input repeats,
  but its input is the current node's matrix and 803DC904 makes a new one for every node), transpose 9.7 vs
  11.3 ns. The generated code for these leaf functions is already straight-line C.
- GX library functions (8038C50C, 80389BD0, 8038C0B4, 8038EB80, 803897D8) skipped: their buckets also hold the
  caller's code after they return, and their own work is mostly pipe writes, already inline appends.
- Not verifiable in the cloud: the effect on the Switch's frame times; how many indirect calls a frame makes.

## 2026-11-22: Switch test 10 (Build A, bd343d6) results

- the owner: Forest 1 held a steady 30 fps, including the busy spot that dropped to 20-25 fps in tests 4-9. Stock
  clocks (log: CPU 1020 / GPU 384 / memory 1331 MHz handheld), sys-clk off, SR_PROFILE=1 only.
- Log (stamp bd343d66, about 180 s of frames): every [pace] line 28.9-30.0 frames/s. Game work in the busy
  stretch 19.6-26.3 ms a frame (tests 4-9: about 29-34 ms), so about 7 ms of the 33.3 ms budget is left at the
  worst 10 s window. GPU 7-11 ms.
- The review's predictions held: render wait 0.01 ms a frame (test 9: 5.7), draw done reached by the GP thread
  0.48-0.64 ms after issue (test 9: 6.4), game core idle with GP work queued 0.16-0.23 ms, [cull] off.
- Hitches in play after frame 2000: four single frames of 50-67 ms (GX draws 7-12 ms with 1-2 new textures);
  the rest are loading (disc reads) and the area entry (247 new textures in one frame).
- Profile, top game functions: 803DB560 4.3% (from 803DBE50), 8038C50C 2.0%, 80389BD0 1.7%, 80376420 2.3% over
  two callers, 8037653C 1.0%; idle 30.8%.
- psoplus.prev.log in the folder is from test 9 (597c8f5), not from this build.

## 2026-11-21: Switch tests 4-9 (GX pipe and cull work), Build A (review fixes)

- Tests 4-9 (commits 8b4e00c, a500102, 93c294c, 1756a8a, 7aa1264, 597c8f5): SR_GXSTAT stream survey (test 4:
  about 11,000 matrix loads a frame in the Forest's busy spots, a fifth of all pipe stores); pipe stores as
  inline appends into a 4 KB batch (5); native matrix writers 8038EA3C / 8038EA70 and an off-screen draw count
  (6: 50-64% of 3D draws wholly outside the view in busy spots); the model cull in cull.c (7-9). Tests 7, 8 and 9
  each logged 0.0% of models skipped.
- An independent code review of tests 6-9 found:
  - The cull never ran in tests 7-9: the current matrix pointer (r13-0x2E8C) is always in the locked cache
    (0xE0000000, the game's matrix stack set up by 803DEC98), and the check accepted RAM only. Test 7 likely
    also read the boxes before the decode finished; a projection fault in test 8 is possible but unproven.
  - The same RAM-only check made the native matrix writers fall back to the game's code for every model matrix.
  - The cull's bounds pass cost about 5.6 ms a frame inside the frame end path the game waits on (GXDrawDone),
    and the [perf] / [hitch] "waiting for the render thread" figure included it.
  - sched.c's idle wait could spin forever inside an interrupt handler (deliver() refuses there and leaves the
    pending flag set).
- Build A fixes: guest_span_ok (runtime.h) accepts RAM and the 16 KiB locked cache, used by cull.c and
  hle_gx.c; SR_CULL is off unless SR_CULL=1, and then no bounds work runs on either thread; the cull's skip path
  also runs 803DC41C and 803DC3BC (the model's GXSetArray calls, r3 = model+0) as 803DC460 does; the idle wait
  returns at once inside a handler; the render-thread wait timer covers render_submit alone.
- Evidence (cloud): tools/tests/run_mtx_writer_diff.sh links the recompiled originals with hle_gx.c and the
  runtime's guest window: 100,000 random cases (half in the locked cache, about 9% straddling a batch flush),
  0 mismatches, 0 fall backs. Signalling NaN inputs differ only in the quiet bit (the original goes through the
  host FPU; the native copy keeps the bits, as the GameCube's paired-single store does). guest_span_ok passed
  16 of 16 edge cases. The changed files compile for the Switch with no warnings.
- Not verifiable in the cloud: anything at run time (the cull's effect with SR_CULL=1, frame times).

## 2026-11-20: Switch test 2 results, test 3 (range fog, game-thread profile)

- Test 2 (2a6e50e, handheld, stock CPU 1020 / GPU 384 / memory 1331 MHz): precompiled shaders from the 7
  recorded keys covered every draw (121212 of 121212 states); GPU 20-27 ms per frame (test 1, ubershader) ->
  6-9 ms. 30 fps except one stretch in the Forest's first large area: 18-21 fps for about 40 s with about
  4000 draws per frame; game thread 34-37 ms per frame, GPU 9-10 ms, render thread 10 ms. The vertex decode
  already runs on the GP thread and its helper, so the limit is the game thread itself.
- Fog: the Forest uses range fog (BP 0xE8 = 0x696, K 102100 108104 11210C 11F118 130127).
- Test 3: range fog as Dolphin computes it (offset from the vertex shader's viewport x, K / 64); the
  game-thread sampling profiler (SR_PROFILE=1 in env.txt) with per-function totals; the port's own icon.
- GameCube saves: tools/import_gci.py put the owner's GameCube character on the card (PSO_SYSTEM serial and CRC
  redone as Dolphin does; the tool's CRC matched the original file, and its serial matched the one the port's
  own PSO_SYSTEM carried).

## 2026-11-19: Switch test 1 (first NRO, commit e42ee70)

- Cloud build with devkitPro's Linux toolchain (copied out of ghcr.io/danfromtico/switch-dev-legacy-vulkan:
  devkitA64 GCC 15.2, libnx, deko3d, uam). 109 of 109 steps, no errors; NRO 31.4 MB, sha256 d4420b4f...
  ELF kept as build/switch/psoplus-e42ee70.elf for crash offsets.
- deko3d renderer; no PSO shader keys yet, so every draw uses the precompiled ubershader (nothing compiles on
  the console). The PC build now records keys to shader_keys.txt by default; the next Switch build folds them in.
- Stall report skipped while movie.rel is linked; function-entry trace off on Switch.
- Preflight: tree, codegen, compile (PC), hardware, addresses all PASS at e42ee70.
- Known going in: movies show black (sound only, Start skips), no fog, Riders colours in the menu.

## 2026-11-19: test 3 result, test 4 (interrupt-delivery diagnostics)

- Test 3 (3d6b7ff4): sound works (the owner). Black screen right after choosing offline mode, with no movie.rel
  link this time, so the movie player is not the cause. [stall] report after 6 s:
  - game thread 80543FC8 (prio 16) asleep in GXDrawDone (parked 80375D1C 8038A0B0 803DCEFC, the flip);
  - thread 80575E80 (prio 24, started at 803CBA58) suspended itself; no thread ready; the host thread
    spins in SelectThread's idle loop (exe+179A810 = f_8037522C, code_044.c:12656, `while (RunQueueBits == 0)`);
  - interrupts pending and posted but never taken: 07001880 = DSP AI/ARAM/DSP, PE finish (the draw done the
    game waits for), SI, VI. The OS mask does not mask PE finish.
  So the GPU did finish; the CPU is not taking interrupts while idle. Cause not proven: deliver() refuses when
  its host thread is already inside handlers (t_in_irq), and ppc_preempt skips when MSR.EE is off.
- Test 4 adds counters to the [stall] report (interrupt checks, how many were refused for EE off or for being
  inside handlers, deliveries), a second sample 1 s later, and marks each guest thread whose host thread is
  inside the interrupt handlers.

## 2026-11-19: test 2 result, test 3 (old AX audio, stall report)

- Test 2 first run failed in configure (no baserom/movie.rel): build_pc.ps1's step 1 updated the script but
  PowerShell ran the old copy. build_pc now restarts itself when the fast-forward changed it (0e733963).
- Test 2 (0e733963): boots, 30 fps, but no sound (the owner). Character creation: OSLink of movie.rel logged, then
  no [pace]/[perf] line until the window was closed (black screen; frozen or not drawing unknown).
- No sound, cause proven: the DSP image's Dolphin hash (HashEctor over 6624 bytes at 0x804F7980) is 0x4E8A8B21,
  the older AX microcode; Riders' stand-in implements 0x3DAF59B9. In the older one mixer control is route flags
  (Dolphin ConvertMixerControl), so a voice routed "main only" (0x0000) had none of the newer bits set and was
  never mixed; its PB also lacks the 4-word filter block. ax.c now handles both (dsp.c picks by hash).
- The black screen: no evidence in the log. frame_pacing.c now reports a stall (no flip for 5 s): function-entry
  trace, hardware registers, guest threads, host thread PCs.

## 2026-11-19: test 1 result, test 2 (movie.rel)

- Test 1 (ce8665b9, the owner's PC): boots to the title screen; 30 fps in every [pace] line after the first two;
  game work 1 to 5 ms per frame; card formatted and written in the menus. Stopped after idling at the title
  with `FATAL: dispatch to 810779E8: not a function entry (lr 80320498)`: the attract movie. 0x80320478 calls
  OSLink (0x80372D00) on movie.rel read into the heap, then the module's prolog (+0x34).
- movie.rel is the only code module (the only Nintendo REL on the disc; one OSLink call site). Now recompiled
  at 0x81800000 (bss 0x81845C80+0x53D34). gcrecomp: REL section numbers from dtk's splits.txt (movie.rel's
  are text, ctors, dtors, rodata, data, bss, not Riders' layout), which found its 7 switch tables. The
  runtime's REL linker read fixSize from a version 2 header that has none: fixed (whole file, as gcbin.py).
- rel.c: OSLink relinks the fixed image from the disc every time (fresh data and bss), sets prolog, epilog
  and unresolved; OSUnlink reports success. configure.py now recompiles when any recompiler input is newer
  than build/gen/report.txt (the file count changed from 55 to 58).
- Not yet: fog (the renderer does not draw it; PSO uses it from the title screen on).

## 2026-11-19: first PC build ready (commit a45a21d2)

- gcrecomp: REL made optional (config/GPOE8P/recomp.txt). 27,593 functions, clean report: data words, the
  "Metr" string at 0x8000C2FC, one table-less dispatch (__ptmf_scall 0x80396A50, a computed tail call).
- Runtime: boot from PsoV3.dol via the FST; PSO OS statics (OSThread 0x310 bytes in this SDK); frame_end hook
  on the game's flip for logging only; Riders-only files removed with stubs (pso_stubs.c).
- Cloud compile check with llvm-mingw 20261006 (clang 23) and SDL2 2.32.10, the versions setup_pc.ps1 installs:
  first link failed on Riders addresses in hle_misc.c (memcpy/memset/memmove originals), fixed via game_addrs.h.
- tools/sweep_hw.py: every device block the game touches is modelled except MI (memory protection, explained);
  rfi functions all replaced or never reached. Preflight RESULT: PASS (commit a45a21d2).
- Known gap: OSResetSystem ends in Reset(), a hang loop on the console; reached only by a reset or the DOL
  chain loader (0x8007FA08). To replace with a native exit when it is reached.

## 2026-11-19: phase 1 analysis

- the owner approved MPL-2.0, our own copy of the runtime, and matching the original frame rate.
- Copied sonic-riders-recomp (commit 75380c0) runtime, recompiler and tools; the game-data guard now knows
  PSO's hashes.
- decomp-toolkit 1.8.4 on PsoV3.dol stopped with "Unpaired epilogue 3:0x8042EA04": the DOL chain loader at
  0x8042E8C4 jumps to the next program, leaving its epilogue dead. A seed symbol with the full size fixed it
  (config/GPOE8P/symbols_seed.txt); 27,573 functions found. main.dol needed the same seed (0x80035558).
- main.dol loads switcher.prs and chain-loads it with a block at 0x80003100 naming psov3.dol. PsoV3.dol checks
  for "SEGA" at 0x80003210 and builds defaults when it is missing, so a direct boot is fine.
- Frame rate: the flip at 0x803DCF2C waits for an interval set once to 2 retraces at boot (0x8000FB2C):
  30 fps cap in every mode.
- Wrote config/GPOE8P/hle.txt (context switch, audio and memory-protection init, memcpy/memset/memmove);
  frame-end wiring is listed but left for the first build.

## 2026-11-19: kickoff

- Chose the GameCube Plus version over Dreamcast Ver. 2 after finding teclilla18/sonic-riders-recomp (static
  recompilation of a GameCube game to Switch with deko3d, MPL-2.0, released on Switch and PC).
- Converted the owner's .rvz to ISO in the desktop Linux VM with sonic-riders-recomp's `rvz2iso.py` (21 s), listed
  the disc with our `psodisc.py`. Findings in docs/design.md: the game is PsoV3.dol (4.15 MiB of code, no code
  RELs), main.dol is a loader, CRI ADX and AVE-TCP libraries present.
- Wrote .gitignore, CLAUDE.md, docs/design.md (draft for the owner's review), docs/upstream.md.
- Not done yet: Redump hash check, dtk analysis, main.dol handoff check.

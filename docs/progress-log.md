# Progress log (newest first)

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

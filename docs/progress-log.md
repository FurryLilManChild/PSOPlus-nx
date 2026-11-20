# Progress log (newest first)

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

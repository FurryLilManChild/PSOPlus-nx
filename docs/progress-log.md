# Progress log (newest first)

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

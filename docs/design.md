# PSOPlus-nx design (reviewed with the owner 2026-11-19)

## Goal

Phantasy Star Online Episode I & II Plus running as native code on a modded Switch (and on Windows for fast
build and test cycles): the game's PowerPC code translated to C ahead of time, game memory as plain Switch
memory, only hardware registers going through a device layer, graphics drawn with deko3d. No emulator.

First milestone: every offline mode playable for one player. Then 2 to 4 player split screen. Online play is out
of scope; the network code stays in the game but finds no adapter.

## What we know about the disc (measured 2026-11-19)

- Disc: GPOE8P, revision 2, 1,459,978,240 bytes as ISO, SHA-1 `0ff155311f4cc57951c03baeb1a8cc15de5fe1cd`
  (converted from the owner's .rvz). The size matches a full GameCube disc; the SHA-1 has not yet been checked against
  Redump.
- `main.dol` (214 KB) is only a loader. It reads `switcher.prs` (a compressed copy of `switcher.dol`, which
  runs high in memory at 0x806C5000) and chain-loads it, handing over a block at 0x80003100 that names the next
  program, `psov3.dol`. The game is **`PsoV3.dol`** (5.2 MB): entry 0x8000C040, code 0x8000F740 to 0x80433F20
  (4.15 MiB of text), bss up to 0x805E1224, so it fits in the 24 MiB of MEM1 with room for the game's heaps.
- **Booting PsoV3.dol directly works by design**: at start the game looks for a shared settings block with the
  magic "SEGA" at 0x80003210; when it is missing the game clears 32 KiB there and fills in its own defaults. So
  the port skips main.dol and the switcher. PsoV3.dol also contains the same chain loader (0x8042E8C4), used to
  hand control back to another program; it is called from one place (0x8007FA08) and will be replaced natively
  if that path is ever reached.
- **No code modules.** 648 of the 649 `.rel` files are Sega's own data format (map, NPC, text and table data
  with pointer fixups), not Nintendo RELs, so the whole game is one executable. Simpler than Sonic Riders, which
  links `_Main.rel`. The one file that looks like a Nintendo REL still has to be checked.
- Other executables on the disc (`switcher.dol`, `iwanaga.dol` and debug `D` copies) are not part of the normal
  game boot; to identify later.
- Libraries found by their version strings: Dolphin OS (SDK "Revision 54"), CRI ADX (ADXT 8.05, ADXF 6.24,
  CVFS 2.24, ADXGC 1.10; Sonic Riders also uses CRI middleware), ACCESS AVE-TCP 1.16.1 network stack (online
  only), DSP/AX audio.
- 3,510 files, 1.41 GB: 111 ADX music and voice streams, Ninja models (.nj, .gj), GVM textures, AFS archives.

**Frame rate: 30 fps.** The game's display flip (Sega's Ninja layer, 0x803DCF2C) waits until at least N retraces
have passed since the last flip, and N is set exactly once, at boot, to 2 (0x8000FB2C, through 0x803DD080). On a
60 Hz display that caps every mode at 30 fps; split screen uses the same flip, so it can only be 30 fps or lower.
The port targets a steady 30 fps first. 60 fps would mean changing that interval and checking that game logic,
animation and physics are tied to the frame time, not the frame count: a later experiment, not a goal yet.

Analysis numbers (decomp-toolkit 1.8.4): 27,573 functions. System libraries sit in 0x8036F900 to 0x80433F20
(OS, DVD, VI, PAD, AI, AR, DSP, CARD, GX, SI, EXI, plus CRI and AVE-TCP); the game itself is 0x8000F740 to about
0x8036F900 (3.4 MiB). 1,450 functions matched Dolphin's signature database with sizes of 0x24 bytes or more.

Unknown, to measure later: CPU load on the original hardware, how far below 30 fps split screen drops.

## Pieces and plan

| Piece | Plan | Taken from |
|---|---|---|
| Function list | decomp-toolkit 1.8.4 analysis of PsoV3.dol, plus Dolphin SDK signature matching | sonic-riders-recomp's config and `sigmatch.py` |
| Code generator | gcrecomp (PowerPC to C, guest registers in host locals, paired singles, GQR) with the REL path unused | sonic-riders-recomp `tools/recomp/` (MPL-2.0) |
| OS and threads | The game's own recompiled OS scheduler; native context switch only (`OSSaveContext`/`OSLoadContext`) | sonic-riders-recomp `sched.c` |
| Disc | Reads the player's image directly (FST lookup), .iso and .rvz | sonic-riders-recomp `disc.c` plus `rvz2iso.py` |
| Audio | DSP mailbox plus AX microcode HLE; CRI ADX runs recompiled | sonic-riders-recomp `dsp.c`, `ax.c` |
| GPU front end | GX FIFO parser on its own thread into draw state | sonic-riders-recomp `gx_fifo.c`, `gx_render.c` |
| Switch renderer | deko3d with a TEV ubershader plus specialized shaders compiled ahead of time from recorded keys (a starter key list from PC and console play, shipped from the first drawing build) | sonic-riders-recomp `render_deko.c`, `gen_shaders.py` |
| PC renderer | OpenGL 3.3 backend, for bring-up and comparison on the owner's PC | sonic-riders-recomp `render_gl.c` |
| Memory card | Native card formatting, GCI-style saves in the app folder | sonic-riders-recomp `exi.c` |
| Network adapter | EXI reports no broadband or modem adapter; the game's offline path takes over | ours |
| Controls | GameCube pad layout on Joy-Cons and Pro Controller, per-button remapping, up to 4 pads for split screen | sonic-riders-recomp `si.c`, `input_hid.c` |
| Port menu | Settings, controls, resolution, performance, exit | sonic-riders-recomp `menu.cpp` (trimmed) |

## Phases

1. **Analysis**: dtk config for PsoV3.dol, function list, SDK map, the loader handoff from main.dol, the HLE
   list (OS context switch, memory protection, audio init) at PSO's addresses.
2. **First PC build**: recompile, link with the runtime, boot on the owner's PC to the Sega logo and title. Every
   whole-program sweep from switch-port-base runs before the owner gets the first build.
3. **Offline play on PC**: character creation, Pioneer 2, Forest, saving to the memory card, music and sound.
4. **First Switch build**: deko3d with the starter shader cache, logs, crash handler, port menu.
5. **Performance at stock clocks**, then split screen (2 to 4 players), then a test release.

## Decisions (the owner, 2026-11-19)

1. **License**: MPL-2.0 throughout (required for the files forked from sonic-riders-recomp).
2. **Base strategy**: our own copy of sonic-riders-recomp's runtime and recompiler in this repo, changed freely,
   with the pinned commit in docs/upstream.md.
3. **Frame rate target**: the original's 30 fps (see above).

# PSOPlus-nx design (draft for review, 2026-11-19)

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
- `main.dol` (214 KB) is only a loader: its one notable string is `psov3.dol`. The game is **`PsoV3.dol`**
  (5.2 MB): entry 0x8000C040, code 0x8000F740 to 0x80433F20 (4.15 MiB of text), bss up to 0x805E1224, so it
  fits in the 24 MiB of MEM1 with room for the game's heaps. Plan: boot PsoV3.dol directly, after checking what
  main.dol sets up before it jumps (arguments, memory).
- **No code modules.** 648 of the 649 `.rel` files are Sega's own data format (map, NPC, text and table data
  with pointer fixups), not Nintendo RELs, so the whole game is one executable. Simpler than Sonic Riders, which
  links `_Main.rel`. The one file that looks like a Nintendo REL still has to be checked.
- Other executables on the disc (`switcher.dol`, `iwanaga.dol` and debug `D` copies) are not part of the normal
  game boot; to identify later.
- Libraries found by their version strings: Dolphin OS (SDK "Revision 54"), CRI ADX (ADXT 8.05, ADXF 6.24,
  CVFS 2.24, ADXGC 1.10; Sonic Riders also uses CRI middleware), ACCESS AVE-TCP 1.16.1 network stack (online
  only), DSP/AX audio.
- 3,510 files, 1.41 GB: 111 ADX music and voice streams, Ninja models (.nj, .gj), GVM textures, AFS archives.

Unknown, to measure before decisions: the game's frame rate and frame limit (single player and split screen),
CPU load on the original hardware, which SDK functions the game reaches.

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

## Open decisions for the owner

1. **License**: MPL-2.0 for the repo (needed for the files forked from sonic-riders-recomp; our own files could
   be MIT, but one license is simpler). Recommended: MPL-2.0 throughout.
2. **Frame rate target**: decided after measuring the original (30 or 60).
3. **Base strategy**: copy sonic-riders-recomp's runtime into our repo (our own copy, changed freely, with the
   pinned commit recorded) rather than a submodule with patches. Recommended, because PSO needs changes in many
   shared files (no REL, different HLE list, no Riders mods).

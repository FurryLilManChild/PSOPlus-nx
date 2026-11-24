# PSOPlus-nx

A native Nintendo Switch homebrew port of **Phantasy Star Online Episode I & II Plus** (GameCube, USA,
revision 2, GPOE8P), made by static recompilation: the game's PowerPC code is translated ahead of time
into C and compiled for the Switch. It is not an emulator. Offline play and online play on
the Schtserv community server.

**No game data is included.** You need your own copy of the game.

Current release: **0.6.0** (adds online play).

## What you need

- A Switch that can run homebrew (Atmosphere).
- Your own disc image of Phantasy Star Online Episode I & II Plus, USA, revision 2 (game ID GPOE8P), as a
  full, untrimmed `.iso` or `.gcm` (about 1.4 GB). RVZ, WIA, CISO, NKit and trimmed images are not read: convert
  an RVZ to ISO first, for example in Dolphin (right-click the game, Convert File, format ISO). The port checks
  the disc at start and says so if it is the wrong game or revision.

## Install

1. Copy the `psoplus` folder from the release zip to `sdmc:/switch/`, so you have
   `sdmc:/switch/psoplus/psoplus.nro`.
2. Put your disc image in the same folder (any name ending in `.iso` or `.gcm`).
3. Start the Homebrew Menu by holding R while opening any game (title takeover), not from the Album, then
   open PSO Episode I and II Plus.

The first start creates a memory card file, `sdmc:/switch/psoplus/saves/card_a.raw`. Saves from a real
GameCube (`.gci` files) can be copied onto it with `tools/import_gci.py` on a PC; it keeps PSO's card check
valid.

## Port menu

Hold **Minus** for half a second (single Joy-Con: + or -). L/R or ZL/ZR change pages, the D-pad or stick
moves, A chooses, B goes back or closes.

- **Performance**: CPU clock (Stock, 1224, 1428 or 1785 MHz) and an FPS counter. The clock applies only while
  the game is on screen and returns to stock in the HOME menu, in sleep and on exit. If you use sys-clk, turn
  off its profile for this game, or it overrides the menu.
- **Display**: resolution, field of view, sharpness (FSR 1), colour.
- **Controls**: a button map for every GameCube button, and Reset to default. Holding Minus always opens the
  menu, whatever the map.
- **System**: version, build, current clocks, online status, screenshot.

Settings are saved in `sdmc:/switch/psoplus/settings.ini`.

## Online play

Choose online from the game's title menu, as on a GameCube with a Broadband Adapter. The port connects to
Schtserv by itself; no network setup is needed in the game. **First time on Schtserv: register your serial
number first, see [ONLINE.md](ONLINE.md).** While online, press the right stick to chat with the Switch
keyboard. Your memory card is backed up before each online session.

## Performance

The game runs at its original 30 fps. At stock clocks the Episode I areas tested so far hold 30 fps. Busy
parts of Episode II can drop to about 24 fps at stock; 1428 MHz in the port menu holds 30 there. Higher
resolutions and sharpness cost graphics time and can lower the frame rate in busy areas.

## Known issues

- Movies show a black picture with sound. Start skips them.
- Resetting the game from inside the game can hang. Close it from the HOME menu instead.
- Some busy areas dip below 30 fps at stock clocks.
- The port menu does not pause the game (it keeps running behind the menu, with your buttons going to the
  menu), and the frame rate can drop while the menu is open.
- Only the USA disc, revision 2.

## Reporting problems

Each session writes `sdmc:/switch/psoplus/psoplus.log` (the one before is `psoplus.prev.log`). Please attach
both to an issue, with what you were doing and where in the game.

## Building from source

You need your own disc and:

- devkitPro with devkitA64, libnx and deko3d, plus Python 3;
- Dear ImGui 1.92.9b, extracted to `build/ext/imgui-1.92.9b`.

Extract `PsoV3.dol` and `movie.rel` from your disc into `baserom/` (`tools/disc/psodisc.py ISO OUTDIR`
lists and extracts them), then:

```
python3 configure.py --target switch --devkitpro /opt/devkitpro --imgui build/ext/imgui-1.92.9b
ninja
```

The output is `build/switch/psoplus.nro`. Everything generated from the game (recompiled code, analysis,
disc files) stays in ignored folders and must never be committed or shared.

## Credits and licenses

This port is released under the Mozilla Public License 2.0 (`LICENSE`). It builds on:

- [sonic-riders-recomp](https://github.com/teclilla18/sonic-riders-recomp) (MPL-2.0): the runtime this port's
  runtime is a modified copy of, and its fork of the ZeldaWWHDRecomp translator (MPL-2.0).
- [decomp-toolkit](https://github.com/encounter/decomp-toolkit): analysis of the game's code.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [stb](https://github.com/nothings/stb) (public domain or
  MIT), [bcdec](https://github.com/iOrange/bcdec) (MIT or Unlicense).
- [libnx](https://github.com/switchbrew/libnx) (ISC), [deko3d](https://github.com/devkitPro/deko3d) (zlib) and
  the devkitPro toolchain.

See `docs/upstream.md` for pinned versions.

Phantasy Star Online is a trademark of SEGA. This project is not affiliated with or endorsed by SEGA or
Nintendo.

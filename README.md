# PSOPlus-nx

A native Nintendo Switch homebrew port of **Phantasy Star Online Episode I & II Plus** (GameCube, USA,
revision 2, GPOE8P), made by static recompilation: the game's PowerPC code is translated ahead of time
into C and compiled for the Switch. It is not an emulator. Offline play and online play on
the Schtserv community server.

**No game data is included.** You need your own copy of the game.

Current release: **1.1.1** (fixes a crash at login on Schtserv for some players; 1.1.0 added 16:9 widescreen).

## What you need

- A Switch that can run homebrew (Atmosphere).
- Your own disc image of Phantasy Star Online Episode I & II Plus, USA, revision 2 (game ID GPOE8P), either as a
  full, untrimmed `.iso` or `.gcm` (about 1.4 GB), or as a Dolphin `.rvz` (or `.wia`) made from a full dump.
  CISO, NKit, GCZ and trimmed images are not read. The port checks the disc at start and says so if it is the
  wrong game or revision.

### RVZ images

With an `.rvz` (or `.wia`) and no `.iso` in the folder, the first start converts it once to `<same name>.iso`
in `sdmc:/switch/psoplus/`, behind a progress screen (percent, MB done, time left), then starts the game.
Later starts use the `.iso` directly. It takes a few minutes, at the SD card's speed.

- It needs 1.4 GB (1,459,978,240 bytes) of free space on the SD card; the screen says so if there is not enough.
- The result is written as `<name>.iso.part` and only renamed to `.iso` after its size and disc header are
  checked. Its SHA-1 is compared with the clean dump of this disc and the result is logged (a scrubbed or
  modified dump is still used, with a note on screen).
- Closing the game during the conversion deletes the `.part`; one left by a power cut is deleted at the next
  start, and the conversion starts over.
- Your `.rvz` is never changed or deleted. Once the game has started from the `.iso`, you can delete the `.rvz`
  to free space.
- Converting on a PC instead is also fine: in Dolphin, right-click the game, Convert File, format ISO.

## Install

1. Copy the `psoplus` folder from the release zip to `sdmc:/switch/`, so you have
   `sdmc:/switch/psoplus/psoplus.nro`.
2. Put your disc image in the same folder (any name ending in `.iso` or `.gcm`, or an `.rvz`, see above).
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
- **Display**: screen (4:3 original, or 16:9 widescreen: the game's cameras see more to the sides, nothing
  stretched; 4:3 is the default), HUD in 16:9 (screen edges, the default: status, party HP, lock-on window, action
  palette, radar and info popups move to the sides of the screen, while menus, shops and dialogue stay 4:3 in the
  middle; or middle: the whole HUD 4:3 in the middle), resolution, field of view, sharpness (FSR 1), colour.
  Which window goes where is set in config/GPOE8P/hud_anchor.txt (built in; a copy beside settings.ini overrides it).
- **Controls**: Right stick (see below), a button map for every GameCube button, and Reset to default. Holding
  Minus always opens the menu, whatever the map.
- **System**: version, build, current clocks, online status, screenshot.

Settings are saved in `sdmc:/switch/psoplus/settings.ini`.

### Right stick camera

Controls > Right stick has two choices:

- **Menus** (the default): the right stick works as the GameCube's C-stick did in the original game.
- **Camera**: in the field, left and right turn the camera around your character; push up to look up (the camera
  goes lower) and down to look down (it goes higher). The camera stays where you put it instead of swinging back
  behind you; press L (the game's camera reset button; R if the game's options swap them) to put it behind you
  again (this also resets the height). Your character still runs the way you push the left stick, as seen on screen. When the camera would end
  up in a wall while you run, the game moves it as it always did.

The camera only answers the stick in single-player view while no window is open. In the main menu, chat, word
select, shops, counters and trades the stick works as in the original game. Bosses, quest scenes and
conversations keep the game's own camera, and split screen keeps the original right stick. The change applies
at once; no restart is needed. The camera does not stop at walls while you stand still and turn it: it can
end up inside a wall until you move.

## Online play

Choose online from the game's title menu, as on a GameCube with a Broadband Adapter. The port connects to
Schtserv by itself; no network setup is needed in the game. **First time on Schtserv: register your serial
number first, see [ONLINE.md](ONLINE.md).** While online, press the right stick to chat with the Switch
keyboard. Your memory card is backed up before each online session.

## Performance and testing

The game runs at its original 30 fps. Episode I and Episode II were both played through every area, offline
and online.

- **Episode I** runs at full speed at stock clocks.
- **Episode II**: set the CPU to **1224 MHz** in the port menu (Performance). At stock clocks its busiest
  spots can dip below 30 fps now and then.
- Movies are decoded natively and play at full speed.

Higher resolutions, sharpness and 16:9 widescreen cost graphics time and can lower the frame rate in busy
areas; 4:3 is the lightest.

Every area was tested, but that cannot rule out every bug. If you find one, please report it (see below).

## Known issues

- The ending movies use a different path from the opening movies and have not been checked on the console yet.
- Resetting the game from inside the game can hang. Close it from the HOME menu instead.
- Some busy Episode II areas dip below 30 fps at stock clocks; use 1224 MHz (see Performance).
- The port menu does not pause the game (it keeps running behind the menu, with your buttons going to the
  menu), and the frame rate can drop while the menu is open.
- Only the USA disc, revision 2.

## Reporting problems

Each session writes `sdmc:/switch/psoplus/psoplus.log` (the one before is `psoplus.prev.log`). Please attach
both to an issue, with what you were doing and where in the game.

## Building from source

You need your own disc and:

- devkitPro with devkitA64, libnx and deko3d, the portlibs `switch-libzstd`, `switch-liblzma` and
  `switch-bzip2` (for the RVZ conversion), plus Python 3;
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
- [zeroz41/pso-decomp](https://github.com/zeroz41/pso-decomp) (MIT): a decompilation of this game, the reference
  for the right stick camera (camera and window layouts, function addresses).
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [stb](https://github.com/nothings/stb) (public domain or
  MIT), [bcdec](https://github.com/iOrange/bcdec) (MIT or Unlicense).
- [libnx](https://github.com/switchbrew/libnx) (ISC), [deko3d](https://github.com/devkitPro/deko3d) (zlib) and
  the devkitPro toolchain.

See `docs/upstream.md` for pinned versions.

Phantasy Star Online is a trademark of SEGA. This project is not affiliated with or endorsed by SEGA or
Nintendo.

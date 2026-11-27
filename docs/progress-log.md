# Progress log (newest first)

## 2026-11-27: 16:9 widescreen, build C: the menu screen as one layout (test build, not yet run on the console)

Owner's console run of build B (d99f9c0, 16:9, HUD "Screen edges"; log and two photos):
- Field: the status window was centred. The log names it: `window tl_window TWindowStatus: centred (x 0..256,
  hud_anchor.txt default)`. The one-player status window is TWindowStatus (create_status_window: TWindowSimpleStatus
  only in split screen). With a hud_anchor.txt copy holding `TWindowStatus left` on the SD card it sat at the left
  edge and the owner confirmed it looks great. Folded into the built-in file (TWindowSimpleStatus kept).
- Main menu (Start): came apart. Status pinned left, menu list (TWindowMenuMainMenu x 28..144), help box
  (TWindowMessage 17..223) and stats panel (TWindowSubStatusDisp 299..611) centred, so the list covered the
  status's name tag; TWindowCtrlScreen's panels (x 0..640, 38 draws) partly stretched, partly centred, a dark strip
  at the left; the 3D view's box to the right screen edge.

What the original does, and the fix: docs/widescreen-research.md section 7. In short: the status window stays
shown on the menu screen in one-player play (0x8024DFF4 closes it only in split screen; read in our DOL), so it is
kept, unpinned, in its 4:3 place over the menu.

- Signal (pso_wide.c, pso_hud.h hud_menu_screen): the 3D view's box, TWindowCtrlMain::viewport_states[0] left /
  bottom (0x80518730 +0x0C / +0x18), set only by TWindowMainMenu1P::update_backdrop_transition (0x80288520, the
  only caller of set_left / set_bottom in the DOL). Read at the start of each walk; never in split screen or while
  the states are still zero. All marks carry it (kind byte flag WIDE_CTX_MENU), so a frame decides from one state.
- Windows: hud_anchor.txt grew `menu:` lines, used instead of the ordinary line while the menu screen is up;
  `menu: * centre` (status, menu, help, stats, every page in their 4:3 places relative to each other);
  `menu: TWindowActionPalette right`, `TWindowEffectLight right`, `TWindowLockOn left` (closed by the game while the
  menu is open, so they do not jump while animating), `TWindowMessageInfomation right` (moved by the game into the
  3D box, which reaches the right edge). [hud] window lines say "menu screen, hud_anchor.txt menu: line".
- Panels (gx_render.c backdrop_place, pso_hud.h hud_backdrop_place): TWindowCtrlScreen's own draws (flag
  WIDE_CTX_BACKDROP) placed per vertex: on the 4:3 left / right edge to the screen edge, else centred; pieces 16
  wide or less at an edge move whole. Column 0..267, band and lines to both edges, corner piece and the box's left
  line at the column's edge. In both HUD settings (my choice: "Middle" gets the clean panels too; its windows are
  unchanged from build A). Full-screen viewport only; split screen and mirrored views as before.
- 3D box: unchanged (213..854); the column (drawn over it) ends at 267, so the box shows from the column's edge to
  the right screen edge, as in 4:3. No gap at any step of the transition (left 0..320).
- Covered: the main menu and every page opened from it (they keep the box up). Not this screen: shops, tekker,
  bank, check room, Hunter's Guild, quest counter (no box: field placement, unchanged from build B).
- Log: `[hud] menu screen on: 3D view in the box x 160..640, y 0..256 of 4:3; ...` / `[hud] menu screen off: ...`
  (at most 200); the 10 s [wide] lines add "menu screen panels" draws and windows "on the menu screen".
- 4:3: no marks, nothing changed (as 1.0.0).
- Host test tools/tests/run_pso_wide_test.sh, extended: when the box is up (all 28 steps of the transition, split
  screen, unset states), the `menu:` parser and the shipped lines, the walk's marks in 6 game states (menu flag,
  each window's menu placement, the panel flag only on TWindowCtrlScreen), and every panel piece mapped to the 854
  wide screen for all 28 steps (column edge, band, black and fade pieces, corner, lines, no gap to the 3D, the
  menu's windows relative to the column); all build A and B checks: PASS.

## 2026-11-26: 16:9 widescreen, build B (test build, not yet run on the console)

Build A (3caa2c7) passed the owner's console test: "It was so beautiful!! Successful test." 3D widened, HUD centred
4:3, no problems reported. His log of that run is not in hand yet, so build B's positions come from the decomp and
our DOL (below), and every window's measured x range goes to the log so they can be corrected from the next run.

The plan is docs/widescreen-research.md section 6, build B. What was built:

- Display > HUD (16:9 only): "Middle" / "Screen edges" (menu.cpp; settings.ini hud_edges, default 1 = screen edges).
  The [settings] line says which.
- Windows (pso_wide.c): the walk now names, with every object, the window it belongs to: an object directly under
  a layer, under a window manager (TWindowCtrl*) or under a window root (a plain TObject walked by a manager:
  TWindowCtrlScreen::render walks get_viewport_window_root, where every TWindowBase is made). Everything below a
  window is part of it, a plain TObject of its own included. The marks (command 0xC2) grew from 8 to 12 bytes:
  the window's object address, the class's own hud_anchor.txt line (only the window's own class counts) and a
  split screen bit (camera_split_screen_selector 0x805D34B8 above 1, read by TCameraCtrl::get_view_count 0x8006B330).
- config/GPOE8P/hud_anchor.txt (new, built in by tools/embed_text.py; a copy beside settings.ini is read
  instead): class -> left / right / centre / stretch / auto / follow, prefixes with *, default line "* centre".
  Listed: TWindowSimpleStatus, TWindowEffectParty, TWindowPartyStatus, TWindowLockOn left; TWindowActionPalette,
  TWindowEffectLight, TWindowMessageInfomation right; TRaderMap*, TWindowTradeRequest, TObjSoloPlayWin,
  TObjDeadListWin auto. Positions from the game's 1-player layout table window_layout_rect_table (0x8044FE00, read
  from our DOL; get_window_layout_position 0x8026CBB8) and the decomp's TRaderMapCore tables (corner 480..608;
  its larger map modes 32..480 and 96..544, which stay centred by the measured rule).
- gx_render.c: each HUD draw of a window adds its x range on the 4:3 frame (through the projection and the
  viewport, cut to its scissor) and its vertices. At the frame's XFB copy each window gets one placement
  (pso_hud.h hud_decide: its own line, else the default; auto = inside the left third and left half pins left,
  inside the right third and right half pins right, else centre; split screen: centre), written to all its
  vertices (anchor codes LEFT / RIGHT, STRETCH for stretch) and its states (GX_WIDE_PIN_* flags; states carry the
  window's number in GX_XR_WIDE_FLAGS bits 8-15 so no state is shared by two windows).
- gx.vert: the left / right shift for HUD draws is (320 / ax) (1 - 1 / k) clip units (320 (k - 1) pixels in any
  viewport size; the same as before for full-width viewports). render_deko.c / render_gl.c: a pinned window's
  scissor moves by the same 320 (k - 1) (the radar's box).
- Log: a [hud] window line once per (class, layer, placement) with the x range, why (measured, hud_anchor.txt,
  default, split screen) and the draw count, at most 400; [hud] placements line at start (table source, classes,
  default); [wide] split screen on / off; the 10 s [wide] line adds windows per placement (frames x windows and
  draws), how many by their own line and how many kept in the middle for split screen.
- 4:3: no marks, no tags, no change to any draw (as build A). "Middle": exactly build A's placement.
- Host test tools/tests/run_pso_wide_test.sh, extended: the shipped hud_anchor.txt (no bad lines, 20 lookups), the
  parser (comments, CRLF, prefix precedence, later lines win, 4 kinds of bad line), one placement per window from
  the union of its draws (the status window's icons alone would stay centred), 14 measured ranges, own lines and
  the default, split screen, the edge shift in gx.vert's arithmetic for 5 viewports and the scissor, and the walk's
  windows under a container with their parts and the split bit; all build A checks: PASS.

## 2026-11-26: 16:9 widescreen, build A (test build, not yet run on the console)

The plan is docs/widescreen-research.md section 6, build A. What was built:

- Display > Screen: "4:3 (original)" / "16:9 widescreen" (menu.cpp, settings.c). 4:3 stays the default; a saved
  16:9 is no longer forced back to 4:3 (the old "16:9 stretched" and "Fit" values read as 16:9). SR_SCREEN=1 for
  tests. The 10 s [pace] line now ends with "screen 4:3" or "screen 16:9".
- Three game hooks (config/GPOE8P/hle.txt, runtime/src/pso_wide.c):
  - 0x803D676C set_perspective_projection_from_fov: in 16:9 the game's own function runs with aspect x k
    (k = 854 / 640 = 1.334375, the EFB width factor) and the fov changed so the vertical angle stays (tan of the
    half angle times k, rounded to a whole angle unit; pso_wide.h wide_fov_for). Not for the radar's call
    (return address 0x801F93F4). Every caller, the visibility tests and screen_projection follow.
  - 0x8022A87C TObject::render_nodes: the walk done natively (same frame, same loop); in 16:9 it also classifies
    each object from its layer and class name and tells the GP thread in order with the GX stream (runtime command
    0xC2, gx_fifo.c gx_host_mark; only while the FIFO is linked, never into a display list).
  - 0x803E20B0 load_screen_space_matrix: runs the game's function, then marks position matrix 0 as the screen
    space matrix (sent again when the projection's aspect / screen scale or the frame buffer change, and every
    256 loads).
- Placement (gx_render.c, GP thread): HUD layers (tl_window, tl_radermap, tl_clipout, tl_fadeafter), UI classes
  (TWindow*, TAd*, TTitle*, TLogo, TChCr*, TObjCM*, TObj*Win*, TShop*, TQuestList*, TKeyBoard*, TRaderMap*,
  TNowLoading, TLoadingCur, TLevelSelect, TCharSave*, TEndingPic, TFontSmall*, TCheckMemoryCard) and draws outside
  the task tree: screen space draws 4:3 in the middle, or stretched when they (or a band of them) span the width.
  The radar's own overhead view (3D with its 4:3 camera) also in the middle; HUD 3D with a widened camera is left
  as drawn. Follow mode already in build A: TWindowFukidasi (speech bubbles, per object) and TEF_LensFlare (per
  sprite) keep their place and their 4:3 width. Everything else (particles, sprites, fades in tl_fade, starlight
  trails) is left as the widened projection draws it.
- gx.vert: GX_WIDE_HUD_PERSP (flag 4 in GX_XR_WIDE_FLAGS) applies the anchor code to perspective draws, adds the
  follow code (16 + middle), and leaves the field of view setting out of HUD draws. render_deko.c / render_gl.c:
  for those draws the viewport centre and the scissor move by 320 (k - 1) instead of times k (the radar's scissor
  box offset included), unless the scissor spans the width.
- cull.c: no wide_k factor in the side margin (the projection itself is wider).
- Log: [hud] lines, one per new (layer, class, placement), at most 600 (SR_HUDLOG=0 turns them off); [wide] lines
  every 10 s with the hook and placement counts. 4:3 sends no marks and changes no draw.
- Online: nothing the game sends is touched (section 4 of the research): only the local projection and the local
  placement of the picture's parts.
- Host test tools/tests/run_pso_wide_test.sh: the projection maths (vertical angle kept within 0.008 degrees for
  every fov 0x800-0x7000 at three k), 27 class names, 3000 random trees walked by the native loop and the game's own
  recompiled one (same render calls, r1 / lr per call, stack words, callee-saved registers; 4:3 and 16:9), the
  marks for a game-shaped tree, and the viewport / scissor offset / follow arithmetic: PASS.

## 2026-11-25: Release 1.0.0

- The owner's console run of 468ad988: the attract movie at a steady 30 fps, native decode 6.2 to 6.6 ms per
  picture (longest 9.7) at 1224 MHz, guest idle about 19 ms per frame, 0 slices by the game's decoder.
- The owner then played every area of Episode I and Episode II, offline and online: Episode I at full speed at
  stock clocks; Episode II recommended at 1224 MHz to avoid occasional dips.
- 1.0.0 is main at this point: online play (0.6.0), the RVZ conversion, opening movies shown natively and decoded
  natively. Version and README only changed since the tested build.

## 2026-11-25: Native MPEG-1 decoding for the movies (test build, not yet run on the console)

- Cause of the slow attract movie (owner's log, build cf64678, 1224 MHz): the game core was 100% busy in movie.rel's
  own MPEG-1 decoder (CRI Sofdec, recompiled): block stores and prediction 0x818152C0, 0x818145E4, 0x81814B34,
  coefficient decoding 0x81819EB0, IDCT 0x8180822C / 0x81807E94; 11-16 fps, frames of 67-150 ms.
- Sofdec's decoder mapped from the disassembly (proven, then checked by running both): picture body 0x8180E7E0
  calls the slice decoder 0x8180E498 once per slice; the picture header picks the slice loop by picture type
  (ctx+0x22C) and the macroblock functions from a table (ctx+0x230..0x24C; the paired-single set when
  ctx+0x180 == 3); reference frames come in at ctx+0x1D0/0x1E0, the output at 0x200 (P pictures: 0x1E0 is the
  current frame, the skipped-macroblock copy writes there); dequantisation in integers, then (float)value *
  prescale[i] (ctx+0x34, 64 floats in the decoder's own coefficient order, scan table ctx+0x2C, matrices
  ctx+0xBE0/0xC20); a float AAN IDCT on paired singles rounded +-0.5 and saturated to s16; prediction with
  integer half-pel averages; dcbz on every 32-byte aligned destination row.
- Change: 0x8180E498 is replaced (hle.txt movie_slice, runtime/src/movie_mpeg.c): the slice is decoded natively
  with the game's exact arithmetic (the IDCT's float operations in the game's order, no FMA contraction:
  -ffp-contract=off plus a pragma; GCC fuses 8 operations without it), the dcbz clearing reproduced, the
  decoder state left as the game's loop leaves it, then the game's own start-code skip (0x8180E87C). Headers,
  reference frames, frame skipping, ADX audio and sync stay the game's code. Anything unexpected (another output
  mode or macroblock function, simplified half-pel mode, GQR set-up, IDCT constants, a bad code, an address or
  vector out of range, data running out) runs the game's slice decoder instead, logged once per reason; the
  macroblocks already written are rewritten identically by it. Pending interrupts are delivered between
  macroblocks. VLC tables typed from ISO/IEC 11172-2 annex B.
- Host tests (game data stays under build/ and assets/):
  - tools/tests/run_movie_mpeg_diff.sh: movie.rel's recompiled functions plus the game's maths library on the
    host, MPV initialised as the game does (mode 3), both decoders on every picture from the same memory, all 28
    MiB of guest RAM compared except the stack and the decoder's per-macroblock scratch: psogc_j.sfd 4429
    pictures, psogc_j2.sfd 4417, ult04.sfd 450: identical, no slice handed back. Damaged streams (bits flipped):
    112 of 4429 slices handed back on psogc_j, 44-47 of 450 on ult04, final memory still identical.
  - IDCT alone, 600,000 random blocks through 0x8180822C and the native one: identical (also built with GCC
    -O3 -march=native). VLC tables against pl_mpeg's (tools/tests/run_movie_vlc_check.sh): every bit pattern
    identical. Against FFmpeg (PyAV, tools/tests/movie_ref_compare.py, an integer IDCT): PSNR Y 72.4 dB
    (psogc_j, 1199 frames), 66.1 dB (psogc_j2), 69.7 dB (ult04), largest pixel difference 3, frame order right.
  - Speed on the host (Xeon 2.1 GHz, one thread, the whole picture call): psogc_j 0.55 ms/picture native vs 4.78
    ms for the game's code, psogc_j2 1.03 vs 8.95; busiest second of video 50-56 ms of decoding vs 375-424 ms.
    Switch estimate (the recompiled decoder ran about 7 times slower there than on this host, at 1224 MHz):
    about 5-9 ms per picture on average and 14-16 ms per picture in the busiest second at 1020 MHz, against 33
    ms per picture at 30 fps. Not measured on the console yet.
- The ending movies (ending_normal.sfd, ult01-04.sfd, texture mode) benefit only if their decoder is in the same
  output mode (ctx+0x180 == 3); otherwise the log says "another output mode" and they run as before.

## 2026-11-24: 0.6.1 test build (opening movies, RVZ, on 0.6.0)

- Black movies, cause proven in the disassembly: the attract and new-game movies (psogc_j.sfd, psogc_j2.sfd)
  run in the game's XFB mode. 0x80143C00 asks movie.rel for the decoded Y/Cb/Cr planes and 0x80143988 writes
  them with the CPU as YUYV into the back XFB, then VISetNextFrameBuffer; no GX draw or EFB copy happens, and
  the runtime presented only at an EFB-to-XFB copy. Nothing reads the guest XFB back.
- Fix: 0x80143988 is replaced (hle.txt movie_xfb, runtime/src/movie_xfb.c): BT.601 video range to a 640x480
  RGBA picture, centred as the game does, handed to the GP thread with a runtime-only command 0xC1 and shown 4:3
  through the normal XFB copy path. Review fixes: plane pointers checked against backed memory, upload barrier,
  shown only once uploaded, 0xC1 kept out of SR_DFF recordings.
- The ending movies (ending_normal.sfd, ult01-04.sfd) are texture-mode (an RGBA8 texture drawn with GX) and
  are not changed; not yet seen on the console.
- main now carries release-0.6 (version, README, ONLINE.md) plus the RVZ conversion and the movie fix; version
  0.6.1.

## 2026-11-24: Online test 4 (two Switches on Schtserv) and release 0.6.0

- Two Switches on the port played together on Schtserv (the owner and a community member). The other player was
  first refused with "You are banned from this server" (No.100): a new Hunter's License made in the game must
  have its serial registered on Schtserv's My HL page first. Documented in ONLINE.md.
- The owner's log (build 44f503f0): DNS through the router in 1.1 s, login 9103 -> 9225 -> 9704, about 9 minutes
  connected with no drop, all traffic over TCP (the game opened its peer UDP socket twice, 0 bytes), 30 fps in 58
  of 76 readings at 1224 MHz, a 2.7 s HOME visit kept the connection and the clock was set again after it.
- Release 0.6.0 is cut from that tested build on branch release-0.6 (version, README, ONLINE.md only). The RVZ
  conversion (ab2a32e, 3123b4d on main) is not in it: not yet run on the console.
## 2026-11-24: First-start RVZ conversion on the Switch

- Players often have Dolphin's .rvz. With no .iso/.gcm in sdmc:/switch/psoplus/ but an .rvz (or .wia), the first
  start now converts it once to `<name>.iso` before the game starts, then boots from the ISO as before
  (platform_disc_path -> disc_convert_switch.c). GCZ is not handled.
- Decoder: runtime/src/platform/switch/rvz_decode.c, a C port of tools/disc/rvz2iso.py (MPL-2.0): RVZ and WIA,
  compression none, purge, bzip2, LZMA, LZMA2, zstd; RVZ packed groups with junk regenerated by the disc's
  lagged Fibonacci generator; shared and all-zero groups; header 1 and header 2 SHA-1s and the file size
  checked at open. Libraries: devkitPro portlibs switch-libzstd 1.5.7 (BSD), switch-liblzma 5.8.2 (0BSD),
  switch-bzip2 1.0.8 (bzip2 license), linked in configure.py. NRO 46.7 MB -> 47.1 MB.
- Before converting: the RVZ's disc header must be GPOE8P revision 2 and its disc size 1,459,978,240 bytes;
  free SD space (fsFsGetFreeSpace) must be at least that plus 1 MiB, else a screen says how much is needed.
- Writing: `<name>.iso.part` with write() in 8 MiB buffers on a writer thread (decode and SD writes overlap);
  SHA-1 (libnx, ARMv8 crypto) over the output as it is made. After: size by stat, disc header read back
  (magic, GPOE8P, revision 2); SHA-1 compared with the clean dump (0ff15531..., as setup/build_pc.ps1) and
  logged; a mismatch (scrubbed or modified dump) keeps the file with a note on screen. Then rename (an
  existing `<name>.iso` is removed first: FAT cannot rename onto it); disc.c's own check runs as before.
- Interruption: closing from HOME (appletMainLoop false) deletes the .part at once; a `*.iso.part` left by a
  power cut is deleted at the next start and the conversion starts over (no resume). Auto sleep is off while
  converting. The .rvz is never changed.
- Screen: libnx's text console before the renderer exists (closed before render_deko.c makes its swapchain):
  title, "Converting your disc image once; later starts skip this.", the file names, a 60-cell bar, percent,
  MB done of total, time left (after 2% and 3 s), four redraws a second. Done: the time taken and that the .rvz
  can be deleted, 3 s (10 s with the SHA-1 note). Problems (no disc image at all, wrong game in the .rvz, not
  enough space, a damaged image, write or rename failure) show a message until + is pressed, instead of the
  program closing with only a log line. The no-disc screen also covers a folder with no image at all.
- Log: `[disc] <rvz> (RVZ, zstd, 128 KiB blocks): N bytes; SD card free space N bytes`, `[disc] converting: N of
  1392 MB` every 10 s, `[disc] converted in T (X MB/s): N bytes, SHA-1 H (matches the clean dump)`, or the
  failure / stop line.
- Host test (tools/tests/run_rvz_decode_test.sh, no game data): a synthetic 24 MiB GameCube disc
  (tools/tests/make_synthetic_gc.py: made-up ID ZTST01, FST, random, text and zero files, junk between files
  seeded like pressed discs) converted by nod's nodtool 2.0.0-alpha.9 (an independent writer, MIT/Apache-2.0,
  not shipped) to RVZ zstd, none, LZMA, LZMA2 and WIA LZMA, LZMA2 (nodtool's bzip2 writer fails, so bzip2 is
  untested): the C decoder's output equals the source image byte for byte and rvz2iso.py's output for all
  six; nod packed 685 junk runs, so junk regeneration is covered. A truncated image is refused at open; a
  flipped byte gives a zstd error or a different SHA-1.
- Real disc: not compared here (no copy of the owner's .rvz or ISO in the cloud workspace). Needs the console.

## 2026-11-23: Online build 4, review fix 2 (our chat keyboard is not time away)

- Review of 0d7b770: the applet hook counted every focus state other than InFocus as focus lost, including
  AppletFocusState_OutOfFocus, which our own chat keyboard (swkbd) causes; typing for 15 s or more would have
  ended the connections when the keyboard closed.
- Fix: the time away starts only at AppletFocusState_Background (HOME menu or sleep); OutOfFocus (an applet in
  front, such as the chat keyboard) is its own event and is not counted (platform_switch.c, net_native.c). A
  background report while chat_kbd.c's keyboard is open is ignored as well.
- Host test: 109 + 3 + 4 checks pass (gcc, clang), now with out of focus for 20 s (kept) and a background report
  while the stand-in chat keyboard is open (kept; SR_CHAT_DELAY_MS keeps the PC stand-in open for testing).

## 2026-11-23: Online build 4, review fix (HOME and sleep: end connections only after 15 s away)

- Review of fd9b7bd: AppletHookType_OnResume also comes after a short HOME visit (the default focus mode
  SuspendHomeSleep suspends the program at HOME too), so ending every connection on any resume would have
  disconnected a player who only looked at HOME for a moment.
- Fix (net_native.c): the applet hook records when focus was lost and when the program came back (resume or
  focus back), on the CPU tick clock and the wall clock (the larger difference counts: the tick clock may stand
  still while the console sleeps). Away 15 s or more (HOME or sleep): the connections are ended (-6, as
  before); shorter, or a resume with no recorded time away: kept, and a dead connection then shows through its
  socket errors or the once-a-second link check. Log: "[net] resume: away N s: connections kept" or "...
  connections ended (15 s or more)". Events are taken also while no socket is open, so an old HOME visit
  cannot end a later connection.
- Host test: 106 + 3 + 4 checks pass (gcc, clang), now with away 3 s (kept), time away unknown (kept) and
  asleep 20 s on the wall clock with the tick clock still (ended: status -6, send -6, receive callback -6).

## 2026-11-23: Online build 4 (chat with the system keyboard, clean disconnects, small cleanups)

- Chat (runtime/src/chat_kbd.c). How the game takes typed text, read in the disassembly: Sega's keyboard
  library 0x803F0048-0x803F1E90 (per channel 0x68 bytes at 0x805A2720) is polled from the SI sampling
  callback 0x801320CC: 0x8013206C tries channels 0-3 through 0x80131FC8, which reads 0x803F1604(channel, out)
  (0 = a keyboard: out gets the up to 4 key codes held now, 0-terminated) and the layout 0x803F1548(channel).
  Once a frame 0x80132650 turns the codes seen into presses (held now, not in the previous frame); the text
  windows (TKeyBoardControl 0x80131D9C holds a TWindowKeyBoardHard and a TWindowKeyBoardSoft) turn presses into
  characters with tables chosen by the layout (0x8025EE30: 1 = US, 0x8049E378 / 0x8049E3F8 shifted). Enter
  0x61, Esc 0x4C, backspace 0x50, arrows 0x5C-0x5F are commands, 0x54 / 0x55 shift, 0x56 / 0x57 modifiers
  (0x8025EC54, 0x8013240C); letters 0x10-0x29 (0x8025EC1C).
- Choice: a virtual US keyboard on keyboard channel 3 (port 4) while the network is up (from the game's network
  start to its stop): 0x803F1548, 0x803F1604 and 0x803F0E90 are replaced and run the game's own code for every
  other channel and for all channels while offline, so offline play is unchanged. The right stick click (no
  GameCube button; free in the default button map; ignored if the player mapped it) opens the system keyboard
  (swkbd, Latin, up to 80 characters) on a thread of its own: the game, its network pump and the render thread
  keep running behind it, so a long keyboard session does not stall the connection. The text is then typed one
  character per two frames (key and shift held one frame, nothing the next) with the game's own US table read
  backwards from guest memory, then Enter. Printable ASCII only (space is key 0x59, inferred: the table gives
  it a command code); other characters are left out and counted in the log. Not chosen: the on-screen keyboard
  window (its buffer and confirm flow would need a much larger hook).
- Clean disconnects (net_native.c): after the console sleeps (applet OnResume) every open connection is ended
  by the port (the game gets -6 on its next receive, send and status); while a connection is open the system's
  network is checked once a second (an address from nifm) and two failed checks in a row end the connections
  with -7 and report the link down; socket errors on an open connection map to -6 (reset, broken pipe, other)
  or -7 (timed out, network or host unreachable, network down). A broken connection's status call now reports
  that negative state, so the game's next send gives up at once. Focus changes are logged with the time away.
  "[net] session end" (time, lookups, connections, disconnects and the last reason, bytes) when the game stops
  its network and at exit if it had not.
- Cleanups: the "[boot] the game tried to start switcher.dol" line at most once per 10 s (the scene still goes
  back each time); settings.ini saves net_server empty when it is the default (empty reads as the default).
- Host tests: net_native_test 104 + 3 checks, settings_net_test 4 checks, all pass (gcc, clang): chat text to
  key frames (with layout tables of our own), the hooks offline and online, a typed line frame by frame, a sleep
  while connected (-6 from status, send and the receive callback), errno mapping, the settings round trip.

## 2026-11-23: online test 3 (cdf10e9) results: Schtserv lobby reached

- The owner reached the Schtserv lobby, created his own team (game) and teleported to Pioneer 2 in it.
- Log (stamp cdf10e9d, about 250 s, CPU 1224 MHz by his menu setting): built-in network settings, first card
  backup kept and this session's dated backup written, sockets and nifm started, DHCP from the system
  (192.168.50.208/24, DNS 192.168.50.1), psobb.dyndns.org asked of 192.168.50.1 -> 3.18.217.27 in 1067 ms,
  TCP 3.18.217.27:9103 connected in 3035 ms, then the server's redirects 9225 (96 ms) and 9704 (30 ms), and
  steady TCP traffic in every 10 s window after that (up to 8188 bytes received). UDP 0 in both directions:
  the server keeps game traffic on TCP, as expected.
- No [net] errors, no stall, 30.0 fps in the game on Pioneer 2 (game work 9-15 ms, GPU 8-10 ms).
- Not yet seen: other players in the same game, quests online, the session end (the log stops without a
  shutdown line, so it was closed from HOME or by power), HOME/sleep while online, chat text entry.

## 2026-11-23: Online build 3 (the server's address from net_server, psobb.dyndns.org)

- Console test of 6575100c: network start, card backups, DHCP from nifm (192.168.50.208/24, gateway .1),
  interface, route and resolver set-up all ran; then "DNS game04.st-pso.games.sega.net: no answer from
  3.18.217.27:53 after 2 queries; asking 144.217.87.48:53 ... FAILED: no answer from either server after 7
  queries, 6504 ms, -0x45"; the game showed error 102 and shut its stack down cleanly. Neither Schtserv DNS
  answered from the owner's network.
- ragol.org's / Schtserv's Action Replay code for US v1.02 Plus writes "psobb.dyndns.org" over the hostname at
  0x8043EDA0, and that name resolves (from the cloud, getent) to 3.18.217.27: the server's own name in
  ordinary DNS, no DNS redirect needed.
- Change: for the game's three server names, the game now gets the address of settings.ini net_server
  (default psobb.dyndns.org; an IPv4 address there is used as it is; net_server=off skips this), looked up
  with the system's resolver: on the Switch our A query to the DNS servers nifm reports (the second one after
  2 unanswered queries, 4.5 s for this step), on PC getaddrinfo (one call). If that fails, the game's own name
  is asked of Schtserv's DNS (3.18.217.27, then 144.217.87.48) as in build 2, then the game gets the error.
  net_dns, when set, replaces both steps as before (the game's name asked of it only). SR_NET_FALLBACK_DNS
  (env.txt) replaces Schtserv's list, for testing. The poll contract is unchanged (start returns at once,
  -1 until done). Log: the method and the address ("-> a.b.c.d (net_server, asked of the system's DNS
  servers, N ms)"), and "net_server ... failed (...): asking the DNS servers for ..." on the fallback. Menu:
  "Server" (net_server) and "Then DNS" lines, note updated.
- Host test: 94 + 3 checks pass (gcc, clang), now with net_server through the system resolver (localhost), an
  address in net_server, an unknown net_server falling back to the DNS list in order, and net_server=off.

## 2026-11-23: Online build 2 (built-in network settings, the browser start logged)

- Console test of c72c225b (online build 1 with the review fixes): choosing Online gave a black screen; no
  network start. Cause (the coordinator's reading, re-confirmed in the disassembly): the online menu object
  0x8032C618 runs the network settings load 0x80145A94; with no network settings file on the card it returned
  0, so the menu changes to scene 19 (0x8032C678 -> 0x8000F9CC(0x13); scene table 0x804805E0, scene 19 =
  init 0x803354E8, update 0x803354A8). That scene only starts the disc's web browser: 0x8007FA24 picks the
  page (0x8048DF88: "file:/sonic/config.htm" for the setup, "file:/sonic/index.html" when settings exist) and
  0x8007F970 loads switcher.dol (0x80438D20), stops video, audio and GX (0x8037CA40, 0x8037C8B8, 0x8037BC54)
  and waits in 0x8038A058 for good (the log's parked stack). One correction: scene 19 starts the browser in
  both cases, so it is the browser scene rather than an "online" step; with settings present the menu never
  goes there (0x8032C664 -> 0x8014797C, then the menu goes on).
- The network settings (Sega's settings storage 0x803F2F1C-0x803F632C, read from the card file the disc's
  setup browser writes): the game only reads it; no game code reaches the storage's write functions (all 32
  game calls into the storage listed: init, term, card read, getters). Layout from the getters and the reset
  to defaults 0x803F47BC: a 0x19D4-byte struct at *0x805D79C0, +2 the selected entry (-1 = none), entries of
  0x28 bytes from +0x28 with the profile number, profiles at +0x210 * n with kind +0x206 (4 = Broadband
  Adapter for 0x803F5678 and the other BBA getters, 1 = modem) and address method +0x2B0 (0 DHCP, 1 PPPoE,
  other values fixed (inferred), from 0x80145EE4 / 0x80145E90). The consumer 0x80145F40 copies the BBA getters into 0x8050CCE8
  (+0x120 = valid, which 0x80146B08 checks).
- Change 1: 0x80145A94 is replaced (runtime/src/net_native.c, hle_net_settings_load): the storage is started
  as the original does (0x803F3D60 if it was running, 0x803F3BF8(0, the game's buffer, 4)), the card is not
  touched, and a built-in profile goes into the storage's struct: entry 0 selected, profile 0, kind 4, DHCP,
  everything else 0 (no fixed addresses; DNS from DHCP, 0x801471A0). Storage state 2 (what a read leaves,
  0x803F3EA0) and the game's loaded bit 0x805D56CC; returns 1. A network settings file on the card, if one
  exists, is ignored.
- Change 2: the browser start 0x8007F970 is replaced: "[boot] the game tried to start switcher.dol (the
  disc's web browser, page ...): not supported". In scene 19 the game goes back to the screen it came from
  (its own scene change 0x8000F9CC with the previous scene, 0x804FE120 + 0x14); from anywhere else (0x8007F898,
  a second caller) it only returns. OSResetSystem 0x80373C80 logs "[boot] the game asked for a system reset"
  and then runs as before (it does not come back): the reset hang itself is not fixed. No error message of the
  game's own is shown: none was found that is safe to call from here.
- Kept from c72c225: DNS fallback order, the card backup rule, sockets started at the first network start.
- Host test: 81 + 3 checks pass (gcc, clang), now with the built-in profile (storage start, fields, state,
  a second load) and the browser start (back to the previous scene only from scene 19).

## 2026-11-23: Online build 1, review fixes

- Review of ac381cd (independent reviewer): replacements and pump sound; two fixes required, one recommended.
- DNS servers: by default Schtserv's 3.18.217.27 (named by a Schtserv admin, forum post of Sep 2023,
  schtserv.com/forums viewtopic p=14183), then its older 144.217.87.48 when the first has not answered 2
  queries (about 2 s); 1 s retries, 6.5 s for the whole lookup (the game's own loop waits up to 5400 frames).
  settings.ini net_dns overrides the list: one address (only that one is asked) or two separated by a comma;
  empty (the default) means the two above. The log names the server that answered ("answer from a.b.c.d:53")
  and each switch to the next server. Menu note updated.
- Memory card backup at the game's first network start of a session (avetcp init 0x80421674, before the game
  writes its network settings file or connects), no longer at the first TCP connect:
  saves/card_a.before-online.raw is written only if it does not exist (the first backup is never
  overwritten), and saves/card_a.before-online-YYYYMMDD-HHMMSS.raw once per session (a new name each time;
  512 KiB each, the player deletes old ones).
- Sockets (socketInitializeDefault) and nifm start at that same first network start (or the first lookup or
  socket, whichever comes first), not at boot: a session that never goes online makes no socket or nifm call.
  The adapter type report at boot is unchanged; link status before the start answers "up" without asking.
- Host test: 71 + 3 checks pass (gcc, clang), now with a two-server list whose first server never answers
  (answer from the second after 2 queries and about 2 s; timeout after 2 + 5 queries), the backup kept when it
  exists and written when it does not (the script runs the test twice), the default list when net_dns is empty.

## 2026-11-23: Online build 1 (native sockets in place of the game's network library, option A)

- Goal: from the title screen's online option, connect to Schtserv and reach its lobby with libnx BSD sockets.
  No Broadband Adapter is modelled and the game's AVE-TCP stack, BBA driver and modem driver never run:
  config/GPOE8P/hle.txt points all 51 library functions the game's code calls (plus the game's own PPPoE
  connect 0x80146314) at runtime/src/net_native.c. The game's connection manager, socket classes and protocol
  run as recompiled.
- Adapter: ntUtl 0x8042EE4C is replaced (type 5, Broadband Adapter, also written to its cache 0x805D7CC8) rather
  than giving EXI channel 0 device 2 the BBA ID: the function has four callers and no side effects, while an
  EXI device would also answer the BBA driver's register traffic and the EXI library's attach path, which this
  build never wants to run. Link status 0x80426BD8 = the console has an address (nifm), asked once a second.
- IP set-up: the DHCP calls report the Switch's own address, mask, gateway and DNS servers from nifm
  (nifmGetCurrentIpConfigInfo); with no system address (PC) the address the host routes from, mask
  255.255.255.0, gateway .1 (only the game's screens show these). A PPPoE setting is connected as DHCP.
- DNS: the three names of the server table 0x804A6FD8 are asked of settings.ini net_dns (default 144.217.87.48,
  changed by the review fixes below; "a.b.c.d" or "a.b.c.d:port") with our own A query (runtime/src/net_dns.c), 1 s retries, 3.5 s timeout
  (-0x45 to the game), no such name -0x42. Other names: the system's DNS server (Switch), getaddrinfo (PC).
- TCP and UDP: non-blocking sockets; receive callbacks from a pump run after each display flip
  (frame_pacing.c, net_frame) on the game thread, on the caller's registers (saved and put back) in a
  0x80-byte frame below its stack pointer. One callback per arming, as the library (the game re-arms from
  0x801DEA7C). Sends that the socket cannot take at once are queued and sent from the pump.
- Save safety: before the first TCP connect of a session the slot A card (the in-memory image) is written to
  saves/card_a.before-online.raw (exi_card_backup; changed by the review fixes below). Port menu, System page: "Online": server DNS and
  connection state. Log: [net] lines (first call of each replacement, every lookup and answer, connect
  attempts and results with IP:port and errno, disconnects, traffic per 10 s).
- Analysis re-read for this build (new findings, all in the disassembly): ports given to the library are
  plain numbers that go on the wire as they are (the library's own DNS opens UDP port 0x35 at 0x8040AB88), so
  the game's peer UDP ports are byte-swapped by 0x801E3EFC (4000 + id becomes 0xA00F for id 0); a second UDP
  open on the same local port gets -3 (0x804212E4); the game drops the connection when the status call's send
  space is not larger than the packet (0x800786B0); the game configures one TCP endpoint (0x80146934); the
  object handler at +0x880 is only ever set to 0 (0x80079020); the library's error names (0x8048D3F8): -1
  PENDING, -4 INVALIDHANDLE, -6 CONNRESET, -7 UNREACHABLE, -8 RECVFIN, -11 TIMEOUT, -12 NOBUF.
- Tests: tools/tests/run_net_native_test.sh (host, loopback servers): 70 checks pass (gcc and clang): DNS
  codec against hand-built packets, lookups (answer, NXDOMAIN, timeout after 4 queries, system resolver),
  TCP connect, one-shot receive 3000 bytes as 2048 + 952, send, server close as -8, refused connect, UDP open,
  duplicate port -3, two-phase callback, registers put back after callbacks.
- Not testable from the cloud: the cloud answers every outgoing UDP port 53 itself (144.217.87.48,
  3.18.217.27, 3.18.217.28 and 93.184.215.14 all gave the same ordinary answers, NXDOMAIN for
  game04.st-pso.games.sega.net), and outgoing TCP is refused, so Schtserv's DNS answer and the game server are
  first seen on the console. A Schtserv admin's forum post gives 3.18.217.27 as the current DNS
  (schtserv.com/forums viewtopic p=14183); if 144.217.87.48 fails, net_dns=3.18.217.27 is the next try.
- Not in this build: the software keyboard for chat (next build).

## 2026-11-23: Switch test 14 (0.5.0 build, ff131ea) results

- Log stamp ff131ea6 (version 0.5.0), about 410 s, mostly stock clocks. The first log line and [build] show
  0.5.0; one "priority 0x3B is already the lowest" line and no svcSetThreadPriority failures.
- The menu's CPU clock setting works: "CPU 1224 MHz requested (menu setting): rc 0x0, reads back 1224 MHz", and
  "set back to stock 1020 MHz", each logged and read back.
- Menu closed, normal play: 28.9-30.0 fps; game work up to 30 ms in the busiest windows; GP thread 12-21 ms a
  frame on average, worst frames 20-27 ms (on the CPU 17-23). [hitch] counts per 10 s were 0 in most windows
  after 90 s (10 at 320 s); the early ones are loading (texture lookups 16-26 ms in the worst frames).
- Menu open (about 200-260 s): still 25-28 fps. Menu pictures took up to 73-77 ms each (25-49 per 10 s), and
  the GP thread's worst frames were 40-52 ms busy with only 14-19 ms on the CPU, so the GP thread still lost its
  core for 25-35 ms while the menu was drawing. The 0.25 ms yield did not stop it; cause not yet proven.

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

- The owner: FPS counter fine; button map, Reset and saving all work; the CPU menu setting works (his test; the
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

- The owner played Forest 1 and then Episode II, part of it with sys-clk at CPU 1428 MHz (the log's [pace] lines show
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

- The owner: Forest 1 held a steady 30 fps, including the busy spot that dropped to 20-25 fps in tests 4-9. Stock
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

- Test 3 (3d6b7ff4): sound works (owner). Black screen right after choosing offline mode, with no movie.rel
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
- Test 2 (0e733963): boots, 30 fps, but no sound (owner). Character creation: OSLink of movie.rel logged, then
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

- The owner approved MPL-2.0, our own copy of the runtime, and matching the original frame rate.
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

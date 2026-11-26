# Widescreen (16:9) research

Research pass, 2026-11-26, after release 1.0.0. Nothing here is built yet. Goal: a real 16:9 picture (more of
the world to the sides, nothing stretched), the HUD first kept 4:3 in the middle, later moved to the screen edges.

Marks: **proven** (read in our DOL's disassembly in `build/GPOE8P/asm`, in our runtime, or in the decomp's
source where the decomp's function is shown to match ours), **inferred** (reasoning, not checked at run time).

Sources: zeroz41/pso-decomp (MIT, commit 50d01e6, cloned to /tmp, names only; nothing copied into this repo),
its symbol map `config/GPOE8P/psov3/symbols.txt`, our disassembly, our runtime. The ragol.org report that
GameMasterPLC made a widescreen hack for US Plus by moving the UI in the executable is taken as proof that the
idea works on this game, as passed on to us; it was not checked here and nothing from it is used.

Every address below is PsoV3.dol (GPOE8P rev 2). The decomp's addresses agree with ours for every function
checked (803D676C, 80229A84, 80229EEC, 8022A87C: same code, same callers, same globals). Proven.

## Summary

- The whole game builds its 3D projection in **one function**, `set_perspective_projection_from_fov`
  (803D676C), with a hard-coded aspect of 1.1333334 at every call site. Patching the aspect there (vertical angle
  kept) widens the camera **and** the game's own visibility tests at once, because those tests project through
  the GX projection that function sets. Proven. This is the cleanest hook (section 1).
- The shader-only approach (keep the game's 4:3 projection, widen in `gx.vert`) leaves the game's own culling
  at 4:3: objects, items and doors would pop in at the new side areas. Not recommended for the 3D.
- PSO draws **no** HUD with an orthographic projection: the only `C_MTXOrtho` user is a debug font. All 2D
  (HUD, menus, fades, and also 3D-anchored sprites and particles) is drawn through the perspective projection
  with a "screen space" matrix from `load_screen_space_matrix` (803E20B0). Proven. So the runtime's inherited
  Riders HUD anchoring (orthographic draws only) never sees PSO's HUD; it must be extended (section 2).
- The game's task tree tells HUD from world: TMainTask renders named task layers (tl_window, tl_radermap,
  tl_clipout, tl_fade, tl_fadeafter, ...), and every task carries its class name. One native replacement of
  `TObject::render_nodes` (8022A87C) gives the runtime the current layer and object for every draw. Proven.
- Game hooks needed: 3 for build A (803D676C, 8022A87C, 803E20B0), no more for build B (a class table and
  per-object placement in the runtime). Plus runtime work in gx_render.c, gx.vert, both backends, cull.c,
  settings and the menu.
- Online: nothing about the view is sent; settings_fix_wide_k has no caller in PSO; the view only changes local
  drawing and local lock-on candidates, the same things a camera turn changes. Safe (section 4).
- Cost: 33% more pixels, up to about a third more visible objects in open areas. Episode II, already GPU heavy
  (21-24 ms a frame at stock in test 11), is the risk (section 5).

## 1. 3D: projection, culling, pop-in

### Where the projection is built

`set_perspective_projection_from_fov(s32 fov, f32 aspect, f32 near, f32 far)` at **803D676C** (0x1F4 bytes).
Proven (disassembly matches the decomp line by line). What it does:

1. `fov` is a horizontal angle (65536 units a turn) scaled by 0.85 (`fov_angle_scale`, 805E0E18). The vertical
   angle is derived: `vert = 2 * atan(tan(fov * 0.85 / 2) / aspect)`.
2. `C_MTXPerspective` (80376E94) with that vertical angle and `aspect`, then `GXSetProjection` (8038E8F0).
3. Stores `scene_projection_state` (8058CA60: matrix, type, near, far, **aspect** at +0x4C, screen_scale,
   inv_screen_scale), `screen_projection` (8058D4F8: distance, `width = aspect / 1.3333334` at +0x1C, scales)
   and `camera_clip_parameters` (8058D4D0).

Callers (every `bl 803D676C` in the DOL), all passing aspect 1.1333334. Proven:

| Call site | Function | What |
|---|---|---|
| 80068FB4 | TCameraCtrl::restore_saved_view (80068EF0) | main camera |
| 8006A9B0 | TCameraCtrl::update_active_controller (8006A7D8) | main camera, every frame |
| 8006B0A8 | TCameraCtrl::setup_default_view (8006AF9C) | main camera |
| 80083138 | TLiveCameraView::render_shadows (800830B0) | main view put back after the shadow pass |
| 800F90EC, 800F9170, 800F91E4, 800F9288, 800F9304 | TLobby::run_disk_error_check (800F8FB8) | lobby / error screens, fov 0x3555 |
| 801448A0 | TMovie::run_task (80144778) | the game's movie plane (fov 0x4B) |
| 801F93F0 | TRaderMapCore::setup_overhead_projection (801F9314) | **the radar's** overhead view |
| 80338344 | draw_loading_compass (803382E4) | the loading screen's compass |

Other `GXSetProjection` users: the shadow map pass (begin_shadow_map_pass 80083D54: a 256x256 viewport and a
`C_MTXFrustum` of its own; end_shadow_map_pass 80083B8C restores `scene_projection_state`), and the debug font
(render_debug_text 803D85D8, the only orthographic projection). Proven.

1.1333334 is 0.85 x 4/3: the game's 3D is drawn slightly wider than square pixels would give. That is the
original look and stays (proven arithmetic: x scale / y scale in pixels = (4/3) / 1.1333 = 1.176).

### The hook: patch the aspect at its source (recommended)

HLE `803D676C`: when the wide mode is on and the call is not the radar's, call the game's own function with
`aspect * k` and with `fov` replaced so the vertical angle is unchanged:
`tan(h') = k * tan(h)`, `fov' = round(2 * atan(k * tan(fov * 0.85 / 2)) / 0.85)` in angle units (one unit is
0.0055 degrees, so the rounding is invisible). k = 854/640 = 1.334375 (settings.c round_aspect for 16:9).
About 20 lines. Inferred design; the formula is proven from the function's code.

Why at the source and not in the shader. Proven unless marked:

- Everything the function derives follows: `scene_projection_state.aspect`, `screen_projection.width` and
  `scale_x`, the GX library's stored projection (`__GXData`, which `GXGetProjectionv` returns).
- The game's object visibility tests project through that stored projection and compare with 0..640:
  `TObjBase::update_object_view_visibility` (801A2340) and `is_point_on_screen` (80100A40, items, mags) call
  `project_world_to_screen` (803D8D1C), which is `GXGetProjectionv` + `GXGetViewportv` + `GXProject`. With the
  aspect patched, x 0..640 covers the whole wide view, so these tests widen by themselves. The overrides
  (TItem 80100B8C, TOComputerMachine01 8015B2C4, TOKey 80169FBC, TObjCamera 8017F6C0, TObjContainerIdo
  8018BEE4, TObjHashiBase 80191874, TObjLazerFence4 801930E8, TMotorcycle 80194E40) either call the base test
  or project the same way (decompiled ones read; the rest inferred).
- 3D-anchored sprites and particles (TQuadSprite* in tl_particle, TParticle, TTecZondeEff, enemy effects) work
  out their screen size and place from `screen_projection.width` and `distance`, then draw through the screen
  space matrix. With `width` patched their positions and shapes stay right (checked by working the formulas of
  TQuadSpriteNormal and TParticle through: position and width both scale by 1/k in game units, which the wide
  screen space matrix maps back to the same physical size).
- `load_screen_space_matrix` (803E20B0) multiplies by `aspect / (fbWidth / efbHeight)` while the projection
  divides by `aspect`: the two cancel, so screen space x 0..640 always fills the full viewport width at any
  aspect. Proven (decomp source of both, matrix entries [0][0] and [0][2]).
- The shader-side approach (gx.vert dividing perspective clip x by k, the game left at 4:3) would draw the side
  areas, but every test above would still cull at the old 4:3 edge: objects, items, doors, traps and enemies
  appearing only when they reach the old frame (pop-in). Fixing that needs a hook per visibility function (at
  least 10, some not decompiled). It would also squeeze the screen space HUD into the middle, which is handy,
  but section 2 gets that more cleanly.
- The radar call (801F93F0) is left alone: the radar is HUD (section 2). The loading compass and the movie
  plane are centred 3D; widening their projection keeps them the same size in the middle, so they need no
  exception (inferred).
- The port's field of view setting today scales the picture in gx.vert (GX_XR_FOV), so above 100% it has the
  same culling gap. The same hook can apply it at the source (vertical angle times the setting) and retire the
  shader path. Inferred; optional.

### Culling sites and how they widen

| Site | Address | How it decides | With the source patch |
|---|---|---|---|
| Map sections, mode 0 / 1 | TScene::cull_sections_within_map_radius, cull_sections (via TScene::render, `get_cull_mode`) | XZ only: always drawn within 60 degrees of the camera yaw (dot >= 0.5); beyond that, kept if within the section radius of an edge line turned by `camera_ctrl_instance->fov` | Nothing to do. 16:9 at 100% field of view is about 39.7 degrees half width, 49.1 at the 130% setting, both inside the 60 degree rule. Proven code, inferred margins (computed). |
| Map sections, mode 2 | TScene::update_room_visibility_from_camera | the floor's room visibility table (rooms seen from the room the camera is in) | Not angle based. Nothing to do. Proven. |
| Scene objects in sections | TScene::is_in_view_depth | depth only (in front, within s_near + 80) | Nothing to do. Proven. |
| Objects, items, mags | 801A2340, 80100A40 and overrides (above) | screen x 0..640 via GXProject | Widen automatically. Proven. |
| Our model cull (off unless SR_CULL=1) | runtime/src/cull.c `model_hidden` | reads the GX projection from `__GXData`, side margin `1.15 * wide_k` | Drop the `wide_k` factor: the projection itself is already wider. Proven. |
| Hardware clip | our backends | viewport and scissor x times k (render_deko.c apply_state, XSC) | Already in place. Proven. |

What could still pop in at the new edges. Inferred:

- Classes whose visibility code is not decompiled and that might test against a 4:3 bound of their own
  (constants such as 640 or 320 in the class). The decompiled ones all go through GXProject. Check in build A
  by turning slowly in busy rooms.
- Effects culled by distance only are unaffected.
- Distance fades (`CameraDistanceFade`) are by distance, not angle.

### Other things the wide EFB touches

- Shadow map pass: a 256x256 viewport; the backend scales the viewport and the EFB copy source by the same k
  (render_deko.c lines 500 and 951), so the copy comes back square. Proven code, inferred result.
- Split screen (offline 2 to 4 players): viewports are halves or quarters of the 640x480 frame, all scaled by k,
  and every view's projection goes through the same hook. Should work; if a view looks wrong, k = 1 while
  `is_split_screen()` is true is a one-line fallback. Inferred.
- The port's own movie picture (movie_xfb.c, shown by the backend) is already presented at most 4:3 and
  centred (render_deko.c around line 976). Proven. Stays 4:3.

## 2. HUD and 2D

### How the game draws 2D

Proven:

- Screen space draws: x 0..640, y 0..480 game units with a depth term `w`; `load_screen_space_matrix`
  (803E20B0) loads the matrix into PNMTX0 and the draw goes through the current **perspective** projection.
  Nine functions load it: begin_screen_draw (803DEE6C, 102 call sites), begin_screen_textured_draw (803E2EE4),
  setup_projected_color_draw (803E2D10), draw_screen_color_strip (803E26A4), draw_screen_textured_strip
  (803E2810), draw_polygon_2d (801E35C4), draw_color_lines (801E3330), draw_textured_screen_quad (800842A4),
  draw_screen_quad (80084038).
- The same path draws the HUD **and** world-anchored things: particles and sprites (tl_particle), lens flare
  (TEF_LensFlare, tl_05), speech bubbles (TWindowFukidasi places itself with `project_world_to_screen`).
- The lock-on cursors are 3D models (TLockOnCursor draws a chunk model turned to the camera yaw; TMainCursor
  via render_marker, inferred the same), in tl_loc_start / tl_loc_end, drawn in the geometry pass. They follow
  the 3D by themselves.
- The radar (TRaderMapCore, tl_radermap) renders its own 3D overhead view (801F9314) centred at 320,240 and
  moves it to its corner with `set_screen_center` and `GXSetScissorBoxOffset` (BP 0x59), with its own scissor
  (TRaderMapCore::set_view_scissor). Its 1P centre in the data table is 544,128 (upper right).

### The task layers (the HUD's natural seam)

TMainTask::render (802298D8) draws, in order: render_geometry (80229B9C: all layers except the excluded ones,
so tl_00 to tl_06, tl_loc_*, tl_item_equip), render_effects (tl_07), render_particle_effects (80229AF0:
tl_particle, tl_particlemodel), render_objects, then **render_ui** (80229A84: tl_window, tl_radermap). After the
frame, render_screen_overlay (802294E4) runs **render_clipout_and_fade** (80229EEC: tl_clipout, tl_fade,
tl_fadeafter) and render_info_icon_overlay (803384D4). Layer pointers: tl_window 805D6374, tl_radermap
805D6380, tl_clipout 805D6384, tl_fade 805D6388, tl_fadeafter 805D638C (the rest 805D6340 to 805D637C). Proven.

`TObject::render_nodes` (8022A87C, 0x60 bytes) walks the children: skip if flag 0x10 (+4), call the render
method (vtable at +0x18, slot +0x10), recurse; children at +0x14, next at +0xC. Every TObject has its class
name string at +0 (e.g. "TWindowPartyStatus"). Proven (disassembly and decomp header).

But UI is not only in the UI layers. Objects made directly under each layer (decomp, `new X(tl_..)`). Proven
for those listed:

- tl_window: TWindowCtrlScreen (the window manager), TAdDialingGC, TAdScrollWinGC.
- tl_clipout: TWindowCtrlClipOut, TRaderMapCore's parts, TAdGameStartGC, TAdSelectCurGC, TObjWarpString,
  TDLGraph, T_ATS_DrawCalendar, TUnk_804EE470 (the title's overlay).
- tl_fade: TScreenFadeController, TScreenFadeIn, TScreenFadeOut, TAdFadeGC.
- tl_fadeafter: TWindowCtrlMain (message windows over fades).
- **tl_00**: TWindowChat, TWindowCtrl, TKeyBoardControl (drawn in the geometry pass).
- **tl_01**: TTitleScene, TLogo, TWindowCtrlSub, the front end (TAdSinglePlyChrSelectGC, TAdStorySelectGC,
  TAdSelectGameModeGC, TAdMultiSaveGC, licence and network screens).
- **tl_07**: TObjResultWin, TWindowReturnCity, TWindowSetLevelUp, TPlayerLevelFeedback.
- **tl_03**: character creation tags (TChCrTagName and others). **tl_02**: TAdEnterPasswordGC,
  TAdEnterSerialAccessGC.
- Outside the task tree: the loading compass (803382E4) from wait loops (open_afs_file_wait_render 801E3E9C,
  TLobby::run_frame 800F9E18, TLobby::wait_for_sound_init 802280C4, init_sound_aux_effects 8024034C).

So the rule for a screen space draw has to use both the layer and the class name:

1. HUD if the layer is tl_window, tl_radermap, tl_clipout or tl_fadeafter; or the draw happens outside
   TMainTask's tree (loading screens); or the object or one of its parents has a UI class name (TWindow*,
   TAd*, TTitle*, TLogo, TObj*Win, TChCr*, TObjCM*, TNowLoading, TLoadingCur, TLevelSelect, TCharSave*,
   TEndingPic, TFontSmall*; a table, logged so we can add to it).
2. Inside a HUD draw: a draw spanning the full 640 width (backgrounds, dimming, fades) is stretched to the
   screen (the Riders rule in gx_render.c wide_anchor, extended to screen space draws).
3. Everything else (screen space draws by world objects, and tl_fade) is left as the source patch leaves it:
   stretched across the wide screen, which is right for particles, sprites and full-screen fades.
4. "Follow" exceptions: fixed-size 2D placed from a 3D point keeps its place but would come out k times wider
   (speech bubbles TWindowFukidasi, lens flare TEF_LensFlare, TEffStarLight2D_Base, possibly TObjWarpString).
   Build B gives them a follow mode (centre kept, width divided by k). In build A they are just wider.

### Inventory and proposed placement

Placement codes: **L** pin left, **R** pin right, **C** centre (4:3), **S** stretch, **3D** follows the world.
Positions come from the decomp where noted; the rest are inferred and get measured with the anchor log in build
A (SR_ANCHORLOG plus the class name per draw).

| Element | Class / layer | Build A | Build B |
|---|---|---|---|
| Player status (name, level, HP/TP bars) | TWindowSimpleStatus, TWindowSubStatusDisp*, TWindowStatusGauge (tl_window) | C | L (or R, from its measured side) |
| Party status (other players' HP) | TWindowPartyStatus (tl_window) | C | same side as player status |
| Action palette | TWindowActionPalette (tl_window) | C | R or L, from its measured side |
| Radar / minimap | TRaderMapCore (tl_radermap, parts in tl_clipout), centre 544,128 | C | R |
| Lock-on cursors | TLockOnCursor, TMainCursor (tl_loc_*), 3D models | 3D | 3D |
| Locked target window | TWindowLockOn (tl_window) | C | C (top) or measured side |
| Damage numbers | particles (inferred), tl_particle | 3D | 3D |
| Speech bubbles | TWindowFukidasi | 3D (k wider) | follow |
| Chat log | TWindowChat (tl_00), TWindowMessageLog | C | L |
| Word select, symbol chat | TWindowDD_WS_*, TWindowSubSymbolChat* | C | C |
| Software keyboard | TWindowKeyBoardSoft/Hard, TKeyBoardControl (tl_00) | C | C |
| Message windows, yes / no, quest text | TWindowMessage*, TWindowCtrlMain (tl_fadeafter) | C | C |
| Main menu (in field) | TWindowMainMenu1P/4P, TWindowMenuMainMenu | C | C |
| Item / equipment / mag / status pages | TWindowSubItem*, TWindowSubEquip*, TWindowSubMag*, TWindowStatus | C | C |
| Shops, tekker | TShop*Window, TWindowShop | C | C |
| Bank | TWindowDeposit | C | C |
| Guild card | TWindowComCard*, TWindowSubComCard* | C | C |
| Quest board, counters | TWindowSubQuestBoard*, TQuestList* | C | C |
| Lobby menus, team | TWindowRoom, TWindowMakeTeam, TLobby screens | C | C |
| Title, logos | TTitleScene, TAdTitleGC, TAdTitleTexGC, TLogo (tl_01) | C, backgrounds S | same |
| Character select, creation | TAdSinglePlyChrSelectGC, TAdMultiPlyChrSelectGC, TChCr*, TObjCM* | C, backgrounds S | same |
| Options | TWindowOptionCtrl* | C | C |
| Pause / game end | TWindowSubGameEnd, TWindowReturnCity (tl_07) | C | C |
| Results, level up, new floor | TObjResultWin, TWindowSetLevelUp (tl_07), TObjNewFloorWin | C | C |
| Loading screen | TNowLoading, TLoadingCur, loading compass (outside the tree) | C | C |
| Full-screen fades | TScreenFade*, TAdFadeGC (tl_fade) | S | S |
| Menu dimming, window backgrounds spanning 640 | any HUD draw spanning the width | S | S |
| Info icon (memory card / network) | render_info_icon_overlay (803384D4) | C | R or L, measured |
| The port's movie picture | movie_xfb.c, presented by the backend | 4:3 | 4:3 |
| The port's own menu and perf readout | menu.cpp, overlay | unchanged | unchanged |

Distinct hooks: 3 game functions (803D676C, 8022A87C, 803E20B0), the same for both builds. The per-element
work in build B is data (a class table), not hooks. The runtime side: one placement path in gx_render.c, one
in gx.vert, one scissor / viewport rule in each backend.

## 3. Mechanism for HUD placement

Proposal: **object level placement in the runtime, chosen from the task tree**, applied per draw in the shader.

1. `hle_render_nodes` (8022A87C, native copy of the 0x60 byte loop): keeps the current layer (when the parent
   is one of the tl_* pointers) and the current object and its class name for each `render` call; marks the
   start and end of each placement unit (the top object under a window manager or layer). Cheap; the function
   is already 0.8% of profile samples in gx_survey.txt, so a native copy is likely a small gain.
2. `hle_screen_space_matrix` (803E20B0): runs the game's function, then records that PNMTX0 now holds the
   screen space matrix (and its values), so gx_render.c knows which draws are 2D.
3. gx_render.c: for 2D draws in a HUD context, a placement code per draw, as today in `wide_anchor`
   (`GX_ANCHOR_MIDDLE / LEFT / RIGHT / STRETCH` in each vertex's nrm[3]); a new state flag bit in
   GX_XR_WIDE_FLAGS says "screen space HUD" so gx.vert applies the anchor to these perspective draws too (the
   formula `clip.x / k + offset * clip.w` already works with any w). Build B: at the end of a placement unit,
   one code for all of its draws from the union of their x ranges (left third, middle, right third), with the
   class table able to force a code; the existing code already re-marks earlier draws of the frame, since the
   frame is drawn only when complete.
4. Backends: for draws with the HUD flag, scissor x and the scissor box offset (BP 0x59) mapped the same way
   as the placement (x + offset in pixels, not times k). Needed for the radar and for scrolling lists.

Alternatives considered:

| Mechanism | For | Against |
|---|---|---|
| Per draw x range only (the Riders heuristic) | No game knowledge; already written | Cannot tell HUD from world in screen space (particles, bubbles); tears windows whose frame pieces and text fall either side of the dead zone; text runs need the letter logic |
| Per object, from the task tree (proposed) | A window moves as one; class names are readable and loggable; UI outside the UI layers is caught by name; world 2D left alone | Needs the class table to stay complete (a missed UI class shows stretched, visibly, and is a one-line fix) |
| Hook each game UI function and change its coordinates | Exact per element | 170 window source files, many not decompiled; many hooks; brittle |
| Rewrite the game's window positions in data | What a disc patch does | Touches game data at run time per screen; many tables; no help for full-width and world 2D |

## 4. Online and replay safety

- `settings_fix_wide_k` has no caller in PSO (Riders' netplay.c is gone). Proven (grep of runtime/src).
  `settings_game_k` is only read for GX_XR_ASPECT, which becomes 1. PSO has no replays. Proven / inferred.
- PSO online is server relayed: each client simulates and sends commands (docs/online-research.md). No
  network source in the decomp reads the camera, the projection or a visibility result. Proven for the
  decompiled network files; inferred for the rest.
- What the view does change, locally: the visibility flags gate drawing and lock-on candidacy. Examples read:
  TObjContainerCommon::render sets `in_view`, and TObjContainerItem::update_state submits a cursor candidate
  only when in view; TOMonumentAncient02, TObjEnePanarms and TObjEnePanBARA likewise; traps and mines gate
  only drawing. Proven. With a wider view, things at the sides become lock-on candidates.
- Why that is safe: the view is already the player's own (camera turns, the port's field of view setting), so
  no state can depend on it that a camera turn would not change too; what other players receive is the
  player's actions, as before. Inferred, strong.
- Nothing the game sends carries the aspect, the HUD layout or screen positions. Inferred from the above.

## 5. Performance

- Pixels: the EFB goes from 640 to 854 wide at each resolution step, 33% more fill and EFB copy work. Proven
  (render_deko.c EFB_W, XSC).
- Geometry: the horizontal view goes from 63.75 to 79.3 degrees at 100% field of view, so up to about a third
  more objects in view in open areas (Forest, Episode II Temple, Spaceship, Jungle, Seabed); corridors and caves
  less. Map sections cost nothing extra (already drawn over 120 degrees). Inferred.
- Test 6 found 50-64% of 3D draws wholly outside the view in busy spots: the game already submits much of what
  becomes visible, so the CPU (game work) side should grow little (estimate 3-10%); the GPU side most (estimate
  20-35%). Inferred.
- Episode II in test 11 at stock: GPU 21-24 ms a frame (Forest 7-16), game work up to 32.7 ms, and 1224 MHz
  recommended. With 16:9 the GPU could reach 28-32 ms there, close to the 33.3 ms of 30 fps. Inferred; must be
  measured. Levers if needed: resolution 1x in 16:9, the handheld graphics profile, SR_CULL with the corrected
  margin.

## 6. Two-build plan

### Build A: 3D widened, HUD centred 4:3, culling widened, menu option

- `config/GPOE8P/hle.txt`: 803D676C perspective, 8022A87C render_nodes, 803E20B0 screen_space_matrix.
- New `runtime/src/pso_wide.c` (about 200 lines): the three hooks, the layer / class state, the UI class table,
  SR_HUDLOG (class name and placement per object, once per class).
- `runtime/src/gx_render.c` (about 80 lines): screen space draws marked; HUD draws MIDDLE or STRETCH (full
  width); the new flag bit; class name added to the SR_ANCHORLOG lines.
- `runtime/shaders/gx.vert` (about 5 lines): anchor applied to flagged perspective draws.
- `runtime/src/platform/switch/render_deko.c`, `runtime/src/platform/host/render_gl.c` (about 20 lines each):
  scissor and BP 0x59 mapping for HUD draws.
- `runtime/src/cull.c` (1 line): no wide_k factor in the side margin.
- `runtime/src/settings.c` and `menu.cpp` (about 30 lines): Display, Screen: "4:3 (original)" / "16:9";
  remove the forced 4:3 on load; GX_XR_ASPECT left at 1. PC: 16:9 only at first (Fit mode later).
- Total about 350 lines.

What the owner checks (Switch, then PC):

1. Pioneer 2, Forest 1 and an Episode II open area in 16:9: more to the sides, nothing stretched (round
   things round).
2. Turning slowly in a room with enemies, boxes and items: nothing appears late at the screen sides.
3. HUD, radar, menus, shops, bank, guild card, quest counter, title, character select and creation, options,
   loading screen: all in the middle 4:3, the radar round and in its place, nothing cut off.
4. Fades and menu dimming cover the whole screen.
5. Speech bubbles and lens flare: expected about a third wider (fixed in B).
6. Split screen with 2 players for a minute.
7. Performance readout, same spots in 4:3 and 16:9, Forest and Episode II, stock and 1224 MHz.
8. One online lobby visit: nothing different for anyone.

Expected: the 3D right on the first try is likely (one hook, formulas checked). The HUD rule is the part most
likely to need a second pass: some UI class missing from the table would show stretched; the log names it.

### Build B: HUD elements pinned

- `runtime/src/pso_wide.c` (about 120 lines more): placement units, union of x ranges, codes per unit, class
  overrides from a new `config/GPOE8P/hud_anchor.txt` (class name, L / R / C / S / follow).
- `runtime/src/gx_render.c` (about 60 lines): codes per unit; the follow code (centre kept, width / k), which
  needs the draw's centre per vertex (a spare vertex float).
- `runtime/shaders/gx.vert` (about 5 lines): the follow code.
- `menu.cpp`, `settings.c` (about 15 lines): "HUD: middle (4:3) / screen edges".
- Total about 200 lines.

What the owner checks:

1. Player and party status, action palette, radar, chat log, info icon at the screen edges, each in one piece
   (frame, bars and text together).
2. Menus and shops stay centred; cursors and highlights line up with their items.
3. Word select, keyboard, message windows centred and whole.
4. Speech bubbles and lens flare the right shape.
5. The same performance spots as build A (no change expected).

## Open questions for later

- Which side each HUD element sits on is measured in build A, not read; the table above gives the proposal.
- Classes not decompiled that might draw world 2D or test visibility their own way: found by the build A logs
  and the turning test.
- Fit to window (PC, wider than 16:9) needs a bigger EFB in render_gl.c and the same hooks; not planned now.

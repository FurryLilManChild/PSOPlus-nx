/* 16:9 widescreen (docs/widescreen-research.md, builds A and B): shared by the game thread side (pso_wide.c:
 * the three game hooks) and the GP thread side (gx_render.c: where each draw goes).
 *
 * The game thread tells the GP thread, in order with the GX command stream, which object is drawing and how
 * its 2D is placed, with 12-byte runtime commands that are not GX opcodes (gx_fifo.c gx_host_mark):
 *   GX_HOST_MARK, kind, placement byte (WIDE_MARK_PLACE...), layer, class name pointer, window (the object
 *   whose draws move as one piece: build B), both big-endian guest addresses. */
#pragma once
#include <math.h>
#include <stdint.h>

#define GX_HOST_MARK 0xC2  /* not a GX opcode: the runtime's own (like GX_HOST_PICTURE 0xC1) */

enum {
    WIDE_MARK_CTX = 1,   /* the object about to draw: placement, layer, class name */
    WIDE_MARK_SS = 2,    /* load_screen_space_matrix ran: position matrix 0 holds a screen space matrix */
    WIDE_MARK_PROJ = 3,  /* set_perspective_projection_from_fov set a widened projection */
};
#define WIDE_MARK_KIND 0x0Fu  /* the kind byte: the kind in bits 0-3, flags of a WIDE_MARK_CTX above */
#define WIDE_CTX_MENU 0x10u      /* the menu screen is up (pso_hud.h hud_menu_screen): windows take their menu: line */
#define WIDE_CTX_BACKDROP 0x20u  /* this object's own draws are the menu screen's panels (TWindowCtrlScreen):
                                  * pso_hud.h hud_backdrop_place per vertex */

/* where an object's screen space (2D) draws go */
enum {
    WIDE_WORLD,        /* left as the widened projection draws it: stretched to the width (particles, fades) */
    WIDE_HUD,          /* HUD and menus: 4:3 in the middle; draws spanning the width stretched */
    WIDE_FOLLOW_UNIT,  /* fixed-size 2D placed from a 3D point, one piece (speech bubbles): its middle kept, k narrower */
    WIDE_FOLLOW_DRAW,  /* the same per draw (lens flare: separate sprites, each placed on its own) */
    WIDE_OUTSIDE,      /* drawn outside the task tree (loading screens, the info icon): screen space as HUD */
};
#define WIDE_NO_LAYER 0xFFu
#define WIDE_MARK_BYTES 12
/* the placement byte: WIDE_* in bits 0-2, the class's own hud_anchor.txt line (pso_hud.h HUD_*) in bits 3-5,
 * split screen on (the game's view count above 1) in bit 6 */
static inline uint8_t wide_place_byte(int place, int force, int split) {
    return (uint8_t)((place & 7) | (force & 7) << 3 | (split ? 0x40 : 0));
}

/* The game's task layers, in address order from tl_su (0x805D6340) to tl_fadeafter (0x805D638C):
 * names from the decomp's symbol map (zeroz41/pso-decomp, MIT). */
#define WIDE_LAYERS 20
#define WIDE_LAYER_BASE 0x805D6340u
static const char* const k_wide_layer_names[WIDE_LAYERS] = {
    "tl_su", "tl_00", "tl_camera", "tl_01", "tl_02", "tl_03", "tl_item_equip", "tl_loc_start", "tl_04", "tl_05",
    "tl_06", "tl_07", "tl_loc_end", "tl_window", "tl_particle", "tl_particlemodel", "tl_radermap", "tl_clipout",
    "tl_fade", "tl_fadeafter"};
/* the HUD layers: tl_window, tl_radermap, tl_clipout, tl_fadeafter (tl_fade stays stretched: full-screen fades) */
static inline int wide_layer_is_hud(int i) { return i == 13 || i == 16 || i == 17 || i == 19; }

/* The projection hook's field of view: set_perspective_projection_from_fov (0x803D676C) takes a horizontal
 * angle in 65536ths of a turn and works out the vertical one as
 *   half = (int)((float)fov * s) / 2   (s = 0.85, fov_angle_scale 0x805E0E18; the division rounds toward 0)
 *   vertical = 2 * atan(tan(half) / aspect).
 * With the aspect k times wider, the same vertical angle needs tan(half') = k * tan(half). Returns the fov
 * whose half angle is the nearest whole unit to that (one unit is 0.0055 degrees). */
static inline int32_t wide_fov_for(int32_t fov, float k, float s) {
    const double unit = 6.283185307179586 / 65536.0;
    int32_t half = (int32_t)((float)fov * s) / 2;
    double h = (double)half * unit;
    if (!(h > 0.0) || h >= 1.5) return fov;  /* not a usable angle: unchanged */
    int32_t target = (int32_t)lround(atan((double)k * tan(h)) / unit);
    int32_t f0 = (int32_t)floor(2.0 * target / s) - 3;
    for (int32_t f = f0; f < f0 + 8; f++)
        if ((int32_t)((float)f * s) / 2 == target) return f;
    return (int32_t)lround(2.0 * target / s);
}

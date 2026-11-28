/* Right-stick camera (settings "Right stick: Camera", cam_stick.c): the stick to yaw and eye height maths, kept
 * apart from the hooks so the host test (tools/tests/cam_stick_test.c) runs it without the game.
 *
 * Units are the game's: a yaw is 0x10000 per full turn, heights are world units (the follow camera's eye is
 * 25.43 above the player and 45.42 behind it; its target 19.8 above). Stick values are the game's controller
 * context (ControllerInputContext cstick_x / cstick_y): -100..100 from the port's pads, |v| <= 31 already zeroed
 * by the game, y positive = stick pulled down (the game stores -substickY). All the tuning is here. */
#pragma once

#include <stdint.h>

#define CAM_STICK_DEAD       31        /* the game's own dead zone: its pad update (which calls map_c_stick_to_buttons at 0x801A62E0) zeroes |v| <= 31 */
#define CAM_STICK_FULL       100       /* full deflection from the port's pads (input_hid.c axis8, input_sdl.c) */
#define CAM_YAW_PER_SEC      32768.0f  /* yaw units per second at full deflection: 0x8000 = 180 degrees */
#define CAM_YAW_SIGN         1         /* stick right turns the view right (the game's manual camera turns the same way) */
#define CAM_HEIGHT_PER_SEC   45.0f     /* eye height change per second at full deflection (world units) */
#define CAM_HEIGHT_SIGN      1         /* stick up lowers the eye (look up), stick down raises it (look down) */
#define CAM_HEIGHT_MIN       -16.0f    /* lowest: eye 25.43 - 16 = 9.4 above the player, under its target (19.8) */
#define CAM_HEIGHT_MAX       40.0f     /* highest: eye 65.4 above the player, looking down at about 45 degrees */
#define CAM_FIELD_HZ         59.94f    /* VI fields per second: the step's time comes from the fields between updates */
#define CAM_FIELDS_MAX       4         /* longer gaps (loading, a pause) count as 4 fields, so the camera never jumps */

/* one stick axis to -1..1: 0 inside the dead zone, then linear to full deflection */
static inline float cam_axis(int v) {
    int a = v < 0 ? -v : v;
    if (a <= CAM_STICK_DEAD) return 0.0f;
    float s = (float)(a - CAM_STICK_DEAD) / (float)(CAM_STICK_FULL - CAM_STICK_DEAD);
    if (s > 1.0f) s = 1.0f;
    return v < 0 ? -s : s;
}

/* seconds covered by one camera update, from the VI fields since the last one (1..CAM_FIELDS_MAX) */
static inline float cam_step_seconds(uint32_t fields) {
    if (fields < 1) fields = 1;
    if (fields > CAM_FIELDS_MAX) fields = CAM_FIELDS_MAX;
    return (float)fields / CAM_FIELD_HZ;
}

/* the new yaw (0..0xFFFF) after one update with stick x for dt seconds */
static inline uint32_t cam_yaw_step(uint32_t yaw, int x, float dt) {
    float d = (float)CAM_YAW_SIGN * cam_axis(x) * CAM_YAW_PER_SEC * dt;
    int32_t step = (int32_t)(d < 0.0f ? d - 0.5f : d + 0.5f);
    return (uint32_t)((int32_t)yaw + step) & 0xFFFFu;
}

/* the eye height offset (added to the game's own eye height) after one update with stick y for dt seconds */
static inline float cam_height_step(float h, int y, float dt) {
    h += (float)CAM_HEIGHT_SIGN * cam_axis(y) * CAM_HEIGHT_PER_SEC * dt;
    if (h < CAM_HEIGHT_MIN) h = CAM_HEIGHT_MIN;
    if (h > CAM_HEIGHT_MAX) h = CAM_HEIGHT_MAX;
    return h;
}

/* the C-stick direction thresholds of map_c_stick_to_buttons (0x801A6A5C): past them the game sets a button bit */
static inline int cam_stick_past_buttons(int x, int y) { return x > 0x30 || x < -0x30 || y > 0x30 || y < -0x30; }

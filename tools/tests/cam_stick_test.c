/* Host test for the right-stick camera maths (runtime/src/cam_stick.h): the dead zone, analog speed, the yaw
 * rate at full deflection (180 degrees a second at 30 and 60 game frames a second), wrap-around, the height
 * clamps and the C-stick button thresholds. No game code or data. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cam_stick.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int yaw_diff(uint32_t a, uint32_t b) { return (int16_t)(uint16_t)(a - b); }

int main(void) {
    /* dead zone and scale */
    for (int v = -CAM_STICK_DEAD; v <= CAM_STICK_DEAD; v++) CHECK(cam_axis(v) == 0.0f, "axis %d not 0", v);
    CHECK(cam_axis(CAM_STICK_FULL) == 1.0f && cam_axis(-CAM_STICK_FULL) == -1.0f, "full deflection not +-1");
    CHECK(cam_axis(127) == 1.0f && cam_axis(-128) == -1.0f, "past full not clamped");
    float prev = 0.0f;
    for (int v = CAM_STICK_DEAD + 1; v <= CAM_STICK_FULL; v++) {
        float a = cam_axis(v);
        CHECK(a > prev && a <= 1.0f, "axis not increasing at %d", v);
        CHECK(cam_axis(-v) == -a, "axis not symmetric at %d", v);
        prev = a;
    }
    /* step time */
    CHECK(fabsf(cam_step_seconds(0) - 1.0f / CAM_FIELD_HZ) < 1e-6f, "0 fields not 1");
    CHECK(fabsf(cam_step_seconds(100) - CAM_FIELDS_MAX / CAM_FIELD_HZ) < 1e-6f, "long gap not clamped");
    /* yaw: one second of full stick turns 180 degrees (0x8000) at 30 fps (2 fields) and 60 fps (1 field) */
    for (int fields = 1; fields <= 2; fields++) {
        uint32_t yaw = 0x1234;
        int steps = (int)lroundf(CAM_FIELD_HZ / fields);
        long total = 0;
        for (int i = 0; i < steps; i++) {
            uint32_t n = cam_yaw_step(yaw, CAM_STICK_FULL, cam_step_seconds((uint32_t)fields));
            total += yaw_diff(n, yaw);
            CHECK(n <= 0xFFFFu, "yaw out of range");
            yaw = n;
        }
        CHECK(labs(total - 0x8000) < 0x8000 / 50, "%d fields: one second turned %ld, want about %d", fields, total, 0x8000);
        uint32_t back = cam_yaw_step(0x1234, -CAM_STICK_FULL, cam_step_seconds((uint32_t)fields));
        uint32_t fwd = cam_yaw_step(0x1234, CAM_STICK_FULL, cam_step_seconds((uint32_t)fields));
        CHECK(yaw_diff(fwd, 0x1234) == -yaw_diff(back, 0x1234), "left and right not symmetric");
        CHECK(CAM_YAW_SIGN * yaw_diff(fwd, 0x1234) > 0, "stick right does not add to the yaw");
    }
    CHECK(cam_yaw_step(0xFFF0, CAM_STICK_FULL, cam_step_seconds(2)) < 0x1000, "yaw does not wrap past 0xFFFF");
    CHECK(cam_yaw_step(0x0010, -CAM_STICK_FULL, cam_step_seconds(2)) > 0xF000, "yaw does not wrap below 0");
    CHECK(cam_yaw_step(0x18000, 0, 0.1f) == 0x8000, "a game yaw past 0xFFFF is not folded");
    CHECK(cam_yaw_step(0x4000, CAM_STICK_DEAD, 0.1f) == 0x4000, "dead zone turns");
    /* half deflection is slower than full */
    int half = yaw_diff(cam_yaw_step(0, (CAM_STICK_DEAD + CAM_STICK_FULL) / 2, 0.0334f), 0);
    int full = yaw_diff(cam_yaw_step(0, CAM_STICK_FULL, 0.0334f), 0);
    CHECK(half > 0 && half < full, "analog speed: half %d full %d", half, full);
    /* height: clamps, speed, direction */
    float h = 0.0f;
    for (int i = 0; i < 600; i++) h = cam_height_step(h, CAM_STICK_FULL, cam_step_seconds(2));
    CHECK(h == CAM_HEIGHT_MAX, "height max %f", h);
    for (int i = 0; i < 600; i++) h = cam_height_step(h, -CAM_STICK_FULL, cam_step_seconds(2));
    CHECK(h == CAM_HEIGHT_MIN, "height min %f", h);
    h = cam_height_step(0.0f, CAM_STICK_FULL, 1.0f);
    CHECK(fabsf(h - CAM_HEIGHT_SIGN * CAM_HEIGHT_PER_SEC) < 1e-3f || h == CAM_HEIGHT_MAX, "one second of height %f", h);
    CHECK(cam_height_step(5.0f, 10, 1.0f) == 5.0f, "height moves in the dead zone");
    /* the game's C-stick thresholds */
    CHECK(!cam_stick_past_buttons(0x30, -0x30) && cam_stick_past_buttons(0x31, 0) && cam_stick_past_buttons(0, -0x31),
          "button thresholds");
    if (g_fail) printf("cam_stick_test: %d FAILED\n", g_fail);
    else printf("cam_stick_test: all passed\n");
    return g_fail != 0;
}

/* User options: see settings.h. The file is plain `key=value` lines, rewritten whole after each change in
 * the menu (a temporary file first, then put in place: a crash or power loss mid-write leaves the old file
 * or the new one, never half of one). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "runtime.h"
#include "settings.h"

/* defaults: stock CPU clock (the project's goal is full speed at stock clocks), no FPS counter */
Settings g_settings = {
    .screen = SCREEN_4_3, .perf_overlay = 0, .resolution = RES_1X, .fullscreen = 0, .framerate = FR_AUTO,
    .handheld_gpu = 1, .hud_stretch = 0, .fov = 100, .pc_scale = 2, .msaa = 1, .bloom = 0, .grade_bright = 0,
    .grade_contrast = 100, .grade_sat = 100, .grade_warm = 0, .char_light = 0, .pc_fps = 0, .pad_background = 1,
    .sharpness = 0, .update_check = 0, .cpu_clock = 0,
    .pad_map = {SW_A, SW_B, SW_X, SW_Y, SW_R, SW_ZL, SW_ZR, SW_PLUS, SW_UP, SW_DOWN, SW_LEFT, SW_RIGHT},
    .net_dns = "",  /* Schtserv's DNS servers (net_native.c) */
};
const int settings_pad_map_default[GC_INPUTS] = {SW_A, SW_B, SW_X, SW_Y, SW_R, SW_ZL, SW_ZR, SW_PLUS,
                                                 SW_UP, SW_DOWN, SW_LEFT, SW_RIGHT};
/* the button map in settings.ini: map_<input>=<button>, both by these names (lower case) */
static const char* const k_gc_key[GC_INPUTS] = {"a", "b", "x", "y", "z", "l", "r", "start", "up", "down", "left", "right"};
static const char* const k_sw_key[SW_BUTTONS] = {"none", "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "lstick",
                                                 "rstick", "up", "down", "left", "right"};
const char* settings_gc_input_label(int in) {
    static const char* const names[GC_INPUTS] = {"A", "B", "X", "Y", "Z", "L", "R", "START", "D-pad up",
                                                 "D-pad down", "D-pad left", "D-pad right"};
    return in >= 0 && in < GC_INPUTS ? names[in] : "?";
}
const char* settings_sw_button_label(int sw) {
    static const char* const names[SW_BUTTONS] = {"None", "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+ (Plus)",
                                                  "Left stick click", "Right stick click", "D-pad up", "D-pad down",
                                                  "D-pad left", "D-pad right"};
    return sw >= 0 && sw < SW_BUTTONS ? names[sw] : "?";
}
/* one map_ line (key without "map_", value without the line end): unknown inputs or buttons are ignored */
static void load_map_entry(const char* key, const char* value) {
    for (int i = 0; i < GC_INPUTS; i++) {
        if (strcmp(key, k_gc_key[i])) continue;
        for (int b = 0; b < SW_BUTTONS; b++)
            if (!strcmp(value, k_sw_key[b])) {
                g_settings.pad_map[i] = b;
                return;
            }
        rt_log("[settings] map_%s: unknown button \"%s\", ignored", key, value);
        return;
    }
    rt_log("[settings] map_%s: unknown GameCube input, ignored", key);
}
volatile double g_aspect_scale = 1.0;
volatile double g_fov_scale = 1.0;
static volatile float g_window_aspect = 16.0f / 9.0f;
static volatile float g_wide_k = 1.0f;
static float g_fixed_k;  /* settings_fix_wide_k */

const char* settings_screen_name(int mode) {
    static const char* const names[SCREEN_MODES] = {"4:3 (original)", "16:9 widescreen", "16:9 stretched (removed)", "Fit to screen"};
    return mode >= 0 && mode < SCREEN_MODES ? names[mode] : "?";
}

const char* settings_resolution_name(int res) {
    static const char* const names[RES_MODES] = {"1x (640x528, fastest)", "1.5x (960x792)", "2x (1280x1056)",
                                                         "1.25x (800x660)"};
    return res >= 0 && res < RES_MODES ? names[res] : "?";
}

const int settings_pc_fps_presets[] = {30, 45, 60, 120, 144, 180, 240, PC_FPS_UNLIMITED, 0};
int settings_pc_fps_valid(int v) {
    if (v == 0) return 1;
    for (int i = 0; settings_pc_fps_presets[i]; i++)
        if (settings_pc_fps_presets[i] == v) return v != 30 && v != 60;  /* those two are pc_fps 0 */
    return 0;
}

const char* settings_framerate_name(int mode) {
    static const char* const names[FR_MODES] = {"60 fps (30 when busy)", "Auto (keep speed)", "30 fps (full speed)"};
    return mode >= 0 && mode < FR_MODES ? names[mode] : "?";
}

float settings_internal_scale(void) { return g_settings.resolution == RES_2X ? 2.0f : g_settings.resolution == RES_1_5X ? 1.5f : g_settings.resolution == RES_1_25X ? 1.25f : 1.0f; }

/* an aspect from 4:3 to 32:9, rounded so the EFB is a whole number of
 * (even) pixels wide: 640 x k */
static float round_aspect(float a) {
    if (!(a >= 4.0f / 3.0f)) a = 4.0f / 3.0f;
    if (a > 32.0f / 9.0f) a = 32.0f / 9.0f;
    float w = floorf(640.0f * a / (4.0f / 3.0f) / 2.0f + 0.5f) * 2.0f;
    return w / 640.0f * (4.0f / 3.0f);
}

/* the shape this machine's own settings and screen ask for */
static float own_display_aspect(void) {
    switch (g_settings.screen) {
    case SCREEN_4_3: return 4.0f / 3.0f;
    /* 16:9 rounded like the fit mode's, so both give the game the same camera aspect (online play) */
    case SCREEN_FIT: return round_aspect(g_window_aspect);
    case SCREEN_WIDE: return round_aspect(16.0f / 9.0f);
    default: return 16.0f / 9.0f;
    }
}

/* of the presented picture: this machine's own shape (narrowed to the room's camera if that is narrower) */
float settings_display_aspect(void) { return g_fixed_k >= 1.0f ? 4.0f / 3.0f * g_wide_k : own_display_aspect(); }
float settings_wide_k(void) { return g_wide_k; }
/* the camera aspect the game itself uses (game state: the room's in an online session, a recording's in a replay) */
float settings_game_k(void) { return (float)g_aspect_scale; }

int settings_cpu_clock_valid(int mhz) { return mhz == 0 || mhz == 1224 || mhz == 1428 || mhz == 1785; }

/* the log says what was on, at start-up and after each change in the menu (hardware test reports) */
static void settings_log(const char* when) {
    char cpu[32];
    if (g_settings.cpu_clock) snprintf(cpu, sizeof cpu, "%d MHz", g_settings.cpu_clock);
    else snprintf(cpu, sizeof cpu, "stock");
    rt_log("[settings] %sCPU clock %s, handheld graphics %s, FPS counter %s, resolution %s, field of view %d%%, "
           "sharpness %d%%, colour %+d/%d/%d/%+d, screen %s",
           when, cpu, g_settings.handheld_gpu ? "384 MHz" : "system default", g_settings.perf_overlay ? "on" : "off",
           settings_resolution_name(g_settings.resolution), g_settings.fov, g_settings.sharpness, g_settings.grade_bright,
           g_settings.grade_contrast, g_settings.grade_sat, g_settings.grade_warm, settings_screen_name(g_settings.screen));
#ifdef __SWITCH__
    char map[256];
    size_t k = 0;
    map[0] = 0;
    for (int i = 0; i < GC_INPUTS; i++)
        if (g_settings.pad_map[i] != settings_pad_map_default[i] && k < sizeof map) {
            int w = snprintf(map + k, sizeof map - k, "%s%s = %s", k ? ", " : "", k_gc_key[i], k_sw_key[g_settings.pad_map[i]]);
            if (w > 0) k += (size_t)w;
        }
    rt_log("[settings] %sbuttons: %s", when, k ? map : "default layout");
#endif
}

static void apply_view(void) {
    /* widescreen: the cameras' aspect ratio from 4:3 to the screen's, and an EFB as much wider */
    /* online / replays (settings_fix_wide_k): the game keeps the fixed camera aspect (game state), while this
     * machine draws its own screen shape; gx.vert narrows the perspective views by game k / own k
     * (GX_XR_ASPECT). A picture wider than the game's camera would show past what the game culls, so the
     * own shape is used only up to the fixed one (the room uses its widest player's shape). */
    int wide = g_settings.screen == SCREEN_WIDE || g_settings.screen == SCREEN_FIT;
    float own = wide ? own_display_aspect() / (4.0f / 3.0f) : 1.0f;
    g_wide_k = g_fixed_k >= 1.0f ? (own < g_fixed_k ? own : g_fixed_k) : own;
    g_aspect_scale = g_fixed_k >= 1.0f ? g_fixed_k : own;
    g_fov_scale = g_settings.fov / 100.0;
}

static void apply(void) {
    apply_view();
    platform_set_handheld_gpu(g_settings.handheld_gpu);
    platform_set_cpu_clock(g_settings.cpu_clock);  /* after the apm configuration, which can set the CPU too */
}

/* a Steam Deck or another gamescope session (Linux): Steam sets SteamDeck=1, gamescope its display */
int settings_handheld_pc(void) {
    const char* deck = getenv("SteamDeck");
    return (deck && atoi(deck) == 1) || getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL;
}

static const char* tmp_path(void) {
    static char p[512];
    snprintf(p, sizeof p, "%s.tmp", platform_settings_path());
    return p;
}

void settings_load(void) {
    FILE* f = fopen(platform_settings_path(), "r");
    if (!f && (f = fopen(tmp_path(), "r")))  /* a save stopped between removing the old file and the rename */
        rt_log("[settings] %s missing: read %s", platform_settings_path(), tmp_path());
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (!strcmp(line, "net_dns")) {  /* checked by net_native.c, which logs and falls back to the default */
                char* v = eq + 1;
                v += strspn(v, " \t");
                v[strcspn(v, "\r\n \t")] = 0;
                if (strlen(v) < sizeof g_settings.net_dns) snprintf(g_settings.net_dns, sizeof g_settings.net_dns, "%s", v);
                else rt_log("[settings] net_dns \"%s\" is too long: the default DNS servers are used", v);
                continue;
            }
            if (!strncmp(line, "map_", 4)) {
                eq[1 + strcspn(eq + 1, "\r\n \t")] = 0;
                load_map_entry(line + 4, eq + 1);
                continue;
            }
            int v = atoi(eq + 1);
            if (!strcmp(line, "screen") && v >= 0 && v < SCREEN_MODES) g_settings.screen = v;
            else if (!strcmp(line, "perf_overlay")) g_settings.perf_overlay = v != 0;
            else if (!strcmp(line, "cpu_clock") && settings_cpu_clock_valid(v)) g_settings.cpu_clock = v;
            else if (!strcmp(line, "resolution") && v >= 0 && v < RES_MODES) g_settings.resolution = v;
            else if (!strcmp(line, "fullscreen")) g_settings.fullscreen = v != 0;
            else if (!strcmp(line, "framerate") && v >= 0 && v < FR_MODES) g_settings.framerate = v;
            else if (!strcmp(line, "handheld_gpu")) g_settings.handheld_gpu = v != 0;
            else if (!strcmp(line, "hud_stretch")) g_settings.hud_stretch = v != 0;
            else if (!strcmp(line, "fov") && v >= FOV_MIN && v <= FOV_MAX) g_settings.fov = v;
            else if (!strcmp(line, "pc_scale") && v >= PC_SCALE_MIN) g_settings.pc_scale = v > PC_SCALE_MAX ? PC_SCALE_MAX : v;
            else if (!strcmp(line, "msaa") && (v == 1 || v == 2 || v == 4 || v == 8)) g_settings.msaa = v;
            else if (!strcmp(line, "bloom") && v >= 0 && v <= 100) g_settings.bloom = v;
            else if (!strcmp(line, "grade_bright") && v >= -50 && v <= 50) g_settings.grade_bright = v;
            else if (!strcmp(line, "grade_contrast") && v >= 50 && v <= 150) g_settings.grade_contrast = v;
            else if (!strcmp(line, "grade_sat") && v >= 0 && v <= 200) g_settings.grade_sat = v;
            else if (!strcmp(line, "grade_warm") && v >= -50 && v <= 50) g_settings.grade_warm = v;
            else if (!strcmp(line, "char_light")) g_settings.char_light = v != 0;
            else if (!strcmp(line, "pc_fps") && settings_pc_fps_valid(v)) g_settings.pc_fps = v;
            else if (!strcmp(line, "pad_background")) g_settings.pad_background = v != 0;
            else if (!strcmp(line, "sharpness") && v >= 0 && v <= 100) g_settings.sharpness = v;
            else if (!strcmp(line, "update_check")) g_settings.update_check = v != 0;
        }
        fclose(f);
        /* PSO has no camera patch for wider screens (sonic-riders-recomp's widescreen widened the game's own
         * camera): a wide mode only stretched the 3D picture, so the menu no longer offers it and 4:3 is used.
         * PC in-between pictures assume a 60-picture game (PSO draws 30): off. */
        if (g_settings.screen != SCREEN_4_3 || g_settings.pc_fps != 0)
            rt_log("[settings] saved screen mode %d / PC picture rate %d not used by this port: 4:3, the game's own rate",
                   g_settings.screen, g_settings.pc_fps);
        g_settings.screen = SCREEN_4_3;
        g_settings.pc_fps = 0;
    } else if (settings_handheld_pc()) {
        /* first run on a Steam Deck (or under gamescope): fill the screen. gamescope scales the window to
         * the display, so a 4:3 picture got bars (gbatemp report, 2026-11-06) */
        g_settings.fullscreen = 1;
    }
    const char* fr = getenv("SR_FRAMERATE");  /* testing: 0 = 60 fps, 1 = Auto, 2 = 30 fps (not saved unless changed in the menu) */
    if (fr && atoi(fr) >= 0 && atoi(fr) < FR_MODES) g_settings.framerate = atoi(fr);
    const char* pf = getenv("SR_FPS");  /* testing: the PC picture rate (pc_fps; 0 = the game's, -1 = unlimited) */
    if (pf && settings_pc_fps_valid(atoi(pf))) g_settings.pc_fps = atoi(pf);
    const char* sh = getenv("SR_SHARPNESS");  /* testing: FSR sharpening 0-100 (not saved unless changed in the menu) */
    if (sh && atoi(sh) >= 0 && atoi(sh) <= 100) g_settings.sharpness = atoi(sh);
    /* testing, not saved unless changed in the menu: SR_BLOOM=0-100, SR_GRADE=brightness,contrast,saturation,warmth,
     * SR_RES=0-3 (1x / 1.5x / 2x / 1.25x; Switch and phones) */
    const char* bl = getenv("SR_BLOOM");
    if (bl && atoi(bl) >= 0 && atoi(bl) <= 100) g_settings.bloom = atoi(bl);
    const char* gr = getenv("SR_GRADE");
    if (gr && *gr)
        sscanf(gr, "%d,%d,%d,%d", &g_settings.grade_bright, &g_settings.grade_contrast, &g_settings.grade_sat,
               &g_settings.grade_warm);
    const char* rs = getenv("SR_RES");
    if (rs && atoi(rs) >= 0 && atoi(rs) < RES_MODES) g_settings.resolution = atoi(rs);
    /* testing, not saved unless changed in the menu: SR_SCREEN=0 or 3 (SCREEN_*), SR_FOV=80-130 */
    const char* sc = getenv("SR_SCREEN");
    if (sc && atoi(sc) >= 0 && atoi(sc) < SCREEN_MODES) g_settings.screen = atoi(sc);
    /* two screen options only (user, 2026-11-14): 4:3 and Fit to screen (HUD at the edges). The old "16:9
     * widescreen" (1) and "16:9 stretched" (2) read as Fit; the HUD is never stretched */
    if (g_settings.screen == SCREEN_WIDE || g_settings.screen == SCREEN_REMOVED_2) g_settings.screen = SCREEN_FIT;
    g_settings.hud_stretch = 0;
    const char* fv = getenv("SR_FOV");
    if (fv && atoi(fv) >= FOV_MIN && atoi(fv) <= FOV_MAX) g_settings.fov = atoi(fv);
    apply();
    settings_log("");
}

void settings_fix_wide_k(float k) {
    g_fixed_k = k >= 1.0f && k <= 8.0f ? k : 0.0f;
    apply_view();
    rt_log("[settings] camera aspect fixed for this run: %.4f (game k %.4f); this screen draws %.4f (EFB width x%.4f)",
           4.0f / 3.0f * (float)g_aspect_scale, (float)g_aspect_scale, 4.0f / 3.0f * g_wide_k, g_wide_k);
}

int settings_wide_k_fixed(void) { return g_fixed_k >= 1.0f; }

void settings_set_window_aspect(float aspect) {
    if (!(aspect > 0.1f) || fabsf(aspect - g_window_aspect) < 0.001f) return;
    g_window_aspect = aspect;
    if (g_settings.screen == SCREEN_FIT) {
        apply_view();
        rt_log("[settings] window aspect %.3f: picture %.3f, EFB width x%.3f", aspect, settings_display_aspect(), g_wide_k);
    }
}

void settings_apply(void) {
    apply();
    settings_log("changed: ");
    game_mods_apply();
    const char* path = platform_settings_path();
    const char* tmp = tmp_path();
    FILE* f = fopen(tmp, "w");
    if (!f) {
        rt_log("[settings] cannot write %s: the change holds for this session only", tmp);
        return;
    }
    fprintf(f, "perf_overlay=%d\ncpu_clock=%d\nscreen=%d\nresolution=%d\nfullscreen=%d\nframerate=%d\nhandheld_gpu=%d\n"
               "hud_stretch=%d\nfov=%d\npc_scale=%d\nmsaa=%d\nbloom=%d\ngrade_bright=%d\ngrade_contrast=%d\ngrade_sat=%d\n"
               "grade_warm=%d\nchar_light=%d\npc_fps=%d\npad_background=%d\nsharpness=%d\nupdate_check=%d\n",
            g_settings.perf_overlay, g_settings.cpu_clock, g_settings.screen, g_settings.resolution, g_settings.fullscreen,
            g_settings.framerate, g_settings.handheld_gpu, g_settings.hud_stretch, g_settings.fov, g_settings.pc_scale,
            g_settings.msaa, g_settings.bloom, g_settings.grade_bright, g_settings.grade_contrast, g_settings.grade_sat,
            g_settings.grade_warm, g_settings.char_light, g_settings.pc_fps, g_settings.pad_background, g_settings.sharpness,
            g_settings.update_check);
    for (int i = 0; i < GC_INPUTS; i++) {
        int b = g_settings.pad_map[i];
        fprintf(f, "map_%s=%s\n", k_gc_key[i], b >= 0 && b < SW_BUTTONS ? k_sw_key[b] : k_sw_key[settings_pad_map_default[i]]);
    }
    fprintf(f, "net_dns=%s\n", g_settings.net_dns);
    int ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        rt_log("[settings] writing %s failed: the old settings file is kept", tmp);
        remove(tmp);
        return;
    }
    /* FAT (the Switch's SD card) and Windows refuse a rename onto an existing file: remove it first */
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            rt_log("[settings] cannot put %s in place of %s: the next start reads %s", tmp, path, tmp);
            return;
        }
    }
}

/* The port's menu and frame rate counter (overlay.h), drawn with Dear ImGui over the game's picture.
 *
 * Opened by holding Minus (Switch; on a single Joy-Con held sideways its + or -) or Back / F1 (PC) for half
 * a second; while it is open the game sees no buttons. Pages: Performance (Switch: the CPU clock, the
 * handheld graphics profile; the FPS counter), Display, Controls, System (build, the clocks now, online status, screenshot).
 * L / R (ZL / ZR) switch pages, the D-pad or left stick moves, A chooses, B or Minus closes. Every item works
 * with a controller alone, handheld or docked. The Switch has no Quit: HOME closes games there.
 *
 * Cost when closed: overlay_image() is called once a frame by the render thread and does a few flag
 * checks; nothing is laid out or drawn unless the frame rate counter is on, a banner is up or the menu is
 * open. The counter ("30 FPS", small, top left; on the Switch in the black bar beside the 4:3 picture) is
 * laid out again only when the whole number changes; gx_render.c gives a new figure every 30 game frames
 * (about once a second). When it is off, the only cost is one settings check per frame.
 *
 * ImGui runs on the render thread (which pumps input). When its picture changes, the draw lists are copied
 * and drawn in software (menu_raster.cpp) by a low-priority helper thread into the image that is not on
 * screen, so the render thread never waits for it; the image is then swapped in. Based on
 * sonic-riders-recomp's menu (MPL-2.0); its game options, tabs and colours are not used here. */
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  /* NavMoveRequestTryWrapping */
#include "menu_raster.h"
#include "net_native.h"
#include "overlay.h"
#include "runtime.h"
#include "settings.h"
#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" {
extern const char g_build_version[], g_port_version[];
/* picture options (Display page): each one shows only where the renderer does it */
#if !defined(__SWITCH__)
#define PIC_SCALE_SLIDER 1  /* render_gl.c: 1x-4x */
#define PIC_MSAA 1
#define PIC_FSR 1
#define PIC_GRADE 1
#define PIC_CHARLIGHT 1
#define PIC_WINDOW 1
#define HAVE_BUG_REPORT 1
int platform_bug_report(char* name, size_t n);  /* platform_host.c */
void platform_open_game_folder(void);
#else
#define PIC_RES_RADIO 1  /* render_deko.c: 1x / 1.25x / 1.5x / 2x (the EFB is allocated at 2x) */
#define PIC_FSR 1
#define PIC_GRADE 1
#endif
}

namespace {

constexpr uint64_t kHoldTicks = 40500000ull / 2;  /* Minus / Back / F1 held this long opens the menu (0.5 s) */
constexpr float kFontSize = 19.0f;

/* two images: g_front is shown, the other is drawn by the helper thread; each with its content
 * rectangle (x0, y0, x1, y1; empty when x0 >= x1), outside which it is transparent */
uint8_t g_img[2][OVERLAY_W * OVERLAY_H * 4];
int g_box[2][4];
int g_front;
unsigned g_version;
bool g_open, g_gate, g_quit_popup;
unsigned g_prev;
uint64_t g_menu_since;
/* the frame rate counter: the figure from gx_render.c (any thread), and the text shown (render thread) */
std::atomic<int> g_fps10{-1};
int g_ro_fps = -2;  /* the whole number shown (-1: none yet) */
bool g_ro_on;
char g_ro_text[24];
constexpr float kReadoutFontSize = 15.0f;  /* smaller than the menu's 19 */
int g_tab, g_select_tab = -1;
const int kTabs = 4;  /* settings_window() */
int g_shot_request, g_pads_request, g_quit_request;
bool g_just_opened;
bool g_ready;
uint64_t g_last_ns;
uint64_t g_last_hash;
bool g_had_image;
/* overlay_set_banner: a message across the top. The mutexes here are never destroyed: threads still use
 * them while exit() runs the static destructors. */
std::mutex& g_banner_lock = *new std::mutex;
char g_banner_next[256], g_banner[256];
std::atomic<unsigned> g_banner_seq{0};
unsigned g_shown_banner_seq;

/* the helper thread's job: a copy of one frame's draw lists */
struct Job {
    ImDrawData dd;
    int buf;
    int box[4];
    double ms;      /* wall time on the helper thread */
    double cpu_ms;  /* of it running on the CPU (< 0: unknown) */
    bool open;  /* the picture shows the menu */
};
bool g_front_open;  /* g_front's picture shows the menu */
std::mutex& g_mx = *new std::mutex;
/* never destroyed: the helper thread waits on it for the whole run, and glibc's destructor waits for
 * the waiters, so exit() hung forever on Linux */
std::condition_variable& g_cv = *new std::condition_variable;
Job* g_job;        /* waiting for the helper (render thread -> helper) */
Job* g_done;       /* drawn (helper -> render thread) */
bool g_busy;       /* a job is out (render thread only) */
bool g_retry;      /* a changed picture waits for the helper */
bool g_worker_started;
bool g_worker_stopping, g_worker_stopped;

void job_free(Job* j) {
    for (ImDrawList* l : j->dd.CmdLists) IM_DELETE(l);
    delete j;
}

/* the area the draw lists cover: their vertices, inside their clip rectangles */
void content_box(const ImDrawData* dd, int box[4]) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const ImDrawList* l : dd->CmdLists)
        for (const ImDrawCmd& c : l->CmdBuffer) {
            if (c.UserCallback || !c.ElemCount) continue;
            float vx0 = 1e9f, vy0 = 1e9f, vx1 = -1e9f, vy1 = -1e9f;
            const ImDrawIdx* ix = l->IdxBuffer.Data + c.IdxOffset;
            const ImDrawVert* v = l->VtxBuffer.Data + c.VtxOffset;
            for (unsigned i = 0; i < c.ElemCount; i++) {
                const ImVec2 p = v[ix[i]].pos;
                vx0 = fminf(vx0, p.x), vy0 = fminf(vy0, p.y), vx1 = fmaxf(vx1, p.x), vy1 = fmaxf(vy1, p.y);
            }
            vx0 = fmaxf(vx0, c.ClipRect.x), vy0 = fmaxf(vy0, c.ClipRect.y);
            vx1 = fminf(vx1, c.ClipRect.z), vy1 = fminf(vy1, c.ClipRect.w);
            if (vx0 >= vx1 || vy0 >= vy1) continue;
            x0 = fminf(x0, vx0), y0 = fminf(y0, vy0), x1 = fmaxf(x1, vx1), y1 = fmaxf(y1, vy1);
        }
    box[0] = (int)fmaxf(0.0f, floorf(x0) - 1);
    box[1] = (int)fmaxf(0.0f, floorf(y0) - 1);
    box[2] = (int)fminf((float)OVERLAY_W, ceilf(x1) + 1);
    box[3] = (int)fminf((float)OVERLAY_H, ceilf(y1) + 1);
    if (box[0] >= box[2] || box[1] >= box[3]) box[0] = box[1] = box[2] = box[3] = 0;
}

/* The helper thread gives way. It shares core 2 with the GP thread (gx_fifo.c) and the device timers, and
 * was meant to run below them, but on the Switch every thread here has the same priority (0x3B, the lowest
 * this program may use: platform_switch.c rt_thread_lower_priority), and a thread of equal priority that
 * wakes does not take the core from a running one. Tests 12 and 13: a menu picture took 25-38 ms, and with
 * the menu open the GP thread's frame often took 30-60 ms instead of about 11 (the [hitch] lines' "GX
 * draws"). So the drawing stops every 0.25 ms to let any thread of its priority that is ready on its core
 * run first (rt_thread_yield; none ready: it goes on at once). The GP thread then waits at most about
 * 0.25 ms for it, and the picture takes longer when the core is busy. */
uint64_t g_pace_ns;  /* the helper thread's last yield */
void raster_pace() {
    uint64_t t = platform_now_ns();
    if (t - g_pace_ns < 250000) return;
    rt_thread_yield();
    g_pace_ns = platform_now_ns();
}

void clear_box(uint8_t* img, const int b[4]) {
    for (int y = b[1]; y < b[3]; y++) {
        if (((y - b[1]) & 31) == 31) raster_pace();
        memset(img + ((size_t)y * OVERLAY_W + b[0]) * 4, 0, (size_t)(b[2] - b[0]) * 4);
    }
}

/* [pace] menu figures (overlay_take_stats): pictures drawn and their cost, ImGui's frames on the render
 * thread. Written by the render thread (collect, overlay_image), read and reset by the game thread. */
std::atomic<unsigned> g_st_pics{0}, g_st_ui_frames{0};
std::atomic<uint64_t> g_st_draw_us{0}, g_st_draw_max_us{0}, g_st_cpu_us{0}, g_st_ui_us{0}, g_st_ui_max_us{0};
std::atomic<int> g_st_cpu_unknown{0};
void st_max(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

/* off the game's core; gives way to the GP thread there (raster_pace) */
int raster_thread(void*) {
    rt_thread_pin(2);
    rt_thread_lower_priority();  /* nothing on the Switch (see raster_pace); lower elsewhere */
    for (;;) {
        Job* j;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_job != nullptr || g_worker_stopping; });
            if (g_worker_stopping) break;
            j = g_job;
            g_job = nullptr;
        }
        uint64_t t0 = platform_now_ns(), c0 = rt_thread_cpu_ns();
        g_pace_ns = t0;
        uint8_t* img = g_img[j->buf];
        clear_box(img, g_box[j->buf]);
        menu_raster(&j->dd, img, OVERLAY_W, OVERLAY_H, raster_pace);
        uint64_t c1 = rt_thread_cpu_ns();
        j->ms = (platform_now_ns() - t0) / 1e6;
        j->cpu_ms = c0 && c1 >= c0 ? (c1 - c0) / 1e6 : -1.0;
        std::lock_guard<std::mutex> lk(g_mx);
        g_done = j;
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_worker_stopped = true;
    }
    g_cv.notify_all();
    return 0;
}

/* ---- look: plain dark grey panels with a muted cyan accent (no game art) ---- */
const ImVec4 kAccent(0.45f, 0.86f, 0.92f, 1.0f);  /* headings, the controller cursor, check marks */
const ImVec4 kNote(0.70f, 0.75f, 0.79f, 1.0f);
const ImVec4 kWarn(1.00f, 0.66f, 0.30f, 1.0f);    /* amber */

void setup_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 6;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 4;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.AntiAliasedLinesUseTex = false;  /* the software renderer samples without filtering (menu_raster.cpp) */
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.54f, 0.58f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.10f, 1.00f);  /* opaque: plain stores in menu_raster.cpp */
    c[ImGuiCol_ChildBg] = ImVec4(0.095f, 0.105f, 0.125f, 1.00f);  /* the page area, a shade lighter */
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.32f, 0.52f, 0.58f, 0.60f);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.27f, 0.32f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.20f, 0.90f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.30f, 0.34f, 0.95f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.38f, 0.43f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.30f, 0.35f, 0.95f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.42f, 0.48f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.52f, 0.59f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.30f, 0.35f, 0.80f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.42f, 0.48f, 0.90f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.52f, 0.59f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.17f, 0.20f, 0.95f);
    c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.42f, 0.48f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.34f, 0.40f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.76f, 0.82f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.90f, 0.95f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.32f, 0.52f, 0.58f, 0.45f);
    c[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.97f, 1.00f, 1.0f);  /* the controller cursor: near white, easy to follow */
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

void setup_fonts() {
    ImGuiIO& io = ImGui::GetIO();
#ifdef __SWITCH__
    /* the console's own font, as SwitchWakerHD (its overlay_switch.cpp) */
    static bool pl;
    if (!pl && R_SUCCEEDED(plInitialize(PlServiceType_User))) pl = true;
    PlFontData font;
    if (pl && R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  /* the system's shared memory */
        if (io.Fonts->AddFontFromMemoryTTF(font.address, (int)font.size, kFontSize, &cfg)) {
            rt_log("[menu] font: the system font");
            return;
        }
    }
#else
    static const char* const candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* path : candidates) {
        if (FILE* f = fopen(path, "rb")) {
            fclose(f);
            if (io.Fonts->AddFontFromFileTTF(path, kFontSize)) {
                rt_log("[menu] font: %s", path);
                return;
            }
        }
    }
#endif
    ImFontConfig cfg;
    cfg.SizePixels = kFontSize;
    io.Fonts->AddFontDefault(&cfg);
    rt_log("[menu] font: Dear ImGui's default");
}

void init() {
    if (g_ready) return;
    g_ready = true;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(OVERLAY_W, OVERLAY_H);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasTextures;
    io.BackendRendererName = "psoplus-software";
    io.ConfigNavCursorVisibleAlways = true;
    setup_style();
    setup_fonts();
    g_last_ns = platform_now_ns();
}

/* ---- widgets ---- */
void heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}
void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kNote);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
void warn(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
/* the explanation of the item just drawn, while it is selected */
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(520, FLT_MAX));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(500);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
bool radio(const char* label, bool active, const char* tip = nullptr) {
    bool clicked = ImGui::RadioButton(label, active) && !active;
    if (tip) help(tip);
    return clicked;
}
bool check(const char* label, int* value, const char* tip = nullptr) {
    bool v = *value != 0;
    bool changed = ImGui::Checkbox(label, &v);
    if (tip) help(tip);
    if (changed) *value = v;
    return changed;
}

void close_menu() {
    g_open = false;
    g_gate = true;
    g_quit_popup = false;
}

#ifdef __SWITCH__
/* "CPU 1020 / GPU 384 / memory 1331 MHz, handheld" from the last reads (platform_clock_info) */
void clocks_line(const PlatformClockInfo& ci, char* out, size_t n) {
    if (!ci.available) snprintf(out, n, "The clocks cannot be read on this system.");
    else if (!ci.cpu_mhz) snprintf(out, n, "Reading the clocks...");
    else
        snprintf(out, n, "CPU %u MHz,  GPU %u MHz,  memory %u MHz  (%s)", ci.cpu_mhz, ci.gpu_mhz, ci.mem_mhz,
                 ci.docked ? "docked" : "handheld");
}
#endif

/* ---- pages ---- */
void tab_performance() {
#ifdef __SWITCH__
    PlatformClockInfo ci;
    platform_clock_info(&ci);
    heading("CPU clock");
    static const int mhz[4] = {0, 1224, 1428, 1785};
    static const char* const names[4] = {"Stock (1020 MHz)", "1224 MHz", "1428 MHz", "1785 MHz"};
    static const char* const tips[4] = {
        "The Switch's own CPU clock for games. The port aims to run at full speed here; some busy areas "
        "(Episode II) can still drop below 30 fps for now.",
        "A small overclock: more headroom in busy areas, a little more battery use and heat.",
        "Held 30 fps in Episode II in testing. More battery use and heat.",
        "The highest the Switch uses itself (briefly, while loading). The most headroom, the most battery use "
        "and heat; best docked."};
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        if (radio(names[i], g_settings.cpu_clock == mhz[i], tips[i])) {
            g_settings.cpu_clock = mhz[i];
            settings_apply();
        }
    }
    char line[128];
    clocks_line(ci, line, sizeof line);
    ImGui::Text("Now: %s", line);
    if (ci.set_failed) warn("The system refused the clock change: the CPU stays as it was.");
    if (ci.other_tool)
        warn("Clock is being set by another tool (sys-clk?). Its setting wins while it has a profile for this "
             "game; remove that profile to use the setting here.");
    note("Used only while the game is on screen: the HOME menu, sleep and closing the game set the CPU back to "
         "stock. The memory clock is not changed.");
    heading("Handheld graphics");
    if (check("Nintendo's 384 MHz graphics profile in handheld", &g_settings.handheld_gpu,
              "One of the clock profiles Nintendo offers its own games (the system's default in handheld is "
              "307 MHz). Not an overclock; docked play is not changed (768 MHz)."))
        settings_apply();
#endif
    heading("Frame rate counter");
    if (check("Show the frame rate (FPS) in the corner", &g_settings.perf_overlay,
              "A small number in the top left corner (on the Switch beside the picture), updated about once a second. "
              "The game's full speed is 30 FPS."))
        settings_apply();
    note("Phantasy Star Online runs at its original 30 fps, as on the GameCube.");
}

void tab_display() {
    heading("Screen");
    if (radio("4:3 (original)", g_settings.screen != SCREEN_WIDE,
              "The GameCube's picture, with black bars at the sides of a wide screen.")) {
        g_settings.screen = SCREEN_4_3;
        settings_apply();
    }
    ImGui::SameLine();
    if (radio("16:9 widescreen", g_settings.screen == SCREEN_WIDE,
              "The game's cameras see more to the sides, nothing stretched. The graphics chip draws a third more "
              "pixels, so busy areas can run slower.")) {
        g_settings.screen = SCREEN_WIDE;
        settings_apply();
    }
    heading("HUD (16:9 only)");
    ImGui::BeginDisabled(g_settings.screen != SCREEN_WIDE);
    if (radio("Middle", !g_settings.hud_edges,
              "The whole HUD stays 4:3 in the middle of the picture, as on the GameCube.")) {
        g_settings.hud_edges = 0;
        settings_apply();
    }
    ImGui::SameLine();
    if (radio("Screen edges", g_settings.hud_edges != 0,
              "Status, party HP, lock-on window, action palette, radar and info popups move to the sides of the "
              "screen. Menus, shops, dialogue and word select stay in the middle.")) {
        g_settings.hud_edges = 1;
        settings_apply();
    }
    ImGui::EndDisabled();
    heading("Field of view");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::SliderInt("##fov", &g_settings.fov, FOV_MIN, FOV_MAX, "%d%%");
    help("How much the camera sees; 100% is the game's own view. Above 100% things at the edges of the "
         "picture can appear late, as the game only draws what its own view holds.");
    if (ImGui::IsItemDeactivatedAfterEdit()) settings_apply();
    ImGui::SameLine();
    if (ImGui::Button("Reset##fov") && g_settings.fov != 100) {
        g_settings.fov = 100;
        settings_apply();
    }
#ifdef PIC_RES_RADIO
    heading("Resolution");
    for (int i = 0; i < RES_MODES; i++) {
        static const int order[RES_MODES] = {RES_1X, RES_1_25X, RES_1_5X, RES_2X};  /* smallest first */
        const int r = order[i];
        static const char* const names[RES_MODES] = {"1x  (640x528)", "1.5x  (960x792)", "2x  (1280x1056)",
                                                     "1.25x  (800x660)"};
        static const char* const tips[RES_MODES] = {
            "The GameCube's own resolution: the lightest on the graphics chip.",
            "Sharper; needs more from the graphics chip.",
            "Sharpest; needs the most from the graphics chip.",
            "A little sharper than 1x; lighter than 1.5x."};
        if (i) ImGui::SameLine();
        if (radio(names[r], g_settings.resolution == r, tips[r])) {
            g_settings.resolution = r;
            settings_apply();
        }
    }
    note("Higher resolutions make the picture sharper and give the graphics chip more work.");
#endif
#ifdef PIC_SCALE_SLIDER
    heading("Resolution");
    {
        char fmt[48];
        snprintf(fmt, sizeof fmt, "%%dx  (%dx%d)", 640 * g_settings.pc_scale, 528 * g_settings.pc_scale);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::SliderInt("##pcscale", &g_settings.pc_scale, PC_SCALE_MIN, PC_SCALE_MAX, fmt);
        help("The size the game is drawn at, in times the GameCube's 640x528. Higher is sharper and needs a "
             "stronger graphics card; 3x suits 1440p, 4x suits 4K.");
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_apply();
    }
#endif
#ifdef PIC_MSAA
    heading("Anti-aliasing");
    for (int i = 0; i < 4; i++) {
        static const int samples[4] = {1, 2, 4, 8};
        static const char* const names[4] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
        if (i) ImGui::SameLine();
        if (radio(names[i], g_settings.msaa == samples[i],
                  i ? "Smooths jagged edges of 3D models; more samples are smoother and cost more."
                    : "No smoothing (the game's own look).")) {
            g_settings.msaa = samples[i];
            settings_apply();
        }
    }
#endif
    auto slider = [](const char* id, int* v, int lo, int hi, const char* fmt, const char* tip) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::SliderInt(id, v, lo, hi, fmt);
        help(tip);
        if (ImGui::IsItemDeactivatedAfterEdit()) settings_apply();
    };
    (void)slider;
#ifdef PIC_FSR
    heading("Sharpness (AMD FSR 1)");
    slider("##sharp", &g_settings.sharpness, 0, 100, g_settings.sharpness ? "%d%%" : "Off",
           "Sharpens the final picture (FSR 1 RCAS). When the screen is bigger than the resolution above, FSR also "
           "does the upscaling (EASU) instead of a plain stretch. Off = the picture as before.\n\n"
           "FSR costs graphics time every frame: in busy areas it can lower the frame rate. Turn it off if the "
           "game slows down.");
#endif
#ifdef PIC_GRADE
    heading("Colour");
    {
        struct Preset { const char* name; int bright, contrast, sat, warm; const char* tip; };
        static const Preset presets[] = {
            {"Original", 0, 100, 100, 0, "The game's own colours."},
            {"Vivid", 0, 118, 160, 0, "Stronger colours and more contrast."},
            {"Warm", 8, 110, 120, 45, "Warmer, red-orange."},
            {"Cool", 0, 112, 110, -45, "Crisper, blue."},
        };
        ImGui::PushID("grade");
        for (int i = 0; i < 4; i++) {
            const Preset& p = presets[i];
            int on = g_settings.grade_bright == p.bright && g_settings.grade_contrast == p.contrast &&
                     g_settings.grade_sat == p.sat && g_settings.grade_warm == p.warm;
            if (i) ImGui::SameLine();
            if (radio(p.name, on, p.tip)) {
                g_settings.grade_bright = p.bright, g_settings.grade_contrast = p.contrast;
                g_settings.grade_sat = p.sat, g_settings.grade_warm = p.warm;
                settings_apply();
            }
        }
        ImGui::PopID();
    }
    slider("##gbright", &g_settings.grade_bright, -50, 50, "Brightness %+d", "Lighter or darker 3D picture.");
    slider("##gcontrast", &g_settings.grade_contrast, 50, 150, "Contrast %d%%", "Difference between dark and light.");
    slider("##gsat", &g_settings.grade_sat, 0, 200, "Saturation %d%%", "Colour strength; 0% is black and white.");
    slider("##gwarm", &g_settings.grade_warm, -50, 50, "Warmth %+d", "Bluer (left) or redder (right).");
#endif
#ifdef PIC_CHARLIGHT
    heading("Lighting");
    if (radio("Original##light", !g_settings.char_light, "Lighting worked out at the corners of each triangle, as on the GameCube.")) {
        g_settings.char_light = 0;
        settings_apply();
    }
    ImGui::SameLine();
    if (radio("Smooth (per pixel)##light", g_settings.char_light != 0,
              "Lighting worked out for every pixel: smoother shading and highlights on lit 3D models.")) {
        g_settings.char_light = 1;
        settings_apply();
    }
#endif
#ifdef PIC_WINDOW
    heading("Window");
    if (check("Fullscreen  (F11 / Alt+Enter)", &g_settings.fullscreen, "Borderless fullscreen on the current display."))
        settings_apply();
#endif
    note(g_settings.screen != SCREEN_WIDE ? "The picture is 4:3, as on the GameCube."
         : g_settings.hud_edges ? "16:9: the HUD at the screen edges; menus stay 4:3 in the middle."
                                : "16:9: the HUD and menus stay 4:3 in the middle of the picture.");
}

#ifdef __SWITCH__
/* the button map (settings.h pad_map): one list per GameCube input; A opens it, the D-pad picks, A chooses,
 * B closes it unchanged. One Switch button may press several GameCube inputs: allowed, and listed in amber
 * under the table so it is never a surprise; so is an input with no button. Reset to default comes last, so
 * moving down to it scrolls everything above into view. */
void button_map() {
    heading("Buttons");
    note("For the Pro Controller, a Joy-Con pair and handheld mode (every player); the sticks stay as they are. A "
         "single Joy-Con held sideways keeps its fixed layout (SL / SR = L / R, stick click = Z, + or - = START). "
         "Holding - (Minus) opens this menu with any layout.");
    const float combo_w = ImGui::GetFontSize() * 9.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4));
    if (ImGui::BeginTable("bmap", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("gc1", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        ImGui::TableSetupColumn("sw1", ImGuiTableColumnFlags_WidthFixed, combo_w + ImGui::GetFontSize() * 2);
        ImGui::TableSetupColumn("gc2", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        ImGui::TableSetupColumn("sw2", ImGuiTableColumnFlags_WidthFixed, combo_w);
        const int rows = (GC_INPUTS + 1) / 2;
        for (int r = 0; r < rows; r++) {
            ImGui::TableNextRow();
            for (int half = 0; half < 2; half++) {
                const int in = r + half * rows;  /* left: A B X Y Z L, right: R START and the D-pad */
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(settings_gc_input_label(in));
                ImGui::TableNextColumn();
                ImGui::PushID(in);
                ImGui::SetNextItemWidth(combo_w);
                const int cur = g_settings.pad_map[in];
                if (ImGui::BeginCombo("##map", settings_sw_button_label(cur), ImGuiComboFlags_HeightLarge)) {
                    for (int sw = 0; sw < SW_BUTTONS; sw++) {
                        const bool sel = sw == cur;
                        if (ImGui::Selectable(settings_sw_button_label(sw), sel) && !sel) {
                            g_settings.pad_map[in] = sw;
                            settings_apply();
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (in == GC_IN_L || in == GC_IN_R)
                    help("The GameCube's L and R are analog triggers; a Switch button presses them fully.");
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    for (int sw = SW_NONE; sw < SW_BUTTONS; sw++) {
        char line[160];
        size_t k = 0;
        int n = 0;
        line[0] = 0;
        for (int in = 0; in < GC_INPUTS; in++)
            if (g_settings.pad_map[in] == sw && k < sizeof line) {
                int w = snprintf(line + k, sizeof line - k, "%s%s", n ? " and " : "", settings_gc_input_label(in));
                if (w > 0) k += (size_t)w;
                n++;
            }
        char msg[256];
        if (sw == SW_NONE && n) {
            snprintf(msg, sizeof msg, "No button presses %s.", line);
            warn(msg);
        } else if (sw != SW_NONE && n > 1) {
            snprintf(msg, sizeof msg, "%s presses %s together.", settings_sw_button_label(sw), line);
            warn(msg);
        }
    }
    if (ImGui::Button("Reset to default")) {
        memcpy(g_settings.pad_map, settings_pad_map_default, sizeof g_settings.pad_map);
        settings_apply();
    }
    help("A B X Y = A B X Y,  + = START,  ZL / ZR = L / R,  R = Z,  D-pad = D-pad.");
}
#endif

/* Right stick: Menus (the original game) or Camera (cam_stick.c); applies at once */
void right_stick_option() {
    heading("Right stick");
    if (radio("Menus", g_settings.right_stick != RIGHT_STICK_CAMERA,
              "The original game: the right stick (the GameCube C-stick) works as it did on the GameCube.")) {
        g_settings.right_stick = RIGHT_STICK_MENUS;
        settings_apply();
    }
    ImGui::SameLine();
    if (radio("Camera", g_settings.right_stick == RIGHT_STICK_CAMERA,
              "Left and right turn the camera around your character; up looks up and down looks down. The camera "
              "stays where you put it; L still puts it back behind you. Only in single-player view, and only while no "
              "window is open: in menus, chat, word select and shops the stick works as in the original game.")) {
        g_settings.right_stick = RIGHT_STICK_CAMERA;
        settings_apply();
    }
    note("Camera: bosses, quest scenes and conversations keep the game's own camera. Split screen keeps the original "
         "right stick.");
}

void tab_controls() {
#ifdef __SWITCH__
    heading("Controllers");
    int pads = 0;
    for (int c = 0; c < 4; c++) pads += c == 0 || input_connected(c);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Connected: %d", pads);
    ImGui::SameLine();
    if (ImGui::Button("Set up controllers")) {
        close_menu();
        g_pads_request = 1;
    }
    help("The system's controller screen: pair or split Joy-Cons and change the player order.");
    right_stick_option();
    button_map();
#else
    heading("Buttons");
    if (ImGui::BeginTable("map", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("GameCube");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        const char *gc, *key, *pad;
        for (int i = 0; input_map_row(i, &gc, &key, &pad); i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(gc);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(key);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(pad);
        }
        ImGui::EndTable();
    }
    note("Hold F1, or Back (View) on a controller, to open this menu (Q / E or the shoulder buttons switch its "
         "pages). F11 = fullscreen. More controllers are players 2 to 4.");
    right_stick_option();
    heading("Background");
    if (check("Controllers work in the background", &g_settings.pad_background,
              "Controllers keep playing while another window has focus. Turn off to ignore them then."))
        settings_apply();
#endif
}

void tab_system() {
    heading("PSO Episode I and II Plus native port");
    ImGui::Text("Version %s (build %s)", g_port_version, g_build_version);
    note("A fan-made native port built from your own disc. Not affiliated with SEGA or Nintendo.");
#ifdef __SWITCH__
    heading("Clocks now");
    PlatformClockInfo ci;
    platform_clock_info(&ci);
    char line[128];
    clocks_line(ci, line, sizeof line);
    ImGui::TextUnformatted(line);
    if (g_settings.cpu_clock) ImGui::Text("CPU setting: %d MHz (%s)", g_settings.cpu_clock, ci.overridden ? "in force" : "not set now");
    else ImGui::TextUnformatted("CPU setting: stock (the system decides)");
    if (ci.other_tool) warn("Clock is being set by another tool (sys-clk?).");
#endif
    heading("Online");
    NetStatus ns;
    net_status(&ns);
    ImGui::Text("Server: %s", ns.server);
    ImGui::Text("Then DNS: %s", ns.dns);
    ImGui::Text("Connection: %s", ns.state);
    if (ns.started && !ns.sockets_ok) warn("Network not available: online play will fail (see the log).");
    note("The game connects to net_server in settings.ini (default psobb.dyndns.org, Schtserv; a name or an IPv4 "
         "address). If that cannot be found: Schtserv's DNS 3.18.217.27, then 144.217.87.48. net_dns= (one address, "
         "or two with a comma) asks only those DNS servers instead.");
#ifdef __SWITCH__
    note("Chat: while online, press the right stick to type a line with the system keyboard (ASCII letters, digits "
         "and symbols); it is typed into the game and sent with Enter.");
#endif
    heading("Settings");
    ImGui::Text("Saved in %s", platform_settings_path());
    heading("Bug reports");
    if (ImGui::Button("Save screenshot")) {
        close_menu();
        g_shot_request = 1;
    }
    help("Saves the next picture next to the game's log, for bug reports.");
#ifdef HAVE_BUG_REPORT
    static char report[64];
    static int report_failed;
    ImGui::SameLine();
    if (ImGui::Button("Save a bug report")) {
        report_failed = platform_bug_report(report, sizeof report) != 0;
        if (!report_failed) platform_open_game_folder();
    }
    help("Puts the game's log, your settings and system information into one .zip in the game's folder and "
         "opens that folder. It holds folder names from your PC (in the logs): look through it first if that "
         "matters to you.");
    if (report_failed) note("Could not write the bug report in the game's folder.");
    else if (report[0]) {
        char msg[160];
        snprintf(msg, sizeof msg, "Saved %s in the game's folder (the folder is open now).", report);
        note(msg);
    }
#endif
}

void quit_row() {
    const float w_close = ImGui::CalcTextSize("Close").x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::PushStyleColor(ImGuiCol_Text, kNote);
    ImGui::TextUnformatted("L / R: pages    A: choose    B: close");
    ImGui::PopStyleColor();
#ifdef __SWITCH__
    /* no Quit Game: games on the Switch are closed from the HOME menu */
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w_close);
    if (ImGui::Button("Close")) close_menu();
#else
    const float w_quit = ImGui::CalcTextSize("Quit Game").x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w_close - w_quit - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Close")) close_menu();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.14f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.22f, 0.18f, 1.0f));
    if (ImGui::Button("Quit Game")) {
        g_quit_popup = true;
        ImGui::OpenPopup("Quit the game?");
    }
    ImGui::PopStyleColor(2);
    ImGui::SetNextWindowPos(ImVec2(OVERLAY_W * 0.5f, OVERLAY_H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit the game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        note("Progress since your last save in the game is lost.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(140, 0)) || !g_quit_popup) {
            g_quit_popup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();  /* A straight away cancels */
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(140, 0))) {
            g_quit_popup = false;
            ImGui::CloseCurrentPopup();
            close_menu();
            g_quit_request = 1;
        }
        ImGui::EndPopup();
    }
#endif
}

void settings_window() {
    const ImVec2 size(fminf(980.0f, OVERLAY_W * 0.94f), fminf(620.0f, OVERLAY_H * 0.90f));
    ImGui::SetNextWindowPos(ImVec2(OVERLAY_W * 0.5f, OVERLAY_H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (g_just_opened) {
        ImGui::SetNextWindowFocus();
        g_just_opened = false;
    }
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("PSO Episode I and II Plus: port settings", nullptr, fl)) {
        struct Tab {
            const char* name;
            void (*draw)();
        };
        static const Tab tabs[kTabs] = {{"Performance", tab_performance}, {"Display", tab_display},
                                        {"Controls", tab_controls}, {"System", tab_system}};
        if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
            for (int i = 0; i < kTabs; i++) {
                ImGuiTabItemFlags f = g_select_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(tabs[i].name, nullptr, f)) {
                    g_tab = i;
                    /* the page ends one row above the window's bottom: the Close / Quit row */
                    ImGui::BeginChild("page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4), ImGuiChildFlags_NavFlattened);
                    tabs[i].draw();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            g_select_tab = -1;
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        quit_row();
        /* up from the top row (the tabs) goes to the Close / Quit row and down from there back to the top */
        ImGui::NavMoveRequestTryWrapping(ImGui::GetCurrentWindow(), ImGuiNavMoveFlags_LoopY);
    }
    ImGui::End();
}

/* the counter: "30 FPS" in a small dark box at the top left. On the Switch the overlay covers the whole
 * 16:9 screen and the game's 4:3 picture sits in its middle 960 of 1280 pixels, so the box (about 60 x 22
 * overlay pixels) is in the black bar beside the picture. */
void readout_box() {
    if (!g_ro_on || !g_ro_text[0]) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    ImVec2 ts = font->CalcTextSizeA(kReadoutFontSize, FLT_MAX, 0.0f, g_ro_text);
    const float x = 8, y = 8, px = 6, py = 3;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ts.x + 2 * px, y + ts.y + 2 * py), IM_COL32(14, 16, 20, 190), 3.0f);
    dl->AddText(font, kReadoutFontSize, ImVec2(x + px, y + py), IM_COL32(236, 240, 242, 255), g_ro_text);
}

void banner_box() {
    if (!g_banner[0]) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize(g_banner, nullptr, false, OVERLAY_W - 160.0f);
    float x0 = (OVERLAY_W - ts.x) * 0.5f - 20, y0 = 40;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + ts.x + 40, y0 + ts.y + 24), IM_COL32(60, 6, 10, 225), 8.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + ts.x + 40, y0 + ts.y + 24), IM_COL32(255, 80, 80, 230), 8.0f, 0, 2.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0 + 20, y0 + 12), IM_COL32(255, 240, 240, 255), g_banner,
                nullptr, OVERLAY_W - 160.0f);
}

uint64_t draw_hash(const ImDrawData* dd) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, b + i, 8);
            h = (h ^ v) * 1099511628211ull;
        }
        for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const ImDrawList* l : dd->CmdLists) {
        mix(l->VtxBuffer.Data, (size_t)l->VtxBuffer.Size * sizeof(ImDrawVert));
        mix(l->IdxBuffer.Data, (size_t)l->IdxBuffer.Size * sizeof(ImDrawIdx));
        for (const ImDrawCmd& c : l->CmdBuffer) mix(&c.ClipRect, sizeof c.ClipRect);
    }
    return h;
}

/* a finished image becomes the shown one */
void collect() {
    Job* j;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        j = g_done;
        g_done = nullptr;
    }
    if (!j) return;
    memcpy(g_box[j->buf], j->box, sizeof j->box);
    g_front = j->buf;
    g_front_open = j->open;
    g_version++;
    g_busy = false;
    static int logged;
    if (logged < 3 && g_open) {
        logged++;
        if (j->cpu_ms >= 0.0) rt_log("[menu] picture drawn in %.2f ms (helper thread; %.2f ms of it on the CPU)", j->ms, j->cpu_ms);
        else rt_log("[menu] picture drawn in %.2f ms (helper thread)", j->ms);
    }
    g_st_pics.fetch_add(1, std::memory_order_relaxed);
    uint64_t us = (uint64_t)(j->ms * 1000.0);
    g_st_draw_us.fetch_add(us, std::memory_order_relaxed);
    st_max(g_st_draw_max_us, us);
    if (j->cpu_ms >= 0.0) g_st_cpu_us.fetch_add((uint64_t)(j->cpu_ms * 1000.0), std::memory_order_relaxed);
    else g_st_cpu_unknown.store(1, std::memory_order_relaxed);
    job_free(j);
}

/* one ImGui frame; the image is drawn again only when ImGui's output changed */
void frame() {
    g_retry = false;
    init();
    ImGuiIO& io = ImGui::GetIO();
    uint64_t now = platform_now_ns();
    float dt = (float)((now - g_last_ns) / 1e9);
    g_last_ns = now;
    io.DeltaTime = dt > 0.0f && dt < 0.5f ? dt : 1.0f / 60.0f;
    ImGui::NewFrame();
    if (g_open) settings_window();
    readout_box();
    banner_box();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    uint64_t h = draw_hash(dd);
    bool tex_pending = false;
    if (dd->Textures)
        for (ImTextureData* t : *dd->Textures) tex_pending |= t->Status != ImTextureStatus_OK && t->Status != ImTextureStatus_Destroyed;
    if (h == g_last_hash && g_had_image && !tex_pending) return;
    if (g_busy) {  /* the helper is still drawing: this picture is looked at again next frame */
        g_retry = true;
        return;
    }
    /* texture requests (the font atlas) are answered here, while the helper is idle: it reads the
     * pixels of textures in use, and ImGui frees a texture's pixels only after it was answered */
    if (tex_pending) menu_raster_textures(dd);
    g_last_hash = h;
    g_had_image = true;
    Job* j = new Job();
    j->dd.Valid = true;
    j->dd.DisplayPos = dd->DisplayPos;
    j->dd.DisplaySize = dd->DisplaySize;
    j->dd.FramebufferScale = dd->FramebufferScale;
    j->dd.Textures = nullptr;
    for (ImDrawList* l : dd->CmdLists) j->dd.CmdLists.push_back(l->CloneOutput());
    j->dd.CmdListsCount = j->dd.CmdLists.Size;
    j->buf = g_front ^ 1;
    j->open = g_open;
    content_box(dd, j->box);
    if (!g_worker_started) {
        g_worker_started = true;
        rt_thread_start(raster_thread, nullptr);
    }
    g_busy = true;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_job = j;
    }
    g_cv.notify_one();
}

void feed(ImGuiKey k, unsigned held, unsigned changed, unsigned bit) {
    if (changed & bit) ImGui::GetIO().AddKeyEvent(k, (held & bit) != 0);
}

}  // namespace

extern "C" {

void overlay_set_fps(float fps) { g_fps10.store((int)(fps * 10.0f + 0.5f)); }

void overlay_take_stats(OverlayStats* s) {
    s->pictures = g_st_pics.exchange(0, std::memory_order_relaxed);
    s->draw_ms = g_st_draw_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    s->draw_max_ms = g_st_draw_max_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    s->draw_cpu_ms = g_st_cpu_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    s->cpu_known = !g_st_cpu_unknown.exchange(0, std::memory_order_relaxed);
    s->ui_frames = g_st_ui_frames.exchange(0, std::memory_order_relaxed);
    s->ui_ms = g_st_ui_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    s->ui_max_ms = g_st_ui_max_us.exchange(0, std::memory_order_relaxed) / 1000.0;
    s->open = g_open;
}

void overlay_set_banner(const char* text) {
    std::lock_guard<std::mutex> l(g_banner_lock);
    snprintf(g_banner_next, sizeof g_banner_next, "%s", text ? text : "");
    g_banner_seq.fetch_add(1);
}

int overlay_take_controllers_request(void) {
    int r = g_pads_request;
    g_pads_request = 0;
    return r;
}

void overlay_request_screenshot(void) { g_shot_request = 1; }  /* SR_SHOTSTEP (frame_pacing.c) */

/* the overlay is drawn from the helper thread's last finished picture, which can still be the menu's for a
 * frame or two after "Save screenshot" closed it. The request waits until the shown picture is one drawn
 * with the menu closed. */
int overlay_take_screenshot_request(void) {
    if (g_shot_request && (g_open || g_front_open)) return 0;
    int r = g_shot_request;
    g_shot_request = 0;
    return r;
}

int overlay_menu_open(void) { return g_open; }

/* a touch control's menu button: open or close at once (no hold) */
void overlay_menu_toggle(void) {
    if (g_open) {
        close_menu();
        return;
    }
    g_open = g_just_opened = true;
    if (g_ready) ImGui::GetIO().ClearInputKeys();
}

/* a finger on the open menu, in overlay pixels (OVERLAY_W x OVERLAY_H); down = 0 when lifted */
void overlay_pointer(float x, float y, int down) {
    init();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down != 0);
    if (!down) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  /* no hover left behind after the finger lifts */
}

/* the PC mouse (render_gl.c, the render thread, like the menu itself): point, click, drag sliders,
 * scroll; events while the menu is closed are dropped, except releases (no button left held) */
void overlay_mouse_pos(float x, float y) {
    if (!g_open) return;
    init();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMousePosEvent(x, y);
}

void overlay_mouse_button(int button, int down) {
    if ((!g_open && down) || button < 0 || button > 2) return;
    init();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMouseButtonEvent(button, down != 0);
}

void overlay_mouse_wheel(float dx, float dy) {
    if (!g_open) return;
    init();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    io.AddMouseWheelEvent(dx, dy);
}

void overlay_request_quit(void) { g_quit_request = 1; }

void overlay_shutdown(void) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_worker_stopping) return;
        g_worker_stopping = true;
    }
    g_cv.notify_all();
    if (g_worker_started) {
        std::unique_lock<std::mutex> lk(g_mx);
        g_cv.wait(lk, [] { return g_worker_stopped; });
    }
    /* No worker can retain an ImDrawList or font-atlas pointer after this point. */
    if (g_job) job_free(g_job);
    if (g_done) job_free(g_done);
    g_job = g_done = nullptr;
    g_busy = g_open = g_had_image = false;
    if (g_ready) ImGui::DestroyContext();
    g_ready = false;
}

int overlay_take_quit_request(void) {
    int r = g_quit_request;
    g_quit_request = 0;
    return r;
}

int overlay_input(unsigned held) {
    static int tested;
    if (!tested++ && getenv("SR_TEST_MENU")) {  /* debugging: start with the menu open (=N: on tab N) */
        g_open = g_just_opened = true;
        g_select_tab = atoi(getenv("SR_TEST_MENU"));
    }
    unsigned pressed = held & ~g_prev;
    unsigned changed = held ^ g_prev;
    g_prev = held;
    if (!g_open) {
        if (g_gate) {  /* closed, but buttons still held from the menu: kept from the game */
            if (held) return 1;
            g_gate = false;
        }
        if (held & OV_MENU) {
            uint64_t now = rt_ticks();
            if (!g_menu_since) g_menu_since = now;
            if (now - g_menu_since >= kHoldTicks) {
                g_open = g_just_opened = true;
                g_menu_since = 0;
                g_prev = held | OV_MENU;  /* the held Minus does not close it again */
                if (g_ready) {
                    /* start from a clean input state */
                    ImGuiIO& io = ImGui::GetIO();
                    io.ClearInputKeys();
                }
                return 1;
            }
        } else {
            g_menu_since = 0;
        }
        return 0;
    }
    init();
    if ((pressed & OV_MENU) && !g_quit_popup) {
        close_menu();
        return 1;
    }
    if (pressed & OV_BACK) {
        if (g_quit_popup) g_quit_popup = false;  /* the dialog closes itself next frame */
        else if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            close_menu();
            return 1;
        }
    }
    if (!g_quit_popup) {
        if (pressed & OV_TAB_L) g_select_tab = (g_tab + kTabs - 1) % kTabs;
        if (pressed & OV_TAB_R) g_select_tab = (g_tab + 1) % kTabs;
    }
    feed(ImGuiKey_GamepadDpadUp, held, changed, OV_UP);
    feed(ImGuiKey_GamepadDpadDown, held, changed, OV_DOWN);
    feed(ImGuiKey_GamepadDpadLeft, held, changed, OV_LEFT);
    feed(ImGuiKey_GamepadDpadRight, held, changed, OV_RIGHT);
    feed(ImGuiKey_GamepadFaceDown, held, changed, OV_ACCEPT);  /* ImGui's "activate" */
    feed(ImGuiKey_GamepadFaceRight, held, changed, OV_BACK);   /* ImGui's "cancel": B closes an open list */
    return 1;
}

const uint8_t* overlay_image(unsigned* version, int box[4]) {
    /* the frame rate counter: its text is made again only when the whole number changes or it was switched
     * on or off (one atomic load per frame while on; nothing while off) */
    bool ro_changed = false;
    if (g_settings.perf_overlay) {
        int f10 = g_fps10.load(std::memory_order_relaxed);
        int fps = f10 >= 0 ? (f10 + 5) / 10 : -1;
        if (!g_ro_on || fps != g_ro_fps) {
            if (fps >= 0) snprintf(g_ro_text, sizeof g_ro_text, "%d FPS", fps);
            else snprintf(g_ro_text, sizeof g_ro_text, "-- FPS");
            g_ro_fps = fps;
            g_ro_on = true;
            ro_changed = true;
        }
    } else if (g_ro_on) {
        g_ro_on = false;
        g_ro_text[0] = 0;
        g_ro_fps = -2;
        ro_changed = true;
    }
    unsigned bseq = g_banner_seq.load();
    bool banner_changed = bseq != g_shown_banner_seq;
    if (banner_changed) {
        std::lock_guard<std::mutex> l(g_banner_lock);
        memcpy(g_banner, g_banner_next, sizeof g_banner);
        g_shown_banner_seq = bseq;
    }
    static bool was_open;
    bool shown = g_open || g_ro_on || g_banner[0];
    /* the menu runs every frame while open (navigation, tooltips); the counter only when its text changes */
    if (g_busy) collect();  /* nothing can be waiting unless a job is out */
    /* a picture left waiting because the helper was busy is made again too */
    bool open_before = g_open;
    if (g_open || ro_changed || banner_changed || was_open != g_open || g_retry) {
        uint64_t t0 = platform_now_ns();
        frame();
        uint64_t us = (platform_now_ns() - t0) / 1000;
        g_st_ui_frames.fetch_add(1, std::memory_order_relaxed);
        g_st_ui_us.fetch_add(us, std::memory_order_relaxed);
        st_max(g_st_ui_max_us, us);
    }
    /* Close (or Quit) closes the menu inside frame(): that picture still shows the menu, so the next call
     * draws again without it */
    was_open = open_before;
    *version = g_version;
    if (box) memcpy(box, g_box[g_front], sizeof g_box[g_front]);
    return shown && g_had_image ? g_img[g_front] : nullptr;
}

}  // extern "C"

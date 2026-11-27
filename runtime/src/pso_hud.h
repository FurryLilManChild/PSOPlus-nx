/* 16:9 widescreen, build B (docs/widescreen-research.md section 6): HUD windows at the screen edges.
 *
 * Shared by the game thread (pso_wide.c: which placement each class asks for, from hud_anchor.txt), the GP
 * thread (gx_render.c: one placement per window from the x range of all its draws) and the host test
 * (tools/tests/pso_wide_test.c). Pure functions, no state.
 *
 * Coordinates are the game's 4:3 frame, x 0..640. A window placed left (right) keeps its distance from the
 * 4:3 frame's left (right) edge, measured from the 16:9 screen's edge instead: shifted by -320 (k - 1)
 * (+320 (k - 1)) from where build A put it (4:3 in the middle; k = the EFB width factor, 854 / 640). */
#pragma once
#include <stdint.h>
#include <string.h>

/* placements (hud_anchor.txt words); 0 = the class has no line of its own */
enum { HUD_NONE, HUD_AUTO, HUD_LEFT, HUD_RIGHT, HUD_CENTRE, HUD_STRETCH, HUD_FOLLOW, HUD_PLACES };
static const char* const k_hud_place_names[HUD_PLACES] = {"none", "auto", "left", "right", "centre", "stretch", "follow"};

/* why a window got its placement ([hud] lines) */
enum { HUD_WHY_MEASURED, HUD_WHY_FILE, HUD_WHY_DEFAULT, HUD_WHY_SPLIT, HUD_WHY_NO_DRAWS, HUD_WHY_MIRROR, HUD_WHY_MENU, HUD_WHYS };
static const char* const k_hud_why[HUD_WHYS] = {
    "measured", "hud_anchor.txt", "hud_anchor.txt default", "split screen: kept in the middle",
    "auto, but no measured draws", "mirrored view: kept in the middle", "menu screen, hud_anchor.txt menu: line"};

#define HUD_RULES_MAX 96
#define HUD_NAME_MAX 48
/* menu: the line is for the menu screen (a "menu:" line, see hud_rules_parse) */
typedef struct { char name[HUD_NAME_MAX]; uint8_t prefix, place, menu; } HudRule;
typedef struct { HudRule r[HUD_RULES_MAX]; int n; uint8_t dflt, menu_dflt; int errors; int first_error_line; } HudRules;
const HudRules* pso_hud_rules(void);  /* pso_wide.c: the table in use (built in, or hud_anchor.txt beside settings.ini) */

static inline int hud_place_from_word(const char* w, size_t n) {
    for (int i = HUD_AUTO; i < HUD_PLACES; i++)
        if (strlen(k_hud_place_names[i]) == n && !strncmp(w, k_hud_place_names[i], n)) return i;
    if (n == 6 && !strncmp(w, "center", 6)) return HUD_CENTRE;
    if (n == 6 && !strncmp(w, "middle", 6)) return HUD_CENTRE;
    return HUD_NONE;
}

/* hud_anchor.txt: "<class>[*] <placement>" per line, '#' starts a comment, "* <placement>" is the default.
 * A line starting with the word "menu:" is for the menu screen (the in-field main menu with the 3D view shrunk
 * into a box, pso_hud_menu_screen): "menu: <class>[*] <placement>", "menu: * <placement>" its default (centre
 * when not given). While the menu screen is up a window takes its class's menu: line, else the menu: default;
 * its ordinary line does not apply. Bad lines are counted (t->errors, the first in t->first_error_line) and
 * skipped. A later line for the same name replaces the earlier one. Returns the number of rules. */
static inline int hud_rules_parse(HudRules* t, const char* text) {
    memset(t, 0, sizeof *t);
    t->dflt = HUD_CENTRE;
    t->menu_dflt = HUD_CENTRE;
    int line = 0;
    for (const char* p = text; p && *p;) {
        line++;
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char* h = memchr(p, '#', len);
        if (h) len = (size_t)(h - p);
        const char* q = p;
        const char* end = p + len;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
        int menu = 0;
        if (end - q >= 5 && !strncmp(q, "menu:", 5)) {
            menu = 1;
            q += 5;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
        }
        const char* n0 = q;
        while (q < end && *q != ' ' && *q != '\t' && *q != '\r') q++;
        size_t nlen = (size_t)(q - n0);
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
        const char* w0 = q;
        while (q < end && *q != ' ' && *q != '\t' && *q != '\r') q++;
        size_t wlen = (size_t)(q - w0);
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
        if (nlen || menu) {
            int place = wlen ? hud_place_from_word(w0, wlen) : HUD_NONE;
            int bad = !nlen || place == HUD_NONE || q != end || nlen >= HUD_NAME_MAX;
            if (!bad && nlen == 1 && n0[0] == '*') {
                if (place == HUD_FOLLOW) bad = 1;  /* not a default */
                else if (menu) t->menu_dflt = (uint8_t)place;
                else t->dflt = (uint8_t)place;
            } else if (!bad) {
                int prefix = n0[nlen - 1] == '*';
                size_t m = nlen - (size_t)prefix;
                int i;
                for (i = 0; i < t->n; i++)
                    if (t->r[i].prefix == prefix && t->r[i].menu == menu && strlen(t->r[i].name) == m &&
                        !strncmp(t->r[i].name, n0, m))
                        break;
                if (i == t->n) {
                    if (t->n == HUD_RULES_MAX) bad = 1;
                    else t->n++;
                }
                if (!bad) {
                    memcpy(t->r[i].name, n0, m);
                    t->r[i].name[m] = 0;
                    t->r[i].prefix = (uint8_t)prefix;
                    t->r[i].place = (uint8_t)place;
                    t->r[i].menu = (uint8_t)menu;
                }
            }
            if (bad) {
                if (!t->errors) t->first_error_line = line;
                t->errors++;
            }
        }
        p = e ? e + 1 : NULL;
    }
    return t->n;
}

/* the class's own line: exact name, else the longest matching prefix; HUD_NONE when it has none (the default
 * is not applied here: a window takes the first own line of its classes, else the default) */
static inline int hud_rules_lookup_in(const HudRules* t, const char* name, int menu) {
    int best = HUD_NONE;
    size_t best_len = 0;
    for (int i = 0; i < t->n; i++) {
        const HudRule* r = &t->r[i];
        if (r->menu != menu) continue;
        if (!r->prefix) {
            if (!strcmp(name, r->name)) return r->place;
            continue;
        }
        size_t m = strlen(r->name);
        if (m >= best_len && !strncmp(name, r->name, m)) { best = r->place; best_len = m + 1; }
    }
    return best;
}
static inline int hud_rules_lookup(const HudRules* t, const char* name) { return hud_rules_lookup_in(t, name, 0); }
/* the class's own menu: line (HUD_NONE: none; the menu: default then applies) */
static inline int hud_rules_lookup_menu(const HudRules* t, const char* name) { return hud_rules_lookup_in(t, name, 1); }
/* the placement a window's own class asks for while the menu screen is up: its menu: line, else the menu:
 * default (never HUD_NONE, never follow) */
static inline int hud_menu_place(const HudRules* t, const char* name) {
    int p = hud_rules_lookup_menu(t, name);
    if (p == HUD_NONE || p == HUD_FOLLOW) p = t->menu_dflt;
    return p;
}

/* The menu screen (docs/widescreen-research.md section 7). In one-player play the main menu
 * (TWindowMainMenu1P::update_backdrop_transition 0x80288520, the only caller of TWindowCtrlMain::set_left
 * 0x8024CC5C and set_bottom 0x8024CC2C in the DOL) shrinks the 3D view into a box at the top right:
 * viewport_states[0].left = 53.33 x (0..3, or 6 while the Log page is open), .bottom = 480 - 74.67 x (0..3),
 * stepping one a frame; TWindowCtrlScreen::render (0x8024CA5C) then draws the teal panels left of and below
 * the box (draw_menu_backdrop_gradients 0x8024CE68). The box is open whenever left > 0 or bottom < 480; both
 * return to 0 / 480 on the frame the view goes back to full screen (end_view_override). Split screen uses
 * its own menu (TWindowMainMenu4P), which never shrinks the view. */
static inline int hud_menu_screen(float left, float bottom, int split) {
    if (split) return 0;
    /* comparisons are false for NaN; before TWindowCtrlMain first sets the states up (WindowControlState::init:
     * left 0, bottom 480) they are zero, which is no box either */
    return (left >= 0.1f && left <= 640.0f) || (bottom >= 1.0f && bottom <= 479.9f);
}

/* The menu screen's panels (TWindowCtrlScreen's own draws) in 16:9: the 4:3 layout in the middle, with what
 * touches the 4:3 frame's edges carried out to the screen's edges, so the panels fill the screen as one:
 * a vertex on the left (right) edge of the 4:3 frame goes to the left (right) screen edge, the others stay
 * in the middle (the left column 0..left becomes 0..left + 107, the bottom band 0..640 the full width).
 * A narrow piece touching an edge (16 columns or less: the box's corner piece at left..left + 16 when left
 * is 0, the box's left line at x = left = 0 while the panels fade out) moves whole to that edge instead of
 * being stretched. x, lo..hi in columns of the 4:3 frame (lo..hi the draw's range). Returns a pso_hud.h
 * placement for the vertex: HUD_LEFT, HUD_RIGHT or HUD_CENTRE. */
#define HUD_EDGE_EPS 0.5f
#define HUD_NARROW_PIECE 16.5f
static inline int hud_backdrop_place(float x, float lo, float hi) {
    if (!(lo <= hi)) return HUD_CENTRE;
    if (hi - lo <= HUD_NARROW_PIECE) {
        if (lo <= HUD_EDGE_EPS) return HUD_LEFT;
        if (hi >= 640.0f - HUD_EDGE_EPS) return HUD_RIGHT;
        return HUD_CENTRE;
    }
    if (x <= HUD_EDGE_EPS) return HUD_LEFT;
    if (x >= 640.0f - HUD_EDGE_EPS) return HUD_RIGHT;
    return HUD_CENTRE;
}

/* the measured rule: a window inside the left third and the left half (4 columns of slack) pins left, inside
 * the right third and the right half pins right; spanning the middle, or the full width, stays centred */
#define HUD_THIRD (640.0f / 3.0f)
#define HUD_SLACK 4.0f
static inline int hud_measured(float lo, float hi) {
    if (!(lo <= hi)) return HUD_CENTRE;
    if (lo < HUD_THIRD && hi <= 320.0f + HUD_SLACK) return HUD_LEFT;
    if (hi > 640.0f - HUD_THIRD && lo >= 320.0f - HUD_SLACK) return HUD_RIGHT;
    return HUD_CENTRE;
}

/* one window's placement: force = its own hud_anchor.txt line (HUD_NONE: none), dflt = the file's default,
 * menu = the menu screen is up (force is then the class's menu: placement, pso_wide.c),
 * lo..hi = the x range of its measured draws (lo > hi: none), split = split screen is on. Returns HUD_LEFT,
 * HUD_RIGHT, HUD_CENTRE or HUD_STRETCH, and why (HUD_WHY_*). */
static inline int hud_decide_menu(int force, int dflt, float lo, float hi, int split, int menu, int* why) {
    int w = menu ? HUD_WHY_MENU : HUD_WHY_FILE, p = force;
    if (p == HUD_NONE) { p = dflt; w = HUD_WHY_DEFAULT; }
    if (split) { *why = HUD_WHY_SPLIT; return HUD_CENTRE; }  /* each player's HUD stays inside its own view */
    if (p == HUD_AUTO) {
        if (!(lo <= hi)) { *why = HUD_WHY_NO_DRAWS; return HUD_CENTRE; }
        *why = HUD_WHY_MEASURED;
        return hud_measured(lo, hi);
    }
    *why = w;
    if (p == HUD_LEFT || p == HUD_RIGHT || p == HUD_STRETCH) return p;
    return HUD_CENTRE;  /* centre, and follow (not a window placement) */
}
static inline int hud_decide(int force, int dflt, float lo, float hi, int split, int* why) {
    return hud_decide_menu(force, dflt, lo, hi, split, 0, why);
}

/* the shift from the centred place, in pixels of the 640-wide frame (the backends' scissor) */
static inline float hud_shift_px(int place, float k) {
    float hx = 320.0f * (k - 1.0f);
    return place == HUD_LEFT ? -hx : place == HUD_RIGHT ? hx : 0.0f;
}
/* the same shift in clip units of a HUD draw whose 4:3 viewport is ax columns half wide (gx.vert: the backend
 * draws it in a viewport ax k half wide, so one clip unit is ax k pixels) */
static inline float hud_shift_clip(int place, float k, float ax) {
    return hud_shift_px(place, k) / (ax * k);
}

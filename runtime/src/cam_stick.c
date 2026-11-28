/* Right-stick camera: menu > Controls > "Right stick: Camera" (settings.h right_stick). Off (Menus, the
 * default) every hook below only runs the game's own function, so the game behaves as on the GameCube.
 *
 * On, in single-player view with the follow camera (TCameraFollow, the game's normal third-person camera)
 * driving the view and no window taking input:
 *   - the right stick (the GameCube C-stick) turns the camera around the player and raises or lowers the eye
 *     (cam_stick.h has the speeds and limits);
 *   - the camera stays where it was put: update_follow_motion_state no longer swings it back behind the player
 *     while running or after the idle timeout. Its wall handling still runs (follow_flags 1: an obstruction is
 *     predicted while moving, the game snaps or eases the camera behind the player), and L still recentres
 *     (run_task's own reset button path, not touched here);
 *   - the C-stick does not also press the game's C-stick direction buttons.
 * While a window takes input (main menu, chat, word select / short lists, shops and counters, trade, a card
 * exchange), the stick is the game's again (menu navigation) and the camera does not move. Bosses with their
 * own follow camera offsets, quest camera overrides, conversation close-ups and every other camera controller
 * keep their own cameras. Movement follows: the game turns the stick by the view's yaw (view_yaw[0], copied from
 * the camera each frame in TCameraCtrl::update_active_controller 0x8006A7D8, offline and online).
 *
 * Game layout from zeroz41/pso-decomp (MIT, docs/upstream.md), each offset checked in PsoV3.dol's code (run_task
 * 0x8006E310, update_follow_motion_state 0x8006D6DC, calc_follow_position 0x8006D95C, apply_override_pose
 * 0x8006CDD0, TObjPlayer::check_flag 0x800495E0, get_controller_port 0x801AFD84). */
#include <stdint.h>
#include <string.h>

#include "cam_stick.h"
#include "ppc.h"
#include "runtime.h"
#include "settings.h"

void f_8006D6DC_orig(Cpu* c);  /* TCameraFollow::update_follow_motion_state (generated) */
void f_8006D95C_orig(Cpu* c);  /* TCameraFollow::calc_follow_position */
void f_801A6A5C_orig(Cpu* c);  /* map_c_stick_to_buttons */

/* game globals (r13 = 0x805DA600) */
#define FOLLOW_VTABLE       0x80488E00u  /* __vt__13TCameraFollow: TCameraFollow itself, not a subclass */
#define CAM_REGISTRY        0x80488A58u  /* camera_controller_registry[13] */
#define CAM_VIEW_CONTROLLER 0x804FF628u  /* TCameraCtrl::view_state.controller (u8): the active registry index */
#define CAM_OVERRIDE_FLAGS  0x805D4E20u  /* camera_override_flags (u8): the quest camera opcodes' fixed poses */
#define SPLIT_SELECTOR      0x805D34B8u  /* camera_split_screen_selector: is_split_screen 0x80019690 is "not 1" */
#define LOCAL_CLIENT_ID     0x805D53A0u
#define PADS                0x8050E228u  /* controller_input_state.controllers[4] (0x8050E208 + 0x20), 0x50 each */
#define PAD_SIZE            0x50u
#define PAD_CSTICK_X        0x18u        /* s16 */
#define PAD_CSTICK_Y        0x1Au        /* s16, -substickY */
/* game functions: TWindowCtrl's own "is the player in a window" tests (TWindowCtrlMain hides the
 * lock-on window on the same four) */
#define FN_HAS_ACTIVE_MENU  0x8024AF74u  /* has_player_active_menu(client): the main menu (and its submenus) */
#define FN_IS_SUPPRESSED    0x8024B054u  /* is_slot_visible(client): the player's windows suppressed (shops, counters) */
#define FN_HAS_SHORT_LIST   0x8024AFB8u  /* has_client_short_list(this, client): word select and the short lists */
#define FN_CHAT_IN_USE      0x8024A314u  /* is_chat_window_in_use() */

/* TCameraController / TCameraFollow (0x110 bytes) */
#define CAM_VTABLE      0x18u
#define CAM_ACTOR       0x1Cu   /* TObjPlayer* */
#define CAM_ACTOR_ID    0x20u   /* u16 */
#define CAM_STATE       0x24u   /* 1 = following */
#define CAM_YAW         0x30u
#define CAM_EYE_Y       0xE0u   /* eye_offset.y */
#define CAM_MOTION      0xF8u   /* motion_state: 0 still, 1 moving, 2 recentring */
#define CAM_FOLLOW_FL   0xFCu   /* follow_flags: 1 obstruction predicted, 8 offsets requested (boss arenas) */
#define CAM_CLOSE_UP    0x100u  /* conversation close-up */
/* TObjPlayer */
#define PL_ACTOR_FLAGS  0x30u   /* 0x10000: at a shop or counter (TShopSequence, init_effect_5f0) */
#define PL_STATE_FLAGS  0x334u  /* 0x100: controls locked by a window (is_controls_locked 0x801B5680) */
#define PL_PORT         0x3D0u
#define PL_ROT_Y        0x60u   /* facing angle (update_follow_motion_state: rotation_y + 0x8000) */

#define STALE_FIELDS    8u      /* no camera update for this long: the stick goes back to the game */
#define LOG_LINES_MAX   300

enum { OWN_GAME, OWN_CAMERA, OWN_MENUS };
static int g_owner = OWN_GAME;
static uint32_t g_owner_why;      /* OWN_MENUS: the window bits; OWN_GAME: the reason bit */
static uint32_t g_stamp;          /* VI field of the last camera update */
static uint32_t g_pad_ea;         /* the controller context the camera reads */
static int g_latch;               /* the stick was turning the camera: no direction bits until it is released */
static float g_height;            /* eye height offset (cam_stick.h), kept between frames */
static uint32_t g_last_yaw;       /* the yaw as our position hook left it, to see the game's recentre */
static unsigned g_lines;

/* window bits (logged) */
enum { W_MENU = 1, W_SUPPRESSED = 2, W_SHORT_LIST = 4, W_CHAT = 8, W_LOCKED = 0x10, W_COUNTER = 0x20 };
/* reasons the game keeps the stick */
enum { G_SPLIT = 1, G_NOT_ACTIVE = 2, G_OVERRIDE = 4, G_CLOSE_UP = 8, G_BOSS = 0x10, G_STATE = 0x20, G_NO_PLAYER = 0x40,
       G_STOPPED = 0x80, G_GAME_TURNS = 0x100 };

static int enabled(void) { return g_settings.right_stick == RIGHT_STICK_CAMERA; }

/* a guest function with r3, r4, called as game code would (net_native.c call_guest3): registers put back after */
static uint32_t call_game(Cpu* c, uint32_t fn, uint32_t r3, uint32_t r4) {
    uint32_t r[32], lr = c->lr, ctr = c->ctr, fpscr = c->fpscr;
    uint8_t cr[32], so = c->xer_so, ov = c->xer_ov, ca = c->xer_ca, bc = c->xer_bc;
    struct { double ps0, ps1; } f[32];
    memcpy(r, c->r, sizeof r);
    memcpy(cr, c->cr, sizeof cr);
    memcpy(f, c->f, sizeof f);
    uint32_t sp = (c->r[1] - 0x80) & ~0xFu;
    st32r(sp, c->r[1]);  /* back chain */
    c->r[1] = sp;
    c->r[3] = r3;
    c->r[4] = r4;
    guest_call(c, fn);
    uint32_t ret = c->r[3];
    memcpy(c->r, r, sizeof r);
    memcpy(c->cr, cr, sizeof cr);
    memcpy(c->f, f, sizeof f);
    c->lr = lr; c->ctr = ctr; c->fpscr = fpscr;
    c->xer_so = so; c->xer_ov = ov; c->xer_ca = ca; c->xer_bc = bc;
    return ret;
}

static int guest_ram(uint32_t ea) { return ea >= 0x80000000u && ea < 0x81800000u; }

/* the follow camera at cam may act: 0, or the G_* reason it may not (no game calls, no state changes) */
static uint32_t not_eligible(uint32_t cam) {
    if (ld32r(cam + CAM_VTABLE) != FOLLOW_VTABLE) return G_NOT_ACTIVE;
    if (ld32r(SPLIT_SELECTOR) != 1) return G_SPLIT;
    uint32_t idx = ld8r(CAM_VIEW_CONTROLLER);
    if (idx >= 13 || ld32r(CAM_REGISTRY + idx * 4) != cam) return G_NOT_ACTIVE;
    if (ld8r(CAM_OVERRIDE_FLAGS)) return G_OVERRIDE;
    if (ld32r(cam + CAM_CLOSE_UP)) return G_CLOSE_UP;
    if (ld32r(cam + CAM_FOLLOW_FL) & 8) return G_BOSS;  /* select_follow_camera_offsets: Dragon, Gal Gryphon, Gol Dragon */
    if (ld32r(cam + CAM_STATE) != 1) return G_STATE;
    uint32_t actor = ld32r(cam + CAM_ACTOR);
    if (!guest_ram(actor) || ld16r(cam + CAM_ACTOR_ID) != (ld32r(LOCAL_CLIENT_ID) & 0xFFFFu)) return G_NO_PLAYER;
    if (ld32r(actor + PL_PORT) > 3) return G_NO_PLAYER;
    /* run_task 0x8006E660: actorFlags 0x800 hands the yaw to rotate_heading_by_stick (left stick) */
    if (ld32r(actor + PL_ACTOR_FLAGS) & 0x800) return G_GAME_TURNS;
    return 0;
}

/* which windows take input now (W_* bits; 0 = none) */
static uint32_t windows_open(Cpu* c, uint32_t actor) {
    uint32_t w = 0, client = ld32r(LOCAL_CLIENT_ID);
    if (ld32r(actor + PL_STATE_FLAGS) & 0x100) w |= W_LOCKED;
    if (ld32r(actor + PL_ACTOR_FLAGS) & 0x10000) w |= W_COUNTER;
    if (call_game(c, FN_HAS_ACTIVE_MENU, client, 0) & 0xFF) w |= W_MENU;
    if (call_game(c, FN_IS_SUPPRESSED, client, 0) & 0xFF) w |= W_SUPPRESSED;
    if (call_game(c, FN_HAS_SHORT_LIST, 0, client) & 0xFF) w |= W_SHORT_LIST;
    if (call_game(c, FN_CHAT_IN_USE, 0, 0) & 0xFF) w |= W_CHAT;
    return w;
}

static void log_owner(int owner, uint32_t why) {
    if (g_lines >= LOG_LINES_MAX) return;
    g_lines++;
    if (owner == OWN_CAMERA) {
        rt_log("[camera] right stick: camera (field, follow camera; eye height %+.1f)", g_height);
    } else if (owner == OWN_MENUS) {
        rt_log("[camera] right stick: menus (window:%s%s%s%s%s%s)", why & W_MENU ? " main menu" : "",
               why & W_SUPPRESSED ? " suppressed (shop or counter)" : "", why & W_SHORT_LIST ? " short list" : "",
               why & W_CHAT ? " chat" : "", why & W_LOCKED ? " controls locked" : "", why & W_COUNTER ? " counter" : "");
    } else {
        rt_log("[camera] right stick: game (%s)", why & G_SPLIT ? "split screen" : why & G_NOT_ACTIVE ? "another camera is active"
               : why & G_OVERRIDE ? "quest camera" : why & G_CLOSE_UP ? "conversation close-up"
               : why & G_BOSS ? "boss camera offsets" : why & G_STATE ? "camera starting" : why & G_NO_PLAYER ? "no local player"
               : why & G_STOPPED ? "the follow camera stopped updating"
               : why & G_GAME_TURNS ? "the game turns the camera from the left stick" : "off");
    }
    if (g_lines == LOG_LINES_MAX) rt_log("[camera] %d right stick lines: no more", LOG_LINES_MAX);
}

static void set_owner(int owner, uint32_t why) {
    if (owner == g_owner && why == g_owner_why) return;
    /* the game keeping the stick for the same kind of reason twice is one line; a change of window is logged */
    g_owner = owner;
    g_owner_why = why;
    log_owner(owner, why);
}

/* update_follow_motion_state: the game's own, then the yaw put back unless its wall handling turned it */
void hle_cam_motion(Cpu* c) {
    uint32_t cam = c->r[3];
    if (!enabled() || not_eligible(cam)) {
        f_8006D6DC_orig(c);
        return;
    }
    uint32_t yaw = ld32r(cam + CAM_YAW), motion = ld32r(cam + CAM_MOTION), follow = ld32r(cam + CAM_FOLLOW_FL);
    /* run_task's recentre (L, quest cam_adj) stores rotation_y + 0x8000 unmasked (0x8006E614) before this runs:
     * that value, new since our last frame, resets the eye height too */
    if (yaw != g_last_yaw && yaw == ld32r(ld32r(cam + CAM_ACTOR) + PL_ROT_Y) + 0x8000u) g_height = 0.0f;
    f_8006D6DC_orig(c);
    if (!(motion == 1 && (follow & 1))) st32r(cam + CAM_YAW, yaw);
}

/* calc_follow_position: the stick turns the yaw and moves the eye height, then the game places the eye */
void hle_cam_position(Cpu* c) {
    uint32_t cam = c->r[3];
    if (!enabled()) {
        f_8006D95C_orig(c);
        return;
    }
    uint32_t why = not_eligible(cam), now = vi_field_count();
    if (why) {
        set_owner(OWN_GAME, why);
        f_8006D95C_orig(c);
        return;
    }
    uint32_t actor = ld32r(cam + CAM_ACTOR);
    uint32_t w = windows_open(c, actor);
    float dt = cam_step_seconds(g_stamp ? now - g_stamp : 2u);
    g_stamp = now ? now : 1;
    g_pad_ea = PADS + ld32r(actor + PL_PORT) * PAD_SIZE;
    if (w) {
        set_owner(OWN_MENUS, w);
    } else {
        set_owner(OWN_CAMERA, 0);
        int x = (int16_t)ld16r(g_pad_ea + PAD_CSTICK_X), y = (int16_t)ld16r(g_pad_ea + PAD_CSTICK_Y);
        st32r(cam + CAM_YAW, cam_yaw_step(ld32r(cam + CAM_YAW), x, dt));
        g_height = cam_height_step(g_height, y, dt);
    }
    /* the game rewrites eye_offset each frame (select_follow_camera_offsets): ours goes on top for this placement
     * only, and the game's value is put back after */
    uint32_t eye_y = ld32r(cam + CAM_EYE_Y);
    st32r(cam + CAM_EYE_Y, f32_as_u32(u32_as_f32(eye_y) + g_height));
    f_8006D95C_orig(c);
    st32r(cam + CAM_EYE_Y, eye_y);
    g_last_yaw = ld32r(cam + CAM_YAW);
}

/* map_c_stick_to_buttons (pad, x, y, previous): while the stick turns the camera, no direction bits; after it
 * (a window opened, the camera stopped), none until the stick is back inside the thresholds, so a held stick
 * does not step through the window that just opened */
void hle_cam_cstick(Cpu* c) {
    if (!enabled()) {
        g_latch = 0;
        g_height = 0.0f;  /* the option off resets the height */
        if (g_owner != OWN_GAME) set_owner(OWN_GAME, 0);
        f_801A6A5C_orig(c);
        return;
    }
    if (g_owner != OWN_GAME && vi_field_count() - g_stamp > STALE_FIELDS) {
        g_stamp = 0;
        set_owner(OWN_GAME, G_STOPPED);
    }
    if (c->r[3] == g_pad_ea) {
        if (g_owner == OWN_CAMERA) {
            g_latch = 1;
            return;
        }
        if (g_latch) {
            if (cam_stick_past_buttons((int16_t)c->r[4], (int16_t)c->r[5])) return;
            g_latch = 0;
        }
    }
    f_801A6A5C_orig(c);
}

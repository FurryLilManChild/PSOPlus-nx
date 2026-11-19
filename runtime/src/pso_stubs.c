/* PSOPlus-nx: stand-ins for sonic-riders-recomp files that are Riders-only and were removed here: online lockstep play (netplay.c), Riders' game mods and level probes
 * (game_mods.c, mods_data.c, game_levels.c), and the GitHub release check (update_check.c).
 * The rest of the runtime calls these; offline PSO needs none of their behaviour. */
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

/* netplay.c: never in an online session */
void netplay_init(void) {}
void netplay_connect(void) {}
int netplay_ports(void) { return 1; }
int netplay_online_save(void) { return 0; }
int netplay_in_session(void) { return 0; }
int netplay_view_player(void) { return -1; }  /* the whole picture */
void netplay_frame_start(void) {}
void netplay_latch(GcPad* pads) { (void)pads; }

/* game_mods.c / mods_data.c / game_levels.c: Riders' gear tables, unlocks and level probes */
void game_mods_apply(void) {}
void game_mods_frame(void) {}
void game_mods_linked(void) {}
typedef struct GxTexture GxTexture;
void lv_texture_decoded(GxTexture* t) { (void)t; }

/* update_check.c: no release check */
const char* update_available(void) { return NULL; }
const char* update_notes(void) { return NULL; }

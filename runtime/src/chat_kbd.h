/* Chat text from the system keyboard (chat_kbd.c): a virtual ASCII keyboard on keyboard channel 3 while online. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the GameCube keyboard's key codes the game itself tells apart (0x8025EC54, 0x8013240C) */
enum { KB_ENTER = 0x61, KB_ESC = 0x4C, KB_BACKSPACE = 0x50, KB_SPACE = 0x59, KB_LSHIFT = 0x54 };

/* one frame of the virtual keyboard: the codes held then (0 = none); a typed character is a frame with its key
 * (and shift) held, then a frame with nothing held */
typedef struct ChatStep { uint8_t shift, code; } ChatStep;

/* text (UTF-8 or ASCII) -> frames, through the game's own code -> character tables (unshifted, shifted; 0x80
 * entries each, from guest memory). Printable ASCII only: other characters are left out (*dropped counts them).
 * Ends with Enter. Returns the number of steps written (at most max). */
int chat_steps_from_text(const uint8_t lower[0x80], const uint8_t upper[0x80], const char* text, ChatStep* out,
                         int max, int* dropped);

void chat_request(void);     /* input (any thread): the chat button (right stick click) was pressed */
void chat_frame(void);       /* the game thread, once a frame (net_frame): the next frame of typing */
void chat_set_online(int on);
int chat_keyboard_open(void);  /* the system keyboard is on screen now */  /* net_native.c: the virtual keyboard is there only while the network is up */

/* platform: the system's text entry (Switch: swkbd). Blocks the calling thread (not the game or render thread).
 * 1 text entered (UTF-8 in out), 0 cancelled, -1 not available here */
int platform_text_input(const char* header, char* out, size_t n);

#ifdef __cplusplus
}
#endif

/* Chat text from the system keyboard (Switch: swkbd) through a virtual GameCube ASCII keyboard.
 *
 * PSO reads keyboards through Sega's keyboard library (0x803F0048-0x803F1E90, per channel 0x68 bytes at
 * 0x805A2720). The game's keyboard object 0x80506270 asks it from the SI sampling callback 0x801320CC:
 * 0x8013206C tries channels 0-3 through 0x80131FC8, which calls the read 0x803F1604(channel, out) (0 = a
 * keyboard: out gets the up to 4 key codes held now, 0-terminated) and the type 0x803F1548(channel) (the
 * layout: 1 = US, tables 0x8049E378 / 0x8049E3F8; 0 Japanese; 2-4 others, 0x8025EE30). Once a frame 0x80132650
 * turns the codes seen into presses (a code held now and not in the previous frame, 0x80132598) and the text
 * windows (TWindowKeyBoardHard, 0x8025FAF0) turn presses into characters with those tables; Enter 0x61,
 * backspace 0x50, Esc 0x4C and the arrows 0x5C-0x5F are commands, 0x54 / 0x55 shift (0x8025EC54, 0x8013240C).
 *
 * Here, while the network is up (from the game's network start to its stop), channel 3 (port 4) answers as a
 * US keyboard: the three library calls are replaced (config/GPOE8P/hle.txt) and run the game's own code for
 * every other channel and whenever the network is down, so offline play sees exactly what it saw before. The
 * player presses the right stick (a button the GameCube pad does not have and the default button map leaves
 * free) to open the system keyboard on a thread of its own (the game, its network and the picture keep
 * running behind it); the text is then typed one character per two frames (held, released) with the game's
 * own US table read backwards, then Enter. Printable ASCII only. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "chat_kbd.h"
#include "runtime.h"
#include "settings.h"

#define VCHAN 3
#define MAX_STEPS 512
#define TEXT_MAX 80

int chat_steps_from_text(const uint8_t lower[0x80], const uint8_t upper[0x80], const char* text, ChatStep* out,
                         int max, int* dropped) {
    int n = 0, drop = 0;
    if (max <= 0) return 0;
    out[n++] = (ChatStep){0, 0};  /* start from nothing held */
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        unsigned ch = *p;
        if (ch >= 0x80) {  /* a UTF-8 sequence: one character left out */
            if ((ch & 0xC0) != 0x80) drop++;
            continue;
        }
        uint8_t code = 0, shift = 0;
        if (ch == ' ') code = KB_SPACE;
        else if (ch > 0x20 && ch < 0x7F) {
            for (int k = 0x10; k < 0x54 && !code; k++)  /* keys, not commands or modifiers */
                if (k != KB_ESC && k != KB_BACKSPACE && lower[k] == ch) code = (uint8_t)k;
            for (int k = 0x10; k < 0x54 && !code; k++)
                if (k != KB_ESC && k != KB_BACKSPACE && upper[k] == ch) { code = (uint8_t)k; shift = 1; }
        }
        if (!code) { drop++; continue; }
        if (n + 2 > max - 2) break;
        out[n++] = (ChatStep){shift, code};
        out[n++] = (ChatStep){0, 0};
    }
    if (n + 2 <= max) {
        out[n++] = (ChatStep){0, KB_ENTER};
        out[n++] = (ChatStep){0, 0};
    }
    if (dropped) *dropped = drop;
    return n;
}

/* ---------------------------------------------------------------- state */
static _Atomic int g_online;     /* the virtual keyboard is there */
static _Atomic int g_busy;       /* the system keyboard is open or text is being typed */
static _Atomic int g_requested;
static _Atomic int g_in_kbd;    /* the system keyboard is on screen */
static mtx_t g_lock;
static cnd_t g_cv;
static once_flag g_once = ONCE_FLAG_INIT;
static ChatStep g_steps[MAX_STEPS];
static int g_nsteps, g_pos;      /* under g_lock */
static _Atomic unsigned g_held;  /* the codes reported now: shift << 8 | code */

static void init_once(void) {
    mtx_init(&g_lock, mtx_plain);
    cnd_init(&g_cv);
}

void chat_set_online(int on) {
    call_once(&g_once, init_once);
    if (atomic_exchange(&g_online, on ? 1 : 0) != (on ? 1 : 0))
        rt_log("[chat] virtual keyboard on port 4 %s%s", on ? "connected (network up)" : "removed (network down)",
               on ? ": press the right stick to type a chat line with the system keyboard" : "");
    if (!on) {
        mtx_lock(&g_lock);
        g_nsteps = g_pos = 0;
        mtx_unlock(&g_lock);
        atomic_store(&g_held, 0);
        if (!atomic_load(&g_in_kbd)) atomic_store(&g_busy, 0);
    }
}

static int chat_thread(void* arg) {
    (void)arg;
    for (;;) {
        mtx_lock(&g_lock);
        while (!atomic_load(&g_requested)) cnd_wait(&g_cv, &g_lock);
        atomic_store(&g_requested, 0);
        mtx_unlock(&g_lock);
        char text[TEXT_MAX * 4 + 4] = "";
        rt_log("[chat] system keyboard opened");
        atomic_store(&g_in_kbd, 1);
        int r = platform_text_input("Chat", text, sizeof text);
        atomic_store(&g_in_kbd, 0);
        if (r <= 0 || !text[0] || !atomic_load(&g_online)) {
            rt_log("[chat] %s", r < 0 ? "no system keyboard here" : !atomic_load(&g_online) ? "network down: nothing typed"
                                                                                           : "cancelled (nothing typed)");
            atomic_store(&g_busy, 0);
            continue;
        }
        /* the game's own US layout (the tables are the game's data, read from guest memory, never stored) */
        uint8_t lower[0x80], upper[0x80];
        mem_read_bytes(0x8049E378u, lower, sizeof lower);
        mem_read_bytes(0x8049E3F8u, upper, sizeof upper);
        static ChatStep steps[MAX_STEPS];
        int dropped = 0;
        int n = chat_steps_from_text(lower, upper, text, steps, MAX_STEPS, &dropped);
        rt_log("[chat] typing %d characters, %d left out (not ASCII or no key)", (n - 3) / 2, dropped);
        if ((n - 3) / 2 <= 0) {
            atomic_store(&g_busy, 0);
            continue;
        }
        mtx_lock(&g_lock);
        memcpy(g_steps, steps, sizeof(ChatStep) * (size_t)n);
        g_nsteps = n;
        g_pos = 0;
        mtx_unlock(&g_lock);
    }
    return 0;
}

void chat_request(void) {
    call_once(&g_once, init_once);
    if (!atomic_load(&g_online)) return;
    if (atomic_exchange(&g_busy, 1)) return;  /* open already, or still typing */
    static int started;
    mtx_lock(&g_lock);
    if (!started) {
        started = 1;
        rt_thread_start(chat_thread, NULL);
    }
    atomic_store(&g_requested, 1);
    cnd_signal(&g_cv);
    mtx_unlock(&g_lock);
}

void chat_frame(void) {
    if (!atomic_load(&g_online) || !atomic_load(&g_busy)) return;
    mtx_lock(&g_lock);
    if (g_pos < g_nsteps) {
        ChatStep s = g_steps[g_pos++];
        atomic_store(&g_held, (unsigned)s.shift << 8 | s.code);
        if (g_pos == g_nsteps) {
            g_nsteps = g_pos = 0;
            atomic_store(&g_busy, 0);
            rt_log("[chat] line typed (Enter sent)");
        }
    }
    mtx_unlock(&g_lock);
}

#if !defined(__SWITCH__)
/* no system keyboard here; SR_CHAT_TEXT (testing) stands in for one */
int platform_text_input(const char* header, char* out, size_t n) {
    (void)header;
    const char* t = getenv("SR_CHAT_TEXT");
    if (!t) return -1;
    const char* d = getenv("SR_CHAT_DELAY_MS");  /* testing: the keyboard stays open this long */
    if (d && atoi(d) > 0) platform_sleep_ns((uint64_t)atoi(d) * 1000000ull);
    snprintf(out, n, "%s", t);
    return 1;
}
#endif

/* ---------------------------------------------------------------- the keyboard library, channel 3 */
void f_803F1548_orig(Cpu* c);
void f_803F1604_orig(Cpu* c);
void f_803F0E90_orig(Cpu* c);

int chat_keyboard_open(void) { return atomic_load(&g_in_kbd); }

static int virtual_chan(Cpu* c) { return atomic_load(&g_online) && (int32_t)c->r[3] == VCHAN; }

/* 0x803F1548(channel): the keyboard's layout */
void hle_kbd_type(Cpu* c) {
    if (!virtual_chan(c)) { f_803F1548_orig(c); return; }
    c->r[3] = 1;  /* US */
}

/* 0x803F1604(channel, out): 0 and the codes held now (out may be 0) */
void hle_kbd_read(Cpu* c) {
    if (!virtual_chan(c)) { f_803F1604_orig(c); return; }
    unsigned h = atomic_load(&g_held);
    uint8_t codes[4] = {0, 0, 0, 0};
    if (h & 0xFF) {
        int k = 0;
        if (h >> 8) codes[k++] = KB_LSHIFT;
        codes[k] = (uint8_t)h;
    }
    uint32_t out = c->r[4];
    if (out && guest_span_ok(out, 4)) mem_write_bytes(out, codes, 4);
    c->r[3] = 0;
}

/* 0x803F0E90(channel, callback): starts the keyboard detection on a channel; nothing to start for ours */
void hle_kbd_detect(Cpu* c) {
    if (!virtual_chan(c)) { f_803F0E90_orig(c); return; }
    c->r[3] = 0;
}

/* The opening movies' picture (psogc_j.sfd, psogc_j2.sfd: the attract movie and the new-game intro).
 *
 * PSO plays those in "XFB mode" (the movie object's +0xCC set, from the callers at 0x802A13B4 and
 * 0x802A1410): each frame, 0x80143C00 asks movie.rel for the decoded frame's planes (Y, Cb, Cr: 4:2:0)
 * and 0x80143988 writes them with the CPU straight into the external frame buffer as YUYV, then
 * VISetNextFrameBuffer shows that buffer. No GX draw and no EFB copy happens, and this runtime shows a
 * picture only at an EFB-to-XFB copy, so the screen kept its last picture (black) while the sound played.
 * The ending movies use the other mode (an RGBA8 texture drawn with GX) and are not affected.
 *
 * Here 0x80143988 is replaced (config/GPOE8P/hle.txt, movie_xfb): the planes are converted to RGBA
 * (BT.601, video range, as the TV shows the XFB) into a 640x480 picture, centred as the game centres
 * it, and handed to the GP thread in order with the GX stream (gx_fifo.c gx_host_picture), which shows
 * it as that frame's picture (gx_render.c gx_picture_present). The XFB in guest RAM is not written:
 * nothing reads it back. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define MOVIE_W_ADDR 0x805D56A4u  /* the frame's width and height (0x80143C94, from movie.rel) */
#define MOVIE_H_ADDR 0x805D56A8u
#define PIC_W 640
#define PIC_H 480
#define NSLOTS 3

static uint8_t* g_pic[NSLOTS];
static atomic_int g_busy[NSLOTS];  /* handed to the GP thread, not shown yet */
static int g_next;
static unsigned g_frames, g_dropped;

void gx_host_picture(int slot);  /* gx_fifo.c */

/* GP thread: the picture in `slot` (PIC_W x PIC_H RGBA) for gx_picture_present, then free again */
const uint8_t* movie_picture(int slot, uint32_t* w, uint32_t* h) {
    *w = PIC_W; *h = PIC_H;
    return slot >= 0 && slot < NSLOTS ? g_pic[slot] : NULL;
}
void movie_picture_done(int slot) { if (slot >= 0 && slot < NSLOTS) atomic_store(&g_busy[slot], 0); }

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void hle_movie_xfb(Cpu* c) {
    uint32_t s = c->r[3];
    uint32_t ya = ld32(s + 0x00), ua = ld32(s + 0x04), va = ld32(s + 0x08);
    uint32_t ys = ld32(s + 0x0C), cs = ld32(s + 0x10);
    uint32_t w = ld32(MOVIE_W_ADDR), h = ld32(MOVIE_H_ADDR);
    if (w > PIC_W) w = PIC_W;
    if (h > PIC_H) h = PIC_H;
    w &= ~1u; h &= ~1u;
    uint64_t yspan = (uint64_t)(h - 1) * ys + w, cspan = (uint64_t)(h / 2 - 1) * cs + w / 2;
    if (!w || !h || yspan > GUEST_RAM_SIZE || cspan > GUEST_RAM_SIZE || !guest_span_ok(ya, (uint32_t)yspan) ||
        !guest_span_ok(ua, (uint32_t)cspan) || !guest_span_ok(va, (uint32_t)cspan)) {
        static int warned;
        if (!warned++)
            rt_log("[movie] frame planes outside game memory (Y %08X Cb %08X Cr %08X, strides %u %u, %ux%u): not shown",
                   ya, ua, va, ys, cs, w, h);
        return;
    }
    int slot = g_next;
    if (atomic_load(&g_busy[slot])) {  /* the GP thread is still two pictures behind: skip this one */
        if (g_dropped++ < 5) rt_log("[movie] picture dropped (the GP thread is behind)");
        return;
    }
    g_next = (g_next + 1) % NSLOTS;
    if (!g_pic[slot]) g_pic[slot] = malloc((size_t)PIC_W * PIC_H * 4);
    if (!g_pic[slot]) {
        static int warned;
        if (!warned++) rt_log("[movie] out of memory for the movie picture: not shown");
        return;
    }
    uint8_t* out = g_pic[slot];
    memset(out, 0, (size_t)PIC_W * PIC_H * 4);
    for (size_t i = 3; i < (size_t)PIC_W * PIC_H * 4; i += 4) out[i] = 255;
    uint32_t x0 = ((PIC_W - w) / 2) & ~1u, y0 = (PIC_H - h) / 2;
    if (!g_frames++)
        rt_log("[movie] XFB-mode movie: %ux%u frame, Y %08X (stride %u), Cb %08X, Cr %08X (stride %u); shown natively",
               w, h, ya, ys, ua, va, cs);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* yr = (const uint8_t*)ppc_ptr(ya + y * ys);
        const uint8_t* ur = (const uint8_t*)ppc_ptr(ua + (y >> 1) * cs);
        const uint8_t* vr = (const uint8_t*)ppc_ptr(va + (y >> 1) * cs);
        uint8_t* o = out + ((size_t)(y0 + y) * PIC_W + x0) * 4;
        for (uint32_t x = 0; x < w; x += 2) {
            int cb = ur[x >> 1] - 128, cr = vr[x >> 1] - 128;
            int rr = 104597 * cr, gg = -25675 * cb - 53279 * cr, bb = 132201 * cb;  /* 16.16 fixed point */
            for (int k = 0; k < 2; k++) {
                int l = 76309 * (yr[x + k] - 16) + 32768;
                o[0] = clamp8((l + rr) >> 16);
                o[1] = clamp8((l + gg) >> 16);
                o[2] = clamp8((l + bb) >> 16);
                o += 4;
            }
        }
    }
    atomic_store(&g_busy[slot], 1);
    gx_host_picture(slot);
}

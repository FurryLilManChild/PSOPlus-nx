/* First launch with a Dolphin RVZ (or WIA) and no ISO: convert it once to sdmc:/switch/psoplus/<name>.iso,
 * behind a full-screen progress screen, then boot from the ISO as usual. Also the screens for "no disc
 * image" and for problems found before the game starts (wrong disc, not enough space), which otherwise
 * ended the program with only a log line.
 *
 * The screen is libnx's text console on the default window, used before the renderer exists and closed
 * (consoleExit) before render_deko.c makes its swapchain on the same window.
 *
 * Safety: the output is written to <name>.iso.part and renamed to <name>.iso only after the size, the disc
 * header (magic, GPOE8P, revision 2) and the SHA-1 were checked. A .part left by an interrupted conversion
 * (power off, crash) is deleted at the next start and the conversion starts over; closing the game from
 * HOME during the conversion deletes it at once. The player's .rvz is only read, never changed. */
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "runtime.h"
#include "rvz_decode.h"

#define BASE_DIR "sdmc:/switch/psoplus"
#define DISC_SIZE 1459978240ull
/* SHA-1 of the clean 1:1 dump of GPOE8P revision 2 (the same value as setup/build_pc.ps1) */
static const char k_disc_sha1[] = "0ff155311f4cc57951c03baeb1a8cc15de5fe1cd";

/* ---------------------------------------------------------------- the text screen (80 x 45 cells) */
static int g_con;
static PadState g_cpad;

static void screen_open(void) {
    if (g_con) return;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_cpad);
    g_con = 1;
}
static void show(void) {
    fflush(stdout);
    consoleUpdate(NULL);
}
static void disc_screen_close(void) {
    if (!g_con) return;
    consoleExit(NULL);
    g_con = 0;
}
/* centred text from `row`; longer than 76 columns: wrapped at spaces onto the next rows. Returns the rows used */
static int at(int row, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static int at(int row, const char* fmt, ...) {
    char text[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    const char* p = text;
    int rows = 0;
    do {
        int len = (int)strlen(p);
        if (len > 76) {
            int cut = 76;
            while (cut > 30 && p[cut] != ' ') cut--;
            if (p[cut] != ' ') cut = 76;
            len = cut;
        }
        printf("\x1b[%d;1H\x1b[2K\x1b[%d;%dH%.*s", row + rows, row + rows, 1 + (80 - len) / 2, len, p);
        p += len;
        while (*p == ' ') p++;
        rows++;
    } while (*p && rows < 4);
    return rows;
}
static void title(void) {
    printf("\x1b[2J\x1b[0m");
    printf("\x1b[36;1m");
    at(6, "Phantasy Star Online Episode I & II Plus");
    printf("\x1b[0m");
    at(7, "native Switch port");
}

/* a message until + (or closing from HOME); then the program ends */
static _Noreturn void screen_fail(const char* head, const char* l1, const char* l2, const char* l3) {
    rt_log("[disc] %s: %s %s %s", head, l1, l2 ? l2 : "", l3 ? l3 : "");
    fflush(stderr);
    screen_open();
    title();
    printf("\x1b[31;1m");
    at(14, "%s", head);
    printf("\x1b[0m");
    int row = 17;
    row += at(row, "%s", l1);
    if (l2) row += at(row, "%s", l2);
    if (l3) row += at(row, "%s", l3);
    at(row + 2, "Folder: sdmc:/switch/psoplus/");
    at(30, "Press + to close.");
    show();
    while (appletMainLoop()) {
        padUpdate(&g_cpad);
        if (padGetButtonsDown(&g_cpad) & HidNpadButton_Plus) break;
        svcSleepThread(50000000);
    }
    disc_screen_close();
    exit(0);
}

static void fmt_time(char* out, size_t n, double s) {
    int t = (int)(s + 0.5);
    if (t >= 60) snprintf(out, n, "%d min %02d s", t / 60, t % 60);
    else snprintf(out, n, "%d s", t);
}

/* ---------------------------------------------------------------- writing: a thread writes one buffer while the next fills */
#define WBUF (8u << 20)
typedef struct {
    int fd;
    uint8_t* buf[2];
    size_t fill;    /* bytes in buf[cur] */
    int cur;
    pthread_t th;
    pthread_mutex_t mx;
    pthread_cond_t cv;
    size_t pending; /* bytes of buf[cur ^ 1] the thread is to write (0: idle) */
    int quit, werr;
    RvzSha1 sha;
    uint64_t done, total;
    u64 t0, last_draw, last_log;
    const char *src, *dst;
    int stop;       /* closed from HOME */
} Conv;

static void* writer(void* p) {
    Conv* c = p;
    rt_thread_pin(1);  /* its own core: on the main thread's core the main thread's higher priority would starve it */
    pthread_mutex_lock(&c->mx);
    for (;;) {
        while (!c->pending && !c->quit) pthread_cond_wait(&c->cv, &c->mx);
        if (!c->pending && c->quit) break;
        size_t n = c->pending;
        const uint8_t* b = c->buf[c->cur ^ 1];
        pthread_mutex_unlock(&c->mx);
        int ok = 1;
        for (size_t o = 0; o < n && ok;) {  /* write() straight to the SD card, the whole buffer at once */
            ssize_t k = write(c->fd, b + o, n - o);
            if (k <= 0) ok = 0;
            else o += (size_t)k;
        }
        pthread_mutex_lock(&c->mx);
        if (!ok) c->werr = 1;
        c->pending = 0;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mx);
    return NULL;
}

/* hand buf[cur] to the writer (after it finished the previous one) and fill the other */
static int flush_buf(Conv* c) {
    pthread_mutex_lock(&c->mx);
    while (c->pending) pthread_cond_wait(&c->cv, &c->mx);
    int err = c->werr;
    if (!err && c->fill) {
        c->pending = c->fill;
        c->cur ^= 1;
        c->fill = 0;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mx);
    return !err;
}

static void draw_progress(Conv* c) {
    double el = (double)armTicksToNs(armGetSystemTick() - c->t0) / 1e9;
    int pct = (int)(c->done * 100 / c->total);
    char left[48] = "estimating the time left";
    if (c->done > c->total / 50 && el > 3) {
        char t[32];
        fmt_time(t, sizeof t, el * (double)(c->total - c->done) / (double)c->done);
        snprintf(left, sizeof left, "about %s left", t);
    }
    static int drawn;
    if (!drawn++) title();  /* the rest redraws its own lines */
    printf("\x1b[33;1m");
    at(12, "Converting your disc image once; later starts skip this.");
    printf("\x1b[0m");
    const char* s = strrchr(c->src, '/');
    const char* d = strrchr(c->dst, '/');
    at(14, "%s  ->  %s", s ? s + 1 : c->src, d ? d + 1 : c->dst);
    /* the bar: 60 cells, # done (green), . to do */
    char bar[61];
    int w = 60, full = (int)(c->done * (uint64_t)w / c->total);
    memset(bar, '#', (size_t)full);
    memset(bar + full, '.', (size_t)(w - full));
    bar[w] = 0;
    printf("\x1b[18;10H[\x1b[32;1m%.*s\x1b[0m%s]", full, bar, bar + full);
    at(21, "%d%%    %llu of %llu MB    %s", pct, (unsigned long long)(c->done >> 20),
       (unsigned long long)(c->total >> 20), left);
    at(26, "Please keep the console on. This takes a few minutes.");
    at(27, "Closing the game now stops the conversion; it starts over next time.");
    at(30, "Your .rvz is kept; once the game starts you can delete it to free space.");
    show();
}

static int sink(void* user, const uint8_t* d, size_t n) {
    Conv* c = user;
    rvz_sha1_update(&c->sha, d, n);
    while (n) {
        size_t k = WBUF - c->fill < n ? WBUF - c->fill : n;
        memcpy(c->buf[c->cur] + c->fill, d, k);
        c->fill += k;
        d += k;
        n -= k;
        c->done += k;
        if (c->fill == WBUF && !flush_buf(c)) return 0;
    }
    u64 now = armGetSystemTick();
    if (armTicksToNs(now - c->last_draw) >= 250000000ull) {  /* 4 times a second: the screen and HOME */
        c->last_draw = now;
        if (!appletMainLoop()) {
            c->stop = 1;
            return 0;
        }
        draw_progress(c);
    }
    if (armTicksToNs(now - c->last_log) >= 10000000000ull) {
        c->last_log = now;
        rt_log("[disc] converting: %llu of %llu MB", (unsigned long long)(c->done >> 20), (unsigned long long)(c->total >> 20));
    }
    return 1;
}

static int is_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

/* the disc header's identity: "" when it is GPOE8P revision 2, else what it is (for the message) */
static const char* disc_identity(const uint8_t* h, char* why, size_t n) {
    uint32_t magic = (uint32_t)h[0x1C] << 24 | h[0x1D] << 16 | h[0x1E] << 8 | h[0x1F];
    if (magic != 0xC2339F3Du) snprintf(why, n, "it is not a GameCube disc");
    else if (memcmp(h, "GPOE8P", 6) || h[7] != 2) snprintf(why, n, "it is %.6s revision %d, not GPOE8P revision 2", (const char*)h, h[7]);
    else why[0] = 0;
    return why;
}

/* convert src (.rvz/.wia) to dst (.iso); returns only on success (else a message screen, then exit) */
static void convert(const char* src, const char* dst, RvzImage* img) {
    char part[600], err[256], line[160];
    snprintf(part, sizeof part, "%s.part", dst);
    uint64_t total = rvz_iso_size(img);

    s64 free_b = -1;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (fs) fsFsGetFreeSpace(fs, "/", &free_b);
    rt_log("[disc] %s (%s): %llu bytes; SD card free space %lld bytes", src, rvz_describe(img), (unsigned long long)total,
           (long long)free_b);
    if (free_b >= 0 && (uint64_t)free_b < total + (1u << 20)) {
        snprintf(line, sizeof line, "It needs %llu MB free on the SD card; %lld MB are free.",
                 (unsigned long long)((total >> 20) + 1), (long long)(free_b >> 20));
        rvz_close(img);
        screen_fail("Not enough space to convert the disc image", line,
                    "Free some space, or copy an .iso made on a PC instead of the .rvz (Dolphin: right-click the "
                    "game, Convert File, format ISO).", NULL);
    }

    Conv c = {0};
    c.total = total;
    c.src = src;
    c.dst = dst;
    c.buf[0] = malloc(WBUF);
    c.buf[1] = malloc(WBUF);
    c.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (!c.buf[0] || !c.buf[1] || c.fd < 0) {
        rvz_close(img);
        screen_fail("Cannot write the converted disc image", c.fd >= 0 ? "Out of memory." : "The file could not be created:",
                    c.fd >= 0 ? NULL : part + strlen(BASE_DIR "/"), "Check that the SD card is not write-protected or full.");
    }
    pthread_mutex_init(&c.mx, NULL);
    pthread_cond_init(&c.cv, NULL);
    pthread_create(&c.th, NULL, writer, &c);
    rvz_sha1_init(&c.sha);
    appletSetAutoSleepDisabled(true);
    screen_open();
    c.t0 = c.last_log = armGetSystemTick();
    c.last_draw = 0;
    draw_progress(&c);
    rt_log("[disc] converting %s to %s", src, dst);

    int ok = rvz_extract(img, sink, &c, err, sizeof err);
    if (ok && !flush_buf(&c)) ok = 0, snprintf(err, sizeof err, "writing to the SD card failed (full or removed?)");
    pthread_mutex_lock(&c.mx);
    c.quit = 1;
    pthread_cond_broadcast(&c.cv);
    pthread_mutex_unlock(&c.mx);
    pthread_join(c.th, NULL);
    if (c.werr) ok = 0, snprintf(err, sizeof err, "writing to the SD card failed (full or removed?)");
    if (!ok && !err[0]) snprintf(err, sizeof err, "writing to the SD card failed (full or removed?)");
    if (close(c.fd) && ok) ok = 0, snprintf(err, sizeof err, "closing the file failed (SD card full?)");
    free(c.buf[0]);
    free(c.buf[1]);
    uint8_t h[20];
    rvz_sha1_final(&c.sha, h);
    rvz_close(img);
    appletSetAutoSleepDisabled(false);
    double el = (double)armTicksToNs(armGetSystemTick() - c.t0) / 1e9;
    if (c.stop) {
        remove(part);
        rt_log("[disc] conversion stopped (closed from HOME) after %.0f s at %llu MB; %s deleted", el,
               (unsigned long long)(c.done >> 20), part);
        fflush(stderr);
        disc_screen_close();
        exit(0);
    }
    if (!ok) {
        remove(part);
        rt_log("[disc] conversion failed after %.0f s at %llu MB: %s", el, (unsigned long long)(c.done >> 20), err);
        screen_fail("The disc image could not be converted", err,
                    "The .rvz may be damaged or incomplete. Copy it again, or convert it to ISO on a PC (Dolphin: "
                    "right-click the game, Convert File, format ISO).", NULL);
    }

    /* checks before the file gets its .iso name */
    char sha_hex[41];
    for (int i = 0; i < 20; i++) snprintf(sha_hex + i * 2, 3, "%02x", h[i]);
    struct stat st;
    uint8_t head[0x440];
    FILE* f = fopen(part, "rb");
    size_t got = f ? fread(head, 1, sizeof head, f) : 0;
    if (f) fclose(f);
    char why[96];
    if (stat(part, &st) || (uint64_t)st.st_size != total || got != sizeof head || disc_identity(head, why, sizeof why)[0]) {
        rt_log("[disc] the converted file fails its check (size %lld, expected %llu; header read %zu bytes)",
               (long long)(stat(part, &st) ? -1 : st.st_size), (unsigned long long)total, got);
        remove(part);
        screen_fail("The converted disc image failed its check", "Its size or disc header is not as expected.",
                    "The SD card may be failing or full. Please try again, or convert to ISO on a PC and copy the .iso "
                    "instead.", NULL);
    }
    int clean = !strcmp(sha_hex, k_disc_sha1);
    char t[32];
    fmt_time(t, sizeof t, el);
    rt_log("[disc] converted in %s (%.1f MB/s): %llu bytes, SHA-1 %s (%s)", t, el > 0 ? (double)total / 1048576.0 / el : 0.0,
           (unsigned long long)total, sha_hex, clean ? "matches the clean dump" : "differs from the clean dump 0ff15531...");

    /* FAT cannot rename onto an existing file. No .iso was there when the folder was read (it would have been
     * used instead of converting), so a <name>.iso now can only be one copied in during the conversion: ours,
     * just checked, replaces it */
    struct stat old;
    if (!stat(dst, &old)) {
        rt_log("[disc] %s already exists (%lld bytes): replaced", dst, (long long)old.st_size);
        remove(dst);
    }
    if (rename(part, dst)) {
        rt_log("[disc] renaming %s to %s failed", part, dst);
        screen_fail("Cannot rename the converted disc image", "The converted file is kept as:", part + strlen(BASE_DIR "/"),
                    "Rename it on a PC so the name ends in .iso (remove .part); otherwise the next start converts again.");
    }
    fflush(stderr);

    title();
    printf("\x1b[32;1m");
    at(14, "Done in %s. The game starts now.", t);
    printf("\x1b[0m");
    const char* s = strrchr(src, '/');
    at(17, "Later starts use %s directly.", strrchr(dst, '/') + 1);
    at(18, "You can delete %s to free space.", s ? s + 1 : src);
    if (!clean) {
        printf("\x1b[33;1m");
        at(22, "Note: this image differs from the known clean dump of this disc.");
        printf("\x1b[0m");
        at(23, "It may be a scrubbed or modified dump. It may still play; if the game");
        at(24, "misbehaves, use a full 1:1 dump of your disc.");
    }
    show();
    for (int i = 0; i < (clean ? 30 : 100) && appletMainLoop(); i++) svcSleepThread(100000000);  /* 3 s (10 s with the note) */
    disc_screen_close();
}

/* platform_disc_path's search: the first .iso/.gcm; else convert the first GPOE8P .rvz/.wia; else a message */
void disc_find_or_convert(char* path, size_t n) {
    char rvz[2][512] = {{0}};
    int nrvz = 0;
    path[0] = 0;
    DIR* d = opendir(BASE_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            const char* nm = e->d_name;
            if (is_ext(nm, ".iso.part")) {  /* an interrupted conversion (power off, crash): start over */
                char p[600];
                snprintf(p, sizeof p, BASE_DIR "/%s", nm);
                rt_log("[disc] %s is left from an interrupted conversion: deleted", p);
                remove(p);
            } else if (!path[0] && (is_ext(nm, ".iso") || is_ext(nm, ".gcm"))) {
                snprintf(path, n, BASE_DIR "/%s", nm);
            } else if (nrvz < 2 && (is_ext(nm, ".rvz") || is_ext(nm, ".wia"))) {
                snprintf(rvz[nrvz++], sizeof rvz[0], BASE_DIR "/%s", nm);
            }
        }
        closedir(d);
    }
    if (path[0]) return;
    if (!nrvz) {
        snprintf(path, n, BASE_DIR "/game.iso");
        screen_fail("No disc image found", "Copy your own PSO Episode I & II Plus disc image (USA, GPOE8P revision 2) "
                    "into this folder, as an .iso or .gcm (about 1.4 GB) or as a Dolphin .rvz (converted to .iso once, "
                    "at the first start).", NULL, NULL);
    }
    char err[256], why[96], line[400];
    for (int i = 0; i < nrvz; i++) {
        RvzImage* img = rvz_open(rvz[i], err, sizeof err);
        const char* nm = strrchr(rvz[i], '/') + 1;
        if (!img) {
            rt_log("[disc] %s: %s", rvz[i], err);
            if (i + 1 < nrvz) continue;
            snprintf(line, sizeof line, "%s: %s", nm, err);
            screen_fail("The disc image cannot be read", line, "Copy it again, or convert it to ISO on a PC (Dolphin: "
                        "right-click the game, Convert File, format ISO).", NULL);
        }
        if (disc_identity(rvz_disc_header(img), why, sizeof why)[0] || rvz_iso_size(img) != DISC_SIZE) {
            if (!why[0]) snprintf(why, sizeof why, "it holds %llu bytes, a full disc %llu (trimmed or NKit?)",
                                  (unsigned long long)rvz_iso_size(img), DISC_SIZE);
            rt_log("[disc] %s: %s", rvz[i], why);
            rvz_close(img);
            if (i + 1 < nrvz) continue;
            snprintf(line, sizeof line, "%s: %s.", nm, why);
            screen_fail("This disc image is not the one the port needs", line,
                        "The port needs PSO Episode I & II Plus, USA, revision 2 (GPOE8P), as a full 1:1 image.", NULL);
        }
        /* <name>.rvz -> <name>.iso */
        snprintf(path, n, "%s", rvz[i]);
        char* dot = strrchr(path, '.');
        snprintf(dot, n - (size_t)(dot - path), ".iso");
        convert(rvz[i], path, img);
        return;
    }
}

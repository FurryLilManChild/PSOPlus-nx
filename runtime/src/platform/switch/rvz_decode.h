/* RVZ and WIA (Dolphin's compressed GameCube disc formats) back to a plain 1:1 disc image.
 * A C port of tools/disc/rvz2iso.py (from sonic-riders-recomp, MPL-2.0); format reference: Dolphin's docs/WiaAndRvz.md.
 * Portable C (libnx only for the hardware SHA-1): the Switch's first-launch conversion uses it (disc_convert_switch.c), and
 * tools/tests/rvz_decode_test.c checks it on a PC. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct RvzImage RvzImage;

/* opens and checks the headers (magic, the header SHA-1s, GameCube only) and reads the entry tables.
 * NULL on failure, with the reason in err */
RvzImage* rvz_open(const char* path, char* err, size_t errn);
void rvz_close(RvzImage* img);

uint64_t rvz_iso_size(const RvzImage* img);       /* the disc image's size, from header 1 */
const uint8_t* rvz_disc_header(const RvzImage* img); /* the disc's first 0x80 bytes (game ID, revision) */
const char* rvz_describe(const RvzImage* img);     /* "RVZ, zstd, 128 KiB blocks" */

/* the whole disc image in order, in pieces; sink returns 0 to stop (rvz_extract then returns 0 with
 * err "stopped"). 1 when complete and the size matched */
typedef int (*RvzSink)(void* user, const uint8_t* data, size_t n);
int rvz_extract(RvzImage* img, RvzSink sink, void* user, char* err, size_t errn);

/* 1 when the file starts with RVZ's or WIA's magic */
int rvz_is_image(const char* path);

/* SHA-1 (libnx's ARMv8 crypto version on the Switch, a plain C one elsewhere) */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t used; void* hw; } RvzSha1;
void rvz_sha1_init(RvzSha1* s);
void rvz_sha1_update(RvzSha1* s, const void* data, size_t n);
void rvz_sha1_final(RvzSha1* s, uint8_t out[20]);

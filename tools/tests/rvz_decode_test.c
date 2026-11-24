/* Host check of the Switch's RVZ/WIA decoder (runtime/src/platform/switch/rvz_decode.c):
 *     rvz_decode_test IMAGE.rvz OUT.iso
 * writes the disc image and prints its size, SHA-1 and the format. tools/tests/run_rvz_decode_test.sh
 * compares the result byte for byte with the source image and with tools/disc/rvz2iso.py. */
#include <stdio.h>
#include <string.h>

#include "rvz_decode.h"

typedef struct { FILE* f; RvzSha1 sha; unsigned long long n; } Out;

static int sink(void* user, const uint8_t* d, size_t n) {
    Out* o = user;
    rvz_sha1_update(&o->sha, d, n);
    o->n += n;
    return fwrite(d, 1, n, o->f) == n;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s IMAGE.rvz OUT.iso\n", argv[0]);
        return 2;
    }
    char err[256];
    RvzImage* img = rvz_open(argv[1], err, sizeof err);
    if (!img) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    Out o = {0};
    o.f = fopen(argv[2], "wb");
    if (!o.f) {
        fprintf(stderr, "error: cannot write %s\n", argv[2]);
        return 1;
    }
    rvz_sha1_init(&o.sha);
    int ok = rvz_extract(img, sink, &o, err, sizeof err);
    fclose(o.f);
    uint8_t h[20];
    rvz_sha1_final(&o.sha, h);
    if (!ok) {
        fprintf(stderr, "error: %s\n", err);
        rvz_close(img);
        return 1;
    }
    printf("%s: %.6s rev %d, %s, %llu bytes, SHA-1 ", argv[1], (const char*)rvz_disc_header(img), rvz_disc_header(img)[7],
           rvz_describe(img), o.n);
    for (int i = 0; i < 20; i++) printf("%02x", h[i]);
    printf("\n");
    rvz_close(img);
    return 0;
}

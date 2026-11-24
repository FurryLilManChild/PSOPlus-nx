/* RVZ and WIA (Dolphin's compressed disc formats) back to a plain GameCube disc image: a C port of
 * tools/disc/rvz2iso.py (from sonic-riders-recomp, MPL-2.0). Format reference: Dolphin's docs/WiaAndRvz.md.
 *
 * Handled, as in the Python tool: compression none, purge (WIA), bzip2, LZMA, LZMA2 and Zstandard (RVZ's
 * default), any block size that is a multiple of 32 KiB, RVZ's packed groups with junk data (regenerated
 * with the disc's lagged Fibonacci generator, as Dolphin does), groups shared by several places (Dolphin
 * stores identical groups once) and all-zero groups. Wii images are refused (they need partition crypto).
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL
 * was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 * Libraries: zstd (BSD), liblzma (0BSD / public domain), bzip2 (BSD-style), from devkitPro's portlibs. */
#define _FILE_OFFSET_BITS 64
#include "rvz_decode.h"

#include <bzlib.h>
#include <fcntl.h>
#include <lzma.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define BLOCK 0x8000u /* junk data restarts every 32 KiB of the disc */
enum { C_NONE, C_PURGE, C_BZIP2, C_LZMA, C_LZMA2, C_ZSTD };
static const char* const k_comp_names[] = {"none", "purge", "bzip2", "LZMA", "LZMA2", "zstd"};

typedef struct { uint64_t off, size; uint32_t group, ngroups; } RawEntry;

#define RWIN (4u << 20) /* reads go through a 4 MiB window: groups are small and mostly in order */

struct RvzImage {
    int fd;
    uint8_t* win;
    uint64_t win_off;
    size_t win_len;
    uint64_t fsize, iso_size;
    int rvz;
    uint32_t ctype, chunk;
    uint8_t cdata[7], cdata_n;
    uint8_t dhead[0x80];
    RawEntry* raws;
    uint32_t nraw;
    uint8_t* grp;  /* group entries, big-endian as stored */
    uint32_t ngrp;
    ZSTD_DCtx* zd;
    uint8_t *in, *mid, *out; /* compressed group, decompressed (packed) group, final group */
    size_t in_cap, mid_cap;
    char desc[64];
    char* err;
    size_t errn;
};

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int fail(char* err, size_t n, const char* fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* ---------------------------------------------------------------- SHA-1 */
#ifdef __SWITCH__
void rvz_sha1_init(RvzSha1* s) {
    s->hw = malloc(sizeof(Sha1Context));
    sha1ContextCreate((Sha1Context*)s->hw);
}
void rvz_sha1_update(RvzSha1* s, const void* d, size_t n) { sha1ContextUpdate((Sha1Context*)s->hw, d, n); }
void rvz_sha1_final(RvzSha1* s, uint8_t out[20]) {
    sha1ContextGetHash((Sha1Context*)s->hw, out);
    free(s->hw);
    s->hw = NULL;
}
#else
static uint32_t rol(uint32_t x, int k) { return x << k | x >> (32 - k); }
static void sha1_block(RvzSha1* s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
void rvz_sha1_init(RvzSha1* s) {
    static const uint32_t h0[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, h0, sizeof h0);
    s->len = 0;
    s->used = 0;
    s->hw = NULL;
}
void rvz_sha1_update(RvzSha1* s, const void* data, size_t n) {
    const uint8_t* p = data;
    s->len += n;
    while (n) {
        if (!s->used && n >= 64) {
            sha1_block(s, p);
            p += 64;
            n -= 64;
            continue;
        }
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, k);
        s->used += (uint32_t)k;
        p += k;
        n -= k;
        if (s->used == 64) {
            sha1_block(s, s->buf);
            s->used = 0;
        }
    }
}
void rvz_sha1_final(RvzSha1* s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = {0x80};
    size_t padn = (s->used < 56 ? 56 : 120) - s->used;
    for (int i = 0; i < 8; i++) pad[padn + i] = (uint8_t)(bits >> (56 - 8 * i));
    rvz_sha1_update(s, pad, padn + 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}
#endif

static void sha1_of(const void* d, size_t n, uint8_t out[20]) {
    RvzSha1 s;
    rvz_sha1_init(&s);
    rvz_sha1_update(&s, d, n);
    rvz_sha1_final(&s, out);
}

/* ---------------------------------------------------------------- junk data (the disc's lagged Fibonacci generator) */
#define LFG_K 521
#define LFG_J 32
#define LFG_BYTES (LFG_K * 4)
typedef struct { uint32_t w[LFG_K]; uint32_t pos; } Junk;

static void junk_forward(Junk* j) {
    for (int i = 0; i < LFG_J; i++) j->w[i] ^= j->w[i + LFG_K - LFG_J];
    for (int i = LFG_J; i < LFG_K; i++) j->w[i] ^= j->w[i - LFG_J];
}
static void junk_init(Junk* j, const uint8_t* seed) {
    for (int i = 0; i < 17; i++) j->w[i] = be32(seed + i * 4);
    for (int i = 17; i < LFG_K; i++) j->w[i] = (j->w[i - 17] << 23) ^ (j->w[i - 16] >> 9) ^ j->w[i - 1];
    for (int i = 0; i < LFG_K; i++) j->w[i] = (j->w[i] & 0xFF00FFFFu) | ((j->w[i] >> 2) & 0x00FF0000u);
    for (int i = 0; i < 4; i++) junk_forward(j);
    j->pos = 0;
}
static void junk_skip(Junk* j, uint32_t n) {
    j->pos += n;
    while (j->pos >= LFG_BYTES) {
        junk_forward(j);
        j->pos -= LFG_BYTES;
    }
}
static void junk_take(Junk* j, uint8_t* out, size_t n) {
    while (n) {
        uint32_t p = j->pos;
        if (!(p & 3) && n >= 4) { /* whole words */
            size_t words = (LFG_BYTES - p) / 4;
            if (words > n / 4) words = n / 4;
            for (size_t i = 0; i < words; i++) {
                uint32_t v = j->w[p / 4 + i];
                out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16); out[2] = (uint8_t)(v >> 8); out[3] = (uint8_t)v;
                out += 4;
            }
            j->pos += (uint32_t)words * 4;
            n -= words * 4;
        } else {
            *out++ = (uint8_t)(j->w[p / 4] >> (24 - 8 * (p & 3)));
            j->pos++;
            n--;
        }
        if (j->pos == LFG_BYTES) {
            junk_forward(j);
            j->pos = 0;
        }
    }
}

/* RVZ packed group: runs of (u32 size, data) or (u32 size | 0x80000000, 68-byte seed) for junk */
static int rvz_unpack(RvzImage* m, const uint8_t* d, size_t dn, uint64_t disc_off, uint8_t* out, size_t out_len) {
    size_t p = 0, o = 0;
    uint64_t off = disc_off;
    static Junk j; /* 2 KiB: not on the stack */
    while (o < out_len) {
        if (p + 4 > dn) return fail(m->err, m->errn, "packed group ends early (damaged image?)");
        uint32_t size = be32(d + p);
        p += 4;
        int junk = size >> 31;
        size &= 0x7FFFFFFFu;
        if (size > out_len - o) return fail(m->err, m->errn, "packed group has the wrong size (damaged image?)");
        if (junk) {
            if (p + 68 > dn) return fail(m->err, m->errn, "packed group ends early (damaged image?)");
            junk_init(&j, d + p);
            p += 68;
            junk_skip(&j, (uint32_t)(off % BLOCK));
            junk_take(&j, out + o, size);
        } else {
            if (p + size > dn) return fail(m->err, m->errn, "packed group ends early (damaged image?)");
            memcpy(out + o, d + p, size);
            p += size;
        }
        o += size;
        off += size;
    }
    return 1;
}

/* ---------------------------------------------------------------- decompression: in[0..n) -> out (exactly want bytes) */
static int decompress(RvzImage* m, const uint8_t* in, size_t n, uint8_t* out, size_t want) {
    switch (m->ctype) {
    case C_NONE:
        if (n < want) return fail(m->err, m->errn, "stored data is short (damaged image?)");
        memcpy(out, in, want);
        return 1;
    case C_PURGE: { /* segments (u32 offset, u32 size, data), then a SHA-1; the rest is zeros */
        memset(out, 0, want);
        size_t p = 0;
        while (n >= 20 && p + 8 <= n - 20) {
            uint32_t o = be32(in + p), s = be32(in + p + 4);
            p += 8;
            if ((uint64_t)o + s > want || p + s > n) return fail(m->err, m->errn, "purge segment out of range");
            memcpy(out + o, in + p, s);
            p += s;
        }
        return 1;
    }
    case C_BZIP2: {
        unsigned int got = (unsigned int)want;
        int rc = BZ2_bzBuffToBuffDecompress((char*)out, &got, (char*)in, (unsigned int)n, 0, 0);
        if (rc != BZ_OK && rc != BZ_OUTBUFF_FULL) return fail(m->err, m->errn, "bzip2 error %d", rc);
        if (got != want) return fail(m->err, m->errn, "bzip2 data decompressed to %u bytes, expected %zu", got, want);
        return 1;
    }
    case C_LZMA:
    case C_LZMA2: {
        lzma_filter filters[2] = {{.id = m->ctype == C_LZMA ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2}, {.id = LZMA_VLI_UNKNOWN}};
        if (lzma_properties_decode(&filters[0], NULL, m->cdata, m->cdata_n) != LZMA_OK)
            return fail(m->err, m->errn, "bad LZMA settings in the header");
        lzma_stream s = LZMA_STREAM_INIT;
        lzma_ret rc = lzma_raw_decoder(&s, filters);
        free(filters[0].options);
        if (rc != LZMA_OK) return fail(m->err, m->errn, "LZMA decoder error %d", (int)rc);
        s.next_in = in;
        s.avail_in = n;
        s.next_out = out;
        s.avail_out = want;
        while (s.avail_out) {
            rc = lzma_code(&s, LZMA_FINISH);
            if (rc != LZMA_OK) break;
        }
        size_t got = want - s.avail_out;
        lzma_end(&s);
        if (got != want) return fail(m->err, m->errn, "LZMA data decompressed to %zu bytes, expected %zu (error %d)", got, want, (int)rc);
        return 1;
    }
    case C_ZSTD: {
        size_t got = ZSTD_decompressDCtx(m->zd, out, want, in, n);
        if (ZSTD_isError(got)) return fail(m->err, m->errn, "zstd: %s (damaged image?)", ZSTD_getErrorName(got));
        if (got != want) return fail(m->err, m->errn, "zstd data decompressed to %zu bytes, expected %zu", got, want);
        return 1;
    }
    }
    return fail(m->err, m->errn, "unknown compression type %u", m->ctype);
}

static int read_raw(RvzImage* m, uint64_t off, void* dst, size_t n) {
    if (lseek(m->fd, (off_t)off, SEEK_SET) != (off_t)off) return 0;
    uint8_t* p = dst;
    while (n) {
        ssize_t k = read(m->fd, p, n);
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static int read_at(RvzImage* m, uint64_t off, void* dst, size_t n) {
    if (off > m->fsize || n > m->fsize - off) return fail(m->err, m->errn, "the image file is incomplete (truncated download or copy?)");
    if (off >= m->win_off && off + n <= m->win_off + m->win_len) {
        memcpy(dst, m->win + (off - m->win_off), n);
        return 1;
    }
    if (n > RWIN / 2) {
        if (!read_raw(m, off, dst, n)) return fail(m->err, m->errn, "read error at offset %llu", (unsigned long long)off);
        return 1;
    }
    size_t len = m->fsize - off < RWIN ? (size_t)(m->fsize - off) : RWIN;
    m->win_len = 0;
    if (!read_raw(m, off, m->win, len)) return fail(m->err, m->errn, "read error at offset %llu", (unsigned long long)off);
    m->win_off = off;
    m->win_len = len;
    memcpy(dst, m->win, n);
    return 1;
}

static int grow(uint8_t** buf, size_t* cap, size_t need) {
    if (need <= *cap) return 1;
    uint8_t* b = realloc(*buf, need);
    if (!b) return 0;
    *buf = b;
    *cap = need;
    return 1;
}

/* a table (raw data or group entries): stored with the image's compression, decompressed to exactly n bytes */
static uint8_t* read_table(RvzImage* m, uint64_t off, uint32_t stored, size_t n) {
    if (stored > m->fsize) {
        fail(m->err, m->errn, "the image file is incomplete (truncated download or copy?)");
        return NULL;
    }
    uint8_t* in = malloc(stored ? stored : 1);
    uint8_t* out = malloc(n ? n : 1);
    if (!in || !out || !read_at(m, off, in, stored) || !decompress(m, in, stored, out, n)) {
        if (in && out && !m->err[0]) fail(m->err, m->errn, "out of memory");
        free(in);
        free(out);
        return NULL;
    }
    free(in);
    return out;
}

/* ---------------------------------------------------------------- the image */
int rvz_is_image(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint8_t mg[4] = {0};
    ssize_t n = read(fd, mg, 4);
    close(fd);
    return n == 4 && (!memcmp(mg, "RVZ\x01", 4) || !memcmp(mg, "WIA\x01", 4));
}

void rvz_close(RvzImage* m) {
    if (!m) return;
    if (m->fd >= 0) close(m->fd);
    free(m->win);
    if (m->zd) ZSTD_freeDCtx(m->zd);
    free(m->raws);
    free(m->grp);
    free(m->in);
    free(m->mid);
    free(m->out);
    free(m);
}

static int cmp_raw(const void* a, const void* b) {
    const RawEntry *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

RvzImage* rvz_open(const char* path, char* err, size_t errn) {
    if (err && errn) err[0] = 0;
    RvzImage* m = calloc(1, sizeof *m);
    if (!m) { fail(err, errn, "out of memory"); return NULL; }
    m->fd = -1;
    m->err = err;
    m->errn = errn;
    uint8_t h1[0x48], h2[0xDC], sha[20];
    m->fd = open(path, O_RDONLY);
    if (m->fd < 0) { fail(err, errn, "cannot open the file"); goto bad; }
    if (!(m->win = malloc(RWIN))) { fail(err, errn, "out of memory"); goto bad; }
    struct stat st;
    if (fstat(m->fd, &st)) { fail(err, errn, "cannot read the file's size"); goto bad; }
    m->fsize = (uint64_t)st.st_size;
    if (m->fsize < sizeof h1 || !read_raw(m, 0, h1, sizeof h1) ||
        (memcmp(h1, "RVZ\x01", 4) && memcmp(h1, "WIA\x01", 4))) {
        fail(err, errn, "not an RVZ or WIA image");
        goto bad;
    }
    m->rvz = !memcmp(h1, "RVZ\x01", 4);
    sha1_of(h1, 0x34, sha);
    if (memcmp(sha, h1 + 0x34, 20)) { fail(err, errn, "header 1 is damaged (its SHA-1 does not match)"); goto bad; }
    uint32_t h2_size = be32(h1 + 12);
    m->iso_size = be64(h1 + 0x24);
    if (be64(h1 + 0x2C) != m->fsize) {
        fail(err, errn, "the file is %llu bytes, its header says %llu (incomplete copy or download?)",
             (unsigned long long)m->fsize, (unsigned long long)be64(h1 + 0x2C));
        goto bad;
    }
    if (h2_size < sizeof h2 || h2_size > 0x10000) { fail(err, errn, "header 2 has an unusual size %u", h2_size); goto bad; }
    uint8_t* h2full = malloc(h2_size);
    if (!h2full || h2_size > m->fsize - sizeof h1 || !read_raw(m, sizeof h1, h2full, h2_size)) { free(h2full); fail(err, errn, "header 2 is incomplete"); goto bad; }
    sha1_of(h2full, h2_size, sha);
    memcpy(h2, h2full, sizeof h2);
    free(h2full);
    if (memcmp(sha, h1 + 0x10, 20)) { fail(err, errn, "header 2 is damaged (its SHA-1 does not match)"); goto bad; }
    uint32_t disc_type = be32(h2), n_part = be32(h2 + 0x90);
    m->ctype = be32(h2 + 4);
    m->chunk = be32(h2 + 0xC);
    memcpy(m->dhead, h2 + 0x10, 0x80);
    m->nraw = be32(h2 + 0xB4);
    uint64_t raw_off = be64(h2 + 0xB8);
    uint32_t raw_size = be32(h2 + 0xC0);
    m->ngrp = be32(h2 + 0xC4);
    uint64_t grp_off = be64(h2 + 0xC8);
    uint32_t grp_size = be32(h2 + 0xD0);
    m->cdata_n = h2[0xD4] > 7 ? 7 : h2[0xD4];
    memcpy(m->cdata, h2 + 0xD5, m->cdata_n);
    if (disc_type != 1 || n_part) { fail(err, errn, "this is a Wii image; only GameCube discs are supported"); goto bad; }
    if (m->ctype > C_ZSTD || (m->rvz && m->ctype == C_PURGE)) { fail(err, errn, "unknown compression type %u", m->ctype); goto bad; }
    if (!m->chunk || m->chunk % BLOCK || m->chunk > (64u << 20)) { fail(err, errn, "unusual block size %u", m->chunk); goto bad; }
    if (m->iso_size < 0x80 || m->iso_size > (8ull << 30)) { fail(err, errn, "unusual disc size %llu", (unsigned long long)m->iso_size); goto bad; }
    if (m->nraw > 1000000 || m->ngrp > 10000000) { fail(err, errn, "the entry tables are too large (damaged image?)"); goto bad; }
    snprintf(m->desc, sizeof m->desc, "%s, %s, %u KiB blocks", m->rvz ? "RVZ" : "WIA", k_comp_names[m->ctype], m->chunk / 1024);
    if (m->ctype == C_ZSTD && !(m->zd = ZSTD_createDCtx())) { fail(err, errn, "out of memory"); goto bad; }
    if ((m->ctype == C_LZMA && m->cdata_n < 5) || (m->ctype == C_LZMA2 && m->cdata_n < 1)) {
        fail(err, errn, "LZMA settings missing");
        goto bad;
    }

    uint8_t* raw = read_table(m, raw_off, raw_size, (size_t)m->nraw * 24);
    if (!raw) goto bad;
    m->raws = malloc(sizeof(RawEntry) * (m->nraw ? m->nraw : 1));
    if (!m->raws) { free(raw); fail(err, errn, "out of memory"); goto bad; }
    for (uint32_t i = 0; i < m->nraw; i++) {
        const uint8_t* e = raw + i * 24;
        m->raws[i] = (RawEntry){be64(e), be64(e + 8), be32(e + 16), be32(e + 20)};
    }
    free(raw);
    qsort(m->raws, m->nraw, sizeof *m->raws, cmp_raw);
    m->grp = read_table(m, grp_off, grp_size, (size_t)m->ngrp * (m->rvz ? 12 : 8));
    if (!m->grp) goto bad;
    m->out = malloc(m->chunk);
    if (!m->out) { fail(err, errn, "out of memory"); goto bad; }
    return m;
bad:
    rvz_close(m);
    return NULL;
}

uint64_t rvz_iso_size(const RvzImage* m) { return m->iso_size; }
const uint8_t* rvz_disc_header(const RvzImage* m) { return m->dhead; }
const char* rvz_describe(const RvzImage* m) { return m->desc; }

/* group i, holding disc bytes [disc_off, disc_off + n), into m->out */
static int group(RvzImage* m, uint32_t i, uint64_t disc_off, size_t n) {
    if (i >= m->ngrp) return fail(m->err, m->errn, "group %u out of range (damaged image?)", i);
    uint32_t o4, dsize, packed = 0;
    int compressed = 1;
    if (m->rvz) {
        const uint8_t* e = m->grp + (size_t)i * 12;
        o4 = be32(e);
        dsize = be32(e + 4);
        packed = be32(e + 8);
        compressed = dsize >> 31;
        dsize &= 0x7FFFFFFFu;
    } else {
        const uint8_t* e = m->grp + (size_t)i * 8;
        o4 = be32(e);
        dsize = be32(e + 4);
    }
    if (!dsize) {
        memset(m->out, 0, n);
        return 1;
    }
    if (dsize > m->fsize) return fail(m->err, m->errn, "the image file is incomplete (truncated download or copy?)");
    if (!grow(&m->in, &m->in_cap, dsize)) return fail(m->err, m->errn, "out of memory");
    if (!read_at(m, (uint64_t)o4 * 4, m->in, dsize)) return 0;
    const uint8_t* d = m->in;
    size_t dn = dsize;
    if (compressed && m->ctype != C_NONE) {
        size_t want = packed ? packed : n;
        uint8_t* dst = m->out;
        if (packed) {
            if (!grow(&m->mid, &m->mid_cap, want)) return fail(m->err, m->errn, "out of memory");
            dst = m->mid;
        }
        if (!decompress(m, d, dn, dst, want)) {
            char tmp[160];
            snprintf(tmp, sizeof tmp, "%s", m->err);
            return fail(m->err, m->errn, "group %u: %s", i, tmp);
        }
        if (!packed) return 1;
        d = m->mid;
        dn = want;
    } else if (!packed) {
        if (dn != n) return fail(m->err, m->errn, "group %u is %zu bytes, expected %zu", i, dn, n);
        memcpy(m->out, d, n);
        return 1;
    }
    return rvz_unpack(m, d, dn, disc_off, m->out, n);
}

typedef struct { RvzSink sink; void* user; uint64_t pos; RvzImage* m; } Emit;

static int emit(Emit* e, const uint8_t* d, size_t n) {
    if (!n) return 1;
    if (!e->sink(e->user, d, n)) return fail(e->m->err, e->m->errn, "stopped");
    e->pos += n;
    return 1;
}
static int fill(Emit* e, uint64_t upto) {
    static const uint8_t zeros[64 * 1024];
    while (e->pos < upto) {
        uint64_t k = upto - e->pos;
        if (!emit(e, zeros, k > sizeof zeros ? sizeof zeros : (size_t)k)) return 0;
    }
    return 1;
}
/* b holds disc bytes from `start`; emit what lies past pos (gaps between entries are zeros) */
static int emit_range(Emit* e, const uint8_t* b, size_t n, uint64_t start) {
    if (start + n <= e->pos) return 1;
    if (start > e->pos && !fill(e, start)) return 0;
    uint64_t skip = e->pos - start;
    return emit(e, b + skip, n - (size_t)skip);
}

int rvz_extract(RvzImage* m, RvzSink sink, void* user, char* err, size_t errn) {
    m->err = err;
    m->errn = errn;
    if (err && errn) err[0] = 0;
    Emit e = {sink, user, 0, m};
    if (!emit(&e, m->dhead, 0x80)) return 0;
    for (uint32_t r = 0; r < m->nraw; r++) {
        const RawEntry* re = &m->raws[r];
        if (re->off >= m->iso_size || re->size > UINT64_MAX - re->off)
            return fail(err, errn, "a data entry lies outside the disc (damaged image?)");
        uint64_t end = re->off + re->size;
        if (end > m->iso_size) end = m->iso_size;
        uint64_t start = re->off - re->off % BLOCK; /* entries start at a 32 KiB boundary inside their first group */
        for (uint32_t g = 0; g < re->ngroups; g++) {
            uint64_t gs = start + (uint64_t)g * m->chunk;
            if (gs >= end) break;
            size_t n = (size_t)(end - gs < m->chunk ? end - gs : m->chunk);
            if (!group(m, re->group + g, gs, n)) return 0;
            uint64_t lo = re->off > gs ? re->off : gs;
            if (!emit_range(&e, m->out + (lo - gs), n - (size_t)(lo - gs), lo)) return 0;
        }
    }
    if (!fill(&e, m->iso_size)) return 0;
    if (e.pos != m->iso_size)
        return fail(err, errn, "output size %llu differs from the disc size %llu", (unsigned long long)e.pos,
                    (unsigned long long)m->iso_size);
    return 1;
}

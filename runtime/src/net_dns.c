/* DNS query encoder and answer decoder (net_dns.h). */
#include <string.h>

#include "net_dns.h"

size_t dns_build_query(uint8_t* out, size_t cap, uint16_t id, const char* name) {
    size_t len = name ? strlen(name) : 0;
    if (len && name[len - 1] == '.') len--;
    if (!len || len > 253 || cap < 12 + len + 2 + 4) return 0;
    uint8_t* p = out;
    *p++ = (uint8_t)(id >> 8); *p++ = (uint8_t)id;
    *p++ = 0x01; *p++ = 0x00;  /* standard query, recursion desired */
    *p++ = 0; *p++ = 1;        /* one question */
    memset(p, 0, 6); p += 6;   /* no answer, authority or additional records */
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && name[j] != '.') j++;
        size_t l = j - i;
        if (l == 0 || l > 63) return 0;
        *p++ = (uint8_t)l;
        memcpy(p, name + i, l);
        p += l;
        i = j + 1;
        if (j == len) break;
        if (i == len) return 0;  /* "a..": an empty last label (the one trailing dot was removed above) */
    }
    *p++ = 0;
    *p++ = 0; *p++ = 1;  /* type A */
    *p++ = 0; *p++ = 1;  /* class IN */
    return (size_t)(p - out);
}

/* skips a (possibly compressed) name at p[*off]; 0 on success */
static int skip_name(const uint8_t* p, size_t n, size_t* off) {
    size_t o = *off;
    for (int guard = 0; guard < 128; guard++) {
        if (o >= n) return -1;
        uint8_t l = p[o];
        if (l == 0) { *off = o + 1; return 0; }
        if ((l & 0xC0) == 0xC0) {  /* a pointer ends the name here */
            if (o + 2 > n) return -1;
            *off = o + 2;
            return 0;
        }
        if (l & 0xC0) return -1;
        o += 1 + (size_t)l;
    }
    return -1;
}

int dns_parse_answer(const uint8_t* p, size_t n, uint16_t id, uint8_t ip[4], int* rcode) {
    if (n < 12) return DNS_MALFORMED;
    uint16_t rid = (uint16_t)(p[0] << 8 | p[1]);
    if (rid != id || !(p[2] & 0x80)) return DNS_NOT_ANSWER;
    int rc = p[3] & 0x0F;
    if (rcode) *rcode = rc;
    unsigned qd = (unsigned)(p[4] << 8 | p[5]), an = (unsigned)(p[6] << 8 | p[7]);
    if (qd != 1) return DNS_NOT_ANSWER;
    if (rc == 3) return DNS_NXDOMAIN;
    if (rc != 0) return DNS_SERVFAIL;
    size_t o = 12;
    if (skip_name(p, n, &o) || o + 4 > n) return DNS_MALFORMED;
    o += 4;  /* question type and class */
    for (unsigned k = 0; k < an; k++) {
        if (skip_name(p, n, &o) || o + 10 > n) return DNS_MALFORMED;
        unsigned type = (unsigned)(p[o] << 8 | p[o + 1]), cls = (unsigned)(p[o + 2] << 8 | p[o + 3]);
        size_t rdlen = (size_t)(p[o + 8] << 8 | p[o + 9]);
        o += 10;
        if (o + rdlen > n) return DNS_MALFORMED;
        if (type == 1 && cls == 1 && rdlen == 4) {
            memcpy(ip, p + o, 4);
            return DNS_OK;
        }
        o += rdlen;
    }
    return DNS_NO_A;
}

int dns_parse_ipv4(const char* s, uint8_t ip[4], uint16_t* port) {
    if (!s) return 0;
    unsigned v[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned x = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            x = x * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3) return 0;
        }
        if (x > 255) return 0;
        v[i] = x;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    unsigned pt = 0;
    if (*s == ':') {
        s++;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') {
            pt = pt * 10 + (unsigned)(*s++ - '0');
            if (pt > 65535) return 0;
        }
        if (!pt) return 0;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s) return 0;
    for (int i = 0; i < 4; i++) ip[i] = (uint8_t)v[i];
    if (port) *port = (uint16_t)pt;
    return 1;
}

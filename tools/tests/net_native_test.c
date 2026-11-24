/* Host test of the native network layer (runtime/src/net_native.c, net_dns.c) with real loopback sockets.
 *
 * The game's side is played by this file: it calls the replacement functions (hle_net_*) with arguments laid
 * out as the game's code lays them out (docs/online-research.md), and its guest_call() stands in for the two
 * game callbacks (TCP 0x80078578, UDP 0x80078378), checking what they are given. The other side is a fake DNS
 * server, a TCP server and a UDP peer on 127.0.0.1, each on its own thread. Checked:
 *   - the DNS codec against hand-built packets (a query for game04.st-pso.games.sega.net byte for byte; an
 *     answer with a CNAME and compression; NXDOMAIN; truncation; net_dns parsing);
 *   - DNS through the API with a two-server list in net_dns whose first server never answers: the server name
 *     answered by the second after 2 queries to the first (about 2 s; answer written as {4, address}, count
 *     1), a server name answered NXDOMAIN (-0x42), one never answered (-0x45 after the timeout, 2 + 5
 *     queries), localhost through the system resolver;
 *   - the memory card backup at the network start: card_a.before-online.raw written only when missing (an
 *     existing one is kept byte for byte), plus a dated one per session (run twice by the script: with and
 *     without a first backup); the default DNS list (3.18.217.27, then 144.217.87.48) when net_dns is empty;
 *   - TCP: create, option, bind, connect, status until state 4, the one-shot receive callback with at most the
 *     armed length (3000 bytes arrive as 2048 + 952 over two armings), the bytes in the game's buffer, send of
 *     a {u16 length, u32 data} piece, the server's close as callback -8, close and delete; the memory card
 *     backup once per session; a refused connect as a negative state;
 *   - UDP: open, a second open on the same local port refused (-3), send, the two-phase callback with the info
 *     block (handle, length, buffer, source address and port), close;
 *   - the callbacks run on the caller's registers, which are put back afterwards, below its stack pointer.
 * Not game data: no file from the disc is read. Usage: tools/tests/run_net_native_test.sh */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net_dns.h"
#include "net_native.h"
#include "runtime.h"
#include "settings.h"

/* ---- stubs for the runtime around net_native.c ---- */
Settings g_settings;
static int g_verbose;
void rt_log(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("    log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
_Noreturn void rt_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}
int rt_atexit(void (*h)(void)) { (void)h; return 0; }
uint64_t platform_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static char g_save_path[256];
const char* platform_save_path(void) { return g_save_path; }
void platform_local_time(int* y, int* mo, int* d, int* h, int* mi, int* s) { *y = 2026; *mo = 11; *d = 23; *h = 20; *mi = 5; *s = 9; }
static int g_backups;
static char g_backup_path[4][512];
int exi_card_backup(const char* path) {  /* writes a stand-in card image */
    if (g_backups < 4) snprintf(g_backup_path[g_backups], sizeof g_backup_path[0], "%s", path);
    g_backups++;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fputs("NEW CARD", f);
    fclose(f);
    return 0;
}
static int file_is(const char* path, const char* text) {
    char b[64] = {0};
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    return n == strlen(text) && !memcmp(b, text, n);
}
uint32_t mmio_read(uint32_t ea, int size) { (void)ea; (void)size; return 0; }
void mmio_write(uint32_t ea, uint32_t v, int size) { (void)ea; (void)v; (void)size; }
void mmio_write64(uint32_t ea, uint64_t v) { (void)ea; (void)v; }
uint8_t g_wg[8192];
int g_wg_n, g_prev_bp_prefix, g_gxstat;
unsigned long long g_pipe_stores, g_pipe_bytes;
void gx_pipe_write(uint32_t v, int size) { (void)v; (void)size; }
void gs_store(void) {}
void f_801465C0(Cpu* c) { c->r[3] = 0; }

/* the replacements under test */
#define H(n) void hle_net_##n(Cpu* c);
H(adapter_type) H(link_status) H(stack_init) H(dhcp_request) H(dns_start) H(dns_poll) H(dns_free) H(tcp_create)
H(tcp_option) H(tcp_bind) H(tcp_connect) H(tcp_status) H(tcp_receive) H(tcp_send) H(tcp_close) H(tcp_delete)
H(udp_open) H(udp_send) H(udp_close) H(ppp_status)

static int g_fail, g_checks;
#define CHECK(cond, ...) do {                                                     \
        g_checks++;                                                               \
        if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* ---- guest layout ---- */
#define STACK_TOP 0x80400000u
#define NAME_EA   0x80300000u
#define ADDR_EA   0x80300100u  /* {4, address} arguments and results */
#define COUNT_EA  0x80300120u
#define STATE_EA  0x80300130u
#define SPACE_EA  0x80300134u
#define X_EA      0x80300138u
#define OPT_EA    0x80300140u
#define LIST_EA   0x80300150u
#define SEND_EA   0x80300200u
#define TCPBUF_EA 0x80310000u  /* the game object's +0x74 receive buffer (0x800 bytes) */
#define UDPBUF_EA 0x80320000u
#define TCP_CB 0x80078578u
#define UDP_CB 0x80078378u

static Cpu g_c;
static void reset_cpu(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.r[1] = STACK_TOP;
    g_c.r[2] = 0x805E0000u;
    g_c.r[13] = 0x805D0000u;
    g_c.lr = 0x80078AC4u;
}
static uint32_t call(void (*fn)(Cpu*), uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8,
                     uint32_t r9) {
    g_c.r[3] = r3; g_c.r[4] = r4; g_c.r[5] = r5; g_c.r[6] = r6; g_c.r[7] = r7; g_c.r[8] = r8; g_c.r[9] = r9;
    fn(&g_c);
    return g_c.r[3];
}
static void put_addr(uint32_t ea, uint8_t a, uint8_t b, uint8_t cc, uint8_t d) {
    st32(ea, 4);
    uint8_t ip[4] = {a, b, cc, d};
    mem_write_bytes(ea + 4, ip, 4);
}

/* ---- the game's callbacks (guest_call) ---- */
static int g_tcp_calls, g_tcp_last_len, g_tcp_last_h, g_udp_phase0, g_udp_phase1, g_udp_len;
static uint8_t g_udp_src[4];
static uint16_t g_udp_sport;
static int g_bad_frame;
void guest_call(Cpu* c, uint32_t addr) {
    if (c->r[1] >= STACK_TOP || c->r[1] < STACK_TOP - 0x100 || (c->r[1] & 15) || ld32(c->r[1]) != STACK_TOP) g_bad_frame++;
    if (addr == TCP_CB) {
        g_tcp_calls++;
        g_tcp_last_len = (int16_t)c->r[3];
        g_tcp_last_h = (int16_t)c->r[4];
    } else if (addr == UDP_CB) {
        uint32_t info = c->r[4];
        if (c->r[3] == 0) {  /* as 0x80078378: the buffer and its size, and the source address copied */
            g_udp_phase0++;
            CHECK((int16_t)ld16(info) == 0x400, "UDP info handle 0x%X", ld16(info));
            st32(info + 4, UDPBUF_EA);
            st16(info + 2, 0x800);
            CHECK(ld32(info + 8) == 4, "UDP info source length %u", ld32(info + 8));
            mem_read_bytes(info + 12, g_udp_src, 4);
            g_udp_sport = ld16(info + 0x30);
        } else {
            g_udp_phase1++;
            g_udp_len = (int16_t)ld16(info + 2);
        }
    } else {
        g_fail++;
        printf("FAIL: guest_call to unexpected %08X\n", addr);
    }
    /* a callee may change any volatile register (and r14 here, to see the caller's are put back) */
    c->r[0] = c->r[5] = c->r[11] = c->r[12] = 0xDEADBEEF;
    c->r[14] = 0xBAD;
    c->lr = 0x12345678;
    c->r[3] = 0;
}

/* ---- fake servers ---- */
static int udp_socket(uint16_t* port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(s, (struct sockaddr*)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(s, (struct sockaddr*)&a, &l);
    *port = ntohs(a.sin_port);
    return s;
}

static int g_dns_fd;
static int g_dns_queries;
static uint8_t g_dns_last_query[512];
static int g_dns_last_n;
/* answers game04 with a CNAME then an A record (127.0.0.1), ss702 with NXDOMAIN, ignores everything else */
static void* dns_server(void* arg) {
    (void)arg;
    for (;;) {
        uint8_t q[512], r[512];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = (int)recvfrom(g_dns_fd, q, sizeof q, 0, (struct sockaddr*)&from, &fl);
        if (n <= 0) return NULL;
        g_dns_queries++;
        memcpy(g_dns_last_query, q, (size_t)n);
        g_dns_last_n = n;
        int game04 = n > 20 && q[12] == 6 && !strncasecmp((const char*)q + 13, "game04", 6);
        int ss702 = n > 20 && q[12] == 5 && !strncasecmp((const char*)q + 13, "ss702", 5);
        if (!game04 && !ss702) continue;
        memcpy(r, q, (size_t)n);
        r[2] = 0x81;
        r[3] = ss702 ? 0x83 : 0x80;
        int k = n;
        if (game04) {
            r[6] = 0; r[7] = 2;  /* two answers */
            static const uint8_t cname[] = {0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'f', 'o', 'o', 0xC0, 12};
            static const uint8_t a[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 127, 0, 0, 1};
            memcpy(r + k, cname, sizeof cname); k += (int)sizeof cname;
            memcpy(r + k, a, sizeof a); k += (int)sizeof a;
        }
        sendto(g_dns_fd, r, (size_t)k, 0, (struct sockaddr*)&from, fl);
    }
}

static int g_silent_fd, g_silent_queries;
static void* silent_server(void* arg) {  /* a DNS server that never answers */
    (void)arg;
    for (;;) {
        uint8_t q[512];
        if (recv(g_silent_fd, q, sizeof q, 0) <= 0) return NULL;
        g_silent_queries++;
    }
}

static int g_tcp_listen;
static int g_srv_got;
static uint8_t g_srv_buf[256];
static void* tcp_server(void* arg) {
    (void)arg;
    int s = accept(g_tcp_listen, NULL, NULL);
    if (s < 0) return NULL;
    uint8_t out[3000];
    for (int i = 0; i < 3000; i++) out[i] = (uint8_t)(i * 7 + 3);
    send(s, out, sizeof out, 0);
    while (g_srv_got < 100) {
        int n = (int)recv(s, g_srv_buf + g_srv_got, sizeof g_srv_buf - (size_t)g_srv_got, 0);
        if (n <= 0) break;
        g_srv_got += n;
    }
    usleep(100000);
    close(s);
    return NULL;
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* ---- tests ---- */
static void test_codec(void) {
    uint8_t q[300];
    size_t n = dns_build_query(q, sizeof q, 0xBEEF, "game04.st-pso.games.sega.net");
    static const uint8_t want[] = {0xBE, 0xEF, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                   6, 'g', 'a', 'm', 'e', '0', '4', 6, 's', 't', '-', 'p', 's', 'o',
                                   5, 'g', 'a', 'm', 'e', 's', 4, 's', 'e', 'g', 'a', 3, 'n', 'e', 't', 0,
                                   0, 1, 0, 1};
    CHECK(n == sizeof want && !memcmp(q, want, sizeof want), "query bytes (length %zu)", n);
    CHECK(dns_build_query(q, sizeof q, 1, "game04.st-pso.games.sega.net.") == sizeof want, "trailing dot");
    CHECK(!dns_build_query(q, sizeof q, 1, "a..b"), "empty label refused");
    CHECK(!dns_build_query(q, sizeof q, 1, ""), "empty name refused");
    CHECK(!dns_build_query(q, 20, 1, "game04.st-pso.games.sega.net"), "small buffer refused");

    /* answer: question, CNAME (compressed), A 144.217.87.48 */
    uint8_t r[200];
    memcpy(r, want, sizeof want);
    r[2] = 0x81; r[3] = 0x80; r[7] = 2;
    size_t k = sizeof want;
    static const uint8_t cname[] = {0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 12};
    static const uint8_t a[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 144, 217, 87, 48};
    memcpy(r + k, cname, sizeof cname); k += sizeof cname;
    memcpy(r + k, a, sizeof a); k += sizeof a;
    uint8_t ip[4] = {0};
    int rc = -1;
    CHECK(dns_parse_answer(r, k, 0xBEEF, ip, &rc) == DNS_OK && ip[0] == 144 && ip[1] == 217 && ip[2] == 87 && ip[3] == 48,
          "answer parsed (%u.%u.%u.%u)", ip[0], ip[1], ip[2], ip[3]);
    CHECK(dns_parse_answer(r, k, 0xBEEE, ip, &rc) == DNS_NOT_ANSWER, "other id ignored");
    CHECK(dns_parse_answer(r, k - 3, 0xBEEF, ip, &rc) == DNS_MALFORMED, "truncated answer");
    r[3] = 0x83;
    CHECK(dns_parse_answer(r, k, 0xBEEF, ip, &rc) == DNS_NXDOMAIN && rc == 3, "NXDOMAIN");
    r[3] = 0x80; r[7] = 1;
    CHECK(dns_parse_answer(r, sizeof want + sizeof cname, 0xBEEF, ip, &rc) == DNS_NO_A, "CNAME only: no A");
    uint16_t port = 1;
    CHECK(dns_parse_ipv4("144.217.87.48", ip, &port) && port == 0 && ip[3] == 48, "net_dns plain");
    CHECK(dns_parse_ipv4("10.0.0.2:5353", ip, &port) && port == 5353 && ip[0] == 10, "net_dns with port");
    CHECK(!dns_parse_ipv4("256.1.1.1", ip, &port) && !dns_parse_ipv4("1.2.3", ip, &port) &&
          !dns_parse_ipv4("dns.example", ip, &port) && !dns_parse_ipv4("1.2.3.4:0", ip, &port), "bad net_dns refused");
}

static int dns_lookup(const char* name, uint8_t out[4], int max_ms) {
    size_t n = strlen(name);
    mem_write_bytes(NAME_EA, name, n);
    int id = (int16_t)call(hle_net_dns_start, NAME_EA, (uint32_t)n, 4, 0, 0, 0, 0);
    if (id < 0) return 1000 + id;
    int r = -1;
    for (int i = 0; i < max_ms / 10 && r == -1; i++) {
        st32(ADDR_EA, 4); st32(ADDR_EA + 4, 0); st16(COUNT_EA, 1);
        r = (int16_t)call(hle_net_dns_poll, (uint32_t)id, ADDR_EA, COUNT_EA, 0, X_EA, 0xFFFFFFFFu, 0);
        if (r == -1) sleep_ms(10);
    }
    if (r == 0) {
        CHECK(ld32(ADDR_EA) == 4 && ld16(COUNT_EA) == 1, "DNS result {4, address}, count 1");
        mem_read_bytes(ADDR_EA + 4, out, 4);
    }
    CHECK(call(hle_net_dns_free, (uint32_t)id, 0, 0, 0, 0, 0, 0) == 0, "DNS free");
    return r;
}

static void test_dns(void) {
    uint8_t ip[4] = {0};
    uint64_t t0 = platform_now_ns();
    int r = dns_lookup("game04.st-pso.games.sega.net", ip, 5000);
    unsigned ms = (unsigned)((platform_now_ns() - t0) / 1000000);
    CHECK(r == 0 && ip[0] == 127 && ip[3] == 1, "game04 via net_dns: %d %u.%u.%u.%u", r, ip[0], ip[1], ip[2], ip[3]);
    CHECK(g_silent_queries == 2 && g_dns_queries == 1 && ms >= 1900 && ms < 3500,
          "first server asked twice, then the second answered (%d, %d queries, %u ms)", g_silent_queries, g_dns_queries, ms);
    CHECK(g_dns_last_n > 12 && g_dns_last_query[2] == 0x01 && g_dns_last_query[5] == 1, "the server got a standard query");
    r = dns_lookup("GAME04.ST-PSO.GAMES.SEGA.NET.", ip, 5000);
    CHECK(r == 0, "server name matched without case and with a trailing dot (%d)", r);
    r = dns_lookup("ss702.sonicteam.com", ip, 5000);
    CHECK(r == -0x42, "NXDOMAIN -> -0x42 (%d)", r);
    int before = g_dns_queries, sbefore = g_silent_queries;
    t0 = platform_now_ns();
    r = dns_lookup("ddplanet.sega.co.jp", ip, 9000);
    ms = (unsigned)((platform_now_ns() - t0) / 1000000);
    CHECK(r == -0x45 && ms >= 6400 && ms < 8000, "no answer -> -0x45 after the timeout (%d, %u ms)", r, ms);
    CHECK(g_silent_queries - sbefore == 2 && g_dns_queries - before == 5, "queries: 2 to the first server, 5 to the second (%d, %d)",
          g_silent_queries - sbefore, g_dns_queries - before);
    r = dns_lookup("localhost", ip, 3000);
    CHECK(r == 0 && ip[0] == 127, "another name through the system resolver (%d, %u.%u.%u.%u)", r, ip[0], ip[1], ip[2], ip[3]);
}

static void pump(int ms) {
    for (int i = 0; i < ms / 5; i++) {
        net_frame(&g_c);
        sleep_ms(5);
    }
}

static void test_tcp(uint16_t port) {
    int h = (int16_t)call(hle_net_tcp_create, 0, 0, 0, 0, 0, 0, 0);
    CHECK(h == 0, "TCP handle 0 (%d)", h);
    st32(OPT_EA, 1);
    CHECK(call(hle_net_tcp_option, (uint32_t)h, 0x2001, OPT_EA, 0, 0, 0, 0) == 0, "option");
    put_addr(ADDR_EA, 0, 0, 0, 0);
    st32(ADDR_EA, 4);
    CHECK(call(hle_net_tcp_bind, (uint32_t)h, ADDR_EA, 0, 0, 0, 0, 0) == 0, "bind");
    put_addr(ADDR_EA, 127, 0, 0, 1);
    st32(OPT_EA, 4);
    CHECK(call(hle_net_tcp_connect, (uint32_t)h, ADDR_EA, port, OPT_EA, 0, 0, 0) == 0, "connect started");
    int state = 0;
    for (int i = 0; i < 200 && state < 4; i++) {
        CHECK(call(hle_net_tcp_status, (uint32_t)h, STATE_EA, 0, SPACE_EA, X_EA, 0, 0) == 0, "status");
        state = (int16_t)ld16(STATE_EA);
        if (state < 0) break;
        if (state < 4) sleep_ms(5);
    }
    CHECK(state == 4, "connected (state %d)", state);
    CHECK(ld32(SPACE_EA) > 0x800, "send space %u", ld32(SPACE_EA));

    /* the receive: one callback per arming, at most the armed 0x800 bytes */
    uint8_t got[3000];
    int total = 0;
    for (int round = 0; round < 4 && total < 3000; round++) {
        int before = g_tcp_calls;
        g_c.r[5] = 0x1234; g_c.r[14] = 0x4444; g_c.lr = 0x80078AC4u;
        call(hle_net_tcp_receive, (uint32_t)h, TCP_CB, 0x800, TCPBUF_EA, 0, 0, 0);
        uint32_t keep14 = g_c.r[14], keep1 = g_c.r[1], keeplr = g_c.lr;
        g_c.r[5] = 0x1234;
        for (int i = 0; i < 200 && g_tcp_calls == before; i++) { net_frame(&g_c); sleep_ms(5); }
        CHECK(g_tcp_calls == before + 1, "one callback per arming");
        CHECK(g_c.r[5] == 0x1234 && g_c.r[14] == keep14 && g_c.r[1] == keep1 && g_c.lr == keeplr,
              "caller's registers put back after the callback");
        CHECK(g_tcp_last_h == h && g_tcp_last_len > 0 && g_tcp_last_len <= 0x800, "callback (len %d, handle %d)",
              g_tcp_last_len, g_tcp_last_h);
        if (g_tcp_last_len <= 0) break;
        mem_read_bytes(TCPBUF_EA, got + total, (size_t)g_tcp_last_len);
        total += g_tcp_last_len;
        pump(20);
        CHECK(g_tcp_calls == before + 1, "no callback without a new arming");
    }
    int same = total == 3000;
    for (int i = 0; same && i < 3000; i++) same = got[i] == (uint8_t)(i * 7 + 3);
    CHECK(same, "3000 bytes in order (%d)", total);

    /* send: {u16 length, u16, u32 data} */
    uint8_t out[100];
    for (int i = 0; i < 100; i++) out[i] = (uint8_t)(255 - i);
    mem_write_bytes(SEND_EA, out, 100);
    st16(LIST_EA, 100); st16(LIST_EA + 2, 0); st32(LIST_EA + 4, SEND_EA);
    CHECK(call(hle_net_tcp_send, (uint32_t)h, 0, 1, LIST_EA, 0, 0, 0) == 0, "send accepted");
    for (int i = 0; i < 200 && g_srv_got < 100; i++) { net_frame(&g_c); sleep_ms(5); }
    CHECK(g_srv_got == 100 && !memcmp(g_srv_buf, out, 100), "server got the 100 bytes (%d)", g_srv_got);

    /* the server closes: the next arming gets -8 */
    int before = g_tcp_calls;
    call(hle_net_tcp_receive, (uint32_t)h, TCP_CB, 0x800, TCPBUF_EA, 0, 0, 0);
    for (int i = 0; i < 200 && g_tcp_calls == before; i++) { net_frame(&g_c); sleep_ms(5); }
    CHECK(g_tcp_calls == before + 1 && g_tcp_last_len == -8, "server close -> callback -8 (%d)", g_tcp_last_len);
    CHECK(call(hle_net_tcp_close, (uint32_t)h, 0, 0, 0, 0, 0, 0) == 0, "close");
    CHECK(call(hle_net_tcp_delete, (uint32_t)h, 0, 0, 0, 0, 0, 0) == 0, "delete");
    CHECK((int)call(hle_net_tcp_status, (uint32_t)h, STATE_EA, 0, SPACE_EA, X_EA, 0, 0) == -4, "deleted handle invalid");

    /* refused: a port nobody listens on */
    uint16_t dead;
    int s = udp_socket(&dead);  /* a free port number (UDP; no TCP listener there) */
    close(s);
    h = (int16_t)call(hle_net_tcp_create, 0, 0, 0, 0, 0, 0, 0);
    CHECK(h == 0, "handle reused (%d)", h);
    put_addr(ADDR_EA, 127, 0, 0, 1);
    call(hle_net_tcp_connect, (uint32_t)h, ADDR_EA, dead, OPT_EA, 0, 0, 0);
    state = 2;
    for (int i = 0; i < 200 && state >= 0 && state < 4; i++) {
        call(hle_net_tcp_status, (uint32_t)h, STATE_EA, 0, SPACE_EA, X_EA, 0, 0);
        state = (int16_t)ld16(STATE_EA);
        sleep_ms(5);
    }
    CHECK(state < 0, "refused connect -> negative state (%d)", state);
    call(hle_net_tcp_close, (uint32_t)h, 0, 0, 0, 0, 0, 0);
    call(hle_net_tcp_delete, (uint32_t)h, 0, 0, 0, 0, 0, 0);
}

static void test_udp(void) {
    uint16_t pport, lport;
    int peer = udp_socket(&pport);
    int tmp = udp_socket(&lport);
    close(tmp);
    put_addr(ADDR_EA, 0, 0, 0, 0);
    put_addr(ADDR_EA + 8, 127, 0, 0, 1);
    int h = (int16_t)call(hle_net_udp_open, ADDR_EA, lport, ADDR_EA + 8, pport, 0, UDP_CB, 0);
    CHECK(h == 0x400, "UDP handle 0x400 (0x%X)", h);
    int h2 = (int16_t)call(hle_net_udp_open, ADDR_EA, lport, ADDR_EA + 8, pport, 0, UDP_CB, 0);
    CHECK(h2 == -3, "second open on the same local port -> -3 (%d)", h2);
    mem_write_bytes(SEND_EA, "hello", 5);
    st16(LIST_EA, 5); st16(LIST_EA + 2, 0); st32(LIST_EA + 4, SEND_EA);
    put_addr(ADDR_EA + 16, 127, 0, 0, 1);
    put_addr(ADDR_EA + 24, 127, 0, 0, 1);
    CHECK(call(hle_net_udp_send, (uint32_t)h, ADDR_EA + 16, ADDR_EA + 24, pport, OPT_EA, 1, LIST_EA) == 0, "UDP send");
    char b[16] = {0};
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = (int)recvfrom(peer, b, sizeof b, 0, (struct sockaddr*)&from, &fl);
    CHECK(n == 5 && !memcmp(b, "hello", 5) && ntohs(from.sin_port) == lport, "peer got it from the local port (%d)", n);
    sendto(peer, "world!", 6, 0, (struct sockaddr*)&from, fl);
    for (int i = 0; i < 200 && !g_udp_phase1; i++) { net_frame(&g_c); sleep_ms(5); }
    CHECK(g_udp_phase0 == 1 && g_udp_phase1 == 1 && g_udp_len == 6, "two-phase callback (%d %d, len %d)", g_udp_phase0,
          g_udp_phase1, g_udp_len);
    char got[8] = {0};
    mem_read_bytes(UDPBUF_EA, got, 6);
    CHECK(!memcmp(got, "world!", 6), "datagram in the game's buffer");
    CHECK(g_udp_src[0] == 127 && g_udp_src[3] == 1 && g_udp_sport == pport, "source %u.%u.%u.%u:%u", g_udp_src[0],
          g_udp_src[1], g_udp_src[2], g_udp_src[3], g_udp_sport);
    CHECK(call(hle_net_udp_close, (uint32_t)h, 0, 0, 0, 0, 0, 0) == 0, "UDP close");
    close(peer);
}

static const char* const k_dated = "build/tests/net/card_a.before-online-20261123-200509.raw";
static const char* const k_first = "build/tests/net/card_a.before-online.raw";

/* run 2 (--fresh): no first backup yet, net_dns empty: the first backup is written; the default list */
static int fresh_run(void) {
    remove(k_first);
    remove(k_dated);
    net_init();
    call(hle_net_stack_init, 0, 0, 0, 0, 0, 0, 0);
    CHECK(g_backups == 2 && file_is(k_first, "NEW CARD") && file_is(k_dated, "NEW CARD"),
          "no first backup: it is written, and the dated one (%d)", g_backups);
    call(hle_net_stack_init, 0, 0, 0, 0, 0, 0, 0);
    CHECK(g_backups == 2, "once a session (%d)", g_backups);
    NetStatus ns;
    net_status(&ns);
    CHECK(!strcmp(ns.dns, "3.18.217.27, then 144.217.87.48"), "default DNS list \"%s\"", ns.dns);
    printf("net_native_test --fresh: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}

int main(int argc, char** argv) {
    int fresh = 0;
    for (int i = 1; i < argc; i++) {
        g_verbose |= !strcmp(argv[i], "-v");
        fresh |= !strcmp(argv[i], "--fresh");
    }
    snprintf(g_save_path, sizeof g_save_path, "build/tests/net/card_a.raw");
    mem_init();
    reset_cpu();
    if (fresh) return fresh_run();
    test_codec();
    /* a good first backup from an earlier session: kept */
    FILE* f = fopen(k_first, "wb");
    if (f) { fputs("GOOD OLD CARD", f); fclose(f); }
    remove(k_dated);

    uint16_t dport;
    g_dns_fd = udp_socket(&dport);
    uint16_t sport;
    g_silent_fd = udp_socket(&sport);
    snprintf(g_settings.net_dns, sizeof g_settings.net_dns, "127.0.0.1:%u,127.0.0.1:%u", sport, dport);
    pthread_t td, ts;
    pthread_create(&td, NULL, dns_server, NULL);
    pthread_create(&ts, NULL, silent_server, NULL);

    g_tcp_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(g_tcp_listen, (struct sockaddr*)&a, sizeof a);
    listen(g_tcp_listen, 4);
    socklen_t l = sizeof a;
    getsockname(g_tcp_listen, (struct sockaddr*)&a, &l);
    pthread_t tt;
    pthread_create(&tt, NULL, tcp_server, NULL);

    net_init();
    CHECK(call(hle_net_adapter_type, 0x80300300u, 0, 0, 0, 0, 0, 0) == 0 && ld32(0x80300300u) == 5, "adapter type 5");
    CHECK(call(hle_net_link_status, 0, 0, 0, 0, 0, 0, 0) == 1, "link up");
    CHECK(g_backups == 0, "no backup before the network start");
    call(hle_net_stack_init, 0, 0, 0, 0, 0, 0, 0);
    CHECK(file_is(k_first, "GOOD OLD CARD") && file_is(k_dated, "NEW CARD") && g_backups == 1,
          "existing first backup kept, this session's dated one written (%d)", g_backups);
    st32(0x80300310u, 0); st32(0x80300314u, 0);
    CHECK(call(hle_net_dhcp_request, 0, 0, 0, 0, 0x80300310u, 0x80300314u, 0) == 0 && ld32(0x80300310u) != 0 &&
          ld32(0x80300314u) == 0xFFFFFF00u, "DHCP address %08X mask %08X", ld32(0x80300310u), ld32(0x80300314u));
    st32(0x80300320u, 0x11111111);
    CHECK((int)call(hle_net_ppp_status, 0x80300320u, 0, 0, 0, 0, 0, 0) == -1 && ld32(0x80300324u) == 0, "PPP status none");
    test_dns();
    test_tcp(ntohs(a.sin_port));
    test_udp();
    CHECK(!g_bad_frame, "callbacks ran in a frame below the caller's stack pointer (%d bad)", g_bad_frame);
    NetStatus ns;
    net_status(&ns);
    CHECK(ns.sockets_ok && ns.dns[0], "status for the menu: %s / %s / %s", ns.state, ns.dns, ns.console_ip);
    printf("net_native_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}

/* Online play: the game's network library (Network SDK avetcp.a 1.16.1 on ACCESS AVE-TCP, and ntUtl's adapter
 * query) replaced at its API by native sockets (docs/online-research.md, "Findings: network library map").
 *
 * No Broadband Adapter is modelled and the game's own TCP/IP stack never runs: config/GPOE8P/hle.txt points
 * every library function the game's code calls at a function here. The game's code above the library (its
 * connection manager 0x80145A44-0x801478C4, its socket classes 0x80078110-0x80078FC0, the protocol) runs as
 * recompiled. What the game sees:
 *   - adapter: a Broadband Adapter (ntUtl type 5), link up while the system has a connection;
 *   - DHCP: the system's own address, mask, gateway and DNS servers (Switch: nifm; elsewhere the address the
 *     host routes from, mask 255.255.255.0 and gateway .1, which only the game's screens show);
 *   - DNS: for the game's three server names (game04.st-pso.games.sega.net, ss702.sonicteam.com,
 *     ddplanet.sega.co.jp: the table at 0x804A6FD8) the game gets the address of settings.ini net_server
 *     (default psobb.dyndns.org, the name ragol.org's Action Replay code writes over game04; or an IPv4
 *     address used as it is), looked up with the system's resolver (Switch: its DNS servers from nifm with a
 *     standard A query of our own; PC: getaddrinfo). If that fails, the game's own name is asked of Schtserv's
 *     DNS 3.18.217.27 (named by a Schtserv admin, forum post of Sep 2023), then of 144.217.87.48 when the
 *     first gives no answer after 2 queries. settings.ini net_dns (one address, or two with a comma) replaces
 *     all of that: the game's name is asked of it only. Other names: the system's resolver;
 *   - TCP and UDP: non-blocking BSD sockets (libnx on the Switch, Winsock on Windows). Handles keep the
 *     library's ranges (TCP 0..3, UDP 0x400 + n), address arguments its {u32 length 4, 4 address bytes in
 *     network order} structures, ports are plain numbers (the library's DNS opens port 0x35 = 53).
 * Receive callbacks (TCP 0x80078578: (length or error, handle); UDP 0x80078378: two-phase, (0, info) for the
 * buffer then (1, info)) ran on the stack's own thread on the hardware. Here they run on the game thread, from
 * net_frame() at the end of the game's display flip (frame_pacing.c), on the calling thread's own registers
 * and guest stack (a small frame below its stack pointer), exactly as a call from game code. A TCP receive is
 * one-shot, as the library's: one callback per arming (the game re-arms from its update, 0x801DEA7C).
 * Nothing here waits: connect and DNS are polled (the game's own loops call the status and poll functions
 * every frame), and a send the socket cannot take at once is queued and sent from the pump.
 * Sockets and nifm start at the game's first network start (avetcp init), not at boot: offline play runs
 * as it did before this file. The memory card is backed up at that point too (backup_card).
 * All of it runs on guest threads, one at a time (sched.c), so no locking; the menu reads a status line under
 * a mutex. Log lines start with [net]. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "chat_kbd.h"
#include "net_dns.h"
#include "net_native.h"
#include "runtime.h"
#include "settings.h"

/* ---------------------------------------------------------------- sockets, portable */
#if defined(_WIN32)
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
static int sock_errno(void) { return WSAGetLastError(); }
static int err_wouldblock(int e) { return e == WSAEWOULDBLOCK; }
static int err_inprogress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static void sock_close(sock_t s) { closesocket(s); }
static int sock_nonblock(sock_t s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on); }
/* 1 when a connect in progress has finished (connected or failed), 0 not yet */
static int sock_connect_done(sock_t s) {
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    struct timeval tv = {0, 0};
    return select(0, NULL, &w, &e, &tv) > 0;
}
#define SHUT_WR SD_SEND
#define SEND_FLAGS 0
typedef int sock_len;
#else
typedef int sock_t;
#define BAD_SOCK (-1)
static int sock_errno(void) { return errno; }
static int err_wouldblock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
static int err_inprogress(int e) { return e == EINPROGRESS || e == EAGAIN || e == EWOULDBLOCK; }
static void sock_close(sock_t s) { close(s); }
static int sock_nonblock(sock_t s) {
    int fl = fcntl(s, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(s, F_SETFL, fl | O_NONBLOCK);
}
static int sock_connect_done(sock_t s) {
    struct pollfd p = {.fd = s, .events = POLLOUT};
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLOUT | POLLERR | POLLHUP));
}
typedef socklen_t sock_len;
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL  /* Linux: a closed connection gives EPIPE, not SIGPIPE */
#else
#define SEND_FLAGS 0
#endif
#endif

static const char* err_name(int e) {
#if defined(_WIN32)
    switch (e) {
    case WSAECONNREFUSED: return "connection refused";
    case WSAECONNRESET: return "connection reset";
    case WSAETIMEDOUT: return "timed out";
    case WSAENETUNREACH: return "network unreachable";
    case WSAEHOSTUNREACH: return "host unreachable";
    case WSAEADDRINUSE: return "address in use";
    default: return "error";
    }
#else
    return strerror(e);
#endif
}

/* a socket error on an open connection -> the library's code the game is given (net_native.h) */
int net_error_code(int e) {
#if defined(_WIN32)
    if (e == WSAETIMEDOUT || e == WSAENETUNREACH || e == WSAEHOSTUNREACH || e == WSAENETDOWN) return -7;
#else
    if (e == ETIMEDOUT || e == ENETUNREACH || e == EHOSTUNREACH || e == ENETDOWN) return -7;  /* API_UNREACHABLE */
#endif
    return -6;  /* API_CONNRESET: reset, broken pipe, aborted, anything else */
}

static void sa_make(struct sockaddr_in* sa, const uint8_t ip[4], uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    memcpy(&sa->sin_addr, ip, 4);
}

/* ---------------------------------------------------------------- platform hooks (net_native.h) */
#if !defined(__SWITCH__)
int platform_net_init(void) {
#if defined(_WIN32)
    WSADATA w;
    int r = WSAStartup(MAKEWORD(2, 2), &w);
    if (r) rt_log("[net] WSAStartup failed (%d)", r);
    return r ? -1 : 0;
#else
    return 0;
#endif
}
void platform_net_exit(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}
int platform_net_ipconfig(NetIpConfig* out) { (void)out; return 0; }  /* unknown: net_native.c works it out */
int platform_net_link(void) { return -1; }
#endif

/* ---------------------------------------------------------------- state */
enum { MAX_TCP = 4, MAX_UDP = 16, MAX_DNS = 8, UDP_BASE = 0x400 };
/* the library's error numbers (strings at 0x8048D3F8, by -code): the game logs and maps them */
enum { API_PENDING = -1, API_PORTFULL = -2, API_DUPSOCK = -3, API_INVALIDHANDLE = -4, API_SENDPENDING = -5,
       API_CONNRESET = -6, API_UNREACHABLE = -7, API_RECVFIN = -8, API_RECVPENDING = -9, API_TIMEOUT = -11,
       API_NOBUF = -12 };
/* DNS poll results the game tells apart (0x80078E6C): -0x45 and its own 5400-poll limit give one code
 * (inferred: timeout), -0x42 another (inferred: no such name) */
enum { DNS_ERR_NONAME = -0x42, DNS_ERR_TIMEOUT = -0x45, DNS_ERR_NOSLOT = -0x40 };
/* TCP states the status call reports (the library's TCB state: 4 = established, the game waits for >= 4) */
enum { TS_CLOSED = 0, TS_SYN_SENT = 2, TS_ESTABLISHED = 4, TS_CLOSE_WAIT = 5 };
#define TX_LIMIT (1u << 20)        /* queued send data beyond this: the connection is stuck */
#define CONNECT_TIMEOUT_NS 20000000000ull
#define DNS_RETRY_NS 1000000000ull     /* a query again after 1 s without an answer */
#define DNS_NEXT_QUERIES 2             /* queries to one server before the next server in the list is asked */
#define DNS_TIMEOUT_NS 6500000000ull   /* the whole lookup (the game itself waits up to 5400 frames) */
#define DNS_SERVERS_DEFAULT "3.18.217.27,144.217.87.48"  /* Schtserv's current DNS, then its older address */
#define MAX_DNS_SERVERS 2

typedef struct Tcp {
    int used;
    sock_t fd;
    int state;              /* TS_*, or a negative API_* code: the connect failed */
    uint8_t ip[4];
    uint16_t port, lport;
    uint64_t t_connect;
    uint32_t cb, buf;       /* armed receive: the guest callback and buffer */
    int armed, armed_len;
    int end;                /* 0, or the code to deliver once armed: API_RECVFIN (peer closed), API_CONNRESET, ... */
    int end_logged;
    char end_why[48];       /* set when the port ends it (sleep, network lost) */
    uint8_t* tx;
    size_t txn, txcap;
    uint64_t sent, recvd;
} Tcp;
typedef struct Udp {
    int used;
    sock_t fd;
    uint16_t lport, pport;
    uint8_t peer[4];
    uint32_t cb;
} Udp;
typedef struct Dns {
    int used;
    int result;             /* 1 pending, 0 done (ip), < 0 a DNS_ERR_* code */
    char name[256];
    uint8_t servers[MAX_DNS_SERVERS][4];
    uint16_t sports[MAX_DNS_SERVERS];
    int nservers, cur;      /* the server list for this lookup and the one being asked now */
    int cur_sends;          /* queries sent to servers[cur] */
    sock_t fd;
    uint16_t qid;
    uint64_t t0, t_sent;    /* the lookup's start, the last query */
    uint64_t t_stage, stage_timeout;
    int sends, stage;       /* queries in this stage; ST_* */
    char qname[256];        /* the name asked now (the game's, or net_server) */
    uint8_t ip[4];
    const char* via;        /* for the log */
} Dns;

static Tcp g_tcp[MAX_TCP];
static Udp g_udp[MAX_UDP];
static Dns g_dns[MAX_DNS];
static int g_sock_ok;               /* platform_net_init succeeded */
static int g_open;                  /* sockets or lookups in use: net_frame has work */
static NetIpConfig g_cfg;           /* what the DHCP calls report */
static int g_cfg_from_system;
static uint8_t g_dns_server[MAX_DNS_SERVERS][4];  /* settings.ini net_dns, or DNS_SERVERS_DEFAULT */
static uint16_t g_dns_port[MAX_DNS_SERVERS];
static int g_dns_count;
static int g_dns_override;          /* net_dns was set */
static int g_net_started;           /* platform_net_init tried (the game's first network start) */
static uint64_t g_stat_t0, g_stat_tx, g_stat_rx, g_stat_utx, g_stat_urx;
static int g_link_cached = -2;
static uint64_t g_link_t;

/* the session summary ([net] session end): from the game's network start to its stop, or to the exit */
static struct {
    int active;
    uint64_t t0;
    unsigned connects, lookups, disconnects;
    unsigned long long tx, rx, utx, urx;
    char last_end[48];
} g_sess;
static _Atomic int g_ev_resume, g_ev_focus;  /* platform events (net_platform_event), handled in net_frame */
static uint64_t g_focus_lost_ns, g_link_check_ns;

static mtx_t g_status_lock;
static char g_status[128] = "not used yet";
static char g_server_str[64];

static void set_status(const char* fmt, ...) {
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    mtx_lock(&g_status_lock);
    memcpy(g_status, b, sizeof b);
    mtx_unlock(&g_status_lock);
}

static int server_setting_off(void) {  /* net_server=off: the game's names go straight to the DNS servers */
    const char* v = g_settings.net_server;
    return !strcmp(v, "off") || !strcmp(v, "OFF") || !strcmp(v, "none");
}

void net_status(NetStatus* out) {
    mtx_lock(&g_status_lock);
    snprintf(out->state, sizeof out->state, "%s", g_status);
    mtx_unlock(&g_status_lock);
    snprintf(out->dns, sizeof out->dns, "%s", g_server_str);
    if (g_dns_override) snprintf(out->server, sizeof out->server, "not used (net_dns is set)");
    else if (server_setting_off()) snprintf(out->server, sizeof out->server, "off");
    else snprintf(out->server, sizeof out->server, "%s", g_settings.net_server[0] ? g_settings.net_server : SETTINGS_NET_SERVER_DEFAULT);
    snprintf(out->console_ip, sizeof out->console_ip, "%u.%u.%u.%u%s", g_cfg.ip[0], g_cfg.ip[1], g_cfg.ip[2],
             g_cfg.ip[3], g_cfg_from_system ? "" : " (worked out)");
    out->sockets_ok = g_sock_ok;
    out->started = g_net_started;
}

#define IP4(a) (a)[0], (a)[1], (a)[2], (a)[3]

/* every replaced function logs its first call (with the game's return address) */
#define FIRST_CALL(name) do {                                                              \
        static int seen_;                                                                  \
        if (!seen_) { seen_ = 1; rt_log("[net] first call: %s (from %08X)", name, c->lr); } \
    } while (0)

static int span(uint32_t ea, uint32_t n) { return ea && guest_span_ok(ea, n); }
static void put_bytes(uint32_t ea, const void* p, uint32_t n) { if (span(ea, n)) mem_write_bytes(ea, p, n); }
static void get_bytes(uint32_t ea, void* p, uint32_t n) {
    if (span(ea, n)) mem_read_bytes(ea, p, n);
    else memset(p, 0, n);
}
static void put32(uint32_t ea, uint32_t v) { if (span(ea, 4)) st32(ea, v); }
static void put16(uint32_t ea, uint16_t v) { if (span(ea, 2)) st16(ea, v); }
static uint32_t get32(uint32_t ea) { return span(ea, 4) ? ld32(ea) : 0; }
static uint16_t get16(uint32_t ea) { return span(ea, 2) ? ld16(ea) : 0; }
/* an address argument {u32 4, 4 address bytes}: 1 and the bytes when it holds an IPv4 address */
static int get_addr(uint32_t ea, uint8_t ip[4]) {
    memset(ip, 0, 4);
    if (!span(ea, 8) || ld32(ea) != 4) return 0;
    mem_read_bytes(ea + 4, ip, 4);
    return 1;
}
static void put_addr(uint32_t ea, const uint8_t ip[4]) {
    put32(ea, 4);
    put_bytes(ea + 4, ip, 4);
}

/* ---------------------------------------------------------------- guest callbacks */
/* calls the guest function `fn` with r3, r4 on the calling thread, as a call from game code would: its own
 * registers (saved and put back after), a 0x80-byte frame below its stack pointer with a back chain. Returns
 * the callee's r3. extra_ea (if nonzero) gets the address of 0x40 free bytes inside that frame. */
static uint32_t frame_base(Cpu* c) { return (c->r[1] - 0x80) & ~0xFu; }
static uint32_t call_guest3(Cpu* c, uint32_t fn, uint32_t r3, uint32_t r4, uint32_t r5) {
    uint32_t r[32], lr = c->lr, ctr = c->ctr, fpscr = c->fpscr;
    uint8_t cr[32], so = c->xer_so, ov = c->xer_ov, ca = c->xer_ca, bc = c->xer_bc;
    struct { double ps0, ps1; } f[32];
    memcpy(r, c->r, sizeof r);
    memcpy(cr, c->cr, sizeof cr);
    memcpy(f, c->f, sizeof f);
    uint32_t sp = frame_base(c);
    put32(sp, c->r[1]);  /* back chain */
    c->r[1] = sp;
    c->r[3] = r3;
    c->r[4] = r4;
    c->r[5] = r5;
    guest_call(c, fn);
    uint32_t ret = c->r[3];
    memcpy(c->r, r, sizeof r);
    memcpy(c->cr, cr, sizeof cr);
    memcpy(c->f, f, sizeof f);
    c->lr = lr; c->ctr = ctr; c->fpscr = fpscr;
    c->xer_so = so; c->xer_ov = ov; c->xer_ca = ca; c->xer_bc = bc;
    return ret;
}
static uint32_t call_guest(Cpu* c, uint32_t fn, uint32_t r3, uint32_t r4) { return call_guest3(c, fn, r3, r4, 0); }

/* ---------------------------------------------------------------- start-up, settings */
static void count_open(void) {
    int n = 0;
    for (int i = 0; i < MAX_TCP; i++) n += g_tcp[i].used;
    for (int i = 0; i < MAX_UDP; i++) n += g_udp[i].used;
    for (int i = 0; i < MAX_DNS; i++) n += g_dns[i].used;
    g_open = n;
}

/* "a.b.c.d[:port][,a.b.c.d[:port]]": 1 and the list, 0 when any part is not an IPv4 address */
static int parse_server_list(const char* text) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", text);
    int n = 0;
    for (char* part = buf; part && n < MAX_DNS_SERVERS;) {
        char* comma = strchr(part, ',');
        if (comma) *comma = 0;
        while (*part == ' ') part++;
        uint16_t port = 0;
        if (!dns_parse_ipv4(part, g_dns_server[n], &port)) return 0;
        g_dns_port[n++] = port ? port : 53;
        part = comma ? comma + 1 : NULL;
    }
    g_dns_count = n;
    return n > 0;
}

static void read_dns_setting(void) {
    g_dns_override = g_settings.net_dns[0] != 0;
    if (!g_dns_override || !parse_server_list(g_settings.net_dns)) {
        if (g_dns_override)
            rt_log("[net] settings.ini net_dns=\"%s\" is not one or two IPv4 addresses (a.b.c.d or a.b.c.d:port, a comma "
                   "between two): using the default %s", g_settings.net_dns, DNS_SERVERS_DEFAULT);
        g_dns_override = 0;
        /* SR_NET_FALLBACK_DNS (env.txt on the Switch; testing): another list in place of Schtserv's */
        const char* env = getenv("SR_NET_FALLBACK_DNS");
        if (!(env && *env && parse_server_list(env))) parse_server_list(DNS_SERVERS_DEFAULT);
    }
    size_t k = 0;
    g_server_str[0] = 0;
    for (int i = 0; i < g_dns_count && k < sizeof g_server_str; i++) {
        int w = g_dns_port[i] == 53 ? snprintf(g_server_str + k, sizeof g_server_str - k, "%s%u.%u.%u.%u", i ? ", then " : "",
                                               IP4(g_dns_server[i]))
                                    : snprintf(g_server_str + k, sizeof g_server_str - k, "%s%u.%u.%u.%u:%u", i ? ", then " : "",
                                               IP4(g_dns_server[i]), g_dns_port[i]);
        if (w > 0) k += (size_t)w;
    }
}

/* the address this machine sends from (a UDP socket "connected" to the DNS server sends nothing) */
static int local_address(uint8_t ip[4]) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCK) return 0;
    struct sockaddr_in sa;
    sa_make(&sa, g_dns_server[0], g_dns_port[0]);
    int ok = 0;
    if (connect(s, (struct sockaddr*)&sa, sizeof sa) == 0) {
        struct sockaddr_in me;
        sock_len n = sizeof me;
        if (getsockname(s, (struct sockaddr*)&me, &n) == 0 && me.sin_addr.s_addr) {
            memcpy(ip, &me.sin_addr, 4);
            ok = 1;
        }
    }
    sock_close(s);
    return ok;
}

static void refresh_ipconfig(const char* when) {
    NetIpConfig c;
    memset(&c, 0, sizeof c);
    if (g_sock_ok && platform_net_ipconfig(&c) && (c.ip[0] | c.ip[1] | c.ip[2] | c.ip[3])) {
        g_cfg = c;
        g_cfg_from_system = 1;
        rt_log("[net] %s: this console is %u.%u.%u.%u, mask %u.%u.%u.%u, gateway %u.%u.%u.%u, DNS %u.%u.%u.%u / "
               "%u.%u.%u.%u (system settings)", when, IP4(c.ip), IP4(c.mask), IP4(c.gateway), IP4(c.dns1), IP4(c.dns2));
        return;
    }
    g_cfg_from_system = 0;
    memset(&g_cfg, 0, sizeof g_cfg);
    if (!(g_sock_ok && local_address(g_cfg.ip))) {
        static const uint8_t fixed[4] = {192, 168, 0, 100};
        memcpy(g_cfg.ip, fixed, 4);
    }
    static const uint8_t mask[4] = {255, 255, 255, 0};
    memcpy(g_cfg.mask, mask, 4);
    memcpy(g_cfg.gateway, g_cfg.ip, 3);
    g_cfg.gateway[3] = 1;
    memcpy(g_cfg.dns1, g_dns_server[0], 4);
    memcpy(g_cfg.dns2, g_dns_server[g_dns_count > 1], 4);
    rt_log("[net] %s: no address from the system; the game is told %u.%u.%u.%u, mask %u.%u.%u.%u, gateway "
           "%u.%u.%u.%u (worked out; only the game's screens show these)", when, IP4(g_cfg.ip), IP4(g_cfg.mask),
           IP4(g_cfg.gateway));
}

static void session_end(const char* when) {
    if (!g_sess.active) return;
    g_sess.active = 0;
    rt_log("[net] session end (%s): %.0f s since the network start, %u lookups, %u connections, %u disconnects; TCP "
           "%llu bytes sent, %llu received; UDP %llu sent, %llu received", when, (platform_now_ns() - g_sess.t0) / 1e9,
           g_sess.lookups, g_sess.connects, g_sess.disconnects, g_sess.tx + g_stat_tx, g_sess.rx + g_stat_rx,
           g_sess.utx + g_stat_utx, g_sess.urx + g_stat_urx);
    if (g_sess.last_end[0]) rt_log("[net]   last disconnect: %s", g_sess.last_end);
}

static void net_exit(void) {
    session_end("at exit");
    for (int i = 0; i < MAX_TCP; i++)
        if (g_tcp[i].used && g_tcp[i].fd != BAD_SOCK) sock_close(g_tcp[i].fd);
    for (int i = 0; i < MAX_UDP; i++)
        if (g_udp[i].used && g_udp[i].fd != BAD_SOCK) sock_close(g_udp[i].fd);
    for (int i = 0; i < MAX_DNS; i++)
        if (g_dns[i].used && g_dns[i].fd != BAD_SOCK) sock_close(g_dns[i].fd);
    memset(g_tcp, 0, sizeof g_tcp);
    memset(g_udp, 0, sizeof g_udp);
    memset(g_dns, 0, sizeof g_dns);
    g_open = 0;
    if (g_sock_ok) platform_net_exit();
    g_sock_ok = 0;
}

void net_init(void) {
    mtx_init(&g_status_lock, mtx_plain);
    read_dns_setting();
    rt_log("[net] native sockets in place of the game's network library; started when the game first starts its "
           "network. Server names asked of DNS %s (%s)", g_server_str, g_dns_override ? "settings.ini net_dns" : "default");
}

/* the game's first network start: the platform's sockets (Switch: bsd:u and nifm) */
static void net_start(void) {
    if (g_net_started) return;
    g_net_started = 1;
    g_sock_ok = platform_net_init() == 0;
    rt_log("[net] sockets %s", g_sock_ok ? "ready" : "NOT available (online play will fail)");
    if (g_sock_ok) rt_atexit(net_exit);
}

/* ---------------------------------------------------------------- save safety */
/* At the game's first network start of a session (before it writes its network settings file or goes online):
 *   saves/card_a.before-online.raw            the first one ever; never overwritten
 *   saves/card_a.before-online-YYYYMMDD-HHMMSS.raw  one per session (a new name each time; the player deletes old ones)
 * both the in-memory card image, which the card file catches up with 0.5 s after the game's last write. */
static int g_backup_done;
static void backup_card(void) {
    if (g_backup_done) return;
    g_backup_done = 1;
    char base[512], first[560], dated[600];
    snprintf(base, sizeof base, "%s", platform_save_path());
    size_t n = strlen(base);
    if (n > 4 && !strcmp(base + n - 4, ".raw")) base[n - 4] = 0;
    snprintf(first, sizeof first, "%s.before-online.raw", base);
    int y, mo, d, h, mi, se;
    platform_local_time(&y, &mo, &d, &h, &mi, &se);
    snprintf(dated, sizeof dated, "%s.before-online-%04d%02d%02d-%02d%02d%02d.raw", base, y, mo, d, h, mi, se);
    FILE* f = fopen(first, "rb");
    int have_first = f != NULL;
    if (f) fclose(f);
    if (!have_first) {
        int r = exi_card_backup(first);
        if (r == 1) { rt_log("[net] no memory card file in use (a fresh card in memory): no backup made"); return; }
        rt_log(r == 0 ? "[net] memory card backed up to %s (the first backup; it is never overwritten)"
                      : "[net] could NOT write the memory card backup %s (online play goes ahead)", first);
    } else {
        rt_log("[net] %s exists: kept as it is", first);
    }
    int r = exi_card_backup(dated);
    if (r == 1) rt_log("[net] no memory card file in use (a fresh card in memory): no backup made");
    else rt_log(r == 0 ? "[net] memory card backed up to %s (this session's backup)"
                       : "[net] could NOT write the memory card backup %s (online play goes ahead)", dated);
}

/* ---------------------------------------------------------------- adapter, link, stack lifecycle */
/* ntUtl 0x8042EE4C(int* type): 5 = Broadband Adapter (what 0x8014618C and 0x80145DB4 accept), cached as the
 * original caches it (sdata 0x805D7CC8); returns 0 */
void hle_net_adapter_type(Cpu* c) {
    FIRST_CALL("adapter type (reports a Broadband Adapter)");
    put32(c->r[3], 5);
    put32(0x805D7CC8, 5);
    c->r[3] = 0;
}

/* 0x80426BD8 link status: 1 linked, 0 not (the system's connection; asked at most once a second, since the
 * game asks every frame while online) */
void hle_net_link_status(Cpu* c) {
    FIRST_CALL("link status");
    uint64_t now = platform_now_ns();
    if (g_link_cached == -2 || now - g_link_t > 1000000000ull) {
        int l = !g_net_started ? -1 : g_sock_ok ? platform_net_link() : 0;  /* -1 before the network start */
        int up = l != 0;  /* -1: the platform cannot tell (PC): up */
        if (up != g_link_cached) rt_log("[net] link %s%s", up ? "up" : "DOWN", l < 0 ? " (assumed: no system status here)" :
                                                                  up ? "" : " (the system has no network connection)");
        g_link_cached = up;
        g_link_t = now;
    }
    c->r[3] = (uint32_t)g_link_cached;
}

void hle_net_media_mode(Cpu* c) {  /* 0x80426C34(mode): the game passes 4; nothing to set natively */
    FIRST_CALL("media mode");
    rt_log("[net] media mode %d (ignored)", (int)c->r[3]);
    c->r[3] = 0;
}

void hle_net_stack_memory(Cpu* c) {  /* 0x803F8470(heap, size): the game's memory for the stack; unused */
    FIRST_CALL("stack memory");
    rt_log("[net] the game gives the network library %u bytes at %08X (not used: native sockets)", c->r[4], c->r[3]);
    c->r[3] = 0;
}

void hle_net_stack_init(Cpu* c) {  /* 0x80421674: avetcp init (the BBA driver attach and the stack's threads) */
    FIRST_CALL("stack init");
    backup_card();
    net_start();
    rt_log("[net] network library init (from %08X): native sockets %s", c->lr, g_sock_ok ? "ready" : "NOT available");
    if (!g_sess.active) {
        memset(&g_sess, 0, sizeof g_sess);
        g_sess.active = 1;
        g_sess.t0 = platform_now_ns();
    }
    chat_set_online(g_sock_ok);
    refresh_ipconfig("network start");
    set_status("network started (%u.%u.%u.%u)", IP4(g_cfg.ip));
    c->r[3] = 0;
}

void hle_net_stack_term(Cpu* c) {  /* 0x80421620: avetcp term */
    FIRST_CALL("stack term");
    int n = 0;
    for (int i = 0; i < MAX_TCP; i++) n += g_tcp[i].used;
    for (int i = 0; i < MAX_UDP; i++) n += g_udp[i].used;
    rt_log("[net] network library shut down by the game (%d sockets still open)", n);
    session_end("the game stopped its network");
    chat_set_online(0);
    set_status("network stopped");
    c->r[3] = 0;
}

/* module and interface set-up and tear-down with no native counterpart (their callers want 0; the interface
 * open 0x8040876C returns the interface id the game keeps at 0x805D2D00: 0) */
#define NET_OK(fn, label) void hle_net_##fn(Cpu* c) { FIRST_CALL(label); c->r[3] = 0; }
NET_OK(module_init, "module init 0x804073B8")
NET_OK(module_term, "module term 0x80407358")
NET_OK(iface_open, "interface open 0x8040876C")
NET_OK(iface_close, "interface close 0x8040874C")
NET_OK(ip_init, "module init 0x804150EC")
NET_OK(ip_term, "module term 0x8041509C")
NET_OK(dhcp_start, "DHCP start")
NET_OK(dhcp_hostname, "DHCP host name")
NET_OK(dhcp_release, "DHCP release")
NET_OK(dhcp_stop, "DHCP stop")
NET_OK(if_addr_remove, "interface address remove")
NET_OK(route_delete, "default route delete")
NET_OK(dns_init, "resolver init")
NET_OK(dns_term, "resolver shutdown")
NET_OK(dns_clear, "DNS server list clear")
NET_OK(ppp_term, "PPP term")
NET_OK(ppp_mode, "PPPoE mode")
NET_OK(ppp_modem, "modem parameters")
NET_OK(ppp_disconnect, "PPP disconnect")

/* ---------------------------------------------------------------- network settings (the setup browser's file) */
/* 0x80145A94, the game's network settings load (boot 0x80146218; the online menu 0x8032C664; the browser
 * scene 0x803354B4). The original starts Sega's settings storage (0x803F3BF8), then reads and checks the
 * card's network settings file (written only by the disc's setup browser, switcher.dol / iwanaga.dol), and
 * returns 1 when it was read and the adapter is a BBA. With no such file the online menu starts the browser
 * (scene 19, 0x8032C678), which cannot run here. This replacement starts the storage as the original does,
 * leaves the card alone, and puts in a built-in profile through the storage's own layout (its getters
 * 0x803F5678 and the others read it):
 *   struct at *0x805D79C0 (0x19D4 bytes, defaults written by 0x803F47BC: no selection, entry i -> profile i)
 *   +0x002 s16  selected entry      = 0
 *   +0x028 s16  entry 0's profile   = 0 (the default)
 *   profile 0 (fields at struct + 0x210 * n):
 *   +0x206 s16  kind                = 4 (Broadband Adapter; 0x803F5678 and the BBA getters require 4)
 *   +0x2B0 u8   address method      = 0 (DHCP: 0x80145EE4 returns 1 for 0; 1 is PPPoE, 0x80145E90)
 *   everything else 0: no fixed address, mask, gateway or DNS servers (the game then takes DNS from DHCP,
 *   0x801471A0), no host name, no proxy.
 * Then the storage's state 2 (what a successful read leaves, 0x803F3EA0) and the game's "settings loaded" bit
 * (0x805D56CC bit 0). The game never writes this file (no game call reaches the storage's write functions). */
void hle_net_settings_load(Cpu* c) {
    FIRST_CALL("network settings load (built-in profile)");
    put16(0x805D56CC, 0);
    put32(0x805D7CC8, 5);  /* the adapter type cache (the original asks 0x8042EE4C here) */
    if (get16(0x805D56CE)) {
        call_guest3(c, 0x803F3D60, 0, 0, 0);  /* the storage's term, as the original */
        put16(0x805D56CE, 0);
    }
    /* the storage init (0, the game's buffer from 0x801461EC, adapter type: a BBA (5) is passed as 4) */
    int r = (int32_t)call_guest3(c, 0x803F3BF8, 0, get32(0x805D56C8), 4);
    put16(0x805D56CE, 1);
    uint32_t s = get32(0x805D79C0);
    if (r != 0 || !span(s, 0x19D4)) {
        rt_log("[net] network settings: the game's settings storage did not start (%d, struct %08X): no settings", r, s);
        c->r[3] = 0;
        return;
    }
    put16(s + 0x002, 0);
    put16(s + 0x028, 0);
    put16(s + 0x206, 4);
    if (span(s + 0x2B0, 1)) st8(s + 0x2B0, 0);
    put32(0x805D79A4, 2);
    put16(0x805D56CC, (uint16_t)(get16(0x805D56CC) | 1));
    static int logged;
    if (!logged) {
        logged = 1;
        rt_log("[net] network settings: built-in profile (Broadband Adapter, DHCP, DNS from DHCP); the memory card's "
               "network settings file is neither read nor written");
    }
    c->r[3] = 1;
}

/* ---------------------------------------------------------------- other programs on the disc */
/* 0x8007F970(page): starts the disc's web browser (switcher.dol, then iwanaga.dol) at a page ("file:/sonic/
 * index.html" for the Website, "file:/sonic/config.htm" for the network setup): it stops the video, audio and
 * the GX (0x8037CA40, 0x8037C8B8, 0x8037BC54) and waits in 0x8038A058 for good. Not run here. In the browser
 * scene (19, which does nothing but this) the game goes back to the scene it came from, through its own scene
 * change (0x8000F9CC with the previous scene, 0x804FE120 + 0x14); from anywhere else it just returns. */
void hle_boot_browser(Cpu* c) {
    FIRST_CALL("start the disc's web browser");
    char page[64] = "";
    if (span(c->r[3], 1)) {
        for (int i = 0; i < 63 && span(c->r[3] + (uint32_t)i, 1); i++) {
            page[i] = (char)ld8(c->r[3] + (uint32_t)i);
            if (!page[i]) break;
        }
        page[63] = 0;
    }
    uint32_t scene = get32(0x804FE120), prev = get32(0x804FE120 + 0x14);
    static uint64_t last_log;
    static unsigned quiet;
    uint64_t now = platform_now_ns();
    if (last_log && now - last_log < 10000000000ull) {  /* at most one line per 10 s */
        quiet++;
        if (scene == 19 && prev && prev != 19) call_guest3(c, 0x8000F9CC, prev, 0, 0);
        c->r[3] = 1;
        return;
    }
    last_log = now;
    if (quiet) rt_log("[boot] (%u more browser starts in the last 10 s, not logged)", quiet);
    quiet = 0;
    rt_log("[boot] the game tried to start switcher.dol (the disc's web browser, page \"%s\", from %08X): not "
           "supported%s", page, c->lr, scene == 19 && prev ? "; back to the previous screen" : "");
    if (scene == 19 && prev && prev != 19) call_guest3(c, 0x8000F9CC, prev, 0, 0);
    c->r[3] = 1;
}

/* OSResetSystem 0x80373C80: a reset or a return to the system menu (the game's exit 0x8000F8D8, the DOL chain
 * load 0x8042E914, and 0x801E2C7C / 0x801E2CB8). Logged, then the original runs (it does not return here). */
void f_80373C80_orig(Cpu* c);
void hle_boot_reset(Cpu* c) {
    rt_log("[boot] the game asked for a system reset (OSResetSystem(%d, 0x%X, %d) from %08X): not supported, the "
           "game stops here; close it from the HOME menu", (int)c->r[3], c->r[4], (int)c->r[5], c->lr);
    fflush(stderr);
    f_80373C80_orig(c);
}

/* ---------------------------------------------------------------- DHCP and IP set-up (BBA path 0x80146B08) */
/* 0x80424D58(iface, 0, 0, block, &ip, &mask, ..., 3): the lease. Returns 0 */
void hle_net_dhcp_request(Cpu* c) {
    FIRST_CALL("DHCP request");
    refresh_ipconfig("DHCP");
    put_bytes(c->r[7], g_cfg.ip, 4);
    put_bytes(c->r[8], g_cfg.mask, 4);
    set_status("got an address (%u.%u.%u.%u)", IP4(g_cfg.ip));
    c->r[3] = 0;
}
void hle_net_dhcp_gateway(Cpu* c) {  /* 0x8042489C(block, &gateway) */
    FIRST_CALL("DHCP gateway");
    put_bytes(c->r[4], g_cfg.gateway, 4);
    c->r[3] = 0;
}
void hle_net_dhcp_lease(Cpu* c) {  /* 0x8042474C(&a, &b, &c): lease times (inferred); one day, half, 7/8 */
    FIRST_CALL("DHCP lease times");
    put32(c->r[3], 86400);
    put32(c->r[4], 43200);
    put32(c->r[5], 75600);
    c->r[3] = 0;
}
void hle_net_dhcp_dns(Cpu* c) {  /* 0x80424910(block, domain, &dns1, &dns2) */
    FIRST_CALL("DHCP DNS servers");
    put_bytes(c->r[5], g_cfg.dns1, 4);
    put_bytes(c->r[6], g_cfg.dns2, 4);
    c->r[3] = 0;
}
void hle_net_dhcp_state(Cpu* c) {  /* 0x804245DC: the game (0x80139770) treats < 0 as a lost lease */
    FIRST_CALL("DHCP state");
    c->r[3] = 0;
}
void hle_net_if_addr_set(Cpu* c) {  /* 0x8042315C(iface, &{4, ip, mask, 0}) */
    FIRST_CALL("interface address set");
    uint8_t a[8];
    get_bytes(c->r[4] + 4, a, 8);
    rt_log("[net] the game sets its address %u.%u.%u.%u mask %u.%u.%u.%u (native sockets use the system's)",
           a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    c->r[3] = 0;
}
void hle_net_route_add(Cpu* c) {  /* 0x80421A50(0, 0, gateway) */
    FIRST_CALL("default route add");
    uint32_t g = c->r[5];
    rt_log("[net] the game adds its default route via %u.%u.%u.%u", g >> 24, (g >> 16) & 255, (g >> 8) & 255, g & 255);
    c->r[3] = 0;
}

/* 0x80146314, the game's PPPoE connect (its settings chose PPPoE): there is no PPP here, so it takes the DHCP
 * path instead (0x801465C0, which calls the DHCP functions above); same argument (the mask out) and results */
void f_801465C0(Cpu* c);
void hle_net_pppoe_as_dhcp(Cpu* c) {
    FIRST_CALL("PPPoE connect (done as DHCP)");
    rt_log("[net] the game's settings say PPPoE: connecting as DHCP instead (the system's own connection)");
    f_801465C0(c);
}

/* PPP and modem (only on the modem and PPPoE paths, which a Broadband Adapter with DHCP never takes): fail */
void hle_net_ppp_init(Cpu* c) { FIRST_CALL("PPP init (fails: no PPP)"); c->r[3] = (uint32_t)-1; }
void hle_net_ppp_start(Cpu* c) { FIRST_CALL("PPP connect (fails: no PPP)"); c->r[3] = (uint32_t)-1; }
void hle_net_ppp_status(Cpu* c) {  /* 0x803FB184(&status): all zero (no connection, no state); returns -1 */
    FIRST_CALL("PPP status (none)");
    static const uint8_t zero[0x20];
    put_bytes(c->r[3], zero, sizeof zero);
    c->r[3] = (uint32_t)-1;
}

/* ---------------------------------------------------------------- DNS */
static const char* const k_server_names[] = {"game04.st-pso.games.sega.net", "ss702.sonicteam.com", "ddplanet.sega.co.jp"};

static int name_is_server(const char* n) {
    for (size_t i = 0; i < sizeof k_server_names / sizeof *k_server_names; i++) {
        const char* a = k_server_names[i];
        size_t k = 0;
        for (; a[k] && n[k]; k++) {
            char x = n[k] >= 'A' && n[k] <= 'Z' ? (char)(n[k] - 'A' + 'a') : n[k];
            if (x != a[k]) break;
        }
        if (!a[k] && !n[k]) return 1;
    }
    return 0;
}

/* The game's server names go through stages (dns_next): 1. settings.ini net_server (default psobb.dyndns.org,
 * the name ragol.org's Action Replay code for US v1.02 Plus writes over game04 at 0x8043EDA0; an IPv4 address
 * there is used as it is), looked up with the system's resolver; 2. the game's own name asked of the DNS list
 * (net_dns, or Schtserv's 3.18.217.27 then 144.217.87.48). With net_dns set, only stage 2 (the override).
 * Other names: the system's resolver only. */
enum { ST_NONE, ST_SERVER, ST_DNSLIST, ST_OTHER };
#define SERVER_STAGE_TIMEOUT_NS 4500000000ull

static void dns_done(Dns* d, int result, const char* why) {
    d->result = result;
    if (d->fd != BAD_SOCK) {
        sock_close(d->fd);
        d->fd = BAD_SOCK;
    }
    unsigned ms = (unsigned)((platform_now_ns() - d->t0) / 1000000ull);
    if (result == 0) {
        rt_log("[net] DNS %s -> %u.%u.%u.%u (%s, %u ms)", d->name, IP4(d->ip), d->via, ms);
        set_status("found %s at %u.%u.%u.%u", d->name, IP4(d->ip));
    } else {
        rt_log("[net] DNS %s FAILED: %s (%s, %u ms; the game is told -0x%X)", d->name, why, d->via, ms, (unsigned)-result);
        set_status("DNS failed for %s: %s", d->name, why);
    }
}

static void dns_next(Dns* d, int result, const char* why);

static void copy_name(char dst[256], const char* src) {  /* both 256 bytes or shorter strings */
    size_t n = strlen(src);
    if (n > 255) n = 255;
    memmove(dst, src, n);
    dst[n] = 0;
}

/* a stage's own failure: on to the next stage, or the game is told */
static void dns_finish(Dns* d, int result, const char* why) {
    if (result == 0 || d->stage != ST_SERVER) { dns_done(d, result, why); return; }
    rt_log("[net] DNS %s: net_server %s failed (%s): asking the DNS servers for %s", d->name, d->qname, why, d->name);
    dns_next(d, result, why);
}

static int dns_open_socket(Dns* d) {
    if (d->fd != BAD_SOCK) return 1;
    d->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (d->fd == BAD_SOCK || sock_nonblock(d->fd)) {
        int e = sock_errno();
        char why[64];
        snprintf(why, sizeof why, "no UDP socket: errno %d (%s)", e, err_name(e));
        if (d->fd != BAD_SOCK) sock_close(d->fd);
        d->fd = BAD_SOCK;
        dns_finish(d, DNS_ERR_NONAME, why);
        return 0;
    }
    return 1;
}

static void dns_send(Dns* d) {
    uint8_t q[300];
    size_t n = dns_build_query(q, sizeof q, d->qid, d->qname);
    struct sockaddr_in sa;
    sa_make(&sa, d->servers[d->cur], d->sports[d->cur]);
    if (!n) { dns_finish(d, DNS_ERR_NONAME, "the name cannot be put in a query"); return; }
    if (sendto(d->fd, (const char*)q, (int)n, 0, (struct sockaddr*)&sa, sizeof sa) < 0) {
        int e = sock_errno();
        if (!err_wouldblock(e)) {
            char why[96];
            snprintf(why, sizeof why, "send to the DNS server: errno %d (%s)", e, err_name(e));
            dns_finish(d, DNS_ERR_NONAME, why);
            return;
        }
    }
    d->sends++;
    d->cur_sends++;
    d->t_sent = platform_now_ns();
}

/* an A query for d->qname to the servers set up in d, with retries */
static void dns_query_start(Dns* d, uint64_t timeout) {
    d->cur = 0;
    d->cur_sends = 0;
    d->sends = 0;
    d->t_stage = platform_now_ns();
    d->stage_timeout = timeout;
    d->qid = (uint16_t)((platform_now_ns() >> 10) ^ (uint64_t)(d - g_dns) * 0x9E37u ^ (uint64_t)d->stage * 0x51u);
    rt_log("[net] DNS %s: A query for %s to %u.%u.%u.%u:%u (%s%s)", d->name, d->qname, IP4(d->servers[0]), d->sports[0],
           d->via, d->nservers > 1 ? "; the next server if it does not answer" : "");
    set_status("looking up %s", d->qname);
    if (!dns_open_socket(d)) return;
    dns_send(d);
}

/* the system's resolver for d->qname: the system's DNS servers (Switch, from nifm) with our query, or
 * getaddrinfo where there are none (PC: this one call blocks, normally a few ms) */
static void dns_system(Dns* d, uint64_t timeout) {
    static const uint8_t zero[4];
    if (g_cfg_from_system && memcmp(g_cfg.dns1, zero, 4)) {
        memcpy(d->servers[0], g_cfg.dns1, 4);
        d->sports[0] = 53;
        d->nservers = 1;
        if (memcmp(g_cfg.dns2, zero, 4) && memcmp(g_cfg.dns2, g_cfg.dns1, 4)) {
            memcpy(d->servers[1], g_cfg.dns2, 4);
            d->sports[1] = 53;
            d->nservers = 2;
        }
        dns_query_start(d, timeout);
        return;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    uint64_t t = platform_now_ns();
    int r = getaddrinfo(d->qname, NULL, &hints, &res);
    unsigned ms = (unsigned)((platform_now_ns() - t) / 1000000ull);
    if (r == 0 && res) {
        memcpy(d->ip, &((struct sockaddr_in*)res->ai_addr)->sin_addr, 4);
        freeaddrinfo(res);
        rt_log("[net] DNS %s: getaddrinfo(%s) -> %u.%u.%u.%u (%u ms)", d->name, d->qname, IP4(d->ip), ms);
        dns_finish(d, 0, "");
    } else {
        if (res) freeaddrinfo(res);
        char why[64];
        snprintf(why, sizeof why, "getaddrinfo error %d after %u ms", r, ms);
        dns_finish(d, DNS_ERR_NONAME, why);
    }
}


/* to the next stage after `stage` (ST_NONE: the first), or the game is told result */
static void dns_next(Dns* d, int result, const char* why) {
    if (d->stage == ST_NONE && !g_dns_override && !server_setting_off()) {
        d->stage = ST_SERVER;
        const char* srv = g_settings.net_server[0] ? g_settings.net_server : SETTINGS_NET_SERVER_DEFAULT;
        uint16_t port = 1;
        if (dns_parse_ipv4(srv, d->ip, &port) && !port) {
            d->via = "net_server is an address";
            dns_done(d, 0, "");
            return;
        }
        copy_name(d->qname, srv);
        d->via = g_cfg_from_system ? "net_server, asked of the system's DNS servers" : "net_server, the system's resolver";
        dns_system(d, SERVER_STAGE_TIMEOUT_NS);
        return;
    }
    if (d->stage == ST_NONE || d->stage == ST_SERVER) {
        d->stage = ST_DNSLIST;
        copy_name(d->qname, d->name);
        for (int k = 0; k < g_dns_count; k++) {
            memcpy(d->servers[k], g_dns_server[k], 4);
            d->sports[k] = g_dns_port[k];
        }
        d->nservers = g_dns_count;
        d->via = g_dns_override ? "the game's server name, asked of net_dns" : "the game's server name, asked of Schtserv's DNS";
        dns_query_start(d, DNS_TIMEOUT_NS);
        return;
    }
    dns_done(d, result, why);
}

static void dns_step(Dns* d) {
    if (d->result != 1 || d->fd == BAD_SOCK) return;
    uint8_t p[1500];
    for (;;) {
        struct sockaddr_in from;
        sock_len fl = sizeof from;
        int n = (int)recvfrom(d->fd, (char*)p, sizeof p, 0, (struct sockaddr*)&from, &fl);
        if (n < 0) {
            int e = sock_errno();
            if (err_wouldblock(e)) break;
            rt_log("[net] DNS %s: receive errno %d (%s)", d->qname, e, err_name(e));
            break;  /* e.g. an ICMP port unreachable: retry and time out as usual */
        }
        int rc = 0;
        int r = dns_parse_answer(p, (size_t)n, d->qid, d->ip, &rc);
        if (r == DNS_NOT_ANSWER || r == DNS_MALFORMED) {
            rt_log("[net] DNS %s: ignored a %d-byte reply (%s)", d->qname, n, r == DNS_MALFORMED ? "malformed" : "not ours");
            continue;
        }
        rt_log("[net] DNS %s: answer from %u.%u.%u.%u:%u", d->qname, IP4((uint8_t*)&from.sin_addr), ntohs(from.sin_port));
        if (r == DNS_OK) { dns_finish(d, 0, ""); return; }
        char why[80];
        snprintf(why, sizeof why, "%s (RCODE %d)", r == DNS_NXDOMAIN ? "no such name" : r == DNS_NO_A ?
                 "no IPv4 address in the answer" : "server failure", rc);
        dns_finish(d, DNS_ERR_NONAME, why);
        return;
    }
    uint64_t now = platform_now_ns();
    if (now - d->t_stage > d->stage_timeout) {
        char why[96];
        snprintf(why, sizeof why, "no answer from %s after %d queries", d->nservers > 1 ? "either server" : "the server",
                 d->sends);
        dns_finish(d, DNS_ERR_TIMEOUT, why);
    } else if (now - d->t_sent > DNS_RETRY_NS) {
        if (d->cur + 1 < d->nservers && d->cur_sends >= DNS_NEXT_QUERIES) {
            d->cur++;
            d->cur_sends = 0;
            rt_log("[net] DNS %s: no answer from %u.%u.%u.%u:%u after %d queries; asking %u.%u.%u.%u:%u", d->qname,
                   IP4(d->servers[d->cur - 1]), d->sports[d->cur - 1], DNS_NEXT_QUERIES, IP4(d->servers[d->cur]),
                   d->sports[d->cur]);
        }
        dns_send(d);
    }
}

/* 0x8040A8B0(name, length, 4): starts a lookup, returns its id (>= 0) */
void hle_net_dns_start(Cpu* c) {
    FIRST_CALL("DNS lookup start");
    int len = (int16_t)c->r[4];
    int id = -1;
    for (int i = 0; i < MAX_DNS; i++)
        if (!g_dns[i].used) { id = i; break; }
    if (len <= 0 || len > 255 || !span(c->r[3], (uint32_t)len) || c->r[5] != 4) {
        rt_log("[net] DNS start: bad arguments (name %08X length %d type %u)", c->r[3], len, c->r[5]);
        c->r[3] = (uint32_t)-2;
        return;
    }
    if (id < 0) {
        rt_log("[net] DNS start: no free lookup slot");
        c->r[3] = (uint32_t)DNS_ERR_NOSLOT;
        return;
    }
    Dns* d = &g_dns[id];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->fd = BAD_SOCK;
    d->result = 1;
    mem_read_bytes(c->r[3], d->name, (size_t)len);
    d->name[len] = 0;
    size_t nl = strlen(d->name);
    if (nl && d->name[nl - 1] == '.') d->name[nl - 1] = 0;
    copy_name(d->qname, d->name);
    d->t0 = platform_now_ns();
    count_open();
    c->r[3] = (uint32_t)id;
    uint16_t port;
    if (dns_parse_ipv4(d->name, d->ip, &port) && !port) {  /* the game skips DNS for these; just in case */
        d->via = "a numeric address";
        dns_done(d, 0, "");
        return;
    }
    net_start();
    if (!g_sock_ok) {
        d->via = "no sockets";
        dns_done(d, DNS_ERR_NONAME, "native sockets are not available");
        return;
    }
    g_sess.lookups++;
    if (name_is_server(d->name)) {
        read_dns_setting();  /* settings.ini may have changed (menu) */
        dns_next(d, DNS_ERR_NONAME, "");
        return;
    }
    d->stage = ST_OTHER;
    d->via = g_cfg_from_system ? "another name, asked of the system's DNS servers" : "another name, the system's resolver";
    dns_system(d, DNS_TIMEOUT_NS);
}

/* 0x8040A698(id, &{4, addr}, &count, 0, &x, -1): -1 still pending, 0 done, else an error */
void hle_net_dns_poll(Cpu* c) {
    FIRST_CALL("DNS lookup poll");
    int id = (int16_t)c->r[3];
    if (id < 0 || id >= MAX_DNS || !g_dns[id].used) { c->r[3] = (uint32_t)-2; return; }
    Dns* d = &g_dns[id];
    dns_step(d);
    if (d->result == 1) {
        if ((int32_t)c->r[8] != -1) {
            static int logged;
            if (!logged) { logged = 1; rt_log("[net] DNS poll asked to wait (%d): answered pending instead", (int)c->r[8]); }
        }
        c->r[3] = (uint32_t)API_PENDING;
        return;
    }
    if (d->result == 0) {
        put_addr(c->r[4], d->ip);
        put16(c->r[5], 1);
        c->r[3] = 0;
        return;
    }
    c->r[3] = (uint32_t)d->result;
}

void hle_net_dns_free(Cpu* c) {  /* 0x8040A630(id) */
    FIRST_CALL("DNS lookup free");
    int id = (int16_t)c->r[3];
    if (id < 0 || id >= MAX_DNS || !g_dns[id].used) { c->r[3] = (uint32_t)-2; return; }
    Dns* d = &g_dns[id];
    if (d->result == 1) rt_log("[net] DNS %s: the game gave up waiting", d->name);
    if (d->fd != BAD_SOCK) sock_close(d->fd);
    memset(d, 0, sizeof *d);
    count_open();
    c->r[3] = 0;
}

void hle_net_dns_add_server(Cpu* c) {  /* 0x8040ADE8(&{4, ip}): recorded in the log only */
    FIRST_CALL("DNS server add");
    uint8_t ip[4];
    get_addr(c->r[3], ip);
    rt_log("[net] the game's DNS server %u.%u.%u.%u (not used: its server names go to %s)", IP4(ip), g_server_str);
    c->r[3] = 0;
}

/* ---------------------------------------------------------------- TCP */
static Tcp* tcp_get(uint32_t h) {
    int i = (int16_t)h;
    return i >= 0 && i < MAX_TCP && g_tcp[i].used ? &g_tcp[i] : NULL;
}

static void tcp_flush(Tcp* t) {
    while (t->txn) {
        int n = (int)send(t->fd, (const char*)t->tx, (int)t->txn, SEND_FLAGS);
        if (n < 0) {
            int e = sock_errno();
            if (!err_wouldblock(e) && !t->end) {
                t->end = net_error_code(e);
                rt_log("[net] TCP %u.%u.%u.%u:%u: send errno %d (%s); the game is told %d", IP4(t->ip), t->port, e,
                       err_name(e), t->end);
            }
            return;
        }
        memmove(t->tx, t->tx + n, t->txn - (size_t)n);
        t->txn -= (size_t)n;
        t->sent += (uint64_t)n;
        g_stat_tx += (uint64_t)n;
    }
}

/* a connect in progress: done, failed or timed out? */
static void tcp_check_connect(Tcp* t) {
    if (t->state != TS_SYN_SENT) return;
    int done = sock_connect_done(t->fd);
    uint64_t now = platform_now_ns();
    unsigned ms = (unsigned)((now - t->t_connect) / 1000000ull);
    if (done) {
        int err = 0;
        sock_len el = sizeof err;
        if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &el)) err = sock_errno();
        if (!err) {
            t->state = TS_ESTABLISHED;
            rt_log("[net] TCP connected to %u.%u.%u.%u:%u (%u ms)", IP4(t->ip), t->port, ms);
            set_status("connected to %u.%u.%u.%u:%u", IP4(t->ip), t->port);
            return;
        }
#if defined(_WIN32)
        t->state = err == WSAECONNREFUSED ? API_CONNRESET : API_UNREACHABLE;
#else
        t->state = err == ECONNREFUSED ? API_CONNRESET : err == ETIMEDOUT ? API_TIMEOUT : API_UNREACHABLE;
#endif
        rt_log("[net] TCP connect to %u.%u.%u.%u:%u FAILED after %u ms: errno %d (%s); the game is told %d",
               IP4(t->ip), t->port, ms, err, err_name(err), t->state);
        set_status("could not connect to %u.%u.%u.%u:%u (%s)", IP4(t->ip), t->port, err_name(err));
    } else if (now - t->t_connect > CONNECT_TIMEOUT_NS) {
        t->state = API_TIMEOUT;
        rt_log("[net] TCP connect to %u.%u.%u.%u:%u FAILED: no answer in %u ms; the game is told %d", IP4(t->ip),
               t->port, ms, t->state);
        set_status("could not connect to %u.%u.%u.%u:%u (no answer)", IP4(t->ip), t->port);
    }
}

/* 0x80422DE4(): a new endpoint, returns its handle */
void hle_net_tcp_create(Cpu* c) {
    FIRST_CALL("TCP create");
    net_start();
    for (int i = 0; i < MAX_TCP; i++) {
        if (g_tcp[i].used) continue;
        Tcp* t = &g_tcp[i];
        memset(t, 0, sizeof *t);
        if (!g_sock_ok) break;
        t->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (t->fd == BAD_SOCK || sock_nonblock(t->fd)) {
            int e = sock_errno();
            rt_log("[net] TCP create: no socket: errno %d (%s)", e, err_name(e));
            if (t->fd != BAD_SOCK) sock_close(t->fd);
            t->fd = BAD_SOCK;
            c->r[3] = (uint32_t)API_NOBUF;
            return;
        }
        int one = 1;
        setsockopt(t->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(t->fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one);  /* Apple: EPIPE, not SIGPIPE */
#endif
        t->used = 1;
        t->state = TS_CLOSED;
        count_open();
        c->r[3] = (uint32_t)i;
        return;
    }
    rt_log("[net] TCP create: %s", g_sock_ok ? "all endpoints in use" : "native sockets are not available");
    c->r[3] = (uint32_t)(g_sock_ok ? API_PORTFULL : API_NOBUF);
}

/* 0x8042284C(h, option, &value): the game sets 0x2001 = 1 (meaning not identified); TCP_NODELAY is always on */
void hle_net_tcp_option(Cpu* c) {
    FIRST_CALL("TCP option");
    rt_log("[net] TCP option 0x%X = %u (noted; sockets use TCP_NODELAY)", c->r[4], get32(c->r[5]));
    c->r[3] = tcp_get(c->r[3]) ? 0 : (uint32_t)API_INVALIDHANDLE;
}

/* 0x80422CFC(h, &{4, address}, local port): a fixed local port is used if the system allows it */
void hle_net_tcp_bind(Cpu* c) {
    FIRST_CALL("TCP bind");
    Tcp* t = tcp_get(c->r[3]);
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    t->lport = (uint16_t)c->r[5];
    if (t->lport) {
        int one = 1;
        setsockopt(t->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        struct sockaddr_in sa;
        static const uint8_t any[4];
        sa_make(&sa, any, t->lport);
        if (bind(t->fd, (struct sockaddr*)&sa, sizeof sa)) {
            int e = sock_errno();
            rt_log("[net] TCP bind to local port %u failed: errno %d (%s); a system-chosen port is used", t->lport, e,
                   err_name(e));
        }
    }
    c->r[3] = 0;
}

/* 0x80422B8C(h, &{4, ip}, port, &opts): starts the connect; 0 = started (the game then polls the status) */
void hle_net_tcp_connect(Cpu* c) {
    FIRST_CALL("TCP connect");
    Tcp* t = tcp_get(c->r[3]);
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    if (!get_addr(c->r[4], t->ip)) {
        rt_log("[net] TCP connect: not an IPv4 address argument (%08X)", c->r[4]);
        c->r[3] = (uint32_t)API_PORTFULL;  /* the library's answer to a bad address (-2) */
        return;
    }
    t->port = (uint16_t)c->r[5];
    struct sockaddr_in sa;
    sa_make(&sa, t->ip, t->port);
    t->t_connect = platform_now_ns();
    rt_log("[net] TCP connecting to %u.%u.%u.%u:%u (handle %d)", IP4(t->ip), t->port, (int)(t - g_tcp));
    g_sess.connects++;
    set_status("connecting to %u.%u.%u.%u:%u", IP4(t->ip), t->port);
    if (connect(t->fd, (struct sockaddr*)&sa, sizeof sa) == 0) {
        t->state = TS_ESTABLISHED;
        rt_log("[net] TCP connected to %u.%u.%u.%u:%u at once", IP4(t->ip), t->port);
        set_status("connected to %u.%u.%u.%u:%u", IP4(t->ip), t->port);
    } else {
        int e = sock_errno();
        if (err_inprogress(e)) {
            t->state = TS_SYN_SENT;
        } else {
            t->state = API_UNREACHABLE;
            rt_log("[net] TCP connect to %u.%u.%u.%u:%u FAILED: errno %d (%s); the game is told %d", IP4(t->ip),
                   t->port, e, err_name(e), t->state);
            set_status("could not connect to %u.%u.%u.%u:%u (%s)", IP4(t->ip), t->port, err_name(e));
        }
    }
    c->r[3] = 0;
}

/* 0x804228E8(h, &state (s16), &x (s16, listening only), &send space, &y): returns 0, or -4 */
void hle_net_tcp_status(Cpu* c) {
    FIRST_CALL("TCP status");
    Tcp* t = tcp_get(c->r[3]);
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    tcp_check_connect(t);
    if (t->state == TS_ESTABLISHED && t->txn) tcp_flush(t);
    int state = t->state;
    if (state == TS_ESTABLISHED && t->end == API_RECVFIN) state = TS_CLOSE_WAIT;
    else if (state == TS_ESTABLISHED && t->end) state = t->end;  /* broken: the game's next send gives up */
    put16(c->r[4], (uint16_t)state);
    put16(c->r[5], 0);
    /* the game closes the connection when its packet is not smaller than this (0x800786B0) */
    put32(c->r[6], t->txn < TX_LIMIT ? 0x10000u : 0u);
    put32(c->r[7], 0);
    c->r[3] = 0;
}

/* 0x8042213C(h, callback, length, buffer): arms one receive; the callback comes from net_frame */
void hle_net_tcp_receive(Cpu* c) {
    FIRST_CALL("TCP receive (arm)");
    Tcp* t = tcp_get(c->r[3]);
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    int len = (int16_t)c->r[5];
    if (!c->r[4] || len <= 0 || !span(c->r[6], (uint32_t)len)) {
        rt_log("[net] TCP receive: unsupported arguments (callback %08X, %d bytes at %08X)", c->r[4], len, c->r[6]);
        c->r[3] = (uint32_t)-2;
        return;
    }
    t->cb = c->r[4];
    t->buf = c->r[6];
    t->armed_len = len;
    t->armed = 1;
    c->r[3] = (uint32_t)API_PENDING;
}

/* 0x80422448(h, 0, count, &{u16 length, u16, u32 data}[count]): queued and sent at once if the socket takes it;
 * 0 = accepted (the game also accepts -1; anything else closes the connection) */
void hle_net_tcp_send(Cpu* c) {
    FIRST_CALL("TCP send");
    Tcp* t = tcp_get(c->r[3]);
    int count = (int8_t)c->r[5];
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    if (count < 0 || count > 3) { c->r[3] = (uint32_t)-2; return; }
    if (t->state != TS_ESTABLISHED || (t->end && t->end != API_RECVFIN)) { c->r[3] = (uint32_t)(t->end ? t->end : API_CONNRESET); return; }
    if (count != 1) {
        static int logged;
        if (!logged) { logged = 1; rt_log("[net] TCP send with %d pieces (the game was seen sending 1)", count); }
    }
    for (int i = 0; i < count; i++) {
        uint32_t e = c->r[6] + 8u * (uint32_t)i;
        uint32_t n = get16(e), p = get32(e + 4);
        if (!n) continue;
        if (!span(p, n)) { rt_log("[net] TCP send: bad buffer %08X (%u bytes)", p, n); c->r[3] = (uint32_t)-2; return; }
        if (t->txn + n > TX_LIMIT) {
            rt_log("[net] TCP %u.%u.%u.%u:%u: %u bytes queued and the server takes no more", IP4(t->ip), t->port,
                   (unsigned)t->txn);
            c->r[3] = (uint32_t)API_SENDPENDING;
            return;
        }
        if (t->txn + n > t->txcap) {
            size_t cap = t->txcap ? t->txcap : 0x4000;
            while (cap < t->txn + n) cap *= 2;
            uint8_t* nb = realloc(t->tx, cap);
            if (!nb) { c->r[3] = (uint32_t)API_NOBUF; return; }
            t->tx = nb;
            t->txcap = cap;
        }
        mem_read_bytes(p, t->tx + t->txn, n);
        t->txn += n;
    }
    tcp_flush(t);
    c->r[3] = 0;
}

static void tcp_free(Tcp* t, const char* why) {
    rt_log("[net] TCP %u.%u.%u.%u:%u closed (%s): %llu bytes sent, %llu received%s", IP4(t->ip), t->port, why,
           (unsigned long long)t->sent, (unsigned long long)t->recvd, t->txn ? ", unsent data dropped" : "");
    if (t->fd != BAD_SOCK) sock_close(t->fd);
    free(t->tx);
    memset(t, 0, sizeof *t);
    t->fd = BAD_SOCK;
    count_open();
}

/* 0x804229F8(h): close (the game then deletes): what is queued goes out first if the socket takes it */
void hle_net_tcp_close(Cpu* c) {
    FIRST_CALL("TCP close");
    Tcp* t = tcp_get(c->r[3]);
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    if (t->state == TS_ESTABLISHED) {
        tcp_flush(t);
        shutdown(t->fd, SHUT_WR);
    }
    c->r[3] = 0;
}

void hle_net_tcp_delete(Cpu* c) {  /* 0x8042298C(h) */
    FIRST_CALL("TCP delete");
    Tcp* t = tcp_get(c->r[3]);
    if (!t) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    tcp_free(t, "by the game");
    set_status("disconnected");
    c->r[3] = 0;
}

/* ---------------------------------------------------------------- UDP */
static Udp* udp_get(uint32_t h) {
    int i = (int16_t)h - UDP_BASE;
    return i >= 0 && i < MAX_UDP && g_udp[i].used ? &g_udp[i] : NULL;
}

/* 0x80421DEC(&{4, local}, local port, &{4, peer}, peer port, 0, callback): returns 0x400 + n */
void hle_net_udp_open(Cpu* c) {
    FIRST_CALL("UDP open");
    net_start();
    uint16_t lport = (uint16_t)c->r[4], pport = (uint16_t)c->r[6];
    uint8_t peer[4];
    get_addr(c->r[5], peer);
    if (!c->r[8]) { c->r[3] = (uint32_t)-2; return; }
    for (int i = 0; lport && i < MAX_UDP; i++)
        if (g_udp[i].used && g_udp[i].lport == lport) {  /* the library refuses a second socket on a port */
            rt_log("[net] UDP open: local port %u is already open; the game is told %d (as the library would)", lport,
                   API_DUPSOCK);
            c->r[3] = (uint32_t)API_DUPSOCK;
            return;
        }
    int id = -1;
    for (int i = 0; i < MAX_UDP; i++)
        if (!g_udp[i].used) { id = i; break; }
    if (id < 0 || !g_sock_ok) { c->r[3] = (uint32_t)(g_sock_ok ? API_PORTFULL : API_NOBUF); return; }
    Udp* u = &g_udp[id];
    memset(u, 0, sizeof *u);
    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->fd == BAD_SOCK || sock_nonblock(u->fd)) {
        int e = sock_errno();
        rt_log("[net] UDP open: no socket: errno %d (%s)", e, err_name(e));
        if (u->fd != BAD_SOCK) sock_close(u->fd);
        c->r[3] = (uint32_t)API_NOBUF;
        return;
    }
    struct sockaddr_in sa;
    static const uint8_t any[4];
    sa_make(&sa, any, lport);
    if (bind(u->fd, (struct sockaddr*)&sa, sizeof sa)) {
        int e = sock_errno();
        rt_log("[net] UDP open: local port %u: bind errno %d (%s)", lport, e, err_name(e));
        sock_close(u->fd);
        c->r[3] = (uint32_t)API_DUPSOCK;
        return;
    }
    u->used = 1;
    u->lport = lport;
    u->pport = pport;
    memcpy(u->peer, peer, 4);
    u->cb = c->r[8];
    count_open();
    rt_log("[net] UDP open: local port %u, peer %u.%u.%u.%u:%u (handle 0x%X)", lport, IP4(peer), pport, UDP_BASE + id);
    c->r[3] = (uint32_t)(UDP_BASE + id);
}

/* 0x80421BAC(h, &{4, dest}, &{4, source}, port, &opts, count, &{u16 length, u16, u32 data}[count]) */
void hle_net_udp_send(Cpu* c) {
    FIRST_CALL("UDP send");
    Udp* u = udp_get(c->r[3]);
    int count = (int8_t)c->r[8];
    if (!u) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    if (count < 0 || count > 3) { c->r[3] = (uint32_t)-2; return; }
    uint8_t ip[4];
    if (!get_addr(c->r[4], ip)) memcpy(ip, u->peer, 4);
    uint16_t port = (uint16_t)c->r[6];
    if (!port) port = u->pport;
    uint8_t buf[0x8000];
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        uint32_t e = c->r[9] + 8u * (uint32_t)i;
        uint32_t n = get16(e), p = get32(e + 4);
        if (total + n > 0x7FE3 || (n && !span(p, n))) { c->r[3] = (uint32_t)-2; return; }
        if (n) mem_read_bytes(p, buf + total, n);
        total += n;
    }
    struct sockaddr_in sa;
    sa_make(&sa, ip, port);
    if (sendto(u->fd, (const char*)buf, (int)total, 0, (struct sockaddr*)&sa, sizeof sa) < 0) {
        int e = sock_errno();
        static int logged;
        if (logged++ < 5) rt_log("[net] UDP send to %u.%u.%u.%u:%u: errno %d (%s)", IP4(ip), port, e, err_name(e));
        c->r[3] = (uint32_t)API_UNREACHABLE;
        return;
    }
    g_stat_utx += total;
    c->r[3] = 0;
}

void hle_net_udp_close(Cpu* c) {  /* 0x80421B3C(h) */
    FIRST_CALL("UDP close");
    Udp* u = udp_get(c->r[3]);
    if (!u) { c->r[3] = (uint32_t)API_INVALIDHANDLE; return; }
    rt_log("[net] UDP close: local port %u", u->lport);
    sock_close(u->fd);
    memset(u, 0, sizeof *u);
    u->fd = BAD_SOCK;
    count_open();
    c->r[3] = 0;
}

/* ---------------------------------------------------------------- the pump (game thread, once a frame) */
static void deliver_tcp(Cpu* c, Tcp* t, int idx) {
    if (!t->armed || t->state != TS_ESTABLISHED) return;
    int code = 0;
    if (!t->end) {
        uint8_t buf[0x800];
        int want = t->armed_len < (int)sizeof buf ? t->armed_len : (int)sizeof buf;
        int n = (int)recv(t->fd, (char*)buf, want, 0);
        if (n > 0) {
            mem_write_bytes(t->buf, buf, (size_t)n);
            t->recvd += (uint64_t)n;
            g_stat_rx += (uint64_t)n;
            t->armed = 0;
            call_guest(c, t->cb, (uint32_t)n, (uint32_t)idx);
            return;
        }
        if (n == 0) {
            t->end = API_RECVFIN;
        } else {
            int e = sock_errno();
            if (err_wouldblock(e)) return;
            rt_log("[net] TCP %u.%u.%u.%u:%u: receive errno %d (%s)", IP4(t->ip), t->port, e, err_name(e));
            t->end = net_error_code(e);
        }
    }
    code = t->end;
    if (!t->end_logged) {
        t->end_logged = 1;
        rt_log("[net] TCP %u.%u.%u.%u:%u: %s; the game is told %d", IP4(t->ip), t->port,
               code == API_RECVFIN ? "the server closed the connection" : t->end_why[0] ? t->end_why : "the connection broke",
               code);
        g_sess.disconnects++;
        snprintf(g_sess.last_end, sizeof g_sess.last_end, "%s", code == API_RECVFIN ? "closed by the server" :
                 t->end_why[0] ? t->end_why : "connection broke");
        set_status("disconnected by %u.%u.%u.%u:%u", IP4(t->ip), t->port);
    }
    t->armed = 0;
    call_guest(c, t->cb, (uint32_t)code, (uint32_t)idx);
}

/* one datagram for an open UDP socket: the two-phase callback (0x80421FB8 on the hardware) */
static void deliver_udp(Cpu* c, Udp* u, int idx) {
    uint8_t pkt[0x8000];
    struct sockaddr_in from;
    sock_len fl = sizeof from;
    int n = (int)recvfrom(u->fd, (char*)pkt, sizeof pkt, 0, (struct sockaddr*)&from, &fl);
    if (n < 0) return;
    uint8_t src[4];
    memcpy(src, &from.sin_addr, 4);
    uint16_t sport = ntohs(from.sin_port);
    static const uint8_t zero[4];
    if ((memcmp(u->peer, zero, 4) && memcmp(u->peer, src, 4)) || (u->pport && u->pport != sport)) {
        static int logged;
        if (logged++ < 5) rt_log("[net] UDP port %u: dropped %d bytes from %u.%u.%u.%u:%u (not its peer)", u->lport,
                                 n, IP4(src), sport);
        return;
    }
    g_stat_urx += (uint64_t)n;
    /* the info block (0x80421F10): +0 handle, +2 length, +4 buffer, +8 source {4, address, 12 zero},
     * +0x1C destination {4, address, ...}, +0x30 source port. In the call frame, above the callee's LR slot */
    uint32_t info = frame_base(c) + 0x10;
    uint8_t b[0x34];
    memset(b, 0, sizeof b);
    b[0] = (uint8_t)((UDP_BASE + idx) >> 8); b[1] = (uint8_t)(UDP_BASE + idx);
    b[2] = (uint8_t)(n >> 8); b[3] = (uint8_t)n;
    b[0x0B] = 4; memcpy(b + 0x0C, src, 4);
    b[0x1F] = 4; memcpy(b + 0x20, g_cfg.ip, 4);
    b[0x30] = (uint8_t)(sport >> 8); b[0x31] = (uint8_t)sport;
    put_bytes(info, b, sizeof b);
    if (call_guest(c, u->cb, 0, info)) return;  /* the game has no object for it: dropped */
    int len = (int16_t)get16(info + 2);
    uint32_t buf = get32(info + 4);
    if (len >= n) { len = n; put16(info + 2, (uint16_t)n); }
    if (len <= 0 || !span(buf, (uint32_t)len)) return;
    mem_write_bytes(buf, pkt, (size_t)len);
    call_guest(c, u->cb, 1, info);
}

/* the port ends every open connection (the console slept, the system lost its network): the game gets `code` on
 * its next receive or send and shows its own message */
static void end_connections(int code, const char* why) {
    for (int i = 0; i < MAX_TCP; i++) {
        Tcp* t = &g_tcp[i];
        if (!t->used || t->state != TS_ESTABLISHED || t->end) continue;
        t->end = code;
        snprintf(t->end_why, sizeof t->end_why, "%s", why);
        rt_log("[net] TCP %u.%u.%u.%u:%u ended by the port: %s; the game is told %d", IP4(t->ip), t->port, why, code);
    }
}

void net_platform_event(int ev) {  /* any thread (Switch: the applet hook on the render thread) */
    if (ev == NET_EV_RESUME) atomic_store(&g_ev_resume, 1);
    else if (ev == NET_EV_FOCUS_LOST) atomic_store(&g_ev_focus, 1);
    else if (ev == NET_EV_FOCUS_BACK) atomic_store(&g_ev_focus, 2);
}

static void platform_events(void) {
    int f = atomic_exchange(&g_ev_focus, 0);
    uint64_t now = platform_now_ns();
    if (f == 1) g_focus_lost_ns = now;
    if (f == 2 && g_focus_lost_ns && g_open) {
        rt_log("[net] back in focus after %.1f s away (connections kept; a dead one shows on its next use)",
               (now - g_focus_lost_ns) / 1e9);
        g_focus_lost_ns = 0;
    }
    if (atomic_exchange(&g_ev_resume, 0)) {
        g_link_cached = -2;  /* the link is asked again */
        end_connections(API_CONNRESET, "the console slept");
    }
    /* the system's network, once a second while a connection is open */
    int any = 0;
    for (int i = 0; i < MAX_TCP; i++) any |= g_tcp[i].used && g_tcp[i].state == TS_ESTABLISHED && !g_tcp[i].end;
    if (any && g_sock_ok && now - g_link_check_ns > 1000000000ull) {
        g_link_check_ns = now;
        static int downs;
        downs = platform_net_link() == 0 ? downs + 1 : 0;
        if (downs >= 2) {  /* two checks in a row: not a passing hiccup of the system service */
            downs = 0;
            g_link_cached = 0;
            g_link_t = now;
            rt_log("[net] link DOWN (the system lost its network connection)");
            end_connections(API_UNREACHABLE, "the system lost its network connection");
        }
    }
}

void net_frame(Cpu* c) {
    chat_frame();
    if (!g_open) return;
    platform_events();
    for (int i = 0; i < MAX_TCP; i++) {
        Tcp* t = &g_tcp[i];
        if (!t->used) continue;
        tcp_check_connect(t);
        if (t->state == TS_ESTABLISHED && t->txn) tcp_flush(t);
        deliver_tcp(c, t, i);
    }
    for (int i = 0; i < MAX_UDP; i++)
        if (g_udp[i].used) deliver_udp(c, &g_udp[i], i);
    for (int i = 0; i < MAX_DNS; i++)
        if (g_dns[i].used) dns_step(&g_dns[i]);
    uint64_t now = platform_now_ns();
    if (!g_stat_t0) g_stat_t0 = now;
    if (now - g_stat_t0 >= 10000000000ull) {
        if (g_stat_tx | g_stat_rx | g_stat_utx | g_stat_urx)
            rt_log("[net] last %.0f s: TCP %llu bytes sent, %llu received; UDP %llu sent, %llu received",
                   (now - g_stat_t0) / 1e9, (unsigned long long)g_stat_tx, (unsigned long long)g_stat_rx,
                   (unsigned long long)g_stat_utx, (unsigned long long)g_stat_urx);
        g_stat_t0 = now;
        g_sess.tx += g_stat_tx; g_sess.rx += g_stat_rx; g_sess.utx += g_stat_utx; g_sess.urx += g_stat_urx;
        g_stat_tx = g_stat_rx = g_stat_utx = g_stat_urx = 0;
    }
}

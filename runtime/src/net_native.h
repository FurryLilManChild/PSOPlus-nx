/* Online play with native sockets in place of the game's network library (net_native.c). */
#pragma once
#include <stdint.h>

#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

void net_init(void);         /* main.c, after settings_load: the net_dns setting (sockets start later) */
void net_frame(Cpu* c);      /* frame_pacing.c, after each display flip, on the game thread: the pump */

typedef struct NetStatus {   /* for the port menu (System page) */
    char state[128];         /* "not used yet", "connecting to ...", "connected to ...", "DNS failed ..." */
    char dns[64];            /* the DNS servers asked for the game's server names, in order */
    char console_ip[40];     /* what the game was told this console's address is */
    int sockets_ok;
    int started;             /* the game has started its network this session (sockets tried) */
} NetStatus;
void net_status(NetStatus* out);  /* any thread */

/* platform layer (Switch: platform_switch.c, libnx sockets and nifm; elsewhere net_native.c) */
typedef struct NetIpConfig {
    uint8_t ip[4], mask[4], gateway[4], dns1[4], dns2[4];  /* network byte order */
} NetIpConfig;
int platform_net_init(void);   /* 0: sockets usable */
void platform_net_exit(void);
int platform_net_ipconfig(NetIpConfig* out);  /* 1: filled from the system's current connection */
int platform_net_link(void);   /* 1 the system has a connection, 0 it has none, -1 cannot tell */

int exi_card_backup(const char* path);  /* exi.c: 0 written, 1 no card file in use, -1 write error */

#ifdef __cplusplus
}
#endif

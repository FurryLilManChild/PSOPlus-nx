# Online play: preliminary research (2026-11-23)

Goal: PSOPlus-nx players connect to today's community PSO servers from the Switch, as a real GameCube with a
Broadband Adapter (BBA) does. Nothing is built yet; this is what is known, what is still unknown, and the
options.

## What the disc shows (our own analysis of PsoV3.dol)

- Our disc is US Plus v1.02. The server hostname `game04.st-pso.games.sega.net` sits at 0x8043EDA0, the same
  address ragol.org's Action Replay redirect code for "US v1.02 (Plus)" writes to.
- The network stack is in the game itself: "Network SDK - avetcp.a Ver.1.16.1", built on ACCESS AVE-TCPv6.0
  (loopback, dix, arp, ppp, ipv4, tcp, dns modules), plus Nintendo's ntUtl 1.16.00. On a real GameCube this
  stack talks to the BBA (or the modem) over EXI channel 0, device 2 (serial port 1).
- Today the port reports no adapter there, so the game takes its offline path (docs/design.md).

## What the community setups do

- Servers that support GC Episode I & II Plus: Schtserv (the oldest public server, still active), Sylverant,
  ragol.org, and anyone running newserv (open source; it supports GC Ep1&2 Plus for lobbies, games and proxy).
- Connecting is done by DNS redirection: the game's DNS server setting points at the server, which answers the
  hostname lookup with its own address. Where an ISP blocks that, an Action Replay code rewrites the hostname
  string (the 0x8043EDA0 code above).
- Plus and Episode III refuse a server on the same local network as the console (judged from the console's IP
  and subnet mask). That matters only for self-hosting.
- Serial number, access key and password: private servers accept random values (ragol.org asks for random ones,
  not patterns like 1111-1111-11).
- Dolphin has three BBA modes. HLE (the game's Ethernet frames translated to host sockets inside Dolphin) is
  faster but "prone to freezing while online" with Episode I & II; tapserver (frames sent to newserv's IP stack
  simulator) "will never crash" but adds latency; TAP needs a virtual adapter on the host. Dolphin's HLE also
  does not fully implement listening sockets.

## Options for the port

### A. Native sockets (recommended to investigate first)

Replace the avetcp.a API (stack init and link-up, DHCP results, DNS lookup, TCP open/connect/send/receive/
close, timers or callbacks it uses) with native calls on the Switch's BSD sockets (libnx). The game's code
above the library is unchanged; no BBA hardware, no Ethernet frames and no game TCP/IP stack run.

- Fits the project goal: native, not emulation.
- DNS is ours: the port menu picks the server and we answer the hostname lookup with it, so no AR code and no
  DNS tricks, and the Plus same-LAN check never sees a private address unless we want it to.
- Unknown and the main risk: the size and shape of the avetcp.a API PSO uses, and whether it is synchronous
  calls or callbacks and events. The functions have no names in the DOL. Ways to label them: call graphs from
  PSO's own network code, the library's strings and error messages, and other GameCube games that link the same
  Network SDK (a game that shipped a symbol map on disc would label it outright).
- The game's network settings screens (DHCP, PPPoE, IP fields) still run; we report a working connection.

### B. BBA hardware model plus a host-side stack (Dolphin's HLE approach)

Model the BBA's registers on EXI so the game's own AVE-TCP stack runs and sends Ethernet frames; translate
frames to sockets with a small ARP/DHCP/DNS/TCP layer of our own (or send them to a tapserver endpoint, which
ragol.org offers).

- Known to work with PSO in Dolphin, so the behaviour is well understood.
- Emulation-style (hardware registers, per-packet EXI traffic, the game's stack doing TCP), and Dolphin's HLE
  version of it has the online freezes players report. tapserver ties us to servers that offer it.
- A fallback if A turns out much larger than expected.

### Pieces needed either way

- A server choice in the port menu (Schtserv, Sylverant, ragol.org, custom host).
- Chat: PSO supports a USB-style keyboard controller; on the Switch, the system keyboard (swkbd) for chat
  entry. Word Select and symbol chat already work with the pad.
- Network status and errors in the log (each connect, lookup, disconnect and error code), and a test plan that
  uses one server at first.
- Server-relayed play: as far as we know, PSO clients talk only to the server, which forwards game commands
  between players, so no port forwarding or peer-to-peer is needed. To be confirmed in the first test.

## Unknowns to settle before any build

1. The avetcp.a functions PSO calls, how many, and their calling style (static analysis in the cloud first).
2. How the game detects the adapter at boot and in the network settings (EXI ID read on channel 0 device 2,
   link status), so it takes the online path.
3. Whether PSO GC sends anything over UDP besides DNS.
4. Which servers the owner wants to support first.

## Suggested phases

1. Analysis only, no test build: map the Network SDK functions PSO uses and the adapter checks; decide A or B
   with evidence.
2. First online build: adapter present, native sockets (A), one server, connect to the lobby. Full logging.
3. Play online: games, quests, item trades, stability over long sessions, disconnect handling.
4. Port menu server list, chat keyboard, release.

Sources: newserv README (github.com/fuzziqersoftware/newserv), ragol.org/gc-connect and its forum, the Dolphin
wiki BBA page and issue tracker, and our own reading of PsoV3.dol.

## Findings: network library map (2026-11-23, static analysis only)

Method: decomp-toolkit's analysis of PsoV3.dol (function boundaries and symbolized references in its asm
output, kept in ignored build/ folders), a small call-graph script run in /tmp, and reading the disassembly of
every function named below. No build and no console test. Each claim is marked **proven** (read in the code, with
the address) or **inferred** (a reasoned identification not yet confirmed by a run). Function names in quotes
are our own labels; the DOL has no symbols for this library.

### Where the library is

- **Proven (layout by version strings and call graph):** the network code is one block of about 830 functions,
  roughly 0x803F632C to 0x8042F230, after Sega's memory-card and network-settings code:
  - 0x803F2F1C to 0x803F632C, about 55 functions: Sega's network settings storage (the memory-card file named
    for "sega network information", field getters and setters). It reaches the CARD library, never the network.
    It can stay recompiled under either option.
  - 0x803F632C to 0x803F9F04: modem dialer, the stack's memory setup, and the stack's OS porting layer at
    0x803F95A4 to 0x803F9F04 (mutex lock and unlock wrappers 0x803F95C4 and 0x803F95A4 around OSLockMutex
    0x80373724 and OSUnlockMutex 0x80373800, both proven by their bodies; task creation 0x803F9B94; timed wait
    0x803F97E8 built on OSCreateAlarm and OSSetAlarm plus a message receive; priority setter 0x803F9CF4).
  - 0x803F9F04 to 0x80425A14, about 590 functions: AVE-TCP itself (PPP with CHAP/PAP/IPCP/IPv6CP, IPv4, routing,
    TCP, UDP, DNS resolver, DHCP client, the socket-style API). The init function 0x80421674 prints the
    "Network SDK - avetcp.a 1.16.1" banner (string at 0x804800F0) and calls 13 module initialisers in turn.
  - 0x80425A14 to 0x8042C8D8: the BBA driver (all EXI channel 0 device 2 traffic, receive task 0x80425CFC).
  - 0x8042C8D8 to 0x8042E8C4: the modem driver (AT command strings).
  - 0x8042E8C4 to 0x8042F230: ntUtl 1.16.00 (banner user 0x8042EC14): adapter type query, the "shared memory
    connect parameter" block for the Website browser, and the DOL chain loader 0x8042E8C4.
- **Proven:** the EXI library at 0x8042F230 onwards (EXIImm, EXIDma, EXISync, EXISelect, EXIDeselect, EXILock,
  EXIUnlock, EXIGetID 0x80430694, EXIGetType 0x80430A10) is the only EXI copy; its callers in the network block
  are all in 0x804295BC to 0x8042E488 (the two drivers).
- **Proven:** game code never references the library's data or bss directly (cross-check of every symbolized
  reference: no label is shared between game code and the block), and game code takes no library function's
  address. The only function pointers that cross the boundary are two game callbacks handed to the library
  (0x80078378 and 0x80078578, below). Limit: references computed at run time from returned pointers are not
  covered by this check.

### 1. Adapter detection and link status

- **Proven:** the adapter type query is ntUtl 0x8042EE4C(int* type_out). It returns a cached type 2 or 5 if set
  (cache at sdata 0x805D7CC8); otherwise it calls EXIGetType(0, 2, &id) (0x80430A10, channel 0 device 2, which
  masks the low byte of the ID for the 0x0402xx00 family). ID 0x02020000 gives type 2 (modem, cached);
  0x04020200 (BBA) calls 0x8042BE64 and gives type 5 (cached) if that returns 0, else type 1; 0xFFFFFFFF gives
  type 0; any other ID gives type 1; an EXIGetType failure returns -5.
- **Proven:** 0x8042BE64 locks EXI channel 0 device 2, selects it at frequency index 5, sends the two command
  bytes 0x01 0x00 and reads one byte back; that byte is its return value. Which BBA-side register this is: not
  determined (it is not the MX98728EC register path, which uses 4-byte commands with bit 31 set).
- **Proven:** the game's checks on that type: 0x8014618C returns 1 only for types 4 and 5 ("BBA present");
  0x80145DB4(mode) returns 1 for type 2 when mode is 1 (modem) and for types 4 and 5 otherwise. Their callers
  are the boot-time settings load 0x80145A94 (via 0x801461EC from 0x8000FAE4), menu code at 0x8032745C,
  0x8032C618, 0x80328DB0, 0x80339480 and 0x8033954C, and 0x8007FA24 and 0x8014797C. **Inferred:** these are
  the places that decide between the online menus and the "no adapter" path.
- **Proven:** link status is 0x80426BD8 (driver, returns 1 linked, 0 not, -1 on EXI lock failure). It calls
  0x8042B9C0, which reads MX register 0x31 through 0x804295BC (4-byte command 0x80000000 | reg << 8, then the
  data bytes) and reports link if bit 0 or bit 1 is set. **Inferred (register names from Dolphin's BBA model,
  from memory):** 0x31 is NWAYS, bits 0 and 1 are 10 and 100 Mbit link.
- **Proven:** 0x8014622C waits up to 300 frames for link (returns 2 linked, 0 timeout, 1 cancelled), and
  0x801467C4 (called before each TCP send and in the connect loop) re-checks the adapter type every 900 frames
  and, once the BBA path is up, link status on every call. 0x80426C34(mode) only stores a media mode (0 to 4) in
  sdata 0x805D7BC8; the game passes 4.
- **Proven (our runtime):** runtime/src/exi.c models only channel 0 devices 0 (card) and 1 (IPL). Device 2
  (chip select value 4) has no device, so every byte read there is 0. **Inferred:** EXIGetID therefore succeeds
  with ID 0, 0x8042EE4C reports type 1 (unknown device), and every check above answers "no BBA", which is why
  the game is offline today.

### 2. The API PSO's own code calls

93 library functions are called from game code. All game call sites are in three places: the game's socket
classes 0x80078110 to 0x80078FC0, the game's connection manager 0x80145A44 to 0x801478C4, and the Website
hand-off 0x8007F8D0 to 0x8007FA90 (plus one status read at 0x80328590). The settings-storage functions (32 of
the 93, 0x803F2F1C to 0x803F6270, called from 0x80145A44 to 0x80146158, 0x801461EC and 0x8014734C) are left out
of the tables; they stay as they are.

Calling style, **proven** unless marked:

- Every socket-layer API function takes the stack's API mutex (pointer at 0x805C1C50) through 0x803F95C4 and
  releases it through 0x803F95A4, so the game calls the API directly from its own thread.
- The stack runs in its own OS threads, created by 0x803F9B94 with OSCreateThread's argument pattern (thread,
  entry trampoline 0x803F9C78, stack, size, priority from 0x803F9CF4, attribute 1). Created from 0x80419B04
  (core tasks 0x8041A000 and 0x80419C6C), 0x804262CC (BBA receive task 0x80425CFC), 0x803F7A90 (PPP task
  0x80405150) and 0x803F8314. The game sets the stack's thread priority to 16 (0x80146910). **Inferred:** that
  0x80375498 is OSCreateThread (argument shape only).
- TCP connect and DNS are **non-blocking plus polling from the game's frame loop**: the game calls the API, then
  loops rendering frames and polling status (connect loop 0x80078B94 to 0x80078C3C, DNS loop 0x80078DF4 to
  0x80078E68, up to 5400 polls).
- TCP and UDP receive are **callback driven**: the game registers a callback and a buffer; the library calls it
  when data arrives. **Inferred:** the callbacks run on the stack's thread, not the game's.
- TCP send is a plain call that returns at once. **Inferred:** DHCP request 0x80424D58 blocks (it reaches the
  timed wait 0x803F97E8, which waits on an OSAlarm and a message).

Socket handles (**proven** by the game's tables): TCP handles are small integers (the game indexes a table at
0x8048D378 by handle * 4; **inferred** from the layout: at most 16, since the UDP table starts at 0x8048D3B8); UDP handles are 0x400 + n (table 0x8048D3B8 - 0x1000 + handle * 4;
the library checks (h & 0xC00) == 0x400 at 0x80421B44). A native replacement must hand out handles in the same
ranges. Address arguments are {u32 length = 4, u32 IPv4 address} structures (the stack also does IPv6).

**TCP** (game class vtable 0x8048D494; slots: connect 0x80078A88, close 0x80078A14, arm receive 0x80078514,
send string 0x8007877C, send 0x80078620; PSO's subclass vtable 0x804C5208):

| Function | Calls | Call sites | Identification |
|---|---|---|---|
| 0x80422DE4 | 1 | 0x80078AC0 | create TCP endpoint, returns handle (**proven** by use) |
| 0x8042284C | 1 | 0x80078B00 | set option (h, 0x2001, &1); option meaning not identified |
| 0x80422CFC | 1 | 0x80078B20 | bind (h, &{4, 0}, local port) (**inferred**) |
| 0x80422B8C | 1 | 0x80078B5C | connect (h, &{4, ip}, port, &opts{4, 0x78, 0, 0}); returns at once, nonzero is an error (**proven** by use) |
| 0x804228E8 | 1 | 0x800789F4 | status (h, &state, NULL, &free_send_space, &x); state >= 4 means connected, < 0 means failed (**proven** by use) |
| 0x8042213C | 1 | 0x80078560 | arm receive (h, callback 0x80078578, 0x800, buffer); the callback gets (length, handle) and the game copies the count and calls the object's handler at +0x880 (**proven**) |
| 0x80422448 | 2 | 0x80078710, 0x80078858 | send (h, 0, 1, &{u16 len, ptr}); 0 and -1 are accepted, anything else closes the connection (**proven** by use) |
| 0x804229F8 | 1 | 0x80078A34 | close or abort (**inferred**: called first in close) |
| 0x8042298C | 1 | 0x80078A3C | delete endpoint (**inferred**: called second in close) |

**UDP** (game class vtable 0x8048D43C; slots: open 0x800782B8, close 0x80078264, 0x800780D0, send string
0x800781B8, send 0x80078110; PSO's subclass vtable 0x804C22D8):

| Function | Calls | Call sites | Identification |
|---|---|---|---|
| 0x80421DEC | 1 | 0x80078330 | open (&{4, 0}, local port, &{4, peer ip}, peer port, 0, callback 0x80078378), returns handle 0x400 + n (**proven** by use) |
| 0x80421BAC | 2 | 0x800781A0, 0x8007824C | send (h, &dst, &src, port, &opts, count <= 3, buffer list); total at most 0x7FE3 bytes (**proven** limits) |
| 0x80421B3C | 1 | 0x80078284 | close (**proven** by use) |

The UDP callback 0x80078378 is two-phase (**proven**): called with 0 it supplies the object's 0x800-byte buffer
and copies the peer address; called with a length it records it and calls the object's handler.

**DNS** (game function 0x80078D54(obj, hostname); dotted-quad names skip DNS):

| Function | Calls | Call sites | Identification |
|---|---|---|---|
| 0x8040A45C | 1 | 0x801471B0 | resolver init (**inferred**) |
| 0x8040ADE8 | 2 | 0x801471F0, 0x8014721C | add DNS server &{4, ip}: primary then secondary, from DHCP or from the manual settings (**proven** by use and the debug messages after it) |
| 0x8040AD64 | 1 | 0x80147180 | clear DNS servers (**proven** by body) |
| 0x8040A3C0 | 1 | 0x80147184 | resolver shutdown (**inferred**) |
| 0x8040A8B0 | 1 | 0x80078DB8 | start lookup (name, length, 4), returns a request id >= 0 (**proven** by use) |
| 0x8040A698 | 1 | 0x80078E0C | poll result (id, &{4, addr}, &count=1, 0, &x, -1): -1 still pending, 0 done, other values are errors the game maps to its own codes (**proven** by use) |
| 0x8040A630 | 1 | 0x80078F30 | free the request (**proven** by body) |

**IP setup (BBA path, game function 0x80146B08)**, all called once unless noted:

| Function | Call sites | Identification |
|---|---|---|
| 0x804073B8 / 0x80407358 | 0x80146B3C / 0x80146AE8 | module init / term, flag 0x80 (**inferred**) |
| 0x8040876C / 0x8040874C | 0x80146B4C / 0x80146ACC | interface open (returns the interface id kept at 0x805D2D00) / close, flag 0x40 (**proven** by use of the return value; layer **inferred**) |
| 0x804150EC / 0x8041509C | 0x80146B98 / 0x80146AB0 | module init / term, flag 0x20 (**inferred**) |
| 0x80424E44 | 0x801465E4 | DHCP start on the interface (**inferred**) |
| 0x8042455C | 0x8014661C | DHCP host name (only if one is set) (**inferred**: string argument) |
| 0x80424D58 | 0x80146660 | DHCP request, blocking, writes the leased IP to 0x805D56D8 (**proven** by use) |
| 0x8042489C | 0x8014667C | DHCP result: gateway to 0x805D56E4 (**inferred**: later passed to the route call) |
| 0x8042474C | 0x8014669C | DHCP result: three values to 0x805D56EC to 0x805D56F4 (**inferred**: lease times) |
| 0x80424910 | 0x801466C8 | DHCP result: DNS servers to 0x805D56DC and 0x805D56E0 (**proven**: these feed the DNS server calls) |
| 0x804245DC | 0x80146760 | DHCP state getter (returns one word) (**proven** body) |
| 0x80424CF4 / 0x80424CA8 | 0x80146A1C / 0x80146A2C | DHCP release / stop at teardown (**inferred**) |
| 0x8042315C | 0x80146C38, 0x801477FC | set interface address (iface, &{4, ip, mask, 0}) (**proven** argument; also used by the PPP path) |
| 0x80423064 | 0x80146A54, 0x801472A4 | remove interface address (**inferred**) |
| 0x80421A50 / 0x804219D0 | 0x80146300 / 0x801462B8 | add / delete default route via the gateway (**inferred**) |
| 0x80426BD8 | 0x8014626C, 0x80146878 | link status (above) |
| 0x80426C34 | 0x80146B94 | media mode (above) |
| 0x8042EE4C | 0x80145ACC, 0x80145DCC, 0x80146168, 0x8014619C | adapter type (above) |

Static IP (0x80146588) reads the manual settings block at 0x8050CCE8 (IP +4, mask +8, DNS +0x10 and +0x14,
gateway +0x18) instead of DHCP (**proven**). The game's own copy of the address used as the UDP source is at
0x8050C0A8 + 4 (getter 0x8014672C, **proven**).

**Stack lifecycle and OS layer:** 0x803F9CF4 (thread priority, 0x8014691C), 0x803F861C, 0x803F8564 and
0x803F8470 (memory configuration and the heap the game gives the stack: 0x1000 and 0x3000 sized pools,
0x80146924 to 0x80146964, **inferred** roles), 0x80421674 avetcp init (0x80146968, **proven** by banner) and
0x80421620 term (0x801468D0, **proven**: it calls the module terminators), 0x803F9E84 (a strlen wrapper,
0x80078DA8).

**PPP, PPPoE and modem connection manager** (only on the modem and PPPoE paths): 0x803FB01C (2 calls,
0x80146328, 0x80146998), 0x803FAFDC (0x80146A8C, 0x801472C8), 0x803FB280 (0x80146414, 0x801476F8),
0x803FAEA0 (0x8014640C), 0x803FB120 (0x801475C4), 0x803FB184 (7 calls: 0x801464E0, 0x801467E0, 0x80146A6C,
0x80147314, 0x801477D0, 0x8014783C, 0x80328590; connection status), 0x803FB1A4 (5 calls: 0x80146484,
0x80146520, 0x80146A64, 0x801472B4, 0x80147834). **Inferred** roles: init, term, start connection, PPPoE mode,
modem parameters, status, request disconnect. 0x80146908 picks the BBA path when 0x805D5704 is set, the modem
path otherwise (**proven**).

**ntUtl, Website hand-off:** 0x8042EC14 (init, 0x8007F904), 0x8042EF48 (0x8007F90C), 0x8042ED98 (5 calls,
0x8007F91C to 0x8007F95C, string field setters), 0x8042EAD4 (0x8007F9FC), 0x8042EBA4 (0x8007FA08, chain-loads
the browser DOL through 0x8042E8C4), 0x8042EA10 (2 calls, 0x8007FA3C, 0x8007FA68, the shared connect-parameter
block) (**proven** by strings and calls).

### 3. DNS lookup and connect for the server

- **Proven:** the server table at 0x804A6FD8 (read only by 0x801399B8) holds three (hostname, port) entries,
  each written twice: game04.st-pso.games.sega.net (0x8043EDA0) port 9103, ss702.sonicteam.com (0x8043EDC0)
  port 9100, ddplanet.sega.co.jp (0x8043EDD4) port 9001. 0x801399B8 picks one by the value of 0x8033605C
  (2 gives ss702, 0 or 1 game04, other values ddplanet). **Inferred:** that value is a region or language
  setting. **Inferred (newserv's port list, from memory):** 9103 is newserv's US Plus port.
- **Proven:** 0x801399B8 calls 0x801DAEB0(protocol object, ..., hostname, port), which calls 0x80078D54 (DNS
  lookup, result stored byte-reversed at object +0x1C by 0x80078D24), sets the port (0x80078CF0), then calls the
  TCP connect method (vtable slot 0x18, 0x80078A88: create, option, bind, connect to +0x1C and port +0x20, poll
  status) and the receive method (slot 0x20, 0x80078514).
- **Proven:** server redirects (0x801DD430 and 0x801DD4D8) take an IP and port from the server's command, store
  them at 0x805D5E1C and 0x805D5E20, close and reconnect through 0x801DCB40 with no DNS.

### 4. UDP besides DNS

- **Proven:** the game opens peer-to-peer UDP sockets to other players. 0x80135208 (debug message "udp open")
  creates a UDP object per player, local port 4000 + own client id, remote port 4000 + the peer's id, to the
  peer IP from the per-player table at 0x8050ABB8 (stride 0x20, IP at +8). Callers: 0x801352C0, 0x801354AC,
  0x8013634C (player creation and removal handlers) and 0x801A3CFC (a "UDPTEST" path). 0x801E4260 sends each peer's
  buffered data, one peer per frame.
- **Proven:** whether game commands use UDP is decided by sdata 0x805D5598. 0x801E45D0 sends a command through
  the TCP server connection (0x801E4570 then 0x801DC134) when it is 1, and appends it to every UDP peer's buffer
  (0x801A5544) otherwise. The lobby join handler 0x801368B0 sets it to 1; the game join handler 0x80136C94 copies
  it from byte 0x106 of the server's join command, right after the client id (0x104) and leader id (0x105).
  0x801399B8 also sets it to 1 before connecting.
- **Inferred (newserv source, read on the web):** newserv names this byte disable_udp and sends 1, so on
  community servers game traffic stays on TCP. The UDP sockets are still opened on joins, so a native layer
  must support UDP open, send and close, even if it only discards.
- DNS itself runs inside the stack over its own UDP API (0x8040A8B0 reaches the UDP send 0x80421BAC); with
  option A that disappears.

### 5. The Plus "same local network" check

Not found. Ruled out (**proven**): the game reads the console's IP, mask and gateway variables (0x805D56D8,
0x805D56E4, 0x8050C0A8) only in the connection manager 0x80145A44 to 0x801478C4 and the UDP source getter
0x8014672C; the DNS lookup 0x80078D54, the connect 0x801DAEB0, 0x80078A88 and the redirect handlers do no
subnet compare; no game code compares against private-range constants. The only IP-and-mask compares in the
whole DOL are the stack's own route lookup 0x8040EF50 (normal on-link routing, used by connect and send).
**Inferred:** if the check exists in code it is inside the stack or relies on stack behaviour, and option A,
which removes the stack and answers DNS itself, does not run it. To confirm with a first online test.

### 6. Things that could make option A hard

- Direct access to stack internals by game code: none found (see "Where the library is").
- Raw Ethernet from game code: none. The game touches the BBA driver only through link status 0x80426BD8 and
  media mode 0x80426C34; avetcp init 0x80421674 attaches the driver internally (through 0x80419A34 and
  0x803F8E9C to 0x804294E8 and 0x80425A14), so init itself must be replaced, not run.
- Callbacks: the two game callbacks were called from the stack's thread on hardware. A native layer can call
  them from the game thread at a safe point (inside the status and poll calls, or at frame end), which is
  simpler for the recompiled code than a second guest thread.
- Timing: only frame-count loops in game code (300 frames for link, 5400 DNS polls, the connect loop, a
  900-frame adapter re-check). The stack's own timers (OSAlarm) go away with it.
- The Website browser is a separate program on the disc (iwanaga.dol), started by chain-loading with the
  connection settings passed in shared memory (0x8042EA10, 0x8042EBA4); it does not share the in-game stack.
  Out of scope.
- Handle ranges and the address structure layout (above) must be kept exactly.

### 7. Size estimate and recommendation

**Option A** (native replacements at the library boundary, in config/GPOE8P/hle.txt style): about 50 functions.

- Real work, about 15: TCP create, option, bind, connect, status, arm receive (callback), send, close, delete;
  UDP open (callback), send, close; DNS start, poll, free; plus a small pump that delivers receive callbacks and
  connect completions on the game thread. On libnx BSD sockets this is non-blocking sockets and poll().
- Trivial, about 35: stack init and term, module init and term pairs, memory setup, thread priority, adapter
  type (return 5), link status (return 1), media mode, the DHCP calls (return the Switch's real address, mask,
  gateway and DNS from libnx nifm, or fixed values), interface address and route calls, DNS server add and
  clear (recorded, not used: our DNS answers from the port menu's server choice), DHCP state getter.
- PPP and modem functions: about 7, made to fail or forced off by presenting a BBA with DHCP settings.
- Nothing in game code needs to change. EXI can stay as it is if 0x8042EE4C is replaced.

**Option B** (BBA register model plus a host-side stack): the game's 830-function stack runs recompiled with
its 4 or 5 threads and OSAlarm timers. Needed: the EXI side of the BBA (ID 0x04020200, the 2-byte EXI-side
commands, MX98728EC registers through 4-byte commands, receive and transmit through EXI DMA, the BBA interrupt,
MAC address reads, link bits in register 0x31), then a host stack that answers ARP and DHCP and terminates TCP
and UDP from Ethernet frames, or a tapserver client. That is Dolphin's EXI_DeviceEthernet plus its built-in
network layer: much more code than A, emulation-style, and dependent on our thread and timer emulation
behaving well under per-packet load.

**Recommendation: option A.** The boundary is clean (93 call sites in three game areas, no shared data, two
callbacks), the calling style is simple (game-thread calls, polling, two callbacks), and most functions are
trivial. Open items for the first online build: the exact meaning of TCP option 0x2001, the status call's
fourth output, the stack's error numbers the game maps (DNS: -0x42, -0x45, -0x47, -1, -2), and confirming the
callbacks are safe to call from the game thread.

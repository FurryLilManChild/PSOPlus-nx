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

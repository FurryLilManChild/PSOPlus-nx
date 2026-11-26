# Episode 4 (Blue Burst) research

First pass, 2026-11-26, after release 1.0.0. Question from the owner: could Blue Burst's Episode 4 content be
added to this port? Marks: **found** (seen in a source), **inferred** (reasoning, not checked).

## What Episode 4 is

- Blue Burst (PC, 2004 JP, 2005 US) contains Episodes 1 and 2 plus an all-new Episode 4. Found (Phantasy Star
  Wiki, Sega Retro).
- Areas: Crater (five sub-areas, each reached from the Pioneer 2 teleporter) and the Subterranean Desert (three
  floors and a boss). New enemies with immunity to status effects, teleporting and displacement. Found (Ephinea
  wiki).
- Blue Burst was developed from the Xbox version of Episode I & II. Found (Sega Retro).
- Blue Burst has no offline mode; character data, drops and quests are server side. Found (Sega Retro).

## What already exists

| Project | What | Licence | Use to us |
|---|---|---|---|
| zeroz41/pso-decomp (GitHub) | Matching decompilation of **our exact game**, PSO Ep I&II Plus USA v1.02 GPOE8P: 89.12% of code, 96.53% of functions matched, rebuilds PsoV3.dol and checks the SHA-1 | MIT | Found. Names and structures for the whole GC engine: map loading, enemy classes, episode select. Also useful for the current port (debugging, HLE targets). |
| mrb0nk500/pso-gc (GitLab) | Earlier GC Ep I&II decompilation | not checked | Found; superseded by the above for our version (inferred). |
| fuzziqersoftware/newserv | Server, proxy and RE tools for every PSO version including BB: protocol, map .dat format, quest opcodes for all versions, item and rare tables, save formats, BB patch server | permissive: use allowed if its LICENSE file is included (found); exact licence name not checked | Found. Reference for Episode 4 server-side logic (maps, drops, quests). A built-in copy could stand in for the server in offline play (inferred). |
| psobb.io "Client Decompilation Matrix" | Matching decompilation of the Japanese psobb.exe in progress (MSVC 7.1); one page shows 220 of 2,729 functions banked (8%) | not stated; no public repository found | Found. Too early and licence unknown: do not use as a base. |
| gitlab.psobb.io original-psobb-client-source | A Ghidra decompilation dump of psobb.exe | not stated | Found. A dump of Sega's binary: reference reading only, never copy into our repo. |
| souzajluiz/PSOBB.web | A browser BB client that connects to newserv, using the user's own BB install | not checked | Found. Shows BB formats and protocol are well understood. |

No project that brings Episode 4 to the GameCube version was found (two searches). Not proof none exists.

## Routes

### A. Graft Episode 4 into this GameCube port

Write the Episode 4 game logic natively in our runtime (new areas, enemies, bosses, quest hooks, episode select),
using the GC decompilation for the engine side and Blue Burst's behaviour (newserv, the BB dumps as reading
reference) for the Episode 4 side. Load Episode 4 maps, models and textures from the player's own Blue Burst
files, converted where formats differ.

- For: keeps one app, the GC engine is now well documented (pso-decomp).
- Against: every Episode 4 enemy and boss is new game code written by hand to match BB's behaviour; drops,
  quests and item tables (server side in BB) must be added; asset formats differ in places (ninja headers are
  little endian on all platforms, found; texture container differences between GC and PC are inferred, not
  checked). Online: Schtserv treats us as a GC client, so Episode 4 would be offline only. Months of work.

### B. A separate Blue Burst port (static recompilation of psobb.exe)

Recompile the Blue Burst client itself for the Switch. Episode 4 comes with all its own logic, plus BB's
other additions.

- Inferred key point: psobb.exe is 32-bit x86 Windows code derived from the Xbox version, and our Xbox ports
  (xboxrecomp, XboxXBE-Recomp on sheroes-nx) already statically recompile 32-bit x86 Xbox code to the Switch
  with an API-level D3D layer. A Windows PE instead of an XBE, Win32 calls instead of the Xbox kernel, and BB's
  Direct3D version (not checked) are the new parts.
- Other static recompilers of Windows x86 executables exist (an SRW tool for Windows games, i386-to-arm64 used
  for a macOS port); found, quality not checked.
- Offline play: BB needs a server. A built-in newserv-based local server (licence permitting) would give solo
  play; online could connect to BB servers. Inferred.
- Data: players supply their own Blue Burst install, as with the GC disc. We ship nothing.

### C. Wait for a finished Blue Burst decompilation

The psobb.io effort is about 8% matched with no stated licence. Not a plan.

## Recommendation

Route B, as a new project (working name PSOBB-nx), after checking three things:

1. Blue Burst client: which build players use (a fan server's client), its size, protections or packers
   (fan clients are often patched), and its Direct3D version.
2. How much of the Xbox x86 pipeline carries over to a Windows PE (loader, imports, D3D layer).
3. newserv's exact licence and how much of it a local server needs.

Route A only if the owner wants Episode 4 inside this app specifically and accepts offline only.

## Sources

- github.com/zeroz41/pso-decomp (README)
- github.com/fuzziqersoftware/newserv and its mirror git.circlewithadot.net/incentive/psopeeps-newserv
- psobb.io/decryption, gitlab.psobb.io/psobb/original-psobb-client-source
- github.com/souzajluiz/PSOBB.web
- wiki.pioneer2.net/w/Game_modes (Ephinea wiki), phantasystar.fandom.com, segaretro.org (Blue Burst)
- phantasystardocs.github.io/file-formats/common-formats/sega-formats

# Upstreams

| Project | Pinned commit | License | What we take |
|---|---|---|---|
| [teclilla18/sonic-riders-recomp](https://github.com/teclilla18/sonic-riders-recomp) | `75380c099bcf439ad03ac9c5619dfe9d777eec18` (2026-11-19, after v0.8.7) | MPL-2.0 | The base: `tools/recomp/` (gcrecomp.py and the forked ZeldaWWHDRecomp translator), the runtime device models (VI, DI, CP/PE/PI GX FIFO, DSP/ARAM/AI with AX HLE, EXI, SI), the GX renderer and its deko3d and GL backends, the Switch platform layer. Riders-specific parts (`config/GXEE8P`, game mods, Riders Plus, netplay, launcher, Android/iOS) are not taken. |
| ZeldaWWHDRecomp translator | upstream commit f10b566 (as recorded by sonic-riders-recomp) | MPL-2.0 | Comes in through sonic-riders-recomp's fork. |
| [encounter/decomp-toolkit](https://github.com/encounter/decomp-toolkit) | v1.8.4 (the version sonic-riders-recomp uses) | MIT/Apache-2.0 | Analysis only: function boundaries, jump tables, relocations. Not a matching decomp. |

Files under MPL-2.0 keep that license, and changed MPL files must stay available in source form to anyone who
gets a binary (a public repo covers it).

Reference only (not copied): DolRecomp (GPL-3.0, evaluated by sonic-riders-recomp and rejected for speed),
newserv (PSO server and reverse-engineering notes, for the online code paths we stub).

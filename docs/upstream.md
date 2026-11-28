# Upstreams

| Project | Pinned commit | License | What we take |
|---|---|---|---|
| [teclilla18/sonic-riders-recomp](https://github.com/teclilla18/sonic-riders-recomp) | `75380c099bcf439ad03ac9c5619dfe9d777eec18` (2026-11-19, after v0.8.7) | MPL-2.0 | The base: `tools/recomp/` (gcrecomp.py and the forked ZeldaWWHDRecomp translator), the runtime device models (VI, DI, CP/PE/PI GX FIFO, DSP/ARAM/AI with AX HLE, EXI, SI), the GX renderer and its deko3d and GL backends, the Switch platform layer. Riders-specific parts (`config/GXEE8P`, game mods, Riders Plus, netplay, launcher, Android/iOS) are not taken. |
| ZeldaWWHDRecomp translator | upstream commit f10b566 (as recorded by sonic-riders-recomp) | MPL-2.0 | Comes in through sonic-riders-recomp's fork. |
| [facebook/zstd](https://github.com/facebook/zstd) | 1.5.7 (devkitPro portlib `switch-libzstd` 1.5.7-1) | BSD-3-Clause (dual BSD/GPLv2; we use it under BSD) | Linked into the Switch build: decompression for the first-start RVZ conversion (`runtime/src/platform/switch/rvz_decode.c`). Not copied into the repo. |
| [tukaani-project/xz](https://github.com/tukaani-project/xz) liblzma | 5.8.2 (portlib `switch-liblzma` 5.8.2-1) | 0BSD | Linked: LZMA and LZMA2 RVZ/WIA images. |
| [bzip2](https://sourceware.org/bzip2/) libbz2 | 1.0.8 (portlib `switch-bzip2` 1.0.8-1) | bzip2 license (BSD-style) | Linked: bzip2 RVZ/WIA images. |
| [phoboslab/pl_mpeg](https://github.com/phoboslab/pl_mpeg) | `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023` | MIT | Tests only, not shipped or copied: `tools/tests/run_movie_vlc_check.sh` fetches `pl_mpeg.h` into build/tests and checks the movie decoder's VLC tables (typed from ISO/IEC 11172-2 annex B in runtime/src/movie_mpeg.c) against its tables, every bit pattern. |
| [PyAV](https://github.com/PyAV-Org/PyAV) with FFmpeg | PyAV 19.0.2 (pip `av`, its bundled libavcodec) | BSD-3-Clause (PyAV); FFmpeg under its own licence (LGPL or GPL, as bundled) | Tests only, not shipped: `tools/tests/movie_ref_compare.py`, an independent MPEG-1 decoder to compare the native movie decoder's pictures with (PSNR). |
| [encounter/nod](https://github.com/encounter/nod) nodtool | 2.0.0-alpha.9 | MIT/Apache-2.0 | Tests only, not shipped or copied: an independent RVZ/WIA writer for `tools/tests/run_rvz_decode_test.sh` (`NODTOOL=`). |
| [zeroz41/pso-decomp](https://github.com/zeroz41/pso-decomp) | `6b38b53` (2026-11-27) | MIT | Reference for the right stick camera (`runtime/src/cam_stick.c`): the names, addresses and layouts of TCameraFollow, TCameraCtrl's view state, the controller input context and TWindowCtrl's window tests, each checked in PsoV3.dol's own code before use. No source copied; the hooks are our own. |
| [encounter/decomp-toolkit](https://github.com/encounter/decomp-toolkit) | v1.8.4 (the version sonic-riders-recomp uses) | MIT/Apache-2.0 | Analysis only: function boundaries, jump tables, relocations. Not a matching decomp. |

Files under MPL-2.0 keep that license, and changed MPL files must stay available in source form to anyone who
gets a binary (a public repo covers it).

The RVZ/WIA decoder in `rvz_decode.c` (MPL-2.0) is our C port of `tools/disc/rvz2iso.py` (from sonic-riders-recomp, MPL-2.0), checked against
Dolphin's published format description (docs/WiaAndRvz.md); no Dolphin or nod code was copied.

Reference only (not copied): DolRecomp (GPL-3.0, evaluated by sonic-riders-recomp and rejected for speed),
newserv (PSO server and reverse-engineering notes, for the online code paths we stub).

# PSOPlus-nx

Native Nintendo Switch homebrew (and PC) port of Phantasy Star Online Episode I & II Plus (GameCube, USA,
GPOE8P revision 2), by static recompilation. Offline modes first; online play is out of scope for now.

## Who does what

- the owner: owns the disc, runs PC builds in PowerShell on his PC, tests on his Switch (Atmosphere, sys-clk) and
  PC (PC), sends logs. He writes no code.
- Claude: writes all code, scripts and docs, reads logs, builds Switch NROs in the cloud when that is quicker.
- Instructions for the owner: numbered, click by click, copy-paste PowerShell.

## Hard rules

- Never commit game data or anything derived from it (disc, DOLs, RELs, assets, generated C, shader keys built
  from play). `.gitignore` covers the usual paths; run `git status` before every commit.
- No em dashes anywhere (chat, docs, commits, READMEs, release notes). No decorative emoji.
- Commit as `FurryLilManChild <346934328+FurryLilManChild@users.noreply.github.com>`.
- Running an upstream project's own scripts or building third-party code: the owner gave a standing OK for this
  project on 2026-11-19 (sonic-riders-recomp tools, decomp-toolkit, the RVZ converter).
- Upstream is a resource, never the design. sonic-riders-recomp is our starting point; we change anything that
  does not serve a native, fast port.
- Present findings and the plan for the next test to the owner and wait for his go-ahead before building it.

## Layout

- `assets/` (ignored): the owner's disc (.rvz) and `assets/extracted/` (main.dol, PsoV3.dol, file list).
- `tools/disc/`: `rvz2iso.py` (from sonic-riders-recomp, MPL-2.0) and `psodisc.py` (ours).
- `docs/design.md`: the plan, reviewed with the owner before build work. `docs/upstream.md`: pinned upstreams.
- `docs/progress-log.md`: newest first, one entry per session with evidence.

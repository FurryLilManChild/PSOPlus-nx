#!/usr/bin/env python3
"""List a GameCube ISO's files and extract main.dol, apploader info and every .rel/.str/.map-like file.

  psodisc.py ISO OUTDIR

Writes OUTDIR/files.txt (path, offset, size) and OUTDIR/sha1.txt. Never commit OUTDIR.
"""
import hashlib, os, struct, sys

EXTRACT_EXT = (".rel", ".str", ".map", ".sel", ".dol")


def main():
    iso, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    f = open(iso, "rb")
    hdr = f.read(0x440)
    game_id = hdr[0:6].decode("ascii", "replace")
    rev = hdr[7]
    title = hdr[0x20:0x60].split(b"\0")[0].decode("ascii", "replace")
    dol_off, fst_off, fst_size = struct.unpack(">III", hdr[0x420:0x42C])
    lines = [f"id {game_id} rev {rev} title {title}", f"dol 0x{dol_off:X} fst 0x{fst_off:X} size 0x{fst_size:X}"]
    # DOL size from its header
    f.seek(dol_off)
    dh = f.read(0x100)
    offs = struct.unpack(">18I", dh[0:72]); sizes = struct.unpack(">18I", dh[0x90:0x90 + 72])
    addrs = struct.unpack(">18I", dh[0x48:0x48 + 72])
    dol_len = max(o + s for o, s in zip(offs, sizes) if s)
    f.seek(dol_off); dol = f.read(dol_len)
    open(os.path.join(out, "main.dol"), "wb").write(dol)
    bss_addr, bss_size, entry = struct.unpack(">III", dh[0xD8:0xE4])
    lines.append(f"main.dol {dol_len} bytes entry 0x{entry:X} bss 0x{bss_addr:X}+0x{bss_size:X}")
    for i in range(18):
        if sizes[i]:
            kind = "text" if i < 7 else "data"
            lines.append(f"  {kind}{i if i < 7 else i - 7} 0x{addrs[i]:08X} size 0x{sizes[i]:X}")
    f.seek(fst_off); fst = f.read(fst_size)
    n = struct.unpack(">I", fst[8:12])[0]
    strtab = n * 12
    def name(o):
        e = fst.index(b"\0", strtab + o); return fst[strtab + o:e].decode("shift_jis", "replace")
    paths = [""] * n
    ends = []
    stack = [("", n)]
    entries = []
    for i in range(1, n):
        while stack and i >= stack[-1][1]:
            stack.pop()
        flags_name, a, b = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        is_dir = flags_name >> 24; nm = name(flags_name & 0xFFFFFF)
        p = stack[-1][0] + "/" + nm if stack else "/" + nm
        if is_dir:
            stack.append((p, b))
        else:
            entries.append((p, a, b))
    total = 0
    with open(os.path.join(out, "files.txt"), "w") as fl:
        for p, a, b in entries:
            fl.write(f"{p}\t0x{a:X}\t{b}\n"); total += b
    lines.append(f"{len(entries)} files, {total} bytes")
    sha = [f"{hashlib.sha1(dol).hexdigest()}  main.dol"]
    for p, a, b in entries:
        if p.lower().endswith(EXTRACT_EXT):
            f.seek(a); d = f.read(b)
            dst = os.path.join(out, "files", p.lstrip("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(d)
            sha.append(f"{hashlib.sha1(d).hexdigest()}  {p}")
    open(os.path.join(out, "sha1.txt"), "w").write("\n".join(sha) + "\n")
    open(os.path.join(out, "summary.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


main()

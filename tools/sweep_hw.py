#!/usr/bin/env python3
"""Whole-program hardware sweep for PsoV3.dol (switch-port-base: "every hardware register access").

  sweep_hw.py BASEROM/PsoV3.dol CONFIG/symbols.txt [--exceptions config/GPOE8P/hw_exceptions.txt]

Lists, for every function, the hardware it touches directly:
  - memory-mapped registers: `lis rX, 0xCC00` (or 0xCC01) followed by loads/stores off rX in the same
    function, grouped by device block and offset (the runtime models them in runtime/src/mmio.c);
  - special-purpose registers moved to or from (mtspr/mfspr), other than LR/CTR/XER and the GQRs, which
    the translator handles as ordinary state;
  - locked-cache DMA (DMAU/DMAL) and cache-control instructions (dcbz_l, icbi, dcbi);
  - every function containing rfi (the runtime's ppc_rfi stops the game): it must be replaced in hle.txt
    or listed as never reached.
The MetroTRK debugger stub (functions named TRK*, InitMetroTRK, and everything between the first and
last of them) never runs in the port and is skipped.
Every device block the game touches must be one the runtime models, or listed in the exceptions file
with the reason it is safe. Prints PASS or FAIL. Nothing from the game is written to disk.
"""
import argparse
import collections
import re
import struct
import sys

MODELLED = {  # runtime/src/mmio.c
    0xCC000000: "CP", 0xCC001000: "PE", 0xCC002000: "VI", 0xCC003000: "PI", 0xCC005000: "DSP",
    0xCC006000: "DI", 0xCC006400: "SI", 0xCC006800: "EXI", 0xCC006C00: "AI", 0xCC008000: "GX FIFO",
}
BLOCKS = [(0xCC000000, 0x80, "CP"), (0xCC001000, 0x100, "PE"), (0xCC002000, 0x100, "VI"), (0xCC003000, 0x100, "PI"),
          (0xCC004000, 0x80, "MI"), (0xCC005000, 0x200, "DSP"), (0xCC006000, 0x40, "DI"), (0xCC006400, 0x100, "SI"),
          (0xCC006800, 0x40, "EXI"), (0xCC006C00, 0x20, "AI"), (0xCC008000, 0x20, "GX FIFO")]
SPR_NAMES = {1: "XER", 8: "LR", 9: "CTR", 18: "DSISR", 19: "DAR", 22: "DEC", 25: "SDR1", 26: "SRR0", 27: "SRR1",
             272: "SPRG0", 273: "SPRG1", 274: "SPRG2", 275: "SPRG3", 282: "EAR", 284: "TBL(w)", 285: "TBU(w)",
             268: "TBL", 269: "TBU", 287: "PVR", 912: "GQR0", 913: "GQR1", 914: "GQR2", 915: "GQR3", 916: "GQR4",
             917: "GQR5", 918: "GQR6", 919: "GQR7", 920: "HID2", 921: "WPAR", 922: "DMAU", 923: "DMAL",
             936: "UMMCR0", 940: "UMMCR1", 952: "MMCR0", 953: "PMC1", 954: "PMC2", 955: "SIA", 956: "MMCR1",
             957: "PMC3", 958: "PMC4", 1008: "HID0", 1009: "HID1", 1010: "IABR", 1013: "DABR", 1017: "L2CR",
             1019: "ICTC", 1020: "THRM1", 1021: "THRM2", 1022: "THRM3"}
PLAIN_SPRS = {1, 8, 9} | set(range(912, 920))


def block_of(a):
    for lo, n, name in BLOCKS:
        if lo <= a < lo + n:
            return lo, name
    return None, "unknown %08X" % a


def load_dol(path):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[:72]); addrs = struct.unpack(">18I", d[0x48:0x90]); sizes = struct.unpack(">18I", d[0x90:0xD8])
    return [(addrs[i], d[offs[i]:offs[i] + sizes[i]]) for i in range(7) if sizes[i]]


def load_funcs(path):
    fs = []
    for line in open(path):
        m = re.match(r"(\S+) = \.(?:init|text):0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)", line)
        if m:
            fs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    return sorted(fs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol"); ap.add_argument("symbols")
    ap.add_argument("--exceptions", default="")
    ap.add_argument("--hle", default="")
    a = ap.parse_args()
    exc = {}
    if a.exceptions:
        for line in open(a.exceptions):
            line = line.split("#", 1)
            key = line[0].strip()
            if key:
                exc[key] = line[1].strip() if len(line) > 1 else ""
    text = load_dol(a.dol)

    def word(addr):
        for lo, data in text:
            if lo <= addr < lo + len(data):
                return struct.unpack_from(">I", data, addr - lo)[0]
        return None

    funcs = load_funcs(a.symbols)
    trk = [f for f in funcs if f[2].startswith("TRK") or f[2] == "InitMetroTRK"]
    trk_lo, trk_hi = (min(f[0] for f in trk), max(f[0] + f[1] for f in trk)) if trk else (0, 0)
    hle = set()
    if a.hle:
        for line in open(a.hle):
            line = line.split("#")[0].split()
            if line:
                hle.add(int(line[0], 16))
    rfi_funcs = []
    mmio = collections.defaultdict(set)    # block name -> {(offset, function)}
    sprs = collections.defaultdict(set)    # spr name -> {function}
    other = collections.defaultdict(set)
    for start, size, name in funcs:
        if trk_lo <= start < trk_hi:
            continue
        base = {}  # register -> high half loaded by lis
        for pc in range(start, start + size, 4):
            w = word(pc)
            if w is None:
                break
            op = w >> 26
            if w == 0x4C000064:  # rfi
                rfi_funcs.append((start, name))
            if op == 18 and not (w & 1):  # unconditional branch: what follows starts another path
                base.clear()
            rt, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:  # lis
                if imm in (0xCC00, 0xCC01):
                    base[rt] = imm << 16
                else:
                    base.pop(rt, None)
                continue
            if op in (32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55) and ra in base:
                ea = (base[ra] + simm) & 0xFFFFFFFF
                lo, bname = block_of(ea)
                mmio[bname].add((ea - (lo or 0), name))
                if op in (32, 33, 34, 35, 40, 41, 42, 43, 48, 49, 50, 51) and rt in base:  # a load overwrote the base
                    base.pop(rt, None)
                continue
            if op == 14 and ra in base and rt != ra:  # addi rX, base, off: a derived pointer
                lo, bname = block_of((base[ra] + simm) & 0xFFFFFFFF)
                mmio[bname].add(((base[ra] + simm - (lo or 0)) & 0xFFFFFFFF, name + " (addi)"))
            if op == 31:
                xo = (w >> 1) & 0x3FF
                if xo in (339, 467):  # mfspr / mtspr
                    spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                    if spr not in PLAIN_SPRS:
                        sprs[("mfspr " if xo == 339 else "mtspr ") + SPR_NAMES.get(spr, str(spr))].add(name)
                elif xo == 470:
                    other["dcbi"].add(name)
                elif xo == 982:
                    other["icbi"].add(name)
                elif xo == 83 or xo == 146:
                    other["mfmsr" if xo == 83 else "mtmsr"].add(name)
                if rt in base and xo not in (339, 467):
                    base.pop(rt, None)
            elif op == 4 and ((w >> 1) & 0x3FF) == 1014:
                other["dcbz_l (locked cache)"].add(name)
            elif rt in base and op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55):
                base.pop(rt, None)

    fail = 0
    print("== memory-mapped registers by device block")
    for bname in sorted(mmio):
        offs = sorted({o for o, _ in mmio[bname]})
        funcs = sorted({f for _, f in mmio[bname]})
        modelled = bname in MODELLED.values()
        key = "mmio " + bname
        status = "modelled" if modelled else ("exception: " + exc[key] if key in exc else "NOT MODELLED")
        if not modelled and key not in exc:
            fail += 1
        print("  %-12s %-40s offsets %s; %d functions (%s%s)" % (
            bname, status, " ".join("%X" % o for o in offs[:16]) + (" ..." if len(offs) > 16 else ""), len(funcs),
            ", ".join(funcs[:4]), ", ..." if len(funcs) > 4 else ""))
    print("== special-purpose registers and privileged instructions")
    for k in sorted(set(sprs) | set(other)):
        funcs = sorted(sprs.get(k, set()) | other.get(k, set()))
        status = "exception: " + exc[k] if k in exc else "CHECK"
        if k not in exc:
            fail += 1
        print("  %-24s %-50s %d functions (%s%s)" % (k, status, len(funcs), ", ".join(funcs[:4]), ", ..." if len(funcs) > 4 else ""))
    print("== functions with rfi")
    for start, name in sorted(set(rfi_funcs)):
        key = "rfi %s" % name
        if start in hle:
            status = "replaced (hle.txt)"
        elif key in exc:
            status = "exception: " + exc[key]
        else:
            status = "NOT REPLACED"
            fail += 1
        print("  %08X %-28s %s" % (start, name, status))
    print("(MetroTRK %08X-%08X skipped)" % (trk_lo, trk_hi))
    print("RESULT: %s (%d unexplained)" % ("PASS" if not fail else "FAIL", fail))
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()

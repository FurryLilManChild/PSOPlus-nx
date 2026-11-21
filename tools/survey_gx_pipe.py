#!/usr/bin/env python3
"""Survey: which functions of the game write the GX write-gather pipe (0xCC008000) directly, and how.

usage: python tools/survey_gx_pipe.py baserom/PsoV3.dol config/GPOE8P/symbols.txt [--profile psoplus.log]

For every function (dtk's symbols) the code is scanned in order, tracking registers that hold a known
constant (lis/addis, addi, ori, mr). A store whose address is base + displacement = 0xCC008000 is a
pipe store. Reported per function: the store count by kind (stw/sth/stb words, stfs floats, psq_st
pairs, stfd), and the size. With --profile, the profiler's per-function share from a Switch log is
put beside each function (the busiest window of the log).

The output names addresses only (no game code or data); it is for docs/design.md's native GX plan."""
import argparse
import bisect
import re
import struct
import sys

PIPE = 0xCC008000


def load_dol(path):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[0:72])
    addrs = struct.unpack(">18I", d[72:144])
    sizes = struct.unpack(">18I", d[144:216])
    text = []
    for i in range(7):
        if sizes[i]:
            text.append((addrs[i], d[offs[i]:offs[i] + sizes[i]]))
    return text


def load_syms(path):
    out = []
    for line in open(path):
        m = re.match(r"(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function(?: size:0x([0-9A-Fa-f]+))?", line)
        if m and m.group(3):
            out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    out.sort()
    return out


def word_at(text, a):
    for base, data in text:
        if base <= a < base + len(data):
            return struct.unpack(">I", data[a - base:a - base + 4])[0]
    return None


def simm(x):
    return x - 0x10000 if x & 0x8000 else x


STORES = {36: ("word", 4), 37: ("word", 4), 44: ("half", 2), 45: ("half", 2), 38: ("byte", 1), 39: ("byte", 1),
          52: ("float", 4), 53: ("float", 4), 54: ("double", 8), 55: ("double", 8)}


def scan(text, start, size):
    regs = {}
    kinds = {}
    for a in range(start, start + size, 4):
        w = word_at(text, a)
        if w is None:
            break
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15:  # addis / lis
            regs[rd] = ((regs.get(ra, 0) if ra else 0) + (imm << 16)) & 0xFFFFFFFF if (ra == 0 or ra in regs) else None
            if regs[rd] is None: del regs[rd]
        elif op == 14:  # addi / li
            if ra == 0: regs[rd] = simm(imm) & 0xFFFFFFFF
            elif ra in regs: regs[rd] = (regs[ra] + simm(imm)) & 0xFFFFFFFF
            else: regs.pop(rd, None)
        elif op == 24:  # ori: rA = rS | imm
            if rd in regs: regs[ra] = regs[rd] | imm
            else: regs.pop(ra, None)
        elif op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == ((w >> 11) & 31):  # mr rA, rS
            if rd in regs: regs[ra] = regs[rd]
            else: regs.pop(ra, None)
        elif op in STORES or op == 60 or op == 61:  # stores (60/61 psq_st/psq_stu with 12-bit offset)
            if op in (60, 61):
                d = w & 0xFFF
                d = d - 0x1000 if d & 0x800 else d
                kind = "pair"
            else:
                d = simm(imm)
                kind = STORES[op][0]
            if ra in regs and (regs[ra] + d) & 0xFFFFFFFF == PIPE:
                kinds[kind] = kinds.get(kind, 0) + 1
            if op in (37, 39, 45, 53, 55, 61):  # update forms change rA
                regs.pop(ra, None)
        elif op in (32, 33, 34, 35, 40, 41, 42, 43, 48, 49, 50, 51, 56, 57):  # loads: rD changes
            regs.pop(rd, None)
        elif op == 18 and (w & 1):  # bl: volatile registers are lost
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                regs.pop(r, None)
        elif op in (31, 21, 20, 23, 28, 29, 25, 26, 27, 7, 8, 10, 11, 12, 13):  # other integer writes
            tgt = ra if op in (21, 20, 23, 28, 29, 25, 26, 27) or (op == 31 and ((w >> 1) & 0x3FF) in (444, 28, 60, 124, 316, 476, 412, 24, 536, 792, 824, 954, 922, 26)) else rd
            regs.pop(tgt, None)
    return kinds


def profile_shares(path):
    lines = open(path, errors="replace").read().split("\n")
    best, cur, cur_n = {}, None, 0
    windows = []
    for l in lines:
        m = re.match(r"\[profile\] (\d+) samples", l)
        if m:
            cur = {}
            windows.append(cur)
            continue
        if l.startswith("[profile] by function and return"):
            cur = None
            continue
        m = re.match(r"\[profile\]\s+([\d.]+)%\s+([0-9A-F]{8})$", l)
        if m and cur is not None:
            cur[int(m.group(2), 16)] = float(m.group(1))
    # the window with the least time in the idle loop's OSEnableInterrupts: the busiest
    return min(windows, key=lambda w: w.get(0x8037222C, 100.0)) if windows else {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol")
    ap.add_argument("symbols")
    ap.add_argument("--profile")
    args = ap.parse_args()
    text = load_dol(args.dol)
    syms = load_syms(args.symbols)
    prof = profile_shares(args.profile) if args.profile else {}
    rows = []
    for a, size, name in syms:
        k = scan(text, a, size)
        if k:
            rows.append((a, size, name, k))
    total_sites = sum(sum(k.values()) for _, _, _, k in rows)
    print("functions storing to the GX pipe directly: %d, store instructions: %d" % (len(rows), total_sites))
    by_kind = {}
    for _, _, _, k in rows:
        for kk, n in k.items():
            by_kind[kk] = by_kind.get(kk, 0) + n
    print("by kind: " + ", ".join("%s %d" % kv for kv in sorted(by_kind.items(), key=lambda x: -x[1])))
    starts = [s[0] for s in syms]
    def owner(x):
        i = bisect.bisect_right(starts, x) - 1
        return syms[i] if i >= 0 and x < syms[i][0] + syms[i][1] else None
    if prof:
        print("\nprofile (busiest window): share, function, size, pipe stores in it")
        pipe_fns = {a for a, _, _, _ in rows}
        tot_pipe = 0.0
        for x, share in sorted(prof.items(), key=lambda kv: -kv[1]):
            o = owner(x)
            k = next((r[3] for r in rows if r[0] == x), None)
            if k: tot_pipe += share
            print("  %5.1f%%  %08X  %-28s %6s  %s" % (share, x, o[2] if o else "(tag)", hex(o[1]) if o else "",
                                                   ", ".join("%s %d" % kv for kv in k.items()) if k else ""))
        print("share of the top-40 samples in functions that write the pipe: %.1f%%" % tot_pipe)
    print("\nall functions with pipe stores (address, size, name, stores):")
    for a, size, name, k in rows:
        print("  %08X %6s %-28s %s" % (a, hex(size), name, ", ".join("%s %d" % kv for kv in sorted(k.items()))))


if __name__ == "__main__":
    main()

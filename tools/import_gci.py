#!/usr/bin/env python3
"""Import GameCube save files (.gci) into the port's memory card image (saves/card_a.raw).

usage: python tools/import_gci.py CARD.raw FILE.gci [FILE.gci ...] [--out NEW.raw]

The card image is a standard 4 Mbit GameCube card (the layout Dolphin's .raw files use): block 0
header, blocks 1-2 directory, blocks 3-4 block allocation table (BAT), data from block 5. A file on
the card with the same game code, maker code and file name as an imported one is replaced.

PSO saves are tied to the card they were made on: PSO_SYSTEM carries the card's serial (from the
header) and a CRC-32 over it. As Dolphin's memory card manager does on import
(GCMemcard::PSO_MakeSaveGameValid), the serial of this card is written into PSO_SYSTEM and the CRC
recomputed; the character and Guild Card files are copied unchanged. Before changing anything the
tool checks that its CRC matches the one already in the imported PSO_SYSTEM.

Without --out the card is rewritten in place after a backup (CARD.before-import.raw)."""
import argparse
import os
import shutil
import struct
import sys

BLOCK = 0x2000
DENTRY = 0x40


def checksum(data):
    """the CARD checksum pair (runtime/src/exi.c card_checksum)"""
    c1 = c2 = 0
    for i in range(0, len(data), 2):
        w = (data[i] << 8) | data[i + 1]
        c1 = (c1 + w) & 0xFFFF
        c2 = (c2 + (~w & 0xFFFF)) & 0xFFFF
    if c1 == 0xFFFF:
        c1 = 0
    if c2 == 0xFFFF:
        c2 = 0
    return struct.pack(">HH", c1, c2)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def pick(card, a, b, sum_off, sum_len, ctr_off, sum_at):
    """the valid copy with the newer update counter (directory or BAT)"""
    best = None
    for blk in (a, b):
        d = card[blk * BLOCK:(blk + 1) * BLOCK]
        ok = checksum(d[sum_off:sum_off + sum_len]) == d[sum_at:sum_at + 4]
        ctr = s16(struct.unpack(">H", d[ctr_off:ctr_off + 2])[0])
        if ok and (best is None or ctr > best[1]):
            best = (bytearray(d), ctr)
    if best is None:
        sys.exit("the card's directory or allocation table is damaged (both copies fail their checksum)")
    return best


CRC_LUT = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    CRC_LUT.append(c)


def pso_crc(block1, end):
    crc = 0xDEBB20E3
    for i in range(0x4C, end):
        crc = ((crc >> 8) & 0xFFFFFF) ^ CRC_LUT[(crc ^ block1[i]) & 0xFF]
    return struct.pack(">I", crc ^ 0xFFFFFFFF)


def pso_fix(name, data, header):
    """PSO_SYSTEM / PSO3_SYSTEM: this card's serial and a new CRC (Dolphin's PSO_MakeSaveGameValid)"""
    extra = {b"PSO_SYSTEM": 0, b"PSO3_SYSTEM": 0x10}.get(name)
    if extra is None:
        return data, ""
    b1 = bytearray(data[BLOCK:2 * BLOCK])
    end = 0x164 + extra
    if pso_crc(b1, end) != bytes(b1[0x48:0x4C]):
        sys.exit("%s: its CRC does not match what this tool computes; nothing changed" % name.decode())
    old = bytes(b1[0x158:0x160])
    s1 = bytes(header[i] ^ header[8 + i] ^ header[16 + i] ^ header[24 + i] for i in range(4))
    s2 = bytes(header[4 + i] ^ header[12 + i] ^ header[20 + i] ^ header[28 + i] for i in range(4))
    b1[0x158:0x15C] = s1
    b1[0x15C:0x160] = s2
    b1[0x48:0x4C] = pso_crc(b1, end)
    note = "card serial %s -> %s, CRC checked and redone" % (old.hex(), (s1 + s2).hex())
    return data[:BLOCK] + bytes(b1) + data[2 * BLOCK:], note


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("card")
    ap.add_argument("gci", nargs="+")
    ap.add_argument("--out", help="write the result here (default: the card itself, after a backup)")
    args = ap.parse_args()

    card = bytearray(open(args.card, "rb").read())
    if len(card) < 5 * BLOCK or len(card) % BLOCK:
        sys.exit("%s is not a memory card image (%d bytes)" % (args.card, len(card)))
    nblocks = len(card) // BLOCK
    header = bytes(card[:0x20])
    dirb, dctr = pick(card, 1, 2, 0, 0x1FFC, 0x1FFA, 0x1FFC)
    bat, bctr = pick(card, 3, 4, 4, 0x1FFC, 4, 0)

    def bat_get(blk):
        return struct.unpack(">H", bat[0x0A + (blk - 5) * 2:0x0C + (blk - 5) * 2])[0]

    def bat_set(blk, v):
        bat[0x0A + (blk - 5) * 2:0x0C + (blk - 5) * 2] = struct.pack(">H", v)

    def entries():
        for n in range(127):
            e = dirb[n * DENTRY:(n + 1) * DENTRY]
            yield n, e

    for path in args.gci:
        g = open(path, "rb").read()
        if len(g) < DENTRY + BLOCK or (len(g) - DENTRY) % BLOCK:
            sys.exit("%s is not a .gci file (%d bytes)" % (path, len(g)))
        ent = bytearray(g[:DENTRY])
        data = g[DENTRY:]
        count = len(data) // BLOCK
        if struct.unpack(">H", ent[0x38:0x3A])[0] != count:
            sys.exit("%s: its block count does not match its size" % path)
        name = bytes(ent[8:0x28]).split(b"\0")[0]
        ident = bytes(ent[0:6]) + bytes(ent[8:0x28])
        data, note = pso_fix(name, data, header)
        # replace a file of the same name: free its blocks and its directory slot
        for n, e in entries():
            if e[0:4] != b"\xff\xff\xff\xff" and bytes(e[0:6]) + bytes(e[8:0x28]) == ident:
                blk = struct.unpack(">H", e[0x36:0x38])[0]
                for _ in range(struct.unpack(">H", e[0x38:0x3A])[0]):
                    nxt = bat_get(blk)
                    bat_set(blk, 0)
                    blk = nxt
                dirb[n * DENTRY:(n + 1) * DENTRY] = b"\xff" * DENTRY
                print("  replaced the card's %s" % name.decode())
        slot = next((n for n, e in entries() if e[0:4] == b"\xff\xff\xff\xff"), None)
        free = [b for b in range(5, nblocks) if bat_get(b) == 0]
        if slot is None or len(free) < count:
            sys.exit("%s: the card is full (%d free blocks, %d needed)" % (path, len(free), count))
        use = free[:count]
        for i, b in enumerate(use):
            bat_set(b, use[i + 1] if i + 1 < count else 0xFFFF)
            card[b * BLOCK:(b + 1) * BLOCK] = data[i * BLOCK:(i + 1) * BLOCK]
        ent[0x36:0x38] = struct.pack(">H", use[0])
        dirb[slot * DENTRY:(slot + 1) * DENTRY] = ent
        print("  %s: %d blocks at %d%s" % (name.decode(), count, use[0], (", " + note) if note else ""))

    nfree = sum(1 for b in range(5, nblocks) if bat_get(b) == 0)
    bat[6:8] = struct.pack(">H", nfree)
    bat[8:10] = struct.pack(">H", max([4] + [b for b in range(5, nblocks) if bat_get(b) != 0]))
    dctr = (dctr + 1) & 0xFFFF
    bctr = (bctr + 1) & 0xFFFF
    dirb[0x1FFA:0x1FFC] = struct.pack(">H", dctr)
    dirb[0x1FFC:0x2000] = checksum(dirb[0:0x1FFC])
    bat[4:6] = struct.pack(">H", bctr)
    bat[0:4] = checksum(bat[4:0x2000])
    for k in (1, 2):
        card[k * BLOCK:(k + 1) * BLOCK] = dirb
    for k in (3, 4):
        card[k * BLOCK:(k + 1) * BLOCK] = bat

    out = args.out or args.card
    if not args.out:
        backup = os.path.splitext(args.card)[0] + ".before-import.raw"
        shutil.copyfile(args.card, backup)
        print("  backup: %s" % backup)
    open(out, "wb").write(card)
    print("RESULT: imported %d file(s) into %s, %d blocks free" % (len(args.gci), out, nfree))


if __name__ == "__main__":
    main()

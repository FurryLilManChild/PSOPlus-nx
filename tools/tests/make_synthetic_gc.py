"""Write a small synthetic GameCube disc image (no game data) for the RVZ decoder tests.

    python -I tools/tests/make_synthetic_gc.py OUT.iso [--size MIB] [--seed N]

The image has a GameCube disc header (game ID ZTST01, a made-up ID), a tiny apploader and DOL, an FST with
files of random, repetitive and all-zero contents at unaligned sizes, and the gaps between files filled with
the disc's junk data (the lagged Fibonacci generator seeded from the game ID and the 32 KiB sector, as
pressed discs have it). RVZ writers that pack junk (Dolphin, nod) then store those gaps as seeds, so the
decoder's junk regeneration is exercised.
"""
import argparse
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "disc"))
import rvz2iso  # noqa: E402  (our Junk generator)

SECTOR = 0x8000
GAME_ID = b"ZTST01"


def junk_seed(disc_id, disc_num, sector):
    """the generator's seed for a sector (17 big-endian words), as pressed discs and Dolphin/nod compute it"""
    seed = ((disc_id[2] << 24) | (disc_id[1] << 16) | (((disc_id[3] + disc_id[2]) & 0xFF) << 8) |
            ((disc_id[0] + disc_id[1]) & 0xFF)) ^ disc_num
    n = (seed * 0x260BCD5 ^ sector * 0x1EF29123) & 0xFFFFFFFF
    out = []
    for _ in range(17):
        v = 0
        for _ in range(32):
            n = (n * 0x5D588B65 + 1) & 0xFFFFFFFF
            v = (v >> 1) | (n & 0x80000000)
        out.append(v)
    out[16] ^= ((out[0] >> 9) ^ (out[16] << 23)) & 0xFFFFFFFF
    return struct.pack(">17I", *out)


def fill_junk(img, start, end):
    off = start
    while off < end:
        sector = off // SECTOR
        n = min(end, (sector + 1) * SECTOR) - off
        j = rvz2iso.Junk(junk_seed(GAME_ID[:4], 0, sector))
        j.skip(off % SECTOR)
        img[off:off + n] = j.take(n)
        off += n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=24, help="image size in MiB")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    size = a.size << 20
    img = bytearray(size)

    # disc header
    img[0:6] = GAME_ID
    img[6] = 0      # disc number
    img[7] = 2      # revision
    struct.pack_into(">I", img, 0x1C, 0xC2339F3D)
    title = b"RVZ DECODER SYNTHETIC TEST DISC"
    img[0x20:0x20 + len(title)] = title

    # apploader at 0x2440: date, entry, size, trailer size, code
    app_code = bytes(rnd.getrandbits(8) for _ in range(0x200))
    img[0x2440:0x2450] = b"2026/11/24\0\0\0\0\0\0"
    struct.pack_into(">III", img, 0x2450, 0x81200000, len(app_code), 0)
    img[0x2460:0x2460 + len(app_code)] = app_code

    # a DOL with one text section
    dol_off = 0x4000
    text = bytes(rnd.getrandbits(8) for _ in range(0x1000))
    hdr = bytearray(0x100)
    struct.pack_into(">I", hdr, 0x00, 0x100)          # text0 file offset
    struct.pack_into(">I", hdr, 0x48, 0x80003100)     # text0 address
    struct.pack_into(">I", hdr, 0x90, len(text))      # text0 size
    struct.pack_into(">III", hdr, 0xD8, 0x80100000, 0x1000, 0x80003100)  # bss address, size, entry
    img[dol_off:dol_off + 0x100] = hdr
    img[dol_off + 0x100:dol_off + 0x100 + len(text)] = text
    dol_end = dol_off + 0x100 + len(text)

    # files: (name, size, kind)
    specs = [("random.bin", 0x23456, "random"), ("text.txt", 0x31111, "text"), ("zeros.bin", 0x20003, "zeros"),
             ("mixed.bin", 0x88888, "mixed"), ("small.bin", 0x123, "random"), ("big.bin", 0x1A0001, "random"),
             ("tail.bin", 0x7FFF, "text")]
    fst_off = 0x6000
    names = b""
    name_offs = []
    for nm, _, _ in specs:
        name_offs.append(len(names))
        names += nm.encode() + b"\0"
    fst_size = 12 * (len(specs) + 1) + len(names)
    fst = bytearray(fst_size)
    pos = 0x40000
    file_ranges = []
    for i, (nm, fs, kind) in enumerate(specs):
        pos += rnd.randrange(0, 0x30000) & ~3     # files are 4-byte aligned; gaps of junk between them
        if kind == "random":
            data = bytes(rnd.getrandbits(8) for _ in range(fs))
        elif kind == "text":
            line = b"The quick brown fox jumps over the lazy dog. 0123456789\n"
            data = (line * (fs // len(line) + 1))[:fs]
        elif kind == "zeros":
            data = bytes(fs)
        else:
            parts = []
            while sum(map(len, parts)) < fs:
                parts.append(bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(16, 4096))) if rnd.random() < .5
                             else bytes(rnd.randrange(16, 8192)))
            data = b"".join(parts)[:fs]
        if pos + fs > size:
            sys.exit("image too small for the files (raise --size)")
        img[pos:pos + fs] = data
        struct.pack_into(">III", fst, 12 * (i + 1), name_offs[i], pos, fs)  # file: flags 0, name, offset, size
        file_ranges.append((pos, pos + fs))
        pos += fs
    struct.pack_into(">III", fst, 0, 0x01000000, 0, len(specs) + 1)  # root directory: entry count
    fst[12 * (len(specs) + 1):] = names
    img[fst_off:fst_off + fst_size] = fst

    # boot header: DOL offset, FST offset, size, max size
    struct.pack_into(">IIII", img, 0x420, dol_off, fst_off, fst_size, fst_size)

    # junk between the FST and the first file, between files and after the last one
    used_end = max(fst_off + fst_size, dol_end)
    prev = (used_end + 3) & ~3
    for s, e in file_ranges:
        if s > prev:
            fill_junk(img, prev, s)
        prev = e
    fill_junk(img, prev, size)
    with open(a.out, "wb") as f:
        f.write(img)
    print("wrote %s (%d bytes, %d files)" % (a.out, size, len(specs)))


if __name__ == "__main__":
    main()

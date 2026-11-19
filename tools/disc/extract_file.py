#!/usr/bin/env python3
"""Copy one file out of a GameCube disc image (ISO), by its path in the disc's file system.

  extract_file.py IMAGE.iso PsoV3.dol OUT [--sha1 HEX]

PSOPlus-nx: the build needs PsoV3.dol in baserom/ for the recompiler. --sha1 checks the copy and
deletes it on a mismatch. The output is game data: keep it out of git (baserom/ is ignored).
"""
import argparse
import hashlib
import os
import struct
import sys


def find(f, want):
    f.seek(0x424)
    fst_off, fst_size = struct.unpack(">II", f.read(8))
    f.seek(fst_off)
    fst = f.read(fst_size)
    n = struct.unpack(">I", fst[8:12])[0]
    strtab = n * 12
    for i in range(1, n):
        word, off, size = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        if word >> 24:
            continue
        start = strtab + (word & 0xFFFFFF)
        name = fst[start:fst.index(b"\0", start)].decode("latin-1")
        if name.lower() == want.lower():  # names in the root folder (PsoV3.dol is there)
            return off, size
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image"); ap.add_argument("name"); ap.add_argument("out")
    ap.add_argument("--sha1", default="")
    a = ap.parse_args()
    with open(a.image, "rb") as f:
        hit = find(f, a.name)
        if not hit:
            sys.exit("%s: no file named %s on the disc" % (a.image, a.name))
        f.seek(hit[0])
        data = f.read(hit[1])
    digest = hashlib.sha1(data).hexdigest()
    if a.sha1 and digest != a.sha1.lower():
        sys.exit("%s from %s has SHA-1 %s, expected %s (a different disc revision?)" % (a.name, a.image, digest, a.sha1))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as o:
        o.write(data)
    print("%s: %d bytes, SHA-1 %s" % (a.out, len(data), digest))


main()

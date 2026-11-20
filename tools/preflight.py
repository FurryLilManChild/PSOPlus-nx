#!/usr/bin/env python3
"""Gate 2: whole-program checks before the owner gets build commands (switch-port-base "The five gates").

  python3 tools/preflight.py --llvm-mingw DIR --sdl DIR --imgui DIR [--skip-build]

Runs in the cloud on a clean push. Prints PASS or FAIL per check and a final RESULT line, which the reply
to the owner quotes. Checks:
  tree       the working tree is clean and HEAD is what GitHub's main has (the build the owner gets)
  codegen    gcrecomp's report: only the explained leftovers (data words, the 0x8000C2FC string, the
             __ptmf_scall dispatch at 0x80396A74; movie.rel's 7 switch tables found), and every hle.txt line became a native replacement
  compile    the PC build with the same clang (llvm-mingw) and SDL2 as the owner's PC: every generated file
             and runtime source compiles and the program links
  hardware   tools/sweep_hw.py: every device block, special register and rfi function is modelled,
             replaced, or explained in config/GPOE8P/hw_exceptions.txt
  addresses  every hard-coded guest address in runtime/ is listed with its evidence in
             config/GPOE8P/addresses.txt (a base-port address left behind is the failure this catches)
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = "config/GPOE8P"
results = []


def check(name, ok, detail):
    results.append((name, ok))
    print("%-10s %s  %s" % (name, "PASS" if ok else "FAIL", detail))


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def tree():
    st = run(["git", "status", "--porcelain", "--untracked-files=no"]).stdout.strip()
    run(["git", "fetch", "-q", "origin", "main"])
    head = run(["git", "rev-parse", "HEAD"]).stdout.strip()
    remote = run(["git", "rev-parse", "origin/main"]).stdout.strip()
    ok = not st and head == remote
    check("tree", ok, "HEAD %s%s%s" % (head[:8], "" if head == remote else " differs from origin/main %s" % remote[:8],
                                       "; local changes:\n" + st if st else ""))
    return head[:8]


def codegen():
    r = run([sys.executable, "tools/recomp/gcrecomp.py", GAME, "baserom", "build/gen"])
    if r.returncode:
        check("codegen", False, r.stderr.strip()[-400:])
        return
    rep = open(os.path.join(ROOT, "build/gen/report.txt")).read()
    problems = []
    m = re.search(r"switch dispatches without a jump table: (\d+)(.*)", rep)
    if m and m.group(2).split() != ["80396A74"]:
        problems.append("unexpected table-less dispatches:%s" % m.group(2))
    unhandled = re.findall(r"^\s+(\d+)\s+(op.*)$", rep, re.M)
    for count, kind in unhandled:
        if kind != "op=0" and kind != "op19 xo=569":
            problems.append("unhandled %s x%s" % (kind, count))
    want = sum(1 for line in open(os.path.join(ROOT, GAME, "hle.txt")) if line.split("#")[0].strip())
    got = int(re.search(r"native replacements: (\d+)", rep).group(1))
    if got != want:
        problems.append("hle.txt has %d lines, %d replacements made" % (want, got))
    funcs = re.search(r"functions: (\d+)", rep).group(1)
    check("codegen", not problems, "%s functions, %d native replacements%s" % (funcs, got, "; " + "; ".join(problems) if problems else ""))


def compile_check(a):
    r = run([sys.executable, "configure.py", "--target", "host", "--toolchain", "mingw", "--llvm-mingw", a.llvm_mingw,
             "--sdl", a.sdl, "--imgui", a.imgui])
    if r.returncode:
        check("compile", False, "configure: " + (r.stdout + r.stderr).strip()[-400:])
        return
    r = run(["ninja", "-k", "0"])
    errs = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l or l.startswith("FAILED")]
    exe = os.path.join(ROOT, "build/host-mingw/psoplus.exe")
    ok = r.returncode == 0 and os.path.isfile(exe)
    check("compile", ok, ("psoplus.exe %.1f MB" % (os.path.getsize(exe) / 1e6)) if ok else "\n" + "\n".join(errs[:30]))


def hardware():
    r = run([sys.executable, "tools/sweep_hw.py", "baserom/PsoV3.dol", GAME + "/symbols.txt", "--hle", GAME + "/hle.txt",
             "--exceptions", GAME + "/hw_exceptions.txt", "--rel", "baserom/movie.rel", GAME + "/movie/symbols.txt"])
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip()[-200:]
    bad = [l.strip() for l in r.stdout.splitlines() if "NOT " in l or " CHECK " in l]
    check("hardware", r.returncode == 0, last + ("; " + "; ".join(bad[:10]) if bad else ""))


def addresses():
    listed = {}
    for line in open(os.path.join(ROOT, GAME, "addresses.txt")):
        parts = line.split("#", 1)
        if parts[0].strip():
            listed[parts[0].strip().upper()] = parts[1].strip() if len(parts) > 1 else ""
    found = {}
    skip = ("third_party",)
    for d, _, files in os.walk(os.path.join(ROOT, "runtime")):
        if any(s in d for s in skip):
            continue
        for fn in files:
            if not fn.endswith((".c", ".cpp", ".h")):
                continue
            path = os.path.join(d, fn)
            text = open(path, encoding="utf-8", errors="replace").read()
            text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)  # comments out
            text = re.sub(r"//[^\n]*", "", text)
            for m in re.finditer(r"0x(8[01][0-9A-Fa-f]{6})u?\b", text):
                v = m.group(1).upper()
                if v.startswith("8000") and int(v, 16) < 0x80004000 or v in ("80000000", "81800000"):
                    continue  # low memory (the OS ABI) and the MEM1 bounds
                found.setdefault(v, set()).add(os.path.relpath(path, ROOT))
    missing = {v: f for v, f in found.items() if v not in listed}
    check("addresses", not missing, "%d guest addresses in runtime/, all listed%s" % (len(found), "" if not missing else
          "; NOT LISTED: " + ", ".join("%s (%s)" % (v, ", ".join(sorted(f))) for v, f in sorted(missing.items()))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--llvm-mingw", required=True)
    ap.add_argument("--sdl", required=True)
    ap.add_argument("--imgui", required=True)
    ap.add_argument("--skip-build", action="store_true", help="skip the compile check (only for a quick look)")
    a = ap.parse_args()
    head = tree()
    codegen()
    if not a.skip_build:
        compile_check(a)
    hardware()
    addresses()
    ok = all(r[1] for r in results) and not a.skip_build
    print("RESULT: %s (commit %s)" % ("PASS" if ok else "FAIL", head))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

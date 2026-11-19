#!/usr/bin/env python3
"""Gate 3: read a test log by script before anything is concluded from it.

  python3 tools/read_log.py LOG [--commit HEX]

--commit defaults to the commit Claude last pushed (HEAD). A log from any other build stops here with
WRONG BUILD. Otherwise prints the facts, each with the log line it comes from:
  build      the [build] line (commit, dirty or not)
  boot       disc, PsoV3.dol load, the [boot] line
  end        how the run ended: fatal error, host crash (with exe+offsets to symbolize), or still running
  pace       the [pace] lines (frames per second, game work, GPU)
  devices    unmodelled register accesses ([mmio]), DSP/AX, disc and card errors
  last       the last 15 lines
"""
import argparse
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--commit", default="")
    a = ap.parse_args()
    want = a.commit or subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True).stdout.strip()
    lines = open(a.log, encoding="utf-8", errors="replace").read().splitlines()
    build = next((l for l in lines if "[build]" in l), None)
    if not build:
        print("WRONG BUILD: no [build] line in %s (an old or empty log?)" % a.log)
        sys.exit(2)
    if want[:7] not in build:
        print("WRONG BUILD: the log is from '%s', the latest push is %s" % (build.strip(), want))
        sys.exit(2)
    print("build      %s%s" % (build.strip(), "  (DIRTY: local changes)" if "dirty" in build else ""))

    def grep(pat, n=8):
        return [(i + 1, l) for i, l in enumerate(lines) if re.search(pat, l)][:n]

    for tag, pat in (("boot", r"\[(disc|load|boot)\]"), ("end", r"FATAL|fatal|\[crash\]|Assertion|panic"),
                     ("pace", r"\[pace\]"), ("devices", r"\[mmio\]|\[dsp\].*(unknown|unexpected)|\[di\].*error|\[exi\].*(error|unknown)"),
                     ("watchdog", r"\[watchdog\]|stall|hang")):
        hits = grep(pat, 12)
        if not hits:
            print("%-10s (no lines)" % tag)
        for k, (i, l) in enumerate(hits):
            print("%-10s %6d: %s" % (tag if k == 0 else "", i, l.strip()[:220]))
    crash = [l for l in lines if "exe+" in l]
    if crash:
        print("symbolize  llvm-symbolizer --obj=psoplus.exe with the image base + each exe+ offset:")
        for l in crash[:12]:
            print("           " + l.strip()[:200])
    print("last       (%d lines in all)" % len(lines))
    for l in lines[-15:]:
        print("           " + l.strip()[:220])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""al1_sites_selftest.py - S108 lane AL1: the wired sites' unit (tools\\weapons\\al1_sites_unit.cpp), clean and planted.

The clean run must pass all seven site checks and every `[rdr][PROBLEM]` line it prints must be a triage `problem` finding named
`draw_shortfall`; the planted twin `--plant silent` (every report call skipped: the old code) must fail all seven.
Call: py -I tools\\weapons\\al1_sites_selftest.py   (after pwsh -NoProfile -File tools\\weapons\\build_al1_sites.ps1)
"""
# entry: main
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "report"))
import triage  # noqa: E402

PROBLEM_RX = next(c["rx"] for c in triage.CLASSES if c["name"] == "problem")
EXE = ROOT / "out" / "weapons" / "al1_sites_unit.exe"
SITES = ["site.cap", "site.model_part", "site.pose_rows_ring", "site.pose_rows_weapon", "site.row_bound", "site.texture_pac", "site.texture_upload"]
FAILS = 0


def say(ok, what):
    global FAILS
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        FAILS += 1


def run(*args):
    p = subprocess.run([str(EXE)] + list(args), capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout, {m.group(2): m.group(1) for m in re.finditer(r"^(PASS|FAIL)  (site\.\w+)", p.stdout, re.M)}


def main():
    if not EXE.exists():
        print("missing %s: build it first (tools\\weapons\\build_al1_sites.ps1)" % EXE)
        return 2
    rc, out, c = run()
    say(rc == 0 and sorted(c) == SITES and all(v == "PASS" for v in c.values()), "clean: the seven site checks pass (%s)" % ", ".join("%s %s" % kv for kv in sorted(c.items())))
    lines = [ln[5:] for ln in out.splitlines() if ln.startswith("LINE ") and "[rdr][PROBLEM]" in ln]
    bad = [ln for ln in lines if not (PROBLEM_RX.search(ln) and PROBLEM_RX.search(ln).group("name") == "draw_shortfall")]
    say(lines and not bad, "clean: %d [rdr][PROBLEM] line(s), each a triage `problem` finding named draw_shortfall%s" % (len(lines), (": not: " + bad[0]) if bad else ""))
    rc, out, c = run("--plant", "silent")
    failed = sorted(k for k, v in c.items() if v == "FAIL")
    say(rc == 1 and failed == SITES, "--plant silent (the old code: no report) fails all seven site checks (%s)" % failed)
    print("al1_sites_selftest: %s" % ("FAIL" if FAILS else "PASS"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

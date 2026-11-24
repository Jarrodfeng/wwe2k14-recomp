#!/usr/bin/env python3
"""draw_shortfall_selftest.py - S108 lane AL1: the shared report's unit (tools\\report\\draw_shortfall_unit.cpp), clean and planted, player and dev builds.

The clean runs must pass every check; every `[rdr][PROBLEM]` line they print must be a triage `problem` finding named `draw_shortfall`
(tools\\report\\triage.py's own regex). The planted twin (`--plant problem`: the old FAN way, rdr::Problem per model) must fail exactly
sf.one_line and sf.each_model_named in the player build and abort the dev build (no `PASS  sf.no_abort`).
Call: py -I tools\\report\\draw_shortfall_selftest.py   (after pwsh -NoProfile -File tools\\report\\build_draw_shortfall_unit.ps1)
"""
# entry: main
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import triage  # noqa: E402

PROBLEM_RX = next(c["rx"] for c in triage.CLASSES if c["name"] == "problem")
PLAYER = ROOT / "out" / "report" / "draw_shortfall_unit.exe"
DEV = ROOT / "out" / "report" / "draw_shortfall_unit_dev.exe"
FAILS = 0


def say(ok, what):
    global FAILS
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        FAILS += 1


def run(exe, *args):
    p = subprocess.run([str(exe)] + list(args), capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def checks(out):
    return {m.group(2): m.group(1) for m in re.finditer(r"^(PASS|FAIL)  (sf\.\w+)", out, re.M)}


def main():
    for exe in (PLAYER, DEV):
        if not exe.exists():
            print("missing %s: build it first (tools\\report\\build_draw_shortfall_unit.ps1)" % exe)
            return 2
    for exe, tag in ((PLAYER, "player"), (DEV, "dev")):
        rc, out = run(exe)
        c = checks(out)
        say(rc == 0 and len(c) == 8 and all(v == "PASS" for v in c.values()), "%s clean: every check passes (%s)" % (tag, ", ".join("%s %s" % kv for kv in sorted(c.items()) if kv[1] != "PASS") or "8 of 8"))
        lines = [ln[5:] for ln in out.splitlines() if ln.startswith("LINE ") and "[rdr][PROBLEM]" in ln]
        bad = [ln for ln in lines if not (PROBLEM_RX.search(ln) and PROBLEM_RX.search(ln).group("name") == "draw_shortfall")]
        say(lines and not bad, "%s clean: %d [rdr][PROBLEM] line(s), each a triage `problem` finding named draw_shortfall%s" % (tag, len(lines), (": not: " + bad[0]) if bad else ""))
    rc, out = run(PLAYER, "--plant", "problem")
    c = checks(out)
    failed = sorted(k for k, v in c.items() if v == "FAIL")
    say(rc == 1 and failed == ["sf.each_model_named", "sf.one_line"], "player --plant problem fails exactly sf.each_model_named, sf.one_line (%s)" % failed)
    rc, out = run(DEV, "--plant", "problem")
    say(rc != 0 and "PASS  sf.no_abort" not in out, "dev --plant problem: the process stops at the first report (exit %d), sf.no_abort never passes" % rc)
    # review fix: a report after the store's static destructor ran (exit with a drawing thread still alive). On the store as a plain function-local static this run crashed or spun forever.
    for exe, tag in ((PLAYER, "player"), (DEV, "dev")):
        try:
            p = subprocess.run([str(exe), "--late-exit"], capture_output=True, text=True, timeout=30)
            rc, out = p.returncode, p.stdout
        except subprocess.TimeoutExpired:
            rc, out = None, "(no exit in 30 s)"
        say(rc == 0 and "PASS  sf.late_exit" in out, "%s --late-exit: a report from an exit handler after the static destructors names each new key, no crash or hang (exit %s)" % (tag, rc))
    print("draw_shortfall_selftest: %s" % ("FAIL" if FAILS else "PASS"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

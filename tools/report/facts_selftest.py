#!/usr/bin/env python3
"""facts_selftest.py - the proof of the problem report's IN PLAY / PROBLEMS / SHORTFALLS / ERRORS / REPRODUCE blocks (S108 lane RPT). 0 game runs.

Every case is a synthetic log WRITTEN HERE (never a copy of a real log), read by BOTH twins: the launcher's C++ (tools\\report\\findings_test.cpp
over src\\launcher\\report_findings.h + report_facts.h, which players run) and tools\\report\\triage.py (report_facts.py); their texts must be
identical, and the case's own checks run on the text. Then the table lint (src\\launcher\\report_facts.inc: every row's writer holds its marker,
every switch / mod row names a cvar src defines) and the setup line's writer -> reader round trip (src\\session_log.h's format strings).

  py -I tools\\report\\facts_selftest.py [--exe PATH] [--no-build] [--no-big]
  TRIAGE_DIR=DIR runs the Python side from another copy (the fail-first check against the pre-S108 triage).
Exit 0 = every check passed.
"""
# entry: main
import argparse
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, os.environ.get("TRIAGE_DIR") or str(HERE))
import triage  # noqa: E402

sys.path.insert(1, str(HERE))
import report_facts  # noqa: E402  (always this lane's copy: the table lint and the round trip)

SCRATCH = ROOT / "out" / "RPT"
FAILS = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        FAILS.append(what)


def ts(t):
    return "2026-01-01 %02d:%02d:%06.3f" % (10 + int(t // 3600), int(t // 60) % 60, t % 60)


def L(t, msg, level="info"):
    return "[%s] [%s] [core] [t1] %s" % (ts(t), level, msg)


def build(exe):
    exe.parent.mkdir(parents=True, exist_ok=True)
    cmd = '. "%s"  | Out-Null; clang++ -std=c++20 -O2 -Wall -o "%s" "%s"' % (ROOT / "tools" / "env.ps1", exe, HERE / "findings_test.cpp")
    r = subprocess.run(["pwsh", "-NoProfile", "-Command", cmd], capture_output=True, text=True, cwd=str(ROOT))
    if r.returncode != 0 or not exe.exists():
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit("findings_test.exe did not build")


def cpp(exe, files, source, out, peak=False):
    env = dict(os.environ)
    if peak:
        env["FINDINGS_PEAK"] = "1"
    r = subprocess.run([str(exe), str(out), source] + [str(f) for f in files], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit("findings_test exit %d: %s" % (r.returncode, r.stderr))
    return Path(out).read_bytes().decode("utf8"), r.stderr


def py_text(folder):
    buf, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = triage.main([str(folder)])
    return rc, buf.getvalue().replace("\r\n", "\n")


def block(text):
    """The new block's lines (IN PLAY .. the end of REPRODUCE) of every LOG section."""
    out, on, rep = [], False, False
    for ln in text.splitlines():
        if ln.startswith("  IN PLAY"):
            on, rep = True, False
        elif on and ln.startswith("  REPRODUCE"):
            rep = True
        elif on and rep and not ln.startswith("    "):
            on = False
        if on:
            out.append(ln)
    return out


def both(exe, name, files_text, phone=None):
    """Write the files, read them with both twins; identical text required. -> the C++ text (phone = a list that gets the C++ text made with the
    phone launcher's source label, mobile_main.cpp: the same Build, and the phone's zip has no Redact pass)."""
    SCRATCH.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=str(SCRATCH)) as tmp:
        d = Path(tmp) / "logs"
        d.mkdir()
        for fn, text in files_text.items():
            (d / fn).write_text(text, encoding="utf8", newline="\n")
        got, _err = cpp(exe, sorted(d.iterdir()), str(d), Path(tmp) / "out.txt")
        if phone is not None:
            phone.append(cpp(exe, sorted(d.iterdir()), "this phone's game log", Path(tmp) / "phone.txt")[0])
        rc, want = py_text(d)
        same = rc in (0, 1) and want.rstrip("\n") == got.rstrip("\n")      # S111: exit 1 = a RUN failed (no end marker ...), not a read fault
        if not same:
            import difflib
            for ln in list(difflib.unified_diff(want.splitlines(), got.splitlines(), "triage.py", "report_findings.h", lineterm="", n=0))[:30]:
                print("      " + ln)
        check(same, "%s: the C++ and triage.py texts are identical" % name)
        return got


HEAD = [L(0, "[wwe2k14] version vRPT"), L(0, "[wwe2k14] run log number: 777 (pid 1)"), L(0, "[wwe2k14] exe built (file time, local): 2026-11-20 18:04"),
        L(0, "[wwe2k14] built with WWE2K14_KEEP_STREAM=OFF: the scene host is the renderer (present host: ...)"),
        L(0, "[rdr][live] owner mode: the old path draws nothing (gpu_no_draw on)")]


def seq(t, a, b, name, frame):
    return L(t, "[wwe2k14][scene] sequence %d -> %d (%s) at frame %d, 1.0 s after start (the part before: 1 frames in 1.0 s, 1.0 fps)" % (a, b, name, frame))


def setup_lines(t, mode, typ, pace, arena, rules, seats, frame):
    """The writer's format (src\\session_log.h LogMatchSetup): 12 seats on the first line, then setup+ lines of 24."""
    lst = lambda a, b: ", ".join(seats[a:b]) or "(none)"
    out = [L(t, "[wwe2k14][match] setup: mode %d type %d pace %d arena %d; rules %s; seats %s frame=%d" % (mode, typ, pace, arena, rules, lst(0, 12), frame))]
    for k in range(12, len(seats), 24):
        out.append(L(t, "[wwe2k14][match] setup+: seats %s frame=%d" % (lst(k, k + 24), frame)))
    return out


RULES = "".join("%02X" % ((i * 7) % 256) for i in range(64))


def case_line_5000(exe):
    lines = HEAD + [seq(1, 0, 75, "main menu / match setup: CSmartMenuSequence", 10)]
    i = 0
    while len(lines) < 4999:
        i += 1
        lines.append(L(2 + i * 0.01, "[wwe2k14][perf] filler line %d nothing to see" % i))
    lines += [seq(60, 75, 9, "in-game: CGameMainSequence", 5000),
              L(60.5, "[rdr][PROBLEM] late_line: planted at line 5000 of 6000"),
              L(60.5, "[wwe2k14] modified: wwe2k14_step_hz = '60' (source: command line)"),
              L(60.5, "[wwe2k14][weapon_rows] event prop id 2591 -> PACH 400000 (entry 4)")]
    lines += setup_lines(60.6, 0, 3, 1, 9, RULES, ["0:343/p0/a1", "1:324/cpu/a0/s2"], 5001)
    while len(lines) < 6000:
        i += 1
        lines.append(L(61 + i * 0.001, "[wwe2k14][perf] filler line %d nothing to see" % i))
    got = both(exe, "line 5000 of 6000", {"wwe2k14_100.log": "\n".join(lines) + "\n"})
    b = "\n".join(block(got))
    check("[rdr][PROBLEM] late_line: planted at line 5000 of 6000" in b, "line 5000 of 6000: the PROBLEM line far down the log is listed")
    check("wwe2k14_step_hz = 60" in b and "event prop id 2591 -> PACH 400000 (entry 4)" in b, "line 5000 of 6000: the flag and the event prop far down are listed")
    check("--wwe2k14_launch_participants=343:0:1,324::0/2" in b, "line 5000 of 6000: the setup line far down gives the launch line")


def case_draw_shortfall(exe):
    long_tail = " detail" * 120
    pl = "[rdr][PROBLEM] draw_shortfall: prop 2591 model 0x40000004 texture tex_ab12 (entry 4): no rows, not drawn;" + long_tail
    got = both(exe, "draw_shortfall", {"wwe2k14_101.log": "\n".join(HEAD + [seq(1, 0, 9, "in-game: CGameMainSequence", 10), L(2, pl + " frame=88"),
                                                                            L(3, pl + " frame=99")]) + "\n"})
    rows = [ln for ln in block(got) if "draw_shortfall" in ln]
    check(len(rows) == 2 and rows[0].endswith(pl + " frame=88") and rows[1].endswith(pl + " frame=99"),
          "a draw_shortfall PROBLEM line is listed whole (%d chars), each distinct text once" % len(pl))
    check(rows and "frame 88 in 9 in-game: CGameMainSequence:" in rows[0], "the PROBLEM row carries its first frame and mode")


def case_shortfall(exe):
    ev = HEAD + [seq(1, 0, 9, "in-game: CGameMainSequence", 10)]
    for t, cap in ((2, 0), (3, 12), (4, 12), (5, 4374)):
        ev.append(L(t, "[scene][weapon_rows] frames 600, rows 1234, capped %d, overflow 0; [scene][props] not in index: %d" % (cap, 3 if t < 4 else 0)))
    got = both(exe, "shortfall counter", {"wwe2k14_102.log": "\n".join(ev) + "\n"})
    b = block(got)
    want = "    [scene][weapon_rows] capped: first non-zero 12 at 10:00:03, last 4374  (rows / nodes cut at a constant (the rest has no row))"
    check(want in b, "a non-zero shortfall counter prints its first non-zero value and time and its last value")
    check(not any("not_in_index" in ln for ln in b), "a counter that rose and fell back to 0 is not listed")
    check(not any("overflow" in ln for ln in b), "a counter that stayed 0 is not listed")


def case_old_log(exe):
    ev = [L(0, "[wwe2k14] version v0.3.9"), L(0, "[wwe2k14] run log number: 12 (pid 1)"), seq(1, 0, 75, "main menu / match setup: CSmartMenuSequence", 10),
          L(2, "[rdr][live] frames 600: drawn 590, skipped 10, replay 12 ms"), seq(3, 75, 9, "in-game: CGameMainSequence", 500),
          L(3, "[wwe2k14][match] card (in-game sequence started): mode 0 type 0 arena 9 extras [255, 150, 153]; participants 0:343 (slot 34312), 1:324 (slot 32402), 40:255 (slot 25502) frame=500"),
          L(4, "[wwe2k14][match] titles (in-game sequence started): 0:343 t255/255, 1:324 t0/255 frame=500"),
          L(5, "[rdr][live] frames 1200: drawn 1190, skipped 10"), L(6, "[gpu] command processor: ring buffer wrapped", "warning")]
    got = both(exe, "old stream-era log", {"wwe2k14_012.log": "\n".join(ev) + "\n"})
    b = "\n".join(block(got))
    check("renderer: no stream-free marker: a stream build" in b, "an old stream-era log: read without a crash, renderer says a stream build")
    check("from: the card line" in b and "--wwe2k14_launch_participants=343::1,324::0" in b and "40:" not in b.split("launch line:")[-1],
          "an old log (no setup line): REPRODUCE from the card (attire from the slot key, extras left out)")
    check("pads, pace and rules are not known" in b and "--wwe2k14_quick_rules" not in b, "an old log: what is not known is said, no rules switch printed")


USER = "Some Name"
PRIV = ["C:\\Users\\%s\\Documents\\WWE\\mods" % USER, "/storage/emulated/0/Android/data/com.wwe2k14.recomp/files/dlc",
        "\\\\nas\\share\\%s\\scenarios" % USER, "/data/user/0/com.wwe2k14.recomp/files/logs/x.txt", "D:/Users/%s/game/pac/ch/CH343.PAC" % USER,
        # S108 RPT review: a path that IS the profile folder (its last name is the person's name), and a POSIX path after a comma / a tab
        "C:\\Users\\%s" % USER, "/home/someone/clips/a"]


def case_privacy(exe):
    ev = [L(0, "[wwe2k14] version v1 (built in C:\\Users\\%s\\src)" % USER), L(0, "[wwe2k14] run log number: 5 (pid 1)"),
          L(0, "[wwe2k14] modified: wwe2k14_mods_dir = '%s' (source: command line)" % PRIV[0]),
          L(0, "[wwe2k14] modified: wwe2k14_dlc_dir = '%s' (source: command line)" % PRIV[1]),
          L(0, "[wwe2k14] modified: wwe2k14_scenario_dir = '%s' (source: command line)" % PRIV[2]),
          L(0, "[wwe2k14] modified: wwe2k14_mods = 'true' (source: command line)"),
          L(0, "[wwe2k14][dlc] pack 1 'DLC_1' (Pack One): ready, 120 file(s) listed, 512.0 MB -> %s" % PRIV[1]),
          seq(1, 0, 9, "in-game: C:\\Users\\%s\\x" % USER, 10),
          L(2, "[wwe2k14][file] NtCreateFile '%s' open access=0x1 -> 0x00000000 handle=0x1 frame=3" % PRIV[4]),
          L(2, "[wwe2k14][file] NtCreateFile 'C:\\Users\\%s\\game\\pac\\bg\\BG09.PAC' open access=0x1 -> 0x00000000 handle=0x1 frame=3" % USER),
          L(3, "[rdr][PROBLEM] pipeline_store: cannot write C:\\Users\\%s\\AppData\\Local\\wwe\\store.bin (disk full)" % USER),
          L(3, "[wwe2k14][weapon_rows] ROWS REFUSED: frame 9: object 0x1 (%s) node count 99" % PRIV[3]),
          L(4, "cannot open %s for the log" % PRIV[3], "error"), L(4, "[vk] failed at '%s'" % PRIV[0], "critical"),
          L(5, "[wwe2k14][match] card (in-game sequence started): mode 0 type 0 arena 9 extras [255, 150, 153]; participants 0:343 (slot 34312) frame=10"),
          L(5, "[wwe2k14][match] titles (C:\\Users\\%s\\t): 0:343 t255/255 frame=10" % USER),
          L(6, "[scene][weapon_rows] capped 7 (%s)" % PRIV[3]),
          L(6, "[wwe2k14] modified: wwe2k14_case_dir = '%s' (source: command line)" % PRIV[5]),
          L(6, "[wwe2k14] modified: wwe2k14_clip_dir = 'x,%s' (source: command line)" % PRIV[6]),
          L(6, "cannot open\t%s now" % PRIV[6], "error")]
    phone = []
    got = both(exe, "privacy (phone + PC paths)", {"wwe2k14_005.log": "\n".join(ev) + "\n"}, phone)
    leak = lambda ln: (USER in ln or "Users\\" in ln or "Users/" in ln or "/storage/" in ln or "/data/user" in ln or "emulated" in ln or "\\\\nas" in ln
                       or "someone" in ln)
    check(all(leak("x " + p) for p in PRIV), "the leak test sees every planted path when it is NOT cut (its teeth)")
    for label, text in (("PC", got), ("phone", phone[0] if phone else "")):
        b = block(text)
        leaks = [ln for ln in b if leak(ln)]
        check(len(b) > 20 and not leaks, "%s Build: no user name and no full path in ANY row of the new block (flags, DLC, route, files, problems, errors, titles, counters): %s"
              % (label, leaks[:3]))
    b = block(got)
    want = ["wwe2k14_mods_dir = <path>\\mods", "wwe2k14_dlc_dir = <path>/dlc", "wwe2k14_scenario_dir = <path>\\scenarios", "CH343.PAC", "BG09.PAC"]
    check(all(any(w in ln for ln in b) for w in want), "paths keep their last name only (<path>\\NAME; files by name)")


def case_reproduce(exe):
    seats = ["0:343/p0/a1", "1:324/cpu/a0/s2", "2:12/cpu/a3"] + ["%d:%d/cpu/a0" % (i, 300 + i) for i in range(3, 14)]
    ev = HEAD + [seq(1, 0, 75, "main menu / match setup: CSmartMenuSequence", 10), seq(2, 75, 9, "in-game: CGameMainSequence", 400)]
    ev += setup_lines(2, 0, 37, 2, 9, RULES, seats, 400)
    got = both(exe, "REPRODUCE from setup + setup+ lines", {"wwe2k14_200.log": "\n".join(ev) + "\n"})
    b = "\n".join(block(got))
    toks = "343:0:1,324::0/2,12::3," + ",".join("%d::0" % (300 + i) for i in range(3, 14))
    rules = ",".join("%d=%d" % (12 + i, (i * 7) % 256) for i in range(64))
    want = "launch line: --wwe2k14_launch=@quick --wwe2k14_quick_type=37 --wwe2k14_quick_pace=2 --wwe2k14_launch_arena=9 --wwe2k14_launch_participants=%s --wwe2k14_quick_rules=%s" % (toks, rules)
    check(want in b, "the launch line carries type, pace, arena, every seat (14, over two setup lines) and the 64 rule bytes")
    check("seat 2: 12 CPU, attire 3: a created superstar" in b, "a created superstar's seat is named as custom, nothing from its data")
    table = report_facts.load_table()
    cvars = {t for k, _a, t, _w in table if k == "switch"}
    names = set(re.findall(r"--(\w+)=", b))
    check(names and names <= cvars, "every --switch the launch line prints is a switch row of the table (%s)" % sorted(names - cvars))
    ev2 = HEAD + [seq(1, 0, 9, "in-game: CGameMainSequence", 10)] + setup_lines(1, 0, 23, 0, 300, RULES, ["%d:%d/cpu/a0" % (i, 300 + i) for i in range(30)], 10)
    b2 = "\n".join(block(both(exe, "REPRODUCE Royal Rumble", {"wwe2k14_201.log": "\n".join(ev2) + "\n"})))
    check("launch line: none: a built Royal Rumble" in b2 and "seat 29:" in b2, "type 23: no launch line, said why; all 30 seats read from setup+ lines")
    ev3 = HEAD + [seq(1, 0, 9, "in-game: CGameMainSequence", 10)] + setup_lines(1, 3, 0, 0, 300, RULES, ["0:343/p0/a0", "1:324/cpu/a0"], 10)
    b3 = "\n".join(block(both(exe, "REPRODUCE not an exhibition", {"wwe2k14_202.log": "\n".join(ev3) + "\n"})))
    check("not an exhibition" in b3 and "--wwe2k14_launch_arena" not in b3, "mode != 0: said; arena 300 (the type's own) prints no arena switch")
    # S108 RPT review: a seat whose attire is not known (`a?`): `ID:` alone is PAD 0 (src\launch_participant.h: "397:" = pad 0), so a CPU seat must be
    # the bare id (1 part = CPU) and a pad seat `ID:P` (2 parts)
    ev5 = HEAD + [seq(1, 0, 9, "in-game: CGameMainSequence", 10)] + setup_lines(1, 0, 0, 0, 9, RULES, ["0:343/p0/a?", "1:324/cpu/a?", "2:330/cpu/a2"], 10)
    b5 = "\n".join(block(both(exe, "REPRODUCE unknown attire", {"wwe2k14_204.log": "\n".join(ev5) + "\n"})))
    check("--wwe2k14_launch_participants=343:0,324,330::2 " in b5, "a seat with no known attire: CPU = the bare id, a pad seat = ID:PAD (never `ID:`, which is pad 0)")
    b4 = "\n".join(block(both(exe, "no match", {"wwe2k14_203.log": "\n".join(HEAD + [seq(1, 0, 26, "Create a Finisher: CCafMenuSequence", 10)]) + "\n"})))
    check("no match was set up in this log" in b4 and "26 Create a Finisher" in b4, "no match: the route is the menu path, the scene named")


def case_big(exe):
    """A 200 MB-shaped log: read by the C++ (what players run) in bounded memory. Python is not run on it (a PC tool; it reads in memory)."""
    SCRATCH.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(dir=str(SCRATCH)))
    try:
        p = tmp / "wwe2k14_900.log"
        n = 0
        with open(p, "w", encoding="utf8", newline="\n") as f:
            f.write("\n".join(HEAD) + "\n")
            k = 0
            while f.tell() < 200 * 1000 * 1000:
                t = 10 + k * 0.001
                f.write(L(t, "[wwe2k14][perf] filler %d the quick brown fox jumps over the lazy dog, again and again" % k) + "\n")
                f.write(L(t, "[wwe2k14][file] NtCreateFile 'G:\\pac\\ch\\CH%05d.PAC' open access=0x1 -> 0x00000000 handle=0x1 frame=%d" % (k % 9000, k)) + "\n")
                f.write(L(t, "[rdr][PROBLEM] distinct_%d: a different problem text every time" % k) + "\n")
                f.write(L(t, "error group %s number %d" % ("x" * (k % 300), k), "error") + "\n")
                f.write(L(t, "[wwe2k14] modified: wwe2k14_flag_%d = '1' (source: command line)" % k) + "\n")
                f.write(L(t, "[scene][c%d] capped %d" % (k % 1000, k % 7)) + "\n")
                k += 1
                n += 6
            f.write(L(99999, "[rdr][PROBLEM] the_end: planted at the end of a 200 MB log") + "\n")
        size = p.stat().st_size
        out, err = cpp(exe, [p], str(tmp), tmp / "out.txt", peak=True)
        m = re.search(r"peak_working_set_bytes (\d+)", err)
        peak = int(m.group(1)) if m else -1
        check(size >= 200 * 1000 * 1000 and 0 < peak < 64 * 1024 * 1024,
              "a %d MB log (%d lines) is read by the C++ with a peak working set of %.1f MB (bound 64 MB)" % (size // 1000000, n, peak / 1048576.0))
        b = "\n".join(block(out))
        check("(+" in b and "more line(s)" in b and b.count("\n      wwe2k14_flag_") == 300, "the capped lists say how many more lines were not listed (flags capped at 300)")
        check("the_end: planted at the end" not in b and "more line(s) of other problems" in b, "problems over the cap are counted, not listed")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    check(not tmp.exists(), "the 200 MB temp log is deleted")


def lint_table(table, roots):
    """Every row's writer holds its marker; every mod / switch row's cvar is in its writer file (REXCVAR_DEFINE_*(NAME, or, since S110 CVARSHARD, REXCVAR_DECLARE(type, NAME))
    and is defined there or in a generated unit src\\cvars\\*.cpp (tools\\compile\\cvar_shard.py moved the header definitions there)."""
    bad = []
    for kind, arg, text, writer in table:
        if writer == "-":
            continue
        path, _sep, lit = writer.partition(" @ ")
        if kind in ("mod", "switch"):
            name = arg if kind == "mod" else text
            rx = re.compile(r"REXCVAR_(?:DEFINE_\w+\(\s*%s\s*,|DECLARE\(\s*[^,()]+,\s*%s\s*\))" % (re.escape(name), re.escape(name)))
            defrx = re.compile(r"REXCVAR_DEFINE_\w+\(\s*%s\s*," % re.escape(name))
        else:
            rx = defrx = None
            lit = lit or arg
        for root in roots:
            fp = Path(root) / path.strip()
            if fp.exists():
                body = fp.read_text(encoding="utf8", errors="replace")
                if (rx and not rx.search(body)) or (not rx and lit not in body):
                    bad.append("%s %s: %s does not hold %s" % (kind, arg, path, rx.pattern if rx else repr(lit)))
                elif rx and not defrx.search(body) and not any(defrx.search(p.read_text(encoding="utf8", errors="replace"))
                                                               for p in sorted((Path(root) / "src" / "cvars").glob("*.cpp"))):
                    bad.append("%s %s: %s declares %s, but no src\\cvars unit defines it" % (kind, arg, path, name))
                break
        else:
            bad.append("%s %s: %s not found under any root" % (kind, arg, path))
    return bad


def case_table():
    roots = [ROOT, os.environ.get("RDR_MAIN_ROOT") or r"C:\Users\Jarrod\wwe2k14-recomp"]
    table = report_facts.load_table()
    probs = lint_table(table, roots)
    check(not probs, "every report_facts.inc row's writer holds its marker / defines its cvar %s" % probs)
    renamed = [(k, a, (t + "_renamed" if k == "switch" and a == "quick_pace" else t), w) for k, a, t, w in table]
    check(len(lint_table(renamed, roots)) == 1, "a renamed switch fails the lint (planted: wwe2k14_quick_pace_renamed)")
    moved = [(k, a, t, (w.replace("src/wwe2k14_app.h", "src/session_log.h") if a == "[wwe2k14] version" else w)) for k, a, t, w in table]
    check(len(lint_table(moved, roots)) == 1, "a marker whose writer file no longer holds it fails the lint (planted)")
    row = [r for r in table if r[0] == "switch" and r[1] == "quick_pace"]
    with tempfile.TemporaryDirectory() as td:   # S110 CVARSHARD: the writer alone (its REXCVAR_DECLARE), no src\cvars unit beside it
        w = row[0][3].partition(" @ ")[0].strip() if row else "src/scenario.h"
        (Path(td) / w).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / w, Path(td) / w)
        check(len(row) == 1 and len(lint_table(row, [td])) == 1, "a switch whose writer only declares it and no src\\cvars unit defines it fails the lint (planted)")


def case_setup_writer():
    """The writer's format strings (src\\session_log.h) -> filled like fmt -> parsed by the reader's regexes (report_facts.py; the C++ uses the same)."""
    src = (ROOT / "src" / "session_log.h").read_text(encoding="utf8")
    f1 = re.search(r'"(\[wwe2k14\]\[match\] setup: [^"]*)"', src)
    f2 = re.search(r'"(\[wwe2k14\]\[match\] setup\+: [^"]*)"', src)
    seat = re.search(r'fmt::format\("(\{\}:\{\}/\{\}/a\{\})"', src)
    ok = bool(f1 and f2 and seat and "/s{}" in src)
    if ok:
        fill = lambda f, vals: f.replace("{}", "%s") % vals
        s1 = fill(seat.group(1), (0, 343, "p0", 1)) + "/s2"
        line1 = fill(f1.group(1), (0, 3, 1, 9, RULES, s1 + ", " + fill(seat.group(1), (1, 324, "cpu", 0)), 400))
        line2 = fill(f2.group(1), (fill(seat.group(1), (12, 301, "cpu", "?")), 400))
        m1 = report_facts.RX_SETUP.search(line1)
        m2 = report_facts.RX_SETUP_MORE.search(line2)
        seats = [s.groups() for s in report_facts.RX_SEAT.finditer(m1.group(6) + ", " + m2.group(1))] if m1 and m2 else []
        ok = bool(m1 and m2 and m1.group(5) == RULES and seats == [("0", "343", "p0", "1", "2"), ("1", "324", "cpu", "0", None), ("12", "301", "cpu", "?", None)])
        # the widest lines the writer makes: 12 / 24 seats of the widest text (seat 39, id 499, pad 3, attire 9, set -1), 128 rule hex digits
        wide = lambda k: ", ".join(fill(seat.group(1), (39, 499, "p3", 9)) + "/s-1" for _ in range(k))
        long1 = L(0, fill(f1.group(1), (-1, 118, 2, 299, RULES, wide(12), 4294967295)))
        long2 = L(0, fill(f2.group(1), (wide(24), 4294967295)))
        check(len(long1) < 600 and len(long2) < 600, "the setup line (12 seats) and a setup+ line (24 seats) stay under 600 characters with the stamp (%d, %d)"
              % (len(long1), len(long2)))
    check(ok, "the setup line's format in src\\session_log.h round-trips through the report's parser (rules, pads, attire, set, setup+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(SCRATCH / "facts_test.exe"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-big", action="store_true")
    a = ap.parse_args()
    exe = Path(a.exe)
    if not a.no_build:
        build(exe)
    case_table()
    case_setup_writer()
    case_line_5000(exe)
    case_draw_shortfall(exe)
    case_shortfall(exe)
    case_old_log(exe)
    case_privacy(exe)
    case_reproduce(exe)
    if not a.no_big:
        case_big(exe)
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

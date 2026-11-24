#!/usr/bin/env python3
"""findings_compare.py - the proof of the C++ port (src\\launcher\\report_findings.h) against triage.py. 0 game runs.

Builds tools\\report\\findings_test.cpp (clang++ from the project's build shell), writes the synthetic logs triage_selftest.py builds (our own text) into
a temp folder per case, and for each case compares the text triage.py prints for that folder with the text the C++ prints for the same files. Any
difference FAILS (same findings, same modes, same counts, same notes, the whole text).

Call:  py -I tools\\report\\findings_compare.py [--real PATH ...] [--exe PATH] [--no-build]
  --real PATH : a game log (.log) or a folder of logs or a report .zip (its logs are unpacked to a temp folder under out\\ and deleted) read the same way.
Exit 0 = every case identical.
"""
# entry: main
import argparse
import contextlib
import difflib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import triage  # noqa: E402
import triage_selftest as T  # noqa: E402


def build(exe):
    exe.parent.mkdir(parents=True, exist_ok=True)
    cmd = '. "%s"  | Out-Null; clang++ -std=c++20 -O2 -Wall -o "%s" "%s"' % (ROOT / "tools" / "env.ps1", exe, HERE / "findings_test.cpp")
    r = subprocess.run(["pwsh", "-NoProfile", "-Command", cmd], capture_output=True, text=True, cwd=str(ROOT))
    if r.returncode != 0 or not exe.exists():
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit("findings_test.exe did not build")


def triage_text(folder_or_file):
    buf, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = triage.main([str(folder_or_file)])
    return rc, buf.getvalue(), err.getvalue()


def cpp_text(exe, files, source, out):
    r = subprocess.run([str(exe), str(out), source] + [str(f) for f in files], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("findings_test exit %d: %s" % (r.returncode, r.stderr))
    return Path(out).read_bytes().decode("utf8")


FAILS = []


def compare(name, exe, tmp, files, source, tri_arg):
    rc, want, err = triage_text(tri_arg)
    if rc != 0:
        FAILS.append(name)
        print("FAIL  %s: triage.py exit %d: %s" % (name, rc, err.strip()))
        return
    got = cpp_text(exe, files, source, Path(tmp) / "out.txt")
    want = want.replace("\r\n", "\n")
    if want.rstrip("\n") == got.rstrip("\n"):
        print("PASS  %s (%d lines identical)" % (name, want.count("\n") + 1))
    else:
        FAILS.append(name)
        print("FAIL  %s: the C++ text differs from triage.py's:" % name)
        for ln in list(difflib.unified_diff(want.splitlines(), got.splitlines(), "triage.py", "report_findings.h", lineterm="", n=1))[:60]:
            print("      " + ln)


def case(name, exe, files_text):
    """files_text = {file name: text}. Written to a fresh folder; compared as a folder."""
    with tempfile.TemporaryDirectory(dir=str(ROOT / "out" / "C0b")) as tmp:
        d = Path(tmp) / "logs"
        d.mkdir()
        for fn, text in files_text.items():
            (d / fn).write_text(text, encoding="utf8", newline="\n")
        files = sorted(d.iterdir())
        compare(name, exe, tmp, files, str(d), d)


def custom_cases():
    """The review rows of triage_selftest.review_rows, as logs (same text)."""
    cs = {}
    exit_line = ("[scene][2d] exit: frames 100, 2d-whole 50, fallback 50 (by reason: scene not_ready:50; render -); rows 10, pair (0 1); sum ok; none uncounted; "
                 "orphan notes 0, pending dropped 0, window 100, pending unadopted 0; in scene create_a_finisher_cafmenu; run totals: frames 500, 2d-whole 400, fallback 100 (by reason: scene not_ready:100; render -)")
    cs["R1 2d exit line"] = {"wwe2k14_001.log": T.custom([(22, exit_line)])}
    cs["R2 2d in-scene attribution"] = {"wwe2k14_001.log": T.custom([(25, exit_line.replace("exit: ", ""))])}
    cs["R3 crossing"] = {"wwe2k14_001.log": T.custom([(15, T.hl(1000, 0)), (25, T.hl(1100, 200))])}
    head = [T.L(0, "[wwe2k14] version vTWIN")]
    part2 = "\n".join(head + [T.L(1, "[wwe2k14][scene] sequence " + T.S3[1]), T.L(10, "[wwe2k14][scene] sequence " + T.S75[1]), T.L(12, T.hl(1000, 10)), T.L(14, T.hl(1100, 10))]) + "\n"
    part1 = "\n".join([T.L(20, "[wwe2k14][scene] sequence " + T.S26[1]), T.L(21, T.hl(1200, 10)), T.L(22, T.hl(1300, 30))]) + "\n"
    part0 = "\n".join([T.L(24, T.hl(1400, 50))]) + "\n"
    cs["R4 rotated parts"] = {"wwe2k14_007.log": part0, "wwe2k14_007.1.log": part1, "wwe2k14_007.2.log": part2}
    cs["R4c parts missing"] = {"wwe2k14_007.log": part0, "wwe2k14_007.3.log": part2}
    cs["R5 condensed"] = {"wwe2k14_001.log": T.custom([(12, T.hl(1000, 0))]) +
                          "[report] lines left out after their first 30 copies (count left out, message):\n  23  [info] [core] [t1] [scene][front_end] technique fallback: yTrail technique 1 (1 fallbacks, 2 table picks)\n"}
    cs["R5b cut"] = {"wwe2k14_001.log": T.custom([(12, T.hl(1000, 0))]) + "[report] ... 123456 bytes left out ...\n" + T.L(30, T.hl(1100, 0)) + "\n"}
    cs["R6 class line before stamp"] = {"wwe2k14_001.log": "[scene][front_end] technique fallback: yU technique 1 (1 fallbacks, 2 table picks)\n" + T.custom([])}
    cs["R6b unstamped continuation"] = {"wwe2k14_001.log": T.custom([(21, "RAW:[scene][front_end] technique fallback: yCont technique 2 (1 fallbacks, 3 table picks)")])}
    cs["R7 sequence format changed"] = {"wwe2k14_001.log": T.custom([(12, T.hl(1000, 0))], seqs=((1, "0 -> 3 (startup / title: Startup) at frame 1"), (10, "3 to 75 (main menu) at frame 500")))}
    cs["R8 no sequence"] = {"wwe2k14_001.log": T.custom([(12, T.hl(1000, 40))], seqs=())}
    cs["R10 counter falls"] = {"wwe2k14_001.log": T.custom([(12, T.hl(1000, 0)), (14, T.hl(1100, 30)), (16, T.hl(1200, 5))])}
    cs["R11 pair reason"] = {"wwe2k14_001.log": T.custom([(12, T.hl(900, 0, 100, "picture 100")), (21, T.hl(1000, 0, 100, "picture 100")),
                                                          (22, T.hl(1100, 0, 130, "picture 100, page_rows 30"))])}
    ev = [(12 + i, "[wwe2k14][file] NtOpenFile 'D:\\twin\\o%d.pac' (relative to handle 0x1A) open access=0x1 -> 0x00000000 handle=0x1 frame=%d" % (i, i)) for i in range(3)]
    ev.append((16, "[wwe2k14][file] NtCreateFile 'D:\\twin\\bad.pac' open access=0x1 -> 0xC0000034 handle=0x0 frame=9"))
    cs["R12 file opens (relative, NtOpenFile, a failure)"] = {"wwe2k14_001.log": T.custom(ev)}
    for n in (3990, 4001, 4100, 100 * 1024):   # S105 C0b review fix 2: lines around / over the 4000-char regex cap must not throw or change a finding
        pad = "z" * n
        cs["long lines %d" % n] = {"wwe2k14_001.log": T.custom([
            (21, "[scene][front_end] technique fallback: yLong technique 3 (1 fallbacks) " + pad),
            (22, "[host][strikes] 2 strikes: miss 1 | why: frame not built: long strike " + pad),
            (23, "[wwe2k14][scene] sequence 26 -> 75 (main menu / match setup: MainMenu) at frame 1000 " + pad),
            (24, T.hl(1000, 7) + " " + pad), (24.5, T.hl(1100, 9) + " " + pad)])}
    wmain = "[wwe2k14][load] WATCH: no loader progress for 12.0 s while 700 guest frames advanced: req 0x00000001 state 9 (twin)"
    wdet = ["[wwe2k14][load] WATCH request 0x00001234 words 0..15: 00000001 00000002", "[wwe2k14][load] WATCH read-end job 0x00009999 words 0..7: 00000001 00000002", "[wwe2k14][load] WATCH read-end object 0x00005678 words 32..43: 00000001 ????????",
            "[wwe2k14][load] WATCH stack tid 12 'twin thread': 0x82000000 0x82000004", "[wwe2k14][load] WATCH stacks: the guest thread list did not answer in 2 s"]
    cs["WATCH dump (S105 H detail lines)"] = {"wwe2k14_001.log": T.custom([(21, wmain)] + [(21, d) for d in wdet])}
    cs["WATCH mangled main line + detail"] = {"wwe2k14_001.log": T.custom([(21, wmain)] + [(21, d) for d in wdet[:2]] + [(22, "[wwe2k14][load] WATCH no loader progress twin")])}
    # S106 0FFN N2: the hang guard's lines: three are findings, the over / picture / guard-on lines are neither a finding nor UNPARSED
    cs["hang lines"] = {"wwe2k14_001.log": T.custom([(21, "[hang] HANG? no present for 20 s, guest frame 1234"), (22, "[hang] BLACK: no present for 25 s, 900 guest frames ran (presents 5, acquire timeouts 0)"),
                                                     (22, "[hang] HANG: guest side, no guest frame for 60 s, guest frame 1234"), (23, "[hang] HANG over: guest frames and presents go on, guest frame 1300"),
                                                     (23, "[hang] picture 30 s in: fine, 20 of 24 points bright, 5 ms"), (23, "[hang] BLACK over: presents came back")])}
    cs["WATCH detail only"] = {"wwe2k14_001.log": T.custom([(21, d) for d in wdet])}
    cs["CRLF log"] = {"wwe2k14_002.log": T.gen(plant=("holes", "black_no_3d")).replace("\n", "\r\n")}
    cs["two logs in one report"] = {"wwe2k14_001.log": T.gen(plant=("holes",)), "wwe2k14_002.log": T.gen(plant=("problem", "strikes"))}
    cs["problem + every plant"] = {"wwe2k14_003.log": T.gen(plant=tuple(list(T.PLANTS) + list(T.HOST_PLANTS)))}
    cs["mem alloc + heap failures + adopted + caw textures"] = {"wwe2k14_004.log": T.custom([
        (12, "MmAllocatePhysicalMemoryEx failed #1 size=0x00100000 something"), (13, "MmAllocatePhysicalMemoryEx failed #2 size=0x00200000 x"),
        (14, "BaseHeap::Alloc failed at 0x1000 size 5"), (15, "[scene][load] adopted #3: scene Foo, waited 1500 ms"), (16, "[scene][load] adopted #4: scene Bar, waited 900 ms"),
        (17, "[rdr][caw] guest draw: models built 3, holes 0, actors drawn from them 36 (last at frame 4006, skipped: palette mismatch 0, own-skeleton piece holes of ALL wrestlers 4), textures bound 86 from override 0 missing 2 engine_default_cube 2 (parts without a diffuse 3)"),
        (18, "[rdr][caw] guest draw: models built 3, holes 0, actors drawn from them 36 (last at frame 4006, skipped: palette mismatch 0, own-skeleton piece holes of ALL wrestlers 9), textures bound 86 from override 0 missing 5 engine_default_cube 2 (parts without a diffuse 3)")])}
    # S108 RPT: the facts block on a stream-free run with a match setup, a problem, a counter, an error and paths (both twins, the whole text)
    import facts_selftest as FS
    ev = FS.HEAD + [FS.seq(1, 0, 75, "main menu / match setup: CSmartMenuSequence", 10), FS.seq(2, 75, 9, "in-game: CGameMainSequence", 400),
                    FS.L(2, "[wwe2k14] modified: wwe2k14_mods_dir = 'C:\\Users\\Twin Name\\mods' (source: command line)"),
                    FS.L(3, "[rdr][PROBLEM] draw_shortfall: prop 2591 model 0x40000004: no rows frame=500"), FS.L(3, "[scene][weapon_rows] capped 9, overflow 0"),
                    FS.L(4, "vkCreateGraphicsPipelines failed 0x1234 for /data/user/0/com.twin/files/store.bin", "error"),
                    FS.L(5, "[wwe2k14][match] card (in-game sequence started): mode 0 type 3 arena 9 extras [255, 150, 153]; participants 0:343 (slot 34312), 1:12 (slot 1202) frame=400")]
    ev += FS.setup_lines(2, 0, 3, 1, 9, FS.RULES, ["0:343/p0/a1", "1:12/cpu/a0/s1"], 400)
    cs["S108 RPT facts block (setup, problem, counter, error, paths)"] = {"wwe2k14_001.log": "\n".join(ev) + "\n"}
    return cs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", nargs="*", default=[])
    ap.add_argument("--exe", default=str(ROOT / "out" / "C0b" / "findings_test.exe"))
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()
    exe = Path(a.exe)
    (ROOT / "out" / "C0b").mkdir(parents=True, exist_ok=True)
    if not a.no_build:
        build(exe)
    names = list(T.PLANTS) + list(T.HOST_PLANTS)
    for c in names:
        case("%s planted" % c, exe, {"wwe2k14_999.log": T.gen(plant=(c,))})
        case("%s format changed" % c, exe, {"wwe2k14_999.log": T.gen(mangle=(c,))})
    case("clean", exe, {"wwe2k14_999.log": T.gen()})
    for g in ("advance", "frozen", "short"):
        case("gap %s" % g, exe, {"wwe2k14_999.log": T.gen(gap=g)})
    case("gap in the middle", exe, {"wwe2k14_999.log": T.gen(mid_gap=True)})
    case("no file opens", exe, {"wwe2k14_999.log": "\n".join(l for l in T.gen().splitlines() if "[file]" not in l) + "\n"})
    for n, files in custom_cases().items():
        case(n, exe, files)
    # a log that is not a game log: the C++ must say so in words (triage.py exits 2)
    with tempfile.TemporaryDirectory(dir=str(ROOT / "out" / "C0b")) as tmp:
        p = Path(tmp) / "wwe2k14_005.log"
        p.write_text("hello\nworld\n", encoding="utf8")
        got = cpp_text(exe, [p], "x", Path(tmp) / "o.txt")
        ok = got.startswith("could not read the log:")
        print(("PASS  " if ok else "FAIL  ") + "not a game log: findings.txt says 'could not read the log': %r" % got[:90])
        if not ok:
            FAILS.append("not a game log")
        got = cpp_text(exe, [Path(tmp) / "missing" / "wwe2k14_006.log"], "x", Path(tmp) / "o.txt")
        ok = got.startswith("could not read the log:")
        print(("PASS  " if ok else "FAIL  ") + "an unreadable file: %r" % got[:90])
        if not ok:
            FAILS.append("unreadable file")
    for r in a.real:
        rp = Path(r)
        if rp.suffix.lower() == ".zip":
            with tempfile.TemporaryDirectory(dir=str(ROOT / "out" / "C0b")) as tmp:
                d = Path(tmp) / "unz"
                d.mkdir()
                with zipfile.ZipFile(rp) as z:
                    for n in z.namelist():
                        if triage.RX_PART.search(n):
                            (d / n.replace("\\", "/").split("/")[-1]).write_bytes(z.read(n))
                compare("real zip %s" % rp.name, exe, tmp, sorted(d.glob("wwe2k14_*.log")), str(d), d)
                shutil.rmtree(d, ignore_errors=True)
        elif rp.is_dir():
            compare("real folder %s" % rp, exe, str(ROOT / "out" / "C0b"), sorted(rp.glob("wwe2k14_*.log")), str(rp), rp)
        else:
            compare("real log %s" % rp, exe, str(ROOT / "out" / "C0b"), [rp], str(rp.parent), rp)
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

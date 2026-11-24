#!/usr/bin/env python3
"""triage_selftest.py - planted twins for tools\\report\\triage.py (S105 C0). 0 game runs; the logs are our own synthetic text.

For every class in triage.CLASSES (the pattern table): a log with the fault planted must give that class's finding in the right mode, the same log
with the fault line removed must give none (and no UNPARSED anywhere), and the same line with its format changed must give UNPARSED for that class,
never a quiet 0. Plus: a cumulative counter carried into a mode is not that mode's fault (delta), the file-open gap rule (frames advancing, runs to
the end of the log, over 10 s) in its three directions, and unreadable inputs exit 2.

Call: py -I tools\\report\\triage_selftest.py        (exit 0 = all twins behaved)
"""
# entry: main
import contextlib
import io
import json
import re
import sys
import tempfile
import zipfile
from pathlib import Path

import os  # noqa: E402
sys.path.insert(0, os.environ.get("TRIAGE_DIR") or str(Path(__file__).resolve().parent))   # TRIAGE_DIR: run these rows against another copy (fail-first check)
import triage  # noqa: E402


def ts(t):
    return "2026-01-01 10:%02d:%06.3f" % (int(t // 60), t % 60)


def L(t, msg):
    return "[%s] [info] [core] [t1] %s" % (ts(t), msg)


# class -> (planted line(s), mangled line(s) (class line present, format changed), expected reason substring)
PLANTS = {
    "problem": (["[rdr][PROBLEM] device_lost: vkQueueSubmit returned VkResult -4 (twin)"], ["[rdr][PROBLEM] device_lost vkQueueSubmit twin"], "device_lost"),
    "load_stall": (["[wwe2k14][load] STALL: req 0x00000001 (file X) state 9 for 12.0 s (twin)"], ["[wwe2k14][load] STALL req 0x1 twin"], "req 0x00000001"),
    "load_watch": (["[wwe2k14][load] WATCH: no loader progress for 12.0 s while 700 guest frames advanced (twin)"], ["[wwe2k14][load] WATCH no loader progress twin"], "no loader progress"),
    "holes": (["[scene][ready] calls 600 (warm 1, live 599), ready 599; not built 0; misses 1 (assets asked 2); holes 5 skipped [wrestlers 5 (first: twin hole reason (nested))]; no stage 0; shadow input 0 skipped"],
              ["[scene][ready] calls 600 holes five twin"], "twin hole reason"),
    "hole_skip": (["[record_frame] [scene][wrestlers] skip frame 10 obj skel:TWIN reason twin skip reason hole yes"],
                  ["[record_frame] [scene][wrestlers] skip frame 10 obj skel:TWIN reason twin skip reason"], "twin skip reason"),
    "caw_holes": (["[rdr][caw] guest draw: models built 3, holes 0, actors drawn from them 36 (last at frame 4006, skipped: palette mismatch 0, own-skeleton piece holes of ALL wrestlers 13), textures bound 86 from override 0 missing 1 engine_default_cube 2 (parts without a diffuse 3)"],
                  ["[rdr][caw] guest draw: models built 3, holes 0, actors drawn from them 36 (own-skeleton holes 13), textures bound 86 from override 0 missing 1 engine_default_cube 2 (parts without a diffuse 3)"], "guest draw"),
    "pic_refusals": (["[host] picture refusals 741: unclassified_creator 741; first refused key 0x00010000 (unclassified_creator): unknown, birth site 0x82BB07C4"],
                     ["[host] picture refusals many twin"], "0x82BB07C4"),
    "strikes": (["[host][strikes] 2 strikes: miss 1, no_last3d 1 | why: frame not built: twin strike 2"], ["[host][strikes] 2 strikes twin"], "twin strike"),
    "fallback_2d": (["[scene][2d] frames 100, 2d-whole 50, fallback 50 (by reason: scene not_ready:50; render -; hole -); rows 10, pair (0 1)"],
                    ["[scene][2d] frames 100, 2d-whole 50, fallback 50 twin"], "not_ready:50"),
    "technique_fallback": (["[scene][front_end] technique fallback: yTwin_Shader technique 7 (1 fallbacks, 20 table picks)"], ["[scene][front_end] technique fallback: twin"], "yTwin_Shader"),
    "reflect_fallback": (["[scene][front_end] reflect fallback: yTwin technique 7: the game's variant twin_variant is not bindable here (cube 0, amount map 0)"],
                         ["[scene][front_end] reflect fallback: twin"], "twin_variant"),
}
# host-frames classes: the planted twin raises a cumulative counter inside the mode
HOST_PLANTS = {"black_no_3d": ("black_no_3d {b} (mid_match 0)", "black_no_3d {b} mid"), "rows_not_drawn": ("not_drawn {n} (picture {n})", "not_drawn {n} picture")}

MODE_FAULT = 26   # the mode the faults are planted in (t 20..40)


def host_line(t, frames, b, n, form=None):
    bf = (form or {}).get("black_no_3d", HOST_PLANTS["black_no_3d"][0])
    nf = (form or {}).get("rows_not_drawn", HOST_PLANTS["rows_not_drawn"][0])
    return L(t, "[host] frames %d, ours %d, 2d_only %d, repeat_3d 0 (loading 0), repeat_full 0, no_record 0, no_picture 0, no_size 0, resize_black 0, %s, strikes 0, 2d_rows 100 drawn 100 %s holes 0 frames_2d_dropped 0 of 1 particles_not_drawn 0"
             % (frames, frames - 100, 100, bf.format(b=b, n=n), nf.format(b=b, n=n)))


def gen(plant=(), mangle=(), gap=None, mid_gap=False):
    """A synthetic log. gap = None | 'advance' | 'frozen' | 'short': the file-open pattern at the END of the log (last mode 75)."""
    out = [L(0, "[wwe2k14] version vTWIN"), L(0, "[wwe2k14] run log number: 999 (pid 1)"), L(0, "[wwe2k14] quality tier: TWIN tier"),
           L(0, "[wwe2k14][gpu] device 0: Twin GPU (integrated)")]
    seqs = [(1, "0 -> 3 (startup / title: Startup) at frame 1"), (10, "3 -> 75 (main menu / match setup: MainMenu) at frame 500"),
            (20, "75 -> 26 (Create a Finisher: CafMenu) at frame 900"), (40, "26 -> 75 (main menu / match setup: MainMenu) at frame 1500")]
    for t, s in seqs:
        out.append(L(t, "[wwe2k14][scene] sequence " + s + ", 1.0 s after start"))
    t_end = 70.0
    frames = 100
    t = 2.0
    while t <= t_end:
        frames_now = frames
        in_fault = 20 <= t < 40
        # file opens: every step, except the planted gaps
        opens = True
        if gap in ("advance", "frozen", "short") and t > (44 if gap != "short" else 65):
            opens = False
        if mid_gap and 24 < t < 38:
            opens = False
        if opens:
            out.append(L(t, "[wwe2k14][file] NtCreateFile 'D:\\twin\\f%d.pac' open access=0x1 -> 0x00000000 handle=0x1 frame=%d" % (int(t), frames_now)))
        if gap == "frozen" and t == 42:
            frozen_val = frames_now
        if gap == "frozen" and t >= 44:
            frames_now = frozen_val
        b = 0
        n = 0
        form = {}
        if in_fault and t >= 30:
            if "black_no_3d" in plant:
                b = 50
            if "rows_not_drawn" in plant:
                n = 20
        if t >= 40:
            b = 50 if "black_no_3d" in plant else 0   # leaving the mode keeps the counter: the delta must not repeat in mode 75
            n = 20 if "rows_not_drawn" in plant else 0
        if "black_no_3d" in mangle and in_fault:
            form["black_no_3d"] = HOST_PLANTS["black_no_3d"][1]
        if "rows_not_drawn" in mangle and in_fault:
            form["rows_not_drawn"] = HOST_PLANTS["rows_not_drawn"][1]
        out.append(host_line(t, frames_now, b, n, form))
        if gap != "frozen" or t < 44:
            frames += 100
        t += 2.0
    # per-class lines, inside the fault mode (t=25)
    for c in list(plant) + list(mangle):
        if c in PLANTS:
            for ln in (PLANTS[c][0] if c in plant else PLANTS[c][1]):
                out.append(L(25.5, ln))
    out.sort(key=lambda s: s[1:24])
    return "\n".join(out) + "\n"


def run(argv):
    buf, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = triage.main(argv)
    return rc, buf.getvalue(), err.getvalue()


def triage_text(tmp, name, text):
    p = Path(tmp) / name
    p.write_text(text, encoding="utf8")
    rc, out, err = run(["--json", str(p)])
    assert rc == 0, "exit %d: %s" % (rc, err)
    return json.loads(out)["logs"][0]


FAILS = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        FAILS.append(what)


def finds(j, cls):
    return [f for f in j["findings"] if f["cls"] == cls and not f["text"].startswith("UNPARSED")]


def unparsed(j, cls=None):
    return [f for f in j["findings"] if f["text"].startswith("UNPARSED") and (cls is None or f["cls"] == cls)]


# ---------------------------------------------------------------------------------------------------------------------------------
# Review round (docs\research\s105\review_C0.md): custom logs with explicit sequence and census lines
S3 = (1, "0 -> 3 (startup / title: Startup) at frame 1")
S75 = (10, "3 -> 75 (main menu / match setup: MainMenu) at frame 500")
S26 = (20, "75 -> 26 (Create a Finisher: CafMenu) at frame 900")
S75B = (23, "26 -> 75 (main menu / match setup: MainMenu) at frame 1000")
HOST = "[host] frames {f}, ours {o}, 2d_only 100, repeat_3d 0 (loading 0), repeat_full 0, no_record 0, no_picture 0, no_size 0, resize_black 0, black_no_3d {b} (mid_match 0), strikes 0, 2d_rows 100 drawn 100 not_drawn {n} ({w}) holes 0 frames_2d_dropped 0 of 1 particles_not_drawn 0"


def hl(f, b, n=0, w="-"):
    return HOST.format(f=f, o=f - 100, b=b, n=n, w=w)


def custom(evs, seqs=(S3, S75, S26, S75B), head=True):
    allv = [(t, "[wwe2k14][scene] sequence " + x + ", 1.0 s after start") for t, x in seqs] + list(evs)
    allv.sort(key=lambda e: e[0])
    lines = [L(0, "[wwe2k14] version vTWIN"), L(0, "[wwe2k14] run log number: 998 (pid 1)")] if head else []   # S106 0FFN N4: the head carries the run log number line
    lines += [(m[4:] if m.startswith("RAW:") else L(t, m)) for t, m in allv]
    return "\n".join(lines) + "\n"


def lint_writers(rows, extra, roots):
    """Each row's writer FILE must hold its literal on some line (S108 RPT: the file + text form; a line that only moves within its file is fine, a text
    that is gone or moved to another file fails). An old `file:line` writer is read as its file. Returns the problems (= a stale pattern table)."""
    bad = []
    for name, writer, lit in [(r["name"], r.get("writer"), r.get("lit")) for r in rows] + list(extra):
        if not writer:
            continue
        path = re.sub(r":\d+$", "", writer)
        for root in roots:
            fp = Path(root) / path
            if fp.exists():
                if lit not in fp.read_text(encoding="utf8", errors="replace"):
                    bad.append("%s: %s does not hold %r" % (name, writer, lit))
                break
        else:
            bad.append("%s: %s not found under any root" % (name, writer))
    return bad


def review_rows(tmp):
    def one(text, name="wwe2k14_r.log", extra=()):
        p = Path(tmp) / name
        p.write_text(text, encoding="utf8")
        rc, out, err = run(["--json"] + list(extra) + [str(p)])
        assert rc == 0, err
        return json.loads(out)["logs"][0]

    exit_line = ("[scene][2d] exit: frames 100, 2d-whole 50, fallback 50 (by reason: scene not_ready:50; render -); rows 10, pair (0 1); sum ok; none uncounted; "
                 "orphan notes 0, pending dropped 0, window 100, pending unadopted 0; in scene create_a_finisher_cafmenu; run totals: frames 500, 2d-whole 400, fallback 100 (by reason: scene not_ready:100; render -)")
    j = one(custom([(22, exit_line)]))
    f = finds(j, "fallback_2d")
    check(len(f) == 1 and f[0]["count"] == 50 and f[0]["mode_seq"] == 26, "R1 the 2D exit line is read, window part only (50, not the 100 run total): %s" % [(x["mode_seq"], x["count"]) for x in f])
    j = one(custom([(25, exit_line.replace("exit: ", ""))]))
    f = finds(j, "fallback_2d")
    check([x["mode_seq"] for x in f] == [26], "R2 a 2D window printed after the next mode began is the mode its `in scene` names: %s" % [x["mode_seq"] for x in f])
    j = one(custom([(15, hl(1000, 0)), (25, hl(1100, 200))]))
    f = finds(j, "black_no_3d")
    check(len(f) == 1 and f[0].get("crossing") and f[0]["count"] == 200 and f[0]["mode_seq"] is None and "26" in f[0]["mode"],
          "R3 a short visit between two census lines: the rise is 'mode not known' and names the mode crossed: %s" % [(x["mode"], x["count"]) for x in f])
    head = [L(0, "[wwe2k14] version vTWIN")]
    part2 = "\n".join(head + [L(1, "[wwe2k14][scene] sequence " + S3[1]), L(10, "[wwe2k14][scene] sequence " + S75[1]), L(12, hl(1000, 10)), L(14, hl(1100, 10))]) + "\n"
    part1 = "\n".join([L(20, "[wwe2k14][scene] sequence " + S26[1]), L(21, hl(1200, 10)), L(22, hl(1300, 30))]) + "\n"
    part0 = "\n".join([L(24, hl(1400, 50))]) + "\n"
    zp = Path(tmp) / "rot.zip"
    with zipfile.ZipFile(zp, "w") as z:     # members deliberately out of order
        z.writestr("wwe2k14_007.log", part0)
        z.writestr("wwe2k14_007.1.log", part1)
        z.writestr("wwe2k14_007.2.log", part2)
    rc, out, err = run(["--json", str(zp)])
    lg = json.loads(out)["logs"] if rc == 0 else []
    ok = rc == 0 and len(lg) == 1 and len(lg[0].get("parts", [])) == 3
    c = {m["seq"]: m["counters"].get("black_no_3d.n") for m in lg[0]["modes"]} if ok else {}
    check(ok and c.get(75) == 10 and c.get(26) == 40, "R4 rotated parts (zip, members out of order) are one log read oldest first, counters and mode carried: %s" % c)
    fd = Path(tmp) / "rotdir"
    fd.mkdir()
    (fd / "wwe2k14_007.log").write_text(part0, encoding="utf8")
    (fd / "wwe2k14_007.1.log").write_text(part1, encoding="utf8")
    (fd / "wwe2k14_007.2.log").write_text(part2, encoding="utf8")
    rc, out, err = run(["--json", str(fd)])
    lg = json.loads(out)["logs"] if rc == 0 else []
    c = {m["seq"]: m["counters"].get("black_no_3d.n") for m in lg[0]["modes"]} if lg else {}
    check(len(lg) == 1 and c.get(26) == 40, "R4b the same parts in a folder: one log, same counters")
    zp2 = Path(tmp) / "rot2.zip"
    with zipfile.ZipFile(zp2, "w") as z:
        z.writestr("wwe2k14_007.log", part0)
        z.writestr("wwe2k14_007.3.log", part2)
    rc, out, err = run(["--json", str(zp2)])
    lg = json.loads(out)["logs"][0] if rc == 0 else {}
    check(any("PARTS MISSING" in n for n in lg.get("notes", [])), "R4c parts .3 + newest with .1 / .2 absent: says PARTS MISSING")
    j = one(custom([(12, hl(1000, 0))]) +
            "[report] lines left out after their first 30 copies (count left out, message):\n  23  [info] [core] [t1] [scene][front_end] technique fallback: yTrail technique 1 (1 fallbacks, 2 table picks)\n")
    check(j.get("condensed") and any("CONDENSED" in n for n in j["notes"]) and not finds(j, "technique_fallback"), "R5 a condensed report log: loud CONDENSED note, trailer lines not counted")
    j = one(custom([(12, hl(1000, 0))]) + "[report] ... 123456 bytes left out ...\n" + L(30, hl(1100, 0)) + "\n")
    check(j.get("cut") and any("CUT" in n for n in j["notes"]), "R5b an over-24 MB cut marker: loud CUT note")
    j = one("[scene][front_end] technique fallback: yU technique 1 (1 fallbacks, 2 table picks)\n" + custom([]))
    check(any("before the first stamped" in n for n in j["notes"]) and not finds(j, "technique_fallback"), "R6 a class line before any stamp: reported as not placed, not counted")
    j = one(custom([(21, "RAW:[scene][front_end] technique fallback: yCont technique 2 (1 fallbacks, 3 table picks)")]))
    f = finds(j, "technique_fallback")
    check(len(f) == 1 and f[0]["mode_seq"] == 26 and any("unstamped" in n for n in j["notes"]), "R6b an unstamped continuation line takes the previous stamp's mode and is noted")
    j = one(custom([(12, hl(1000, 0))], seqs=((1, "0 -> 3 (startup / title: Startup) at frame 1"), (10, "3 to 75 (main menu) at frame 500"))))
    check(any(f["cls"] == "sequence" and f["text"].startswith("UNPARSED") for f in j["findings"]), "R7 a sequence line whose format changed is UNPARSED, not silently no mode")
    j = one(custom([(12, hl(1000, 40))], seqs=()))
    check(j.get("modes_read") is False and any("NO MODE READ" in n for n in j["notes"]), "R8 no sequence line at all: NO MODE READ and modes_read false")
    base = one(gen())
    check("technique_fallback" in base.get("classes_not_read", []) and "holes" in base.get("classes_not_read", []), "R9 a class whose line never appears is listed 'not read'")
    pl = one(gen(plant=("technique_fallback",)))
    check("classes_not_read" in pl and "technique_fallback" not in pl["classes_not_read"], "R9b ... and is not listed once its line is there")
    j = one(custom([(12, hl(1000, 0)), (14, hl(1100, 30)), (16, hl(1200, 5))]))
    f = finds(j, "black_no_3d")
    check(len(f) == 1 and f[0]["count"] == 35 and f[0]["mode_seq"] == 75, "R10 a counter that falls mid-mode adds the new rise (30 + 5): %s" % [x["count"] for x in f])
    # S105 H: the loader WATCH is several lines; the main line is the finding, the rest is detail, never UNPARSED
    wmain = "[wwe2k14][load] WATCH: no loader progress for 12.0 s while 700 guest frames advanced: req 0x00000001 state 9 (twin)"
    wdet = ["[wwe2k14][load] WATCH request 0x00001234 words 0..15: 00000001 00000002",
            "[wwe2k14][load] WATCH request 0x00001234 words 16..31: 00000001 00000002",
            "[wwe2k14][load] WATCH read-end object 0x00005678 words 32..43: 00000001 ????????",
            "[wwe2k14][load] WATCH read-end job 0x00005678 words 0..15: 00000001 00000002",
            "[wwe2k14][load] WATCH stack tid 12 'twin thread': 0x82000000 0x82000004",
            "[wwe2k14][load] WATCH stacks: the guest thread list did not answer in 2 s (the object table lock is held)"]
    j = one(custom([(21, wmain)] + [(21, d) for d in wdet]))
    f = finds(j, "load_watch")
    check(len(f) == 1 and f[0]["count"] == 1 and "+6 detail" in f[0]["text"] and not unparsed(j, "load_watch"),
          "R11 a multi-line loader WATCH: one finding (the main line), 6 detail lines counted, no UNPARSED: %s" % [x["text"] for x in j["findings"]])
    j = one(custom([(21, wmain)] + [(21, d) for d in wdet[:3]] + [(22, "[wwe2k14][load] WATCH no loader progress twin")]))
    check(len(finds(j, "load_watch")) == 1 and len(unparsed(j, "load_watch")) == 1, "R11b ... a mangled MAIN line is still UNPARSED next to a good dump")
    j = one(custom([(21, d) for d in wdet]))
    check(not finds(j, "load_watch") and not unparsed(j, "load_watch"), "R11c detail lines without a main line: no finding, no UNPARSED")
    j = one(custom([(12, hl(900, 0, 100, "picture 100")), (21, hl(1000, 0, 100, "picture 100")), (22, hl(1100, 0, 130, "picture 100, page_rows 30"))]))
    f = [x for x in finds(j, "rows_not_drawn") if x["mode_seq"] == 26]
    check(len(f) == 1 and f[0]["count"] == 30 and "page_rows" in f[0]["reason"] and "picture" not in f[0]["reason"],
          "R11 the reason is the pair that rose on the rising line, not the span's first line: %s" % [x["reason"] for x in f])
    ev = [(12 + i, "[wwe2k14][file] NtOpenFile 'D:\\twin\\o%d.pac' (relative to handle 0x1A) open access=0x1 -> 0x00000000 handle=0x1 frame=%d" % (i, i)) for i in range(3)]
    j = one(custom(ev))
    check(j["file_opens_logged"] == 3, "R12 NtOpenFile and the '(relative to handle ..)' form are counted as file opens (%d)" % j["file_opens_logged"])
    bp = Path(tmp) / "baseline.json"
    rc, out, err = run(["--json", str(Path(tmp) / "wwe2k14_p_holes.log")])
    bp.write_text(out, encoding="utf8")
    j = one(gen(plant=("holes",)), "wwe2k14_b1.log", ["--baseline", str(bp)])
    check(j["findings"] == [], "R13 --baseline: the same finding as the reference is hidden (%d left)" % len(j["findings"]))
    j = one(gen(plant=("holes", "pic_refusals")), "wwe2k14_b2.log", ["--baseline", str(bp)])
    check([f["cls"] for f in j["findings"]] == ["pic_refusals"], "R13b --baseline: a class not in the reference stays: %s" % [f["cls"] for f in j["findings"]])
    rc, out, err = run(["--baseline", str(Path(tmp) / "nope.json"), str(Path(tmp) / "wwe2k14_b1.log")])
    check(rc == 2, "R13c an unreadable baseline exits 2")
    one(gen(plant=("problem",)), "wwe2k14_pr.log")
    rc, out, err = run([str(Path(tmp) / "wwe2k14_pr.log")])
    check("first-of-name" in out, "R14 problems are first-of-name: said in the output")
    here = Path(__file__).resolve().parents[2]
    roots = [here, os.environ.get("RDR_MAIN_ROOT") or r"C:\Users\Jarrod\wwe2k14-recomp"]
    probs = lint_writers(triage.CLASSES, getattr(triage, "EXTRA_WRITERS", []), roots)
    check(not probs, "R15 every CLASSES row's writer file:line holds its literal %s" % probs)
    moved = [dict(r) for r in triage.CLASSES]
    moved[0]["writer"] = "src/session_log.h"          # the text moved to another file (or is gone): caught
    check(len(lint_writers(moved, [], roots)) == 1, "R15b a writer whose file no longer holds the text is caught (planted)")
    shifted = [dict(r) for r in triage.CLASSES]
    shifted[0]["writer"] = "src/renderer/problem.cpp:1"   # an old file:line form whose line moved: still the same file, not a failure
    check(not lint_writers(shifted, [], roots), "R15c a text that only moved within its file is not a failure (the file + text form)")
    # S108 RPT: the facts table (src\launcher\report_facts.inc) is linted the same way: each row's writer holds its marker, each switch / mod row's cvar is
    # defined in src (the planted twins are in tools\report\facts_selftest.py case_table)
    sys.path.insert(1, str(Path(__file__).resolve().parent))
    import facts_selftest as FS  # noqa: E402
    import report_facts  # noqa: E402
    probs = FS.lint_table(report_facts.load_table(), roots)
    check(not probs, "R16 every report_facts.inc row's writer holds its marker / defines its cvar %s" % probs)
    # S108 AL1: the shared report (src\renderer\problem.cpp DrawShortfall): one line per (kind, object), the `problem` class reads it unchanged, the verdict fails on it
    sf = ["[rdr][PROBLEM] draw_shortfall: pose_rows: event prop 2591 entry 4: asked 658, given 96: model 0xA0001000, pool weapon:ev2591/4: the vertices past row 95 stay at their file position",
          "[rdr][PROBLEM] draw_shortfall: model_part: arena 5: slot 973: asked 1, given 0: slot 973: node table out of range"]
    sp = Path(tmp) / "wwe2k14_sf.log"
    sp.write_text(custom([(21, sf[0]), (22, sf[1]), (60, "Execution complete")]), encoding="utf8")
    j = one(sp.read_text(encoding="utf8"), "wwe2k14_sf.log")
    pf = finds(j, "problem")
    check(len(pf) == 1 and pf[0]["mode_seq"] == 26 and pf[0]["count"] == 2 and pf[0]["reason"].startswith("draw_shortfall: pose_rows: event prop 2591 entry 4: asked 658, given 96")
          and not unparsed(j), "R17 draw_shortfall: both models are one `problem` finding of 2 lines in mode 26, the first reason names the model (%s)" % [(f["count"], f["reason"][:60]) for f in pf])
    rc, out, err = run(["--accepted", str(ACCEPTED), "--verdict", str(sp)])
    check(rc == 1 and "FAIL" in out and "[problem]" in out, "R17b draw_shortfall: the verdict FAILs (rc %d)" % rc)
    pcpp = None
    for root in roots:
        if (Path(root) / "src/renderer/problem.cpp").exists():
            pcpp = (Path(root) / "src/renderer/problem.cpp").read_text(encoding="utf8", errors="replace")
            break
    check(pcpp is not None and '"[rdr][PROBLEM] draw_shortfall: %s: %s: asked %llu, given %llu%s%s"' in pcpp, "R17c problem.cpp still writes the draw_shortfall line this row plants")


# S106 0F: the verdict (triage --accepted --verdict): every FAIL twin is planted in the UNSAFE direction (can it say PASS when the thing is wrong?),
# and the unplanted arm must PASS. entry: triage.main
ACCEPTED = Path(os.environ.get("TRIAGE_DIR") or Path(__file__).resolve().parent) / "triage_accepted.json"
HOSTV = ("[host] frames {f}, ours {o}, 2d_only 100, repeat_3d 0 (loading 0), repeat_full 0, no_record 0, no_picture 0, no_size 0, resize_black 0, black_no_3d {b} (mid_match {mm}), strikes 0, "
         "2d_rows 100 drawn 100 not_drawn {n} ({w}) holes {h} frames_2d_dropped 0 of 1 particles_not_drawn 0")


def hv(f, b=0, mm=0, n=0, w="-", h=0):
    return HOSTV.format(f=f, o=f - 100, b=b, mm=mm, n=n, w=w, h=h)


def win(by, k):
    return "[scene][2d] frames 100, 2d-whole 50, fallback %d (by reason: %s); rows 10, pair (0 1)" % (k, by)


def verdict_rows(tmp):
    def vrun(name, text, acc=None, extra=(), end=True):
        p = Path(tmp) / name
        p.write_text(text + (L(70.5, "Execution complete") + "\n" + L(70.6, "Title terminated; hard-exiting process.") + "\n" if end else ""), encoding="utf8")   # S106 0FF B1: a finished run ends with these two lines
        rc, out, err = run(["--json", "--accepted", str(acc or ACCEPTED), "--verdict"] + list(extra) + [str(p)])
        lg = json.loads(out)["logs"][0] if out.strip().startswith("{") else {}
        return rc, lg.get("verdict", {}), lg

    def flip(tag, ok_text, bad_text, what, bad_has=None, acc=None):
        rc0, v0, _ = vrun("wwe2k14_v%s_ok.log" % tag, ok_text, acc=acc)
        rc1, v1, _ = vrun("wwe2k14_v%s_bad.log" % tag, bad_text, acc=acc)
        check(v0.get("verdict") == "PASS" and rc0 == 0, "V%s unplanted arm: PASS, exit 0 (%s %s %s)" % (tag, rc0, v0.get("verdict"), v0.get("bad")))
        check(v1.get("verdict") == "FAIL" and rc1 == 1 and (bad_has is None or any(bad_has in b for b in v1.get("bad", []))),
              "V%s %s: FAIL, exit 1 (%s %s %s)" % (tag, what, rc1, v1.get("verdict"), v1.get("bad")))

    base = [(21, hv(1000)), (22, hv(1100, b=5, h=10)), (22, win("scene not_ready:50; render -; hole -", 50))]   # S106 0FFN N3: all in mode 26 (the per-mode caps; a counter rising across 75 -> 26 is a straddle)
    flip("1", custom(base), custom(base + [(22, win("refused planes:2; render -", 2))]),
         "a second reason (refused planes) behind an accepted first one, the case --baseline hid", "refused planes")
    flip("2", custom(base), custom([(21, hv(1000)), (22, hv(1100, b=5, h=10)), (22, win("scene not_ready:40; hole 0x82BC1111:10", 50))]),
         "a 2D hole with a named site", "0x82BC1111")
    flip("3", custom(base), custom([(21, hv(1000)), (22, hv(1100, b=5, h=80)), (22, win("scene not_ready:50; render -; hole -", 50))]),
         "2D holes with no site but more than the load-window fallback frames", "[holes_2d]")
    flip("4", custom(base), custom([(21, hv(1000)), (22, hv(1100, b=5, mm=3, h=10)), (22, win("scene not_ready:50; render -; hole -", 50))]),
         "a black frame counted mid-match (mid_match 3)", "mid_match 3")
    flip("5", custom(base), custom(base + [(22, "[host][strikes] 1 strikes: miss 1 | why: twin unaccepted strike")]), "an unaccepted strike text", "twin unaccepted strike")
    # a straddle is accepted only if EVERY mode it names accepts the token
    st_ok = [(15, hv(1000)), (25, hv(1100, n=30, w="page_rows 30"))]
    st_bad = [(15, hv(1000)), (25, hv(1100, n=30, w="twin_pair 30"))]
    wide = Path(tmp) / "accepted_wide.json"
    j = json.loads(ACCEPTED.read_text(encoding="utf8"))
    kept, seen = [], False
    for e in j["entries"]:
        if e["class"] == "rows_not_drawn" and e["sub"] == "^page_rows$":
            if seen:
                continue
            seen = True
            e["modes"], e["max"] = ["*"], 300000     # S106 0FFN: one wide entry: the straddle rule is what V6 / V7 test, the per-mode caps are N3's
        kept.append(e)
    j["entries"] = kept
    wide.write_text(json.dumps(j), encoding="utf8")
    flip("6", custom(st_ok), custom(st_bad), "a straddling window with an unaccepted pair", "twin_pair", acc=wide)
    one_mode = Path(tmp) / "accepted_one_mode.json"
    for e in j["entries"]:
        if e["class"] == "rows_not_drawn" and e["sub"] == "^page_rows$":
            e["modes"] = [26]
    one_mode.write_text(json.dumps(j), encoding="utf8")
    rc, v, _ = vrun("wwe2k14_v7.log", custom(st_ok), acc=one_mode)
    check(v.get("verdict") == "FAIL" and rc == 1, "V7 a straddle over modes 75 and 26 with the token accepted in 26 only: FAIL (%s %s)" % (rc, v.get("bad")))
    # the loader refuses entries that can never be accepted: exit 2 (unreadable), never a quiet accept
    for tag, ent in (("8a", {"class": "load_watch", "sub": ".*", "verdict": "normal"}), ("8b", {"class": "holes_2d", "sub": ".*", "verdict": "normal"}),
                     ("8c", {"class": "black_no_3d", "sub": ".*", "verdict": "normal"}), ("8d", {"class": "fallback_2d", "sub": "^hole ", "verdict": "normal"}),
                     ("8e", {"class": "no_file_open", "sub": ".*", "verdict": "normal"}), ("8f", {"class": "problem", "sub": ".*", "verdict": "normal"})):
        f = Path(tmp) / ("accepted_%s.json" % tag)
        f.write_text(json.dumps({"entries": [ent]}), encoding="utf8")
        rc, v, _ = vrun("wwe2k14_v%s.log" % tag, custom(base), acc=f)
        check(rc == 2, "V%s an accepted entry for a never-acceptable %s is refused: exit 2 (%s)" % (tag, ent["class"], rc))
    rc, out, err = run(["--verdict", str(Path(tmp) / "wwe2k14_v1_ok.log")])
    check(rc == 2, "V9 --verdict without --accepted exits 2")
    rc, out, err = run(["--accepted", str(Path(tmp) / "nope.json"), "--verdict", str(Path(tmp) / "wwe2k14_v1_ok.log")])
    check(rc == 2, "V9b an unreadable accepted file exits 2")
    # the loader WATCH is never accepted; the A6W2 dump lines (h3 / h2 fields, loader heap, where) are detail, not UNPARSED
    wmain = "[wwe2k14][load] WATCH: no loader progress for 12.0 s while 700 guest frames advanced: req 0x00000001 state 9 (twin)"
    wdet = ["[wwe2k14][load] WATCH h3 fields: request 0x00001234 +36=00000000", "[wwe2k14][load] WATCH h2 fields: queue object [0x839400A8]=AF43EEC0",
            "[wwe2k14][load] WATCH loader heap: object 0xAF444D60 index 13", "[wwe2k14][load] WATCH where: worker tid 5 '': lr=82C54904",
            "[wwe2k14][load] WATCH where: no guest thread named like the read-end thread"]
    rc, v, lg = vrun("wwe2k14_v10.log", custom(base + [(23.5, wmain)] + [(23.5, d) for d in wdet]))
    check(not unparsed(lg) and v.get("verdict") == "FAIL" and any("[load_watch]" in b for b in v["bad"]) and not any("UNPARSED" in b for b in v["bad"]),
          "V10 the A6W2 detail lines are not UNPARSED; the WATCH main line is a FAIL no entry can accept (%s)" % v.get("bad"))
    # an idle to the end of the log is normal only with the route's quit line and no failed script
    g = gen(gap="advance")
    rc0, v0, _ = vrun("wwe2k14_v11a.log", g)
    check(v0.get("verdict") == "FAIL" and any("trail, no quit" in b for b in v0.get("bad", [])), "V11 a trailing idle with no quit line: FAIL (%s)" % v0.get("bad"))
    gq = g + L(69.5, "[wwe2k14][input] line 5: quit (frame 9999)") + "\n"
    rc1, v1, _ = vrun("wwe2k14_v11b.log", gq)
    check(v1.get("verdict") == "PASS" and rc1 == 0, "V11b the same idle with the quit line: PASS (%s %s)" % (v1.get("verdict"), v1.get("bad")))
    gf = gq + L(69.6, "[wwe2k14][input] script FAILED at line 19: no file containing 'm3.pac' opened within 600 frames") + "\n"
    rc2, v2, _ = vrun("wwe2k14_v11c.log", gf)
    check(v2.get("verdict") == "FAIL" and rc2 == 1, "V11c a FAILED script is a FAIL, quit line or not (%s)" % v2.get("verdict"))
    # KNOWN names its row, never PASS
    rc, v, _ = vrun("wwe2k14_v12.log", custom(base + [(22, "[scene][ready] calls 600 (warm 1, live 599), ready 599; not built 0; misses 1 (assets asked 2); holes 5 skipped "
                                                       "[wrestlers 5 (first: own-skeleton piece skel:A8F78BE0 is a hole (the game draws it and no model of ours does))]; no stage 0; shadow input 0 skipped")]))
    check(v.get("verdict") == "KNOWN" and v.get("known") == ["CAWP"] and rc == 0, "V12 an accepted known row reads KNOWN with its row id, exit 0 (%s %s)" % (v.get("verdict"), v.get("known")))
    # the 2D hole class is read at all (the S103 logs read as ordinary findings before)
    rc, v, lg = vrun("wwe2k14_v13.log", custom([(12, hv(1000)), (22, hv(1100, h=7))]))
    check(any(f["cls"] == "holes_2d" and f["count"] == 7 for f in lg.get("findings", [])), "V13 the [host] frames line's holes N is read as class holes_2d")
    # ------------------------------------------------------------------------------------------------------------------------------------
    # S106 0FF (docs\research\s106\review_0F.md): the four BLOCK and the FOLLOW rows. Every bad arm is the UNSAFE direction (the old verdict said PASS / KNOWN).
    g0 = gen()
    ln0 = g0.splitlines()
    crash = L(30, "[wwe2k14][crash] unhandled exception 0xC0000005 at wwe2k14.exe+0x1234 (if inside the exe). Backtrace:")
    # B1: a crashed, killed or empty log is never a PASS
    rc, v, _ = vrun("wwe2k14_b1ok.log", g0)
    check(v.get("verdict") == "PASS" and rc == 0, "B1 unplanted arm: a whole synthetic log with its end markers: PASS (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b1a.log", "\n".join(ln0[:int(len(ln0) * 0.6)]) + "\n", end=False)
    check(v.get("verdict") == "FAIL" and rc == 1 and any("end marker" in b for b in v["bad"]), "B1a a log cut at 60 percent (no end marker): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b1d.log", "\n".join(ln0[:int(len(ln0) * 0.7)]) + "\n", end=False)
    check(v.get("verdict") == "FAIL" and rc == 1, "B1d killed mid-match (cut at 70 percent): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b1b.log", "\n".join(ln0[:4]) + "\n")
    check(v.get("verdict") == "FAIL" and rc == 1, "B1b a log cut at boot (the head lines and the end markers only): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b1c.log", g0 + crash + "\n")
    check(v.get("verdict") == "FAIL" and rc == 1 and any("[crash]" in b for b in v["bad"]), "B1c a [crash] line with the end markers present: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # B2: the normal rows have a size
    rc0, v0, _ = vrun("wwe2k14_b2ok.log", custom([(21, hv(1000)), (22, hv(1100, b=5))]))
    check(v0.get("verdict") == "PASS" and rc0 == 0, "B2 unplanted arm: a few scene-start black frames with 3D frames in the mode: PASS (%s %s)" % (v0.get("verdict"), v0.get("bad")))
    rc, v, _ = vrun("wwe2k14_b2a.log", custom([(21, hv(1000)), (22, hv(1100, b=5 * 400))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("over the accepted" in b for b in v["bad"]), "B2a black_no_3d x2000 over the max: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b2b.log", custom([(21, hv(1000)), (22, hv(1100)), (22, win("scene not_ready:5000", 5000))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("over the accepted" in b for b in v["bad"]), "B2b scene not_ready x5000 over the max: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b2c.log", custom([(12, hv(1000)), (21, HOSTV.format(f=1000, o=900, b=0, mm=0, n=0, w="-", h=0)), (22, HOSTV.format(f=1100, o=900, b=5, mm=0, n=0, w="-", h=0))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("no 3D frame" in b for b in v["bad"]), "B2c black frames in a mode that drew no 3D frame of ours (ours +0): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # B3: pass_unknown and the census self-check
    PU = "scene -; render -; hole -; refused -; unknown_site -; remnant_extra -; pass_unknown %d; blend_code -"
    rc, v, _ = vrun("wwe2k14_b3ok.log", custom([(21, hv(1000)), (22, hv(1100)), (22, win("scene not_ready:50", 50))]))
    check(v.get("verdict") == "PASS" and rc == 0, "B3 unplanted arm: a census window with an accepted reason: PASS (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b3a.log", custom([(21, hv(1000)), (22, hv(1100)), (22, win(PU % 50, 50))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("pass_unknown" in b for b in v["bad"]), "B3a 50 pass_unknown fallback frames (the line has no name:count): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    SUMW = "[scene][2d] frames 100, 2d-whole 0, fallback 500 (by reason: scene not_ready:500); rows 10, pair (0 1); SUM MISMATCH (reasons 500 vs fallback 500, whole 0 of 600)"
    for tag, tail, want in (("b", "", "SUM MISMATCH"), ("c", "; UNCOUNTED 7 (handled 107, closed 100)", "UNCOUNTED"), ("d", "; orphan notes 3, pending dropped 0, window 100", "orphan notes"),
                            ("e", "; orphan notes 0, pending dropped 2, window 100", "pending dropped")):
        wl = win("scene not_ready:50", 50) + tail if tag != "b" else SUMW
        rc, v, _ = vrun("wwe2k14_b3%s.log" % tag, custom([(21, hv(1000)), (22, hv(1100)), (22, wl)]))
        check(v.get("verdict") == "FAIL" and rc == 1 and any(want in b for b in v["bad"]), "B3%s a census window with %s: FAIL (%s %s)" % (tag, want, v.get("verdict"), v.get("bad")))
    # B4: only the CAWP own-skeleton texts are known
    CAWP = "[scene][ready] calls 600 (warm 1, live 599), ready 599; not built 0; misses 1 (assets asked 2); holes 5 skipped [wrestlers 5 (first: %s)]; no stage 0; shadow input 0 skipped"
    for tag, txt, want in (("ok", "own-skeleton piece skel:AB12 is a hole (the game draws it and no model of ours does)", None),
                           ("ok2", "own-skeleton piece skel:AB12: the owner model has no mesh of piece 7", None),
                           ("a", "own-skeleton piece skel:AB12: the record does not name its piece id / owner slot", "does not name"),
                           ("b", "own-skeleton piece skel:AB12: the record has no readable [model+252] (MdlAlpha) row for it", "MdlAlpha")):
        rc, v, _ = vrun("wwe2k14_b4%s.log" % tag, custom(base + [(22, CAWP % txt)]))
        if want is None:
            check(v.get("verdict") == "KNOWN" and v.get("known") == ["CAWP"] and rc == 0, "B4%s the real CAWP text reads KNOWN (%s %s %s)" % (tag, rc, v.get("verdict"), v.get("bad")))
        else:
            check(v.get("verdict") == "FAIL" and rc == 1 and any(want in b for b in v["bad"]), "B4%s another own-skeleton fault text: FAIL (%s %s %s)" % (tag, rc, v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_b4c.log", custom(base + [(22, "[record_frame] [scene][wrestlers] skip frame 10 obj skel:T reason own-skeleton piece skel:T: the record has no readable [model+252] (MdlAlpha) row for it hole yes")]))
    check(v.get("verdict") == "FAIL" and rc == 1, "B4c the same text as a hole_skip line: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # F1: a KNOWN row has a size too
    CAWH = PLANTS["caw_holes"][0][0]
    rc, v, _ = vrun("wwe2k14_f1ok.log", custom(base + [(21, CAWH.replace("ALL wrestlers 13", "ALL wrestlers 0")), (22, CAWH)]))
    check(v.get("verdict") == "KNOWN" and rc == 0, "F1 unplanted arm: 13 own-skeleton piece holes: KNOWN (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_f1a.log", custom(base + [(21, CAWH.replace("ALL wrestlers 13", "ALL wrestlers 0")), (22, CAWH.replace("ALL wrestlers 13", "ALL wrestlers 13000"))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("over the accepted" in b for b in v["bad"]), "F1 a KNOWN row (CAWP) x13000 over its max: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    many = [(21 + i * 0.001, "[record_frame] [scene][wrestlers] skip frame %d obj skel:H%d reason own-skeleton piece skel:H%d is a hole (the game draws it and no model of ours does) hole yes" % (i, i, i)) for i in range(3000)]
    rc, v, _ = vrun("wwe2k14_f1b.log", custom(base + many))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("over the accepted" in b for b in v["bad"]), "F1b 3000 hole_skip lines each with its own piece hash (distinct texts): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # F4: a rotated log: the newest file given alone is read with its older parts
    fd = Path(tmp) / "f4"
    fd.mkdir()
    (fd / "wwe2k14_070.log").write_text(g0 + L(70.5, "Execution complete") + "\n", encoding="utf8")
    (fd / "wwe2k14_070.1.log").write_text(L(0, "[wwe2k14] version vTWIN") + "\n" + L(1, "[rdr][PROBLEM] device_lost: vkQueueSubmit returned VkResult -4 (twin)") + "\n", encoding="utf8")
    rc, out, err = run(["--json", "--accepted", str(ACCEPTED), "--verdict", str(fd / "wwe2k14_070.log")])
    v = json.loads(out)["logs"][0]["verdict"] if out.strip().startswith("{") else {}
    check(v.get("verdict") == "FAIL" and rc == 1 and any("[problem]" in b for b in v["bad"]), "F4 the newest file of a rotated log is read with its older part (a PROBLEM line in .1): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # F6: the never-acceptable lint tests every mid_match 1..1000 and the exact holes_2d text
    for tag, ent in (("6a", {"class": "black_no_3d", "sub": "^mid_match (0|2)$", "verdict": "normal"}), ("6b", {"class": "holes_2d", "sub": "^no site", "verdict": "normal"}),
                     ("6c", {"class": "black_no_3d", "sub": "^mid_match 0|mid_match 9$", "verdict": "normal"})):
        f = Path(tmp) / ("accepted_%s.json" % tag)
        f.write_text(json.dumps({"entries": [ent]}), encoding="utf8")
        rc, v, _ = vrun("wwe2k14_v%s.log" % tag, custom(base), acc=f)
        check(rc == 2, "V%s a too-wide entry for %s (%s) is refused: exit 2 (%s)" % (tag, ent["class"], ent["sub"], rc))
    # F7: the [host] frames counters nobody read before: each one rising is a finding with no accepted entry
    for ctr in ("no_record", "no_picture", "no_size", "resize_black", "repeat_full", "particles_not_drawn"):
        rc, v, lg = vrun("wwe2k14_f7_%s.log" % ctr, custom([(21, hv(1000)), (22, hv(1100).replace(ctr + " 0", ctr + " 9"))]))
        check(v.get("verdict") == "FAIL" and rc == 1 and any("[%s]" % ctr in b for b in v["bad"]) and not unparsed(lg), "F7 [host] frames %s rising: a class, FAIL (%s %s)" % (ctr, v.get("verdict"), v.get("bad")))
    # F9: a cut / condensed / part-missing report is not a PASS
    rc, v, _ = vrun("wwe2k14_f9a.log", custom(base) + "[report] ... 123456 bytes left out ...\n")
    check(v.get("verdict") == "FAIL" and rc == 1 and any("incomplete" in b for b in v["bad"]), "F9a a cut report log: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_f9b.log", custom(base) + "[report] lines left out after their first 30 copies (count left out, message):\n")
    check(v.get("verdict") == "FAIL" and rc == 1 and any("incomplete" in b for b in v["bad"]), "F9b a condensed report log: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    zp = Path(tmp) / "f9.zip"
    with zipfile.ZipFile(zp, "w") as z:
        z.writestr("wwe2k14_007.log", g0 + L(70.5, "Execution complete") + "\n")
        z.writestr("wwe2k14_007.3.log", g0)
    rc, out, err = run(["--json", "--accepted", str(ACCEPTED), "--verdict", str(zp)])
    v = json.loads(out)["logs"][0]["verdict"] if out.strip().startswith("{") else {}
    check(v.get("verdict") == "FAIL" and rc == 1 and any("incomplete" in b for b in v["bad"]), "F9c a zip with a part missing (.3 and the newest, no .1 / .2): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # F8: over a folder the verdict lists accepted rows no log matched
    fd2 = Path(tmp) / "f8"
    fd2.mkdir()
    (fd2 / "wwe2k14_001.log").write_text(g0 + L(70.5, "Execution complete") + "\n", encoding="utf8")
    (fd2 / "wwe2k14_002.log").write_text(g0 + L(70.5, "Execution complete") + "\n", encoding="utf8")
    rc, out, err = run(["--accepted", str(ACCEPTED), "--verdict", str(fd2)])
    check(rc == 0 and "STALE accepted rows" in out and "CAWP" in out, "F8 a folder verdict lists the accepted rows no log matched (%s)" % out[-300:].replace("\n", " | "))
    # F5: the sweep wrapper's verdict block (tools\bench\sweep_verdict.ps1): a missing log / run line is a FAIL, the sweep exits 1 on any row that is not PASS or KNOWN
    import subprocess
    good, missing = Path(tmp) / "wwe2k14_999.log", Path(tmp) / "wwe2k14_998.log"
    good.write_text(g0 + L(70.5, "Execution complete") + "\n" + L(70.6, "Title terminated; hard-exiting process.") + "\n", encoding="utf8")
    here = Path(__file__).resolve().parents[2]
    r = subprocess.run(["pwsh", "-NoProfile", "-File", str(here / "tools" / "bench" / "sweep_verdict_test.ps1"), "-GoodLog", str(good), "-MissingLog", str(missing)],
                       capture_output=True, text=True, cwd=str(here), timeout=120)
    kv = dict(ln.split("=", 1) for ln in r.stdout.splitlines() if "=" in ln)
    check(kv.get("GOOD") == "PASS", "F5 unplanted arm: the wrapper's verdict for a whole log reads PASS (%s %s)" % (kv.get("GOOD"), r.stderr[-200:]))
    check(kv.get("MISSING") == "FAIL (no verdict)", "F5 a planted missing log reads FAIL (no verdict) (%s)" % kv.get("MISSING"))
    check(kv.get("NORUN") == "FAIL (no run line)", "F5 a route with no run line reads FAIL (no run line) (%s)" % kv.get("NORUN"))
    check(kv.get("CRASHED") == "FAIL (run)", "F5 a run line with a crash exit code reads FAIL (run) (%s)" % kv.get("CRASHED"))
    check(kv.get("EXIT_EMPTY") == "1" and "FAIL" in kv.get("SUMMARY_EMPTY", ""), "N5 a sweep with no verdicts at all exits 1 (%s %s)" % (kv.get("EXIT_EMPTY"), kv.get("SUMMARY_EMPTY")))   # S106 0FFN N5
    check(kv.get("EXIT_OK") == "0" and kv.get("EXIT_BAD") == "1" and "FAIL 1" in kv.get("SUMMARY_BAD", ""), "F5 the sweep's count line and exit code: 0 on PASS / KNOWN only, 1 otherwise (%s %s %s)" % (kv.get("EXIT_OK"), kv.get("EXIT_BAD"), kv.get("SUMMARY_BAD")))
    src = (here / "tools" / "bench" / "sweep_runlist.ps1").read_text(encoding="utf8")
    check("Get-RunVerdict" in src and "exit $sum.Exit" in src and "Get-SweepSummary" in src, "F5 sweep_runlist.ps1 uses the verdict functions and exits with the summary")

    # ------------------------------------------------------------------------------------------------------------------------------------
    # S106 0FFN (the third round, docs\research\s106\review_0FF.md N1-N5). Every bad arm is the UNSAFE direction: the old verdict said PASS / KNOWN.
    def routed(g, quit_line=True):
        """A route's log: the `modified: wwe2k14_input_script` line and the script's load line after the head; its `quit` line near the end (or none)."""
        ls = g.splitlines()
        ls[4:4] = [L(0.5, "[wwe2k14] modified: wwe2k14_input_script = 'C:\\twin\\route.txt' (source: command line)"), L(0.6, "[wwe2k14][input] loaded 31 named states from C:\\twin\\states.toml")]
        return "\n".join(ls) + "\n" + (L(69.5, "[wwe2k14][input] line 5: quit (frame 9999)") + "\n" if quit_line else "")
    # N1: a run whose window closed before the route finished (the end lines are written on ANY close) is a FAIL
    rc, v, _ = vrun("wwe2k14_n1ok.log", routed(g0))
    check(v.get("verdict") == "PASS" and rc == 0, "N1 unplanted arm: a route log with its quit line and the end lines: PASS (%s %s)" % (v.get("verdict"), v.get("bad")))
    for tag, frac in (("a", 1.0), ("b", 0.6), ("c", 0.8)):
        r = routed(g0, quit_line=False)
        rl = r.splitlines()
        rc, v, _ = vrun("wwe2k14_n1%s.log" % tag, "\n".join(rl[:int(len(rl) * frac)]) + "\n")
        check(v.get("verdict") == "FAIL" and rc == 1 and any("no quit line" in b for b in v["bad"]),
              "N1%s a route log cut at %d percent, the two end lines appended (the window closed): FAIL, no quit line (%s %s)" % (tag, int(frac * 100), v.get("verdict"), v.get("bad")))
    onlyin = g0.splitlines()
    onlyin.insert(4, L(0.6, "[wwe2k14][input] loaded 31 named states from C:\\twin\\states.toml"))
    rc, v, _ = vrun("wwe2k14_n1d.log", "\n".join(onlyin) + "\n")
    check(v.get("verdict") == "FAIL" and rc == 1 and any("no quit line" in b for b in v["bad"]), "N1d an [input] line with no `modified:` line and no quit: FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # N2: the hang guard's lines are a class (HANG? / HANG: / BLACK:); the recovery and picture lines are not findings and not UNPARSED
    HG = L(30, "[hang] HANG? no present for 20 s, guest frame 1234")
    rc, v, lg = vrun("wwe2k14_n2ok.log", g0 + L(30, "[hang] HANG over: guest frames and presents go on, guest frame 1300") + "\n" + L(31, "[hang] BLACK over: presents came back") + "\n"
                     + L(32, "[hang] picture 30 s in: fine, 20 of 24 points bright, 5 ms") + "\n" + L(2, "[hang] guard on: HANG? 20 s, HANG: 60 s (120 s before the first frame), exit off") + "\n")
    check(v.get("verdict") == "PASS" and rc == 0 and not unparsed(lg), "N2 unplanted arm: the over / picture / guard-on lines: PASS, nothing UNPARSED (%s %s)" % (v.get("verdict"), v.get("bad")))
    for tag, txt in (("a", HG), ("b", L(30, "[hang] BLACK: no present for 25 s, 900 guest frames ran (presents 5, acquire timeouts 0)")),
                     ("c", L(30, "[hang] HANG: guest side, no guest frame for 60 s, guest frame 1234")), ("d", L(30, "[hang] BLACK: dark picture, all 3 captures, 30 to 120 s in"))):
        rc, v, lg = vrun("wwe2k14_n2%s.log" % tag, g0 + txt + "\n" + L(31, "[hang] HANG over: guest frames and presents go on, guest frame 1300") + "\n")
        check(v.get("verdict") == "FAIL" and rc == 1 and any("[hang]" in b for b in v["bad"]) and not unparsed(lg), "N2%s a [hang] line in the log (a later `over` does not undo it): FAIL (%s %s)" % (tag, v.get("verdict"), v.get("bad")))
    # N3: the caps are per mode: a mode the row was never seen in FAILs, and a mode's own max holds
    S9 = (10, "3 -> 9 (in-game: Match) at frame 500")
    rc, v, _ = vrun("wwe2k14_n3ok.log", custom([(21, hv(1000)), (22, hv(1100, b=150))]))
    check(v.get("verdict") == "PASS" and rc == 0, "N3 unplanted arm: 150 scene-start black frames in mode 26 (its own max 240): PASS (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_n3ok2.log", custom([(12, hv(1000)), (13, hv(1100, b=600))], seqs=(S3, S9, S26)))
    check(v.get("verdict") == "PASS" and rc == 0, "N3 unplanted arm: 600 scene-start black frames in mode 9 (seen there, max 700): PASS (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_n3a.log", custom([(21, hv(1000)), (22, hv(1100, b=600))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("over the accepted" in b for b in v["bad"]), "N3a 600 black frames in mode 26 (max 240 there; the old one cap said 1500): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    S36 = (10, "3 -> 36 (streak menu: CStreakMeunSequence) at frame 500")
    rc, v, _ = vrun("wwe2k14_n3b.log", custom([(12, hv(1000)), (13, hv(1100, b=5))], seqs=(S3, S36, S26)))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("[black_no_3d]" in b for b in v["bad"]), "N3b 5 black frames in mode 36 (never seen there): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_n3c.log", custom([(21, hv(1000)), (22, hv(1100)), (22, win("scene not_ready:300", 300))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("over the accepted" in b for b in v["bad"]), "N3c scene not_ready x300 in mode 26 (max 240 there): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    rc, v, _ = vrun("wwe2k14_n3d.log", custom([(12, hv(1000)), (13, hv(1100)), (13, win("scene not_ready:5", 5))]))
    check(v.get("verdict") == "FAIL" and rc == 1 and any("[fallback_2d]" in b for b in v["bad"]), "N3d scene not_ready x5 in mode 75 (never seen there): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # N4: a log missing its oldest part reads KNOWN / PASS today: the head (run log number) is gone
    rc, v, _ = vrun("wwe2k14_n4a.log", "\n".join(l for l in g0.splitlines() if "run log number" not in l) + "\n")
    check(v.get("verdict") == "FAIL" and rc == 1 and any("log head missing" in b for b in v["bad"]), "N4a a log with no `run log number:` line: FAIL, log head missing (%s %s)" % (v.get("verdict"), v.get("bad")))
    fd4 = Path(tmp) / "n4"
    fd4.mkdir()
    (fd4 / "wwe2k14_071.log").write_text(g0 + L(70.5, "Execution complete") + "\n", encoding="utf8")
    (fd4 / "wwe2k14_071.1.log").write_text(L(0, "[wwe2k14] version vTWIN") + "\n" + L(1, "[wwe2k14][scene] sequence " + S3[1] + ", 1.0 s after start") + "\n", encoding="utf8")
    rc, out, err = run(["--json", "--accepted", str(ACCEPTED), "--verdict", str(fd4 / "wwe2k14_071.log")])
    v = json.loads(out)["logs"][0]["verdict"] if out.strip().startswith("{") else {}
    check(v.get("verdict") == "FAIL" and rc == 1 and any("log head missing" in b for b in v["bad"]), "N4b a rotated log whose oldest part (.1) has no run log line (the newest part has it): FAIL (%s %s)" % (v.get("verdict"), v.get("bad")))
    # minor: a row is stale only when NONE of its entries matched
    st = triage.stale_rows([dict(row="X", cls="holes", hits=3), dict(row="X", cls="holes"), dict(row="Y", cls="holes")])
    check(st == ["Y"], "stale rows: a row with one entry that matched is not stale, a row with none is (%s)" % st)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        base = triage_text(tmp, "wwe2k14_base.log", gen())
        check(base["findings"] == [], "clean synthetic log: no finding at all (%d)" % len(base["findings"]))
        check([m["seq"] for m in base["modes"]] == [-1, 3, 75, 26] or [m["seq"] for m in base["modes"]] == [3, 75, 26], "modes read from the sequence lines: %s" % [m["seq"] for m in base["modes"]])
        names = list(PLANTS) + list(HOST_PLANTS)
        for c in names:
            planted = triage_text(tmp, "wwe2k14_p_%s.log" % c, gen(plant=(c,)))
            fl = finds(planted, c)
            check(len(fl) >= 1 and all(f["mode_seq"] == MODE_FAULT for f in fl), "%s planted: finding in mode %d (%d)" % (c, MODE_FAULT, len(fl)))
            exp = PLANTS[c][2] if c in PLANTS else None
            if exp:
                check(fl and exp in fl[0]["reason"], "%s planted: first reason carries '%s'" % (c, exp))
            check(not unparsed(planted), "%s planted: nothing UNPARSED" % c)
            if c in HOST_PLANTS:
                check(fl and fl[0]["count"] in (50, 20), "%s planted: count is the mode's rise (%s)" % (c, fl[0]["count"] if fl else None))
            clean = triage_text(tmp, "wwe2k14_c_%s.log" % c, gen())
            check(not finds(clean, c) and not unparsed(clean), "%s removed: no finding, nothing UNPARSED" % c)
            mangled = triage_text(tmp, "wwe2k14_m_%s.log" % c, gen(mangle=(c,)))
            check(len(unparsed(mangled, c)) >= 1 and not finds(mangled, c), "%s format changed: UNPARSED, not 0" % c)
        # a counter carried over: planted in mode 26, mode 75 afterwards must not repeat it
        p = triage_text(tmp, "wwe2k14_carry.log", gen(plant=("black_no_3d",)))
        check(all(f["mode_seq"] == MODE_FAULT for f in finds(p, "black_no_3d")), "a counter that stays raised after the mode is not the next mode's fault")
        # the file-open gap
        g = triage_text(tmp, "wwe2k14_gap.log", gen(gap="advance"))
        gf = finds(g, "no_file_open")
        check(len(gf) == 1 and gf[0]["mode_seq"] == 75 and gf[0]["count"] >= 25, "gap to the end of the log with frames advancing: finding (%s)" % [(f["mode"], f["count"]) for f in gf])
        check(not finds(triage_text(tmp, "wwe2k14_frozen.log", gen(gap="frozen")), "no_file_open"), "same gap with the frames frozen: no finding")
        check(not finds(triage_text(tmp, "wwe2k14_short.log", gen(gap="short")), "no_file_open"), "a gap under 10 s: no finding")
        mg = triage_text(tmp, "wwe2k14_mid.log", gen(mid_gap=True))
        check(not finds(mg, "no_file_open") and any(m["gap_s"] for m in mg["modes"]), "a gap in the middle of the log is shown per mode but is not a finding")
        # a log with no file opens at all says so, it does not claim "no gap"
        nof = "\n".join(l for l in gen().splitlines() if "[file]" not in l) + "\n"
        rc, out, err = run([str(Path(tmp) / "wwe2k14_nof.log")]) if (Path(tmp) / "wwe2k14_nof.log").write_text(nof, encoding="utf8") else (0, "", "")
        check(rc == 0 and "class no_file_open not read" in out, "no file opens logged: the class is reported as not read")
        # unreadable inputs
        bad = Path(tmp) / "bad.zip"
        bad.write_bytes(b"this is not a zip")
        check(run([str(bad)])[0] == 2, "an unreadable zip exits 2")
        txt = Path(tmp) / "wwe2k14_notalog.log"
        txt.write_text("hello\nworld\n", encoding="utf8")
        check(run([str(txt)])[0] == 2, "a text file with no log stamps exits 2")
        check(run([str(Path(tmp) / "missing.log")])[0] == 2, "a missing path exits 2")
        empty = Path(tmp) / "emptydir"
        empty.mkdir()
        check(run([str(empty)])[0] == 2, "a folder with no game log exits 2")
        zf = Path(tmp) / "empty.zip"
        with zipfile.ZipFile(zf, "w") as z:
            z.writestr("about.txt", "WWE 2K14 recomp problem report\n")
        check(run([str(zf)])[0] == 2, "a zip with no game log exits 2")
        # a good zip: machine lines + a log read in place
        good = Path(tmp) / "good.zip"
        with zipfile.ZipFile(good, "w") as z:
            z.writestr("about.txt", "WWE 2K14 recomp problem report\nVersion: vTWIN\nGPU: Twin GPU\nCPU: Twin CPU\nRAM: 1 GB\nPlayer's note: none\n")
            z.writestr("wwe2k14_999.log", gen(plant=("holes",)))
        rc, out, err = run(["--json", str(good)])
        j = json.loads(out)["logs"][0] if rc == 0 else {}
        check(rc == 0 and "GPU: Twin GPU" in j.get("machine_lines", []) and finds(j, "holes"), "a zip: machine block read and the planted finding found")
        rc, out, err = run([str(good)])
        check(rc == 0 and "FINDINGS (worst first):" in out and "[holes]" in out, "the text output ends in a FINDINGS block")
        review_rows(tmp)
        verdict_rows(tmp)   # S106 0F
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

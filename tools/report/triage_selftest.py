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
    return L(t, "[host] frames %d, ours %d, 2d_only %d, repeat_3d 0 (loading 0), repeat_full 0, %s, strikes 0, 2d_rows 100 drawn 100 %s holes 0"
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
HOST = "[host] frames {f}, ours {o}, 2d_only 100, repeat_3d 0 (loading 0), repeat_full 0, black_no_3d {b} (mid_match 0), strikes 0, 2d_rows 100 drawn 100 not_drawn {n} ({w}) holes 0"


def hl(f, b, n=0, w="-"):
    return HOST.format(f=f, o=f - 100, b=b, n=n, w=w)


def custom(evs, seqs=(S3, S75, S26, S75B), head=True):
    allv = [(t, "[wwe2k14][scene] sequence " + x + ", 1.0 s after start") for t, x in seqs] + list(evs)
    allv.sort(key=lambda e: e[0])
    lines = [L(0, "[wwe2k14] version vTWIN")] if head else []
    lines += [(m[4:] if m.startswith("RAW:") else L(t, m)) for t, m in allv]
    return "\n".join(lines) + "\n"


def lint_writers(rows, extra, roots):
    """Each row's writer file:line must hold its literal. Returns the problems (a writer that moved or changed = a stale pattern table)."""
    bad = []
    for name, writer, lit in [(r["name"], r.get("writer"), r.get("lit")) for r in rows] + list(extra):
        if not writer:
            continue
        path, _, ln = writer.rpartition(":")
        for root in roots:
            fp = Path(root) / path
            if fp.exists():
                lines = fp.read_text(encoding="utf8", errors="replace").splitlines()
                if not (0 < int(ln) <= len(lines) and lit in lines[int(ln) - 1]):
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
    moved[0]["writer"] = "src/renderer/problem.cpp:70"
    check(len(lint_writers(moved, [], roots)) == 1, "R15b a writer that moved is caught (planted)")


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
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

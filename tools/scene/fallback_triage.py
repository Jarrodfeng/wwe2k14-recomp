#!/usr/bin/env python3
"""Run triage: what kept the frames from being ours, per scene (S103 L2b, landing 2 piece P2; docs\\renderer\\LANDING2_PLAN.md section 3).

  py -I tools\\scene\\fallback_triage.py LOG [--top N] [--stale FRAMES]
  py -I tools\\scene\\fallback_triage.py --selftest          (the check: parses fixtures of both line formats)

Reads a run log and prints, per game scene, the top reasons the frame was not wholly ours:
  1. the `[scene][2d] frames F, 2d-whole W, fallback B (by reason: ...)` census lines (src\\renderer_game\\fallback_census.h). Every line's FIRST part is one window (the exit line's first part is the
     final window; its `; run totals:` part is only cross-checked: the windows must add up to it). A window belongs to the scene its `in scene NAME` tag names (a scene change flushes the window under the
     old scene), else to the scene of the last `[wwe2k14][scene] sequence A -> B (name)` line before it. `n/a` = nobody counts that reason yet (not zero).
     INCOMPLETE (the census is not a proof) when any line says SUM MISMATCH / UNCOUNTED, or has orphan notes, pending dropped, pending unadopted > 0, or the windows do not add up to the run totals.
  2. the scene inputs: `[scene][inputs] moved ... name N/fL` (and `name (open) N/fL` for an input with no reader) gives each input's last stamped frame L; an input whose last stamp is more than
     --stale (default 120) frames behind the newest stamp in the same line STOPPED being captured: it keeps `records ours 0` at 0. The `[blocks the frame: ...]` segment names the inputs with no reader.
Exit code 0 always (a reader, not a check); the last line names the top blocker of the whole run."""
import argparse
import re
import sys
from collections import OrderedDict

SEQ = re.compile(r"\[wwe2k14\]\[scene\] sequence (\d+) -> (\d+) \(([^)]*)\)")
BIT = re.compile(r"\b([a-z_0-9]+) (?:\(open\) )?(\d+)/f(\d+)")
BLOCKS = re.compile(r"\[blocks the frame:([^\]]*)\]")
OURS = re.compile(r"\[scene\]\[changes\] records ours (\d+) walked (\d+)")
TWOD = re.compile(r"\[scene\]\[2d\] (exit: )?(frames (\d+), 2d-whole (\d+), fallback (\d+) \(by reason: (.*?)\))")
BITNAMES = ["post_settings", "model_runtime", "sphere_occ", "wrestler_palette", "light_states", "arena_worlds", "ring_rows", "caster_entries", "circle_points",
            "plane_constants", "crowd_snapshot", "particles", "camera", "material_params", "screens", "stage_pools", "arena_models", "stage_clock"]


def slug(text, cap=40):
    """The census's Slug (fallback_census.h): lower case, runs of non [a-z0-9] -> one '_', trimmed, capped."""
    out = ""
    for c in text:
        o = c.lower()
        if ("a" <= o <= "z") or ("0" <= o <= "9"):
            out += o
        elif out and out[-1] != "_":
            out += "_"
        if len(out) >= cap:
            break
    return out.strip("_") or "unnamed"


def parse_groups(text):
    groups = OrderedDict()
    for part in text.split("; "):
        name, _, rest = part.partition(" ")
        d = {}
        if rest and rest not in ("-", "n/a"):
            if name == "pass_unknown":
                d["all"] = int(rest)
            else:
                for kv in rest.split(","):
                    k, _, v = kv.rpartition(":")
                    d[k] = int(v)
        groups[name] = d
    return groups


def parse_2d(line):
    """-> dict(exit, frames, whole, fallback, groups, problems[], scene_tag, totals) from one [scene][2d] line; only the WINDOW part (before '; run totals:') is read for the window."""
    head, _, tail = line.partition("; run totals: ")
    m = TWOD.search(head)
    if not m:
        return None
    r = {"exit": bool(m.group(1)), "frames": int(m.group(3)), "whole": int(m.group(4)), "fallback": int(m.group(5)), "groups": parse_groups(m.group(6)), "problems": [], "scene_tag": None, "totals": None}
    if "SUM MISMATCH" in head:
        r["problems"].append("SUM MISMATCH")
    if "UNCOUNTED" in head:
        r["problems"].append("UNCOUNTED")
    for what in ("orphan notes", "pending dropped", "pending unadopted"):
        mm = re.search(what + r" (\d+)", head)
        if mm and int(mm.group(1)) > 0:
            r["problems"].append(f"{what} {mm.group(1)}")
    mm = re.search(r"; in scene ([a-z0-9_]+)", head)
    if mm:
        r["scene_tag"] = mm.group(1)
    if tail:
        mt = TWOD.search("[scene][2d] " + tail)
        if mt:
            r["totals"] = (int(mt.group(3)), int(mt.group(4)), int(mt.group(5)))
    return r


class Scene:
    def __init__(self, name):
        self.name = name
        self.win = []      # (frames, whole, fallback, groups) of every window (exit lines' final windows included)
        self.problems = set()
        self.bits = None   # the last [scene][inputs] reading: {name: (count, last_frame)}
        self.blocks = None  # the last `[blocks the frame: ...]` names
        self.ours = None   # the last (ours, walked) from [scene][changes]


def read(lines):
    """-> (scenes OrderedDict, n2d, run problems[])."""
    scenes = OrderedDict()
    by_slug = {}
    cur = Scene("(before the first scene line)")
    scenes[cur.name] = cur

    def get(name):
        sc = scenes.get(name)
        if sc is None:
            sc = scenes[name] = Scene(name)
        by_slug.setdefault(slug(name), sc)
        return sc

    n2d = 0
    win_total = [0, 0, 0]
    run_totals = None
    run_problems = []
    for line in lines:
        if "[wwe2k14][scene] sequence" in line:
            m = SEQ.search(line)
            if m:
                cur = get(m.group(3))
            continue
        if "[scene][2d]" in line:
            r = parse_2d(line)
            if r:
                n2d += 1
                sc = by_slug.get(r["scene_tag"]) if r["scene_tag"] else None
                if sc is None and r["scene_tag"]:
                    sc = get(r["scene_tag"])   # a tag that names no scene line of this log (kept under its slug)
                sc = sc or cur
                sc.win.append((r["frames"], r["whole"], r["fallback"], r["groups"]))
                sc.problems.update(r["problems"])
                win_total[0] += r["frames"]
                win_total[1] += r["whole"]
                win_total[2] += r["fallback"]
                if r["exit"]:
                    run_totals = r["totals"]
            continue
        if "[scene][inputs] moved" in line:
            body = line.split("[scene][inputs] moved", 1)[1]
            seg = body.split("[blocks the frame", 1)[0]
            bits = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in BIT.finditer(seg) if m.group(1) in BITNAMES}
            if bits:
                cur.bits = bits
            mb = BLOCKS.search(line)
            if mb:
                cur.blocks = [f"{n} ({o})" for n, o in re.findall(r"([a-z_0-9]+) \(([^)]*)\)", mb.group(1))]
            continue
        if "[scene][changes] records ours" in line:
            m = OURS.search(line)
            if m:
                cur.ours = (int(m.group(1)), int(m.group(2)))
    if run_totals is not None and tuple(win_total) != tuple(run_totals):
        run_problems.append(f"the windows add up to frames/whole/fallback {tuple(win_total)} but the run totals say {tuple(run_totals)}")
    return scenes, n2d, run_problems


def report(scenes, n2d, run_problems, top=6, stale_frames=120, out=print):
    run_blockers = {}
    bad = bool(run_problems)
    for sc in scenes.values():
        if not sc.win and sc.bits is None and sc.ours is None:
            continue
        out(f"\nscene: {sc.name}")
        if sc.win:
            fr, wh, fb = (sum(u[i] for u in sc.win) for i in range(3))
            flag = ""
            if sc.problems:
                bad = True
                flag = "   !! INCOMPLETE (" + ", ".join(sorted(sc.problems)) + "): the census is NOT a proof here"
            out(f"  census ({len(sc.win)} window(s)): frames {fr}, 2d-whole {wh}, fallback {fb}" + flag)
            tally = {}
            for u in sc.win:
                for g, d in u[3].items():
                    for k, v in d.items():
                        if v:
                            tally[(g, k)] = tally.get((g, k), 0) + v
            for (g, k), v in sorted(tally.items(), key=lambda kv: -kv[1])[:top]:
                out(f"    {v:8d}  {g} {k}")
                run_blockers[(g, k)] = run_blockers.get((g, k), 0) + v
        if sc.ours is not None:
            out(f"  records ours {sc.ours[0]} walked {sc.ours[1]} (cumulative, [scene][changes])")
        if sc.blocks:
            out("  blocks the frame (no reader): " + ", ".join(sc.blocks))
        if sc.bits:
            newest = max(v[1] for v in sc.bits.values())
            stale = [(n, c, l) for n, (c, l) in sc.bits.items() if l > 0 and newest - l > stale_frames]
            never = [n for n, (c, l) in sc.bits.items() if l == 0]
            stale.sort(key=lambda t: -(newest - t[2]))
            if stale:
                out("  inputs that stopped being stamped (the legacy reading of the scene gate):")
                for n, c, l in stale:
                    bit = BITNAMES.index(n)
                    out(f"    bit {bit} {n}: last stamped frame {l}, {newest - l} frames behind the newest ({newest}); {c} stamps in all")
                    run_blockers[("scene", f"bit{bit}_{n}")] = run_blockers.get(("scene", f"bit{bit}_{n}"), 0) + (newest - l)
            else:
                out("  inputs: none stopped (every stamped input is within " + str(stale_frames) + " frames of the newest)")
            if never:
                out("  never stamped in this scene (f0; blocks only if required): " + ", ".join(f"bit {BITNAMES.index(n)} {n}" for n in never))
    for p in run_problems:
        out("\n!! INCOMPLETE: " + p)
    if run_blockers:
        (g, k), v = max(run_blockers.items(), key=lambda kv: kv[1])
        out(f"\ntop blocker of the run: {g} {k} ({v})" + ("   [census INCOMPLETE]" if bad else ""))
    else:
        out("\nno fallback reasons found (no [scene][2d] line and no stale input: the log has no scene-mode-2 census)")
    return run_blockers, bad


# --- the check: fixtures in both formats (the census line as fallback_census.h prints it; the inputs line as inputs_core.h CensusLine prints it)
FIX_WINDOW = ("[wwe2k14] [scene][2d] frames 600, 2d-whole 100, fallback 500 (by reason: scene inputs_missing_bit4_light_states:420,hold:30; render no_frame_image:5; hole 0x82BC3758:20; refused n/a; "
              "unknown_site 0x82BC4404:15; remnant_extra 0x82BB10C0:10; pass_unknown n/a; blend_code n/a); rows n/a, pair n/a; sum ok; none uncounted; orphan notes 0, pending dropped 0, window 600; in scene main_menu")
FIX_EXIT = ("[scene][2d] exit: frames 40, 2d-whole 40, fallback 0 (by reason: scene -; render -; hole -; refused n/a; unknown_site -; remnant_extra -; pass_unknown n/a; blend_code n/a); rows n/a, pair n/a; sum ok; none uncounted; "
            "orphan notes 0, pending dropped 0, window 40, pending unadopted 0; in scene match; run totals: frames 640, 2d-whole 140, fallback 500 (by reason: scene inputs_missing_bit4_light_states:420,hold:30; "
            "render no_frame_image:5; hole 0x82BC3758:20; refused n/a; unknown_site 0x82BC4404:15; remnant_extra 0x82BB10C0:10; pass_unknown n/a; blend_code n/a)")
FIX_BAD = FIX_WINDOW.replace("sum ok", "SUM MISMATCH (reasons 1 vs fallback 2, whole 3 of 4)").replace("pending dropped 0", "pending dropped 7")
FIX_INPUTS = ("[scene][inputs] moved 2 of 18, post_settings 900/f1200, model_runtime 800/f1199, light_states (open) 0/f0, wrestler_palette (open) 12/f300, circle_points 5/f1100 "
              "[blocks the frame: light_states (lane X), wrestler_palette (lane Y)] [held by --wwe2k14_rdr_scene_hold: none]")
# S103 L2aF: L2a's own draw-side counter line (live.cpp Draw2DOurs) is [scene][2d-draw]: NOT a census line (the triage must neither count nor misread it); a wired census line has refused / blend_code reasons, rows and pair
FIX_DRAW = "[scene][2d-draw] ours frames 600, 2d-whole 100 (rows drawn 900, stream row draws left out 900); stream 2D kept: not taken 0, holes 3, no origins 0, scan refused 497 (stream_target 0, interleaved 400, count 97, blend_pair 0), refused 0 (-)"
FIX_WIRED = ("[scene][2d] frames 600, 2d-whole 100, fallback 500 (by reason: scene -; render -; hole 0x82BC3758:20; refused interleaved:400,count:70; unknown_site -; remnant_extra 0x82BB10C0:10; pass_unknown n/a; blend_code 7:0); "
             "rows 900, pair (0 880, -1 20, +1 0, other 0); sum ok; none uncounted; orphan notes 0, pending dropped 0, window 600; in scene match")
FIX_SEQ_A = "[wwe2k14][scene] sequence 3 -> 4 (main menu) at frame 100, 1.0 s after start"
FIX_SEQ_B = "[wwe2k14][scene] sequence 4 -> 9 (Match) at frame 700, 11.0 s after start"


def selftest():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    w = parse_2d(FIX_WINDOW)
    check(w and w["frames"] == 600 and w["fallback"] == 500 and not w["exit"] and w["scene_tag"] == "main_menu" and not w["problems"], "window line parse")
    check(w and w["groups"]["scene"] == {"inputs_missing_bit4_light_states": 420, "hold": 30} and w["groups"]["refused"] == {} and w["groups"]["hole"] == {"0x82BC3758": 20}, "window groups (n/a = no entries)")
    e = parse_2d(FIX_EXIT)
    check(e and e["exit"] and e["frames"] == 40 and e["whole"] == 40 and e["fallback"] == 0 and e["totals"] == (640, 140, 500) and e["scene_tag"] == "match", "exit line: the window part, not the totals")
    b = parse_2d(FIX_BAD)
    check(b and "SUM MISMATCH" in b["problems"] and "pending dropped 7" in b["problems"], "problem flags")
    lines = [FIX_SEQ_A, FIX_INPUTS, FIX_WINDOW, FIX_SEQ_B, FIX_EXIT]
    scenes, n2d, rp = read(lines)
    check(n2d == 2 and not rp, "windows add up to the run totals")
    mm = scenes.get("main menu")
    check(mm is not None and sum(u[0] for u in mm.win) == 600, "tagged window under its scene")
    ma = scenes.get("Match")
    check(ma is not None and sum(u[0] for u in ma.win) == 40, "exit window under the last scene")
    check(mm is not None and mm.bits and mm.bits["light_states"] == (0, 0) and mm.bits["wrestler_palette"] == (12, 300), "inputs line with (open) parsed")
    check(mm is not None and mm.blocks == ["light_states (lane X)", "wrestler_palette (lane Y)"], "blocks segment parsed")
    scenes, n2d, rp = read([FIX_SEQ_A, FIX_WINDOW, FIX_EXIT.replace("frames 640", "frames 999")])
    check(bool(rp), "run totals mismatch flagged")
    scenes, n2d, rp = read([FIX_SEQ_A, FIX_DRAW, FIX_WIRED])
    check(n2d == 1 and not rp, "the [scene][2d-draw] line is not a census line (not counted, not misread)")
    wr = parse_2d(FIX_WIRED)
    check(wr and wr["groups"]["refused"] == {"interleaved": 400, "count": 70} and wr["frames"] == 600 and not wr["problems"], "a wired census line: refused reasons parsed")
    lines_out = []
    scenes, n2d, rp = read([FIX_SEQ_A, FIX_BAD])
    _, bad = report(scenes, n2d, rp, out=lines_out.append)
    check(bad and any("INCOMPLETE" in x for x in lines_out), "an incomplete window is reported INCOMPLETE")
    for f in fails:
        print("SELFTEST FAIL: " + f)
    print("fallback_triage selftest: " + ("FAILED" if fails else "all checks passed"))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?")
    ap.add_argument("--top", type=int, default=6)
    ap.add_argument("--stale", type=int, default=120)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.log:
        ap.error("a LOG file (or --selftest)")
    with open(a.log, "r", encoding="utf-8", errors="replace") as f:
        scenes, n2d, rp = read(f)
    print(f"fallback_triage: {a.log}   ([scene][2d] lines: {n2d})")
    report(scenes, n2d, rp, a.top, a.stale)
    return 0


if __name__ == "__main__":
    sys.exit(main())

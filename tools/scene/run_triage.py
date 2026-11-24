#!/usr/bin/env python3
"""Run triage with a what-if (S103 X1; docs\\renderer\\S103_AUDIT_REVIEW.md 2 row 2): everything one mode-2 run says about WHY frames were not ours, per scene, from the run log alone.

  py -I tools\\scene\\run_triage.py LOG [--exe EXE] [--route ROUTE.txt] [--stale FRAMES] [--top N]

Per scene (route `scene=NAME (frame N)` markers split the in-game sequence; `[wwe2k14][scene] sequence A -> B` lines otherwise):
  - the OURS share (frames the scene hook decided OURS / frames decided) and holes / doubles / latched (the last `[scene][frame]` census), validation errors from the mirror lines;
  - each REQUIRED input bit: stamps in the scene, rate (stamps / frames), the last frame it was stamped (the `[scene][inputs] ... name N/fL` census, cumulative: L is the last stamp of the whole run
    up to that window), and a status: ok / partial / STOPPED at fL / never stamped;
  - the missing-bits histogram (`[scene][missing]` lines, src\\renderer_game\\scene_frame.h missing_hist): the SET of missing required bits per frame, summed over the scene, and the WHAT-IF
    "frames freed if bit X (or X + Y) were stamped" = frames whose whole set lies inside {X} (or {X,Y}); a log that predates the histogram says so (the per-bit readings above are then all there is);
  - the crowd input's own withheld reasons (`crowd_snapshot withheld .. stamped a/b [reasons]`), the 2D fallback census (`[scene][2d]`) when the log has it;
  - the route's `# intent:` line against the real flags the log shows, honest-mode check (no force_inputs / unrequire / hold), and the exe against HEAD (an exe older than the last src commit = STALE).
Windows are 600 frames; a window that straddles a scene boundary goes to the scene holding its middle frame. Reads old logs (the legacy census lines) as well as new ones. Exit code 0 always: a reader, not a check."""
import argparse
import datetime
import os
import re
import subprocess
import sys
from collections import OrderedDict, defaultdict

NAMES = ["post_settings", "model_runtime", "sphere_occ", "wrestler_palette", "light_states", "arena_worlds", "ring_rows", "caster_entries", "circle_points",
         "plane_constants", "crowd_snapshot", "particles", "camera", "material_params", "screens", "stage_pools", "arena_models", "stage_clock"]
SEQ = re.compile(r"\[wwe2k14\]\[scene\] sequence (\d+) -> (\d+) \(([^)]*)\) at frame (\d+)")
ROUTE_SCENE = re.compile(r"\[wwe2k14\]\[input\] line \d+: scene=(\S+).*?\(frame (\d+)\)")
LIVE = re.compile(r"\[scene\]\[live\] frames (\d+)-(\d+):")
INPUTS = re.compile(r"\[scene\]\[inputs\] moved \d+ of \d+, (.*?)(?: \[|$)")
BITRX = re.compile(r"\b([a-z_0-9]+) (\d+)/f(\d+)")
CHANGES = re.compile(r"\[scene\]\[changes\] records ours (\d+) walked (\d+)")
FRAME = re.compile(r"\[scene\]\[frame\] mode (\d+), ours (\d+), walks (\d+), holes (\d+), doubles (\d+), latched (\d+), inputs req (0x[0-9a-fA-F]+)")
STREAMS = re.compile(r"streams skip\+draw (\d+), skip-only (\d+), draw-only (\d+), neither (\d+)(?:, partial (\d+), evicted (\d+), never_rendered (\d+), ours_unpaired (-?\d+)(?:, render_skipped (\d+))?)?")
THREADS = re.compile(r"frames asked on swap-thread/other-thread \(phase\):(.*)$")   # S103 CLK2: the [scene][skips] thread split per hook, with the hook's phase
THREADHOOK = re.compile(r"(\w+) (\d+)/(\d+)\((walk|model)\)")
UNPAIRED_TOLERANCE = 8   # decisions OURS whose stream is still in the render queue when the line prints   # S103 CLK2: the stream census on the [scene][frame] line
MISSING = re.compile(r"\[scene\]\[missing\] window (\d+): hook frames (\d+)-(\d+), decided (\d+), ours (\d+), inputs_missing (\d+), other (\d+); sets (.*?)(?: \((sum ok|SUM MISMATCH)\))?\s*$")
SETRX = re.compile(r"\{([\d,]*)\}:(\d+)")
CROWD = re.compile(r"crowd_snapshot withheld (\d+) stamped (\d+)/(\d+) \[([^\]]*)\]")
D2 = re.compile(r"\[scene\]\[2d\] (exit: )?frames (\d+), 2d-whole (\d+), fallback (\d+)")
VALID = re.compile(r"validation errors (\d+)")
MODIFIED = re.compile(r"\[wwe2k14\] modified: (\w+) = '(.*?)' \(source")
HOLDMASK = re.compile(r"L1C: hold mask (0x[0-9a-fA-F]+)")


def tsplit(line):
    return line.split("] ", 4)[-1] if line.startswith("[20") else line


def parse(path):
    d = dict(seqs=[], routes=[], live=[], missing=[], frames=[], changes=[], crowd=[], d2=[], validation=0, flags=OrderedDict(), holdmask=None, script=None, req=None)
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if "[scene]" not in ln and "modified:" not in ln and "scene=" not in ln:
                continue
            m = SEQ.search(ln)
            if m:
                d["seqs"].append((int(m.group(4)), m.group(3).split(":")[0].strip()))
                continue
            m = ROUTE_SCENE.search(ln)
            if m:
                d["routes"].append((int(m.group(2)), m.group(1)))
                continue
            m = MODIFIED.search(ln)
            if m:
                d["flags"][m.group(1)] = m.group(2)
                if m.group(1) == "wwe2k14_input_script":
                    d["script"] = m.group(2)
                continue
            m = HOLDMASK.search(ln)
            if m:
                d["holdmask"] = int(m.group(1), 16)
            m = LIVE.search(ln)
            if m:
                w = dict(a=int(m.group(1)), b=int(m.group(2)), bits={})
                mi = INPUTS.search(ln)
                if mi:
                    for b in BITRX.finditer(mi.group(1)):
                        if b.group(1) in NAMES:
                            w["bits"][b.group(1)] = (int(b.group(2)), int(b.group(3)))
                mc = CROWD.search(ln)
                if mc:
                    w["crowd"] = (int(mc.group(1)), int(mc.group(2)), int(mc.group(3)), mc.group(4))
                d["live"].append(w)
                continue
            mt = THREADS.search(ln) if "[scene][skips]" in ln else None
            if mt:
                d["threads"] = [(h.group(1), int(h.group(2)), int(h.group(3)), h.group(4)) for h in THREADHOOK.finditer(mt.group(1))]
            m = MISSING.search(ln)
            if m:
                sets = {}
                for s in SETRX.finditer(m.group(8)):
                    mask = 0
                    for t in s.group(1).split(","):
                        if t:
                            mask |= 1 << int(t)
                    sets[mask] = sets.get(mask, 0) + int(s.group(2))
                d["missing"].append(dict(n=int(m.group(1)), a=int(m.group(2)), b=int(m.group(3)), decided=int(m.group(4)), ours=int(m.group(5)), missing=int(m.group(6)),
                                         other=int(m.group(7)), sets=sets, ok=m.group(9) != "SUM MISMATCH"))
                continue
            m = CHANGES.search(ln)
            if m:
                d["changes"].append((int(m.group(1)), int(m.group(2))))
            m = FRAME.search(ln)
            if m:
                d["frames"].append(tuple(int(x, 16) if i == 6 else int(x) for i, x in enumerate(m.groups())))
                d["req"] = int(m.group(7), 16)
                ms = STREAMS.search(ln)
                d["streams"] = tuple(int(x) if x is not None else 0 for x in ms.groups()) if ms else None
            m = D2.search(ln)
            if m:
                d["d2"].append((bool(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))))
            for v in VALID.finditer(ln):
                d["validation"] += int(v.group(1))
    return d


def scenes_of(d, last_frame):
    """-> [(name, start, end)]: sequence segments, the in-game one split by the route's scene= markers."""
    cuts = sorted(set(d["seqs"]))
    segs = []
    for i, (f, name) in enumerate(cuts):
        segs.append([name, f, cuts[i + 1][0] if i + 1 < len(cuts) else last_frame + 1])
    if not segs:
        segs = [["run", 0, last_frame + 1]]
    out = []
    routes = sorted(d["routes"])
    for name, a, b in segs:
        inside = [(f, r) for f, r in routes if a <= f < b]
        if not inside:
            out.append((name, a, b))
            continue
        if inside[0][0] > a:
            out.append((name, a, inside[0][0]))
        for i, (f, r) in enumerate(inside):
            out.append((f"{name} / {r}", f, inside[i + 1][0] if i + 1 < len(inside) else b))
    return out


def which(scenes, frame):
    for i, (n, a, b) in enumerate(scenes):
        if a <= frame < b:
            return i
    return len(scenes) - 1


def truthy(v):
    return str(v).lower() in ("1", "true", "on", "yes")


def intent_terms(route):
    try:
        for ln in open(route, encoding="utf-8", errors="replace"):
            m = re.match(r"^#\s*intent:\s*(.+)$", ln.strip())
            if m:
                return [t.strip() for t in m.group(1).split(",") if t.strip()]
    except OSError:
        pass
    return None


def check_term(term, flags):
    m = re.match(r"^([\w.]+)\s+(\S+)$", term)
    if not m:
        return "?", "not understood"
    name, val = m.groups()
    cands = [f"wwe2k14_rdr_{name}", f"wwe2k14_{name}", name] + sorted(k for k in flags if k.endswith("_" + name))
    for c in cands:
        if c in flags:
            got = flags[c]
            v = val.lower()
            ok = truthy(got) if v == "on" else (not truthy(got)) if v == "off" else got == val
            return ("ok" if ok else "MISMATCH"), f"--{c}={got}"
    if name == "owner":
        return "ok?", "no flag line; the owner is a cvar default (the [owner] lines of the log say it)"
    return "not logged", "the flag is not among the log's 'modified:' lines (a default, or never set)"


def exe_vs_head(exe, root):
    out = []
    if not exe or not os.path.isfile(exe):
        return ["exe: not found (" + str(exe) + "); pass --exe"]
    mt = os.path.getmtime(exe)
    out.append("exe: %s built %s" % (exe, datetime.datetime.fromtimestamp(mt).strftime("%Y-%m-%d %H:%M")))
    try:
        head = subprocess.run(["git", "-C", root, "log", "-1", "--format=%h %ct %s"], capture_output=True, text=True, timeout=30).stdout.strip().split(" ", 2)
        src = subprocess.run(["git", "-C", root, "log", "-1", "--format=%h %ct %s", "--", "src", "config", "CMakeLists.txt"], capture_output=True, text=True, timeout=30).stdout.strip().split(" ", 2)
        out.append("HEAD: %s %s  (%s)" % (head[0], datetime.datetime.fromtimestamp(int(head[1])).strftime("%Y-%m-%d %H:%M"), head[2][:70]))
        if len(src) == 3:
            age = (int(src[1]) - mt) / 60.0
            out.append("last src/config commit: %s %s  (%s) -> exe is %s" % (src[0], datetime.datetime.fromtimestamp(int(src[1])).strftime("%Y-%m-%d %H:%M"), src[2][:60],
                       "STALE (older than that commit by %.0f min)" % age if age > 1 else "newer than it (ok)"))
    except (OSError, ValueError, IndexError, subprocess.SubprocessError) as e:
        out.append("git: could not read HEAD (%s)" % e)
    return out


def bitlist(mask):
    return [b for b in range(32) if (mask >> b) & 1]


def nm(b):
    return "bit %d %s" % (b, NAMES[b] if b < len(NAMES) else "?")


def selftest():
    """Synthetic log: 900 frames missing {3,4}, 300 missing {10}, 50 missing {3,4,10}, 40 ours. Expect: stamping bit 4 alone frees 0 (every {4}-containing set also holds 3 or 10), bits 3+4 free 900, 3+4+10 would free all
    but 10 alone frees 300; a set counted under the wrong key (twin: {3,4} filed under bit 3) must give a different answer."""
    import io
    import tempfile
    from contextlib import redirect_stdout
    def log(sets_line):
        return "\n".join([
            "[2026-11-17 01:00:00.000] [info] [core] [t1] [wwe2k14][scene] sequence 0 -> 9 (in-game: CGameMainSequence) at frame 1, 1.0 s after start",
            "[2026-11-17 01:00:01.000] [info] [core] [t1] [scene][live] frames 0-1289: arena objects 0 [scene][inputs] moved 3 of 18, wrestler_palette 3/f12, light_states 3/f5, crowd_snapshot 700/f1200 [blocks the frame: x]",
            "[scene][frame] mode 2, ours 40, walks 1250, holes 0, doubles 0, latched 0, inputs req 0x00000418, streams skip+draw 1, skip-only 39, draw-only 0, neither 1250, partial 0, evicted 0, never_rendered 2, ours_unpaired 40",
            "[2026-11-17 01:00:02.000] [info] [core] [t1] [scene][skips] model 5/9 ring 3/7 (skipped/walked); frames asked on swap-thread/other-thread (phase): model 0/100(model) ring 100/0(walk) plane 10/90(walk)",
            "[scene][missing] window 1: hook frames 0-1289, decided 1290, ours 40, inputs_missing 1250, other 0; sets " + sets_line + " (sum ok)", ""])
    good = "{3,4}:900 {10}:300 {3,4,10}:50"
    twin = "{3}:900 {10}:300 {3}:50"
    res = []
    for name, sets in (("good", good), ("twin", twin)):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False, encoding="utf-8") as f:
            f.write(log(sets))
        buf = io.StringIO()
        sys.argv = ["run_triage", f.name]
        with redirect_stdout(buf):
            main()
        os.unlink(f.name)
        res.append(buf.getvalue())
    ok = True
    def chk(c, why):
        nonlocal ok
        print(("ok   " if c else "FAIL ") + why)
        ok = ok and c
    g, t = res
    chk("if bit 10 crowd_snapshot were stamped: 300 frames freed" in g, "good: bit 10 alone frees 300")
    chk("if bit 3 + bit 4 were stamped: 950 frames freed" not in g and "if bit 3 + bit 4 were stamped: 900 frames freed" in g, "good: bits 3 + 4 free exactly the 900 {3,4} frames")
    chk("if bit 3 wrestler_palette were stamped" not in g and "if bit 4 light_states were stamped" not in g, "good: neither bit alone frees anything")
    chk("{3,4}: 900" in g, "good: the set line is printed")
    chk("if bit 3 wrestler_palette were stamped: 950 frames freed" in t and "{3,4}: 900" not in t, "twin: the wrong key is visible (bit 3 alone would 'free' 950)")
    chk("FLAG: 39 HOLE stream(s)" in g, "good: a skip-only stream census is flagged as a HOLE (run 38: 81 skipped, 1 drawn)")
    chk("FLAG: ours_unpaired 40" in g, "good: decisions OURS no stream accounts for are flagged (ours_unpaired)")
    chk("note: hook plane asked 10 of 100 times on the swap thread" in g, "good: a hook asking on the swap thread is noted (lane ST: every hook runs on the update thread)")
    chk("FLAG: hook" not in g, "good: the thread split no longer flags a phase (lane ST, run 39)")
    print("selftest:", "all ok" if ok else "FAILED")
    return 0 if ok else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--exe")
    ap.add_argument("--route")
    ap.add_argument("--stale", type=int, default=120)
    ap.add_argument("--top", type=int, default=5)
    a = ap.parse_args()
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    d = parse(a.log)
    live = d["live"]
    last_frame = max([w["b"] for w in live] + [m["b"] for m in d["missing"]] + [f for f, _ in d["seqs"]] + [0])
    scenes = scenes_of(d, last_frame)
    req = d["req"] or 0
    print("run triage: %s" % a.log)
    print("windows: %d [scene][live], %d [scene][missing] (%s)" % (len(live), len(d["missing"]),
          "histogram present" if d["missing"] else "LEGACY log: no missing-bits histogram, per-bit readings only"))

    # ---- the run, against its intent
    print("\n== the run ==")
    for ln in exe_vs_head(a.exe or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.log))), "wwe2k14.exe"), root):
        print("  " + ln)
    fl = d["flags"]
    dev = [k for k in fl if re.search(r"rdr_scene_(force_inputs|unrequire|hold)", k) and fl[k] not in ("", "0", "false", "none")]
    hm = d["holdmask"]
    honest = not dev and not hm
    print("  mode-2 flags: scene=%s sentinel=%s%s; honest mode 2 (no force / unrequire / hold): %s%s" % (
        fl.get("wwe2k14_rdr_scene", "?"), fl.get("wwe2k14_rdr_scene_sentinel", "0"), ("; hold mask 0x%x" % hm) if hm is not None else "",
        "YES" if honest else "NO", "" if honest else " (" + ", ".join("%s=%s" % (k, fl[k]) for k in dev) + (", hold mask 0x%x" % hm if hm else "") + ")"))
    route = a.route
    if not route and d["script"]:
        route = d["script"] if os.path.isfile(d["script"]) else os.path.join(root, "tools", "input_scripts", "scenes", os.path.basename(d["script"].replace("\\", "/")))
    terms = intent_terms(route) if route and os.path.isfile(route) else None
    if terms is None:
        print("  route intent: %s" % ("no '# intent:' line in " + route if route and os.path.isfile(route) else "route file not found (%s)" % route))
    else:
        for t in terms:
            st, why = check_term(t, fl)
            print("  intent '%s': %s  (%s)" % (t, st, why))

    # ---- per scene
    sc = [dict(name=n, a=s, b=e, frames=0, ours=0, decided=0, bits=defaultdict(lambda: [0, 0, 0]), sets=defaultdict(int), win=0, crowd=None, nsets_missing=0) for n, s, e in scenes]
    prev = {}
    for i, w in enumerate(live):
        s = sc[which(scenes, (w["a"] + w["b"]) // 2)]
        s["win"] += 1
        size = w["b"] - w["a"] + (1 if w["a"] else 0)
        s["frames"] += size
        for name, (n, last) in w["bits"].items():
            pn = prev.get(name, 0)
            bb = s["bits"][name]
            bb[0] += max(0, n - pn)
            bb[1] = max(bb[1], last)
            bb[2] = n
            prev[name] = n
        if "crowd" in w:
            s["crowd"] = w["crowd"]
    use_missing = bool(d["missing"])
    for m in d["missing"]:
        s = sc[which(scenes, (m["a"] + m["b"]) // 2)]
        s["decided"] += m["decided"]
        s["ours"] += m["ours"]
        s["nsets_missing"] += m["missing"]
        for k, v in m["sets"].items():
            s["sets"][k] += v
    if not use_missing:   # legacy: [scene][changes] records ours/walked, cumulative, one per window in the live windows' order
        po = pw = 0
        for i, (o, w_) in enumerate(d["changes"][: len(live)]):
            win = live[i]
            s = sc[which(scenes, (win["a"] + win["b"]) // 2)]
            s["ours"] += max(0, o - po)
            s["decided"] += max(0, o - po) + max(0, w_ - pw)
            po, pw = o, w_
    fin = d["frames"][-1] if d["frames"] else None
    top_blocker = None
    for s in sc:
        if s["frames"] == 0 and s["decided"] == 0:
            continue
        print("\n== scene: %s  (frames %d-%d, %d windows) ==" % (s["name"], s["a"], s["b"] - 1, s["win"]))
        dec = s["decided"] or s["frames"]
        print("  OURS %d of %d decided (%.1f%%)%s" % (s["ours"], dec, 100.0 * s["ours"] / dec if dec else 0.0, "" if use_missing else "  [legacy: [scene][changes] records]"))
        if s["frames"]:
            print("  required input bits (stamps / frames in the scene):")
        stopped = []
        for b in bitlist(req):
            name = NAMES[b] if b < len(NAMES) else "bit%d" % b
            bb = s["bits"].get(name)
            if bb is None or s["frames"] == 0:
                continue
            n, last, total = bb
            rate = n / float(s["frames"])
            newest = max(v[1] for v in s["bits"].values())
            if total == 0:
                status = "never stamped"
            elif n == 0:
                status = "STOPPED (last f%d)" % last
            elif last < newest - a.stale and rate < 0.98:
                status = "STOPPED at f%d" % last
            elif rate >= 0.98:
                status = "ok"
            else:
                status = "partial"
            if status != "ok":
                stopped.append((b, status))
            print("    %-22s stamped %6d / %6d  rate %5.1f%%  last f%-6d  %s" % (nm(b), n, s["frames"], 100 * rate, last, status))
        if s["crowd"]:
            wh, st, tot, why = s["crowd"]
            nz = ", ".join(x for x in why.split() if False) or ", ".join("%s %s" % (a_, b_) for a_, b_ in zip(why.split()[0::2], why.split()[1::2]) if b_ != "0")
            print("  crowd_snapshot (run so far): stamped %d / %d, withheld %d; reasons with counts: %s%s" % (st, tot, wh, nz or "none",
                  "  <- the crowd stamp is not given on frames the model does not cover: sign / foam-hand frames (R14 counts)" if "unmodelled" in nz else ""))
        if use_missing and s["nsets_missing"]:
            tot = s["nsets_missing"]
            print("  frames missing inputs: %d.  sets (frames):" % tot)
            for mask, n in sorted(s["sets"].items(), key=lambda kv: -kv[1])[: a.top]:
                print("    {%s}: %d" % (",".join(str(x) for x in bitlist(mask)), n))
            bits = sorted({b for k in s["sets"] for b in bitlist(k)})
            sing = sorted(((sum(v for k, v in s["sets"].items() if k & ~(1 << b) == 0), b) for b in bits), reverse=True)
            pairs = []
            for i in range(len(bits)):
                for j in range(i + 1, len(bits)):
                    msk = (1 << bits[i]) | (1 << bits[j])
                    pairs.append((sum(v for k, v in s["sets"].items() if k & ~msk == 0), bits[i], bits[j]))
            pairs.sort(reverse=True)
            print("  what-if (frames freed = frames whose whole missing set is stamped by the fix; of %d not-OURS-for-inputs frames):" % tot)
            for fr, b in sing[: a.top]:
                if fr:
                    print("    if %s were stamped: %d frames freed (%.1f%% of the scene's decided)" % (nm(b), fr, 100.0 * fr / dec if dec else 0))
            for fr, b1, b2 in pairs[: a.top]:
                if fr and fr > (max([x[0] for x in sing if x[1] in (b1, b2)] or [0])):
                    print("    if bit %d + bit %d were stamped: %d frames freed (%.1f%%)" % (b1, b2, fr, 100.0 * fr / dec if dec else 0))
            if sing and sing[0][0] and s["ours"] < dec and (top_blocker is None or (dec - s["ours"]) > top_blocker[3]):
                top_blocker = (s["name"], "stamping %s alone frees %d frames" % (nm(sing[0][1]), sing[0][0]), None, dec - s["ours"])
        elif not use_missing and stopped:
            print("  (legacy log: no sets, so no 'freed if'; the stopped / never / partial bits above are the blockers)")
        if not use_missing and stopped and s["ours"] < dec and (top_blocker is None or (dec - s["ours"]) > top_blocker[3]):
            bad = sorted(((0 if st.startswith("STOP") else 1), (s["bits"][NAMES[b]][0] / float(s["frames"]) if (s["frames"] and NAMES[b] in s["bits"]) else 0.0), b, st) for b, st in stopped if b < len(NAMES))
            top_blocker = (s["name"], "; ".join("%s %s (rate %.0f%%)" % (nm(b), st, 100 * r) for _, r, b, st in bad[:4]) or "none stamped at all", None, dec - s["ours"])
    # ---- run totals
    print("\n== holes / doubles / latch / validation (the last [scene][frame] census) ==")
    if fin:
        print("  mode %d: ours %d, walks %d, holes %d, doubles %d, latched %d, required mask 0x%08x" % (fin[0], fin[1], fin[2], fin[3], fin[4], fin[5], fin[6]))
    else:
        print("  no [scene][frame] census line in the log")
    st = d.get("streams")
    if st:
        print("  stream census (hooks skipped the stream / DrawLive drew it): both %d, skip-only %d, draw-only %d, neither %d, partial %d, evicted %d, never_rendered %d, ours_unpaired %d, render_skipped %d" % st)
        if st[8]:
            print("  note: %d decided stream(s) were render-skipped (the whole frame message dropped by the rate / queue skip): nothing of them is presented, no hole; run 38 saw only ~120 of ~15000 frames in SceneOurs" % st[8])
        if st[1]:
            print("  FLAG: %d HOLE stream(s): the hooks skipped the game's walks but DrawLive drew nothing for the stream (a decision / draw clock mismatch, run 38); holes in the line above do not count these" % st[1])
        if st[2]:
            print("  FLAG: %d DOUBLE stream(s): DrawLive drew a stream whose game walks were NOT skipped (our picture over the game's)" % st[2])
        if st[4]:
            print("  FLAG: %d PARTIAL stream(s): one decision with hooks that skipped AND hooks that walked (a hook bypassing the board's memo, or a wrong phase)" % st[4])
        if st[6]:
            print("  FLAG: %d never-rendered stream slot(s): hooks skipped a decision no stream of the render side ever read (render-side skip, dropped message, NoRecord)" % st[6])
        if st[7] > UNPAIRED_TOLERANCE:
            print("  FLAG: ours_unpaired %d (> %d in flight): decisions OURS that no skip+draw / skip-only stream accounts for (a render path that never reached the stream census; run 38's missing ~80)" % (st[7], UNPAIRED_TOLERANCE))
        if st[5]:
            print("  note: %d stream(s) evicted (the render side was more than 64 frames behind): unknown, not counted as holes or doubles" % st[5])
    elif fin:
        print("  no stream census on the [scene][frame] line (an older build)")
    # Lane ST (docs\research\s103\ST_swap_thread.md, run 39): walk-time hooks (ring, ring2, plane) run on the UPDATE thread like the model
    # hooks, so the swap / other split cannot name a hook's phase. Only a hook asking on the swap thread at all is unexpected: note it.
    for name, sw, ot, ph in d.get("threads", []):
        if sw:
            print("  note: hook %s asked %d of %d times on the swap thread (every hook is expected on the update thread; lane ST)" % (name, sw, sw + ot))
    print("  validation errors summed over the mirror lines: %d" % d["validation"])
    if d["d2"]:
        w = [x for x in d["d2"] if not x[0]]
        e = [x for x in d["d2"] if x[0]]
        t = e[-1] if e else (w[-1] if w else None)
        if t:
            print("  [scene][2d] census: frames %d, 2d-whole %d, fallback %d (%s line)" % (t[1], t[2], t[3], "exit" if e else "last window"))
    print("\nTOP BLOCKER (the scene with the most non-OURS frames): %s" % (("scene '%s': %s" % (top_blocker[0], top_blocker[1])) if top_blocker else "none found"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

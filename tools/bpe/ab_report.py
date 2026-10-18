"""Session 11: native BPE A/B report from timed runs (frames_NNN.csv + the run log's file-open anchors).

Usage: py tools/bpe/ab_report.py RUN [RUN ...]
Each run is labelled by its own log: "[native_bpe] active: mode=native" -> on, otherwise off (verify runs are
labelled verify and kept apart). Scenes, from the file opens (like tools/bench/fps60.py): first entrance = 343.bik2
.. the second titantron (103.bik2 or 324.bik2); second entrance = the second titantron .. M3.PAC - 80 or the last
frame; transition = the 300 frames before the second titantron opens + 120 after it (the end-of-first-entrance hitch
of docs/research/decoder_native.md is there: run 655 frame 5360, 65 frames before 324.bik2). Per run and scene:
frames, fps, frames > 20 / 25 / 35 ms, worst frame (ms @ frame). Then per arm: mean of fps, of the > 20 ms count and
of the worst frame per scene, and the route's total native / guest decode time from the [native_bpe] totals.
"""
import csv
import re
import statistics
import sys
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
UPDATE_SLOT = "82260EB0"  # CGameMainThread's barrier slot (barrier_probe.h)
RE_DEC = re.compile(r"\[wwe2k14\]\[decode\] frame (\d+) tid (\d+): bpe calls=\d+ out=\d+ in=\d+ ms=([\d.]+) "
                    r"native_ms=[\d.]+ max_call_ms=[\d.]+; zlib calls=\d+ ms=([\d.]+)")
RE_CALL_G = re.compile(r"\[wwe2k14\]\[decode\] call bpe frame=(\d+) tid=(\d+) .*? guest_ms=([\d.]+)")
RE_CALL_N = re.compile(r"\[wwe2k14\]\[decode\] call (?:native|verify) frame=(\d+) tid=(\d+) out=\d+ ms=([\d.]+)")


def load(run):
    n = str(run).zfill(3)
    anch, mode, native_ms, preset = {}, "off", None, "1440p"
    for line in open(LOGS / f"wwe2k14_{n}.log", encoding="utf-8", errors="replace"):
        m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
        if m:
            p = m.group(1).lower()
            for k in ("343.bik2", "103.bik2", "324.bik2", "m3.pac"):
                if k in p and k not in anch:
                    anch[k] = int(m.group(2))
        if "[native_bpe] active: mode=" in line:
            mode = "verify" if "mode=verify" in line else "on"
        m = re.search(r"\[native_bpe\] at frame \d+: .*native_ms=([\d.]+)", line)
        if m:
            native_ms = float(m.group(1))
        if "wwe2k14_preset_4k60 = 'true'" in line:
            preset = "4k"
        if "wwe2k14_preset_1440p60 = 'false'" in line:
            preset = "720p"
    frames = {}
    with open(LOGS / f"frames_{n}.csv", newline="") as f:
        for r in csv.DictReader(f):
            frames[int(r["frame"])] = float(r["frame_ms"])
    # Update thread (CGameMainThread = barrier slot 82260EB0, --wwe2k14_barrier_probe): its work per barrier row.
    work, tids = {}, {}
    bpath = LOGS / f"barrier_{n}.csv"
    if bpath.exists():
        with open(bpath, newline="") as f:
            rd = csv.reader(f)
            next(rd, None)
            for row in rd:
                try:
                    fr = int(row[0])
                except (ValueError, IndexError):
                    continue
                for slot in row[5:]:
                    p = slot.split(";")
                    if len(p) == 4 and p[0].upper() == UPDATE_SLOT:
                        try:
                            work[fr] = float(p[3])
                            tids[int(p[1])] = tids.get(int(p[1]), 0) + 1
                        except ValueError:
                            pass
    upd = max(tids, key=tids.get) if tids else None
    # Per-thread per-frame decode lines (--wwe2k14_decode_probe; native runs too since Session 11).
    # The per-frame line is flushed only at the thread's next call (it may never come on a short route), so sum the
    # per-call lines (calls >= wwe2k14_decode_probe_call_ms, 0.25 ms) too and use the frame line where it exists.
    dec, calls = {}, {}
    for line in open(LOGS / f"wwe2k14_{n}.log", encoding="utf-8", errors="replace"):
        if upd is None:
            break
        m = RE_DEC.search(line)
        if m and int(m.group(2)) == upd:
            dec[int(m.group(1))] = (float(m.group(3)), float(m.group(4)))
            continue
        m = RE_CALL_G.search(line) or RE_CALL_N.search(line)
        if m and int(m.group(2)) == upd:
            f = int(m.group(1))
            calls[f] = calls.get(f, 0.0) + float(m.group(3))
    for f, ms in calls.items():
        if f not in dec:
            dec[f] = (ms, 0.0)
    return {"run": int(run), "anch": anch, "mode": mode, "native_ms": native_ms, "frames": frames, "preset": preset,
            "work": work, "upd": upd, "dec": dec}


def hitch_split(d, a, b):
    """The decode hitch = the update thread's biggest BPE row k in the transition window [a, b) (the worst frame
    there can be another stall: run 405's is a 142 ms frame 50 frames later). The calls that ran while g_guest_frame
    was k land in frame k+1's present, as the barrier rows do (decode_report.py), so: frame k+1's time (and k+2's,
    in case it slipped), the update thread's BPE and zlib ms of row k and its barrier work in row k."""
    if d["upd"] is None:
        return None
    rows = [(f, v) for f, v in d["dec"].items() if a <= f < b]
    if not rows:
        return None
    k, (bpe, zl) = max(rows, key=lambda x: x[1][0])
    return {"k": k, "bpe": bpe, "zlib": zl, "work": d["work"].get(k), "ms1": d["frames"].get(k + 1),
            "ms2": d["frames"].get(k + 2)}


def scene_stats(frames, a, b):
    sel = [(f, frames[f]) for f in range(a, b) if f in frames]
    if not sel:
        return None
    ms = [m for _, m in sel]
    wf, wm = max(sel, key=lambda x: x[1])
    return {"n": len(ms), "fps": 1000 * len(ms) / sum(ms), "gt20": sum(m > 20 for m in ms),
            "gt25": sum(m > 25 for m in ms), "gt35": sum(m > 35 for m in ms), "worst": wm, "worst_f": wf}


def scenes(d):
    an, fr = d["anch"], d["frames"]
    second = "103.bik2" if "103.bik2" in an else "324.bik2"
    out = {}
    if "343.bik2" in an and second in an:
        out["first entrance"] = scene_stats(fr, an["343.bik2"], an[second])
        out["transition"] = scene_stats(fr, an[second] - 300, an[second] + 120)
        d["trans"] = (an[second] - 300, an[second] + 120)
    if second in an:
        end = an["m3.pac"] - 80 if "m3.pac" in an else max(fr) + 1
        out["second entrance"] = scene_stats(fr, an[second], end)
    return out


def main():
    runs = [load(r) for r in sys.argv[1:]]
    per = {}
    for d in runs:
        sc = scenes(d)
        print(f"run {d['run']} [{d['mode']}, {d['preset']}] anchors {d['anch']} native_ms={d['native_ms']}")
        for name, s in sc.items():
            if not s:
                continue
            print(f"  {name:16s} {s['n']:5d} frames {s['fps']:6.2f} fps  >20 {s['gt20']:3d} >25 {s['gt25']:3d} "
                  f">35 {s['gt35']:3d}  worst {s['worst']:6.1f} ms @ {s['worst_f']}")
            if name == "transition":
                h = hitch_split(d, *d["trans"])
                s["split"] = h
                if h:
                    w = f"{h['work']:.1f}" if h["work"] is not None else "-"
                    print(f"  {'':16s} decode hitch: frame {h['k'] + 1} {h['ms1']:.1f} ms (next {h['ms2']:.1f}); update "
                          f"thread (tid {d['upd']}) BPE {h['bpe']:.2f} ms, zlib {h['zlib']:.2f} ms, total work {w} ms")
                else:
                    print(f"  {'':16s} decode hitch: no update-thread decode rows (needs --wwe2k14_decode_probe and "
                          f"--wwe2k14_barrier_probe)")
            per.setdefault((d["preset"], d["mode"], name), []).append(s)
    print("\nper arm (mean over runs; worst = mean of each run's worst frame, max in brackets):")
    for (preset, mode, name), ss in sorted(per.items()):
        print(f"  {preset:5s} {mode:6s} {name:16s} runs {len(ss)}  fps {statistics.mean(s['fps'] for s in ss):6.2f}  "
              f">20 {statistics.mean(s['gt20'] for s in ss):5.1f}  >35 {statistics.mean(s['gt35'] for s in ss):4.1f}  "
              f"worst {statistics.mean(s['worst'] for s in ss):6.1f} ms [{max(s['worst'] for s in ss):.1f}]  "
              f"worsts {', '.join(f'{s['worst']:.1f}' for s in ss)}")
        sp = [s["split"] for s in ss if s.get("split")]
        if sp:
            wk = [x["work"] for x in sp if x["work"] is not None]
            print(f"  {'':5s} {'':6s} {'':16s} decode hitch: frame "
                  f"{statistics.mean(x['ms1'] for x in sp):.1f} ms ({', '.join(f'{x['ms1']:.1f}' for x in sp)}); "
                  f"update thread BPE {statistics.mean(x['bpe'] for x in sp):.2f} ms "
                  f"({', '.join(f'{x['bpe']:.1f}' for x in sp)}); total work "
                  + (f"{statistics.mean(wk):.1f} ms ({', '.join(f'{x:.1f}' for x in wk)})" if wk else "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

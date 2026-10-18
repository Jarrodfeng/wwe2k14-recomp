"""Session 10 (Track C): compare short-route runs scene by scene (same measures as fps60.py).

  py tools/bench/route_compare.py ARM=RUN,RUN,... [ARM=RUN,...]     e.g. menu=649,651,653 launch=650,652,654

Scenes, anchored on file opens in each run's log: "brock" = 343.bik2 -> 324.bik2 (Brock Lesnar's entrance), "pan" =
324.bik2 -> +600 frames (the start of Daniel Bryan's entrance, the ramp pan; the short routes end there). Per run and
scene: frames, fps, frames > 20 / 25 / 35 ms, worst frame, GPU ms per frame (sum of gpu_ts submission durations between
the scene's vdswaps, as fps60.py; needs --vulkan_gpu_timestamps=true). Then per arm: mean and range.
"""
from __future__ import annotations

import bisect
import csv
import re
import statistics
import sys

L = r"C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs"


def load(n: int):
    anch = {}
    for line in open(f"{L}/wwe2k14_{n:03d}.log", encoding="utf-8", errors="replace"):
        m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
        if m:
            p = m.group(1).lower()
            for k in ("343.bik2", "324.bik2"):
                if k in p and k not in anch:
                    anch[k] = int(m.group(2))
    frames = {int(r["frame"]): float(r["frame_ms"]) for r in csv.DictReader(open(f"{L}/frames_{n:03d}.csv"))}
    vd, ts = [], []
    try:  # same parsing as fps60.py: t_ns, a / b in hex; gpu_ts b = submission duration
        for r in csv.DictReader(open(f"{L}/timeline_{n:03d}.csv", newline="")):
            try:
                e = r["event"]
                if e == "vdswap":
                    vd.append((int(r["t_ns"]), int(r["a"], 16)))
                elif e == "gpu_ts":
                    ts.append((int(r["t_ns"]), int(r["b"], 16)))
            except Exception:  # noqa: BLE001  (a truncated last line, as fps60.py)
                break
    except FileNotFoundError:
        pass
    return anch, frames, sorted(vd), sorted(ts)


def gpu_ms(vd, ts, a, b):
    if not vd or not ts:
        return float("nan")
    t0 = next((t for t, f in vd if f >= a), None)
    t1 = next((t for t, f in vd if f >= b), None)
    if t0 is None or t1 is None:
        return float("nan")
    tt = [t for t, _ in ts]
    i, j = bisect.bisect_left(tt, t0), bisect.bisect_left(tt, t1)
    return sum(d for _, d in ts[i:j]) / 1000 / max(1, b - a)


def scene(frames, vd, ts, a, b):
    ms = [frames[f] for f in range(a, b) if f in frames]
    if not ms:
        return None
    return {"frames": len(ms), "fps": 1000 * len(ms) / sum(ms), "gt20": sum(m > 20 for m in ms),
            "gt25": sum(m > 25 for m in ms), "gt35": sum(m > 35 for m in ms), "worst": max(ms),
            "gpu": gpu_ms(vd, ts, a, b)}


def main() -> None:
    arms = {}
    for arg in sys.argv[1:]:
        name, runs = arg.split("=", 1)
        arms[name] = [int(x) for x in runs.split(",") if x]
    keys = ("fps", "gt20", "gt25", "gt35", "worst", "gpu")
    print("| arm | run | scene | frames | fps | >20 ms | >25 ms | >35 ms | worst ms | GPU ms/frame |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    agg = {}
    for arm, runs in arms.items():
        for n in runs:
            anch, frames, vd, ts = load(n)
            if "343.bik2" not in anch or "324.bik2" not in anch:
                print(f"| {arm} | {n} | missing anchors {anch} | | | | | | | |")
                continue
            for sc, a, b in (("brock", anch["343.bik2"], anch["324.bik2"]),
                             ("pan", anch["324.bik2"], anch["324.bik2"] + 600)):
                s = scene(frames, vd, ts, a, b)
                if not s:
                    continue
                agg.setdefault((arm, sc), []).append(s)
                print(f"| {arm} | {n} | {sc} | {s['frames']} | {s['fps']:.2f} | {s['gt20']} | {s['gt25']} | {s['gt35']} | "
                      f"{s['worst']:.1f} | {s['gpu']:.2f} |")
    print("\n| arm | scene | runs | " + " | ".join(f"{k} mean (min-max)" for k in keys) + " |")
    print("|---|---|---|" + "---|" * len(keys))
    for (arm, sc), ss in sorted(agg.items(), key=lambda x: (x[0][1], x[0][0])):
        cells = []
        for k in keys:
            v = [s[k] for s in ss]
            cells.append(f"{statistics.mean(v):.2f} ({min(v):.2f}-{max(v):.2f})")
        print(f"| {arm} | {sc} | {len(ss)} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()

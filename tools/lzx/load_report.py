"""Session 12: load times from runs of tools\\input_scripts\\short\\load_times.txt.

Reads out\\build\\win-amd64-release\\logs\\wwe2k14_NNN.log (wall-clock stamps on every line) and reports, per run:
  boot    process start (first log line) -> "lt: title data" (SaveData.Dat read)
  menu    "lt: title pressed" -> "lt: main menu" (MNUIHD.PAC)
  launch  "[scenario] ... in-game sequence started" -> "lt: first titantron" (the loading screen before the entrances)
  m_clock "lt: match scene file" -> "lt: match running" (match_clock != 0)
and the LZX decode time inside each segment (sum of "[wwe2k14][lzx] call ... ms=" lines stamped in it; needs
--wwe2k14_lzx_probe=true, game decoder or native). With --off RUNS --on RUNS it also prints the means of each arm.

Usage: py tools\\lzx\\load_report.py 850 851 ...   or   py tools\\lzx\\load_report.py --off 850,852 --on 851,853
"""
import argparse
import datetime as dt
import re
import statistics
import sys
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
STAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
SEGMENTS = [
    ("boot", "START", "lt: title data"),
    ("menu", "lt: title pressed", "lt: main menu"),
    ("launch", "SEQ", "lt: first titantron"),
    ("m_clock", "lt: match scene file", "lt: match running"),
]


def parse(run):
    path = LOGS / f"wwe2k14_{run:03d}.log"
    marks, calls, decoder = {}, [], "?"
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = STAMP.match(line)
            if not m:
                continue
            t = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
            if "START" not in marks:
                marks["START"] = t
            if "[input] line" in line and "lt: " in line:
                label = line.split("lt: ", 1)[1].split(" (frame")[0].strip()
                for _, a, b in SEGMENTS:
                    for key in (a, b):
                        if key.startswith("lt: ") and label.startswith(key[4:]):
                            marks.setdefault(key, t)
                if label.startswith("wrestler") or label.startswith("second"):
                    marks.setdefault("lt: " + label, t)
            elif "in-game sequence started" in line and "[scenario]" in line:
                marks.setdefault("SEQ", t)
            elif "[wwe2k14][lzx] call" in line:
                ms = re.search(r" ms=([0-9.]+)", line)
                if ms:
                    calls.append((t, float(ms.group(1))))
                d = re.search(r"decoder=(\w+)", line)
                if d:
                    decoder = d.group(1)
    out = {"run": run, "decoder": decoder, "lzx_total": sum(ms for _, ms in calls)}
    for name, a, b in SEGMENTS:
        if a in marks and b in marks:
            out[name] = marks[b] - marks[a]
            out[name + "_lzx"] = sum(ms for t, ms in calls if marks[a] <= t <= marks[b])
        else:
            out[name] = None
            out[name + "_lzx"] = None
    return out


def fmt(v, scale=1.0, digits=2):
    return "-" if v is None else f"{v * scale:.{digits}f}"


def table(rows):
    head = "run   decoder  " + "  ".join(f"{n:>8} {n + '_lzx':>12}" for n, _, _ in SEGMENTS) + "  lzx_total_ms"
    print(head)
    for r in rows:
        cells = "  ".join(f"{fmt(r[n]):>8} {fmt(r[n + '_lzx'], 1, 1):>12}" for n, _, _ in SEGMENTS)
        print(f"{r['run']:<5} {r['decoder']:<8} {cells}  {r['lzx_total']:.1f}")


def mean_row(rows, label):
    m = {"run": label, "decoder": rows[0]["decoder"] if rows else "?",
         "lzx_total": statistics.mean(r["lzx_total"] for r in rows)}
    for n, _, _ in SEGMENTS:
        for k in (n, n + "_lzx"):
            vals = [r[k] for r in rows if r[k] is not None]
            m[k] = statistics.mean(vals) if vals else None
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="*", type=int)
    ap.add_argument("--off", default="")
    ap.add_argument("--on", default="")
    args = ap.parse_args()
    off = [int(x) for x in args.off.split(",") if x]
    on = [int(x) for x in args.on.split(",") if x]
    runs = args.runs + off + on
    if not runs:
        ap.error("give run numbers")
    rows = {r: parse(r) for r in runs}
    print("seconds per segment; *_lzx = LZX decode ms inside it")
    table([rows[r] for r in runs])
    if off and on:
        mo, mn = mean_row([rows[r] for r in off], "OFF"), mean_row([rows[r] for r in on], "ON")
        print("\nmeans")
        table([mo, mn])
        print("\ndelta ON - OFF (s): " + "  ".join(
            f"{n} {fmt(None if mo[n] is None or mn[n] is None else mn[n] - mo[n], 1, 3)}" for n, _, _ in SEGMENTS))


if __name__ == "__main__":
    sys.exit(main())

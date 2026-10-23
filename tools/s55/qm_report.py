"""Session 55 part 2: one line per Quick Match finish run (tools/input_scripts/short/s55_qm_finish.txt).

    py tools\\s55\\qm_report.py RUN [RUN ...]      (run numbers or log paths)

Per run: match type, wrestlers, how it ended (finish type G+472 0x8376ACCC, winner / loser bytes 0x8376E3EC), the
match clock at the decision, real seconds from the match start to the decision, and the run's fast-forward speed
(mean of the [wwe2k14][frames] fps lines while the match ran, plus the whole run's frames per wall second).
"""
import re
import sys
from datetime import datetime
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
FPS = re.compile(r"\[wwe2k14\]\[frames\] t=\d+ s, frames (\d+)-(\d+): ([\d.]+) fps")
STATE = re.compile(r"line \d+: state (\S+) = (\S+) .*\(frame (\d+)\)")
FLAG = re.compile(r"modified: (wwe2k14_quick_type|wwe2k14_launch_participants|wwe2k14_step_hz|"
                  r"gpu_native_truth_draw_height) = '([^']*)'")


def ts(line):
    m = TS.match(line)
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f") if m else None


def report(arg, logs=LOGS):
    path = Path(arg) if arg.endswith(".log") else Path(logs) / f"wwe2k14_{int(arg):03d}.log"
    if not path.exists():
        return f"{arg}: no log at {path}"
    flags, states = {}, {}
    t_first = t_run = t_over = t_last = None
    run_fps, all_fps = [], []
    last_frame = 0
    crash = ""
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        t = ts(line)
        if t:
            t_first = t_first or t
            t_last = t
        for k, v in FLAG.findall(line):
            flags.setdefault(k, v.strip("'\","))
        if "qm_finish: match running" in line:
            t_run = t
        elif "qm_finish: wait over" in line:
            t_over = t
        m = STATE.search(line)
        if m:
            states[m.group(1)] = int(m.group(2), 0) if m.group(2).lstrip("-").replace("0x", "").isalnum() else m.group(2)
        m = FPS.search(line)
        if m:
            last_frame = int(m.group(2))
            all_fps.append(float(m.group(3)))
            if t_run and not t_over:
                run_fps.append(float(m.group(3)))
        if "EXCEPTION" in line or "crash" in line.lower() and "[triage]" in line:
            crash = " CRASH"
    qtype = flags.get("wwe2k14_quick_type", "0")  # (left at its default 0: not logged as modified)
    ids = flags.get("wwe2k14_launch_participants", "?")
    fin = states.get("0x8376ACCC")
    clock = states.get("match_clock")
    who = states.get("0x8376E3EC")
    if t_run is None:
        verdict = "NO MATCH (never ran)"
    elif t_over is None:
        verdict = "UNFINISHED (run ended before the wait was over)"
    elif fin:
        verdict = f"ENDED finish {fin}"
    else:
        verdict = "STUCK (no decision in 144,000 frames)"
    secs = (t_over - t_run).total_seconds() if t_run and t_over else None
    wall = (t_last - t_first).total_seconds() if t_first and t_last else 0
    clock_s = f"clock {clock} ({clock / 60:.0f} s at 60)" if isinstance(clock, int) else "clock ?"
    mfps = sum(run_fps) / len(run_fps) if run_fps else 0
    return (f"{path.stem.split('_')[-1]}: type {qtype} ids {ids} step {flags.get('wwe2k14_step_hz', '-')} "
            f"h {flags.get('gpu_native_truth_draw_height', '-')}: {verdict}; {clock_s}; winner/loser "
            f"{who if who is None else hex(who)}; {secs if secs is None else round(secs)} s real; match fps "
            f"{mfps:.0f} (whole run {last_frame / wall if wall else 0:.0f} frames/s over {wall:.0f} s){crash}")


if __name__ == "__main__":
    args = sys.argv[1:]
    logs = LOGS
    if args[:1] == ["--logs"]:  # another exe's logs folder (tools\s55\qm_sweep.ps1 -Exe)
        logs, args = args[1], args[2:]
    for a in args:
        print(report(a, logs))

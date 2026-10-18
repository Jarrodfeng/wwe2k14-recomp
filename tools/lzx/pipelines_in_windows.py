"""Session 15 (A2): how many Vulkan pipelines / shaders are created inside each load-turbo window of a run.

Counts, per window ("[wwe2k14][turbo] NAME on at frame F" -> "... off at frame G"), the log lines
  "Creating graphics pipeline state with VS ..."   (pipeline_cache.cpp, one per new pipeline, any thread)
  "Translating ..." / "shader" translation lines    (new guest shaders translated to SPIR-V)
plus the same counts before the first window (startup: pipelines re-created from the shader storage) and outside
any window. Pipelines are created on background threads (async_shader_compilation): this gives counts and when they
start, not how long each one takes.

Usage: py tools\\lzx\\pipelines_in_windows.py 1089 1090 ...
"""
import datetime as dt
import re
import sys
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
STAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
ON = re.compile(r"\[wwe2k14\]\[turbo\] (\w+) on at frame (\d+)")
OFF = re.compile(r"\[wwe2k14\]\[turbo\] (\w+) off at frame (\d+) \((.*?);")


def scan(run):
    path = LOGS / f"wwe2k14_{run:03d}.log"
    windows, cur, outside, startup = [], None, {"pipe": 0, "shader": 0}, {"pipe": 0, "shader": 0}
    seen_window = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = STAMP.match(line)
            if not m:
                continue
            t = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
            on = ON.search(line)
            if on:
                cur = {"name": on.group(1), "t0": t, "f0": int(on.group(2)), "pipe": 0, "shader": 0,
                       "first_pipe": None, "last_pipe": None}
                seen_window = True
                continue
            off = OFF.search(line)
            if off and cur:
                cur.update(t1=t, f1=int(off.group(2)), why=off.group(3))
                windows.append(cur)
                cur = None
                continue
            kind = None
            if "Creating graphics pipeline state" in line:
                kind = "pipe"
            elif "[gpu]" in line and re.search(r"Translat|translat", line):
                kind = "shader"
            if not kind:
                continue
            if cur:
                cur[kind] += 1
                if kind == "pipe":
                    cur["first_pipe"] = cur["first_pipe"] or t
                    cur["last_pipe"] = t
            elif not seen_window:
                startup[kind] += 1
            else:
                outside[kind] += 1
    return windows, startup, outside


def main():
    for arg in sys.argv[1:]:
        run = int(arg)
        windows, startup, outside = scan(run)
        print(f"run {run}: before the first window {startup['pipe']} pipelines, {startup['shader']} shader lines; "
              f"outside windows later {outside['pipe']} pipelines, {outside['shader']} shader lines")
        for w in windows:
            ms = (w.get("t1", w["t0"]) - w["t0"]) * 1000
            span = ""
            if w["first_pipe"]:
                span = (f", first at +{(w['first_pipe'] - w['t0']) * 1000:.0f} ms, last at "
                        f"+{(w['last_pipe'] - w['t0']) * 1000:.0f} ms")
            print(f"  {w['name']:6s} frames {w['f0']}-{w.get('f1', '?')} ({ms:.0f} ms, {w.get('why', '?')}): "
                  f"{w['pipe']} pipelines, {w['shader']} shader lines{span}")


if __name__ == "__main__":
    main()

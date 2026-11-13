"""memline.py - compare two runs' memory / thread / boot numbers from their game logs (lane 249).

Answers: "what does run B save (or cost) against run A?" - used for --wwe2k14_owner_guest_gpu_only vs the SDK GPU (docs\\renderer\\LEAVE_THE_COMMAND_STREAM.md,
section "Measuring what it saves").

  py tools\\bench\\memline.py LOG_A LOG_B            compare table (median / max per field, last value at exit, boot milestones)
  py tools\\bench\\memline.py LOG_A LOG_B --board    one line for the board / summary
  py tools\\bench\\memline.py LOG                    one run's table
  py tools\\bench\\memline.py --selftest             synthetic logs + the real logs under out\\ (0 game runs)

A LOG is a path or a run number (out\\build\\win-amd64-release\\logs\\wwe2k14_NNNN.log, then logs\\ next to this repo).

Reads (src/perf_mem.h, lane 249):
  [wwe2k14][perf][mem] t=30.0 s frame=1800 ws_mb=.. private_mb=.. peak_ws_mb=.. gpu_local_mb=.. gpu_nonlocal_mb=.. threads=.. gpu_threads=..
  [wwe2k14][perf][mem] exit t=.. ...                        (the quit flush's line)
  [wwe2k14][perf][boot] first guest frame (frame 1) at t=1.12 s since process start
Boot milestones also come from older logs (log timestamps): first line = process start, "[frames] first guest frame", the scene lines
"sequence 0 -> 3" (title) and "sequence 3 -> 75" (main menu). So runs from before lane 249 still compare on boot, and say "no mem lines" for memory.
"""
import os
import re
import statistics
import sys

MEM_FIELDS = ["ws_mb", "private_mb", "peak_ws_mb", "gpu_local_mb", "gpu_nonlocal_mb", "threads", "gpu_threads"]
LINE_TS = re.compile(r"^\[(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\]")
MEM_RE = re.compile(r"\[perf\]\[mem\](?P<exit> exit)? t=(?P<t>[\d.]+) s frame=(?P<frame>\d+)(?P<rest>.*)$")
KV_RE = re.compile(r"(\w+)=(-?[\d.]+)")
BOOT_RE = re.compile(r"\[perf\]\[boot\] first guest frame \(frame 1\) at t=([\d.]+) s")
FPS_RE = re.compile(r"\[frames\] t=\d+ s, frames \d+-\d+: ([\d.]+) fps")


def seconds(line):
    m = LINE_TS.match(line)
    if not m:
        return None
    y, mo, d, h, mi, s, ms = map(int, m.groups())
    # day of month is enough for a run (a run that crosses midnight still gets the right difference; small numbers keep the ms exact)
    return ((d * 24 + h) * 3600 + mi * 60 + s) + ms / 1000.0


def resolve(arg):
    if os.path.isfile(arg):
        return arg
    if arg.isdigit():
        here = os.path.dirname(os.path.abspath(__file__))
        roots = [os.path.join(here, "..", "..", "out", "build", "win-amd64-release", "logs"),
                 "C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs",
                 os.path.join(here, "..", "..", "logs")]
        for r in roots:
            p = os.path.join(r, "wwe2k14_%s.log" % arg)
            if os.path.isfile(p):
                return p
    raise SystemExit("memline: no such log: %s" % arg)


def parse(path):
    r = {"path": path, "mem": [], "exit": None, "boot_t": None, "t0": None, "first_frame": None, "title": None, "menu": None,
         "gpu_only": False, "gpu_names": None, "fps": []}
    with open(path, "r", errors="replace") as f:
        for line in f:
            ts = seconds(line)
            if ts is not None and r["t0"] is None:
                r["t0"] = ts
            if "[perf][mem]" in line:
                m = MEM_RE.search(line)
                if m:
                    row = {"t": float(m.group("t")), "frame": int(m.group("frame"))}
                    for k, v in KV_RE.findall(m.group("rest")):
                        row[k] = float(v)
                    if m.group("exit"):
                        r["exit"] = row
                    else:
                        r["mem"].append(row)
                elif "gpu thread names:" in line:
                    r["gpu_names"] = line.split("gpu thread names:")[1].strip()
                continue
            m = BOOT_RE.search(line)
            if m:
                r["boot_t"] = float(m.group(1))
            if ts is None:
                continue
            if r["first_frame"] is None and "[frames] first guest frame" in line:
                r["first_frame"] = ts - r["t0"]
            elif r["title"] is None and "[scene] sequence 0 -> 3 " in line:
                r["title"] = ts - r["t0"]
            elif r["menu"] is None and "[scene] sequence 3 -> 75 " in line:
                r["menu"] = ts - r["t0"]
            elif "guest_gpu_only on:" in line:
                r["gpu_only"] = True
            else:
                m = FPS_RE.search(line)
                if m:
                    r["fps"].append(float(m.group(1)))
    return r


def stat(run, field):
    vals = [m[field] for m in run["mem"] if field in m]
    if run["exit"] and field in run["exit"]:
        vals_exit = run["exit"][field]
    else:
        vals_exit = None
    if not vals:
        return None
    return {"median": statistics.median(vals), "max": max(vals), "exit": vals_exit, "n": len(vals)}


def fmt(v, d=0):
    return "-" if v is None else ("%.*f" % (d, v))


def summarise(run):
    out = {"mem_lines": len(run["mem"]), "fields": {f: stat(run, f) for f in MEM_FIELDS}}
    out["first_frame"] = run["boot_t"] if run["boot_t"] is not None else run["first_frame"]
    out["title"], out["menu"] = run["title"], run["menu"]
    out["fps_median"] = statistics.median(run["fps"]) if run["fps"] else None
    out["gpu_only"] = run["gpu_only"]
    return out


def delta(a, b):
    if a is None or b is None:
        return "-"
    d = b - a
    pct = " (%+.0f%%)" % (100.0 * d / a) if a else ""
    return "%+.0f%s" % (d, pct)


def table(runs):
    sums = [summarise(r) for r in runs]
    names = ["A" if len(runs) > 1 else "run", "B"]
    lines = []
    for i, r in enumerate(runs):
        lines.append("%s = %s (%s; %d mem lines%s)" % (names[i], os.path.basename(r["path"]), "guest_gpu_only" if r["gpu_only"] else "SDK GPU",
                                                       len(r["mem"]), "" if r["mem"] else ": no mem lines"))
        if r["gpu_names"]:
            lines.append("   gpu threads: %s" % r["gpu_names"])
    if not any(s["mem_lines"] for s in sums):
        lines.append("no mem lines in any log (runs from before lane 249, or --wwe2k14_perf_mem=false): memory not compared")
    else:
        hdr = "%-16s" % "field" + "".join("%-30s" % ("%s median / max / exit" % names[i]) for i in range(len(runs)))
        if len(runs) > 1:
            hdr += "B-A median"
        lines.append(hdr)
        for f in MEM_FIELDS:
            row = "%-16s" % f
            meds = []
            for s in sums:
                st = s["fields"][f]
                if st:
                    row += "%-30s" % ("%s / %s / %s" % (fmt(st["median"]), fmt(st["max"]), fmt(st["exit"])))
                    meds.append(st["median"])
                else:
                    row += "%-30s" % "-"
                    meds.append(None)
            if len(runs) > 1:
                row += delta(meds[0], meds[1])
            lines.append(row)
    lines.append("boot (s)         " + "".join("%-46s" % ("%s first frame %s, title %s, menu %s" % (names[i], fmt(s["first_frame"], 2), fmt(s["title"], 1), fmt(s["menu"], 1)))
                                               for i, s in enumerate(sums)))
    lines.append("median fps       " + "".join("%-30s" % fmt(s["fps_median"], 1) for s in sums))
    return "\n".join(lines)


def board(a, b):
    sa, sb = summarise(a), summarise(b)

    def d(f):
        x, y = sa["fields"][f], sb["fields"][f]
        return delta(x["median"], y["median"]) if x and y else "n/a"

    def g(s):
        st = s["fields"]["gpu_threads"]
        return fmt(st["median"]) if st else "?"

    return ("memline %s -> %s: ws_mb %s, private_mb %s, gpu_local_mb %s, threads %s, gpu_threads %s->%s, first frame %ss->%ss, menu %ss->%ss"
            % (os.path.basename(a["path"]), os.path.basename(b["path"]), d("ws_mb"), d("private_mb"), d("gpu_local_mb"), d("threads"), g(sa), g(sb),
               fmt(sa["first_frame"], 2), fmt(sb["first_frame"], 2), fmt(sa["menu"], 1), fmt(sb["menu"], 1)))


# ---------------------------------------------------------------- selftest

def _synth(path, ws, priv, gpu_local, threads, gpu_threads, first, menu, gpu_only):
    ts = lambda s: "[2026-11-13 12:00:%06.3f] [info] [core] [t1] " % s
    L = []
    L.append(ts(0.0) + "[wwe2k14] run log number: 1")
    if gpu_only:
        L.append(ts(0.1) + "[owner][gpu-only] guest_gpu_only on: no SDK graphics provider")
    L.append(ts(first) + "[wwe2k14][frames] first guest frame")
    L.append(ts(first) + "[wwe2k14][perf][boot] first guest frame (frame 1) at t=%.2f s since process start" % first)
    L.append(ts(first) + "[wwe2k14][scene] sequence 0 -> 3 (startup / title: CStartupCtrlSequence) at frame 1, 1.1 s after start (x)")
    L.append(ts(menu) + "[wwe2k14][scene] sequence 3 -> 75 (main menu) at frame 99, 4.0 s after start (x)")
    L.append(ts(menu) + "[wwe2k14][perf][mem] gpu thread names: %s" % ("(none)" if not gpu_threads else "GPU Commands, GPU VSync, GPU Submit"))
    for i in range(5):
        L.append(ts(10 + i) + "[wwe2k14][perf][mem] t=%.1f s frame=%d ws_mb=%d private_mb=%d peak_ws_mb=%d gpu_local_mb=%d gpu_nonlocal_mb=50 threads=%d gpu_threads=%d"
                 % (10.0 * (i + 1), 600 * (i + 1), ws + i, priv + i, ws + 100, gpu_local + i, threads, gpu_threads))
        L.append(ts(10 + i) + "[wwe2k14][frames] t=%d s, frames 1-600: 60.0 fps, avg 16.7 ms" % (5 * (i + 1)))
    L.append(ts(20) + "[wwe2k14][perf][mem] exit t=51.6 s frame=3050 ws_mb=%d private_mb=%d" % (ws + 9, priv + 9))
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def selftest():
    import tempfile
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok = ok and cond

    d = tempfile.mkdtemp(prefix="memline_")
    a, b = os.path.join(d, "a.log"), os.path.join(d, "b.log")
    _synth(a, 3000, 3600, 1800, 80, 3, 1.5, 6.0, False)
    _synth(b, 2400, 3000, 600, 70, 0, 1.2, 5.0, True)
    ra, rb = parse(a), parse(b)
    sa, sb = summarise(ra), summarise(rb)
    check(sa["mem_lines"] == 5 and sa["fields"]["ws_mb"]["median"] == 3002, "A: 5 mem lines, ws median 3002")
    check(sa["fields"]["ws_mb"]["exit"] == 3009, "A: exit line read apart (ws exit 3009)")
    check(sb["fields"]["gpu_threads"]["median"] == 0 and sa["fields"]["gpu_threads"]["median"] == 3, "gpu_threads 3 -> 0")
    check(sa["first_frame"] == 1.5 and sb["first_frame"] == 1.2, "boot t from the [perf][boot] line")
    check(abs(sa["menu"] - 6.0) < 1e-6 and abs(sb["title"] - 1.2) < 1e-6, "menu / title from log timestamps")
    check(sb["gpu_only"] and not sa["gpu_only"], "guest_gpu_only detected")
    check(delta(3002, 2402) == "-600 (-20%)", "delta text")
    out = table([ra, rb])
    check("gpu_local_mb" in out and "B-A median" in out, "table prints")
    bd = board(ra, rb)
    check("ws_mb -600 (-20%)" in bd and "gpu_threads 3->0" in bd, "board line: " + bd)
    print(out)
    # a real log with no mem lines (any under out\): must say so, never crash
    here = os.path.dirname(os.path.abspath(__file__))
    real = None
    for root in [os.path.join(here, "..", "..", "out", "build", "win-amd64-release", "logs"), "C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs"]:
        if os.path.isdir(root):
            for n in ("wwe2k14_7149.log", "wwe2k14_7163.log"):
                if os.path.isfile(os.path.join(root, n)):
                    real = root
                    break
        if real:
            break
    if real:
        p149, p163 = os.path.join(real, "wwe2k14_7149.log"), os.path.join(real, "wwe2k14_7163.log")
        if os.path.isfile(p149) and os.path.isfile(p163):
            r149, r163 = parse(p149), parse(p163)
            t = table([r149, r163])
            check("no mem lines" in t, "real logs 7149 / 7163 (before lane 249): 'no mem lines', no crash")
            # known fact (today, LEAVE_THE_COMMAND_STREAM.md): 7149 ran the SDK GPU, 7163 was guest_gpu_only
            check(not r149["gpu_only"] and r163["gpu_only"], "re-found: 7149 = SDK GPU, 7163 = guest_gpu_only")
            check(r163["first_frame"] is not None and r163["menu"] is not None, "boot milestones come out of an older log (7163 first frame %.2f s, menu %.1f s)"
                  % (r163["first_frame"] or -1, r163["menu"] or -1))
            print(t)
        else:
            print("skip real-log checks: 7149 / 7163 not both present")
    else:
        print("skip real-log checks: no logs directory")
    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    paths = [a for a in argv if not a.startswith("--")]
    if not paths or len(paths) > 2:
        print(__doc__)
        return 2
    runs = [parse(resolve(p)) for p in paths]
    if "--board" in argv and len(runs) == 2:
        print(board(*runs))
    else:
        print(table(runs))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

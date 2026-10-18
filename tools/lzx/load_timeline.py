"""Session 15 (sub-second loads): the pac loader's requests inside a load-turbo window, from a --wwe2k14_load_probe run.

For the window (default: the first "ingame" window), one row per request: when it was enqueued and done (ms from the
window start, and guest frames), its queue wait (enq -> start of the previous done), its size, callback time. Then
where the window's wall time went on the loader's side: idle gaps with nothing queued (= the game had not asked
yet), time with requests in flight, callbacks, and the time from the last done to the window's end.

Usage: py tools\\lzx\\load_timeline.py RUN [--window ingame|match]
"""
import argparse
import re
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
US = re.compile(r"us=(\d+) frame=(\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=int)
    ap.add_argument("--window", default="ingame")
    args = ap.parse_args()
    enq, done, w0, w1, f0, f1 = {}, [], None, None, None, None
    lines = open(LOGS / f"wwe2k14_{args.run:03d}.log", encoding="utf-8", errors="replace").read().splitlines()
    # Window bounds in guest frames from the turbo lines, then in probe microseconds from the nearest probe lines.
    for ln in lines:
        m = re.search(rf"\[turbo\] {args.window} on at frame (\d+)", ln)
        if m and f0 is None:
            f0 = int(m.group(1))
        m = re.search(rf"\[turbo\] {args.window} off at frame (\d+)", ln)
        if m and f0 is not None and f1 is None:
            f1 = int(m.group(1))
    reqs = []
    for ln in lines:
        if "[wwe2k14][load] enq" in ln or "[wwe2k14][load] done" in ln:
            m = US.search(ln)
            us, fr = int(m.group(1)), int(m.group(2))
            path = re.search(r"path=(\S+)", ln).group(1)
            reader = re.search(r"reader=(\S+)", ln).group(1)
            if w0 is None and fr >= f0:
                w0 = us
            if fr <= f1:
                w1 = us
            if "] enq" in ln:
                enq[(reader, path)] = (us, fr)
            else:
                size = int(re.search(r"size=(\d+)", ln).group(1))
                cb = int(re.search(r"cb_us=(\d+)", ln).group(1))
                e = enq.pop((reader, path), (None, None))
                if f0 <= fr <= f1:
                    reqs.append((e[0], e[1], us, fr, size, cb, path))
    print(f"run {args.run}: window '{args.window}' frames {f0}-{f1}; {len(reqs)} requests done inside it")
    print("  enq_ms  done_ms  frames(enq->done)    size KB  cb ms  path")
    for e_us, e_fr, d_us, d_fr, size, cb, path in reqs:
        es = f"{(e_us - w0) / 1000:7.1f}" if e_us else "   pre "
        ef = f"{d_fr - e_fr:4d}" if e_fr else "   ?"
        print(f"  {es} {(d_us - w0) / 1000:8.1f}  {ef}          {size / 1024:9.0f} {cb / 1000:6.1f}  {path}")
    if reqs:
        last = reqs[-1][2]
        print(f"  last done at {(last - w0) / 1000:.1f} ms; callbacks {sum(r[5] for r in reqs) / 1000:.1f} ms in all")


if __name__ == "__main__":
    main()

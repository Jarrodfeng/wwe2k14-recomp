r"""Session 14 (S2): compare a CineCam path recording run with its replay run, frame by frame, from the run logs
(--wwe2k14_cinecam_path_log=true in both).

    py tools\cinecam\path_compare.py REC_RUN PLAY_RUN

Reads out\build\win-amd64-release\logs\wwe2k14_NNN.log. Prints:
  - the view written each anchor-relative frame (matrix rows + FOV) in both runs: frames in both, frames that match
    within 1e-3, the largest difference;
  - entrance timing: each slot's step changes (0 / 5 / 10 / -1) as anchor-relative frames in both runs, and the
    difference (how well the game's entrance lines up across runs when timed from the anchor).
"""
import re
import sys
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
VIEW = re.compile(r"\[path\] (rec|play) rel=(-?\d+) frame=(\d+) m=\[([^\]]*)\] \[([^\]]*)\] \[([^\]]*)\] \[([^\]]*)\] "
                  r"fov=([-\d.]+)")
ANCHOR = re.compile(r"\[path\] anchor \S+ at frame (\d+)")
OPEN = re.compile(r"NtCreateFile '([^']*(?:titantron|ENTRANCE|EVTMOV)[^']*)' open .*frame=(\d+)")
STEP = re.compile(r"\[path\] step slot (\d) (-?\d+) -> (-?\d+) frame=(\d+) rel=(-?\d+)")


def read(run):
    views, steps, opens, anchor = {}, [], [], None
    for line in (LOGS / f"wwe2k14_{int(run):03d}.log").read_text(encoding="utf-8", errors="replace").splitlines():
        m = VIEW.search(line)
        if m:
            rel = int(m.group(2))
            vals = [float(x) for g in m.groups()[3:7] for x in g.split()] + [float(m.group(8))]
            views.setdefault(rel, vals)  # first view of each frame
            continue
        a = ANCHOR.search(line)
        if a and anchor is None:
            anchor = int(a.group(1))
        o = OPEN.search(line)
        if o:
            opens.append((o.group(1).rsplit("\\", 1)[-1], int(o.group(2))))
        s = STEP.search(line)
        if s and int(s.group(5)) > -10**12:
            steps.append((int(s.group(1)), int(s.group(2)), int(s.group(3)), int(s.group(5))))
    rel_opens = []
    if anchor is not None:
        seen = set()
        for name, fr in opens:
            if (name, fr) not in seen:
                seen.add((name, fr))
                rel_opens.append((name, fr - anchor))
    return views, steps, rel_opens


def main():
    rec, play = sys.argv[1], sys.argv[2]
    rv, rs, ro = read(rec)
    pv, ps, po = read(play)
    common = sorted(set(rv) & set(pv))
    worst, worst_rel, match = 0.0, None, 0
    for rel in common:
        d = max(abs(a - b) for a, b in zip(rv[rel], pv[rel]))
        if d <= 1e-3:
            match += 1
        if d > worst:
            worst, worst_rel = d, rel
    print(f"views: record {len(rv)} frames (rel {min(rv)}..{max(rv)}), replay {len(pv)} frames "
          f"(rel {min(pv) if pv else '-'}..{max(pv) if pv else '-'}); in both {len(common)}; "
          f"equal within 1e-3: {match}; largest difference {worst:.6g} at rel {worst_rel}")
    print("entrance steps (slot: from -> to at anchor-relative frame; record / replay / difference):")
    for key in sorted({(s[0], s[1], s[2]) for s in rs} | {(s[0], s[1], s[2]) for s in ps}):
        a = [s[3] for s in rs if s[:3] == key]
        b = [s[3] for s in ps if s[:3] == key]
        for i in range(max(len(a), len(b))):
            x = a[i] if i < len(a) else None
            y = b[i] if i < len(b) else None
            diff = (y - x) if x is not None and y is not None else None
            print(f"  slot {key[0]}: {key[1]:>2} -> {key[2]:>2}   {x}  /  {y}   diff {diff}")
    opens_table(ro, po)


def opens_table(ro, po):
    print("file opens after the anchor (name: record / replay anchor-relative frame, difference):")
    for i in range(max(len(ro), len(po))):
        a = ro[i] if i < len(ro) else None
        b = po[i] if i < len(po) else None
        if (a and a[1] < 0) and (b and b[1] < 0):
            continue
        same = a and b and a[0] == b[0]
        print(f"  {a[0] if a else '-':<16} {a[1] if a else '-':>6} / {b[1] if b else '-':>6}"
              f"{'   diff ' + str(b[1] - a[1]) if same else ('   (different file: ' + b[0] + ')' if b else '')}")


if __name__ == "__main__":
    main()

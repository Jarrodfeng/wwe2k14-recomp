"""Session 95: per-scene power from tools\\surface\\power_log.ps1's power.csv and the run log's scene marks.

Scenes come from the route's "log scene=NAME" lines in the step's run log (wwe2k14_NNN.log, same Surface clock as
power.csv's time column) with the guest frame number each mark carries; a scene runs from its mark to the next mark
(the last one to "bench: done" or the log's end is dropped unless a later mark closes it). Each power row is the
mean over the second ending at its time stamp; rows whose second crosses a mark are left out.
Prints, per scene: seconds, frames, fps, mean watts PKG / cores (PP0) / GPU (PP1) / DRAM, mJ per frame
(= watts * 1000 / fps), CPU utility %, CPU MHz, 3D-engine %, hottest thermal zone (C), and the logger's own CPU (ms/s).

RAPL unit: the Energy Meter 'Power' counter is milliwatts (confirmed 2026-11-07 on the Surface: idle PKG 1277); --div turns it
into watts. mJ per frame uses the package's cumulative Energy counter (picowatt-hours) over the scene when power.csv has
it, else the mean of the Power samples. A warning prints when PKG is outside 0.3-40 W.

  py -I tools\\surface\\power_read.py STEP_FOLDER [--csv power.csv] [--log wwe2k14_NNN.log] [--div 1000]
  py -I tools\\surface\\power_read.py POWER_CSV --log RUN_LOG
"""
import argparse
import csv
import re
import sys
from pathlib import Path

MARK = re.compile(r'\[\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)\].*\[wwe2k14\]\[input\] line \d+: '
                  r'(scene=[^()\n]+?|bench: done)(?: \((?!frame)[^)]*\))?(?: \(frame (\d+)\))?\s*$')


def secs(h, m, s):
    return int(h) * 3600 + int(m) * 60 + float(s)


def day_unwrap(t, ref):
    while t < ref - 43200:
        t += 86400
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--csv')
    ap.add_argument('--log')
    ap.add_argument('--div', type=float, default=1000.0)
    a = ap.parse_args()
    p = Path(a.path)
    if p.is_dir():
        pc = Path(a.csv) if a.csv else p / 'power.csv'
        if a.log:
            lg = Path(a.log)
        else:
            logs = sorted(p.glob('wwe2k14_*.log'))
            lg = logs[-1] if logs else None
    else:
        pc, lg = p, (Path(a.log) if a.log else None)
    rows = [r for r in csv.DictReader(open(pc, encoding='utf-8')) if r.get('time') and r['time'] != 'time']
    if not rows:
        sys.exit('power.csv has no rows')
    marks = []
    if lg and lg.exists():
        for line in open(lg, encoding='utf-8', errors='replace'):
            m = MARK.search(line)
            if m:
                marks.append((secs(*m.group(1, 2, 3)), m.group(4).replace('scene=', '').strip(),
                              int(m.group(5)) if m.group(5) else None))
    else:
        print('no run log: whole file as one scene "all"')
    pts = []
    base = None
    for r in rows:
        h, mi, s = r['time'].split(':')
        t = secs(h, mi, s)
        base = t if base is None else base
        pts.append((day_unwrap(t, base), r))
    keys = rows[0].keys()
    tz = [k for k in keys if k.startswith('tz_temp_')]

    def num(r, k):
        try:
            return float(r[k])
        except (KeyError, ValueError, TypeError):
            return None

    def mean(rs, k, div=1.0):
        v = [num(r, k) for r in rs]
        v = [x for x in v if x is not None]
        return sum(v) / len(v) / div if v else None

    def f(x, fmt='{:6.2f}'):
        return fmt.format(x) if x is not None else '     -'

    scenes = []
    if marks:
        marks = [(day_unwrap(t, base), n, fr) for t, n, fr in marks]
        for i, (t, n, fr) in enumerate(marks[:-1]):
            if n == 'bench: done':
                continue
            t2, _, fr2 = marks[i + 1]
            scenes.append((n, t, t2, (fr2 - fr) if fr is not None and fr2 is not None else None))
    else:
        scenes.append(('all', pts[0][0] - 1, pts[-1][0], None))
    print(f'{pc}: {len(rows)} samples; RAPL divisor {a.div:g}')
    print(f'{"scene":14s} {"s":>5s} {"frames":>6s} {"fps":>6s} {"PKG W":>6s} {"cores":>6s} {"GPU W":>6s} {"DRAM":>6s} '
          f'{"mJ/frm":>7s} {"CPU%":>5s} {"MHz":>5s} {"3D%":>5s} {"maxC":>5s} {"self":>5s}')
    for n, t1, t2, fr in scenes:
        rs = [r for t, r in pts if t - 1.0 >= t1 and t <= t2]   # whole seconds inside the scene
        if not rs:
            continue
        secs_n = len(rs)
        fps = None
        if fr is not None and t2 > t1:
            fps = fr / (t2 - t1)
        pkg = mean(rs, 'rapl_pkg', a.div)
        if pkg is not None and not 0.3 <= pkg <= 40:
            print(f'  WARNING {n}: PKG {pkg:.2f} W after /{a.div:g} is implausible: check the unit (--div)')
        # exact package power from the cumulative Energy counter (picowatt-hours; 1 pWh = 3.6e-9 J) over the rows' span
        e = [(t, num(r, 'rapl_pkg_pwh')) for t, r in pts if t - 1.0 >= t1 and t <= t2]
        e = [(t, v) for t, v in e if v is not None]
        pkg_e = None
        if len(e) >= 2 and e[-1][0] > e[0][0]:
            pkg_e = (e[-1][1] - e[0][1]) * 3.6e-9 / (e[-1][0] - e[0][0])
        base_w = pkg_e if pkg_e is not None else pkg
        mj = base_w * 1000.0 / fps if base_w is not None and fps else None
        if pkg_e is not None and pkg is not None and abs(pkg_e - pkg) > 0.15 * max(pkg, 0.1):
            print(f'  note {n}: PKG from Energy {pkg_e:.2f} W vs mean of Power samples {pkg:.2f} W (mJ/frame uses the Energy figure)')
        temps = [mean(rs, k) for k in tz]
        temps = [x for x in temps if x is not None]
        # thermal zone Temperature is Kelvin on Windows ('Temperature' counter); >200 = Kelvin
        maxc = max(temps) - 273.15 if temps and max(temps) > 200 else (max(temps) if temps else None)
        print(f'{n[:14]:14s} {secs_n:5d} {str(fr) if fr is not None else "-":>6s} {f(fps, "{:6.1f}")} {f(pkg)} '
              f'{f(mean(rs, "rapl_pp0", a.div))} {f(mean(rs, "rapl_pp1", a.div))} {f(mean(rs, "rapl_dram", a.div))} '
              f'{f(mj, "{:7.0f}")} {f(mean(rs, "cpu_utility"), "{:5.0f}")} {f(mean(rs, "cpu_freq_mhz"), "{:5.0f}")} '
              f'{f(mean(rs, "gpu3d_pct"), "{:5.0f}")} {f(maxc, "{:5.1f}")} {f(mean(rs, "self_cpu_ms"), "{:5.0f}")}')


main()

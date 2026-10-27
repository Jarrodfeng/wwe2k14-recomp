"""Session 70: per-segment smoothness of a real-time entrance run (route short\\s70_entrance_rt.txt).

Usage: py tools\\perf\\s70_entrance_report.py LOG [LOG ...] [--worst N] [--csv OUT]
LOG = a run's wwe2k14_NNN.log (its frames_NNN.csv next to it). Segments, by the log's frame anchors:
  boot      frame 0 -> "s70: main menu reached"
  load      menu -> the first entrance tron opens (the Quick Match load, the loading screens)
  ent1      first tron (343.bik2 Brock / 324.bik2 Bryan) -> the second tron
  ent2      second tron -> "s70: match started"
  match     match started -> "s70: end" (600 frames)
Per segment: frames, wall s, frames over 20 / 50 / 100 ms, time lost (sum of each frame's ms over 16.7), worst frame,
NATIVE pipelines made (from the [s58][gpu] lines, --gpu_session_stats_frames=60: 60-frame resolution) and their average
creation ms, draws skipped while compiling. Plus the run's setup lines (workers, stored records, shader list) and the
worst frames with their segment.
"""
import argparse
import bisect
import csv
import os
import re
import sys

FRAME_MS = 1000.0 / 60.0
SEGS = ['boot', 'load', 'ent1', 'ent2', 'match']


def parse(log):
    anch = {}
    trons = []
    s58 = []
    waits = []
    setup = []
    chars = []
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'\[input\] line \d+: s70: (main menu reached|match started|end) \(frame (\d+)\)', line)
            if m:
                anch[m.group(1)] = int(m.group(2))
                continue
            m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
            if m:
                p = m.group(1).lower()
                fr = int(m.group(2))
                for k in ('343.bik2', '324.bik2'):
                    if p.endswith(k) and k not in [t[0] for t in trons]:
                        trons.append((k, fr))
                mc = re.search(r'\\(ch\d+)\.pac$', p)
                if mc and mc.group(1) not in [c[0] for c in chars]:
                    chars.append((mc.group(1), fr))
                continue
            m = re.search(r'\[s58\]\[gpu\] frames (\d+)-(\d+): pipelines made (\d+) \(avg ([\d.]+) ms\) \+ native (\d+) '
                          r'\(avg ([\d.]+) ms\), slowest so far ([\d.]+) / ([\d.]+) ms; draws skipped while compiling '
                          r'(\d+)', line)
            if m:
                s58.append(dict(a=int(m.group(1)), b=int(m.group(2)), stock=int(m.group(3)), stock_ms=float(m.group(4)),
                                native=int(m.group(5)), native_ms=float(m.group(6)), slow_native=float(m.group(8)),
                                skipped=int(m.group(9))))
                continue
            m = re.search(r'\[load_wait\] (on|off).*frame=(\d+)', line)
            if m:
                waits.append((m.group(1), int(m.group(2))))
                continue
            if re.search(r'pipeline worker thread|\[native\]\[storage\] \d+ stored native|\[wwe2k14\]\[shaders\] list|'
                         r'prewarm|stored pipelines queued|Created \d+ graphics pipelines from Vulkan storage', line):
                setup.append(line.split('] ', 4)[-1].strip()[:240])
            m = re.search(r'process affinity mask (0x[0-9A-Fa-f]+)', line)
            if m:
                setup.append('affinity ' + m.group(1))
    return anch, trons, s58, waits, setup, chars


def frames_of(log):
    d = os.path.dirname(log)
    n = re.search(r'wwe2k14_(\d+)\.log$', log).group(1)
    out = {}
    with open(os.path.join(d, f'frames_{n}.csv'), newline='') as f:
        for r in csv.DictReader(f):
            out[int(r['frame'])] = float(r['frame_ms'])
    return out


def gpu_of(log):
    """GPU ms per frame for frames [a, b): the gpu_ts submission durations (--vulkan_gpu_timestamps) between the
    segment's first and last vdswap (as tools\\bench\\fps60.py). None without timestamps."""
    d = os.path.dirname(log)
    n = re.search(r'wwe2k14_(\d+)\.log$', log).group(1)
    vd, ts = [], []
    try:
        with open(os.path.join(d, f'timeline_{n}.csv'), newline='') as f:
            for r in csv.DictReader(f):
                try:
                    if r['event'] == 'vdswap':
                        vd.append((int(r['t_ns']), int(r['a'], 16)))
                    elif r['event'] == 'gpu_ts':
                        ts.append((int(r['t_ns']), int(r['b'], 16)))
                except Exception:
                    break
    except FileNotFoundError:
        return None
    if not vd or not ts:
        return None
    vd.sort()
    ts.sort()
    ts_t = [t for t, _ in ts]
    cum = [0]
    for _, x in ts:
        cum.append(cum[-1] + x)
    vd_f = [f for _, f in vd]

    def t_of(frame):
        i = bisect.bisect_left(vd_f, frame)
        return vd[i][0] if i < len(vd) else None

    def per_frame(a, b):
        t0, t1 = t_of(a), t_of(b)
        if t0 is None or t1 is None:
            return float('nan')
        i, j = bisect.bisect_left(ts_t, t0), bisect.bisect_left(ts_t, t1)
        return (cum[j] - cum[i]) / 1000.0 / max(1, b - a)
    return per_frame


def bounds(anch, trons, last):
    menu = anch.get('main menu reached')
    t = sorted(trons, key=lambda x: x[1])
    ms = anch.get('match started')
    end = anch.get('end', last)
    b = {'boot': (0, menu)}
    b['load'] = (menu, t[0][1] if t else ms)
    b['ent1'] = (t[0][1], t[1][1]) if len(t) >= 2 else (None, None)
    b['ent2'] = (t[1][1], ms) if len(t) >= 2 else (None, None)
    b['match'] = (ms, end)
    return b


def report(log, worst_n, rows):
    anch, trons, s58, waits, setup, chars = parse(log)
    fr = frames_of(log)
    gpu = gpu_of(log)
    last = max(fr) if fr else 0
    b = bounds(anch, trons, last)
    name = os.path.basename(log)
    print(f'== {log}')
    for s in setup:
        print('   ' + s)
    print('   trons: ' + ', '.join(f'{k}@{v}' for k, v in sorted(trons, key=lambda x: x[1])) +
          '; char files: ' + ', '.join(f'{k}@{v}' for k, v in chars[:8]) +
          '; load_wait: ' + ' '.join(f'{o}@{f}' for o, f in waits))
    print(f'   {"seg":6} {"frames":>6} {"wall s":>7} {"fps":>6} {"gpu ms":>6} {">20":>5} {">50":>5} {">100":>5} '
          f'{"lost s":>7} {"worst":>7} {"native":>6} {"avg ms":>6} {"skip":>6}')
    for seg in SEGS:
        a, z = b[seg]
        if a is None or z is None or z <= a:
            print(f'   {seg:6} (missing anchors)')
            continue
        ms = [fr[i] for i in range(a, z) if i in fr]
        lost = sum(max(0.0, x - FRAME_MS) for x in ms) / 1000.0
        nat = [r for r in s58 if a < r['b'] <= z + 59]
        nn = sum(r['native'] for r in nat)
        navg = (sum(r['native'] * r['native_ms'] for r in nat) / nn) if nn else 0.0
        sk = sum(r['skipped'] for r in nat)
        wall = sum(ms) / 1000.0
        fps = len(ms) / wall if wall else 0.0
        gms = gpu(a, z) if gpu else float('nan')
        row = dict(log=name, seg=seg, frames=len(ms), wall=wall, fps=fps, gpu_ms=gms, o20=sum(x > 20 for x in ms),
                   o50=sum(x > 50 for x in ms), o100=sum(x > 100 for x in ms), lost=lost, worst=max(ms) if ms else 0,
                   native=nn, native_avg=navg, skipped=sk)
        rows.append(row)
        print(f'   {seg:6} {row["frames"]:6d} {row["wall"]:7.1f} {fps:6.1f} {gms:6.2f} {row["o20"]:5d} {row["o50"]:5d} '
              f'{row["o100"]:5d} '
              f'{row["lost"]:7.2f} {row["worst"]:7.1f} {nn:6d} {navg:6.1f} {sk:6d}')
    def seg_of(i):
        for seg in SEGS:
            a, z = b[seg]
            if a is not None and z is not None and a <= i < z:
                return seg
        return '-'
    worst = sorted(((v, k) for k, v in fr.items()), reverse=True)[:worst_n]
    print('   worst frames: ' + ', '.join(f'{k}({seg_of(k)}) {v:.0f}' for v, k in worst))


def check(rows, worst_ms, lost_s):
    """--check (tools\\release\\entrance_check.ps1): both entrances and the match within the bars. Returns failures."""
    bad = []
    for seg in ('ent1', 'ent2', 'match'):
        r = [x for x in rows if x['seg'] == seg]
        if not r:
            bad.append(f'{seg}: missing')
            continue
        r = r[-1]
        if r['worst'] > worst_ms:
            bad.append(f'{seg}: worst frame {r["worst"]:.0f} ms > {worst_ms:.0f}')
        if r['lost'] > lost_s:
            bad.append(f'{seg}: {r["lost"]:.2f} s lost > {lost_s:.2f}')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--worst', type=int, default=10)
    ap.add_argument('--csv')
    ap.add_argument('--check', action='store_true',
                    help='exit 1 unless both entrances and the match keep within --max-frame-ms / --max-lost-s')
    ap.add_argument('--max-frame-ms', type=float, default=500.0)
    ap.add_argument('--max-lost-s', type=float, default=2.0)
    a = ap.parse_args()
    rows = []
    for log in a.logs:
        report(log, a.worst, rows)
    if a.csv:
        with open(a.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    if a.check:
        bad = check(rows, a.max_frame_ms, a.max_lost_s)
        print('entrance check: ' + ('PASS' if not bad else 'FAIL: ' + '; '.join(bad)))
        return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

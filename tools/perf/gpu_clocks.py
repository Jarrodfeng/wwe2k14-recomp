"""Session 20 item 6: GPU clocks / power / utilisation per scene from an nvidia-smi log taken alongside a run.

    py tools\\perf\\gpu_clocks.py RUN SMI_CSV

RUN is the run number (logs\\wwe2k14_RUN.log + frames_RUN.csv under the exe folder). SMI_CSV is written by
tools\\perf\\s20_item6_batch.ps1 (nvidia-smi --format=csv -lms 100). Scene windows come from the route's scene marks
(profile_lesnar_bryan.txt): brock = tron_343..tron_324, bryan = tron_324..match, pan = tron_324 +380..+500 (Bryan's
heavy ramp pan, as bench_report.py), match = match +0..+2000. Frame -> wall time uses the frame log's t_seconds,
anchored at each scene mark's log timestamp.

Throttle reasons (clocks_throttle_reasons.active bits): 0x1 idle, 0x2 app clocks setting, 0x4 SW power cap,
0x8 HW slowdown, 0x20 SW thermal, 0x40 HW thermal, 0x80 HW power brake, 0x100 display clock setting.
"""
import csv
import re
import statistics
import sys
from datetime import datetime, timedelta
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / 'out' / 'build' / 'win-amd64-release' / 'logs'
REASONS = {0x1: 'idle', 0x2: 'app-clocks', 0x4: 'sw-power-cap', 0x8: 'hw-slowdown', 0x20: 'sw-thermal',
           0x40: 'hw-thermal', 0x80: 'hw-power-brake', 0x100: 'display-clock'}


def num(s):
    m = re.search(r'-?[\d.]+', s)
    return float(m.group()) if m else None


def main():
    run, smi_path = sys.argv[1], sys.argv[2]
    marks = {}
    for line in open(LOGS / f'wwe2k14_{run}.log', encoding='utf-8', errors='replace'):
        m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*?\[input\].*?(?:scene=(\S+)|(bench: done)) \(frame (\d+)\)', line)
        if m:
            marks[m.group(2) or 'done'] = (datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f'), int(m.group(4)))
    tsec = {}
    with open(LOGS / f'frames_{run}.csv', newline='') as f:
        for r in csv.DictReader(f):
            tsec[int(r['frame'])] = float(r['t_seconds'])

    def wall(anchor, off):
        t0, f0 = marks[anchor]
        f = f0 + off
        while f not in tsec and f > f0:
            f -= 1
        return t0 + timedelta(seconds=tsec[f] - tsec.get(f0, tsec[f])), f

    windows = {
        'brock': (wall('tron_343', 0)[0], wall('tron_324', 0)[0]),
        'bryan': (wall('tron_324', 0)[0], wall('match', 0)[0]),
        'pan': (wall('tron_324', 380)[0], wall('tron_324', 500)[0]),
        'match': (wall('match', 0)[0], wall('match', 2000)[0]),
    }
    rows = []
    with open(smi_path, newline='') as f:
        rd = csv.reader(f)
        hdr = [h.strip() for h in next(rd)]
        for r in rd:
            if len(r) != len(hdr):
                continue
            d = dict(zip(hdr, [c.strip() for c in r]))
            try:
                d['_t'] = datetime.strptime(d['timestamp'], '%Y/%m/%d %H:%M:%S.%f')
            except ValueError:
                continue
            rows.append(d)

    def col(name):
        return next(h for h in hdr if h.startswith(name))
    c_gr, c_mem, c_pw, c_lim = col('clocks.current.graphics'), col('clocks.current.memory'), col('power.draw'), col('enforced.power.limit')
    c_ug, c_um, c_tmp, c_ps, c_thr = col('utilization.gpu'), col('utilization.memory'), col('temperature.gpu'), col('pstate'), next(h for h in hdr if h.startswith('clocks_') and h.endswith('reasons.active'))
    print(f'run {run}, {len(rows)} nvidia-smi samples, {smi_path}')
    print(f"{'scene':6} {'n':>4} {'gr MHz p50 [min-max]':>22} {'mem MHz':>8} {'W p50 [max] / limit':>22} {'util% p50 [min]':>16} {'mem%':>5} {'temp max':>8}  pstates  throttle")
    for name, (a, b) in windows.items():
        s = [d for d in rows if a <= d['_t'] <= b]
        if not s:
            print(f'{name:6} no samples ({a} - {b})')
            continue
        gr = [num(d[c_gr]) for d in s]
        pw = [num(d[c_pw]) for d in s]
        ug = [num(d[c_ug]) for d in s]
        thr = 0
        for d in s:
            thr |= int(d[c_thr], 16)
        reasons = ','.join(v for k, v in REASONS.items() if thr & k) or 'none'
        print(f"{name:6} {len(s):4} {statistics.median(gr):8.0f} [{min(gr):.0f}-{max(gr):.0f}]   "
              f"{statistics.median([num(d[c_mem]) for d in s]):8.0f} {statistics.median(pw):9.1f} [{max(pw):.1f}] / {num(s[0][c_lim]):.0f}"
              f" {statistics.median(ug):9.0f} [{min(ug):.0f}] {statistics.median([num(d[c_um]) for d in s]):5.0f} {max(num(d[c_tmp]) for d in s):8.0f}"
              f"  {'/'.join(sorted(set(d[c_ps] for d in s)))}  {reasons}")


if __name__ == '__main__':
    main()

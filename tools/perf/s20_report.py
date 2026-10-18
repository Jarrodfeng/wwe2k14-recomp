"""Session 20: per-arm table for a tools\\perf\\s20_batch.ps1 batch (fps, GPU ms per frame, draws, CP busy per scene,
power from the nvidia-smi log, and the [s20][RECSTATS] lines nearest Bryan's pan).

    py tools\\perf\\s20_report.py TAG [--base ARM] [--recstats]

Reads out\\harness\\step20\\background.txt for the TAG's '-> wwe2k14_NNN' lines (the last line per arm wins), runs
tools\\bench\\bench_report.py report RUN for each and keeps the tron_324_pan and match rows.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BG = REPO / 'out' / 'harness' / 'step20' / 'background.txt'
LOGS = REPO / 'out' / 'build' / 'win-amd64-release' / 'logs'
SCENES = ('tron_324_pan', 'match', 'tron_343', 'tron_324')

ap = argparse.ArgumentParser()
ap.add_argument('tag')
ap.add_argument('--base', default='base')
ap.add_argument('--recstats', action='store_true')
a = ap.parse_args()
arms = {}
for line in BG.read_text(encoding='utf-8', errors='replace').splitlines():
    m = re.search(rf'\b{re.escape(a.tag)} (\S+) -> .*?wwe2k14_(\d+)(.*?)\| smi (\S+)', line)
    if m:
        arms[m.group(1)] = (int(m.group(2)), m.group(3), m.group(4))
rows = {}
for arm, (run, status, smi) in arms.items():
    out = subprocess.run([sys.executable, str(REPO / 'tools' / 'bench' / 'bench_report.py'), 'report', str(run)],
                         capture_output=True, text=True).stdout
    r = {}
    for s in SCENES:
        m = re.search(rf'^\s+{s}\s+(\d+)-(\d+)\s+\d+\s+([\d.]+).*?([\d.]+) fps.*?draws\s+(\d+).*?gpu\s+([\d.]+)', out, re.M)
        if m:
            r[s] = dict(frames=(int(m.group(1)), int(m.group(2))), busy=float(m.group(3)), fps=float(m.group(4)),
                        draws=int(m.group(5)), gpu=float(m.group(6)))
    rows[arm] = (run, 'EXITED' in status and 'exit code 0' in status, r)
base = rows.get(a.base, (None, None, {}))[2]
for s in ('tron_324_pan', 'match'):
    print(f'\n{s}:')
    print(f"{'arm':10} {'run':>5} {'ok':>3} {'fps':>7} {'GPU ms':>7} {'saved':>6} {'draws':>6} {'CP busy':>8}")
    for arm, (run, ok, r) in rows.items():
        x = r.get(s)
        if not x:
            print(f'{arm:10} {run:5} {"y" if ok else "n":>3}  (no row)')
            continue
        saved = base[s]['gpu'] - x['gpu'] if s in base else float('nan')
        print(f"{arm:10} {run:5} {'y' if ok else 'n':>3} {x['fps']:7.1f} {x['gpu']:7.2f} {saved:6.2f} {x['draws']:6} {x['busy']:8.2f}")
if a.recstats:
    for arm, (run, ok, r) in rows.items():
        pan = r.get('tron_324_pan')
        if not pan:
            continue
        f0, f1 = pan['frames']
        print(f'\n{arm} (run {run}) RECSTATS windows overlapping CP frames near the pan (guest {f0}-{f1}):')
        log = (LOGS / f'wwe2k14_{run}.log').read_text(encoding='utf-8', errors='replace').splitlines()
        for i, line in enumerate(log):
            m = re.search(r'\[s20\]\[RECSTATS\] frames (\d+)-(\d+)', line)
            if m and int(m.group(2)) >= f0 and int(m.group(1)) <= f1:
                for l in log[i:i + 3]:
                    print('  ' + l.split('[s20][RECSTATS] ')[1])

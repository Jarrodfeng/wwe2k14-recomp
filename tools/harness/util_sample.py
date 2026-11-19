r"""How much of the PC is the work using? (Session 103, 2026-11-17; Jarrod: "you have a whole 16 cores and 24gb of vram, you should
ABSOLUTELY be leveraging this to the best of your ability".)

Samples CPU %, free RAM, GPU utilisation and VRAM every N seconds into a csv, then summarises it. Run it in the BACKGROUND alongside
gates / builders (it ends by itself after --minutes). Read-only; starts nothing else.

    py -I tools\harness\util_sample.py --minutes 30 [--every 5] [--out out\harness\util\NAME.csv]
    py -I tools\harness\util_sample.py --summary out\harness\util\NAME.csv

Summary: mean / p50 / p90 of each column and the share of samples where the CPU is under 50 % AND the GPU under 30 % ("idle
headroom": the PC could take more lanes / slots then). Use it to tune GATE_JOBS / GATE_GPU_SLOTS and the number of builder lanes.
"""
import argparse
import csv
import os
import subprocess
import sys
import time


def gpu():
    try:
        out = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu,memory.used,memory.total', '--format=csv,noheader,nounits'],
                             capture_output=True, text=True, timeout=10).stdout.strip().splitlines()[0]
        u, used, tot = [float(x) for x in out.split(',')]
        return u, used / 1024.0, tot / 1024.0
    except Exception:
        return float('nan'), float('nan'), float('nan')


def cpu_ram():
    try:
        ps = ("$c=(Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average; "
              "$m=(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory; Write-Output \"$c,$m\"")
        out = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True, timeout=20).stdout.strip()
        c, m = out.split(',')
        return float(c), float(m) / 1048576.0
    except Exception:
        return float('nan'), float('nan')


def pct(v, q):
    v = sorted(x for x in v if x == x)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float('nan')


def summary(path):
    rows = list(csv.DictReader(open(path, encoding='utf-8')))
    if not rows:
        print('no samples'); return 1
    cols = ['cpu_pct', 'ram_free_gb', 'gpu_pct', 'vram_gb']
    print(f'{path}: {len(rows)} samples, {rows[0]["time"]} .. {rows[-1]["time"]}')
    for c in cols:
        v = [float(r[c]) for r in rows]
        good = [x for x in v if x == x]
        print(f'  {c:12} mean {sum(good) / max(1, len(good)):6.1f}  p50 {pct(v, .5):6.1f}  p90 {pct(v, .9):6.1f}')
    idle = sum(1 for r in rows if float(r['cpu_pct']) < 50 and float(r['gpu_pct']) < 30)
    print(f'  idle headroom (CPU < 50 % and GPU < 30 %): {100 * idle / len(rows):.0f} % of samples')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--minutes', type=float, default=30)
    ap.add_argument('--every', type=float, default=5)
    ap.add_argument('--out', default=os.path.join('out', 'harness', 'util', time.strftime('util_%Y%m%d_%H%M.csv')))
    ap.add_argument('--summary')
    a = ap.parse_args()
    if a.summary:
        return summary(a.summary)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    end = time.time() + a.minutes * 60
    with open(a.out, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['time', 'cpu_pct', 'ram_free_gb', 'gpu_pct', 'vram_gb'])
        while time.time() < end:
            t0 = time.time()
            c, ram = cpu_ram()
            g, vram, _ = gpu()
            w.writerow([time.strftime('%H:%M:%S'), f'{c:.0f}', f'{ram:.1f}', f'{g:.0f}', f'{vram:.2f}'])
            f.flush()
            time.sleep(max(0.0, a.every - (time.time() - t0)))
    return summary(a.out)


if __name__ == '__main__':
    sys.exit(main())

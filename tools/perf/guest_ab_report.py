r"""Per-thread CPU of the match window for guest-codegen A/B runs (Session 95).
    py tools\perf\guest_ab_report.py ARM=RUNLOG_N=CSV [ARM=...]     (RUNLOG_N: the run number in out\s95\cg\ARM\exe\logs)
Sums cpu_ms per thread name between the run's 'scene=match' and 'scene=end' marks (thread_busy.ps1 rows, 1 s apart), divides by
the window's wall seconds: 1.00 = one core. Prints the guest threads (XThread F8..) and the total of all threads.
The window is a fixed number of frames, not a fixed time, so the headline is core-SECONDS per window (= per ~1500 frames) as well:
the rate hides a slow run. Sample rows are scaled by the time they cover (summed interval_ms vs the window); a run whose CSV does not
span the whole window (sampler attached to another process or died early) is REJECTED, not averaged."""
import csv, os, re, sys
from collections import defaultdict
from datetime import datetime, timedelta
R = r'C:\Users\Jarrod\wwe2k14-recomp'
ROOT = os.environ.get('GUEST_AB_ROOT', 'out/s95/cg')  # guest_ab_run.ps1 -Root (Session 95)
def window(arm, n):
    p = f'{R}/{ROOT}/{arm.split("+")[0]}/exe/logs/wwe2k14_{n.zfill(3)}.log'
    t = {}
    for line in open(p, encoding='utf-8', errors='replace'):
        m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[input\] line \d+(?: \([^)]*\))?: (scene=\S+)', line)
        if m and m.group(2) in ('scene=match', 'scene=end') and m.group(2) not in t:
            t[m.group(2)] = datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f')
    return t['scene=match'], t['scene=end']
def main():
    res = {}
    for a in sys.argv[1:]:
        arm, n, p = a.split('=', 2)
        t0, t1 = window(arm, n)
        day = t0.date()
        agg = defaultdict(float)
        rows = 0
        stamps = {}
        allt = []
        for r in csv.DictReader(open(p)):
            tt = datetime.combine(day, datetime.strptime(r['time'], '%H:%M:%S.%f').time())
            allt.append(tt)
            if t0 <= tt <= t1:
                stamps[r['time']] = float(r['interval_ms'])
                nm = r['name']
                mm = re.search(r'\((F8[0-9A-F]+)\)', nm)
                agg['guest ' + mm.group(1) if (mm and nm.startswith('XThread')) else (nm if nm not in ('-', '') else 'other')] += float(r['cpu_ms'])
                rows += 1
        wall = (t1 - t0).total_seconds()
        if not allt or allt[0] > t0 + timedelta(seconds=2) or allt[-1] < t1 - timedelta(seconds=2):
            print(f'{a.split("=")[0]} run {n}: REJECTED, the CSV ({allt[0] if allt else None} .. {allt[-1] if allt else None}) does not span the window {t0} .. {t1}')
            continue
        covered = sum(stamps.values()) / 1000
        scale = wall / covered if covered else 1.0   # rows cover covered s of the wall s of the window
        res[a] = {k: v / 1000 * scale / wall for k, v in agg.items()}
        print(f'{a.split("=")[0]} run {n}: window {wall:.1f} s (rows cover {covered:.1f} s, scaled x{scale:.3f}), total {sum(res[a].values()):.3f} cores = {sum(res[a].values()) * wall:.1f} core-s')
    if not res:
        return
    names = sorted({k for v in res.values() for k in v}, key=lambda k: -max(v.get(k, 0) for v in res.values()))
    ngpu = lambda v: sum(x for k, x in v.items() if not k.startswith(('GPU ', 'wwe2k14 UI', 'TimerQueue')))
    print('all threads except GPU Commands / Submit / VSync / UI / TimerQueue: ' + '  '.join(f'{a.split("=")[0]}#{a.split("=")[1]} {ngpu(v):.3f}' for a, v in res.items()))
    guest = lambda v: sum(x for k, x in v.items() if k.startswith('guest '))
    print('guest XThreads total: ' + '  '.join(f'{a.split("=")[0]}#{a.split("=")[1]} {guest(v):.3f}' for a, v in res.items()))
    wl = {a: (lambda w: (w[1] - w[0]).total_seconds())(window(*a.split('=')[:2])) for a in res}
    print('guest XThreads core-seconds per window (same frame count per run): ' + '  '.join(f'{a.split("=")[0]}#{a.split("=")[1]} {guest(v) * wl[a]:.2f}' for a, v in res.items()))
    print(f'{"thread":<28}' + ''.join(f'{a.split("=")[0] + "#" + a.split("=")[1]:>10}' for a in res))
    for k in names[:16]:
        print(f'{k[:27]:<28}' + ''.join(f'{v.get(k, 0):10.3f}' for v in res.values()))
main()

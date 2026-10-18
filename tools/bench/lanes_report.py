"""Report for timed runs in shared lanes (Session 8, task L3; EXPERIMENTAL; docs/harness/lanes_timed.md).

    py tools/bench/lanes_report.py BATCH_DIR [BATCH_DIR ...] [--json OUT.json] [--runs LOGS_DIR:N,N ...]

BATCH_DIR = a folder written by tools/bench/run_timed_concurrent.ps1 (out/harness/lanes/batches/<stamp>_<label>).
Per run (bench routes): per scene (Brock / second entrance / match; regress.py's bench_metrics, i.e. fps60.py's
scene boundaries) fps, frames > 20 / 25 / 35 ms and >= 45 ms, % > 20 ms, min full 600-frame window fps, GPU ms per
frame, wall s; CP busy mean / p95 (bench_report.py report); and the regress perf verdict against
out/harness/regress/baseline.json (regress.py's compare(): fps, n45, GPU ms, wall s against its A/A thresholds;
plus the 60-fps bar on taker4k). Nothing is written to the regress folder and the baseline is only read.
Per in-run A/B run (script ab_*.txt, made by make_ab.py): bench_report.py report --ab NAME over the whole run (GPU ms
and CP busy, B - A per adjacent segment pair, bootstrap 95% CI) and ab_paired.py per scene (CP busy).
Per batch: GPU (nvidia-smi) and CPU (lanes_monitor.ps1) over the window where every lane's game runs, and over each
scene's intersection across lanes (wall-clock times from the logs' [file] lines); and any game that was not one of
the batch's lanes while a lane ran ("other games next to the lanes", from the CPU monitor's per_game column).
bench_report JSONs are cached in out/harness/lanes/reports/ and rebuilt when the run's log / CSVs are newer (or, for
A/B, when made for another cvar).
Reads only our own run output (logs, frames/timeline CSVs, monitor CSVs). Numbers only.
"""
import argparse
import datetime
import glob
import importlib.util
import json
import os
import re
import statistics
import subprocess
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
OUT = os.path.join(REPO, 'out', 'harness', 'lanes')
REPORTS = os.path.join(OUT, 'reports')
BENCH = os.path.join(REPO, 'tools', 'bench')


def load_regress():
    spec = importlib.util.spec_from_file_location('regress_mod', os.path.join(REPO, 'tools', 'regress', 'regress.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


RG = load_regress()
TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')


def parse_time(s):
    return datetime.datetime.strptime(s, '%Y-%m-%d %H:%M:%S.%f')


def log_lines(logs, n):
    with open(os.path.join(logs, 'wwe2k14_%s.log' % n), encoding='utf-8', errors='replace') as f:
        return f.read().split('\n')


def cmdline_flags(lines):
    out = {}
    for ln in lines:
        m = re.search(r"modified: (\S+) = '([^']*)' \(source: command line\)", ln)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def scene_times(lines):
    """Wall-clock (start, end) per scene from the [file] lines: brock = 343.bik2 .. second titantron, second = that ..
    M3.PAC, match = M3.PAC + ~2 s .. last log line. Also the run's first and last log time."""
    anch = {}
    first = last = None
    for ln in lines:
        m = TS.match(ln)
        if not m:
            continue
        t = parse_time(m.group(1))
        first = first or t
        last = t
        fm = RG.FILE_FRAME.search(ln)
        if fm:
            p = fm.group(1).lower()
            for k in ('343.bik2', '103.bik2', '324.bik2', 'm3.pac'):
                if k in p and k not in anch:
                    anch[k] = t
    sec = '103.bik2' if '103.bik2' in anch else '324.bik2'
    sc = {}
    if '343.bik2' in anch and sec in anch:
        sc['brock'] = (anch['343.bik2'], anch[sec])
    if sec in anch and 'm3.pac' in anch:
        sc['second'] = (anch[sec], anch['m3.pac'])
    if 'm3.pac' in anch and last:
        sc['match'] = (anch['m3.pac'] + datetime.timedelta(seconds=2), last)
    return sc, first, last


def bench_report_json(logs, n, ab=None):
    os.makedirs(REPORTS, exist_ok=True)
    tag = re.sub(r'\W+', '_', os.path.relpath(logs, REPO)).strip('_').lower()
    out = os.path.join(REPORTS, tag)
    os.makedirs(out, exist_ok=True)
    js = os.path.join(out, 'report_%s.json' % n)
    cmd = [sys.executable, os.path.join(BENCH, 'bench_report.py'), 'report', n, '--logs', logs, '--out', out,
           '--no-frames-csv']
    if ab:
        cmd += ['--ab', ab]
    # Fixer (review): reuse a cached report only if it is newer than the run's log / timeline / frames files (log
    # numbers restart at 001 when a logs folder is emptied, so number alone does not identify a run) and, for A/B,
    # only if it was made for the same cvar.
    src = [os.path.join(logs, f % n) for f in ('wwe2k14_%s.log', 'timeline_%s.csv', 'frames_%s.csv')]
    src_mtime = max([os.path.getmtime(p) for p in src if os.path.exists(p)] or [0])
    if os.path.exists(js) and os.path.getmtime(js) >= src_mtime:
        try:
            d = json.load(open(js, encoding='utf-8'))
            names = [a.get('name') for a in (d.get('ab') or [])]
            if str(d.get('run')) == str(n) and ((not ab) or ab in names):
                return d
        except ValueError:
            pass
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    try:
        return json.load(open(js, encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def ab_paired_lines(logs, n, baseline=None):
    cmd = [sys.executable, os.path.join(BENCH, 'ab_paired.py'), n, '--logs', logs]
    if baseline:
        cmd += ['--baseline', baseline]
    r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    return [x for x in r.stdout.split('\n') if x.strip()]


CP_SCENE = {'tron_343': 'brock', 'tron_324': 'second', 'tron_103': 'second', 'match': 'match'}


def route_of(script, flags):
    s = os.path.basename(script or '').lower()
    if s.startswith('ab_'):
        return 'ab'
    if s == 'bench_lesnar_bryan.txt' and 'wwe2k14_preset_4k60' not in flags:
        return 'lesnar'
    if s == 'bench_taker_wm.txt' and flags.get('wwe2k14_preset_4k60') == 'true':
        return 'taker4k'
    return None


def run_metrics(logs, n, baseline):
    n = str(n).zfill(3)
    lines = log_lines(logs, n)
    flags = cmdline_flags(lines)
    script = flags.get('wwe2k14_input_script', '')
    route = route_of(script, flags)
    res = dict(run=n, logs=logs, script=os.path.basename(script), route=route,
               flags={k: v for k, v in flags.items() if k not in RG.RUNLIST_FLAGS and k != 'wwe2k14_input_script'})
    hard = [ln[:160] for ln in lines if any(p in ln for p in ('[wwe2k14][crash]', 'device lost', 'Failed to await',
                                                                'script FAILED'))]
    res['hard_fails'] = hard[:3]
    sc_t, first, last = scene_times(lines)
    res['t_first'] = first.isoformat(sep=' ') if first else None
    pm = re.search(r'run log number: \d+ \(pid (\d+)\)', lines[0] if lines else '')
    res['pid'] = pm.group(1) if pm else None
    res['t_last'] = last.isoformat(sep=' ') if last else None
    res['scene_times'] = {k: (a.isoformat(sep=' '), b.isoformat(sep=' ')) for k, (a, b) in sc_t.items()}
    if route == 'ab':
        first_line = open(script if os.path.isabs(script) else os.path.join(REPO, 'out', 'build', 'win-amd64-release', script),
                          encoding='utf-8').readline()
        m = re.search(r'In-run A/B of (\S+)', first_line)
        cvar = m.group(1) if m else None
        res['cvar'] = cvar
        rep = bench_report_json(logs, n, ab=cvar)
        ab = (rep.get('ab') or [{}])[0]
        res['ab'] = {}
        for k in ('gpu_ms', 'busy', 'asub'):
            s = (ab.get('metrics') or {}).get(k)
            if s:
                res['ab'][k] = dict(mean=s['mean'], ci_lo=s['ci_lo'], ci_hi=s['ci_hi'], pos=s['pos'], neg=s['neg'])
        res['ab_pairs'] = ab.get('pairs')
        res['ab_arms'] = {a: v.get('means', {}) for a, v in (ab.get('arms') or {}).items()}
        res['ab_paired'] = ab_paired_lines(logs, n)
        return res
    bm = RG.bench_metrics(logs, n, lines)
    res['scenes'] = {}
    rep = bench_report_json(logs, n)
    cp = {}
    for s in rep.get('scenes', []):
        k = CP_SCENE.get(s.get('name'))
        if k and k not in cp:
            cp[k] = s
    for k, v in bm['scenes'].items():
        d = {x: v.get(x) for x in ('fps', 'n20', 'pct20', 'n25', 'n35', 'n45', 'max_ms', 'gpu_ms', 'wall_s',
                                   'min_full_window_fps', 'frames')}
        c = cp.get(k, {})
        d['cp_busy'] = c.get('busy_mean')
        d['cp_p95'] = c.get('busy_p95')
        d['asub'] = c.get('asub_mean')
        res['scenes'][k] = d
    # regress perf verdict (thresholds from A/A noise) + the 60-fps bar on taker4k
    verdict = []
    if route and baseline and route in baseline['routes']:
        bsc = baseline['routes'][route].get('scenes', {})
        for s in RG.SCENES:
            sc = bm['scenes'].get(s)
            if sc is None:
                verdict.append(('perf.%s' % s, 'fail', 'scene missing'))
                continue
            for metric in RG.METRICS:
                b = bsc.get(s, {}).get(metric)
                if not b:
                    continue
                st, txt = RG.compare(metric, sc.get(metric), b['base'], b['thr'])
                verdict.append(('perf.%s.%s' % (s, metric), st, txt))
            if route == 'taker4k':
                mw = sc['min_full_window_fps']
                ok = mw is not None and mw >= RG.BAR60['min_window_fps'] and sc['pct20'] < RG.BAR60['max_pct20']
                verdict.append(('bar60.%s' % s, 'pass' if ok else 'warn', 'min window %s, >20 ms %.2f%%' % (mw, sc['pct20'])))
    res['verdict'] = verdict
    res['perf_fail'] = [v[0] for v in verdict if v[1] == 'fail']
    res['bar_warn'] = [v[0] for v in verdict if v[1] == 'warn']
    return res


def read_csv(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding='utf-8-sig') as f:
        head = f.readline().strip().split(',')
        for ln in f:
            p = ln.rstrip('\n').split(',')
            if len(p) < len(head):
                continue
            rows.append(dict(zip(head, p)))
    return rows


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def stats(vals):
    v = sorted(x for x in vals if x is not None)
    if not v:
        return None
    p = lambda q: v[min(len(v) - 1, int(round(q * (len(v) - 1))))]
    return dict(n=len(v), mean=statistics.mean(v), p50=p(0.5), p95=p(0.95), max=v[-1], min=v[0])


def window_stats(gpu, cpu, t0, t1):
    g = [r for r in gpu if t0 <= parse_time(r['time']) <= t1]
    c = [r for r in cpu if t0 <= parse_time(r['time']) <= t1]
    util = [fnum(r['util']) for r in g]
    out = dict(seconds=round((t1 - t0).total_seconds(), 1), gpu_samples=len(g), cpu_samples=len(c),
               gpu_util=stats(util), power_w=stats([fnum(r['power_w']) for r in g]),
               clock_mhz=stats([fnum(r['clock_mhz']) for r in g]), mem_mib=stats([fnum(r['mem_mib']) for r in g]),
               cpu_total=stats([fnum(r['cpu_total']) for r in c]), ccd0=stats([fnum(r['ccd0']) for r in c]),
               ccd1=stats([fnum(r['ccd1']) for r in c]), game_cpu=stats([fnum(r['game_cpu']) for r in c]),
               mem_avail_mb=stats([fnum(r['mem_avail_mb']) for r in c]),
               games=stats([fnum(r['games']) for r in c]))
    out['util_ge95_pct'] = round(100.0 * sum(1 for u in util if u is not None and u >= 95) / len(util), 1) if util else None
    per = {}
    for r in c:
        for item in (r.get('per_game') or '').split(';'):
            p = item.split(':')
            if len(p) == 3 and p[1] != 'new':
                per.setdefault(p[0], []).append(float(p[1]))
    out['per_game_cpu'] = {k: round(statistics.mean(v), 0) for k, v in per.items()}
    fg = [r.get('fg_pid') for r in c if r.get('fg_pid')]
    out['fg_share'] = {k: round(100.0 * fg.count(k) / len(fg), 0) for k in sorted(set(fg))} if fg else {}
    return out


def batch_report(bdir, baseline):
    b = json.load(open(os.path.join(bdir, 'batch.json'), encoding='utf-8-sig'))
    logs = os.path.join(os.path.dirname(b['exe']), 'logs')
    runs = []
    for ln in b['lanes']:
        if ln.get('idle') or not ln.get('run'):
            continue
        n = ln['run'].split('_')[1]
        m = run_metrics(logs, n, baseline)
        m['lane'] = ln['lane']
        m['slot'] = ln.get('slot')
        m['affinity'] = ln.get('affinity')
        m['run_list_line'] = ln.get('run_list_line')
        runs.append(m)
    gpu = read_csv(os.path.join(bdir, 'gpu.csv'))
    cpu = read_csv(os.path.join(bdir, 'cpu.csv'))
    mon = {}
    if runs and all(r['t_first'] and r['t_last'] for r in runs):
        t0 = max(parse_time(r['t_first']) for r in runs)
        t1 = min(parse_time(r['t_last']) for r in runs)
        if t1 > t0:
            mon['all_running'] = window_stats(gpu, cpu, t0, t1)
        for s in ('brock', 'second', 'match'):
            if all(s in r['scene_times'] for r in runs):
                a = max(parse_time(r['scene_times'][s][0]) for r in runs)
                z = min(parse_time(r['scene_times'][s][1]) for r in runs)
                if z > a:
                    mon[s] = window_stats(gpu, cpu, a, z)
    starts = sorted(parse_time(r['t_first']) for r in runs if r['t_first'])
    return dict(batch=os.path.basename(bdir), label=b['label'], mode=b['mode'], lanes=b['lanes_n'],
                idle_holds=b.get('idle_holds'), affinity=b.get('affinity'), exe_sha256=b['exe_sha256'],
                slot_holds=b.get('slot_holds', 'lanes'), start_offsets_s=[round((t - starts[0]).total_seconds(), 1) for t in starts],
                runs=runs, monitor=mon, foreign_games=foreign_games(cpu, runs) if cpu else None)


def foreign_games(cpu, runs):
    """Games in the CPU monitor's per_game column that are not one of the batch's lanes, in samples where at least one
    lane game was running (fixer, after review: other agents' runs joined 3 batches when a lane gave its slot back)."""
    lane = {m['pid']: m['run'] for m in runs if m.get('pid')}
    out = {}
    for r in cpu:
        pids = [x.split(':')[0] for x in (r.get('per_game') or '').split(';') if x]
        alive = [p for p in pids if p in lane]
        if not alive:
            continue
        for p in pids:
            if p in lane:
                continue
            d = out.setdefault(p, dict(pid=p, first=r['time'], last=r['time'], samples=0, fg_samples=0, next_to=set()))
            d['last'] = r['time']
            d['samples'] += 1
            d['fg_samples'] += 1 if r.get('fg_pid') == p else 0
            d['next_to'].update(lane[x] for x in alive)
    return [dict(d, next_to=sorted(d['next_to'])) for d in out.values()]


def fmt(x, nd=2):
    if x is None:
        return '-'
    if isinstance(x, float):
        return ('%.' + str(nd) + 'f') % x
    return str(x)


def print_batch(r):
    print('== %s  (%s, %d lane(s)%s%s; exe %s; start offsets %s s)' % (
        r['batch'], r['mode'], r['lanes'], ', %d idle hold(s)' % r['idle_holds'] if r['idle_holds'] else '',
        ', affinity %s' % r['affinity'] if r['affinity'] else '', r['exe_sha256'][:12], r['start_offsets_s']))
    for m in r['runs']:
        print('  lane %s slot %s run %s %s %s%s' % (m['lane'], m['slot'], m['run'], m['script'], m['route'],
                                                    '  HARD FAIL: %s' % m['hard_fails'] if m['hard_fails'] else ''))
        if m['route'] == 'ab':
            for k, s in m.get('ab', {}).items():
                print('     A/B %-7s B-A %+.3f  CI [%+.3f, %+.3f] (width %.3f)  %d+/%d-  pairs %s' % (
                    k, s['mean'], s['ci_lo'], s['ci_hi'], s['ci_hi'] - s['ci_lo'], s['pos'], s['neg'], m['ab_pairs']))
            for x in m.get('ab_paired', [])[1:]:
                print('     ' + x.strip()[:150])
            continue
        print('     %-7s %8s %5s %5s %5s %5s %6s %8s %7s %7s %6s %6s' % ('scene', 'fps', '>20', '>25', '>35', '>=45',
                                                                    '%>20', 'minwin', 'gpu_ms', 'wall_s', 'cp', 'cp95'))
        for s in ('brock', 'second', 'match'):
            d = m['scenes'].get(s)
            if not d:
                print('     %-7s missing' % s)
                continue
            print('     %-7s %8s %5s %5s %5s %5s %6s %8s %7s %7s %6s %6s' % (
                s, fmt(d['fps'], 3), d['n20'], d['n25'], d['n35'], d['n45'], fmt(d['pct20']), fmt(d['min_full_window_fps']),
                fmt(d['gpu_ms']), fmt(d['wall_s']), fmt(d['cp_busy']), fmt(d['cp_p95'])))
        if m['verdict']:
            print('     regress perf: %s%s' % ('PASS' if not m['perf_fail'] else 'FAIL ' + ', '.join(
                v[0] + ' (' + v[2] + ')' for v in m['verdict'] if v[1] == 'fail'),
                '; bar60 warn: ' + ', '.join(m['bar_warn']) if m['bar_warn'] else ''))
    fgn = r.get('foreign_games')
    if fgn is not None:
        print('  other games next to the lanes (CPU monitor): %s' % ('none' if not fgn else '; '.join(
            'pid %s %s..%s (%d samples, foreground in %d) next to run(s) %s' % (
                d['pid'], d['first'][11:19], d['last'][11:19], d['samples'], d['fg_samples'], ','.join(d['next_to']))
            for d in fgn)))
    for k, w in r['monitor'].items():
        g, c = w['gpu_util'], w['cpu_total']
        print('  monitor %-11s %6.1f s: GPU util mean %s p95 %s, >=95%% in %s%% of samples, power %s W (p95 %s), clock %s MHz, '
              'VRAM %s MiB | CPU total %s%% (p95 %s), CCD0 %s%% CCD1 %s%%, games %s%% of one LP %s, mem avail min %s MB' % (
                  k, w['seconds'], fmt(g and g['mean'], 1), fmt(g and g['p95'], 1), fmt(w['util_ge95_pct'], 1),
                  fmt(w['power_w'] and w['power_w']['mean'], 0), fmt(w['power_w'] and w['power_w']['p95'], 0),
                  fmt(w['clock_mhz'] and w['clock_mhz']['mean'], 0), fmt(w['mem_mib'] and w['mem_mib']['max'], 0),
                  fmt(c and c['mean'], 1), fmt(c and c['p95'], 1), fmt(w['ccd0'] and w['ccd0']['mean'], 1),
                  fmt(w['ccd1'] and w['ccd1']['mean'], 1), fmt(w['game_cpu'] and w['game_cpu']['mean'], 0),
                  w['per_game_cpu'], fmt(w['mem_avail_mb'] and w['mem_avail_mb']['min'], 0)))
        if w.get('fg_share'):
            runpid = {m.get('pid'): m['run'] for m in r['runs']}
            print('          foreground window: ' + ', '.join('%s %s%%' % ('run ' + runpid[k] if k in runpid else 'pid ' + k, v)
                                                       for k, v in w['fg_share'].items()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('batches', nargs='*')
    ap.add_argument('--json', help='write every number to this JSON file')
    ap.add_argument('--runs', action='append', default=[],
                    help='extra runs without a batch folder: LOGS_DIR:N,N (e.g. the release logs folder:419)')
    args = ap.parse_args()
    baseline = RG.load_baseline(RG.BASELINE)
    out = []
    for pat in args.batches:
        for bdir in sorted(glob.glob(pat)):
            if os.path.exists(os.path.join(bdir, 'batch.json')):
                r = batch_report(bdir, baseline)
                print_batch(r)
                out.append(r)
    for spec in args.runs:
        logs, _, nums = spec.rpartition(':')
        rs = []
        for n in nums.split(','):
            rs.append(run_metrics(logs, n, baseline))
        r = dict(batch='runs %s' % spec, label='extra', mode='?', lanes=len(rs), idle_holds=0, affinity=None,
                 exe_sha256='?' * 12, start_offsets_s=[], runs=rs, monitor={})
        for m in rs:
            m.update(lane='-', slot='-')
        print_batch(r)
        out.append(r)
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(out, f, indent=1, default=str)


if __name__ == '__main__':
    main()

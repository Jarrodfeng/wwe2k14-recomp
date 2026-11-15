"""Tests of tools\\surface\\bench_line.py (S102 lane M3, review round 1). Called by budget_selftest.run(); every "refused" test is a planted twin of the
fixture (tools\\board\\fixtures\\bench: trimmed real frames CSVs and log lines of out\\s101\\surface_rebench\\y runs 187 / 188 / 189, with power.csv times
moved to the log's clock so the fixture is a fresh, overlapping heat record).
Past bugs re-found: (1) the first bench_line took the CSV's last row as the window end and printed baselines that were off by 2-4 fps (the quit frame is
1.6-2.5 s long); (2) the step folders' power.csv are from 2026-11-07 beside 2026-11-13 logs, and an old tool printed them as the run's heat."""
import os, re, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REAL = Path('C:/Users/Jarrod/wwe2k14-recomp/out/s101/surface_rebench/y')
CLI = HERE.parent / 'surface' / 'bench_line.py'


def run(tmp, ok, B, BL, FIX):
    # work on a copy whose power.csv carry their log's mtime (a git checkout writes files seconds apart; the freshness rule allows 120 s but a test must not depend on it)
    FIX0, FIX = FIX, tmp / 'fixture_norm'
    shutil.copytree(FIX0, FIX)
    for lg in FIX.glob('*/wwe2k14_*.log'):
        os.utime(lg.parent / 'power.csv', (lg.stat().st_mtime, lg.stat().st_mtime))

    def cli(d, *a):
        r = subprocess.run([sys.executable, '-I', str(CLI), str(d)] + list(a), capture_output=True, text=True)
        return r.returncode, r.stdout, r.stderr

    def fixture(label):
        d = tmp / ('b_' + label.replace(' ', '_').replace('/', '_'))
        shutil.copytree(FIX, d)
        return d

    def same_mtimes(d):   # a rewritten power.csv gets the log's mtime again, so only the planted fault is under test (not "stale")
        for lg in d.glob('*/wwe2k14_*.log'):
            pc = lg.parent / 'power.csv'
            if pc.exists():
                os.utime(pc, (lg.stat().st_mtime, lg.stat().st_mtime))

    def twin(label, mutate, expect, keep_mtime=False):
        d = fixture(label)
        mutate(d)
        if not keep_mtime:
            same_mtimes(d)
        rc, out, err = cli(d)
        ok(rc == 2 and out == '' and expect in err, f'planted: {label} -> refused, nothing printed ({err.strip()[:100]})')

    def nums(d, step, run_n, kind, label):
        s = BL.read_step(kind, d / step, run_n)
        return [w for w in s['windows'] if w['label'] == label][0]

    # ---- the fixture: what the line carries
    line, steps, name = BL.line_for(FIX)
    ok(all(w in line for w in ('settings settings_native720.json', 'tier STANDARD', 'res 720p', 'arm wwe2k14_fixed_seed=1', 'heat [steady]', 'bryan [frames 5514..5814]',
                               'brock [frames 6055..6655]', 'match [frames 5058..5658]')),
       'bench line carries settings file, tier, resolution, the arm, heat and the three windows with their frames')
    ok('MHz' not in line and 'limit median 88 min 88' in line and 'flags {2}' in line and ' pkg ' in line, 'heat is perf_limit / flags / package W; no fixed-rate MHz counter')
    ok(len(line.splitlines()) == 1 and not line.startswith('NOT A BENCH'), 'bench line is one line')
    rc, out, err = cli(FIX)
    ok(rc == 0 and out.strip() == line, 'the command prints that line, exit 0')
    ok('bryan/tron [frames 4288..5814]' in line, 'the tron..end window is printed too, labelled')
    # ---- numbers: the review's expected values (match 28.3, Bryan step15 31.4, Brock step15 29.6; +-0.1)
    for src, where in ((FIX, 'fixture'), (REAL, 'real folder y')):
        if not src.is_dir():
            print(f'skip  numbers on the {where}: not present on this machine')
            continue
        for step, n, kind, label, want, frames in (('frames_match', 189, 'match', 'match', 28.3, (5058, 5658)), ('frames_bryan', 187, 'bryan', 'bryan', 31.4, (5514, 5814)),
                                                   ('frames_brock', 188, 'brock', 'brock', 29.6, (6055, 6655))):
            rows = BL.read_frames(src / step / f'frames_{n}.csv')
            w = BL.window_numbers(rows, *frames)
            ok(abs(w['fps'] - want) <= 0.1, f'{where}: {label} window {frames[0]}..{frames[1]} shown {w["fps"]:.2f} fps = {want} +-0.1')
    if REAL.is_dir():
        rc, out, err = cli(REAL)
        ok(rc == 2 and out == '' and 'stale' in err, f'real folder y (power.csv of 2026-11-07 beside 2026-11-13 logs): the line is REFUSED ({err.strip()[:100]})')
        rc, out, err = cli(REAL, '--allow-no-heat')
        ok(rc == 0 and out.startswith('NOT A BENCH NUMBER') and 'heat NOT RECORDED' in out and 'match [frames 5058..5658]: shown 28.3' in out and 'bryan [frames 5514..5814]: shown 31.4' in out
           and 'brock [frames 6055..6655]: shown 29.6' in out, '--allow-no-heat on the real folder prints the windows, marked NOT A BENCH NUMBER')
    # ---- the quit frame is never in the window (past bug 1)
    d = fixture('quit row')
    p = d / 'frames_match' / 'frames_189.csv'
    txt = p.read_text(encoding='utf-8').splitlines(True)
    cols = txt[0].strip().split(',')
    last = int(txt[-1].split(',')[0])
    q = {c: '' for c in cols}
    q.update(frame=str(last + 1), t_seconds='99.0', frame_ms='2000.0', game_cpu_ms='0', draw_rows='0', presented='0')
    p.write_text(''.join(txt) + ','.join(q[c] for c in cols) + '\n', encoding='utf-8')
    a0 = nums(FIX, 'frames_match', 189, 'match', 'match')['n']['fps']
    a1 = nums(d, 'frames_match', 189, 'match', 'match')['n']['fps']
    ok(abs(a1 - a0) < 1e-9, f'a 2000 ms quit row after scene=end does not move the number ({a0:.3f} == {a1:.3f})')
    # ---- refusals
    def drop_end(dd):
        for lg in dd.glob('*/wwe2k14_*.log'):
            lg.write_text(''.join(l for l in lg.read_text(encoding='utf-8').splitlines(True) if 'scene=end' not in l), encoding='utf-8')
    twin('log without scene=end', drop_end, 'no scene=end')
    twin('no power.csv', lambda dd: (dd / 'frames_bryan' / 'power.csv').unlink(), 'missing')
    twin('power.csv header only', lambda dd: (dd / 'frames_match' / 'power.csv').write_text('time,t_s\n', encoding='utf-8'), 'no samples')
    twin('power.csv without cpu_perf / perf_limit', lambda dd: (dd / 'frames_brock' / 'power.csv').write_text('time,t_s,gpu3d_pct\n19:36:50.500,1,50\n19:36:51.500,2,50\n', encoding='utf-8'),
         'no cpu_perf / perf_limit')

    def shift_clock(dd):   # another run's power.csv: same rows, clock of day 6 hours earlier (no overlap with the log)
        pc = dd / 'frames_match' / 'power.csv'
        out_ = []
        for i, l in enumerate(pc.read_text(encoding='utf-8').splitlines(True)):
            if i:
                h = int(l[:2])
                l = '%02d' % (h - 6) + l[2:]
            out_.append(l)
        pc.write_text(''.join(out_), encoding='utf-8')
    twin('power.csv from another time of day (no overlap with the log)', shift_clock, 'no overlap')

    def old_mtime(dd):
        pc = dd / 'frames_match' / 'power.csv'
        t = time.time() - 86400
        os.utime(pc, (t, t))
    twin('power.csv written a day before the log (stale)', old_mtime, 'stale', keep_mtime=True)
    twin('log without the settings-file line', lambda dd: [lg.write_text(''.join(l for l in lg.read_text(encoding='utf-8').splitlines(True) if 'settings file' not in l), encoding='utf-8')
                                                             for lg in dd.glob('*/wwe2k14_*.log')], 'settings')
    twin('log without the quality-tier line', lambda dd: [lg.write_text(''.join(l for l in lg.read_text(encoding='utf-8').splitlines(True) if 'quality tier' not in l), encoding='utf-8')
                                                            for lg in dd.glob('*/wwe2k14_*.log')], 'quality-tier')

    def other_arm(dd):
        lg = dd / 'frames_bryan' / 'wwe2k14_187.log'
        lg.write_text(lg.read_text(encoding='utf-8') + "[2026-11-13 19:35:07.000] [info] [core] [t1] [wwe2k14] modified: wwe2k14_rdr_skip_prune = 'false' (source: command line)\n",
                      encoding='utf-8')
    twin('steps with different flags (an A/B arm in one step)', other_arm, 'different flags')
    # a lone step names its arm
    d = fixture('arm')
    lg = d / 'frames_bryan' / 'wwe2k14_187.log'
    lg.write_text(lg.read_text(encoding='utf-8') + "[2026-11-13 19:35:07.000] [info] [core] [t1] [wwe2k14] modified: wwe2k14_rdr_skip_prune = 'false' (source: command line)\n", encoding='utf-8')
    same_mtimes(d)
    one = BL.line_for(d / 'frames_bryan')[0]
    ok('arm wwe2k14_fixed_seed=1 wwe2k14_rdr_skip_prune=false' in one and one.startswith('bench '), 'a single step names its arm (the command-line flags)')
    # ---- render side: threads.csv's render thread over the window, else not measured
    ok('render not measured' in line, 'no render-thread column and no render thread in threads.csv: "not measured"')
    d = fixture('threads')
    (d / 'frames_match' / 'threads.csv').write_text('time,t_s,tid,name,cpu_ms,interval_ms,mcycles\n' + ''.join(
        f'19:37:{s:02d}.100,{s},77,rdr render,500.0,1000.0,900.0\n' for s in range(29, 38)), encoding='utf-8')
    w = nums(d, 'frames_match', 189, 'match', 'match')['n']
    ok(w['render'] is not None and w['render_src'] == 'threads.csv' and abs(w['render'] - 4500.0 / w['frames']) < 1e-6, f'render from the threads.csv render thread: {w["render"]:.2f} ms a swap')
    # ---- the ledger rows a bench writes are valid ledger rows, one set per scene window (not the labelled extras)
    led_path = tmp / 'append.csv'
    BL.append_ledger(led_path, steps, name, '2026-11-14 04:00')
    rows = B.read_ledger(led_path)
    ok(len(rows) >= 12 and all(x['kind'] == 'bench' for x in rows) and {x['scene'] for x in rows} == {'bryan', 'brock', 'match'},
       f'--ledger rows ({len(rows)}) parse as ledger rows of kind bench, one set per scene')
    one = BL.line_for(FIX / 'frames_bryan')[0]
    ok(one.startswith('bench ') and 'bryan [' in one and 'brock [' not in one, 'a single step folder gives a one-scene line')
    return 0

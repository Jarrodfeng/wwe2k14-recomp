#!/usr/bin/env python3
r"""S103 GRF: bench_line.py on a small committed synthetic fixture (tools\surface\fixtures\bench_synth: our own made-up numbers, no game data).
Replaces the old gate check that read out\s101\surface_rebench\y (a Surface capture the tidy removed).

  py -I tools\surface\bench_line_fixture_check.py [--root DIR]

1. the fixture's line carries the numbers worked out by hand from the csv (61 swaps, 60 of 20 ms and one of 40 ms: shown 49.2 fps, 1% low 25.0, gpu 10.0,
   swap 20.3) and the window the route's own marks give (frames 100..160), with the heat honestly NOT RECORDED (no power.csv) and the NOT A BENCH NUMBER prefix.
2. planted twins, each must be REFUSED (exit 2): the log without its scene=end mark, the log without the settings-file line, the csv with a lost-device row."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run(root, folder):
    p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'bench_line.py'), folder, '--allow-no-heat'], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout, p.stderr


# S103 GT: the renderer's by-role / uploads log lines (src\renderer\renderer.cpp LogGpuWindow), as the stream build AND the stream-free build print them; our own made-up numbers
ROLE_A = ('[2026-01-01 10:00:02.000] [rdr][gpu] 600 frames by role, mean ms a frame over 600 frames: colour 3.600, shadow 0.300, rim 0.400, plane 0.300, cards 0.200, post 0.800, output 0.800, ui 0.000, frame 3.700, '
          'copies 2.200, prelight 0.200, feedback 0.000, other 0.800 = work 13.300 (median 13.200) + start gap [0]-[1] 0.100 (median 0.100) = 13.400 (the bracket above)\n'
          '[2026-01-01 10:00:02.001] [rdr][gpu][uploads] 600 frames: frames 600; steady (<= 1024 KB uploaded) frames 590 = 0.020 MB/frame; loading (> 1024 KB) frames 10 = 4.000 MB/frame (1.7 % of the frames, 77.2 % of the bytes); all frames 0.072 MB/frame; biggest frame 9.00 MB; GPU ms of the uploads\' commands 0.100 a frame over 600 frames with the stamps\n')
ROLE_B = ('[2026-01-01 10:00:02.000] [rdr][gpu] 600 frames by role, mean ms a frame over 300 frames: colour 9.000, shadow 0.600, rim 0.000, plane 0.000, cards 0.400, post 4.000, output 1.000, ui 0.000, frame 6.000, '
          'copies 3.000, prelight 0.000, feedback 0.000, other 2.000 = work 26.000 (median 25.000) + start gap [0]-[1] 0.100 (median 0.100) = 26.100 (the bracket above)\n'
          '[2026-01-01 10:00:02.001] [rdr][gpu][uploads] 600 frames: frames 300; steady (<= 1024 KB uploaded) frames 100 = 0.500 MB/frame; loading (> 1024 KB) frames 200 = 30.000 MB/frame (66.7 % of the frames, 99.2 % of the bytes); all frames 20.000 MB/frame; biggest frame 40.00 MB; GPU ms of the uploads\' commands 2.000 a frame over 300 frames with the stamps\n')


def win_line(name, n, colour, other, uploads, work, gap, bracket, unst=0.0, steady=0.02, loading=5.0):
    return (f'[2026-01-01 10:00:02.100] [rdr][gpu][window] {name}: GPU frames {n}; by role, mean ms a frame: colour {colour:.3f}, shadow 0.300, rim 0.000, plane 0.000, cards 0.200, post 0.800, output 0.800, ui 0.000, frame 1.000, copies 0.000, '
            f'prelight 0.000, feedback 0.000, other {other:.3f}, uploads {uploads:.3f} = work {work:.3f}; start gap [0]-[1] {gap:.3f}; bracket {bracket:.3f} (the csv gpu_ms basis: [0] to the tail stamp of every frame); unstamped {unst:.3f} (bracket - work - gap); '
            f'uploads steady (<= 1024 KB) frames {n - 10} = {steady:.3f} MB/frame, loading frames 10 = {loading:.3f} MB/frame\n')


def window_checks(root, fix):
    """The exact route-window block: bench_line --roles reads the blocks from the route's start mark up to the `end` window only (a block before it, and the `end` block, are not read); the csv-vs-bracket divergence
    is flagged; a log with only 600-frame blocks says NOT window-exact. Planted twins: a pre-route block that would be averaged in must change the numbers if it were read; a divergent bracket must print the flag."""
    fails = []
    tmp = tempfile.mkdtemp(prefix='bl_win_')
    try:
        def make(name, text):
            dst = os.path.join(tmp, name)
            shutil.copytree(fix, dst)
            lp = os.path.join(dst, 'frames_match', 'wwe2k14_1.log')
            with open(lp, newline='') as fh:
                body = fh.read()
            body = body.replace('[2026-01-01 10:00:03.000]', text + '[2026-01-01 10:00:03.000]', 1)
            with open(lp, 'w', newline='') as fh:
                fh.write(body)
            with open(os.path.join(dst, 'frames_match', 'power.csv'), 'w', newline='') as fh:
                fh.write('time,t_s,cpu_perf,perf_limit,perf_limit_flags,rapl_pkg\n' + ''.join(f'10:00:{sec:02d}.000,{sec},100,88,0,15000\n' for sec in range(0, 5)))
            return dst
        def run(dst):
            p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'bench_line.py'), dst, '--allow-no-heat', '--roles'], capture_output=True, text=True, timeout=120)
            return [l for l in p.stdout.splitlines() if l.startswith('roles [')]
        # the fixture csv has gpu 10.0 ms a frame: a window with bracket 10.1 agrees
        good = win_line('warm', 300, 40.0, 40.0, 40.0, 130.0, 1.0, 131.0) + win_line('match', 300, 4.000, 1.000, 0.500, 9.900, 0.100, 10.000) + win_line('end', 50, 99.0, 99.0, 99.0, 500.0, 1.0, 501.0)
        ls = run(make('good', good))
        ok = ls and 'colour 4.00' in ls[0] and 'other 1.00' in ls[0] and 'uploads 0.50' in ls[0] and '= work 9.90' in ls[0] and 'exact route window of 300 GPU frames' in ls[0] and 'DIVERGES' not in ls[0] and 'uploads steady 0.020 MB/frame (290 frames)' in ls[0]
        print(('PASS' if ok else 'FAIL') + f'  --roles reads only the exact route window (the warm-up block before it and the end block are not read): {(ls or [""])[0][:210]}')
        if not ok:
            fails.append('window exact')
        # two windows inside the route (a tron mark between step15 and end): summed, frame-weighted
        two = win_line('match', 100, 2.0, 1.0, 0.0, 5.0, 0.0, 5.0) + win_line('tron_9', 300, 6.0, 1.0, 0.0, 15.0, 0.0, 15.0) + win_line('end', 50, 99.0, 99.0, 99.0, 500.0, 1.0, 501.0)
        ls = run(make('two', two))
        ok = ls and 'colour 5.00' in ls[0] and 'exact route window of 400 GPU frames' in ls[0]
        print(('PASS' if ok else 'FAIL') + f'  two windows before end are summed frame-weighted (colour (100*2+300*6)/400 = 5.00): {(ls or [""])[0][:120]}')
        if not ok:
            fails.append('window two')
        # twin: the roles' bracket is half the csv GPU time (the 'work is half the GPU time' report): flagged
        ls = run(make('half', win_line('match', 300, 2.0, 0.5, 0.2, 4.9, 0.1, 5.0) + win_line('end', 10, 1.0, 1.0, 1.0, 5.0, 0.0, 5.0)))
        ok = ls and 'BRACKET DIVERGES FROM THE CSV' in ls[0]
        print(('PASS' if ok else 'FAIL') + '  a bracket far from the csv gpu_ms is flagged (twin)')
        if not ok:
            fails.append('window diverge twin')
        # twin: only 600-frame blocks in the log: not exact
        ls = run(make('old', ROLE_A))
        ok = ls and 'NOT window-exact' in ls[0]
        print(('PASS' if ok else 'FAIL') + '  a log with only 600-frame blocks says NOT window-exact (twin)')
        if not ok:
            fails.append('window old blocks')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return fails


def roles_checks(root, fix):
    """bench_line --roles reads the by-role + uploads lines (frame-weighted, inside the route window only), prints 'not measured' without them; ab_line --roles prints the A-vs-B table per role; a planted
    out-of-window line is ignored."""
    fails = []
    tmp = tempfile.mkdtemp(prefix='bl_roles_')
    try:
        def make(name, text, extra_out_of_window=''):
            dst = os.path.join(tmp, name)
            shutil.copytree(fix, dst)
            lp = os.path.join(dst, 'frames_match', 'wwe2k14_1.log')
            with open(lp, newline='') as fh:
                body = fh.read()
            body = body.replace('[2026-01-01 10:00:03.000]', text + '[2026-01-01 10:00:03.000]', 1)
            if extra_out_of_window:
                body += extra_out_of_window
            with open(lp, 'w', newline='') as fh:
                fh.write(body)
            with open(os.path.join(dst, 'frames_match', 'power.csv'), 'w', newline='') as fh:   # a flat made-up heat record (ab_line reads the line WITH its heat)
                fh.write('time,t_s,cpu_perf,perf_limit,perf_limit_flags,rapl_pkg\n' + ''.join(f'10:00:{sec:02d}.000,{sec},100,88,0,15000\n' for sec in range(0, 5)))
            return dst
        def line(dst):
            p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'bench_line.py'), dst, '--allow-no-heat', '--roles'], capture_output=True, text=True, timeout=120)
            return p.returncode, [l for l in p.stdout.splitlines() if l.startswith('roles [')]
        a = make('arm_a', ROLE_A)
        rc, ls = line(a)
        want = ['roles [match frames 100..160]', 'colour 3.60', 'frame 3.70', 'copies 2.20', 'other 0.80', '= work 13.30 ms a frame (NOT window-exact: 600 GPU frames in 1 blocks', 'from before the route window', 'uploads steady 0.020 MB/frame (590 frames)', 'loading 4.000 MB/frame (10 frames, 1.7 % of the frames)']
        miss = [w for w in want if not ls or w not in ls[0]]
        ok = rc == 0 and len(ls) == 1 and not miss
        print(('PASS' if ok else 'FAIL') + f'  --roles reads the stamped lines: missing {miss}: {(ls or [""])[0][:200]}')
        if not ok:
            fails.append('roles line')
        late = '[2026-01-01 10:00:09.000] ' + ROLE_B.split('\n')[0].split('] ', 1)[1] + '\n'   # after the window + 3 s: must not count
        a2 = make('arm_late', ROLE_A, late)
        rc, ls = line(a2)
        ok = rc == 0 and ls and 'work 13.30 ms a frame (NOT window-exact: 600 GPU frames in 1 blocks' in ls[0]
        print(('PASS' if ok else 'FAIL') + '  a by-role line stamped after the window is not read')
        if not ok:
            fails.append('roles late line read')
        rc, ls = line(fix)
        ok = rc == 0 and ls and 'not measured' in ls[0]
        print(('PASS' if ok else 'FAIL') + f'  a log with no stamps says not measured: {(ls or [""])[0][:120]}')
        if not ok:
            fails.append('roles not-measured')
        # ab_line: two arms, A with stamps and B with stamps, then a B without stamps
        folder = os.path.join(tmp, 'job')
        for arm, txt in (('ab_1_A', ROLE_A), ('ab_1_B', ROLE_B)):
            d = make('x_' + arm, txt)
            shutil.copytree(os.path.join(d, 'frames_match'), os.path.join(folder, arm), dirs_exist_ok=True)
        # (ab_line copies an arm folder under the scene's name itself)
        p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'ab_line.py'), folder, '--scene', 'frames_match', '--roles'], capture_output=True, text=True, timeout=180)
        rows = {}
        for l in p.stdout.splitlines():
            if l.startswith('roles ') and not l.startswith('roles ['):
                parts = l[6:].split()
                if len(parts) >= 4 and not l.startswith('roles (GPU'):
                    try:
                        rows[' '.join(parts[:-3])] = [float(x) for x in parts[-3:]]
                    except ValueError:
                        pass
        ok = rows.get('colour') == [3.6, 9.0, 5.4] and rows.get('work') == [13.3, 26.0, 12.7] and rows.get('uploads loading MB/frame') == [4.0, 30.0, 26.0] and 'roles [match frames 100..160]' in p.stdout
        print(('PASS' if ok else 'FAIL') + f'  ab_line --roles: the A vs B table per role (colour {rows.get("colour")}, work {rows.get("work")}, loading uploads {rows.get("uploads loading MB/frame")})')
        if not ok:
            fails.append('ab_line roles table: ' + p.stdout[-400:] + p.stderr[-200:])
        # twin: a B with the stamps and uploads removed prints "B 0 arms with stamps" and no table
        d = make('x_none', '')
        shutil.rmtree(os.path.join(folder, 'ab_1_B'))
        shutil.copytree(os.path.join(d, 'frames_match'), os.path.join(folder, 'ab_1_B'))
        p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'surface', 'ab_line.py'), folder, '--scene', 'frames_match', '--roles'], capture_output=True, text=True, timeout=180)
        ok = 'B 0 arms with stamps of 1' in p.stdout and 'roles (GPU ms' not in p.stdout
        print(('PASS' if ok else 'FAIL') + '  ab_line --roles with one side unstamped prints no table (twin)')
        if not ok:
            fails.append('ab_line roles twin')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    root = os.path.abspath(ap.parse_args().root)
    fix = os.path.join(root, 'tools', 'surface', 'fixtures', 'bench_synth')
    if not os.path.isdir(fix):
        print(f'FAIL  cannot read its input: {fix}')
        return 1
    fails = []
    rc, out, err = run(root, fix)
    want = ['match [frames 100..160]', 'shown 49.2 fps', '1% low 25.0', 'gpu 10.0', 'swap 20.3 ms', 'settings settings_native720.json', 'tier STANDARD', 'res 720p', 'heat NOT RECORDED']
    miss = [w for w in want if w not in out]
    ok = rc == 0 and not miss and out.startswith('NOT A BENCH NUMBER')
    print(('PASS' if ok else 'FAIL') + f'  the fixture line: exit {rc}, missing {miss}: {out.strip()[:200]}')
    if not ok:
        fails.append('fixture line')
    plants = (('no scene=end', 'wwe2k14_1.log', lambda s: ''.join(l for l in s.splitlines(True) if 'scene=end' not in l)),
              ('no settings line', 'wwe2k14_1.log', lambda s: ''.join(l for l in s.splitlines(True) if 'settings file' not in l)),
              ('device lost', 'frames_1.csv', lambda s: s.replace('\r\n', '\n').replace('frame,frame_ms,gpu_ms,presented,present_gap_ms', 'frame,frame_ms,gpu_ms,presented,present_gap_ms,device_lost', 1).replace('\n', ',1\n').replace('present_gap_ms,device_lost,1', 'present_gap_ms,device_lost', 1)))
    for name, fname, edit in plants:
        tmp = tempfile.mkdtemp(prefix='bl_plant_')
        try:
            dst = os.path.join(tmp, 'bench_synth')
            shutil.copytree(fix, dst)
            fp = os.path.join(dst, 'frames_match', fname)
            with open(fp, newline='') as fh:
                txt = fh.read()
            new = edit(txt)
            if new == txt:
                print(f'FAIL  planted {name}: the edit changed nothing (fixture drifted)')
                fails.append(name + ' plant inert')
                continue
            with open(fp, 'w', newline='') as fh:
                fh.write(new)
            rc, out, err = run(root, dst)
            ok = rc == 2 and 'REFUSED' in err and not out.strip()
            print(('PASS' if ok else 'FAIL') + f'  planted {name} is REFUSED: exit {rc}: {err.strip()[:120]}')
            if not ok:
                fails.append(name + ' not refused')
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    fails += roles_checks(root, fix)
    fails += window_checks(root, fix)
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  bench_line fixture check')
    return 0


if __name__ == '__main__':
    sys.exit(main())

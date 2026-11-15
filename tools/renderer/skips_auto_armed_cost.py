#!/usr/bin/env python3
"""S102 K3 rework (ab11): what being ARMED costs the render thread, measured offline (0 game runs).
  py -I tools\\renderer\\skips_auto_armed_cost.py [--root DIR] [--runs N] [--frames N]

rplay replays Bryan's entrance (the main checkout's run 20261112_122700, unpacked once into out\\k3bryan) three ways, interleaved N times each (a shared PC: the MEDIAN of N):
  unarmed   no D3 chain at all (what the player has today with the skips off)
  armed     --d3-chains: the arena and caster chains armed as live.cpp arms them, every frame bound WITH the chains (ab11's armed-off cost: the chains' Begin / End on every drawn frame)
  armed-off --d3-chains-off-frames: armed, every frame bound WITHOUT the chains (binder.d3_chain_frame false: what the rework makes the armed-off frame)
Bars: armed-off bind us a draw not more than 3 % above unarmed (one-sided: a shared PC swings +-7 % run to run); the chains' bookkeeping per frame (d3_timers.h) 0 in armed-off; the `armed` arm must sit MORE than 3 % above unarmed (the twin: a bar that cannot see the
regression is blind and FAILS). Bind time, not record time (SKIP_COST.md: rplay's record is GPU-wait dominated). rplay's own "load" is the capture file read, not the live parse, so "parse + contents" is
shown by the D3 bookkeeping line (the render side work the rework removes) and not claimed as a bar here.
Exit 0 = pass."""
import argparse, csv, os, re, statistics, subprocess, sys, zipfile


def main_checkout(here):
    try:
        c = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return here


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--frames', type=int, default=237)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.isfile(exe):
        print('FAIL  rplay.exe missing: ' + exe)
        return 1
    cap = os.path.join(root, 'out', 'k3bryan', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        src = os.path.join(main_co, 'out', 's101', 'runs', '20261112_122700_s100_owner_bryan_entrance')
        zs = [os.path.join(src, f) for f in os.listdir(src) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(src) else []
        if not zs:
            print('FAIL  input missing: the Bryan entrance case zip under ' + src)
            return 1
        zipfile.ZipFile(zs[0]).extractall(os.path.join(root, 'out', 'k3bryan'))
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: ' + game)
        return 1
    out = os.path.join(root, 'out', 'k3armed')
    os.makedirs(out, exist_ok=True)
    arms = [('unarmed', []), ('armed', ['--d3-chains']), ('armed-off', ['--d3-chains-off-frames'])]
    res = {n: [] for n, _ in arms}
    d3line = {}
    for r in range(a.runs):
        for name, extra in arms:
            t = os.path.join(out, '%s_%d.csv' % (name, r))
            cmd = [exe, '--capture', cap, '--frames', str(a.frames), '--thin', 'arena', '--arena-ours', '--game', game, '--timing', t] + extra
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, errors='replace')
            text = (p.stdout or '') + (p.stderr or '')
            if p.returncode or not os.path.isfile(t):
                print(text[-2000:])
                print('FAIL  rplay arm %s failed (exit %d)' % (name, p.returncode))
                return 1
            bind = draws = 0
            with open(t) as fh:
                for row in csv.DictReader(fh):
                    bind += float(row['bind_us'])
                    draws += float(row['draws'])
            res[name].append(bind / max(1, draws))
            m = re.search(r'\[rdr\]\[d3_time\] per handled frame .*total ([\d.]+) us a frame', text)
            if m:
                d3line.setdefault(name, []).append(float(m.group(1)))
            print('  %s run %d: bind %.3f us a draw%s' % (name, r, res[name][-1], (', D3 bookkeeping %.1f us a frame' % float(m.group(1))) if m else ''), flush=True)
    med = {n: statistics.median(v) for n, v in res.items()}
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok
    base = med['unarmed']
    d_off = (med['armed-off'] / base - 1.0) * 100.0
    d_on = (med['armed'] / base - 1.0) * 100.0
    verdict(d_off <= 3.0, 'armed-off bind %.3f us a draw vs unarmed %.3f: %+.2f %% (bar: not more than 3 %% above; below is run noise on a shared PC, the three runs of each arm are printed)' % (med['armed-off'], base, d_off))
    verdict(d_on > 3.0, 'the armed arm (the chains on every frame, ab11) sits %+.2f %% above unarmed (%.3f us a draw): the bar can see the regression' % (d_on, med['armed']))
    off_d3 = statistics.median(d3line.get('armed-off', [0.0]))
    on_d3 = statistics.median(d3line.get('armed', [0.0]))
    verdict(off_d3 < 5.0 and on_d3 > 100.0, 'D3 bookkeeping a frame: armed %.1f us, armed-off %.1f us (the chains do no work on an armed-off frame)' % (on_d3, off_d3))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

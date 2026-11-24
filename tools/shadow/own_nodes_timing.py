r"""S102 lane P4P5: the PC cost of the shadow module's and the rim map's passes, from rplay --timing (a measurement, not a check; 0 game runs, headless).

  py -I tools\shadow\own_nodes_timing.py [--capture bryan|match] [--reps 3] [--frames 237] [--skip 30]

Arms, interleaved a repeat at a time (the GPU is shared with other lanes: a number is only trusted when the arms were measured next to each other): game, records_only (the casters / rim map drawn from the scene records: the proxy for what drawing ours costs), none_m (the module's passes left out),
none_r (the rim map's), none_b (both). Per frame: gpu_ns (GPU timestamps around the command buffer), bind_us (the binder's walk on the swap-thread side of our replay), render_us. The first --skip frames are
dropped (pipelines build on demand). Prints the median over frames of each run, and the median of the runs. The difference game - none_x is the cost of the group; it is NOT a Surface number.
rplay is the RELEASE build (tools\renderer\build.ps1 -Release); the capture is unpacked into out\s102\p4p5 and deleted afterwards."""
import argparse
import csv
import os
import shutil
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
ARMS = {'game': [], 'records_only': ['--wrestlers-ours', '--shadow-casters-ours', '--rim-ours'], 'none_m': ['--own-shadow-module=none'], 'none_r': ['--own-rim-map=none'], 'none_b': ['--own-shadow-module=none', '--own-rim-map=none']}


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default='bryan')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--frames', type=int, default=237)
    ap.add_argument('--skip', type=int, default=30)
    a = ap.parse_args()
    main_co = main_checkout()
    rplay = os.path.join(ROOT, 'out', 'renderer', 'release', 'rplay.exe')
    if not os.path.isfile(rplay):
        print('FAIL  no ' + rplay + ' (tools\\renderer\\build.ps1 -Release)')
        return 1
    zip_path = os.path.join(main_co, CAPS[a.capture])
    work = os.path.join(ROOT, 'out', 's102', 'p4p5')
    dest = os.path.join(work, 'unpack_time_' + a.capture)
    os.makedirs(dest, exist_ok=True)
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    if not os.path.isfile(os.path.join(dest, 'capture', 'images.bin')):
        subprocess.run([tar, '-xf', zip_path, '-C', dest], check=True, timeout=1800)
    cap = os.path.join(dest, 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    res = {k: [] for k in ARMS}
    try:
        for rep in range(a.reps):
            for arm, flags in ARMS.items():
                out = os.path.join(work, 'time_%s_%s_%d.csv' % (a.capture, arm, rep))
                p = subprocess.run([rplay, '--capture', cap, '--frames', str(a.frames), '--game', game, '--timing', out] + flags, capture_output=True, text=True, timeout=3000)
                if not os.path.isfile(out):
                    print(p.stdout[-1500:] + p.stderr[-1500:])
                    print('FAIL  rplay wrote no timing for ' + arm)
                    return 1
                rows = list(csv.DictReader(open(out)))[a.skip:]
                os.remove(out)
                med = lambda key: statistics.median(float(r[key]) for r in rows)
                res[arm].append((med('gpu_ns') / 1e6, med('bind_us') / 1e3, med('render_us') / 1e3, statistics.mean(float(r['draws']) for r in rows)))
                print('rep %d %-7s gpu %.3f ms  bind %.3f ms  render %.3f ms  draws %.0f' % ((rep,) + (arm,) + res[arm][-1]), flush=True)
    finally:
        shutil.rmtree(dest, ignore_errors=True)
    print('== %s, %d frames, %d reps, medians of the runs' % (a.capture, a.frames, a.reps))
    base = {k: statistics.median(r[i] for r in res['game']) for i, k in enumerate(('gpu', 'bind', 'render', 'draws'))}
    for arm in ARMS:
        m = {k: statistics.median(r[i] for r in res[arm]) for i, k in enumerate(('gpu', 'bind', 'render', 'draws'))}
        print('%-7s gpu %.3f ms (%+.3f)  bind %.3f ms (%+.3f)  render %.3f ms (%+.3f)  draws %.0f (%+.0f)' % (arm, m['gpu'], m['gpu'] - base['gpu'], m['bind'], m['bind'] - base['bind'], m['render'], m['render'] - base['render'], m['draws'], m['draws'] - base['draws']))
    return 0


if __name__ == '__main__':
    sys.exit(main())

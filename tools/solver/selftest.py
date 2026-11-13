r"""Gate check for the constant solver (lane 16). Needs the six Check A case zips and the atlas (main checkout out\).
  1. baseline run over all zips: exits 0, prints one scene row per zip and one effect table; known classes hold (expect.json).
  2. positive controls are the same run: S98's value-matched FILE constants, and a constant S98 left as "RUNTIME, unconfirmed".
  3. planted fault A: one FILE comparison perturbed in one draw -> that (effect, constant) must come out RUNTIME (solve.py exits 0 only then).
  4. planted fault B: names stripped -> every draw unjoined, loudly (solve.py exits 0 only when nothing joined).
  5. a glob that matches nothing fails loudly (exit 1, 'FAIL').
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOLVE = HERE / 'solve.py'
sys.path.insert(0, str(HERE))
import solve  # noqa: E402


# The corpus is PINNED to Check A's zips (out\s101\runs\20261111_17*, KEEP files): every later check adds zips to out\s101\runs, and the default glob
# (every case zip) made this selftest grow past the gate's 900 s timeout (S101 full gate after Check D).
PINNED = str(solve.MAIN / 'out' / 's101' / 'runs' / '20261111_17*' / 'case_*.zip')


def run(args, timeout=280):
    t = time.time()
    if '--zips' not in args:
        args = ['--zips', PINNED] + args
    p = subprocess.run([sys.executable, '-I', str(SOLVE)] + args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr, time.time() - t


def main():
    fails = []
    expect = json.loads((HERE / 'expect.json').read_text(encoding='utf-8'))
    rc, out, dt = run([])
    print(out.strip().splitlines()[-1] if out.strip() else '(no output)')
    if rc != 0:
        fails.append(f'baseline run exit {rc}')
    zips = sorted(solve.MAIN.glob('out/s101/runs/20261111_17*/case_*.zip'))
    rows = [l for l in out.splitlines() if ' frames (' in l]
    if len(rows) != len(zips) or not zips:
        fails.append(f'baseline printed {len(rows)} scene rows for {len(zips)} zips')
    if dt > 120:
        fails.append(f'baseline took {dt:.0f} s (bar: seconds to a minute)')
    res = json.loads((solve.OUT / 'constants.json').read_text(encoding='utf-8'))
    for cls, items in expect['classes'].items():
        for it in items:
            eff, key = it.split(' ', 1)
            got = res['effects'].get(eff, {}).get(key, {}).get('verdict')
            if got != cls:
                fails.append(f'expected {it} = {cls}, got {got}')
    joined = sum(s['joined'] + s['joined_ambiguous'] for s in res['scenes'])
    if joined < expect['min_joined']:
        fails.append(f"only {joined} draws joined (expected >= {expect['min_joined']})")
    print(f'baseline: {len(rows)} scenes, {joined} draws joined, {dt:.1f} s, {sum(len(v) for v in expect["classes"].values())} known classes checked')
    # review fixes (lane 16 round 2): each of these failed before its fix
    if any('|' in e for e in res['effects']):
        fails.append('an effect label holds two effects (' + ', '.join(e for e in res['effects'] if '|' in e) + '): a shared shader hash was not resolved by the mesh')
    if not any(r.get('disputed') for recs in res['effects'].values() for r in recs.values()):
        fails.append('no verdict line carries a disputed count (rejected joins must be shown, not dropped)')
    if not any(r.get('partial') for recs in res['effects'].values() for r in recs.values()):
        fails.append('no constant reported partial (a moving component with no file source must make the constant RUNTIME)')
    if solve.pick_frames(5, 1) != [0] or solve.pick_frames(5, 2) != [0, 4]:
        fails.append('pick_frames(n, 1) is broken')
    # planted fault A, with its unplanted twin at the same frames per zip
    scratch = solve.OUT / 'selftest'
    for plant in expect['plant']:
        eff, field, stage = plant.split(':')
        rc, out, _ = run(['--quiet', '--frames-per-zip', '6', '--out', str(scratch)])
        base_res = json.loads((scratch / 'constants.json').read_text(encoding='utf-8'))
        v0 = base_res['effects'].get(eff, {}).get(f'{stage}:{field}', {}).get('verdict')
        rc, out, _ = run(['--plant', plant, '--quiet', '--frames-per-zip', '6'])
        line = [l for l in out.splitlines() if l.startswith('planted fault')]
        print(' ', f'unplanted {v0};', line[-1] if line else out.strip()[-200:])
        if v0 != 'FILE':
            fails.append(f'unplanted {plant} at 6 frames per zip is {v0}, not FILE: the planted fault proves nothing')
        if rc != 0:
            fails.append(f'planted fault {plant} was not reported RUNTIME')
    # planted fault B
    rc, out, _ = run(['--strip-names', '--quiet', '--frames-per-zip', '3'])
    line = [l for l in out.splitlines() if l.startswith('names stripped')]
    print(' ', line[-1] if line else out.strip()[-200:])
    if rc != 0 or not line:
        fails.append('names-stripped capture still joined draws (or printed nothing)')
    # missing input
    rc, out, _ = run(['--zips', str(solve.MAIN / 'out' / 's101' / 'runs' / 'nothing_here' / '*.zip')])
    print(' ', out.strip().splitlines()[-1] if out.strip() else '(no output)')
    if rc == 0 or 'FAIL' not in out:
        fails.append('a missing input did not fail loudly')
    if fails:
        for f in fails:
            print('FAIL  ' + f)
        return 1
    print('solver selftest: all pass')
    return 0


if __name__ == '__main__':
    sys.exit(main())

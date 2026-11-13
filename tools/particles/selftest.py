r"""Lane P0 (S101) selftest of the particles census: the shipped census.py is run as a program on Check B's case zip, clean (must PASS, exit 0) and with each planted fault
(must exit 1 and fail the named checks). A positive control: a census that passes everything proves nothing.

  py -I tools\particles\selftest.py

Plants (census.py --plant): float_key (c200 unmasked + a float compare of the VS bank), nan_float (c200.xzw masked, floats compared: only the NaN in c200.y of the camera-facing sprites splits), shuffle (the
particle draws of each frame in another order), merge_sites (the key forgets the call site), no_names (origins stripped = a version-1 capture; must fail loudly).
0 game runs; reads the case zip from the main checkout's out\ (RDR_MAIN_CHECKOUT or git's common dir)."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


ZIP = os.path.join(main_checkout(), 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock', 'case_20261111_215233_217_0_pc_run_exit.zip')
# plant -> checks that must FAIL (and, for no_names, the only ones)
PLANTS = {'float_key': {'runs', 'runs_frame', 'boundaries'}, 'nan_float': {'runs', 'runs_frame', 'boundaries'}, 'shuffle': {'runs', 'runs_frame', 'boundaries'},
          'merge_sites': {'boundaries', 'runs'}, 'no_names': {'names'}}
# the exact run count a plant must reach (the sprites' NaN splits all 30,133 sprite draws; float_key also splits the quads on c200.z/w: 406 -> 557 quad runs)
RUNS = {'float_key': 30133 + 557, 'nan_float': 30133 + 406}


def run(plant):
    cmd = [sys.executable, '-I', os.path.join(HERE, 'census.py'), 'brock=' + ZIP, '-q'] + (['--plant', plant] if plant else [])
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    failed = {l.split()[2] for l in r.stdout.splitlines() if l.startswith('FAIL')}
    return r.returncode, failed, r.stdout


def main():
    if not os.path.exists(ZIP):
        print('FAIL  cannot read %s (a selftest that reads nothing must fail)' % ZIP)
        return 1
    bad = 0
    code, failed, out = run(None)
    ok = code == 0 and not failed
    print('%s  clean: exit %d, failed checks %s' % ('PASS' if ok else 'FAIL', code, sorted(failed) or 'none'))
    bad += not ok
    if not ok:
        print(out)
    for plant, want in PLANTS.items():
        code, failed, out = run(plant)
        exact = want == failed if plant == 'no_names' else want <= failed
        if plant in RUNS:
            exact = exact and ('[91724, %d]' % RUNS[plant]) in out
        ok = code == 1 and exact
        print('%s  plant %-11s: exit %d, failed checks %s (must include %s)' % ('PASS' if ok else 'FAIL', plant, code, sorted(failed), sorted(want)))
        bad += not ok
        if not ok:
            print(out)
    print('%d problems' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

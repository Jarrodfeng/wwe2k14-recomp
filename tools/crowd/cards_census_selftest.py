r"""Lane CC (S101) selftest of the cards census: the shipped cards_census.py is run as a program on Check B's case zip (every 7th frame), clean (must PASS, exit 0) and with each
planted fault (must exit 1 and fail the named check). A positive control: a census that passes everything proves nothing. Also an unreadable input must fail loudly.

  py -I tools\crowd\cards_census_selftest.py

Plants (cards_census.py --plant): type_shift (the type -> picture binding taken for type T + 1: lane 35's wrong-texture class) -> binding; order_swap (two cards of a type swapped in
the stream order) -> order; names_stripped (origins dropped = a version-1 capture) -> names; record_offset (record j instead of j-1) -> pairing; one_picture (every card expected to bind
the first card's picture: lane 35's one texture for every member) -> binding; atlas_picture (the 512 x 512 light-map page taken for the card picture) -> binding.
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
FRAMES = '0:341:7'
PLANTS = {'type_shift': {'binding'}, 'order_swap': {'order'}, 'names_stripped': {'names'}, 'record_offset': {'pairing'}, 'one_picture': {'binding'}, 'atlas_picture': {'binding'}}
# the only other checks a plant may break (a wrong pairing also breaks the block join)
ALSO = {'record_offset': {'binding'}}


def run(plant, path=ZIP):
    cmd = [sys.executable, '-I', os.path.join(HERE, 'cards_census.py'), 'brock=' + path, '--frames', FRAMES, '-q'] + (['--plant', plant] if plant else [])
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
        ok = code == 1 and want <= failed and failed <= want | ALSO.get(plant, set())
        print('%s  plant %-14s: exit %d, failed checks %s (must be %s)' % ('PASS' if ok else 'FAIL', plant, code, sorted(failed), sorted(want)))
        bad += not ok
        if not ok:
            print(out)
    code, failed, out = run(None, os.path.join(os.path.dirname(ZIP), 'no_such_case.zip'))
    ok = code == 1 and 'cannot read' in out
    print('%s  unreadable input: exit %d (must fail loudly)' % ('PASS' if ok else 'FAIL', code))
    bad += not ok
    print('%d problems' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

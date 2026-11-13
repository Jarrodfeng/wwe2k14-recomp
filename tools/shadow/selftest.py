r"""Lane SC (S101) selftest of the shadow census: the shipped shadow_census.py is run as a program on Check B's case zip, clean (must PASS, exit 0), with each planted fault
(must exit 1 and fail the named checks), and on a path that does not exist (must exit 1). A positive control: a census that passes everything proves nothing.

  py -I tools\shadow\selftest.py

Plants (shadow_census.py --plant):
  proxy_part_shift  the chCommon.pac entry 0 join is made one mesh off          -> chcommon_join (part = mesh)
  hash_only         the shadow passes picked by the surface role / shader alone  -> passes (the rim light map swallowed as a shadow map: stray draws), order, roles
  no_names          origins stripped = a version-1 capture                       -> names, and only names (nothing else can be read)
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
# plant -> (checks that must FAIL, exact?)
PLANTS = {'proxy_part_shift': ({'chcommon_join', 'expect.chcommon_join'}, False),
          'hash_only': ({'passes', 'order'}, False),
          'no_names': ({'names'}, True)}


def run(args):
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'shadow_census.py')] + args + ['-q'], capture_output=True, text=True, timeout=900)
    failed = {l.split()[2] for l in r.stdout.splitlines() if l.startswith('FAIL') and len(l.split()) > 2}
    return r.returncode, failed, r.stdout


def main():
    if not os.path.exists(ZIP):
        print('FAIL  cannot read %s (a selftest that reads nothing must fail)' % ZIP)
        return 1
    bad = 0
    code, failed, out = run(['brock=' + ZIP])
    ok = code == 0 and not failed
    print('%s  clean: exit %d, failed checks %s' % ('PASS' if ok else 'FAIL', code, sorted(failed) or 'none'))
    bad += not ok
    if not ok:
        print(out)
    for plant, (want, exact) in PLANTS.items():
        code, failed, out = run(['brock=' + ZIP, '--plant', plant])
        match = (want == failed) if exact else (want <= failed)
        ok = code == 1 and match
        print('%s  plant %-17s: exit %d, failed checks %s (must %s %s)' % ('PASS' if ok else 'FAIL', plant, code, sorted(failed), 'be exactly' if exact else 'include', sorted(want)))
        bad += not ok
        if not ok:
            print(out)
    code, failed, out = run(['brock=' + ZIP + '.missing'])
    ok = code == 1 and 'cannot read' in out
    print('%s  missing input: exit %d (must fail loudly)' % ('PASS' if ok else 'FAIL', code))
    bad += not ok
    print('%d problems' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

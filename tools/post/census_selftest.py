r"""Lane PC (S101) selftest of the post census: the shipped post_census.py is run as a program on the case zips and a version-1 capture, clean (must PASS, exit 0), with each planted fault
(must exit 1 and fail the named checks), and on a path that does not exist (must exit 1). A positive control: a census that passes everything proves nothing.

  py -I tools\post\census_selftest.py

Plants (post_census.py --plant), run on the Bryan entrance zip (the only capture with two DoF layers):
  names_stripped    origins stripped = a version-1 capture asked to be a named one   -> names, and only names (the rest is classified by effect and agrees)
  combine_dropped   the combine draw is not seen                                    -> combine (and the shapes / readers that depend on it)
  layer_merged      the combine's slot 6 is read as its slot 2 (Bryan's two layers collapse) -> layers, expect.layers
Twins: layer_merged and combine_dropped's counterpart on Check B / wm30 (one layer) -> layer_merged must NOT fail `layers` there (the plant moves nothing where there is nothing to merge).
0 game runs; reads the zips and captures from the main checkout's out\ (RDR_MAIN_CHECKOUT or git's common dir)."""
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


M = main_checkout()
RUNS = os.path.join(M, 'out', 's101', 'runs')
BROCK = ('brock', os.path.join(RUNS, '20261111_215201_s100_owner_s89_brock', 'case_20261111_215233_217_0_pc_run_exit.zip'))
BRYAN = ('bryan', os.path.join(RUNS, '20261111_174549_s100_owner_bryan_entrance', 'case_20261111_174607_799_0_pc_run_exit.zip'))
WM30 = ('wm30', os.path.join(RUNS, '20261111_174707_s100_owner_s90_wm30', 'case_20261111_174739_724_0_pc_run_exit.zip'))
V1 = ('match300', os.path.join(M, 'out', 'captures', 's100_match300'))
# (plant, capture, checks that must FAIL, exact?)
PLANTS = [('names_stripped', BRYAN, {'names'}, True),
          ('combine_dropped', BRYAN, {'combine'}, False),
          ('layer_merged', BRYAN, {'layers', 'expect.layers'}, False)]
# twins: (plant, capture, checks that must NOT fail)
TWINS = [('layer_merged', BROCK, {'layers'}), ('layer_merged', WM30, {'layers'})]


def run(args):
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'post_census.py')] + args + ['-q'], capture_output=True, text=True, timeout=900)
    failed = {l.split()[2] for l in r.stdout.splitlines() if l.startswith('FAIL') and len(l.split()) > 2}
    return r.returncode, failed, r.stdout


def main():
    bad = 0
    for label, path in (BROCK, BRYAN, WM30, V1):
        if not os.path.exists(path):
            print('FAIL  cannot read %s (a selftest that reads nothing must fail)' % path)
            return 1
    for label, path in (BROCK, BRYAN, WM30, V1):
        code, failed, out = run(['%s=%s' % (label, path)])
        ok = code == 0 and not failed
        print('%s  clean %-8s: exit %d, failed checks %s' % ('PASS' if ok else 'FAIL', label, code, sorted(failed) or 'none'))
        bad += not ok
        if not ok:
            print(out)
    for plant, (label, path), want, exact in PLANTS:
        code, failed, out = run(['%s=%s' % (label, path), '--plant', plant])
        match = (want == failed) if exact else (want <= failed)
        ok = code == 1 and match
        print('%s  plant %-16s on %-6s: exit %d, failed checks %s (must %s %s)' % ('PASS' if ok else 'FAIL', plant, label, code, sorted(failed), 'be exactly' if exact else 'include', sorted(want)))
        bad += not ok
        if not ok:
            print(out)
    for plant, (label, path), notwant in TWINS:
        code, failed, out = run(['%s=%s' % (label, path), '--plant', plant])
        ok = not (failed & notwant)
        print('%s  twin  %-16s on %-6s: failed checks %s (must not include %s)' % ('PASS' if ok else 'FAIL', plant, label, sorted(failed) or 'none', sorted(notwant)))
        bad += not ok
        if not ok:
            print(out)
    code, failed, out = run(['brock=' + BROCK[1] + '.missing'])
    ok = code == 1 and 'cannot read' in out
    print('%s  missing input: exit %d (must fail loudly)' % ('PASS' if ok else 'FAIL', code))
    bad += not ok
    print('%d problems' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

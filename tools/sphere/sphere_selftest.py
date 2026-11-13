r"""S101 lane 242 gate check: H3, the wrestlers' sphere occluders (VS c168-c171). Offline, 0 game runs, headless.

  py -I tools\sphere\sphere_selftest.py [--no-build] [--captures]

  1. builds out\sphere\sphere_rows_test.exe (tools\sphere\build_sphere_rows.ps1) and runs it: the recorder (decode, tracker, contributor, record file) against a fake guest built from the listing of
     sub_826DF8E0, and its six plants, each of which must fail its NAMED check (first_call, skeleton_off, v_from_p, stale_frame, skipped_as_ran, no_ready_check);
  2. the Python reader (ledger_io Record.sphere_occ) on the record the unit test wrote (12 rows, fields as the C++ wrote them);
  3. with --captures: tools\skip\sphere_check.py on the kept 7149 capture (SKIP when absent): the formula matches every sphere_occ draw (frames 40:300:10 here; the full 374 frames take ~12 s),
     and its plants fail (lag0 = lane 227's 3.4-4.8 unit miss re-found, skin, strength_a, swap).
Exit code 1 when any check failed."""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger_io  # noqa: E402

TEST = os.path.join(ROOT, 'out', 'sphere', 'sphere_rows_test.exe')
SAMPLE = os.path.join(ROOT, 'out', 'sphere', 'sample.rsc')
PLANTS = {'first_call': 'hook.last_call', 'skeleton_off': 'hook.object', 'v_from_p': 'hook.arrays', 'stale_frame': 'hook.frame_stamp', 'skipped_as_ran': 'hook.skipped_sphere',
          'no_ready_check': 'hook.early_outs'}
bad = 0


def check(name, ok, why=''):
    global bad
    print('%s  %-40s %s' % ('PASS' if ok else 'FAIL', name, why))
    bad += 0 if ok else 1


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    args = sys.argv[1:]
    if '--no-build' not in args:
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'sphere', 'build_sphere_rows.ps1')])
        check('build sphere_rows_test', rc == 0, out.strip().splitlines()[-1] if out.strip() else '')
    rc, out = run([TEST, '--write-record', SAMPLE])
    check('unit test honest run', rc == 0 and '0 failed' in out, out.strip().splitlines()[-1])
    for p, name in PLANTS.items():
        rc, out = run([TEST, '--plant', p])
        failed = re.findall(r'^FAIL\s+(\S+)', out, re.M)
        check('plant %s fails %s' % (p, name), rc == 1 and name in failed, 'failed: %s' % ','.join(failed))
    if os.path.exists(SAMPLE):
        rec = ledger_io.Record(SAMPLE)
        rows = rec.sphere_occ()
        ok = len(rows) == 12 and rows[0]['frame'] == 500 and rows[1]['object'] == 0x40310000 and rows[0]['ran'] == [True, True] and rows[0]['table'][0][2] == 59 and rows[0]['count'] == 2
        check('python reader Record.sphere_occ', ok, '%d rows' % len(rows))
        raw = open(SAMPLE, 'rb').read()
        check('python reader float order', ok and struct.pack('<f', rows[0]['p'][0][3]) == struct.pack('<f', 12.0), 'P.w of row 0 = %r' % (rows[0]['p'][0][3],) if rows else '')
    if '--captures' in args:
        cap = os.path.join(ROOT, 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture')
        if not os.path.isdir(cap):
            cap = os.path.normpath(os.path.join(ROOT, '..', '..', '..', 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture'))
        if not os.path.isdir(cap):
            print('SKIP  captures: %s absent' % cap)
        else:
            tool = os.path.join(ROOT, 'tools', 'skip', 'sphere_check.py')
            rc, out = run([sys.executable, '-I', tool, cap, '--frames', '40:300:10'])
            check('sphere_check honest', rc == 0 and 'VERDICT MATCH' in out, out.strip().splitlines()[-1])
            for p in ('lag0', 'skin', 'strength_a', 'swap'):
                rc, out = run([sys.executable, '-I', tool, cap, '--frames', '40:300:10', '--plant', p])
                check('sphere_check plant %s fails' % p, rc == 1 and 'VERDICT MISMATCH' in out, out.strip().splitlines()[-1])
    print('%s' % ('ALL PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

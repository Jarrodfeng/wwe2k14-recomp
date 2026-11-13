r"""S101 lane 92 (KL) gate check: the values ledger (tools\skip\ledger.py) on Check B (run 7051) and on run 7079, plus its planted twins.
Offline, 0 game runs. Input: the two case zips (unpacked once under <main checkout>\out\s101\lane92, game data, local only). A missing input FAILS loudly.

  py -I tools\skip\ledger_selftest.py [--frames 0:340:6]

Positive: Check B passes `--expect checkB` (the main wrestler's palette rows are NOT all sourced: the record held the update-hook palette for nodes 59-108)
and `--expect mirror`; run 7079 passes `--expect r7079` (every palette row of the main wrestler sourced, lane 43) and `--expect mirror`; every class has draws;
the per-source register counts add up; no draw without a registry variant.
Planted twins (each must make the named expectation FAIL, and must FAIL for the stated reason in the output):
  brock_update_palette  (re-finds a real bug) run 7051's records against the r7079 expectation: the palette rows of nodes 59-108 are unsourced
  brock_update_palette_back  run 7079 against the checkB expectation (the fixed palette must not look like the bug)
  offset0               the records read at offset 0 (lane 36's +1 missed): the record rows turn unsourced
  no_mirror_rule        the 0x82C0B94C cameras get no mirror rule: they turn unsourced
  no_record             the record side disabled
  derived_drop_w        (S101 lane 109) the c197 / c199 derived rules computed with colour w dropped: `--expect derived` must FAIL
  no_derived            (S101 lane 109) the derived column off: `--expect derived` must FAIL
  missing_input         a path that does not exist must fail (exit 1), not report an empty ledger"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
LEDGER = os.path.join(HERE, 'ledger.py')
B_ZIP = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
R_ZIP = 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip'


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def ledger(args, timeout=1200):
    p = subprocess.run([sys.executable, '-I', LEDGER] + args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main():
    frames = '0:340:6'
    if '--frames' in sys.argv:
        frames = sys.argv[sys.argv.index('--frames') + 1]
    main_co = main_checkout()
    bz, rz = os.path.join(main_co, B_ZIP), os.path.join(main_co, R_ZIP)
    bad = 0

    def check(name, ok, why):
        nonlocal bad
        print('%s  %-26s %s' % ('PASS' if ok else 'FAIL', name, why))
        bad += 0 if ok else 1

    for z in (bz, rz):
        if not os.path.isfile(z):
            print('FAIL  input missing: ' + z)
            return 1
    jb = os.path.join(main_co, 'out', 's101', 'lane92', 'selftest_b.json')
    jr = os.path.join(main_co, 'out', 's101', 'lane92', 'selftest_r.json')
    rc, out = ledger([bz, '--frames', frames, '-q', '--expect', 'checkB', '--expect', 'mirror', '--expect', 'derived', '--json', jb])
    check('checkB.expect', rc == 0 and out.count('PASS  expect') == 2 + 5, 'rc %d, %d expectations passed' % (rc, out.count('PASS  expect')))
    if rc != 0:
        print(out[-1500:])
    rc, out = ledger([rz, '--frames', frames, '-q', '--expect', 'r7079', '--expect', 'mirror', '--expect', 'derived', '--json', jr])
    check('r7079.expect', rc == 0 and out.count('PASS  expect') == 2 + 5, 'rc %d, %d expectations passed' % (rc, out.count('PASS  expect')))
    if rc != 0:
        print(out[-1500:])
    for tag, jp in (('checkB', jb), ('r7079', jr)):
        try:
            j = json.load(open(jp))
        except (OSError, ValueError) as e:
            check(tag + '.json', False, 'cannot read ' + jp + ': ' + str(e))
            continue
        cl = j['classes']
        check(tag + '.classes', all(cl[c]['draws'] > 0 for c in ('arena', 'wrestlers', 'ring', 'flash')),
              'draws ' + ' '.join('%s %d' % (c, cl[c]['draws']) for c in ('arena', 'wrestlers', 'ring', 'flash')))
        check(tag + '.variants', all(cl[c]['no_variant'] == 0 for c in cl), 'draws without a registry variant: %d' % sum(cl[c]['no_variant'] for c in cl))
        check(tag + '.sources', all(sum(cl[c]['regs'].values()) > 0 and set(cl[c]['regs']) <= {'record', 'pass', 'mirror', 'template', 'role', 'light_state', 'derived', 'unsourced'} for c in cl),
              'every register read has exactly one named source')
        check(tag + '.unsourced_named', all(cl[c]['regs'].get('unsourced', 0) == cl[c]['unsourced_reads'] for c in cl),
              'the unsourced list adds up to the unsourced reads')
    # planted twins: each must fail its expectation
    def plant(name, cap, expect, why, plant_arg=None):
        args = [cap, '--frames', frames, '-q', '--expect', expect]
        if plant_arg:
            args += ['--plant', plant_arg]
        rc, out = ledger(args)
        failed = rc != 0 and ('FAIL  expect %s' % expect) in out
        check('plant.' + name, failed, ('failed as planted (%s)' % why) if failed else 'DID NOT FAIL: rc %d  %s' % (rc, out[-300:].replace('\n', ' | ')))

    plant('brock_update_palette', bz, 'r7079', "7051's records vs the fixed expectation", None)
    plant('brock_update_palette_back', rz, 'checkB', "7079's records vs the bug expectation", None)
    plant('offset0', rz, 'r7079', 'records at offset 0', 'offset0')
    plant('no_mirror_rule', rz, 'mirror', 'mirrored cameras unsourced', 'no_mirror_rule')
    plant('no_record', rz, 'r7079', 'record side off', 'no_record')
    # S101 lane 109 (LS2): the derived column. The rules hold on both captures (above, pinned: every applicable wrestler read matches); the twins must fail them
    plant('derived_drop_w', rz, 'derived', 'c197 / c199 computed with colour w dropped (the record shape today)', 'derived_drop_w')
    plant('no_derived', bz, 'derived', 'the derived column off', 'no_derived')
    for tag, jp in (('checkB', jb), ('r7079', jr)):  # the light_state column is dormant on both (no record carries tables yet) and the JSON says so
        try:
            j = json.load(open(jp))
            check(tag + '.light_state_dormant', j.get('light_records') == 0 and all(not c.get('light_state_regs') for c in j['classes'].values()),
                  'light_records %s: the column sources nothing until a capture carries the tables' % j.get('light_records'))
        except (OSError, ValueError) as e:
            check(tag + '.light_state_dormant', False, 'cannot read ' + jp + ': ' + str(e))
    rc, out = ledger([os.path.join(main_co, 'out', 's101', 'lane92', 'does_not_exist_zip.zip'), '-q'])
    check('plant.missing_input', rc == 1 and out.startswith('FAIL'), 'rc %d: %s' % (rc, out.strip()[:100]))
    print('ledger_selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

r"""Lane PS (S101) selftest of the post plan test: the shipped post_plan_test.exe is run as a program on the 10 captures, clean (must PASS, exit 0) and with each planted fault
(must exit 1 and fail exactly the named check). A positive control: a test that passes everything proves nothing.

  py -I tools\post\post_plan_selftest.py [--exe PATH]

Plants (post_plan_test.exe --plant), each run through the same BuildPostPlan the game would use:
  layer_merged       Bryan's two DoF layers collapse into one (slot 6 read as slot 2)       -> `layers`, on bryan only
  dead_bloom_kept    the dead work kept: c31.x / c32.x == 0 not noticed                     -> `dead_work`, on bryan / brock_tron / cards only
  dof_filter_fixed   the dof depth filter fixed to linear (the old "match linear" guess)    -> `dof_filter`, on the point-filtered captures (brock, bryan, s89k_a, cards)
  last_pass_by_hash  the chain's end found by the combine's vertex hash (zeroed in the test) -> `chain_end`, everywhere
Each plant must fail its check on the captures that carry the fault and NOT on the others (the twins), and no other check anywhere. 0 game runs."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import post_plan_check as pc  # noqa: E402

PLANTS = {'layer_merged': ('layers', {'bryan'}),
          'dead_bloom_kept': ('dead_work', {'bryan', 'brock_tron', 'cards'}),
          'dof_filter_fixed': ('dof_filter', {'brock', 'bryan', 's89k_a', 'cards'}),
          'last_pass_by_hash': ('chain_end', None)}   # None: every capture


def run(plant, exe):
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'post_plan_check.py'), '--exe', exe, '-q'] + (['--plant', plant] if plant else []), capture_output=True, text=True, timeout=1500)
    fails = set()
    for line in r.stdout.splitlines():
        m = re.match(r'FAIL\s+(\S+)\s+(\S+)', line)
        if m:
            fails.add((m.group(1), m.group(2)))
    return r.returncode, fails, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(pc.ROOT, 'out', 'tools', 'post_plan_test.exe'))
    a = ap.parse_args()
    bad = 0
    code, fails, out = run('', a.exe)
    ok = code == 0 and not fails
    print('%s  clean: exit %d, %d FAIL lines' % ('PASS' if ok else 'FAIL', code, len(fails)))
    if not ok:
        print(out)
        bad += 1
    every = {l for l, _ in pc.ZIPS} | {l for l, _ in pc.FOLDERS}
    for plant, (check, labels) in PLANTS.items():
        code, fails, out = run(plant, a.exe)
        want = {(l, check) for l in (labels if labels is not None else every)}
        ok = code == 1 and fails == want
        extra, missing = sorted(fails - want), sorted(want - fails)
        print('%s  plant %-18s exit %d; fails exactly %s on %d capture(s)%s%s' % ('PASS' if ok else 'FAIL', plant, code, check, len(fails),
              '' if not extra else '; EXTRA ' + str(extra), '' if not missing else '; MISSING ' + str(missing)))
        if not ok:
            bad += 1
    print('%d problem(s)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

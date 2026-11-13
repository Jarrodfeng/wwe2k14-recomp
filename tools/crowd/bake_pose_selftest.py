r"""Lane 155 (S101): the planted faults of bake_pose_check.py, each must exit 1 and fail ONLY its named check (a positive control: a plant that passes means the check cannot see that fault).
  py -I tools\crowd\bake_pose_selftest.py        exit 0 = every plant fails its named check
Runs on Check B (every 3rd frame), about 30 s."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'bake_pose_check.py')
BROCK = 'brock=out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
# plant -> the checks that must FAIL (others may fail as a consequence of the same fault, listed here when they do)
PLANTS = {'record_prev': {'bones', 'perm'}, 'identity_perm': {'bones'}, 'type_shift': {'bones', 'perm'}, 'one_frame_perm': {'perm'}}


def run(args):
    p = subprocess.run([sys.executable, '-I', TOOL, BROCK, '--frames', '0:341:3'] + args, capture_output=True, text=True, timeout=900)
    failed = {ln.split()[2] for ln in p.stdout.splitlines() if ln.startswith('FAIL ')}
    return p.returncode, failed, p.stdout


bad = 0
rc, failed, out = run([])
if rc != 0:
    print('FAIL: the clean run fails: %s' % sorted(failed))
    print(out[-1500:])
    bad += 1
else:
    print('clean run: 0 FAIL')
for plant, want in PLANTS.items():
    rc, failed, out = run(['--plant', plant])
    ok = rc == 1 and failed == want
    print('plant %-14s %s (failed checks %s, wanted %s)' % (plant, 'caught' if ok else 'NOT CAUGHT', sorted(failed), sorted(want)))
    bad += 0 if ok else 1
print('bake_pose_selftest:', 'PASS' if not bad else '%d problem(s)' % bad)
sys.exit(1 if bad else 0)

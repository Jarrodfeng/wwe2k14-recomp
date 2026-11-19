"""run_dirs_selftest (S103 PSO): the per-run output folder helper + the behavioural twin of the serve_equal race. 0 game runs, seconds.

  py -I tools\\scene\\run_dirs_selftest.py

Unit: --out wins; else GATE_CHECK_DIR\\<name>; else a per-run folder with the pid; finish() removes a per-run folder on PASS, keeps it on FAIL, never touches --out / the gate's folder.
Twin: two copies of a fake check run AT ONCE, each writing its own reference + served files into its output folder, then reading them back and comparing (what serve_equal does).
  fixed shared folder (the old default): at least one copy must FAIL (they overwrite each other);   run_out folder (the fix): both must PASS.
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_dirs  # noqa: E402

fails = []


def check(ok, text):
    print(('PASS' if ok else 'FAIL') + '  ' + text)
    if not ok:
        fails.append(text)


FAKE = r'''
import os, sys, time
sys.path.insert(0, %r)
import run_dirs
mode, tag, shared = sys.argv[1], sys.argv[2], sys.argv[3]
out = shared if mode == 'fixed' else run_dirs.run_out('twin', None, env={})
os.makedirs(out, exist_ok=True)
for i in range(60):
    open(os.path.join(out, 'ref_%%d.txt' %% i), 'w').write(tag * 200)
    time.sleep(0.005)
    open(os.path.join(out, 'served_%%d.txt' %% i), 'w').write(tag * 200)
    time.sleep(0.005)
bad = 0
for i in range(60):
    if open(os.path.join(out, 'ref_%%d.txt' %% i)).read() != open(os.path.join(out, 'served_%%d.txt' %% i)).read() or open(os.path.join(out, 'ref_%%d.txt' %% i)).read() != tag * 200:
        bad += 1
rc = run_dirs.finish(out, 1 if bad else 0, quiet=True) if mode != 'fixed' else (1 if bad else 0)
print('bad', bad)
sys.exit(rc)
''' % HERE


def twin(mode, td):
    p = os.path.join(td, 'fake_%s.py' % mode)
    open(p, 'w').write(FAKE)
    shared = os.path.join(td, 'shared_fixed_folder')
    procs = [subprocess.Popen([sys.executable, '-I', p, mode, tag, shared], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) for tag in 'ab']
    rcs = [pr.wait() for pr in procs]
    return rcs


def main():
    with tempfile.TemporaryDirectory() as td:
        env = {'GATE_CHECK_DIR': os.path.join(td, 'chk')}
        r = run_dirs.run_out('x', os.path.join(td, 'mine'), env=env)
        check(str(r) == os.path.join(td, 'mine') and not r.owned, '--out wins over GATE_CHECK_DIR and is never owned')
        r = run_dirs.run_out('x', None, env=env)
        check(str(r) == os.path.join(td, 'chk', 'x') and not r.owned, 'GATE_CHECK_DIR\\<name> when there is no --out')
        env.pop('GATE_CHECK_DIR')
        a, b = run_dirs.run_out('x', None, env=env, pid=111), run_dirs.run_out('x', None, env=env, pid=222)
        check(a != b and a.endswith('x_111') and a.owned and os.sep + 'out' + os.sep in a, 'per-run folder out\\<name>_<pid>: two runs never share one')
        # finish()
        d = tempfile.mkdtemp(dir=td)
        mine = run_dirs.RunOut(d)
        mine.owned = True
        check(run_dirs.finish(mine, 1, quiet=True) == 1 and os.path.isdir(d), 'FAIL keeps the per-run folder')
        check(run_dirs.finish(mine, 0) == 0 and not os.path.exists(d), 'PASS removes the per-run folder')
        d2 = tempfile.mkdtemp(dir=td)
        check(run_dirs.finish(run_dirs.RunOut(d2), 0) == 0 and os.path.isdir(d2), 'PASS never removes --out / the gate folder (not owned)')
        # the twin
        t0 = time.time()
        rf = twin('fixed', td)
        check(any(rc != 0 for rc in rf), 'twin: two copies on ONE fixed folder: at least one FAILS (rcs %s)' % rf)
        rr = twin('run_out', td)
        check(all(rc == 0 for rc in rr), 'twin: two copies on run_out folders: both PASS (rcs %s)' % rr)
        print('INFO  twin took %.1f s' % (time.time() - t0))
    print('PASS  run_dirs_selftest' if not fails else 'FAIL  run_dirs_selftest: %d' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

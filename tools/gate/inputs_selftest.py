r"""S102 lane M1 (review I): tools\gate\inputs.py restore is safe when several gates restore the same input at once.
Everything happens in a temp folder standing in for the main checkout (RDR_MAIN_CHECKOUT); the real out\ is never touched.
  py -I tools\gate\inputs_selftest.py
Checks: N parallel restorers -> exactly one "restored", the rest "present", the folder whole, no .part / .lock left; a held lock is honoured (a restorer waits and
then fails with a timeout instead of unpacking under the holder: the planted fault); a half dest folder is replaced; an existing whole dest is never removed.
"""
import os, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import inputs as gi

spec = gi.INPUTS['bryan']
ok = True


def check(name, cond, why=''):
    global ok
    ok &= bool(cond)
    print('%s  %s %s' % ('PASS' if cond else 'FAIL', name, why))


def run(main, wait='900'):
    env = dict(os.environ, RDR_MAIN_CHECKOUT=str(main), RDR_INPUTS_LOCK_WAIT_S=wait)
    return subprocess.Popen([sys.executable, '-I', str(HERE / 'inputs.py'), 'restore', 'bryan'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


with tempfile.TemporaryDirectory(prefix='inputs_selftest_') as td:
    main = Path(td)
    zp = main / spec['zip']
    zp.parent.mkdir(parents=True)
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('manifest.json', '{"files": []}')
        z.writestr('capture/images.bin', os.urandom(1 << 20))
        for i in range(300):
            z.writestr('capture/frames/frame_%06d.bin' % i, os.urandom(20000))
    dest = main / spec['dest']
    procs = [run(main) for _ in range(5)]
    outs = [(p.wait(timeout=300), p.stdout.read().strip()) for p in procs]
    results = [o[1].split()[-1] if o[1] else '' for o in outs]
    check('5 parallel restorers all exit 0', all(o[0] == 0 for o in outs), str(outs))
    check('exactly one restored, the rest present', results.count('restored') == 1 and results.count('present') == 4, str(results))
    check('the folder is whole', gi._present(dest) and len(list((dest / 'capture' / 'frames').glob('*.bin'))) == 300)
    left = [p.name for p in dest.parent.iterdir() if '.part' in p.name or p.name.endswith('.lock')]
    check('no .part or .lock left behind', not left, str(left))
    check('an existing whole folder is left alone (second run says present)', run(main).communicate()[0].strip().endswith('present'))
    # planted fault: a lock held by someone else must be honoured (the old code unpacked anyway and could delete the other's folder)
    import shutil
    shutil.rmtree(dest)
    lock = dest.with_name(dest.name + '.lock')
    lock.write_text('99999')
    p = run(main, wait='2')
    out, _ = p.communicate(timeout=120)
    check('a held lock is honoured: the restorer times out and unpacks nothing', p.returncode != 0 and not dest.exists() and 'lock' in out.lower(), out.strip()[-120:])
    lock.unlink()
    # a half dest folder (no manifest) is replaced by a whole one
    (dest).mkdir(parents=True)
    (dest / 'junk.txt').write_text('half')
    p = run(main)
    out, _ = p.communicate(timeout=120)
    check('a half folder is replaced', p.returncode == 0 and gi._present(dest) and not (dest / 'junk.txt').exists(), out.strip()[-80:])
print('inputs_selftest: %s' % ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)

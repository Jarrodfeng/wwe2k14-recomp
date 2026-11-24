"""S106 C5FF R1: unit + one-child probe for runlock.ensure_lock(alone=True) / alone_held_by_us (review_C5F.md R1).

  py -I tools\\harness\\runlock_alone_unit.py                 the unit (no process started, no real lock touched; ~1 s)
  py -I tools\\harness\\runlock_alone_unit.py --probe ...     internal: the script alone_selftest.ps1 step 12 re-runs under `runlock.ps1 run -Alone`

The fault (review R1): since C5F the pause record carries WWE_ALONE_TOKEN (runlock_alone.ps1), but alone_held_by_us() compared it with
WWE2K14_RUNLOCK_TOKEN: always False, so ensure_lock(alone=True) re-ran itself under `runlock.ps1 run -Alone` forever (an unbounded pwsh + python chain).
The unit builds a C5F-layout held pause record (byte 0 locked, JSON from byte 1, token = the WWE_ALONE_TOKEN value) in a scratch folder.
No load: the probe mode bounds itself (it counts its own starts and refuses to re-enter past 4).
"""
# entry: alone_held_by_us
import json
import msvcrt
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
RL = None   # the runlock module under test (imported lazily: the probe can be pointed at an OLD copy)

FAILS = []


def ok(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


def _held_pause_file(path, token):
    """A C5F-layout pause: byte 0 locked by this handle, the record from byte 1 (runlock_alone.ps1 Enter-RunLockAlone). Returns the fd (keep it open)."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_BINARY)
    os.lseek(fd, 0, 0)
    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
    rec = {'pid': os.getpid(), 'pid_start': 0, 'token': token, 'name': 'unit', 'since': 1, 'until': 2 ** 40}
    os.lseek(fd, 1, 0)
    os.write(fd, json.dumps(rec).encode('utf-8'))
    return fd


def unit():
    global RL
    import runlock as RL  # noqa: E402
    tmp = tempfile.mkdtemp(prefix='runlock_alone_unit_')
    path = os.path.join(tmp, 'alone.lock')
    saved = {k: os.environ.get(k) for k in ('WWE_ALONE_FILE', 'WWE_ALONE_TOKEN', 'WWE2K14_RUNLOCK_TOKEN')}
    try:
        os.environ['WWE_ALONE_FILE'] = path
        # the C5F layout: the record's token is the DEDICATED alone token; the run-lock token is a different string
        os.environ['WWE2K14_RUNLOCK_TOKEN'] = 'runlock-token-aaaa'
        os.environ['WWE_ALONE_TOKEN'] = 'alone-token-bbbb'
        ok(RL.alone_held_by_us() is False, 'no pause file: not held')
        fd = _held_pause_file(path, 'alone-token-bbbb')
        try:
            ok(RL.alone_held_by_us() is True, 'R1 a C5F-layout held pause record naming WWE_ALONE_TOKEN -> True')
            os.environ['WWE_ALONE_TOKEN'] = 'someone-elses-token'
            ok(RL.alone_held_by_us() is False, 'a pause that names another token -> False (the detector can say no)')
            os.environ.pop('WWE_ALONE_TOKEN')
            ok(RL.alone_held_by_us() is False, 'no WWE_ALONE_TOKEN in the environment (an exclusive hold that is not alone) -> False')
            os.environ['WWE_ALONE_TOKEN'] = 'runlock-token-aaaa'
            ok(RL.alone_held_by_us() is False, 'the run-lock token is not the pause token -> False')
        finally:
            os.close(fd)   # closing the handle frees the byte-0 lock
        os.environ['WWE_ALONE_TOKEN'] = 'alone-token-bbbb'
        ok(RL.alone_held_by_us() is False, 'the pause file with nothing holding it -> False (a stale record is no pause)')
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    return 1 if FAILS else 0


def probe(argv):
    """argv: SCRATCH_DIR RUNLOCK_PS1 LOCK_DIR [RUNLOCK_PY_DIR]. Appends one line per start to SCRATCH_DIR\\starts.txt and one to ran.txt after ensure_lock returns.
    Refuses to call ensure_lock from the 5th start on (a bound, so the old fault ends in a count of 4 instead of a runaway chain)."""
    global RL
    scratch, ps1, lock_dir = argv[0], argv[1], argv[2]
    sys.path.insert(0, argv[3] if len(argv) > 3 else HERE)   # argv[3] = a folder holding the runlock.py to test (default: this checkout's)
    import runlock as RL  # noqa: E402
    starts = os.path.join(scratch, 'starts.txt')
    with open(starts, 'a') as f:
        f.write('start %s alone_token=%s\n' % (os.getpid(), bool(os.environ.get('WWE_ALONE_TOKEN'))))
    n = sum(1 for _ in open(starts))
    if n >= 4:
        with open(os.path.join(scratch, 'ran.txt'), 'a') as f:
            f.write('BOUND %d\n' % n)
        return 0
    RL.RUNLOCK_PS1 = ps1
    RL.LOCK_DIR = lock_dir
    RL.LOCK_JSON = os.path.join(lock_dir, 'lock.json')
    RL.ensure_lock('alone_probe', alone=True)
    with open(os.path.join(scratch, 'ran.txt'), 'a') as f:
        f.write('ran %d\n' % os.getpid())
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--probe':
        sys.exit(probe(sys.argv[2:]))
    code = unit()
    print('runlock_alone_unit: %s' % ('%d FAIL' % len(FAILS) if FAILS else 'all ok'))
    sys.exit(code)

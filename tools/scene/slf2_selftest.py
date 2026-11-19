#!/usr/bin/env python3
r"""SLF2 selftest (S103): the stage_look camera race. Two spellings of one capture ('/' and '\') must be ONE memo entry in frame_rig (one fview camera invocation), a failure must not be kept by the memo,
and a memo key holding a non-canonical path must be refused.  py -I tools\scene\slf2_selftest.py [--plant]
--plant restores the old spelling (canon = identity): the twin must FAIL (exit 1)."""
import os, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
import frame_rig as fr  # noqa: E402


def main():
    plant = '--plant' in sys.argv
    if plant:
        fr.canon = lambda p: p
    tmp = tempfile.mkdtemp(prefix='slf2_')
    fr.SESSION = fr.RigSession(serve=False, prep_root=tmp)
    calls = []

    def fake(fview, capture, n, game, work):
        calls.append(capture)
        time.sleep(0.2)
        return ('cam', 'eye', 1, None), 'ok'
    fr._stream_camera_raise = fake
    cap = os.path.join(tmp, 'caps', 'cap_x')
    spellings = [cap.replace('\\', '/'), cap.replace('/', '\\')]
    out = []
    ts = [threading.Thread(target=lambda c=c: out.append(fr.prep_camera('fview', c, 20, 'g', None))) for c in spellings]
    [t.start() for t in ts]
    [t.join() for t in ts]
    ok = True
    if len(calls) != 1:
        print('FAIL: %d camera invocations for two spellings of one capture (want 1)' % len(calls)); ok = False
    # a failure is not memoised
    n_try = []

    def flaky(fview, capture, n, game, work):
        n_try.append(1)
        if len(n_try) == 1:
            raise fr.CameraError('boom')
        return ('cam', 'eye', 2, None), 'ok'
    fr._stream_camera_raise = flaky
    r1 = fr.prep_camera('fview', cap, 21, 'g', None)
    r2 = fr.prep_camera('fview', cap, 21, 'g', None)
    if not (r1[0] is None and r2[0] is not None):
        print('FAIL: a failed camera step was kept by the memo: %r %r' % (r1, r2)); ok = False
    # the key assert
    try:
        fr.SESSION.memo(('camera', cap.replace('\\', '/').replace('/', '\\') + '/..\\x', 1), lambda: 1)
        print('FAIL: a non-canonical path key was accepted'); ok = False
    except AssertionError:
        pass
    print('SLF2 selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

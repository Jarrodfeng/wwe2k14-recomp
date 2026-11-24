"""Selftest for q name / unit / cover (S110 lane NAMES). 0 game runs. Needs out/harness/index (else SKIP).
Pins: the Lua binding walk (51 register-site decodes, 1,212 raw rows of which 2 are not tables = 1,210 names; DRIVE.md's
"1,211" counts one more undecodable row, see the hand-back), 11 globals, two cited addresses, and qcover's totals before
names are added (COVER.md: 83,012 functions, 45.1 % mechanical). Exit 1 = a check failed."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402

fails = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        fails.append(what)


def main():
    if not (os.path.exists(qnames.INDEX_DB) and os.path.exists(qnames.IMAGE)):
        print('SKIP: no index')
        return 0
    img = qnames.Image()
    con = qnames.ro(qnames.INDEX_DB)
    tabs = qnames.lua_tables(img, con)
    check('lua tables decoded', len(tabs), 51)
    check('raw rows', sum(len(r) for _, _, r in tabs), 1212)
    check('rows that are names', sum(1 for _, _, r in tabs for x in r if x[0] != '?bad'), 1210)
    check('globals', len(qnames.lua_globals(img, con)), 11)
    for addr, want in (('0x826D44A0', 'yGame.SetMotion'), ('0x827381B0', 'yMatch.CallFunction')):
        out = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'name', addr], capture_output=True, text=True).stdout
        check('q name ' + addr, want in out, True)
    import qcover
    r = qcover.compute(32, use_names=False)
    check('functions with a body', r['functions'], 83012)
    check('mechanical % (callers and callees complete)', round(100.0 * r['callers_and_callees_complete'] / r['functions'], 1), 45.1)
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

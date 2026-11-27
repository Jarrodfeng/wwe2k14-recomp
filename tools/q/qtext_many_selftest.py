"""Self-test of `q text ID [ID...]` (S111 QFIX4): `py -I tools/q/qtext_many_selftest.py`   (0 game runs)

A. routing (q.bare_ours): `text 12` / `text 12 40356` / `text 0x10 7 --lang 1` go to the string-table lookup; `text hello world` / `text 12 word` stay the code search.
B. text_many on a planted cmd_text: every id is looked up, a missing id is said and exits 1, --json carries every id.
C. planted twins that must FAIL: the old routing (at most one positional) sends two ids to the code search; a lookup that only reads the first id misses the second.
D. the real tables (skipped without the game files): `q text 40356 45004` finds both.
Exit 0 = pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import contextlib
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import q  # noqa: E402
import qgx  # noqa: E402


def old_bare(argv):
    """the routing before QFIX4 (at most one numeric positional)"""
    pos, i = [], 1
    while i < len(argv):
        x = argv[i]
        i += 2 if x == '--lang' else 1
        if x not in ('--lang', '--all', '--json', '-h', '--help') and not x.startswith('--lang='):
            pos.append(x)
    return not (len(pos) <= 1 and all(re.fullmatch(r'0[xX][0-9a-fA-F]+|\d+', x) for x in pos))


def routing(fn):
    bad = []
    for argv in (['text', '12'], ['text', '12', '40356'], ['text', '0x10', '7', '--lang', '1'], ['text', '--all', '5', '6', '7']):
        if fn(argv):
            bad.append(f'{argv}: want the string-table lookup')
    for argv in (['text', 'hello', 'world'], ['text', '12', 'word'], ['text', 'GetString']):
        if not fn(argv):
            bad.append(f'{argv}: want the code search')
    return bad


def run_many(tids, first_only=False):
    class A:
        lang, all, json = 0, False, False
    tab = {1: [{'pac': 'p', 'entry': '0000', 'leaf': 0, 'lang': 0, 'text': 'one'}], 2: [{'pac': 'p', 'entry': '0000', 'leaf': 0, 'lang': 0, 'text': 'two'}]}
    keep = qgx.cmd_text
    qgx.cmd_text = lambda t, lang=0, allf=False: tab.get(t, []) if not (first_only and t != tids[0]) else []
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            rc = qgx.text_many(tids, A)
    finally:
        qgx.cmd_text = keep
    return rc, buf.getvalue()


def many_checks(first_only=False):
    bad = []
    rc, o = run_many([1, 2], first_only)
    if rc != 0 or "'one'" not in o or "'two'" not in o:
        bad.append(f'two ids: both looked up, got rc {rc} {o!r}')
    rc, o = run_many([1, 99], first_only)
    if rc != 1 or '99: not in the string tables' not in o or '2 ids, 1 found' not in o:
        bad.append(f'a missing id: said, rc 1, got rc {rc} {o!r}')
    return bad


def main():
    fails = routing(q.bare_ours) + many_checks()
    twins = {'old routing (one positional only)': lambda: not routing(old_bare), 'lookup reads the first id only': lambda: not many_checks(first_only=True)}
    missed = [n for n, f in twins.items() if f()]
    skipped = None
    try:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = qgx.text_main(['40356', '45004'])
        o = buf.getvalue()
        if rc != 0 or 'OVERALL' not in o or '45004 =' not in o:
            fails.append(f'real tables: 40356 = OVERALL and 45004 found, got rc {rc} {o[:200]!r}')
    except Exception as e:      # noqa: BLE001 - no game files
        skipped = f'{type(e).__name__}: {e}'
    for f in fails:
        print('FAIL', f)
    for m in missed:
        print('NOT CAUGHT', m)
    print(f"qtext_many_selftest: {'ok' if not fails else 'FAILED'}; {len(twins) - len(missed)}/{len(twins)} planted twins caught; "
          + (f'real tables skipped ({skipped})' if skipped else 'real tables ok'))
    return 1 if fails else (2 if missed else 0)


if __name__ == '__main__':
    sys.exit(main())

r"""Self-test of `q field OFF --writes --values` (S111 lane QWRITERS; tools/q/qfieldvals.py).

    py -I tools/q/qfieldvals_selftest.py [--root .] [--no-real]

Fixture (synthetic generated source + image, nothing from the game): one function stores +1208 = 1 (li), +232 = 1, +224 = 0 through
a0, a float-sized constant by lis + ori, a register it cannot trace, and a store through another root; a second stores +1208 = 0
alone. The same checks run twice: with the value tracer (must pass) and with the PLANTED TWIN, the tracer disabled (every value
`reg rN`): the twin must FAIL them, else the checks prove nothing.
Real data (skipped with --no-real or without the field index / generated sources): the facts of docs/research/s111/KIND3ROUTE.md
re-found by the tool: the reel setup sub_827054C0 stores +1208 = 1 (its DRTR enable goes through the paired setter call, so no store
of the enable word is in its listing) and the teardown sub_82705B70 stores +1208 = 0 together with the DRTR enable word (global
0x836F3AA8 +8 = 0, seen with --co-any: another root than the character's, so a same-root list does not hold it);
the match task sub_8273D590 clears +1208 and no +224 / +232 / DRTR enable beside it.
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf   # noqa: E402
import qfield               # noqa: E402
import qfieldvals           # noqa: E402


def fn(addr, lines):
    out = [f'DEFINE_REX_FUNC(sub_{addr:08X}) {{', '\tREX_FUNC_PROLOGUE();']
    for ln in lines:
        out.append(f'\t// {ln}')
        out.append('\t/* body */')
    out.append('}')
    return '\n'.join(out) + '\n'


def make_fixture(d):
    gen = os.path.join(d, 'generated', 'default')
    os.makedirs(gen, exist_ok=True)
    funcs = [
        (0x82000100, ['li r11,1', 'stw r11,1208(r3)', 'li r10,0', 'stw r10,232(r3)', 'stw r4,224(r3)',
                      'lis r9,16256', 'ori r9,r9,1', 'stw r9,16(r3)', 'lis r8,-31853', 'lwz r8,80(r8)', 'stw r10,8(r8)', 'blr ']),
        (0x82000200, ['li r11,0', 'stw r11,1208(r3)', 'blr ']),
    ]
    with open(os.path.join(gen, 'wwe2k14_recomp.0.cpp'), 'w', encoding='utf-8') as f:
        f.write('#include "x.h"\n' + ''.join(fn(a, ls) for a, ls in funcs))
    ip = os.path.join(d, 'image.bin')
    with open(ip, 'wb') as f:
        f.write(bytes(0x100100))
    return gen, ip


def run(argv):
    buf, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = qfield.main(argv + ['--json'])
    if rc != 0:
        raise AssertionError(f'q field {argv} -> rc {rc}: {buf.getvalue()} {err.getvalue()}')
    return json.loads(buf.getvalue())


def by_func(res, f):
    return [r for r in res['rows'] if r['func'] == f]


def co_of(row, off, root=None):
    return [c for c in row['co'] if c['off'] == off and (root is None or c['root'] == root)]


def fixture_failures(base):
    """-> the failed checks, as text"""
    bad = []

    def check(c, msg):
        if not c:
            bad.append(msg)
    res = run(['1208', '--writes', '--values'] + base)
    a, = by_func(res, 'sub_82000100')
    z, = by_func(res, 'sub_82000200')
    check(a['val']['text'] == '1' and a['val']['imm'] == 1, f'li 1 traced to the store (got {a["val"]["text"]})')
    check(z['val']['text'] == '0', f'li 0 traced (got {z["val"]["text"]})')
    check([c['val']['text'] for c in co_of(a, 232)] == ['0'], 'co +232 = 0')
    check([c['val']['text'] for c in co_of(a, 16)] == ['0x3F800001'], f'co +16 = lis + ori constant (got {[c["val"]["text"] for c in co_of(a, 16)]})')
    check([c['val']['text'] for c in co_of(a, 224)] == ['reg r4=a1+0'], f'co +224 = an argument register (got {[c["val"]["text"] for c in co_of(a, 224)]})')
    check(z['co'] == [] and z['co_more'] == 0, 'the lone clear has no co')
    res = run(['1208', '--writes', '--values', '--co-any'] + base)
    a, = by_func(res, 'sub_82000100')
    other = [c for c in a['co'] if c['root'] != a['root']]
    check([(c['off'], c['root'], c['val']['text']) for c in other] == [(8, 'g:83930050', '0')],
          f'--co-any adds the store through the global-rooted pointer (+8 = 0): {other}')
    res = run(['1208', '--writes', '--values', '--co', '1'] + base)
    a, = by_func(res, 'sub_82000100')
    check(len(a['co']) == 1 and a['co_more'] == 2, f'--co 1 keeps one and counts the rest (got {len(a["co"])} + {a["co_more"]})')
    return bad


def fixture_checks(fails):
    d = tempfile.mkdtemp(prefix='qfv_fixture_')
    try:
        gen, img = make_fixture(d)
        base = ['--gen', gen, '--image', img, '--db', os.path.join(d, 'f.db')]
        run(['1208', '--writes'] + base)                       # builds the index
        base += ['--no-rebuild']
        plain = run(['1208', '--writes'] + base)
        check(all('val' not in r and 'co' not in r and 'rid' not in r for r in plain['rows']), 'without --values the rows carry no val / co / rid (output unchanged)', fails)
        bad = fixture_failures(base)
        check(not bad, 'fixture with the value tracer: ' + ('all checks hold' if not bad else '; '.join(bad)), fails)
        orig = qfieldvals.trace_stores
        qfieldvals.trace_stores = lambda f, t, trace=True: orig(f, t, False)       # the planted twin: the value tracer disabled
        try:
            twin = fixture_failures(base)
        finally:
            qfieldvals.trace_stores = orig
        check(len(twin) >= 3, f'planted twin (tracer off) FAILS the checks ({len(twin)} fail: {twin[:2]} ...)', fails)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def real_checks(root, fails):
    gen, image, db = bf.default_paths(root)
    if not os.path.exists(db):
        print('skip real proofs: no field index at', db)
        return
    base = ['--root', root, '--no-rebuild']
    res = run(['1208', '--writes', '--values', '--co-any', '--co', '40', '--limit', '5000'] + base)
    check(res['count'] > 100 and res['functions'] > 50, f'the +1208 writers are all there ({res["count"]} accesses in {res["functions"]} functions)', fails)
    setup = by_func(res, 'sub_827054C0')
    check(len(setup) == 1 and setup[0]['val']['text'] == '1', f'reel setup sub_827054C0 stores +1208 = 1 (got {[r["val"]["text"] for r in setup]})', fails)
    tear = by_func(res, 'sub_82705B70')
    drtr = co_of(tear[0], 8, 'g:836F3AA8') if tear else []
    check(len(tear) == 1 and tear[0]['val']['text'] == '0' and [c['val']['text'] for c in drtr] == ['0'],
          'reel teardown sub_82705B70 stores +1208 = 0 together with the DRTR enable word g:836F3AA8 +8 = 0', fails)
    task = by_func(res, 'sub_8273D590')
    clears = [r for r in task if r['val']['text'] == '0']
    check(clears and all(not co_of(r, 224) and not co_of(r, 232) and not [c for c in r['co'] if c['root'] == 'g:836F3AA8'] for r in task),
          'match task sub_8273D590 clears +1208 with no +224 / +232 / DRTR enable beside it', fails)
    unknown = [r for r in res['rows'] if r['rw'] == 'W' and r['val']['text'] == '?']
    check(not unknown, f'every writer\'s listing was found ({len(unknown)} stores with val ?)', fails)


def check(cond, msg, fails):
    if not cond:
        fails.append(msg)
        print('FAIL', msg)
    else:
        print('ok  ', msg)


def main():
    root = '.'
    args = sys.argv[1:]
    if '--root' in args:
        root = args[args.index('--root') + 1]
    fails = []
    fixture_checks(fails)
    if '--no-real' not in args:
        real_checks(root, fails)
    print('PASS' if not fails else f'FAIL ({len(fails)})')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

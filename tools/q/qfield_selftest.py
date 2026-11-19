r"""Self-test of the field index (S103 lane QF): a fixture (synthetic generated sources + image, nothing from the game) and the
real-data proofs of the findings the index was built for. Gate lane `qf_fields`.

    py -I tools/q/qfield_selftest.py [--root .] [--no-real]

Fixture checks (exact sets, so a wrong fold fails): the typed writers of +108, the planted twin (an UNTYPED function writing
+108, and a typed one writing +112: neither may appear in the typed +108 query), a join that disagrees (the base must not
survive it), the global fold ([0x83930050]+32) and its flow into a callee, and the mutation check: the same fixture with the
slot's offset changed 108 -> 112 must move that function from the +108 answer to the +112 answer.
Real-data proofs (skipped with --no-real or when generated/ is missing; the index is built on first use, ~10 s):
  1. writers of +108 on vtable 0x8201F294 include sub_825BA448 (the stage-page fade timer);
  2. readers of [0x83930050]+32 include sub_82C302A0 (the task runner) and sub_82C2FEB8 (the registration);
  3. the reader of device +592 (sub_82513A38 0x82513B18) is typed 'r:82513B08' = the return of sub_82BA7748, and the writers of
     +592 AND +600 as 8-byte pairs are the known candidate list (sub_827F70D0 among them)."""
import contextlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf   # noqa: E402
import qfield               # noqa: E402

VT = 0x82100000


def fn(addr, lines, labels=()):
    out = [f'DEFINE_REX_FUNC(sub_{addr:08X}) {{', '\tREX_FUNC_PROLOGUE();']
    for ln in lines:
        if ln.startswith('loc_'):
            out.append(f'{ln}')
            continue
        if ln.startswith('goto '):
            out[-1] = out[-1]
            out.append(f'\tif (cond) goto {ln[5:]};')
            continue
        out.append(f'\t// {ln}')
        out.append('\t/* body */')
    out.append('}')
    return '\n'.join(out) + '\n'


def make_fixture(d, slot_off=108):
    gen = os.path.join(d, 'generated', 'default')
    os.makedirs(gen, exist_ok=True)
    funcs = [
        # ctor of the type: stores the vtable constant at a0+0
        (0x82000010, ['lis r11,-32240', 'stw r11,0(r3)', 'blr ']),
        # slot 0 of the vtable: writes +slot_off (108 in the good fixture)
        (0x82000100, [f'stw r4,{slot_off}(r3)', 'blr ']),
        # planted twin 1: an UNTYPED function writing +108 (not a slot, not a ctor)
        (0x82000200, ['stw r4,108(r3)', 'blr ']),
        # slot 1 of the vtable: writes the adjacent wrong offset +112
        (0x82000300, ['stw r4,112(r3)', 'blr ']),
        # global fold: r11 = [0x83930050]; read +32; pass it to the callee
        (0x82000400, ['lis r11,-31853', 'lwz r11,80(r11)', 'lwz r10,32(r11)', 'mr r3,r11', 'bl 0x82000500', 'blr ']),
        # the callee reads +32 of its a0
        (0x82000500, ['lwz r10,32(r3)', 'blr ']),
        # a join that disagrees: r11 = a1 on one path, a0 on the other; +40 after the join has no known base
        (0x82000600, ['cmpwi cr6,r5,0', 'beq cr6,0x82000614', 'goto loc_82000614', 'mr r11,r4', 'b 0x82000618', 'goto loc_82000618',
                      'loc_82000614:', 'mr r11,r3', 'loc_82000618:', 'lwz r10,40(r11)', 'blr ']),
        # a join that agrees: r11 = a0 on both paths
        (0x82000700, ['cmpwi cr6,r5,0', 'beq cr6,0x82000714', 'goto loc_82000714', 'mr r11,r3', 'b 0x82000718', 'goto loc_82000718',
                      'loc_82000714:', 'mr r11,r3', 'loc_82000718:', 'lwz r10,44(r11)', 'blr ']),
    ]
    text = '#include "x.h"\n' + ''.join(fn(a, ls) for a, ls in funcs)
    with open(os.path.join(gen, 'wwe2k14_recomp.0.cpp'), 'w', encoding='utf-8') as f:
        f.write(text)
    img = bytearray(0x100100)
    # vtable at VT: slot 0 = sub_82000100, slot 1 = sub_82000300, then a zero word (end)
    struct.pack_into('>II', img, VT - bf.IMAGE_BASE, 0x82000100, 0x82000300)
    # a word at each function start so "is a function" is by the generated list, not the image
    ip = os.path.join(d, 'image.bin')
    with open(ip, 'wb') as f:
        f.write(img)
    return gen, ip


def run(argv):
    buf = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = qfield.main(argv + ['--json'])
    if rc != 0:
        raise AssertionError(f'q field {argv} -> rc {rc}: {buf.getvalue()} {err.getvalue()}')
    return json.loads(buf.getvalue())


def funcs_of(res):
    return sorted({r['func'] for r in res['rows']})


def check(cond, msg, fails):
    if not cond:
        fails.append(msg)
        print('FAIL', msg)
    else:
        print('ok  ', msg)


def fixture_checks(fails):
    d = tempfile.mkdtemp(prefix='qf_fixture_')
    try:
        gen, img = make_fixture(d)
        db = os.path.join(d, 'f.db')
        base = ['--gen', gen, '--image', img, '--db', db]
        res = run(['108', '--vtable', hex(VT), '--writes'] + base)
        check(funcs_of(res) == ['sub_82000100'], f'typed writers of +108 = [sub_82000100] (got {funcs_of(res)})', fails)
        res = run(['108', '--writes'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000100', 'sub_82000200'], 'untyped query sees both +108 writers incl. the planted twin', fails)
        res = run(['112', '--vtable', hex(VT), '--writes'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000300'], 'typed writers of +112 = the adjacent slot only', fails)
        res = run(['32', '--global', '0x83930050'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000400', 'sub_82000500'], f'[0x83930050]+32 readers incl. the callee via callers (got {funcs_of(res)})', fails)
        res = run(['32', '--global', '0x83930050', '--strict'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000400'], '--strict drops the inferred-from-callers reader', fails)
        res = run(['40', '--reads'] + base + ['--no-rebuild'])
        check([r['root'] for r in res['rows']] == ['u'], 'a disagreeing join gives no base (root u)', fails)
        res = run(['44', '--reads'] + base + ['--no-rebuild'])
        check([r['root'] for r in res['rows']] == ['a0'], 'an agreeing join keeps the base (a0)', fails)
        t = run(['type', hex(VT)] + base + ['--no-rebuild'])
        offs = {e['off']: e for e in t['layout']}
        check(sorted(offs) == [0, 108, 112] and offs[0]['e_writer'] == 'sub_82000010' and offs[108]['e_writer'] == 'sub_82000100'
              and offs[112]['e_writer'] == 'sub_82000300', 'type layout: +0 (the constructor), +108 and +112, one writer each', fails)
        # the planted wrong offset: the slot writes +112 instead of +108
        gen2, img2 = make_fixture(os.path.join(d, 'm'), slot_off=112)
        base2 = ['--gen', gen2, '--image', img2, '--db', os.path.join(d, 'm.db')]
        res = run(['108', '--vtable', hex(VT), '--writes'] + base2)
        check(funcs_of(res) == [], 'mutation: slot moved to +112 -> the +108 answer is empty (the wrong offset is caught)', fails)
        res = run(['112', '--vtable', hex(VT), '--writes'] + base2 + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000100', 'sub_82000300'], 'mutation: both slots now answer +112', fails)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def real_checks(root, fails):
    base = ['--root', root]
    gen, image, _db = bf.default_paths(root)
    if not os.path.exists(image):
        print('skip real proofs: no image.bin (out/harness/index)')
        return
    res = run(['108', '--vtable', '0x8201F294', '--writes'] + base)
    check('sub_825BA448' in funcs_of(res), 'proof 1: writers of +108 on vtable 0x8201F294 include sub_825BA448', fails)
    res = run(['32', '--global', '0x83930050'] + base)
    got = funcs_of(res)
    check('sub_82C302A0' in got and 'sub_82C2FEB8' in got, 'proof 2: readers of [0x83930050]+32 include sub_82C302A0 (runner) and sub_82C2FEB8', fails)
    res = run(['592', '--func', 'sub_82513A38'] + base)
    check(any(r['root'] == 'r:82513B08' and r['rw'] == 'R' and r['width'] == 8 for r in res['rows']),
          'proof 3a: the LightMapParam reader (0x82513B18) is +592 of the return of the call at 0x82513B08 (sub_82BA7748)', fails)
    both = set()
    r6 = run(['592', '--writes', '--width', '8', '--limit', '5000'] + base)
    r7 = run(['600', '--writes', '--width', '8', '--limit', '5000'] + base)
    both = set(funcs_of(r6)) & set(funcs_of(r7))
    check('sub_827F70D0' in both, f'proof 3b: the 8-byte pair writers of +592/+600 ({len(both)} functions) include sub_827F70D0', fails)


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
    import multiprocessing
    multiprocessing.freeze_support()
    sys.exit(main())

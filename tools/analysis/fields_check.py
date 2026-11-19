r"""Check for tools/analysis/fields.py (S103 lane T2): the known finds must be re-found, planted twins (wrong displacement,
wrong kind, wrong range, wrong tag) must NOT be, and a synthetic function must index exactly. Needs the local generated code.

  py -I tools\analysis\fields_check.py"""
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fields  # noqa: E402

con = fields.open_db()
fails = []


def q(cmd, disp, **kw):
    a = types.SimpleNamespace(cmd=cmd, range=None, func=None, width=None, tag=None, reg=None, eff=False, sp=False, limit=5000)
    a.__dict__.update(kw)
    kind = {'load': 'L', 'store': 'S', 'any': None, 'lea': 'A'}[cmd]
    return fields.run_query(con, kind, disp, a)


def has(rows, func, addr):
    return any(r[0] == func and r[1] == addr for r in rows)


def expect(name, ok):
    print(('PASS ' if ok else 'FAIL ') + name)
    if not ok:
        fails.append(name)


# known finds (S103 review Q3.3), each from the listing: sub_82BD4F88 reads 616(r11) at 0x82BD4FB0; sub_82BDA038 stores rec[+0] by stwx at 0x82BDA7A0
R = '0x82BD0000-0x82C14000'
expect('find: load 616 -> sub_82BD4F88 0x82BD4FB0', has(q('load', 616, range=R), 0x82BD4F88, 0x82BD4FB0))
expect('find: indexed store through a +480 pointer -> sub_82BDA038 0x82BDA7A0',
       has(q('store', 'x', range=R, tag='+480]'), 0x82BDA038, 0x82BDA7A0))
expect('find: the wrestler light object (char+3936) is handed on by sub_8259F630 0x8259F67C (A8: the block is at +3984 = +3936 + 48)',
       has(q('lea', 3936), 0x8259F630, 0x8259F67C))
expect('find: char+3984 itself is stored by name in the index (sub_82730DD8 0x82730E70)', has(q('store', 3984), 0x82730DD8, 0x82730E70))
# planted twins: each must fail
expect('twin: displacement 617 does not find the 616 load', not has(q('load', 617, range=R), 0x82BD4F88, 0x82BD4FB0))
expect('twin: a store query does not find the 616 load', not has(q('store', 616, range=R), 0x82BD4F88, 0x82BD4FB0))
expect('twin: a range that excludes it does not find it', not has(q('load', 616, range='0x82BD5000-0x82BD6000'), 0x82BD4F88, 0x82BD4FB0))
expect('twin: a wrong tag does not find it', not has(q('load', 616, range=R, tag='+480]'), 0x82BD4F88, 0x82BD4FB0))
expect('twin: a load query does not find the stwx', not has(q('load', 'x', range=R, tag='+480]'), 0x82BDA038, 0x82BDA7A0))
expect('twin: width 2 does not find the 4-byte load', not has(q('load', 616, range=R, width=2), 0x82BD4F88, 0x82BD4FB0))

# synthetic function: exact rows, tags, addresses, label re-sync
items = [('c', 'mr r31,r3'), ('c', 'lwz r11,480(r31)'), ('c', 'addi r10,r31,100'), ('c', 'stw r9,16(r10)'), ('l', 0x82000020),
         ('c', 'stwx r9,r8,r11'), ('c', 'bl 0x82000000'), ('c', 'stb r4,3(r3)'), ('c', 'lwz r9,-8(r1)')]
rows, n, bad = fields.scan_function('sub_82000000', items)
exp = [(0x82000004, 'L', 480, 'a0', 0), (0x8200000C, 'S', 16, 'a0', 100), (0x82000020, 'S', None, '[a0+480]', 0), (0x82000028, 'S', 3, None, None),
       (0x8200002C, 'L', -8, 'sp', 0)]
got = [(r[1], r[2], r[7], r[8], r[9]) for r in rows if r[2] != 'A']
expect('synthetic: rows, tags (a0 copy, addi offset, [a0+480], bl clears r3), label re-sync %s' % got, got == exp and bad == 1)
twin = list(exp)
twin[1] = (0x8200000C, 'S', 16, 'a0', 0)
expect('twin: a wrong tag offset in the expectation is rejected', got != twin)
print('fields_check: %s' % ('OK' if not fails else 'FAILED: ' + ', '.join(fails)))
sys.exit(1 if fails else 0)

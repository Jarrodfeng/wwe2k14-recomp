"""qargs_selftest: the S111 QARGS verbs on a PLANTED listing + index in a temp folder (0 game runs), each case with its twin:
  q args F        `li r5,7` that reaches the bl (A) vs one overwritten before it (B); r6 set by a caller but never read by F is dropped; a constant, an image address,
                  the caller's own in_r4 passed through, and an expression each fold to their own kind; --reg histogram, --value, --in
  q vcall --reg   an indirect call (lwz slot / mtctr / bctrl) with `li r4,42` vs one that overwrites r4
  q asm --writes  `lwzu r22,8(r23)` writes r22 (and r23); `lwz r3,0(r22)` only reads it; a `bl` clobbers r3-r12, a __savegpr helper only r0 / r11 / r12; the entry row
  q asm --writes sp+D / --reads sp+D   `stw r0,208(r1)` is the slot, `stw r0,208(r31)` is not when r31 is an ordinary pointer, and is when r31 is `mr r31,r1`
Then the real tree (skipped when the index / generated\\ is absent): the three lines the QGAPS review logged.
Run:  py -I tools/q/qargs_selftest.py"""
import contextlib
import io
import os
import re
import sqlite3
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qargs  # noqa: E402
import qdecomp  # noqa: E402
import qregs  # noqa: E402
import qx6  # noqa: E402

Q = os.path.join(HERE, 'q.py')
fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:600]))
    if not ok:
        fails.append(what)


F = 0x82000500
FUNCS = [   # name, start, [instructions]
    ('sub_82000500', F, ['stw r5,0(r3)', 'stw r4,4(r3)', 'li r6,9', 'blr']),          # reads r3, r4, r5; r6 only written
    ('sub_82000100', 0x82000100, ['li r5,7', 'li r6,3', 'li r4,1', 'bl 0x82000500', 'blr']),                       # A: r5=7 reaches
    ('sub_82000200', 0x82000200, ['li r5,7', 'li r5,5', 'li r4,1', 'bl 0x82000500', 'blr']),                       # B: the 7 is overwritten
    ('sub_82000300', 0x82000300, ['lis r5,-32256', 'addi r5,r5,4660', 'bl 0x82000500', 'blr']),                    # C: image address 0x82001234, r4 passed through
    ('sub_82000400', 0x82000400, ['lwz r5,8(r3)', 'bl 0x82000500', 'blr']),                                          # D: an expression
    ('sub_82000700', 0x82000700, ['lwz r11,0(r3)', 'lwz r10,76(r11)', 'mtctr r10', 'li r4,42', 'bctrl', 'blr']),   # E: vcall, r4 = 42
    ('sub_82000740', 0x82000740, ['lwz r11,0(r3)', 'lwz r10,76(r11)', 'mtctr r10', 'li r4,42', 'li r4,43', 'bctrl', 'blr']),   # E2: r4 overwritten
    ('sub_82000800', 0x82000800, ['mflr r12', 'bl 0x82000A00', 'stwu r1,-96(r1)', 'lwz r31,0(r3)', 'lwzu r22,8(r23)', 'lwz r3,0(r22)', 'li r5,1', 'stw r0,208(r1)',
                                   'stw r0,208(r31)', 'bl 0x82000500', 'blr']),                                                          # G1: r31 an ordinary pointer
    ('sub_82000900', 0x82000900, ['stwu r1,-96(r1)', 'mr r31,r1', 'stw r0,208(r31)', 'blr']),                                           # G2: r31 = a frame copy
    ('__savegprlr_25', 0x82000A00, ['blr']),
]


def build(root):
    gen = os.path.join(root, 'generated', 'default')
    os.makedirs(gen)
    lines, rows, edges = [], [], []
    for name, start, ins in FUNCS:
        rows.append((start, name, 4 * len(ins), len(ins), 'wwe2k14_recomp.0.cpp', len(lines) + 1))
        lines.append('DEFINE_REX_FUNC(%s) {' % name)
        for i, t in enumerate(ins):
            lines.append('\t// ' + t)
            m = re.fullmatch(r'bl (0x[0-9a-f]+)', t)
            if m:
                edges.append((start, start + 4 * i, int(m.group(1), 16), 'call'))
        lines.append('}')
    with open(os.path.join(gen, 'wwe2k14_recomp.0.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.makedirs(os.path.join(root, 'index'))
    con = sqlite3.connect(os.path.join(root, 'index', 'index.db'))
    con.row_factory = sqlite3.Row
    con.execute('CREATE TABLE funcs(addr INTEGER PRIMARY KEY, name TEXT, size INTEGER, insns INTEGER, file TEXT, line INTEGER, n_bctrl INTEGER, n_bctr INTEGER, resync INTEGER, has_body INTEGER)')
    con.execute('CREATE TABLE edges(src INTEGER, site INTEGER, dst INTEGER, kind TEXT)')
    con.executemany('INSERT INTO funcs VALUES (?,?,?,?,?,?,0,0,0,1)', rows)
    con.executemany('INSERT INTO edges VALUES (?,?,?,?)', edges)
    con.commit()
    qdecomp.GEN, qdecomp.CACHE_DIR, qdecomp.IMAGE = gen, os.path.join(root, 'cache'), os.path.join(root, 'no_image.bin')
    qdecomp._SHA.clear()
    return con


def resolve(con, s):
    r = con.execute('SELECT addr FROM funcs WHERE name = ?', (s.strip(),)).fetchone()
    if r:
        return r['addr'], None
    return int(s.replace('0x', ''), 16), None


def run(fn, *a, **k):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = fn(*a, **k)
    return rc, buf.getvalue()


def args_cases(con):
    rc, out = run(qargs.args_main, ['sub_82000500'], con, resolve)
    check('args: F reads r3, r4, r5 (r6 is only written by F)', 'F reads r3, r4, r5\n' in out, out)
    check('twin: r6 (set by caller A, never read by F) is dropped from every line', 'r6=' not in out, out)
    ln = {m.group(1): m.group(0) for m in re.finditer(r'^\s+(sub_\w+)\+\w+ .*$', out, re.M)}
    check('args: A (li r5,7 reaches the bl) shows r5=7', 'sub_82000100' in ln and 'r5=7' in ln['sub_82000100'] and 'r4=1' in ln['sub_82000100'], out)
    check('twin: B (the 7 overwritten by 5) shows r5=5, never 7', 'sub_82000200' in ln and 'r5=5' in ln['sub_82000200'] and 'r5=7' not in ln['sub_82000200'], out)
    check('args: C folds lis + addi to an image address', 'sub_82000300' in ln and 'r5=0x82001234' in ln['sub_82000300'], out)
    check('args: C passes the caller\'s in_r4 through unchanged', 'sub_82000300' in ln and "r4=in_r4 (the caller's own, unchanged)" in ln['sub_82000300'], out)
    check('args: D\'s r5 stays an expression', 'sub_82000400' in ln and 'r5=[in_r3+8]' in ln['sub_82000400'], out)
    rc, out = run(qargs.args_main, ['sub_82000500', '--reg', 'r5'], con, resolve)
    check('args --reg r5: a histogram (7, 5, address, computed once each)', re.search(r'r5: .*\b7 x1\b', out) and '5 x1' in out and '0x82001234 x1' in out and 'computed x1' in out, out)
    rc, out = run(qargs.args_main, ['sub_82000500', '--reg', 'r5', '--value', '7'], con, resolve)
    check('args --value 7: A only; twin B (7 overwritten) and D (an expression) do not pass', '1 pass 7' in out and 'sub_82000100' in out and 'sub_82000200' not in out, out)
    rc, out = run(qargs.args_main, ['sub_82000500', '--in', 'sub_82000200'], con, resolve)
    check('args --in B: one line, B\'s', 'in sub_82000200' in out and 'sub_82000200+' in out and 'sub_82000100' not in out, out)
    rc, out = run(qargs.args_main, ['sub_82000500', '--json'], con, resolve)
    check('args --json is one document', out.lstrip().startswith('{') and '"sites_total": 5' in out, out[:200])


def vcall_cases(con):
    res = [{'site': '0x82000710', 'func': 'sub_82000700'}, {'site': '0x82000754', 'func': 'sub_82000740'}]
    rc, out = run(qargs.vcall_regs, con, res, 'r4', None, False, 60, 0x4C)
    check('vcall --reg r4: the 42 reaches the call (E), the overwritten twin E2 shows 43', 'r4: 42 x1, 43 x1' in out or 'r4: 43 x1, 42 x1' in out, out)
    rc, out = run(qargs.vcall_regs, con, res, 'r4', '42', False, 60, 0x4C)
    check('vcall --reg r4 --value 42: E passes, E2 (overwritten) does not', '1 pass 42' in out and 'sub_82000700' in out and 'sub_82000740' not in out, out)


def regs_cases(root, con):
    def asm(*a):
        return run(qx6.asm_main, list(a), con, resolve, root)[1]
    out = asm('sub_82000800', '--writes', 'r22')
    check('asm --writes r22: lwzu r22 writes it (update form is the BASE r23 for that row\'s other register)', re.search(r'lwzu r22,8\(r23\)\s.*writes r22', out) is not None, out)
    check('twin: lwz r3,0(r22) only reads r22, it is not a write', 'lwz r3,0(r22)' not in out, out)
    out = asm('sub_82000800', '--writes', 'r23')
    check('asm --writes r23: the update form writes its base', re.search(r'lwzu r22,8\(r23\).*update form', out) is not None, out)
    out = asm('sub_82000800', '--reads', 'r22')
    check('asm --reads r22: the lwz reads it (and the lwzu\'s only r23 base is not r22)', 'lwz r3,0(r22)' in out and 'lwzu' not in out, out)
    out = asm('sub_82000800', '--writes', 'r5')
    check('asm --writes r5: li r5 and both bl clobbers', 'li r5,1' in out and 'call: clobbers r0, r3-r12' in out, out)
    out = asm('sub_82000800', '--writes', 'r5')
    check('twin: the __savegprlr helper call is not a write of r5', '82000804' not in out, out)
    out = asm('sub_82000800', '--writes', 'r12')
    check('asm --writes r12: the helper kills r12 (names the helper)', 'clobbers r0, r11, r12 only' in out and '__savegprlr_25' in out, out)
    out = asm('sub_82000800', '--writes', 'r25')
    check('asm --writes r25: the entry row says callee-saved and which helper saves it', '<entry> r25' in out and 'callee-saved' in out and '__savegprlr_25' in out, out)
    out = asm('sub_82000800', '--writes', 'sp+208')
    check('asm --writes sp+208: stw r0,208(r1) is the slot', 'stw r0,208(r1)' in out, out)
    check('twin: stw r0,208(r31) is not (r31 is an ordinary pointer here)', 'stw r0,208(r31)' not in out, out)
    out = asm('sub_82000900', '--writes', 'sp+208')
    check('twin: stw r0,208(r31) IS the slot when r31 is `mr r31,r1`', 'stw r0,208(r31)' in out and 'r31=sp+0' in out, out)
    out = asm('sub_82000800', '--writes', 'sp+210')
    check('asm --writes sp+210: a 4-byte store at 208 covers it', 'stw r0,208(r1)' in out, out)
    out = asm('sub_82000800', '--writes', 'sp+212')
    check('twin: sp+212 is past that store', 'stw r0,208(r1)' not in out, out)
    out = asm('sub_82000800', '--writes', 'r1')
    check('asm --writes r1: stwu r1 writes it', 'stwu r1,-96(r1)' in out, out)
    try:
        asm('sub_82000800', '--writes', 'banana')
        check('asm --writes: a bad register is refused', False)
    except qx6.XError:
        check('asm --writes: a bad register is refused', True)


def real_cases():
    main_idx = r'C:\Users\Jarrod\wwe2k14-recomp\out\harness\index\index.db'
    if not os.path.exists(main_idx) or not os.path.isdir(r'C:\Users\Jarrod\wwe2k14-recomp\generated\default'):
        print('SKIP real lines: no game index / generated')
        return

    def q(*a):
        r = subprocess.run([sys.executable, '-I', Q] + list(a), capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600)
        return r.stdout
    out = q('args', 'sub_82BB16A0', '--reg', 'r5')
    check('real: q args sub_82BB16A0 --reg r5 is a histogram with constants and computed', re.search(r'r5: .*\bx\d+', out) and 'computed x' in out and ' 7 x' in out, out)
    out = q('args', 'sub_82BB7898', '--in', 'sub_823B27F0')
    check('real: the wrapper called twice in sub_823B27F0, r4 shown at both', out.count('r4=') == 2 and 'F reads r3, r4' in out, out)
    out = q('asm', 'sub_8272D710', '--writes', 'r22')
    check('real: asm sub_8272D710 --writes r22: the entry row and the mr r22 writes', '<entry> r22' in out and 'mr r22,r3' in out, out)


def main():
    with tempfile.TemporaryDirectory(prefix='qargs_', ignore_cleanup_errors=True) as root:
        con = build(root)
        args_cases(con)
        vcall_cases(con)
        regs_cases(root, con)
        con.close()
    real_cases()
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

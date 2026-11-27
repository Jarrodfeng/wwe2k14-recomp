"""Self-test of qvtarget (S111 Lane E VCALL, sourced from vcall.s4.db icall / icall_target): `py -I tools/q/qvtarget_selftest.py`   (0 game runs, offline, invented tables only)

Fixture: a function F with indirect call sites over invented in-memory copies of the stores (icall / icall_target, fields rows / roots / objtype / slots, seen).
  S1 +0x10 V_this exact, one target (how this-class)                                  -> [C] that function, the rule named
  S2 +0x20 V_open narrow {OWN, BASE} (how b); root W: this function's vptr store (VT_A, slot word OWN) BEFORE the site -> [C] OWN only (F1)
  S3 +0x30 the same set; root V: the vptr store AFTER the site                          -> [L] narrow 2, both listed
  S4 +0x40 narrow {X1, X2}; root W2: a vptr store before the site whose slot word (OWN) is NOT in the set -> [L] narrow 2 (F1 only narrows, never adds)
  S5 +0x50 V_open cand 6 (how fL), coverage saw one target                              -> [L] seen first, then the candidates capped with the rest counted
  S6 +0x60 V_open offset (n 1250), coverage saw six targets                             -> [L] seen 6 of 1250 fns, capped
  S7 +0x70 V_open offset, nothing seen                                                   -> no fact (the slot's everything is not an answer)
  S8 +0x80 F open, seen one                                                              -> `field +56 [L] seen 1: ...` (no "of N": open has no count)
  S9 +0x90 K exact const                                                                 -> [C]
  S10 the provider: through qfactcard.collect (offset = site - function) and qfactcard.apply (the comment lands before the line holding the offset).
Planted twins that must FAIL the same checks (exit 2 when one is NOT caught): F1 without the intersection with icall's set, F1 reading the wrong root,
an uncapped list. The checks fail on the objtype-derived code before this round (it read no icall table).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfactcard  # noqa: E402
import qvtarget as qv  # noqa: E402

F = 0x82100000
VT_A = 0x82200000
T_EX, OWN, BASE, X1, X2, K_FN, S_C = 0x82300000, 0x82300010, 0x82300020, 0x82300030, 0x82300040, 0x82300050, 0x82300060
CAND = [0x82400000 + 0x10 * i for i in range(6)]
SEEN = [0x82700010 + 0x10 * i for i in range(6)]
F_SEEN = 0x82700500
# (offset, shape, off, status, n, [(target, how)])
SITES = ((0x10, 'V_this', 24, 'exact', 1, [(T_EX, 'this-class')]),
         (0x20, 'V_open', 52, 'narrow', 2, [(OWN, 'b'), (BASE, 'b')]),
         (0x30, 'V_open', 52, 'narrow', 2, [(OWN, 'b'), (BASE, 'b')]),
         (0x40, 'V_open', 52, 'narrow', 2, [(X1, 'b'), (X2, 'b')]),
         (0x50, 'V_open', 8, 'cand', 6, [(t, 'fL') for t in CAND]),
         (0x60, 'V_open', 8, 'offset', 1250, []),
         (0x70, 'V_open', 8, 'offset', 1250, []),
         (0x80, 'F', 56, 'open', 0, []),
         (0x90, 'K', 0, 'exact', 1, [(K_FN, 'const')]))


def fixture():
    v = sqlite3.connect(':memory:')
    v.execute('create table icall(site integer, func integer, kind text, shape text, off integer, recv text, status text, n integer)')
    v.execute('create table icall_target(site integer, target integer, how text)')
    for o, shape, off, status, n, tg in SITES:
        v.execute('insert into icall values(?,?,?,?,?,?,?,?)', (F + o, F, 'call', shape, off, '', status, n))
        for t, h in tg:
            v.execute('insert into icall_target values(?,?,?)', (F + o, t, h))
    v.execute('insert into icall values(?,?,?,?,?,?,?,?)', (F + 0x1000, F, 'call', 'V_this', 24, '', 'exact', 1))      # outside the function's size: no fact
    v.execute('insert into icall_target values(?,?,?)', (F + 0x1000, T_EX, 'this-class'))
    f = sqlite3.connect(':memory:')
    f.execute('create table roots(id integer primary key, text text unique, glob integer)')
    f.execute('create table rows(func integer, addr integer, rw text, width integer, fl integer, off integer, root integer)')
    f.execute('create table objtype(func integer, root integer, vt integer, how text, primary key(func, root, vt))')
    f.execute('create table slots(vt integer, idx integer, func integer)')
    roots = {'W': 1, 'V': 2, 'W2': 3, 'p(W,0)': 11, 'p(V,0)': 12, 'p(W2,0)': 13}
    for t, i in roots.items():
        f.execute('insert into roots values(?,?,NULL)', (i, t))
    for o, root in ((0x20, 'p(W,0)'), (0x30, 'p(V,0)'), (0x40, 'p(W2,0)')):
        f.execute('insert into rows values(?,?,?,?,?,?,?)', (F, F + o - 8, 'R', 4, 0, 52, roots[root]))       # the slot-load row
    f.execute('insert into rows values(?,?,?,?,?,?,?)', (F, F + 0x20 - 0x14, 'W', 4, 0, 0, roots['W']))      # W's vptr store, before its site
    f.execute('insert into rows values(?,?,?,?,?,?,?)', (F, F + 0x30 + 0x40, 'W', 4, 0, 0, roots['V']))      # V's vptr store, after its site
    f.execute('insert into rows values(?,?,?,?,?,?,?)', (F, F + 0x40 - 0x14, 'W', 4, 0, 0, roots['W2']))     # W2's vptr store, before its site
    for root in ('W', 'V', 'W2'):
        f.execute('insert into objtype values(?,?,?,?)', (F, roots[root], VT_A, 'vptr'))
    f.execute('insert into slots values(?,?,?)', (VT_A, 13, OWN))
    s = sqlite3.connect(':memory:')
    s.execute('create table seen(site integer, func integer, target integer, runs integer)')
    s.execute('insert into seen values(?,?,?,?)', (F + 0x50, F, S_C, 13))
    for k, t in enumerate(SEEN):
        s.execute('insert into seen values(?,?,?,?)', (F + 0x60, F, t, 10 + k))
    s.execute('insert into seen values(?,?,?,?)', (F + 0x80, F, F_SEEN, 10))
    return qv.Sources(v, f, s)


def checks(src, cap=None):
    bad = []

    def ck(name, cond, detail=''):
        if not cond:
            bad.append(f'{name} {detail}')

    old = qv.CAP
    if cap is not None:
        qv.CAP = cap
    try:
        t = {r[0]: qv.site_text(src, F, F + r[0], r[1], r[2], r[3], r[4]) for r in SITES}
        txt = {k: (v[0] if v else '') for k, v in t.items()}
        ck('S1 exact = [C], the rule named', txt[0x10] == f'slot +24 [C] sub_{T_EX:08X} (icall this-class)', txt[0x10])
        ck('S1 weight above [L]', t[0x10] is not None and t[0x50] is not None and t[0x10][1] > t[0x50][1], str(t))
        ck('S2 own vptr store before the site narrows icall\'s set to one: [C]', txt[0x20].startswith(f'slot +52 [C] sub_{OWN:08X}') and f'sub_{BASE:08X}' not in txt[0x20], txt[0x20])
        ck('S3 own vptr store after the site: [L] narrow 2, both', txt[0x30] == f'slot +52 [L] narrow 2: sub_{OWN:08X}, sub_{BASE:08X} (icall b)', txt[0x30])
        ck('S4 F1 never adds a function icall did not give', txt[0x40] == f'slot +52 [L] narrow 2: sub_{X1:08X}, sub_{X2:08X} (icall b)', txt[0x40])
        ck('S5 cand: seen first, then the candidates capped, the rest counted',
           txt[0x50].startswith(f'slot +8 [L] seen 1: sub_{S_C:08X} x13 | cand 6: ') and txt[0x50].count('sub_') <= 1 + qv.CAP
           and f'+{len(CAND) - qv.CAP} more (icall fL)' in txt[0x50], txt[0x50])
        ck('S6 offset: seen K of N fns first, capped', txt[0x60].startswith('slot +8 [L] seen 6 of 1250 fns: sub_82700010 x10, sub_82700020 x11')
           and txt[0x60].count('sub_') <= qv.CAP and f'+{len(SEEN) - qv.CAP} more' in txt[0x60], txt[0x60])
        ck('S6 the counts survive the 100-char comment clip', 'seen 6 of 1250 fns' in qfactcard._clip('; [vcall] ' + txt[0x60], qfactcard.COMMENT_CHARS), txt[0x60])
        ck('S7 offset, nothing seen = no fact', t[0x70] is None, str(t[0x70]))
        ck('S8 a field call site, open, seen: no "of N"', txt[0x80] == f'field +56 [L] seen 1: sub_{F_SEEN:08X} x10', txt[0x80])
        ck('S9 a constant target is [C]', txt[0x90] == f'icall K [C] sub_{K_FN:08X} (icall const)', txt[0x90])
        qv.set_sources(src)
        try:
            fs = [x for x in qfactcard.collect(F, None, 0x100, 'decomp', 'card') if x.tag == 'vcall']
            ck('S10 the sites with a fact, offsets = site - function (0x70 and the site past the size print nothing)',
               sorted(x.off for x in fs) == [0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x80, 0x90], str(fs))
            body = ['  +4  r3 = 1', '  +10  vcall#10 *(...)', '  +20  vcall#20 *(...)', '  +30  vcall#30 *(...)'] + ['  pad'] * 400
            hdr, out = qfactcard.apply(body, [x for x in qfactcard.collect(F, None, 0x100, 'decomp', 'all')], 'all')
            i = [k for k, l in enumerate(out) if l.strip().startswith('; [vcall] slot +24 [C]')]
            ck('S10 the comment is the line before the call line', i and 'vcall#10' in out[i[0] + 1], str(out[:6]))
            ck('S10 asm window: the fact inside the window only', len(qfactcard.window_marks(fs, 0x08, 0x14, 'card')) == 1)
        finally:
            qv.set_sources(None)
    finally:
        qv.CAP = old
    return bad


class WrongRoot(qv.Sources):
    """planted twin: F1 reads the receiver root of the WRONG site (every site gets root W, whose store precedes)"""
    def recv_root(self, func, site, slot):
        return 'W'


def no_intersection(src, func, site, slot, fns):
    """planted twin: F1 without the intersection with icall's set (it may add a function icall never gave)"""
    root = src.recv_root(func, site, slot)
    vts = src.own_vtables(func, root)
    if not vts or not src.stored_before(func, site, root):
        return None
    got = {src.slot_func(vt, slot) for vt in vts} - {None}
    return got.pop() if len(got) == 1 else None


def main():
    bad = checks(fixture())
    for b in bad:
        print('FAIL', b)
    if bad:
        return 1
    caught = 0
    s = fixture()
    r1 = checks(WrongRoot(s.vcon, s.fcon, s.scon))
    orig = qv.own_vptr_target
    qv.own_vptr_target = no_intersection
    try:
        r2 = checks(fixture())
    finally:
        qv.own_vptr_target = orig
    r3 = checks(fixture(), cap=999)
    for name, r in (('F1 wrong root', r1), ('F1 adds a function', r2), ('uncapped list', r3)):
        if r:
            caught += 1
            print(f'twin {name}: failed as planted ({len(r)} check(s), first: {r[0][:90]})')
        else:
            print(f'twin {name}: NOT caught')
    if caught < 3:
        return 2
    print('qvtarget_selftest: all checks pass, all 3 twins caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""Self-test of qenum (S111 Lane D ENUMS): `py -I tools/q/qenum_selftest.py`   (0 game runs, offline, a synthetic listing)

A. The enum table: blend code 0 = alpha blend, 1 = additive; alpha func 5 = GEQUAL, 2 = LESS; z func 2 = GEQUAL; render category 35 names the chamber
   CDTB; match type 0 = NORMAL (read from db/ids, not copied); every enum and call row cites a source and a mark; every state call is a wrapper.
B. State of one function (qenum.state_rows) on a synthetic listing: values in call order, a path join (0 | 1), and a z write put back from the getter's
   result read as `restored` (not as a literal).
C. A tiny function that only forwards is followed (the node's draw method is its target); a big one with no recorder call shows its direct callee.
D. The qfactcard provider: a literal r4 at a named wrapper prints `[enum] blend code: r4=1 additive ...` at the call's offset; the provider is registered.
E. Planted twins that must FAIL the same checks: blend code 0 / 1 swapped in the table, a restore read as a literal, path values collapsed to one,
   a thunk not followed.
F. (S111 INDEX-D review) The state at the exits is read PER PATH: a branch-only blend code 1 between two code 0 calls (the chamber's spotlight
   shape, sub_826F8F88 +710) ends 0 on every path, and no listing-order line reads as the end state; a path that sets nothing prints `inherited`.
   A `none` line says what it did not read: indirect calls (a bctr thunk, sub_83336580's shape) and an unreadable callee are named, never
   counted as read. The table says `by a literal` where the category page only checked literals (7 ...), and blend code 12's tuple is (0, 7, 6).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import collections
import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qenum as E  # noqa: E402
import qfactcard as QF  # noqa: E402

I = collections.namedtuple('I', 'addr mn ops')
SYN = 0x82000000
THK = 0x82000100
BIG = 0x82000200
PG = 0x82000300       # the chamber's spotlight shape: code 0, a branch-only code 1, code 0 again
EO = 0x82000400       # an early return before the only z write
VC = 0x82000500       # a thunk that tails through the vtable (bctr), no direct target
BIG2 = 0x82000600     # a big function whose one direct callee cannot be read
UNR = 0x82000700
THK2 = 0x82000800     # a thunk into a function with no listing (an import: sub_8250FAF0 -> ... the kernel event call)


def syn_insns():
    rows = [(0x00, 'li', 'r4, 1'), (0x04, 'bl', '0x82BB7630'),
            (0x08, 'li', 'r4, 5'), (0x0C, 'bl', '0x82BB7678'),
            (0x10, 'bl', '0x82BB82F0'), (0x14, 'mr', 'r30, r3'),
            (0x18, 'li', 'r4, 0'), (0x1C, 'bl', '0x82BB7AD8'),
            (0x20, 'cmpwi', 'r5, 0'), (0x24, 'beq', '0x82000030'),
            (0x28, 'li', 'r4, 1'), (0x2C, 'b', '0x82000034'),
            (0x30, 'li', 'r4, 0'), (0x34, 'bl', '0x82BB7630'),
            (0x38, 'mr', 'r4, r30'), (0x3C, 'bl', '0x82BB7AD8'),
            (0x40, 'blr', '')]
    return [I(SYN + o, mn, ops) for o, mn, ops in rows]


DLINES = ['  +10  call#10 sub_82BB82F0 (get_word_84_of_thread_block_object)(r3=[0x8392DC80])',
          '  +3C  call#3C sub_82BB7AD8 (vcall_slot18_on_thread_block_object)(r3=[0x8392DC80], r4=ret#10)']


def listing(a):
    if a == SYN:
        return syn_insns()
    if a == THK:
        return [I(THK, 'lwz', 'r3, 48(r3)'), I(THK + 4, 'b', '0x82000000')]
    if a == BIG:
        return [I(BIG + 4 * i, 'nop', '') for i in range(30)] + [I(BIG + 120, 'bl', '0x82000000'), I(BIG + 124, 'blr', '')]
    if a == PG:
        rows = [(0x00, 'li', 'r4, 0'), (0x04, 'bl', '0x82BB7630'), (0x08, 'cmpwi', 'r5, 0'), (0x0C, 'beq', '0x82000320'),
                (0x10, 'li', 'r4, 1'), (0x14, 'bl', '0x82BB7630'), (0x18, 'li', 'r4, 0'), (0x1C, 'bl', '0x82BB7630'), (0x20, 'blr', '')]
        return [I(PG + o, mn, ops) for o, mn, ops in rows]
    if a == EO:
        rows = [(0x00, 'cmpwi', 'r5, 0'), (0x04, 'beqlr', 'cr6'), (0x08, 'li', 'r4, 0'), (0x0C, 'bl', '0x82BB7AD8'), (0x10, 'blr', '')]
        return [I(EO + o, mn, ops) for o, mn, ops in rows]
    if a == VC:
        return [I(VC, 'lwz', 'r11, 16(r3)'), I(VC + 4, 'addi', 'r3, r3, 16'), I(VC + 8, 'lwz', 'r10, 4(r11)'), I(VC + 12, 'mtctr', 'r10'),
                I(VC + 16, 'bctr', '')]
    if a == BIG2:
        return [I(BIG2 + 4 * i, 'nop', '') for i in range(30)] + [I(BIG2 + 120, 'bl', '0x82000700'), I(BIG2 + 124, 'blr', '')]
    if a == THK2:
        return [I(THK2, 'lwz', 'r3, 48(r3)'), I(THK2 + 4, 'b', '0x82000700')]
    raise KeyError(a)


def is_func(a):
    return a in (SYN, THK, BIG, PG, EO, VC, BIG2, UNR, THK2)


def checks(d=None):
    """-> [failure text]"""
    d = d or E.load()
    bad = []

    def chk(ok, what):
        if not ok:
            bad.append(what)

    def row(enum, v):
        r = E.enum_row(enum, v, d)
        return r[0] if r else ''

    # A
    chk(row('blend_code', 0).startswith('alpha blend'), 'A blend code 0 is the alpha blend')
    chk(row('blend_code', 1).startswith('additive'), 'A blend code 1 is additive')
    chk(row('alpha_func', 5) == 'GEQUAL' and row('alpha_func', 2) == 'LESS', 'A alpha func 5 GEQUAL / 2 LESS')
    chk(row('z_func', 2) == 'GEQUAL', 'A z func 2 GEQUAL')
    chk('chamber CDTB' in row('render_category', 35), 'A category 35 names the chamber CDTB')
    chk(row('match_type', 0).startswith('NORMAL'), 'A match type 0 NORMAL (from db/ids)')
    chk(all(e.get('source') and e.get('mark') in ('C', 'L') for e in d['enums'].values()), 'A every enum has a source and a mark')
    chk(all(c.get('source') and c['mark'] in ('C', 'L') for c in d['calls'] + d['getters']), 'A every call / getter row has a source and a mark')
    chk(all(c['enum'] in d['enums'] for c in d['calls'] if c['enum']), 'A every call names an enum that exists')
    # B
    rows = E.state_rows(syn_insns(), SYN, lambda: DLINES, d)
    got = [(r['off'], r['what'], r['vals'], r['restore']) for r in rows]
    want = [(0x04, 'blend code', [1], None), (0x0C, 'alpha func', [5], None), (0x1C, 'z write', [0], None),
            (0x34, 'blend code', [0, 1], None), (0x3C, 'z write', None, 0x10)]
    chk(got == want, f'B state rows {got}')
    chk(rows and 'restored' in rows[-1]['text'], 'B the z write put back reads as restored')
    chk(rows and rows[3]['text'].count('|') == 1, 'B a path join prints both values')
    # C
    segs = E.chase(THK, listing, lambda a: DLINES, is_func, d)
    chk(len(segs) == 1 and segs[0]['fn'] == SYN and segs[0]['via'] == [THK] and segs[0]['how'] == 'own' and len(segs[0]['rows']) == 5, f'C a thunk is followed to its target: {segs}')
    segs = E.chase(BIG, listing, lambda a: DLINES, is_func, d)
    chk(len(segs) == 1 and segs[0]['how'] == 'callee' and segs[0]['fn'] == SYN, f'C a big function with no call shows its callee: {segs}')
    sl = E.segment_lines(E.chase(SYN, listing, lambda a: DLINES, is_func, d)[0])
    chk(any('exit, per path (1 exit(s)): blend code 0 alpha blend | 1 additive; alpha func 5 GEQUAL; z write restored' in l for l in sl),
        f'C the printed segment has its per-path exit line: {sl}')
    # D
    ctx = QF.Ctx(SYN, 0x44, None, 'decomp', 'card')
    facts = E.provider(ctx, insns=syn_insns(), d=d)
    bytext = {f.off: f.text for f in facts}
    chk(0x04 in bytext and 'additive' in bytext[0x04] and bytext[0x04].startswith('blend code'), f'D the enum fact on the blend code call: {bytext}')
    chk(0x0C in bytext and 'GEQUAL' in bytext[0x0C], 'D the alpha func fact')
    chk(0x3C not in bytext, 'D a non-literal argument prints no enum fact')
    chk(any(p[0] == 'enum' for p in QF._PROVIDERS), 'D the provider is registered')
    # F (S111 INDEX-D review)
    pl = E.segment_lines(E.chase(PG, listing, lambda a: [], is_func, d)[0])
    ex = [l for l in pl if 'exit' in l]
    chk(len(ex) == 1 and 'blend code 0 alpha blend' in ex[0] and 'additive' not in ex[0], f'F a branch-only code 1 does not end the state: {pl}')
    chk(not any(l.strip().startswith('net:') for l in pl), f'F no listing-order line reads as the end state: {pl}')
    el = E.segment_lines(E.chase(EO, listing, lambda a: [], is_func, d)[0])
    chk(any('exit' in l and 'z write 0 off | inherited' in l for l in el), f'F a path that sets nothing prints inherited: {el}')
    vl = E.segment_lines(E.chase(VC, listing, lambda a: [], is_func, d)[0])
    chk(len(vl) == 1 and '1 indirect call' in vl[0], f'F a bctr thunk names its indirect call: {vl}')
    bl2 = E.segment_lines(E.chase(BIG2, listing, lambda a: [], is_func, d)[0])
    chk(len(bl2) == 1 and '0 of its 1 direct callee(s)' in bl2[0] and '1 unreadable' in bl2[0], f'F an unreadable callee is not counted as read: {bl2}')
    try:
        tl = [l for s in E.chase(THK2, listing, lambda a: [], is_func, d) for l in E.segment_lines(s)]
    except Exception as e:  # noqa: BLE001 - the unfixed chase raised here: the whole node was lost
        tl = [f'raised {type(e).__name__}']
    chk(len(tl) == 1 and 'NOT READ' in tl[0] and 'sub_82000800 -> sub_82000700' in tl[0], f'F a thunk target with no listing is said, not raised: {tl}')
    lit_only = ('7', '9', '10', '11', '31', '32', '34', '45', '49')
    chk(all('by a literal' in row('render_category', k) for k in lit_only), 'F the literal-only categories say `by a literal`')
    chk('(0, 7, 6)' in row('blend_code', 12) and '+ dst' not in row('blend_code', 12), 'F blend code 12 is the tuple (0, 7, 6)')
    return bad


def main():
    bad = checks()
    for b in bad:
        print('FAIL', b)
    if bad:
        return 1
    print('qenum selftest: A..D, F pass')
    # E planted twins: each must make checks() fail
    twins = []
    d0 = copy.deepcopy(E.load())
    v = d0['enums']['blend_code']['values']
    v['0']['name'], v['1']['name'] = v['1']['name'], v['0']['name']
    twins.append(('blend code 0 / 1 swapped', lambda: checks(d0), lambda: None))
    saved = {}

    def patch(name, fn):
        saved[name] = getattr(E, name)
        setattr(E, name, fn)

    twins.append(('a restore read as a literal', lambda: checks(), lambda: patch('_ret_target', lambda *a: None)))
    twins.append(('path values collapsed to one', lambda: checks(), lambda: patch('_lit_list', lambda v: None if v is None else [min(v)] if isinstance(v, frozenset) else [v])))

    def no_thunk():
        saved['THUNK_MAX'] = E.THUNK_MAX
        E.THUNK_MAX = 0
    twins.append(('a thunk not followed', lambda: checks(), no_thunk))
    missed = 0
    for name, run, setup in twins:
        setup()
        try:
            r = run()
        finally:
            for k, f in list(saved.items()):
                setattr(E, k, f)
            saved.clear()
        print(f'twin {name}: ' + (f'caught ({len(r)} failure(s): {r[0][:70]})' if r else 'NOT CAUGHT'))
        missed += 0 if r else 1
    return 2 if missed else 0


if __name__ == '__main__':
    sys.exit(main())

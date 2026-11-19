"""Self-test of `q const` (S103 lane QC): `py -I tools/q/qconst_selftest.py [--root .]`   (0 game runs)

A. Synthetic fixture (invented instruction sequences in a TEMP folder, no game code): two recorders (tags 21 and 28), a
   constructor storing two name handles (offsets 300 / 304, two different name strings), four callers:
     C1 loads the handle at 304 (must name BBB, not AAA), C2 a literal 184 (data from a stack store), C3 sets r4 15
     instructions before the call (outside the window: unresolved, not a literal), C4 sets it 14 before (literal).
B. Real data (needs generated/ and the q index; else SKIP): c184 -> sub_827F22C8 (vs 0x827F240C, ps 0x827F2420, 3 callers);
   c72 / EyePos -> 0x82513B40 through crowd+31200; c189 MatAmbCol and ps c200 SpecularCol (checked by hand against the ctor
   listing); the setter table check passes on the real code.
C. Planted twins: a wrong handle name, a literal where the window must say unresolved, a setter missing from the table
   must each be REJECTED by the same checks.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import decomp_selftest as ds   # noqa: E402
import qconst   # noqa: E402
import qdecomp   # noqa: E402

FAILS = []
CAUGHT = []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, msg):
    """msg = the checker's failure message for a planted wrong expectation; None = the checker accepted it (a defect)."""
    if msg is None:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name}')
    else:
        print(f'ok   planted twin caught: {name}')


# ------------------------------------------------------------------ fixture
R21, R28, CTOR, C1, C2, C3, C4 = 0x82002000, 0x82002100, 0x82003000, 0x82004000, 0x82005000, 0x82006000, 0x82007000
COPY, RESOLVE = 0x82009100, 0x82009200
STR_A, STR_B, BUF_A, BUF_B = 0x82000500, 0x82000540, 0x82000800, 0x82000810


def build(e):
    e.name(COPY, 'copy_string')
    e.name(RESOLVE, 'resolve_name')
    for r, tag in ((R21, 21), (R28, 28)):
        e.add(r, ['li r11,%d' % tag, 'stw r11,0(r3)', 'stw r4,16(r3)', 'stw r6,20(r3)', 'li r10,1048', 'li r9,68', 'blr'])

    def absolute(reg, a):
        hi = (a >> 16) & 0xFFFF
        lo = a & 0xFFFF
        return [f'lis {reg},{hi - 0x10000 if hi >= 0x8000 else hi}', f'addi {reg},{reg},{lo}']

    ctor = ['mr r30,r3']
    for buf, st, off, coff in ((BUF_A, STR_A, 300, 320), (BUF_B, STR_B, 304, 324)):
        ctor += absolute('r3', buf) + absolute('r4', st) + ['bl 0x%08x' % COPY]
        ctor += ['li r3,0'] + absolute('r4', buf) + ['bl 0x%08x' % RESOLVE]
        ctor += ['lhz r9,16(r3)', f'sth r9,{off}(r30)', 'lhz r8,18(r3)', f'sth r8,{coff}(r30)']
    ctor += ['blr']
    e.add(CTOR, ctor)
    e.add(C1, ['lhz r4,304(r3)', 'lhz r6,324(r3)', 'addi r5,r1,96', 'bl 0x%08x' % R21, 'blr'])
    e.add(C2, ['li r10,5', 'stw r10,96(r1)', 'li r4,184', 'li r6,1', 'addi r5,r1,96', 'bl 0x%08x' % R28, 'blr'])
    e.add(C3, ['li r4,72'] + ['li r8,0'] * 12 + ['li r6,1', 'addi r5,r1,96', 'bl 0x%08x' % R21, 'blr'])
    e.add(C4, ['li r4,72'] + ['li r8,0'] * 11 + ['li r6,1', 'addi r5,r1,96', 'bl 0x%08x' % R21, 'blr'])
    c = e.con
    c.execute('CREATE TABLE imms (func INTEGER, site INTEGER, value INTEGER, op TEXT)')
    c.execute('CREATE TABLE edges (src INTEGER, dst INTEGER, target INTEGER, site INTEGER, kind TEXT)')
    c.execute('CREATE TABLE strings (addr INTEGER PRIMARY KEY, enc TEXT, text TEXT)')
    for r in (R21, R28):
        c.execute('INSERT INTO imms VALUES (?,?,1048,"li")', (r, r))
        c.execute('INSERT INTO imms VALUES (?,?,68,"li")', (r, r))
    for src, dst in ((C1, R21), (C2, R28), (C3, R21), (C4, R21)):
        r = e.get(src)
        for site in r['summary']['sites'].get(f'{dst:08X}', []):
            c.execute('INSERT INTO edges VALUES (?,?,?,?,"call")', (src, dst, dst, site['site']))
    c.execute('INSERT INTO strings VALUES (?,?,?)', (STR_A, 'a', 'g_f4AAA'))
    c.execute('INSERT INTO strings VALUES (?,?,?)', (STR_B, 'a', 'g_f4BBB'))


def engine(e):
    def strings(a):
        r = e.con.execute('SELECT text FROM strings WHERE addr = ?', (a,)).fetchone()
        return r['text'] if r else None

    def insns(addr):
        f = e.con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
        return qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], f['addr'])[0]
    return qconst.Engine(e.con, lambda a: e.get(a), lambda off: [CTOR] if off in (300, 304, 320, 324) else [], strings, insns)


TABLE = {21: ('vs', 'float range'), 28: ('ps', 'float range')}


def expect(hits, func, want):
    """-> None if the hit of `func` has reg.kind == want[0] and (name | value | why-substring) == want[1]; else a message."""
    hs = [h for h in hits if h['func'] == func]
    if len(hs) != 1:
        return f'{len(hs)} hits for {func:#x}'
    r = hs[0]['reg']
    kind, val = want
    if r['kind'] != kind:
        return f'kind {r["kind"]} != {kind}: {r}'
    if kind == 'name':
        got = [n['name'] for n in r['names']]
        return None if got == [val] else f'names {got} != {val}'
    if kind == 'literal':
        return None if r['value'] == val else f'literal {r["value"]} != {val}'
    return None if val in r['why'] else f'why {r["why"]!r} lacks {val!r}'


def synthetic(e):
    build(e)
    eng = engine(e)
    found = qconst.discover_setters(e.con, eng.dec)
    check('discover: tags', sorted(found) == [21, 28], str(sorted(found)))
    check('discover: recorder addresses', found[21][0]['addr'] == R21 and found[28][0]['addr'] == R28, str(found))
    check('setter table check (full table of this fixture)', qconst.check_setters(found, TABLE) == [], str(qconst.check_setters(found, TABLE)))
    twin('a setter missing from the table (tag 28)', None if qconst.check_setters(found, {21: TABLE[21]}) == [] else 'caught')
    twin('a table setter with no recorder in the code', None if qconst.check_setters({21: found[21]}, TABLE) == [] else 'caught')
    setters = {t: {**v[0], 'tag': t} for t, v in found.items()}
    hits = eng.scan(setters)
    check('scan: 4 sites', len(hits) == 4, str(len(hits)))
    check('C1 names BBB (handle at +304, not AAA at +300)', expect(hits, C1, ('name', 'g_f4BBB')) is None, str(expect(hits, C1, ('name', 'g_f4BBB'))))
    twin('wrong handle: C1 expected to name AAA', expect(hits, C1, ('name', 'g_f4AAA')))
    check('C1 count = the handle count', [h for h in hits if h['func'] == C1][0]['count']['kind'] == 'handle_count',
          str([h for h in hits if h['func'] == C1][0]['count']))
    check('C2 literal 184 (ps)', expect(hits, C2, ('literal', 184)) is None, str(expect(hits, C2, ('literal', 184))))
    h2 = [h for h in hits if h['func'] == C2][0]
    check('C2 stage ps, count 1', h2['stage'] == 'ps' and h2['count'] == {'kind': 'literal', 'value': 1}, str(h2['count']))
    check('C2 data: the stack store folded', h2['data_kind'] == 'stores' and '= 5' in h2['data'], f'{h2["data_kind"]} {h2["data"]}')
    check('C3 (15 before the call) is unresolved, not a literal', expect(hits, C3, ('unresolved', 'window')) is None,
          str(expect(hits, C3, ('unresolved', 'window'))))
    twin('window: C3 expected as literal 72', expect(hits, C3, ('literal', 72)))
    check('C4 (14 before the call) is the literal 72', expect(hits, C4, ('literal', 72)) is None, str(expect(hits, C4, ('literal', 72))))
    # queries
    check('covers: c184 ps -> C2 only', [h['func'] for h in hits if qconst.hit_covers(h, 184, 'ps')] == [C2], '')
    check('covers: c72 vs -> C4 only (C3 unresolved)', [h['func'] for h in hits if qconst.hit_covers(h, 72, 'vs')] == [C4], '')
    check('covers: c72 ps -> none (vs-only setter)', not [h for h in hits if qconst.hit_covers(h, 72, 'ps')], '')


def real():
    import q
    if not os.path.exists(q.DB) or not os.path.isdir(qdecomp.GEN):
        print('SKIP real-data proofs (no generated/ or q index here)')
        return
    import argparse
    con = q.connect(q.DB)
    a = argparse.Namespace(window=qconst.WINDOW, raw=False, no_cache=False, root=ARGS_ROOT)
    import time
    t = time.time()
    d = qconst.get_scan(con, a)
    secs = time.time() - t
    print(f'     real scan {secs:.1f} s ({"cached" if d["cached"] else "built"}), setters {len(d["setters"])}, sites {len(d["hits"])}')
    check('real: setter table check passes', d['problems'] == [], str(d['problems']))
    hits = d['hits']
    h184 = [h for h in hits if qconst.hit_covers(h, 184, None)]
    check('real c184: exactly the two sites of sub_827F22C8', sorted((h['func'], h['site'], h['stage']) for h in h184)
          == [(0x827F22C8, 0x827F240C, 'vs'), (0x827F22C8, 0x827F2420, 'ps')], str([(hex(h['func']), hex(h['site'])) for h in h184]))
    check('real c184: both literal 184, count 1', all(h['reg'] == {'kind': 'literal', 'value': 184} and h['count']['value'] == 1 for h in h184), '')
    callers = {r[0] for r in con.execute('SELECT DISTINCT src FROM edges WHERE dst = ?', (0x827F22C8,))}
    check('real c184: 3 callers', callers == {0x82513A38, 0x825185B0, 0x825B97B8}, str([hex(x) for x in callers]))
    check('real c184: data formula has 1/[rec+4] and 0.01', '1 / float([rec+4])' in h184[0]['data'] and '0.01' in h184[0]['data'], h184[0]['data'])
    h72 = [h for h in hits if qconst.hit_covers(h, 72, 'vs')]
    crowd = [h for h in h72 if h['site'] == 0x82513B40]
    check('real c72: the crowd upload 0x82513B40 is found', len(crowd) == 1, str([hex(h['site']) for h in h72]))
    check('real c72: it is named g_f4EyePos via crowd+31200', crowd and crowd[0]['reg']['kind'] == 'name'
          and [n['name'] for n in crowd[0]['reg']['names']] == ['g_f4EyePos'] and crowd[0]['reg']['off'] == 31200, str(crowd and crowd[0]['reg']))
    twin('real: 0x82513B40 expected to be LightMapParam (T3\'s mistake)',
         None if crowd and any(name_is(n, 'LightMapParam') for n in crowd[0]['reg']['names']) else 'caught')
    twin('real: c184 expected at 0x82513B40', None if any(h['site'] == 0x82513B40 for h in h184) else 'caught')
    amb = [h for h in hits if h['site'] == 0x82513A94]
    check('real c189: 0x82513A94 names g_f4MatAmbCol (ctor +348 name buffer 0x83763020 <- string 0x820062A4, hand-checked)',
          amb and [n['name'] for n in amb[0]['reg']['names']] == ['g_f4MatAmbCol'] and qconst.hit_covers(amb[0], 189, 'vs'), str(amb and amb[0]['reg']))
    spec = [h for h in hits if h['site'] == 0x82513C14]
    check('real ps c200: 0x82513C14 names g_f4SpecularCol (ctor +3C0, buffer 0x83762FD0 <- 0x8201C1D8, hand-checked)',
          spec and [n['name'] for n in spec[0]['reg']['names']] == ['g_f4SpecularCol'] and qconst.hit_covers(spec[0], 200, 'ps'), str(spec and spec[0]['reg']))


def name_is(n, key):
    return qconst.name_key(n['name']) == key


ARGS_ROOT = None


def main():
    global ARGS_ROOT
    if '--root' in sys.argv:
        ARGS_ROOT = sys.argv[sys.argv.index('--root') + 1]
    e = ds.Env()
    try:
        synthetic(e)
    finally:
        e.close()
    real()
    if FAILS:
        print(f'\n{len(FAILS)} check(s) FAILED')
        return 1
    if CAUGHT:
        print('\na planted twin was not caught')
        return 2
    print('\nall q const checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

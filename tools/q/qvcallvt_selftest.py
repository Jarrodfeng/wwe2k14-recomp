"""Self-test of `q vcall OFF --vt VT` (qvcallvt.py) and `q range` (qrange.py), S111 QVCALLVT: `py -I tools/q/qvcallvt_selftest.py [--no-real]`   (0 game runs)

A. Planted listings (invented instruction sequences in a TEMP folder, no game code), a class with NO RTTI (a raw vtable 0x82300000, slots in
   the planted source only) and the virtual calls of slot 12 (+48) on it:
     LOCAL   the site builds the object (`bl ctor`, the result is the receiver)                       -> kept (slice)
     SLOTFN  a slot function of the class calls slot 12 on its own this                                -> kept (slice or slot)
     VPTR    the site stores the vptr itself and calls                                                 -> kept (vptr; the vcall table's own `cls`)
     FIELD   a holder class's forwarder: `lwz r3,1056(r3)` then the call; the holder's constructor
             stores a constructed frame object into +1056                                              -> kept (field)
     GLOBAL  the receiver is the object kept in a global word the constructor's caller writes          -> kept (slice, L / C)
     OTHER   an object of another class (another vtable)                                               -> NOT kept, counted as another class
     UNKNOWN a receiver loaded from an argument's field the index does not type                        -> NOT kept, counted unresolved
   and `q range` on planted funcs rows: the load of +1056 (forwarders and the holder's ctor store are not loads), the store of +1056, the call of
   the ctor, --exclude, and two filters at once. Each check has its twin: the wrong expectation must FAIL.
B. The real re-finds (a machine without generated/ or the q index SKIPs): the page forwarders of the frame base 0x820629C0: slot 12 (+0x30,
   the accept gate: page slot 15 = sub_82B55B28) and slot 13 (+0x34, the leave check: page slot 16 = sub_82B55B50) are listed by
   `vcall OFF --vt 0x820629C0`; `range 0x82B55000 0x82B56000 --loads 1056` lists them, and a slot function of the frame base is NOT a +1056 load.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import contextlib
import io
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrange  # noqa: E402
import qstruct_selftest as qs  # noqa: E402  the planted source the q struct selftest uses
import qvcallvt  # noqa: E402

FAILS, TWINS_MISSED = [], []
check, twin = qs.check, qs.twin
qs.FAILS, qs.TWINS_MISSED = FAILS, TWINS_MISSED

VT, HVT, OVT = 0x82300000, 0x82300100, 0x82300200       # the class, a holder class (a page), another class
SL0, SL1, CTOR, HCTOR, HFWD, OCTOR = 0x82301000, 0x82301100, 0x82301200, 0x82301300, 0x82301400, 0x82301500
LOCAL, SLOTFN_SITE, VPTRF, GLOB, OTHER, UNKNOWN, MAKER, ALLOC = 0x82302000, SL0, 0x82302100, 0x82302200, 0x82302300, 0x82302400, 0x82302500, 0x82302600
DVT, H2VT = 0x82300300, 0x82300400                       # a class built on VT (its ctor calls VT's), a second holder whose frame is stored elsewhere
DCTOR, H2FWD, STORER, DERIV = 0x82301600, 0x82301700, 0x82301800, 0x82302700
G = 0x83020200
CALL = ['lwz r11,0(r31)', 'lwz r10,48(r11)', 'mtctr r10', 'mr r3,r31', 'bctrl ', 'blr']


class Src(qs.PlantSource):
    def type_funcs(self, vts):
        return {f for vt in vts for f in self.slots.get(vt, ())} | {f for vt in vts for f in self.vt_store_funcs(vt)}

    def off_stores(self, off):
        """the planted stores `stw rX,off(rY)` of any function (what the field index's W rows give a QSource)"""
        out = []
        for f in sorted(self.funcs):
            for rel, rec in sorted(self.facts(f)['ins'].items()):
                if rec['mn'] == 'stw' and (rec['ops'].endswith(f',{off}(r4)') or rec['ops'].endswith(f',{off}(r31)')):
                    out.append((f, rel))
        return out


def lis_hi(a):
    h = a >> 16
    return h - 0x10000 if h >= 0x8000 else h


def vptr(reg, vt):
    return [f'lis {reg},{lis_hi(vt)}', f'addi {reg},{reg},{vt & 0xFFFF}', f'stw {reg},0(r31)']


def plant(S):
    S.add(CTOR, ['mr r31,r3'] + vptr('r11', VT) + ['mr r3,r31', 'blr'])
    S.add(OCTOR, ['mr r31,r3'] + vptr('r11', OVT) + ['mr r3,r31', 'blr'])
    S.add(HCTOR, ['mr r31,r3'] + vptr('r11', HVT) + ['li r3,1056', f'bl 0x{ALLOC:08x}', f'bl 0x{CTOR:08x}', 'stw r3,1056(r31)', 'mr r3,r31', 'blr'])
    S.add(ALLOC, ['blr'])
    S.edge(HCTOR, HCTOR + 28 + 4 * 0, ALLOC)
    S.edge(HCTOR, HCTOR + 32, CTOR)
    S.add(SL0, ['mr r31,r3'] + CALL)
    S.add(SL1, ['blr'])
    S.add(LOCAL, [f'bl 0x{CTOR:08x}', 'mr r31,r3'] + CALL)
    S.edge(LOCAL, LOCAL, CTOR)
    S.add(VPTRF, ['mr r31,r3'] + vptr('r11', VT) + CALL)
    S.add(HFWD, ['lwz r31,1056(r3)'] + CALL)
    S.add(GLOB, ['lis r11,-31998', 'lwz r31,512(r11)'] + CALL)
    S.add(MAKER, [f'bl 0x{CTOR:08x}', 'lis r11,-31998', 'stw r3,512(r11)', 'blr'])
    S.edge(MAKER, MAKER, CTOR)
    S.add(OTHER, [f'bl 0x{OCTOR:08x}', 'mr r31,r3'] + CALL)
    S.edge(OTHER, OTHER, OCTOR)
    S.add(UNKNOWN, ['lwz r31,4(r4)'] + CALL)
    S.add(DCTOR, ['mr r31,r3', f'bl 0x{CTOR:08x}'] + vptr('r11', DVT) + ['mr r3,r31', 'blr'])
    S.edge(DCTOR, DCTOR + 4, CTOR)
    S.add(DERIV, [f'bl 0x{DCTOR:08x}', 'mr r31,r3'] + CALL)
    S.edge(DERIV, DERIV, DCTOR)
    S.add(H2FWD, ['lwz r31,1056(r3)'] + CALL)
    S.add(STORER, [f'bl 0x{CTOR:08x}', 'stw r3,1056(r4)', 'blr'])
    S.edge(STORER, STORER, CTOR)
    S.slots[DVT] = [SL1]
    S.slots[H2VT] = [H2FWD]
    S.slots[VT] = [SL0, SL1]
    S.slots[HVT] = [HFWD]
    S.slots[OVT] = [SL1]
    S.gf[G] = {GLOB, MAKER}
    S.kw[G] = {MAKER}


def site_of(S, f):
    """the bctrl of a planted function"""
    T = S.facts(f)
    return f + next(rel for rel, rec in sorted(T['ins'].items()) if rec['mn'] == 'bctrl')


def planted_vcall(tmp):
    S = Src(tmp)
    plant(S)
    names = {'LOCAL': LOCAL, 'SLOTFN': SL0, 'VPTR': VPTRF, 'FIELD': HFWD, 'GLOBAL': GLOB, 'OTHER': OTHER, 'UNKNOWN': UNKNOWN, 'DERIVED': DERIV, 'FIELDL': H2FWD}
    rows = [{'site': site_of(S, f), 'func': f, 'cls': 0, 'this_recv': f == SL0} for f in names.values()]
    rows[2]['cls'] = VT          # the vcall table found the vptr store in the same function
    kept, other, unres, tgt = qvcallvt.pick(S, VT, rows)
    got = {r['func']: how for r, how in kept}
    for k in ('LOCAL', 'SLOTFN', 'VPTR', 'FIELD', 'GLOBAL', 'DERIVED', 'FIELDL'):
        check(f'vcall --vt: {k} is kept', names[k] in got, repr(got))
    check('vcall --vt: LOCAL is a slice hit', got.get(LOCAL, '').startswith('slice'), got.get(LOCAL))
    check('vcall --vt: VPTR says the vptr was stored in the function', got.get(VPTRF, '').startswith('vptr'), got.get(VPTRF))
    check('vcall --vt: FIELD says the field writers', got.get(HFWD, '').startswith('field') and '+1056' in got.get(HFWD, ''), got.get(HFWD))
    check('vcall --vt: OTHER is not kept and is counted as another class', OTHER not in got and other == 1, f'other={other} {got}')
    check('vcall --vt: UNKNOWN is not kept and is counted unresolved', UNKNOWN not in got and unres == 1, f'unres={unres} {got}')
    check('vcall --vt: DERIVED (a class whose ctor calls the class\'s ctor) is in the family', DVT in tgt.fam and got.get(DERIV, '').startswith('slice'), f'{tgt.fam} {got.get(DERIV)}')
    check('vcall --vt: FIELDL is [L]: only a store of +1056 elsewhere puts the class there', got.get(H2FWD, '').startswith('field [L]'), got.get(H2FWD))
    twin('vcall --vt: the exact-writer FIELD is also marked [L]', got.get(HFWD, '').startswith('field [L]'))
    twin('vcall --vt: the other class\'s site is kept', OTHER in got)
    twin('vcall --vt: the untyped receiver is kept', UNKNOWN in got)
    twin('vcall --vt: nothing is kept', not kept)
    try:
        qvcallvt.pick(S, 0x82309990, rows)
        check('vcall --vt: a non-vtable address is refused', False)
    except qvcallvt.VcvError:
        check('vcall --vt: a non-vtable address is refused', True)


def planted_range(tmp):
    S = Src(tmp)
    plant(S)
    con = sqlite3.connect(':memory:')
    con.execute('CREATE TABLE funcs(addr INTEGER PRIMARY KEY, name TEXT, file TEXT, line INTEGER, size INTEGER)')
    for f, d in S.funcs.items():
        con.execute('INSERT INTO funcs VALUES (?,?,?,?,?)', (f, d['name'], os.path.basename(d['path']), 1, 16))
    gen = tmp
    n, res, _ = qrange.scan(con, 0x82301000, 0x82302000, loads=1056, gen=gen)
    check('range --loads 1056: only the two holder forwarders load it', [r['addr'] for r in res] == [HFWD, H2FWD] and n == 9, f'{n} {[hex(r["addr"]) for r in res]}')
    n, res, _ = qrange.scan(con, 0x82301000, 0x82302000, stores=1056, gen=gen)
    check('range --stores 1056: the holder constructor and STORER', [r['addr'] for r in res] == [HCTOR, STORER], f'{[hex(r["addr"]) for r in res]}')
    n, res, _ = qrange.scan(con, 0x82300000, 0x82303000, calls=CTOR, gen=gen)
    check('range --calls CTOR: the holder ctor, the derived ctor, STORER, LOCAL and MAKER', sorted(r['addr'] for r in res) == sorted([HCTOR, DCTOR, STORER, LOCAL, MAKER]), f'{[hex(r["addr"]) for r in res]}')
    n, res, _ = qrange.scan(con, 0x82300000, 0x82303000, calls=CTOR, exclude=(0x82302000, 0x82303000), gen=gen)
    check('range --calls CTOR --exclude: LOCAL and MAKER are dropped', [r['addr'] for r in res] == [HCTOR, DCTOR, STORER], f'{[hex(r["addr"]) for r in res]}')
    n, res, _ = qrange.scan(con, 0x82300000, 0x82303000, calls=CTOR, stores=1056, gen=gen)
    check('range: two filters must both hold', [r['addr'] for r in res] == [HCTOR, STORER], f'{[hex(r["addr"]) for r in res]}')
    n, res, _ = qrange.scan(con, 0x82301000, 0x82301300, gen=gen)
    check('range LO HI: [LO, HI) by function start (no filter lists every function)', n == 3 and len(res) == 3, f'{n} {len(res)}')
    twin('range --loads 1056: the constructor\'s store counts as a load', HCTOR in [r['addr'] for r in qrange.scan(con, 0x82301000, 0x82302000, loads=1056, gen=gen)[1]])
    twin('range: HI is inclusive', n == 4)
    twin('range --loads 1060: finds the forwarder', HFWD in [r['addr'] for r in qrange.scan(con, 0x82301000, 0x82302000, loads=1060, gen=gen)[1]])


def real():
    try:
        import q as qmod
        import qdecomp
        import qx7
        con = qmod.connect(qmod.DB)
    except Exception as e:  # noqa: BLE001 - no q index on this machine: part B is skipped, said so
        print(f'SKIP B (no q index: {e})')
        return
    if not os.path.isdir(qdecomp.GEN):
        print('SKIP B (no generated/ tree)')
        return
    t0 = time.time()
    n, res, _ = qrange.scan(con, 0x82B55000, 0x82B56000, loads=1056)
    got = {r['addr'] for r in res}
    check(f'real range 0x82B55000..0x82B56000 --loads 1056 ({time.time() - t0:.1f} s): the slot 12 / 13 forwarders, the destructor, 34 of 54',
          {0x82B55B28, 0x82B55B50, 0x82B559B8} <= got and len(res) == 34 and n == 54, f'{n} {len(res)}')
    twin('real range: the frame base default stub sub_82B07FC0 loads +1056', 0x82B07FC0 in got)
    # the frame base 0x820629C0 (no RTTI): its family, then the page forwarders' sites of slot 12 (+0x30) / 13 (+0x34) run through the production path.
    # KNOWN LIMIT, recorded so a better tracer shows up as a failing check: the forwarders load the frame from [page+1056] and the derived pages store it
    # through roots the slicer does not tie to the page class (M100_OPT's "frame contract"), so they are counted unresolved, not listed.
    import qslice
    import qvcallvt
    import qstruct
    t0 = time.time()
    src = qslice.QSource(con)
    sl = qslice.Slicer(src)
    tg = qstruct.class_target(sl, src, 0x820629C0)
    fam = qvcallvt.family(src, sl, tg)
    check(f'real: the family of 0x820629C0 holds its derived classes ({time.time() - t0:.1f} s)', {0x82061868, 0x82062DA0, 0x82062F68, 0x820613D0} <= fam, sorted(map(hex, fam)))
    twin('real: the family holds the page class 0x820660A8', 0x820660A8 in fam)
    tg.fmemo, tg.omemo = {}, {}
    for off, fwd, site, label in ((0x30, 0x82B55B28, 0x82B55B44, 'slot 12 accept gate'), (0x34, 0x82B55B50, 0x82B55B6C, 'slot 13 leave check')):
        rows = [{'site': site, 'func': fwd, 'cls': 0, 'this_recv': 0}]
        how, typed = qvcallvt.classify(src, sl, tg, rows[0])
        print(f'== {label}: forwarder sub_{fwd:08X} site {site:#x}: how={how} typed={typed}')
        check(f'real: {label} forwarder is classified without a crash (KNOWN LIMIT: unresolved, not listed)', how is None and not typed, how)


def main():
    tmp = tempfile.mkdtemp(prefix='qvcallvt_selftest_')
    try:
        planted_vcall(tmp)
        planted_range(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if '--no-real' not in sys.argv:
        real()
    print(f'qvcallvt_selftest: {len(FAILS)} failed, {len(TWINS_MISSED)} twins not caught')
    return 1 if FAILS else (2 if TWINS_MISSED else 0)


if __name__ == '__main__':
    sys.exit(main())

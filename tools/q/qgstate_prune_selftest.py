"""qgstate_prune_selftest: the unreachable-path pruning of q gstate --inherit (S111 GPRUNE) on planted listings (invented, no game code),
each case with its planted twin that must FAIL it, then the real re-find.
  A  a callee whose draw call needs in_r4 == 3 (`cmpwi cr6,r4,3` / `bne`): a caller passing {0, 1} (a phi of constants) is pruned and
     named (twin: the same caller with the wrong expectation 'kept'), a caller passing 3 is kept, a caller passing a register is kept
  B  the guard is read by the listing's branch sense: a `beq` that SKIPS the call needs != 0 (a caller passing 0 is pruned, 5 kept)
  C  the walk: the draw function's alpha test, two callers of its guarded parent (0 / 1 -> sets 7, 3 -> sets 9): only 9 survives and the
     footer names the pruned caller (twin WWE_QGINHERIT_PLANT=no_prune: UNSETTLED {7, 9}); a parent whose only caller misses the value
     is dead: no path reaches a setter
  D  a draw path: the hop through the guarded call is dropped (twin no_prune keeps it)
  real  q gstate --inherit sub_82813228: sub_827E7338 (needs r4 == 3 at its blob call, its only caller sub_827E8080 passes {0, 1}) is
        pruned and named, and no alpha_ref path runs through it
Skipped (said) when the q index is absent. 0 game runs.  Run:  py -I tools/q/qgstate_prune_selftest.py"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qgstate_inherit as GI  # noqa: E402
import qgstate_prune as GP  # noqa: E402
from qslice_selftest import PlantSource  # noqa: E402  the planted-listing source qslice's own selftest uses

fails = []
SETTER = 0x82BB75E8
LEAF = 0x82100F00


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:900]))
    if not ok:
        fails.append(what)


def setrow(off, v, key='alpha_test'):
    return {'off': off, 'kind': 'set', 'key': key, 'fn': SETTER, 'via': None, 'tail': False, 'args': {'r4': {'imm': [v], 'root': None}},
            'restore': None, 'value_reg': 'r4'}


class Fake:
    def __init__(self, pr, rows=None, callers=None, callees=None, leaves=()):
        self.pr, self.r, self.c, self.e, self.lv = pr, rows or {}, callers or {}, callees or {}, frozenset(leaves)

    def prune(self, fn, site, callers):
        return self.pr.prune(fn, site, callers)

    def draw_leaves(self):
        return self.lv

    def categories(self):
        return {}, frozenset(), []

    def rows(self, f):
        return self.r.get(f, [])

    def callers(self, f):
        return self.c.get(f, [])

    def callees(self, f):
        return self.e.get(f, {})

    def is_setter(self, t):
        return t == SETTER

    def reset(self):
        return {}

    def census(self, field):
        return None


def planted():
    tmp = tempfile.mkdtemp(prefix='qgstate_prune_selftest_')
    try:
        S = PlantSource(tmp)
        T, TB, C1, C2, C3, C4, D = 0x82100000, 0x82100100, 0x82100200, 0x82100300, 0x82100400, 0x82100500, 0x82100900
        # T: the call of D (+8) runs only when in_r4 == 3; TB: it runs only when in_r4 != 0 (a beq skips it)
        S.add(T, ['cmpwi cr6,r4,3', 'bne cr6,<L>', f'bl 0x{D:08x}', ('label', 'L'), 'blr'])
        S.add(TB, ['cmpwi cr6,r4,0', 'beq cr6,<L>', f'bl 0x{D:08x}', ('label', 'L'), 'blr'])
        # C1 passes 0 or 1 (a phi of constants), C2 passes 3, C3 passes its own r5, C4 passes 0
        S.add(C1, ['cmpwi cr6,r3,0', 'beq cr6,<L1>', 'li r4,1', 'b <L2>', ('label', 'L1'), 'li r4,0', ('label', 'L2'), f'bl 0x{T:08x}', 'blr'])
        S.add(C2, ['li r4,3', f'bl 0x{T:08x}', 'blr'])
        S.add(C3, ['mr r4,r5', f'bl 0x{T:08x}', 'blr'])
        S.add(C4, ['li r4,0', f'bl 0x{TB:08x}', 'blr'])
        P = GP.Pruner(S)
        req = P.requires(T, 8)
        check('A the guard of T +8 read from the listing: r4 == 3', req == [('r4', '==', 3, True)], req)
        kept, recs = P.prune(T, 8, [(C1, 0x14, 'call')])
        check('A a caller passing {0, 1} is pruned and named', not kept and len(recs) == 1 and recs[0]['need'] == 'r4 == 3' and recs[0]['vals'] == [0, 1], (kept, recs))
        blind = GP.Pruner(S)
        blind._req[(T, 8)] = []          # twin: a pruner that reads no guard keeps the dead caller
        k2, _r2 = blind.prune(T, 8, [(C1, 0x14, 'call')])
        check('A twin: a pruner that reads no guard FAILS it (keeps the caller)', len(k2) == 1)
        kept, recs = P.prune(T, 8, [(C2, 4, 'call')])
        check('A a caller passing 3 is kept', len(kept) == 1 and not recs, (kept, recs))
        kept, recs = P.prune(T, 8, [(C3, 4, 'call')])
        check('A a caller passing a register is kept (unknown never prunes)', len(kept) == 1 and not recs, (kept, recs))
        kept, recs = P.prune(T, 8, [(C1, 0x14, 'call'), (C2, 4, 'call'), (C3, 4, 'call')])
        check('A mixed: only the {0, 1} caller dropped', [k[0] for k in kept] == [C2, C3] and len(recs) == 1, (kept, recs))
        check('A the footer names the function, the need and the values',
              GP.footer(recs) == f'pruned: 1 path(s) (sub_{T:08X} needs r4 == 3 at +0x8; callers pass {{0, 1}})', GP.footer(recs))
        check('B a beq that skips the call: needs r4 != 0', P.requires(TB, 8) == [('r4', '!=', 0, True)], P.requires(TB, 8))
        kept, recs = P.prune(TB, 8, [(C4, 4, 'call')])
        check('B a caller passing 0 is pruned', not kept and recs and recs[0]['need'] == 'r4 != 0', (kept, recs))
        # C: the walk. D draws; T calls it at +8. C1 (0 / 1) sets 7 before calling T, C2 (3) sets 9
        rows = {C1: [setrow(0x4, 7)], C2: [setrow(0x0, 9)]}
        callers = {D: [(T, 8, 'call')], T: [(C1, 0x14, 'call'), (C2, 4, 'call')]}
        w = GI.Walker(Fake(P, rows, callers))
        s = w.summary(D, 'alpha_test')
        check('C only the caller that reaches the call is read: 9 settled', s['settled'] and s['value'] == 9, [GI.path_text(p) for p in s['paths']])
        check('C the dropped caller is in the footer', len(w.pruned) == 1 and w.pruned[0]['caller'] == C1 and 'callers pass {0, 1}' in (GP.footer(w.pruned) or ''), w.pruned)
        GI.PLANT = 'no_prune'
        s2 = GI.Walker(Fake(P, rows, callers)).summary(D, 'alpha_test')
        GI.PLANT = ''
        check('C twin no_prune FAILS it (UNSETTLED {7, 9})', not s2['settled'] and {p['v'] for p in s2['paths']} == {7, 9}, [GI.path_text(p) for p in s2['paths']])
        w = GI.Walker(Fake(P, rows, {D: [(T, 8, 'call')], T: [(C1, 0x14, 'call')]}))
        s = w.summary(D, 'alpha_test')
        check('C a parent whose only caller misses the value: no path (dead, named)', not s['settled'] and all(p['v'] is None for p in s['paths']) and len(w.pruned) >= 1,
              ([GI.path_text(p) for p in s['paths']], w.pruned))
        # D: draw paths. P0 calls T (+0); T's call of D (+8) is the guarded hop; D calls the recorder at +0
        S.add(D, ['blr'])
        g = Fake(P, callees={C1: {0x14: [T]}, C2: {4: [T]}, T: {8: [D]}, D: {0: [LEAF]}}, leaves=(LEAF,))
        for root, want in ((C1, 0), (C2, 1)):
            GI.PLANT = ''
            paths, _p = GI.Walker(g).draw_paths(root)
            check(f'D draw paths from {GI.sub(root)}: {want} (C1 passes {{0, 1}}, C2 passes 3)', len(paths) == want, paths)
        GI.PLANT = 'no_prune'
        paths, _p = GI.Walker(g).draw_paths(C1)
        GI.PLANT = ''
        check('D twin no_prune FAILS it (the dropped hop is back)', len(paths) == 1, paths)
        # E: q gstate --leaks: the pass T calls Y (sets and leaves the alpha test) then its guarded call of D (which draws): a leak only
        # when some caller of T can take the guard
        import qgstate_leaks as GL
        Y = 0x82100A00
        ev = dict(rows={Y: [setrow(0x8, 5)]}, callees={T: {0: [Y], 8: [D]}, D: {0: [LEAF]}}, leaves=(LEAF,))
        for cs, want in (([(C1, 0x14, 'call')], 0), ([(C1, 0x14, 'call'), (C2, 4, 'call')], 1)):
            got = GL.leaks(GI.Walker(Fake(P, callers={T: cs}, **ev)), T, ('alpha_test',))
            check(f'E leaks of the guarded pass with callers {[GI.sub(c[0]) for c in cs]}: {want}', len(got) == want, got)
        GI.PLANT = 'no_prune'
        got = GL.leaks(GI.Walker(Fake(P, callers={T: [(C1, 0x14, 'call')]}, **ev)), T, ('alpha_test',))
        GI.PLANT = ''
        check('E twin no_prune FAILS it (the false leak is back)', len(got) == 1, got)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def real():
    try:
        import q as Q_
        con = Q_.connect(Q_.DB)
        con.execute('SELECT 1 FROM edges LIMIT 1')
    except Exception as e:  # noqa: BLE001 - no index: said, exit 0
        print(f'SKIP real: no q index ({e})')
        return
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'gstate', '--inherit', 'sub_82813228', '--json'],
                       capture_output=True, text=True, timeout=600)
    try:
        doc = json.loads(r.stdout)
    except ValueError:
        check('real: --inherit --json answers', False, r.stdout[-400:] + r.stderr[-400:])
        return
    pr = [x for x in doc.get('pruned', []) if x['fn'] == 0x827E7338 or x['fn'] == 'sub_827E7338']
    check('real sub_827E7338 is named pruned: needs r4 == 3, its caller sub_827E8080 passes {0, 1}',
          any(x['need'] == 'r4 == 3' and x['vals'] == [0, 1] and x['caller'] == 'sub_827E8080' for x in pr), doc.get('pruned'))
    via = [f['value'] for f in doc['inherited'] if f['field'] == 'alpha_ref']
    texts = json.dumps(via)
    check('real alpha_ref: no path runs through sub_827E7338', 'sub_827E7338' not in texts, texts[:600])


planted()
real()
print('qgstate_prune_selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'all passed'))
sys.exit(1 if fails else 0)

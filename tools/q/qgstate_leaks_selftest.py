"""qgstate_leaks_selftest: q gstate --leaks (S111 GSTATELEAK) on planted passes, each with its planted twin, then the real shadow pass.
  A  a caster sets the alpha ref and returns without restoring; the pass restores the ALPHA TEST (another key) and draws: LEAK
  B  the caster saves and restores the ref: no LEAK                                    (the planted twin of the fault)
  C  the pass itself sets the ref before the draw: no LEAK
  D  twin leak_any_restore: a restore of any key ends the leaks, so A prints none (FAILS case A)
  real  sub_827E7F00: the three casters' alpha_ref (1, [in_r3+1644], 128) reach the circles' draw at +124 (sub_82813228)
Skipped (said) when the q index is absent. 0 game runs.  Run:  py -I tools/q/qgstate_leaks_selftest.py"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qgstate_inherit as GI  # noqa: E402
import qgstate_leaks as GL  # noqa: E402

fails = []
SETTER, LEAF = 0x82BB75E8, 0x900
PASS, CASTER, DRAW = 0x100, 0x200, 0x300


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:900]))
    if not ok:
        fails.append(what)


def setrow(off, v, key):
    return {'off': off, 'kind': 'set', 'key': key, 'fn': SETTER, 'via': None, 'tail': False,
            'args': {'r4': {'imm': [v], 'root': None}}, 'restore': None, 'value_reg': 'r4'}


def read(off, key):
    return {'off': off, 'kind': 'read', 'key': key, 'fn': 0x82BB8188, 'tail': False}


def restore(off, save, key):
    return {'off': off, 'kind': 'restore', 'key': key, 'fn': SETTER, 'via': None, 'tail': False,
            'args': {'r4': {'imm': None, 'root': 'ret#X'}}, 'restore': save, 'value_reg': 'r4'}


class Fake:
    def __init__(self, rows, callees):
        self.r, self.e = rows, callees

    def draw_leaves(self):
        return frozenset({LEAF})

    def rows(self, f):
        return self.r.get(f, [])

    def callers(self, f):
        return []

    def callees(self, f):
        return self.e.get(f, {})

    def is_setter(self, t):
        return t == SETTER


def run(rows, plant=''):
    GI.PLANT = plant
    g = Fake(rows, {PASS: {0x10: [CASTER], 0x30: [DRAW]}, CASTER: {0x20: [LEAF]}, DRAW: {0x8: [LEAF]}})
    out = GL.leaks(GI.Walker(g), PASS, ('alpha_ref', 'alpha_test'))
    GI.PLANT = ''
    return out


def planted():
    pass_rows = [setrow(0x4, 0, 'alpha_test'), restore(0x20, 0x4, 'alpha_test')]   # the pass saves / restores the TEST only
    a = run({PASS: pass_rows, CASTER: [setrow(0x8, 1, 'alpha_ref')]})
    check('A the ref leaks past the alpha-test restore: 1 LEAK, value 1, at the draw +30', len(a) == 1 and a[0]['key'] == 'alpha_ref'
          and a[0]['value'] == 1 and a[0]['draw_off'] == 0x30 and a[0]['draw_fn'] == DRAW and a[0]['setter'] == CASTER, a)
    b = run({PASS: pass_rows, CASTER: [read(0x4, 'alpha_ref'), setrow(0x8, 1, 'alpha_ref'), restore(0x28, 0x4, 'alpha_ref')]})
    check('B the caster restores the ref: no LEAK', not b, b)
    c = run({PASS: pass_rows + [setrow(0x28, 5, 'alpha_ref')], CASTER: [setrow(0x8, 1, 'alpha_ref')]})
    check('C the pass sets the ref itself before the draw: no LEAK', not c, c)
    d = run({PASS: pass_rows, CASTER: [setrow(0x8, 1, 'alpha_ref')]}, 'leak_any_restore')
    check('D twin leak_any_restore (a restore of any key ends the leak) FAILS case A', not d, d)


def real():
    try:
        import q as Q_
        Q_.connect(Q_.DB).execute('SELECT 1 FROM edges LIMIT 1')
    except Exception as e:  # noqa: BLE001
        print(f'SKIP real: no q index ({e})')
        return
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'gstate', '--leaks', 'sub_827E7F00', '--field', 'alpha_ref'],
                       capture_output=True, text=True, timeout=900)
    ls = [x for x in r.stdout.splitlines() if 'LEAK key=alpha_ref' in x and 'reaches draw at +124 (sub_82813228)' in x]
    for what, needle in (('1', '(value 1)'), ('+1644', '[in_r3+1644]'), ('128', '(value 128)')):
        check(f'real sub_827E7F00: a caster\'s alpha_ref {what} reaches the circles\' draw at +124', any(needle in x for x in ls), r.stdout[-900:] + r.stderr[-300:])


planted()
real()
print('qgstate_leaks_selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'all passed'))
sys.exit(1 if fails else 0)

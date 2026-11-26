r"""Self-test of tools/q/qsort.py: planted facts for every class and the refusals, then four functions of the real index (SKIPPED without
one). py -I tools/q/qsort_selftest.py          --loosen = the trivial rule loosened: the planted non-trivial cases must then FAIL (exit 1)"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qsort  # noqa: E402

FAILS = []


def check(name, cond):
    print(('ok   ' if cond else 'FAIL ') + name)
    if not cond:
        FAILS.append(name)


def fn(**kw):
    f = {'db': None, 'game': None, 'lib': None, 'pat': [], 'n_insns': 3, 'n_call': 0, 'n_tail': 0, 'n_bctr': 0,
         'loops': 0, 'switches': 0, 'chains': 0, 'twin': None}
    f.update(kw)
    return f


if '--loosen' in sys.argv:
    qsort.TRIVIAL_MAX_INSNS = 100
    qsort.TRIVIAL_ALLOW_LOOPS = True
    print('LOOSENED: the planted non-trivial cases below must FAIL')

cls = lambda **kw: qsort.classify_one(fn(**kw))[0]  # noqa: E731
# --- planted
check('a getter is LEAF-TRIVIAL', cls() == 'LEAF-TRIVIAL')
check('a single tail branch is a THUNK', cls(n_tail=1) == 'THUNK')
check('a single indirect branch is a THUNK', cls(n_bctr=1) == 'THUNK')
check('a tail branch with a call is not a thunk', cls(n_tail=1, n_call=1) != 'THUNK')
check('two tail branches are not a thunk', cls(n_tail=2) != 'THUNK')
check('PLANTED non-trivial: 30 insns, no call, no loop', cls(n_insns=30) == 'LEFT')
check('PLANTED non-trivial: 6 insns with a loop', cls(n_insns=6, loops=1) == 'LEFT')
check('PLANTED non-trivial: 4 insns with a switch', cls(n_insns=4, switches=1) == 'LEFT')
check('a leaf that calls is not trivial', cls(n_insns=5, n_call=1) == 'LEFT')
check('db beats everything', cls(db='x', lib=('Havok', 's', 'e'), pat=['p']) == 'DB')
check('game name', cls(game='n') == 'GAME-NAMED')
check('library', cls(lib=('Lua', 'strings', 'e'), n_insns=50) == 'LIB')
check('pattern', cls(pat=['field_accessor'], n_insns=50) == 'PATTERN')
check('a clone of a read function', cls(n_insns=50, twin=(0x82000000, 'identical text', 'db entry x')) == 'CLONE')
# review SORT40: a twin equal only with the immediates ignored does other work (another field, tag, global): never CLONE
check('PLANTED: a same-shape twin with other constants stays LEFT',
      cls(n_insns=50, twin=(0x82000000, 'same text, immediates ignored', 'db entry x')) == 'LEFT')
check('the LEFT shape twin is named as a hint', 'sub_82000000' in qsort.classify_one(fn(n_insns=50, twin=(0x82000000, 'same text, immediates ignored', 'x')))[1])
check('pick_twin never returns the function itself', qsort.pick_twin([(0, 5, 'a')], 5) is None)
check('pick_twin prefers a db entry over a reading', qsort.pick_twin([(2, 9, 'r'), (0, 7, 'd')], 1) == (7, 'd'))

# review SORT40: the db part of the key is the CONTENT of db/*.json, not mtimes (a worktree checks the same files out with other mtimes,
# and the table is shared by every checkout: an mtime key rebuilt it on every call that alternated between two checkouts)
import shutil  # noqa: E402
import tempfile  # noqa: E402
_d = tempfile.mkdtemp(prefix='qsort_fp_')
try:
    for _n, _t in (('a.json', '[{"address": "0x82000000", "name": "a"}]'), ('b.json', '[]')):
        with open(os.path.join(_d, _n), 'w', encoding='utf-8') as _f:
            _f.write(_t)
    _k0 = qsort.db_fingerprint(_d)
    os.utime(os.path.join(_d, 'a.json'), (1000000000, 1000000000))
    check('PLANTED: another mtime, the same db content -> the same key', qsort.db_fingerprint(_d) == _k0)
    with open(os.path.join(_d, 'a.json'), 'wb') as _f:   # the main checkout holds files of other line endings than a fresh worktree
        _f.write(b'[{"address": "0x82000000",\r\n "name": "a"}]')
    _k1 = qsort.db_fingerprint(_d)
    with open(os.path.join(_d, 'a.json'), 'wb') as _f:
        _f.write(b'[{"address": "0x82000000",\n "name": "a"}]')
    check('PLANTED: CRLF or LF line endings of one file -> the same key', qsort.db_fingerprint(_d) == _k1)
    with open(os.path.join(_d, 'b.json'), 'w', encoding='utf-8') as _f:
        _f.write('[{"address": "0x82000004", "name": "b"}]')
    check('another db content -> another key', qsort.db_fingerprint(_d) != _k0)
finally:
    shutil.rmtree(_d, ignore_errors=True)

# --- the real index
try:
    con = qsort.open_sort()
except Exception as e:  # noqa: BLE001
    print('SKIPPED real index: %s' % e)
    con = None
if con is not None:
    for name, a, want in [('thunk (one tail branch)', 0x82512930, 'THUNK'), ('trivial getter', 0x823A3148, 'LEAF-TRIVIAL'),
                          ('clone of a db-entry function', 0x82ED83D8, 'CLONE'), ('Havok library function', 0x82F6F910, 'LIB'),
                          # review SORT40: the old clone example, an h_noimm twin testing 91 / 34000..34099 where its twin tests 68 / 5650..5749
                          ('same-shape twin with other ids', 0x825653B0, 'LEFT')]:
        r = con.execute('select cls,evidence from sort_fn where func=?', (a,)).fetchone()
        check('real %s sub_%08X -> %s (got %s)' % (name, a, want, r and r[0]), bool(r) and r[0] == want)
    r = con.execute("select evidence from sort_fn where func=?", (0x82ED83D8,)).fetchone()
    check('the clone names its twin', bool(r) and 'sub_82EDFBE8' in r[0])
    # review SORT40: sub_826781D0 increments +130 of the 1576-byte record, its h_noimm twin sub_826776F0 (increment_halfword_568...) +568
    r = con.execute('select cls,evidence from sort_fn where func=?', (0x826781D0,)).fetchone()
    check('real same-shape twin with another field sub_826781D0 -> LEFT naming sub_826776F0 (got %s)' % (r and r[0]),
          bool(r) and r[0] == 'LEFT' and 'sub_826776F0' in r[1])
    check('no CLONE through immediates ignored',
          con.execute("select count(*) from sort_fn where cls='CLONE' and evidence like '%immediates ignored%'").fetchone()[0] == 0)
    n = con.execute('select count(*) from sort_fn').fetchone()[0]
    check('the sorted set is not empty (33,121 at S111; a rebuilt index may move it)', n > 0)
    check('no LEAF-TRIVIAL over %d insns' % qsort.TRIVIAL_MAX_INSNS,
          con.execute("select count(*) from sort_fn where cls='LEAF-TRIVIAL' and n_insns>?", (qsort.TRIVIAL_MAX_INSNS,)).fetchone()[0] == 0)

print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all ok')
sys.exit(1 if FAILS else 0)

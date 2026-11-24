r"""Self-test of tools/q/qlib.py: planted units for the `strings` and `range` rules (and their refusals), then the real index
(SKIPPED without one). py -I tools/q/qlib_selftest.py"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qlib  # noqa: E402

FAILS = []


def check(name, cond):
    print(('ok   ' if cond else 'FAIL ') + name)
    if not cond:
        FAILS.append(name)


def unit(texts=(), nstr=0, dbs=()):
    return {'texts': list(texts), 'nstr': nstr, 'dbs': list(dbs)}


def spans(n, step=0x100, base=0x82000000):
    return {u: (base + u * step, base + u * step + 0x80) for u in range(n)}


# --- the patterns
for t, lib in [('hkp3AxisSweep', 'Havok'), ('Compression\\Predictive\\hkaPredictiveBlockCompression.cpp', 'Havok'), ('hclClothState', 'Havok'),
               ('AK::BankManager', 'Wwise'), ('Lua\\source\\lapi.cpp', 'Lua'), ('luaO_nilobject', 'Lua'),
               ('C:\\devel\\projects\\bink\\src\\sdk\\decode\\binkread.c', 'Bink'), ('invalid block type', 'zlib'), ('R6002', 'CRT')]:
    check('pattern %s -> %s' % (t[:30], lib), qlib.libs_of(t) == {lib})
for t in ('hk', 'Akira', 'Break', 'SmartMenu', 'g_fHdLigLvl', '[%d] %d', 'Block (', '', None, 'x' * 700 + 'hkpWorld'):
    check('no library: %r' % (t if t is None or len(t) < 20 else t[:20]), qlib.libs_of(t) == set())

# --- strings rule
per = {0: unit([('rtti', 'hkpWorld')]),                                   # Havok by RTTI
       1: unit([('str', 'LuaAPI failure: %s')], nstr=1),                  # Lua by a string
       2: unit([('rtti', 'hkpWorld'), ('game', 'wrestler.getName')]),     # a Lua binding name: game code inside
       3: unit([('str', 'hkpWorld'), ('str', 'lua_pushnil')], nstr=2),    # two libraries: unsure
       4: unit([('rtti', 'hkpWorld'), ('rtti', 'CWrestler')]),            # a game class inside
       5: unit([('name', 'AK::BankManager')]),                            # a fn_string name
       6: unit([], nstr=3)}                                               # strings that match nothing
r = qlib.classify(per, spans(7))
check('strings: Havok by rtti', r.get(0, ('',))[0:2] == ('Havok', 'strings'))
check('strings: Lua by a string', r.get(1, ('',))[0:2] == ('Lua', 'strings'))
check('strings: a game name inside voids it', 2 not in r)
check('strings: two libraries is unsure', 3 not in r)
check('strings: a game RTTI class inside voids it', 4 not in r)
check('strings: Wwise by a fn_string name', r.get(5, ('',))[0:2] == ('Wwise', 'strings'))
check('strings: nothing matched', 6 not in r)

# --- range rule
H = ('rtti', 'hkpWorld')


def blocks(**mid):
    p = {0: unit([H]), 3: unit([H])}
    p.update(mid)
    return p


r = qlib.classify(blocks(), spans(4))
check('range: clean units between two Havok units', r.get(1, ('',))[0:2] == ('Havok', 'range') and r.get(2, ('',))[0:2] == ('Havok', 'range'))
check('range: the evidence says where', r[1][2] == 'between 0 and 3')
r = qlib.classify({0: unit([H]), 1: unit(nstr=1), 2: unit(), 3: unit([H])}, spans(4))
check('range: one string reference voids the whole block', 1 not in r and 2 not in r)
check('range: a game name voids the block', 2 not in qlib.classify({0: unit([H]), 1: unit([('name', 'CWrestler::Draw')]), 2: unit(), 3: unit([H])}, spans(4)))
check('range: a db name that is not the library voids it', 1 not in qlib.classify({0: unit([H]), 1: unit(dbs=['draw_wrestler']), 2: unit(), 3: unit([H])}, spans(4)))
check('range: a db name of the library does not', 1 in qlib.classify({0: unit([H]), 1: unit(dbs=['hkp_world_step']), 2: unit(), 3: unit([H])}, spans(4)))
check('range: a wide address span is refused', 1 not in qlib.classify(blocks(), spans(4, step=0x10000)))
check('range: two different libraries at the ends', 1 not in qlib.classify({0: unit([H]), 1: unit(), 2: unit([('str', 'lua_pushnil')], nstr=1)}, spans(3)))
check('range: no unit between is not a range', set(qlib.classify({0: unit([H]), 1: unit([H])}, spans(2))) == {0, 1})
check('range: another library between the two voids the span',
      1 not in qlib.classify({0: unit([H]), 1: unit(), 2: unit([('str', 'lua_pushnil')], nstr=1), 3: unit(), 4: unit([H])}, spans(5)) and
      3 not in qlib.classify({0: unit([H]), 1: unit(), 2: unit([('str', 'lua_pushnil')], nstr=1), 3: unit(), 4: unit([H])}, spans(5)))

# --- the real index
if os.path.exists(qlib.qnames.INDEX_DB) and os.path.exists(qlib.qunits.UNITS_DB) and os.path.exists(qlib.qnames.NAMES_DB):
    tmp = os.path.join(tempfile.mkdtemp(), 'lib.s1.db')
    qlib.build(tmp)
    con = qlib.open_lib(tmp)
    for u, lib in ((34859, 'Bink'), (17772, 'Wwise'), (15064, 'Lua'), (20906, 'Havok'), (22569, 'Havok')):
        r = qlib.lookup(u, con)
        check('real unit %d is %s' % (u, lib), bool(r) and r[0] == lib)
    for u in (1784, 5600, 4638, 14500, 9671):
        check('real unit %d is NOT library' % u, qlib.lookup(u, con) is None)
    con.close()
else:
    print('SKIPPED real-index part (no index)')
print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)

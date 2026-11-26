r"""Self-test of tools/q/qlsheet_ctx.py on planted databases: pick_connected (unnamed first, callers, observed callers, under 40 bytes ignored,
ties by size), and (S111 FOLD) that the listing sheet no longer carries the neighbours' earlier readings, which lent a reading their confidence.
py -I tools/q/qlsheet_ctx_selftest.py"""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qlsheet_ctx as X  # noqa: E402

FAILS = []


def check(name, cond):
    print(('ok   ' if cond else 'FAIL ') + name)
    if not cond:
        FAILS.append(name)


def mem():
    return sqlite3.connect(':memory:')


ix, un, seen, nm = mem(), mem(), mem(), mem()
ix.executescript('create table edges(src integer, dst integer, target integer, site integer, kind text);'
                 'create table funcs(addr integer, size integer, name text);')
un.executescript('create table units(func integer primary key, unit_id integer);'
                 'create table unit(unit_id integer primary key, lo integer, hi integer, n_funcs integer);')
seen.executescript('create table seen(site integer, func integer, target integer, runs integer);')
nm.executescript('create table names(func integer, name text, source text, table_addr integer);')

funcs = {}     # unit -> [(addr, size)]


def add_unit(u, sizes):
    base = 0x10000 * (u + 1)
    fs = [(base + 0x100 * i, s) for i, s in enumerate(sizes)]
    funcs[u] = fs
    un.execute('insert into unit values (?,?,?,?)', (u, fs[0][0], fs[-1][0], len(fs)))
    for a, s in fs:
        un.execute('insert into units values (?,?)', (a, u))
        ix.execute('insert into funcs values (?,?,?)', (a, s, None))


def call(su, si, du, di, kind='call'):
    ix.execute('insert into edges values (?,?,?,?,?)', (funcs[su][si][0], funcs[du][di][0], funcs[du][di][0], 0, kind))


def observed(su, si, du, di):
    seen.execute('insert into seen values (?,?,?,?)', (1, funcs[su][si][0], funcs[du][di][0], 1))


# --- S111 FOLD: no neighbour readings on the listing sheet (a reading's confidence is its own)
check('FOLD: qlsheet_ctx has no neighbours / format_neighbours / reading', not any(hasattr(X, n) for n in ('neighbours', 'format_neighbours', 'choose', 'unit_edges')))
check('FOLD: Data holds no reading store', not hasattr(X.Data, 'reading'))
src = open(os.path.join(HERE, 'qlsheet.py'), encoding='utf-8').read()
check('FOLD: qlsheet.py writes no # NEIGHBOURS part and does not call the neighbour code', 'NEIGHBOURS' not in src and 'neighbours(' not in src)

# --- pick_connected: A named 200B 1 caller; B 100B 2 direct + 1 observed; C 30B 9 callers; D 300B 3 callers; E 120B 0 callers
add_unit(40, [200, 100, 30, 300, 120])
add_unit(41, [100] * 12)
call(41, 0, 40, 0)
call(41, 1, 40, 1)
call(41, 2, 40, 1)
observed(41, 3, 40, 1)
for i in range(9):
    call(41, 3 + i, 40, 2)
for i in range(3):
    call(41, i, 40, 3)
ix.execute('update funcs set name=? where addr=?', ('GameThing', funcs[40][0][0]))
d = X.Data(ix=ix, un=un, seen=seen, nm=nm, dbdir=tempfile.mkdtemp())
check('pick: unnamed first, by callers, ties by size, <40 B ignored',
      X.pick_connected(40, 4, d) == [funcs[40][3][0], funcs[40][1][0], funcs[40][4][0], funcs[40][0][0]])
check('pick: at most n', len(X.pick_connected(40, 2, d)) == 2)
check('pick: a caller set counts distinct callers', X.connectivity(d, 40)[funcs[40][1][0]] == 3)
print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)

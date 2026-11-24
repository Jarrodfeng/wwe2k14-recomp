r"""Self-test of tools/q/qlsheet_ctx.py on planted databases: the neighbours (counts, weak readings, the fill-up to 3, top 6, the 160 cut,
the plain "leads" line) and pick_connected (unnamed first, callers, observed callers, under 40 bytes ignored, ties by size).
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


ix, un, seen, rd, nm = mem(), mem(), mem(), mem(), mem()
ix.executescript('create table edges(src integer, dst integer, target integer, site integer, kind text);'
                 'create table funcs(addr integer, size integer, name text);')
un.executescript('create table units(func integer primary key, unit_id integer);'
                 'create table unit(unit_id integer primary key, lo integer, hi integer, n_funcs integer);')
seen.executescript('create table seen(site integer, func integer, target integer, runs integer);')
rd.executescript('create table unit_read(id integer primary key, unit_id integer, purpose text, confidence text, system text);')
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


def read(u, purpose, conf='high', system='crowd'):
    rd.execute('insert into unit_read(unit_id,purpose,confidence,system) values (?,?,?,?)', (u, purpose, conf, system))


# --- neighbours of unit 1: callers 2,3,4,5 good (3,2,1,1 pairs); 6 unknown (5); 7 low (4); 8 no reading (9)
add_unit(1, [100, 100, 100])
for u in range(2, 9):
    add_unit(u, [100] * 10)
for i in range(3):
    call(2, i, 1, 0)
call(3, 0, 1, 1)
observed(3, 1, 1, 1)                  # an OBSERVED pair joins the direct ones (2 pairs)
call(3, 0, 1, 1)                      # the same direct pair twice counts once
call(4, 0, 1, 2)
call(5, 0, 1, 2, 'tail')
for i in range(5):
    call(6, i, 1, 0)
for i in range(4):
    call(7, i, 1, 1)
for i in range(9):
    call(8, i, 1, 2)
read(2, 'long ' + 'x' * 400)
read(3, 'old answer', 'low')
read(3, 'draws the crowd', 'medium')    # the NEWEST row is the answer
read(4, 'ring ropes', 'high', 'match')
read(5, 'camera cut', 'medium', 'camera')
read(6, 'unknown purpose', 'high')
read(7, 'a guess', 'low')
# unit 1 calls out to unit 2 (2 pairs, good) only
call(1, 0, 2, 0)
call(1, 1, 2, 0)
d = X.Data(ix=ix, un=un, seen=seen, rd=rd, nm=nm, dbdir=tempfile.mkdtemp())
nb = X.neighbours(1, d)
check('callers: the good ones by edge count, weak and unread left out', [x['unit'] for x in nb['in']] == [2, 3, 4, 5])
check('callers: counts are distinct pairs, direct + observed', [x['n'] for x in nb['in']] == [3, 2, 1, 1])
check('callers: newest reading wins', nb['in'][1]['purpose'] == 'draws the crowd' and nb['in'][1]['confidence'] == 'medium')
check('callers: purpose cut to 160', len(nb['in'][0]['purpose']) == 160 and nb['in'][0]['purpose'].endswith('~'))
check('callees: unit 2 only (2 pairs)', [(x['unit'], x['n']) for x in nb['out']] == [(2, 2)])
txt = X.format_neighbours(nb)
check('format: says plainly these are earlier readings, not facts', 'EARLIER MODEL READINGS' in txt and 'NOT facts' in txt)
check('format: a line', 'called by unit 3 (x2) [crowd, medium]: draws the crowd' in txt and 'calls unit 2 (x2) [crowd, high]: long' in txt)

# fewer than 3 good: the weak ones fill up to 3 (best edge count first); never an unread one
add_unit(10, [100, 100])
for u in (11, 12, 13, 14):
    add_unit(u, [100] * 6)
call(11, 0, 10, 0)
for i in range(5):
    call(12, i, 10, 0)
for i in range(2):
    call(13, i, 10, 1)
for i in range(6):
    call(14, i, 10, 1)
read(11, 'good one', 'high')
read(12, 'unknown', 'high')
read(13, 'a guess', 'low')
d = X.Data(ix=ix, un=un, seen=seen, rd=rd, nm=nm, dbdir=tempfile.mkdtemp())
nb = X.neighbours(10, d)
check('weak readings fill up to 3, never an unread unit', [x['unit'] for x in nb['in']] == [12, 13, 11])
# more than 6 good
add_unit(20, [100])
for u in range(21, 29):
    add_unit(u, [100])
    call(u, 0, 20, 0)
    read(u, 'p%d' % u)
d = X.Data(ix=ix, un=un, seen=seen, rd=rd, nm=nm, dbdir=tempfile.mkdtemp())
nb = X.neighbours(20, d)
check('top 6 each way', len(nb['in']) == 6)
add_unit(30, [100])
check('a unit nobody calls prints nothing', X.format_neighbours(X.neighbours(30, d)) == '')

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
d = X.Data(ix=ix, un=un, seen=seen, rd=rd, nm=nm, dbdir=tempfile.mkdtemp())
check('pick: unnamed first, by callers, ties by size, <40 B ignored',
      X.pick_connected(40, 4, d) == [funcs[40][3][0], funcs[40][1][0], funcs[40][4][0], funcs[40][0][0]])
check('pick: at most n', len(X.pick_connected(40, 2, d)) == 2)
check('pick: a caller set counts distinct callers', X.connectivity(d, 40)[funcs[40][1][0]] == 3)
print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all passed')
sys.exit(1 if FAILS else 0)

"""Self-test of `q reach` (S111 QREACH): `py -I tools/q/qreach_selftest.py [--no-real]`   (0 game runs)

A. A planted call graph (in-memory sqlite, invented addresses, no game code), each check with its twin (the same check against a WRONG
   expectation must FAIL):
     path        A -> X -> Y -> B by calls: the shortest path with each call-site offset; a tail call shows `tail`
     shortest    a long and a short route: the short one is printed, both shortest ties are listed
     no path     B not downstream of A: "no path within N (visited K functions; cut: ...)" with K counted
     depth cut   B 3 calls away, --depth 2: no path, the cut says depth and counts the functions at the last level
     indirect    B only behind a bctrl: no path without --indirect (the cut counts the bctrl site); found with --indirect through a resolved
                 vcall row (status exact) and, separately, through an observed seen row; an unresolved vcall row (status many) is not followed
     helper      a __savegprlr_ callee is not walked
B. The real re-finds on the local tree: `q reach sub_8235BA80 sub_822CA740 --depth 4` = no path, and `q reach sub_82825A58 sub_827E5848` = found.
   A machine without the q index SKIPs part B.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qreach  # noqa: E402

nf = nt = 0


def check(ok, what, info=''):
    global nf
    if not ok:
        nf += 1
        print('FAIL', what, info)
    else:
        print('ok  ', what)


def twin(ok, what):
    """the same check with a wrong expectation: it must come out False"""
    global nt
    if ok:
        nt += 1
        print('TWIN NOT CAUGHT', what)
    else:
        print('ok   twin', what)


A, X, Y, B, S, L, H, V, Z, T, U, W = (0x82000000 + 0x1000 * i for i in range(1, 13))


def planted():
    ix = sqlite3.connect(':memory:')
    ix.executescript('create table funcs(addr integer, name text, n_bctrl integer);'
                     'create table edges(src integer, dst integer, target integer, site integer, kind text);')
    names = {A: 'sub_%08X' % A, H: '__savegprlr_14'}
    nb = {V: 1, U: 2, W: 0}      # U: one observed site (+0x40) and one unresolved (+0x30)
    for f in (A, X, Y, B, S, L, H, V, Z, T, U, W):
        ix.execute('insert into funcs values (?,?,?)', (f, names.get(f), nb.get(f, 0)))
    for s, d, off, k in ((A, X, 0x10, 'call'), (X, Y, 0x20, 'call'), (Y, B, 0, 'tail'),        # the path (the last a tail)
                         (A, S, 0x30, 'call'), (S, L, 0x14, 'call'),                           # a branch with no B
                         (A, H, 0x40, 'call'), (H, Z, 0x4, 'call'),                            # a helper that would lead to Z
                         (V, W, 0x8, 'call'), (A, V, 0x50, 'call'), (A, U, 0x60, 'call')):
        ix.execute('insert into edges values (?,?,?,?,?)', (s, d, d, s + off, k))
    vc = sqlite3.connect(':memory:')
    vc.executescript('create table icall(site integer, func integer, kind text, shape text, off integer, recv text, status text, n integer);'
                     'create table icall_target(site integer, target integer, how text);')
    vc.execute("insert into icall values (?,?,?,?,?,?,?,?)", (V + 0x30, V, 'bctrl', 's', 8, 'r3', 'exact', 1))
    vc.execute("insert into icall_target values (?,?,?)", (V + 0x30, T, 'rule'))
    vc.execute("insert into icall values (?,?,?,?,?,?,?,?)", (U + 0x30, U, 'bctrl', 's', 8, 'r3', 'many', 9))
    vc.execute("insert into icall_target values (?,?,?)", (U + 0x30, Z, 'rule'))
    seen = sqlite3.connect(':memory:')
    seen.execute('create table seen(site integer, func integer, target integer, runs integer)')
    seen.execute('insert into seen values (?,?,?,?)', (U + 0x40, U, B, 2))
    return ix, vc, seen


def run(ix, vc, seen, a, b, depth=6, indirect=False):
    g = qreach.Graph(ix, vc, seen, indirect)
    return g, qreach.reach(g, a, b, depth)


def hops(res, i=0):
    return [(h['func'], h['off'], h['kind']) for h in res['paths'][i]]


def part_a():
    ix, vc, seen = planted()
    g, r = run(ix, vc, seen, A, B)
    got = hops(r) if r['paths'] else None
    check(r['found'] and r['dist'] == 3 and got == [(A, 0x10, 'call'), (X, 0x20, 'call'), (Y, 0, 'tail')],
          'path: A +0x10 -> X +0x20 -> Y tail -> B', got)
    twin(r['dist'] == 2, 'path: wrong distance 2')
    check(qreach.show_hop(g, Y, 0, 'tail') == 'sub_%08X tail' % Y, 'path: a tail hop prints `tail`')
    # no path: B is not under L
    g, r = run(ix, vc, seen, S, B)
    check(not r['found'] and r['visited'] == 2 and 'depth' not in r['cuts'], 'no path: S reaches only L (visited 2)', r)
    twin(r['visited'] == 3, 'no path: wrong visited count')
    # depth cut
    g, r = run(ix, vc, seen, A, B, depth=2)
    check(not r['found'] and r['cuts'].get('depth', 0) >= 1, 'depth cut: B is 3 calls away, depth 2 is a cut', r)
    twin(qreach.cut_text(r) == 'none', 'depth cut: the cut text is not "none"')
    g, r = run(ix, vc, seen, A, B, depth=3)
    check(r['found'], 'depth cut: depth 3 finds it')
    # indirect
    g, r = run(ix, vc, seen, V, T)
    check(not r['found'] and r['cuts'].get('indirect') == 1, 'indirect: no path without --indirect, the bctrl site is counted', r)
    g, r = run(ix, vc, seen, V, T, indirect=True)
    check(r['found'] and hops(r)[0][2] == 'vcall', 'indirect: found through the resolved (exact) vcall row', r)
    twin(not r['found'], 'indirect: twin expects no path with --indirect')
    g, r = run(ix, vc, seen, U, B, indirect=True)
    check(r['found'] and hops(r)[0][2] == 'seen', 'indirect: found through an observed edge', r)
    g, r = run(ix, vc, seen, U, Z, indirect=True)
    check(not r['found'], 'indirect: an unresolved (many) vcall row is not followed', r)
    check(r['cuts'].get('indirect') == 1, 'indirect: the unresolved site is counted as a cut', r)
    # helper
    g, r = run(ix, vc, seen, A, Z)
    check(not r['found'], 'helper: a __savegprlr_ callee is not walked')
    # multi seeds
    g = qreach.Graph(ix, vc, seen, False)
    r = qreach.reach(g, [S, X], B, 6)
    check(r['found'] and r['dist'] == 2 and hops(r)[0][0] == X, 'seeds: several start functions, the nearer one wins', r)
    # ties
    ix.execute('insert into funcs values (?,?,?)', (0x83000000, None, 0))
    ix.execute('insert into edges values (?,?,?,?,?)', (A, 0x83000000, 0x83000000, A + 0x70, 'call'))
    ix.execute('insert into edges values (?,?,?,?,?)', (0x83000000, B, B, 0x83000010, 'call'))
    g, r = run(ix, vc, seen, A, B)
    check(r['dist'] == 2 and len(r['paths']) == 1, 'shortest: the 2-call route beats the 3-call route', r)


def qrun(*args):
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py')] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def part_b():
    rc, out = qrun('reach', 'sub_8235BA80', 'sub_822CA740', '--depth', '4')
    if 'index' in out.lower() and rc != 0:
        print('SKIP part B (no q index here):', out.strip()[:100])
        return
    check(rc == 0 and out.startswith('no path within 4 (visited') and 'cut:' in out, 're-find: the close step never reaches sub_822CA740 in 4', out)
    rc, out = qrun('reach', 'sub_82825A58', 'sub_827E5848')
    check(rc == 0 and 'sub_82825A58 +0x3FC -> sub_827E5848' in out, 're-find: a known chain is found with its call site', out)
    twin('no path' in out, 're-find: twin expects no path for the known chain')


def main():
    part_a()
    if '--no-real' not in sys.argv:
        part_b()
    print('qreach_selftest: %d failed, %d twins not caught' % (nf, nt))
    return 2 if nt else (1 if nf else 0)


if __name__ == '__main__':
    sys.exit(main())

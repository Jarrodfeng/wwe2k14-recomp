r"""q seen: the indirect call pairs that coverage runs have OBSERVED (S110 lane SEEN).

    py tools/q/q.py seen [ADDR]

No argument: totals (runs, pairs, static sites observed of how many, by static status).
ADDR (a function): the observed callers of it, and the observed targets of its own indirect sites.

Source: every out/harness/coverage/*/icalls.tsv of the main checkout (folders ending _failed included: a pair is true whatever the
script did later), written by a run with --wwe2k14_coverage=true --wwe2k14_icalls=true. Header `func lr target`, hex.
A bctrl row's call site is lr - 4. When lr - 4 is not a static indirect site (vcall.s*.db `icall`), the row is a TAIL call from
`func` (a tail bctr has no site of its own): stored with site 0 and joined on func.
The table lives in out/harness/index/seen.s1.db (rebuilt when the set of tsv files, their mtimes / sizes, or the static site table change).
MAY BE INCOMPLETE: observed = seen in N runs, never a complete list. A site or caller not listed has not been seen in a run,
which is not "never calls".
"""
import glob
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
IDX = os.path.join(MAIN, 'out', 'harness', 'index')
COV = os.path.join(MAIN, 'out', 'harness', 'coverage')
SEEN_DB = os.path.join(IDX, 'seen.s1.db')
INCOMPLETE = 'observed = seen in the recorded runs, never a complete list; a site or caller not listed has not been seen, which is not "never calls".'
MASK = 0xFFFFFFFF


class SeenError(Exception):
    pass


def vdb_file():
    """the static indirect-site table: whatever vcall.s*.db the index folder holds at the newest version (read only, never built here)"""
    env = os.environ.get('WWE_QVCALL_DB')
    if env:
        return env
    best = None
    for f in glob.glob(os.path.join(IDX, 'vcall.s*.db')):
        m = re.search(r'vcall\.s(\d+)\.db$', f)
        if m and (best is None or int(m.group(1)) > best[0]):
            best = (int(m.group(1)), f)
    return best[1] if best else None


def tsv_files():
    return sorted(glob.glob(os.path.join(COV, '*', 'icalls.tsv')))


def run_name(path):
    return os.path.basename(os.path.dirname(path))


def ro(path):
    if not path or not os.path.exists(path):
        raise SeenError('no file %s' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def stamp(files, vdb):
    parts = ['%s:%d:%d' % (run_name(f), int(os.stat(f).st_mtime), os.stat(f).st_size) for f in files]
    if vdb and os.path.exists(vdb):
        st = os.stat(vdb)
        parts.append('vdb:%s:%d:%d' % (os.path.basename(vdb), int(st.st_mtime), st.st_size))
    return '|'.join(parts)


def static_sites(vdb):
    if not vdb or not os.path.exists(vdb):
        return {}
    c = ro(vdb)
    try:
        return {s: st for s, st in c.execute('select site, status from icall')}
    finally:
        c.close()


def site_funcs(vdb):
    """{site: the function that holds it}. A row belongs to the site at lr-4 only when the row's func IS that function:
    a function reached through a bctrl that then tail-branches (bctr) records the CALLER's lr with its own func, and that
    row is a tail call from func, not a target of the caller's site (S110 CLASSFLOW review: 7 of 8 'contradictions' of the
    static rules were this misreading)."""
    if not vdb or not os.path.exists(vdb):
        return {}
    c = ro(vdb)
    try:
        return {s: f for s, f in c.execute('select site, func from icall')}
    finally:
        c.close()


def is_site_row(func, lr, sfunc):
    return sfunc.get(lr - 4) == func


def parse(path):
    """-> [(func, lr, target)]; bad rows skipped"""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for i, line in enumerate(f):
            p = line.split()
            if i == 0 and p[:3] == ['func', 'lr', 'target']:
                continue
            if len(p) < 3:
                continue
            try:
                out.append((int(p[0], 16) & MASK, int(p[1], 16) & MASK, int(p[2], 16) & MASK))
            except ValueError:
                continue
    return out


def build(verbose=False, db_path=None):
    t0 = time.time()
    db_path = db_path or SEEN_DB
    files = tsv_files()
    vdb = vdb_file()
    sfunc = site_funcs(vdb)
    count = {}
    runs_meta = []
    for f in files:
        rows = parse(f)
        keys = set()
        for func, lr, target in rows:
            site = lr - 4 if is_site_row(func, lr, sfunc) else 0
            keys.add((site, func, target))
        for k in keys:
            count[k] = count.get(k, 0) + 1
        runs_meta.append((run_name(f), len(rows), int(os.stat(f).st_mtime)))
    tmp = db_path + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table seen(site integer, func integer, target integer, runs integer);'
                    'create table seen_runs(run text, pairs integer, mtime integer);'
                    'create table meta(key text primary key, value text);')
    w.executemany('insert into seen values (?,?,?,?)', [(s, f, t, n) for (s, f, t), n in sorted(count.items())])
    w.executemany('insert into seen_runs values (?,?,?)', runs_meta)
    w.execute('create index seen_target on seen(target)')
    w.execute('create index seen_site on seen(site)')
    w.execute('create index seen_func on seen(func)')
    w.executemany('insert into meta values (?,?)', [('stamp', stamp(files, vdb)), ('build_s', '%.1f' % (time.time() - t0))])
    w.commit()
    w.close()
    os.replace(tmp, db_path)
    if verbose:
        print('seen.s1.db: %d runs, %d distinct (site, func, target) rows, %.1f s' % (len(files), len(count), time.time() - t0), file=sys.stderr)


def open_seen(db_path=None):
    db_path = db_path or SEEN_DB
    want = stamp(tsv_files(), vdb_file())
    ok = False
    if os.path.exists(db_path):
        c = sqlite3.connect('file:%s?mode=ro' % db_path.replace('\\', '/'), uri=True)
        try:
            ok = dict(c.execute('select key,value from meta')).get('stamp') == want
        except sqlite3.Error:
            ok = False
        finally:
            c.close()
    if not ok:
        build(True, db_path)
    return sqlite3.connect('file:%s?mode=ro' % db_path.replace('\\', '/'), uri=True)


def n_runs(con=None):
    con = con or open_seen()
    return con.execute('select count(*) from seen_runs').fetchone()[0]


def callers_of(target, con=None):
    """-> [(func, site, runs)] observed callers of target; site 0 = a tail call from func"""
    con = con or open_seen()
    return con.execute('select func, site, runs from seen where target=? order by func, site', (target,)).fetchall()


def sites_seen(con=None):
    """-> set of static sites observed in at least one run"""
    con = con or open_seen()
    return set(s for s, in con.execute('select distinct site from seen where site<>0'))


def targets_of(site, con=None):
    """-> [(target, runs)] observed targets of one static site"""
    con = con or open_seen()
    return con.execute('select target, runs from seen where site=? order by target', (site,)).fetchall()


def tail_targets_of(func, con=None):
    con = con or open_seen()
    return con.execute('select target, runs from seen where site=0 and func=? order by target', (func,)).fetchall()


def targets_with_callers(con=None):
    con = con or open_seen()
    return set(t for t, in con.execute('select distinct target from seen'))


def slot_observed(offsets, skip, con=None):
    """(K, N): of the N static sites calling a slot at one of `offsets` (not in `skip`), K have run (observed) """
    vdb = vdb_file()
    if not offsets or not vdb:
        return 0, 0
    con = con or open_seen()
    seen = sites_seen(con)
    v = ro(vdb)
    try:
        ss = set()
        for off in offsets:
            for s, in v.execute("select site from icall where off=? and shape in ('V_this','V_open','F')", (off,)):
                if s not in skip:
                    ss.add(s)
    finally:
        v.close()
    return len(ss & seen), len(ss)


def totals(con=None):
    con = con or open_seen()
    runs = con.execute('select count(*) from seen_runs').fetchone()[0]
    pairs = sum(p for _, p, _ in con.execute('select run,pairs,mtime from seen_runs'))
    seen = sites_seen(con)
    sites = static_sites(vdb_file())
    by = {}
    for s, st in sites.items():
        a = by.setdefault(st, [0, 0])
        a[1] += 1
        a[0] += s in seen
    tails = con.execute('select count(*) from (select distinct func, target from seen where site=0)').fetchone()[0]
    rows_tail = 0
    return {'runs': runs, 'pairs': pairs, 'sites_observed': len(seen & set(sites)), 'sites_static': len(sites),
            'by_status': by, 'tail_pairs': tails, 'seen_rows': con.execute('select count(*) from seen').fetchone()[0],
            'rows_not_static': rows_tail}


def print_observed(r):
    """the q callers hook: r = cmd_callers' result (r['addr'] hex, r['indirect']). Prints the observed block, then the K-of-N line."""
    try:
        con = open_seen()
        a = int(r['addr'], 16)
        cs = callers_of(a, con)
        nr = n_runs(con)
        print('  observed indirect callers (%d runs): %d' % (nr, len(cs)) + ('' if cs else ' (none seen; not "never called")'))
        for func, site, runs in cs[:40]:
            print('    sub_%08X  %s  (%d of %d runs)' % (func, ('site 0x%08X' % site) if site else 'tail call', runs, nr))
        if len(cs) > 40:
            print('    ... %d more (q seen 0x%08X lists all)' % (len(cs) - 40, a))
        ind = r.get('indirect') or {}
        if ind.get('n_incomplete'):
            skip = set(int(s['site'], 16) for s in ind.get('sites', []) if s.get('status') in ('exact', 'narrow'))
            k, n = slot_observed(list(ind.get('offsets', {})), skip, con)
            print('  observed: %d of the %d sites at this slot have run; a site not listed has not been seen in a run, which is not "never calls"' % (k, n))
    except (SeenError, sqlite3.Error) as e:
        print('  observed indirect callers: NOT KNOWN (%s)' % e)


def parse_addr(s):
    m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', s.strip())
    return int(m.group(1), 16) if m else None


def pairs_total_rows_not_static(files):
    """rows (not distinct pairs) whose lr-4 is not a static site, over all runs"""
    sfunc = site_funcs(vdb_file())
    return sum(1 for f in files for func, lr, _ in parse(f) if not is_site_row(func, lr, sfunc))


def main(argv):
    pos = [x for x in argv if not x.startswith('--')]
    con = open_seen(db_path=None) if '--rebuild' not in argv else (build(True) or open_seen())
    names = {}
    if pos:
        a = parse_addr(pos[0])
        if a is None:
            raise SeenError('not a function address: %s' % pos[0])
        cs = callers_of(a, con)
        print('sub_%08X: observed in %d run(s)' % (a, n_runs(con)))
        print('  observed callers (%d):' % len(cs))
        for func, site, runs in cs:
            print('    sub_%08X  %s  in %d run(s)' % (func, ('site 0x%08X' % site) if site else 'tail call (no site)', runs))
        own = con.execute('select site, target, runs from seen where func=? and site<>0 order by site, target', (a,)).fetchall()
        print('  observed targets of its own indirect sites (%d pairs):' % len(own))
        for site, t, runs in own:
            print('    site 0x%08X -> sub_%08X  in %d run(s)' % (site, t, runs))
        tl = tail_targets_of(a, con)
        if tl:
            print('  observed tail-call targets from it (%d):' % len(tl))
            for t, runs in tl:
                print('    -> sub_%08X  in %d run(s)' % (t, runs))
    else:
        t = totals(con)
        files = tsv_files()
        print('runs %d, pairs (tsv rows) %d, distinct (site, func, target) rows %d' % (t['runs'], t['pairs'], t['seen_rows']))
        print('static indirect sites observed: %d of %d' % (t['sites_observed'], t['sites_static']))
        for st in sorted(t['by_status']):
            k, n = t['by_status'][st]
            print('  status %-8s %6d of %6d' % (st, k, n))
        print('rows whose lr-4 is not a static site (tail calls): %d rows, %d distinct (func, target)' % (pairs_total_rows_not_static(files), t['tail_pairs']))
    print(INCOMPLETE)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except SeenError as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)

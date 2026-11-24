r"""q feed, the OBSERVED side (S110 lane FEEDJOIN): which game functions were seen writing the bytes our renderer's read sites read.

    py -I tools\q\qfeedseen.py [--also DIR ...] [--rebuild]       totals
    py -I tools\q\q.py feed --writers FUNC                         the read sites FUNC was observed writing

Source: every out\harness\coverage\*\feed_reads.tsv of the main checkout with the stores.tsv beside it, plus each --also DIR
(a run made with --wwe2k14_feedrec=true). feed_reads.tsv: `file line addr width n_addrs_dropped`; stores.tsv: `func line addr width [lr]`
(lr = the return address at the store; absent in old recordings; a last `# lr_dropped N` line says the 16-lr cap dropped rows).
  - COPIERS (S110 COPIER, tools\q\qcopier.py): when a store row's func is a generic copier (memset / memcpy ...) that is a leaf and
    the row has an lr, the writer is the function holding lr - 4 (`via` keeps the copier); a copier row without an lr (old format),
    or for a copier that is not a leaf, stays the copier's own row, flagged `nocaller`: it does NOT count as a known writer.
  - the read file is normalised (`src/renderer_game/../scene_live/x.h` -> `src/scene_live/x.h`), the key of a site is (file, line);
  - a store covers [addr, addr+width), a read likewise; a store row whose range overlaps a read address names its function as a
    writer of that read in that run;
  - width 0 rows (a raw address that may be a LOAD) are never a writer; func 0 (the name could not be parsed) is kept apart as
    `writer not named` and never promoted to a name.
Table (out\harness\index\feedseen.s2.db, rebuilt when the set of run folders, their file mtimes / sizes or the copier set change):
  reads(file, line, addr, width, runs)         distinct (addr, width) a site read, in how many runs
  writers(file, line, func, n_addrs, runs, via, nocaller)
                                               per site and function: distinct read addresses it overlapped, in how many runs (func 0 = not named);
                                               via = ',82C79530,' the copiers it was named through; nocaller = 1: func is a copier, caller unknown
  runs(run, read_rows, store_rows, mtime, lr_dropped)
  copiers(func, reason, attrib, note)          the copier set used for this build
  breadth(func, n_files, n_sites)              raw (unattributed) writers seen over read sites in >= 6 source files: copier candidates
MAY BE INCOMPLETE: an observed writer is exact for the runs it was seen in and never a complete list. A store that overlaps a read
proves that function wrote those bytes in that run, not that it is the only writer; a field written once at load BEFORE our first
read of it is NOT seen (the watch list fills at the first read).
Rule 1: addresses, our file:line and function names only.
"""
import argparse
import bisect
import glob
import os
import posixpath
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qcopier  # noqa: E402

MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
IDX = os.path.join(MAIN, 'out', 'harness', 'index')
COV = os.path.join(MAIN, 'out', 'harness', 'coverage')
DB = os.path.join(IDX, 'feedseen.s2.db')
MASK = 0xFFFFFFFF
INCOMPLETE = ('observed = seen in the recorded runs, never a complete list or the only writer; a field written once before our first read of it '
              'is NOT seen (the watch list fills at the first read); width-0 rows and func-0 rows are never named writers.')


class FeedSeenError(Exception):
    pass


def norm_file(f):
    f = f.strip().replace('\\', '/')
    return posixpath.normpath(f)


def run_dirs(also=(), cov=None):
    """folders holding a feed_reads.tsv and a stores.tsv: the coverage folders then each --also, de-duplicated by real path"""
    seen, out = set(), []
    cands = sorted(os.path.dirname(p) for p in glob.glob(os.path.join(cov or COV, '*', 'feed_reads.tsv')))
    cands += [os.path.abspath(d) for d in also]
    for d in cands:
        rp = os.path.normcase(os.path.realpath(d))
        if rp in seen or not (os.path.exists(os.path.join(d, 'feed_reads.tsv')) and os.path.exists(os.path.join(d, 'stores.tsv'))):
            continue
        seen.add(rp)
        out.append(d)
    return out


def stamp(dirs, cop=None):
    parts = ['cop:' + cop.stamp] if cop is not None else []
    for d in dirs:
        for f in ('feed_reads.tsv', 'stores.tsv'):
            st = os.stat(os.path.join(d, f))
            parts.append('%s/%s:%d:%d' % (os.path.normcase(os.path.realpath(d)), f, int(st.st_mtime), st.st_size))
    return '|'.join(parts)


def parse_reads(path):
    out = []
    with open(path, encoding='utf-8', errors='replace') as fh:
        for i, line in enumerate(fh):
            p = line.rstrip('\n').split('\t')
            if i == 0 and p[:1] == ['file']:
                continue
            if len(p) < 4:
                continue
            try:
                out.append((norm_file(p[0]), int(p[1]), int(p[2], 16) & MASK, int(p[3])))
            except ValueError:
                continue
    return out


def parse_stores(path):
    """-> ([(addr, width, func, lr)], row count, lr_dropped): width > 0 only (a width-0 row may be a load: never a writer);
    lr is 0 for an old-format row (4 columns) and for a store that recorded none; `# lr_dropped N` is the recorder's cap count"""
    out, n, dropped = [], 0, 0
    with open(path, encoding='utf-8', errors='replace') as fh:
        for i, line in enumerate(fh):
            p = line.split()
            if i == 0 and p[:1] == ['func']:
                continue
            if p[:2] == ['#', 'lr_dropped'] and len(p) > 2:
                try:
                    dropped = int(p[2])
                except ValueError:
                    pass
                continue
            if len(p) < 4:
                continue
            try:
                w = int(p[3])
                lr = int(p[4], 16) & MASK if len(p) > 4 else 0
                n += 1
                if w > 0:
                    out.append((int(p[2], 16) & MASK, w, int(p[0], 16) & MASK, lr))
            except ValueError:
                continue
    return out, n, dropped


class Overlap:
    """stores sorted by start with the running maximum end: the stores overlapping [a, a+w)"""

    def __init__(self, stores):
        self.s = sorted(stores)
        self.starts = [x[0] for x in self.s]
        self.pm, m = [], 0
        for a, w, _f, _lr in self.s:
            m = max(m, a + w)
            self.pm.append(m)

    def funcs(self, a, w):
        """-> {(func, lr)} of the stores overlapping the read"""
        end = a + max(w, 1)
        i = bisect.bisect_left(self.starts, end) - 1
        out = set()
        while i >= 0 and self.pm[i] > a:
            sa, sw, f, lr = self.s[i]
            if sa + sw > a and sa < end:
                out.add((f, lr))
            i -= 1
        return out


def attribute(fn, lr, cop):
    """-> (writer, via, nocaller) for one overlapping store row (func fn, return address lr).
    A copier row with an lr that resolves to another, non-copier function through the index is that function's write
    (via = the copier); every other copier row stays the copier's own and is flagged nocaller; any other row is its own."""
    if not cop.is_copier(fn):
        return fn, 0, 0
    if lr and cop.attrib(fn) and cop.index is not None:
        caller = cop.index.func_of((lr - 4) & MASK)
        if caller and not cop.is_copier(caller):
            return caller, fn, 0
    return fn, 0, 1


def build(dirs, db_path=None, verbose=False, cop=None):
    t0 = time.time()
    db_path = db_path or DB
    cop = cop or qcopier.default()
    reads = {}      # (file, line, addr, width) -> runs
    writers = {}    # (file, line, func) -> [set of addrs, runs, set of via copiers, nocaller]
    raw = {}        # func -> [set of files, set of sites]: every overlapping store row's function BEFORE the copier attribution
    runs_meta = []
    for d in dirs:
        rd = parse_reads(os.path.join(d, 'feed_reads.tsv'))
        stores, nst, dropped = parse_stores(os.path.join(d, 'stores.tsv'))
        ov = Overlap(stores)
        keys = set(rd)
        site_funcs = {}
        for f, ln, a, w in keys:
            for fn, lr in ov.funcs(a, w):
                if fn:
                    r = raw.setdefault(fn, [set(), set()])
                    r[0].add(f)
                    r[1].add((f, ln))
                wr, via, noc = attribute(fn, lr, cop)
                e = site_funcs.setdefault((f, ln, wr), [set(), set(), 0])
                e[0].add(a)
                if via:
                    e[1].add(via)
                e[2] |= noc
        for k in keys:
            reads[k] = reads.get(k, 0) + 1
        for k, (addrs, vias, noc) in site_funcs.items():
            e = writers.setdefault(k, [set(), 0, set(), 0])
            e[0] |= addrs
            e[1] += 1
            e[2] |= vias
            e[3] |= noc
        runs_meta.append((os.path.basename(os.path.normpath(d)), len(rd), nst, int(os.stat(os.path.join(d, 'feed_reads.tsv')).st_mtime), dropped))
    tmp = db_path + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table reads(file text, line integer, addr integer, width integer, runs integer);'
                    'create table writers(file text, line integer, func integer, n_addrs integer, runs integer, via text, nocaller integer);'
                    'create table runs(run text, read_rows integer, store_rows integer, mtime integer, lr_dropped integer);'
                    'create table copiers(func integer, reason text, attrib integer, note text);'
                    'create table breadth(func integer, n_files integer, n_sites integer);'
                    'create table meta(key text primary key, value text);')
    w.executemany('insert into reads values (?,?,?,?,?)', [(f, l, a, wd, n) for (f, l, a, wd), n in sorted(reads.items())])
    w.executemany('insert into writers values (?,?,?,?,?,?,?)',
                  [(f, l, fn, len(e[0]), e[1], ''.join(',%08X' % v for v in sorted(e[2])) + (',' if e[2] else ''), e[3])
                   for (f, l, fn), e in sorted(writers.items())])
    w.executemany('insert into runs values (?,?,?,?,?)', runs_meta)
    w.executemany('insert into copiers values (?,?,?,?)', [(f, c['reason'], int(c['attrib']), c['note']) for f, c in sorted(cop.copiers.items())])
    w.executemany('insert into breadth values (?,?,?)',
                  [(f, len(r[0]), len(r[1])) for f, r in sorted(raw.items()) if qcopier.breadth_candidate(len(r[0]))])
    w.execute('create index reads_site on reads(file, line)')
    w.execute('create index writers_site on writers(file, line)')
    w.execute('create index writers_func on writers(func)')
    w.executemany('insert into meta values (?,?)', [('stamp', stamp(dirs, cop)), ('build_s', '%.1f' % (time.time() - t0))])
    w.commit()
    w.close()
    os.replace(tmp, db_path)
    if verbose:
        print('feedseen.s2.db: %d runs, %d read rows, %d writer rows, %.1f s' % (len(dirs), len(reads), len(writers), time.time() - t0),
              file=sys.stderr)


def open_db(also=(), cov=None, db_path=None, rebuild=False, cop=None):
    db_path = db_path or DB
    dirs = run_dirs(also, cov)
    if not dirs:
        raise FeedSeenError('no run folder with feed_reads.tsv and stores.tsv (a coverage run with --wwe2k14_feedrec=true)')
    cop = cop or qcopier.default()
    want = stamp(dirs, cop)
    ok = False
    if os.path.exists(db_path) and not rebuild:
        c = sqlite3.connect('file:%s?mode=ro' % db_path.replace('\\', '/'), uri=True)
        try:
            ok = dict(c.execute('select key,value from meta')).get('stamp') == want
        except sqlite3.Error:
            ok = False
        finally:
            c.close()
    if not ok:
        os.makedirs(os.path.dirname(db_path), exist_ok=True)
        build(dirs, db_path, True, cop)
    return sqlite3.connect('file:%s?mode=ro' % db_path.replace('\\', '/'), uri=True)


def n_runs(con):
    return con.execute('select count(*) from runs').fetchone()[0]


def sites(con):
    """-> {(file, line): (read rows, distinct addrs, max runs)} every site a run reached"""
    return {(f, l): (n, na, r) for f, l, n, na, r in
            con.execute('select file, line, count(*), count(distinct addr), max(runs) from reads group by file, line')}


def writers_of(file, line, con):
    """-> [(func, n_addrs, runs)] writers observed for the site, most addresses first: func 0, width-0 rows and a copier whose
    caller is not known (nocaller) are not here (copier_only_of has the last)"""
    return con.execute('select func, n_addrs, runs from writers where file=? and line=? and func<>0 and nocaller=0 order by n_addrs desc, func',
                       (file, line)).fetchall()


def writers_via(file, line, con):
    """-> {func: [copier addrs]} for the writers of the site that were named through a copier's lr"""
    out = {}
    for fn, via in con.execute("select func, via from writers where file=? and line=? and via<>''", (file, line)):
        out[fn] = [int(x, 16) for x in via.strip(',').split(',')]
    return out


def copier_only_of(file, line, con):
    """-> [(func, n_addrs, runs)] copiers seen over the site whose caller is not known (old recording, or a non-leaf copier)"""
    return con.execute('select func, n_addrs, runs from writers where file=? and line=? and func<>0 and nocaller=1 order by n_addrs desc, func',
                       (file, line)).fetchall()


def copier_info(func, con):
    """-> (reason, attrib, note) when func is in this build's copier set, else None"""
    r = con.execute('select reason, attrib, note from copiers where func=?', (func,)).fetchone()
    return r


def callers_of_copier(func, con):
    """-> [(caller, file, line, n_addrs, runs)] the sites a copier was observed writing FOR a caller (lr recorded)"""
    return con.execute("select func, file, line, n_addrs, runs from writers where via like ? order by func, file, line",
                       ('%%,%08X,%%' % func,)).fetchall()


def breadth_rows(con):
    """-> [(func, n_files, n_sites)] raw writers seen over read sites in >= 6 files (copier candidates)"""
    return con.execute('select func, n_files, n_sites from breadth order by n_files desc, func').fetchall()


def unnamed_of(file, line, con):
    """-> (n_addrs, runs) of store rows that overlapped the site with no function name (func 0), or (0, 0)"""
    r = con.execute('select n_addrs, runs from writers where file=? and line=? and func=0', (file, line)).fetchone()
    return r or (0, 0)


def sites_of_func(func, con):
    """-> [(file, line, n_addrs, runs)] the read sites a function was observed writing"""
    return con.execute('select file, line, n_addrs, runs from writers where func=? order by file, line', (func,)).fetchall()


def named_sites(con):
    """sites with at least one observed writer that is not a copier without a caller"""
    return {(f, l) for f, l in con.execute('select distinct file, line from writers where func<>0 and nocaller=0')}


def copier_only_sites(con):
    """sites whose only named observed writers are copiers without a caller (a func-0 row may also be there)"""
    named = named_sites(con)
    return {(f, l) for f, l in con.execute('select distinct file, line from writers where func<>0 and nocaller=1')} - named


def any_overlap_sites(con):
    return {(f, l) for f, l in con.execute('select distinct file, line from writers')}


def summary(con):
    s = sites(con)
    return {'runs': n_runs(con), 'sites': len(s), 'read_rows': sum(v[0] for v in s.values()),
            'sites_named': len(named_sites(con)), 'sites_copier_only': len(copier_only_sites(con)),
            'sites_any_overlap': len(any_overlap_sites(con))}


def row_suffix(file, line, con, fname, top=3):
    """the text appended to a `q feed FILE:LINE` row"""
    nr = n_runs(con)
    s = con.execute('select count(*), max(runs) from reads where file=? and line=?', (file, line)).fetchone()
    if not s[0]:
        return 'seen: site not reached in %d runs' % nr
    ws = writers_of(file, line, con)
    un = unnamed_of(file, line, con)
    via = writers_via(file, line, con)
    t = 'seen writers: %d funcs (site reached in up to %d of %d runs)' % (len(ws), s[1], nr)
    if ws:
        t += ': ' + '; '.join('%s x%d addr, %d runs%s' % (fname(f), na, r, (' via ' + '/'.join(fname(v) for v in via[f])) if f in via else '')
                              for f, na, r in ws[:top]) + (' ...' if len(ws) > top else '')
    co = copier_only_of(file, line, con)
    if co:
        t += ' [+ copier, caller not recorded: ' + '; '.join('%s x%d addr' % (fname(f), na) for f, na, _r in co[:top]) + ']'
    if un[0]:
        t += ' [+ %d addr with a store row, writer not named]' % un[0]
    return t


def seen_summary(rows, con, drawn, out=print):
    """per family: sites in the table / reached in runs / static writer / observed / either / unknown (all of the reached); one overall line"""
    sites_t = {}
    for r in rows:
        if r['kind'] == 'reg':
            continue
        e = sites_t.setdefault((r['file'], r['line']), [r['family'], False])
        e[1] = e[1] or r.get('result') in ('known', 'image')
    reached = sites(con)
    named = named_sites(con)
    co_only = copier_only_sites(con)
    any_ov = any_overlap_sites(con)
    fams = [f for f in drawn if any(v[0] == f for v in sites_t.values())] + sorted({v[0] for v in sites_t.values()} - set(drawn))
    out('')
    out('OBSERVED writers (%d runs; read sites a run reached, of the table sites): sites / reached / static writer / observed / either / '
        'only a copier seen / unknown' % n_runs(con))
    tot = [0] * 7
    for f in fams + ['ALL']:
        ks = [k for k, v in sites_t.items() if f == 'ALL' or v[0] == f]
        rc = [k for k in ks if k in reached]
        st = sum(1 for k in rc if sites_t[k][1])
        ob = sum(1 for k in rc if k in named)
        ei = sum(1 for k in rc if sites_t[k][1] or k in named)
        co = sum(1 for k in rc if k in co_only and not (sites_t[k][1] or k in named))
        row = [len(ks), len(rc), st, ob, ei, co, len(rc) - ei - co]
        if f == 'ALL':
            tot = row
        out('  %-10s %4d / %4d / %4d / %4d / %4d / %4d / %4d' % ((f,) + tuple(row)))
    out('bar: of %d read sites a run reached, %d (%.1f%%) have a known writer, static or observed; %d only a generic copier (memset / memcpy, '
        'caller not recorded) was seen over; %d still unknown.'
        % (tot[1], tot[4], 100.0 * tot[4] / max(1, tot[1]), tot[5], tot[6]))
    extra = sorted(k for k in reached if k not in sites_t)
    out('reached, not in the table (the table script could not see them): %d sites, %d with a named writer, %d with any overlapping store row'
        % (len(extra), sum(1 for k in extra if k in named), sum(1 for k in extra if k in any_ov)))
    for k in extra[:25]:
        out('    %s:%d  %d rows' % (k[0], k[1], reached[k][0]))
    if len(extra) > 25:
        out('    ... %d more' % (len(extra) - 25))
    out('static = our table says a writer is known (K/G/T/named or image data); observed = a named game function other than a copier-without-caller '
        'stored over the bytes in a run (a copier row with a recorded lr counts as its caller); only a copier seen = not static, no other observed '
        'writer (%d of the reached sites have func-0 store rows only or a copier only: writer not named).'
        % sum(1 for k in reached if k in any_ov and k not in named))
    out(INCOMPLETE)


def print_writers(func, con, fname, out=print):
    """q feed --writers FUNC: the sites FUNC was observed writing; for a copier, per caller"""
    ss = sites_of_func(func, con)
    ci = copier_info(func, con)
    cs = callers_of_copier(func, con) if ci else []
    if not ci:
        out('%s observed writing %d read sites (%d runs recorded)' % (fname(func), len(ss), n_runs(con)))
        for fl, ln, na, nr in ss:
            out('  %s:%d  %d addr, in %d runs' % (fl, ln, na, nr))
        out(INCOMPLETE)
        return
    out('%s is a generic copier (%s): its stores are named after the CALLER (the function holding lr) when lr was recorded; (%d runs recorded)'
        % (fname(func), ci[0], n_runs(con)))
    by = {}
    for c, fl, ln, na, nr in cs:
        by.setdefault(c, []).append((fl, ln, na, nr))
    out('  written for %d callers (%d sites):' % (len(by), sum(len(v) for v in by.values())))
    for c, v in sorted(by.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        out('  %s: %d sites' % (fname(c), len(v)))
        for fl, ln, na, nr in v:
            out('      %s:%d  %d addr, in %d runs' % (fl, ln, na, nr))
    if ss:
        why = 'caller not recorded (old recording)' if ci[1] else ci[2]
        out('  the copier itself, %s: %d sites' % (why, len(ss)))
        for fl, ln, na, nr in ss:
            out('      %s:%d  %d addr, in %d runs  [copier, caller not recorded]' % (fl, ln, na, nr))
    elif not cs:
        out('  no site seen')
    if not ci[1]:
        out('  lr is NOT used for this copier: %s' % ci[2])
    out(INCOMPLETE)


def main(argv):
    ap = argparse.ArgumentParser(prog='qfeedseen')
    ap.add_argument('--also', action='append', default=[])
    ap.add_argument('--rebuild', action='store_true')
    a = ap.parse_args(argv)
    con = open_db(a.also, rebuild=a.rebuild)
    s = summary(con)
    print('runs %d; read sites reached %d (%d distinct read rows); with a named writer %d (+ %d with only a copier, caller not recorded); '
          'with any overlapping store row (named or not) %d'
          % (s['runs'], s['sites'], s['read_rows'], s['sites_named'], s['sites_copier_only'], s['sites_any_overlap']))
    print(INCOMPLETE)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except FeedSeenError as e:
        print('qfeedseen: %s' % e, file=sys.stderr)
        sys.exit(2)

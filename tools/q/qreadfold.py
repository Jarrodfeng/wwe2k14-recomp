r"""qreadfold (S111 lane FOLD): the three one-time folds of the function-reading store (read.s2.db), each a function that works on an open
connection, so a selftest runs it on a temp db and the real store is changed once (after a copy in out\backup).

    py -I tools/q/qreadfold.py DB [--apply] --sheets out\s110 --out-json db/reader_names.json --orphans TSV --replaced TSV
                                  [--restore-confidence BACKUP_DB]

  fold_verdicts(c)             the separate table verify_read goes: each verdict is written onto the reading row it was pinned to
                               (read_id), the table is dropped; verdicts pinned to no reading are returned (nothing to fold them onto),
                               and so are the earlier verdicts a later one on the same reading replaced (both go to history files).
  fold_names(c, dbdir, indep)  reader-proposed names (name_read): a name is PROMOTED to db/*.json only when two independent proposals of the
                               function agree; every other proposal row is deleted, except a row a probe judged (it carries that verdict).
                               With --sheets the test is independent_by_sheet (S111 FOLD2): the LATER reading's own sheet file did not show
                               the earlier name; without it, independent() (same batch). Never invents a name; a function the db already
                               names is listed as a conflict, never overwritten; the db `system` is db_system() of the reading's.
  fold_confidence(c, nb_rows)  the confidence a reading took from its neighbour lines (the `# NEIGHBOURS` block of the listing sheets, removed
                               from qlsheet in this lane) goes back to the value of the unit's last reading made WITHOUT neighbour lines
                               whenever it was raised above it. nb_rows (rows_with_neighbours) = the readings whose OWN sheet carried
                               neighbour lines AND whose own text (purpose + because) cites one of them (S111 FOLD2: a reading that cites
                               only its listing keeps the confidence the listing earned; S110's first listing pass, L01, had no lines at all).
  restore_confidence(c, bk)    the correction of a fold applied with the earlier per-unit rule: every listing reading takes its confidence back
                               from the untouched backup, so fold_confidence can be run again on what the readers wrote.
Without --apply the changes are rolled back and only the counts are printed.
"""
import collections
import glob
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402

ORD = {'low': 0, 'medium': 1, 'high': 2}
NAME_OK = re.compile(r'[a-z][a-z0-9_]*')
LISTING_MODEL = 'opus-low-listing%'          # the models of the listing passes: the only readings whose sheet COULD carry a # NEIGHBOURS block
SOURCE = 'reader names S111 fold: two independent readings of one unit agree'
DB_SYSTEMS = ('camera', 'render', 'match', 'assets', 'audio', 'io', 'ui', 'other')      # tools/db/validate.py SYSTEMS
READ_TO_DB = {'menus': 'ui', 'creation': 'ui', 'model': 'render', 'material': 'render', 'light': 'render', 'particle': 'render',
              'screens': 'render', 'crowd': 'render', 'liquids': 'render', 'arena': 'render'}
NEIGHBOUR_WORD = re.compile(r'neighbou?r', re.I)
UNIT_ID = re.compile(r'\bunit (\d+)')


# ------------------------------------------------------------------ 3. verdicts
def fold_verdicts(c):
    """-> (folded, replaced, orphans). orphans: [(kind, target, verdict, note, source, when_)] pinned to no reading (the table is dropped with them);
    replaced: the same tuples for an earlier verdict a later one on the same reading replaced (only the newest stays on the reading)"""
    if not c.execute("select 1 from sqlite_master where type='table' and name='verify_read'").fetchone():
        return 0, [], []
    qread.add_verdict_cols(c)
    folded = 0
    orphans, replaced = [], []
    for kind, target, verdict, note, source, when_, rid in c.execute(
            'select target_kind,target,verdict,note,source,when_,read_id from verify_read order by id').fetchall():
        tab = 'unit_read' if kind == 'unit' else 'name_read'
        if rid is None or not c.execute('select 1 from %s where id=?' % tab, (rid,)).fetchone():
            orphans.append((kind, target, verdict, note, source, when_))
            continue
        was = c.execute('select verdict,v_note,v_source,v_when from %s where id=?' % tab, (rid,)).fetchone()
        if was[0] is not None:
            replaced.append((kind, target) + tuple(was))
        c.execute('update %s set verdict=?, v_note=?, v_source=?, v_when=? where id=?' % tab, (verdict, note, source, when_, rid))
        folded += 1
    c.execute('drop index if exists verify_read_t')
    c.execute('drop table verify_read')
    return folded, replaced, orphans


# ------------------------------------------------------------------ 1. names
def independent(a, b):
    """Two stored proposals (rows of name_read: id,func,name,evidence,confidence,unit_id,batch,model) are two independent labels when they come
    from two readings made from the same sheet (same unit, same batch) and differ in what they wrote. Why so narrow: `q sheet` shows every
    stored name of a function on the sheet of a later reading (`read NAME [r:..]`, qsheet.py), so a later reading of an already read unit was
    shown the earlier name and agrees with it by being shown it; two rows of one batch with the SAME text are one answer stored twice."""
    return a[5] == b[5] and a[6] == b[6] and a[0] != b[0] and (a[3], a[4]) != (b[3], b[4])


def independent_by_sheet(sheet_of):
    """-> independent(a, b) decided from the LATER reading's own sheet file (S111 FOLD2): a (earlier) and b (later) are two labels when
    they differ in what they wrote and b's sheet exists and does not carry a's name (so b's reader was not shown it). A reading whose sheet
    is not on disk counts as shown (nothing promotes without the file)."""
    cache = {}

    def text(batch, unit):
        k = (batch, unit)
        if k not in cache:
            p = sheet_of(batch, unit)
            cache[k] = open(p, encoding='utf-8', errors='replace').read() if p else None
        return cache[k]

    def indep(a, b):
        a, b = (a, b) if a[0] < b[0] else (b, a)
        if a[0] == b[0] or (a[3], a[4]) == (b[3], b[4]):
            return False
        t = text(b[6], b[5])
        return t is not None and a[2] not in t
    indep.by_sheet = True
    return indep


def db_system(read_system):
    """a reading's system word -> the db `system` validate.py accepts (the reader's list is wider: menus, creation, pose, engine, ...)"""
    s = (read_system or '').strip()
    return s if s in DB_SYSTEMS else READ_TO_DB.get(s, 'other')


def db_index(dbdir):
    """({addr: [(file, name)]}, {name: {addr}}) over every db/*.json"""
    by_addr, by_name = collections.defaultdict(list), collections.defaultdict(set)
    for p in sorted(glob.glob(os.path.join(dbdir, '*.json'))):
        try:
            ents = json.load(open(p, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            if e.get('name'):
                by_addr[a].append((os.path.basename(p), e['name']))
                by_name[e['name']].add(a)
    return by_addr, by_name


def fold_names(c, dbdir, indep=independent):
    """-> dict(promoted [db entries], dropped, kept (rows kept because a probe judged them), conflicts [(addr, name, why)], low [(addr, name)],
    single (functions with one label), claims (claim_read rows deleted with their proposals)). Changes c: deletes the rows; the caller writes the entries.
    indep: the independence test of two agreeing proposals (independent: same batch; independent_by_sheet: the later reading's own sheet)."""
    by_addr, by_name = db_index(dbdir)
    rows = c.execute('select id,func,name,evidence,confidence,unit_id,batch,model,verdict from name_read order by id').fetchall()
    sysof = {(u, b): s for u, b, s in c.execute("select unit_id,batch,system from unit_read where system<>'' order by id")}
    byf = collections.defaultdict(list)
    for r in rows:
        byf[r[1]].append(r)
    promoted, conflicts, low = [], [], []
    for f, rs in sorted(byf.items()):
        pair = None
        for i, a in enumerate(rs):
            for b in rs[i + 1:]:
                if a[2] == b[2] and indep(a, b):
                    pair = (a, b)
                    break
            if pair:
                break
        if not pair:
            continue
        a, b = pair
        name = a[2]
        if min(ORD[a[4]], ORD[b[4]]) < ORD['medium']:
            low.append((f, name))
            continue
        if not NAME_OK.fullmatch(name):
            conflicts.append((f, name, 'not a snake_case name'))
            continue
        if f in by_addr:
            conflicts.append((f, name, 'the db already names it %s (%s)' % (by_addr[f][0][1], by_addr[f][0][0])))
            continue
        if by_name.get(name):
            conflicts.append((f, name, 'the name is already used in the db for %08X' % sorted(by_name[name])[0]))
            continue
        cf = 'medium' if min(ORD[a[4]], ORD[b[4]]) == ORD['medium'] else 'high'
        if getattr(indep, 'by_sheet', False):
            why = ('two readings of unit %d (batches %s and %s) named it so with different evidence; the later one was made from a sheet that '
                   'did not show the earlier name' % (a[5], a[6], b[6]))
        else:
            why = 'two readings of the same unit (%d) made from one sheet named it so with different evidence' % a[5]
        promoted.append({
            'address': '0x%08X' % f, 'name': name,
            'summary': 'Reader-proposed name; %s. A reading proposes, it is not the game\'s own name.' % why,
            'evidence': ['reading %s: %s' % (a[6], a[3]), 'reading %s: %s' % (b[6], b[3]), 'labels: A %s; B %s (agree)' % (a[2], b[2])],
            'confidence': cf, 'system': db_system(sysof.get((a[5], a[6]))), 'source': SOURCE})
    keep = {r[0] for r in rows if r[8] is not None}      # a probe judged it: its verdict lives on this row
    gone = [r[0] for r in rows if r[0] not in keep]
    single = sum(1 for rs in byf.values() if len(rs) == 1)
    for i in range(0, len(gone), 500):
        part = gone[i:i + 500]
        c.execute('delete from name_read where id in (%s)' % ','.join('?' * len(part)), part)
    claims = 0
    if c.execute("select 1 from sqlite_master where type='table' and name='claim_read'").fetchone():
        claims = c.execute("delete from claim_read where read_kind='name' and read_id not in (select id from name_read)").rowcount
    return {'promoted': promoted, 'dropped': len(gone), 'kept': len(keep), 'conflicts': conflicts, 'low': low, 'single': single,
            'claims': claims, 'funcs': len(byf), 'rows': len(rows), 'db_funcs': sum(1 for f in byf if f in by_addr)}


def write_entries(path, entries):
    """db/reader_names.json: existing entries are kept (an address already in the file is never rewritten)"""
    cur = []
    if os.path.exists(path):
        cur = json.load(open(path, encoding='utf-8'))
    have = {e['address'] for e in cur}
    cur += [e for e in entries if e['address'] not in have]
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        json.dump(cur, fh, indent=2)
        fh.write('\n')


# ------------------------------------------------------------------ 2. confidence
def confidence_counts(c):
    """newest answer per unit: {confidence: n}, and over every stored answer"""
    newest = {}
    allr = collections.Counter()
    for u, cf in c.execute('select unit_id,confidence from unit_read order by id'):
        newest[u] = cf
        allr[cf] += 1
    return collections.Counter(newest.values()), allr


def s110_sheet(s110):
    """-> sheet_of(batch, unit): the sheet file a listing-pass reading of S110 was made from (out\\s110: L01 = listing1, L02 = listing2,
    Pnn = p2\\Pnn.lst, s110_l3_rN = l3\\rN.lst), or None"""
    lists = {}

    def from_list(lst, unit):
        if lst not in lists:
            lists[lst] = {}
            if os.path.exists(lst):
                for line in open(lst, encoding='utf-8'):
                    if line.strip():
                        lists[lst][os.path.basename(line.strip())] = line.strip()
        return lists[lst].get('%d.txt' % unit)

    def sheet_of(batch, unit):
        b = batch or ''
        if b in ('L01', 'L02'):
            p = os.path.join(s110, 'listing1' if b == 'L01' else 'listing2', '%d.txt' % unit)
        elif re.fullmatch(r'P\d+', b):
            p = from_list(os.path.join(s110, 'p2', b + '.lst'), unit)
        elif b.startswith('s110_l3_'):
            p = from_list(os.path.join(s110, 'l3', b[len('s110_l3_'):] + '.lst'), unit)
        elif re.fullmatch(r'feed[AB]\d+', b):                         # the first pass's feed batches
            p = os.path.join(s110, b[:5], '%d.txt' % unit)
        elif re.fullmatch(r'[ABCD]\d\d', b):                          # the first pass's whole-game batches (allA .. allD)
            p = os.path.join(s110, 'all' + b[0], '%d.txt' % unit)
        elif b == 'reread1':
            p = os.path.join(s110, 'reread1', '%d.txt' % unit)
        else:
            p = None
        return p if p and os.path.exists(p) else None
    return sheet_of


def neighbour_units(text):
    """the unit ids of a sheet's `# NEIGHBOURS` lines ([]: no such part)"""
    if '# NEIGHBOURS' not in text:
        return []
    part = text.split('# NEIGHBOURS', 1)[1].split('\n# LISTINGS', 1)[0]
    return [int(m.group(1)) for m in (re.match(r'(?:called by|calls) unit (\d+)\b', x) for x in part.split('\n')) if m]


def cites_neighbour(text, units):
    """the reading's own text (purpose + because) cites a neighbour of its sheet: the word neighbour, or `unit N` of a listed neighbour"""
    return bool(NEIGHBOUR_WORD.search(text or '')) or any(int(m) in units for m in UNIT_ID.findall(text or ''))


def rows_with_neighbours(c, sheet_of):
    """-> ({readings whose own sheet carried neighbour lines AND whose own text cites one of them}, {readings whose sheet carried them},
    [(id, batch, unit)] listing readings whose sheet was not found). S111 FOLD2: a reading that cites only its listing keeps the confidence
    its listing earned; only one that leans on a neighbour in its own words is lowered."""
    rows, shown, missing = set(), set(), []
    for rid, u, b, pu, bc in c.execute('select id,unit_id,batch,purpose,because from unit_read where model like ? order by id', (LISTING_MODEL,)).fetchall():
        p = sheet_of(b, u)
        if p is None:
            missing.append((rid, b, u))
            continue
        units = set(neighbour_units(open(p, encoding='utf-8', errors='replace').read()))
        if units:
            shown.add(rid)
            if cites_neighbour('%s %s' % (pu or '', bc or ''), units):
                rows.add(rid)
    return rows, shown, missing


def restore_confidence(c, backup):
    """every listing-pass reading takes its confidence back from the backup store (opened read-only) -> rows changed"""
    bk = sqlite3.connect('file:%s?mode=ro' % backup.replace('\\', '/'), uri=True)
    try:
        was = dict(bk.execute('select id,confidence from unit_read where model like ?', (LISTING_MODEL,)))
    finally:
        bk.close()
    n = 0
    for rid, cf in c.execute('select id,confidence from unit_read where model like ?', (LISTING_MODEL,)).fetchall():
        if rid in was and was[rid] != cf:
            c.execute('update unit_read set confidence=? where id=?', (was[rid], rid))
            n += 1
    return n


def fold_confidence(c, nb_rows):
    """-> (changed rows [(id, unit, was, now)], readings with neighbour lines examined). nb_rows: the readings whose own sheet carried
    neighbour lines. Such a reading is set to the confidence of the unit's last earlier reading NOT in nb_rows (the last one made without
    neighbour lines, a listing pass without them included) when it is higher; a unit with no such reading is left alone; a lowered or equal
    confidence is never touched."""
    nb = set(nb_rows)
    base = {}
    changed = []
    seen = 0
    for rid, u, cf in c.execute('select id,unit_id,confidence from unit_read order by id').fetchall():
        if rid not in nb:
            base[u] = cf
            continue
        if u not in base:
            continue
        seen += 1
        if ORD[cf] > ORD[base[u]]:
            changed.append((rid, u, cf, base[u]))
    for rid, u, was, now in changed:
        c.execute('update unit_read set confidence=? where id=?', (now, rid))
    return changed, seen


# ------------------------------------------------------------------ run
def main(argv):
    if not argv:
        print(__doc__)
        return 2

    def opt(flag):
        return argv[argv.index(flag) + 1] if flag in argv and argv.index(flag) + 1 < len(argv) else None
    path = argv[0]
    root = os.path.dirname(os.path.dirname(HERE))
    dbdir = opt('--db-dir') or os.path.join(root, 'db')
    c = qread.open_rw(path)
    b_new, b_all = confidence_counts(c)
    if opt('--restore-confidence'):
        print('restore: %d listing readings took their confidence back from %s' % (restore_confidence(c, opt('--restore-confidence')), opt('--restore-confidence')))
    nb, shown, missing = rows_with_neighbours(c, s110_sheet(opt('--sheets'))) if opt('--sheets') else (set(), set(), [])
    if opt('--sheets'):
        print('sheets: %d listing readings made from a sheet with neighbour lines, %d of them cite a neighbour in their own text; %d listing '
              'readings whose sheet was not found%s' % (len(shown), len(nb), len(missing), (' (first: %s)' % (missing[:5],)) if missing else ''))
    folded, replaced, orphans = fold_verdicts(c)
    print('verdicts: %d folded onto their reading (%d replaced an earlier one on the same reading), %d pinned to no reading' % (folded, len(replaced), len(orphans)))
    for flag, rows, head in (('--orphans', orphans, 'probe verdicts that judged no stored reading (S111 FOLD: the verify_read table is gone; these had read_id NULL)'),
                             ('--replaced', replaced, 'probe verdicts a LATER verdict on the same reading replaced in the fold (S111 FOLD: one verdict per reading; '
                                                      'the reading now carries the newest)')):
        if opt(flag) and rows:
            with open(opt(flag), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write('# %s. kind, target (unit id / func address), verdict, source, when, note\n' % head)
                for k, t, v, n, s, w in rows:
                    fh.write('%s\t%s\t%s\t%s\t%s\t%s\n' % (k, t if k == 'unit' else '%08X' % t, v, s, w, n.replace('\t', ' ').replace('\n', ' ')))
    r = fold_names(c, dbdir, independent_by_sheet(s110_sheet(opt('--sheets'))) if opt('--sheets') else independent)
    print('names: %d proposals on %d functions: promoted %d, dropped %d, kept %d (a probe judged them), conflicts %d, two agreeing but a low one %d; '
          'functions with one label %d; %d functions already in the db; claim rows deleted %d' %
          (r['rows'], r['funcs'], len(r['promoted']), r['dropped'], r['kept'], len(r['conflicts']), len(r['low']), r['single'], r['db_funcs'], r['claims']))
    for f, n, why in r['conflicts']:
        print('  conflict %08X %s: %s' % (f, n, why))
    changed, seen = fold_confidence(c, nb)
    a_new, a_all = confidence_counts(c)
    print('confidence: %d listing readings with neighbour lines examined, %d lowered to their own value' % (seen, len(changed)))
    for what, b, a in (('newest answer per unit', b_new, a_new), ('every stored answer    ', b_all, a_all)):
        print('  %s  before high %d medium %d low %d   after high %d medium %d low %d' %
              ((what,) + tuple(b.get(k, 0) for k in ('high', 'medium', 'low')) + tuple(a.get(k, 0) for k in ('high', 'medium', 'low'))))
    if '--apply' in argv:
        c.commit()
        if r['promoted'] and opt('--out-json'):
            write_entries(opt('--out-json'), r['promoted'])
        c.execute('vacuum')
        print('applied')
    else:
        c.rollback()
        print('dry run: rolled back')
    c.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

r"""q runlog: an index of OUR run logs (S103 lane QX4): which exe, which route, which flags, what the census lines said, which pacs the run opened.

    py tools/q/q.py runlog WORDS... [--exe X] [--route R] [--limit N] [--json]
        every indexed line holding ALL the words (case-insensitive, word prefixes; `a_b` = a phrase is not supported: quote nothing, just list the words). A hit is
        `run NNNN  exe  route  start  :line  text`. The lines indexed per run: the run header (exe dir + build time, route script, every `modified:` command-line
        flag, version) and the census lines, FIRST occurrence of each shape (digits normalised) with how many times it repeated:
        [scene][ready] (holes / reasons), [stage_show], draw_asked, [rdr][weapons], event-prop lines, [wwe2k14][census], [rdr][census],
        [scene][live] / [skips] / [maps] / [screens] / [frame], [rdr][crowd], [rdr][verdict].
        --exe X / --route R keep the runs whose exe path / route script path contains X / R. With no WORDS the matching runs are listed.
    py tools/q/q.py runlog which --mode X [--exe X] [--route R] [--limit N]
        which runs and which routes reached a scene id / pac / arena whose name contains X (ENTRANCE00, BG04, ARENA05): the pacs a run opened (NtCreateFile
        lines `D:\PAC\...`, with the first guest frame) and the census lines that name X; grouped by route script, newest runs first.
    py tools/q/q.py runlog field FIELD [--run RUN]
        (QCF) FIELD's value on every window of one run log: --run RUN = the log file number (7317, 027) (only via --run: a number among the words is part of the field), a log path, or none = the newest log. A number keeps its
        parenthesis / `of N`, a [bracket list] is kept whole, a field that is part of the line's [tag] takes the rest of the line; identical neighbours collapse.
    py tools/q/q.py runlog info | update [--full]
        what is indexed (and how many logs are gone from disk but still known); `update` forces the scan now (every other call does it itself).

Logs read: logs\wwe2k14_NNN.log, out\build\*\logs\wwe2k14_NNN.log, out\release\build\logs (the folder next to the exe it ran). The exe is the wwe2k14.exe in the log
folder's parent (path + its modified time = the build time; `gone` when it is not there any more). The index (out\harness\index\runlog.s<VERSION>.db; a new version starts from the older file's rows of vanished logs, read only) is incremental: a stat
walk (size / mtime) then only the new / changed logs are parsed (<= 4 workers, below-normal priority); a log that is deleted afterwards (the tidy clears out\ at
session end) KEEPS its rows, marked `log gone`. Game-derived text: stays in out\ (gitignored). No game run is needed or started.
"""
import argparse
import glob
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
IDX = os.path.join(REPO, 'out', 'harness', 'index')
VERSION = 7      # S105 D (F1): also in the FILE NAME (runlog.s7.db, db_name()): a q of another version never opens, wipes or rebuilds this file


def db_name():
    """the index file's name carries the version (runlog.s7.db): a q of another version uses ANOTHER file and never opens, wipes or rebuilds this one (the design of qasset.db_name)"""
    return 'runlog.s%d.db' % VERSION


def rdb_path():
    r"""out\harness\index\runlog.s<VERSION>.db; WWE_QRUNLOG_DB=<file> = another file (how a builder / a test works without touching the shared index)"""
    return os.environ.get('WWE_QRUNLOG_DB') or os.path.join(IDX, db_name())


def other_index_files(path=None):
    """index files of OTHER names beside ours (the old runlog.db, runlog.s6.db ...): never written by this q, named so the tidy / a person can clear them"""
    path = path or rdb_path()
    me = os.path.basename(path)
    try:
        return sorted(f for f in os.listdir(os.path.dirname(path)) if re.match(r'^runlog(\.s\d+)?\.db$', f) and f != me)
    except OSError:
        return []


def _file_version(fp):
    try:
        c = sqlite3.connect(f'file:{fp}?mode=ro', uri=True)
        try:
            r = c.execute("SELECT value FROM meta WHERE key='version'").fetchone()
            return int(r[0]) if r else None
        finally:
            c.close()
    except (sqlite3.Error, ValueError):
        return None


def _carry_over(path):
    r"""a NEW index file starts from the newest index file of an older (or the same) version beside it, read-only (sqlite backup of a mode=ro connection): the rows of logs that are GONE
    from disk (the tidy clears out\) exist nowhere else, a re-parse cannot bring them back. The other file is never written; update() then migrates the copy as before."""
    d = os.path.dirname(path)
    best = None
    for f in other_index_files(path):
        fp = os.path.join(d, f)
        v = _file_version(fp)
        if v is None or v > VERSION:
            continue
        mt = os.path.getmtime(fp)
        if best is None or mt > best[0]:
            best = (mt, fp, v)
    if best is None:
        return
    tmp = f'{path}.{os.getpid()}.tmp'
    try:
        src = sqlite3.connect(f'file:{best[1]}?mode=ro', uri=True)
        dst = sqlite3.connect(tmp)
        try:
            src.backup(dst)
        finally:
            dst.close()
            src.close()
        if os.path.exists(path):
            os.remove(tmp)      # another q made the new file meanwhile: keep theirs
        else:
            os.replace(tmp, path)
            print(f'q runlog: new index {os.path.basename(path)}: started from {os.path.basename(best[1])} (version {best[2]}, read only; it is left alone): the rows of logs gone from disk are kept', file=sys.stderr)
    except (sqlite3.Error, OSError) as e:
        if os.path.exists(path):      # lost the race to another first caller: theirs stands, nothing to report
            try:
                os.remove(tmp)
            except OSError:
                pass
            return
        print(f'q runlog: could not start from {best[1]} ({e}); the new index is built from the logs still on disk', file=sys.stderr)
        try:
            os.remove(tmp)
        except OSError:
            pass
MAX_PER_TAG = 500

PREFIX_RE = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+\] \[\w+\] \[[\w ]+\] (?:\[t\d+\] )?')
RUNNUM_RE = re.compile(r'run log number: (\d+) \(pid (\d+)\)')
VERSION_RE = re.compile(r'\[wwe2k14\] version (\S+)')
MOD_RE = re.compile(r"\[wwe2k14\] modified: (\w+) = '(.*)' \(source: ([^)]*)\)")
EXE_RE = re.compile(r'\[wwe2k14\] exe built \(file time, local\): (\d{4}-\d\d-\d\d \d\d:\d\d)')
PAC_RE = re.compile(r"\[wwe2k14\]\[file\] NtCreateFile '([^']+)' open .*?frame=(\d+)")
CENSUS = (('stage_show', re.compile(r'stage_show', re.I)), ('draw_asked', re.compile(r'draw_asked')),
          ('weapons', re.compile(r'\[rdr\]\[weapons\]')), ('event_prop', re.compile(r'event[ _]prop', re.I)), ('scene_ready', re.compile(r'\[scene\]\[ready\]')),
          ('census', re.compile(r'\[wwe2k14\]\[census\]')), ('rdr_census', re.compile(r'\[rdr\]\[census\]')),
          ('scene', re.compile(r'\[scene\]\[(?:live|skips|maps|screens|frame|inputs)\]')), ('crowd', re.compile(r'\[rdr\]\[crowd\]')),
          ('verdict', re.compile(r'\[rdr\]\[verdict\]')))
NORM_RE = re.compile(r'(?<![\w.])\d+(?![\w.])')      # a STANDALONE decimal only: 0x82C30A10, sub_82ABCDEF, f5403, 1.5 keep their digits (lines that differ there are different shapes)


class RunlogError(Exception):
    pass


def find_logs():
    pats = [os.path.join(REPO, 'logs', 'wwe2k14_*.log'), os.path.join(REPO, 'out', 'build', '*', 'logs', 'wwe2k14_*.log'),
            os.path.join(REPO, 'out', 'release', 'build', 'logs', 'wwe2k14_*.log'),
            os.path.join(REPO, 'out', 'exe', '*', 'logs', 'wwe2k14_*.log')]   # S106 G2L: lane_exe exes (log numbers repeat per folder: `q runlog field FOLDER:NNN`)
    out = []
    for p in pats:
        out.extend(glob.glob(p))
    return sorted(set(out))


def _low():
    try:
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)
    except Exception:
        pass


def parse_log(path):
    """-> dict: header fields, census rows [(tag, first lineno, count, first text, last lineno, last text)], pacs {name: (first_frame, n)}, capped tags"""
    start = None
    runnum = None
    version = ''
    exe_log = ''
    flags = []           # (key, value, source)
    census = {}          # (tag, hash of the digit-normalised line) -> [first_n, count, first_text, last_n, last_text]
    per_tag = {}
    capped = {}
    pacs = {}
    n = 0
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for n, line in enumerate(f, 1):
                if start is None:
                    pm = PREFIX_RE.match(line)
                    if pm:
                        start = pm.group(1)
                if runnum is None and 'run log number' in line:
                    m = RUNNUM_RE.search(line)
                    if m:
                        runnum = int(m.group(1))
                if not version and '] version ' in line:
                    m = VERSION_RE.search(line)
                    if m:
                        version = m.group(1)
                if not exe_log and 'exe built (file time' in line:
                    m = EXE_RE.search(line)
                    if m:
                        exe_log = m.group(1)
                if 'modified:' in line and '(source:' in line:
                    m = MOD_RE.search(line)
                    if m:
                        flags.append((m.group(1), m.group(2), m.group(3).strip().replace(' ', '-')))
                    continue
                if 'NtCreateFile' in line and '.PAC' in line.upper():
                    m = PAC_RE.search(line)
                    if m:
                        nm = m.group(1).replace('\\', '/').split('/')
                        nm = '/'.join(nm[1:]) if len(nm) > 1 and ':' in nm[0] else '/'.join(nm)
                        nm = nm.upper()
                        pf = pacs.get(nm)
                        pacs[nm] = (pf[0], pf[1] + 1) if pf else (int(m.group(2)), 1)
                    continue
                for tag, rx in CENSUS:
                    if rx.search(line):
                        body = PREFIX_RE.sub('', line.rstrip('\n'), 1)[:40000]
                        k = (tag, hash(NORM_RE.sub('#', body)))
                        row = census.get(k)
                        if row:
                            row[1] += 1
                            row[3], row[4] = n, body
                        elif per_tag.get(tag, 0) < MAX_PER_TAG:
                            per_tag[tag] = per_tag.get(tag, 0) + 1
                            census[k] = [n, 1, body, n, body]
                        else:
                            capped[tag] = capped.get(tag, 0) + 1       # a new shape past the cap: counted, not stored
                        break
    except OSError:
        return None
    return {'path': path, 'start': start or '', 'runnum': runnum, 'version': version, 'flags': flags, 'nlines': n, 'exe_log': exe_log,
            'census': [(k[0], v[0], v[1], v[2], v[3], v[4]) for k, v in census.items()], 'pacs': pacs, 'capped': capped}


def exe_of(logpath):
    d = os.path.dirname(os.path.dirname(logpath))
    if os.path.basename(os.path.dirname(logpath)).lower() != 'logs':
        d = os.path.dirname(logpath)
    exe = os.path.join(d, 'wwe2k14.exe')
    if os.path.exists(exe):
        return exe, time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(exe))), 1
    return exe, '', 0


def _connect(path=None):
    path = path or rdb_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    con = sqlite3.connect(path, timeout=120, isolation_level=None)       # autocommit: transactions are explicit (BEGIN IMMEDIATE)
    con.row_factory = sqlite3.Row
    return con


LINES_SQL = ('CREATE VIRTUAL TABLE IF NOT EXISTS {name} USING fts5(text, run_id UNINDEXED, lineno UNINDEXED, kind UNINDEXED, count UNINDEXED, which UNINDEXED, '
             'tokenize="unicode61", prefix=\'2 3 4\')')


def _schema(con):
    for stmt in """
        CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY, path TEXT UNIQUE, size INTEGER, mtime REAL, gone INTEGER DEFAULT 0, run_number INTEGER,
            exe TEXT, exe_built TEXT, exe_exists INTEGER, script TEXT, flags TEXT, version TEXT, start TEXT, nlines INTEGER, exe_built_src TEXT DEFAULT '', capped TEXT DEFAULT '');
        CREATE TABLE IF NOT EXISTS pacs(run_id INTEGER, name TEXT, first_frame INTEGER, n INTEGER);
        CREATE INDEX IF NOT EXISTS pacs_name ON pacs(name);
        CREATE INDEX IF NOT EXISTS pacs_run ON pacs(run_id);
    """.split(';'):      # execute() not executescript(): executescript would COMMIT the caller's transaction
        if stmt.strip():
            con.execute(stmt)
    con.execute(LINES_SQL.format(name='lines'))
    cols = {r[1] for r in con.execute('PRAGMA table_info(runs)')}
    for c in ('exe_built_src', 'capped'):
        if c not in cols:
            con.execute(f"ALTER TABLE runs ADD COLUMN {c} TEXT DEFAULT ''")


def _migrate(con, full):
    """a version bump KEEPS what cannot be re-read: the rows of logs that are gone from disk are copied into the new tables; logs still on disk are re-parsed"""
    _schema(con)
    if full:
        for t in ('runs', 'pacs', 'lines'):
            con.execute(f'DELETE FROM {t}')
        return
    gone_ids = [r['id'] for r in con.execute('SELECT id, path FROM runs').fetchall() if not os.path.exists(r['path'])]
    con.execute('DROP TABLE IF EXISTS lines_new')
    con.execute(LINES_SQL.format(name='lines_new'))
    has_which = 'which' in {r[1] for r in con.execute('PRAGMA table_info(lines)')}
    which_col = 'which' if has_which else "'first'"
    for i in range(0, len(gone_ids), 500):
        ids = gone_ids[i:i + 500]
        q = ','.join('?' * len(ids))
        con.execute(f'INSERT INTO lines_new(text, run_id, lineno, kind, count, which) SELECT text, run_id, lineno, kind, count, {which_col} FROM lines WHERE run_id IN ({q})', ids)
    con.execute('DROP TABLE lines')
    con.execute('ALTER TABLE lines_new RENAME TO lines')
    keep = set(gone_ids)
    for r in con.execute('SELECT id FROM runs').fetchall():
        if r['id'] not in keep:
            con.execute('DELETE FROM pacs WHERE run_id = ?', (r['id'],))
            con.execute('DELETE FROM runs WHERE id = ?', (r['id'],))


def update(path=None, full=False, quiet=True, logs=None):
    from concurrent.futures import ProcessPoolExecutor
    default = path is None
    path = path or rdb_path()
    if default and not os.path.exists(path):
        _carry_over(path)
    con = _connect(path)
    con.execute('PRAGMA journal_mode=WAL')
    con.execute('PRAGMA busy_timeout=120000')
    try:
        ver = con.execute("SELECT value FROM meta WHERE key='version'").fetchone()
    except sqlite3.Error:
        ver = None
    if full or not ver or ver[0] != str(VERSION):
        con.execute('BEGIN IMMEDIATE')
        try:
            try:
                cur_ver = con.execute("SELECT value FROM meta WHERE key='version'").fetchone()      # another process may have migrated while we waited for the lock
            except sqlite3.Error:
                cur_ver = None
            if full or not cur_ver or cur_ver[0] != str(VERSION):
                _migrate(con, full)
                con.execute("INSERT OR REPLACE INTO meta VALUES ('version', ?)", (str(VERSION),))
            con.execute('COMMIT')
        except Exception:
            con.execute('ROLLBACK')
            raise
    else:
        _schema(con)
    known = {r['path']: (r['id'], r['size'], r['mtime']) for r in con.execute('SELECT id, path, size, mtime FROM runs')}
    todo, seen = [], set()
    for p in (logs if logs is not None else find_logs()):
        seen.add(p)
        try:
            st = os.stat(p)
        except OSError:
            continue
        k = known.get(p)
        if k and k[1] == st.st_size and abs(k[2] - st.st_mtime) < 1:
            continue
        todo.append((p, st.st_size, st.st_mtime))
    t0 = time.time()
    results = []
    if todo:      # parse OUTSIDE any transaction (it takes seconds); two callers may parse the same log, the second write replaces the first
        with ProcessPoolExecutor(max_workers=min(4, len(todo)), initializer=_low) as ex:
            results = list(ex.map(parse_log, [t[0] for t in todo], chunksize=2))
    con.execute('BEGIN IMMEDIATE')
    try:
        known = {r['path']: r['id'] for r in con.execute('SELECT id, path FROM runs')}       # re-read under the lock
        gone = [k for k in known if k not in seen]
        if gone:
            con.executemany('UPDATE runs SET gone = 1 WHERE path = ?', [(g,) for g in gone])
        con.executemany('UPDATE runs SET gone = 0 WHERE path = ? AND gone = 1', [(p,) for p in seen if p in known])
        for (p, size, mt), r in zip(todo, results):
            if r is None:
                continue
            old = known.get(p)
            if old is not None:
                con.execute('DELETE FROM pacs WHERE run_id = ?', (old,))
                con.execute('DELETE FROM lines WHERE run_id = ?', (old,))
                con.execute('DELETE FROM runs WHERE id = ?', (old,))
            exe, built_now, ex_ = exe_of(p)
            built, src = (r['exe_log'], 'log') if r['exe_log'] else (built_now, 'file')
            script = next((v for k, v, sc in r['flags'] if k == 'wwe2k14_input_script' and sc == 'command-line'), '') or next((v for k, v, sc in r['flags'] if k == 'wwe2k14_input_script'), '')
            flags = ' '.join(f'{k}={v}@{sc}' for k, v, sc in r['flags'] if k != 'wwe2k14_input_script')
            capped = ' '.join(f'{k}:{v}' for k, v in sorted(r['capped'].items()))
            cur = con.execute('INSERT OR REPLACE INTO runs(path,size,mtime,run_number,exe,exe_built,exe_exists,script,flags,version,start,nlines,exe_built_src,capped) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                              (p, size, mt, r['runnum'], exe, built, ex_, script, flags, r['version'], r['start'], r['nlines'], src, capped))
            rid = cur.lastrowid
            head = f'run {r["runnum"]} exe {exe} built {built} route {script} version {r["version"]} flags {flags}'
            con.execute('INSERT INTO lines(text, run_id, lineno, kind, count, which) VALUES (?,?,?,?,?,?)', (head, rid, 0, 'run', 1, 'first'))
            rows = []
            for tag, ln, c, t, ln2, t2 in r['census']:
                rows.append((t, rid, ln, tag, c, 'first'))
                if c > 1 and ln2 != ln:
                    rows.append((t2, rid, ln2, tag, c, 'last'))
            con.executemany('INSERT INTO lines(text, run_id, lineno, kind, count, which) VALUES (?,?,?,?,?,?)', rows)
            con.executemany('INSERT INTO pacs VALUES (?,?,?,?)', [(rid, nm, fr, nn) for nm, (fr, nn) in r['pacs'].items()])
        con.execute("INSERT OR REPLACE INTO meta VALUES ('updated', ?)", (time.strftime('%Y-%m-%d %H:%M'),))
        con.execute('COMMIT')
    except Exception:
        con.execute('ROLLBACK')
        raise
    if todo:
        print(f'q: runlog index updated: {len(todo)} log(s) in {time.time() - t0:.1f} s', file=sys.stderr)
    return con


def _fts_query(words):
    """each word is split at every non-alphanumeric character into tokens that must follow each other as a PHRASE (`rdr_owner` = "rdr owner"), prefix on the last
    part (`rdr_own` finds rdr_owner); several words are ANDed"""
    parts = []
    for w in words:
        toks = [x for x in re.split(r'[^0-9A-Za-z]+', w) if x]
        if toks:
            parts.append('"' + ' '.join(toks) + '"' + ('*' if len(toks[-1]) >= 2 else ''))
    return ' AND '.join(parts)


def _run_filter(a):
    cl, pr = [], []
    if a.exe:
        cl.append('lower(r.exe) LIKE ?')
        pr.append('%' + a.exe.lower().replace('\\', '/').replace('/', '%') + '%' if False else '%' + a.exe.lower() + '%')
    if a.route:
        cl.append('lower(r.script) LIKE ?')
        pr.append('%' + a.route.lower() + '%')
    return cl, pr


def _short(p, n=2):
    parts = p.replace('\\', '/').split('/')
    return '/'.join(parts[-n:])


def snippet(text, words, before=40, after=190):
    """the part of a long census line around the first word that matches (lines run to 10 k characters)"""
    low = text.lower()
    pos = [low.find(w.lower()) for w in words if low.find(w.lower()) >= 0]
    i = min(pos) if pos else 0
    st = max(0, i - before)
    return ('...' if st else '') + text[st:i + after] + ('...' if i + after < len(text) else '')


def exe_note(r):
    """the exe's time is the file's time when the log was indexed unless the game logged its own `exe built` line; a file newer than the run = a rebuild since"""
    if not r['exe_built']:
        return 'exe gone'
    if r['exe_built_src'] == 'log':
        return 'built ' + r['exe_built']
    if r['start'] and r['exe_built'][:16] > r['start'][:16]:
        return 'exe file time ' + r['exe_built'] + ': REBUILT AFTER this run (the exe that ran is not the one on disk)'
    return 'exe file time ' + r['exe_built']


def run_label(r):
    return f"run {r['run_number'] if r['run_number'] is not None else '?'}"


def search(con, a):
    cl, pr = _run_filter(a)
    if not a.words:
        q = 'SELECT * FROM runs r' + (' WHERE ' + ' AND '.join(cl) if cl else '') + ' ORDER BY r.start DESC LIMIT ?'
        rows = con.execute(q, pr + [a.limit]).fetchall()
        res = [{'run': r['run_number'], 'start': r['start'], 'exe': r['exe'], 'exe_built': r['exe_built'], 'exe_note': exe_note(r), 'capped': r['capped'], 'route': r['script'], 'flags': r['flags'][:300], 'log': r['path'],
                'log_gone': bool(r['gone'])} for r in rows]
        return {'kind': 'runs', 'n': len(res), 'results': res}
    fq = _fts_query(a.words)
    if not fq:
        raise RunlogError('no searchable words')
    q = ('SELECT r.*, l.lineno, l.kind, l.count, l.which, l.text FROM lines l JOIN runs r ON r.id = l.run_id WHERE lines MATCH ?' + (' AND ' + ' AND '.join(cl) if cl else '')
         + ' ORDER BY r.start DESC, l.lineno LIMIT ?')
    rows = con.execute(q, [fq] + pr + [a.limit]).fetchall()
    total = con.execute('SELECT COUNT(*) FROM lines l JOIN runs r ON r.id = l.run_id WHERE lines MATCH ?' + (' AND ' + ' AND '.join(cl) if cl else ''), [fq] + pr).fetchone()[0]
    res = [{'run': r['run_number'], 'start': r['start'], 'exe': r['exe'], 'route': r['script'], 'line': r['lineno'], 'kind': r['kind'], 'times': r['count'], 'which': r['which'], 'capped': r['capped'], 'exe_note': exe_note(r), 'text': r['text'],
            'log': r['path'], 'log_gone': bool(r['gone'])} for r in rows]
    return {'kind': 'lines', 'n': total, 'results': res}


def which(con, a):
    if not a.mode:
        raise RunlogError('which: --mode X is required')
    x = a.mode.upper()
    cl, pr = _run_filter(a)
    where = (' AND ' + ' AND '.join(cl)) if cl else ''
    rows = con.execute('SELECT r.*, p.name AS pac, p.first_frame AS pf, p.n AS pn FROM pacs p JOIN runs r ON r.id = p.run_id WHERE upper(p.name) LIKE ?' + where + ' ORDER BY r.start DESC',
                       ['%' + x + '%'] + pr).fetchall()
    runs = {}
    for r in rows:
        e = runs.setdefault(r['id'], {'run': r['run_number'], 'start': r['start'], 'exe': r['exe'], 'route': r['script'], 'log_gone': bool(r['gone']), 'pacs': {}, 'census': []})
        e['pacs'][r['pac']] = (r['pf'], r['pn'])
    fq = _fts_query([a.mode])
    if fq:
        lrows = con.execute('SELECT r.*, l.lineno, l.text FROM lines l JOIN runs r ON r.id = l.run_id WHERE lines MATCH ? AND l.kind != ?' + where + ' ORDER BY r.start DESC', [fq, 'run'] + pr).fetchall()
        for r in lrows:
            e = runs.setdefault(r['id'], {'run': r['run_number'], 'start': r['start'], 'exe': r['exe'], 'route': r['script'], 'log_gone': bool(r['gone']), 'pacs': {}, 'census': []})
            if len(e['census']) < 2:
                e['census'].append((r['lineno'], r['text'][:200]))
    by_route = {}
    for e in runs.values():
        by_route.setdefault(e['route'] or '(no route script: launched with the launcher / by hand)', []).append(e)
    return {'mode': a.mode, 'runs': len(runs), 'routes': {k: sorted(v, key=lambda e: e['start'], reverse=True) for k, v in by_route.items()}}


def print_which(r, limit):
    print(f"q runlog which --mode {r['mode']}: {r['runs']} run(s) in {len(r['routes'])} route(s) opened a pac or logged a census line naming it")
    shown = 0
    for route, es in sorted(r['routes'].items(), key=lambda kv: kv[1][0]['start'], reverse=True):
        print(f"  route {_short(route, 3)}: {len(es)} run(s), newest {es[0]['start']}")
        for e in es[:3]:
            pacs = ', '.join(f"{n}@f{f}" for n, (f, c) in sorted(e['pacs'].items(), key=lambda kv: kv[1][0])[:3])
            print(f"    run {e['run']}  {e['start']}  exe {_short(e['exe'], 4)}" + (f"  pac {pacs}" if pacs else '') + ('  [log gone]' if e['log_gone'] else ''))
            for ln, tx in e['census'][:1]:
                print(f'        :{ln} {tx}')
        shown += 1
        if shown >= limit:
            print(f"  ... {len(r['routes']) - shown} more route(s) (--limit N)")
            break
    if not r['runs']:
        print('  (none: no indexed run opened a pac or logged a census line with that name; `q runlog info` says how many logs are indexed)')


def info(con):
    n, g = con.execute('SELECT COUNT(*), SUM(gone) FROM runs').fetchone()
    exes = con.execute('SELECT exe, COUNT(*) c, MIN(start) a, MAX(start) b FROM runs GROUP BY exe ORDER BY b DESC').fetchall()
    return {'runs': n, 'logs_gone': g or 0, 'lines': con.execute('SELECT COUNT(*) FROM lines').fetchone()[0], 'pacs': con.execute('SELECT COUNT(DISTINCT name) FROM pacs').fetchone()[0],
            'updated': dict(con.execute('SELECT key, value FROM meta').fetchall()).get('updated'),
            'exes': [{'exe': e['exe'], 'runs': e['c'], 'first': e['a'], 'last': e['b']} for e in exes]}


# ---- field values per window (S103 QCF): `q runlog field FIELD [--run RUN]`

_NUM = r'-?\d[\d.]*'
FIELD_VAL = re.compile(r'(?:\s*[:=]\s*|\s+)(\[[^\]]*\]|' + _NUM + r'(?:\s*\([^)]*\))?(?:\s+of\s+' + _NUM + r')?|[^,;]{1,100})')
WINDOW_RE = re.compile(r'^(?:\[[^\]]+\]\s*)*frames? (\d+(?:-\d+)?)|\bframes (\d+-\d+)')      # display only: the window the line leads with (`[host] frames 510`, `[scene][live] frames 0-600`)
TS_RE = re.compile(r'^\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)')


def resolve_run(run):
    """a run number (as the log file name: 7317 / 027), a log path, or None = the newest log -> (path, note)"""
    if run is not None and os.path.isfile(str(run)):
        return str(run), ''
    logs = find_logs()
    if not logs:
        raise RunlogError('no run logs found (logs\\, out\\build\\*\\logs, out\\release\\build\\logs, out\\exe\\*\\logs)')
    if run is None:
        best = max(logs, key=lambda p: os.path.getmtime(p))
        return best, 'newest log'
    folder = None
    run = str(run)
    if ':' in run and not os.path.isabs(run):      # S106 G2L: FOLDER:NNN = log NNN of out\exe\FOLDER (numbers repeat per folder)
        folder, run = run.rsplit(':', 1)
        logs = [p for p in logs if os.path.basename(os.path.dirname(os.path.dirname(p))).lower() == folder.lower()
                and os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(p)))).lower() == 'exe']
    try:
        want = int(str(run))
    except ValueError:
        raise RunlogError(f'run "{run}": give a run number, FOLDER:NNN (an out\\exe\\ folder) or a log path')
    if folder is None:      # S106 G2LF F10: a bare number keeps out\build's / logs\'s answer; out\exe logs (each folder numbers from 001) only through FOLDER:NNN
        exe_root = os.path.normcase(os.path.join(REPO, 'out', 'exe')) + os.sep
        logs = [p for p in logs if not os.path.normcase(p).startswith(exe_root)]
    hit = []
    for p in logs:
        m = re.search(r'wwe2k14_(\d+)\.log$', p)
        if m and int(m.group(1)) == want:
            hit.append(p)
    if not hit:
        raise RunlogError(f'no log wwe2k14_{want:03d}.log among {len(logs)} logs' + (f' of out\\exe\\{folder}' if folder else ' (out\\exe\\ logs are reached as FOLDER:NNN, e.g. lane-x-abc123def:' + str(want) + ')'))
    hit.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return hit[0], ((f'{len(hit)} logs carry that number (newest taken; others: ' + ', '.join(hit[1:3]) + ')') if len(hit) > 1 else '')


def field_values(field, run=None, path=None):
    """the field's value on every log line that prints it: [{line, ts, window, tag, value}], consecutive identical values collapsed in the printout"""
    field = field.strip().strip('"')
    if not field:
        raise RunlogError('runlog field: give the field name')
    p, note = resolve_run(run) if path is None else (path, '')
    rx = re.compile(r'(?<![\w])' + re.escape(field) + r'(?![\w])')
    rows = []
    with open(p, encoding='utf-8', errors='replace') as f:
        for i, ln in enumerate(f, 1):
            if field not in ln:
                continue
            body = PREFIX_RE.sub('', ln.rstrip('\n'))
            for m in rx.finditer(body):
                v = FIELD_VAL.match(body, m.end())
                tag = re.match(r'((?:\[[^\]]+\])+)', body)
                if tag and m.start() < tag.end():       # the field is a part of the line's [tag][tag]: its value is the rest of the line
                    rest = body[tag.end():].strip()
                    v = re.match(r'(.{0,110})', rest) if rest else None
                ts = TS_RE.match(ln)
                w = WINDOW_RE.search(body)
                wtxt = (w.group(1) or w.group(2)) if w else ''
                rows.append({'line': i, 'ts': ts.group(1) if ts else '', 'window': wtxt, 'tag': tag.group(1) if tag else '',
                             'value': re.sub(r'\s+', ' ', v.group(1)).strip() if v else ''})
                break
    return {'field': field, 'log': p, 'note': note, 'n': len(rows), 'rows': rows}


def print_field_values(r, limit=30):
    print(f'q runlog field "{r["field"]}": {r["n"]} line(s) in {r["log"]}' + (f'  ({r["note"]})' if r['note'] else ''))
    if not r['rows']:
        print('  the log never prints it (check the spelling with `q ours census` / `q ours str`: the format literal may spell it differently)')
        return
    prev, shown, skipped = None, 0, 0
    for k, x in enumerate(r['rows']):
        last = k == len(r['rows']) - 1
        if x['value'] == prev and not last:
            skipped += 1
            continue
        if skipped:
            print(f'      (... {skipped} more line(s) with the same value)')
            skipped = 0
        d = ''
        try:
            if prev is not None and re.fullmatch(_NUM, x['value'].split()[0]) and re.fullmatch(_NUM, prev.split()[0]):
                d = f'  ({float(x["value"].split()[0]) - float(prev.split()[0]):+g})'
        except (ValueError, IndexError):
            pass
        print(f'  :{x["line"]:<7d}{x["ts"]}  {x["window"] or "-":>12s}  {x["tag"] or "-"}  {x["value"]}{d}')
        prev = x['value']
        shown += 1
        if shown >= limit * 2 and not last:
            print(f'  ... {len(r["rows"]) - k - 1} more line(s) (--limit)')
            break


def main(argv):
    ap = argparse.ArgumentParser(prog='q runlog', description=__doc__.split('\n\n')[0])
    ap.add_argument('words', nargs='*')
    ap.add_argument('--mode', default=None, help='with `which`: a scene id / pac name part (ENTRANCE00, BG04)')
    ap.add_argument('--exe', default=None)
    ap.add_argument('--route', default=None)
    ap.add_argument('--limit', type=int, default=20)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--run', default=None, help='with `field`: the run (log file number or a log path); default the newest log')
    ap.add_argument('--db', default=None)
    a = ap.parse_args(argv)
    words = a.words
    verb = words[0] if words and words[0] in ('which', 'info', 'update', 'field') else None
    if verb:
        a.words = words[1:]
    if verb == 'field':
        if not a.words:
            raise RunlogError('runlog field FIELD [--run RUN]: give the field (and optionally --run N: the run number or a log path)')
        r = field_values(' '.join(a.words), a.run)
        if a.json:
            print(json.dumps(r, indent=1))
        else:
            print_field_values(r, a.limit)
        return 0
    con = update(a.db, full=a.full and verb == 'update')
    if verb == 'update':
        print(json.dumps(info(con), indent=1) if a.json else f"q runlog: {info(con)['runs']} run(s) indexed")
        return 0
    if verb == 'info':
        r = info(con)
        if a.json:
            print(json.dumps(r, indent=1))
        else:
            print(f"q runlog: {r['runs']} run log(s) indexed ({r['logs_gone']} gone from disk, rows kept), {r['lines']} searchable lines, {r['pacs']} distinct pacs opened, updated {r['updated']}")
            for e in r['exes'][:a.limit]:
                print(f"  {e['runs']:4d} run(s)  {e['first']} .. {e['last']}  {e['exe']}")
        return 0
    if verb == 'which':
        r = which(con, a)
        if a.json:
            for ro in r['routes'].values():
                for e in ro:
                    e['pacs'] = {k: list(v) for k, v in e['pacs'].items()}
            print(json.dumps(r, indent=1))
        else:
            print_which(r, a.limit)
        return 0
    r = search(con, a)
    if a.json:
        print(json.dumps(r, indent=1))
        return 0
    if r['kind'] == 'runs':
        print(f"q runlog: {r['n']} run(s)")
        for x in r['results']:
            print(f"  run {x['run']}  {x['start']}  exe {_short(x['exe'], 4)} [{x['exe_note']}]  route {_short(x['route'], 2) or '-'}" + ('  [log gone]' if x['log_gone'] else ''))
        return 0
    print(f"q runlog: {r['n']} line(s) hold all of: {' '.join(a.words)}" + (f" (first {a.limit})" if r['n'] > a.limit else ''))
    for x in r['results']:
        rep = (f" (x{x['times']}, {x['which']} of them)" if x['which'] == 'last' else f" (x{x['times']}, first)") if x['times'] and x['times'] > 1 else ''
        print(f"  run {x['run']}  {x['start']}  exe {_short(x['exe'], 4)} [{x['exe_note']}]  route {_short(x['route'], 1) or '-'}  :{x['line']}{rep}  {snippet(x['text'], a.words)}")
    capped = sorted({f"run {x['run']} ({x['capped']})" for x in r['results'] if x.get('capped')})
    if capped:
        print(f"  (capped: {MAX_PER_TAG} line shapes per kind per run are stored; past that only a count is kept, so a line may be missing for: {', '.join(capped[:6])})")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except RunlogError as e:
        print(f'q: {e}', file=sys.stderr)
        sys.exit(2)

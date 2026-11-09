"""q fact: one search over the project's knowledge library (Session 98, "lodge what you learn").

    py tools\\q\\q.py fact WORDS... [--json] [--limit N] [--kind format|system|module|db|session] [--rebuild]

Searches, in one ranked list:
  docs\\formats\\*.md      file formats / tables / record kinds        (weight 1.0)
  docs\\systems\\*.md      system pages                                 (weight 1.0)
  docs\\modules\\*.md      module summaries                             (weight 0.85)
  db\\*.json              function db: name, summary, evidence         (weight 0.7)
  SESSION*_SUMMARY.md    session running logs                         (weight 0.35, lowest rank)
Each hit: file + heading + line, the matching lines of that section (short), and every guest address in the section
expanded to its db name / confidence / file.

WORDS: all must match (a section that has every word); when nothing matches, any word (marked "partial").
Words of 4+ letters match as prefixes (particle -> particles); short ones also match a trailing s (cap -> caps);
an address (0x8230C688, sub_8230C688, 8230C688) matches every spelling of it; a_b matches the phrase "a b".

The index is our own text only (docs and db, no game data): out\\harness\\index\\fact.db, rebuilt automatically
when any source file changes (size / mtime), ~1-2 s; a query on a fresh index takes well under 1 s.
`documented_in(addr)` (used by `q fn` / `q db`) lists the format / system / module sections that name an address.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import sqlite3
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
INDEX = os.path.join(ROOT, 'out', 'harness', 'index', 'fact.db')
VERSION = 2

# kind, glob (relative to ROOT), weight
SOURCES = (
    ('format', os.path.join('docs', 'formats', '*.md'), 1.0),
    ('system', os.path.join('docs', 'systems', '*.md'), 1.0),
    ('module', os.path.join('docs', 'modules', '*.md'), 0.85),
    ('db', os.path.join('db', '*.json'), 0.7),
    ('session', 'SESSION*_SUMMARY.md', 0.35),
)
WEIGHT = {k: w for k, _, w in SOURCES}
DOC_KINDS = ('format', 'system', 'module')

ADDR_RE = re.compile(r'(?<![0-9A-Za-z])(?:sub_|0x)?(8[23][0-9A-Fa-f]{6})(?![0-9A-Za-z])')
HEAD_RE = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
STOP = {'the', 'a', 'an', 'of', 'and', 'in', 'is', 'to', 'how', 'what', 'does', 'where', 'which', 'for', 'on', 'by',
        'it', 'its', 'with', 'from', 'at', 'as', 'or', 'be', 'do', 'why', 'when', 'who'}
MAX_CHUNK_LINES = 40


def _addr_tokens(text):
    """'addr8230c688' tokens for every guest address in the text (all spellings map to one token)."""
    return sorted({'addr' + m.group(1).lower() for m in ADDR_RE.finditer(text)})


def _addrs(text):
    out = []
    for m in ADDR_RE.finditer(text):
        a = int(m.group(1), 16)
        if a not in out:
            out.append(a)
    return out


# ---------------------------------------------------------------- sources -> chunks

def source_files():
    files = []
    for kind, pat, _ in SOURCES:
        for p in sorted(glob.glob(os.path.join(ROOT, pat))):
            if os.path.isfile(p) and not os.path.basename(p).startswith('_template'):
                files.append((kind, p))
    return files


def signature(files):
    h = hashlib.sha1(str(VERSION).encode())
    for kind, p in files:
        try:
            st = os.stat(p)
        except OSError:
            continue
        h.update(f'{kind}|{os.path.relpath(p, ROOT)}|{st.st_size}|{st.st_mtime_ns}\n'.encode('utf-8', 'replace'))
    return h.hexdigest()


def md_chunks(lines, session=False):
    """[(heading_path, start_line, [lines])] by heading; long sections split at blank lines (<= ~40 lines). In a session
    summary every top-level bullet is its own chunk (a running-log entry)."""
    chunks = []
    title = ''
    heads = {}
    cur_head = ''
    buf, start = [], 1

    def flush():
        nonlocal buf
        if any(x.strip() for x in buf):
            chunks.append((cur_head, start, buf))
        buf = []

    for i, line in enumerate(lines, 1):
        m = HEAD_RE.match(line)
        if m:
            flush()
            lvl, text = len(m.group(1)), m.group(2)
            if lvl == 1 and not title:
                title = text
            heads = {k: v for k, v in heads.items() if k < lvl}
            heads[lvl] = text
            path = [heads[k] for k in sorted(heads) if not (k == 1 and heads[k] == title)]
            cur_head = (title + (' > ' if path else '') + ' > '.join(path)) if title else ' > '.join(path)
            buf, start = [line], i
            continue
        if session and re.match(r'^[-*] ', line):
            flush()
            buf, start = [line], i
            continue
        if not buf:
            start = i
        buf.append(line)
        if not session and len(buf) >= MAX_CHUNK_LINES and not line.strip():
            flush()
            start = i + 1
    flush()
    return chunks


def db_chunks(path):
    """[(heading, line, [lines], entry)] per db entry; line = the entry's "address" line in the file."""
    try:
        with open(path, encoding='utf-8-sig') as f:   # (a BOM must not break json.loads)
            raw = f.read()
        d = json.loads(raw)
    except (OSError, ValueError) as e:
        print(f'q fact: WARNING cannot parse {os.path.relpath(path, ROOT)} ({e}): left out of the index', file=sys.stderr)
        return []
    lines_of = {}
    for i, line in enumerate(raw.splitlines(), 1):
        m = re.search(r'"address"\s*:\s*"([^"]+)"', line)
        if m:
            lines_of.setdefault(m.group(1), i)
    ents = d if isinstance(d, list) else (d.get('functions', []) if isinstance(d, dict) else [])
    out = []
    for e in ents:
        if not isinstance(e, dict) or 'address' not in e:
            continue
        a = str(e.get('address'))
        body = [f'{a} {e.get("name", "")}  [{e.get("system", "")}, {e.get("confidence", "")}]',
                str(e.get('summary', ''))]
        body += [f'- {x}' for x in e.get('evidence', []) if isinstance(x, str)]
        if e.get('source'):
            body.append(f'source: {e["source"]}')
        out.append((f'{a} {e.get("name", "")}', lines_of.get(a, 0), body, e))
    return out


CONF_RANK = {'high': 3, 'medium': 2, 'low': 1, 'unknown': 0}


def _fill(con, files, sig):
    con.executescript('''
        CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE chunks(id INTEGER PRIMARY KEY, kind TEXT, file TEXT, heading TEXT, line INTEGER, text TEXT);
        CREATE VIRTUAL TABLE fts USING fts5(heading, body, content='', tokenize='unicode61');
        CREATE TABLE names(addr INTEGER, name TEXT, confidence TEXT, system TEXT, file TEXT, rank INTEGER);
        CREATE TABLE mentions(addr INTEGER, kind TEXT, file TEXT, heading TEXT, line INTEGER);
        CREATE INDEX names_addr ON names(addr);
        CREATE INDEX mentions_addr ON mentions(addr);
    ''')
    cid = 0
    n_by_kind = {}

    def add(kind, rel, heading, line, body_lines):
        nonlocal cid
        cid += 1
        text = '\n'.join(body_lines)
        con.execute('INSERT INTO chunks VALUES (?,?,?,?,?,?)', (cid, kind, rel, heading, line, text))
        con.execute('INSERT INTO fts(rowid, heading, body) VALUES (?,?,?)',
                    (cid, heading + ' ' + ' '.join(_addr_tokens(heading)), text + ' ' + ' '.join(_addr_tokens(text))))
        n_by_kind[kind] = n_by_kind.get(kind, 0) + 1
        return text

    for kind, p in files:
        rel = os.path.relpath(p, ROOT)
        if kind == 'db':
            for heading, line, body, e in db_chunks(p):
                add(kind, rel, heading, line, body)
                try:
                    a = int(str(e['address']), 16)
                except ValueError:
                    continue
                con.execute('INSERT INTO names VALUES (?,?,?,?,?,?)',
                            (a, e.get('name'), e.get('confidence'), e.get('system'), os.path.basename(p),
                             CONF_RANK.get(e.get('confidence'), 0) + (0 if os.path.basename(p) == 'bulk_sonnet.json' else 10)))
            continue
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        for heading, line, body in md_chunks(lines, session=(kind == 'session')):
            text = add(kind, rel, heading, line, body)
            if kind in DOC_KINDS:
                seen = set()
                for k, bl in enumerate(body):
                    for a in _addrs(bl):
                        if a not in seen:
                            seen.add(a)
                            con.execute('INSERT INTO mentions VALUES (?,?,?,?,?)', (a, kind, rel, heading, line + k))
    con.execute('INSERT INTO meta VALUES (?,?)', ('signature', sig))
    con.execute('INSERT INTO meta VALUES (?,?)', ('built', time.strftime('%Y-%m-%d %H:%M:%S')))
    con.execute('INSERT INTO meta VALUES (?,?)', ('counts', json.dumps(n_by_kind)))
    con.commit()


TMP_YOUNG_S = 10.0     # another process's build younger than this: answer from the old index instead of building too
TMP_OLD_S = 120.0      # leftover tmp files older than this are removed


def _tmps(path):
    return glob.glob(glob.escape(path) + '.*.tmp')


def build(path=INDEX, files=None, sig=None):
    """Build into a per-process tmp and move it over `path`. -> (seconds, the path to read). When `path` cannot be
    replaced (another process has it open: WinError 5 / 32) after 5 tries 100 ms apart, the tmp itself is served
    (kept; a later build removes it once old)."""
    t0 = time.time()
    files = files if files is not None else source_files()
    sig = sig or signature(files)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for old in _tmps(path):   # leftovers of crashed or locked builds
        try:
            if time.time() - os.path.getmtime(old) > TMP_OLD_S:
                os.remove(old)
        except OSError:
            pass
    tmp = f'{path}.{os.getpid()}.tmp'
    keep = False
    try:
        if os.path.exists(tmp):
            os.remove(tmp)
        con = sqlite3.connect(tmp)
        try:
            _fill(con, files, sig)
        finally:
            con.close()
        for _ in range(5):
            try:
                os.replace(tmp, path)
                return time.time() - t0, path
            except OSError:
                time.sleep(0.1)
        keep = True
        return time.time() - t0, tmp
    finally:
        if not keep and os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass


def _open(path):
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    con.row_factory = sqlite3.Row
    return con


def connect(rebuild=False):
    """The index, rebuilt first when a source changed. -> (con, {'rebuilt_s': seconds or None, 'note': str or None})."""
    files = source_files()
    sig = signature(files)
    if os.path.exists(INDEX) and not rebuild:
        try:
            con = _open(INDEX)
            row = con.execute("SELECT value FROM meta WHERE key = 'signature'").fetchone()
            if row and row[0] == sig:
                return con, {'rebuilt_s': None, 'note': None}
            # another process is rebuilding right now: answer from the old index rather than build in parallel
            mine = f'{INDEX}.{os.getpid()}.tmp'
            if any(t != mine and time.time() - os.path.getmtime(t) < TMP_YOUNG_S for t in _tmps(INDEX)):
                return con, {'rebuilt_s': None, 'note': '(stale) another process is rebuilding the library index'}
            con.close()
        except (sqlite3.Error, OSError):
            pass
    took, served = build(INDEX, files, sig)
    note = None if served == INDEX else '(index file locked by another process: served from a fresh copy)'
    return _open(served), {'rebuilt_s': took, 'note': note}


# ---------------------------------------------------------------- query

def query_terms(words):
    """WORDS -> ([fts term], [plain lowercase term for highlighting])."""
    terms, plain = [], []
    for w in words:
        for tok in re.findall(r'[\w+.-]+', w):   # \w: non-ASCII words too
            tok = tok.strip('+.-')
            if not tok:
                continue
            m = ADDR_RE.fullmatch(tok) or re.fullmatch(r'(?:sub_|0x)?(8[23][0-9A-Fa-f]{6})', tok)
            if m:
                terms.append('addr' + m.group(1).lower())
                plain.append(m.group(1).lower())
                continue
            low = tok.lower()
            if low in STOP:
                continue
            parts = [x for x in re.split(r'[_.-]', low) if x]   # a_b, a.b, a-b = the phrase "a b"
            if len(parts) > 1:
                terms.append('"' + ' '.join(parts) + '"')
                plain.append(' '.join(parts))
                continue
            if low.isalpha() and len(low) >= 4:
                terms.append(f'{low}*')
            elif low.isalpha():
                terms.append(f'({low} OR {low}s)')
            else:
                terms.append(f'"{low}"')
            plain.append(low)
    return terms, plain


def search(con, words, limit=8, kind=None):
    terms, plain = query_terms(words)
    if not terms:
        return {'query': ' '.join(words), 'hits': [], 'mode': 'none'}
    mode = 'all'
    rows = _run(con, ' AND '.join(terms), kind)
    if not rows and len(terms) > 1:
        mode = 'any (partial: no section has every word)'
        rows = _run(con, ' OR '.join(terms), kind)
    scored = []
    phrase = ' '.join(plain) if len(plain) > 1 or (plain and ' ' in plain[0]) else None
    for r in rows:
        s = -r['score'] * WEIGHT.get(r['kind'], 0.5)
        low = (r['heading'] + '\n' + r['text']).lower()
        if mode != 'all':   # partial: how many words the section has counts first
            s *= 1 + sum(1 for p in set(plain) if p in low)
        if r['kind'] != 'db':   # (db entries are short: bm25 already favours them)
            if phrase and re.search(re.escape(phrase).replace(r'\ ', r'[\s_-]+'), low):
                s *= 2.0        # the words side by side ("cap swap", "soft-particle"): the section is about that
            last = r['heading'].rsplit(' > ', 1)[-1]
            if all(_matches(last, [p]) for p in plain):
                s *= 1.5        # every word in the section's own heading
        scored.append((s, r))
    scored.sort(key=lambda x: -x[0])
    hits = []
    for s, r in scored[:limit]:
        hits.append(_hit(con, r, s, plain))
    return {'query': ' '.join(words), 'mode': mode, 'n_matches': len(rows), 'hits': hits}


def _run(con, match, kind):
    sql = ('SELECT c.id, c.kind, c.file, c.heading, c.line, c.text, bm25(fts, 4.0, 1.0) AS score FROM fts '
           'JOIN chunks c ON c.id = fts.rowid WHERE fts MATCH ?')
    params = [match]
    if kind:
        sql += ' AND c.kind = ?'
        params.append(kind)
    sql += ' ORDER BY score LIMIT 400'
    try:
        return con.execute(sql, params).fetchall()
    except sqlite3.OperationalError as e:
        raise ValueError(f'bad query {match!r}: {e}')


def _matches(line, plain):
    low = line.lower()
    n = 0
    for p in plain:
        if re.fullmatch(r'8[23][0-9a-f]{6}', p):
            n += p in low
        elif ' ' in p:   # a phrase: its words joined by space, _, - or .
            n += re.search(re.escape(p).replace(r'\ ', r'[\s_.-]+'), low) is not None
        elif len(p) >= 4:
            n += p in low
        else:
            n += re.search(r'(?<![a-z0-9])' + re.escape(p) + r's?(?![a-z0-9])', low) is not None
    return n


def _hit(con, r, score, plain, max_lines=4, width=200):
    lines = r['text'].splitlines()
    body = lines[1:] if lines and HEAD_RE.match(lines[0]) else lines
    ranked = sorted(((_matches(x, plain), i) for i, x in enumerate(body) if x.strip()), key=lambda t: (-t[0], t[1]))
    if r['kind'] == 'db':   # a db entry: always its name line and summary, then the matching evidence lines
        pick = sorted({0, 1} | {i for n, i in ranked[:max_lines] if n > 0 and i > 1})[:max_lines + 1]
        pick = [i for i in pick if i < len(body)]
    else:
        pick = sorted(i for n, i in ranked[:max_lines] if n > 0) or [i for _, i in sorted(ranked, key=lambda t: t[1])[:2]]
    off = 1 if lines and HEAD_RE.match(lines[0]) else 0
    shown = []
    for i in pick:
        t = body[i].strip()
        shown.append({'line': r['line'] + i + off, 'text': t if len(t) <= width else t[:width - 3] + '...'})
    funcs = []
    for a in _addrs(r['text'])[:12]:
        n = con.execute('SELECT name, confidence, system, file FROM names WHERE addr = ? ORDER BY rank DESC LIMIT 1',
                        (a,)).fetchone()
        funcs.append({'addr': f'0x{a:08X}', 'name': n['name'] if n else None,
                      'confidence': n['confidence'] if n else None, 'file': n['file'] if n else None})
    return {'kind': r['kind'], 'file': r['file'], 'heading': r['heading'], 'line': r['line'],
            'score': round(score, 3), 'lines': shown, 'functions': funcs}


def documented_in(addr, kinds=('format', 'system', 'module')):
    """[{kind, file, heading, line}] of the doc sections that name `addr` (format pages first). Never raises: the
    caller (q fn / q db) still answers when the library index cannot be built."""
    try:
        con, _ = connect()
        rows = con.execute('SELECT kind, file, heading, MIN(line) AS line FROM mentions WHERE addr = ? '
                           'GROUP BY kind, file, heading ORDER BY CASE kind WHEN \'format\' THEN 0 WHEN \'system\' '
                           'THEN 1 ELSE 2 END, file, line', (addr,)).fetchall()
        con.close()
        return [dict(r) for r in rows if r['kind'] in kinds]
    except Exception as e:   # noqa: BLE001 - a library problem must not break q fn
        return [{'kind': 'error', 'file': '', 'heading': f'library index unavailable: {e!r}', 'line': 0}]


def print_result(r, info):
    if info.get('rebuilt_s') is not None:
        print(f'(library index rebuilt in {info["rebuilt_s"]:.1f} s)')
    if info.get('note'):
        print(info['note'])
    if not r['hits']:
        print(f'no match for: {r["query"]}')
        return
    if r['mode'] != 'all':
        print(f'match: {r["mode"]}')
    for k, x in enumerate(r['hits'], 1):
        print(f'{k}. {x["file"]}:{x["line"]}  [{x["kind"]}]  {x["heading"]}')
        for ln in x['lines']:
            print(f'     {ln["line"]:>5}: {ln["text"]}')
        if x['functions']:
            parts = []
            for f in x['functions'][:8]:
                parts.append(f'{f["addr"]} = {f["name"]} ({f["confidence"]}, {f["file"]})' if f['name']
                             else f'{f["addr"]} (no db entry)')
            print('     fn: ' + '; '.join(parts) + (' ...' if len(x['functions']) > 8 else ''))


def main(argv=None):
    ap = argparse.ArgumentParser(prog='q fact', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('words', nargs='+')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--limit', type=int, default=8)
    ap.add_argument('--kind', choices=[k for k, _, _ in SOURCES])
    ap.add_argument('--rebuild', action='store_true', help='rebuild the library index first')
    args = ap.parse_args(argv)
    try:
        con, info = connect(args.rebuild)
        r = search(con, args.words, args.limit, args.kind)
    except (ValueError, sqlite3.Error, OSError) as e:
        if args.json:
            print(json.dumps({'error': str(e)}))
        else:
            print(f'q fact: {e}', file=sys.stderr)
        return 2
    if args.json:
        r['index'] = INDEX
        r['rebuilt_s'] = info['rebuilt_s']
        r['index_note'] = info['note']
        print(json.dumps(r, indent=1))
    else:
        print_result(r, info)
    return 0


if __name__ == '__main__':
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    sys.exit(main())

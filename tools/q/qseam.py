r"""q seam (S111 lane SEAM): the SEAM index. For one topic (depth, fade, readiness, ...) one lookup shows the GAME's rule (addresses, what
each does in our own words, its evidence, a mark [C] / [L] / [U] and the note that settled it) BESIDE our lines that handle the same thing
(file:line), each marked match (we do what the game does), replace (we do it our own way on purpose: why) or repeat (we redo work the game
already did). A probe that opens an entry must not have to go back to the listings for what the entry states.

    py tools/q/q.py seam                 every topic, one line each (its summary, row counts, stale rows)
    py tools/q/q.py seam TOPIC           the two sides of one entry; every ours row with its CURRENT line (ok / AMBIGUOUS / STALE)
    py tools/q/q.py seam --check         lint every entry: exit 1 on a refused entry or a STALE row, or a game row whose source note is missing
    py tools/q/q.py seam --diffs [--topic T] [--verdict replace|repeat] [--files] [--json]
                                         every replace / repeat ours row of every entry (never a match row): where ours departs from the
                                         game, one line each with the game row(s) it stands against; --files groups by our file (tools/q/qseam_diff.py)
    py tools/q/q.py seam --cites FILE[:LINE]   every seam row and db/claims row that cites the file (an ours row's path, or a file:line in a row's text) (qseam_cites.py)
    (--json on any of them; --root DIR answers for another checkout, as the selftest does)

Store: db/seams/<topic>.json, committed, OUR annotations only: addresses and our own words, never game code, never listing text.
  {"topic": "depth", "summary": "one paragraph",
   "game": [{"addr": "0x82C224A0", "what": "...", "evidence": "...", "mark": "C", "source": "docs/research/..."}],
   "ours": [{"path": "src/...", "anchor": "the text of that line", "verdict": "match", "why": "..."}],
   "open": ["an open question, with the one piece of evidence that would settle it"]}
Refused (by --check and by the writers): no game row; a game row without an address (0x82XXXXXX / 0x83XXXXXX), a mark other than
C / L / U, or no what / evidence / source; no ours row; an ours row without a repo path (/ separators), an anchor, a verdict
match / replace / repeat, or a why; an ours row that stores a `line` (or the retired `ctx`); a path with a `..` segment.
No line number (S111 SEAMNOLINE: it rotted with every edit above it and made two lanes that edited one file conflict on every merge): a row is
located by its `anchor` ALONE, the text of the line, compared stripped of surrounding white space; the writers store the anchor of the line
they are given and refuse a blank line. When the anchor text is on SEVERAL lines of the file, the row also stores `"occurrence": k` (1-based:
the k-th of those lines, top to bottom), and a row without it is a lint failure (AMBIGUOUS; row_state).
Staleness: one line holds the anchor (or the `occurrence` names one): ok, and the lookup shows the line it is at NOW; no line holds it, or
`occurrence` is past the last, or the file is gone: STALE (loud; --check and the lookup exit 1).
Writers for other tools (the note-lodging lane): game_row(), ours_row(), add_entry(), add_row(); each raises SeamError and writes nothing
when refused. `q fact` indexes these files too (tools/q/qfact.py kind `seam`, fact_chunk() here); `q find WORD` names the entries whose
topic or summary holds WORD (find_block() here).
"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SEAMS_REL = 'db/seams'
TOPIC_RE = re.compile(r'^[a-z0-9][a-z0-9_-]{0,47}$')
ADDR_RE = re.compile(r'^0x8[23][0-9A-F]{6}$')
MARKS = ('C', 'L', 'U')
VERDICTS = ('match', 'replace', 'repeat')
# selftest only: each plant switches ONE rule off, so the selftest can see its planted case fail without the rule
PLANT = os.environ.get('WWE_QSEAM_PLANT', '')     # no_addr_rule | no_stale | no_occurrence | no_line_rule


class SeamError(ValueError):
    """a refused entry or row; the message names every rule it broke"""


def _root(root):
    return os.path.abspath(root) if root else ROOT


def seams_dir(root=None):
    return os.path.join(_root(root), *SEAMS_REL.split('/'))


def entry_path(topic, root=None):
    return os.path.join(seams_dir(root), topic + '.json')


def topics(root=None):
    """the topic names stored under db/seams (sorted)"""
    return sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(seams_dir(root), '*.json')))


def norm_addr(a):
    """'sub_82c224a0' / '82C224A0' / '0x82c224a0' -> '0x82C224A0'; anything else comes back as given (and problems() refuses it)"""
    s = str(a).strip()
    m = re.fullmatch(r'(?:sub_|0[xX])?([0-9A-Fa-f]{8})', s)
    return '0x' + m.group(1).upper() if m else s


def _file_lines(path, root=None):
    try:
        with open(os.path.join(_root(root), *str(path).split('/')), encoding='utf-8', errors='replace', newline='') as f:
            text = f.read()
    except OSError:
        return None
    # lines end at \n only (as q show and an editor count them): str.splitlines() also splits at \x0c, \x1c-\x1e, \x85, \u2028 and moves
    # every later line number by one
    lines = [x[:-1] if x.endswith('\r') else x for x in text.split('\n')]
    return lines[:-1] if lines and lines[-1] == '' else lines


def anchor_lines(lines, anchor):
    """the 1-based numbers of the lines whose text, stripped, is the anchor (stripped)"""
    want = str(anchor).strip()
    return [i + 1 for i, t in enumerate(lines) if t.strip() == want] if want else []


def occurrence_of(lines, line):
    """-> (k, n): `line` (1-based) is the k-th (1-based) of the n lines that hold its own text"""
    at = anchor_lines(lines, lines[line - 1])
    return at.index(line) + 1, len(at)


def row_state(row, root=None):
    """one ours row against its file as it is NOW -> {'state': 'ok' | 'stale' | 'ambiguous', 'line': N or None, 'why': str}.
    A row is located by its anchor ALONE (S111 SEAMNOLINE: a stored line number rots with every edit above it and conflicted every merge
    of two lanes that edited the file). ok = exactly one line holds the anchor text ('line' = its number now), or several do and the row's
    `occurrence` k (1-based, which of them) names one; stale = no line holds it, `occurrence` is past the last one, or the file is gone;
    ambiguous = several lines hold it and the row has no `occurrence` (a lint failure: the row must say which)."""
    lines = _file_lines(row.get('path', ''), root)
    if lines is None:
        return {'state': 'stale', 'line': None, 'why': 'the file is gone'}
    at = anchor_lines(lines, row.get('anchor', ''))
    if not at:
        if PLANT == 'no_stale':
            return {'state': 'ok', 'line': 1, 'why': ''}
        return {'state': 'stale', 'line': None, 'why': 'no line of the file holds the anchor text'}
    k = row.get('occurrence')
    k = k if isinstance(k, int) and not isinstance(k, bool) and k >= 1 else None
    if len(at) == 1 and k in (None, 1):
        return {'state': 'ok', 'line': at[0], 'why': ''}
    if k is None:
        if PLANT == 'no_occurrence':
            return {'state': 'ok', 'line': at[0], 'why': ''}
        return {'state': 'ambiguous', 'line': None,
                'why': f'{len(at)} lines hold the anchor text (lines {", ".join(str(x) for x in at[:6])}{"..." if len(at) > 6 else ""}): the row needs "occurrence": 1..{len(at)}'}
    if k > len(at):
        if PLANT == 'no_stale':
            return {'state': 'ok', 'line': at[-1], 'why': ''}
        return {'state': 'stale', 'line': None, 'why': f'occurrence {k} asked, but {len(at)} line(s) hold the anchor text now'}
    return {'state': 'ok', 'line': at[k - 1], 'why': ''}


def set_anchor_at(row, line, root=None):
    """point one ours row at `line` of its file: its anchor becomes that line's text and `occurrence` is stored when the text is on several
    lines (dropped when it is on one), right after `anchor` as ours_row() puts it. -> True when the row changed."""
    lines = _file_lines(row.get('path', ''), root)
    if lines is None or not 1 <= line <= len(lines) or not lines[line - 1].strip():
        return False
    before = json.dumps(row, ensure_ascii=False)
    k, n = occurrence_of(lines, line)
    items = [(key, v) for key, v in row.items() if key not in ('anchor', 'occurrence', 'line', 'ctx')]
    row.clear()
    for key, v in items:
        row[key] = v
        if key == 'path':
            row['anchor'] = lines[line - 1].strip()
            if n > 1:
                row['occurrence'] = k
    row.setdefault('anchor', lines[line - 1].strip())
    return json.dumps(row, ensure_ascii=False) != before


def problems(entry, root=None, anchors=False):
    """[str]: every rule the entry breaks (empty = accepted). anchors=True also refuses an ours row whose anchor is not its line's text
    now (the writers' rule; --check reports moved / STALE rows separately); a set of row numbers (1-based) checks only those rows (add_row:
    the new row only, so a row that moved or went stale since it was lodged never blocks lodging another; the lookup and --check report it)."""
    if not isinstance(entry, dict):
        return ['the entry is not a JSON object']
    out = []
    t = entry.get('topic')
    if not isinstance(t, str) or not TOPIC_RE.match(t):
        out.append(f'topic {t!r}: lower-case letters, digits, _ or -, at most 48')
    if not isinstance(entry.get('summary'), str) or not entry['summary'].strip():
        out.append('no summary')
    game = entry.get('game')
    if not isinstance(game, list) or not game:
        out.append('no game row: an entry needs at least one game row carrying an address')
        game = []
    for i, g in enumerate(game, 1):
        if not isinstance(g, dict):
            out.append(f'game row {i}: not an object')
            continue
        if PLANT != 'no_addr_rule' and not ADDR_RE.match(str(g.get('addr', ''))):
            out.append(f'game row {i}: no address (addr {g.get("addr")!r}; a game row carries 0x82XXXXXX / 0x83XXXXXX)')
        if g.get('mark') not in MARKS:
            out.append(f'game row {i}: mark {g.get("mark")!r} is not C, L or U')
        for k in ('what', 'evidence', 'source'):
            if not isinstance(g.get(k), str) or not g[k].strip():
                out.append(f'game row {i}: no {k}')
    ours = entry.get('ours')
    if not isinstance(ours, list) or not ours:
        out.append('no ours row: an entry needs at least one ours row carrying a path and an anchor')
        ours = []
    for i, o in enumerate(ours, 1):
        if not isinstance(o, dict):
            out.append(f'ours row {i}: not an object')
            continue
        p = o.get('path')
        if not isinstance(p, str) or not p.strip() or '\\' in p or os.path.isabs(p) or p.startswith('/'):
            out.append(f'ours row {i}: path {p!r} is not a repo-relative path with / separators')
        elif '..' in p.split('/'):
            out.append(f'ours row {i}: path {p!r} leaves the repo (a .. segment)')
        if 'line' in o and PLANT != 'no_line_rule':
            out.append(f'ours row {i}: a stored `line` rots with every edit above it: remove it (the anchor locates the row)')
        if 'ctx' in o:
            out.append(f'ours row {i}: `ctx` is retired: remove it ("occurrence": k says which of several identical lines)')
        if 'occurrence' in o and (not isinstance(o['occurrence'], int) or isinstance(o['occurrence'], bool) or o['occurrence'] < 1):
            out.append(f'ours row {i}: occurrence {o["occurrence"]!r} is not a number >= 1 (which of the lines holding the anchor, 1-based)')
        if not isinstance(o.get('anchor'), str) or not o['anchor'].strip():
            out.append(f'ours row {i}: no anchor (the text of the line)')
        if o.get('verdict') not in VERDICTS:
            out.append(f'ours row {i}: verdict {o.get("verdict")!r} is not match, replace or repeat')
        if not isinstance(o.get('why'), str) or not o['why'].strip():
            out.append(f'ours row {i}: no why')
        if isinstance(p, str) and isinstance(o.get('anchor'), str) and o['anchor'].strip() and '..' not in p.split('/'):
            st = row_state(o, root)
            if st['state'] == 'ambiguous':
                out.append(f'ours row {i} ({p}): AMBIGUOUS anchor: {st["why"]}')
            elif st['state'] == 'stale' and anchors and (anchors is True or i in anchors):
                out.append(f'ours row {i} ({p}): the anchor is in no line of the file now (STALE: {st["why"]})')
    op = entry.get('open', [])
    if not isinstance(op, list) or not all(isinstance(x, str) and x.strip() for x in op):
        out.append('open: a list of non-empty strings')
    return out


# ---------------------------------------------------------------- writers (other tools call these)

def game_row(addr, what, evidence, mark, source):
    """-> a game row: addr (any of sub_X / 0xX / X, stored as 0x82XXXXXX), what it does in OUR words, the evidence (offsets, constants, the
    q call that shows it), mark 'C' | 'L' | 'U', source = the note that settled it (repo path). Checked when it is written."""
    return {'addr': norm_addr(addr), 'what': what, 'evidence': evidence, 'mark': mark, 'source': source}


def ours_row(path, line, verdict, why, root=None, expect=None):
    """-> an ours row for the line `line` of path (repo-relative, / separators; the number is only how the caller names the line NOW, it is
    never stored): the anchor is that line's text, plus `occurrence` when that text is on several lines; verdict 'match' | 'replace' |
    'repeat', why = one sentence. expect = a piece of text the line must hold (the tree moves while a probe reads: a line number read
    earlier may now be another line). Raises SeamError when the file or the line is not there, the line is blank, or it does not hold
    `expect`."""
    path = str(path).replace('\\', '/')
    if '..' in path.split('/') or os.path.isabs(path) or path.startswith('/'):
        raise SeamError(f'{path}: a seam path is repo-relative and never leaves the repo (no .. segment)')
    lines = _file_lines(path, root)
    if lines is None:
        raise SeamError(f'{path}: no such file under {_root(root)}')
    if not isinstance(line, int) or not 1 <= line <= len(lines):
        raise SeamError(f'{path}:{line}: the file has {len(lines)} lines')
    if not lines[line - 1].strip():
        raise SeamError(f'{path}:{line}: a blank line is no anchor')
    if expect is not None and expect not in lines[line - 1]:
        near = [i + 1 for i, t in enumerate(lines) if expect in t]
        raise SeamError(f'{path}:{line}: the line does not hold {expect!r}' + (f' (lines that do: {near[:5]})' if near else ''))
    row = {'path': path, 'anchor': lines[line - 1].strip()}
    k, n = occurrence_of(lines, line)
    if n > 1:
        row['occurrence'] = k
    row.update({'verdict': verdict, 'why': why})
    return row


def _text(entry):
    keep = {k: entry[k] for k in ('topic', 'summary', 'game', 'ours', 'open') if k in entry}
    keep.setdefault('open', [])
    return json.dumps(keep, indent=1, ensure_ascii=False) + '\n'


def _write(entry, root, dry_run, anchors=True):
    bad = problems(entry, root, anchors=anchors)
    if bad:
        raise SeamError(f'seam {entry.get("topic")!r} refused, nothing written:\n  ' + '\n  '.join(bad))
    path, text = entry_path(entry['topic'], root), _text(entry)
    if not dry_run:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    return path, text


def add_entry(topic, summary, game, ours, open_questions=(), root=None, replace=False, dry_run=False):
    """Write db/seams/<topic>.json from game rows (game_row()), ours rows (ours_row()) and open questions (strings). Refused (SeamError,
    nothing written) when any rule of problems(anchors=True) fails, or when the topic exists and replace is False.
    dry_run=True: checked, not written. -> (path, text)."""
    if not replace and os.path.exists(entry_path(str(topic), root)):
        raise SeamError(f'seam {topic!r} exists ({entry_path(str(topic), root)}): add_row() adds to it, replace=True rewrites it')
    return _write({'topic': topic, 'summary': summary, 'game': list(game), 'ours': list(ours), 'open': list(open_questions)}, root, dry_run)


def add_row(topic, side, row, root=None, dry_run=False):
    """Append one row to an existing entry: side 'game' (a game_row()), 'ours' (an ours_row()) or 'open' (a string). The whole entry is
    checked again before it is written, the anchor of the NEW ours row only (an older row that moved or went stale is reported by the
    lookup and --check, it does not block this one); a refusal (SeamError) writes nothing. -> (path, text)."""
    path = entry_path(str(topic), root)
    try:
        entry = load(path)
    except (OSError, ValueError) as e:
        raise SeamError(f'seam {topic!r}: cannot read {path} ({e}); add_entry() makes a new one')
    if side not in ('game', 'ours', 'open'):
        raise SeamError(f'side {side!r} is not game, ours or open')
    entry.setdefault(side, []).append(row)
    new = {len(entry['ours'])} if side == 'ours' and isinstance(entry.get('ours'), list) else set()
    return _write(entry, root, dry_run, anchors=new)


# ---------------------------------------------------------------- readers

def load(path):
    with open(path, encoding='utf-8-sig') as f:
        return json.load(f)


def _rel(path, root):
    return os.path.relpath(path, _root(root)).replace('\\', '/')


def _summary_line(s, width=140):
    s = ' '.join(str(s).split())
    m = re.search(r'\. ', s)
    s = s[:m.start() + 1] if m else s
    return s if len(s) <= width else s[:width - 3] + '...'


def find_block(root, x):
    """q find's seam section: the entries whose topic is the question (or one of its words) or whose summary holds it as a word;
    `seam` / `seams` = every entry. -> {'topics': [{topic, file, summary, exact}]} or None. Never raises."""
    q = ' '.join(str(x).lower().split())
    out = []
    for t in topics(root):
        p = entry_path(t, root)
        try:
            e = load(p)
        except (OSError, ValueError):
            continue
        summ = str(e.get('summary', '')) if isinstance(e, dict) else ''
        exact = t == q or t in q.split()
        word = bool(q) and re.search(r'(?<![a-z0-9_])' + re.escape(q) + r'(?![a-z0-9_])', summ.lower()) is not None
        if q in ('seam', 'seams') or exact or word:
            out.append({'topic': t, 'file': _rel(p, root), 'summary': _summary_line(summ), 'exact': exact})
    out.sort(key=lambda r: (not r['exact'], r['topic']))
    return {'topics': out} if out else None


def fact_chunk(path):
    """q fact's chunk of one entry -> (heading, [lines]); a file that cannot be read -> ('', []) with a warning (never raises)."""
    try:
        e = load(path)
        topic = e['topic']
    except (OSError, ValueError, KeyError, TypeError) as ex:
        print(f'q fact: WARNING cannot parse seam {path} ({ex}): left out of the index', file=sys.stderr)
        return '', []
    body = [f'seam {topic}: {e.get("summary", "")}']
    for g in e.get('game', []) or []:
        if isinstance(g, dict):
            body.append(f'game {g.get("addr", "")} [{g.get("mark", "")}] {g.get("what", "")} (evidence: {g.get("evidence", "")}; {g.get("source", "")})')
    for o in e.get('ours', []) or []:
        if isinstance(o, dict):
            body.append(f'ours {o.get("path", "")} {o.get("verdict", "")}: {o.get("why", "")} [{str(o.get("anchor", "")).strip()[:100]}]')
    for q in e.get('open', []) or []:
        body.append(f'open: {q}')
    body.append(f'(q seam {topic})')
    return f'seam {topic}', body


def _states(e, root):
    ours = e.get('ours') or [] if isinstance(e, dict) else []
    return [row_state(o, root) if isinstance(o, dict) else {'state': 'stale', 'line': None, 'why': 'not an object'} for o in ours]


def _where(o, st):
    """path:LINE-NOW of an ours row (the line the anchor holds now; the bare path when it holds none or several without `occurrence`)"""
    return f'{o.get("path")}:{st["line"]}' if st.get('line') else f'{o.get("path")}'


def print_entry(e, rel, root):
    """the two sides of one entry -> the number of STALE rows"""
    bad = problems(e, root)
    print(f'== seam {e.get("topic")}   ({rel})')
    if bad:
        print('!! REFUSED (q seam --check exits 1):\n  ' + '\n  '.join(bad))
    print(str(e.get('summary', '')))
    game = [g for g in e.get('game') or [] if isinstance(g, dict)]
    print(f'GAME ({len(game)} rows: the rule, in our words)')
    for g in game:
        print(f'  {g.get("addr")} [{g.get("mark")}]  {g.get("what")}')
        print(f'      evidence: {g.get("evidence")}')
        print(f'      settled in: {g.get("source")}')
    ours = [o for o in e.get('ours') or [] if isinstance(o, dict)]
    print(f'OURS ({len(ours)} rows: our lines that handle the same thing)')
    stale = 0
    for o, st in zip(ours, _states({'ours': ours}, root)):
        where = _where(o, st)
        if st['state'] == 'ok':
            head = where
        elif st['state'] == 'ambiguous':
            stale += 1
            head = f'!! AMBIGUOUS {where}'
        else:
            stale += 1
            head = f'!! STALE {where}'
        print(f'  {head}  {o.get("verdict")}')
        print(f'      why: {o.get("why")}')
        a = str(o.get('anchor', ''))
        print(f'      line: {a if len(a) <= 160 else a[:157] + "..."}')
        if st['state'] == 'stale':
            print(f'      !! STALE: {st["why"]}: the row no longer says where we handle this; re-read the code and re-lodge the row')
        elif st['state'] == 'ambiguous':
            print(f'      !! AMBIGUOUS: {st["why"]}')
    op = e.get('open') or []
    if op:
        print(f'OPEN ({len(op)})')
        for q in op:
            print(f'  - {q}')
    return stale


def check(root=None):
    """-> [{topic, file, problems, stale}] for every entry (an AMBIGUOUS anchor is a problem, see problems())"""
    out = []
    for t in topics(root):
        p = entry_path(t, root)
        r = {'topic': t, 'file': _rel(p, root), 'problems': [], 'stale': []}
        try:
            e = load(p)
        except (OSError, ValueError) as ex:
            r['problems'].append(f'cannot parse: {ex}')
            out.append(r)
            continue
        r['problems'] = problems(e, root)
        if isinstance(e, dict) and isinstance(e.get('game'), list):
            for i, g in enumerate(e['game'], 1):
                src = re.split(r'[\s#]', str(g.get('source', '')).strip())[0].rstrip(',;') if isinstance(g, dict) else ''
                if src and not os.path.exists(os.path.join(_root(root), *src.split('/'))):
                    r['problems'].append(f'game row {i}: source note {src!r} does not exist')
        if isinstance(e, dict) and e.get('topic') != t:
            r['problems'].append(f'the file is {t}.json but its topic is {e.get("topic")!r}')
        ours = e.get('ours') if isinstance(e, dict) and isinstance(e.get('ours'), list) else []
        for i, (o, st) in enumerate(zip(ours, _states(e, root)), 1):
            if isinstance(o, dict) and st['state'] == 'stale':
                r['stale'].append(f'ours row {i} {o.get("path")}: {st["why"]}')
        out.append(r)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog='q seam', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('topic', nargs='?')
    ap.add_argument('--check', action='store_true', help='lint every entry (exit 1 on a refusal or a STALE row)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--diffs', action='store_true', help='every replace / repeat ours row (where ours departs from the game)')
    ap.add_argument('--topic', dest='diffs_topic', default=None, help='--diffs: one topic only (as is `--diffs TOPIC`)')
    ap.add_argument('--verdict', choices=('replace', 'repeat'), default=None, help='--diffs: one verdict only')
    ap.add_argument('--files', action='store_true', help='--diffs: group by our file instead of by topic')
    ap.add_argument('--cites', default=None, metavar='FILE[:LINE]', help='which seam rows and db/claims rows cite this file (or file:line) (qseam_cites.py)')
    ap.add_argument('--all', action='store_true', help='--cites: every row (default 40)')
    ap.add_argument('--root', default=None, help='answer for another checkout (default: the one holding this tool)')
    a = ap.parse_args(argv)
    root = _root(a.root)
    if a.cites:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qseam_cites
        return qseam_cites.run(a, root)
    if a.diffs:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qseam_diff
        return qseam_diff.run(a, root)
    if a.check:
        rs = check(root)
        n_bad = sum(1 for r in rs if r['problems'] or r['stale'])
        if a.json:
            print(json.dumps({'entries': rs, 'failed': n_bad}, indent=1))
            return 1 if n_bad else 0
        for r in rs:
            ok = not r['problems'] and not r['stale']
            print(f'{"ok  " if ok else "FAIL"} {r["topic"]}  ({r["file"]})')
            for x in r['problems']:
                print(f'     refused: {x}')
            for x in r['stale']:
                print(f'     !! STALE {x}')
        print(f'seam check: {len(rs)} entries, {n_bad} failed' + (' (exit 1)' if n_bad else ''))
        return 1 if n_bad else 0
    if not a.topic:
        ts = topics(root)
        rows = []
        for t in ts:
            p = entry_path(t, root)
            try:
                e = load(p)
                sts = _states(e, root)
                rows.append({'topic': t, 'file': _rel(p, root), 'summary': _summary_line(e.get('summary', '')), 'game': len(e.get('game') or []),
                             'ours': len(e.get('ours') or []), 'stale': sum(1 for s in sts if s['state'] == 'stale')})
            except (OSError, ValueError, AttributeError) as ex:
                rows.append({'topic': t, 'file': _rel(p, root), 'summary': f'(cannot parse: {ex})', 'game': 0, 'ours': 0, 'stale': 0})
        if a.json:
            print(json.dumps({'topics': rows}, indent=1))
            return 0
        print(f'{len(rows)} seam topics ({SEAMS_REL}); one entry: q seam TOPIC; lint: q seam --check')
        for r in rows:
            print(f'  {r["topic"]:<14} {r["summary"]}  [{r["game"]} game / {r["ours"]} ours' + (f', !! {r["stale"]} STALE' if r['stale'] else '') + ']')
        return 0
    t = a.topic.strip().lower()
    p = entry_path(t, root)
    if not os.path.exists(p):
        fb = find_block(root, t)
        msg = f'q seam: no entry {t!r} ({_rel(p, root)})' + ('; entries that name it: ' + ', '.join(x['topic'] for x in fb['topics']) if fb else
                                                            '; topics: ' + (', '.join(topics(root)) or 'none'))
        if a.json:
            print(json.dumps({'error': msg}))
        else:
            print(msg)
        return 1
    try:
        e = load(p)
    except (OSError, ValueError) as ex:
        print(f'q seam: cannot parse {_rel(p, root)}: {ex}')
        return 1
    if a.json:
        print(json.dumps({'entry': e, 'file': _rel(p, root), 'problems': problems(e, root), 'ours_state': _states(e, root)}, indent=1))
        return 1 if any(s['state'] == 'stale' for s in _states(e, root)) or problems(e, root) else 0
    stale = print_entry(e, _rel(p, root), root)
    return 1 if stale or problems(e, root) else 0


if __name__ == '__main__':
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    sys.exit(main())

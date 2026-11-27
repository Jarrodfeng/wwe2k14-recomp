r"""q seam --diffs (S111 lane SEAMDIFF): the one list of where ours departs from the game; judge() = check.py's `seam` row.

    py tools/q/q.py seam --diffs [--topic T] [--verdict replace|repeat] [--files] [--json]
        every `replace` and `repeat` ours row of every db/seams entry (`match` rows are never listed), grouped by topic (or by our file with
        --files): verdict, path:line (the line the anchor holds NOW; `!! STALE` / `!! AMBIGUOUS` as the lookup shows it), the why, and under
        it the game row(s) of the same entry the row stands against (address + the first line of what, at most 3, then a count). The end:
        totals per verdict and per topic. Exit 1 when a listed row is STALE.
"""
import json
import os
import sys

import qseam

GAME_SHOWN = 3


def _first(s, w=110):
    s = ' '.join((str(s).strip().splitlines() or [''])[0].split())
    return s if len(s) <= w else s[:w - 3] + '...'


def diff_rows(root=None, topic=None, verdict=None):
    """-> [{topic, verdict, path, line (now, None when stale / ambiguous), state, why, game: [{addr, what}]}] for every replace / repeat ours row"""
    out = []
    for t in qseam.topics(root):
        if topic and t != topic:
            continue
        try:
            e = qseam.load(qseam.entry_path(t, root))
        except (OSError, ValueError):
            continue
        if not isinstance(e, dict):
            continue
        game = [{'addr': g.get('addr'), 'what': str(g.get('what', ''))} for g in e.get('game') or [] if isinstance(g, dict)]
        for o in e.get('ours') or []:
            if not isinstance(o, dict) or o.get('verdict') not in ('replace', 'repeat') or (verdict and o['verdict'] != verdict):
                continue
            st = qseam.row_state(o, root)
            out.append({'topic': t, 'verdict': o['verdict'], 'path': o.get('path'), 'line': st['line'], 'state': st['state'],
                        'why': str(o.get('why', '')), 'game': game})
    return out


def _where(r):
    w = f'{r["path"]}:{r["line"]}' if r['line'] else str(r['path'])
    if r['state'] == 'stale':
        w = f'!! STALE {w}'
    elif r['state'] == 'ambiguous':
        w = f'!! AMBIGUOUS {w}'
    return w


def totals(rows):
    t = {'replace': 0, 'repeat': 0, 'rows': len(rows), 'stale': sum(1 for r in rows if r['state'] == 'stale'), 'topics': {}}
    for r in rows:
        t[r['verdict']] += 1
        t['topics'].setdefault(r['topic'], {'replace': 0, 'repeat': 0})[r['verdict']] += 1
    return t


def print_diffs(rows, by_file):
    key = (lambda r: r['path']) if by_file else (lambda r: r['topic'])
    groups = {}
    for r in rows:
        groups.setdefault(key(r), []).append(r)
    for g in sorted(groups):
        rs = groups[g]
        print(f'== {"file" if by_file else "topic"} {g}   ({len(rs)} rows: replace {sum(1 for r in rs if r["verdict"] == "replace")}, '
              f'repeat {sum(1 for r in rs if r["verdict"] == "repeat")})')
        for r in rs:
            tag = f'[{r["topic"]}] ' if by_file else ''
            loc = _where(r) if not by_file else _where(r).replace(r['path'] + ':', 'line ', 1)
            print(f'  {r["verdict"]:<7} {tag}{loc}   {r["why"]}')
            for gr in r['game'][:GAME_SHOWN]:
                print(f'      vs {gr["addr"]}  {_first(gr["what"])}')
            if len(r['game']) > GAME_SHOWN:
                print(f'      vs (+{len(r["game"]) - GAME_SHOWN} more game rows: q seam {r["topic"]})')
    t = totals(rows)
    print(f'seam diffs: {t["rows"]} rows: replace {t["replace"]}, repeat {t["repeat"]}'
          + (f', !! {t["stale"]} STALE' if t['stale'] else '') + f' in {len(t["topics"])} topics (match rows are not listed)')
    for k in sorted(t['topics']):
        print(f'  {k}: replace {t["topics"][k]["replace"]}, repeat {t["topics"][k]["repeat"]}')


def judge(root, files):
    """check.py's `seam` row: the seam rows a change touches. files = the paths of the change (any iterable of repo paths, deletions too).
    -> (fails, judged): a row whose path is in files and is STALE (anchor gone, or its file gone) or breaks a row rule (a stored `line`, an
    AMBIGUOUS anchor without `occurrence`) is a fail (the entry is fixed in the same change); rows of other files are not judged. An entry the change
    itself edits (db/seams/<topic>.json in files) is judged whole: every row, and its entry rules (a hand-edited row cannot land STALE).
    A game row's missing source note is not judged here (q seam --check reports it: the index's own problem, never a code lane's)."""
    fs = {str(f).replace('\\', '/') for f in files}
    fails, judged = [], 0
    for t in qseam.topics(root):
        whole = f'{qseam.SEAMS_REL}/{t}.json' in fs
        try:
            e = qseam.load(qseam.entry_path(t, root))
        except (OSError, ValueError) as ex:
            if whole:
                fails.append(f'db/seams/{t}.json: cannot parse ({ex})')
            continue
        if not isinstance(e, dict) or not isinstance(e.get('ours'), list):
            if whole:
                fails += [f'db/seams/{t}.json: {p}' for p in qseam.problems(e, root)]
            continue
        probs = qseam.problems(e, root)
        if whole:
            fails += [f'db/seams/{t}.json: {p}' for p in probs]
        for i, o in enumerate(e['ours'], 1):
            if not isinstance(o, dict) or not isinstance(o.get('path'), str) or (not whole and o.get('path') not in fs):
                continue  # a row without a text path: problems() above already failed it (whole), never a crash = BLOCKED
            judged += 1
            where = f'db/seams/{t}.json ours row {i} {o.get("path")}'
            st = qseam.row_state(o, root)
            if st['state'] == 'stale':
                fails.append(f'{where}: STALE ({st["why"]}); this change touches '
                             + ('the entry' if whole else 'the file') + f': fix the entry in the same change (q seam {t})')
            if not whole:
                fails += [f'db/seams/{t}.json: {p}' for p in probs if p.startswith(f'ours row {i}:') or p.startswith(f'ours row {i} (')]
    return fails, judged


def run(a, root):
    topic = (getattr(a, 'diffs_topic', None) or a.topic or '').strip().lower() or None
    if topic and topic not in qseam.topics(root):
        msg = f'q seam --diffs: no entry {topic!r}; topics: ' + (', '.join(qseam.topics(root)) or 'none')
        print(json.dumps({'error': msg}) if a.json else msg)
        return 1
    rows = diff_rows(root, topic, a.verdict)
    if a.json:
        print(json.dumps({'rows': rows, 'totals': totals(rows)}, indent=1))
    else:
        print_diffs(rows, a.files)
    return 1 if any(r['state'] == 'stale' for r in rows) else 0

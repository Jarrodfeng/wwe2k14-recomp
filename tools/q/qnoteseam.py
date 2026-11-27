"""qnoteseam: the lane notes and the seam rows at a function, printed in the fact card (S111 NOTESEAM, from the INDEX-F `q brief` review).

    q decomp sub_823C8548 --fields       header lines:
      ; [notes] CAMSLOT (answer: One scene manager per scene ...; settled sub_823C8190 +3) | CAMOTHER (...) | +4 more (q ours text sub_823C8548 --docs)
      ; [seams] pose_timing via CUTSTUTTER ours=src/scene_live/inputs_contrib.h (open 3) | +1 more (q seam slots)

Two qfactcard providers (registered at the bottom of qfactcard.py, header lines only, ranked BELOW the claim headers: prio 35 / 36, the card's `facts` line is 40):
  notes   the research notes (docs/research/s1*/*.md) that NAME the function (`sub_X` or `0xX`), most mentions first; each with its `## Answer` /
          `## Verdict` text and the other functions its lodged claims settled. The first NOTES_WITH_ANSWER carry the answer, the rest are names;
          a note that does not fit is counted and the exact call for the rest is printed (never dropped silently). `--facts all`: one line per note.
  seams   db/seams/*.json topics at the function, a lead's own seam first: (1) the topic of a seam-claim row at it / a game row of the seam whose
          addr lies in the function, (2) `via NOTE`: a note that has a claim at the function and also a claim naming a seam (the 2 commonest topics),
          (3) `named`: the function's own claim text names the topic as a WHOLE word (`pose_timing` / `pose timing`, a plural s; `promotion` does
          not name `motion`). Each prints our line (`ours=PATH:LINE`, from a seam claim row, else the seam file's first `ours` row) and `(open N)`.
Both read the files once per process. Selftest: py -I tools\\q\\qnoteseam_selftest.py
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WIDTH = 160                # one header line (qfactcard.HEADER_CHARS)
NOTES_WITH_ANSWER = 1      # the notes that carry their answer text in the card line (the rest are names: more of them fit)
ANSWER_CHARS = 34
VIA_TOPICS = 2             # a note's seams reach a function through its claims: the commonest two
ADDR = re.compile(r'(?:sub_|0x)(8[23][0-9A-Fa-f]{6})(?![0-9A-Za-z_])', re.I)
HEAD = re.compile(r'##\s+(Answer|Verdict|The answer)', re.I)

_C = {}                    # cache: ('notes'|'seams'|'settled', root-or-id) -> data


def set_root(root):
    """selftest: read docs/ and db/seams/ from this folder (None = the checkout)"""
    global ROOT
    ROOT = root or os.path.dirname(os.path.dirname(HERE))
    _C.clear()


def _stem(p):
    return os.path.splitext(os.path.basename(str(p or '').replace('\\', '/')))[0]


def _clip(s, n):
    s = ' '.join(str(s).split())
    return s if len(s) <= n else s[:max(n - 3, 0)] + '...'


def _addr(v):
    try:
        return int(str(v), 16)
    except (TypeError, ValueError):
        return None


# ------------------------------------------------------------------ the stores
def _notes():
    """{address: [(count, rel path, stem, title, answer)]}, one pass over the notes"""
    if 'n' in _C:
        return _C['n']
    idx = {}
    for p in sorted(glob.glob(os.path.join(ROOT, 'docs', 'research', 's1*', '*.md'))):
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                txt = f.read()
        except OSError:
            continue
        cnt = {}
        for m in ADDR.finditer(txt):
            a = int(m.group(1), 16)
            cnt[a] = cnt.get(a, 0) + 1
        if not cnt:
            continue
        lines = txt.splitlines()
        title = _clip((lines[0] if lines else '').lstrip('# '), 90)
        ans = ''
        for i, ln in enumerate(lines):
            if HEAD.match(ln):
                buf = []
                for x in lines[i + 1:i + 12]:
                    if x.startswith('#'):
                        break
                    if x.strip():
                        buf.append(x.strip())
                    elif buf:
                        break
                ans = ' '.join(buf)
                break
        rel = os.path.relpath(p, ROOT).replace('\\', '/')
        for a, n in cnt.items():
            idx.setdefault(a, []).append((n, rel, _stem(p), title, ans))
    for v in idx.values():
        v.sort(key=lambda t: (-t[0], t[1]))
    _C['n'] = idx
    return idx


def _seams():
    """[dict] of db/seams/*.json, each with `_topic`"""
    if 's' in _C:
        return _C['s']
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'db', 'seams', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                d = json.load(f)
        except (OSError, ValueError):
            continue
        if isinstance(d, dict):
            d['_topic'] = str(d.get('topic') or os.path.splitext(os.path.basename(p))[0])
            out.append(d)
    _C['s'] = out
    return out


def _rows():
    """the lodged claim rows: the card's own (injected in the selftest), else db/claims"""
    import qfactcard
    if qfactcard._ROWS:
        return qfactcard._ROWS[0]
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import q
    return q.load_claims()


def _settled(rows):
    """{note stem: [function address]} in row order (the functions a note lodged claims about)"""
    key = ('t', id(rows), len(rows))
    if key not in _C:
        _C.pop(next((k for k in _C if isinstance(k, tuple) and k[0] == 't'), None), None)
        out = {}
        for r in rows:
            a = _addr(r.get('function'))
            if a is not None:
                lst = out.setdefault(_stem(r.get('note')).lower(), [])
                if a not in lst:
                    lst.append(a)
        _C[key] = out
    return _C[key]


# ------------------------------------------------------------------ one line within the width, the rest counted with its exact call
def _fit(label, items, rest_call, width):
    """'label a | b | c | +K more (call)': every item that fits; K = the number left out. rest_call(i) -> the call that lists the items from i on"""
    head = label + ' ' if label else ''
    out = []
    for i, it in enumerate(items):
        left = len(items) - i - 1
        tail = '' if not left else f' | +{left} more ({rest_call(i + 1)})'
        if len(head + ' | '.join(out + [it]) + tail) > width:
            if out:
                return head + ' | '.join(out) + f' | +{len(items) - i} more ({rest_call(i)})'
            it = _clip(it, max(width - len(head) - len(tail), 20))      # one item wider than the line: cut it, the rest counted after it
        out.append(it)
    return head + ' | '.join(out)


# ------------------------------------------------------------------ notes
def notes_facts(ctx):
    import qfactcard
    found = _notes().get(ctx.addr, [])
    if not found:
        return []
    rows = _rows()
    sett = _settled(rows)
    here = {_stem(r.get('note')).lower() for r in rows if _addr(r.get('function')) == ctx.addr}
    found = sorted(found, key=lambda t: (t[2].lower() not in here, -t[0], t[1]))      # a note that lodged a claim AT the function first, then the most mentions
    call = f'q ours text sub_{ctx.addr:08X} --docs'
    allm = ctx.mode == 'all'
    items = []
    for n, rel, stem, title, ans in found:
        fs = [f for f in sett.get(stem.lower(), []) if f != ctx.addr]
        fstr = ' '.join(f'sub_{f:08X}' for f in fs[:6]) + (f' +{len(fs) - 6}' if len(fs) > 6 else '')
        if allm:
            items.append(f'{rel} ({n} mentions): {title}' + (f' | answer: {ans}' if ans else '') + (f' | settled {fstr}' if fs else ''))
        elif len(items) < NOTES_WITH_ANSWER and (ans or fs):
            items.append(f'{stem} (' + '; '.join(([f'answer: {_clip(ans.replace("*", ""), ANSWER_CHARS)}'] if ans else []) + ([f'settled {len(fs)}'] if fs else [])) + ')')
        else:
            items.append(stem)
    if allm:
        return [qfactcard.Fact('notes', t, None, None, 35) for t in items]
    return [qfactcard.Fact('notes', _fit('', items, lambda i: call, WIDTH - 11), None, None, 35)]


# ------------------------------------------------------------------ seams
def _named(topic, text):
    """the topic as a WHOLE word of the text (`pose_timing` / `pose timing` / `pose-timing`, a plural s): 'promotion' does not name 'motion'"""
    pat = r'(?<![A-Za-z0-9])' + re.escape(topic.lower()).replace('_', r'[\s_-]+') + r's?(?![A-Za-z0-9])'
    return re.search(pat, str(text).lower()) is not None


def _ours_of(d, rows):
    for r in rows:
        if r.get('ours'):
            return str(r['ours'])
    for o in d.get('ours') or []:
        if isinstance(o, dict) and o.get('path'):
            return str(o['path'])
    return ''


def seam_blocks(addr, size, rows):
    """-> [(rank, topic, how, ours, open count)] the seams at the function, a lead's own first (rank 0 own rows, 1 via a note, 2 named)"""
    rows_at = [r for r in rows if _addr(r.get('function')) == addr or _addr(r.get('address')) == addr]
    by_topic = {}
    for r in rows:
        if r.get('seam'):
            by_topic.setdefault(str(r['seam']), []).append(r)
    own = {t for t in (str(r['seam']) for r in rows_at if r.get('seam'))}
    notes_here = {_stem(r.get('note')).lower() for r in rows_at}
    via = {}
    for t, rs in by_topic.items():
        k = len({1 for r in rs if _stem(r.get('note')).lower() in notes_here})
        if k and t not in own:
            via[t] = (k, sorted(_stem(r.get('note')) for r in rs if _stem(r.get('note')).lower() in notes_here)[0])
    via_top = dict(sorted(via.items(), key=lambda kv: (-kv[1][0], kv[0]))[:VIA_TOPICS])
    text = ' '.join(str(r.get('claim') or '') for r in rows_at if _addr(r.get('function')) == addr)
    out = []
    for d in _seams():
        t = d['_topic']
        game = [g for g in d.get('game') or [] if isinstance(g, dict) and _addr(g.get('addr')) is not None
                and (addr <= _addr(g.get('addr')) < addr + size if size else _addr(g.get('addr')) == addr)]
        if t in own or game:
            rank, how = 0, 'own'
        elif t in via_top:
            rank, how = 1, f'via {via_top[t][1]}'
        elif '_' in t and text and _named(t, text):      # only a compound topic name (a one-word topic such as `latch` / `depth` is in every claim's prose)
            rank, how = 2, 'named'
        else:
            continue
        op = d.get('open') or []
        pick = [r for r in by_topic.get(t, []) if _addr(r.get('function')) == addr] or [r for r in by_topic.get(t, []) if _stem(r.get('note')).lower() in notes_here]
        out.append((rank, t, how, _ours_of(d, pick), len(op)))
    return sorted(out, key=lambda b: (b[0], b[1]))


def seams_facts(ctx):
    import qfactcard
    bl = seam_blocks(ctx.addr, ctx.size, _rows())
    if not bl:
        return []
    items = [f'{t} {how}' + (f' ours={ours}' if ours else '') + (f' (open {n})' if n else '') for _rk, t, how, ours, n in bl]
    if ctx.mode == 'all':
        return [qfactcard.Fact('seams', it + f'  (q seam {b[1]})', None, None, 36) for it, b in zip(items, bl)]
    return [qfactcard.Fact('seams', _fit('', items, lambda i: 'q seam ' + bl[i][1], WIDTH - 11), None, None, 36)]

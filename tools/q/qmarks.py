r"""q marks (S111 QMARKS): the [U] and [L] rows of the whole index in one lookup (the goal: every row at least [L], none [U]).

    py tools/q/q.py marks [--mark U|L|C] [--seam TOPIC] [--note NOTE] [--addr ADDR] [--json] [--all]

Reads db/claims/*.json (every claim row) and db/seams/*.json (every game row, plus each entry's `open` items). Totals first (the whole index, then
the rows the filters keep). Per row: the mark, the address (or our file:line), the claim text, the note it came from, and what would settle it: the
sentence of the row's text that says "settle(s) ..." (notes write "settles: ...", "one ... settles it"); a row with none says so and shows its evidence.
Sorted U, then L, then C. `--mark U` prints every [U] row AND every open item (an open item is an unsettled question by definition; `--mark L|C` and
`--addr` leave them out). Row text is cut at TEXT_CAP characters and the row list through qcap (cap ROW_CAP); the closing line gives the exact call
for the rest (`--all` = no cut; `--json` is never cut).
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qcap  # noqa: E402

ROW_CAP = 60
TEXT_CAP = 240
ORDER = {'U': 0, 'L': 1, 'C': 2}
# selftest planted twin: WWE_QMARKS_PLANT=no_open drops the open items (the case that lists them must then fail)
PLANT = os.environ.get('WWE_QMARKS_PLANT', '')


def _root(root):
    if root:
        return os.path.abspath(root)
    import qseam
    return qseam.ROOT


def _hex(a):
    m = re.fullmatch(r'\s*(?:0[xX])?([0-9A-Fa-f]{1,8})\s*', a or '')
    return int(m.group(1), 16) if m else None


def settle_of(text):
    """the sentence(s) of a row's text that name what settles it"""
    text = text or ''
    out = []
    for m in re.finditer(r'\bsettles?\b|\bsettled by\b', text, re.I):      # not "is not settled" / "settled in review": a state, not a way
        starts = [b.end() for b in re.finditer(r'(?:[.;,]|\]:?)\s+', text[:m.start()])]
        s = starts[-1] if starts else 0
        e = re.compile(r'\.(?:\s|$)').search(text, m.end())
        frag = text[s:e.end() if e else len(text)].strip()
        frag = re.sub(r'^settles?:\s*', '', frag, flags=re.I)
        if frag and frag not in out:
            out.append(frag)
    return ' '.join(out)


def _load(path):
    try:
        with open(path, encoding='utf8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def collect(root=None):
    """-> list of row dicts: kind claim|game|open, mark (C/L/U, or 'open'), where, text, note, seam, settle, evidence"""
    base = _root(root)
    rows = []
    for p in sorted(glob.glob(os.path.join(base, 'db', 'claims', '*.json'))):
        d = _load(p)
        for r in d if isinstance(d, list) else []:
            if not isinstance(r, dict):
                continue
            where = r.get('address') or r.get('ours') or ''
            rows.append({'kind': 'claim', 'mark': r.get('mark') or '?', 'where': where, 'fn': r.get('function') or '', 'text': r.get('claim') or '',
                         'note': r.get('note') or '', 'seam': r.get('seam') or '', 'evidence': r.get('evidence') or '',
                         'file': os.path.basename(p)})
    for p in sorted(glob.glob(os.path.join(base, 'db', 'seams', '*.json'))):
        d = _load(p)
        if not isinstance(d, dict):
            continue
        topic = d.get('topic') or os.path.splitext(os.path.basename(p))[0]
        for g in d.get('game') or []:
            rows.append({'kind': 'game', 'mark': g.get('mark') or '?', 'where': g.get('addr') or '', 'fn': '', 'text': g.get('what') or '',
                         'note': g.get('source') or '', 'seam': topic, 'evidence': g.get('evidence') or '', 'file': os.path.basename(p)})
        if PLANT == 'no_open':
            continue
        for o in d.get('open') or []:
            t = o if isinstance(o, str) else (o.get('what') or o.get('text') or json.dumps(o)) if isinstance(o, dict) else str(o)
            rows.append({'kind': 'open', 'mark': 'open', 'where': 'seam ' + topic, 'fn': '', 'text': t, 'note': '', 'seam': topic, 'evidence': '',
                         'file': os.path.basename(p)})
    for r in rows:
        r['settle'] = settle_of(r['text'])
    return rows


def totals(rows):
    t = {'claim': {'C': 0, 'L': 0, 'U': 0}, 'game': {'C': 0, 'L': 0, 'U': 0}, 'open': 0}
    for r in rows:
        if r['kind'] == 'open':
            t['open'] += 1
        elif r['mark'] in t[r['kind']]:
            t[r['kind']][r['mark']] += 1
    return t


def totals_line(t):
    c, g = t['claim'], t['game']
    return (f"marks: claims C {c['C']} / L {c['L']} / U {c['U']}; seam game rows C {g['C']} / L {g['L']} / U {g['U']}; {t['open']} open items")


def select(rows, mark=None, seam=None, note=None, addr=None):
    out = []
    want = _hex(addr) if addr else None
    for r in rows:
        if mark:
            if r['kind'] == 'open':
                if mark != 'U':
                    continue
            elif r['mark'] != mark:
                continue
        if seam and r['seam'].lower() != seam.lower():
            continue
        if note and note.lower() not in (r['note'] + ' ' + r['file']).lower():
            continue
        if addr:
            if r['kind'] == 'open' or want is None or want not in (_hex(r['where']), _hex(r['fn'])):
                continue
        out.append(r)
    out.sort(key=lambda r: (-1 if r['kind'] == 'open' else ORDER.get(r['mark'], 3)))      # open items first: they are the unsettled questions
    return out


def cut(s, n, all_):
    s = ' '.join((s or '').split())
    if all_ or len(s) <= n:
        return s, 0
    return s[:n].rstrip() + ' ...', len(s) - n


def usage():
    return ('usage: q marks [--mark U|L|C] [--seam TOPIC] [--note NOTE] [--addr ADDR] [--json] [--all]   '
            '(the [U] / [L] rows of db/claims and db/seams, plus the seams\' open items; --mark U = every [U] row and open item)')


def main(argv, root=None):
    args, o, i = list(argv), {}, 0
    flags = {'--mark': 'mark', '--seam': 'seam', '--note': 'note', '--addr': 'addr'}
    j = False
    all_ = False
    while i < len(args):
        a = args[i]
        if a in ('-h', '--help'):
            print(usage())
            return 0
        if a == '--json':
            j = True
        elif a == '--all':
            all_ = True
        elif a in flags and i + 1 < len(args):
            o[flags[a]] = args[i + 1]
            i += 1
        elif a.split('=')[0] in flags and '=' in a:
            o[flags[a.split('=')[0]]] = a.split('=', 1)[1]
        else:
            print(f'q marks: unexpected {a!r}\n' + usage())
            return 2
        i += 1
    if o.get('mark'):
        o['mark'] = o['mark'].upper()
        if o['mark'] not in ORDER:
            print('q marks: --mark is U, L or C\n' + usage())
            return 2
    if o.get('addr') and _hex(o['addr']) is None:
        print(f"q marks: --addr {o['addr']!r} is not a hex address\n" + usage())
        return 2
    rows = collect(root)
    t = totals(rows)
    sel = select(rows, o.get('mark'), o.get('seam'), o.get('note'), o.get('addr'))
    if j:
        print(json.dumps({'totals': t, 'shown': len(sel), 'rows': sel}, indent=1))
        return 0
    print(totals_line(t))
    filt = ' '.join(f'--{k} {v}' for k, v in o.items())
    st = totals(sel)
    print(f"{len(sel)} row(s)" + (f' for {filt}' if filt else '') + f": claims U {st['claim']['U']} L {st['claim']['L']} C {st['claim']['C']}; "
          f"game rows U {st['game']['U']} L {st['game']['L']} C {st['game']['C']}; open {st['open']}")
    shown, n_cut = qcap.cap(sel, ROW_CAP, all_)
    ncut_text = 0
    for r in shown:
        tag = 'open' if r['kind'] == 'open' else r['mark']
        text, c1 = cut(r['text'], TEXT_CAP, all_)
        ncut_text += bool(c1)
        print(f"[{tag}] {r['kind']:5} {r['where'] or '-'}  seam={r['seam'] or '-'}  note={r['note'] or r['file']}")
        print(f'      {text}')
        if r['kind'] != 'open' or r['settle']:
            if r['settle']:
                s, c2 = cut(r['settle'], TEXT_CAP, all_)
                ncut_text += bool(c2)
                print(f'      settles: {s}')
            elif r['mark'] != 'C':
                ev, _ = cut(r['evidence'], TEXT_CAP, all_)
                print(f"      settles: (the row names nothing; its evidence: {ev or 'none'})")
    call = 'q marks ' + ' '.join(f'--{k} {v}' for k, v in o.items()) + (' ' if o else '') + '--all'
    if n_cut:
        print(qcap.trim_line(len(shown), len(sel), 'rows', call, narrow=False) + ', or narrow with --mark / --seam / --note / --addr')
    elif ncut_text:
        print(f'({ncut_text} text(s) cut at {TEXT_CAP} characters; the full text: {call})')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

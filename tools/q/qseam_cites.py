r"""q seam --cites FILE[:LINE] (S111 QGAPS): which seam rows and which db/claims rows cite a file, or one line of it.

    py tools/q/q.py seam --cites guest_model_draw.h            every row that names the file
    py tools/q/q.py seam --cites src/renderer_scene/guest_model_draw.h:364   rows that name that line (a cited range 364-365 holds it)
    py tools/q/q.py seam --cites FILE:LINE --json [--all]

Two ways a row cites a file:
  - an `ours` row of db/seams/<topic>.json whose `path` is the file (with LINE: the stored line, or the line the anchor holds NOW, is LINE);
  - a `file:line` (or `file:a-b`) written in the text of a seam row (what / evidence / source / why / summary / open) or of a db/claims/*.json row
    (claim / evidence / note / ours / source). A cite with no line number answers only the file-wide question (no LINE).
FILE matches by whole path components: `guest_model_draw.h` or `renderer_scene/guest_model_draw.h` or the full path; a cite of the bare basename
matches a longer FILE. Nothing is read but db/seams and db/claims (our own annotations).
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

CITE_RX = re.compile(r'([A-Za-z0-9_./\\-]+\.[A-Za-z][A-Za-z0-9]{0,4})(?::(\d+)(?:-(\d+))?)?')
SEAM_TEXT = ('what', 'evidence', 'source', 'why')
CLAIM_TEXT = ('claim', 'evidence', 'note', 'source', 'ours', 'why')


class CitesError(Exception):
    pass


def norm(p):
    return p.replace('\\', '/').strip('/').lower()


def parse_spec(spec):
    m = re.fullmatch(r'(.+?)(?::(\d+))?', spec.strip())
    if not m or not m.group(1):
        raise CitesError(f'{spec}: give FILE or FILE:LINE')
    return norm(m.group(1)), (int(m.group(2)) if m.group(2) else None)


def same_file(cite, want):
    c = norm(cite)
    return c == want or c.endswith('/' + want) or want.endswith('/' + c)


def text_cites(text, want, line):
    """-> ['file:a-b', ...] the cites in `text` of the wanted file (and line, when one is asked)"""
    out = []
    for m in CITE_RX.finditer(text or ''):
        if not same_file(m.group(1), want):
            continue
        a = int(m.group(2)) if m.group(2) else None
        b = int(m.group(3)) if m.group(3) else a
        if line is not None and (a is None or not (a <= line <= max(a, b or a))):
            continue
        out.append(m.group(0))
    return out


def _load(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def find_cites(root, spec):
    import qseam
    want, line = parse_spec(spec)
    hits = []
    for p in sorted(glob.glob(os.path.join(root, 'db', 'seams', '*.json'))):
        e = _load(p)
        if not isinstance(e, dict):
            continue
        topic = e.get('topic') or os.path.splitext(os.path.basename(p))[0]
        ours = e.get('ours') or []
        for i, o in enumerate(ours):
            if isinstance(o, dict) and same_file(str(o.get('path', '')), want):
                st = qseam.row_state(o, root, ours)
                stored, now = o.get('line'), st.get('line')
                if line is None or line in (stored, now):
                    hits.append({'kind': 'seam-ours', 'where': f'db/seams/{topic}.json ours[{i}]', 'topic': topic, 'cite': f'{o.get("path")}:{stored}',
                                 'stored': stored, 'now': now, 'state': st.get('state'), 'verdict': o.get('verdict'), 'why': o.get('why', '')})
        for side in ('game', 'ours'):
            for i, r in enumerate(e.get(side) or []):
                if not isinstance(r, dict):
                    continue
                cs = [c for k in SEAM_TEXT for c in text_cites(str(r.get(k, '')), want, line)]
                if cs:
                    hits.append({'kind': f'seam-{side}-text', 'where': f'db/seams/{topic}.json {side}[{i}]', 'topic': topic, 'cite': ' '.join(sorted(set(cs))),
                                 'addr': r.get('addr'), 'what': r.get('what') or r.get('why', '')})
        cs = [c for k in ('summary',) for c in text_cites(str(e.get(k, '')), want, line)] + [c for t in (e.get('open') or []) for c in text_cites(str(t), want, line)]
        if cs:
            hits.append({'kind': 'seam-summary-text', 'where': f'db/seams/{topic}.json summary / open', 'topic': topic, 'cite': ' '.join(sorted(set(cs)))})
    for p in sorted(glob.glob(os.path.join(root, 'db', 'claims', '*.json'))):
        rows = _load(p)
        if not isinstance(rows, list):
            continue
        rel = 'db/claims/' + os.path.basename(p)
        for i, r in enumerate(rows):
            if not isinstance(r, dict):
                continue
            cs = [c for k in CLAIM_TEXT for c in text_cites(str(r.get(k, '')), want, line)]
            if cs:
                hits.append({'kind': 'claim', 'where': f'{rel} [{i}]', 'cite': ' '.join(sorted(set(cs))), 'addr': r.get('address'), 'seam': r.get('seam'),
                             'mark': r.get('mark'), 'what': r.get('claim', '')})
    return {'spec': spec, 'file': want, 'line': line, 'n': len(hits), 'hits': hits}


def print_cites(r, limit=40, all_=False):
    import qcap
    print(f'seam --cites {r["spec"]}: {r["n"]} rows cite it')
    shown, cut = qcap.cap(r['hits'], limit, all_)
    for h in shown:
        if h['kind'] == 'seam-ours':
            mv = f', now line {h["now"]}' if h['now'] and h['now'] != h['stored'] else ''
            print(f'  {h["where"]}  ours row {h["cite"]} [{h["verdict"]}, {h["state"]}{mv}]  {h["why"][:110]}')
        elif h['kind'].startswith('seam-'):
            print(f'  {h["where"]}  cites {h["cite"]}' + (f'  {h["addr"]}' if h.get('addr') else '') + (f'  {h.get("what", "")[:90]}' if h.get('what') else ''))
        else:
            print(f'  {h["where"]}  cites {h["cite"]}  [{h.get("mark")}] {h.get("addr")}' + (f' seam={h["seam"]}' if h.get('seam') else '') + f'  {h.get("what", "")[:90]}')
    if cut:
        print('  ' + qcap.trim_line(len(shown), r['n'], 'rows', f'q seam --cites {r["spec"]} --all', narrow=False))


def run(a, root):
    """qseam.main hands over here for --cites (a.cites = the spec)"""
    try:
        r = find_cites(root, a.cites)
    except CitesError as e:
        print(f'q seam --cites: {e}', file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print_cites(r, all_=getattr(a, 'all', False))
    return 0

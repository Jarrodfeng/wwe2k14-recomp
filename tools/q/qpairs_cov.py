r"""q pairs --coverage (S111 PAIRCOV): which of OUR state-setting sites the code diff can compare, and the best pairing candidates
of the ones it cannot.

    py tools\q\q.py pairs --coverage [--json] [--all] [--no-game]

The sites are `q ostate`'s (tools/q/qostate.py collect): each DrawState block (one draw), each pass_state kTable row, each
InstancedDraw caller store. A site is PAIRED when a source `q codediff` counts stands for it:
  block   an rstdiff kind it is read under (qrstdiff KINDS: the kind's game windows' functions), or a PRECISE MIRRORS `q pairs` row of its
          function (comment / code / claim / seam-why): every row of a one-block function; in a function of several blocks only the
          rows on the block's own lines (its start to its last fact); in a block whose facts serve several rstdiff kinds only a row on
          the block's start (or the line above) or on a fact's line (or the line above): codediff's block_sources / cite_scope rules.
  store   (a bind record's, S111 CDARMS) one site per if-arm, the rows in scope of that arm (tools/q/qcdarms.py arm_scope).
  table   its `addr` cell names a game function (qostate's `game` list).
  store   its function has a precise `q pairs` row (codediff compares a caller's stores at its own pairs).
A paired site whose every game function has no `q gstate --facts` fact is PAIRED-NO-FACTS (the diff has nothing to compare it with;
--no-game skips the gstate read and counts it paired). A block in qrstdiff.OURS_ONLY (no game draw) is OURS-ONLY; a site whose every
fact is a field codediff never compares (no_blend, the tick-blend tag) is NEVER-COMPARED. The rest are UNPAIRED, each with its
candidates from evidence that already exists, each with its evidence and a confidence:
  [C] addr-cell   a kTable row's addr cell names a `sub_X` the row's `game` list does not carry (qostate.table_facts reads only `0x..`)
  [L] kind-class  an rstdiff kind whose `classes` name the row: the kind's game windows draw that class (the row's literals may come
                  from a census, not from that function)
  [L] fn-cite     a precise q pairs row of the block's function outside the block's own lines (another block's citation)
  [L] seam        a topic-level `seam` row of the function (every game row of a topic against every ours row)
  [L] assume      a `q assume` row (out/sweep/assume.json, kinds A-G) within 3 lines of the site carrying a game address
  [L] doc         a line of docs/systems/render_categories.md naming the site's family (kind / row name) and a game address
Only a [C] candidate may be written as a pair (a citation at the site / the cell's 0x form); an [L] one stays listed.
OUTSIDE: the DrawState field stores (`.depth_test =`, `.cull =`, `.blend[0].x =` ...) of src/renderer_scene, src/renderer_host and
src/renderer_adapter (tests left out) on no line q ostate read: draws of ours the code diff cannot see at all (q ostate's reach,
not a pair), grouped by function.
"""
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

PRECISE = ('comment', 'code', 'claim', 'seam-why')
NEVER = ('no_blend',)
STATES = ('PAIRED', 'PAIRED-NO-FACTS', 'UNPAIRED', 'OURS-ONLY', 'NEVER-COMPARED')
OUTSIDE_DIRS = ('src/renderer_scene', 'src/renderer_host', 'src/renderer_adapter')
STORE_RE = re.compile(r'\.\s*(depth_test|depth_write|depth_compare|cull|stencil_test|blend\[0\]\.(?:enable|src_color|dst_color|color_op|write_mask)'
                      r'|blend\[0\])\s*=(?!=)')
SUB_RE = re.compile(r'\bsub_(8[0-9A-Fa-f]{7})\b')
ADDR_RE = re.compile(r'\b(?:sub_|0x)(8[23][0-9A-Fa-f]{6})\b')
ASSUME_NEAR = 3
DOC = 'docs/systems/render_categories.md'
# selftest only: WWE_QPAIRSCOV_PLANT=any_fn_cite pairs a block of a several-block function with every citation of the function
# (codediff's block_sources rule dropped)
PLANT = os.environ.get('WWE_QPAIRSCOV_PLANT', '')


def _ln(at):
    rel, _, n = str(at).rpartition(':')
    return rel, int(n) if n.isdigit() else 0


def kind_of(label):
    return label.split(' (')[0]


# ------------------------------------------------------------------ the sites
def sites_of(facts):
    """q ostate facts -> [site]: one per DrawState block, kTable row, InstancedDraw store (in fact order)"""
    by = collections.OrderedDict()
    for f in facts:
        if f['kind'] == 'table':
            key, typ = ('table', f['fn']), 'table'
        elif f.get('block'):
            key, typ = ('block', f['fn'], f['block']), 'block'
        elif f['kind'] in ('caller', 'bind'):   # (S111 OSTATEWIDE: a bind record's store, qostate_wide.bind_facts; S111 CDARMS: per if-arm)
            key, typ = ('store', f['fn'], f.get('arm') or f['at']), 'store'
        else:
            key, typ = ('other', f['fn'], f['at']), 'other'
        s = by.get(key)
        if s is None:
            at = f.get('block') or f['at']
            s = by[key] = {'type': typ, 'fn': f['fn'], 'at': at, 'block': f.get('block'), 'facts': [], 'kinds': [], 'game': []}
        s['facts'].append(f)
        for k in f.get('kinds', ()):
            if k not in s['kinds']:
                s['kinds'].append(k)
        for g in f.get('game', ()):
            if g not in s['game']:
                s['game'].append(g)
        if f.get('addr') and 'addr' not in s:
            s['addr'] = f['addr']
    out = list(by.values())
    for s in out:
        s['fields'] = sorted({f['field'] for f in s['facts']})
        s['lines'] = sorted({_ln(f['at'])[1] for f in s['facts']} | ({_ln(s['block'])[1]} if s['block'] else set()))
    return out


def kinds_games():
    """{kind: [sub_X]}: qrstdiff KINDS' game window functions"""
    import qrstdiff as R
    out = collections.defaultdict(list)
    for k in R.KINDS:
        for w in k['game']:
            g = f"sub_{w['fn']:08X}"
            if g not in out[k['kind']]:
                out[k['kind']].append(g)
    return dict(out)


def kind_classes():
    """{class row name: [(kind, [sub_X])]}: the rstdiff kinds that name a pass class"""
    import qrstdiff as R
    out = collections.defaultdict(list)
    for k in R.KINDS:
        for c in k.get('classes') or ():
            out[c].append((k['kind'], list(dict.fromkeys(f"sub_{w['fn']:08X}" for w in k['game']))))
    return dict(out)


def _block_rows(site, rows, n_blocks, passes=None):
    """the precise rows of the function that stand for one site: q codediff's own rule (qcodediff.cite_scope over block_sources), so
    coverage and the diff never disagree on what a citation stands for"""
    import qcodediff as CDF
    if PLANT == 'any_fn_cite':
        return list(rows)
    by = {f"{r['src']} {r['file']}:{r['line']}": r for r in rows}
    facts = [f for f in site['facts'] if f['field'] not in NEVER]
    if any(f.get('arm') for f in facts):   # S111 CDARMS: a bind arm takes the rows in its own scope (qcdarms.arm_scope)
        import qcdarms as A
        return [r for r in rows if A.arm_scope('sub_' + r['game'], [f"{r['src']} {r['file']}:{r['line']}"], facts, passes)]
    kept = CDF.cite_scope(list(by), site['block'], facts, n_blocks, site['kinds'])
    return [by[s] for s, _fl in kept if s in by]


def judge(sites, pair_rows, kgames, ours_only=(), have_game=None, passes=None):
    """sets each site's `state` and `pairs` [(sub_X, source)]; pair_rows = {fn: [q pairs row dict]}; have_game = the game functions
    with q gstate facts (None: not read)"""
    n_blocks = collections.Counter(s['fn'] for s in sites if s['type'] == 'block')
    skip = set(ours_only)
    for s in sites:
        pairs = []
        if s['type'] == 'table':
            pairs = [(g, 'kTable addr') for g in s['game']]
        else:
            for lab in s['kinds']:
                pairs += [(g, f'rstdiff {kind_of(lab)}') for g in kgames.get(kind_of(lab), ())]
            prec = [r for r in pair_rows.get(s['fn'], ()) if r['src'] in PRECISE and r['rel'] == 'MIRRORS']   # S111 PAIRMEAN: codediff's pairs
            for r in _block_rows(s, prec, n_blocks[s['fn']], passes):
                pairs.append(('sub_' + r['game'], f"{r['src']} {r['file']}:{r['line']}"))
        seen, s['pairs'] = set(), []
        for p in pairs:
            if p not in seen:
                seen.add(p)
                s['pairs'].append(p)
        if s['block'] and s['block'] in skip:
            s['state'] = 'OURS-ONLY'
        elif all(f['field'] in NEVER for f in s['facts']):
            s['state'] = 'NEVER-COMPARED'
        elif not s['pairs']:
            s['state'] = 'UNPAIRED'
        elif have_game is not None and not any(g in have_game for g, _src in s['pairs']):
            s['state'] = 'PAIRED-NO-FACTS'
        else:
            s['state'] = 'PAIRED'
    return sites


# ------------------------------------------------------------------ the candidates of an unpaired site
def candidates(s, pair_rows, kclasses, assume_rows, doc_lines):
    """-> [{game, conf, src, evidence}] for one site, [C] first"""
    out = []
    have = {g for g, _src in s.get('pairs', ())}

    def add(g, conf, src, ev):
        g = 'sub_' + g.upper().replace('SUB_', '').replace('0X', '')
        if g in have or any(c['game'] == g and c['src'] == src for c in out):
            return
        out.append({'game': g, 'conf': conf, 'src': src, 'evidence': ev})
    if s['type'] == 'table':
        name = re.fullmatch(r'pass_state::kTable\[(\w+)\]', s['fn'])
        cell = s.get('addr') or ''
        for a in SUB_RE.findall(cell):
            if 'sub_' + a.upper() not in s['game']:
                add(a, 'C', 'addr-cell', f'the row\'s own addr cell names sub_{a.upper()} ("{cell[:120]}"); qostate.table_facts reads only the 0x form')
        for kind, games in kclasses.get(name.group(1) if name else '', ()):
            for g in games:
                add(g, 'L', 'kind-class', f'rstdiff kind {kind} names class {name.group(1)}; {g} is one of its game windows')
    else:
        allp = pair_rows.get(s['fn'], ())
        for r in allp:
            if r['src'] in PRECISE:
                add(r['game'], 'L', 'fn-cite', f"{r['src']} {r['file']}:{r['line']} (the function's, outside this block): {r['text'][:110]}")
        for r in allp:
            if r['src'] == 'seam':
                add(r['game'], 'L', 'seam', f"seam row {r['file']}:{r['line']} (topic-level): {r['text'][:110]}")
    rel = _ln(s['at'])[0]
    lo, hi = min(s['lines']), max(s['lines'])
    for r in assume_rows:
        if r.get('file') != rel or r.get('kind') not in 'ABCDEFG' or not r.get('addrs'):
            continue
        if r.get('line', 0) > hi + ASSUME_NEAR or r.get('end', r.get('line', 0)) < lo - ASSUME_NEAR:
            continue
        for a in r['addrs']:
            m = re.fullmatch(r'(?:sub_|0x)(8[0-9A-Fa-f]{7})', a)
            if m and a.startswith('sub_'):
                add(m.group(1), 'L', 'assume', f"q assume {r.get('sub') or r['kind']} {rel}:{r['line']}: {str(r.get('text', ''))[:100]}")
    words = {kind_of(k) for k in s['kinds']} | ({m.group(1)} if (m := re.fullmatch(r'pass_state::kTable\[(\w+)\]', s['fn'])) else set())
    for ln, text in doc_lines:
        if any(re.search(r'(?<![\w])' + re.escape(w) + r'(?![\w])', text) for w in words):
            for a in ADDR_RE.findall(text):
                add(a, 'L', 'doc', f'{DOC}:{ln}: {text.strip()[:110]}')
    out.sort(key=lambda c: (c['conf'] != 'C', c['src']))
    return out


# ------------------------------------------------------------------ outside q ostate's reach
def outside(facts, root, rels=None, aside=()):
    """-> {(file, fn): [line]} DrawState field stores on no line q ostate read (OUTSIDE_DIRS, tests left out) and set aside by no
    reason of qostate_wide (aside: its counts['aside'] [(file, line, reason)])"""
    import qostate as O
    import qconstdiff as CD
    seen = {_ln(f['at']) for f in facts} | {(rel, ln) for rel, ln, _why in aside}
    if rels is None:
        rels = []
        for d in OUTSIDE_DIRS:
            for dp, dn, fn in os.walk(os.path.join(root, d.replace('/', os.sep))):
                dn[:] = [x for x in dn if not x.startswith('.') and x != '__pycache__']
                for n in fn:
                    if n.endswith(('.cpp', '.h', '.inc')) and 'test' not in n.lower():
                        rels.append(os.path.relpath(os.path.join(dp, n), root).replace(os.sep, '/'))
    out = collections.OrderedDict()
    for rel in sorted(rels):
        try:
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as fh:
                text = fh.read()
        except OSError:
            continue
        if not STORE_RE.search(text):
            continue
        t = CD._mask(text)
        src = O.TextSrc(rel, text)
        for m in STORE_RE.finditer(t):
            ln = O.line_of(t, m.start())
            if (rel, ln) in seen:
                continue
            out.setdefault((rel, src.name(ln)), []).append(ln)
    return out


# ------------------------------------------------------------------ the inputs
def assume_rows(root):
    for base in (root, _main_checkout(root)):
        p = os.path.join(base or '', 'out', 'sweep', 'assume.json')
        if base and os.path.exists(p):
            try:
                with open(p, encoding='utf-8') as fh:
                    return json.load(fh).get('rows', []), p
            except (OSError, ValueError):
                pass
    return [], None


def _main_checkout(root):
    r = subprocess.run(['git', 'rev-parse', '--git-common-dir'], cwd=root, capture_output=True, text=True)
    if r.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, r.stdout.strip())))


def doc_lines(root):
    try:
        with open(os.path.join(root, DOC), encoding='utf-8', errors='replace') as fh:
            return [(i, ln) for i, ln in enumerate(fh.read().split('\n'), 1) if ADDR_RE.search(ln)]
    except OSError:
        return []


def pair_rows_for(fns, root=None):
    import qpairs
    con, _info = qpairs.update(root)
    try:
        return {fn: [qpairs._rowd(r) for r in qpairs.by_ours(con, fn)] for fn in fns if not fn.startswith('pass_state::')}
    finally:
        con.close()


def game_have():
    import q as Q
    import qgstate
    con = Q.connect(Q.DB)
    idx = qgstate.load_index(con, log=lambda m: print(m, file=sys.stderr))
    return {f['fn'] for f in qgstate.facts(idx)}


def run(facts, root, pair_rows, kgames, kclasses, ours_only, have_game, assume, docs, outside_rels=None, aside=(), passes=None):
    """-> report dict: the sites with state / pairs / candidates, the counts, the outside stores"""
    sites = judge(sites_of(facts), pair_rows, kgames, ours_only, have_game, passes)
    for s in sites:
        s['candidates'] = candidates(s, pair_rows, kclasses, assume, docs) if s['state'] in ('UNPAIRED', 'PAIRED-NO-FACTS') else []
    cnt = collections.Counter(s['state'] for s in sites)
    by_type = collections.Counter(s['type'] for s in sites)
    out = outside(facts, root, outside_rels, aside)
    return {'sites': sites, 'counts': {st: cnt.get(st, 0) for st in STATES}, 'types': dict(by_type),
            'outside': [{'file': rel, 'fn': fn, 'lines': lns} for (rel, fn), lns in out.items()],
            'aside': [{'file': rel, 'line': ln, 'why': why} for rel, ln, why in aside]}


def collect(root=None, no_game=False):
    import qcdarms as A
    import qostate as O
    import qrstdiff as R
    root = os.path.abspath(root) if root else O.REPO
    facts, problems, counts = O.collect(None)
    fns = sorted({f['fn'] for f in facts})
    skip, p2 = R.ours_only(O.default_reader(None))
    problems += p2
    assume, apath = assume_rows(root)
    have = None if no_game else game_have()
    rep = run(facts, root, pair_rows_for(fns, root), kinds_games(), kind_classes(), {f'{rel}:{ln}' for rel, ln, _w in skip},
              have, assume, doc_lines(root), aside=counts.get('aside', ()), passes=A.pass_techniques(O.default_reader(None)))
    rep['problems'] = problems
    rep['assume'] = apath
    rep['game_facts'] = not no_game
    return rep


def _strip(s):
    return {k: v for k, v in s.items() if k not in ('facts',)}


def print_report(rep, show_all=False):
    c, n = rep['counts'], len(rep['sites'])
    t = rep['types']
    compared = c['PAIRED']
    print(f"q pairs --coverage: {n} state-setting sites of ours (q ostate: {t.get('block', 0)} DrawState blocks, {t.get('table', 0)} kTable rows, "
          f"{t.get('store', 0)} InstancedDraw stores); paired {compared} of {n}" + (' (with q gstate facts)' if rep.get('game_facts') else ' (gstate not read)')
          + f"; paired, no game facts {c['PAIRED-NO-FACTS']}; unpaired {c['UNPAIRED']}; ours-only {c['OURS-ONLY']}; never compared {c['NEVER-COMPARED']}")
    for s in rep['sites']:
        if s['state'] == 'PAIRED' and not show_all:
            continue
        games = sorted({g for g, _x in s['pairs']})
        print(f"{s['state']:16s} {s['type']:5s} {s['fn']}  @{s['at']}  [{', '.join(s['fields'])}]"
              + (f"  kinds {', '.join(s['kinds'])}" if s['kinds'] else '') + (f"  pairs {', '.join(games[:6])}" if games else ''))
        for cd in s.get('candidates', ()):
            print(f"    [{cd['conf']}] {cd['game']}  {cd['src']}: {cd['evidence']}")
        if s['state'] in ('UNPAIRED', 'PAIRED-NO-FACTS') and not s.get('candidates'):
            print('    (no candidate from the evidence read: kTable cell, rstdiff kinds, q pairs rows, q assume, render_categories.md)')
    if not show_all and compared:
        print(f'  ({compared} PAIRED sites not shown: --all)')
    o = rep['outside']
    print(f"OUTSIDE q ostate: {sum(len(x['lines']) for x in o)} DrawState field stores in {len(o)} functions on no line q ostate reads "
          f"({', '.join(OUTSIDE_DIRS)}; tests left out): draws the code diff cannot see (ostate's reach, not a pair)")
    for x in (o if show_all else o[:25]):
        print(f"    {x['file']}  {x['fn']}  lines {', '.join(map(str, x['lines'][:8]))}" + (f" (+{len(x['lines']) - 8})" if len(x['lines']) > 8 else ''))
    if not show_all and len(o) > 25:
        print(f'    ... {len(o) - 25} more (--all)')
    ad = rep.get('aside', ())
    print(f"SET ASIDE by q ostate's wide reader (tools/q/qostate_wide.py): {len(ad)} stores of a DrawState field name that are not our DrawState state")
    for x in ad:
        print(f"    {x['file']}:{x['line']}  {x['why']}")
    if not rep.get('assume'):
        print('  (out/sweep/assume.json not found here or in the main checkout: no q assume candidates; `q assume` writes it)')
    for p in rep.get('problems', ()):
        print('PROBLEM: ' + p)


def main(argv, root=None, show_all=False, as_json=False, no_game=False):
    rep = collect(root, no_game)
    if as_json:
        print(json.dumps({**rep, 'sites': [_strip(s) for s in rep['sites']]}, indent=1, default=str))
    else:
        print_report(rep, show_all)
    return 2 if rep['problems'] else 0

r"""qwhen: WHEN a game function runs (S111 Lane C WHEN, docs\research\s111\INDEX_PLAN.md T3). No game run; computed from the index, never hand-written per function.

    py tools/q/q.py when FUNC [--json]     which task / frame phase / draw walk and category reaches FUNC, the frame order, the thread, the path

It joins what the index already knows:
  * the TASKS: the task table of `q order` (the `bl sub_82C307B0` registrations: update, group, registered-by) and the frame phases of db/order/frame.json;
  * the DRAW NODES: the `sub_82C30A10` registrations (`q nodes`: the category literal and the node's vtable slot 1) and, where a node's vtable is set by a base
    constructor in another function, the rows of docs/systems/render_categories.md (our words, read by CATTABLE; such an answer prints [L doc], never [C]);
  * the SIDE WALKS of that doc's passes table (map A / map B / rim / re-draw ...), which walk a category from inside another node;
  * the CALL GRAPH (direct edges = listing calls [C]; OBSERVED indirect pairs = [L seen in N runs]): the callers are followed up to the first root of any of
    those kinds (the path printed), then the root's own place (a node's walk `sub_82C2FA10` is itself located: its frame phase);
  * the THREAD: the callers followed up to a function the db names `*thread_main|loop|entry|proc` / `*_io_thread` (a name, so [L]; spawners `start_*` / `create_*` /
    `*spawn*` and `*_for_current_thread` helpers are not thread entries); the task runner's thread for a task / phase first, then EVERY other thread entry found
    above the function itself (a task / phase also climbs through the runner, a node slot 1 through the walk): the walk is also called by the CParallelDraw
    threads (sub_82C3B640 +B8 -> sub_82C30518), so a draw node prints `+1` (review S111 Lane C).
  * a root found first does not hide the rest: the direct callers not on a printed path are counted (`N other direct callers not followed`).
Limits (printed with the answer): an indirect call never observed is not followed; a function reached by no root says `no root found`, never a guess;
the period (every frame / per load step / construction only) is only said for a task update and a frame phase (the runner calls them every frame); the
category order inside the walk is ascending (render_categories.md, CAT33).

PROVIDER (the card): qfactcard registers `when` for decomp / asm; one `[when]` header line, at most CARD_CHARS characters (the card's 160 less its `; [when] `).
Selftest: py -I tools\q\qwhen_selftest.py
"""
import argparse
import contextlib
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

RUNNER = 0x82C302A0                         # the task runner: slot 1 of every enabled task at +120
WALK = 0x82C2FA10                           # the colour walk: the categories in ascending order (render_categories.md, CAT33)
DOC = os.path.join(HERE, '..', '..', 'docs', 'systems', 'render_categories.md')
WALK_DEPTH, WALK_CAP, MAX_ROOTS = 12, 6000, 4
THREAD_RE = re.compile(r'thread_(main|loop|entry|proc)$|_io_thread$')
NOT_THREAD_RE = re.compile(r'^(g_|start_|create_)|spawn|_for_current_thread$')      # a spawner / helper / data word named *_thread is not a thread body
CARD_CHARS = 160 - len('; [when] ')         # qfactcard.HEADER_CHARS less the prefix qfactcard._fmt puts before the text
CAT_HEAD = ('Cat', 'What registers there', 'Reset', 'State', 'Passes', 'Re-keys', 'Ours', 'Marks')      # the columns parse_doc reads by index
PASS_HEAD = ('Pass', 'Function', 'Categories')
SUB = re.compile(r'sub_([0-9A-Fa-f]{8})')
PASS_TAG = {'props map': 'pP', 'map A': 'pA', 'map B': 'pB', 'rim map': 'rim', 'far layer': 'far', 're-draw': 'redraw', 'circle blobs': 'blobs'}
PASS_RE = re.compile(r'\b(C|pA|pB|pP|rim|far|redraw)\b')


def fn(a):
    return f'sub_{a:08X}'


def clip(s, n):
    return s if len(s) <= n else s[:n - 3] + '...'


# ------------------------------------------------------------------ the doc (render_categories.md)
def parse_cats(s):
    """'13..21' -> [13..21]; '4, 5' -> [4, 5]; anything else -> []"""
    out = []
    for part in re.split(r',\s*', s.strip()):
        m = re.fullmatch(r'(\d+)\.\.(\d+)', part)
        if m:
            out += list(range(int(m.group(1)), int(m.group(2)) + 1))
        elif part.isdigit():
            out.append(int(part))
        else:
            return []
    return out


def parse_doc(text):
    """-> {'slot1': {func: [row]}, 'named': {func: [row]}, 'side': {func: {...}}}. A row = {'cats', 'spec', 'passes', 'mark'}.
    slot1 = the doc names the function as a node's slot 1 (`slot 1 `sub_S`` or `` `sub_B` (`sub_S`) ``); named = any other mention in the builder column
    (a constructor registers there: it is not drawn there)."""
    out = {'slot1': {}, 'named': {}, 'side': {}, 'headers': [], 'cats_seen': set(), 'rows': []}
    for line in text.replace('\\|', '\x01').split('\n'):
        if not line.startswith('|'):
            continue
        cells = [c.strip().replace('\x01', '|') for c in line.strip().strip('|').split('|')]
        if cells and cells[0] in (CAT_HEAD[0], PASS_HEAD[0]):
            out['headers'].append(cells)
        if len(cells) >= 8 and parse_cats(cells[0].replace('`', '')):
            cats = parse_cats(cells[0].replace('`', ''))
            out['cats_seen'] |= set(cats)
            row = {'cats': cats, 'spec': cells[0], 'passes': PASS_RE.findall(cells[4]), 'mark': 'C' if '[C]' in cells[7] else 'L',
                   'reset': cells[2], 'rekeys': cells[5]}     # read by q gstate --inherit (qgstate_inherit.categories)
            out['rows'].append(row)
            b = cells[1]
            s1 = {m.group(1) for m in re.finditer(r'slot 1 `sub_([0-9A-Fa-f]{8})`', b)}
            s1 |= {m.group(1) for m in re.finditer(r'`sub_[0-9A-Fa-f]{8}` \((?:[^`()]*, )?`sub_([0-9A-Fa-f]{8})`\)', b)}
            allf = {m.group(1) for m in SUB.finditer(b)}
            for h in s1:
                out['slot1'].setdefault(int(h, 16), []).append(row)
            for h in allf - s1:
                out['named'].setdefault(int(h, 16), []).append(row)
        elif len(cells) >= 4 and cells[0] in PASS_TAG:
            m = SUB.search(cells[1])
            if m:
                out['side'][int(m.group(1), 16)] = {'name': cells[0], 'tag': PASS_TAG[cells[0]], 'cats_text': clip(cells[2].replace('`', ''), 110)}
    return out


def layout_problems(doc, full=False):
    """-> [problem] when render_categories.md no longer has the layout parse_doc reads by column index (a moved / inserted column would silently read the
    wrong cell); full = also every category 0..70 parsed and every passes-table walk found (the real doc)"""
    bad = []
    for head, what in ((CAT_HEAD, 'category'), (PASS_HEAD, 'passes')):
        hs = [h for h in doc.get('headers', []) if h[0] == head[0]]
        if not hs:
            bad.append(f'no {what} table header `| {head[0]} | ...`')
        for h in hs:
            if (head is CAT_HEAD and len(h) != len(head)) or len(h) < len(head) or any(not h[i].startswith(c) for i, c in enumerate(head)):
                bad.append(f"the {what} table header is `{' | '.join(h)}`, parse_doc reads the columns {', '.join(head)}")
    if full:
        miss = sorted(set(range(71)) - doc.get('cats_seen', set()))
        if miss:
            bad.append(f'categories with no parsed row: {cat_text(miss)}')
        tags = {v['name'] for v in doc['side'].values()}
        if set(PASS_TAG) - tags:
            bad.append(f"passes-table walks not parsed: {', '.join(sorted(set(PASS_TAG) - tags))}")
    return bad


# ------------------------------------------------------------------ the world (the sources; the selftest plants its own)
class World:
    """names {addr: name}; callers(a) -> [(src, site, how)]; tasks {update: {'group','where','reg'}}; phases {func: {'id','what','src','where'}};
    nodes {slot1: [cats]} (index literals); doc = parse_doc(...); waits {func: text}"""

    def __init__(self, names, callers, tasks, phases, nodes, doc, waits=None, runner=RUNNER, walk=WALK, slots=None):
        self.slots = slots      # slots(func) -> [row] of qwhen_slots: the vtable words that hold func (None = no vtable step)
        self.names, self.callers, self.tasks, self.phases = names, callers, tasks, phases
        self.nodes, self.doc, self.waits, self.runner, self.walk = nodes, doc, waits or {}, runner, walk
        self._thr = {}

    def roots(self):
        r = set(self.tasks) | set(self.phases) | set(self.nodes) | set(self.doc['slot1']) | set(self.doc['side']) | {self.walk}
        return r


def load_world(con=None):
    """the real sources: the index, the task table, db/order/frame.json, the node registry, the doc"""
    import qorder
    import qx7
    icon = qx7._index_con()
    t = qorder.table(False, con)
    facts = qorder.frame_facts()
    try:
        import qseen
        scon = qseen.open_seen()
    except Exception:      # noqa: BLE001 - no observed pairs: direct edges only
        scon = None

    memo = {}

    def callers(a):
        if a in memo:
            return memo[a]
        ins = [(r[0], r[1], r[2]) for r in icon.execute("SELECT DISTINCT src, site, kind FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (a,))]
        if scon is not None:
            ins += [(r[0], r[1], 'seen in %d runs' % r[2]) for r in scon.execute('SELECT func, site, runs FROM seen WHERE target = ?', (a,))]
        memo[a] = ins
        return ins

    tasks = {}
    for k in t['by_update']:
        u = int(k)
        gs, rows, _ann = qorder.groups_of_update(u, t, facts)
        pos = qorder.position(u, t, facts, [{'update': u, 'chain': []}])
        regs = sorted({(r['func'], r['site']) for r in rows})
        tasks[u] = {'group': re.sub(r'^\[([\d, ]+)\]', r'\1', qorder.group_text(u, t, facts)), 'where': qorder.where(pos, facts),
                    'reg': '; '.join(f'{fn(f)} +{s - f:X}' for f, s in regs[:2]) + (' ...' if len(regs) > 2 else '')}
    phases = {}
    for i, p in enumerate(facts['phases']):
        fs = p.get('func') or []
        for f in (fs if isinstance(fs, list) else [fs]):
            phases[qorder.addr(f)] = {'id': p['id'], 'what': p['what'], 'src': p['src'], 'where': p['id'], 'pos': f"{i + 1} of {len(facts['phases'])}"}
    waits = {qorder.addr(w['func']): f"[{w['mark']}] {w['what']}" for w in facts.get('waits', [])}
    nodes = {}
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        qx7.nodes_main(['--json', '--limit', '5000'], icon)
    for n in json.loads(buf.getvalue())['nodes']:
        cats = [n['category']] if n['category'] is not None else (n['category_from_callers'] or [])
        if n['slot1'] and cats:
            a = int(n['slot1'][4:], 16)
            nodes[a] = sorted(set(nodes.get(a, [])) | set(cats))
    try:
        with open(DOC, encoding='utf-8') as f:
            doc = parse_doc(f.read())
    except OSError:
        doc = {'slot1': {}, 'named': {}, 'side': {}, 'headers': [], 'cats_seen': set()}
    names = {k: v for k, v in qx7._db_names().items()}
    import qwhen_slots
    return World(names, callers, tasks, phases, nodes, doc, waits, slots=qwhen_slots.make_slots(icon))


_WORLD = []


def world():
    if not _WORLD:
        _WORLD.append(load_world())
    return _WORLD[0]


# ------------------------------------------------------------------ the walk up the callers
def climb(w, a, roots, exclude=None, extra=None):
    """-> [(root, chain)] the roots found at the smallest depth; chain = [(src, site, how, dst)] from the root down to a (an empty chain: a is itself a root);
    extra(x) = one more root test (the vtable-slot step: a function a table word points at)"""
    rs = roots - {exclude} if exclude is not None else roots
    if a in rs or (extra and extra(a)):
        return [(a, [])]
    prev, frontier, found = {a: None}, [a], []
    for _d in range(WALK_DEPTH):
        if found or not frontier or len(prev) > WALK_CAP:
            break
        nxt = []
        for g in frontier:
            for src, site, how in w.callers(g):
                if src == w.runner or src in prev:
                    continue
                prev[src] = (g, site, how)
                (found if src in rs or (extra and extra(src)) else nxt).append(src)
        frontier = nxt
    out = []
    for r in found:
        chain, x = [], r
        while prev.get(x):
            g, site, how = prev[x]
            chain.append((x, site, how, g))
            x = g
        out.append((r, chain))
    return out


def chain_mark(chain):
    return 'C' if all(how in ('call', 'tail') for _, _, how, _ in chain) else 'L'


def chain_text(w, root, chain, limit=6):
    nm = lambda x: fn(x) + (f' {w.names[x][:28]}' if w.names.get(x) else '')      # noqa: E731
    parts = [nm(root)] + [nm(d) for _, _, _, d in chain]
    seen = [h for _, _, h, _ in chain if h not in ('call', 'tail')]
    if len(parts) > limit:
        parts = parts[:2] + ['...'] + parts[-(limit - 3):]
    return ' -> '.join(parts) + (f'  [{len(seen)} observed-indirect hop(s): {seen[0]}]' if seen else '')


def is_thread(w, x):
    n = w.names.get(x, '') or ''
    return bool(THREAD_RE.search(n)) and not NOT_THREAD_RE.search(n)


def all_threads(w, a):
    """-> ([(thread function, name, depth, mark)], capped): EVERY thread entry above `a`, not only the nearest; a task update / frame phase climbs on through
    the runner (the runner calls it by its slot), a node slot 1 through the walk (the walk calls it by its vtable). capped = the search hit its limits,
    so there may be more."""
    tr, nd = set(w.tasks) | set(w.phases), set(w.nodes) | set(w.doc['slot1'])
    if is_thread(w, a):
        return [(a, w.names[a], 0, 'L')], False
    seen, frontier, found, depth = {a}, [a], [], 0
    while frontier and depth < WALK_DEPTH + 4 and len(seen) <= WALK_CAP:
        depth += 1
        nxt = []
        for g in frontier:
            ups = [src for src, _s, _h in w.callers(g)] + ([w.runner] if g in tr else []) + ([w.walk] if g in nd else [])
            for src in ups:
                if src in seen:
                    continue
                seen.add(src)
                if is_thread(w, src):
                    found.append((src, w.names[src], depth, 'L'))
                else:
                    nxt.append(src)
        frontier = nxt
    return found, bool(frontier)


def threads_of(w, start):
    """-> [(thread function, name, depth, mark)] the thread entries (db names, is_thread) at the smallest depth above `start`"""
    if start in w._thr:
        return w._thr[start]
    isT = lambda x: is_thread(w, x)      # noqa: E731
    out = []
    if isT(start):
        out = [(start, w.names[start], 0, 'L')]
    else:
        prev, frontier, found, depth = {start: None}, [start], [], 0
        while frontier and not found and depth < WALK_DEPTH + 4 and len(prev) <= WALK_CAP:
            depth += 1
            nxt = []
            for g in frontier:
                for src, site, how in w.callers(g):
                    if src in prev:
                        continue
                    prev[src] = (g, site, how)
                    (found if isT(src) else nxt).append(src)
            frontier = nxt
        out = [(x, w.names[x], depth, 'L') for x in found]
    w._thr[start] = out
    return out


def locate(w, a, depth=0):
    """-> {'anchors': [anchor], 'threads': [...]} of a function: anchors = the roots found above it (kind task / phase / node / side / walk)"""
    anchors = []
    found = climb(w, a, w.roots(), exclude=w.walk if depth else None)
    slot_mode = not found and not depth and w.slots is not None      # S111 QFIX4: no task / phase / node above it: step to the vtable slot that holds a function on the way up
    if slot_mode:
        found = climb(w, a, w.roots(), extra=lambda x: bool(w.slots(x)))
    on_path = {chain[-1][0] for _r, chain in found[:MAX_ROOTS] if chain}
    # the walk calling a node slot 1 is that node's own place (printed), so it is not an untraced caller; an observed caller makes the count [L]
    skip = {w.runner} | ({w.walk} if any(r in w.nodes or r in w.doc['slot1'] for r, _c in found[:MAX_ROOTS]) else set())
    kinds = {}
    for src, _s, how in w.callers(a):
        if src not in skip and src not in on_path:
            kinds[src] = kinds.get(src, set()) | {how}
    untraced = [(src, 'C' if kinds[src] & {'call', 'tail'} else 'L') for src in sorted(kinds)]
    for root, chain in found[:MAX_ROOTS]:
        mk = chain_mark(chain)
        if slot_mode and root not in w.roots():
            anchors.append({'kind': 'slot', 'root': root, 'chain': chain, 'mark': mk, 'slots': w.slots(root)})
            continue
        if root in w.tasks:
            g = w.tasks[root]['group']
            anchors.append({'kind': 'task', 'root': root, 'chain': chain, 'mark': mk if '[U' not in g and g != '?' else 'L', 'info': w.tasks[root]})
        if root in w.phases:
            anchors.append({'kind': 'phase', 'root': root, 'chain': chain, 'mark': mk, 'info': w.phases[root]})
        if root in w.nodes or root in w.doc['slot1']:
            idx = w.nodes.get(root)
            rows = w.doc['slot1'].get(root, [])
            cats = sorted(set(idx or []) | {c for r in rows for c in r['cats']})
            passes = sorted({p for r in rows for p in r['passes'] if p != 'C'}, key=['pP', 'pA', 'pB', 'rim', 'far', 'redraw'].index) if rows else []
            an = {'kind': 'node', 'root': root, 'chain': chain, 'cats': cats, 'passes': passes, 'index_cats': idx or [], 'doc_spec': [r['spec'] for r in rows],
                  'doc_mark': rows[0]['mark'] if rows else None, 'mark': mk if idx and set(cats) <= set(idx) else 'L', 'src': 'index' if idx else 'doc'}
            if depth == 0:
                an['walk'] = locate(w, w.walk, 1)
            anchors.append(an)
        if root in w.doc['side']:
            anchors.append({'kind': 'side', 'root': root, 'chain': chain, 'mark': 'L', 'info': w.doc['side'][root]})
        if root == w.walk and not any(x['kind'] in ('node', 'side') for x in anchors if x['root'] == root):
            anchors.append({'kind': 'walk', 'root': root, 'chain': chain, 'mark': mk})
    return {'anchors': anchors, 'untraced': untraced, 'more_roots': max(len(found) - MAX_ROOTS, 0)}


def thread_start(w, loc, a):
    """the function whose callers lead to the thread: the runner for a task / phase, the walk's own place for a node, else the function itself"""
    for an in loc['anchors']:
        if an['kind'] in ('task', 'phase'):
            return w.runner
        if an['kind'] in ('node', 'side', 'walk'):
            for wa in (an.get('walk') or locate(w, w.walk, 1))['anchors']:
                if wa['kind'] in ('task', 'phase'):
                    return w.runner
            return w.walk
    return a


def compute(w, a):
    loc = locate(w, a)
    th = list(threads_of(w, thread_start(w, loc, a)))
    every, capped = all_threads(w, a)
    th += [t for t in every if t[0] not in {x[0] for x in th}]      # the anchor's thread first, then every other thread entry above the function
    doc_named = w.doc['named'].get(a, [])
    return {'addr': a, 'name': w.names.get(a, ''), 'anchors': loc['anchors'], 'threads': th, 'threads_capped': capped, 'named': doc_named, 'wait': w.waits.get(a),
            'untraced': loc['untraced'], 'more_roots': loc['more_roots']}


# ------------------------------------------------------------------ the words
def cat_text(cats):
    if not cats:
        return '?'
    runs, s = [], cats[0]
    for i, c in enumerate(cats):
        if i + 1 == len(cats) or cats[i + 1] != c + 1:
            runs.append(str(s) if s == c else f'{s}..{c}')
            if i + 1 < len(cats):
                s = cats[i + 1]
    return ', '.join(runs)


def thread_text(th, capped=False):
    if not th:
        return 'thread ? (no function the db names *thread_main/loop/entry/proc above it within the walk limit)'
    first = f"thread {th[0][1]} (named {th[0][2]} callers up)" if th[0][2] else f'thread {th[0][1]} (itself)'
    if len(th) == 1 and not capped:
        return first
    rest = [t[1] for t in th[1:5]] + (['+%d more' % (len(th) - 5)] if len(th) > 5 else [])
    return (first + f"; reached from {len(th)}{' or more' if capped else ''} thread entries in all" + (f", also {', '.join(rest)}" if rest else '')
            + (' (the search stopped at its limit)' if capped else ''))


def lines(w, r):
    """-> [(tag, text)] the full answer"""
    out = []
    for an in r['anchors']:
        k, mk = an['kind'], an['mark']
        if k == 'task':
            i = an['info']
            out.append((mk, f"task {fn(an['root'])}: group {i['group']}, frame position `{i['where']}`, runs every frame while enabled ([task+8] != 0); registered by {i['reg'] or '?'}"))
        elif k == 'phase':
            i = an['info']
            out.append((mk, f"frame phase `{i['id']}`: the runner itself calls {fn(an['root'])} every frame ({i['src']})"))
        elif k == 'node':
            doc_only = an['src'] == 'doc'
            tag = 'L doc' if doc_only else mk
            out.append((tag, f"draw node: slot 1 {fn(an['root'])} of a node in render categories {cat_text(an['cats'])}: the colour walk {fn(w.walk)} calls it when its +8 is "
                             f"non-zero, categories ascending 0..70, no sort (render_categories.md; " + (f"the index has no literal category for this vtable; doc rows {', '.join(an['doc_spec'])}, doc mark {an['doc_mark']})"
                                                                                                           if doc_only else 'the `sub_82C30A10` registrations')))
            if an['passes']:
                out.append(('L doc', f"also walked from inside another node by the side walks {' '.join(an['passes'])} (render_categories.md passes table)"))
            for wa in (an.get('walk') or {'anchors': []})['anchors'][:2]:
                if wa['kind'] in ('phase', 'task'):
                    out.append((wa['mark'], f"the walk itself: {chain_text(w, wa['root'], wa['chain'])} = " + (f"frame phase `{wa['info']['id']}` (frame.json phase {wa['info'].get('pos', '?')}, in the runner's order)" if wa['kind'] == 'phase' else f"task group {wa['info']['group']}")))
        elif k == 'side':
            i = an['info']
            out.append(('L doc', f"side walk `{i['name']}` ({i['tag']}) {fn(an['root'])}: walks categories {i['cats_text']} (render_categories.md passes table)"))
        elif k == 'walk':
            out.append((mk, f'the colour walk itself {fn(an["root"])}'))
        elif k == 'slot':
            import qwhen_slots
            out.append((mk, f"vtable slot: {fn(an['root'])} is " + '; or '.join(qwhen_slots.text(fn, s) for s in an['slots'][:3])
                        + (f" (+{len(an['slots']) - 3} more tables)" if len(an['slots']) > 3 else '')
                        + ": the object's own code calls it through that slot (q vtable --of, q class VTABLE); no task / frame phase / draw node was found above it, so WHEN the object runs is not answered here"))
    for an in r['anchors'][:2]:
        if an['chain']:
            out.append((an['mark'], f"path: {chain_text(w, an['root'], an['chain'])}"))
    if r.get('more_roots'):
        out.append(('L', f"+{r['more_roots']} more roots at that depth not printed"))
    if r.get('untraced') and r['anchors']:
        u = r['untraced']
        n_seen = sum(1 for _x, m in u if m == 'L')
        out.append(('C' if not n_seen else 'L', f"{len(u)} other direct caller{'s' if len(u) > 1 else ''} not followed (the climb stops at the first root it reaches"
                                                + (f"; {n_seen} of them only OBSERVED indirect" if n_seen else '') + f"): "
                                                f"{', '.join(fn(x) for x, _m in u[:4])}{' ...' if len(u) > 4 else ''} (q when CALLER for each; q callers {fn(r['addr'])})"))
    if r['named']:
        out.append(('L doc', f"named in render_categories.md rows {', '.join(x['spec'] for x in r['named'])} in the builder column (a builder, a slot-1 callee or "
                             f"another mention: q nodes CAT says which registration)"))
    if r['wait']:
        out.append(('C' if r['wait'].startswith('[C]') else 'L', 'wait (frame.json): ' + r['wait'].split('] ', 1)[-1]))
    if not r['anchors']:
        out.append(('L', 'no root found: no chain of listing calls / OBSERVED indirect calls from a task update, a frame phase, a draw node slot 1 or a side walk reaches it '
                         '(an indirect call never observed is not followed; this is not "never runs")'))
    out.append(('L', thread_text(r['threads'], r.get('threads_capped')) + ' [by db name]'))
    return out


def card_text(w, r):
    """the one header line of the card (<= CARD_CHARS), or None when nothing is known; the period is said only of the root itself, a function below it gets
    its call depth (it may sit under a branch)"""
    if not r['anchors'] and not r['threads']:
        return None
    ps = []
    for an in r['anchors'][:2]:
        k = an['kind']
        per = 'every frame' if not an['chain'] else f"{len(an['chain'])} call{'s' if len(an['chain']) > 1 else ''} below it"
        if k == 'task':
            ps.append(f"[{an['mark']}] task {fn(an['root'])} group {an['info']['group']} ({an['info']['where']}, {per})")
        elif k == 'phase':
            ps.append(f"[{an['mark']}] frame phase {an['info']['id']} ({per})")
        elif k == 'node':
            wp = [x for x in (an.get('walk') or {'anchors': []})['anchors'] if x['kind'] == 'phase']
            ps.append(f"[{'L doc' if an['src'] == 'doc' else an['mark']}] draw node cat {cat_text(an['cats'])}"
                      + (f" + {' '.join(an['passes'])}" if an['passes'] else '') + (f" in phase {wp[0]['info']['id']}" if wp else ''))
        elif k == 'side':
            ps.append(f"[L doc] side walk {an['info']['name']}")
        elif k == 'walk':
            ps.append(f'[{an["mark"]}] the colour walk')
        elif k == 'slot':
            s0 = an['slots'][0]
            ps.append(f"[{an['mark']}] vtable 0x{s0['vt']:08X} slot {s0['slot']}" + (f" seq {'/'.join(map(str, s0['seq']))}" if s0.get('seq') else '') + f" ({per})")
    t = r['threads']
    more = len(t) - 1 + (1 if r.get('threads_capped') else 0)
    ps.append('[L] thread ' + (t[0][1] + (f" +{len(t) - 1}{'+' if r.get('threads_capped') else ''} more" if more > 0 else '') if t else '?'))
    tail = f"  (+{len(r['untraced'])} callers not followed; q when)" if r.get('untraced') and r['anchors'] else '  (q when for the path)'
    head = ' | '.join(ps)
    return clip(head, CARD_CHARS - len(tail)) + tail


def facts(ctx):
    """the provider: -> [qfactcard.Fact] (one header line)"""
    import qfactcard
    try:
        w = world()
    except Exception as e:      # noqa: BLE001 - no index / no listing: no card, the verb says why
        if os.environ.get('WWE_QWHEN_DEBUG'):
            raise
        return []
    if ctx.addr not in w.names and not w.callers(ctx.addr):
        return []
    t = card_text(w, compute(w, ctx.addr))
    return [qfactcard.Fact('when', t, None, None, 85, 1)] if t else []


# ------------------------------------------------------------------ the verb
def addr_of(s):
    t = str(s).strip()
    t = t[4:] if t.lower().startswith('sub_') else t
    return int(t, 16)


def main(argv, con=None):
    ap = argparse.ArgumentParser(prog='q when')
    ap.add_argument('func')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        ad = addr_of(a.func)
    except ValueError:
        print(f'q: {a.func}: give sub_XXXXXXXX or 0xXXXXXXXX', file=sys.stderr)
        return 2
    w = world()
    r = compute(w, ad)
    ls = lines(w, r)
    lp = layout_problems(w.doc, full=True)
    if lp:
        ls.insert(0, ('!', 'render_categories.md layout changed, the category / side-walk answers may be wrong: ' + '; '.join(lp)))
    if a.json:
        print(json.dumps({'func': fn(ad), 'name': r['name'], 'lines': [{'tag': t, 'text': x} for t, x in ls],
                          'anchors': [{k: v for k, v in an.items() if k not in ('chain', 'walk', 'info')} | {'root': fn(an['root'])} for an in r['anchors']],
                          'threads': [t[1] for t in r['threads']]}, indent=1, default=str))
        return 0
    print(f"q when {fn(ad)} {r['name']}")
    for t, x in ls:
        print(f'  [{t}] {x}')
    print('  (computed from the index: task table, db/order/frame.json, `sub_82C30A10` registrations, render_categories.md, the call graph; q order FUNC / q nodes CAT show the parts)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

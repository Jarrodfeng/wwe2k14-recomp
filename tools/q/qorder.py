r"""qorder: where a game function sits in ONE FRAME of the game (S111 ORDER, docs\research\s111\ORDER.md). No game run; addresses and our words only.

    py tools/q/q.py order               the frame in run order: the runner's phases, then the 35 task groups, each with its tasks' update functions and the
                                        constructors that register them (group = the `li r4` literal, or what the callers pass)
    py tools/q/q.py order FUNC          which task runs FUNC (the chain from the task's update down to FUNC: listing calls and OBSERVED indirect calls), its group and
                                        frame phase, what it waits on, what it makes, and the known points of the frame in run order with FUNC marked
    --json   --limit N   --rebuild (recompute the task table)

COMPUTED from the listing (as `q nodes` reads sub_82C30A10): every `bl sub_82C307B0` site (the task base constructor: task, group, on); the group = the `li r4,N`
before it, or, when r4 is an argument, the literals the callers pass (up to ARG_DEPTH levels); the task's classes = the vtable constants stored over the task object
in that constructor and in the constructors that pass the same object on (CLS_DEPTH levels); a class's update = its slot 1 (the runner calls +4: sub_82C302A0 +108..+120).
Cached in out\harness\index\order.s<VERSION>.json, keyed by the codegen fingerprint (WWE_QORDER_CACHE = another file; game-derived: stays in out\).
READ from the listing and stored as OUR annotations in db\order\frame.json (addresses + our words + mark + source; WWE_QORDER_FRAME = another file): the runner's
phases, the order inside a group that registration order gives (`within`), a group the listing cannot give (`groups`: a group word read from data), what a function
waits on (`waits`), which point makes which object (`spawns`).
Limits: the reverse chain follows the index's direct call edges and the indirect pairs OBSERVED in coverage runs (tools\q\qseen.py); an indirect edge never observed
is not followed (the answer then says `no task found`, never a guess). Inside a group the order is the registration order, which the listing cannot give: only the
`within` annotations rank tasks of one group. A registration whose group is a data word (at the site, or passed by some caller) is printed `[U ...]`, never as a
literal: its computed groups are only what the literal callers pass (S111 ORDER review).
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qx7  # noqa: E402

VERSION = 2                    # 2: the caller walk starts from the entry register r4 holds at the site (S111 ORDER review: sub_823BC218 passes its r5)
TASK_REG = 0x82C307B0          # sub_82C307B0(task, group, on): +10 [task+4] = group, +20 [task+8] = 1, +A0 appends the task to the group (sub_82C2FEB8)
RUNNER = 0x82C302A0            # the owner update: the task's slot 1 is called at +120
N_GROUPS = 35                  # the group array owner +32 .. +40 (docs\systems\task_runner.md, run 034 dump)
FRAME_JSON = os.path.join(HERE, '..', '..', 'db', 'order', 'frame.json')
ARG_DEPTH, CLS_DEPTH, WALK_DEPTH, WALK_CAP = 6, 2, 12, 6000


class OrderError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


def fn(a):
    return f'sub_{a:08X}'


def addr(s):
    """sub_X / 0xX / X (hex) -> int"""
    if isinstance(s, int):
        return s
    t = str(s).strip()
    t = t[4:] if t.lower().startswith('sub_') else t
    try:
        return int(t, 16)
    except ValueError:
        raise OrderError(f'{s}: give sub_XXXXXXXX or 0xXXXXXXXX')


def cache_path():
    return os.environ.get('WWE_QORDER_CACHE') or os.path.join(qx7.IDX, 'order.s%d.json' % VERSION)


# ------------------------------------------------------------------ the task table (computed)
class Bodies:
    """the instruction lists of game functions, read from the generated listing one file at a time (only the asked functions are kept)"""

    def __init__(self, icon, gen=None):
        self.icon, self.gen = icon, gen or os.environ.get('WWE_Q_GEN') or qx7.GEN
        self.got = {}

    def load(self, addrs):
        by_file = {}
        for a in set(addrs):
            if a in self.got:
                continue
            r = self.icon.execute('SELECT file, line FROM funcs WHERE addr = ?', (a,)).fetchone()
            if r is None or not r[0]:
                self.got[a] = None
                continue
            by_file.setdefault(r[0], []).append((a, r[1] or 1))
        for file, items in sorted(by_file.items()):
            try:
                with open(os.path.join(self.gen, file), encoding='utf-8', errors='replace') as f:
                    lines = f.read().split('\n')
            except OSError:
                for a, _ in items:
                    self.got[a] = None
                continue
            for a, line in items:
                tag = f'DEFINE_REX_FUNC(sub_{a:08X})'
                i = line - 1
                if i >= len(lines) or not lines[i].startswith(tag):
                    i = next((k for k, l in enumerate(lines) if l.startswith(tag)), None)
                self.got[a] = qx7._read_body(lines, i, a)[0] if i is not None else None


def _norm(d, off=0):
    """(base def, offset) of an object expression: `r3+32` -> (in r3, 32)"""
    if d and d[0] == 'addi':
        return d[1], d[2] + off
    return d, off


def compute(icon=None):
    """-> {'version', 'fingerprint', 'sites': [registration rows], 'by_update': {str(update): [row index]}}"""
    icon = icon or qx7._index_con()
    img = qx7.Img()
    funcs = frozenset(r[0] for r in icon.execute('SELECT addr FROM funcs'))
    rtti = frozenset(r[0] for r in icon.execute('SELECT vt FROM rtti_vtable'))
    helpers = frozenset(r[0] for r in icon.execute("SELECT addr FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'"))

    def vt_ok(a):
        return a in rtti or img.word(a) in funcs

    ccache = {}

    def callers(a):
        if a not in ccache:
            ccache[a] = icon.execute("SELECT DISTINCT src, site FROM edges WHERE dst = ? AND kind = 'call' ORDER BY src, site", (a,)).fetchall()
        return ccache[a]

    def tail_callers(a):
        """the functions that enter a by a tail branch (`b sub_X`): the tracker keeps no state at a `b`, so the walk reports them open"""
        return icon.execute("SELECT DISTINCT src FROM edges WHERE dst = ? AND kind = 'tail' ORDER BY src", (a,)).fetchall()

    B = Bodies(icon)
    state = {}

    def states(a):
        """-> ({call site: register state at the bl}, Tracker) of a, or (None, None)"""
        if a not in state:
            ins = B.got.get(a)
            if ins is None:
                state[a] = (None, None)
            else:
                at = {}

                def hit(kind, ad, info, tr, at=at):
                    if kind == 'call':
                        at[ad] = info[1]
                state[a] = (at, qx7.run_function(ins, vt_ok, helpers, hit))
        return state[a]

    regs = callers(TASK_REG)
    B.load([s for s, _ in regs])
    sites = []
    for src, site in regs:
        at, _ = states(src)
        st = (at or {}).get(site)
        r4 = st.get('r4') if st else None
        r5 = st.get('r5') if st else None
        x = {'site': site, 'func': src, 'st': st, 'groups': [], 'from': '', 'open': [], 'on': r5[1] if r5 and r5[0] == 'li' else None}
        if r4 is None:
            x['open'].append('body not read')
        elif r4[0] == 'li':
            x['groups'], x['from'] = [r4[1]], 'li r4'
        elif r4[0] == 'in':
            x['from'] = 'callers'
        else:
            x['open'].append(qx7.describe(r4))
        sites.append(x)
    # the group from the callers: the literal each passes in the register, followed up while it is an argument again
    work = [(i, x['func'], x['st'].get('r4')[1], 1) for i, x in enumerate(sites) if x['from'] == 'callers']      # r4 at the bl can copy another entry register (sub_823BC218: r5)
    while work:
        B.load([c for _, f, _, _ in work for c, _ in callers(f)])
        nxt = []
        for i, f, reg, lvl in work:
            cs, tl = callers(f), tail_callers(f)
            if not cs and not tl:
                sites[i]['open'].append(f'{fn(f)}: no direct callers')
            for (c,) in tl:      # S111 ORDER review: sub_82657758 (on sub_82656580's walk) and sub_82BCADF0 are also entered by tail branches
                sites[i]['open'].append(f'{fn(c)}: a tail branch to {fn(f)}, its registers not followed')
            for c, csite in cs:
                at, _ = states(c)
                v = (at or {}).get(csite)
                d = v.get(reg) if v else None
                if d is None:
                    sites[i]['open'].append(f'{fn(c)}: not read')
                elif d[0] == 'li':
                    if d[1] not in sites[i]['groups']:
                        sites[i]['groups'].append(d[1])
                elif d[0] == 'in' and lvl < ARG_DEPTH:
                    nxt.append((i, c, d[1], lvl + 1))
                else:
                    sites[i]['open'].append(f'{fn(c)}: {qx7.describe(d)}')
        work = nxt
    # the classes: the vtable constants stored over the task object here and in the constructors that pass the same object on
    for x in sites:
        x['vts'] = []
        if not x['st']:
            continue
        b0, o0 = _norm(x['st'].get('r3'))
        level = [(x['func'], b0, o0, 0)]
        while level:
            B.load([c for f, b, _, lvl in level if b == ('in', 'r3') and lvl < CLS_DEPTH for c, _ in callers(f)])
            nxt = []
            for f, b, o, lvl in level:
                _, tr = states(f)
                if tr is not None:
                    for (sb, so), vts in tr.stores.items():
                        if _norm(sb, so) == (b, o):
                            for v in vts:
                                if v not in [y[0] for y in x['vts']]:
                                    x['vts'].append((v, f))
                if b == ('in', 'r3') and lvl < CLS_DEPTH:
                    for c, csite in callers(f):
                        at, _ = states(c)
                        v = (at or {}).get(csite)
                        if v and v.get('r3'):
                            cb, co = _norm(v.get('r3'), o)
                            nxt.append((c, cb, co, lvl + 1))
            level = nxt
    out, by_update = [], {}
    for x in sites:
        cls = []
        for v, by in x['vts']:
            w = img.word(v + 4)
            if w in funcs:
                cls.append({'vt': v, 'by': by, 'update': w})
        row = {'site': x['site'], 'func': x['func'], 'groups': sorted(x['groups']), 'from': x['from'], 'open': sorted(set(x['open']))[:6],
               'n_open': len(set(x['open'])), 'on': x['on'], 'obj': qx7.describe(x['st'].get('r3')) if x['st'] else '?', 'classes': cls}
        for u in {c['update'] for c in cls}:
            by_update.setdefault(str(u), []).append(len(out))
        out.append(row)
    return {'version': VERSION, 'fingerprint': qx7._fingerprint(), 'sites': out, 'by_update': by_update}


def table(rebuild=False, icon=None):
    """the task table: the cache when it matches this codegen, else computed (about 6 s) and cached"""
    p = cache_path()
    if not rebuild and os.path.exists(p):
        try:
            with open(p, encoding='utf-8') as f:
                t = json.load(f)
            if t.get('version') == VERSION and (t.get('fingerprint') == qx7._fingerprint() or not os.path.isdir(qx7.GEN)):
                return t
        except (OSError, ValueError):
            pass
    t = compute(icon)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + '.tmp%d' % os.getpid()
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(t, f)
    os.replace(tmp, p)
    return t


def frame_facts(path=None):
    p = path or os.environ.get('WWE_QORDER_FRAME') or FRAME_JSON
    try:
        with open(p, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        raise OrderError(f'the frame annotations {p}: {e}')


def groups_of_update(u, t, facts=None):
    """-> (sorted groups, registration rows, annotation or None) of a task update function: the computed groups; when the listing gives none, a `groups`
    annotation of db/order (its mark says how sure)"""
    rows = [t['sites'][i] for i in t['by_update'].get(str(u), [])]
    gs = set()
    for r in rows:
        gs |= set(r['groups'])
    if not gs and facts:
        for g in facts.get('groups', []):
            if addr(g['update']) == u:
                return [g['group']], rows, g
    return sorted(gs), rows, None


def group_text(u, t, facts=None):
    """the group answer of a task update in words: the groups, then `[U ...]` when a registration of it has callers whose group the listing cannot give"""
    gs, rows, ann = groups_of_update(u, t, facts)
    opn = sorted({o for r in rows for o in r['open']})
    if ann or not opn:
        return str(gs) if gs else '?'
    n = sum(r['n_open'] for r in rows)
    return (str(gs) if gs else '?') + f" [U: {n} open (a data word or a caller not read): {'; '.join(opn[:3])}{' ...' if n > 3 else ''}]"


def range_bad(t):
    """-> [failure]: a computed group outside 0..N_GROUPS-1 (the runner walks only the group records owner+32 .. +40)"""
    return [f"{fn(r['func'])} +{r['site'] - r['func']:X}: group {g}" for r in t['sites'] for g in r['groups'] if not 0 <= g < N_GROUPS]


def word_bad(t):
    """-> [failure]: a site whose r4 at the bl is neither a literal nor an argument (a data word) with a computed group"""
    return [f"{fn(r['func'])} +{r['site'] - r['func']:X}: r4 is {r['open'][:1]} but groups {r['groups']}" for r in t['sites'] if r['from'] == '' and r['groups']]


# ------------------------------------------------------------------ which task runs a function
def _seen_con():
    try:
        import qseen
        return qseen.open_seen()
    except Exception:      # noqa: BLE001 - no observed pairs: direct edges only (the answer says so)
        return None


def task_chain(func, t, icon=None, scon=None):
    """-> [{'update', 'chain': [(caller, site, how, callee)] from the task update down to func}] for the task updates found at the smallest depth"""
    icon = icon or qx7._index_con()
    ups = {int(k) for k in t['by_update']}
    if func in ups:
        return [{'update': func, 'chain': []}]
    prev = {func: None}
    frontier, found = [func], []
    for _depth in range(WALK_DEPTH):
        if found or not frontier or len(prev) > WALK_CAP:
            break
        nxt = []
        for g in frontier:
            ins = [(r[0], r[1], r[2]) for r in icon.execute("SELECT DISTINCT src, site, kind FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (g,))]
            if scon is not None:
                ins += [(r[0], r[1], 'seen in %d runs' % r[2]) for r in scon.execute('SELECT func, site, runs FROM seen WHERE target = ?', (g,))]
            for src, site, how in ins:
                if src == RUNNER or src in prev:
                    continue
                prev[src] = (g, site, how)
                (found if src in ups else nxt).append(src)
        frontier = nxt
    out = []
    for u in found:
        chain, x = [], u
        while prev.get(x):
            g, site, how = prev[x]
            chain.append((x, site, how, g))
            x = g
        out.append({'update': u, 'chain': chain})
    return out


# ------------------------------------------------------------------ the position in the frame
def position(func, t, facts, chains):
    """-> {'phase': index, 'group', 'rank', 'update'} or None; rank = the update's index in a `within` annotation of its group (registration order)"""
    ph = {p['id']: i for i, p in enumerate(facts['phases'])}
    for p in facts['phases']:
        fs = p.get('func') or []
        if func in {addr(f) for f in (fs if isinstance(fs, list) else [fs])}:      # a function the runner calls itself: a phase, not a task
            return {'phase': ph[p['id']], 'group': None, 'rank': None, 'update': None}
    if not chains:
        return None
    u = chains[0]['update']
    gs, rows, ann = groups_of_update(u, t, facts)
    g = gs[0] if len(gs) == 1 and (ann or not any(r['n_open'] for r in rows)) else None      # an open registration: the group is not known
    rank = None
    for w in facts.get('within', []):
        if w.get('group') == g and fn(u) in w['order']:
            rank = w['order'].index(fn(u))
    return {'phase': ph['groups'], 'group': g, 'rank': rank, 'update': u}


def key(pos):
    """the run-order key of a position (unknown parts sort last)"""
    if pos is None:
        return (99, 99, 99)
    return (pos['phase'], 99 if pos['group'] is None else pos['group'], 99 if pos['rank'] is None else pos['rank'])


def where(pos, facts):
    if pos is None:
        return '?'
    s = facts['phases'][pos['phase']]['id']
    if pos['group'] is not None:
        s += f" g{pos['group']}"
    if pos['rank'] is not None:
        s += f" r{pos['rank']}"
    return s


def known_points(t, facts, icon=None, scon=None):
    """-> [(func, position, chains)] of every function the annotations name a wait for, in run order"""
    pts = []
    for a in dict.fromkeys(addr(w['func']) for w in facts.get('waits', [])):
        ch = task_chain(a, t, icon, scon)
        pts.append((a, position(a, t, facts, ch), ch))
    pts.sort(key=lambda p: key(p[1]))
    return pts


def check(t, facts, expect, icon=None, scon=None):
    """-> [failure]: each expected point is run by the expected task update, in the expected group; the points come in the expected run order; each one's wait
    annotation says the expected words (tools\\q\\qorder_selftest.py)"""
    bad, pos = [], {}
    for e in expect:
        a = addr(e['func'])
        ch = task_chain(a, t, icon, scon)
        ups = [c['update'] for c in ch]
        if addr(e['update']) not in ups:
            bad.append(f"{e['func']}: run by {[fn(u) for u in ups] or 'no task'}, expected {e['update']}")
            continue
        gs, _, _ = groups_of_update(addr(e['update']), t, facts)
        if gs != [e['group']]:
            bad.append(f"{e['func']}: its task {e['update']} is in groups {gs}, expected [{e['group']}]")
        pos[e['func']] = position(a, t, facts, [c for c in ch if c['update'] == addr(e['update'])])
        said = ' '.join(w['what'] for w in facts.get('waits', []) if addr(w['func']) == a)
        for word in e['wait_words']:
            if word not in said:
                bad.append(f"{e['func']}: its wait does not say {word!r} (annotation: {said or 'none'})")
    names = [e['func'] for e in expect]
    for x, y in zip(names, names[1:]):
        if x in pos and y in pos and not key(pos[x]) < key(pos[y]):
            bad.append(f'order: {x} ({where(pos[x], facts)}) does not run before {y} ({where(pos[y], facts)}) in a frame')
    return bad


# ------------------------------------------------------------------ printing
def main_frame(a, t, facts):
    names = qx7._db_names()
    groups, unres = {}, []
    for r in t['sites']:
        if not r['groups']:
            unres.append(r)
        for g in r['groups']:
            groups.setdefault(g, []).append(r)
    ann = {}
    for g in facts.get('groups', []):
        ann.setdefault(g['group'], []).append(g)
    if a.json:
        print(json.dumps({'phases': facts['phases'], 'within': facts.get('within', []), 'groups_annotated': facts.get('groups', []),
                          'groups': {str(g): groups.get(g, []) for g in range(N_GROUPS)}, 'unresolved': unres}, indent=1))
        return 0
    print(f"q order: one frame of the game, in run order ({len(t['sites'])} registrations of {fn(TASK_REG)}: group = `li r4`, else what the callers pass)")
    for p in facts['phases']:
        print(f"  [{p['mark']}] {p['id']:9s} {p['what']}  ({p['src']})")
        if p['id'] != 'groups':
            continue
        for g in range(N_GROUPS):
            ups, part = {}, set()
            for r in groups.get(g, []):
                for c in r['classes']:
                    ups.setdefault(c['update'], set()).add(r['func'])
                    if r['n_open']:
                        part.add(c['update'])
            print(f"      group {g:2d}: {len(groups.get(g, []))} registration site(s), {len(ups)} update function(s)")
            for u, by in sorted(ups.items())[:a.limit]:
                print(f"        {fn(u)} {names.get(u, '')[:40]:40s} registered by {', '.join(fn(b) for b in sorted(by))[:64]}"
                      + ('  [U: other callers pass a data word]' if u in part else ''))
            if len(ups) > a.limit:
                print(f'        ... {len(ups) - a.limit} more (q order --limit {len(ups)})')
            for x in ann.get(g, []):
                print(f"        {x['update']} [{x['mark']}: annotated, the listing gives no literal] {x['why'][:120]}")
            for w in facts.get('within', []):
                if w.get('group') == g:
                    print(f"        [{w['mark']}] in this order: {' -> '.join(w['order'])}  ({w['why'][:110]}...)")
    npart = sum(1 for r in t['sites'] if r['groups'] and r['n_open'])
    print(f'  group not resolved: {len(unres)} site(s); {npart} more have literal callers AND open ones (listed under their literal groups, marked [U])'
          ' (q order --json lists them with what the callers pass)')
    return 0


def main_func(a, func, t, facts):
    names = qx7._db_names()
    icon, scon = qx7._index_con(), _seen_con()
    chains = task_chain(func, t, icon, scon)
    pos = position(func, t, facts, chains)
    waits = [w for w in facts.get('waits', []) if addr(w['func']) == func]
    ups = {c['update'] for c in chains}
    makes = [s for s in facts.get('spawns', []) if {addr(s['by']), addr(s['makes'])} & ({func} | ups)]
    pts = known_points(t, facts, icon, scon)
    if a.json:
        print(json.dumps({'func': fn(func), 'tasks': [{'update': fn(c['update']), 'groups': groups_of_update(c['update'], t, facts)[0],
                                                       'group_text': group_text(c['update'], t, facts),
                                                       'chain': [[fn(x), hx(s) if s else None, how, fn(y)] for x, s, how, y in c['chain']]} for c in chains],
                          'position': pos, 'where': where(pos, facts), 'waits': waits, 'makes': makes,
                          'frame': [{'func': fn(p), 'where': where(q, facts)} for p, q, _ in pts], 'observed_pairs': scon is not None}, indent=1))
        return 0
    print(f"q order {fn(func)} {names.get(func, '')}")
    if not chains and pos is not None:
        print(f"  not a task: the frame calls it itself, in the phase `{facts['phases'][pos['phase']]['id']}` ({facts['phases'][pos['phase']]['src']})")
    elif not chains:
        print('  no task found: no chain of listing calls / OBSERVED indirect calls from a registered task update reaches it (an indirect call never observed is not followed)')
    for c in chains[:a.limit]:
        gs, rows, ann = groups_of_update(c['update'], t, facts)
        regs = sorted({(r['func'], r['site'], r['from']) for r in rows})
        print(f"  task: update {fn(c['update'])} {names.get(c['update'], '')[:40]}  group {group_text(c['update'], t, facts)}  [C listing: registered by "
              + '; '.join(f'{fn(f)} +{s - f:X} ({how})' for f, s, how in regs[:3]) + (' ...' if len(regs) > 3 else '') + ']')
        if ann:
            print(f"    group {ann['group']} [{ann['mark']}: annotated, the listing gives no literal]: {ann['why']}")
        print(f'    chain: {fn(RUNNER)} +120 (the task\'s slot 1) -> {fn(c["update"])}')
        for x, s, how, y in c['chain']:
            mark = 'C listing ' + how if how in ('call', 'tail') else 'L ' + how
            print(f"      {fn(x)}{(' site ' + hx(s)) if s else ''} -> {fn(y)} {names.get(y, '')[:36]}  [{mark}]")
        on = {c['update']} | {y for _, _, _, y in c['chain']}
        for w in facts.get('waits', []):
            if addr(w['func']) in on and addr(w['func']) != func:
                print(f"    on its chain, {w['func']} [{w['mark']}]: {w['what']}  ({w['src']})")
    if scon is None:
        print('  (no observed-pair table: only listing calls were followed)')
    print(f'  frame position: {where(pos, facts)}' + ('' if pos is None or pos['rank'] is not None or pos['group'] is None else
                                                     ' (its rank inside the group is the registration order: no `within` annotation)'))
    for w in waits:
        print(f"  waits [{w['mark']}]: {w['what']}  ({w['src']})")
    if not waits:
        print('  waits: no annotation in db/order/frame.json (read its branches: q decomp ' + fn(func) + ')')
    for s in makes:
        print(f"  makes [{s['mark']}]: {s['what']}  ({s['src']})")
    print('  the known points of one frame, in run order (positions computed; the points = the annotated waits):')
    for p, q, _ in pts:
        print(f"    {where(q, facts):14s} {fn(p)} {names.get(p, '')[:44]}" + ('   <== this' if p == func else ''))
    print('  [C] a wait holds only its own task: the runner (sub_82C302A0 +FC..+120) runs every task whose [task+8] != 0, group by group in index order; the only'
          ' hold over a whole group is the sequence\'s slot 4 gate (+D8). So a point above runs before every point below it in the same frame, whatever they wait on.')
    return 0


def main(argv, con=None):
    ap = argparse.ArgumentParser(prog='q order')
    ap.add_argument('func', nargs='?')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--limit', type=int, default=12)
    ap.add_argument('--rebuild', action='store_true')
    a = ap.parse_args(argv)
    t = table(a.rebuild, con)
    facts = frame_facts()
    if a.func is None:
        return main_frame(a, t, facts)
    return main_func(a, addr(a.func), t, facts)


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (OrderError, qx7.XError) as e:
        print(f'q: {e}', file=sys.stderr)
        sys.exit(2)

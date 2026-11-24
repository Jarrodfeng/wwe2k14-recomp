r"""q read pick --all (S110 lane PICKALL): every cluster with no stored reading, in the order Jarrod set for the whole-game reading.

    py tools/q/q.py read pick --all [--n N] [--out DIR] [--group NAME]     one sheet per unit (DIR/<unit>.txt) + batch.txt; no --n: every unit
    py tools/q/q.py read pick --all --count                                units and sheet bytes per group, nothing written

Units with >= 3 functions and no row in unit_read, ordered by GROUP, then by evidence (names + anchors + observed edges, most first).
batch.txt columns (tab-separated): file, unit, group, the grouping reason, bytes, top named functions.

SOUNDNESS: the group ONLY ORDERS THE READING. It is a mechanical guess from names, strings and neighbours; it is never stored as a fact
about a unit (the reader's `system:` line says what the unit is).
The group of a unit is the first rule that fires (each keeps its reason):
  a. neighbours_rule   the stored system of read units this unit's functions call or are called by (strict majority, >= 2 votes)
  b. keyword_rule      keywords in the unit's own names and strings (the group with most hits, >= 2, beating the runner-up)
  c. dbsystem_rule     the db `system` field of its named functions when it is not `other` (strict majority)
  d. rest_rule         else `rest`
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

GROUPS = ('creation', 'menus', 'match', 'camera', 'assets', 'audio', 'ai', 'rest')

# creation: the creation suite (create a superstar / entrance / moveset / arena / title / story), the Threads and paint editors
KW_CREATION = ('create', 'edit', 'caw', 'cae', 'threads', 'paint', 'logo', 'moveset', 'ceditex')
# menus: menus and the UI framework
KW_MENUS = ('menu', 'page', 'cursor', 'select', 'option', 'smartmenu')
# match: match state and the event bus
KW_MATCH = ('match', 'rule', 'pin', 'referee', 'finisher', 'event', 'judge')
# camera: camera, replays, entrances, cutscene scripting
KW_CAMERA = ('camera', 'replay', 'cutscene', 'entrance', 'director', 'shot')
# assets: asset loading and formats (readend = ReadEndProcess)
KW_ASSETS = ('pach', 'pac', 'load', 'readend', 'texture', 'reader')
# audio
KW_AUDIO = ('sound', 'audio', 'voice', 'bgm', 'xma', 'stream')
# ai: navigation and decision code (yase = the game's AI-script prefix)
KW_AI = ('ai', 'navi', 'think', 'decide', 'yase')
KEYWORDS = {'creation': KW_CREATION, 'menus': KW_MENUS, 'match': KW_MATCH, 'camera': KW_CAMERA, 'assets': KW_ASSETS,
            'audio': KW_AUDIO, 'ai': KW_AI}

# a stored reader `system` (qread.SYSTEMS) -> the group it orders under; systems with no group (pose, model, ...) cast no vote
SYSTEM_GROUP = {'menus': 'menus', 'ui': 'menus', 'match': 'match', 'camera': 'camera', 'audio': 'audio', 'ai': 'ai'}
# the db `system` field (db/*.json) -> group; render and io have no group of their own and fall through to `rest`
DBSYSTEM_GROUP = {'camera': 'camera', 'match': 'match', 'assets': 'assets', 'audio': 'audio', 'ui': 'menus'}
MIN_VOTES = 2
MIN_HITS = 2
SOUND_LINE = ('the group only orders the reading: it is a mechanical guess from names, strings and neighbours, never stored as a fact about a unit '
              '(the reader says what the unit is).')


class Data:
    """what the rules need, however it was loaded.
    members {uid: [func]}   nbr {uid: {neighbour uid}}   text {uid: [name or string]}   dbsys {uid: [db system of each db-named func]}
    ev {uid: evidence count}   top {uid: [(func, name)]} the named functions, best first"""

    def __init__(self, members, nbr, text, dbsys, ev, top):
        self.members, self.nbr, self.text, self.dbsys, self.ev, self.top = members, nbr, text, dbsys, ev, top


# ---------------------------------------------------------------- the four rules
def tokens(text):
    """lower-case words of a name or string, camel case split, plus the joined pairs of neighbours (Read End -> readend)"""
    t = re.sub(r'([a-z0-9])([A-Z])', r'\1 \2', text or '')
    w = re.findall(r'[a-z0-9]+', t.lower())
    return w, [a + b for a, b in zip(w, w[1:])]


def kw_hits(texts):
    """{group: hits}: a word hits a keyword when equal, or (keyword of 5+ letters) when it starts with it; a joined pair only when equal;
    once per token per group"""
    hits = collections.Counter()
    for t in texts:
        words, pairs = tokens(t)
        for tok, prefix in [(x, True) for x in words] + [(x, False) for x in pairs]:
            for g, kws in KEYWORDS.items():
                if any(tok == k or (prefix and len(k) >= 5 and tok.startswith(k)) for k in kws):
                    hits[g] += 1
    return hits


def neighbours_rule(uid, data, readsys):
    """(a) the stored system of read neighbour units (units this one calls or is called by): strict majority of >= 2 votes, else None"""
    votes = collections.Counter()
    for v in data.nbr.get(uid, ()):
        g = SYSTEM_GROUP.get(readsys.get(v, ''))
        if g:
            votes[g] += 1
    n = sum(votes.values())
    if n < MIN_VOTES:
        return None
    g, k = votes.most_common(1)[0]
    if 2 * k <= n:
        return None
    return g, 'neighbours: %d of %d read neighbour votes say %s' % (k, n, g)


def keyword_rule(uid, data, readsys):
    """(b) keyword hits in the unit's own names and strings: the top group needs >= 2 hits and more than the runner-up"""
    hits = kw_hits(data.text.get(uid, ()))
    top = hits.most_common(2)
    if not top or top[0][1] < MIN_HITS:
        return None
    if len(top) > 1 and top[1][1] >= top[0][1]:
        return None
    return top[0][0], 'keywords: %d hits for %s (next %d)' % (top[0][1], top[0][0], top[1][1] if len(top) > 1 else 0)


def dbsystem_rule(uid, data, readsys):
    """(c) the db `system` of the unit's named functions (not `other`): strict majority among those that map to a group"""
    votes = collections.Counter(DBSYSTEM_GROUP[s] for s in data.dbsys.get(uid, ()) if s in DBSYSTEM_GROUP)
    n = sum(votes.values())
    if not n:
        return None
    g, k = votes.most_common(1)[0]
    if 2 * k <= n:
        return None
    return g, 'db system: %d of %d mapped db-named functions say %s' % (k, n, g)


def rest_rule(uid, data, readsys):
    """(d) nothing fired: rest"""
    return 'rest', 'no rule fired'


RULES = (neighbours_rule, keyword_rule, dbsystem_rule, rest_rule)


def classify(uid, data, readsys):
    """-> (group, reason): the first rule that fires"""
    for rule in RULES:
        r = rule(uid, data, readsys)
        if r:
            return r
    raise AssertionError('rest_rule always fires')


# ---------------------------------------------------------------- the plan
def plan(data, answered, readsys, only_group=None):
    """-> [dict(unit, group, reason, ev)] units with >= 3 functions (data.members) not in `answered`, by group order then most evidence first"""
    out = []
    for u, fs in data.members.items():
        if len(fs) < 3 or u in answered:
            continue
        g, why = classify(u, data, readsys)
        if only_group and g != only_group:
            continue
        out.append({'unit': u, 'group': g, 'reason': why, 'ev': data.ev.get(u, 0)})
    out.sort(key=lambda r: (GROUPS.index(r['group']), -r['ev'], r['unit']))
    return out


def count_table(rows, sizes):
    """-> [(group, units, bytes)] in group order, then the lines of the table; sizes {unit: bytes}"""
    n, b = collections.Counter(), collections.Counter()
    for r in rows:
        n[r['group']] += 1
        b[r['group']] += sizes.get(r['unit'], 0)
    tab = [(g, n[g], b[g]) for g in GROUPS]
    lines = ['%-9s %6d units %9.2f MB' % (g, k, x / 1048576.0) for g, k, x in tab]
    lines.append('%-9s %6d units %9.2f MB' % ('total', sum(n.values()), sum(b.values()) / 1048576.0))
    return tab, lines


# ---------------------------------------------------------------- the real loader
def load_data(ctx):
    """Data from a qsheet.Ctx (the real index): edges and strings from index.db, names from ctx.game / ctx.db, observed edges from seen.s1.db"""
    members = collections.defaultdict(list)
    uof = {}
    for f, u in ctx.units.execute('select func,unit_id from units'):
        uof[f] = u
    big = {r[0] for r in ctx.units.execute('select unit_id from unit where n_funcs>=3')}
    for f, u in uof.items():
        if u in big:
            members[u].append(f)
    nbr = collections.defaultdict(set)
    for s, d in ctx.ix.execute("select src,dst from edges where kind in ('call','tail')"):
        us, ud = uof.get(s), uof.get(d)
        if us is not None and ud is not None and us != ud:
            if us in big:
                nbr[us].add(ud)
            if ud in big:
                nbr[ud].add(us)
    text = collections.defaultdict(list)
    dbsys = collections.defaultdict(list)
    ev = collections.Counter()
    top = collections.defaultdict(list)
    for u, fs in members.items():
        for f in sorted(fs):
            nm = None
            if f in ctx.game:
                text[u].extend(ctx.game[f])
                ev[u] += 2                      # a game-carried name is a name and an anchor
                nm = ctx.game[f][0]
            if f in ctx.db:
                text[u].append(ctx.db[f][0])
                dbsys[u].append(ctx.dbsys.get(f, 'other'))
                ev[u] += 1
                nm = nm or ctx.db[f][0]
            if nm:
                top[u].append((f, nm))
    nstr = collections.Counter()
    for f, t in ctx.ix.execute('select r.func,s.text from strrefs r join strings s on s.addr=r.addr'):
        u = uof.get(f)
        if u in members and t and nstr[f] < 40:
            nstr[f] += 1
            text[u].append(t[:100])
    for (f,) in ctx.seen.execute('select func from seen'):
        u = uof.get(f)
        if u in members:
            ev[u] += 1
    return Data(dict(members), nbr, text, dbsys, ev, top)


# ---------------------------------------------------------------- command
def read_state(path=None):
    """-> (answered unit ids, {unit: newest stored system})"""
    import qread
    c = qread.open_ro(path)
    if c is None:
        return set(), {}
    try:
        answered = {r[0] for r in c.execute('select distinct unit_id from unit_read')}
        sysmap = {}
        for u, sy in c.execute('select unit_id,system from unit_read order by id'):
            sysmap[u] = sy or ''
    finally:
        c.close()
    return answered, sysmap


def main(rest):
    import qsheet

    def opt(flag, default=None):
        return rest[rest.index(flag) + 1] if flag in rest and rest.index(flag) + 1 < len(rest) else default
    only = opt('--group')
    if only and only not in GROUPS:
        raise ValueError('--group is one of: %s' % ' '.join(GROUPS))
    n = int(opt('--n')) if opt('--n') else None
    outdir = opt('--out')
    answered, readsys = read_state()
    ctx = qsheet.Ctx()
    data = load_data(ctx)
    rows = plan(data, answered, readsys, only)
    if n is not None:
        rows = rows[:n]
    ctx.hooks()
    if '--count' in rest:
        sizes = {r['unit']: qsheet.build(ctx, r['unit'])[1]['bytes'] for r in rows}
        _tab, lines = count_table(rows, sizes)
        by_rule = collections.Counter(r['reason'].split(':')[0] for r in rows)
        print('\n'.join(lines))
        print('placed by rule: %s' % ' '.join('%s %d' % kv for kv in sorted(by_rule.items())))
        print(SOUND_LINE)
        return 0
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    total = 0
    batch = []
    for r in rows:
        text, fl = qsheet.build(ctx, r['unit'])
        total += fl['bytes']
        if outdir:
            with open(os.path.join(outdir, '%d.txt' % r['unit']), 'w', encoding='utf-8') as fh:
                fh.write(text)
        topn = ' '.join(nm for _f, nm in data.top.get(r['unit'], [])[:4])
        batch.append('%d.txt\tunit %d\t%s\t%s\t%d B\t%s\n' % (r['unit'], r['unit'], r['group'], r['reason'], fl['bytes'], topn))
        print('unit %d  %s  %s  %d B' % (r['unit'], r['group'], r['reason'], fl['bytes']))
    if outdir:
        with open(os.path.join(outdir, 'batch.txt'), 'w', encoding='utf-8') as fh:
            fh.writelines(batch)
    print('batch: %d units, %d bytes total%s' % (len(rows), total, ' -> ' + outdir if outdir else ''))
    return 0

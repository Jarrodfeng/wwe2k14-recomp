r"""q gstate --inherit: the render state IN FORCE when a game function runs, read up its callers (S111 GINHERIT; the q miss of
docs/research/s111/ALPHAWREST.md: "gstate has no 'what state does this draw inherit' verb").

    py tools/q/q.py gstate --inherit sub_X [sub_Y ...] [--field F] [--depth N] [--json]
    py tools/q/q.py gstate --facts sub_X [...]       adds an `inherited` fact per field (kind `inherited`, `via` = the chain)

Per field (a gstate setter key), the value at the function's ENTRY, from the nearest event above it on every caller path:
  - a set in a caller BEFORE its call site (q gstate's rows, by listing offset): its constant(s); a non-constant = the register;
    a set of the field from the caller's own argument (`reg in_rK`) is that caller's callers' value of rK (gstate's forwarder rows);
  - a restore in a caller before the site: the value at its save (the scan goes on above the save offset): a save / restore pair
    leaves the caller's value;
  - a callee the caller calls before the site (static call / tail edges, observed indirect targets): the value it leaves at its exit,
    read the same way inside it (CDEPTH levels of callees; a callee that restores what it set, or sets nothing, leaves the value
    as found; a setter wrapper or a forwarder whose row at the site already stands for the field is not read again);
  - the colour walk sub_82C2FA10 calling the function (a node's slot 1) after its reset call (+0x90): the category reset
    sub_82C22E88's words (read from its listing: `li` / `stw rX,W(r3)`, the word -> key map of q gstate --list), unless the node's
    categories include one with no reset object (both read from docs/systems/render_categories.md at run time by q when's table
    parser: `categories` below); a field the reset does not write (the colour mask words, the stencil enable) is the recorder
    start value when q rstdiff's census settles it (every setter of it leaves it as found: tools/q/qrstdiff_inherit.py), else open;
  - no event in a function: its own callers (static call / tail edges + the callers observed in coverage runs, q seen), up to DEPTH
    levels. A path that reaches no setter (no caller found, the depth or the caller cap, a site before the reset) is OPEN.
One value on every path = the inherited value; several values, a register or an open path = UNSETTLED with the list.
Limits [L]: a set is taken by listing offset, not by path (a set on one branch before the site counts for every path); the
sequence slot 5 the colour walk runs between its reset and the nodes (+0xB4) is not read (the only alpha / blend setter at a
vtable slot 5 is Create a Finisher's sub_82270C20, category 3: ALPHAWREST review); a callee reached by an indirect call no run
observed is not read; a node the doc's table names in no row takes the reset (5 of the 71 categories have none: said on the path).
Draw positions (GINHERIT review: the structure behind the old "a callee sets it" UNSETTLED): draw_paths(fn) = every call path from
fn down to a draw recorder call (db/draw_submit.json's render_record_draw_indexed / _inline_vertices / _call_function, read by
name), over the same edges; the field AT each draw = the last event above the recorder call in its function (sets, restores,
callee exits, as above), none = the call one level up, none above fn's own call = fn's entry value; a set from the function's
own argument takes the caller's forwarder row at that call site. The colour write mask at a draw is formed from its five recorder
words (mask_combos). A path is a listing path [L, as above]: a set on one branch counts for every path through it.
codediff (tools/q/qcodediff.py run, its `inherit` hook = codediff_hook here): an OURS-ONLY row (ours sets the field, the paired
game function does not) is judged against the value(s) AT the game function's draws (its entry value when it reaches no draw
recorder; then a callee setting the field keeps the row UNSETTLED) with codediff's own judge and units: OK / DIFF / UNSETTLED
(UNSETTLED too when any path is open: a DIFF needs every path read).
Unreachable paths (S111 GPRUNE, tools/q/qgstate_prune.py): a call site whose branch needs an argument value (`cmpwi crN, rX, K` +
beq / bne ... on in_rN) that NO caller of its function passes (every caller's value set is known constants and misses it) is not
climbed into, and a draw path hop through such a site is dropped; each is printed in a `pruned:` footer, never silent. An unknown
value never prunes (a jump table on an argument is not read).
PLANT (selftest only): WWE_QGINHERIT_PLANT = no_restore | no_reset | first_path | no_callee | no_forward | no_inner | no_draws |
mask_raw | no_rekey | no_prune.
"""
import collections
import itertools
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

COLOUR_WALK = 0x82C2FA10        # the per-category colour walk (render_categories.md 'How to use it')
RESET_CALL_OFF = 0x90           # its call of the category's reset object slot 1 (ALPHAWREST claim 0x82C2FA10)
RESET_FN = 0x82C22E88           # recorder slot 38: the category reset's state words (sub_82806A90 -> sub_82BB7E88 -> slot 38)
REPO = os.path.dirname(os.path.dirname(HERE))
CAT_DOC = os.path.join(REPO, 'docs', 'systems', 'render_categories.md')   # the category table: node slot 1s, Reset, Re-keys
REKEY_RE = re.compile(r'->\s*(\d+(?:\s*/\s*\d+)*)')                       # a Re-keys cell's move: `-> 42`, `-> 41 / 43`
DRAW_DB = os.path.join(REPO, 'db', 'draw_submit.json')
DRAW_RECORDERS = ('render_record_draw_indexed', 'render_record_draw_inline_vertices', 'render_record_call_function')   # ops 15 / 19 / 68
APPLY4 = ('blend_code', 'blend_enable', 'alpha_test', 'cull')   # sub_82BD54D0 applies these four from its record
# the colour write mask's recorder words: slot 13 (+92, key colour_write_mask) and the channel slots 14..17 (+96..+108) with the
# bit each channel method records (sub_82C23980 / sub_82C23A20 / sub_82C23AC0 / sub_82C23B60: +96 -> 1, +100 -> 2, +104 -> 4,
# +108 -> 8; the first three only while +92 is non-zero)
CHANNELS = ('colour_mask_14', 'colour_mask_15', 'colour_mask_16', 'colour_mask_17')
CHANNEL_BIT = {'colour_mask_14': 1, 'colour_mask_15': 2, 'colour_mask_16': 4, 'colour_mask_17': 8}
MASK_KEYS = ('colour_write_mask',) + CHANNELS
CENSUS = {'colour_write_mask': 'cmask', 'stencil_enable': 'stencil', **{k: 'cmask' for k in CHANNELS}}   # q rstdiff's census names
FIELDS = ('z_enable', 'z_write', 'z_func', 'blend_enable', 'blend_code', 'cull', 'colour_write_mask', 'stencil_enable',
          'alpha_test', 'alpha_func', 'alpha_ref')
DEPTH, CDEPTH, CALLER_CAP, STEP_CAP = 7, 3, 40, 20000
CLOSURE_CAP, PATH_CAP = 6000, 20000     # the draw search: functions under one game function, call paths to its draws
END = 1 << 30                   # an observed tail call: at the function's exit
INF = 1 << 31
PLANT = os.environ.get('WWE_QGINHERIT_PLANT', '')


def hx(a):
    return f'0x{a:08X}'


def sub(a):
    return f'sub_{a:08X}'


def categories(text=None):
    """-> ({node slot 1: (categories, why)}, categories with no reset object, problems), read from docs/systems/render_categories.md
    (text = a planted doc) by q when's table parser (qwhen.parse_doc / layout_problems): a row's `slot 1` functions take its
    categories and every `-> N` move of its Re-keys cell [L: a row whose re-key names only some of its nodes gives the others the
    move too: more categories, so more paths open, never fewer]; a Reset cell `-` = no reset object"""
    import qwhen
    if text is None:
        try:
            with open(CAT_DOC, encoding='utf-8') as fh:
                text = fh.read()
        except OSError as e:
            return {}, frozenset(), [f'{CAT_DOC}: {e}']
    doc = qwhen.parse_doc(text)
    problems = qwhen.layout_problems(doc)
    no_reset = frozenset(c for row in doc['rows'] if row['reset'].replace('*', '').strip() == '-' for c in row['cats'])
    node = {}
    for f, rows in doc['slot1'].items():
        cats, specs = set(), []
        for row in rows:
            moved = set() if PLANT == 'no_rekey' else {int(x) for m in REKEY_RE.finditer(row['rekeys']) for x in re.split(r'\s*/\s*', m.group(1))}
            cats |= set(row['cats']) | moved
            specs.append(row['spec'].replace('`', '') + (f' (re-keyed to {", ".join(map(str, sorted(moved)))})' if moved else ''))
        node[f] = (tuple(sorted(cats)), 'render_categories.md row ' + '; '.join(specs))
    return node, no_reset, problems


# ------------------------------------------------------------------ the graph (the real one; the selftest plants its own)
class Graph:
    """rows(f) = q gstate index rows; callers(f) -> [(caller, site offset in the caller or END, how)]; callees(f) -> {offset:
    [targets]}; is_setter(t); forwards(f) -> {key: reg}; reset() -> {key: (value, offset)}; census(field) -> value or None"""
    def __init__(self, con, idx=None, observed=True):
        import qgstate
        self.con = con
        self.idx = idx if idx is not None else qgstate.load_index(con, log=lambda m: print(m, file=sys.stderr))
        self.setters, self.getters = qgstate.table()
        self._rows = {int(k, 16): v['rows'] for k, v in self.idx['functions'].items()}
        self._fwd = {int(k, 16): {e['key']: e['reg'] for e in v} for k, v in self.idx.get('forwarders', {}).items()}
        self._seen = None
        self.observed = observed
        self._callers, self._callees, self._census = {}, {}, {}
        self._reset, self._leaves, self._cats = None, None, None
        self.problems = []
        self._pruner = None

    def prune(self, fn, site, callers):
        """-> (kept callers, [prune records]) by the guard of (fn, site) against the constants the callers pass (qgstate_prune)"""
        if self._pruner is None:
            try:
                import qgstate_prune
                import qslice
                self._pruner = qgstate_prune.Pruner(qslice.QSource(self.con))
            except Exception as e:  # noqa: BLE001 - no listing reader: nothing is pruned, said
                self._pruner = False
                self.problems.append(f'unreachable-path pruning off ({type(e).__name__}: {e})')
        return self._pruner.prune(fn, site, callers) if self._pruner else (list(callers), [])

    def draw_leaves(self):
        """the draw recorders (db/draw_submit.json entries named DRAW_RECORDERS)"""
        if self._leaves is None:
            try:
                with open(DRAW_DB, encoding='utf-8') as fh:
                    entries = json.load(fh)
            except (OSError, ValueError) as e:
                entries = []
                self.problems.append(f'{DRAW_DB}: {type(e).__name__}: {e}: no draw positions (the entry state stands)')
            got = {e['name']: int(e['address'], 16) for e in entries if isinstance(e, dict) and e.get('name') in DRAW_RECORDERS}
            miss = sorted(set(DRAW_RECORDERS) - set(got))
            if miss:
                self.problems.append(f'db/draw_submit.json: no entry named {", ".join(miss)}')
            self._leaves = frozenset(got.values())
        return self._leaves

    def categories(self):
        if self._cats is None:
            self._cats = categories()
            self.problems += [f'render_categories.md: {p}' for p in self._cats[2]]
        return self._cats

    def _seen_con(self):
        if self._seen is None and self.observed:
            try:
                import qseen
                self._seen = qseen.open_seen()
            except Exception as e:  # noqa: BLE001 - no coverage table: static edges only, said
                self.problems.append(f'q seen not read ({type(e).__name__}: {e}): observed indirect callers not used')
                self.observed = False
        return self._seen

    def rows(self, f):
        return self._rows.get(f, [])

    def is_setter(self, t):
        return t in self.setters or t in self.getters

    def forwards(self, f):
        return self._fwd.get(f, {})

    def name(self, f):
        r = self.con.execute('SELECT name FROM funcs WHERE addr = ?', (f,)).fetchone()
        return r[0] if r and r[0] else sub(f)

    def callers(self, f):
        if f not in self._callers:
            out = set()
            for src, site, kind in self.con.execute("SELECT DISTINCT src, site, kind FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (f,)):
                out.add((src, site - src, kind))
            sc = self._seen_con()
            if sc is not None:
                import qseen
                for func, site, _runs in qseen.callers_of(f, sc):
                    out.add((func, site - func if site else END, 'observed'))
            self._callers[f] = sorted(out)
        return self._callers[f]

    def callees(self, f):
        if f not in self._callees:
            out = collections.defaultdict(set)
            for site, dst in self.con.execute("SELECT DISTINCT site, dst FROM edges WHERE src = ? AND kind IN ('call', 'tail')", (f,)):
                out[site - f].add(dst)
            sc = self._seen_con()
            if sc is not None:
                for site, tgt in sc.execute('select site, target from seen where func = ?', (f,)):
                    out[site - f if site else END].add(tgt)
            self._callees[f] = {k: sorted(v) for k, v in out.items()}
        return self._callees[f]

    def insns(self, f):
        import qconstdiff
        return qconstdiff._insns(self.con, f)

    def reset(self):
        if self._reset is None:
            import qrstdiff_inherit as RI
            words = RI.init_words(self.insns(RESET_FN))
            by_word = {}
            for s in list(self.setters.values()) + list(self.getters.values()):
                if s.get('word') is not None and s['key'] in FIELDS:
                    by_word.setdefault(s['word'], s['key'])
            offs = {}
            for x in self.insns(RESET_FN):
                ops = [o.strip() for o in (x.ops or '').split(',')]
                if x.mn == 'stw' and len(ops) == 2 and ops[1].endswith('(r3)'):
                    try:
                        offs.setdefault(int(ops[1][:-4], 0), x.addr - RESET_FN)
                    except ValueError:
                        pass
            self._reset = {by_word[w]: (v, offs.get(w, 0)) for w, v in words.items() if w in by_word}
            if not self._reset:
                self.problems.append(f'{hx(RESET_FN)}: no state word read from the category reset')
        return self._reset

    def census(self, field):
        """the device value the census settles (colour_write_mask, stencil_enable), or a channel word's start value (colour_mask_14..17:
        the census of the five mask words settled = every setter leaves them as found); None = not settled"""
        name = CENSUS.get(field)
        if name is None:
            return None
        if name not in self._census:
            import qrstdiff_inherit as RI
            insns_of = self.insns
            try:
                res = RI.census(name, insns_of, RI.callers_reader(self.con))
                self._census[name] = res
                self.problems += [f'census {name}: {p}' for p in res['problems']]
            except Exception as e:  # noqa: BLE001 - a census that cannot run leaves the path open, said
                self._census[name] = None
                self.problems.append(f'census {name}: {type(e).__name__}: {e}')
        res = self._census[name]
        if res is None or res['value'] is None:
            return None
        if field in CHANNELS:
            word = next((s['word'] for s in self.setters.values() if s['key'] == field), None)
            return res['start'].get(word)
        return res['value']


# ------------------------------------------------------------------ the walk
def _res(v=None, reg=None, open_=None, at=None, by='set', via=(), leaves=False, arg=None):
    return {'v': v, 'reg': reg, 'open': open_, 'at': at, 'by': by, 'via': tuple(via), 'leaves': leaves, 'arg': arg}


def _draw_text(path):
    (top, o0), (f, off) = path[0], path[-1]
    return f'{sub(top)} +{o0:X}' if len(path) == 1 else f'{sub(top)} +{o0:X} .. {sub(f)} +{off:X} ({len(path) - 1} calls down)'


def mask_combos(comps, draw=None):
    """-> results: the device colour mask (render state 53, the recorder's row 212) at one draw from the five words' results there
    (comps, in MASK_KEYS order). The channel methods record bit 1 / 2 / 4 for +96 / +100 / +104 while +92 is non-zero (0 when it is
    0) and bit 8 for +108 whatever +92 holds; slot 13 (sub_82C23910) records 15 for a non-zero +92, 0 for zero, and writes no
    channel word. Which method wrote the row last is known when only one kind was SET on the path (the other kind holds its start
    value, by the census); when both were set, both values (their order is not read). An open / register word = that result."""
    out, ok = [], []
    for c in comps:   # one result per (value, set or start) of each word: what the mask depends on (the product stays small)
        good = {}
        for p in c:
            if p['v'] is not None:
                good.setdefault((p['v'], p['by'] == 'census'), p)
        out += [dict(p, draw=draw) for p in c if p['v'] is None]
        ok.append(list(good.values()))
    for combo in itertools.product(*ok):
        gate, ch = combo[0], combo[1:]
        on = gate['v'] != 0
        bits = (sum(CHANNEL_BIT[k] for k, p in zip(CHANNELS[:3], ch[:3]) if p['v']) if on else 0) | (CHANNEL_BIT[CHANNELS[3]] if ch[3]['v'] else 0)
        set13, setch = gate['by'] != 'census', any(p['by'] != 'census' for p in ch)
        vals = {15 if on else 0} if set13 and not setch else ({bits, 15 if on else 0} if set13 else {bits})
        words = ', '.join((f'{k[-2:]} {p["v"]}' if k in CHANNELS else f'+92 {"on" if p["v"] else "off"}')
                          + (f' @{hx(p["at"])}' if p['at'] else ' (start)') for k, p in zip(MASK_KEYS, combo))
        for v in sorted(vals):
            out.append(_res(v=v, by='mask', via=(f'mask words {words}',) + (('slot 13 and a channel both set: order not read',) if len(vals) > 1 else ())) | {'draw': draw})
    return out


class Walker:
    def __init__(self, graph, depth=DEPTH, cdepth=CDEPTH):
        self.g = graph
        self.depth, self.cdepth = depth, cdepth
        self.memo, self.busy = {}, set()
        self.steps = 0
        self._dp, self._atm = {}, {}
        self.pruned, self._pseen = [], set()

    def _note(self, recs):
        for r in recs:
            k = (r['fn'], r['site'], r['caller'], r['off'])
            if k not in self._pseen:
                self._pseen.add(k)
                self.pruned.append(r)

    def _dead(self, c, off, via=None):
        """True when the call site (c, off) cannot run: every caller of c (or the one hop `via` = (caller, site) of a draw path) is
        known to miss the value its branch needs"""
        pr = getattr(self.g, 'prune', None)
        if pr is None or PLANT == 'no_prune':
            return False
        cs = [(via[0], via[1], 'call')] if via else [x for x in self.g.callers(c) if x[0] != c]
        if not cs or off >= END:
            return False
        kept, recs = pr(c, off, cs)
        if kept:
            return False
        self._note(recs)
        return True

    # events of one function for one field, latest first: ('row', off, row) | ('call', off, [targets])
    def _events(self, fn, field):
        ev, row_sites = [], set()
        for r in self.g.rows(fn):
            if r['kind'] == 'read':
                continue
            if r['key'] == field or (r['key'] == 'apply4' and field in APPLY4):
                ev.append(('row', r['off'], r))
                row_sites.add(r['off'])
        if PLANT != 'no_callee':
            for off, tg in self.g.callees(fn).items():
                if off in row_sites:
                    continue
                tg = [t for t in tg if not self.g.is_setter(t) and t != fn]
                if tg:
                    ev.append(('call', off, tg))
        ev.sort(key=lambda e: -e[1])
        return ev

    def scan(self, fn, limit, field, depth, cdepth, mode, site=None):
        """-> [result]: the field's value at offset `limit` of fn. mode 'climb': above the entry, fn's callers; 'exit': a result
        with leaves=True stands for 'as found at the entry'. site = the call site in fn the climb came through (its guard prunes callers)"""
        key = (fn, limit, field, depth, cdepth, mode, site)
        if key in self.memo:
            return self.memo[key]
        if key in self.busy:
            return []
        self.steps += 1
        if self.steps > STEP_CAP:
            return [_res(open_=f'the walk cap ({STEP_CAP} steps) reached at {sub(fn)}', via=(sub(fn),))]
        self.busy.add(key)
        try:
            out = self._scan(fn, limit, field, depth, cdepth, mode, site)
        finally:
            self.busy.discard(key)
        self.memo[key] = out
        return out

    def _scan(self, fn, limit, field, depth, cdepth, mode, site=None):
        me = sub(fn)
        for kind, off, x in self._events(fn, field):
            if off >= limit:
                continue
            if kind == 'row':
                r = x
                if r['kind'] == 'restore' and PLANT != 'no_restore':
                    return self.scan(fn, r['restore'], field, depth, cdepth, mode, site)
                if r['key'] == 'apply4':
                    return [_res(reg=f'apply4 record ({me} +{off:X})', at=fn + off, via=(me,))]
                if r['kind'] == 'restore':
                    return [_res(reg=f'restore of +{r["restore"]:X} (pairing off)', at=fn + off, via=(me,))]
                a = r['args'].get(r['value_reg']) or {}
                if a.get('imm') is not None:
                    return [_res(v=v, at=fn + off, via=(me,)) for v in a['imm']]
                root = a.get('root') or '?'
                if root.startswith('in_r') and mode == 'climb' and PLANT != 'no_forward':
                    return [dict(p, via=(me,) + p['via']) for p in self._forwarded(fn, root[3:], field, depth, cdepth)]
                return [_res(reg=root, at=fn + off, via=(me,), arg=(fn, root) if root.startswith('in_r') else None)]
            effs, leave = [], False
            for t in x:
                e = self.exit_effect(t, field, cdepth - 1)
                if e is None:
                    leave = True
                    continue
                for p in e:
                    if p['leaves']:
                        leave = True
                    else:
                        effs.append(dict(p, via=(f'{me} +{off:X} calls {sub(t)}',) + p['via'][1:] if p['via'] else (f'{me} +{off:X} calls {sub(t)}',)))
            if not effs:
                continue
            if leave:
                effs += self.scan(fn, off, field, depth, cdepth, mode)
            return effs
        if mode == 'exit':
            return [_res(leaves=True)]
        return [dict(p, via=(me,) + p['via']) for p in self.entry(fn, field, depth, site)]

    def _forwarded(self, fn, reg, field, depth, cdepth):
        """fn sets the field from its own argument rK: each caller's value of rK, from its forwarder row at the call site"""
        out = []
        for c, off, _how in self.g.callers(fn):
            rows = [r for r in self.g.rows(c) if r.get('via') == fn and r['off'] == off and r['key'] == field and r['kind'] != 'read']
            if rows:
                a = rows[0]['args'].get(rows[0]['value_reg']) or {}
                if a.get('imm') is not None:
                    out += [_res(v=v, at=c + off, by='argument', via=(sub(c),)) for v in a['imm']]
                else:
                    out.append(_res(reg=a.get('root') or '?', at=c + off, via=(sub(c),)))
            else:
                out.append(_res(open_=f'{sub(c)} +{off:X} passes {reg} (no forwarder row read)', via=(sub(c),)))
        return out or [_res(open_=f'{sub(fn)} sets it from its argument {reg}; no caller found')]

    def exit_effect(self, t, field, cdepth):
        """None = leaves the field as found; else [result] (one may be leaves=True: some paths leave it)"""
        if cdepth < 0:
            return None
        got = self.scan(t, INF, field, 0, cdepth, 'exit')
        if all(p['leaves'] for p in got):
            return None
        return got

    def entry(self, fn, field, depth, site=None):
        if depth <= 0:
            return [_res(open_=f'the depth limit ({self.depth} caller levels) reached at {sub(fn)}')]
        cs = [c for c in self.g.callers(fn) if c[0] != fn]
        if site is not None and PLANT != 'no_prune' and hasattr(self.g, 'prune'):   # the branch to the call site we came through
            cs, recs = self.g.prune(fn, site, cs)
            self._note(recs)
            if not cs and recs:
                return []
        if PLANT == 'first_path':
            cs = cs[:1]
        if not cs:
            return [_res(open_=f'no caller of {sub(fn)} found (static call / tail edges, observed indirect callers)')]
        if len({c for c, _o, _h in cs}) > CALLER_CAP:
            return [_res(open_=f'{sub(fn)} has {len({c for c, _o, _h in cs})} callers (over the cap {CALLER_CAP})')]
        out = []
        for c, off, how in cs:
            if c == COLOUR_WALK:
                out += self.reset_path(fn, field, off)
                continue
            if self._dead(c, off):
                continue
            out += self.scan(c, off, field, depth - 1, self.cdepth, 'climb', off)
        return out or [_res(open_=f'only recursive callers of {sub(fn)}')]

    def reset_path(self, node, field, off):
        walk = f'{sub(COLOUR_WALK)} +{off:X} (the colour walk)' if off != END else f'{sub(COLOUR_WALK)} (the colour walk)'
        if PLANT == 'no_reset':
            return [_res(open_='the colour walk (reset not read)', via=(walk,))]
        if off <= RESET_CALL_OFF:
            return [_res(open_=f'{walk}: called before its category reset (+{RESET_CALL_OFF:X})', via=(walk,))]
        node_cats, no_reset, _p = self.g.categories()
        cats, why = node_cats.get(node, (None, None))
        if cats:
            bad = sorted(set(cats) & no_reset)
            if bad:
                return [_res(open_=f'{walk}: category {", ".join(map(str, bad))} has no reset object (the previous category\'s end, not read)', via=(walk,))]
        note = (f'categories {_cats(cats)}: {why}' if cats else
                f'categories not read [L]: no render_categories.md row names {sub(node)}; {" / ".join(map(str, sorted(no_reset)))} have no reset')
        rv = self.g.reset()
        if field in rv:
            v, woff = rv[field]
            return [_res(v=v, at=RESET_FN + woff, by='reset', via=(walk, f'category reset {sub(RESET_FN)} +{woff:X} ({note})'))]
        cv = self.g.census(field)
        if cv is not None:
            return [_res(v=cv, by='census', via=(walk, f'the reset does not write it: the recorder start value, every setter leaves it as found (qrstdiff_inherit census; {note})'))]
        return [_res(open_=f'{walk}: the category reset does not write {field}; the previous category\'s end (not read)', via=(walk,))]

    def callee_sets(self, fn, field, cdepth=None):
        """-> 'sub_X +OFF calls sub_Y' of the first callee (CDEPTH levels down) with a set / restore of the field, else None: the
        function's draw may run inside that callee's set section, where the entry state is not the draw's"""
        cdepth = self.cdepth if cdepth is None else cdepth
        seen, level = {fn}, [(fn, '')]
        for _ in range(cdepth):
            nxt = []
            for f, path in level:
                for off, tg in sorted(self.g.callees(f).items()):
                    for t in tg:
                        if t in seen or self.g.is_setter(t):
                            continue
                        seen.add(t)
                        p = f'{path}{sub(f)} +{off:X} calls {sub(t)}' if not path else f'{path} -> {sub(t)}'
                        if any(r['key'] == field and r['kind'] in ('set', 'restore') and not r.get('via') for r in self.g.rows(t)):
                            return p
                        nxt.append((t, p))
            level = nxt
        return None

    def results(self, fn, field):
        """the field at fn's ENTRY (its own sets are its gstate facts, not what it inherits); the step cap is per question"""
        self.steps = 0
        return self.scan(fn, 0, field, self.depth, self.cdepth, 'climb')

    def draw_paths(self, fn):
        """-> (paths, problem): every call path from fn down to a draw recorder call (Graph.draw_leaves), over the static call / tail
        edges and the observed indirect callees, setter wrappers and recursion left out: ((fn, off), (callee, off), ..., (f, off of
        its recorder call)). problem = why the list is not whole (CLOSURE_CAP / PATH_CAP)."""
        if fn in self._dp:
            return self._dp[fn]
        leaves = self.g.draw_leaves() if hasattr(self.g, 'draw_leaves') else frozenset()
        if PLANT == 'no_draws' or not leaves:
            return [], None
        order, seen, edges, i = [fn], {fn}, {}, 0
        while i < len(order):          # the functions under fn
            f = order[i]
            i += 1
            es = []
            for off, tg in self.g.callees(f).items():
                for t in tg:
                    if t in leaves:
                        es.append((off, t))
                    elif t != f and not self.g.is_setter(t):
                        es.append((off, t))
                        if t not in seen:
                            seen.add(t)
                            order.append(t)
            edges[f] = sorted(es)
            if len(order) > CLOSURE_CAP:
                self._dp[fn] = ([], f'{sub(fn)}: over {CLOSURE_CAP} functions under it (the draw search stopped)')
                return self._dp[fn]
        reach, changed = set(), True   # those that reach a recorder call
        while changed:
            changed = False
            for f in reversed(order):
                if f not in reach and any(t in leaves or t in reach for _o, t in edges[f]):
                    reach.add(f)
                    changed = True
        paths = []

        def walk(f, stack, prefix):
            for off, t in edges[f]:
                if len(paths) >= PATH_CAP:
                    return
                if prefix and self._dead(f, off, prefix[-1]):
                    continue
                if t in leaves:
                    paths.append(prefix + ((f, off),))
                elif t in reach and t not in stack:
                    walk(t, stack | {t}, prefix + ((f, off),))
        if fn in reach:
            walk(fn, frozenset({fn}), ())
        self._dp[fn] = (paths, f'{sub(fn)}: over {PATH_CAP} call paths to a draw (the rest not read)' if len(paths) >= PATH_CAP else None)
        return self._dp[fn]

    def _entry(self, fn, field):
        return self.scan(fn, 0, field, self.depth, self.cdepth, 'climb')

    def _at(self, path, i, field):
        """the field at path[i]'s call: the last event above it in its function; none = the call above it (path[i - 1]), above the
        first one fn's entry; a set from the function's own argument takes the caller's forwarder row at that call site"""
        key = (path[:i + 1], field)
        if key in self._atm:
            return self._atm[key]
        f, off = path[i]
        out = []
        for p in self.scan(f, off, field, 0, self.cdepth, 'exit'):
            if p['leaves']:
                out += self._at(path, i - 1, field) if i > 0 else self._entry(f, field)
            elif p.get('arg') and p['arg'][0] == f and PLANT != 'no_forward':
                reg = p['arg'][1][3:]
                if i == 0:
                    out += [dict(q, via=(sub(f),) + q['via']) for q in self._forwarded(f, reg, field, self.depth, self.cdepth)]
                    continue
                c, coff = path[i - 1]
                rows = [r for r in self.g.rows(c) if r.get('via') == f and r['off'] == coff and r['key'] == field and r['kind'] != 'read']
                a = (rows[0]['args'].get(rows[0]['value_reg']) or {}) if rows else {}
                if a.get('imm') is not None:
                    out += [_res(v=v, at=c + coff, by='argument', via=(f'{sub(c)} +{coff:X} passes {reg} to {sub(f)}',)) for v in a['imm']]
                else:
                    out.append(_res(reg=(a.get('root') or '?') if rows else f'{reg} ({sub(c)} +{coff:X}: no forwarder row read)', at=c + coff, via=(sub(c),)))
            else:
                out.append(p)
        self._atm[key] = out
        return out

    def draw_summary(self, fn, field):
        """-> summary() of the field AT every draw under fn (draw_paths), 'draws' = their count; None when fn reaches no draw recorder"""
        paths, problem = self.draw_paths(fn)
        if not paths and not problem:
            return None
        self.steps = 0
        got = [dict(p, draw=_draw_text(pth)) for pth in paths for p in self._at(pth, len(pth) - 1, field)]
        if problem:
            got.append(_res(open_=problem))
        return dict(self._summ(got), draws=len(paths))

    def mask_summary(self, fn):
        """-> summary() of the device colour write mask at every draw under fn (at its entry when it reaches none): mask_combos of the
        five words there"""
        paths, problem = self.draw_paths(fn)
        self.steps = 0
        got = []
        for pth in (paths or ([] if problem else [None])):
            comps = [self._at(pth, len(pth) - 1, k) if pth else self._entry(fn, k) for k in MASK_KEYS]
            got += mask_combos(comps, _draw_text(pth) if pth else None)
        if problem:
            got.append(_res(open_=problem))
        return dict(self._summ(got), draws=len(paths))

    def summary(self, fn, field):
        """-> {'settled': bool, 'value': v or None, 'paths': [distinct results]}"""
        return self._summ(self.results(fn, field))

    @staticmethod
    def _summ(got):
        got = got or [_res(open_='no path')]
        seen, paths = set(), []
        for p in got:
            k = (p['v'], p['reg'], p['open'], p['at'])
            if k not in seen:
                seen.add(k)
                paths.append(p)
        vals = {p['v'] for p in paths}
        settled = all(p['v'] is not None for p in paths) and len(vals) == 1
        return {'settled': settled, 'value': next(iter(vals)) if settled else None, 'paths': paths}


def _cats(cats):
    cats = sorted(cats)
    runs, i = [], 0
    while i < len(cats):
        j = i
        while j + 1 < len(cats) and cats[j + 1] == cats[j] + 1:
            j += 1
        runs.append(str(cats[i]) if i == j else f'{cats[i]}..{cats[j]}')
        i = j + 1
    return ', '.join(runs)


# ------------------------------------------------------------------ text / facts
def path_text(p, setter=None):
    import qgstate
    if p['v'] is not None:
        val = qgstate.value_text(setter, [p['v']]) if setter else str(p['v'])
    elif p['reg'] is not None:
        val = f'reg {p["reg"]}'
    else:
        val = f'open: {p["open"]}'
    at = f' @{hx(p["at"])}' if p['at'] else ''
    return (f'{val}{at}' + (f'  via {" <- ".join(p["via"])}' if p['via'] else '')
            + (f'  [at the draw {p["draw"]}]' if p.get('draw') else ''))


def facts(walker, fn, fields=FIELDS):
    """the `inherited` facts of one function (q gstate --facts form): value = the constant, or {'unsettled': [paths]}"""
    out = []
    for fld in fields:
        s = walker.summary(fn, fld)
        if s['settled']:
            p = s['paths'][0]
            out.append({'side': 'game', 'fn': sub(fn), 'field': fld, 'value': s['value'], 'cond': None, 'at': hx(p['at']) if p['at'] else None,
                        'kind': 'inherited', 'via': ' <- '.join(p['via']), 'by': p['by']})
        else:
            out.append({'side': 'game', 'fn': sub(fn), 'field': fld, 'cond': None, 'at': None, 'kind': 'inherited',
                        'value': {'unsettled': [{'value': p['v'], 'reg': p['reg'], 'open': p['open'], 'at': hx(p['at']) if p['at'] else None,
                                                 'via': ' <- '.join(p['via'])} for p in s['paths']]}})
    return out


def main(con, funcs, fields=None, depth=DEPTH, as_json=False, cap=6):
    import qgstate
    g = Graph(con)
    w = Walker(g, depth=depth)
    fields = fields or FIELDS
    by_key = {s['key']: s for s in g.setters.values() if not s.get('via')}
    if as_json:
        out = [f for fn in funcs for f in facts(w, fn, fields)]
        print(json.dumps({'inherited': out, 'pruned': [dict(r, fn=sub(r['fn']), caller=sub(r['caller'])) for r in w.pruned], 'problems': g.problems}, indent=1))
        return 0
    for fn in funcs:
        print(f'q gstate --inherit {g.name(fn)} ({hx(fn)}): the state in force when it runs (callers: static call / tail edges + observed '
              f'indirect; depth {depth}; callees {CDEPTH} levels; the colour walk\'s category reset {sub(RESET_FN)})')
        for fld in fields:
            s = w.summary(fn, fld)
            st = by_key.get(fld)
            if s['settled']:
                print(f'  {fld:18s} {qgstate.value_text(st, [s["value"]]) if st else s["value"]}  inherited on {len(s["paths"])} path(s): '
                      + path_text(s['paths'][0]))
                for p in s['paths'][1:cap]:
                    print(f'  {"":18s}   also {path_text(p)}')
                continue
            print(f'  {fld:18s} UNSETTLED ({len(s["paths"])} paths)')
            for p in s['paths'][:cap]:
                print(f'  {"":18s}   {path_text(p, st)}')
            if len(s['paths']) > cap:
                print(f'  {"":18s}   ... {len(s["paths"]) - cap} more paths (--json for all)')
        for fld in fields:   # the value AT its draws (what q codediff judges an OURS-ONLY row by)
            d = w.mask_summary(fn) if fld == 'colour_write_mask' else w.draw_summary(fn, fld)
            if not d or not d.get('draws'):
                continue
            st = None if fld == 'colour_write_mask' else by_key.get(fld)
            head = (qgstate.value_text(st, [d['value']]) if st else f'0x{d["value"]:X}' if isinstance(d['value'], int) else str(d['value'])) if d['settled'] else f'UNSETTLED ({len(d["paths"])} paths)'
            print(f'  {fld:18s} at its {d["draws"]} draw(s): {head}')
            for p in d['paths'][:cap if not d['settled'] else 1]:
                print(f'  {"":18s}   {path_text(p, st)}')
    import qgstate_prune
    ft = qgstate_prune.footer(w.pruned, g.name)
    if ft:
        print(ft)
    for p in g.problems:
        print('PROBLEM: ' + p)
    return 0


# ------------------------------------------------------------------ codediff
def codediff_hook(units, judge, moot_map, derived, walker=None):
    """-> inherit(g, fld, ours_values, game_enable_values) -> (verdict, why, extra, info) for one OURS-ONLY row of q codediff.
    The inherited values are put in codediff's compared space by its Units.game (a blend code gives op / src / dst); the verdict is
    codediff's judge; any open path makes OK / DIFF UNSETTLED (a DIFF needs every path read). A field whose enable codediff's MOOT
    names (z func / write by the z test, the factors by the blend, alpha func / ref by the alpha test) is moot when the game does
    not set the enable here and its inherited value is off."""
    state = {'w': walker}

    def w():
        if state['w'] is None:
            import q as Q
            state['w'] = Walker(Graph(Q.connect(Q.DB)))
        return state['w']

    def conv(key, fld, v):
        got = [cv for f, cv in units.game(key, v) if f == fld]
        return got[0] if got else v

    def at_draws(fn, key):
        """the field at fn's draws, else (no draw recorder under it) at its entry"""
        s = w().draw_summary(fn, key)
        return s if s is not None else w().summary(fn, key)

    def inherit(g, fld, ours_vals, game_enable=None):
        n0 = len(w().pruned)
        verdict, why, extra, info = inherit0(g, fld, ours_vals, game_enable)
        new = w().pruned[n0:]   # the paths this question's walk dropped (a memoised walk drops none again: the first question shows them)
        if new:
            import qgstate_prune
            ft = qgstate_prune.footer(new, lambda a: sub(a))
            info['pruned'] = ft
            why = f'{why}; {ft}'
        return verdict, why, extra, info

    def inherit0(g, fld, ours_vals, game_enable=None):
        fn = int(g[4:], 16)
        en = moot_map.get(fld)
        if en and not game_enable:
            se = at_draws(fn, en)
            if se['settled'] and conv(en, en, se['value']) == 0:
                p = se['paths'][0]
                return ('OK', f'moot: {en} is off {"at its draws" if se.get("draws") else "inherited"} here ({path_text(p)})', set(),
                        {'field': en, 'settled': True, 'value': 0, 'via': ' <- '.join(p['via']), 'draws': se.get('draws', 0)})
        key = 'blend_code' if fld in derived else fld
        mask = fld == 'colour_write_mask' and PLANT != 'mask_raw'   # the device mask from its five words (mask_combos)
        s = w().mask_summary(fn) if mask else at_draws(fn, key)
        vals, opens = [], []
        for p in s['paths']:
            if p['v'] is not None:
                vals.append(p['v'] if mask else conv(key, fld, p['v']))
            elif p['reg'] is not None:
                vals.append(('reg', p['reg']))
            else:
                opens.append(p)
        n_draws = s.get('draws', 0)
        info = {'field': key, 'settled': s['settled'], 'paths': [path_text(p) for p in s['paths'][:6]], 'n_paths': len(s['paths']), 'draws': n_draws}
        where = f'at its {n_draws} draw(s)' if n_draws else 'inherited'
        if not vals:
            return 'UNSETTLED', f'{where}: no path read to a setter ({len(opens)} open: {path_text(opens[0])})', set(), info
        verdict, why, extra = judge(fld, list(dict.fromkeys(vals)), ours_vals)
        shown = '; '.join(path_text(p) for p in s['paths'][:2]) + (f' (+{len(s["paths"]) - 2} paths)' if len(s['paths']) > 2 else '')
        why = f'{where} ({shown}): {why or "ours matches"}'
        if opens and verdict in ('OK', 'DIFF'):
            info['would_be'] = verdict
            why = (f'{why}; but {len(opens)} of {len(s["paths"])} paths are open ({opens[0]["open"]})'
                   + ('; ours is outside every value read' if verdict == 'DIFF' else ''))
            verdict, extra = 'UNSETTLED', set()
        inner = None
        if verdict in ('OK', 'DIFF') and not n_draws and PLANT != 'no_inner':   # no draw found under it: its entry value stands only if no callee sets it
            inner = next((x for x in (w().callee_sets(fn, k) for k in (MASK_KEYS if mask else (key,))) if x), None)
        if inner:
            info['would_be'] = verdict
            why = (f'{why}; but no draw recorder call was found under it and a callee sets it ({inner}): the draw may run inside its set '
                   'section, where the entry state is not the draw\'s' + ('; ours is outside every value read' if verdict == 'DIFF' else ''))
            verdict, extra = 'UNSETTLED', set()
        return verdict, why, extra, info
    return inherit

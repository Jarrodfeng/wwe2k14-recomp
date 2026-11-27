"""qenum: the game's named constants (db/facts/enums.json) printed where an agent reads the number, and a node's draw state computed
from the listings (S111 Lane D ENUMS + NODE STATE, docs/research/s111/INDEX_PLAN.md T4).

    py tools/q/q.py enum [NAME [VALUE ...]] [--json]    the enum tables (render_category, blend_code, alpha_func, z_func, match_type, scene_type, ...)
    py tools/q/q.py nodes CAT --state                    (qx7.py hook) per node: the recorder-wrapper calls of its draw method, in order, values named
    q decomp / q asm                                     `; [enum] ...` comments at a call with a literal r4 that the table names (qfactcard provider)

Data: db/facts/enums.json (our words and addresses; every enum / row cites its page and carries [C] / [L]). `from` enums are read from their
own file (db/ids/match_types.json), never copied.
State: per slot-1 function of a node, qrstdiff's path-wise literal flow (R.calls) over the recorder wrappers (the `calls` rows with `state`).
The rows list every call in listing order (a call on a branch may not run); the `exit, per path` line is what reaches the function's returns
and tail exits on each path (R.flow with a whole-function window): `inherited` = a path that sets that field nothing (S111 INDEX-D review).
A non-literal r4 that is the result of a recorder getter (sub_82BB82F0 = z write word +84, sub_82BB8398 = cull +112) reads as `restored`
(the draw function saved the state and puts it back). A function with no recorder call that is tiny (<= THUNK_MAX insns) is a thunk: its
branch / call targets are followed (3 levels); a bigger one with none shows the direct callees' state (`via`). A function WITH recorder calls
shows only its own: the callees it calls are not read (said on the line). Indirect calls (bctrl / bctr) are never followed: a `none` line
counts them, and the direct callees it read (an unreadable one is named, not counted as read). Marks: the row's own ([C] code read; the
follow rule is [L tool]).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
REPO = os.path.dirname(os.path.dirname(HERE))
PATH = os.path.join(REPO, 'db', 'facts', 'enums.json')
THUNK_MAX = 16        # a function with no recorder call and at most this many instructions only forwards: follow its targets
THUNK_DEPTH = 3
CALLEE_MAX = 40       # direct callees read for a function that has no recorder call and is not a thunk
TEXT_MAX = 96

_DATA = {}


class EnumError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


def load(path=None):
    """-> the parsed enums.json with every `from` enum filled in (cached by path)."""
    p = path or PATH
    if p in _DATA:
        return _DATA[p]
    with open(p, encoding='utf-8') as f:
        d = json.load(f)
    for nm, e in d['enums'].items():
        src = e.get('from')
        if not src:
            continue
        fp = os.path.join(REPO, src['file'])
        try:
            with open(fp, encoding='utf-8') as f:
                rows = json.load(f)[src['path']]
        except (OSError, KeyError, ValueError) as ex:
            raise EnumError(f'enum {nm}: cannot read {src["file"]}: {ex}')
        vals = {}
        for r in rows:
            k = str(r[src['key']])
            if k in vals:
                continue
            paths = r.get(src.get('path_field') or '', None) or []
            vals[k] = {'name': r[src['name']] + (f' ({paths[0]})' if paths else ''),
                       'mark': src['mark_high'] if r.get(src.get('mark_field') or '') == 'high' else 'L'}
        e['values'] = vals
    d['_calls'] = {int(c['fn'], 16): c for c in d['calls']}
    d['_getters'] = {int(g['fn'], 16): g for g in d['getters']}
    _DATA[p] = d
    return d


def enum_row(name, v, d=None):
    """-> (name, mark, source) of value v in enum `name`, or None."""
    d = d or load()
    e = d['enums'].get(name)
    if e is None:
        return None
    r = e['values'].get(str(v))
    if r is None:
        return None
    return r['name'], r.get('mark', e['mark']), r.get('source', e['source'])


def short(name):
    """the first words of a value name, cut before its parenthesis (a blend code's op / factor list)"""
    return name.split(' (')[0]


def _lit_list(v):
    """r4 as R.calls gives it (int, frozenset, None) -> sorted list of ints, or None"""
    if v is None:
        return None
    return sorted(v) if isinstance(v, frozenset) else [v]


def value_text(enum, vals, d=None):
    """[ints] -> '1 additive (...)' (several: '0 ... | 1 ...'), the name from the enum, `(not in the table)` for an unknown value"""
    out = []
    for v in vals:
        r = enum_row(enum, v, d) if enum else None
        out.append(f'{v} {r[0]}' if r else (f'{v}' if not enum else f'{v} (not in the {enum} table)'))
    return ' | '.join(out)


def _mark_of(vals, enum, c, d):
    ms = {(enum_row(enum, v, d) or (None, c['mark']))[1] for v in vals} if enum else {c['mark']}
    return 'L' if 'L' in ms else c['mark']


# ------------------------------------------------------------------ the state of one function
def _expr_text(dlines, off):
    """the r4 argument text of the call at `off` in the decomp lines (`ret#144`, `r28@826F9254`, ...), or None"""
    for l in dlines or ():
        m = re.search(r'\+%X\s+call#%X\s.*?\br4=([^,)\s]+)' % (off, off), l)
        if m:
            return m.group(1)
    return None


def _ret_target(dlines, tag):
    """`ret#144` -> the address of the function called at +144, from the decomp lines"""
    m = re.match(r'ret#([0-9A-Fa-f]+)$', tag or '')
    if not m:
        return None
    for l in dlines or ():
        mm = re.search(r'\+%s\s+call#%s\s+sub_([0-9A-Fa-f]{8})' % (m.group(1), m.group(1)), l)
        if mm:
            return int(mm.group(1), 16), int(m.group(1), 16)
    return None


def state_rows(insns, fn, dlines_of=None, d=None):
    """[{off, what, slot, vals, text, mark, restore}] in instruction order: the recorder-wrapper calls (`state` rows) of one function."""
    import qrstdiff as R
    d = d or load()
    targets = {a: c['what'] for a, c in d['_calls'].items() if c['state']}
    if not any(x.mn == 'bl' for x in insns):
        return []
    rows = []
    dl = None
    for off, tgt, v in R.calls(insns, fn, targets):
        c = d['_calls'][tgt]
        vals = _lit_list(v)
        row = {'off': off, 'what': c['what'], 'slot': c['slot'], 'vals': vals, 'restore': None, 'mark': c['mark']}
        if vals is not None:
            row['text'] = value_text(c['enum'], vals, d)
            row['mark'] = _mark_of(vals, c['enum'], c, d)
        else:
            if dl is None:
                dl = (dlines_of() if dlines_of else None) or []
            expr = _expr_text(dl, off)
            rt = _ret_target(dl, expr)
            g = d['_getters'].get(rt[0]) if rt else None
            if g is not None and g['what'] == c['what']:
                row['text'] = f'restored: the {g["what"]} read at +{rt[1]:X} (sub_{rt[0]:08X})'
                row['restore'] = rt[1]
                row['mark'] = 'L' if 'L' in (g['mark'], c['mark']) else 'C'
            else:
                row['text'] = f'r4 = {expr or "?"} (not a literal)'
        rows.append(row)
    return rows


def _is_exit(x, own):
    """a return (blr, beqlr ...), a tail through the ctr (bctr) or a branch out of the function: the state there is handed on"""
    import qrstdiff as R
    mn = x.mn.rstrip('+-')
    if mn == 'bl' or mn.endswith(('lrl', 'ctrl')):
        return False
    if mn.startswith('b') and mn.endswith(('lr', 'ctr')):
        return True
    t = R._target(x)
    return t is not None and t not in own


def _indirect(insns):
    """the number of indirect calls / tails (bctrl, bctr, beqctr ...) in a listing: never followed"""
    return sum(1 for x in insns if x.mn.rstrip('+-').startswith('b') and x.mn.rstrip('+-').endswith(('ctr', 'ctrl')))


def exit_text(insns, fn, rows, d=None):
    """one line: per field the rows set, what reaches the function's exits on each path (S111 INDEX-D review: never the listing order).
    `restored` = the getter's value put back, `?` = a non-literal, `inherited` = a path that sets the field nothing."""
    import qrstdiff as R
    d = d or load()
    targets = {a: c['what'] for a, c in d['_calls'].items() if c['state']}
    enum_of = {c['what']: c['enum'] for c in d['_calls'].values()}
    byoff = {r['off']: r for r in rows}
    order = []
    for r in rows:
        if r['what'] not in order:
            order.append(r['what'])
    state, _idx = R.flow(insns, fn, targets, (0, 1 << 30))
    own = {x.addr for x in insns}
    ends = {w: set() for w in order}
    n = 0
    for i, x in enumerate(insns):
        if state[i] is None or not _is_exit(x, own):
            continue
        n += 1
        for w in order:
            ends[w] |= state[i].get(('S', w), frozenset({(R.INH, None)}))
    if not n:
        return 'exit: no return or tail exit in the listing'

    def label(v, off):
        if v == R.INH:
            return 'inherited'
        if v is None:
            r = byoff.get(off)
            return 'restored' if r is not None and r['restore'] is not None else '?'
        return short(value_text(enum_of.get(w), [v], d))

    parts = []
    for w in order:
        labs = []
        for v, off in sorted(ends[w], key=lambda vo: (vo[1] is None, vo[1] or 0, str(vo[0]))):
            t = label(v, off)
            if t not in labs:
                labs.append(t)
        parts.append(f'{w} ' + ' | '.join(labs))
    return f'exit, per path ({n} exit(s)): ' + '; '.join(parts)


# ------------------------------------------------------------------ a node's draw method: thunks and callees
def _branch_targets(insns, fn, is_func, d):
    """function addresses the listing calls (bl) or tails into (b), in order, once each; not the wrappers / getters, not itself"""
    own = {x.addr for x in insns}
    seen, out = set(), []
    for x in insns:
        mn = x.mn.rstrip('+-')
        if mn not in ('bl', 'b'):
            continue
        try:
            t = int((x.ops or '').split(',')[0].strip(), 16)
        except ValueError:
            continue
        if t in own or t == fn or t in seen or t in d['_calls'] or t in d['_getters'] or not is_func(t):
            continue
        seen.add(t)
        out.append(t)
    return out


def chase(fn, insns_of, dlines_of, is_func, d=None, depth=0, via=()):
    """-> [segment {fn, via, rows, how}] for a node's draw method (see the module doc)"""
    d = d or load()
    insns = insns_of(fn)
    rows = state_rows(insns, fn, (lambda: dlines_of(fn)) if dlines_of else None, d)
    if rows:
        return [{'fn': fn, 'via': list(via), 'rows': rows, 'how': 'own', 'exit': exit_text(insns, fn, rows, d)}]
    tg = _branch_targets(insns, fn, is_func, d)
    ind = _indirect(insns)
    if len(insns) <= THUNK_MAX and depth < THUNK_DEPTH:
        out = []
        for t in tg:
            try:
                out += chase(t, insns_of, dlines_of, is_func, d, depth + 1, via + (fn,))
            except Exception as e:  # noqa: BLE001 - a target with no listing (an import) is said on its own line, the node's other targets still read
                out.append({'fn': t, 'via': list(via) + [fn], 'rows': [], 'how': 'none', 'error': f'no listing ({type(e).__name__}: {e})'})
        return out or [{'fn': fn, 'via': list(via), 'rows': [], 'how': 'none', 'callees': 0, 'read': 0, 'unreadable': 0, 'indirect': ind}]
    out = []
    nread = bad = 0
    for t in tg[:CALLEE_MAX]:
        try:
            ti = insns_of(t)
            r = state_rows(ti, t, (lambda t=t: dlines_of(t)) if dlines_of else None, d)
        except Exception:      # noqa: BLE001 - a callee whose listing cannot be read is counted as unreadable, never as read
            bad += 1
            continue
        nread += 1
        if r:
            out.append({'fn': t, 'via': list(via) + [fn], 'rows': r, 'how': 'callee', 'exit': exit_text(ti, t, r, d)})
    return out or [{'fn': fn, 'via': list(via), 'rows': [], 'how': 'none', 'callees': len(tg), 'read': nread, 'unreadable': bad, 'indirect': ind}]


def segment_lines(seg, indent='      '):
    chain = ' -> '.join(f'sub_{a:08X}' for a in seg['via'] + [seg['fn']])
    if seg.get('error'):
        return [f'{indent}state: NOT READ for {chain}: {seg["error"]}']
    if seg['how'] == 'none':
        nc = seg.get('callees', 0)
        t = f'{indent}state: no recorder call in {chain}' + (f' nor in {seg.get("read", nc)} of its {nc} direct callee(s)' if nc else ' (no direct callee)')
        if seg.get('unreadable'):
            t += f' ({seg["unreadable"]} unreadable)'
        if nc > CALLEE_MAX:
            t += f' (the first {CALLEE_MAX} read)'
        if seg.get('indirect'):
            t += f'; {seg["indirect"]} indirect call(s) (bctrl / bctr) not followed'
        return [t + ' [L tool: one callee level read; deeper callees not read]']
    head = {'own': 'state', 'callee': 'state of a callee'}[seg['how']]
    out = [f'{indent}{head} ({chain}):']
    for r in seg['rows']:
        slot = f' (slot {r["slot"]})' if r['slot'] is not None else ''
        out.append(f'{indent}  +{r["off"]:X}  {r["what"]}{slot} = {r["text"]} [{r["mark"]}]')
    out.append(f'{indent}  {seg.get("exit") or "exit: not read"}' + ('  (callees of this function not read)' if seg['how'] == 'own' else ''))
    return out


def node_state(con, slot1, d=None):
    """the segments for the node whose slot-1 function is `slot1` (an int), through q's index; [] when its listing cannot be read"""
    import qrstdiff as R
    cache = R.insns_reader(con)
    dl_cache = {}

    def dlines_of(a):
        if a not in dl_cache:
            import qdecomp
            try:
                dl_cache[a] = qdecomp.decompile(con, a)['lines']
            except Exception:  # noqa: BLE001 - a function the decompiler refuses has no argument text
                dl_cache[a] = []
        return dl_cache[a]

    def is_func(a):
        return con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone() is not None

    try:
        return chase(slot1, cache, dlines_of, is_func, d)
    except Exception as e:  # noqa: BLE001 - said, never hidden
        return [{'fn': slot1, 'via': [], 'rows': [], 'how': 'none', 'callees': 0, 'error': f'{type(e).__name__}: {e}'}]


# ------------------------------------------------------------------ the decomp / asm provider
def provider(ctx, insns=None, d=None):
    """qfactcard provider: a literal r4 at a call the table names -> `[enum] r4=1 blend code 1 additive ... [C]` on that call"""
    import qfactcard
    import qrstdiff as R
    d = d or load()
    if insns is None:
        if ctx.con is None:
            return []
        insns = R.CD._insns(ctx.con, ctx.addr)
    if not any(x.mn == 'bl' for x in insns):
        return []
    facts = []
    for off, tgt, v in R.calls(insns, ctx.addr, d['_calls']):
        vals = _lit_list(v)
        c = d['_calls'][tgt]
        if vals is None or not c['enum']:
            continue
        text = f'{c["what"]}: {c["arg"]}={value_text(c["enum"], vals, d)} [{_mark_of(vals, c["enum"], c, d)}]'
        if len(text) > TEXT_MAX:
            text = text[:TEXT_MAX - 3] + '...'
        facts.append(qfactcard.Fact('enum', text, off, None, 60, 2))
    return facts


def _register():
    import qfactcard
    qfactcard.register('enum', provider, where=('decomp', 'asm'))


_register()


# ------------------------------------------------------------------ q enum
def main(argv, con=None):
    as_json = '--json' in argv
    argv = [a for a in argv if a != '--json']
    d = load()
    if argv and argv[0] == 'state':      # q enum state FUNC...: the draw state of a slot-1 function (a node q nodes cannot place, e.g. a category from a register)
        if con is None or len(argv) < 2:
            print('q enum state FUNC [FUNC ...]  (sub_XXXXXXXX or 0xADDR)', file=sys.stderr)
            return 2
        for f in argv[1:]:
            a = int(f[4:] if f.startswith('sub_') else f, 16)
            print(f'sub_{a:08X}:')
            for seg in node_state(con, a, d):
                print('\n'.join(segment_lines(seg)))
        return 0
    if not argv:
        print(f'q enum: {len(d["enums"])} enums (db/facts/enums.json), {len(d["calls"])} bound call arguments:')
        for nm, e in d['enums'].items():
            print(f'  {nm:16s} {len(e["values"]):3d} values  [{e["mark"]}]  {e["title"]}')
        print('  calls: ' + ', '.join(f'sub_{a:08X}.{c["arg"]}={c["enum"] or c["what"]}' for a, c in d['_calls'].items()))
        print('  (q enum NAME [VALUE ...] prints one; the values carry their marks and sources)')
        return 0
    nm = argv[0]
    e = d['enums'].get(nm)
    if e is None:
        print(f'q enum: no enum {nm!r} (have: {", ".join(d["enums"])})', file=sys.stderr)
        return 2
    want = argv[1:]
    items = [(k, v) for k, v in e['values'].items() if not want or k in want or any(w.lower() in v['name'].lower() for w in want if not w.isdigit())]
    if as_json:
        print(json.dumps({'enum': nm, 'title': e['title'], 'source': e['source'], 'values': dict(items)}, indent=1))
        return 0
    print(f'{nm}: {e["title"]}  [{e["mark"]}]  source: {e["source"]}')
    for k, v in sorted(items, key=lambda kv: int(kv[0])):
        print(f'  {k:>4}  {v["name"]}  [{v.get("mark", e["mark"])}]' + (f'  ({v["source"]})' if 'source' in v else ''))
    if not items:
        print('  (no value matches)')
    return 0 if items else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

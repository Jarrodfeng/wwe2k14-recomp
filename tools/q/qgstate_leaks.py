r"""q gstate --leaks: render state a callee sets and does not give back, that a LATER draw of the same pass sees (S111 GSTATELEAK;
the props shadow pass sub_827E7F00 restores the alpha test but not the alpha ref, so the circles draw with the last caster's ref).

    py tools/q/q.py gstate --leaks sub_X [sub_Y ...] [--field F] [--json]
    py tools/q/q.py gstate --leaks --field alpha_ref [--field K ...]     sweep: every function with a setter row of those keys

For a pass function, per render-state key (qgstate_inherit.FIELDS), its calls in listing order:
  - a callee whose exit leaves the key set (qgstate_inherit's Walker.exit_effect: a callee that restores what it set, or sets nothing,
    leaves it as found) adds its value(s) to the leaks in force: a constant, or `from <register>` (a callee that sets it from its own
    argument takes the pass's forwarder row at that call site, the value passed);
  - a later call that reaches a draw recorder (Walker.draw_paths) with the key's ENTRY state in force at that draw (no set of the key
    above the recorder call in the callee or the functions between) reports `LEAK key=K set by sub_A (...) reaches draw at +OFF (sub_B)`;
  - the pass's own set or restore of THE SAME KEY ends the leaks (a restore of the alpha test does not end the alpha ref's).
Limits [L]: listing order, not path order: callees on different branches (the three shadow casters) all stay in force until a set or
restore of the key; leaks accumulate (a later callee's set does not remove an earlier one, since which branch ran is not read); the
callee exit and the draw reading use the --inherit machinery's limits (CDEPTH levels, indirect callees only when a run observed them).
PLANT (selftest only): WWE_QGINHERIT_PLANT = leak_any_restore (a restore of any key ends the leaks).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import qgstate_inherit as GI  # noqa: E402


def _reads_entry(w, t, key, via=None):
    """does a draw under t (or t itself, a recorder) see the key's value at t's entry: some call path to a recorder with no set of
    the key above the recorder call at any level"""
    if t in w.g.draw_leaves():
        return True
    paths, _problem = w.draw_paths(t)
    for pth in paths:
        if via and w._dead(pth[0][0], pth[0][1], via):     # the pass's call of t cannot reach this first hop (GPRUNE)
            continue
        if all(any(p['leaves'] for p in w.scan(f, off, key, 0, w.cdepth, 'exit')) for f, off in pth):
            return True
    return False


def _value(w, pass_fn, off, t, key, p):
    """-> (value or None, text) of one exit result p of callee t called at +off"""
    if p['v'] is not None:
        return p['v'], str(p['v'])
    if p.get('arg') and p['arg'][0] == t:   # set from its own argument: the pass's forwarder row at this site
        for r in w.g.rows(pass_fn):
            if r.get('via') == t and r['off'] == off and r['key'] == key and r['kind'] != 'read':
                a = r['args'].get(r['value_reg']) or {}
                if a.get('imm') is not None:
                    return None, '/'.join(str(v) for v in a['imm']) + f' (passed at +{off:X}, in {p["arg"][1]})'
                return None, f'from {a.get("root") or "?"} (passed at +{off:X}, in {p["arg"][1]})'
        return None, f'from {p["arg"][1]} (no forwarder row at +{off:X})'
    if p['reg'] is not None:
        return None, f'from {p["reg"]}'
    return None, f'open: {p["open"]}'


def leaks(w, fn, fields=GI.FIELDS):
    """-> [{'key', 'setter', 'set_off', 'value', 'text', 'draw_off', 'draw_fn'}] for the pass fn"""
    g = w.g
    out, seen = [], set()
    own = [r for r in g.rows(fn) if r['kind'] != 'read' and not r.get('via')]
    for key in fields:
        ev = [(r['off'], 0, r) for r in own if r['key'] == key or (GI.PLANT == 'leak_any_restore' and r['kind'] == 'restore')]
        ev += [(off, 1, [t for t in tg if t != fn and not g.is_setter(t)]) for off, tg in g.callees(fn).items()]
        cur = []
        for off, kind, x in sorted(ev, key=lambda e: (e[0], e[1])):
            if kind == 0:
                cur = []
                continue
            if not x or w._dead(fn, off):    # a call the pass's own branch never takes (every caller misses its argument value: GPRUNE)
                continue
            for t in x:
                if cur and _reads_entry(w, t, key, (fn, off)):
                    for c in cur:
                        k = (key, c['setter'], c['set_off'], c['text'], off, t)
                        if k not in seen:
                            seen.add(k)
                            out.append(dict(c, key=key, draw_off=off, draw_fn=t))
            new = []
            for t in x:
                e = w.exit_effect(t, key, GI.CDEPTH - 1)
                for p in (e or []):
                    if not p['leaves']:
                        v, text = _value(w, fn, off, t, key, p)
                        new.append({'setter': t, 'set_off': off, 'value': v, 'text': text, 'at': p['at'], 'via': ' <- '.join(p['via'])})
            cur += new
    return out


def lines(lks, by_key):
    """one LEAK line per (key, value, setting instruction, draw): the pass's call sites that leak it are listed together"""
    import qgstate
    groups = {}
    for lk in lks:
        groups.setdefault((lk['key'], lk['text'], lk['at'], lk['draw_off'], lk['draw_fn'], lk['via']), []).append(lk)
    out = []
    for (key, text, at, doff, dfn, via), g in groups.items():
        st = by_key.get(key)
        if g[0]['value'] is not None and st:
            text = qgstate.value_text(st, [g[0]['value']])
        shown = f'value {text}' if g[0]['value'] is not None else text
        sites = ', '.join(f'+{o:X}' for o in sorted({x['set_off'] for x in g}))
        out.append(f'  LEAK key={key} set by {GI.sub(g[0]["setter"])} (called at {sites}; set at {hx_or(at)}; via {via}) ({shown}) '
                   f'reaches draw at +{doff:X} ({GI.sub(dfn)})')
    return out


def hx_or(at):
    return GI.hx(at) if at else 'no site read'


def main(con, funcs, fields=None, as_json=False, cap=10):
    import qgstate
    g = GI.Graph(con)
    w = GI.Walker(g)
    by_key = {s['key']: s for s in g.setters.values() if not s.get('via')}
    sweep = not funcs
    if sweep:
        want = set(fields or ())
        if not want:
            raise qgstate.GstateError('--leaks needs a function, or --field KEY for the sweep')
        funcs = sorted(int(h, 16) for h, ent in g.idx['functions'].items() if any(r['key'] in want and r['kind'] != 'read' for r in ent['rows']))
    fields = tuple(fields or GI.FIELDS)
    res = {fn: leaks(w, fn, fields) for fn in funcs}
    if as_json:
        print(json.dumps({'leaks': {GI.sub(f): [dict(x, setter=GI.sub(x['setter']), draw_fn=GI.sub(x['draw_fn'])) for x in v] for f, v in res.items() if v or not sweep},
                          'functions': len(funcs), 'problems': g.problems}, indent=1))
        return 0
    n_by = {}
    for fn, lks in res.items():
        if sweep and not lks:
            continue
        ls = lines(lks, by_key)
        print(f'q gstate --leaks {g.name(fn)} ({GI.hx(fn)}): {len(ls)} leak(s)  (listing order; a set or restore of the same key in the pass ends them)')
        for ln in ls:
            print(ln)
        for ln in ls:
            k = ln.split('key=')[1].split(' ')[0]
            n_by[k] = n_by.get(k, 0) + 1
    if sweep:
        print(f'sweep: {len(funcs)} functions read, {sum(1 for v in res.values() if v)} with a leak, {sum(n_by.values())} leaks by key: '
              + ', '.join(f'{k} x{v}' for k, v in sorted(n_by.items(), key=lambda kv: -kv[1])))
    import qgstate_prune
    ft = qgstate_prune.footer(w.pruned, g.name)
    if ft:
        print(ft)
    for p in g.problems:
        print('PROBLEM: ' + p)
    return 0

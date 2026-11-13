"""S101 lane 121: the 30 Years match-script STEP table, read from the player's own files at run time (nothing is written into the repo).

    py -I tools\\wm30\\step_table.py [--game DIR] [--json OUT] [--check]

For each of the 46 matches: every real step of the record's Lua chunk `Action` function (a step = a block `if step == N then ... end`, i.e. at
least one call guarded by `step == N`; N is script variable 267), which steps play a scene bank with call 1003, and for each step its ENTRY
CONDITIONS (lane R1's `entry`: the tests under which the Lua itself sets 267 := N, from the previous step).

  free     the step is REACHABLE FROM STEP 0 THROUGH SETTERS WITH NO TEST (step 0 is free; an unconditional setter in a guarded step does not
           make its target free). A setter inside an else-branch counts as tested (the listing walker does not record else conditions).
  guarded  anything else: some objective test (damage level, zone, event count ...) stands on every path from step 0, or no path is known

`@step:K` (the game side, src/wm30_step.h) writes 267 := K so the next Action tick runs K's body: a guarded step is entered only with `force`
(it skips the objective); a free step needs none. `@moment:BANK` is the resolver's name for the step that plays BANK.
The table never holds game bytes: step numbers, bank numbers and condition text printed by the lister (our own names for ids).
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lua51_list as lua  # noqa: E402
import pac  # noqa: E402
import scene_table  # noqa: E402


def chunk_steps(f):
    """{step: {'entries': [[conds]], 'banks': [bank...]}} for one parsed chunk. A step exists when a call is guarded by `step == N`."""
    allcalls = []

    def walk(fn):
        cs, tests = lua.analyse(fn)
        for c in cs:
            # a call in the else part of an if/else: the then-block ends in a forward JMP over the else, which starts where the test jumps to
            c['else_guards'] = []
            for (tpc, body, s, e) in tests:
                if e > s and 0 < e <= len(fn.code):
                    j = lua.decode(fn.code[e - 1])
                    if j[0] == 'JMP' and j[5] > 0 and e <= c['pc'] < e + j[5]:
                        c['else_guards'].append('else of (%s)' % lua.pretty(body))
        allcalls.extend(cs)
        for sub in fn.protos:
            walk(sub)
    walk(f)
    steps = {}
    setters = {}
    for c in allcalls:
        gs = [lua.pretty(g) for g in c['guards']] + c.get('else_guards', [])
        for g in gs:
            m = re.fullmatch(r'step == (\d+)', g)
            if m:
                steps.setdefault(int(m.group(1)), dict(entries=[], banks=[]))
        if c['fn'] == 'yMatch.CallFunction' and len(c['args']) == 3 and c['args'][0] == '267' and c['args'][2] == '0':
            try:
                srcs = [int(m.group(1)) for m in (re.fullmatch(r'step == (\d+)', x) for x in gs) if m]
                setters.setdefault(int(c['args'][1]), []).append((srcs[0] if len(srcs) == 1 else None, [x for x in gs if not re.fullmatch(r'step == \d+', x)]))
            except ValueError:
                pass
        if c['fn'] == 'yMatch.CallFunction' and len(c['args']) == 6 and c['args'][-1] == '1003':
            for g in gs:
                m = re.fullmatch(r'step == (\d+)', g)
                if m and c['args'][0].isdigit():
                    steps[int(m.group(1))]['banks'].append(int(c['args'][0]))
    for n, st in steps.items():
        st['edges'] = setters.get(n, [])
        st['entries'] = [conds for _, conds in st['edges']]
    return steps


def free_steps(steps, start=0):
    """The steps reachable from `start` through setters with no test. `steps` = {N: {'edges': [(source step or None, [conds])]}}.
    A step with no setter at all is NOT free (nothing is known to reach it); an edge from an unknown source never counts."""
    free = {start}
    changed = True
    while changed:
        changed = False
        for n, st in steps.items():
            if n not in free and any(src in free and not conds for src, conds in st['edges']):
                free.add(n)
                changed = True
    return free


def skipped_path(steps, target, start=0):
    """Every objective test on the shortest setter path from `start` to `target`: ['step a -> b: cond and cond', ...]; None when no path is known."""
    prev = {start: None}
    queue = [start]
    while queue:
        cur = queue.pop(0)
        for n, st in sorted(steps.items()):
            if n in prev:
                continue
            for src, conds in st['edges']:
                if src == cur:
                    prev[n] = (cur, conds)
                    queue.append(n)
                    break
    if target not in prev:
        return None
    out, n = [], target
    while prev[n] is not None:
        src, conds = prev[n]
        if conds:
            out.append('step %d -> %d: %s' % (src, n, ' and '.join(conds)))
        n = src
    return list(reversed(out))


def build(game):
    """{match index: {'record', 'era', 'steps': {N: {'free', 'entries', 'banks'}}, 'plays': {bank: step}}}"""
    rows, missing = scene_table.build(game)
    if missing:
        raise RuntimeError('records %s have no non-stub chunk' % missing)
    out = {}
    for r in rows:
        if r['index'] is None:
            continue
        blob = lua.era_chunks(r['era'])[r['record']]
        steps = chunk_steps(lua.parse(blob))
        plays = {}
        for s in r['scenes']:
            b = int(s['bank'])
            if b in plays:
                raise RuntimeError('match %d plays bank %d twice (steps %d and %s): the resolver cannot pick one' % (r['index'], b, plays[b], s['step']))
            plays[b] = int(s['step'])
            if int(s['step']) not in steps or b not in steps[int(s['step'])]['banks']:
                raise RuntimeError('match %d: the scene table says bank %d is played in step %s but the step table disagrees' % (r['index'], b, s['step']))
        free = free_steps(steps)
        out[r['index']] = dict(record=r['record'], era=r['era'],
                               steps={n: dict(free=n in free, entries=st['entries'], edges=[[s, c] for s, c in st['edges']], banks=st['banks'],
                                              path=skipped_path(steps, n)) for n, st in sorted(steps.items())},
                               plays=plays, flags={int(s['bank']): int(s['flag']) for s in r['scenes'] if str(s['flag']).isdigit()})
    return out


EXPECT = dict(matches=46, plays=50, free_plays=0, guarded_plays=50)


def counts(table):
    plays = [(n, b, s) for n, m in table.items() for b, s in m['plays'].items()]
    free = sum(1 for n, b, s in plays if table[n]['steps'][s]['free'])
    return dict(matches=len(table), plays=len(plays), free_plays=free, guarded_plays=len(plays) - free)


def check(table, expect=EXPECT):
    got = counts(table)
    bad = {k: (got[k], v) for k, v in expect.items() if got[k] != v}
    if bad:
        print('CHECK FAIL (got, expected): %s' % bad)
        return 1
    if table[3]['plays'] != {1090: 2} or table[3]['steps'][2]['free']:
        print('CHECK FAIL: match 3 should play 1090 in step 2 behind an objective test')
        return 1
    twin = dict(expect, plays=expect['plays'] + 1)
    if all(got[k] == v for k, v in twin.items()):
        print('CHECK FAIL: the planted wrong count was not caught')
        return 1
    print('step_table check PASS: %s; planted wrong count caught' % got)
    return 0


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=pac.GAME)
    ap.add_argument('--json')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    table = build(a.game)
    if a.check:
        return check(table)
    for n, m in sorted(table.items()):
        print('match %d (record %d, era %d): steps %s' % (n, m['record'], m['era'], sorted(m['steps'])))
        for b, s in sorted(m['plays'].items(), key=lambda x: x[1]):
            st = m['steps'][s]
            print('    bank %d plays in step %d  %s' % (b, s, 'FREE (no entry test)' if st['free'] else 'guarded (%d way(s) in, all behind a test)' % len(st['entries'])))
    print(counts(table))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump({str(k): v for k, v in table.items()}, open(a.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())

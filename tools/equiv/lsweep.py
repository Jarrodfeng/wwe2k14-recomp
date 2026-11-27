"""S111 LSWEEP: the [L]-claim sweep's candidate list for the equivalence tester (docs\\research\\s111\\EQUIV.md, EQUIV0.md "Plan item 4").

Reads every db\\claims\\*.json row marked [L] and sorts each into
    TESTABLE   names ONE game function and states a computation (a returned / stored value from fields / constants: "answers 1 when",
               "+8 = byte +2", "scales by", "J+156 + 4 x slot"), ranked by isolability: the closure of `q callees` run through
               tools\\equiv\\equiv.py closure() (kernel imports, our hooks, indirect calls = the refusals `equiv.py plan` gives), static tier
               (no refusal) first, then the smaller closure
    SKIP       with the reason: no function address, already settled (REFUTED / SUPERSEDED), names another function (multi-function), a
               draw / picture claim, an ordering / timing claim, an observation of runs or callers, a meaning / purpose claim, no computation
The word rules are a sieve for a person's next pick, not a verdict: a TESTABLE row still needs its model written (equiv\\equiv_tests.cpp)
and its run; a skipped row is not "untestable", only "not a computation of one function by these words".

Usage (py -I):
    lsweep.py [--top N] [--skips] [--no-plan] [--json OUT]   the ranked list (closures from one `q multi` call)
    lsweep.py --selftest                                     planted rows: testable / not testable / one function / multi
Contains nothing from the game. 0 game runs.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import equiv  # noqa: E402  (the driver's closure() and hook scan: the same refusals `equiv.py plan` gives)

CLAIMS = os.path.join(REPO, 'db', 'claims')
PLANT = os.environ.get('LSWEEP_PLANT', '')   # the selftest's planted faults (each must fail its own check)

SUB = re.compile(r'\bsub_([0-9A-Fa-f]{8})\b')
# Skip reasons, tried in this order on the claim's HEAD (head()); the first that matches names the row's reason.
SKIPS = [
    ('settled', re.compile(r'^\s*(REFUTED|SUPERSEDED)\b')),
    ('conclusion', re.compile(r'^\s*so\b', re.I)),
    ('draw / picture', re.compile(r'\b(draws?|drawn|drawing|pixels?|picture|texels?|textures?|resolves?|sampler|samples|blend(ed|s)?|render(s|ed)?|'
                                  r'GPU|shader|depth|cull(s|ed)?|light|shadow|camera|frames? (the|into)|on screen)\b', re.I)),
    ('ordering / timing', re.compile(r'\b(before|after|order|precedes?|each frame|per frame|next frame|one frame later|while|until|'
                                     r'at the start|thread|every update|rate|fps|clock)\b', re.I)),
    ('observation / callers', re.compile(r'\b(runs?|ran|observed|in practice|in play|live|caller|callers|called|calls|reached|reaches|unreached|'
                                         r'found|seen|not read|not found|searched|scope=|\d+ (functions|sites))\b', re.I)),
    ('meaning / purpose', re.compile(r'\b(means?|meaning|belongs|purpose|likely|is an? |are an? |is the [^,;]*?(screen|page|editor|flag|switch|mode|'
                                     r'setting|option|table|action|flow|class|port)|are the|named?|id(s)? of)\b', re.I)),
]
COMPUTE = re.compile(r'\b(answers|returns?|result is|sets?|stores?|writes?|clears?|copies|copied|scales?|bumps?|counts?|rates?|equals?|computes?|'
                     r'adds?|minus|times|divided|sign-extended|rounds?|reads? (the |a )?(\w+ )?(byte|word|half|float))\b|\s=\s|\s[x*/+]\s\d|'
                     r'\+\d+\s*=|<=|>=|\bin \d+\.\.\d+\b', re.I)
PAREN = re.compile(r'\([^()]*\)')


def head(text, asides=False):
    """The claim's head clause: cut at the first '; ' and at ', so ' (what follows is the note's reading of the computation, not the
    computation); parenthetical asides removed unless `asides` (the skip words are judged without them, the computation with them: "the
    judge's rope flag for a slot (J+152, or J+156 + 4 x slot ...)" states its computation in the aside). The tester tests the head; the
    rest stays the note's [L]."""
    t = text.split('; ')[0]
    t = re.split(r',\s+so\s', t)[0]
    if not asides:
        for _ in range(3):
            t = PAREN.sub('', t)
    return t.strip()


def classify(row):
    """-> ('TESTABLE', why) or ('SKIP', reason) for one claims row (a dict with function / claim)."""
    fn = row.get('function')
    if not fn or str(fn) in ('None', 'null', ''):
        return 'SKIP', 'no function address'
    try:
        own = int(str(fn), 16)
    except ValueError:
        return 'SKIP', f'function {fn!r} is not an address'
    text = row.get('claim') or ''
    others = sorted({m.group(1).upper() for m in SUB.finditer(text)} - {f'{own:08X}'})
    if others and PLANT != 'multi_ignored':
        return 'SKIP', 'multi-function: names ' + ', '.join('sub_' + o for o in others)
    h = text if PLANT == 'whole_text' else head(text)
    for reason, rx in SKIPS:
        m = rx.search(h)
        if m:
            return 'SKIP', f'{reason} ("{m.group(0).strip()}")'
    m = COMPUTE.search(text if PLANT == 'whole_text' else head(text, asides=True))
    if not m:
        return 'SKIP', 'no computation stated'
    return 'TESTABLE', f'computation ("{m.group(0).strip()}")'


def l_rows(root=CLAIMS):
    """Every [L] row: (file name, index in the file, row)."""
    out = []
    for p in sorted(glob.glob(os.path.join(root, '*.json'))):
        with open(p, encoding='utf-8') as f:
            rows = json.load(f)
        for i, r in enumerate(rows if isinstance(rows, list) else []):
            if isinstance(r, dict) and r.get('mark') == 'L':
                out.append((os.path.basename(p), i, r))
    return out


def callee_trees(funcs):
    """{sub_X: q callees --json tree} for every function, one `q multi` call (one process, not one per function)."""
    if not funcs:
        return {}
    r = subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'q', 'q.py'), 'multi'] + [f'callees {f} --json --depth 40' for f in funcs],
                       capture_output=True, text=True, errors='replace', cwd=REPO)
    out, cur, buf = {}, None, []
    for line in r.stdout.splitlines():
        m = re.match(r'^== \[\d+/\d+\] q callees (sub_[0-9A-Fa-f]{8}) ', line)
        if m:
            cur, buf = m.group(1), []
            continue
        if cur and re.match(r'^-- \[\d+/\d+\] exit', line):
            try:
                out[cur] = json.loads('\n'.join(buf))
            except ValueError:
                pass
            cur = None
            continue
        if cur:
            buf.append(line)
    return out


def isolability(tree, hooked):
    """-> (tier, closure size, refusals) through equiv.closure (no stubs: what the static tier would run)."""
    if PLANT == 'rank_ignores_refusals':
        return 'static', 1, []
    if tree is None:
        return 'unknown', 0, ['q callees gave no tree']
    funcs, _, refusals = equiv.closure(tree, [], hooked)
    if tree['func'] in hooked:
        refusals = [f'{tree["func"]} itself is hooked by us ({hooked[tree["func"]]})'] + refusals
    return ('static' if not refusals else 'refused'), len(funcs), refusals


def rank(cands):
    """Static tier first, then the smaller closure, then the file order."""
    return sorted(cands, key=lambda c: (c['tier'] != 'static', c['closure'], c['file'], c['index']))


def sweep(plan=True, root=CLAIMS):
    rows = l_rows(root)
    cands, skips = [], []
    for fname_, i, r in rows:
        v, why = classify(r)
        item = {'file': fname_, 'index': i, 'function': f'sub_{int(str(r["function"]), 16):08X}' if v == 'TESTABLE' else r.get('function'),
                'offset': r.get('offset'), 'note': r.get('note'), 'claim': r.get('claim'), 'why': why}
        (cands if v == 'TESTABLE' else skips).append(item)
    if plan:
        hooked = equiv.hooked_functions(equiv.src_texts())
        trees = callee_trees(sorted({c['function'] for c in cands}))
        for c in cands:
            c['tier'], c['closure'], c['refusals'] = isolability(trees.get(c['function']), hooked)
    else:
        for c in cands:
            c['tier'], c['closure'], c['refusals'] = 'unplanned', 0, []
    return rows, rank(cands), skips


def main(argv=None):
    ap = argparse.ArgumentParser(description='S111 LSWEEP: rank the [L] claims the equivalence tester can take')
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--skips', action='store_true')
    ap.add_argument('--no-plan', action='store_true')
    ap.add_argument('--json', default='')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    rows, cands, skips = sweep(plan=not a.no_plan)
    by = {}
    for s in skips:
        k = s['why'].split(' (')[0].split(':')[0]
        by[k] = by.get(k, 0) + 1
    print(f'lsweep: {len(rows)} [L] rows; {len(cands)} testable, {len(skips)} skipped: '
          + ', '.join(f'{k} {n}' for k, n in sorted(by.items(), key=lambda kv: -kv[1])))
    shown = cands[:a.top] if a.top else cands
    for n, c in enumerate(shown, 1):
        print(f'{n:3} {c["tier"]:8} closure {c["closure"]:4}  {c["function"]} {c["file"]}#{c["index"]}  {c["claim"][:150]}')
        for r in c['refusals'][:2]:
            print(f'        refused: {r[:160]}')
    if a.top and len(cands) > a.top:
        print(f'... {len(cands) - a.top} more testable rows (lsweep.py without --top)')
    if a.skips:
        for s in skips:
            print(f'skip {s["file"]}#{s["index"]} {s["function"]}: {s["why"]}  | {s["claim"][:110]}')
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'testable': cands, 'skipped': skips}, f, indent=1)
    return 0


# ---------------------------------------------------------------------------------------------------------------- selftest
def selftest():
    fails = []

    def check(name, ok, detail=''):
        print(f'{"ok  " if ok else "FAIL"} {name}' + (f': {detail}' if detail and not ok else ''))
        if not ok:
            fails.append(name)

    planted = [
        ('testable', {'function': '0x8251D950', 'claim': 'Answers 1 when none of the nine 24-byte records at +392 has a first word in 1..3'}, 'TESTABLE'),
        ('testable_store', {'function': '0x8271B3A8', 'claim': 'replay start: +8 = replay record byte +2'}, 'TESTABLE'),
        ('head_only', {'function': '0x8251D950', 'claim': 'Answers 1 when no record has a first word in 1..3; read as "none loading" (the meaning '
                                                         'comes from the helper\'s name alone)'}, 'TESTABLE'),
        ('conclusion', {'function': '0x82A28CD8', 'claim': 'so a = month (0 = January)'}, 'SKIP:conclusion'),
        ('aside_computation', {'function': '0x825B0AB8', 'claim': 'the judge\'s rope flag for a slot (J+152, or J+156 + 4 x slot in special '
                                                                  'types)'}, 'TESTABLE'),
        ('census', {'function': '0x82BD42E0', 'claim': 'the loader count word is set by 29 functions (78 sites)'}, 'SKIP:observation'),
        ('one_function_self_named', {'function': '0x82B79200', 'claim': 'sub_82B79200 returns the row byte 31 sign-extended'}, 'TESTABLE'),
        ('multi', {'function': '0x823274E8', 'claim': 'stores the value sub_82327400 returns at +16'}, 'SKIP:multi'),
        ('no_function', {'function': None, 'claim': 'G+15204 = the in-match option flag'}, 'SKIP:no function'),
        ('draw', {'function': '0x8272D378', 'claim': 'the own node draws its blended texels with depth write'}, 'SKIP:draw'),
        ('ordering', {'function': '0x82521490', 'claim': 'it writes the table before the walk'}, 'SKIP:ordering'),
        ('purpose', {'function': '0x827658E0', 'claim': 'char+2648 is a stick direction, -1 means neutral'}, 'SKIP:meaning'),
        ('settled', {'function': '0x825A3908', 'claim': 'REFUTED by ALPHAWREST [C]: the reset sets blend on'}, 'SKIP:settled'),
        ('no_computation', {'function': '0x83420708', 'claim': 'the slot motion player of the stage'}, 'SKIP:no computation'),
    ]
    for name, row, want in planted:
        v, why = classify(row)
        got = v if v == 'TESTABLE' else f'SKIP:{why}'
        check(f'classify.{name}', got.startswith(want), f'want {want}, got {got}')

    # isolability: a leaf is static; an import, a hook and an indirect call are refused; ranking puts static and small first
    leaf = {'func': 'sub_8251D950', 'callees': []}
    imp = {'func': 'sub_82000010', 'callees': [{'func': 'sub_82000020', 'callees': [{'func': '__imp__KeQuerySystemTime', 'callees': []}]}]}
    hook = {'func': 'sub_82000030', 'callees': [{'func': 'sub_82C79000', 'callees': []}]}
    ind = {'func': 'sub_82000040', 'callees': [], 'indirect_calls': 1}
    big = {'func': 'sub_82000050', 'callees': [{'func': f'sub_8200{k:04X}', 'callees': []} for k in range(0x100, 0x105)]}
    hooked = {'sub_82C79000': 'src\\nf\\memcpy_fwd.h'}
    t_leaf, t_imp, t_hook, t_ind, t_big = (isolability(t, hooked) for t in (leaf, imp, hook, ind, big))
    check('iso.leaf_static', t_leaf[0] == 'static' and t_leaf[1] == 1, str(t_leaf))
    check('iso.import_refused', t_imp[0] == 'refused' and 'kernel import' in t_imp[2][0], str(t_imp))
    check('iso.hook_refused', t_hook[0] == 'refused' and 'hooked by us' in t_hook[2][0], str(t_hook))
    check('iso.indirect_refused', t_ind[0] == 'refused' and 'indirect' in t_ind[2][0], str(t_ind))
    cands = [dict(file='a', index=i, tier=t[0], closure=t[1], name=n) for i, (n, t) in
             enumerate([('imp', t_imp), ('big', t_big), ('leaf', t_leaf), ('hook', t_hook)])]
    order = [c['name'] for c in rank(cands)]
    check('rank.static_first_then_small', order[:2] == ['leaf', 'big'] and set(order[2:]) == {'imp', 'hook'}, str(order))

    # a planted claims folder: only [L] rows are read, every one is sorted one way or the other, the testable one is found
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, 's999_PLANT.json'), 'w', encoding='utf-8') as f:
            json.dump([dict(r, mark='L', address=r['function'], offset='0x0', evidence='-', note='-') for _, r, _ in planted]
                      + [{'function': '0x82000060', 'mark': 'C', 'claim': 'returns 1'}], f)
        rows, cands, skips = sweep(plan=False, root=tmp)
    check('sweep.every_row_sorted', len(rows) == len(cands) + len(skips) == len(planted), f'{len(rows)} vs {len(cands)}+{len(skips)}')
    check('sweep.finds_8251D950', any(c['function'] == 'sub_8251D950' for c in cands), 'the planted ENTRBODIES row is not testable')
    # the real folder reads without error (its rows change as claims are settled: no count is asserted)
    rows, cands, skips = sweep(plan=False)
    check('sweep.real_folder', len(rows) == len(cands) + len(skips) > 0, f'{len(rows)} vs {len(cands)}+{len(skips)}')
    print(f'lsweep selftest: {len(fails)} failed' + (f' ({", ".join(fails)})' if fails else ''))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

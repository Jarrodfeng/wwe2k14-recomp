#!/usr/bin/env python3
r"""Session 100: works out which options of a hand-written effect reproduce each of the game's shaders, so nobody reads 1,500 near-identical listings.

  py tools\shaders\harness\solve_variants.py <effect> [--stage vertex|fragment] [--jobs 8] [--tries 12] [--rounds 2] [--write] [--root DIR]

The writer writes one shader with #ifdef options and describes everything it can declare in a "solve" block of <effect>.fx.json:

  "solve": {
    "vertex": { "options": ["FX_SKINNED", "FX_SHADOW"],
                "constants": [ {"field": "ViewProj", "register": 56, "count": 4}, ... every constant any option declares ],
                "attributes": [ {"fetch": 0, "location": 0}, {"fetch": 1, "location": 2, "range": [0, 20], "integer": true, "when": ["!FX_SKINNED"]}, ... ] },
    "fragment": { "options": [...], "constants": [...], "samplers": [ {"slot": 12, "kind": "2d", "when": ["FX_SHADOW"]}, ... ] }
  }

"when" is a list of options that must be on ("FX_X") or off ("!FX_X"). Any other key of a stage's block (loop_constants, target_16) is copied into every variant.
"exclusive": [["FX_LAYER2", "FX_LAYER3", "FX_LAYER4"], ...] lists options that never come together; it cuts the option sets tried (limit 4096 sets).
"samplers" in the template is optional: each variant gets the slots its code samples (check_effect.sampled_slots); the template list only names the slots
of a loop that picks textures by a counter. At most four solvers run at once (locks in out\s100\); a fifth waits and says so.
For each of the effect's shaders the tool ranks the option sets by how well their declared constant registers match the registers the game's own constant table
names for that shader, proves the best few with check_effect.py's own comparison, and reports what stayed unsolved. --write replaces the "vertex" and "fragment"
lists of the .fx.json with the variants it found. Only the unsolved shaders need reading. Nothing from the game is written outside out\s100\ (rule 1).
"""
import argparse
import itertools
import json
import multiprocessing
import os
import pickle
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_effect as ce

KIND = {'vertex': 'VS', 'fragment': 'PS'}
STATE = {}


def holds(entry, defines):
    return all((w[1:] not in defines) if w.startswith('!') else (w in defines) for w in entry.get('when', []))


def clean(entry):
    return {k: v for k, v in entry.items() if k != 'when'}


def build_variant(fxdir, effect, stage, template, defines):
    """The variant for one option set, or None when that set does not preprocess. Keeps only what those options really declare."""
    ext, name = ('.vert', 'vert') if stage == 'vertex' else ('.frag', 'frag')
    try:
        text = ce.blocks_to_structs(ce.preprocess(os.path.join(fxdir, effect + ext), name, list(defines)))
    except RuntimeError:
        return None
    block = re.search(r'struct \w+_t \{(.*?)\};\s*\w+_t \w+;', text, re.S)
    members = set(re.findall(r'\b(?:vec4|uvec4)\s+(\w+)', block.group(1))) if block else set()
    locations = {int(x) for x in re.findall(r'layout\(location = (\d+)\)\s*(?:flat\s+|smooth\s+)?in\b', text)}
    variant = {k: v for k, v in template.items() if k not in ('options', 'exclusive', 'constants', 'attributes', 'samplers')}
    variant['name'] = '_'.join(d[3:].lower() if d.startswith('FX_') else d.lower() for d in defines) or 'base'
    variant['defines'] = list(defines)
    variant['constants'] = [clean(c) for c in template.get('constants', []) if holds(c, defines) and c['field'] in members]
    if stage == 'vertex':
        variant['attributes'] = [clean(a) for a in template.get('attributes', []) if holds(a, defines) and a['location'] in locations]
    used, dynamic = ce.sampled_slots(text, stage)   # what the code samples, whatever the template's sampler list says
    if dynamic:   # a loop over slots: the hand list says which, the literal reads are added to it
        for s in template.get('samplers', []):
            if holds(s, defines):
                used.setdefault(s['slot'], s.get('kind', '2d'))
    if used or 'samplers' in template:
        variant['samplers'] = [{'slot': slot, 'kind': kind} for slot, kind in sorted(used.items())]
    variant['_regs'] = sorted(r for c in variant['constants'] for r in range(c['register'], c['register'] + c.get('count', 1)))
    return variant


def read_registers(h, kind, code):
    """The named float registers the shader really reads: the game's constant table also lists parameters a permutation never touches (the lights on a cube-lit shader)."""
    named = ce.uc.named_float_registers(h)
    text = ce.disassemble(code, kind)
    read = {int(r) for r in re.findall(r'\bc(\d+)\b', text)}
    for base in {int(r) for r in re.findall(r'c\[(\d+)\+', text)}:   # an indexed array: the whole named run from its base
        r = base
        while r in named:
            read.add(r)
            r += 1
    return read & named


def ranked_for(variants, named):
    """Best first: fewest constants declared that the game's shader does not name, and the game's shader names we do not declare; fewer options on ties."""
    def miss(index):
        regs = set(variants[index]['_regs'])
        return (len(regs - named) + len(named - regs), len(variants[index]['defines']))
    return sorted(range(len(variants)), key=miss)


def share(state):
    """Workers load the shared state from a file: through the pool's pipe a large one failed with "pickle data was truncated"."""
    path = os.path.join(ce.H, 'solve_state_%d.pkl' % os.getpid())
    with open(path, 'wb') as out:
        pickle.dump(state, out)
    return path


def init(path):
    with open(path, 'rb') as source:
        STATE.update(pickle.load(source))
    ce.FAST_REJECT = True


def solve_one(job):
    stage, h, order = job
    best = (float('inf'), None, 'no candidate')
    for index in order:
        var = STATE['variants'][stage][index]
        rng = np.random.default_rng(1 + (h & 0xFFFF))
        check = ce.check_vertex if stage == 'vertex' else ce.check_fragment
        try:
            worst, output, problems, _, _, _ = check(STATE['fxdir'], var.get('source', STATE['effect']), var, h, STATE['ucodes'][h], rng, 4096 * STATE['rounds'], True, False)
        except Exception as error:
            if best[1] is None:
                best = (float('inf'), index, str(error).split('\n')[0][:140])
            continue
        if not problems and worst <= 1e-3:
            return stage, h, index, worst, ''
        if worst < best[0]:
            best = (worst, index, problems[0][0] if problems else output)
    return stage, h, None, best[0], '%s: %s' % (STATE['variants'][stage][best[1]]['name'] if best[1] is not None else '-', best[2])


SOLVER_SLOTS = 4   # x --jobs 8 = 32 busy threads at most on the 32-thread PC (S100: one slot queued six agents behind one solve; three, with a
                   # sequential setup phase, still starved small effects for 25 minutes)


def solver_lock():
    """At most SOLVER_SLOTS solvers at once on this PC: each runs --jobs processes. Held until this process exits."""
    import msvcrt
    import time
    folder = os.path.join(ce.REPO, 'out', 's100')
    os.makedirs(folder, exist_ok=True)
    handles = [open(os.path.join(folder, 'solver.lock' if i == 0 else 'solver.%d.lock' % i), 'a+') for i in range(SOLVER_SLOTS)]
    waited = False
    while True:
        for handle in handles:
            try:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                if waited:
                    print('solver slot free, starting', flush=True)
                return handle
            except OSError:
                pass
        if not waited:
            print('waiting for a free solver slot (%d solvers at a time); this is a queue, not a hang' % SOLVER_SLOTS, flush=True)
            waited = True
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('effect')
    ap.add_argument('--stage', choices=['vertex', 'fragment'])
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--tries', type=int, default=24)
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--root')
    ap.add_argument('--inventory', default=os.path.join(ce.REPO, 'db', 'shaders', 'inventory.json'))
    args = ap.parse_args()
    lock = solver_lock()  # noqa: F841  kept open until exit
    fxdir = args.root or os.path.join(ce.REPO, 'src', 'renderer', 'shaders', 'fx')
    ce.ensure_tools()
    path = os.path.join(fxdir, args.effect + '.fx.json')
    spec = json.load(open(path, encoding='utf-8'))
    if 'solve' not in spec:
        raise SystemExit('%s has no "solve" block (see the top of this file)' % path)
    wanted = {'vertex': [], 'fragment': []}
    for row in json.load(open(args.inventory))['effects'][args.effect]['hashes']:
        kind, hexa = row.split()[:2]
        wanted['vertex' if kind == 'VS' else 'fragment'].append(int(hexa, 16))
    stages = [args.stage] if args.stage else [s for s in ('vertex', 'fragment') if s in spec['solve']]
    ucodes = ce.uc.get(args.effect, [h for s in stages for h in wanted[s]])
    variants, jobs = {}, []
    for stage in stages:
        template = spec['solve'][stage]
        options = template.get('options', [])
        groups = template.get('exclusive', [])   # options that never come together (layer counts, light modes): at most one of each group
        sets = [c for n in range(len(options) + 1) for c in itertools.combinations(options, n) if all(sum(o in c for o in g) <= 1 for g in groups)]
        if len(sets) > 4096:
            raise SystemExit('%s: %d option sets is too many; list options that never come together in "exclusive" groups' % (stage, len(sets)))
        # Each option set is one glslang preprocess (a separate process): threads run them side by side (S100: done one at a time, this setup held
        # a solver slot for minutes while the CPU idled).
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as threads:
            built = list(threads.map(lambda d: build_variant(fxdir, args.effect, stage, template, d), sets))
        variants[stage] = [v for v in built if v]
        for h in wanted[stage]:
            if h not in ucodes:
                print('NO MICROCODE %016X' % h)
                continue
            jobs.append((stage, h, ranked_for(variants[stage], read_registers(h, KIND[stage], ucodes[h][1]))[:args.tries]))
    state = {'variants': variants, 'ucodes': ucodes, 'fxdir': fxdir, 'effect': args.effect, 'rounds': args.rounds}
    with multiprocessing.Pool(args.jobs, init, (share(state),)) as pool:
        results = []
        for result in pool.imap_unordered(solve_one, jobs, chunksize=1):
            results.append(result)
            if len(results) % 10 == 0:
                print('... %d of %d shaders tried, %d solved' % (len(results), len(jobs), sum(1 for r in results if r[2] is not None)), flush=True)
    solved = {s: {} for s in stages}
    unsolved = []
    for stage, h, index, worst, note in results:
        if index is None:
            unsolved.append((stage, h, worst, note))
        else:
            solved[stage].setdefault(index, []).append(h)
    for stage in stages:
        print('%s: %d of %d shaders solved, %d variants' % (stage, sum(len(v) for v in solved[stage].values()), len(wanted[stage]), len(solved[stage])))
        for index in sorted(solved[stage], key=lambda i: -len(solved[stage][i])):
            print('  %-40s %d' % (variants[stage][index]['name'], len(solved[stage][index])))
    for stage, h, worst, note in sorted(unsolved, key=lambda u: (u[0], u[2])):
        print('UNSOLVED %s %016X best %.3e  %s' % (KIND[stage], h, worst, note))
    if args.write:
        for stage in stages:
            spec[stage] = [dict({k: v for k, v in variants[stage][i].items() if k != '_regs'}, hashes=sorted('%016X' % h for h in solved[stage][i])) for i in sorted(solved[stage])]
        with open(path, 'w', encoding='utf-8', newline='\n') as out:
            json.dump(spec, out, indent=1)
            out.write('\n')
    sys.exit(1 if unsolved else 0)


if __name__ == '__main__':
    multiprocessing.freeze_support()
    main()

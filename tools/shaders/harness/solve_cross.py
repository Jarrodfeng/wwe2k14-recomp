#!/usr/bin/env python3
r"""Session 100: tries every shader we have already written against every game shader nobody has covered yet, across effects. No agent reads anything.

  py tools\shaders\harness\solve_cross.py [--only EFFECT ...] [--jobs 8] [--tries 16] [--max-miss 8] [--rounds 2] [--write]

A game shader only needs a program that matches it; which effect that program was written for does not matter. Candidates are every written variant, plus every
option set of an effect with a "solve" block. They are ranked by how well their constant registers match the game shader's own constant table, and the best few
are proven with the same comparison as check_effect.py. --write adds what it found to the owning effect's .fx.json as variants with a "source" field (the effect whose
.vert / .frag they reuse), creating that file if the effect has none. Do not use --write while agents are editing effects. Nothing from the game leaves out\s100\.
"""
import argparse
import glob
import itertools
import json
import multiprocessing
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import solve_variants as sv

ce = sv.ce


def load_pool(fxdir):
    """Every candidate per stage: {'vertex': [variant, ...], 'fragment': [...]}, each tagged with the effect it comes from, and the set of covered hashes."""
    pool, covered = {'vertex': [], 'fragment': []}, set()
    for path in sorted(glob.glob(os.path.join(fxdir, '*.fx.json'))):
        spec = json.load(open(path, encoding='utf-8'))
        effect = spec['effect']
        for stage in pool:
            for variant in spec.get(stage, []):
                covered.update(int(x, 16) for x in variant.get('hashes', []))
            template = spec.get('solve', {}).get(stage)
            if template and len(template.get('options', [])) <= 12:
                options = template.get('options', [])
                for count in range(len(options) + 1):
                    for defines in itertools.combinations(options, count):
                        variant = sv.build_variant(fxdir, effect, stage, template, defines)
                        if variant:
                            pool[stage].append(dict(variant, source=effect))
            else:
                for variant in spec.get(stage, []):
                    if 'source' not in variant:
                        regs = sorted(r for c in variant.get('constants', []) for r in range(c['register'], c['register'] + c.get('count', 1)))
                        pool[stage].append(dict({k: v for k, v in variant.items() if k != 'hashes'}, source=effect, _regs=regs))
    return pool, covered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--tries', type=int, default=16)
    ap.add_argument('--max-miss', type=int, default=8)
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--write', action='store_true')
    args = ap.parse_args()
    fxdir = os.path.join(ce.REPO, 'src', 'renderer', 'shaders', 'fx')
    ce.ensure_tools()
    pool, covered = load_pool(fxdir)
    inventory = json.load(open(os.path.join(ce.REPO, 'db', 'shaders', 'inventory.json')))['effects']
    owner, ucodes, jobs = {}, {}, []
    for effect, entry in inventory.items():
        if args.only and effect not in args.only:
            continue
        hashes = {}
        for row in entry.get('hashes', []):
            kind, hexa = row.split()[:2]
            value = int(hexa, 16)
            if value not in covered and value not in owner:
                owner[value] = effect
                hashes[value] = 'vertex' if kind == 'VS' else 'fragment'
        if not hashes:
            continue
        codes = ce.uc.get(effect, list(hashes))
        ucodes.update(codes)
        for value, stage in hashes.items():
            if value not in codes:
                continue
            named = ce.uc.named_float_registers(value)
            order = [i for i in sv.ranked_for(pool[stage], named) if len(set(pool[stage][i]['_regs']) ^ named) <= args.max_miss][:args.tries]
            if order:
                jobs.append((stage, value, order))
    print('%d game shaders uncovered, %d with a plausible candidate' % (len(owner), len(jobs)))
    state = {'variants': pool, 'ucodes': ucodes, 'fxdir': fxdir, 'effect': '', 'rounds': args.rounds}
    with multiprocessing.Pool(args.jobs, sv.init, (sv.share(state),)) as workers:
        results = workers.map(sv.solve_one, jobs, chunksize=1)
    found = {}
    for stage, value, index, _worst, _note in results:
        if index is not None:
            found.setdefault(owner[value], {}).setdefault((stage, index), []).append(value)
    total = sum(len(v) for e in found.values() for v in e.values())
    print('solved %d' % total)
    for effect in sorted(found):
        print('  %-20s %d' % (effect, sum(len(v) for v in found[effect].values())))
    if not args.write:
        return
    for effect, groups in found.items():
        path = os.path.join(fxdir, effect + '.fx.json')
        spec = json.load(open(path, encoding='utf-8')) if os.path.exists(path) else {'effect': effect, 'vertex': [], 'fragment': []}
        for (stage, index), values in groups.items():
            variant = {k: v for k, v in pool[stage][index].items() if k != '_regs'}
            variant['name'] = '%s__%s' % (variant['source'], variant['name'])
            variant['hashes'] = sorted('%016X' % v for v in values)
            spec.setdefault(stage, []).append(variant)
        with open(path, 'w', encoding='utf-8', newline='\n') as out:
            json.dump(spec, out, indent=1)
            out.write('\n')


if __name__ == '__main__':
    multiprocessing.freeze_support()
    main()

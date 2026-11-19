#!/usr/bin/env python3
r"""S103 lane J1: merge the per-scene choice tables into ONE table, with a conflict list and coverage counts. Names and numbers only; 0 game runs.

  py -I tools\harvest\choice_merge.py [TABLE.json ...] [--dir tools\harvest\choice_table] [--out out/harvest/choice_merged.json] [--show N]

Input: the tables tools\harvest\choice_harvest.py wrote (one per scene class). With no TABLE and no --dir the files of choice_table\ are merged (a merged file in that folder is skipped).
Output (deterministic):
  { format, scenes:{scene:{arena, looks:[...]}},
    meshes:[ {file, slot, mesh, effect, tex, seen:{"scene/look": {draws, ambiguous, choices}}} ],     # one row per (model file, mesh): every scene / look that drew it
    classes:[ {scene, look, class, technique, draws, choices} ],
    conflicts:[ {file, mesh, kind, entries:{"scene/look": [signatures]}} ],
    coverage:{ arenas:{ "bgNN":{meshes, scenes, looks} }, scenes:{ scene:{meshes, draws, class_rows} } } }
A CONFLICT is a (model file, mesh) whose choice SIGNATURE set (vs, ps, blend, alpha test) differs between two scenes / looks. Several choices in ONE scene / look are normal (a mesh
draws once in the shadow pass and once in the colour pass); differing sets between scenes / looks are the cases a derived rule must explain (a mesh the game draws one way in an
entrance and another way in a match), or a wrong join. Exit 0 always for a merge (the conflict list is the report); --max-conflicts N exits 1 above N."""
import argparse
import collections
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TABLE_DIR = os.path.join(HERE, 'choice_table')
MERGED = os.path.join(ROOT, 'out', 'harvest', 'choice_merged.json')   # derived, scratch: never committed


def signature(c):
    return '%s | %s | %s | alpha %s' % (c['vs'] or c.get('vs_hash', '?'), c['ps'] or c.get('ps_hash', '-'), c['blend'], c['alpha'])


def merge(tables):
    scenes = {}
    meshes = {}
    classes = []
    for t in tables:
        sc = t['scene']
        scenes[sc] = dict(arena=t.get('arena'), looks=sorted(t['looks']))
        for m in t['meshes']:
            row = meshes.setdefault((m['file'], m['mesh']), dict(file=m['file'], slot=m['slot'], mesh=m['mesh'], effect=m['effect'], tex=m['tex'], seen={}))
            for look, v in m['looks'].items():
                key = '%s/%s' % (sc, look)
                if key in row['seen']:   # the same scene / look twice (two captures): add up
                    old = row['seen'][key]
                    old['draws'] += v['draws']
                    old['ambiguous'] += v['ambiguous']
                    by = {json.dumps({k: x for k, x in c.items() if k != 'n'}, sort_keys=True): dict(c) for c in old['choices']}
                    for c in v['choices']:
                        k = json.dumps({k2: x for k2, x in c.items() if k2 != 'n'}, sort_keys=True)
                        if k in by:
                            by[k]['n'] += c['n']
                        else:
                            by[k] = dict(c)
                    old['choices'] = sorted(by.values(), key=signature)
                else:
                    row['seen'][key] = v
        for c in t['classes']:
            classes.append(dict(scene=sc, **c))
    conflicts = []
    for (f, mesh), row in sorted(meshes.items()):
        sets = {k: sorted({signature(c) for c in v['choices']}) for k, v in row['seen'].items()}
        distinct = {json.dumps(v) for v in sets.values()}
        if len(distinct) > 1:
            conflicts.append(dict(file=f, mesh=mesh, kind='signature set differs between scenes / looks', entries=dict(sorted(sets.items()))))
    arenas = collections.defaultdict(lambda: dict(meshes=0, scenes=set(), looks=set()))
    scn = collections.defaultdict(lambda: dict(meshes=0, draws=0, class_rows=0))
    for (f, mesh), row in meshes.items():
        a = arenas[f.split('.')[0]]
        a['meshes'] += 1
        for k, v in row['seen'].items():
            a['scenes'].add(k.split('/')[0])
            a['looks'].add(k)
            scn[k.split('/')[0]]['draws'] += v['draws']
        for s in {k.split('/')[0] for k in row['seen']}:
            scn[s]['meshes'] += 1
    for c in classes:
        scn[c['scene']]['class_rows'] += 1
    cov = dict(arenas={k: dict(meshes=v['meshes'], scenes=sorted(v['scenes']), looks=sorted(v['looks'])) for k, v in sorted(arenas.items())},
               scenes={k: v for k, v in sorted(scn.items())})
    mrows = [meshes[k] for k in sorted(meshes)]
    classes.sort(key=lambda c: (c['scene'], c['look'], c['class'], c['technique']))
    return dict(format=1, scenes=dict(sorted(scenes.items())), meshes=mrows, classes=classes, conflicts=conflicts, coverage=cov)


def load_tables(paths, dirpath):
    if not paths:
        paths = sorted(p for p in glob.glob(os.path.join(dirpath, '*.json')))
    out = []
    for p in paths:
        t = json.load(open(p, encoding='utf-8'))
        if t.get('format') == 1 and 'scene' in t:
            out.append(t)
    if not out:
        raise SystemExit('no choice tables to merge (a merge that reads nothing must fail)')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tables', nargs='*')
    ap.add_argument('--dir', default=TABLE_DIR)
    ap.add_argument('--out', default=MERGED)
    ap.add_argument('--show', type=int, default=10)
    ap.add_argument('--max-conflicts', type=int, default=-1)
    a = ap.parse_args()
    m = merge(load_tables(a.tables, a.dir))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(m, f, indent=1, sort_keys=True)
        f.write('\n')
    print('merged %d scenes: %d meshes, %d class rows, %d conflicts -> %s' % (len(m['scenes']), len(m['meshes']), len(m['classes']), len(m['conflicts']), a.out))
    for k, v in m['coverage']['arenas'].items():
        print('  %-6s %4d meshes in %d scene(s): %s' % (k, v['meshes'], len(v['scenes']), ', '.join(v['looks'])))
    for c in m['conflicts'][:a.show]:
        print('  CONFLICT %s mesh %d: %s' % (c['file'], c['mesh'], ' || '.join('%s = %s' % (k, ' ; '.join(v)) for k, v in c['entries'].items())[:300]))
    if 0 <= a.max_conflicts < len(m['conflicts']):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

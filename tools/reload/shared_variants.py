#!/usr/bin/env python3
"""Session 101 lane 24: which registry variants are SHARED hash targets. The registry holds one variant per game shader hash; when the effect descriptions list the same
hash under variants of several effects, an edit of that variant moves the draws of every one of those effects. A plant (the judge's, a reload test) must use a sole target.

  py -I tools\\reload\\shared_variants.py [--fx-dir DIR]     one line per group: stage, the effects, the variants (our names only, no game data)
"""
import argparse
import collections
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def groups(fx_dir):
    by_hash = collections.defaultdict(list)
    for jp in sorted(Path(fx_dir).glob('*.fx.json')):
        spec = json.loads(jp.read_text(encoding='utf-8'))
        for stage in ('vertex', 'fragment'):
            for v in spec.get(stage, []):
                for h in v.get('hashes', []):
                    by_hash[h].append((spec['effect'], f'{spec["effect"]}_{v["name"]}_{stage}', stage))
    out = []
    for names in by_hash.values():
        if len({n[0] for n in names}) > 1:
            out.append((names[0][2], sorted({n[0] for n in names}), sorted({n[1] for n in names})))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fx-dir', default=str(ROOT / 'src' / 'renderer' / 'shaders' / 'fx'))
    args = ap.parse_args()
    g = groups(args.fx_dir)
    seen = set()
    for stage, effects, variants in g:
        key = (stage, tuple(effects))
        print(f'{stage:8} {" + ".join(effects)}: {len(variants)} variants: {", ".join(variants)}')
    print(f'{len(g)} shared hashes across effects')


if __name__ == '__main__':
    main()

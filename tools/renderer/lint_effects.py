#!/usr/bin/env python3
r"""Session 100: checks every variant of every hand-written effect against what its own shader source really samples.

  py tools\renderer\lint_effects.py [effect ...] [--quiet] [--fix]

For each variant it preprocesses the stage with the variant's defines and lists the texture slots the code samples (textures[i], cubes[i]; in a vertex shader
textures[i] is the game's vertex fetch slot 16 + i; check_effect.sampled_slots). It reports:
  ERROR undeclared   the code samples a slot the variant's "samplers" does not declare: the binder never binds it, so it reads an empty texture
  ERROR kind         the code samples a slot as a cube (or 2D) but the JSON declares the other kind
  note  unread       a declared slot the code never samples (one bind too many)
--fix rewrites each variant's "samplers" to exactly what its code samples (the solver does the same since S100). Exit code 1 when any ERROR is left.
Reads only our own shader files. The proof (check_effect.py) cannot catch this: its fake textures do not depend on the JSON.
"""
import glob
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FX = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders', 'harness'))
import check_effect as ce  # noqa: E402


def lint(effect, quiet, fix):
    path_json = os.path.join(FX, effect + '.fx.json')
    with open(path_json, encoding='utf-8') as f:
        spec = json.load(f)
    errors = 0
    changed = False
    for stage, ext, name in (('vertex', '.vert', 'vert'), ('fragment', '.frag', 'frag')):
        path = os.path.join(FX, effect + ext)
        if not os.path.exists(path):
            continue
        for variant in spec.get(stage, []):
            try:
                used, dynamic = ce.sampled_slots(ce.preprocess(path, name, list(variant.get('defines', []))), stage)
            except RuntimeError as e:
                print(f'ERROR {effect} {stage} {variant.get("name")}: does not preprocess: {str(e)[:120]}')
                errors += 1
                continue
            declared = {s['slot']: s.get('kind', '2d') for s in variant.get('samplers', [])}
            if dynamic:   # a loop over slots: what it declares beyond the literal reads is trusted
                used = dict(declared, **used)
            if fix and declared != used:
                variant['samplers'] = [{'slot': slot, 'kind': kind} for slot, kind in sorted(used.items())]
                print(f'fixed {effect} {stage} {variant["name"]}: {sorted(declared.items())} -> {sorted(used.items())}')
                changed = True
                continue
            for slot, kind in sorted(used.items()):
                if slot not in declared:
                    print(f'ERROR {effect} {stage} {variant["name"]}: samples slot {slot} ({kind}) but does not declare it')
                    errors += 1
                elif declared[slot] != kind:
                    print(f'ERROR {effect} {stage} {variant["name"]}: slot {slot} sampled as {kind}, declared {declared[slot]}')
                    errors += 1
            if not quiet:
                for slot in sorted(set(declared) - set(used)):
                    print(f'note  {effect} {stage} {variant["name"]}: declares slot {slot} but never samples it')
    if changed:
        with open(path_json, 'w', encoding='utf-8', newline='\n') as out:
            json.dump(spec, out, indent=1)
            out.write('\n')
    return errors


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    quiet = '--quiet' in sys.argv
    fix = '--fix' in sys.argv
    effects = args or sorted(os.path.basename(p)[:-len('.fx.json')] for p in glob.glob(os.path.join(FX, '*.fx.json')))
    errors = sum(lint(e, quiet, fix) for e in effects)
    print(f'{len(effects)} effects, {errors} errors')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())

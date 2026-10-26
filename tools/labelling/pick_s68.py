"""Session 68: the next labelling selection = Session 10's bulk order, minus what no longer needs a model.

    py tools\\labelling\\pick_s68.py [--batch-size 40] [--wave-batches 10]

Keeps pick_bulk.py's order (leaf first, then most-called) and its fields, and drops every function that census.py
does not leave for a model: labelled since, library blocks (Havok, CRT, ...), thunks / trivial functions, clones of
a labelled function and the 2nd+ members of clone clusters. Key-system functions stay out (main session). Re-batches
into batches of --batch-size (one packed agent each) and waves of --wave-batches batches, named x01, x01b01, ...

Reads out\\harness\\labelling\\bulk\\selection.json and out\\harness\\labelling\\census_s68.json (run census.py --json
first); writes out\\harness\\labelling\\s68\\selection.json in the selection format export.py --selection reads.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
LAB = os.path.join(REPO, 'out', 'harness', 'labelling')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--batch-size', type=int, default=40)
    ap.add_argument('--wave-batches', type=int, default=10)
    ap.add_argument('--out', default=os.path.join(LAB, 's68', 'selection.json'))
    args = ap.parse_args()

    with open(os.path.join(LAB, 'bulk', 'selection.json'), encoding='utf-8') as f:
        sel = json.load(f)
    with open(os.path.join(LAB, 'census_s68.json'), encoding='utf-8') as f:
        left = set(json.load(f)['left'])

    keep = [fn for fn in sel['functions'] if fn['address'] in left and not fn.get('key')]
    batches = {}
    for i, fn in enumerate(keep):
        b, w = i // args.batch_size, i // (args.batch_size * args.wave_batches)
        fn = dict(fn, order=i + 1, wave=f'x{w + 1:02d}', batch=f'x{w + 1:02d}b{b % args.wave_batches + 1:02d}')
        keep[i] = fn
        batches.setdefault(fn['batch'], []).append(fn['address'])
    out = dict(sel, note='Session 68 selection (tools/labelling/pick_s68.py): Session 10 bulk order minus library, '
               'mechanical, clone and labelled functions. Derived from the game: out/ only.',
               batches=batches, functions=keep)
    out['counts'] = dict(functions=len(keep), batches=len(batches),
                         waves=len({fn['wave'] for fn in keep}), from_selection=len(sel['functions']))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=1)
    ins = sorted(fn['n_instructions'] for fn in keep)
    print(f'{len(keep):,} of {len(sel["functions"]):,} selection functions left for a model -> {args.out}')
    print(f'{len(batches)} batches of {args.batch_size}, {out["counts"]["waves"]} waves; instructions: total '
          f'{sum(ins):,}, median {ins[len(ins) // 2]}, p90 {ins[int(0.9 * (len(ins) - 1))]}')


if __name__ == '__main__':
    sys.exit(main())

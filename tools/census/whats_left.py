r"""What is still the game's, across captures (S101 lane 125): reads leftovers.py outputs and prints one ranked table.

  py -I tools\census\whats_left.py NAME [NAME ...]  [--md FILE]

NAME = a folder under the main checkout's out\s101\leftovers\ (made by `leftovers.py ... --name NAME`). Every frame of every census is one column. Rows = the stream's
draws grouped by (class, technique, call site, pass site); each cell = stream draws / stream primitives / share of the sampled picture the group owns (stream share).
Reads the json only (counts and names, nothing from the game); game-derived output is only the cell numbers.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def main_checkout() -> Path:
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    here = Path(__file__).resolve().parents[2]
    try:
        common = subprocess.run(['git', '-C', str(here), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return here


def load(names):
    root = main_checkout() / 'out' / 's101' / 'leftovers'
    cols = []   # (label, frame json)
    for n in names:
        p = root / n / 'leftovers.json'
        if not p.exists():
            sys.exit(f'FAIL {p} missing')
        d = json.loads(p.read_text(encoding='utf-8'))
        for f in d['frames']:
            unproven = d.get('unproven') or not f.get('complete', True)   # lane 143: incomplete frame or failed / missing proof bar
            cols.append((f"{n}:{f['frame']}" + (' (UNPROVEN)' if unproven else ''), f))
    return cols


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='+')
    ap.add_argument('--md')
    a = ap.parse_args()
    cols = load(a.names)
    if not cols:
        sys.exit('FAIL no frames')
    lines = []
    lines.append('| capture:frame | draws | stream | ours | stream prims of all | picture stream % | ours % |')
    lines.append('|---|---|---|---|---|---|---|')
    for lab, f in cols:
        s = max(f['samples'], 1)
        lines.append(f"| {lab} | {f['draws']} | {f['stream_draws']} | {f['promoted_draws']} | {f['prims_stream']:,} of {f['prims_total']:,} | "
                     f"{100 * f['samples_owned_by_stream'] / s:.1f} | {100 * f['samples_owned_by_ours'] / s:.1f} |")
    lines.append('')
    rows = {}
    for lab, f in cols:
        for r in f['by_site']:
            if r['stream'] == 0:
                continue
            k = tuple(r['key'])
            rows.setdefault(k, {})[lab] = r
    labs = [c[0] for c in cols]
    samples = {lab: max(f['samples'], 1) for lab, f in cols}

    def rank(item):
        k, cells = item
        return -max(100 * c['own_stream'] / samples[lab] for lab, c in cells.items())

    lines.append('| class | technique | call site | pass site | ' + ' | '.join(labs) + ' |')
    lines.append('|---|---|---|---|' + '---|' * len(labs))
    for k, cells in sorted(rows.items(), key=rank):
        out = []
        for lab in labs:
            c = cells.get(lab)
            out.append('-' if not c else f"{c['stream']} / {c['prims_stream']:,} / {100 * c['own_stream'] / samples[lab]:.1f}%")
        lines.append('| ' + ' | '.join(k) + ' | ' + ' | '.join(out) + ' |')
    txt = '\n'.join(lines)
    print(txt)
    if a.md:
        Path(a.md).write_text(txt + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()

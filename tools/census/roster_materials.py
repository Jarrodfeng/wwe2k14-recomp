r"""Lane 158 (S101): the material families of the roster's wrestlers, read OFFLINE from the drawables atlas (out\atlas\atlas.json, tools\atlas\atlas.exe).

What it answers: for each character pac pac/ch/chNNN.pac, which materials ("effect" = the game's material class, yCh_*) do its meshes use, per entry
(entry = one attire / one piece of the model), and how many meshes / bone-palette sizes. The roster-wide match route picks wrestlers whose models differ
in these families (masked, long hair, alpha-heavy, cloth), so a promotion that only worked for the test scenes' people cannot hide.

  py -I tools\census\roster_materials.py                      the family table for the picks in db\ids\characters.json (ids below) -> stdout
  py -I tools\census\roster_materials.py --ids 343,324,139    only these character ids
  py -I tools\census\roster_materials.py --all [--json OUT]   every ch pac
  py -I tools\census\roster_materials.py --selftest           synthetic atlas lines: grouping + family flags (no game files)

Output is counts and the game's material class names (a family name, not code); it is printed, never committed (game-derived).
The atlas is the main checkout's out\atlas\atlas.json (made once; `tools\atlas\build.ps1` + `atlas.exe --all`).
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


CH_RE = re.compile(r'^pac/ch/ch(\d{3,4})\.pac$', re.I)


def scan(lines, want=None):
    """lines: iterable of atlas.json lines. -> {char_id: {'entries': {entry: Counter(effect)}, 'effects': Counter, 'palette_max': int, 'meshes': int}}."""
    out = {}
    for ln in lines:
        ln = ln.strip().rstrip(',')
        if '"type":"mesh"' not in ln.replace(' ', ''):
            continue
        m = re.search(r'"pac"\s*:\s*"([^"]+)"', ln)
        if not m:
            continue
        cm = CH_RE.match(m.group(1))
        if not cm:
            continue
        cid = int(cm.group(1))
        if want is not None and cid not in want:
            continue
        try:
            rec = json.loads(ln)
        except ValueError:
            continue
        c = out.setdefault(cid, {'entries': collections.defaultdict(collections.Counter), 'effects': collections.Counter(), 'palette_max': 0, 'meshes': 0, 'param_names': collections.Counter()})
        c['entries'][rec.get('entry', '?')][rec.get('effect', '?')] += 1
        c['effects'][rec.get('effect', '?')] += 1
        c['palette_max'] = max(c['palette_max'], rec.get('palette', 0) or 0)
        c['meshes'] += 1
        for p in rec.get('params', []):
            if p.get('t') == 'sampler' and p.get('tex'):
                c['param_names'][p['n']] += 1
    return out


def families(c):
    """A short list of family tags for one character: the distinct effect names plus a few flags read from them (alpha / cloth / hair words in the class name)."""
    eff = sorted(c['effects'])
    flags = []
    for e in eff:
        lo = e.lower()
        for key, tag in (('trans', 'alpha'), ('hair', 'hair'), ('cloth', 'cloth'), ('skin', 'skin'), ('metal', 'metal'), ('mask', 'mask')):
            if key in lo and tag not in flags:
                flags.append(tag)
    return eff, flags


def render(table, names):
    L = ['| id | name | meshes | max bone palette | material classes (meshes) | flags |', '|---|---|---|---|---|---|']
    for cid in sorted(table):
        c = table[cid]
        eff, flags = families(c)
        L.append(f"| {cid} | {names.get(cid, '?')} | {c['meshes']} | {c['palette_max']} | " + ', '.join(f"{e} {c['effects'][e]}" for e in eff) + ' | ' + (', '.join(flags) or '-') + ' |')
    return '\n'.join(L)


def selftest():
    sample = [
        '{"type":"mesh","pac":"pac/ch/ch343.pac","entry":"p0/1","mesh":0,"effect":"yCh_skin","palette":30,"params":[{"n":"sampDiffusePrm","t":"sampler","tex":"body"}]},',
        '{"type":"mesh","pac":"pac/ch/ch343.pac","entry":"p0/1","mesh":1,"effect":"yCh_trans","palette":40,"params":[]},',
        '{"type":"mesh","pac":"pac/ch/ch324.pac","entry":"p0/2","mesh":0,"effect":"yCh_hair","palette":12,"params":[]},',
        '{"type":"mesh","pac":"pac/bg/bg00.pac","entry":"p0/0","mesh":0,"effect":"yBG_Default","palette":1,"params":[]},',
        '{"type":"texture","pac":"pac/ch/ch343.pac","entry":"p0/1","name":"x"},',
    ]
    t = scan(sample)
    assert sorted(t) == [324, 343], t.keys()
    assert t[343]['meshes'] == 2 and t[343]['palette_max'] == 40
    assert families(t[343])[1] == ['skin', 'alpha'] or set(families(t[343])[1]) == {'skin', 'alpha'}
    assert families(t[324])[1] == ['hair']
    assert scan(sample, want={324}).keys() == {324}
    print('roster_materials selftest: ok')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ids')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--json')
    ap.add_argument('--atlas')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    atlas = Path(a.atlas) if a.atlas else main_checkout() / 'out' / 'atlas' / 'atlas.json'
    if not atlas.exists():
        sys.exit(f'FAIL {atlas} missing: make it with tools\\atlas\\build.ps1 and atlas.exe --all (docs\\formats\\drawables_atlas.md)')
    names = {}
    ids_file = HERE / 'db' / 'ids' / 'characters.json'
    for e in json.loads(ids_file.read_text(encoding='utf-8'))['entries']:
        names[e['id']] = e.get('name')
    want = None if a.all else {int(x) for x in a.ids.split(',')} if a.ids else None
    if want is None and not a.all:
        sys.exit('give --ids A,B,C or --all')
    with open(atlas, encoding='utf-8', errors='replace') as fh:
        table = scan(fh, want)
    print(render(table, names))
    if a.json:
        Path(a.json).write_text(json.dumps({str(k): {'meshes': v['meshes'], 'palette_max': v['palette_max'], 'effects': dict(v['effects']), 'entries': {e: dict(c) for e, c in v['entries'].items()}} for k, v in table.items()}, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())

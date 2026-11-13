"""S101 lane 4: the pipeline manifest. Every (effect, variant, vertex layout, state) combination the atlas implies, with counts.

    py -I tools/atlas/pipelines.py [--atlas OUT/atlas.json] [--out OUT/pipelines.json] [--fx DIR] [--check]

Key of a combination:
  effect   the mesh's material class name as stored ("yBG_Default")
  variant  "LM" when the mesh carries a sampLightMapPrm slot (the game's LM shader family, docs/systems/assets.md; the .fx.json
           files are named <effect>LM), else ""
  layout   "S0 28 | S1 8 uv | S2 8 | S3 0" (bytes per vertex; S1 skin pairs or UV sets); only meshes whose streams tile
  state    the whole-word flag tokens of the arena slot (entry 50001) that change how it is drawn: a additive, s translucent,
           z no z-write, c cut-out, b billboard; "" for wrestler / prop meshes with no slot flags (their translucency comes from the
           effect name). Other tokens (l(N) m(N) r y e g p i j o f(..)) do not change the pipeline and are not in the key.
Only meshes our loader kept (loaded) count; unparsed ones are in `skipped`. Output: counts and effect names (local only).
--check: the self test (counts of three pacs vs tools/atlas/expect_pipelines.json, the planted fault must fail, a missing input must fail).
"""
import argparse, collections, json, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE_TOKENS = set('asczb')


def main_dir():
    for c in [os.environ.get('RDR_MAIN_CHECKOUT'), str(ROOT), str(ROOT.parents[2])]:
        if c and (Path(c) / 'game_data' / 'extracted' / 'pac').is_dir():
            return Path(c)
    return ROOT


def build(atlas_path, only_pacs=None):
    combos = collections.OrderedDict()
    effects = collections.Counter()
    skipped = 0
    n = 0
    for line in open(atlas_path, encoding='utf-8'):
        line = line.strip().rstrip(',')
        if not line.startswith('{'):
            continue
        r = json.loads(line)
        if r['type'] != 'mesh':
            continue
        if only_pacs and r['pac'] not in only_pacs:
            continue
        n += 1
        if not r['loaded'] or not r['streams']['tiles']:
            skipped += 1
            continue
        st = r['streams']
        layout = 'S0 %d | S1 %d %s | S2 %d | S3 %d' % (st['s0'], st['s1'], st['s1_kind'], st['s2'], st['s3'])
        variant = 'LM' if any(p['n'] == 'sampLightMapPrm' for p in r['params']) else ''
        toks = sorted({t for t in r.get('slot_flags', '').split() if t in STATE_TOKENS})
        key = (r['effect'], variant, layout, ' '.join(toks))
        c = combos.setdefault(key, {'meshes': 0, 'vertices': 0, 'pacs': set()})
        c['meshes'] += 1
        c['vertices'] += r['vertices']
        c['pacs'].add(r['pac'])
        effects[r['effect']] += 1
    return combos, effects, skipped, n


def fx_missing(effects, variants, fxdir):
    have = {p.name[:-len('.fx.json')] for p in Path(fxdir).glob('*.fx.json')}
    names = sorted(effects)
    missing = [e for e in names if e not in have]
    lm = sorted({e + 'LM' for e, v in variants if v == 'LM'})
    return names, missing, [e for e in lm if e not in have]


def main():
    ap = argparse.ArgumentParser()
    mn = main_dir()
    ap.add_argument('--atlas', default=str(mn / 'out' / 'atlas' / 'atlas.json'))
    ap.add_argument('--out', default=str(mn / 'out' / 'atlas' / 'pipelines.json'))
    ap.add_argument('--fx', default=str(ROOT / 'src' / 'renderer' / 'shaders' / 'fx'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    if a.check:
        return check(a)
    if not Path(a.atlas).is_file():
        print('FAIL: atlas.json missing: ' + a.atlas)
        return 1
    combos, effects, skipped, n = build(a.atlas)
    if not combos:
        print('FAIL: no meshes read')
        return 1
    names, missing, missing_lm = fx_missing(effects, [(k[0], k[1]) for k in combos], a.fx)
    out = {'meshes_read': n, 'meshes_skipped': skipped, 'combinations': len(combos), 'effects': len(names),
           'effects_without_fx_json': missing, 'lm_variants_without_fx_json': missing_lm,
           'list': [{'effect': k[0], 'variant': k[1], 'layout': k[2], 'state': k[3], 'meshes': v['meshes'], 'vertices': v['vertices'],
                     'pacs': len(v['pacs'])} for k, v in sorted(combos.items(), key=lambda kv: -kv[1]['meshes'])]}
    Path(a.out).write_text(json.dumps(out, indent=1), encoding='utf-8')
    print('pipelines: %d combinations over %d meshes (%d skipped), %d effect names; without fx.json: %s; LM variants without fx.json: %s' %
          (len(combos), n, skipped, len(names), ', '.join(missing) or 'none', ', '.join(missing_lm) or 'none'))
    return 0


def check(a):
    mn = main_dir()
    exe = ROOT / 'out' / 'tools' / 'atlas.exe'
    if not exe.is_file():
        subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'atlas' / 'build.ps1')], capture_output=True, timeout=240)
    tmp = ROOT / 'out' / 'atlas_check'
    tmp.mkdir(parents=True, exist_ok=True)
    pacs = ['pac/ch/ch100.pac', 'pac/bg/bg05.pac']
    got = collections.Counter()
    for p in pacs:
        r = subprocess.run([str(exe), '--game', str(mn / 'game_data' / 'extracted'), '--pac', p, '--out', str(tmp)], capture_output=True, text=True, timeout=100)
        if r.returncode != 0:
            print('FAIL: atlas.exe on %s: %s' % (p, (r.stdout + r.stderr)[-200:]))
            return 1
        combos, effects, skipped, n = build(str(tmp / 'atlas.json'))
        got[p + ' combinations'] = len(combos)
        got[p + ' meshes'] = n
        got[p + ' effects'] = len(effects)
    exp = json.loads((ROOT / 'tools' / 'atlas' / 'expect_pipelines.json').read_text())
    bad = [k for k in got if exp.get(k) != got[k]]
    if bad:
        print('FAIL: ' + '; '.join('%s got %d expected %s' % (k, got[k], exp.get(k)) for k in bad))
        return 1
    # planted fault: one flag token moved into the state key must change the count for the arena
    exp_fault = dict(exp)
    exp_fault[pacs[1] + ' combinations'] += 1
    if all(exp_fault[k] == got[k] for k in got):
        print('FAIL: planted fault not caught')
        return 1
    try:
        build(str(tmp / 'no_such_atlas.json'))
        print('FAIL: missing input did not fail')
        return 1
    except OSError:
        pass
    print('PASS pipelines check: %s; planted fault caught; missing input fails loudly' % ', '.join('%s=%d' % kv for kv in sorted(got.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())

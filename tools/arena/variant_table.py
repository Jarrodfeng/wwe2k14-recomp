r"""S103 P1 (review rework): the game's OWN pixel-shader variant for every arena mesh, read from a stream (0 game runs).

  py -I tools\arena\variant_table.py CAPTURE:ARENA [--frames A,B,C] [--json OUT] [--game DIR]

For each listed frame: `fview --ops --json` gives every draw's ps variant (the binder's name for the game's pixel-shader hash) and blend; tools\arena\join_census.py's join
(positions + index counts + texture bytes) names the (slot, mesh) the draw is. Output: slot / mesh -> {ps variants, blend on/off, draws}, and the file-side data of that mesh
(effect, texture, spec level, sampSpecularPrm, normal map, texture alpha stats, the record words) so a data rule can be tested against it (tools\arena\variant_rule.py).
Numbers only; nothing from the game is written (--json under out\)."""
import argparse
import collections
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import join_census as jc  # noqa: E402
import arena_files as af  # noqa: E402
import numpy as np  # noqa: E402


def fview_ops(fview, capture, frame, out, game):
    p = subprocess.run([fview, '--capture', capture, '--frame', str(frame), '--ops', '--json', out, '--root', ROOT, '--game', game], capture_output=True, text=True, errors='replace')
    if not os.path.exists(out):
        raise SystemExit('fview --ops failed on frame %d: %s' % (frame, (p.stdout + p.stderr)[-400:]))
    return json.load(open(out))['ops']


def joined_draws(cap, arena, fi, fn, cache):
    """-> {draw index: [(slot, mesh)]} for the frame's stage-site model-part draws (the census' join, ties kept as lists)."""
    cap.catch_up(fn)
    f = cap.frame(fn, None)
    cap.advance_contents(f, fn)
    o, d = f.origins, f.draws
    out = {}
    for i in range(len(d)):
        if o[i]['klass'] != jc.CLASS_MODEL_PART or int(o[i]['pass_site']) not in (jc.SITE_A, jc.SITE_B):
            continue
        part = int(o[i]['part'])
        m = jc.match_draw(cap, f, i, fi, cache)
        if m.get('nogeo') or not m['hits']:
            continue
        hits = [x for x in m['hits'] if x.index == part]
        if not hits:
            continue
        tex_img = next((int(s['image']) for s in f.slots[int(d[i]['tex_first']):int(d[i]['tex_first']) + int(d[i]['tex_count'])] if int(s['slot']) == 0), 0)
        if len(hits) > 1 and tex_img:
            payload = cap.image_data(tex_img)
            if payload is not None:
                keep = [x for x in hits if jc.diffuse_tex(arena, x) is not None and jc.compare_texture(payload, jc.diffuse_tex(arena, x)) in ('equal', 'tail', 'padded')]
                if keep:
                    hits = keep
        if len(hits) > 1:
            arrs = jc.draw_attr_arrays(cap, f, i)
            keep = [y for y in hits if jc.uv_matches(y, arrs)]
            if keep:
                hits = keep
        if len(hits) > 1:
            cols = jc.draw_colour_arrays(cap, f, i)
            keep = [y for y in hits if jc.colour_matches(y, cols)]
            if keep:
                hits = keep
        out[i] = sorted({(x.slot, x.index) for x in hits})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', required=True)
    ap.add_argument('--json', default='')
    ap.add_argument('--game', default=os.path.join(r'C:\Users\Jarrod\wwe2k14-recomp', 'game_data', 'extracted'))
    a = ap.parse_args()
    path, arena_id = a.capture.rsplit(':', 1)
    arena_id = int(arena_id)
    cap = jc.Capture(path)
    arena = af.load_arena(arena_id)
    fi = jc.FileIndex(arena, None)
    import fview_exe
    fview = str(fview_exe.find_fview(root=ROOT, allow_stale=True, quiet=True))
    work = os.path.join(ROOT, 'out', 's103', 'P1', 'vt')
    os.makedirs(work, exist_ok=True)
    table = collections.defaultdict(lambda: {'vs': collections.Counter(), 'ps': collections.Counter(), 'blend': collections.Counter(), 'ambiguous': 0})
    cache = {}
    for fn in [int(x) for x in a.frames.split(',')]:
        ops = fview_ops(fview, path, fn, os.path.join(work, 'ops_%d.json' % fn), a.game)
        byidx = {o['index']: o for o in ops if o['kind'] == 'draw'}
        for i, cands in joined_draws(cap, arena, fi, fn, cache).items():
            o = byidx.get(i)
            if not o or not o.get('ps_variant'):
                continue
            for sm in cands:
                t = table[sm]
                t['ps'][o['ps_variant']] += 1
                t['vs'][o.get('vs_variant', '')] += 1  # S103 L8: the vertex variant too (the vertex-fetch light-map class)
                t['blend'][bool(o['blend0'])] += 1
                t['ambiguous'] += len(cands) > 1
    rows = []
    for (slot, mesh), t in sorted(table.items()):
        m = arena.slots[slot][mesh] if slot in arena.slots else None
        rows.append({'slot': slot, 'mesh': mesh, 'vs': dict(t['vs']), 'ps': dict(t['ps']), 'blend': {str(k): v for k, v in t['blend'].items()}, 'ambiguous': t['ambiguous'],
                     'effect': m.effect if m else None, 'tex': m.samplers.get('sampDiffusePrm') if m else None})
    for r in rows:
        print(r['slot'], r['mesh'], r['tex'], r['effect'], r['ps'], r['blend'], 'AMBIG' if r['ambiguous'] else '')
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()

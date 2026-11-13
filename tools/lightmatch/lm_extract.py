r"""Lane 33 (S101): extract the camera and the light constants of every named draw class from a capture frame.

  py -I tools\lightmatch\lm_extract.py ZIP_OR_CAPTURE_DIR [--frames N,N,..|all] [--out DIR] [--allow-unnamed]

Per frame: the camera (VS c56..c59 = the combined view-projection, c72 = eye, c52..c54 = the view rows when a character effect carries them)
and, per light-set group, the constants the game fed: DifLghVec / DifLghCol (4 lights), SkyDir / SkyCol / GroundCol, the light limit and the
fog. A "group" = draws of one class (arena / prop / crowd / ring / wrestler ...) that were fed the identical tuple; the group keeps the
draw count and one example object / part name. Output out\s101\lightmatch\<capture>\frame_N.json (game values: LOCAL ONLY, never committed).

A capture whose draw names (NAMES.md origins) are stripped FAILS loudly (exit 2) unless --allow-unnamed: "0 lights" must never be a quiet
answer (LESSONS: a checker that reads nothing).
Reads capture bytes in place through tools\solver\capread.py; writes nothing next to them.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'solver'))
import capread  # noqa: E402
import solve  # noqa: E402  (Effects: shader hash -> effect + constant registers)

MAIN = solve.MAIN
OUT = MAIN / 'out' / 's101' / 'lightmatch'
KLASS = solve.KLASS

# constant fields that make up "the light set" of a draw, with the register run the fx description gives (vertex / fragment bank)
LIGHT_FIELDS = ('DifLghVec', 'DifLghCol', 'SkyDir', 'SkyCol', 'GroundCol', 'LightLimit', 'fvLightLimit', 'DifLghMax', 'DifLghMaxA',
                'DifLghMaxD', 'LightMin', 'KeyLightVec', 'KeyLightColor', 'FogColor', 'FogDistance', 'EyePos', 'View', 'LightMapParam')
CAMERA_FIELDS = ('ViewProj', 'EyePos', 'View')


def r6(v):
    return [round(float(x), 5) for x in v]


def frame_json(src, fx, name, strings_ok):
    fr = capread.Frame(src.read(name))
    if not fr.has_origins and strings_ok:
        raise SystemExit(f'FAIL  {name}: no draw names (origins section): a capture with its names stripped cannot say which light set is which draw')
    groups = collections.OrderedDict()
    cams = collections.Counter()
    eyes = collections.Counter()
    cam = None
    nd = len(fr.draws)
    unresolved = 0
    for i in range(nd):
        d = fr.draws[i]
        label, per_stage, why = fx.resolve(int(d['vs_hash']), int(d['ps_hash']))
        if label is None:
            unresolved += 1
            continue
        org = fr.origins[i] if fr.has_origins else None
        klass = KLASS.get(int(org['klass']), '?') if org is not None else 'unnamed'
        vals = {}
        for stage, bank_i in (('vertex', int(d['vs_bank'])), ('fragment', int(d['ps_bank']))):
            for (field, reg, cnt) in per_stage.get(stage, []):
                if field not in LIGHT_FIELDS and field not in CAMERA_FIELDS:
                    continue
                if bank_i >= len(fr.banks):
                    continue
                v = fr.reg(bank_i, reg, cnt)
                vals.setdefault(field, {})[stage[0]] = v
        if not vals:
            continue
        # the camera: the first scene draw that carries ViewProj (VS c56)
        if 'ViewProj' in vals and 'v' in vals['ViewProj'] and vals['ViewProj']['v'].shape[0] == 4:
            vp = vals['ViewProj']['v']
            if np.isfinite(vp).all() and abs(vp).max() > 0:
                cams[tuple(r6(vp.reshape(-1)))] += 1
        if 'EyePos' in vals and 'v' in vals['EyePos'] and 'ViewProj' in vals:
            ey = vals['EyePos']['v'].reshape(-1)[:3]
            if np.isfinite(ey).all():
                eyes[tuple(r6(ey))] += 1
        key = [label, klass]
        for f in ('DifLghVec', 'DifLghCol', 'SkyDir', 'SkyCol', 'GroundCol', 'LightLimit', 'fvLightLimit', 'DifLghMax', 'DifLghMaxA', 'LightMin',
                  'KeyLightVec', 'KeyLightColor'):
            if f in vals:
                for st in sorted(vals[f]):
                    key.append((f, st, tuple(r6(vals[f][st].reshape(-1)))))
        key = tuple(map(lambda x: x if isinstance(x, str) else tuple(x), key))
        g = groups.get(key)
        if g is None:
            g = groups[key] = dict(effect=label, klass=klass, draws=0, example=dict(draw=i, object=int(org['object']) if org is not None else -1,
                                                                                  part=int(org['part']) if org is not None else -1), vals={})
            for f, byst in vals.items():
                g['vals'][f] = {st: r6(v.reshape(-1)) for st, v in byst.items()}
        g['draws'] += 1
    cam = None
    if cams:
        vp, n = cams.most_common(1)[0]
        cam = dict(viewproj_rows=list(vp), draws_with_it=n)
        if eyes:
            cam['eye'] = list(eyes.most_common(1)[0][0])
    return dict(frame=name, capture_frame=int(fr.capture_frame), guest_frame=int(fr.guest_frame), version=int(fr.version), draws=nd,
                unresolved=unresolved, has_origins=bool(fr.has_origins), camera=cam, groups=list(groups.values()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--allow-unnamed', action='store_true')
    args = ap.parse_args()
    src = capread.Source(args.capture)
    fx = solve.Effects()
    names = src.frames
    if args.frames and args.frames != 'all':
        want = {int(x) for x in args.frames.split(',')}
        names = [n for n in names if int(re.search(r'frame_(\d+)', n).group(1)) in want]
    elif not args.frames:
        names = names[:: max(1, len(names) // 6)][:6]
    if not names:
        raise SystemExit('FAIL  no frames selected')
    stem = Path(args.capture).stem if Path(args.capture).suffix == '.zip' else Path(args.capture).name
    outdir = Path(args.out) if args.out else OUT / stem
    outdir.mkdir(parents=True, exist_ok=True)
    total_groups = 0
    for n in names:
        j = frame_json(src, fx, n, not args.allow_unnamed)
        num = int(re.search(r'frame_(\d+)', n).group(1))
        (outdir / f'frame_{num}.json').write_text(json.dumps(j, indent=1), encoding='utf-8')
        total_groups += len(j['groups'])
        print(f"{n}: {j['draws']} draws, {len(j['groups'])} light groups, camera {'yes' if j['camera'] else 'NO'}, unresolved {j['unresolved']}")
        if not j['camera']:
            raise SystemExit(f'FAIL  {n}: no camera found (no draw carried the view-projection)')
    if total_groups == 0:
        raise SystemExit('FAIL  0 light groups in every frame: nothing was read (a checker that reads nothing must fail)')
    print(f'OK  {len(names)} frames -> {outdir}')


if __name__ == '__main__':
    main()

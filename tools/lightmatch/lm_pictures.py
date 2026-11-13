r"""Lane 33 (S101): the viewer at the game's own camera, beside the game's captured picture, before and after the lane's fixes.

  py -I tools\lightmatch\lm_pictures.py --old EXE_BEFORE --new EXE_AFTER [--rdr-old EXE --rdr-new EXE] [--cases bryan,wm30,brock]

Per case (a case zip's LAST capture frame = the picture.png of the case) the viewer is run headless at that frame's camera (c56..c59 / c72)
and, for an entrance, at the clock the light fit found, with post OFF (shadows, DoF, FXAA, bloom, glare) and the game's display curve ON; the
game's picture has its post baked in, which cannot be turned off, so the numbers are a coarse tile comparison (8 x 6 tiles of mean colour,
the structure correlation of the lane-6 tile check) and the sheet shows what remains. Writes out\s101\lightmatch\sheet.png (every panel
labelled) and pictures.json (the numbers). Game pictures and the sheet are game imagery: local only, never committed.
"""
import argparse
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFile, ImageFont

ImageFile.LOAD_TRUNCATED_IMAGES = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lm_viewer as V  # noqa: E402

MAIN = V.MAIN
OUT = MAIN / 'out' / 's101' / 'lightmatch'
SIZE = (1280, 720)
POST_OFF = {'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'film': False}  # (matched outputs: no curve, no film filter on either backend; the curve is applied below)

# case -> (zip glob under out\s101\runs, picture frame, arena, camera frames dir, entrant (wrestler id) or None, clock (cue, local) or None,
#          wrestlers for a match, label)
CASES = {
    'bryan': dict(run='*bryan_entrance', frame=90, arena=4, wrestler=324, clock=None, match=None, label='Bryan entrance, OTL (arena 4)'),
    'brock': dict(run='*175051_s100_owner_s89_brock', frame=330, arena=4, wrestler=343, clock=None, match=None, label='Brock ring-in, OTL (arena 4)'),
    'wm30': dict(run='*s90_wm30', frame=422, arena=29, wrestler=None, clock=None, match=(196, 109), label='30 Years of WM cage match (arena 29)'),
}


def case_zip(pattern):
    zs = sorted((MAIN / 'out' / 's101' / 'runs').glob(pattern + '/case_*.zip'))
    if not zs:
        raise SystemExit(f'FAIL  no case zip for {pattern}')
    return zs[-1]


def game_picture(zpath):
    z = zipfile.ZipFile(zpath)
    tmp = OUT / '_pics' / (zpath.stem + '_picture.png')
    tmp.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_bytes(z.read('picture.png'))
    im = Image.open(tmp).convert('RGB')
    return im.resize(SIZE, Image.LANCZOS)


def viewer_shot(exe, backend, case, cam, clock, shot_path, post_on, tag):
    cmds = [dict(POST_OFF)]
    if post_on:
        cmds[0].update(shadows=True, dof=True, fxaa=True, bloom=True, glare=True)
    cmds.append({'cmd': 'lights', 'haze': False})  # (haze is vk-only post; off on both)
    cmds.append({'cmd': 'load', 'kind': 'arena', 'id': case['arena']})
    if case['wrestler']:
        cmds.append({'cmd': 'entrance', 'wrestler': case['wrestler'], 'attire': 0, 'play': False, 'camera': 'free'})
    else:
        for i, w in enumerate(case['match']):
            cmds.append({'cmd': 'place', 'wrestler': w, 'attire': 0, 'spot': 'ring', 'id': f'w{i + 1}', 'motion': 'idle', 'frame': 30})
    cmds.append({'cmd': 'camera', 'viewproj': cam['viewproj_rows'], 'eye': cam.get('eye', [0, 0, 0])})
    if clock:
        cmds.append({'cmd': 'entrance_time', 'cue': clock[0], 'f': clock[1], 'pause': True})
    cmds.append({'cmd': 'shot', 'path': str(shot_path), 'w': SIZE[0], 'h': SIZE[1]})
    cmds.append({'cmd': 'quit'})
    wd = OUT / '_pic_work'
    wd.mkdir(parents=True, exist_ok=True)
    script = wd / f'{tag}.jsonl'
    script.write_text('\n'.join(json.dumps(c) for c in cmds) + '\n', encoding='utf-8')
    p = subprocess.run([exe, '--backend', backend, '--game', str(MAIN / 'game_data' / 'extracted'), '--script', str(script), '--exit', '--hidden'],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0 or not Path(shot_path).exists():
        raise SystemExit(f'FAIL  viewer ({exe}, {backend}) gave no picture for {tag}: rc {p.returncode} {p.stderr[-300:]}')
    return Image.open(shot_path).convert('RGB').resize(SIZE, Image.LANCZOS)


def curve(im):
    """The game's display curve (BT.709 encode of the sRGB decode, viewer_hlsl.h Bt709Encode(SrgbDecode)), for a picture read without it."""
    a = np.asarray(im, dtype=np.float64) / 255.0
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    enc = np.where(lin < 0.018, 4.5 * lin, 1.099 * np.power(np.maximum(lin, 1e-9), 0.45) - 0.099)
    return Image.fromarray((np.clip(enc, 0, 1) * 255 + 0.5).astype(np.uint8))


def tiles(im, gx=8, gy=6):
    a = np.asarray(im, dtype=np.float64)
    h, w, _ = a.shape
    out = np.zeros((gy, gx, 3))
    for j in range(gy):
        for i in range(gx):
            out[j, i] = a[j * h // gy:(j + 1) * h // gy, i * w // gx:(i + 1) * w // gx].reshape(-1, 3).mean(0)
    return out


def lumg(im, gx=64, gy=36):
    a = np.asarray(im.resize((gx, gy), Image.BOX), dtype=np.float64)
    return 0.3 * a[..., 0] + 0.59 * a[..., 1] + 0.11 * a[..., 2]


def metrics(game, got):
    tg, tv = tiles(game), tiles(got)
    lg, lv = 0.3 * tg[..., 0] + 0.59 * tg[..., 1] + 0.11 * tg[..., 2], 0.3 * tv[..., 0] + 0.59 * tv[..., 1] + 0.11 * tv[..., 2]
    ga, va = lumg(game).ravel(), lumg(got).ravel()
    corr = float(np.corrcoef(ga, va)[0, 1]) if ga.std() > 1e-6 and va.std() > 1e-6 else 0.0
    return dict(mean_tile_abs_err=round(float(np.abs(tg - tv).mean()), 2), mean_lum_game=round(float(lg.mean()), 2), mean_lum_viewer=round(float(lv.mean()), 2),
                lum_ratio=round(float(lv.mean() / max(lg.mean(), 1e-6)), 3), tile_lum_ratio_median=round(float(np.median(lv / np.maximum(lg, 1e-3))), 3),
                structure_corr=round(corr, 3))


def label(im, text):
    im = im.copy()
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.width, 22], fill=(0, 0, 0))
    d.text((6, 4), text, fill=(255, 255, 255))
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old', required=True)
    ap.add_argument('--new', required=True)
    ap.add_argument('--rdr-old', default='')
    ap.add_argument('--rdr-new', default='')
    ap.add_argument('--cases', default='bryan,wm30,brock')
    ap.add_argument('--clocks', default='bryan:15:208,brock:15:520', help='case:cue:local for the entrances (from lm_compare)')
    a = ap.parse_args()
    clocks = {c.split(':')[0]: (int(c.split(':')[1]), int(c.split(':')[2])) for c in a.clocks.split(',') if c}
    res, rows = {}, []
    for name in a.cases.split(','):
        case = dict(CASES[name])
        zpath = case_zip(case['run'])
        game = game_picture(zpath)
        cam = json.loads((OUT / name / f"frame_{case['frame']}.json").read_text(encoding='utf-8'))['camera']
        clock = clocks.get(name) if case['wrestler'] else None
        panels = [label(game, f'GAME (case picture): {case["label"]}')]
        res[name] = {'clock': clock, 'runs': {}}
        raw = {}
        for tag, exe, backend in (('vk before', a.old, 'vk'), ('vk after', a.new, 'vk'), ('rdr before', a.rdr_old, 'rdr'), ('rdr after', a.rdr_new, 'rdr')):
            if not exe:
                continue
            for post_on in (False, True):
                if post_on and backend == 'rdr':
                    continue
                shot = OUT / '_pic_work' / f'{name}_{tag.replace(" ", "_")}_{"post" if post_on else "nopost"}.png'
                got = viewer_shot(exe, backend, case, cam, clock, shot, post_on, f'{name}_{tag.replace(" ", "_")}_{post_on}')
                if not post_on:
                    raw[(tag, backend)] = got
                got = curve(got)
                m = metrics(game, got)
                res[name]['runs'][f'{tag}{" +post" if post_on else ""}'] = m
                print(name, tag, 'post' if post_on else 'no post', m)
                if not post_on:
                    panels.append(label(got, f'{tag.upper()} (post off)  tile err {m["mean_tile_abs_err"]}  lum x{m["lum_ratio"]}  corr {m["structure_corr"]}'))
        for t in ('before', 'after'):
            if (f'vk {t}', 'vk') in raw and (f'rdr {t}', 'rdr') in raw:
                a_, b_ = metrics(raw[(f'vk {t}', 'vk')], raw[(f'rdr {t}', 'rdr')])['lum_ratio'], None
                res[name]['rdr_over_vk_lum_ratio_' + t] = a_  # (metrics(game=vk, got=rdr).lum_ratio = rdr / vk)
                print(name, t, 'rdr / vk mean luminance (both without curve, haze, film):', res[name]['rdr_over_vk_lum_ratio_' + t])
        rows.append(panels)
    w = SIZE[0] // 2
    h = SIZE[1] // 2
    ncol = max(len(r) for r in rows)
    sheet = Image.new('RGB', (w * ncol, h * len(rows)), (20, 20, 20))
    for j, r in enumerate(rows):
        for i, p in enumerate(r):
            sheet.paste(p.resize((w, h), Image.LANCZOS), (i * w, j * h))
    sheet.save(OUT / 'sheet.png')
    (OUT / 'pictures.json').write_text(json.dumps(res, indent=1), encoding='utf-8')
    print('OK ->', OUT / 'sheet.png')


if __name__ == '__main__':
    main()

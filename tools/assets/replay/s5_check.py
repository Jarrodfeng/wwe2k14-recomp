r"""Frame replay S5 (Session 98, 0 game runs): the four run-6818 captures, game | viewer before | viewer after, at full resolution with
the game's camera. Per wrestler part: where it sits (centroid offset in px and overlap of the game's coverage with the viewer's own
pixels of the same meshes) and how bright it is (mean linear colour over each side's own pixels; the game's 8-bit target is PWL-gamma
decoded, the viewer shot raw: no gamma / DoF / bloom / glare / FXAA / film). One viewer launch per exe (all captures in one script).

  py -I tools\assets\replay\s5_check.py --before <main exe> --after <this branch's exe> --out <dir> [--reuse]
Parts are lists of dumped colour draws (rdc_draw.ps1) unioned; the viewer's meshes are the ones with the same vertex counts.
[entrant-specific] CAPTURES below = Orton's draws / cue frames of run 6818. Everything stays under out\ (rule 1).
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rdraw  # noqa: E402

GAME = r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted'
R = r'C:\Users\Jarrod\wwe2k14-recomp\out\s98\replay'
S34, S5 = os.path.join(R, 's3s4'), os.path.join(R, 's5')
# capture -> (cue, cue-local frame, {part: [draw dirs]}). Frames: rp_ringin = 767 (its lights equal the viewer rig at 767 to 1e-3 and the
# motion fits 767 / 768; 766 was one early), the others as the capture script waited (motion fit 428 at the ramp, 121-122 in the ring).
CAPTURES = {
    'rp_stage': (5, 576, {'head': ['rp_stage/parts/ev8877', 'rp_stage/parts/ev8892'], 'arm gear': ['rp_stage/parts/ev8865'],
                          'eyes': ['s3s4:rp_stage/eyes/ev8979']}),
    'rp_ramp': (10, 428, {'skin': ['rp_ramp/parts/ev10134', 'rp_ramp/parts/ev10139'], 'head': ['rp_ramp/parts/ev10107', 'rp_ramp/parts/ev10122'],
                          'arm gear': ['rp_ramp/parts/ev10095'], 'eyes': ['s3s4:rp_ramp/eyes/ev10209']}),
    'rp_ringin': (15, 767, {'skin': ['s3s4:rp_ringin/parts/ev11003'], 'head': ['s3s4:rp_ringin/parts/ev10976', 's3s4:rp_ringin/parts/ev10991'],
                            'shirt': ['s3s4:rp_ringin/parts/ev10969'], 'arm gear': ['s3s4:rp_ringin/parts/ev10964'],
                            'eyes': ['s3s4:rp_ringin/eyes/ev11078']}),
    'rp_ring': (20, 122, {'skin': ['rp_ring/parts/ev11048', 'rp_ring/parts/ev11053'], 'arm gear': ['rp_ring/parts/ev11009']}),
}


def path_of(p):
    return os.path.join(S34, p[5:]) if p.startswith('s3s4:') else os.path.join(S5, p)


def pwl_decode(x):
    """Inverse of the console's piecewise-linear gamma (rdraw.pwl_gamma), on 0..1 values."""
    x = np.asarray(x, np.float64) * 255.0
    out = np.where(x < 64, x / 1023.0, np.where(x < 96, (x - 32) * 2 / 1023.0, np.where(x < 192, (x - 64) * 4 / 1023.0, (x - 128) * 8 / 1023.0)))
    return np.clip(out, 0, 1)


def run(exe, out, tag, nverts_by_part, banks):
    media = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons').replace('\\', '/')
    raw = json.dumps({'cmd': 'look', 'gamma': False, 'dof': False, 'bloom': False, 'glare': False, 'fxaa': False, 'film': False})
    lines = ['{"cmd":"load","kind":"arena","id":5}', json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140}),
             json.dumps({'cmd': 'stage', 'media': media, 'wrestler': 161})]
    shot = lambda n, W, H: json.dumps({'cmd': 'shot', 'path': os.path.join(out, n + '.png').replace('\\', '/'), 'w': W, 'h': H})
    for cap, (cue, f, parts) in CAPTURES.items():
        d0 = rdraw.Draw(path_of(next(iter(parts.values()))[0]))
        W, H = d0.vw, d0.vh
        lines += [json.dumps({'cmd': 'entrance', 'wrestler': 161, 'attire': 1, 'cue': cue, 'f': f, 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}),
                  json.dumps({'cmd': 'camera', 'viewproj': [float(x) for x in d0.vs_c[56:60].reshape(-1)], 'eye': [float(x) for x in d0.vs_c[72][:3]]}),
                  json.dumps({'cmd': 'look', 'gamma': True, 'dof': True, 'bloom': True, 'glare': True, 'fxaa': True, 'film': True}), shot('%s_%s_look' % (tag, cap), W, H),
                  raw, shot('%s_%s' % (tag, cap), W, H), json.dumps({'cmd': 'entrance_info'})]
        for part, nvs in nverts_by_part[cap].items():
            ms = [x['mesh'] for x in banks['draws'] if x['model'] == banks['model'] and x.get('verts') in nvs]
            lines += [json.dumps({'cmd': 'replay', 'clear': True, 'draws': [{'model': banks['model'], 'mesh': m, 'cb': {'mat_dif': [0, 0, 0, 1], 'mat_amb': [0, 0, 0, 1]}} for m in ms]}),
                      json.dumps({'cmd': 'look', 'debug': 13}), shot('%s_%s_M_%s' % (tag, cap, part.replace(' ', '_')), W, H),
                      json.dumps({'cmd': 'replay', 'clear': True}), shot('%s_%s_N_%s' % (tag, cap, part.replace(' ', '_')), W, H), json.dumps({'cmd': 'look', 'debug': 0})]
    script = os.path.join(out, 'script_%s.jsonl' % tag)
    open(script, 'w').write('\n'.join(lines) + '\n')
    p = subprocess.run([exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    open(os.path.join(out, 'viewer_%s.log' % tag), 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad:
        print(tag, 'viewer errors:', bad[:3])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--before', required=True)
    ap.add_argument('--after', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--bank', required=True, help='a bank_dump of the entrant (mesh indices by vertex count)')
    ap.add_argument('--reuse', action='store_true')
    ap.add_argument('--after-only', action='store_true', help='keep the before shots already in --out')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    b = json.load(open(a.bank))
    b['model'] = next(i for i, m in enumerate(b['models']) if m.get('has_wrestler'))
    draws = {cap: {part: [rdraw.Draw(path_of(p)) for p in ps] for part, ps in parts.items()} for cap, (_, _, parts) in CAPTURES.items()}
    nv = {cap: {part: {d.meta['postvs']['bytes'] // d.meta['postvs']['stride'] for d in ds} for part, ds in parts.items()} for cap, parts in draws.items()}
    if not a.reuse:
        if not a.after_only:
            run(a.before, a.out, 'before', nv, b)
        run(a.after, a.out, 'after', nv, b)
    rep = []
    for cap, parts in draws.items():
        last = max((d for ds in parts.values() for d in ds), key=lambda d: d.meta['event'])
        G = last.rt_after[:, :, :3]  # (RGBA as the shader wrote it: oracle.py compares it unswapped)
        Glin = pwl_decode(G / 255.0) if last.gamma else G / 255.0
        im = lambda n: np.asarray(Image.open(os.path.join(a.out, n + '.png')).convert('RGB'))
        rep.append('== %s (cue %d f %d)' % (cap, CAPTURES[cap][0], CAPTURES[cap][1]))
        for part, ds in parts.items():
            gm = np.zeros(G.shape[:2], bool)
            for d in ds:
                ch = d.changed()
                if ch is not None:
                    gm |= ch
            row = '  %-9s game %6d px' % (part, gm.sum())
            gmean = Glin[gm].mean(0) if gm.any() else np.zeros(3)
            for tag in ('before', 'after'):
                key = part.replace(' ', '_')
                vm = np.any(im('%s_%s_M_%s' % (tag, cap, key)) != im('%s_%s_N_%s' % (tag, cap, key)), -1)
                V = im('%s_%s' % (tag, cap)) / 255.0
                if not vm.any() or not gm.any():
                    row += ' | %s: no pixels' % tag
                    continue
                gy, gx = np.nonzero(gm)
                vy, vx = np.nonzero(vm)
                iou = (gm & vm).sum() / (gm | vm).sum()
                ratio = V[vm].mean(0) / np.maximum(gmean, 1e-6)
                row += ' | %s: off (%+.0f, %+.0f) px IoU %.2f, lum x%.2f (rgb %s)' % (tag, vx.mean() - gx.mean(), vy.mean() - gy.mean(), iou,
                                                                                  V[vm].mean() / max(gmean.mean(), 1e-6), np.round(ratio, 2).tolist())
            rep.append(row)
        # the sheet: game (scene target) | before | after (raw viewer shots PWL-encoded like the game's target), half size
        enc = lambda x: (rdraw.pwl_gamma(x) * 255).astype(np.uint8)
        tiles = [G.astype(np.uint8), enc(im('before_%s' % cap) / 255.0), enc(im('after_%s' % cap) / 255.0)]
        h, w = G.shape[0] // 2, G.shape[1] // 2
        sheet = Image.new('RGB', (w * 3, h + 28), (16, 16, 16))
        dr = ImageDraw.Draw(sheet)
        for i, (t, lab) in enumerate(zip(tiles, ['game (run 6818 %s, scene target)' % cap, 'viewer before (main)', 'viewer after (this branch)'])):
            sheet.paste(Image.fromarray(t).resize((w, h)), (i * w, 28))
            dr.text((i * w + 8, 8), lab, fill=(230, 230, 230))
        sheet.save(os.path.join(a.out, 'sheet_%s.png' % cap))
    txt = '\n'.join(rep)
    print(txt)
    open(os.path.join(a.out, 'numbers.txt'), 'w').write(txt + '\n')


if __name__ == '__main__':
    main()

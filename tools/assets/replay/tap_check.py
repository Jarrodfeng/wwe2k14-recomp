r"""Frame replay S5 (Session 98, 0 game runs): the viewer's runtime stand-ins on the GPU against the game's, per part, at one capture.
Viewer: {"cmd":"look","debug":11} (r = its shadow lit, g = its rim map term, b = glow luminance) and 12 (its ambient cube, min'd with the
limit), averaged over the part's own pixels (black-out masks as in s5_check.py). Game: the scene-shader port (shade_scene.py) on the
captured draw: lit, the rim map term (lm_term ^ c161.x), the glow luminance and the cube (sampled x c197, capped), averaged over the
draw's pixels. One viewer launch per exe.

  py -I tools\assets\replay\tap_check.py --capture rp_ringin --exe A[,B] --tags after[,before] --bank BANK --out DIR
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle  # noqa: E402
import rdraw  # noqa: E402
import s5_check  # noqa: E402
import shade_scene as SS  # noqa: E402
import shader_map as SM  # noqa: E402

SHADERS = r'C:\Users\Jarrod\wwe2k14-recomp\out\s98\replay\shaders'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture', required=True)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--tags', required=True)
    ap.add_argument('--bank', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--reuse', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    cue, f, parts = s5_check.CAPTURES[a.capture]
    b = json.load(open(a.bank))
    model = next(i for i, m in enumerate(b['models']) if m.get('has_wrestler'))
    dirs = {p: [s5_check.path_of(x) for x in xs] for p, xs in parts.items()}
    d0 = rdraw.Draw(next(iter(dirs.values()))[0])
    W, H = d0.vw, d0.vh
    shot = lambda n: json.dumps({'cmd': 'shot', 'path': os.path.join(a.out, n + '.png').replace('\\', '/'), 'w': W, 'h': H})
    for exe, tag in zip(a.exe.split(','), a.tags.split(',')):
        if a.reuse:
            break
        media = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons').replace('\\', '/')
        lines = ['{"cmd":"load","kind":"arena","id":5}', json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140}),
                 json.dumps({'cmd': 'stage', 'media': media, 'wrestler': 161}),
                 json.dumps({'cmd': 'entrance', 'wrestler': 161, 'attire': 1, 'cue': cue, 'f': f, 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}),
                 json.dumps({'cmd': 'camera', 'viewproj': [float(x) for x in d0.vs_c[56:60].reshape(-1)], 'eye': [float(x) for x in d0.vs_c[72][:3]]}),
                 json.dumps({'cmd': 'look', 'gamma': False, 'dof': False, 'bloom': False, 'glare': False, 'fxaa': False, 'film': False}),
                 json.dumps({'cmd': 'bank_dump', 'path': os.path.join(a.out, 'bank_%s.json' % tag).replace('\\', '/'), 'w': W, 'h': H})]
        for t in (11, 12):
            lines += [json.dumps({'cmd': 'look', 'debug': t}), shot('%s_t%d' % (tag, t))]
        for part, ds in dirs.items():
            nv = {rdraw.Draw(d).meta['postvs']['bytes'] // rdraw.Draw(d).meta['postvs']['stride'] for d in ds}
            ms = [x['mesh'] for x in b['draws'] if x['model'] == model and x.get('verts') in nv]
            k = part.replace(' ', '_')
            lines += [json.dumps({'cmd': 'look', 'debug': 13}),
                      json.dumps({'cmd': 'replay', 'clear': True, 'draws': [{'model': model, 'mesh': m, 'cb': {'mat_dif': [0, 0, 0, 1], 'mat_amb': [0, 0, 0, 1]}} for m in ms]}),
                      shot('%s_M_%s' % (tag, k)), json.dumps({'cmd': 'replay', 'clear': True}), shot('%s_N_%s' % (tag, k))]
        script = os.path.join(a.out, 'script_%s.jsonl' % tag)
        open(script, 'w').write('\n'.join(lines) + '\n')
        p = subprocess.run([exe, '--game', s5_check.GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
        open(os.path.join(a.out, 'viewer_%s.log' % tag), 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    im = lambda n: np.asarray(Image.open(os.path.join(a.out, n + '.png')).convert('RGB'), dtype=np.float64)
    rep = ['%s (cue %d f %d): mean over each side\'s own pixels; viewer GPU taps vs the game (scene-shader port on the capture)' % (a.capture, cue, f)]
    for part, ds in dirs.items():
        g = {'lit': [], 'rim': [], 'glow': [], 'cube': []}
        for dp in ds:
            meta = json.load(open(os.path.join(dp, 'meta.json')))
            _, kind, regs = SM.resolve(meta['ps_md5'], SHADERS)
            if kind not in oracle.KINDS:
                continue
            d = rdraw.Draw(dp, sorted(regs, key=lambda r: regs[r]))
            sel = d.changed()[d.px[1], d.px[0]]
            if sel.sum() == 0:
                continue
            _, _, st = oracle.KINDS[kind](d)
            g['lit'].append(st['lit'][sel])
            g['rim'].append(SS.pow_abs(st['lm_term'], d.ps_c[161][0])[sel])
            if 'glow_lum' in st:
                g['glow'].append(st['glow_lum'][sel])
            g['cube'].append(st['cube'][sel])
        if not g['lit']:
            continue
        gm = {k: (np.concatenate(v).mean(0) if v else None) for k, v in g.items()}
        row = '  %-9s game: lit %.3f rim map %.4f glow %s cube %s' % (part, gm['lit'], gm['rim'], '-' if gm['glow'] is None else '%.3f' % gm['glow'],
                                                                     np.round(gm['cube'], 3).tolist())
        for tag in a.tags.split(','):
            k = part.replace(' ', '_')
            m = np.any(im('%s_M_%s' % (tag, k)) != im('%s_N_%s' % (tag, k)), -1)
            if not m.any():
                row += ' | %s: no pixels' % tag
                continue
            t11, t12 = im('%s_t11' % tag)[m] / 255, im('%s_t12' % tag)[m] / 255
            row += ' | %s: lit %.3f rim map %.4f glow %.3f cube %s (x%.3f)' % (tag, t11[:, 0].mean(), t11[:, 1].mean(), t11[:, 2].mean(),
                                                                            np.round(t12.mean(0), 3).tolist(), t12.mean() / max(gm['cube'].mean(), 1e-6))
        rep.append(row)
    txt = '\n'.join(rep)
    print(txt)
    open(os.path.join(a.out, 'taps_%s.txt' % a.capture), 'w').write(txt + '\n')


if __name__ == '__main__':
    main()

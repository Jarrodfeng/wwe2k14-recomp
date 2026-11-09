r"""Frame replay (Session 98 S5, 0 game runs): does the viewer's entrant overlay the game's, pixel for pixel? For each dumped colour draw
of a capture (rdc_draw.py), the game's coverage = the pixels the draw changed; the viewer's = the pixels its mesh of the same vertex
count covers at the game's camera (c56..c59 / c72), found by blacking that mesh out ({"cmd":"replay"} mat_dif 0) and differencing.
Reports the screen offset of the centroids (px), the vertical / horizontal extent difference and the overlap (IoU).

  py -I tools\assets\replay\overlay_check.py --draws DIR[,DIR] --cue 15 --f 767 [--root-y canvas] [--extra JSON-lines] --out DIR
        [--wrestler 161 --attire 1] [--exe ...]
One viewer launch (hidden, 120 s). Everything stays under out\ (rule 1).
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
import rdraw  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
GAME = r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--draws', required=True)
    ap.add_argument('--cue', type=int, required=True)
    ap.add_argument('--f', type=float, required=True)
    ap.add_argument('--root-y', default='')
    ap.add_argument('--extra', default='')
    ap.add_argument('--wrestler', type=int, default=161)
    ap.add_argument('--attire', type=int, default=1)
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--bank', help='an earlier bank_dump of the same entrant (mesh indices): one launch instead of two')
    ap.add_argument('--reuse', action='store_true', help='use the shots already in --out')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    dirs = a.draws.split(',')
    draws = [rdraw.Draw(d) for d in dirs]
    d0 = draws[0]
    W, H = d0.vw, d0.vh
    vp = [float(x) for x in d0.vs_c[56:60].reshape(-1)]
    eye = [float(x) for x in d0.vs_c[72][:3]]
    nverts = [dr.meta['postvs']['bytes'] // dr.meta['postvs']['stride'] for dr in draws]
    shot = lambda n: json.dumps({'cmd': 'shot', 'path': os.path.join(a.out, n + '.png').replace('\\', '/'), 'w': W, 'h': H})
    if not a.reuse:
        media = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons').replace('\\', '/')
        e = {'cmd': 'entrance', 'wrestler': a.wrestler, 'attire': a.attire, 'cue': a.cue, 'f': a.f, 'intro_hold': 0, 'camera': 'free',
             'stage': False, 'play': False}
        if a.root_y:
            e['root_y'] = a.root_y
        bank = os.path.join(a.out, 'bank.json').replace('\\', '/')
        lines = ['{"cmd":"load","kind":"arena","id":5}', json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140}),
                 json.dumps({'cmd': 'stage', 'media': media, 'wrestler': a.wrestler}), json.dumps(e)]
        lines += [x for x in a.extra.split('\\n') if x.strip()]
        lines += [json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': eye}), json.dumps({'cmd': 'bank_dump', 'path': bank, 'w': W, 'h': H}),
                  json.dumps({'cmd': 'look', 'dof': False, 'bloom': False, 'glare': False, 'fxaa': False}), shot('A'),
                  json.dumps({'cmd': 'entrance_info'})]
        script = os.path.join(a.out, 'script.jsonl')
        # pass 1: the bank (mesh indices by vertex count), pass 2: one black-out shot per draw
        log = ''
        if not a.bank:
            open(script, 'w').write('\n'.join(lines) + '\n')
            p = subprocess.run([a.exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
            log = p.stdout + '\n--stderr--\n' + p.stderr
        b = json.load(open(a.bank or bank))
        model = next(i for i, m in enumerate(b['models']) if m.get('has_wrestler'))
        more = []
        for i, nv in enumerate(nverts):
            ms = [x['mesh'] for x in b['draws'] if x['model'] == model and x.get('verts') == nv]
            more += [json.dumps({'cmd': 'replay', 'clear': True, 'draws': [{'model': model, 'mesh': m, 'cb': {'mat_dif': [0, 0, 0, 1], 'mat_amb': [0, 0, 0, 1]}} for m in ms]}),
                     json.dumps({'cmd': 'look', 'debug': 13}), shot('M%d' % i), json.dumps({'cmd': 'replay', 'clear': True}), shot('N%d' % i), json.dumps({'cmd': 'look', 'debug': 0})]
        open(script, 'w').write('\n'.join(lines[:-1] + more + [json.dumps({'cmd': 'entrance_info'})]) + '\n')
        p = subprocess.run([a.exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
        log += p.stdout + '\n--stderr--\n' + p.stderr
        open(os.path.join(a.out, 'viewer.log'), 'w').write(log)
    rep = []
    for i, dr in enumerate(draws):
        g = dr.changed()
        im = lambda n: np.asarray(Image.open(os.path.join(a.out, n + '.png')).convert('RGB'), dtype=np.int16)
        v = np.any(im('M%d' % i) != im('N%d' % i), -1)
        if g is None or not g.any() or not v.any():
            rep.append('%s: no pixels (game %d, viewer %d)' % (os.path.basename(dirs[i]), 0 if g is None else g.sum(), v.sum()))
            continue
        gy, gx = np.nonzero(g)
        vy, vx = np.nonzero(v)
        iou = (g & v).sum() / max((g | v).sum(), 1)
        rep.append('%s (%d verts): game %d px, viewer %d px, centroid viewer - game (x, y down) %+.1f %+.1f px, top %+d bottom %+d, IoU %.3f' % (
            os.path.basename(dirs[i]), nverts[i], g.sum(), v.sum(), vx.mean() - gx.mean(), vy.mean() - gy.mean(), vy.min() - gy.min(),
            vy.max() - gy.max(), iou))
        ov = np.zeros((H, W, 3), np.uint8)
        ov[g, 0] = 255
        ov[v, 1] = 255
        Image.fromarray(ov).save(os.path.join(a.out, 'overlay%d.png' % i))
    # the arena itself: shift of the viewer's picture against the game's (edge maps, phase correlation, the entrant masked out)
    try:
        def edges(img):
            g = img.astype(np.float64).mean(-1)
            e = np.hypot(np.diff(g, axis=1)[:-1], np.diff(g, axis=0)[:, :-1])
            return np.log1p(e)
        ga = edges(d0.rt_before[:, :, [2, 1, 0]]) if d0.rt_before is not None else None
        va = edges(np.asarray(Image.open(os.path.join(a.out, 'A.png')).convert('RGB')))
        if ga is not None:
            F = np.fft.fft2(ga - ga.mean()) * np.conj(np.fft.fft2(va - va.mean()))
            r = np.fft.ifft2(F / np.maximum(np.abs(F), 1e-9)).real
            y, x = np.unravel_index(np.argmax(r), r.shape)
            y = y - r.shape[0] if y > r.shape[0] // 2 else y
            x = x - r.shape[1] if x > r.shape[1] // 2 else x
            rep.append('arena: game picture = viewer picture shifted by (x %+d, y %+d) px (peak %.3f)' % (x, y, r.max()))
    except Exception as ex:  # (a report line, not a failure)
        rep.append('arena shift: %s' % ex)
    txt = '\n'.join(rep)
    print(txt)
    open(os.path.join(a.out, 'report.txt'), 'w').write(txt + '\n')


if __name__ == '__main__':
    main()

r"""Frame replay S3 (Session 98, 0 game runs): the Arena Viewer drawn with the GAME's per-draw constants for one wrestler part, against
the viewer as it is, at the captured frame's camera; plus the CPU oracle (oracle.py) on the game's own pixels.

  py -I tools\assets\replay\replay.py --capture rp_ringin --part eyes|skin|head|shirt|trunks|boots [--inputs game|viewer|mix:light,sky,...]
         [--exe out\tools\wwe2k14_assets.exe] [--out DIR]

Needs the part's draw dumped from the capture (rdc_draw.ps1 into out\s98\replay\s3s4\<capture>\<dir>\ev<event>; PARTS below gives the
index counts). One viewer launch (hidden, hard 120 s) renders, at the game camera (c56..c59 / c72 of the draw):
  A    the viewer as is                        (shot A.png, taps A_t11 / A_t12 / A_t13: lit + rim map + glow, cube, base)
  B    the part's meshes with the game's bank   (shot B.png + taps), {"cmd":"replay"} with the regmap.json fields
The viewer's own pixels of the part = where A and B differ (only that mesh's constants changed). Report: mean colour / taps over
that mask (GPU, the viewer's own geometry and runtime maps), and the oracle's A / B / Bi / S / G on the game's pixels (CPU).
Input groups for --inputs mix: light (ldir, lcol), sky (sky, ground), mat (mat_amb, mat_dif, limit), hl (hl_dir, hl_col), lobes
(wspec), rim (refc, wmisc). Everything written stays under out\ (rule 1).
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
import shader_map as SM  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
OUT = os.path.join(MAIN, 'out', 's98', 'replay', 's3s4')
GAME = os.path.join(MAIN, 'game_data', 'extracted')
# capture -> the viewer placement of the same entrance moment (design.md A: the waits of the capture script) and the arena
# (S5: rp_ringin = 767, not 766: its lights equal the viewer rig at 767 to 1e-3 and its bone palette fits 767 / 768; s5_check.py)
CAPTURES = {'rp_stage': (5, 576), 'rp_ramp': (10, 428), 'rp_ringin': (15, 767), 'rp_ring': (20, 122)}
# part -> (scene shader, index counts of Orton's colour draws: probe runs 6816 / 6818, out\s98\run1\_attire.py) [Orton-specific table]
PARTS = {'eyes': ('wrestler_reflect_entrance', (1516,)), 'skin': ('wrestler_skin_entrance', (2087, 2068)),
         'head': ('wrestler_head_entrance', (1887, 1841)), 'shirt': ('wrestler_body_entrance', (2233,)),
         'trunks': ('wrestler_body_entrance', (1995,)), 'boots': ('wrestler_body_entrance', (1919, 982))}
GROUPS = {'light': ('ldir', 'lcol'), 'sky': ('sky', 'ground'), 'mat': ('mat_amb', 'mat_dif', 'limit'), 'hl': ('hl_dir', 'hl_col'),
          'lobes': ('wspec',), 'rim': ('refc', 'wmisc')}


def find_draw(capture, part, shaders):
    shader, counts = PARTS[part]
    best = None
    root = os.path.join(OUT, capture)
    for dp, dn, fn in os.walk(root):
        if 'meta.json' not in fn:
            continue
        m = json.load(open(os.path.join(dp, 'meta.json')))
        if m.get('indices') not in counts:
            continue
        g, name, _ = SM.resolve(m['ps_md5'], shaders)
        if name != shader:
            continue
        d = rdraw.Draw(dp)
        ch = d.changed()
        n = int(ch.sum()) if ch is not None else 0
        if best is None or n > best[1]:
            best = (dp, n)
    return best[0] if best and best[1] > 0 else None


def run_viewer(exe, lines, out):
    script = os.path.join(out, 'script.jsonl')
    open(script, 'w').write('\n'.join(lines) + '\n')
    p = subprocess.run([exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    open(os.path.join(out, 'viewer.log'), 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad:
        raise SystemExit('viewer: %s' % bad[:3])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture', required=True, choices=list(CAPTURES))
    ap.add_argument('--part', required=True, choices=list(PARTS))
    ap.add_argument('--inputs', default='game')
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'))
    ap.add_argument('--shaders', default=os.path.join(MAIN, 'out', 's98', 'replay', 'shaders'))
    ap.add_argument('--out')
    ap.add_argument('--bank', help='a bank_dump of the same placement (skips the dump step)')
    a = ap.parse_args()
    out = a.out or os.path.join(OUT, a.capture, 'replay_' + a.part)
    os.makedirs(out, exist_ok=True)
    dp = find_draw(a.capture, a.part, a.shaders)
    if not dp:
        raise SystemExit('no dumped draw for %s / %s under %s (rdc_draw.ps1 -ICounts %s)' % (a.capture, a.part, OUT, PARTS[a.part][1]))
    meta = json.load(open(os.path.join(dp, 'meta.json')))
    g, kind, regmap = SM.resolve(meta['ps_md5'], a.shaders)
    d = rdraw.Draw(dp, sorted(regmap, key=lambda r: regmap[r]))
    vp = [float(x) for x in d.vs_c[56:60].reshape(-1)]
    eye = [float(x) for x in d.vs_c[72][:3]]
    cue, f = CAPTURES[a.capture]
    media = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons').replace('\\', '/')
    head = ['{"cmd":"load","kind":"arena","id":5}', json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140}),
            json.dumps({'cmd': 'stage', 'media': media, 'wrestler': 161}),
            json.dumps({'cmd': 'entrance', 'wrestler': 161, 'attire': 1, 'cue': cue, 'f': f, 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}),
            json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': eye})]
    bank_path = a.bank or os.path.join(out, 'bank.json')
    if not a.bank:
        run_viewer(a.exe, head + [json.dumps({'cmd': 'bank_dump', 'path': bank_path.replace('\\', '/'), 'w': 1280, 'h': 720})], out)
    bank = json.load(open(bank_path))
    nverts = meta['postvs']['bytes'] // meta['postvs']['stride']
    mesh = oracle.find_viewer_mesh(bank, nverts, 'yCh_')
    same = [x for x in bank['draws'] if x.get('verts') == nverts and 'yCh_' in x.get('shader', '') and x.get('model') == (mesh or {}).get('model')]
    if not mesh:
        raise SystemExit('no viewer mesh with %d vertices' % nverts)
    gcb = oracle.game_cb(d, mesh['cb'])
    fields = list(gcb) if a.inputs == 'game' else ([] if a.inputs == 'viewer' else
                                                     [x for gname in a.inputs[4:].split(',') for x in GROUPS[gname]])
    keep = ('ldir', 'lcol', 'sky', 'ground', 'limit', 'mat_amb', 'mat_dif', 'hl_col', 'hl_dir', 'refc', 'wspec', 'wmisc')
    cbo = {k: gcb[k] for k in fields if k in keep}
    W, H = 2560, 1440
    shot = lambda n: json.dumps({'cmd': 'shot', 'path': os.path.join(out, n + '.png').replace('\\', '/'), 'w': W, 'h': H})
    raw = json.dumps({'cmd': 'look', 'gamma': False, 'dof': False, 'bloom': False, 'glare': False, 'fxaa': False, 'film': False})
    lines = head + [shot('A')]
    lines += [json.dumps({'cmd': 'replay', 'clear': True, 'draws': [{'model': x['model'], 'mesh': x['mesh'], 'cb': cbo} for x in same]}), shot('B')]
    lines += [raw] + [json.dumps({'cmd': 'look', 'debug': t}) + '\n' + shot('B_t%d' % t) for t in (11, 12, 13)]
    lines += [json.dumps({'cmd': 'replay', 'clear': True})] + [json.dumps({'cmd': 'look', 'debug': t}) + '\n' + shot('A_t%d' % t) for t in (11, 12, 13)]
    run_viewer(a.exe, lines, out)
    im = {n: np.asarray(Image.open(os.path.join(out, n + '.png')).convert('RGB'), dtype=np.float64) for n in
          ['A', 'B'] + ['%s_t%d' % (x, t) for x in 'AB' for t in (11, 12, 13)]}
    mask = np.any(np.abs(im['A_t13'] - im['B_t13']) > 0, -1) | np.any(np.abs(im['A'] - im['B']) > 2, -1)
    rep = ['replay %s / %s: game draw %s (%s); viewer model %d mesh %d (%s, %d vertices); inputs %s (%d fields)' % (
        a.capture, a.part, os.path.basename(dp), kind, mesh['model'], mesh['mesh'], mesh.get('part'), nverts, a.inputs, len(cbo)),
        'GPU (the viewer\'s own pose / maps), %d px where A != B:' % mask.sum()]
    if mask.sum():
        for n in ('A', 'B'):
            rep.append('  %s shot mean %s | lit %.3f rim map %.3f glow %.3f | cube %s | base %s' % (
                n, im[n][mask].mean(0).round(1), im[n + '_t11'][mask][:, 0].mean() / 255, im[n + '_t11'][mask][:, 1].mean() / 255,
                im[n + '_t11'][mask][:, 2].mean() / 255, (im[n + '_t12'][mask].mean(0)).round(1), (im[n + '_t13'][mask].mean(0)).round(1)))
    open(os.path.join(out, 'replay.txt'), 'w').write('\n'.join(rep) + '\n')
    print('\n'.join(rep))
    sys.argv = ['oracle', '--draw', dp, '--bank', bank_path, '--out', os.path.join(out, 'oracle.txt')]
    try:
        oracle.main()
    except SystemExit as e:
        print('oracle:', e)


if __name__ == '__main__':
    main()

r"""Frame replay (Session 98 S5, 0 game runs): where the viewer puts an entrant against the game, bone by bone.

The game's skinning palette (VS c76.., 3 rows per bone: file-space rotation | translation) of the entrant's draws in a draw probe is
matched to the viewer's skinning matrices (bank_dump models[].bones, row-vector 4x4 in viewer space: +Y up, vertices Y-mirrored) by
rotation; the translation difference of the matched bones is the placement error (viewer space x, y up, z), the rotation error says
whether the viewer's cue-local frame is the game's (timing). The light block (c0..c3 rgb of the entrant's vertex bank) is compared too.

  py -I tools\assets\replay\place_check.py --frames 9947,9948 --cue 15 --f 764-768 [--root-y canvas] [--wrestler 161 --attire 1]
        [--counts 2087,2068,1887,1841,2233,1995,1919,982,1516] [--exe out\tools\wwe2k14_assets.exe] [--out DIR] [--banks DIR]
Writes the viewer's bank dumps and a report under --out (out\ only, rule 1).
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import probe_rows  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
GAME = os.path.join(MAIN, 'game_data', 'extracted')
S = np.diag([1.0, -1.0, 1.0])
# Orton's attire-1 colour draws (index counts; probe runs 6816 / 6818: out\s98\replay\index_summary.md) [entrant-specific default]
ORTON = (2087, 2068, 1887, 1841, 2233, 1995, 1919, 982, 1516)
# the wrestler colour-pass vertex shaders (skin, head skin, skin 2, gear, reflect: index_summary.md); the shadow / rim passes differ
COLOUR_VS = {'3F91EC05EDABDF7B', '0E92FB108A675C39', 'F006439D879C5FDB', 'F709D14F30FCC95C', 'B15688766DA713DC'}


def game_bones(vs_c):
    out = []
    for b in range(60):
        r = vs_c[76 + 3 * b: 79 + 3 * b]
        if len(r) < 3 or not np.any(r):
            break
        R, t = r[:, :3], r[:, 3]
        if abs(np.linalg.det(R) - 1) > 0.05:
            break
        out.append((S @ R @ S, S @ t))
    return out


def compare(game, bank, model):
    m = bank['models'][model]
    off = np.array(m['offset'])
    VB = [np.array(b).reshape(4, 4) for b in m['bones']]
    VR = np.array([v[:3, :3].T for v in VB])
    VT = np.array([v[3, :3] for v in VB]) + off
    shift = np.zeros(3)
    for it in range(3):  # match by rotation (+ position once a shift is known), then the shift = the median difference
        rot, d = [], []
        for R, t in game:
            cost = np.abs(VR - R).sum((1, 2)) + (np.linalg.norm(VT + shift - t, axis=1) / 20.0 if it else 0)
            j = int(np.argmin(cost))
            rot.append(np.abs(VR[j] - R).sum())
            d.append(t - VT[j])
        shift = np.median(np.array(d), 0)
    return float(np.median(rot)), shift, np.array(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--frames', required=True)
    ap.add_argument('--cue', type=int, required=True)
    ap.add_argument('--f', required=True, help='cue-local frames to try, a-b')
    ap.add_argument('--root-y', default='')
    ap.add_argument('--wrestler', type=int, default=161)
    ap.add_argument('--attire', type=int, default=1)
    ap.add_argument('--counts', default=','.join(str(x) for x in ORTON))
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--banks', help='reuse the bank dumps in this folder (no viewer launch)')
    ap.add_argument('--probe', default=probe_rows.PROBE)
    ap.add_argument('--extra', default='', help='extra JSON lines (\\n separated) sent after the entrance command')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    frames = [int(x) for x in a.frames.split(',')]
    counts = set(int(x) for x in a.counts.split(','))
    rows = probe_rows.rows(a.probe, swaps=set(frames), index_counts=counts, vs=COLOUR_VS)
    if not rows:
        raise SystemExit('no probe rows for %s' % frames)
    f0, f1 = (int(x) for x in a.f.split('-'))
    fl = list(range(f0, f1 + 1))
    vs0 = rows[0][4]
    vp = [float(x) for x in vs0[56:60].reshape(-1)]
    eye = [float(x) for x in vs0[72][:3]]
    banks = a.banks or a.out
    if not a.banks:
        media = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons').replace('\\', '/')
        lines = ['{"cmd":"load","kind":"arena","id":5}', json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140}),
                 json.dumps({'cmd': 'stage', 'media': media, 'wrestler': a.wrestler})]
        for f in fl:
            e = {'cmd': 'entrance', 'wrestler': a.wrestler, 'attire': a.attire, 'cue': a.cue, 'f': f, 'intro_hold': 0, 'camera': 'free',
                 'stage': False, 'play': False}
            if a.root_y:
                e['root_y'] = a.root_y
            lines += [json.dumps(e)] + [x for x in a.extra.split('\\n') if x.strip()]
            lines += [json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': eye}),
                      json.dumps({'cmd': 'bank_dump', 'path': os.path.join(banks, 'bank_f%d.json' % f).replace('\\', '/'), 'w': 640, 'h': 360}),
                      json.dumps({'cmd': 'entrance_info'})]
        script = os.path.join(a.out, 'script.jsonl')
        open(script, 'w').write('\n'.join(lines) + '\n')
        p = subprocess.run([a.exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
        open(os.path.join(a.out, 'viewer.log'), 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    rep = []
    for sw in frames:
        rr = [r for r in rows if r[0] == sw]
        if not rr:
            continue
        game = [b for r in rr for b in game_bones(r[4])]
        sk = [r for r in rr if r[2] == '3F91EC05EDABDF7B'] or rr  # (the skin draw's vertex bank c0..c3 = the entrant's light colours)
        glc = np.array([sk[0][4][k][:3] for k in range(4)])
        for f in fl:
            bp = os.path.join(banks, 'bank_f%d.json' % f)
            if not os.path.exists(bp):
                continue
            bank = json.load(open(bp))
            model = next(i for i, m in enumerate(bank['models']) if m.get('has_wrestler'))
            rot, med, d = compare(game, bank, model)
            ent = [x for x in bank['draws'] if x['model'] == model and 'yCh_skin' in x.get('shader', '')]
            vlc = np.array(ent[0]['cb']['lcol'])[:, :3] if ent else np.zeros((4, 3))
            lr = (vlc[1:3].sum() / max(glc[1:3].sum(), 1e-6))
            rep.append('frame %d vs cue %d f %d: %d bones, rot err median %.3f, game - viewer (x, y up, z) %s (spread y %.1f) | lights 1+2 viewer/game %.3f' % (
                sw, a.cue, f, len(d), rot, np.round(med, 1), np.percentile(d[:, 1], 90) - np.percentile(d[:, 1], 10), lr))
    txt = '\n'.join(rep)
    print(txt)
    open(os.path.join(a.out, 'report.txt'), 'w').write(txt + '\n')


if __name__ == '__main__':
    main()

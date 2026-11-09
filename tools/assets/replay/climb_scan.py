r"""Session 98 (replay S5, 0 game runs): which entrances climb, and where the feet are while they do. For each wrestler, steps the
viewer's entrance timeline (one hidden launch for all) and reads entrance_info: the root-motion height (the `Bip` sample, motion space,
file -Y up: a climb = the root rising > --rise above its standing height), the model origin and the lowest posed foot (world, viewer Y
up). Prints the climb windows per wrestler and the feet heights in them, against the canvas / turnbuckle heights.

  py -I tools\assets\replay\climb_scan.py --wrestlers 161,139,100,109 [--step 10] [--rise 80] [--exe ...] --out DIR
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
GAME = r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wrestlers', required=True)
    ap.add_argument('--step', type=int, default=10)
    ap.add_argument('--rise', type=float, default=80)
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--reuse', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ws = [int(x) for x in a.wrestlers.split(',')]
    log = os.path.join(a.out, 'scan.log')
    if not a.reuse:
        lines = ['{"cmd":"load","kind":"arena","id":5}']
        for w in ws:
            lines.append(json.dumps({'cmd': 'entrance', 'wrestler': w, 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}))
            for f in range(0, a.frames, a.step):
                lines.append(json.dumps({'cmd': 'entrance_time', 'frame': f}))
        script = os.path.join(a.out, 'scan.jsonl')
        open(script, 'w').write('\n'.join(lines) + '\n')
        p = subprocess.run([a.exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
        open(log, 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    cur = None
    rows = {}
    for line in open(log):
        line = line.strip()
        if not line.startswith('{'):
            continue
        try:
            j = json.loads(line)
        except ValueError:
            continue
        if j.get('ok') and 'wrestler' in j and j['wrestler'] != cur:
            cur = j['wrestler']
            rows[cur] = []
        if 'current' in j and cur is not None:
            c = j['current']
            pose = c.get('pose', {})
            rows[cur].append((c['t'], c['cue'], c['local'], c['root'][1], pose.get('origin_y'), pose.get('feet_y'), c['root'][0], c['root'][2]))
        elif not j.get('ok', True):
            print('viewer:', line[:200])
    rep = []
    for w, rr in rows.items():
        rr = [r for r in rr if r[4] is not None and r[1] != 0]
        if not rr:
            rep.append('%d: no entrance frames' % w)
            continue
        stand = sorted(r[3] for r in rr)[len(rr) // 2]  # median root sample (file -Y up)
        # a climb = inside the ring's footprint with the lowest foot more than --rise / 2 above the canvas (on the ropes / turnbuckle),
        # or the root more than --rise above its median
        climb = [r for r in rr if (max(abs(r[6]), abs(r[7])) < 340 and r[4] + r[5] > 120.2 + a.rise / 2) or stand - r[3] > a.rise]
        rep.append('%d: %d frames, median root y %.1f, %d frames > %.0f above it' % (w, len(rr), stand, len(climb), a.rise))
        prev = None
        for r in climb:
            if prev is None or r[0] - prev[0] > 3 * a.step:
                rep.append('   climb from t %.0f (cue %d f %.0f): root y %.1f at (x %.0f, z %.0f); lowest foot (world) %.1f' % (r[0], r[1], r[2], r[3], r[6], r[7], r[4] + r[5]))
            prev = r
        if climb:
            hi = min(climb, key=lambda r: r[3])
            rep.append('   highest: t %.0f (cue %d f %.0f) root y %.1f at (x %.0f, z %.0f), lowest foot (world) %.1f' % (hi[0], hi[1], hi[2], hi[3], hi[6], hi[7], hi[4] + hi[5]))
    txt = '\n'.join(rep)
    print(txt)
    open(os.path.join(a.out, 'climbs.txt'), 'w').write(txt + '\n')


if __name__ == '__main__':
    main()

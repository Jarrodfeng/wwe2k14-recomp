r"""Session 98 (replay S5, 0 game runs): shots of entrance climbs (feet on the turnbuckle / ropes) from two sides, one hidden viewer
launch, and a labelled contact sheet. Moments come from climb_scan.py (wrestler, cue, local frame, root x / z).

  py -I tools\assets\replay\climb_shots.py --moments 161:20:786:-261:-271,100:15:1474:-253:297 [--root-y canvas|ground] [--exe ...] --out DIR --tag after
"""
import argparse
import json
import math
import os
import subprocess

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
GAME = r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--moments', required=True)
    ap.add_argument('--root-y', default='')
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--tag', default='after')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ms = [tuple(float(v) for v in m.split(':')) for m in a.moments.split(',')]
    lines = ['{"cmd":"load","kind":"arena","id":5}', json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140})]
    shots = []
    W, H = 960, 720
    for w, cue, f, x, z in ms:
        e = {'cmd': 'entrance', 'wrestler': int(w), 'cue': int(cue), 'f': f, 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}
        if a.root_y:
            e['root_y'] = a.root_y
        lines.append(json.dumps(e))
        # two views: from the ring centre towards the corner, and from the side (90 degrees round), aimed at rope height
        yaw0 = math.degrees(math.atan2(x, z))
        for k, yaw in enumerate((yaw0 + 180.0, yaw0 + 90.0)):
            n = '%s_%d_c%d_f%d_v%d' % (a.tag, int(w), int(cue), int(f), k)
            lines += [json.dumps({'cmd': 'camera', 'viewproj': None}),
                      json.dumps({'cmd': 'camera', 'yaw': yaw, 'pitch': 4, 'dist': 330, 'fov': 40, 'target': [x, 190, z]}),
                      json.dumps({'cmd': 'shot', 'path': os.path.join(a.out, n + '.png').replace('\\', '/'), 'w': W, 'h': H})]
            shots.append((n, '%d cue %d f %d %s' % (w, cue, f, 'front' if k == 0 else 'side')))
        lines.append(json.dumps({'cmd': 'entrance_info'}))
    script = os.path.join(a.out, 'script_%s.jsonl' % a.tag)
    open(script, 'w').write('\n'.join(lines) + '\n')
    p = subprocess.run([a.exe, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    open(os.path.join(a.out, 'viewer_%s.log' % a.tag), 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad:
        print('viewer errors:', bad[:3])
    tw, th = W // 2, H // 2
    cols = 4
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new('RGB', (tw * cols, (th + 20) * rows), (16, 16, 16))
    dr = ImageDraw.Draw(sheet)
    for i, (n, lab) in enumerate(shots):
        p = os.path.join(a.out, n + '.png')
        if not os.path.exists(p):
            continue
        x0, y0 = (i % cols) * tw, (i // cols) * (th + 20)
        sheet.paste(Image.open(p).convert('RGB').resize((tw, th)), (x0, y0 + 20))
        dr.text((x0 + 6, y0 + 4), '%s (%s)' % (lab, a.tag), fill=(230, 230, 230))
    sheet.save(os.path.join(a.out, 'climbs_%s.png' % a.tag))
    print('sheet', os.path.join(a.out, 'climbs_%s.png' % a.tag))


if __name__ == '__main__':
    main()

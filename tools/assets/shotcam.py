"""Session 97: look-at cameras for the asset viewer, in the game's file space (-Y up).  Writes a viewer script that loads an
arena, shoots one picture per camera, and runs the viewer (hidden, exits).  Local files only.

  py -I tools/assets/shotcam.py ARENA OUT_PREFIX EYE_X,EYE_Y,EYE_Z:TGT_X,TGT_Y,TGT_Z [more cameras ...] [--extra JSONLINE]

Pictures: OUT_PREFIX_0.png, OUT_PREFIX_1.png ... (1280x720).  Intrinsics as the game's main camera (fovy 53 degrees, 16:9)."""
import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))


def look_at(eye, tgt, fy=2.0, aspect=16 / 9):
    f = [t - e for t, e in zip(tgt, eye)]
    n = math.sqrt(sum(x * x for x in f))
    f = [x / n for x in f]
    up = [0.0, -1.0, 0.0]  # file space: -Y is up
    r = [f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]]
    rn = math.sqrt(sum(x * x for x in r))
    r = [x / rn for x in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    # the probe's rows: x = +right, y = -(file y component of up) ... matches the game's c56..c59 (row1 has the negative y scale)
    rows = []
    for v, s in ((r, fy / aspect), (u, fy), (f, 1.0), (f, 1.0)):
        d = -sum(a * b for a, b in zip(v, eye))
        rows.append([s * v[0], s * v[1], s * v[2], s * d])
    # row1 sign: up in file space is -Y; u (computed from r x f) already points to file -Y when the camera is level
    return {"cmd": "camera", "viewproj": [x for row in rows for x in row], "eye": list(eye)}


def main():
    a = sys.argv[1:]
    arena, prefix = int(a[0]), a[1]
    cams, extra = [], []
    i = 2
    while i < len(a):
        if a[i] == '--extra':
            extra.append(a[i + 1]); i += 2; continue
        e, t = a[i].split(':')
        cams.append((tuple(float(x) for x in e.split(',')), tuple(float(x) for x in t.split(','))))
        i += 1
    script = prefix + '.jsonl'
    with open(script, 'w') as f:
        f.write(json.dumps({"cmd": "load", "kind": "arena", "id": arena}) + '\n')
        for e in extra:
            f.write(e + '\n')
        for k, (e, t) in enumerate(cams):
            f.write(json.dumps(look_at(e, t)) + '\n')
            f.write(json.dumps({"cmd": "shot", "path": '%s_%d.png' % (prefix.replace(chr(92), '/'), k), "w": 1280, "h": 720}) + '\n')
    exe = os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe')
    game = os.environ.get('WWE_GAME', os.path.join(REPO, 'game_data', 'extracted'))
    p = subprocess.run([exe, '--game', game, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad:
        print('viewer errors:', bad[:3])
    print('done', len(cams), 'pictures')


if __name__ == '__main__':
    main()

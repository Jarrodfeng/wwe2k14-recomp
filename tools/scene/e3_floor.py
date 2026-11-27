"""Lane E3 (Session 103): the game's wrestler shadow map and its readers' constants from the stream, and a camera-independent comparison with ours.

Used by frame_rig.py (the offline rig) and e3_check.py. 0 game runs: fview replays the capture's own stream, headless.

Why a floor comparison: our shadow camera is FITTED (src\\renderer_scene\\shadow_fit.h), the game's is its own trapezoid fit, so the two maps are framed differently and a pixel
difference between them means nothing. What a reader does with a map is look a point up: so both maps are read the way the colour pass reads them (the readers' matrix, the
range, the stored distance) at the same grid of points on the floor under the wrestlers, and the two answers (in shadow / lit) are compared. The numbers: IoU of the shadowed
points, and the share of each side's shadowed points the other also has.

Conventions (measured, brock_losswin f110 and z7147):
  game space is Y DOWN (a wrestler's body is at negative y; the light travels toward +y); the floor under the casters is their largest y.
  readers' rows: u = dot(r0, p) / dot(r3, p), v = dot(r1, p) / dot(r3, p) (p = (x, y, z, 1)); the receiver's distance is dot(r3, p); stored distance = (distance - near) / (far - near).
  The stream's TexProjLightXY = r0, r1; TexProjLightW = r3 (= ViewLightDepth); ShadowPrm row 0 = (near, far, ...).
"""
import glob, json, os, re, subprocess
import numpy as np
from PIL import Image


def _run(cmd, env=None, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def _case(fview, root, game, capture, n, op, extra=()):
    rc, text = _run([fview, '--capture', capture, '--frame', str(n), '--save-case', '--op', str(op), '--root', root, '--game', game] + list(extra), env=dict(os.environ, CLAUDECODE='1'))
    line = [l for l in text.splitlines() if 'case saved:' in l]   # review 9: the path fview prints, not a diff of out/cases
    return (line[-1].split('case saved:')[1].strip() if line else None), text


def game_shadow(fview, root, game, capture, n, work):
    """-> dict(map=float array HxW (R channel / 255), rows=(r0, r1, r3) as np arrays, near, far, key=(x, y, z)), or (None, reason)."""
    env = dict(os.environ, CLAUDECODE='1')
    js = os.path.join(work, 'e3_ops_%d.json' % n)
    rc, text = _run([fview, '--capture', capture, '--frame', str(n), '--ops', '--json', js, '--root', root, '--game', game], env)
    if rc != 0 or not os.path.exists(js):
        return None, 'fview --ops failed: ' + text[-200:]
    ops = json.load(open(js))['ops']
    passes, cur = [], None
    for o in ops:
        if o['kind'] == 'targets':
            cur = {'op': o['op'], 'surface': o['colors'][0] if o['colors'] else 0, 'tech': {}, 'last': o['op']}
            passes.append(cur)
        elif cur is not None:
            cur['last'] = o['op']
            if o['kind'] == 'draw':
                cur['tech'][o.get('technique', '')] = cur['tech'].get(o.get('technique', ''), 0) + 1
    maps = [p for p in passes if 'T_ShadowMap' in p['tech'] and 'T_CircleShadow' not in p['tech']]
    if not maps:
        return None, 'no shadow map pass in the stream frame'
    mapb = maps[-1]   # map A comes first (collapse), map B last (all casters)
    rc, text = _run([fview, '--capture', capture, '--frame', str(n), '--surface', str(mapb['surface']), '--through-op', str(mapb['last']), '--save-case', '--op', str(mapb['last']), '--root', root, '--game', game], env)
    line = [l for l in text.splitlines() if 'case saved:' in l]
    if not line:
        return None, 'fview --save-case (map surface) failed'
    img = np.asarray(Image.open(os.path.join(line[-1].split('case saved:')[1].strip(), 'picture.png')).convert('RGB')).astype(np.float32) / 255.0
    draws = [o for o in ops if o['kind'] == 'draw' and o.get('outcome') == 'drawn' and str(o.get('effect', '')).startswith('yCh_')]
    if not draws:
        return None, 'no wrestler colour draw in the stream frame'
    cdir, text = _case(fview, root, game, capture, n, draws[0]['op'])
    if not cdir:
        return None, 'fview --save-case (colour draw) wrote no case'
    case = json.load(open(os.path.join(cdir, 'case.json')))
    c = {(k.get('stage'), k.get('field')): k.get('values') or [] for k in case['draw'].get('constants', [])}
    xy, w, prm, key = c.get(('vs', 'TexProjLightXY')), c.get(('vs', 'TexProjLightW')), c.get(('ps', 'ShadowPrm')), c.get(('vs', 'KeyLightVec'))
    rim = c.get(('vs', 'RimLightDepth')) or [0, 0]
    if not (xy and len(xy) >= 8 and w and len(w) >= 4 and prm and len(prm) >= 2 and key and len(key) >= 3):
        return None, 'the colour draw lacks TexProjLightXY / W / ShadowPrm / KeyLightVec'
    return dict(map=img[..., 0], rows=(np.array(xy[0:4], np.float64), np.array(xy[4:8], np.float64), np.array(w[0:4], np.float64)), near=float(prm[0]), far=float(prm[1]), key=tuple(float(x) for x in key[:3]), rim=(float(rim[0]), float(rim[1]))), ''


def parse_ours(report_text):
    """The matrix / params / fit bounds the rig printed for our shadow map; None when it made none (-> the hole text)."""
    m = re.search(r'map_shadow_matrix ([^\n]+)\n\s*map_shadow_params ([^\n]+)', report_text)
    if not m:
        h = re.search(r'map_shadow_hole "([^"]*)" holes (\d+)', report_text)
        return None, (h.group(1) if h else 'no map_shadow line in the report')
    mat = np.array([float(x) for x in m.group(1).split()], np.float64).reshape(4, 4)
    prm = [float(x) for x in m.group(2).split()]
    b = re.search(r'map_fit fitted (\d) center (\S+) (\S+) (\S+) lo (\S+) (\S+) (\S+) hi (\S+) (\S+) (\S+) samples (\d+) holes (\d+)', report_text)
    info = dict(matrix=mat, w=prm[0], h=prm[1], near=prm[2], far=prm[3])
    if b:
        v = [float(x) for x in b.groups()[1:10]]
        info.update(fitted=int(b.group(1)), center=v[0:3], lo=v[3:6], hi=v[6:9], samples=int(b.group(11)), holes=int(b.group(12)))
    return info, ''


def _shadow_flags(img, r0, r1, r3, near, far, pts, bias):
    h, w = img.shape
    p4 = np.concatenate([pts, np.ones((len(pts), 1))], axis=1)
    den = p4 @ r3
    u = (p4 @ r0) / den
    v = (p4 @ r1) / den
    ok = (u >= 0) & (u < 1) & (v >= 0) & (v < 1) & (den > 0)
    px = np.clip((u * w).astype(int), 0, w - 1)
    py = np.clip((v * h).astype(int), 0, h - 1)
    stored = img[py, px]
    recv = (den - near) / (far - near)
    return ok & (stored < 0.995) & (recv > stored + bias), ok


def floor_agreement(game, ours_png, ours, bias=0.025, grid=120, span=2.4):
    """The two maps read at the same floor grid under OUR casters' box. -> dict of numbers."""
    oimg = np.asarray(Image.open(ours_png).convert('RGB')).astype(np.float32)[..., 0] / 255.0
    cx, cy, cz = ours['center']
    R = max(1.0, float(np.linalg.norm((np.array(ours['hi']) - np.array(ours['lo'])) / 2)))
    floor_y = ours['hi'][1]   # Y DOWN: the largest y is the floor under the feet
    xs = np.linspace(cx - span * R, cx + span * R, grid)
    zs = np.linspace(cz - span * R, cz + span * R, grid)
    X, Z = np.meshgrid(xs, zs)
    pts = np.stack([X.ravel(), np.full(X.size, floor_y), Z.ravel()], axis=1)
    g_sh, g_ok = _shadow_flags(game['map'], game['rows'][0], game['rows'][1], game['rows'][2], game['near'], game['far'], pts, bias)
    M = ours['matrix']
    o_sh, o_ok = _shadow_flags(oimg, M[0], M[1], M[3], ours['near'], ours['far'], pts, bias)
    inter, union = int((g_sh & o_sh).sum()), int((g_sh | o_sh).sum())
    return dict(points=int(len(pts)), game_px=int(g_sh.sum()), ours_px=int(o_sh.sum()), inter=inter, iou=(inter / union if union else 1.0),
                ours_in_game=(inter / o_sh.sum() if o_sh.sum() else 1.0), game_in_ours=(inter / g_sh.sum() if g_sh.sum() else 1.0), grid=(X.shape, floor_y, R),
                masks=(g_sh.reshape(X.shape), o_sh.reshape(X.shape)))


def casters_inside(ours, margin=0.0):
    """The 8 corners of the casters' measured box (game space) read through OUR readers' matrix: how many land inside the map (u, v in [margin, 1 - margin])? -> (inside, u range, v range)."""
    lo, hi = ours['lo'], ours['hi']
    M = ours['matrix']
    us, vs, inside = [], [], 0
    for corner in range(8):
        p = np.array([hi[0] if corner & 1 else lo[0], hi[1] if corner & 2 else lo[1], hi[2] if corner & 4 else lo[2], 1.0])
        den = p @ M[3]
        u, v = (p @ M[0]) / den, (p @ M[1]) / den
        us.append(u), vs.append(v)
        inside += int(margin <= u <= 1 - margin and margin <= v <= 1 - margin)
    return inside, (min(us), max(us)), (min(vs), max(vs))

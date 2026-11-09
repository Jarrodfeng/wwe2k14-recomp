r"""Session 99 (smoke density): the entrance particle sprites of one game frame against the viewer's, at the game's own camera.
0 game runs: it reads a RenderDoc dump of the game's particle draws (tools\assets\replay\rdc_smoke.py), the probe CSV of the same
run and the player's own sprite textures, and launches the viewer ONCE (particles on / off shots + its sprite list for the default
draw and for --seeds other random draws, {"cmd":"particles_dump"} / the entrance command's "particle_seed").

  py -I tools\assets\smoke_compare.py OUTDIR --game DUMPDIR --probe PROBE.csv --swap N [--wrestler 107 --arena 0 --cue 5 --f 402
         --bank 107 --effect 5 --seeds 12 --no-launch]

DUMPDIR = rdc_smoke.py's OUT (run.json, rt_before.raw / rt_after.raw, depth.raw of the game's 2560x1440 scene target).
What it prints:
  per emitter: count, centre, spread, vertex alpha, quad edge and view depth w, game vs viewer;
  a CPU REPLAY of the sprites over the game's own pre-smoke picture with the game's per-pixel rule (alpha = vertex alpha x
  texture alpha x min(|scene w - sprite w|, 30) / 30, the sprite discarded behind the scene, SrcAlpha / InvSrcAlpha; checked on
  the game's draws: the replay of the game's own sprites reproduces its frame), for the game's sprites and every viewer draw, as the
  red the smoke adds per box (Kane's shirt / pants, the background).
The viewer's random stream cannot be the game's (one global generator shared by every system), so a single frame is ONE sample:
the seeds give the viewer's spread to judge the game's sample against. Local output only (game imagery, rule 1).
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
import refcheck as R  # noqa: E402
import prtc  # noqa: E402

W, H = 1280, 720
BOXES = {'shirt': (600, 300, 690, 390), 'pants': (600, 450, 640, 520), 'shins': (600, 600, 640, 700), 'bg_left': (300, 300, 400, 400), 'bg_right': (850, 300, 950, 400),
         'bg_top': (600, 60, 700, 140)}  # 1280x720 boxes on Kane's run 071 stage frame (cue 5 f402)
EMITTER_DRAWS = [(0, 29), (29, 59), (59, 82), (82, 109)]  # the game's draw order = the emitter order (Kane 5: by mode + colour keys)


def launch(out, a, vp, eye):
    lines = ['{"cmd":"load","kind":"arena","id":%d}' % a.arena, json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140})]
    st = {'cmd': 'stage', 'wrestler': a.wrestler}
    if os.path.isdir(R.MEDIA):
        st['media'] = R.MEDIA.replace('\\', '/')
    lines.append(json.dumps(st))
    ent = {'cmd': 'entrance', 'wrestler': a.wrestler, 'cue': a.cue, 'f': a.f, 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}
    raw_on = json.dumps({'cmd': 'look', 'gamma': False, 'bloom': False, 'glare': False, 'dof': False, 'fxaa': False})
    raw_off = json.dumps({'cmd': 'look', 'gamma': True, 'bloom': True, 'glare': True, 'dof': True, 'fxaa': True})
    p = lambda n: os.path.join(out, n).replace('\\', '/')  # noqa: E731
    arms = [('on', True, 0), ('off', False, 0)] + [('seed%d' % k, True, k) for k in range(1, a.seeds + 1)]
    for arm, parts, seed in arms:
        e = dict(ent, particles=parts, particle_seed=seed)
        lines += [json.dumps(e), json.dumps({'cmd': 'entrance_time', 'cue': a.cue, 'f': a.f, 'camera': 'free'}),
                  json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': eye})]
        if parts:
            lines.append(json.dumps({'cmd': 'particles_dump', 'path': p('viewer_sprites_%s.json' % arm)}))
        if arm in ('on', 'off'):
            lines += [json.dumps({'cmd': 'entrance_info'}), json.dumps({'cmd': 'shot', 'path': p('viewer_%s.png' % arm), 'w': W, 'h': H}),
                      raw_on, json.dumps({'cmd': 'shot', 'path': p('viewer_%s_raw.png' % arm), 'w': W, 'h': H}), raw_off]
    script = os.path.join(out, 'viewer.jsonl')
    open(script, 'w').write('\n'.join(lines) + '\n')
    pr = subprocess.run([R.EXE, '--game', R.GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=300)
    open(os.path.join(out, 'viewer_out.txt'), 'w').write(pr.stdout[-60000:] + '\n--stderr--\n' + pr.stderr[-4000:])
    bad = [l for l in pr.stdout.splitlines() if '"ok":false' in l]
    if bad:
        print('viewer errors', bad[:4])


class Replay:
    """The game's per-pixel sprite rule on the CPU, at 1280x720, over the game's scene depth."""

    def __init__(self, gdir, vp, eye, textures):
        self.M = np.array(vp, float).reshape(4, 4)  # clip = M @ (world, 1), the files' -Y-up space
        self.Mi = np.linalg.inv(self.M)
        self.eye = np.array(eye, float)
        dep = np.fromfile(os.path.join(gdir, 'depth.raw'), np.float32).reshape(4096, 2560, 2)[:1440:2, ::2, 0].astype(np.float64)
        # the game's depth -> view distance (smoke PS c0 = (0, 0, 1.0000334, -10.000334); stored depth = 0.5 (1 - z_ndc), xenia's range)
        self.scene_w = 10.000334 / np.maximum(1.0000334 - (1 - 2 * dep), 1e-9)
        X, Y = np.meshgrid((np.arange(W) + 0.5) / W * 2 - 1, 1 - (np.arange(H) + 0.5) / H * 2)
        un = lambda z: (lambda c: c[..., :3] / c[..., 3:4])(np.stack([X, Y, np.full_like(X, z), np.ones_like(X)], -1) @ self.Mi.T)  # noqa
        self.O = un(0.5)
        D = un(0.99) - self.O
        self.D = D / np.linalg.norm(D, axis=-1, keepdims=True)
        self.tex = textures

    @staticmethod
    def sample(t, u, v):
        hh, ww = t.shape[:2]
        x, y = np.clip(u * ww - 0.5, 0, ww - 1.001), np.clip(v * hh - 0.5, 0, hh - 1.001)
        x0, y0 = x.astype(int), y.astype(int)
        fx, fy = (x - x0)[..., None], (y - y0)[..., None]
        return (t[y0, x0] * (1 - fx) + t[y0, x0 + 1] * fx) * (1 - fy) + (t[y0 + 1, x0] * (1 - fx) + t[y0 + 1, x0 + 1] * fx) * fy

    def draw(self, img, V0, E1, E2, col, tn):
        """The quad P = V0 + u E1 + v E2 (u, v = its texture coordinates, 0..1)."""
        n = np.cross(E1, E2)
        den = self.D @ n
        ok = np.abs(den) > 1e-9
        t = np.where(ok, ((V0 - self.O) @ n) / np.where(ok, den, 1), -1)
        P = self.O + self.D * t[..., None]
        Rv = P - V0
        a11, a12, a22 = E1 @ E1, E1 @ E2, E2 @ E2
        b1, b2 = Rv @ E1, Rv @ E2
        det = a11 * a22 - a12 * a12
        u, v = (b1 * a22 - b2 * a12) / det, (b2 * a11 - b1 * a12) / det
        m = ok & (t > 0) & (u >= 0) & (u <= 1) & (v >= 0) & (v <= 1)
        if not m.any():
            return
        gap = self.scene_w - np.concatenate([P, np.ones(P.shape[:2] + (1,))], -1) @ self.M[3]
        s = self.sample(self.tex[tn], np.clip(u, 0, 1), np.clip(v, 0, 1))
        al = np.where(m & (gap >= 0), col[3] * s[..., 3] * np.clip(gap, 0, 30) / 30, 0)[..., None]
        img[:] = np.array(col[:3]) * s[..., :3] * 255 * al + img * (1 - al)

    def run(self, base, quads):
        img = base.copy()
        for q in quads:
            self.draw(img, *q)
        return img


def game_quads(gdir, Mi, tex_names):
    run = json.load(open(os.path.join(gdir, 'run.json')))
    out, info = [], []
    for k, (a, b) in enumerate(EMITTER_DRAWS):
        for d in run[a:b]:
            v = np.array(d['verts'], float)
            clip = v[:, 0:4].copy()
            clip[:, 1] = -clip[:, 1]  # the capture's clip y points down (Vulkan); M's is up (the flat ground quads: y -184.8, std 0.1)
            wp = (Mi @ clip.T).T
            wp = wp[:, :3] / wp[:, 3:4]
            out.append((wp[0], wp[1] - wp[0], wp[2] - wp[0], v[0, 16:20], tex_names[k]))  # vertex uv 0 (0,0) 1 (1,0) 2 (0,1) 3 (1,1)
            info.append({'em': k, 'c': wp.mean(0), 'edge': max(np.linalg.norm(wp[1] - wp[0]), np.linalg.norm(wp[2] - wp[0])),
                         'a': v[0, 19] * 255, 'w': clip[:, 3].mean()})
    return out, info


def viewer_quads(path, M, eye, effect, tex_names):
    vd = json.load(open(path))
    rng = [e for e in vd['effects'] if e['effect'] == effect]
    if not rng:
        return [], [], None
    e = rng[0]
    cr, cu = M[0, :3] / np.linalg.norm(M[0, :3]), M[1, :3] / np.linalg.norm(M[1, :3])
    out, info = [], []
    for s in vd['sprites'][e['first']:e['end']]:
        c = np.array([s['c'][0], -s['c'][1], s['c'][2]])  # viewer (Y up) -> files' space
        ax, ay = np.array(s['ax']), np.array(s['ay'])
        if s['bb']:  # camera-facing: ax / ay are in the camera's right / up basis (renderer.cpp BuildSprites)
            AX, AY = ax[0] * cr + ax[1] * cu, ay[0] * cr + ay[1] * cu
        else:
            AX, AY = ax * [1, -1, 1], ay * [1, -1, 1]
        # BuildSprites' corners: u = (s + 1) / 2, v = (1 - t) / 2 -> V(u, v) = c + (2u - 1) AX + (1 - 2v) AY
        out.append((float(np.linalg.norm(c - eye)), (c - AX + AY, 2 * AX, -2 * AY, np.array(s['col']), tex_names[s['em']])))
        info.append({'em': s['em'], 'c': c, 'edge': 2 * max(np.linalg.norm(ax), np.linalg.norm(ay)), 'a': s['col'][3] * 255,
                     'w': float(M[3] @ np.append(c, 1)), 'age': s['age'], 'life': s['life']})
    out.sort(key=lambda q: -q[0])  # back to front, as the viewer draws them
    return [q[1] for q in out], info, e['frame']


def box_means(img):
    return {k: img[y0:y1, x0:x1].reshape(-1, 3).mean(0) for k, (x0, y0, x1, y1) in BOXES.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--game', required=True)
    ap.add_argument('--probe', required=True)
    ap.add_argument('--swap', type=int, required=True)
    ap.add_argument('--wrestler', type=int, default=107)
    ap.add_argument('--arena', type=int, default=0)
    ap.add_argument('--cue', type=int, default=5)
    ap.add_argument('--f', type=float, default=402)
    ap.add_argument('--bank', type=int, default=107)
    ap.add_argument('--effect', type=int, default=5)
    ap.add_argument('--seeds', type=int, default=12)
    ap.add_argument('--no-launch', action='store_true')
    a = ap.parse_args()
    # (review) local output only: OUTDIR must be under <repo>\out\ (gitignored: game imagery never reaches a tracked folder)
    allowed = [os.path.realpath(os.path.join(r, 'out')) for r in {R.REPO, R.MAIN}]
    real = os.path.realpath(a.out)
    def under(p, root):
        try:
            return os.path.commonpath([p, root]) == root
        except ValueError:  # another drive
            return False
    if not any(under(real, al) for al in allowed):
        raise SystemExit('OUTDIR must be under %s' % ' or '.join(allowed))
    os.makedirs(a.out, exist_ok=True)
    cache = os.path.join(a.out, 'camera.json')
    if os.path.exists(cache):
        vp, eye = json.load(open(cache))
    else:
        _, vp, eye = R.camera_from_probe(a.probe, a.swap)
        json.dump([vp, eye], open(cache, 'w'))
    if not a.no_launch:
        launch(a.out, a, vp, eye)
    # the effect's sprite textures, from the player's files (decoded by PIL's DDS reader; nothing written)
    d = prtc.pac.load(prtc.PAC)
    sub = dict(prtc.pac.pach(prtc.bank_entries(d, a.bank)[a.effect]))
    tdir = prtc.pac.decode(sub[1])
    import io
    import struct
    textures = {}
    for i in range(struct.unpack_from('<I', tdir, 0)[0]):
        r = 0x10 + 32 * i
        nm = tdir[r:r + 16].split(b'\0')[0].decode('latin1')
        size, off = struct.unpack_from('<II', tdir, r + 20)
        textures[nm] = np.array(Image.open(io.BytesIO(tdir[off:off + size])).convert('RGBA')).astype(float) / 255
    emitters = [r for r in prtc.records(prtc.pac.decode(sub[0]))]
    tex_names, names = [], []
    for r in emitters:
        if r[0] == 'root':
            names.append(r[1].split(' ')[0])
            tex_names.append(None)
        elif r[1] == 22:
            tex_names[-1] = r[5].rsplit('.', 1)[0]
    rp = Replay(a.game, vp, eye, textures)
    M = rp.M
    gq, gi = game_quads(a.game, rp.Mi, tex_names)
    b = np.fromfile(os.path.join(a.game, 'rt_before.raw'), np.uint8).reshape(4096, 2560, 4)[:1440, :, :3]
    af = np.fromfile(os.path.join(a.game, 'rt_after.raw'), np.uint8).reshape(4096, 2560, 4)[:1440, :, :3]
    base = np.array(Image.fromarray(b).resize((W, H), Image.BOX)).astype(float)
    game_on = np.array(Image.fromarray(af).resize((W, H), Image.BOX)).astype(float)
    rg = rp.run(base, gq)
    print('replay of the game\'s own draws vs its frame: mean |diff| %.2f / 255' % np.abs(rg - game_on).mean())
    vq, vi, vframe = viewer_quads(os.path.join(a.out, 'viewer_sprites_on.json'), M, rp.eye, a.effect, tex_names)
    print('effect %d frame: viewer %.1f; sprites game %d viewer %d' % (a.effect, vframe, len(gi), len(vi)))
    print('%-14s %9s %22s %22s %13s %11s %26s' % ('emitter', 'n g/v', 'centre game', 'centre viewer', 'alpha g/v', 'edge g/v', 'w game | viewer'))
    for k, nm in enumerate(names):
        g = [x for x in gi if x['em'] == k]
        v = [x for x in vi if x['em'] == k]
        if not g or not v:
            continue
        gc, vc = np.mean([x['c'] for x in g], 0), np.mean([x['c'] for x in v], 0)
        gw, vw = [x['w'] for x in g], [x['w'] for x in v]
        print('%-14s %4d/%-4d %22s %22s %6.0f/%-6.0f %5.0f/%-5.0f %4.0f [%3.0f..%3.0f] | %4.0f [%3.0f..%3.0f]' % (
            nm, len(g), len(v), np.round(gc).astype(int).tolist(), np.round(vc).astype(int).tolist(), np.mean([x['a'] for x in g]),
            np.mean([x['a'] for x in v]), np.mean([x['edge'] for x in g]), np.mean([x['edge'] for x in v]), np.mean(gw), min(gw), max(gw),
            np.mean(vw), min(vw), max(vw)))
    # the red the smoke adds per box: the game's frame, the replay of its draws, the replay of each viewer draw
    rows = {'game frame': box_means(game_on), 'game replay': box_means(rg)}
    seeds = ['on'] + ['seed%d' % k for k in range(1, a.seeds + 1)]
    for s in seeds:
        p = os.path.join(a.out, 'viewer_sprites_%s.json' % s)
        if os.path.exists(p):
            q, _, _ = viewer_quads(p, M, rp.eye, a.effect, tex_names)
            img = rp.run(base, q)
            rows['viewer ' + s] = box_means(img)
            if s == 'on':
                Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(a.out, 'replay_viewer_on.png'))
    b0 = box_means(base)
    print('\nred added by the smoke (replayed over the game\'s own pre-smoke picture):')
    print('%-14s ' % '' + ' '.join('%9s' % k for k in BOXES))
    for k, bm in rows.items():
        print('%-14s ' % k + ' '.join('%9.1f' % (bm[n][0] - b0[n][0]) for n in BOXES))
    vs = [k for k in rows if k.startswith('viewer')]
    if len(vs) > 1:
        arr = np.array([[rows[k][n][0] - b0[n][0] for n in BOXES] for k in vs])
        print('%-14s ' % 'viewer mean' + ' '.join('%9.1f' % x for x in arr.mean(0)))
        print('%-14s ' % 'viewer sd' + ' '.join('%9.1f' % x for x in arr.std(0)))
    Image.fromarray(np.clip(rg, 0, 255).astype(np.uint8)).save(os.path.join(a.out, 'replay_game.png'))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
r"""S103 L12: which page is the game's stage light-map page (image 388) in a MATCH, and at what scale?

  py -I tools\scene\stage_page_check.py DUMP [--arena 4]        # DUMP = a stage_page_f<frame>.rgba8 (or .png) from --wwe2k14_dump_stage_page, under out\s103\stage_page\
  py -I tools\scene\stage_page_check.py --synthetic             # offline self-test (the check): scaled / blended copies of the player's own pages must read back their scale

Compares the dumped page with the arena's own fade_lightmap00 / fade_lightmap01 (the player's bgNN.pac, decoded the way tools\arena\arena_files.py / the loader do):
  - mean ratio per channel (dump mean / page mean, of the stored 8-bit values), for each page;
  - the best single scale per page (least squares over all texels, per channel) and the residual after it: the lower residual names the page;
  - the best blend a (dump = (1 - a) * p00 + a * p01), for the fade phase (frame < clock 600).
The reference: L8 says the held page is p01 (ratio 1.0); L11's probes say the game's effective page is ~0.4x ours. Numbers only; nothing from the game is written.
Exit 1 on a self-test failure."""
import argparse
import os
import struct
import sys
from io import BytesIO

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
os.environ.setdefault('RDR_MAIN_CHECKOUT', r'C:\Users\Jarrod\wwe2k14-recomp')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))

PAGE = 512


def load_dump(path):
    """(h, w, 4) uint8 from the .rgba8 dump (u32 width, u32 height, RGBA8 rows) or a PNG."""
    if path.lower().endswith('.png'):
        return np.asarray(Image.open(path).convert('RGBA'))
    raw = open(path, 'rb').read()
    w, h = struct.unpack_from('<II', raw, 0)
    px = np.frombuffer(raw, np.uint8, offset=8)
    if px.size != w * h * 4:
        raise SystemExit('FAIL  %s: %d bytes for %dx%d' % (path, px.size, w, h))
    return px.reshape(h, w, 4)


def arena_pages(arena_id):
    import arena_files as af
    a = af.load_arena(arena_id, False)
    out = {}
    for name in ('fade_lightmap00', 'fade_lightmap01'):
        im = Image.open(BytesIO(a.textures[name].blob))
        im.load()
        out[name] = np.asarray(im.convert('RGBA'))
    return out


def fit_scale(d, p):
    """per channel: least squares k in d ~ k * p, and the rms residual of that fit (of 255)."""
    ks, res = [], []
    for c in range(3):
        x, y = p[..., c].astype(np.float64).ravel(), d[..., c].astype(np.float64).ravel()
        k = float((x * y).sum() / max((x * x).sum(), 1e-9))
        ks.append(k)
        res.append(float(np.sqrt(np.mean((y - k * x) ** 2))))
    return ks, res


def fit_blend(d, p0, p1):
    """a in dump ~ (1 - a) * p0 + a * p1 over all rgb texels (clamped 0..1), and its rms residual."""
    x = (p1[..., :3].astype(np.float64) - p0[..., :3]).ravel()
    y = (d[..., :3].astype(np.float64) - p0[..., :3]).ravel()
    a = float(np.clip((x * y).sum() / max((x * x).sum(), 1e-9), 0, 1))
    return a, float(np.sqrt(np.mean((y - a * x) ** 2)))


def analyse(d, pages):
    """The report dict (also printed by main)."""
    if d.shape[:2] != (PAGE, PAGE):
        # a dump of another size (an arena at scale): resample the pages by nearest to the dump's size
        sh = d.shape[:2]
        pages = {k: np.asarray(Image.fromarray(v).resize((sh[1], sh[0]), Image.NEAREST)) for k, v in pages.items()}
    out = {'shape': d.shape, 'mean': d[..., :3].reshape(-1, 3).mean(0), 'pages': {}}
    for name, p in pages.items():
        pm = p[..., :3].reshape(-1, 3).mean(0)
        ratio = out['mean'] / np.maximum(pm, 1e-9)
        ks, res = fit_scale(d, p)
        out['pages'][name] = {'mean': pm, 'ratio': ratio, 'k': ks, 'rms': res}
    out['blend'] = fit_blend(d, pages['fade_lightmap00'], pages['fade_lightmap01'])
    # which page: the lower residual after its own best scale (mean over channels)
    r = {n: float(np.mean(v['rms'])) for n, v in out['pages'].items()}
    out['match'] = min(r, key=r.get)
    out['match_rms'] = r
    return out


def report(o):
    print('dump %dx%d, mean rgb of 255: %s' % (o['shape'][1], o['shape'][0], np.round(o['mean'], 2)))
    for n, v in o['pages'].items():
        print('  vs %-16s page mean %s  mean ratio per channel %s  best scale %s  rms after scale %s' % (
            n, np.round(v['mean'], 2), np.round(v['ratio'], 3), np.round(v['k'], 3), np.round(v['rms'], 2)))
    print('  blend fit: a = %.3f (dump = (1 - a) p00 + a p01), rms %.2f' % o['blend'])
    print('  MATCHES %s (rms after its own scale: %s)' % (o['match'], {k: round(v, 2) for k, v in o['match_rms'].items()}))
    k1 = float(np.mean(o['pages']['fade_lightmap01']['k']))
    print('  scale of the dump against page 01: %.3f (L8: 1.0 held; L11 probe: ~0.4)' % k1)


def synthetic(arena_id):
    """A scaled copy of page 01 must read back its scale and name page 01; a copy of 00 must name 00; a 0.3 blend must read a ~0.3."""
    try:
        pages = arena_pages(arena_id)
        src = 'arena %d (the player\'s own pages)' % arena_id
    except Exception as e:  # noqa: BLE001  (no game files here: random pages, the arithmetic is the same)
        print('note: no player files (%s): random pages' % e)
        rng = np.random.default_rng(7)
        pages = {}
        for n in ('fade_lightmap00', 'fade_lightmap01'):
            base = rng.integers(20, 120, (PAGE, PAGE, 1)) + rng.integers(0, 30, (1, 1, 3))
            pages[n] = np.concatenate([base.astype(np.uint8), np.full((PAGE, PAGE, 1), 255, np.uint8)], axis=2)
        src = 'random pages'
    p0, p1 = pages['fade_lightmap00'], pages['fade_lightmap01']
    print('synthetic check on %s: page 00 mean %s, page 01 mean %s' % (src, np.round(p0[..., :3].reshape(-1, 3).mean(0), 2), np.round(p1[..., :3].reshape(-1, 3).mean(0), 2)))
    fails = []

    def scaled(p, s):
        q = p.copy().astype(np.float64)
        q[..., :3] = np.clip(np.round(q[..., :3] * s), 0, 255)
        return q.astype(np.uint8)

    for s in (0.4, 1.0, 1.5):
        o = analyse(scaled(p1, s), pages)
        k = float(np.mean(o['pages']['fade_lightmap01']['k']))
        # clipping at 255 bends a big scale: accept it only where nothing clips
        ok_scale = abs(k - s) < 0.02 or (s * float(p1[..., :3].max()) > 255)
        ok_match = o['match'] == 'fade_lightmap01'
        print('  page 01 x %.1f: best scale %.3f, matches %s -> %s' % (s, k, o['match'], 'ok' if ok_scale and ok_match else 'FAIL'))
        if not (ok_scale and ok_match):
            fails.append('page 01 x %.1f read back as scale %.3f / %s' % (s, k, o['match']))
    o = analyse(scaled(p0, 0.5), pages)
    k = float(np.mean(o['pages']['fade_lightmap00']['k']))
    ok = o['match'] == 'fade_lightmap00' and abs(k - 0.5) < 0.02
    print('  page 00 x 0.5: best scale %.3f, matches %s -> %s' % (k, o['match'], 'ok' if ok else 'FAIL'))
    if not ok:
        fails.append('page 00 x 0.5 read back as %.3f / %s' % (k, o['match']))
    blend = np.clip(np.round(0.7 * p0.astype(np.float64) + 0.3 * p1), 0, 255).astype(np.uint8)
    o = analyse(blend, pages)
    ok = abs(o['blend'][0] - 0.3) < 0.03
    print('  0.7 p00 + 0.3 p01: blend fit a = %.3f -> %s' % (o['blend'][0], 'ok' if ok else 'FAIL'))
    if not ok:
        fails.append('blend 0.3 read back as %.3f' % o['blend'][0])
    # the file round trip: a .rgba8 written like the game's dump reads back the same pixels
    import tempfile
    q = scaled(p1, 0.4)
    with tempfile.TemporaryDirectory() as td:
        f = os.path.join(td, 'stage_page_f1.rgba8')
        with open(f, 'wb') as fh:
            fh.write(struct.pack('<II', q.shape[1], q.shape[0]))
            fh.write(q.tobytes())
        back = load_dump(f)
    ok = back.shape == q.shape and bool((back == q).all())
    print('  .rgba8 round trip: %s' % ('ok' if ok else 'FAIL'))
    if not ok:
        fails.append('.rgba8 round trip')
    for f in fails:
        print('FAIL  ' + f)
    print('stage_page_check --synthetic: %s' % ('FAIL' if fails else 'PASS'))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump', nargs='?')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--synthetic', action='store_true')
    a = ap.parse_args()
    if a.synthetic:
        return synthetic(a.arena)
    if not a.dump:
        ap.error('a dump file or --synthetic')
    report(analyse(load_dump(a.dump), arena_pages(a.arena)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

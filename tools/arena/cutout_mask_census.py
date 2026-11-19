r"""S103 CPF: which materials flip when AlphaCutsOut learns "a BC2 / BC3 texture whose max alpha < 128 is a mask, never a cut-out" (0 game runs, read-only, one pac at a time).

  py -I tools\arena\cutout_mask_census.py [--game DIR]

Walks every pac\bg\bg*.pac (stage slots 0..399, texture dirs 400 / 402), pac\bg\bgEtc.pac and pac\evt\evtobj.pac (entries 0..9, texture dirs 10..19) and, per material (a mesh's diffuse
sampler, shader not 'trans'), computes the old rule (BC2/BC3, >= 8 % of texels alpha <= 32) and the new one (old AND max alpha >= 128). Prints the flipping textures with their
alpha summary, and the totals. New rule (jboy.h CPF): a texture with < 0.5 % solid texels (alpha >= 224) is a mask, except yBG_ (arena) and plain yProp_Default materials, which keep the old rule. Prints each flip's effect, spec map and the arena slot flags. The chosen variant's out_alpha is not derivable offline (front_end.cpp picks it). Wrestler ch*.pac prop slots are NOT covered. Numbers only."""
import argparse
import collections
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arena_files as af  # noqa: E402
sys.path.insert(0, os.path.join(af.ROOT, 'tools', 'wm30'))
import pac  # noqa: E402

DXT = (b'DXT3', b'DXT5')
_stat = {}


def stats(t):
    k = (t.dir, t.name, len(t.blob))
    if k in _stat:
        return _stat[k]
    im = Image.open(io.BytesIO(t.blob))
    im.load()
    al = np.asarray(im.convert('RGBA'))[..., 3]
    s = {'le32': float((al <= 32).mean()), 'zero': float((al == 0).mean()), 'max': int(al.max()), 'mean': float(al.mean() / 255.0), 'solid': float((al >= 224).mean())}
    _stat[k] = s
    return s


def pachs(data):
    p = 0x4000 if data[:4] == b'EPAC' else 0
    while p + 8 <= len(data):
        if data[p:p + 4] != b'PACH':
            p += 0x800
            continue
        count = struct.unpack_from('<I', data, p + 4)[0]
        first = p + 8 + 12 * count
        end = first
        for i in range(count):
            _, off, size = struct.unpack_from('<III', data, p + 8 + 12 * i)
            end = max(end, first + off + size)
        yield dict(pac.pach(data[p:end]))
        p = (end + 0x7FF) & ~0x7FF


def census(path, kind, rows):
    data = pac.load(path)
    for pi, ents in enumerate(pachs(data)):
        texs = {}
        flags = {}
        if kind == 'bg' and 50001 in ents:
            try:
                flags = af.filecensus_flags.parse_slot_flags(pac.decode(ents[50001]).decode('latin1'))
            except Exception:
                flags = {}
        dirs = (400, 402) if kind == 'bg' else tuple(range(10, 20))
        for d in dirs:
            if d in ents:
                try:
                    for t in af.parse_tex_dir(pac.decode(ents[d]), d):
                        texs.setdefault(t.name.lower(), t)
                except Exception:
                    pass
        for eid, raw in sorted(ents.items()):
            if eid in dirs or not (0 <= eid < 400):
                continue
            dec = pac.decode(raw)
            if dec[:4] != b'JBOY':
                continue
            try:
                meshes = af.parse_jboy(eid, dec, False)
            except Exception:
                continue
            for m in meshes:
                nm = m.diffuse
                t = texs.get(nm.lower()) if nm else None
                if t is None or t.fourcc not in DXT or 'trans' in m.effect.lower():
                    continue
                s = stats(t)
                old = s['le32'] >= 0.08
                keep = m.effect.lower().startswith('ybg_') or (m.effect.lower() == 'yprop_default' and 'sampSpecularPrm' not in m.samplers and not any('Normal' in k for k in m.samplers))
                new = old and (keep or s['solid'] >= 0.005)
                fl = ','.join(sorted(flags.get(eid, ()))) if flags else ''
                rows.append((os.path.basename(path), pi, eid, m.index, m.effect, 'spec' if 'sampSpecularPrm' in m.samplers else '-', t.name, s, old, new, fl))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='')
    a = ap.parse_args()
    base = a.game or pac.GAME
    rows = []
    d = os.path.join(base, 'pac', 'bg')
    for f in sorted(os.listdir(d)):
        if f.endswith('.pac'):
            census(os.path.join(d, f), 'bg' if f != 'bgEtc.pac' else 'etc', rows)
    census(os.path.join(base, 'pac', 'evt', 'evtobj.pac'), 'evt', rows)
    old = [r for r in rows if r[8]]
    flip = [r for r in old if not r[9]]
    print('materials with a DXT3/5 diffuse (not trans): %d; alpha_test/cutout true today: %d; flip with the new rule: %d' % (len(rows), len(old), len(flip)))
    seen = collections.OrderedDict()
    for r in flip:
        seen.setdefault((r[0], r[6]), []).append(r)
    for (pf, tn), rs in seen.items():
        s = rs[0][7]
        print('FLIP %-12s tex %-20s mats %3d  effects %-28s le32 %.3f zero %.3f max %3d solid %.3f flags %s' % (pf, tn, len(rs), ','.join(sorted({x[4] + ('+spec' if x[5] == 'spec' else '') for x in rs})), s['le32'], s['zero'], s['max'], s['solid'], rs[0][10] or '-'))
    bad = [r for r in flip if r[7]['solid'] >= 0.005]
    print('flips with solid share >= 0.5 %% (impossible by construction; sanity only): %d' % len(bad))
    near = sorted({(r[0], r[6], r[7]['max']) for r in old if r[9] and r[7]['max'] < 200})
    print('kept cut-outs with max alpha < 200 (nearest to the threshold): %s' % near[:20])
    return 0


if __name__ == '__main__':
    sys.exit(main())

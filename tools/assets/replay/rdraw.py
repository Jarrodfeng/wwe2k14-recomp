"""Frame replay (Session 98): loads one draw dumped by rdc_draw.py (system Python 3.12 + numpy + PIL) and rebuilds its per-pixel
inputs: the post-VS vertices rasterised (perspective-correct, triangle strips with restart, nearest fragment wins) into the
draw's interpolators at every covered pixel, the guest float constants by register number, the textures as float arrays with
bilinear / cube sampling, and the colour target before / after the draw. Used by oracle.py. Reads out\\ only; writes nothing.
"""
import json
import os
import struct

import numpy as np
from PIL import Image


class Draw:
    def __init__(self, d, ps_regs=None, rt_bgr=True):
        self.dir = d
        self.meta = json.load(open(os.path.join(d, 'meta.json')))
        m = self.meta
        self.vs_c = np.frombuffer(open(os.path.join(d, 'cb_vs_1_1.bin'), 'rb').read(), dtype='<f4').reshape(-1, 4)
        self.ps_raw = np.frombuffer(open(os.path.join(d, 'cb_ps_1_2.bin'), 'rb').read(), dtype='<f4').reshape(-1, 4)
        self.ps_regs = ps_regs
        self.ps_c = {}
        if ps_regs:
            for i, r in enumerate(ps_regs):
                self.ps_c[r] = self.ps_raw[i].astype(np.float64)
        sysb = open(os.path.join(d, 'cb_ps_1_0.bin'), 'rb').read()
        self.sys_flags = struct.unpack_from('<I', sysb, 0)[0]
        self.alpha_ref = struct.unpack_from('<f', sysb, 272)[0]
        self.exp_bias = struct.unpack_from('<4f', sysb, 288)
        self.gamma = bool(self.sys_flags & (1 << 19))
        self.tex = {}
        for t in m['textures']:
            faces = []
            for f in t['faces']:
                im = np.asarray(Image.open(os.path.join(d, f['png'])).convert('RGBA'), dtype=np.float32) / 255.0
                if f.get('raw') and t['fmt'] == 'R16G16B16A16_UNORM':
                    raw = np.frombuffer(open(os.path.join(d, f['raw']), 'rb').read(), dtype='<u2')
                    im = (raw[:t['w'] * t['h'] * 4].reshape(t['h'], t['w'], 4).astype(np.float32) / 65535.0)
                if rt_bgr and f.get('raw') and t['fmt'] == 'R8G8B8A8_UNORM':  # a guest render target read back as a texture: the guest
                    im = im[:, :, [2, 1, 0, 3]]                                # 8888 order is BGRA to the shader (eye cube A/B, S98)
                faces.append(im)
            self.tex[t['bind']] = {'meta': t, 'faces': faces}
        rt = m['rt']
        def rd_rt(fn):
            p = os.path.join(d, fn)
            if not os.path.exists(p):
                return None
            a = np.frombuffer(open(p, 'rb').read(), dtype=np.uint8)
            return a[:rt['w'] * rt['h'] * 4].reshape(rt['h'], rt['w'], 4)
        vp = m['viewport']
        self.vw, self.vh = int(vp[2]), int(vp[3])
        self.rt_after = rd_rt('rt_after.raw')
        self.rt_before = rd_rt('rt_before.raw')
        if self.rt_after is not None:
            self.rt_after = self.rt_after[:self.vh, :self.vw]
        if self.rt_before is not None:
            self.rt_before = self.rt_before[:self.vh, :self.vw]
        self._raster()

    def _raster(self):
        pv = self.meta['postvs']
        stride = pv['stride'] // 4
        vb = np.frombuffer(open(os.path.join(self.dir, 'postvs.bin'), 'rb').read(), dtype='<f4')
        nv = len(vb) // stride
        V = vb[:nv * stride].reshape(nv, stride // 4, 4).astype(np.float64)  # [vertex, slot, 4]: slot 0 = position, 1.. = interpolators 0..
        self.nslots = V.shape[1] - 1
        idx = np.frombuffer(open(os.path.join(self.dir, 'postvs_idx.bin'), 'rb').read(), dtype='<u2').astype(np.int64)
        tris = []
        strip = []
        topo = pv['topology']
        if 'Strip' in topo:
            for i in idx:
                if i == 0xFFFF:
                    strip = []
                    continue
                strip.append(i)
                if len(strip) >= 3:
                    a, b, c = strip[-3], strip[-2], strip[-1]
                    if a != b and b != c and a != c:
                        tris.append((a, b, c))
        else:
            for k in range(0, len(idx) - 2, 3):
                tris.append(tuple(idx[k:k + 3]))
        tris = np.array(tris, dtype=np.int64)
        W, H = self.vw, self.vh
        pos = V[:, 0, :]
        w = pos[:, 3]
        sx = (pos[:, 0] / w * 0.5 + 0.5) * W
        sy = (pos[:, 1] / w * 0.5 + 0.5) * H
        depth = np.full((H, W), np.inf)
        tri_id = np.full((H, W), -1, dtype=np.int64)
        bary = np.zeros((H, W, 3))
        for t, (a, b, c) in enumerate(tris):
            if w[a] <= 0 or w[b] <= 0 or w[c] <= 0:
                continue
            xs, ys = sx[[a, b, c]], sy[[a, b, c]]
            x0, x1 = max(int(np.floor(xs.min())), 0), min(int(np.ceil(xs.max())), W - 1)
            y0, y1 = max(int(np.floor(ys.min())), 0), min(int(np.ceil(ys.max())), H - 1)
            if x1 < x0 or y1 < y0:
                continue
            px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
            if abs(d) < 1e-12:
                continue
            l1 = ((px - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (py - ys[0])) / d
            l2 = ((xs[1] - xs[0]) * (py - ys[0]) - (px - xs[0]) * (ys[1] - ys[0])) / d
            l0 = 1 - l1 - l2
            inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not inside.any():
                continue
            # perspective-correct weights
            q0, q1, q2 = l0 / w[a], l1 / w[b], l2 / w[c]
            s = q0 + q1 + q2
            zw = 1.0 / s  # the fragment's clip w (view depth)
            sub_d = depth[y0:y1 + 1, x0:x1 + 1]
            nearer = inside & (zw < sub_d)
            sub_d[nearer] = zw[nearer]
            tri_id[y0:y1 + 1, x0:x1 + 1][nearer] = t
            bb = bary[y0:y1 + 1, x0:x1 + 1]
            bb[nearer] = np.stack([q0 / s, q1 / s, q2 / s], -1)[nearer]
        self.cover = tri_id >= 0
        ys, xs = np.nonzero(self.cover)
        self.px = (xs, ys)
        T = tris[tri_id[ys, xs]]
        B = bary[ys, xs]
        # interpolators per covered pixel: [n, slot, 4]
        self.interp = (V[T[:, 0]] * B[:, 0, None, None] + V[T[:, 1]] * B[:, 1, None, None] + V[T[:, 2]] * B[:, 2, None, None])
        self.clip_w = depth[ys, xs]
        self.verts = V
        self.tris = tris

    def I(self, k):
        """Interpolator k (the guest's numbering) at every covered pixel, [n, 4]."""
        return self.interp[:, 1 + k, :]

    def game_pixels(self):
        xs, ys = self.px
        return self.rt_after[ys, xs].astype(np.float64) / 255.0

    def changed(self):
        """Pixels the draw changed (before != after) - the game's own coverage."""
        if self.rt_before is None:
            return None
        return np.any(self.rt_after != self.rt_before, axis=-1)

    def deriv(self, vals):
        """Screen derivatives (d/dx, d/dy) of a per-pixel quantity [n, k] (forward difference inside the coverage, backward at its
        edge; 0 where the pixel has no covered neighbour)."""
        xs, ys = self.px
        H, W = self.vh, self.vw
        k = vals.shape[1]
        grid = np.full((H, W, k), np.nan)
        grid[ys, xs] = vals
        def d(dx, dy):
            f = grid[np.clip(ys + dy, 0, H - 1), np.clip(xs + dx, 0, W - 1)] - vals
            b = vals - grid[np.clip(ys - dy, 0, H - 1), np.clip(xs - dx, 0, W - 1)]
            out = np.where(np.isnan(f), b, f)
            return np.nan_to_num(out)
        return d(1, 0), d(0, 1)

    # ---- texture sampling: bilinear on mip 0, or trilinear on a box-filtered pyramid when `lod_from_uv` (per-pixel LOD from the
    # screen derivatives of the coordinate, as a GPU picks it; `bias` = the shader's LOD bias) ----
    def sample2d(self, bind, uv, clamp=False, lod_from_uv=True, bias=0.0, aniso=False):
        t = self.tex[bind]
        if not lod_from_uv:
            return bilinear(t['faces'][0], uv, clamp)
        pyr = t.get('pyr')
        if pyr is None:
            pyr = t['pyr'] = pyramid(t['faces'][0])[:max(1, int(t['meta']['mips']))]
        h, w = pyr[0].shape[:2]
        dx, dy = self.deriv(uv)
        lx = np.hypot(dx[:, 0] * w, dx[:, 1] * h)
        ly = np.hypot(dy[:, 0] * w, dy[:, 1] * h)
        rho = np.minimum(lx, ly) if aniso else np.maximum(lx, ly)
        lod = np.clip(np.log2(np.maximum(rho, 1e-8)) + bias, 0, len(pyr) - 1)
        l0 = np.floor(lod).astype(int)
        f = (lod - l0)[:, None]
        out = np.zeros((len(uv), pyr[0].shape[2]))
        for L in np.unique(l0):
            m = l0 == L
            a = bilinear(pyr[L], uv[m], clamp)
            b = bilinear(pyr[min(L + 1, len(pyr) - 1)], uv[m], clamp)
            out[m] = a * (1 - f[m]) + b * f[m]
        return out

    def sample_cube(self, bind, d):
        return sample_cube(self.tex[bind]['faces'], d)


def pyramid(img):
    out = [img]
    while min(out[-1].shape[:2]) > 1:
        a = out[-1]
        h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
        a = a[:h, :w]
        out.append((a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) * 0.25)
    return out


def bilinear(img, uv, clamp=False):
    h, w = img.shape[:2]
    x = uv[:, 0] * w - 0.5
    y = uv[:, 1] * h - 0.5
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0)[:, None]
    fy = (y - y0)[:, None]
    def at(xx, yy):
        if clamp:
            xx = np.clip(xx, 0, w - 1); yy = np.clip(yy, 0, h - 1)
        else:
            xx = np.mod(xx, w); yy = np.mod(yy, h)
        return img[yy, xx].astype(np.float64)
    return (at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy +
            at(x0 + 1, y0 + 1) * fx * fy)


def sample_cube(faces, d):
    x, y, z = d[:, 0], d[:, 1], d[:, 2]
    ax, ay, az = np.abs(x), np.abs(y), np.abs(z)
    face = np.where((ax >= ay) & (ax >= az), np.where(x >= 0, 0, 1), np.where(ay >= az, np.where(y >= 0, 2, 3), np.where(z >= 0, 4, 5)))
    sc = np.select([face == 0, face == 1, face == 2, face == 3, face == 4], [-z, z, x, x, x], -x)
    tc = np.select([face == 0, face == 1, face == 2, face == 3, face == 4], [-y, -y, z, -z, -y], -y)
    ma = np.select([face == 0, face == 1, face == 2, face == 3, face == 4], [ax, ax, ay, ay, az], az)
    ma = np.maximum(ma, 1e-20)
    uv = np.stack([(sc / ma + 1) * 0.5, (tc / ma + 1) * 0.5], -1)
    out = np.zeros((len(d), 4))
    for f in range(6):
        m = face == f
        if m.any():
            out[m] = bilinear(faces[f], uv[m], clamp=True)
    return out


def pwl_gamma(x):
    x = np.clip(x, 0, 1)
    p3 = x >= 512 / 1023; p2 = x >= 128 / 1023; p1 = x >= 64 / 1023
    scale = np.where(p2, np.where(p3, 1023 / 8, 1023 / 4), np.where(p1, 1023 / 2, 1023.0))
    off = np.where(p2, np.where(p3, 128 / 255, 64 / 255), np.where(p1, 32 / 255, 0.0))
    return np.trunc(x * scale) / 255 + off


def epilogue(draw, rgb):
    c = rgb * draw.exp_bias[0]
    if draw.gamma:
        c = pwl_gamma(c)
    return np.trunc(np.clip(c, 0, 1) * 255 + 0.5) / 255

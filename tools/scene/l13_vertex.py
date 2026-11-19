#!/usr/bin/env python3
r"""S103 L13b: the per-draw experiment (0 game runs). The game's yBG_Default draws of one stream frame are evaluated from the STREAM's inputs (constants, page = the
file's fade_lightmap01 [C = image 388 in a match, L12], vertices: the stream's, or the file mesh for a thin draw), with fx_bg.glsl's vertex formula (page x c184.z x vertex colour x
(4 lights + hemisphere); the microcode's, check_effect-proven), rasterised at 640x360 with depth among these draws, times the diffuse (file DDS, mip MIP=2), and compared per draw with
the case's picture and the rig's picture on the pixels where the rig agrees with the prediction.
  py -I tools\scene\l13_vertex.py CASE.zip FRAME OURS_WHOLE.png
Numbers only; nothing from the game is written."""
import sys, os, io, struct, zipfile, collections
import numpy as np
from PIL import Image
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'scene')); sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
import feature_diff as fd, arena_files as af
CASE = sys.argv[1]
F = int(sys.argv[2])
ours_png = sys.argv[3]   # the rig's ours_<F>_whole.png of the same record
W, H = 640, 360
fx = fd.Fx(); case = fd.Case(CASE)
fr, g, _ = fd.game_frame(case, fx, F)
a = af.load_arena(4)
def decode(name):
    im = Image.open(io.BytesIO(bytes(a.textures[name].blob))); im.load(); return np.asarray(im.convert('RGBA'), np.float32) / 255
page = decode('fade_lightmap01')
def samp(img, u, v):  # bilinear, wrap
    h, w = img.shape[:2]
    x = (u % 1.0) * w - 0.5; y = (v % 1.0) * h - 0.5
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx_ = (x - x0)[..., None]; fy = (y - y0)[..., None]
    t = lambda xx, yy: img[yy % h, xx % w]
    return t(x0, y0) * (1 - fx_) * (1 - fy) + t(x0 + 1, y0) * fx_ * (1 - fy) + t(x0, y0 + 1) * (1 - fx_) * fy + t(x0 + 1, y0 + 1) * fx_ * fy
def sampc(img, u, v):  # bilinear, clamp
    h, w = img.shape[:2]
    return samp(img, np.clip(u, 0, 0.9999), np.clip(v, 0, 0.9999))
pic = np.asarray(Image.open(io.BytesIO(zipfile.ZipFile(CASE).read('picture.png'))).convert('RGB').resize((W, H), Image.BOX), np.float32) / 255
ours = np.asarray(Image.open(ours_png).convert('RGB').resize((W, H), Image.BOX), np.float32) / 255
buf = fr.buf
nsec = struct.unpack_from('<I', buf, 12)[0]
secs = {}
for i in range(nsec):
    sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * i); secs[sid] = (es, cnt, off)
lay = np.frombuffer(buf, np.dtype([('ef', '<u4'), ('ec', '<u4'), ('hs', '<u4'), ('gs', '<u4')]), secs[11][1], secs[11][2])
fes, fcnt, foff = secs[7]
geo = zipfile.ZipFile(CASE).open('capture/geometry.bin')
pos_index = {}
for slot, ms in a.slots.items():
    for m in ms:
        if m.positions is not None and m.n:
            for q in m.positions: pos_index.setdefault(tuple(np.round(q, 1)), []).append(m)
import capread
obj_slot = {}
geo2 = zipfile.ZipFile(CASE).open('capture/geometry.bin')
for fn in range(F, max(F - 400, 0), -1):
    if fn not in case.frame_names: continue
    f2 = capread.Frame(case.src.read(case.frame_names[fn]))
    b2 = f2.buf
    ns2 = struct.unpack_from('<I', b2, 12)[0]; s2 = {}
    for i in range(ns2):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', b2, 32 + 24 * i); s2[sid] = (es, cnt, off)
    lay2 = np.frombuffer(b2, np.dtype([('ef', '<u4'), ('ec', '<u4'), ('hs', '<u4'), ('gs', '<u4')]), s2[11][1], s2[11][2])
    for i in range(len(f2.draws)):
        d2 = f2.draws[i]; o2 = f2.origins[i]
        key = (int(o2['pass_site']), int(o2['object']))
        if key in obj_slot or int(d2['vertex_bytes']) == 0 or int(d2['layout']) >= len(lay2) or int(o2['klass']) != 1: continue
        st2 = int(lay2[int(d2['layout'])]['hs'])
        if st2 < 12: continue
        geo2.seek(int(d2['vertex_offset'])); r2 = geo2.read(min(int(d2['vertex_bytes']), st2 * 8))
        cand = collections.Counter()
        for q in range(len(r2) // st2):
            for m in pos_index.get(tuple(np.round(np.frombuffer(r2, '<f4', 3, q * st2), 1)), []):
                if m.index == int(o2['part']): cand[m.slot] += 1
        if cand: obj_slot[key] = cand.most_common(1)[0][0]
print('objects joined to slots', len(obj_slot))
tex_cache = {}
zbuf = np.full((H, W), np.inf, np.float32); idbuf = np.full((H, W), -1, np.int32); pred = np.zeros((H, W, 3), np.float32); lmb = np.zeros((H, W, 3), np.float32)
info = {}
MIP = int(os.environ.get('MIP', '2'))
for di in range(len(fr.draws)):
    vs = g[di]['vs'] or ''
    if not (vs.startswith('yBG_Default_vertex_lm_vertex') or vs.startswith('yBG_Default_base_vertex')): continue
    d = fr.draws[di]
    fixed = buf[foff + fes * int(d['fixed']): foff + fes * (int(d['fixed']) + 1)]
    topo = fixed[4]
    lab, per_stage, _ = fx.eff.resolve(int(d['vs_hash']), int(d['ps_hash']))
    R = {f: fr.reg(int(d['vs_bank']), r, c).astype(np.float64) for f, r, c in per_stage['vertex']}
    o = fr.origins[di]
    thin = int(d['vertex_bytes']) == 0
    if thin:
        key = (int(o['pass_site']), int(o['object']))
        if key not in obj_slot: continue
        slot, mi = obj_slot[key], int(o['part'])
        if slot not in a.slots or mi >= len(a.slots[slot]): continue
        mesh = a.slots[slot][mi]
        if mesh.positions is None: continue
        n = mesh.n
        P = np.c_[mesh.positions.astype(np.float64), np.ones(n)]
        N = mesh.normal.astype(np.float64)
        C = mesh.colour[:, [1, 2, 3, 0]].astype(np.float64) / 255
        UV = mesh.uv0.astype(np.float64)
        idx = mesh.indices.astype(np.int64)
        topo = 4
    else:
        st = int(lay[int(d['layout'])]['hs'])
        geo.seek(int(d['vertex_offset'])); raw = geo.read(int(d['vertex_bytes']))
        geo.seek(int(d['index_offset'])); ib = geo.read(int(d['index_bytes']))
        idx = np.frombuffer(ib, '<u4' if d['index_32bit'] else '<u2').astype(np.int64)
        n = len(raw) // st
        rec = np.frombuffer(raw, np.uint8).reshape(n, st)
        P = rec[:, 0:16].copy().view('<f4').astype(np.float64)
        N = rec[:, 16:22].copy().view('<i2').astype(np.float64) / 32767.0
        C = rec[:, 24:28].astype(np.float64) / 255
        UV = rec[:, 28:32].copy().view('<f2').astype(np.float64)
        cand = collections.Counter()
        for q in range(min(n, 8)):
            for m in pos_index.get(tuple(np.round(P[q, :3], 1)), []): cand[(m.slot, m.index)] += 1
        if not cand: continue
        slot, mi = cand.most_common(1)[0][0]
        mesh = a.slots[slot][mi]
    tex = mesh.diffuse
    if tex not in tex_cache:
        try:
            im = Image.open(io.BytesIO(bytes(a.textures[tex].blob))); im.load(); arr = im.convert('RGBA')
            arr = arr.resize((max(1, arr.width >> MIP), max(1, arr.height >> MIP)), Image.BOX)
            tex_cache[tex] = np.asarray(arr, np.float32) / 255
        except Exception:
            tex_cache[tex] = None
    T = tex_cache[tex]
    if T is None: continue
    Wm = R['WorldMatrixArray']
    wp = (Wm @ np.c_[P[:, :3], np.ones(n)].T).T
    nw = (Wm[:, :3] @ N.T).T
    dif = np.zeros((n, 3))
    for k in range(4):
        dif += np.maximum(nw @ R['DifLghVec'][k, :3], R['vMin'][0, k])[:, None] * R['DifLghCol'][k, :3]
    t = 0.5 * (-nw @ R['SkyDir'][0, :3]) + 0.5
    hemi = R['SkyCol'][0, :3] + (R['GroundCol'][0, :3] - R['SkyCol'][0, :3]) * t[:, None]
    L = dif * R['MatDifCol'][0, :3] + hemi * R['MatAmbCol'][0, :3]
    vlm = 'vertex_lm' in vs
    if vlm:
        lmuv = wp[:, [0, 2]] * R['LightMapParam'][0, :2] * 0.5 + 0.5
        pg = sampc(page, lmuv[:, 0], lmuv[:, 1])[:, :3] * R['LightMapParam'][0, 2]
    else:
        pg = np.ones((n, 3))
    E = pg * C[:, :3] * L
    clip = (R['ViewProj'] @ np.c_[wp, np.ones(n)].T).T
    # triangles
    if topo == 4:   # strip with restart
        tris = []
        run = []
        for v in idx:
            if v == 0xFFFF or v == 0xFFFFFFFF: run = []; continue
            run.append(v)
            if len(run) >= 3:
                k = len(run) - 3
                tris.append((run[k], run[k + 1], run[k + 2]) if k % 2 == 0 else (run[k + 1], run[k], run[k + 2]))
        tris = np.array(tris, np.int64).reshape(-1, 3)
    else:
        tris = idx[:len(idx) // 3 * 3].reshape(-1, 3)
    info[di] = dict(tex=tex, slot=slot, mesh=mi, vlm=vlm, topo=int(topo), Lmean=L.mean(0), pgmean=pg.mean(0))
    for tri in tris:
        if tri.max() >= n: continue
        cw = clip[tri]
        if (cw[:, 3] <= 1e-3).any(): continue
        sx = (cw[:, 0] / cw[:, 3] * 0.5 + 0.5) * W; sy = (0.5 - cw[:, 1] / cw[:, 3] * 0.5) * H; sz = cw[:, 2] / cw[:, 3]
        x0, x1 = max(int(np.floor(sx.min())), 0), min(int(np.ceil(sx.max())), W - 1)
        y0, y1 = max(int(np.floor(sy.min())), 0), min(int(np.ceil(sy.max())), H - 1)
        if x0 > x1 or y0 > y1: continue
        area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
        if abs(area) < 1e-9: continue
        X, Y = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((sx[1] - X) * (sy[2] - Y) - (sx[2] - X) * (sy[1] - Y)) / area
        w1 = ((sx[2] - X) * (sy[0] - Y) - (sx[0] - X) * (sy[2] - Y)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any(): continue
        z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2]
        yy, xx = np.nonzero(inside)
        Yi, Xi = yy + y0, xx + x0
        zz = z[inside]
        closer = zz < zbuf[Yi, Xi]
        if not closer.any(): continue
        Yi, Xi, zz = Yi[closer], Xi[closer], zz[closer]
        b = np.stack([w0[inside][closer], w1[inside][closer], w2[inside][closer]], 1)
        # perspective-correct
        iw = b / cw[:, 3]; iw = iw / iw.sum(1, keepdims=True)
        e = iw @ E[tri]; uv = iw @ UV[tri]
        dc = samp(T, uv[:, 0], uv[:, 1])[:, :3]
        zbuf[Yi, Xi] = zz; idbuf[Yi, Xi] = di; pred[Yi, Xi] = e * dc; lmb[Yi, Xi] = e
print('draws rasterised', len(info))
cov = idbuf >= 0
print('pixels covered', cov.sum(), 'of', W * H)
vis = cov & (np.abs(ours.mean(2) - pred.mean(2)) < 0.35 * np.maximum(pred.mean(2), 0.02)) & (pred.mean(2) > 0.01)
print('pixels where ours ~ prediction (within 35 %%): %d' % vis.sum())
print('ALL visible-agreeing pixels: mean pred %.4f ours %.4f picture %.4f   ours/pic %.2f  pred/pic %.2f' % (pred[vis].mean(), ours[vis].mean(), pic[vis].mean(), ours[vis].mean() / pic[vis].mean(), pred[vis].mean() / pic[vis].mean()))
print('draw  slot:mesh tex            class  px    pred    ours    pic   ours/pic  pred/pic   light(mean)        page*gain(mean)')
rows = []
for di, inf in info.items():
    m = vis & (idbuf == di)
    if m.sum() < 30: continue
    rows.append((m.sum(), di, inf, pred[m].mean(), ours[m].mean(), pic[m].mean(), pic[m].mean(0)))
for cnt, di, inf, p_, o_, q_, qc in sorted(rows, key=lambda r: -r[0])[:30]:
    print('%4d %3d:%-2d %-14s %-5s %5d %.4f %.4f %.4f  %.2f  %.2f   %s %s' % (di, inf['slot'], inf['mesh'], inf['tex'][:14], 'VLM' if inf['vlm'] else 'plain', cnt, p_, o_, q_, o_ / q_, p_ / q_, np.round(inf['Lmean'], 3), np.round(inf['pgmean'], 3)))

# ratio vs depth (clip z/w) and vs the predicted brightness, on visible-agreeing pixels
zz = zbuf[vis]; rr = ours[vis].mean(1) / np.maximum(pic[vis].mean(1), 1e-3); pp = pred[vis].mean(1)
qs = np.quantile(zz, np.linspace(0, 1, 9))
print('by depth (z/w):')
for a0, a1 in zip(qs[:-1], qs[1:]):
    s = (zz >= a0) & (zz <= a1)
    print('  z %.5f-%.5f n %6d  ours/pic (of means) %.2f   pred %.3f' % (a0, a1, s.sum(), ours[vis][s].mean() / pic[vis][s].mean(), pp[s].mean()))
qs = np.quantile(pp, np.linspace(0, 1, 7))
print('by predicted brightness:')
for a0, a1 in zip(qs[:-1], qs[1:]):
    s = (pp >= a0) & (pp <= a1)
    print('  pred %.3f-%.3f n %6d  ours/pic %.2f' % (a0, a1, s.sum(), ours[vis][s].mean() / pic[vis][s].mean()))

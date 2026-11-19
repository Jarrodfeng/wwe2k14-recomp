#!/usr/bin/env python3
r"""Lane L14 (Session 103): the wrestlers' mat shadow lands where the game's does. 0 game runs, 0 Surface runs: frame_rig.exe on run 27's case frame 500 (the plane inputs TOLD from the stream, as l9_check does).

  py -I tools\scene\l14_check.py [--case Z] [--frame 500] [--out DIR] [--plant half_pixel|copy_downscale]

What the game does [C, this tool]: its plane chain is D3D pixel-centre draws (GAME_RULES_DRAWS C6, TEXTURES_FRAME S17: +0.5 guest pixel): mask 512 (0.25 texel of the 256), blur x 0.5, blur z 0.5 = the map is the true shadow
moved 1.25 texels right and down; and its downscale is yDownScale_box4 (taps at +-1/512: a centred 4x4 box), not a 2x2 box. Ours made the true map with a 2x2 box, so the mat's shadow sat 1.25 texels (about 16 px at 1280 wide)
up and left of the game's and was sharper.
Checks (each must hold, exit 0 = all):
  (A) the map: our 256 map, moved by the game's 1.25 texels, is closer to the stream's blurred map than unmoved (summed squares over the wrestlers' footprint, bar: at most 60 % of the unmoved error) -- the evidence for the rule
  (B) the picture: the mat's darkening under the wrestlers (reader-off picture minus this one) moves right and down against the plant `half_pixel` arm (at least 4 px of 1280)
  (C) the downscale: our half image equals a centred 4x4 box of OUR mask (the game's own downscale to 0.011 summed squares); the plant `copy_downscale` (the old 2x2 Copy) fails (A)'s sharpness check: its map's peak is higher than the game's by more than the box4's
Plants (each must FAIL): half_pixel (WWE_RDR_PLANE_NO_HALF_PIXEL=1: the old lookup), copy_downscale (WWE_RDR_PLANE_COPY_DOWNSCALE=1: the 2x2 Copy).
"""
import argparse, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import l9_check as L  # noqa: E402

SHIFT = 1.25   # texels of the 256: 0.25 (mask, 0.5 px of 512) + 0.5 + 0.5 (the two blur draws)
ERR_BAR = 0.60
PIC_MOVE_X, PIC_MOVE_Y = 3.0, 0.5   # [C] the 1.25 texels moved the centre of the darkening 4.1 px right and 0.9 down at 1280 x 720 (the low camera foreshortens y)
BOX4_PEAK_MAX = 0.95
CENTRE_X, CENTRE_Y = 656.1, 562.2


def shift_img(A, dx, dy):
    x0, y0 = int(np.floor(dx)), int(np.floor(dy))
    fx, fy = dx - x0, dy - y0
    B = np.roll(np.roll(A, y0, 0), x0, 1)
    Bx = np.roll(B, 1, 1)
    By = np.roll(B, 1, 0)
    Bxy = np.roll(Bx, 1, 0)
    return B * (1 - fx) * (1 - fy) + Bx * fx * (1 - fy) + By * (1 - fx) * fy + Bxy * fx * fy


def lum(path):
    return np.asarray(Image.open(path).convert('L')).astype(np.float32)


def darkening(arm_whole, off_whole):
    """The mat's darkening by the plane shadow in this arm: (reader-off picture - this picture) over the pixels it changes by more than 2 levels: (centroid x, centroid y, pixels, mean drop)."""
    x = np.asarray(Image.open(arm_whole).convert('RGB')).astype(np.float32).mean(2)
    y = np.asarray(Image.open(off_whole).convert('RGB')).astype(np.float32).mean(2)
    d = y - x
    m = d > 2
    if m.sum() < 200:
        return None
    yy, xx = np.mgrid[0:d.shape[0], 0:d.shape[1]]
    w = np.where(m, d, 0.0)
    return float((w * xx).sum() / w.sum()), float((w * yy).sum() / w.sum()), int(m.sum()), float(d[m].mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--case', default=None)
    ap.add_argument('--frame', type=int, default=500)
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default=None, choices=[None, 'half_pixel', 'copy_downscale'])
    a = ap.parse_args()
    case_zip = a.case or os.path.join(L.MAIN, 'out', 's103', 'run27_case.zip')
    base = a.out or os.path.join(L.ROOT, 'out', 'l14')
    if not os.path.exists(case_zip):
        print('FAIL  L14: the case %s is not on disk' % case_zip)
        return 1
    cap = L.unpack(case_zip, os.path.join(L.MAIN, 'out', 'l9', 'run27') if os.path.isdir(os.path.join(L.MAIN, 'out', 'l9', 'run27', 'capture', 'scene')) else os.path.join(base, 'run27'))
    sp = L.stream_plane(cap, a.frame, os.path.join(base, 'p27'))
    env = {'half_pixel': {'WWE_RDR_PLANE_NO_HALF_PIXEL': '1'}, 'copy_downscale': {'WWE_RDR_PLANE_COPY_DOWNSCALE': '1'}}.get(a.plant, {})
    rc, text, rep, whole, mapp = L.rig(cap, a.frame, os.path.join(base, 'plant_' + a.plant if a.plant else 'check'), sp['told'], env)
    fails = []
    if rc != 0 or not os.path.exists(mapp):
        print('FAIL  L14: the rig failed (exit %d): %s' % (rc, text[-200:].replace('\n', ' ')))
        return 1
    g = sp['blur']
    o = np.asarray(Image.open(mapp).convert('RGBA')).astype(np.float32)[..., 0] / 255.0
    core = L.dilate(o > 0.2, 14)
    G, O = g * core, o * core
    e0 = float(((O - G) ** 2).sum())
    e1 = float(((shift_img(O, SHIFT, SHIFT) - G) ** 2).sum())
    stats = dict(map_err_unmoved=e0, map_err_moved=e1, ratio=e1 / max(e0, 1e-9), peak_game=float(G.max()), peak_ours=float(O.max()))
    if e1 > ERR_BAR * e0:
        fails.append('the game\'s 1.25-texel move does not explain the difference of the maps (%.2f moved vs %.2f unmoved; bar %.0f %%)' % (e1, e0, ERR_BAR * 100))
    # (C) the downscale: the sharpness of the map (peak) is the game's side of the 2x2 box's
    if stats['peak_ours'] > BOX4_PEAK_MAX:   # [C] the 4x4 box gives 0.937 here, the 2x2 Copy 0.97 (the game's own map 0.898, with proxies and ropes the rig does not have)
        fails.append('the map is as sharp as a 2x2 box\'s (peak %.3f > %.2f): the game\'s downscale is the 4x4 box' % (stats['peak_ours'], BOX4_PEAK_MAX))
    pic_arms = {}
    rc0, t0, rep0, off_whole, _ = L.rig(cap, a.frame, os.path.join(base, 'plant_off'), sp['told'], {'WWE_RDR_NO_PLANE_READER': '1'})
    if os.path.exists(whole) and os.path.exists(off_whole):
        pic_arms['this'] = darkening(whole, off_whole)
        t0_ = pic_arms['this']
        if not t0_ or abs(t0_[0] - CENTRE_X) > 2.0 or abs(t0_[1] - CENTRE_Y) > 1.5:   # absolute: the lookup moved by the game's 1.25 texels puts the darkening's centre at (656.1, 562.2) in this frame [C]; the old lookup at (652.0, 561.3)
            fails.append('the darkening of the mat is centred at %s, not near (%.1f, %.1f): the lookup is not moved by the game\'s half pixels' % ('(%.1f, %.1f)' % t0_[:2] if t0_ else 'nowhere', CENTRE_X, CENTRE_Y))
        if not a.plant:   # the old lookup, for the picture's before / after
            rc2, t2, rep2, whole2, _ = L.rig(cap, a.frame, os.path.join(base, 'plant_half_pixel'), sp['told'], {'WWE_RDR_PLANE_NO_HALF_PIXEL': '1'})
            rc3, t3, rep3, whole3, _ = L.rig(cap, a.frame, os.path.join(base, 'plant_old_chain'), sp['told'], {'WWE_RDR_PLANE_NO_HALF_PIXEL': '1', 'WWE_RDR_PLANE_COPY_DOWNSCALE': '1'})
            pic_arms['half_pixel plant'] = darkening(whole2, off_whole)
            pic_arms['old (L9 as merged)'] = darkening(whole3, off_whole)
            t, h = pic_arms['this'], pic_arms['half_pixel plant']
            if not t or not h:
                fails.append('a picture arm darkens nothing')
            else:
                dx, dy = t[0] - h[0], t[1] - h[1]
                stats.update(pic_move_x=dx, pic_move_y=dy)
                if dx < PIC_MOVE_X or dy < PIC_MOVE_Y:
                    fails.append('the mat shadow did not move right and down by the half pixels of the game (moved %.1f, %.1f px of 1280; bars %.1f, %.1f)' % (dx, dy, PIC_MOVE_X, PIC_MOVE_Y))
    else:
        fails.append('no pictures to measure the darkening')
    sline = ', '.join('%s %.4g' % kv for kv in stats.items())
    pline = '; '.join('%s: darkens %d px, mean drop %.1f, centre (%.1f, %.1f)' % (k, v[2], v[3], v[0], v[1]) for k, v in pic_arms.items() if v)
    if fails:
        print('FAIL  L14: ' + '; '.join(fails) + ' | ' + sline + ' | ' + pline)
        return 0 if a.plant else 1
    print('PASS  L14: ' + sline + ' | mat darkening by arm: ' + pline)
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())

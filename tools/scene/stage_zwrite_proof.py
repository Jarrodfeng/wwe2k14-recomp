#!/usr/bin/env python3
r"""SZW: the three-arm proof of the see-through stage slots' depth write (docs\research\harness\SZW_contract.md "Proof" 2). 0 game runs: the offline rig only.

  py -I tools\scene\stage_zwrite_proof.py --capture C:\...\out\captures\brock_losswin --frame 200 [--size 960x540] [--out DIR] [--check]
  py -I tools\scene\stage_zwrite_proof.py --record R.rsc --camera 16floats --eye x,y,z [...]      (a record with a told camera: arena_rec's beauty records)
  py -I tools\scene\stage_zwrite_proof.py --selftest                                              (the arithmetic on synthetic pictures; gate lane szw_zwrite)

What it answers: does the game's rule (an `s` / `a` stage slot writes depth unless `z`, every stage draw is alpha-tested GEQUAL 1/255, so a see-through piece hides what draws after it only
where its texel alpha is >= 1/255) hold in OUR renderer without cutting a rectangle out of what draws later? Three one-shot frame_rig draws of the same record and camera, the arena family
alone (--families 0x1, --no-hooks), each with its scene depth (frame_rig --depth-out):
  off   = WWE_RDR_STAGE_ZWRITE=0 (the switch off: today's draw, no depth write for `s` / `a`)
  on    = the default (the fix: depth write + the forced alpha twin)
  plant = WWE_RDR_PLANT_SZW_NO_TEST=1 (the depth write with NO forced alpha test: the planted hole)
Per pixel (shares of the pixels the off arm covers):
  written   = the on arm's depth is NEARER than off's: a tested see-through piece wrote depth there (the footprint of texels with alpha >= 1/255 of the pieces in front)
  farther   = the on arm's depth is FARTHER than off's (a depth write can only bring a surface nearer: must be 0)
  lost      = covered in off, left at the clear depth in on (a vanished surface: must be 0)
  unexplained = colour changed (on vs off, any channel by more than 2) at a pixel NO piece wrote depth at: draw-order changes among the see-through pieces themselves (the `s` group drawn in slot order, not
              far to near) and a z-only slot's blend; bounded by THRESHOLDS (set from the first runs, written beside them)
  hole      = the plant's depth is nearer than on's: a depth written by a texel the test discards (the rectangle cut out of what draws later). The plant must show it (>= plant_hole_min), the on arm shows none by
              construction (the census line [scene][szw] counts depth writes with no discard: 0) and the check says so through `farther` / `lost` / `unexplained`.
Not built (said, not hidden): the per-mesh texel footprint of the contract's proof 2 (arena_cams.h ExpectPx on the pieces' own textures): `written` is defined by the same alpha twin the draw uses.
Writes OUT\<name>_{off,on,plant}.png (+ .depth raw), OUT\<name>_proof.json, OUT\<name>_proof.txt and OUT\<name>_proof_sheet.png (off | on | plant | on vs off | plant vs on). All scratch under out\.
"""
import argparse, json, os, subprocess, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import stage_cull_proof as scp   # the capture camera, the depth reader, the look camera (the SCU proof's own helpers)

# Set from the first runs (see docs\research\harness\SZW_proof.md): the numbers measured on brock_losswin frame 200, the capture's own camera.
THRESHOLDS = {
    'farther_max': 0.0005,        # share of covered pixels the on arm may show FARTHER than off (a depth write only brings a surface nearer; depth format rounding aside)
    'lost_max': 0.0005,           # share the on arm may lose (a surface vanishing: measured 0)
    'unexplained_max': 0.05,      # share of covered pixels that change colour where no piece wrote depth (order changes among see-through pieces; measured in SZW_proof.md)
    'plant_hole_min': 0.0005,     # the plant must show at least this share of hole pixels (a depth written by a discarded texel), else the proof cannot see a hole
}
EPS = 1e-6
CLEAR = 0.999999


def analyse(rgb, depth):
    """rgb / depth: dict arm -> HxWx3 uint8 / HxW float (arms off, on, plant). -> dict of the numbers (shares of the pixels the off arm covers)."""
    cov = depth['off'] < CLEAR
    n = max(1, int(cov.sum()))
    on, off, pl = depth['on'], depth['off'], depth['plant']
    written = (on < off - EPS)
    farther = (on > off + EPS) & cov
    lost = cov & (on >= CLEAR)
    chg = np.abs(rgb['on'].astype(np.int16) - rgb['off'].astype(np.int16)).max(axis=-1) > 2
    unexplained = chg & ~written
    hole = (pl < on - EPS)
    chg_pl = np.abs(rgb['plant'].astype(np.int16) - rgb['on'].astype(np.int16)).max(axis=-1) > 2
    return {'covered': int(cov.sum()),
            'written': float(written.sum()) / n, 'farther': float(farther.sum()) / n, 'lost': float(lost.sum()) / n,
            'changed': float(chg.sum()) / n, 'unexplained': float(unexplained.sum()) / n,
            'hole': float(hole.sum()) / n, 'plant_changed': float((chg_pl & hole).sum()) / n,
            'masks': {'written': written, 'unexplained': unexplained, 'hole': hole}}


def judge(res, th=THRESHOLDS):
    """-> (ok, lines). The on arm must not lose or push a surface back and must not change colour where nothing was written beyond the order bound; the plant must show holes."""
    lines, ok = [], True
    if res['farther'] > th['farther_max']:
        ok = False
        lines.append('FAIL on: %.4f of the covered pixels are FARTHER than with the switch off (a depth write cannot do that)' % res['farther'])
    if res['lost'] > th['lost_max']:
        ok = False
        lines.append('FAIL on: %.4f of the covered pixels vanish (a surface lost: the cut-out rectangle of a depth write with no discard?)' % res['lost'])
    if res['unexplained'] > th['unexplained_max']:
        ok = False
        lines.append('FAIL on: %.4f of the covered pixels change colour where no tested piece wrote depth (above the %.4f order bound)' % (res['unexplained'], th['unexplained_max']))
    if res['hole'] < th['plant_hole_min']:
        ok = False
        lines.append('FAIL plant: the planted depth write with no test shows only %.5f hole pixels: the proof cannot see a hole' % res['hole'])
    else:
        lines.append('PASS plant: the planted depth write with no test cuts %.5f of the pixels out (%.5f of them change the picture): a hole is seen' % (res['hole'], res['plant_changed']))
    if ok:
        lines.append('PASS')
    return ok, lines


def run_arm(rig, base_cmd, prefix, env_add, timeout=600):
    env = dict(os.environ)
    for k in ('WWE_RDR_STAGE_ZWRITE', 'WWE_RDR_PLANT_SZW_NO_TEST'):
        env.pop(k, None)
    env.update(env_add)
    cmd = [rig] + base_cmd + ['--prefix', prefix, '--depth-out', prefix + '.depth']
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, env=env)
    if p.returncode != 0:
        raise RuntimeError('frame_rig failed (%d): %s' % (p.returncode, (p.stdout + p.stderr)[-600:]))
    census = [ln for ln in (p.stdout + p.stderr).splitlines() if '[scene][szw]' in ln]
    return prefix + '_mask1.png', prefix + '.depth', census


def sheet(rgb, masks, path, labels):
    from PIL import Image, ImageDraw
    H, W = rgb['off'].shape[:2]
    def mark(base, m, colour):
        im = (base.astype(np.float32) * 0.35).astype(np.uint8)
        im[m] = colour
        return im
    tiles = [rgb['off'], rgb['on'], rgb['plant'], mark(rgb['on'], masks['written'], (60, 200, 255)), mark(rgb['on'], masks['unexplained'], (255, 220, 0)), mark(rgb['plant'], masks['hole'], (255, 40, 40))]
    out = Image.new('RGB', (W * 3, (H + 24) * 2), (20, 20, 20))
    d = ImageDraw.Draw(out)
    for i, (t, lab) in enumerate(zip(tiles, labels)):
        x, y = (i % 3) * W, (i // 3) * (H + 24)
        out.paste(Image.fromarray(t), (x, y + 24))
        d.text((x + 4, y + 4), lab, fill=(255, 255, 255))
    out.save(path)


# entry: judge (through analyse, as main() calls them)
def selftest():
    """the proof's arithmetic on synthetic pictures: the right arms, a depth write that pushes a surface back, a vanished surface, a bound-breaking order change, a plant that shows no hole"""
    H, W = 40, 80
    d_off = np.full((H, W), 0.5); d_off[:5] = 1.0
    rgb_off = np.full((H, W, 3), 100, np.uint8); rgb_off[:5] = 30
    def arm(write=0.2, hole=0.1, farther=0, lost=0, order=0.0):
        d_on = d_off.copy(); c_on = rgb_off.copy()
        k = int(round(write * 35))
        d_on[5:5 + k, :40] = 0.3; c_on[5:5 + k, :40] = 160             # a tested piece wrote depth here, hid something: changed AND written
        if farther:
            d_on[H - 1, :10] = 0.9
        if lost:
            d_on[5 + 20, :int(lost * 80)] = 1.0
        o = int(round(order * 35))
        if o:
            c_on[H - o:, 40:] = 200                                       # a colour change where nothing was written: order only
        d_pl = d_on.copy(); c_pl = c_on.copy()
        h = int(round(hole * 35))
        d_pl[5 + k:5 + k + h, 40:] = 0.3; c_pl[5 + k:5 + k + h, 40:] = 90   # the planted depth write at discarded texels
        return c_on, d_on, c_pl, d_pl
    fails = 0
    def case(name, a, want_ok, want_token):
        nonlocal fails
        res = analyse({'off': rgb_off, 'on': a[0], 'plant': a[2]}, {'off': d_off, 'on': a[1], 'plant': a[3]})
        ok, lines = judge(res)
        good = ok == want_ok and (want_token is None or any(want_token in l for l in lines))
        print('%s selftest %s: ok=%s %s' % ('PASS' if good else 'FAIL', name, ok, ' | '.join(lines)))
        fails += 0 if good else 1
    case('right arms', arm(), True, 'PASS plant')
    case('a depth write pushes a surface back', arm(farther=1), False, 'FARTHER')
    case('a surface vanishes', arm(lost=0.5), False, 'vanish')
    case('an order change beyond the bound', arm(order=0.6), False, 'order bound')
    case('the plant shows no hole (the proof is blind)', arm(hole=0.0), False, 'cannot see a hole')
    print('selftest: %d failure(s)' % fails)
    return 0 if fails == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture'); ap.add_argument('--frame', type=int)
    ap.add_argument('--record'); ap.add_argument('--camera'); ap.add_argument('--eye')
    ap.add_argument('--game', default=None); ap.add_argument('--size', default='960x540')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'szw', 'proof')); ap.add_argument('--name', default=None)
    ap.add_argument('--rig', default=os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'))
    ap.add_argument('--extra', default='', help='extra frame_rig arguments (space separated), e.g. --look-entrance')
    ap.add_argument('--check', action='store_true'); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    main_root = os.environ.get('RDR_MAIN_CHECKOUT')
    if not main_root:   # the player's files live in the MAIN checkout (a worktree has no game_data)
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main_root = os.path.normpath(os.path.dirname(common)) if common else ROOT
    game = a.game or os.path.join(main_root, 'game_data', 'extracted')
    W, H = (int(v) for v in a.size.split('x'))
    if a.capture is not None:
        if not os.path.isabs(a.capture) and not os.path.isdir(a.capture):
            a.capture = os.path.join(main_root, a.capture)
        M, vp, eye, why = scp.camera_from_capture(a.capture, a.frame)
        rsc = os.path.join(a.capture, 'scene', 'frame_%06d.rsc' % a.frame)
        name = a.name or '%s_f%d' % (os.path.basename(os.path.normpath(a.capture)), a.frame)
    else:
        vp = [float(x) for x in a.camera.split(',')]; eye = [float(x) for x in a.eye.split(',')]
        rsc, why = a.record, 'the told camera'
        name = a.name or os.path.splitext(os.path.basename(a.record))[0]
    os.makedirs(a.out, exist_ok=True)
    base = ['--record', rsc, '--game', game, '--size', '%dx%d' % (W, H), '--families', '0x1', '--no-hooks',
            '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)] + [x for x in a.extra.split() if x]
    arms = {'off': {'WWE_RDR_STAGE_ZWRITE': '0'}, 'on': {}, 'plant': {'WWE_RDR_PLANT_SZW_NO_TEST': '1'}}
    rgb, depth, fmt, census = {}, {}, None, {}
    for arm, env in arms.items():
        png, dpath, census[arm] = run_arm(a.rig, base, os.path.join(a.out, '%s_%s' % (name, arm)), env)
        rgb[arm] = scp.png_rgb(png)
        d, fmt = scp.depth_values(open(dpath, 'rb').read(), W * H)
        depth[arm] = d.reshape(H, W)
    res = analyse(rgb, depth)
    masks = res.pop('masks')
    res['camera'] = why; res['depth_format'] = fmt; res['record'] = rsc; res['size'] = [W, H]; res['census'] = census
    ok, lines = judge(res)
    res['verdict'] = lines
    sheet(rgb, masks, os.path.join(a.out, name + '_proof_sheet.png'),
          ['%s: switch OFF (before)' % name, 'switch ON (the fix)', 'PLANT: depth write, no forced test', 'ON: blue = a tested piece wrote depth (%.3f)' % res['written'],
           'ON vs OFF: yellow = changed with no depth written (%.4f)' % res['unexplained'], 'PLANT: red = depth by a discarded texel (%.5f)' % res['hole']])
    json.dump(res, open(os.path.join(a.out, name + '_proof.json'), 'w'), indent=1)
    txt = ['stage_zwrite_proof %s (%s, depth %s, %dx%d), covered %d px' % (name, why, fmt, W, H, res['covered']),
           'on: written %.4f farther %.5f lost %.5f changed %.4f unexplained %.4f | plant: hole %.5f (changes the picture at %.5f)' %
           (res['written'], res['farther'], res['lost'], res['changed'], res['unexplained'], res['hole'], res['plant_changed'])]
    for arm in ('off', 'on', 'plant'):
        txt += ['census %s: %s' % (arm, ln.strip()) for ln in census[arm][:2]]
    txt += lines
    open(os.path.join(a.out, name + '_proof.txt'), 'w').write('\n'.join(txt) + '\n')
    print('\n'.join(txt))
    return 0 if (ok or not a.check) else 1


if __name__ == '__main__':
    sys.exit(main())

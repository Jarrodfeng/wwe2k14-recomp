#!/usr/bin/env python3
r"""S105 C3: the three-arm depth proof of the family cull (docs\research\s105\C3.md "Sites the build will touch" 12). 0 game runs: the offline rig only.

  py -I tools\scene\family_cull_proof.py --capture C:\...\out\captures\brock_losswin --frame 40 [--families 0x8,0x2,0x4] [--size 960x540] [--out DIR] [--check]
  py -I tools\scene\family_cull_proof.py --selftest                                          (the arithmetic on synthetic depth pictures)

What it answers: with the game's cull block applied to every model family (record word +0x8C bit 0x2 set = back faces culled, front clockwise), does our renderer keep the OUTER side of
every wrestler / ring / prop surface seen from the capture's own camera? Three one-shot frame_rig draws of the same record and camera, ONE family mask each (wrestlers 0x8, ring 0x2, props 0x4),
each with its scene depth (frame_rig --depth-out):
  off   = WWE_RDR_FAMILY_CULL=0 (the switch off: the family two-sided, the draw before)
  on    = the default (the fix)
  plant = WWE_RDR_PLANT_FAMILY_CULL_FLIP=1 (cull on, the front face swapped: the planted wrong winding, the outer side culled)
Per pixel, against off: nearer = depth NEARER than off's (culling only removes faces: must be 0); lost = covered in off, at the clear depth in the arm (a hole: the outer side culled); changed = colour differs by more than 2.
The check (--check): nearer ~0 and lost ~0 with the fix on; the plant must lose at least PLANT_LOST_MIN of the covered pixels, so the proof can see a wrong side (a proof that cannot see it proves nothing).
All scratch under out\ (pictures of game assets are never committed).
"""
import argparse, json, os, subprocess, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import stage_cull_proof as scp   # depth_values / png_rgb / camera_from_capture: the SCU proof's helpers

THRESHOLDS = {
    'nearer_max': 0.0005,       # share of covered pixels an arm may show nearer than off (culling only removes faces)
    'on_lost_max': 0.01,        # cull on: share of covered pixels that vanish (a surface lost: the cull side)
    'plant_lost_min': 0.02,     # the plant must lose at least this share of the covered pixels (the proof sees a wrong side)
    'plant_ratio_min': 10.0,    # ... and at least this many times the cull-on arm's loss
    'min_covered': 500,         # a family with fewer covered pixels in the picture says nothing: reported "empty", never a PASS
}
FAMILY_NAMES = {0x1: 'arena', 0x2: 'ring', 0x4: 'props', 0x8: 'wrestlers', 0xE: 'ring+props+wrestlers'}


def analyse(rgb, depth, clear=0.999999):
    off_cov = depth['off'] < clear
    n = int(off_cov.sum())
    out = {'covered': n}
    if 'off2' in rgb:   # S105 C3 F2: the colour noise floor = a second, identical off arm against the first (the rig's own run-to-run difference)
        nz = np.abs(rgb['off2'].astype(np.int16) - rgb['off'].astype(np.int16)).max(axis=-1) > 2
        out['noise'] = {'changed': int(nz.sum()), 'changed_share': float(nz.sum()) / max(1, n)}
    for arm in ('on', 'plant'):
        if arm not in depth:
            continue
        diff = np.abs(rgb[arm].astype(np.int16) - rgb['off'].astype(np.int16)).max(axis=-1) > 2
        lost = off_cov & (depth[arm] >= clear)
        nearer = (depth[arm] < depth['off'] - 1e-6) & (depth['off'] < 1.0 + 1e-9)
        farther = off_cov & (depth[arm] > depth['off'] + 1e-4)   # a closed mesh with the OUTER side culled shows its inside: still covered, but deeper (not a hole)
        c = max(1, n)
        out[arm] = {'nearer': int(nearer.sum()), 'nearer_share': float(nearer.sum()) / c, 'lost': int(lost.sum()), 'lost_share': float(lost.sum()) / c, 'farther': int(farther.sum()), 'farther_share': float(farther.sum()) / c,
                    'bad_share': float((lost | farther).sum()) / c,
                    'changed': int(diff.sum()), 'changed_share': float(diff.sum()) / c,
                    'unexplained': int((diff & ~(lost | farther | nearer)).sum()), 'unexplained_share': float((diff & ~(lost | farther | nearer)).sum()) / c}   # colour moved where the nearest surface did NOT change: a translucent / additive layer, the thing culling must not touch
    return out


def judge(res, th=THRESHOLDS):
    """-> (ok, lines)"""
    if res['covered'] < th['min_covered']:
        return False, ['EMPTY: only %d covered pixels in the off arm (the family is not in this picture)' % res['covered']]
    lines, ok = [], True
    on, pl = res['on'], res['plant']
    for arm, a in (('on', on), ('plant', pl)):
        if a['nearer_share'] > th['nearer_max']:
            ok = False
            lines.append('FAIL %s: %d pixels (%.4f) nearer than the cull-off arm (culling can only remove faces)' % (arm, a['nearer'], a['nearer_share']))
    if on['bad_share'] > th['on_lost_max']:
        ok = False
        lines.append('FAIL on: %.4f of the covered pixels vanish or fall deeper (a surface lost its outer side: the cull side is wrong for some family)' % on['bad_share'])
    if pl['bad_share'] < th['plant_lost_min'] or pl['bad_share'] < th['plant_ratio_min'] * max(on['bad_share'], 1e-6):
        ok = False
        lines.append('FAIL plant: the planted wrong winding loses / deepens only %.4f (cull on %.4f): the proof cannot see a wrong side' % (pl['bad_share'], on['bad_share']))
    else:
        lines.append('PASS plant: the planted wrong winding loses / deepens %.4f of the covered pixels (cull on %.4f)' % (pl['bad_share'], on['bad_share']))
    if 'noise' in res:   # colour: the fix may change no more pixels than the off arm's own noise (derived from the second off arm, not tuned)
        nz = res['noise']['changed_share']
        if on['unexplained_share'] > nz + 1e-9:   # a pixel whose nearest face was culled (lost / deeper) changes colour by the rule itself; only the rest is judged
            ok = False
            lines.append('FAIL colour: the cull-on arm changes %.5f of the covered pixels where the nearest surface did not move, above the off arm noise floor %.5f (a translucent / additive layer lost)' % (on['unexplained_share'], nz))
        else:
            lines.append('PASS colour: cull-on change off the culled pixels %.5f <= off-arm noise floor %.5f (all changes %.5f)' % (on['unexplained_share'], nz, on['changed_share']))
    if ok:
        lines.append('PASS')
    return ok, lines


def run_arm(rig, base_cmd, prefix, mask, env_add, timeout=600):
    env = dict(os.environ)
    for k in ('WWE_RDR_FAMILY_CULL', 'WWE_RDR_PLANT_FAMILY_CULL_FLIP', 'WWE_RDR_STAGE_CULL', 'WWE_RDR_PLANT_STAGE_CULL_NO_FLIP'):
        env.pop(k, None)
    env.update(env_add)
    cmd = [rig] + base_cmd + ['--families', '0x%x' % mask, '--prefix', prefix, '--depth-out', prefix + '.depth']
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, env=env)
    if p.returncode != 0:
        raise RuntimeError('frame_rig failed (%d): %s' % (p.returncode, (p.stdout + p.stderr)[-600:]))
    return '%s_mask%x.png' % (prefix, mask), prefix + '.depth'


def sheet(rgb, lost_on, lost_pl, path, label):
    from PIL import Image, ImageDraw
    H, W = rgb['off'].shape[:2]
    def mark(base, lost):
        im = (base.astype(np.float32) * 0.35).astype(np.uint8)
        im[lost] = (255, 40, 40)
        return im
    tiles = [rgb['off'], rgb['on'], rgb['plant'], mark(rgb['on'], lost_on), mark(rgb['plant'], lost_pl)]
    labs = [label + ': cull OFF (two-sided, before)', 'cull ON (the fix)', 'PLANT: cull on, winding swapped', 'ON: red = lost vs off', 'PLANT: red = lost vs off']
    out = Image.new('RGB', (W * 3, H * 2 + 48), (20, 20, 20))
    pos = [(0, 0), (W, 0), (2 * W, 0), (W, 1), (2 * W, 1)]
    d = ImageDraw.Draw(out)
    for t, (x, row), lab in zip(tiles, pos, labs):
        y = row * (H + 24)
        out.paste(Image.fromarray(t), (x, y + 24))
        d.text((x + 4, y + 4), lab, fill=(255, 255, 255))
    out.save(path)


def selftest():
    H, W = 40, 80
    d_off = np.full((H, W), 0.5); d_off[:5] = 1.0
    rgb_off = np.full((H, W, 3), 100, np.uint8)
    def arm(lost=0.0, nearer=False):
        d = d_off.copy(); c = rgb_off.copy()
        rows = int(round(lost * 35))
        d[5:5 + rows] = 1.0; c[5:5 + rows] = 30
        if nearer:
            d[H - 1, :10] = 0.1
        return c, d
    fails = 0
    def case(name, on, pl, want_ok, token):
        nonlocal fails
        res = analyse({'off': rgb_off, 'off2': rgb_off, 'on': on[0], 'plant': pl[0]}, {'off': d_off, 'on': on[1], 'plant': pl[1]})
        ok, lines = judge(res, dict(THRESHOLDS, min_covered=100))
        good = ok == want_ok and (token is None or any(token in l for l in lines))
        print('%s selftest %s: ok=%s %s' % ('PASS' if good else 'FAIL', name, ok, ' | '.join(lines)))
        fails += 0 if good else 1
    case('right side', arm(0.0), arm(0.5), True, 'PASS plant')
    case('planted wrong winding never seen (UNSAFE direction)', arm(0.0), arm(0.0), False, 'cannot see a wrong side')
    case('the fix loses a surface', arm(0.3), arm(0.6), False, 'surface lost')
    c_on = arm(0.0); c_on[0][20:25] = 60   # colour moves, depth does not (a lost translucent layer): the colour judge must say FAIL (UNSAFE direction)
    case('colour change above the noise floor', c_on, arm(0.5), False, 'FAIL colour')
    case('a nearer pixel', arm(0.0, nearer=True), arm(0.5), False, 'nearer than')
    res = analyse({'off': rgb_off[:3], 'on': rgb_off[:3], 'plant': rgb_off[:3]}, {'off': np.ones((3, W)), 'on': np.ones((3, W)), 'plant': np.ones((3, W))})
    ok, lines = judge(res)
    good = (not ok) and 'EMPTY' in lines[0]
    print('%s selftest an empty family is never a PASS: %s' % ('PASS' if good else 'FAIL', lines[0]))
    fails += 0 if good else 1
    print('selftest: %d failure(s)' % fails)
    return 0 if fails == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture'); ap.add_argument('--frame', type=int)
    ap.add_argument('--families', default='0x8,0x2,0x4', help='comma list of family masks, one proof each (0x8 wrestlers, 0x2 ring, 0x4 props)')
    ap.add_argument('--game', default=None); ap.add_argument('--size', default='960x540')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's105', 'C3', 'proof')); ap.add_argument('--name', default=None)
    ap.add_argument('--rig', default=os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'))
    ap.add_argument('--link', action='append', default=None, metavar='ID=CH:AT', help='TOLD wrestler models (default for brock_losswin: actor:slot:0=343:0, as frame_rig.py DEFAULT_LINKS)'); ap.add_argument('--look', nargs=3, metavar=('EYE', 'AIM', 'FOV'), help='our own game-space camera (x,y,z x,y,z degrees; up = -y), checked against the capture camera handedness'); ap.add_argument('--no-hooks', action='store_true'); ap.add_argument('--extra', default='', help='extra frame_rig arguments (space separated)')
    ap.add_argument('--check', action='store_true'); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    main_root = os.environ.get('RDR_MAIN_CHECKOUT')
    if not main_root:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main_root = os.path.normpath(os.path.dirname(common)) if common else ROOT
    game = a.game or os.path.join(main_root, 'game_data', 'extracted')
    W, H = (int(v) for v in a.size.split('x'))
    if not os.path.isabs(a.capture) and not os.path.isdir(a.capture):
        a.capture = os.path.join(main_root, a.capture)
    M, vp, eye, why = scp.camera_from_capture(a.capture, a.frame)
    if a.look:
        L = scp.look_camera([float(x) for x in a.look[0].split(',')], [float(x) for x in a.look[1].split(',')], float(a.look[2]), W / float(H))
        if scp.handedness(L) != scp.handedness(M):
            raise SystemExit('family_cull_proof: the --look camera is mirrored against the capture camera: refused')
        M, vp, eye, why = L, [float(x) for x in L.T.reshape(-1)], [float(x) for x in a.look[0].split(',')], 'look ' + ' '.join(a.look)
    rsc = os.path.join(a.capture, 'scene', 'frame_%06d.rsc' % a.frame)
    name = a.name or '%s_f%d%s' % (os.path.basename(os.path.normpath(a.capture)), a.frame, '_look' if a.look else '')
    os.makedirs(a.out, exist_ok=True)
    base = ['--record', rsc, '--game', game, '--size', '%dx%d' % (W, H), '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)] + (['--no-hooks'] if a.no_hooks else []) + [x for x in a.extra.split() if x]
    links = a.link if a.link is not None else (['actor:slot:0=343:0'] if os.path.basename(os.path.normpath(a.capture)).startswith('brock_losswin') else [])
    base += [x for l in links for x in ('--link', l)]
    if os.path.basename(os.path.normpath(a.capture)).startswith('brock_losswin'): base += ['--body-scales', 'actor:slot:0=1.010498']   # frame_rig.py DEFAULT_SCALES (measured from the stream)
    arms = {'off': {'WWE_RDR_FAMILY_CULL': '0'}, 'off2': {'WWE_RDR_FAMILY_CULL': '0'}, 'on': {}, 'plant': {'WWE_RDR_PLANT_FAMILY_CULL_FLIP': '1'}}
    allok, summary = True, {}
    for mask in [int(x, 0) for x in a.families.split(',')]:
        fname = FAMILY_NAMES.get(mask, '0x%x' % mask)
        rgb, depth = {}, {}
        for arm, env in arms.items():
            png, dpath = run_arm(a.rig, base, os.path.join(a.out, '%s_%s_%s' % (name, fname.replace('+', '_'), arm)), mask, env)
            rgb[arm] = scp.png_rgb(png)
            d, _ = scp.depth_values(open(dpath, 'rb').read(), W * H)
            depth[arm] = d.reshape(H, W)
        res = analyse(rgb, depth)
        ok, lines = judge(res)
        res['verdict'] = lines
        off_cov = depth['off'] < 0.999999
        sheet(rgb, off_cov & (depth['on'] >= 0.999999), off_cov & (depth['plant'] >= 0.999999), os.path.join(a.out, '%s_%s_proof_sheet.png' % (name, fname.replace('+', '_'))), '%s %s' % (name, fname))
        print('%s %s (%s, %dx%d): covered %d | on: nearer %.5f bad %.5f changed %.4f | plant: nearer %.5f bad %.5f changed %.4f | noise %.5f | %s' % (
            name, fname, why[:40], W, H, res['covered'], res['on']['nearer_share'], res['on']['bad_share'], res['on']['changed_share'],
            res['plant']['nearer_share'], res['plant']['bad_share'], res['plant']['changed_share'], res['noise']['changed_share'], ' ; '.join(lines)))
        summary[fname] = res
        allok = allok and ok
    json.dump(summary, open(os.path.join(a.out, name + '_family_proof.json'), 'w'), indent=1)
    return 0 if (allok or not a.check) else 1


if __name__ == '__main__':
    sys.exit(main())

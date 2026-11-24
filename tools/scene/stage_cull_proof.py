#!/usr/bin/env python3
r"""SCU: the three-arm proof of the stage-mesh cull (docs\research\harness\SCU_contract.md "Proof" 1). 0 game runs: the offline rig only.

  py -I tools\scene\stage_cull_proof.py --capture C:\...\out\captures\a04_match --frame 40 [--size 960x540] [--out DIR] [--check]
  py -I tools\scene\stage_cull_proof.py --record R.rsc --camera 16floats --eye x,y,z [...]      (a record with a told camera: arena_rec's beauty records)
  py -I tools\scene\stage_cull_proof.py --selftest                                              (the arithmetic on synthetic depth pictures)

What it answers: does the game's cull rule (record word +0x8C bit 0x2 = back faces culled; the `r` copy's winding swapped, sub_82C04DF0 0x82C0A880 / 0x82C0B920) keep the
right side of every stage mesh in OUR renderer? Three one-shot frame_rig draws of the same record and camera, the arena family alone (--families 0x1, --no-hooks), each with its scene depth
(frame_rig --depth-out, FrontEnd::ReadDepthForTest):
  off   = WWE_RDR_STAGE_CULL=0 (the switch off: every stage mesh two-sided, the draw before SCU)
  on    = the default (the fix)
  plant = WWE_RDR_PLANT_STAGE_CULL_NO_FLIP=1 (cull on, the `r` copy WITHOUT the winding swap: the planted wrong side)
Per pixel (game-space world x of the point the OFF arm shows, from its depth and the camera: clip = M [p 1], M = the camera's rows; x < 0 and x > 0 are the two halves the `r` mirror x -> -x pairs):
  nearer  = pixels where an arm's depth is NEARER than off's (culling only removes faces: must be 0, up to the depth format's rounding)
  changed = pixels whose colour differs from off (any channel by more than 2)
  lost    = pixels off covers (depth < the clear depth) that the arm leaves at the clear depth (a hole: the stand's back wall seen through, the clear colour)
Shares are of the half's covered pixels in the off arm. The check (--check): the thresholds in THRESHOLDS below, set from the first runs (their source is written beside each).
Writes OUT\<name>_{off,on,plant}.png (+ .depth raw), OUT\<name>_proof.json, OUT\<name>_proof.txt and OUT\<name>_proof_sheet.png (off | on | plant | where on / plant changed: red = lost,
yellow = changed, a white line at the x = 0 plane). All scratch under out\ (pictures of game assets are never committed).
"""
import argparse, json, os, struct, subprocess, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# Set from the first runs (2026-11-20, captures a04_match and a00_match frame 40, the wide symmetric --look "0,-1100,-2400" "0,-300,600" 75, 960x540; docs\research\harness\SCU_proof.md):
#   on:    changed x<0 / x>0 = 0.0028 / 0.0031 (a4), 0.0021 / 0.0009 (a0); lost 0 / 0; nearer 0
#   plant: changed on the mirrored half (x > 0) = 0.2034 (a4), 0.4543 (a0); lost there 0.0355 / 0.2383; the other half 0.0261 / 0.0251
THRESHOLDS = {
    'nearer_max': 0.0005,          # share of covered pixels an arm may show nearer than off (measured 0: culling only removes faces)
    'on_lost_max': 0.02,           # cull on: lost share per half (measured 0.0000)
    'on_balance_max': 4.0,         # cull on: the larger half's changed share / the smaller's (measured 1.1 and 2.3); only when both are >= on_balance_floor
    'on_balance_floor': 0.002,
    'plant_changed_min': 0.10,     # the plant: the mirrored half changes at least this share (measured 0.20 and 0.45) ...
    'plant_changed_ratio_min': 10.0,  # ... and at least this many times the cull-on arm there (measured 66x and 500x)
}


def depth_values(raw, n):
    """raw 4-byte texels -> float depth in 0..1. D32F: the clear value 1.0 = 0x3F800000; X8_D24: the low 24 bits (1.0 = 0xFFFFFF)."""
    u = np.frombuffer(raw, dtype='<u4', count=n)
    exp = (u >> 23) & 0x1FF
    if np.mean((exp >= 0x60) & (exp <= 0x7F)) > 0.5:   # most texels read as positive floats <= 1.0 (an X8_D24 texel's top byte is 0 / undefined, not a float exponent)
        return u.view('<f4').astype(np.float64), 'D32F'
    return (u & 0xFFFFFF).astype(np.float64) / float(0xFFFFFF), 'X8D24'


def world_x(depth, M, W, H):
    """game-space x of each pixel's point: clip = M [p 1] (M = rows c56..c59), ndc x right, y up (row 0 = top), z = depth. NaN where the depth is the clear depth."""
    ys, xs = np.mgrid[0:H, 0:W]
    nx = (xs + 0.5) / W * 2.0 - 1.0
    ny = 1.0 - (ys + 0.5) / H * 2.0
    Mi = np.linalg.inv(M)
    v = np.stack([nx, ny, depth, np.ones_like(depth)], axis=-1) @ Mi.T
    x = v[..., 0] / v[..., 3]
    x[depth >= 0.999999] = np.nan
    return x


def analyse(rgb, depth, wx, clear_depth=0.999999):
    """rgb / depth: dict arm -> HxWx3 uint8 / HxW float; wx: HxW world x of off's point (NaN = not covered). -> dict of the per-arm, per-half numbers."""
    off_cov = depth['off'] < clear_depth
    halves = {'neg': off_cov & (wx < 0), 'pos': off_cov & (wx > 0)}
    out = {'covered': {h: int(m.sum()) for h, m in halves.items()}}
    for arm in ('on', 'plant'):
        if arm not in depth:
            continue
        diff = np.abs(rgb[arm].astype(np.int16) - rgb['off'].astype(np.int16)).max(axis=-1) > 2
        lost = off_cov & (depth[arm] >= clear_depth)
        nearer = (depth[arm] < depth['off'] - 1e-6) & (depth['off'] < 1.0 + 1e-9)
        a = {'nearer': int(nearer.sum()), 'nearer_share': float(nearer.sum()) / max(1, int(off_cov.sum()))}
        for h, m in halves.items():
            c = max(1, int(m.sum()))
            a['changed_' + h] = float((diff & m).sum()) / c
            a['lost_' + h] = float((lost & m).sum()) / c
        a['changed_px'] = int(diff.sum())
        out[arm] = a
    return out


def judge(res, th=THRESHOLDS, mirrored_half=None):
    """-> (ok, lines). mirrored_half: 'neg' / 'pos' = the half the plant must hurt (default: the half the plant loses most in)."""
    lines, ok = [], True
    on, pl = res.get('on'), res.get('plant')
    for arm, a in (('on', on), ('plant', pl)):
        if a is None:
            continue
        if a['nearer_share'] > th['nearer_max']:
            ok = False
            lines.append('FAIL %s: %d pixels (%.4f) nearer than the cull-off arm (culling can only remove faces)' % (arm, a['nearer'], a['nearer_share']))
    if on is not None:
        for h in ('neg', 'pos'):
            if on['lost_' + h] > th['on_lost_max']:
                ok = False
                lines.append('FAIL on: half x %s loses %.4f of its covered pixels (a surface vanished: the cull side?)' % ('<0' if h == 'neg' else '>0', on['lost_' + h]))
        a, b = on['changed_neg'], on['changed_pos']
        if min(a, b) >= th['on_balance_floor'] and max(a, b) / min(a, b) > th['on_balance_max']:
            ok = False
            lines.append('FAIL on: the halves change unequally (%.4f vs %.4f)' % (a, b))
        if max(a, b) >= th['on_balance_floor'] and min(a, b) < th['on_balance_floor'] and max(a, b) > 4 * th['on_balance_floor']:
            ok = False
            lines.append('FAIL on: only one half changes (%.4f vs %.4f)' % (a, b))
    if pl is not None and on is not None:
        h = mirrored_half or ('neg' if pl['changed_neg'] >= pl['changed_pos'] else 'pos')
        if pl['changed_' + h] < th['plant_changed_min'] or pl['changed_' + h] < th['plant_changed_ratio_min'] * max(on['changed_' + h], 1e-6):
            ok = False
            lines.append('FAIL plant: the mirrored half (x %s) changes only %.4f (cull on %.4f): the proof cannot see a wrong side' % ('<0' if h == 'neg' else '>0', pl['changed_' + h], on['changed_' + h]))
        else:
            lines.append('PASS plant: the mirrored half (x %s) changes %.4f (lost %.4f) against %.4f with the swap (planted wrong side seen)' % ('<0' if h == 'neg' else '>0', pl['changed_' + h], pl['lost_' + h], on['changed_' + h]))
    if ok:
        lines.append('PASS')
    return ok, lines


def png_rgb(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB'))


def run_arm(rig, base_cmd, prefix, env_add, timeout=600):
    env = dict(os.environ)
    for k in ('WWE_RDR_STAGE_CULL', 'WWE_RDR_PLANT_STAGE_CULL_NO_FLIP'):
        env.pop(k, None)
    env.update(env_add)
    cmd = [rig] + base_cmd + ['--prefix', prefix, '--depth-out', prefix + '.depth']
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, env=env)
    if p.returncode != 0:
        raise RuntimeError('frame_rig failed (%d): %s' % (p.returncode, (p.stdout + p.stderr)[-600:]))
    return prefix + '_mask1.png', prefix + '.depth'


def sheet(rgb, lost_on, lost_pl, chg_on, chg_pl, zero_line, path, labels):
    from PIL import Image, ImageDraw
    H, W = rgb['off'].shape[:2]
    def mark(base, lost, chg):
        im = (base.astype(np.float32) * 0.35).astype(np.uint8)
        im[chg] = (255, 220, 0)
        im[lost] = (255, 40, 40)
        im[zero_line] = (255, 255, 255)
        return im
    tiles = [rgb['off'], rgb['on'], rgb['plant'], mark(rgb['on'], lost_on, chg_on), mark(rgb['plant'], lost_pl, chg_pl)]
    out = Image.new('RGB', (W * 3, H * 2 + 2 * 24), (20, 20, 20))
    pos = [(0, 0), (W, 0), (2 * W, 0), (W, 1), (2 * W, 1)]
    d = ImageDraw.Draw(out)
    for t, (x, row), lab in zip(tiles, pos, labels):
        y = row * (H + 24)
        out.paste(Image.fromarray(t), (x, y + 24))
        d.text((x + 4, y + 4), lab, fill=(255, 255, 255))
    out.save(path)


def camera_from_capture(capture, frame):
    """The capture's own game camera: the ViewProj (rows c56..c59) of the first arena colour draw at the BASE stage site only (OFFLINE ONLY). frame_rig.py _bank_camera also takes the
    mirrored-copy site (0x82C0B94C), whose ViewProj has every register's x negated (draw_submit.md lane 39): a mirrored camera, under which every culled face is the other one."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'shadow'))
    import shadow_census as sc
    cap = sc.Cap(capture)
    f = cap.frame(frame)
    d = f.draws
    ps = f.origins['pass_site'].astype(np.int64)
    eff = cap.effs(d['vs_hash'])
    for i in np.nonzero(ps == sc.SITE_COLOUR)[0]:
        e = str(eff[i])
        if not e.startswith('yBG_'):
            continue
        r = sc.FXR.get((e.split('/')[0], 'vertex', 'ViewProj'))
        if r is None:
            continue
        M = sc.regs(f.bank(int(d[i]['vs_bank'])), r, 4).astype(np.float64)
        eye = np.linalg.solve(M[[0, 1, 3], :3], -M[[0, 1, 3], 3])
        return M, [float(x) for x in M.T.reshape(-1)], [float(x) for x in eye], 'ViewProj of the stream bank of draw %d (%s, the base site)' % (int(i), e)
    raise RuntimeError('no arena colour draw (yBG_*) at the base site with a ViewProj in frame %d' % frame)


def look_camera(eye, aim, fov_deg, aspect, n=10.0, f=300000.0):
    """A game-space camera built like the game's (camera.md: view space +x right, +y DOWN, +z forward; the world's up is -y) with record_frame.cpp's D3D projection
    (near 10, far 3e5, negative y scale): clip = M [p 1]. Its handedness is checked against the capture's own camera (handedness())."""
    eye, aim = np.asarray(eye, np.float64), np.asarray(aim, np.float64)
    fwd = aim - eye; fwd /= np.linalg.norm(fwd)
    right = np.cross([0.0, 1.0, 0.0], fwd); right /= np.linalg.norm(right)
    down = np.cross(fwd, right)
    ys = 1.0 / np.tan(np.radians(fov_deg) / 2); xs = ys / aspect
    A, B = f / (f - n), -n * f / (f - n)
    M = np.zeros((4, 4))
    M[0, :3], M[0, 3] = xs * right, -xs * right.dot(eye)
    M[1, :3], M[1, 3] = -ys * down, ys * down.dot(eye)
    M[3, :3], M[3, 3] = fwd, -fwd.dot(eye)
    M[2] = A * M[3]; M[2, 3] += B
    return M


def handedness(M):
    """the sign of the 3x3 determinant of the clip x / y / w rows: equal for two cameras of the same handedness (a mirrored camera flips it)"""
    return float(np.sign(np.linalg.det(M[[0, 1, 3], :3])))


def d2_blocks(path):
    """the d2 lines of a WWE_RDR_FEATURE_DUMP file, one list per Record that wrote any (a seed frame's Record writes none)"""
    blocks, cur = [], None
    for ln in open(path, encoding='utf-8', errors='replace'):
        if '"name":"frame_begin"' in ln:
            cur = []
            blocks.append(cur)
        elif '"k":"d2"' in ln and cur is not None:
            try:
                cur.append(json.loads(ln))
            except ValueError:
                pass
    return [b for b in blocks if b]


def flip_tally(block):
    """-> (stage meshes with the record's cull bit drawn culled, drawn two-sided, mirrored copies with front face 0)"""
    culled = two = ff0 = 0
    for d in block:
        if d.get('slot', -1) < 0 or d.get('slot', -1) >= 400 or not (d.get('cword', 0) & 2):
            continue
        if d.get('cull', 0) > 0:
            culled += 1
        else:
            two += 1
        ff0 += 1 if (d.get('mir') and d.get('ff') == 0) else 0
    return culled, two, ff0


def flip_check(rig, rsc, game, vp, eye, W, H, prefix):
    """SCU (review Q2): the switch is read once per Record and the DrawState is built in every emit (front_end.cpp: op.state = fb.State(state) per draw; the kept draw lists keep
    only the constant blocks). One process, frame 1 with the cull on, frame 2 with it off (frame_rig WWE_RDR_STAGE_CULL_FLIP flips the stand-in before each --repeat frame, kept
    lists on): frame 2's d2 lines must show every bit-set stage mesh two-sided, frame 1's culled."""
    dump = prefix + '_dump.jsonl'
    if os.path.exists(dump):
        os.remove(dump)
    env = dict(os.environ)
    for k in ('WWE_RDR_STAGE_CULL', 'WWE_RDR_PLANT_STAGE_CULL_NO_FLIP'):
        env.pop(k, None)
    env.update({'WWE_RDR_STAGE_CULL_FLIP': '1', 'WWE_RDR_FEATURE_DUMP': dump})
    cmd = [rig, '--record', rsc, '--game', game, '--size', '%dx%d' % (W, H), '--families', '0x1', '--no-hooks', '--repeat', '2', '--prefix', prefix,
           '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)]
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600, env=env)
    if p.returncode != 0:
        print('FAIL flip-check: frame_rig exit %d: %s' % (p.returncode, (p.stdout + p.stderr)[-400:]))
        return 1
    blocks = d2_blocks(dump)
    if len(blocks) < 2:
        print('FAIL flip-check: %d Records wrote d2 lines (2 expected)' % len(blocks))
        return 1
    a, b = flip_tally(blocks[0]), flip_tally(blocks[1])
    print('flip-check frame 1 (cull on): bit-set stage meshes culled %d two-sided %d, r copies front face 0: %d' % a)
    print('flip-check frame 2 (flipped off): culled %d two-sided %d, r copies front face 0: %d' % b)
    ok = a[0] > 0 and a[1] == 0 and b[0] == 0 and b[1] == a[0] and b[2] == 0
    print('PASS flip-check: the flip takes effect on the next frame' if ok else 'FAIL flip-check: the second frame did not draw the other cull')
    return 0 if ok else 1


# entry: judge (through analyse, as main() calls them; and look_camera / handedness, as main() checks a --look camera)
def selftest():
    """the proof's arithmetic on synthetic pictures: the right side, the planted wrong side, a vanished half, a nearer pixel, a mirrored camera's handedness"""
    H, W = 40, 80
    wx = np.tile(np.linspace(-1, 1, W), (H, 1))
    d_off = np.full((H, W), 0.5)
    d_off[:5] = 1.0                               # sky
    rgb_off = np.full((H, W, 3), 100, np.uint8)
    rgb_off[:5] = 30
    wx[:5] = np.nan
    def arm(lost_neg=0, lost_pos=0, chg=0.01, nearer=0):
        d = d_off.copy(); c = rgb_off.copy()
        rows_neg = int(round(lost_neg * 35)); rows_pos = int(round(lost_pos * 35))
        d[5:5 + rows_neg, :W // 2] = 1.0; c[5:5 + rows_neg, :W // 2] = 30
        d[5:5 + rows_pos, W // 2:] = 1.0; c[5:5 + rows_pos, W // 2:] = 30
        k = int(round(chg * 35))
        c[H - k:, :] = 140; d[H - k:, :] = 0.6     # a farther surface shows: changed, not lost
        if nearer:
            d[H - 1, :10] = 0.1   # 10 of 2,800 covered pixels nearer: above nearer_max
        return c, d
    fails = 0
    def case(name, on, pl, want_ok, want_token):
        nonlocal fails
        rgb = {'off': rgb_off, 'on': on[0], 'plant': pl[0]}
        dep = {'off': d_off, 'on': on[1], 'plant': pl[1]}
        ok, lines = judge(analyse(rgb, dep, wx))
        good = ok == want_ok and (want_token is None or any(want_token in l for l in lines))
        print('%s selftest %s: ok=%s %s' % ('PASS' if good else 'FAIL', name, ok, ' | '.join(lines)))
        fails += 0 if good else 1
    case('right side', arm(chg=0.06), arm(lost_neg=0.5, chg=0.06), True, 'PASS plant')
    case('planted wrong side never seen', arm(chg=0.06), arm(chg=0.06), False, 'cannot see a wrong side')
    case('cull side wrong (the on arm loses a half)', arm(lost_neg=0.4, chg=0.06), arm(lost_neg=0.5, chg=0.06), False, 'a surface vanished')
    case('a nearer pixel', arm(chg=0.06, nearer=1), arm(lost_neg=0.5, chg=0.06), False, 'nearer than')
    L = look_camera([0, -1100, -2400], [0, -300, 600], 75, 16 / 9.)
    Mx = L.copy(); Mx[0] = -Mx[0]   # the mirrored-copy site's camera: every register's x negated (draw_submit.md lane 39)
    good = handedness(L) != handedness(Mx) and abs(handedness(L)) == 1
    print('%s selftest mirrored camera has the other handedness: %+d vs %+d' % ('PASS' if good else 'FAIL', handedness(L), handedness(Mx)))
    fails += 0 if good else 1
    print('selftest: %d failure(s)' % fails)
    return 0 if fails == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--capture'); ap.add_argument('--frame', type=int)
    ap.add_argument('--record'); ap.add_argument('--camera'); ap.add_argument('--eye')
    ap.add_argument('--game', default=None); ap.add_argument('--size', default='960x540')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'scu', 'proof')); ap.add_argument('--name', default=None)
    ap.add_argument('--rig', default=os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'))
    ap.add_argument('--extra', default='', help='extra frame_rig arguments (space separated), e.g. --look-entrance')
    ap.add_argument('--look', nargs=3, metavar=('EYE', 'AIM', 'FOV'), help='with --capture: draw the capture record through our own game-space camera (x,y,z x,y,z degrees; game space, up = -y)')
    ap.add_argument('--flip-check', action='store_true', help='one rig process, two consecutive frames of the record with the switch flipped between them (kept draw lists on): the second frame must draw the other cull')
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
            a.capture = os.path.join(main_root, a.capture)   # captures live in the MAIN checkout's out\ (a check in a worktree names them relative)
        M, vp, eye, why = camera_from_capture(a.capture, a.frame)
        rsc = os.path.join(a.capture, 'scene', 'frame_%06d.rsc' % a.frame)
        name = a.name or '%s_f%d' % (os.path.basename(os.path.normpath(a.capture)), a.frame)
        if a.look:   # a wide view of BOTH halves on the capture's record: our camera, checked to have the capture camera's handedness (a mirrored camera would swap every culled face)
            e3, t3 = [float(x) for x in a.look[0].split(',')], [float(x) for x in a.look[1].split(',')]
            L = look_camera(e3, t3, float(a.look[2]), W / float(H))
            if handedness(L) != handedness(M):
                raise SystemExit('stage_cull_proof: the --look camera is mirrored against the capture camera (handedness %+d vs %+d): refused' % (handedness(L), handedness(M)))
            why = 'look eye %s aim %s fov %s (handedness %+d = the capture camera\'s, %s)' % (a.look[0], a.look[1], a.look[2], handedness(L), why)
            M, vp, eye = L, [float(x) for x in L.T.reshape(-1)], e3
            name += '_wide'
    else:
        vp = [float(x) for x in a.camera.split(',')]; eye = [float(x) for x in a.eye.split(',')]
        M = np.array(vp, dtype=np.float64).reshape(4, 4).T   # the rig takes the row-vector form; M = its transpose (clip = M [p 1])
        rsc, why = a.record, 'the told camera'
        name = a.name or os.path.splitext(os.path.basename(a.record))[0]
    os.makedirs(a.out, exist_ok=True)
    if a.flip_check:
        return flip_check(a.rig, rsc, game, vp, eye, W, H, os.path.join(a.out, name + '_flip'))
    base = ['--record', rsc, '--game', game, '--size', '%dx%d' % (W, H), '--families', '0x1', '--no-hooks',
            '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)] + [x for x in a.extra.split() if x]
    arms = {'off': {'WWE_RDR_STAGE_CULL': '0'}, 'on': {}, 'plant': {'WWE_RDR_PLANT_STAGE_CULL_NO_FLIP': '1'}}
    rgb, depth, fmt = {}, {}, None
    for arm, env in arms.items():
        png, dpath = run_arm(a.rig, base, os.path.join(a.out, '%s_%s' % (name, arm)), env)
        rgb[arm] = png_rgb(png)
        raw = open(dpath, 'rb').read()
        depth[arm], fmt = depth_values(raw, W * H)
        depth[arm] = depth[arm].reshape(H, W)
    wx = world_x(depth['off'], M, W, H)
    res = analyse(rgb, depth, wx)
    res['camera'] = why; res['depth_format'] = fmt; res['record'] = rsc; res['size'] = [W, H]
    ok, lines = judge(res)
    res['verdict'] = lines
    off_cov = depth['off'] < 0.999999
    lost_on = off_cov & (depth['on'] >= 0.999999); lost_pl = off_cov & (depth['plant'] >= 0.999999)
    chg_on = np.abs(rgb['on'].astype(np.int16) - rgb['off'].astype(np.int16)).max(axis=-1) > 2
    chg_pl = np.abs(rgb['plant'].astype(np.int16) - rgb['off'].astype(np.int16)).max(axis=-1) > 2
    sgn = np.sign(np.nan_to_num(wx, nan=0.0))
    zero_line = np.zeros_like(off_cov)
    zero_line[:, 1:] |= (sgn[:, 1:] * sgn[:, :-1]) < 0
    zero_line[1:, :] |= (sgn[1:, :] * sgn[:-1, :]) < 0
    sheet(rgb, lost_on, lost_pl, chg_on, chg_pl, zero_line, os.path.join(a.out, name + '_proof_sheet.png'),
          ['%s: cull OFF (two-sided, before)' % name, 'cull ON (the fix)', 'PLANT: cull on, no r-copy swap',
           'ON vs OFF: red lost %.3f / %.3f, yellow changed %.3f / %.3f (x<0 / x>0)' % (res['on']['lost_neg'], res['on']['lost_pos'], res['on']['changed_neg'], res['on']['changed_pos']),
           'PLANT vs OFF: red lost %.3f / %.3f (x<0 / x>0)' % (res['plant']['lost_neg'], res['plant']['lost_pos'])])
    json.dump(res, open(os.path.join(a.out, name + '_proof.json'), 'w'), indent=1)
    txt = ['stage_cull_proof %s (%s, depth %s, %dx%d)' % (name, why, fmt, W, H), 'covered off: x<0 %d px, x>0 %d px' % (res['covered']['neg'], res['covered']['pos'])]
    for arm in ('on', 'plant'):
        r = res[arm]
        txt.append('%-5s changed x<0 %.4f x>0 %.4f | lost x<0 %.4f x>0 %.4f | nearer %d (%.5f)' % (arm, r['changed_neg'], r['changed_pos'], r['lost_neg'], r['lost_pos'], r['nearer'], r['nearer_share']))
    txt += lines
    open(os.path.join(a.out, name + '_proof.txt'), 'w').write('\n'.join(txt) + '\n')
    print('\n'.join(txt))
    return 0 if (ok or not a.check) else 1


if __name__ == '__main__':
    sys.exit(main())

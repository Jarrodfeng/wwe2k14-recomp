#!/usr/bin/env python3
r"""Session 105 lane A1's proof: the launcher's 1440p / 4K / 360p-648p choice draws the 3D scene at that size (stream-free player build). Offline, headless, 0 game runs.

  py -I tools\scene\a1_check.py --capture C:\...\out\captures\brock_losswin [--frame 110] [--size 640x360] [--out DIR] [--plant unscaled|dconv|ghost] [--expect-fail TOKEN]

What it answers: out\tools\post_rig.exe (--scale S: the renderer's render scale; the scene targets carry kUsageScaled exactly as the stream-free host sets them, FrontEnd::SetScaledTargets) draws ONE captured
scene record (camera = the stream's own, as post_rig.py / pst_check.py take it) with a match's post row, at scale 1, 2 and 0.5, and checks:
  SIZES     the scene colour, depth and the post chain's game-depth image (dconv) are really S x the logical size and carry kUsageScaled; the picture read back is that size
  SCREEN    only screen images are scaled: every live image the rig made (the scene's four, the post chain, the maps, the crowd, ...) with kUsageScaled has a size derived from the logical size by
            ceil(S * logical / {1, 4, 8}); a map / crowd / luminance image that was scaled would show a size from none of those
  DETAIL    the scale-2 scene is DRAWN at that size, not the 1x picture enlarged: its gradient energy (mean |dx|) is above the 1x picture enlarged to the same size (ratio > 1.10)
  DEPTH     the DoF / haze read the whole depth: the depth-of-field-only effect map (post minus the same post with DoF off, --a1-no-dof) at scale 2, reduced to the 1x grid, correlates with the 1x one (>= 0.5); dconv left unscaled reads
            the top-left quarter of the depth (correlation 0.09) and the size check names it
  GHOST     the afterimage lands where it does at 1x at every scale (post_rig --ghost alpha: first frame unchanged, nothing outside the sprite's rectangle changes, something inside does)
  CLEAN     the rig exits 0 with validation_errors 0 at every scale (0.5 is the phone LOW row)
  EDGE      (review F1) at scale 0.6667 the post picture's last column is drawn (the old float viewport ended at 853.33 of an 854 wide image)
  ALLOC     (review F2) a scaled target that cannot be made (post_rig --a1-plant alloc) falls back to unscaled targets, logged once, never a black 3D
  HOST      (review F3) the real SceneHost (host_rig.exe, KEEP_STREAM=0): F's real size, the out -> F copy and the [scene][targets] line (twin: host_rig --a1-plant unscaled -> "FAIL  HOST   scale 2")
Planted twins (each must FAIL; --expect-fail TOKEN exits 0 only when the run failed and printed TOKEN), applied to the scale-2 run:
  unscaled  the scene targets left at the logical size (the bug: the choice does nothing)           -> "FAIL  SIZES  scale 2: scene colour"
  dconv     the game-depth image left unscaled (the DoF / haze read a quarter of the depth)          -> "FAIL  DOFMAP" (the picture proof; the size line fails too)
  ghost     the afterimage shader divides by the logical size (the old divide)                        -> "FAIL  GHOST  scale 2"
Needs the main checkout's capture + game_data\extracted (read-only, RDR_MAIN_CHECKOUT) and the fview exe: the same inputs as pst_check.py.
"""
import argparse, math, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig as fr  # noqa: E402
import post_rig as pr   # noqa: E402  (make_row: the words of S that give a match's combine constants)

# a match's combine constants (as pst_check.py: post_pass.cpp DefaultPostParams, measured: run 4511). dof + linear depth = the depth-convert pass runs ("the match keeps the pass").
MATCH = dict(variant='yGlareEtc_dof_fog', DOFParam=[91.33, 338.67, 0.992, 0.8], DOFParam2=[0.991855, 0.99509, 0.0, 0.0], RGB=[0.4, 0.55, 0.55, 1.0], depth_linear=True, down_w=0.0003)
CLEAR_RGB = (31, 36, 46)   # the front-end's clear colour (0.12, 0.14, 0.18)
F1_MAX_ROWS = 0.10     # review F1: the share of rows whose last post column is black where the column before is not (the old code: ~all rows)
DETAIL_MIN = 1.10      # the drawn-at-2x scene's mean |dx| over the 1x picture's enlarged (measured on brock_losswin frame 110; see the sheet)
DOF_CORR_MIN = 0.5      # correlation of the DoF-only effect map at 2x (reduced to 1x) with the 1x one: a correct scale-2 draw gives 0.73 / 0.79 / 0.80 on brock_losswin frames 20 / 110 / 200 (a real 2x draw differs a little from a 1x one); dconv left unscaled gives 0.09
DEPTH_MAX = 3.0        # mean |diff| (levels, RGB) between the 2x post picture reduced to 1x and the 1x post picture


def ceil_f(v):
    return int(math.ceil(np.float32(v)))


def run_rig(rig, game, rsc, work, tag, size, scale, plant, vp, eye, rowf, extra=()):
    cmd = [rig, '--record', rsc, '--game', game, '--size', size, '--prefix', os.path.join(work, tag), '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye), '--row', rowf,
           '--ghost', 'alpha', '--scale', repr(scale)]
    if plant:
        cmd += ['--a1-plant', plant]
    cmd += list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=dict(os.environ, WWE_RDR_VALIDATION='1'), timeout=1800)
    text = (p.stdout or '') + (p.stderr or '')
    return p.returncode, text


def sizes_of(text):
    m = re.search(r'a1-sizes: scale (\S+) logical (\d+)x(\d+) picture (\d+)x(\d+) colour (\d+)x(\d+) scaled (\d) depth (\d+)x(\d+) scaled (\d) dconv (\d+)x(\d+) scaled (\d)', text)
    return None if not m else dict(scale=float(m.group(1)), logical=(int(m.group(2)), int(m.group(3))), picture=(int(m.group(4)), int(m.group(5))), colour=(int(m.group(6)), int(m.group(7)), int(m.group(8))),
                                   depth=(int(m.group(9)), int(m.group(10)), int(m.group(11))), dconv=(int(m.group(12)), int(m.group(13)), int(m.group(14))))


def images_of(text):
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in re.finditer(r'a1-image: id \d+ (\d+)x(\d+) format \d+ scaled (\d)', text)]


def png(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)


def grad_x(a):
    g = a.mean(axis=2)
    return float(np.abs(np.diff(g, axis=1)).mean())


def arm_f1(args, say, rig, game, rsc, work, vp, eye, rowf, capture):
    """Review F1: at scale 0.6666667 (the 480p row) the scene image is ceil(1280 * s) = 854 wide while the viewport used to be 1280 * s = 853.33: column 853 was never drawn (the clear colour: a dark line at the right edge). The last column must hold the scene, as the one before it."""
    rc, text = run_rig(rig, game, rsc, work, 's067', '1280x720', 0.6666667, '', vp, eye, rowf)
    say(rc == 0, 'CLEAN  scale 0.6667 at 1280x720: rig exit %d' % rc, 'rig failed at scale 0.6667')
    sz = sizes_of(text)
    say(sz is not None and sz['colour'] == (854, 480, 1) and sz['picture'] == (854, 480), 'SIZES  scale 0.6667: scene colour %s, picture %s (want 854x480, scaled=1)' % (None if sz is None else sz['colour'], None if sz is None else sz['picture']), 'scale 0.6667 sizes')
    try:
        a = png(os.path.join(work, 's067_post.png'))   # the post chain's combine pass writes the last column only when its viewport reaches it (the scene pass alone happened to cover it)
        edge, prev = a[:, -1, :].max(axis=1) <= 0, a[:, -2, :].max(axis=1) > 8
        extra = float((edge & prev).mean())
        say(extra < F1_MAX_ROWS, 'EDGE   scale 0.6667: the last post column %d is black where column %d is not, on %.1f%% of the rows (want < %.1f%%)' % (a.shape[1] - 1, a.shape[1] - 2, 100 * extra, 100 * F1_MAX_ROWS), 'EDGE column 853 not drawn')
    except Exception as e:  # noqa: BLE001
        say(False, 'EDGE   picture could not be read: %s' % e, 'EDGE column 853 not drawn')


def arm_alloc(args, say, rig, game, rsc, work, vp, eye, rowf, capture):
    """Review F2: a scaled target that cannot be made (4K memory) must not leave the 3D black for the run: the targets fall back to unscaled ones (post_rig --a1-plant alloc fails the scaled depth)."""
    rc, text = run_rig(rig, game, rsc, work, 'alloc', args.size, 2.0, 'alloc', vp, eye, rowf)
    lw, lh = [int(x) for x in args.size.split('x')]
    sz = sizes_of(text)
    say(rc == 0 and sz is not None and sz['colour'] == (lw, lh, 0) and sz['depth'] == (lw, lh, 0) and sz['picture'] == (lw, lh), 'ALLOC  scale 2 with the scaled depth failing: rig exit %d, targets fell back to %s (want %dx%d scaled=0, a picture)' % (rc, None if sz is None else (sz['colour'], sz['picture']), lw, lh), 'ALLOC no fallback')
    say(text.count('falling back to unscaled targets') == 1, 'ALLOC  the fall back is logged once (%d line(s))' % text.count('falling back to unscaled targets'), 'ALLOC no fallback')
    try:
        a = png(os.path.join(work, 'alloc_post.png'))
        say(float(a.mean()) > 5.0, 'ALLOC  the fall-back picture is not black (mean %.1f)' % float(a.mean()), 'ALLOC no fallback')
    except Exception as e:  # noqa: BLE001
        say(False, 'ALLOC  picture could not be read: %s' % e, 'ALLOC no fallback')


def arm_host(args, say, rig, game, rsc, work, vp, eye, rowf, capture):
    """Review F3: the real SceneHost (host_rig.exe, built with KEEP_STREAM=0 like the game): the host's own wiring turns the scaled targets on from render_scale, the out -> F copy, F's real size and the [scene][targets] line."""
    host = os.path.join(ROOT, 'out', 'tools', 'host_rig.exe')
    if not os.path.exists(host):
        say(False, 'HOST   out\\tools\\host_rig.exe is not built (pwsh tools\\scene\\build_host_rig.ps1)', 'host_rig not built')
        return
    scene = os.path.join(capture, 'scene')
    res = {}
    for tag, scale, plant in (('h1', 1.0, ''), ('h2', 2.0, args.plant if args.plant == 'unscaled' else '')):
        od = os.path.join(work, tag)
        os.makedirs(od, exist_ok=True)
        cmd = [host, '--mode', 'host', '--scene', scene, '--game', game, '--out', od, '--frames', str(args.frame), '--size', '1280x720', '--scale', repr(scale)]
        if plant:
            cmd += ['--a1-plant', plant]
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1800)
        text = (p.stdout or '') + (p.stderr or '')
        res[tag] = (p.returncode, text, od)
        m = re.search(r'a1-host: scale (\S+) logical (\d+)x(\d+) F \(shown\) (\d+)x(\d+) present (\d+)x(\d+)', text)
        t = re.search(r'\[scene\]\[targets\] logical (\d+)x(\d+): colour (\d+)x(\d+) scaled=(\d), depth (\d+)x(\d+) scaled=(\d)', text)
        want = (1280, 720) if scale == 1.0 else (int(1280 * scale), int(720 * scale))
        say(p.returncode == 0, 'HOST   scale %g: host_rig exit %d' % (scale, p.returncode), 'host_rig failed at scale %g' % scale)
        say(m is not None and (int(m.group(4)), int(m.group(5))) == want, 'HOST   scale %g: F (through the host\'s out -> F copy) is %s (want %dx%d)' % (scale, None if m is None else '%sx%s' % (m.group(4), m.group(5)), want[0], want[1]), 'HOST F size')
        say(t is not None and (int(t.group(3)), int(t.group(4)), int(t.group(5))) == want + ((0 if scale == 1.0 else 1),) and (int(t.group(6)), int(t.group(7)), int(t.group(8))) == want + ((0 if scale == 1.0 else 1),),
            'HOST   scale %g: the host\'s [scene][targets] line: %s (want colour and depth %dx%d scaled=%d)' % (scale, None if t is None else t.group(0)[:120], want[0], want[1], 0 if scale == 1.0 else 1), 'HOST targets line')
    try:
        a1, a2 = png(os.path.join(res['h1'][2], 'host_%d.png' % args.frame)), png(os.path.join(res['h2'][2], 'host_%d.png' % args.frame))
        up = np.asarray(Image.fromarray(a1.astype(np.uint8)).resize((a2.shape[1], a2.shape[0]), Image.BILINEAR), dtype=np.float32)
        ratio = grad_x(a2) / max(grad_x(up), 1e-6)
        say(ratio >= DETAIL_MIN, 'HOST   scale 2 host picture is drawn at 2x, not the 1x enlarged: mean |dx| %.3f vs %.3f (ratio %.3f, want >= %.2f)' % (grad_x(a2), grad_x(up), ratio, DETAIL_MIN), 'HOST F size')
    except Exception as e:  # noqa: BLE001
        say(False, 'HOST   pictures could not be read: %s' % e, 'HOST F size')


def run_main(args):
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = os.path.abspath(args.capture)
    name = os.path.basename(capture.rstrip('\\/'))
    import run_dirs
    out = run_dirs.run_out('A1', args.out)
    work = os.path.join(out, 'work_' + name)
    os.makedirs(work, exist_ok=True)
    rig = os.path.join(ROOT, 'out', 'tools', 'post_rig.exe')
    os.environ.setdefault('WWE_FVIEW_ALLOW_STALE', '1')
    from fview_exe import find_fview
    fview = find_fview(root=ROOT)
    for need in (rig, game, capture):
        if not os.path.exists(need):
            print('FAIL  cannot read its input: ' + need)
            return 1
    n = args.frame
    rsc = os.path.join(capture, 'scene', 'frame_%06d.rsc' % n)
    cam, why = fr.stream_camera(fview, capture, n, game, work)
    if cam is None:
        print('FAIL  frame %d: no camera (%s)' % (n, why))
        return 1
    vp, eye = cam[0], cam[1]
    rowf = os.path.join(work, 'row_match.bin')
    open(rowf, 'wb').write(pr.make_row(MATCH))
    bad = []

    def say(ok, msg, token=None):
        print(('PASS  ' if ok else 'FAIL  ') + msg)
        if not ok:
            bad.append(token or msg)

    if args.only in ('f1', 'host', 'alloc'):
        {'f1': arm_f1, 'host': arm_host, 'alloc': arm_alloc}[args.only](args, say, rig, game, rsc, work, vp, eye, rowf, capture)
        print(('PASS' if not bad else 'FAIL') + '  a1_check (%s arm only): %d failing check(s)%s' % (args.only, len(bad), (': ' + '; '.join(sorted(set(bad)))) if bad else ''))
        return 0 if not bad else 1
    lw, lh = [int(x) for x in args.size.split('x')]
    runs = {}
    for tag, scale, plant in (('s1', 1.0, ''), ('s2', 2.0, args.plant), ('s05', 0.5, '')):
        rc, text = run_rig(rig, game, rsc, work, tag, args.size, scale, plant, vp, eye, rowf)
        ver = re.search(r'validation_errors (\d+)', text)
        runs[tag] = (rc, text)
        say(rc == 0 and ver is not None and int(ver.group(1)) == 0, 'CLEAN  scale %g%s: rig exit %d, validation errors %s' % (scale, (' plant ' + plant) if plant else '', rc, ver.group(1) if ver else '?'),
            'rig failed at scale %g' % scale)
        for line in text.splitlines():
            if line.startswith(('a1-sizes', 'ghost-test')) or '[scene][targets]' in line:
                print('  ' + line)
        if rc != 0:
            print('  rig output tail: ' + ' | '.join(text.splitlines()[-8:])[-1500:])
    for tag, scale in (('n1', 1.0), ('n2', 2.0)):   # the same row with the depth of field off (--a1-no-dof): the DoF-only effect map is the post picture minus this one
        rc, text = run_rig(rig, game, rsc, work, tag, args.size, scale, args.plant if scale == 2.0 else '', vp, eye, rowf, ('--a1-no-dof',))
        say(rc == 0, 'CLEAN  scale %g, DoF off: rig exit %d' % (scale, rc), 'rig failed at scale %g' % scale)
    for tag, scale in (('s1', 1.0), ('s2', 2.0), ('s05', 0.5)):
        sz = sizes_of(runs[tag][1])
        want = (ceil_f(np.float32(lw) * np.float32(scale)), ceil_f(np.float32(lh) * np.float32(scale)))
        if sz is None:
            say(False, 'SIZES  scale %g: the rig printed no a1-sizes line' % scale)
            continue
        flag = 0 if scale == 1.0 else 1
        say(sz['colour'] == want + (flag,), 'SIZES  scale %g: scene colour is %s (want %dx%d scaled=%d)' % (scale, 'x'.join(map(str, sz['colour'][:2])) + ' scaled=%d' % sz['colour'][2], want[0], want[1], flag), 'scene colour is not')
        say(sz['depth'] == want + (flag,), 'SIZES  scale %g: scene depth is %s (want %dx%d scaled=%d)' % (scale, 'x'.join(map(str, sz['depth'][:2])) + ' scaled=%d' % sz['depth'][2], want[0], want[1], flag), 'scene depth is not')
        say(sz['dconv'] == want + (flag,), 'SIZES  scale %g: the post chain\'s depth image (dconv) is %s (want %dx%d scaled=%d)' % (scale, 'x'.join(map(str, sz['dconv'][:2])) + ' scaled=%d' % sz['dconv'][2], want[0], want[1], flag), 'DoF / haze depth')
        say(sz['picture'] == want, 'SIZES  scale %g: the picture read back is %dx%d (want %dx%d)' % (scale, sz['picture'][0], sz['picture'][1], want[0], want[1]), 'picture is not')
        imgs = images_of(runs[tag][1])
        allowed = set((ceil_f(np.float32(scale) * np.float32(max(1, lw // k))), ceil_f(np.float32(scale) * np.float32(max(1, lh // k)))) for k in (1, 4, 8))
        scaled = [(w_, h_) for (w_, h_, s_) in imgs if s_]
        odd = sorted(set(i for i in scaled if i not in allowed))
        say(len(imgs) > 8 and (scale == 1.0 or len(scaled) >= 6) and not odd,
            'SCREEN scale %g: %d live images, %d scaled, sizes of the scaled ones %s; scaled with a size not derived from the logical size: %s' % (scale, len(imgs), len(scaled), sorted(set(scaled)), odd or 'none'), 'a non-screen image is scaled')
    # pictures
    try:
        n1, n2 = png(os.path.join(work, 's1_nopost.png')), png(os.path.join(work, 's2_nopost.png'))
        p1, p2 = png(os.path.join(work, 's1_post.png')), png(os.path.join(work, 's2_post.png'))
        up = np.asarray(Image.fromarray(n1.astype(np.uint8)).resize((n2.shape[1], n2.shape[0]), Image.BILINEAR), dtype=np.float32)
        ratio = grad_x(n2) / max(grad_x(up), 1e-6)
        say(ratio >= DETAIL_MIN, 'DETAIL scale 2 scene is drawn at 2x, not the 1x enlarged: mean |dx| %.3f vs %.3f enlarged (ratio %.3f, want >= %.2f)' % (grad_x(n2), grad_x(up), ratio, DETAIL_MIN), 'scene colour is not')
        red = p2.reshape(p1.shape[0], 2, p1.shape[1], 2, 3).mean(axis=(1, 3)) if (p2.shape[0] == 2 * p1.shape[0] and p2.shape[1] == 2 * p1.shape[1]) else None
        if red is None:
            say(False, 'DEPTH  the scale-2 post picture is %s, not twice the 1x one %s' % (p2.shape, p1.shape), 'DoF / haze depth')
        else:
            d = float(np.abs(red - p1).mean())
            dn = float(np.abs(n2.reshape(n1.shape[0], 2, n1.shape[1], 2, 3).mean(axis=(1, 3)) - n1).mean())
            say(d <= DEPTH_MAX, 'DEPTH  DoF / haze depth: the scale-2 post picture reduced to 1x differs from the 1x post picture by %.2f levels (no-post pair %.2f; want <= %.1f)' % (d, dn, DEPTH_MAX), 'DoF / haze depth')
        q1, q2 = png(os.path.join(work, 'n1_post.png')), png(os.path.join(work, 'n2_post.png'))
        if red is not None and q2.shape[0] == 2 * q1.shape[0]:
            e1 = np.abs(p1 - q1)                                                          # the DoF-only effect, 1x
            e2 = np.abs(p2 - q2).reshape(p1.shape[0], 2, p1.shape[1], 2, 3).mean(axis=(1, 3))   # ... at 2x, reduced to the 1x grid
            rel = float(np.abs(e2 - e1).mean() / max(float(e1.mean()), 1e-6))
            corr = float(np.corrcoef(e1.ravel(), e2.ravel())[0, 1])
            say(float(e1.mean()) > 0.5 and corr >= DOF_CORR_MIN, 'DOFMAP DoF / haze depth: the depth-of-field effect (post minus post-without-DoF) at scale 2, reduced to 1x, matches the 1x effect: mean effect %.2f levels, correlation %.3f (want >= %.2f), mean |diff| / mean effect %.3f' % (float(e1.mean()), corr, DOF_CORR_MIN, rel), 'DOFMAP')
    except Exception as e:  # noqa: BLE001
        say(False, 'PICTURES could not be read: %s' % e)
    for tag, scale in (('s1', 1.0), ('s2', 2.0), ('s05', 0.5)):
        t = runs[tag][1]
        say('ghost-test PASS' in t, 'GHOST  scale %g: afterimage test %s' % (scale, 'PASS' if 'ghost-test PASS' in t else 'FAIL'), 'afterimage lands wrong at scale %g' % scale)
    arm_f1(args, say, rig, game, rsc, work, vp, eye, rowf, capture)
    arm_alloc(args, say, rig, game, rsc, work, vp, eye, rowf, capture)
    arm_host(args, say, rig, game, rsc, work, vp, eye, rowf, capture)
    print(('PASS' if not bad else 'FAIL') + '  a1_check: frame %d of %s, %d failing check(s)%s' % (n, name, len(bad), (': ' + '; '.join(sorted(set(bad)))) if bad else ''))
    print('  pictures: ' + work)
    return 0 if not bad else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frame', type=int, default=110)
    ap.add_argument('--size', default='640x360')
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default='', choices=['', 'unscaled', 'dconv', 'ghost'])
    ap.add_argument('--only', default='', choices=['', 'f1', 'host', 'alloc'], help='run one arm only (development)')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    args = ap.parse_args()
    if args.expect_fail is None:
        return run_main(args)
    import contextlib, io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = run_main(args)
    text = buf.getvalue()
    sys.stdout.write(text)
    caught = rc != 0 and args.expect_fail in text
    print(('PASS' if caught else 'FAIL') + '  planted fault %s: run exit %d, token "%s" %s' % ('caught' if caught else 'NOT caught', rc, args.expect_fail, 'found' if args.expect_fail in text else 'missing'))
    return 0 if caught else 1


if __name__ == '__main__':
    sys.exit(main())

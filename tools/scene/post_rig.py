#!/usr/bin/env python3
r"""Lane P's rig (Session 103): our post pass from the scene record, beside the game's own picture.

  py -I tools\scene\post_rig.py --capture C:\...\out\captures\brock_losswin --frames 20,110,200 [--out DIR] [--plant swap_dof|no_depth_terms|bad_row] [--expect-fail TOKEN]

The captured records carry no post row (lane A's writer is live-only), so for the TEST only a row is made from the capture's own post draws: the combine draw's constants
(post_proof.exe --dump, read-only) are turned back into the words of S by the inverse of sub_82C1C1A0 (c20 = (1/(S24-S12), 1/(S16-S28), S8, S20), c21 = (S24, S28, S32, 0);
c27 = S676.., c31 = S188, c32 = S192; flags from the combine's variant name). That row goes INTO the record (written and read back by post_rig.exe: section 29) and our
src\renderer_scene\post_from_record.cpp maps it. Never live: live, S is read from the guest at the latch.
Checks per frame (0 game runs; fview + post_proof dump + post_rig.exe, headless):
  1 MAPPING   the PostParams post_rig printed equal the game's own combine constants (c20, c21, c27, c31 x, c32 x, the depth sampler, the variant) to 2e-5 relative.
  2 PICTURE   mean |game - ours| over the frame: ours WITH post must be closer to the game's picture than ours without it (the scene is not draw-for-draw the game's:
              the bar is "the post moves the picture toward the game's", on a sheet; planted faults must move it away or fail the mapping).
  3 VALIDATION  0 errors.
A planted fault run (--plant) is expected to fail: --expect-fail TOKEN exits 0 only when the run failed and printed TOKEN.
Sheet: DIR\post_sheet_fNNN.png = game | ours without post | ours with post | difference heat (post vs game) | the same for the planted run when given.
"""
import argparse, os, re, struct, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig as fr  # noqa: E402  (the pairing, the camera and the picture helpers are the rig's own)

WORDS = 176
OFF = dict(on=0, dof=4, d8=8, d12=12, d16=16, far=20, d24=24, d28=28, floor=32, bloom=176, bloomc31=188, bloomc32=192, fog=656, fogc27=676)


def word_of(f):
    return struct.unpack('<I', struct.pack('<f', float(f)))[0]


def parse_consts(text):
    """The combine draw (yGlareEtc_*) of a post_proof --dump: variant name, constants by name, the depth sampler of the dof draw."""
    out = {}
    m = re.search(r'yGlareEtc_corner_uv_vertex \+ (yGlareEtc_\w+?)_fragment', text)
    if not m:
        return None
    out['variant'] = m.group(1)
    start = text.index(m.group(0))
    block = text[start:]
    nxt = re.search(r'\nop \d+ ', block[10:])
    block = block[:nxt.start() + 10] if nxt else block
    for name in ('DOFParam', 'DOFParam2', 'RGB', 'Alpha', 'ZAdj', 'BloomScale', 'StarScale'):
        c = re.search(r'const ps %s c\d+ x1: ([^\n]+)' % name, block)
        if c:
            out[name] = [float(x) for x in c.group(1).split()]
    d = re.search(r'yDownScale_dof_fragment[^\n]*\n(?:    [^\n]*\n)*?    tex slot 1 image \d+ \S+ \S+ \| (\w+)/', text)
    out['depth_linear'] = bool(d and d.group(1) == 'linear')
    w = re.search(r'const ps g_f4DOFParam2 c\d+ x1: ([^\n]+)', text)   # the downsample's c21: its .w is what S+172 must hold (same unit)
    out['down_w'] = float(w.group(1).split()[3]) if w else 0.0003
    return out


def make_row(c, bad=False):
    """The words of S that give these combine constants (the inverse of sub_82C1C1A0 / sub_82C1C768): the TEST record's row."""
    s = [0] * WORDS
    v = c['variant']
    dof, fog, bloom = '_dof' in v or v.startswith('yGlareEtc_dof'), 'fog' in v, 'bloom' in v
    s[OFF['on'] // 4] = 1
    s[OFF['dof'] // 4] = 1 if dof else 0
    s[OFF['fog'] // 4] = 1 if fog else 0
    s[OFF['bloom'] // 4] = 1 if bloom else 0
    c20, c21 = c['DOFParam'], c['DOFParam2']
    s[172 // 4] = word_of(c['down_w'])   # S+172 (kStrength) -> the downsample's c21.w
    s24, s28, s32 = c21[0], c21[1], c21[2]
    # c20.x = 1 / (S24 - S12): inf means S12 = S24; c20.y = 1 / (S16 - S28)
    s12 = s24 - (0.0 if np.isinf(c20[0]) else 1.0 / c20[0])
    s16 = s28 + (0.0 if np.isinf(c20[1]) else 1.0 / c20[1])
    vals = {'d8': c20[2], 'd12': s12, 'd16': s16, 'far': c20[3], 'd24': s24, 'd28': s28, 'floor': s32}
    for k, x in vals.items():
        s[OFF[k] // 4] = word_of(x)
    for i in range(3):
        s[OFF['fogc27'] // 4 + i] = word_of(c['RGB'][i])
    if 'BloomScale' in c:
        s[OFF['bloomc31'] // 4] = word_of(c['BloomScale'][0])
        s[OFF['bloomc32'] // 4] = word_of(c['StarScale'][0])
    if bad:   # planted fault: S's DoF block moved one slot (the layout fault): c20 / c21 then read the wrong words
        s[OFF['d24'] // 4], s[OFF['d28'] // 4] = s[OFF['d28'] // 4], s[OFF['d24'] // 4]
    return struct.pack('<%dI' % WORDS, *s)


def expect_params(c):
    """What the mapping must print, from the game's own constants."""
    return dict(DOFParam=c['DOFParam'], DOFParam2=c['DOFParam2'], RGB=c['RGB'], BloomScale=c.get('BloomScale', [0])[0] if 'bloom' in c['variant'] else 0.0,
                StarScale=c.get('StarScale', [0])[0] if 'bloom' in c['variant'] else 0.0, depth_linear=c['depth_linear'])


def close(a, b):
    if np.isinf(a) or np.isinf(b):
        return np.isinf(a) and np.isinf(b)
    return abs(a - b) <= 2e-5 * max(1.0, abs(a), abs(b))


def analytic_q(vp):
    """q and nq of the game's own world -> clip matrix (row vectors): clip.z = q * clip.w - nq, fitted over points in front of the camera (also the residual)."""
    rng = np.random.default_rng(7)
    P = np.concatenate([rng.uniform(-3000, 3000, (4000, 3)), np.ones((4000, 1))], axis=1)
    C = P @ np.asarray(vp, dtype=np.float64).reshape(4, 4)
    C = C[C[:, 3] > 1.0]
    A = np.stack([C[:, 3], -np.ones(len(C))], axis=1)
    sol, res, _, _ = np.linalg.lstsq(A, C[:, 2], rcond=None)
    resid = float(np.abs(A @ sol - C[:, 2]).max())
    return float(sol[0]), float(sol[1]), resid


def compare_params(line, c, qn=None):
    m = re.search(r'params: DOFParam (\S+) (\S+) (\S+) (\S+) \| DOFParam2 (\S+) (\S+) (\S+) (\S+) \| RGB (\S+) (\S+) (\S+) (\S+) \| BloomScale.x (\S+) \| StarScale.x (\S+) \|'
                  r' dof (\d) fog (\d) bloom (\d) glare (\d) film (\d) \| depth_linear (\d) \| dead_bloom (\d) dead_glare (\d) \| q (\S+) nq (\S+) \| down_w (\S+)', line)
    if not m:
        return ['no params line']
    g = m.groups()
    f = lambda i: float(g[i])
    bad = []
    e = expect_params(c)
    for j in range(4):
        if not close(f(j), e['DOFParam'][j]): bad.append('DOFParam[%d] ours %g game %g' % (j, f(j), e['DOFParam'][j]))
        if not close(f(4 + j), e['DOFParam2'][j]): bad.append('DOFParam2[%d] ours %g game %g' % (j, f(4 + j), e['DOFParam2'][j]))
        if '_fog' in c['variant'] or 'fog' in c['variant']:
            if not close(f(8 + j), e['RGB'][j]): bad.append('RGB[%d] ours %g game %g' % (j, f(8 + j), e['RGB'][j]))
    if not close(f(12), e['BloomScale']): bad.append('BloomScale.x ours %g game %g' % (f(12), e['BloomScale']))
    if not close(f(13), e['StarScale']): bad.append('StarScale.x ours %g game %g' % (f(13), e['StarScale']))
    if bool(int(g[19])) != e['depth_linear']: bad.append('depth sampler ours %s game %s' % ('linear' if int(g[19]) else 'nearest', 'linear' if e['depth_linear'] else 'nearest'))
    want = (int('dof' in c['variant']), int('fog' in c['variant']), int('bloom' in c['variant']))
    if abs(f(24) - c['down_w']) > 1e-6 * max(1.0, abs(c['down_w'])) and abs(f(24) - c['down_w']) > 1e-9:
        bad.append('S+172 -> dof_param2_down.w ours %g game %g' % (f(24), c['down_w']))
    if qn is not None and (abs((f(22) - 1) - (qn[0] - 1)) > 2e-7 or abs(f(23) - qn[1]) > 1e-3 * max(1, abs(qn[1]))):   # q - 1 (3e-5 in the game: 1e-4 absolute could not see q at all)
        bad.append('PROJQ FAULT: depth terms q %g nq %g, the game own matrix gives q %g nq %g' % (f(22), f(23), qn[0], qn[1]))
    if (int(g[14]), int(g[15]), int(g[16])) != want: bad.append('variant dof/fog/bloom ours %s game %s' % ((int(g[14]), int(g[15]), int(g[16])), want))
    return bad


def md(a, b):
    return float(np.abs(a.astype(np.int16) - b.astype(np.int16)).mean())


def fov_main(args):
    """A camera as the composer's copy gives it (view + FOV, kCamProjFromFov) through rf::BuildFrame: ProjQ must read (f/(f-n), n f/(f-n)); q - 1 compared at 1e-7 absolute (q itself is 1.00003)."""
    main_root = fr.main_checkout()
    rig = os.path.join(ROOT, 'out', 'tools', 'post_rig.exe')
    capture = os.path.abspath(args.capture)
    cmd = [rig, '--fov-test', '--record', os.path.join(capture, 'scene', 'frame_000110.rsc'), '--game', os.path.join(main_root, 'game_data', 'extracted'), '--prefix', os.path.join(main_root, 'out', 's103', 'P', 'fov')]
    if args.plant == 'no_depth_terms':
        cmd += ['--plant', 'no_depth_terms']
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600)
    text = (p.stdout or '') + (p.stderr or '')
    rows = re.findall(r'fov-test near (\S+) far (\S+): ok (\d) q-1 (\S+) expect (\S+) nq (\S+) expect (\S+)', text)
    if p.returncode != 0 or len(rows) != 2:
        print('FAIL  fov-test: exit %d: %s' % (p.returncode, text[-400:]))
        return 1
    bad = []
    for near, far, ok, q1, eq1, nq, enq in rows:
        line = 'near %s far %s: q-1 %s (expect %s), nq %s (expect %s)' % (near, far, q1, eq1, nq, enq)
        good = ok == '1' and abs(float(q1) - float(eq1)) <= 1e-7 and abs(float(nq) - float(enq)) <= 1e-5 * float(enq)
        print(('PASS' if good else 'FAIL') + '  FOV camera ' + line + ('' if good else '  ->  PROJQ FAULT'))
        if not good:
            bad.append(near)
    # the inputs: (built, missing, dof_chain) per case. MISSING inputs block READY / count a miss; "module off" and "no effect" are genuinely no post.
    want = {'no_row': (0, 1, -1), 'unreadable': (0, 1, -1), 'stale': (0, 1, -1), 'module_off': (0, 0, -1), 'no_effect': (0, 0, -1), 'ok': (1, 0, 0), 'bad_camera': (0, 1, -1), 'bloom_no_dof': (1, 0, 1)}
    got = {m[0]: (int(m[1]), int(m[2]), int(m[3])) for m in re.findall(r'info-test (\w+): built (\d) missing (\d) dof_chain (-?\d)', text)}
    for k, v in want.items():
        good = got.get(k) == v
        print(('PASS' if good else 'FAIL') + '  INPUTS %s: built/missing/dof_chain %s (expect %s)' % (k, got.get(k), v) + ('' if good else '  ->  INPUTS FAULT'))
        if not good:
            bad.append(k)
    return 1 if bad else 0


def run_main(args):
    if args.fov_test:
        return fov_main(args)
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = os.path.abspath(args.capture)
    name = os.path.basename(capture.rstrip('\\/'))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('P', args.out)
    work = os.path.join(out, 'work_' + name)
    os.makedirs(work, exist_ok=True)
    rig = os.path.join(ROOT, 'out', 'tools', 'post_rig.exe')
    rsc_dump = os.path.join(main_root, 'out', 'tools', 'rsc_dump.exe')
    proof = os.path.join(main_root, 'out', 'renderer', 'dev', 'post_proof.exe')   # --dump only reads the capture (not the code under test)
    os.environ.setdefault('WWE_FVIEW_ALLOW_STALE', '1')   # the game's picture does not depend on this lane's code (the binder is untouched)
    from fview_exe import find_fview
    fview = find_fview(root=ROOT)
    for need in (rig, rsc_dump, proof, game, capture):
        if not os.path.exists(need):
            print('FAIL  cannot read its input: ' + need)
            return 1
    frames = [int(x) for x in args.frames.split(',')]
    fails = []
    gdir = os.path.join(work, 'game')
    if not all(os.path.exists(os.path.join(gdir, 'frame_%05d.png' % n)) for n in frames):
        rc, text = fr.run([fview, '--capture', capture, '--replay', '--frames', str(max(frames) + 1), '--dump-frames', ','.join(map(str, frames)), '--dump-dir', gdir, '--root', ROOT,
                           '--game', game, '--validation', '1'], timeout=1800)
        if rc != 0:
            print('FAIL  fview replay: ' + text[-400:])
            return 1
    for n in frames:
        game_img = fr.load(os.path.join(gdir, 'frame_%05d.png' % n))
        H, W = game_img.shape[:2]
        rsc = os.path.join(capture, 'scene', 'frame_%06d.rsc' % n)
        sg = fr.stream_guest_frame(capture, n)
        rg, _ = fr.record_guest_frame(rsc_dump, rsc)
        print(('PASS' if sg == rg else 'FAIL') + '  pairing: stream frame %d guest_frame %d vs record %s' % (n, sg, rg))
        if sg != rg:
            fails.append('pairing %d' % n)
            continue
        dump = os.path.join(work, 'dump_%d.txt' % n)
        if not os.path.exists(dump):
            fr.run([proof, '--capture', capture, '--frame', str(n), '--dump', '--out', dump, '--root', ROOT], timeout=600)
        c = parse_consts(open(dump, errors='replace').read()) if os.path.exists(dump) else None
        if not c or 'DOFParam' not in c:
            print('FAIL  frame %d: no combine draw found in the post_proof dump' % n)
            fails.append('dump %d' % n)
            continue
        print('  game combine: %s  c20 %s  c21 %s  c27 %s  c31 %s  c32 %s  depth %s' % (c['variant'], c['DOFParam'], c['DOFParam2'], c['RGB'], c.get('BloomScale', [0])[0], c.get('StarScale', [0])[0],
                                                                                     'linear' if c['depth_linear'] else 'nearest'))
        rowf = os.path.join(work, 'row_%d.bin' % n)
        open(rowf, 'wb').write(make_row(c, bad=(args.plant == 'bad_row')))
        goodrow = os.path.join(work, 'row_good_%d.bin' % n)
        open(goodrow, 'wb').write(make_row(c))
        cam, why = fr.stream_camera(fview, capture, n, game, work)
        if cam is None:
            print('FAIL  frame %d: no camera (%s)' % (n, why))
            fails.append('camera %d' % n)
            continue
        vp, eye = cam[0], cam[1]  # S103 L2: stream_camera returns four values since R2
        aq, anq, resid = analytic_q(vp)
        print('  game matrix (c56-c59): q %.7g nq %.7g, fit residual %.3g' % (aq, anq, resid))
        env = dict(os.environ, WWE_RDR_VALIDATION='1')

        def run_rig(prefix, row, plant):
            cmd = [rig, '--record', rsc, '--game', game, '--size', '%dx%d' % (W, H), '--prefix', prefix, '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye), '--row', row]
            if plant in ('swap_dof', 'no_depth_terms'):
                cmd += ['--plant', plant]
            p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=1800)
            return p.returncode, (p.stdout or '') + (p.stderr or '')

        good = os.path.join(work, 'ours_%d' % n)
        # always run the unplanted pass (a result left by an older post_rig.exe / code must never stand in for it)
        rc, text = run_rig(good, goodrow, '')
        if rc != 0:
            print('FAIL  frame %d: post_rig (unplanted) exit %d: %s' % (n, rc, text[-500:]))
            fails.append('rig %d' % n)
            continue
        prefix = good
        if args.plant:
            prefix = os.path.join(work, 'plant_%s_%d' % (args.plant, n))
            rc, text = run_rig(prefix, rowf, args.plant)
            if rc != 0:
                print('FAIL  frame %d: post_rig exit %d: %s' % (n, rc, text[-500:]))
                fails.append('rig %d' % n)
                continue
        print('  ' + [l for l in text.splitlines() if l.startswith('info:')][0])
        pl = [l for l in text.splitlines() if l.startswith('params:')]
        bad = compare_params(pl[0], c, (aq, anq)) if pl else ['no params line']
        print(('PASS' if not bad else 'FAIL') + '  frame %d MAPPING + DEPTH TERMS: post_from_record.cpp\'s PostParams equal the game\'s combine constants and its own matrix\'s q / nq' % n + ('' if not bad else '  ->  MAPPING FAULT: ' + '; '.join(bad)))
        if bad:
            fails.append('mapping %d' % n)
        ver = re.search(r'validation_errors (\d+)', text)
        vok = ver and int(ver.group(1)) == 0
        print(('PASS' if vok else 'FAIL') + '  frame %d: validation errors %s' % (n, ver.group(1) if ver else '?'))
        if not vok:
            fails.append('validation %d' % n)
        a, b = fr.load(good + '_nopost.png'), fr.load(prefix + '_post.png')
        bg = fr.load(good + '_post.png')
        # EFFECT: the post ran and changed the picture (not equal to the no-post one), finite, not black / blown out; and the planted run's picture equals the unplanted one
        eff = md(a, bg)
        lum_a, lum_b = float(a.mean()), float(bg.mean())
        eok = eff >= 1.0 and 0.5 * lum_a <= lum_b <= 2.0 * lum_a
        print(('PASS' if eok else 'FAIL') + '  frame %d EFFECT: our post changes the scene by mean %.2f levels (need >= 1), mean luminance %.1f -> %.1f' % (n, eff, lum_a, lum_b) + ('' if eok else '  ->  EFFECT FAULT'))
        if not eok:
            fails.append('effect %d' % n)
        if args.plant:
            dp = md(bg, b)
            pok = dp < 0.05
            print(('PASS' if pok else 'FAIL') + '  frame %d PICTURE: the planted run (%s) equals the unplanted run (mean diff %.3f)%s' % (n, args.plant, dp, '' if pok else '  ->  PICTURE FAULT: the planted picture differs from the unplanted one'))
            if not pok:
                fails.append('picture %d' % n)
        # the game beside ours: shown, not a bar (the scene's light / maps / wrestlers are other lanes': the whole-frame rig)
        hm, mean_d, frac = fr.heat(game_img, b)
        row = [fr.label(game_img, 'GAME: %s stream frame %d (its own post)' % (name, n)), fr.label(a, 'OURS scene, no post: mean luminance %.0f' % lum_a),
               fr.label(b, 'OURS + post from the record%s: %.0f' % (' [PLANT %s]' % args.plant if args.plant else '', float(b.mean()))),
               fr.label(np.abs(b.astype(np.int16) - a.astype(np.int16)).clip(0, 255).astype(np.uint8) * 4, 'what our post changed (x4)')]
        bw = 480
        row = [np.asarray(Image.fromarray(x).resize((bw, int(x.shape[0] * bw / x.shape[1])))) for x in row]
        mh = max(x.shape[0] for x in row)
        row = [np.pad(x, ((0, mh - x.shape[0]), (0, 0), (0, 0))) for x in row]
        sheet = np.concatenate(row, axis=1)
        spath = os.path.join(out, 'post_sheet_f%03d%s.png' % (n, ('_' + args.plant) if args.plant else ''))
        Image.fromarray(sheet).save(spath)
        print('  sheet: ' + spath)
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  post_rig')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default='', choices=['', 'swap_dof', 'no_depth_terms', 'bad_row'])
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    ap.add_argument('--fov-test', action='store_true', help='only the kCamProjFromFov camera case (ProjQ == f/(f-n), n f/(f-n))')
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

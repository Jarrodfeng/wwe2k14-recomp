#!/usr/bin/env python3
r"""Lane FXH's proof (Session 103): the highlight-reel cue post effects (defocus + hue S+560, emboss S+592, retro film S+604; docs\research\s103\COV9_story.md 2b / 2c / 2d), offline, headless, 0 game runs.

  py -I tools\scene\fxh_check.py --capture C:\...\out\captures\brock_losswin [--frame 110] [--size 640x360] [--out DIR] [--plant oldmask|emboss|defocus|retro|retro-step|strips|pass-order] [--expect-fail TOKEN]

What it answers: out\tools\post_rig.exe --fxh (tools\scene\post_rig.cpp FxhBuildTest / FxhUnitTest / FxhTest) draws ONE captured scene record (its camera = the stream's own, as pst_check.py takes it) through
post_from_record.Build -> FrontEnd -> PostPass with S words planted in a post row (the production path), and checks the pictures against CPU references written from the game's shader formulas:
  build    the words map to PostParams (rate, (Y, Cb, Cr, A) of sub_82C292D0, step / display size, g_Range, 4 / display height); a word of 0 draws nothing even with stale numbers beside it; FILM GRAIN off leaves
           the retro film out; a non-finite number leaves that effect out; the switch off names the words unsupported; the DoF blur chain runs for the defocus though S+4 is off
  clock    the retro clock (sub_82C288B0) against a double-precision simulation; ticks once per guest frame (idempotent), composes a forward gap, restarts far behind
  emboss   the picture equals the CPU relief of the base picture's red channel (yEmboss.frag); differs from off
  defocus  rate 0 / amount 1 equals the CPU hue shift; rate 1 / amount 0 shows the blur (lower gradient energy), with the combine's DoF on and off
  retro    the picture equals the CPU yRetroFilm on our noise pictures for a run of guest frames; a repeat of a guest frame is identical; the film moves between guest frames
  all      the three together equal retro(emboss(defocus-identity(base))): the order and the ping-pong into the output; three cue passes recorded
  zero     words 0: no cue pass recorded, the picture equals the base one
Planted faults (each must FAIL; --expect-fail TOKEN exits 0 only when the run failed and printed TOKEN):
  oldmask     the cue words unsupported again (post_from_record's old mask)           -> "fxh-build FAIL"
  emboss      the emboss flag ignored on the frame that must show it                     -> "emboss on equals emboss off"
  defocus     the defocus flag ignored                                                    -> "defocus on equals defocus off"
  retro       the retro flag ignored                                                      -> "retro on equals retro off"
  retro-step  the retro clock ticks on every Record (twice per guest frame when repeated)-> "fxh-unit FAIL: a repeat of the same guest frame ticks nothing"
  strips      the two noise strips bound in each other's slot                             -> "does not match yRetroFilm's formula"
  pass-order  the cue passes in the wrong order                                           -> "not applied in the game's order"
Needs the main checkout's capture + game_data\extracted (read-only, RDR_MAIN_CHECKOUT) and the fview exe: the same inputs as pst_check.py.
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig as fr  # noqa: E402
import post_rig as pr   # noqa: E402  (make_row: the words of S that give a match's combine constants)

# a match's combine constants (post_pass.cpp DefaultPostParams, measured: run 4511): the base row every case adds its cue words to
MATCH = dict(variant='yGlareEtc_dof_fog', DOFParam=[91.33, 338.67, 0.992, 0.8], DOFParam2=[0.991855, 0.99509, 0.0, 0.0], RGB=[0.4, 0.55, 0.55, 1.0], depth_linear=True, down_w=0.0003)


def run_main(args):
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = os.path.abspath(args.capture)
    name = os.path.basename(capture.rstrip('\\/'))
    import run_dirs
    out = run_dirs.run_out('FXH', args.out)
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
    cmd = [rig, '--record', rsc, '--game', game, '--size', args.size, '--prefix', os.path.join(work, 'fxh'), '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye), '--row', rowf, '--fxh']
    if args.plant:
        cmd += ['--fxh-plant', args.plant]
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=dict(os.environ, WWE_RDR_VALIDATION='1'), timeout=1800)
    text = (p.stdout or '') + (p.stderr or '')
    for line in text.splitlines():
        if line.startswith(('fxh-test', 'fxh-build', 'fxh-unit')) or line.startswith('validation_errors') or 'FAIL' in line:
            print(line)
    ver = re.search(r'validation_errors (\d+)', text)
    ok = p.returncode == 0 and 'fxh-test PASS' in text and ver is not None and int(ver.group(1)) == 0
    if not ok:
        print('  rig output tail (without the [rdr] log lines): ' + ' | '.join([l[:200] for l in text.splitlines() if l.startswith(('fxh-test', 'fxh-build', 'fxh-unit', 'info', 'params', 'record', 'post_rig', 'validation'))][-25:])[-2500:])
    print(('PASS' if ok else 'FAIL') + '  fxh_check: frame %d of %s, rig exit %d, validation errors %s' % (n, name, p.returncode, ver.group(1) if ver else '?'))
    print('  pictures: ' + work)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frame', type=int, default=110)
    ap.add_argument('--size', default='640x360')
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default='', choices=['', 'oldmask', 'emboss', 'defocus', 'retro', 'retro-step', 'strips', 'pass-order'])
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

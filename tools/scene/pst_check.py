#!/usr/bin/env python3
r"""Lane PST's proof (Session 103 overnight): the tonemap / night / antialiasing post effects (docs\research\s103\COV9_story.md 2a / 2e), offline, headless, 0 game runs.

  py -I tools\scene\pst_check.py --capture C:\...\out\captures\brock_losswin [--frame 110] [--size 640x360] [--out DIR] [--plant step|tonemap|night|aa|pause|hala-word] [--expect-fail TOKEN]

What it answers: out\tools\post_rig.exe --pst draws ONE captured scene record (its camera = the stream's own, as post_rig.py takes it) through FrontEnd -> PostPass with PLANTED post records (flags set straight in
PostParams, as post_from_record maps S+180 / +184 / +280) and checks, from the pictures alone (tools\scene\post_rig.cpp PstTest):
  tonemap  on differs from off; the measured luminance is the scene's log-average luma (the 64 -> 16 -> 4 -> 1 chain); the adapted 1x1 follows 0.0396 per guest frame (the fraction of the way from A's value
           to B's after 5 / 10 / 20 guest frames = 1 - 0.9604^n, within 0.05); a fresh adaptation snaps; a guest frame recorded twice more gives byte-identical pictures (idempotent per guest frame)
  night    on differs from off; night + tonemap differs from tonemap alone
  aa       on differs from off, only at edges (nothing changes inside 5x5-flat regions of the raw scene); the fixed (0.64) and uniform (S+284) variants both draw
Planted faults (each must FAIL; --expect-fail TOKEN exits 0 only when the run failed and printed TOKEN):
  step      the adapted luminance steps on every Record (a twin stepping twice per guest frame)       -> "twin stepping twice per guest frame" / "does not follow 0.0396"
  tonemap   the tonemap flag ignored on the frame that must show it                                    -> "tonemap on equals tonemap off"
  night     the night flag ignored                                                                     -> "night on equals night off"
  aa        the antialiasing flag ignored                                                              -> "antialiasing on equals antialiasing off"
Needs the main checkout's capture + game_data\extracted (read-only, RDR_MAIN_CHECKOUT) and the fview exe: the same inputs as post_rig.py.
"""
import argparse, os, re, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig as fr  # noqa: E402
import post_rig as pr   # noqa: E402  (make_row: the words of S that give a match's combine constants)

# a match's combine constants (post_pass.cpp DefaultPostParams, measured: run 4511); the row only has to make post_from_record.Build succeed, PstTest overrides the effect flags itself
MATCH = dict(variant='yGlareEtc_dof_fog', DOFParam=[91.33, 338.67, 0.992, 0.8], DOFParam2=[0.991855, 0.99509, 0.0, 0.0], RGB=[0.4, 0.55, 0.55, 1.0], depth_linear=True, down_w=0.0003)


def run_main(args):
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = os.path.abspath(args.capture)
    name = os.path.basename(capture.rstrip('\\/'))
    import run_dirs
    out = run_dirs.run_out('PST', args.out)
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
    cmd = [rig, '--record', rsc, '--game', game, '--size', args.size, '--prefix', os.path.join(work, 'pst'), '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye), '--row', rowf, '--pst']
    if args.plant:
        cmd += ['--pst-plant', args.plant]
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=dict(os.environ, WWE_RDR_VALIDATION='1'), timeout=1800)
    text = (p.stdout or '') + (p.stderr or '')
    for line in text.splitlines():
        if line.startswith(('pst-test', 'pst-build')) or line.startswith('validation_errors') or 'FAIL' in line:
            print(line)
    ver = re.search(r'validation_errors (\d+)', text)
    ok = p.returncode == 0 and 'pst-test PASS' in text and ver is not None and int(ver.group(1)) == 0
    if not ok:
        print('  rig output tail (without the [rdr] log lines): ' + ' | '.join([l[:200] for l in text.splitlines() if l.startswith(('pst-test', 'pst-build', 'info', 'params', 'record', 'post_rig', 'validation'))][-25:])[-2500:])
    print(('PASS' if ok else 'FAIL') + '  pst_check: frame %d of %s, rig exit %d, validation errors %s' % (n, name, p.returncode, ver.group(1) if ver else '?'))
    print('  pictures: ' + work)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frame', type=int, default=110)
    ap.add_argument('--size', default='640x360')
    ap.add_argument('--out', default=None)
    ap.add_argument('--plant', default='', choices=['', 'step', 'tonemap', 'night', 'aa', 'pause', 'hala-word'])
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

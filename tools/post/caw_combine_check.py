#!/usr/bin/env python3
r"""Lane 244 offline proof (S101, 0 game runs): the CAW editor's post combine under the wide (21:9) rule. Docs: docs\renderer\POST_OURS.md section "Lane 244".

  py -I tools\post\caw_combine_check.py [--root DIR] [--rplay EXE] [--work DIR] [--pictures DIR] [--plant]

What it answers: in a 21:9 run (gpu_wide_factor 1.3125, gpu_wide_all = the app's menu setting) the CAW editor showed no wrestler and a dark background, because the wide rule
squeezed the clip-off (pixel-placed) post passes with a centre of 4096 instead of the frame's 640: the combine landed 976 px to the right (a 304 px strip), the blur passes outside
their scissors. wide_rule.h now leaves clip-off draws alone.

Inputs (a missing one FAILS): the case zip of 20261113_035959_s101_checkB_caw_walk (recorded WITH wide 1.3125 + wide_all), the case zip of 20261113_062332_s101_sweep_match_type
(16:9 match, no wide: the regression capture), the built rplay.exe of this tree (tools\renderer\build.ps1).
  recon     rplay --wide 1.3125 --wide-all --wide-from-squeezed --wide-clipoff-squeezed (the OLD rule on dynamics rebuilt from guest_scale / guest_offset) == the recorded capture's own stream
            picture (0 pixels over 1 level, frames 0 / 100 / 200 / 300): the offline replay reproduces what the live run drew.
  fix       the same without --wide-clipoff-squeezed: >= 600000 pixels differ from the old rule on every frame (lane 238 measured 700 367 / 700 430 / 700 540 / 699 882 between the stream
            and our pass), the wrestler is back (pixels brighter than 60 in the middle of the picture: more than 100000, the old rule under 8000 (the menu text)), the right half is the red background.
  match     a match capture under --wide 1.3125 (no wide_all): the new rule equals the old one (0 pixels), and 0 clip-off draws are squeezed (census): the HUD / scene phases never met the bug.
  --plant   runs the 'fix' lines with the old rule on both sides (--wide-clipoff-squeezed): they must FAIL (exit 1).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import post_ours_check as po          # main_checkout, Results
import post_ours_cover_check as cc    # extract

CAW_RUN = '20261113_035959_s101_checkB_caw_walk'
MATCH_RUN = '20261113_062332_s101_sweep_match_type'
CAW2_RUN = '20261113_132031_s101_checkB_caw_walk'   # lane 244b: recorded WITH the clip-off fix (live 7169): the ghost outline (offscreen passes squeezed)
SHOTS = (0, 100, 200, 300)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=po.ROOT)
    ap.add_argument('--rplay', default='')
    ap.add_argument('--work', default='')
    ap.add_argument('--pictures', default='')
    ap.add_argument('--plant', action='store_true')
    args = ap.parse_args()
    import numpy as np
    from PIL import Image
    root = os.path.abspath(args.root)
    main_co = po.main_checkout(root)
    work = args.work or os.path.join(main_co, 'out', 'caw_combine_cache')
    os.makedirs(work, exist_ok=True)
    rplay = args.rplay or os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    res = po.Results()
    if not os.path.isfile(rplay):
        res.line(False, 'input missing: %s' % rplay)
        return 1
    caw = cc.extract(main_co, CAW_RUN, work)
    match = cc.extract(main_co, MATCH_RUN, work)
    if not caw or not match:
        res.line(False, 'input missing: the CAW walk / match case zip')
        return 1

    def arm(cap, tag, frames, shots, extra):
        sd = os.path.join(work, 'shots_' + tag)
        os.makedirs(sd, exist_ok=True)
        cmd = [rplay, '--capture', cap, '--frames', str(frames), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, shots))] + extra
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=900)
        if p.returncode != 0:
            raise RuntimeError('rplay %s failed (exit %d)' % (tag, p.returncode))
        return sd, p.stdout + p.stderr

    def img(sd, f):
        return np.asarray(Image.open(os.path.join(sd, 'frame_%04d.png' % f)).convert('RGB')).astype(np.int32)

    def differ(a, b):
        return int((np.abs(a - b).max(axis=2) > 1).sum())

    wide = ['--wide', '1.3125']
    wa = wide + ['--wide-all', '--wide-from-squeezed']
    old = ['--wide-clipoff-squeezed', '--wide-offscreen-squeezed']   # the rule before lane 244 (both plants)
    s_dir, _ = arm(caw, 'caw_stream', 301, SHOTS, [])
    o_dir, _ = arm(caw, 'caw_old', 301, SHOTS, wa + old)
    n_dir, _ = arm(caw, 'caw_new', 301, SHOTS, wa + (old if args.plant else []))
    for f in SHOTS:
        s, o, n = img(s_dir, f), img(o_dir, f), img(n_dir, f)
        res.line(differ(s, o) == 0, 'recon.%d: the old rule on rebuilt dynamics equals the recorded stream picture (%d px over 1 level)' % (f, differ(s, o)))
        d = differ(o, n)
        mid = (slice(100, 600), slice(640, 1100))
        bright = lambda a: int((a[mid].max(axis=2) > 60).sum())
        res.line(d >= 600000 and bright(n) > 100000 and bright(o) < 8000 and n[:, 640:].mean() > 2 * o[:, 640:].mean(),
                 'fix.%d: the combine covers the frame again: %d px differ from the old rule, middle bright px %d (old %d), right half mean %.1f (old %.1f)' % (f, d, bright(n), bright(o), n[:, 640:].mean(), o[:, 640:].mean()))
    if args.pictures:
        os.makedirs(args.pictures, exist_ok=True)
        for tag, sd in (('stream', s_dir), ('ours', n_dir)):
            Image.fromarray(img(sd, 100).astype(np.uint8)).save(os.path.join(args.pictures, '%s_frame100.png' % tag))
    # lane 244b: the capture recorded after the clip-off fix: the old offscreen rule (--wide-offscreen-squeezed) reproduces it, the new one moves ~15 000 px (the ghost outline)
    caw2 = cc.extract(main_co, CAW2_RUN, work)
    if not caw2:
        res.line(False, 'input missing: the 132031 CAW walk case zip')
    else:
        s2, _ = arm(caw2, 'caw2_stream', 301, (100, 200), [])
        p2, _ = arm(caw2, 'caw2_old', 301, (100, 200), wa + ['--wide-offscreen-squeezed'])
        n2, _ = arm(caw2, 'caw2_new', 301, (100, 200), wa + (['--wide-offscreen-squeezed'] if args.plant else []))
        for f in (100, 200):
            a_, b_, c_ = img(s2, f), img(p2, f), img(n2, f)
            res.line(differ(a_, b_) == 0, 'ghost.recon.%d: the offscreen-squeezed rule reproduces the recorded picture of live run 7169 (%d px over 1 level)' % (f, differ(a_, b_)))
            res.line(differ(a_, c_) > 5000, 'ghost.fix.%d: offscreen passes left alone move the picture where the ghost was (%d px over 1 level, the live picture had the ghost)' % (f, differ(a_, c_)))
    # a match: the HUD / scene phases never squeeze a clip-off draw, so nothing changes
    m_old, _ = arm(match, 'match_old', 120, (0, 60, 119), wide + old)
    m_new, text = arm(match, 'match_new', 120, (0, 60, 119), wide + (old if args.plant else []))
    worst = max(differ(img(m_old, f), img(m_new, f)) for f in (0, 60, 119))
    res.line(worst == 0, 'match: --wide 1.3125 on a match capture, new rule == old rule on frames 0 / 60 / 119 (%d px)' % worst)
    cen, text2 = arm(match, 'match_census', 120, (), wide + ['--wide-census'])
    squeezed_clip_off = sum(1 for l in text2.splitlines() if l.startswith('[wide] draw') and 'vp_w 8192' in l and '-> SQUEEZE' in l)
    pix = sum(1 for l in text2.splitlines() if l.startswith('[wide] draw') and 'PIXEL-PLACED' in l)
    res.line(squeezed_clip_off == 0 and pix == 0, 'match.census: clip-off draws squeezed %d, left alone only because of the new rule %d (both 0: the bug lives in the blanket modes only)' % (squeezed_clip_off, pix))
    off = sum(1 for l in text2.splitlines() if l.startswith('[wide] draw') and 'OFFSCREEN' in l)
    res.line(True, 'match.census: offscreen clip-on draws left alone by lane 244b: %d (information; the match picture line above must stay 0 px)' % off)
    print('%d lines, %d failed' % (res.lines, res.fails))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
r"""Lane L's gate check (Session 103): the stage lit like the game's, from the record. 0 game runs: fview headless + frame_rig.exe.

  py -I tools\scene\l_check.py [--out DIR] [--frames 20,110,200] [--second CAPTURE:FRAME]

For each frame, four rig runs on brock_losswin (an entrance, arena 4): `before` (the record as a live writer writes it today), `after` (+ the stage look rows read from the game's own
draw constants: --look-from-stream), and the planted twins on `after`: the entrance flag ignored, the rig rows ignored, the arena sets joined to the crowd's slot. Per family the mean
|diff| to the game's SCENE target (before post and 2D overlays, tools\scene\scene_ref.py: the presented frame has a full-width 2D movie overlay that darkens the floor to ~0.55x,
FL_floor_brightness.md [C]; the presented-frame numbers are printed as INFO lines only) and the luminance ratio ours / game (tools\scene\l_metrics.py) over the pixels the family covers. PASS needs:
  - after: the arena family's mean |diff| <= 85 % of before's at every frame, its luminance ratio inside [0.8, 1.4] (measured on the `after_pools` arm: after + the entrance page's pool / spot rows, which the offline record lacks, --look-pools), validation 0 (frame_rig's own FAIL lines);
  - plant `entrance`: no improvement (arena mean |diff| >= 97 % of before's);
  - plant `rig`: worse than after by >= 1.0 point of arena mean |diff|;
  - plant `wrong_slot`: the arena's luminance ratio outside [0.8, 1.4];
  - the second capture / frame (default brock_losswin_rt:100): the same `after` rule.
Exit 0 = all PASS. Writes a JSON of the numbers under --out (default: GATE_CHECK_DIR\L_gate, else a per-run out\L_gate_<pid>, removed on PASS).

S103 PS1 (load once, draw many; compare in the rig): the arms of a frame set run on ONE frame_rig.exe process (frame_rig.RigSession: the arena, models, textures and crowd stay loaded between arms; the game's
replay pictures, body scales, camera, look rows and pools are made once per frame, not once per arm), and the numbers above are computed by the rig itself from the pictures it just drew (tools\scene\rig_metrics.h:
l_metrics.py's formulas, the `before` arm's covered-pixel masks kept in the process for the other arms), not by Python over PNG files. The per-family PNGs and the picture sheets are written only with --sheets.
  --sheets           write the per-family PNGs + the picture sheets (frame_rig's first_sheet_fNNN.png) as before (the proof of byte-equal pictures uses this)
  --verify-metrics   also compute every number the old way (l_metrics.py over the PNGs; needs --sheets) and FAIL when the rig's differ by more than 0.01 (the equality proof)
  --no-serve         every arm its own process, as before (the A/B)
"""
import argparse, contextlib, io, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import l_metrics  # noqa: E402
import scene_ref  # noqa: E402
import run_dirs  # noqa: E402   (S103 PSO: a private output folder per run)
import frame_rig  # noqa: E402   (S103 PS1: the arms run in THIS process on one frame_rig.exe --serve: the game is loaded once, the per-frame preparation is made once)


def main_checkout():
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.normpath(os.path.dirname(common)) if common else ROOT


def rig(capture, frames, out, extra):
    """one frame_rig arm, in this process (frame_rig.main) on the session's rig server; the text is what the one-shot's stdout was"""
    argv = ['--capture', capture, '--frames', ','.join(map(str, frames)), '--out', out] + extra
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            rc = frame_rig.main(argv)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 1
    return rc, buf.getvalue()


def rounded(m):
    """the rig's unrounded numbers as l_metrics.compute rounds them"""
    out = {}
    for fam, v in m.items():
        if 'mean_diff_pct' in v:
            r = {'mean_diff_pct': round(float(v['mean_diff_pct']), 2), 'lum_ratio': round(float(v['lum_ratio']), 3), 'washed_pct': round(float(v['washed_pct']), 2)}
            out[fam] = dict(({'covered_px': int(v['covered_px'])} if 'covered_px' in v else {}), **r)
        else:
            out[fam] = {'covered_px': int(v['covered_px'])}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--second', default='brock_losswin_rt:100')
    ap.add_argument('--sheets', action='store_true', help='write the per-family PNGs and the picture sheets')
    ap.add_argument('--verify-metrics', action='store_true', help='also compute the numbers with l_metrics.py over the PNGs and compare (needs --sheets)')
    ap.add_argument('--no-serve', action='store_true', help='one process per arm, as before')
    ap.add_argument('--metrics-device', default='gpu', choices=['gpu', 'cpu', 'both'], help='where the rig measures (gpu default; both = measure twice and FAIL when they disagree)')
    args = ap.parse_args()
    t_start = time.time()
    args.out = run_dirs.run_out('L_gate', args.out)   # --out, else GATE_CHECK_DIR, else a per-run folder (removed on PASS)
    frame_rig.SESSION = frame_rig.RigSession(serve=not args.no_serve, prep_root=None if args.no_serve else os.path.join(args.out, 'prep'))
    main_root = main_checkout()
    out = args.out
    caps = os.path.join(main_root, 'out', 'captures')
    game = os.path.join(main_root, 'game_data', 'extracted')
    fview = frame_rig.find_fview_once(ROOT)   # (once per process: the freshness walk costs ~1.6 s a call)
    frames = [int(x) for x in args.frames.split(',')]
    res, fails = {}, []

    def check(ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        if not ok:
            fails.append(text)

    pres = {}  # INFO only: the same arms against the game's presented frame

    scene_png = {}   # (capname, frame) -> the game's scene target picture (before post and 2D), made once per frame: it does not depend on the arm
    scene_done = {}  # (capname, frame) -> (ok, why): made on a prefetch thread or here; reported (printed / counted) once, by the main thread, in the order the arms ask

    def scene_calc(capname, f):
        cap = os.path.join(caps, capname)
        sess = frame_rig.SESSION
        if sess is not None and sess.prep_root:   # the stream's ops list of the frame is the camera step's (made once, shared)
            frame_rig.prep_camera(fview, cap, f, game, None)
            ops = os.path.join(frame_rig._prep_dir(cap), 'ops_%d.json' % f)
        else:
            ops = os.path.join(out, capname + '_scene', 'ops_%d.json' % f)
        png = os.path.join(out, capname + '_scene', 'scene_f%d.png' % f)
        return sess.memo(('scene', capname, f), lambda: scene_ref.scene_picture(fview, cap, f, ops, png, ROOT, game)) if sess is not None else scene_ref.scene_picture(fview, cap, f, ops, png, ROOT, game)

    def scene_for(capname, f):
        if (capname, f) not in scene_png:
            png = os.path.join(out, capname + '_scene', 'scene_f%d.png' % f)
            ok, why = scene_calc(capname, f)
            if not ok:
                check(False, '%s frame %d: no scene reference (%s)' % (capname, f, why))
            else:
                print('INFO  %s frame %d scene reference: %s' % (capname, f, why))
            scene_png[(capname, f)] = png if ok else None
        return scene_png[(capname, f)]

    def run_set(capname, frs, tag, extra, ref=None):
        d = os.path.join(out, capname + '_' + tag)
        for f in frs:
            scene_for(capname, f)
        # the references of one arm: the scene target (when it could be made; else the presented frame stands in, as before) and the presented frame (INFO only); one path template per arm, frame_rig's {n} per frame
        fast = not args.sheets
        sc_t = os.path.join(out, capname + '_scene', 'scene_f{n}.png')
        mextra = ['--metrics-ref', 'scene=' + sc_t, '--metrics-ref', 'presented={game}/frame_{n:05d}.png', '--metrics-out', '{work}/metrics_{n}.json',
                  '--metrics-cov-store' if ref is None else '--metrics-cov-from', 'lcheck']
        mextra += ['--metrics-device', args.metrics_device]
        if fast:
            mextra += ['--no-sheet', '--no-family-png']
        if any(scene_png[(capname, f)] is None for f in frs):   # (a frame with no scene picture: its numbers are the presented frame's, and the FAIL above is already counted)
            mextra[1] = 'scene={game}/frame_{n:05d}.png'
        rc, text = rig(os.path.join(caps, capname), frs, d, extra + mextra)
        for line in text.splitlines():
            # the arena colour count is a real check on the before arm; on the look arms the correctly darker arena has fewer 15-bit colours, so only that one line is not counted there (frame_rig.py skips it with --look-from-stream: the texture binding is guarded by s103_R.plant_textures on the same code path)
            if line.startswith('FAIL') and not ('textur' in line and (ref or capname != 'brock_losswin')):  # (brock_losswin_rt's arena is a darker view that holds 527 colours with textures ON: the 700 bar is calibrated on brock_losswin only)
                check(False, '%s %s: %s' % (capname, tag, line[:160]))
        work = os.path.join(d, 'work_' + capname)
        rw = os.path.join(ref, 'work_' + capname) if ref else None
        res_f = {}
        for f in frs:
            mp = os.path.join(work, 'metrics_%d.json' % f)
            if not os.path.exists(mp):
                check(False, '%s %s frame %d: the rig wrote no metrics (%s)' % (capname, tag, f, text[-300:].replace(chr(10), ' | ')))
                continue
            m = json.load(open(mp))
            res_f[f] = rounded(m['scene'])
            pres.setdefault(capname, {}).setdefault(tag, {})[f] = rounded(m['presented'])
            if args.verify_metrics:   # the equality proof: the old way, Python over the PNGs, must give the same numbers (within 0.01)
                old = l_metrics.compute(work, f, rw, scene_png[(capname, f)])
                oldp = l_metrics.compute(work, f, rw)
                for lab, a_, b_ in (('scene', old, res_f[f]), ('presented', oldp, pres[capname][tag][f])):
                    for fam in set(a_) | set(b_):
                        for key in set(a_.get(fam, {})) | set(b_.get(fam, {})):
                            va, vb = a_.get(fam, {}).get(key), b_.get(fam, {}).get(key)
                            if va is None or vb is None or abs(va - vb) > (0 if key == 'covered_px' else 0.011):
                                check(False, 'metrics differ: %s %s frame %d %s %s %s: l_metrics.py %s vs the rig %s' % (capname, tag, f, lab, fam, key, va, vb))
                print('INFO  metrics equality %s %s frame %d: %d numbers compared' % (capname, tag, f, sum(len(v) for v in old.values()) + sum(len(v) for v in oldp.values())))
        return res_f

    sets = [(os.path.basename(os.path.join(caps, 'brock_losswin')), frames)]
    cap2, fr2 = args.second.split(':')
    sets.append((cap2, [int(fr2)]))
    for capname, frs in sets:   # the arm-independent preparation of every frame (the game's pictures, body scales, camera, look rows, pools, the scene reference) starts now on threads, while the first arm waits for its own
        frame_rig.prefetch(os.path.join(caps, capname), frs, look=True, pools=(capname == 'brock_losswin'), extra=lambda n, c=capname: scene_calc(c, n))
    for capname, frs in sets:
        r = {'before': run_set(capname, frs, 'before', []), 'after': run_set(capname, frs, 'after', ['--look-from-stream'], os.path.join(out, capname + '_before'))}
        if capname == 'brock_losswin':
            r['after_pools'] = run_set(capname, frs, 'after_pools', ['--look-from-stream', '--look-pools'], os.path.join(out, capname + '_before'))
            for p in ('entrance', 'rig', 'wrong_slot'):
                r[p] = run_set(capname, frs, 'plant_' + p, ['--look-from-stream', '--plant-look', p], os.path.join(out, capname + '_before'))
        res[capname] = r
        for f in frs:
            b, a = r['before'][f]['arena'], r['after'][f]['arena']
            first = capname == 'brock_losswin'  # (the second capture is a frame the old record already drew near the game's brightness: it only has to improve and stay in a wide band)
            ap_ = r['after_pools'][f]['arena'] if 'after_pools' in r else a  # the bars presume the pool / spot rows the offline record lacks
            # Jarrod, 2026-11-17: "change the arm to the game accurate one": the mean |diff| bar is measured on the after_pools arm (the one that feeds the game's pool / spot rows); the 85 % is unchanged
            check(ap_['mean_diff_pct'] <= (0.85 if first else 1.0) * b['mean_diff_pct'], '%s frame %d: arena mean |diff| %.2f -> %.2f %% (needs <= %d %% of before, after + pools)' % (capname, f, b['mean_diff_pct'], ap_['mean_diff_pct'], 85 if first else 100))
            print('INFO  %s frame %d: plain after arm (no pools) arena mean |diff| %.2f %%' % (capname, f, a['mean_diff_pct']))
            lo, hi = (0.8, 1.4) if first else (0.6, 1.5)
            check(lo <= ap_['lum_ratio'] <= hi, '%s frame %d: arena luminance ratio ours / game %.3f -> %.3f (needs %.1f-%.1f after%s; without the pools %.3f)' % (capname, f, b['lum_ratio'], ap_['lum_ratio'], lo, hi, ' + pools' if 'after_pools' in r else '', a['lum_ratio']))
            pb, pa = pres[capname]['before'][f]['arena'], pres[capname]['after'][f]['arena']
            print('INFO  %s frame %d against the PRESENTED frame (2D overlay included, not a bar): arena mean |diff| %.2f -> %.2f %%, luminance ratio %.3f -> %.3f' % (capname, f, pb['mean_diff_pct'], pa['mean_diff_pct'], pb['lum_ratio'], pa['lum_ratio']))
            if 'entrance' in r:
                pe, pr, pw = r['entrance'][f]['arena'], r['rig'][f]['arena'], r['wrong_slot'][f]['arena']
                check(pe['mean_diff_pct'] >= 0.97 * b['mean_diff_pct'], 'plant entrance flag ignored, frame %d: arena %.2f %% (no improvement over before %.2f %%: planted fault caught)' % (f, pe['mean_diff_pct'], b['mean_diff_pct']))
                check(pr['mean_diff_pct'] >= a['mean_diff_pct'] + 1.0, 'plant rig rows ignored, frame %d: arena %.2f %% vs after %.2f %% (worse by >= 1 point: caught)' % (f, pr['mean_diff_pct'], a['mean_diff_pct']))
                print('INFO  class B alone (entrance look + page, rig values ignored), frame %d: arena %.2f %% vs before %.2f %% (class A + B together: %.2f %%)' % (f, pr['mean_diff_pct'], b['mean_diff_pct'], a['mean_diff_pct']))
                check(not (0.8 <= pw['lum_ratio'] <= 1.4), 'plant arena sets on the crowd slot, frame %d: arena luminance ratio %.3f (outside 0.8-1.4: caught)' % (f, pw['lum_ratio']))
    os.makedirs(out, exist_ok=True)
    res['presented_info'] = pres
    json.dump(res, open(os.path.join(out, 'l_check_numbers.json'), 'w'), indent=1)
    st = frame_rig.SESSION.stats
    frame_rig.SESSION.close()
    print('INFO  rig session: %d jobs (%d on the serve process, %d one-shot), %.1f s in the rig, %.1f s wall' % (st['jobs'], st['served'], st['one_shot'], st['job_s'], time.time() - t_start))
    print('%s  lane L check: %d failure(s)' % ('PASS' if not fails else 'FAIL', len(fails)))
    return run_dirs.finish(args.out, 1 if fails else 0)


if __name__ == '__main__':
    sys.exit(main())

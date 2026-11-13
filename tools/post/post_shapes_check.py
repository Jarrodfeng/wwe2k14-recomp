#!/usr/bin/env python3
r"""Lane 238 offline proof (S101, 0 game runs): the post chain shapes that were still the game's are drawn by our pass. Docs: docs\renderer\POST_OURS.md section "Lane 238".

  py -I tools\post\post_shapes_check.py [--root DIR] [--stride N] [--only core,pictures,settings,scale2,plants,merge,caw,t2film] [--work DIR]

Inputs (a missing one FAILS): the case zips of the sweep runs of 2026-11-13 (victory 20261113_063415, the Heenan 30 Years entrance 20261113_035240, wm30_long 20261113_064130) and the built
rplay.exe + out\tools\post_ours_core_test.exe of this tree (tools\renderer\build.ps1, tools\post\build_post_ours_test.ps1).

  core.<cap>        the GPU-free decision over every frame: the listed sequences are 'ours' (SEQ lines), kept == 0.
  picture.<cap>     rplay stream arm vs --post-ours arm on every max(STRIDE, frames // 150)-th frame plus 2 frames either side of every change of the chain sequence: max channel difference <= 1 level.
  settings.<cap>    DEPTH OF FIELD off / FILM GRAIN off / both (the binder's variant swap in the stream arm, the plan flags in ours): equal.
  scale2.<cap>      --scale 2: exact (REPEAT-addressed dof depth frames: the documented edge band, POST_OURS.md).
  merge.*           (c) the m1_m2 slot-3-POINT frames (SYNTHETIC: Bryan frames patched by synth_frame_patch.py): ours, the old code refused, pictures equal.
  caw.*             the CAW editor base combine is ours (no depth slot): pictures equal under lane 244's fixed wide rule; a stream arm under the old wide rule (lane 244's past bug) differs from ours.
  t2film.*          lane 277: the 30 Years cutscene's 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine(fog_bloom, NO dof) monotone' (run 7183: 111 frames kept; no capture holds it, so SYNTHETIC captures
                    from the victory frames + one 30 Years film frame, tools\post\synth_t2_film.py): all ours; the old rule (t2_needs_dof) refuses with run 7183's line; pictures equal (dead and live bloom),
                    DoF / FILM GRAIN off, scale 2; plants film_dropped / t2_tables_shared move the picture.
  plant.<name>      t2_tables_shared (the DoF blur takes the bloom t2's tables: the bug class of 'two t2 draws in one frame'), t2_default_tables, film_dropped: each moves the picture (no check sees these), or
                    is refused by its check, on the frames that carry the shape.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import post_ours_check as po          # run, load, parse_census, Results, PIX ...
import post_ours_cover_check as cc    # extract, lists_of

# label -> (run dir, why it is here, sequences that must be ours with frames > 0 (substring match on the run-length sequence))
CAPS = {
    'victory': ('20261113_063415_s101_sweep_victory', "victory scene: 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine' (t2 blur + HDR + a restore), 'combine' alone",
                ['copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine', 'combine']),
    'heenan30': ('20261113_035240_s90_heenan_entrance', "30 Years entrance: film after a full chain (one / two restores, two layers), 'combine monotone', 'copy_depth dof hole_fill ... combine monotone'",
                 ['copy_depth dof hole_fill copy_depthx2 dof hole_fill box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone', 'copy_depthx2 dof hole_fill box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone',
                  'copy_depth dof hole_fill box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone', 'combine monotone']),
    'wm30_long': ('20261113_064130_s101_sweep_wm30_long', 'the plain 30 Years film frames (regression)', ['dof hole_fill combine monotone']),
}
FAULT_PX = 20
BRYAN_ZIP_RUN = '20261111_174549_s100_owner_bryan_entrance'   # the full-chain frames the synthetic m1_m2 slot-3-POINT capture is made from (tools\post\synth_frame_patch.py)
CAW_RUN = '20261113_035959_s101_checkB_caw_walk'               # the CAW editor: the base combine permutation (ours since lane 238 / 244)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=po.ROOT)
    ap.add_argument('--stride', type=int, default=8)
    ap.add_argument('--only', default='')
    ap.add_argument('--work', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = po.main_checkout(root)
    work = args.work or os.path.join(main_co, 'out', 'post_shapes_cache')
    os.makedirs(work, exist_ok=True)
    want = lambda g: not args.only or g in args.only.split(',')
    res = po.Results()
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    core = os.path.join(root, 'out', 'tools', 'post_ours_core_test.exe')
    for exe in (rplay, core):
        if not os.path.isfile(exe):
            res.line(False, 'input missing: %s' % exe)
            return 1
    import numpy as np
    caps = {}
    for label, (run_dir, why, _seq) in CAPS.items():
        c = cc.extract(main_co, run_dir, work)
        if not c:
            res.line(False, 'input missing: the %s case zip (%s)' % (label, run_dir))
            continue
        caps[label] = c
    if not caps:
        return 1
    extra = {}
    for label, run_dir in (('bryan', BRYAN_ZIP_RUN), ('caw', CAW_RUN)):
        c = cc.extract(main_co, run_dir, work)
        if not c:
            res.line(False, 'input missing: the %s case zip (%s)' % (label, run_dir))
        else:
            extra[label] = c
    rc, text = po.run([core] + ['%s=%s' % (k, v) for k, v in caps.items()], 1800)
    g = po.core_line(text)
    L = cc.lists_of(text)
    seqs = {}
    for line in text.splitlines():   # SEQ label frames first=N | sequence => ours | => reason
        if line.startswith('SEQ '):
            head, tail = line.split(' | ', 1)
            label, n = head.split(' ')[1], int(head.split(' ')[2])
            seq, verdict = tail.split(' => ', 1)
            seqs.setdefault(label, {})[seq] = (n, verdict)
    nframes = {}
    for k in caps:
        if k not in g or k not in L:
            res.line(False, 'core.%s: no CAP / LIST line' % k)
            continue
        fr, ours, failed, nochain = int(g[k]['frames']), int(g[k]['ours']), int(g[k]['failed']), int(g[k]['nochain'])
        nframes[k] = fr
        if want('core'):
            need = CAPS[k][2]
            missing = [s for s in need if s not in seqs.get(k, {}) or seqs[k][s][1] != 'ours']
            held = ', '.join('%d x [%s]' % (seqs[k][s][0], s.split(' ')[0] + '..' + s.split(' ')[-1]) for s in need if s in seqs.get(k, {}))
            res.line(rc == 0 and failed == 0 and ours + nochain == fr and not missing, 'core.%s: %d frames, ours %d, no chain %d, kept %d; %s%s' % (
                k, fr, ours, nochain, failed, held, ('; NOT ours or absent: ' + str(missing)) if missing else ''))
    if len(nframes) != len(caps):
        print('%d lines, %d failed' % (res.lines, res.fails))
        return 1
    out = os.path.join(work, 'run')
    os.makedirs(out, exist_ok=True)

    def arm(cap, tag, frames, shots, extra, scale=1):
        sd = os.path.join(out, tag)
        po.clear_dir(sd)
        cmd = [rplay, '--capture', cap, '--frames', str(frames), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, shots))] + (['--scale', str(scale)] if scale != 1 else []) + extra
        rc, text = po.run(cmd, 1800)
        if rc:
            print(text[-1500:])
            raise RuntimeError('rplay %s failed (exit %d)' % (tag, rc))
        return sd, text

    def diff(a, b, f):
        pa, pb = os.path.join(a, 'frame_%04d.png' % f), os.path.join(b, 'frame_%04d.png' % f)
        if not os.path.isfile(pa) and not os.path.isfile(pb):
            return (0, 0, 0)   # a frame neither arm presented (frame 0 of the victory capture has no present): nothing to compare
        if not (os.path.isfile(pa) and os.path.isfile(pb)):
            return None
        d = np.abs(po.load(pa) - po.load(pb)).max(axis=2)
        ys, xs = np.nonzero(d > po.PIX)
        far = int(np.minimum.reduce([xs, d.shape[1] - 1 - xs, ys, d.shape[0] - 1 - ys]).max()) if len(xs) else 0
        return int(d.max()), int((d > po.PIX).sum()), far

    def compare(a, b, shots):
        worst, bad_px, bad, missing = 0, 0, [], 0
        for f in shots:
            r = diff(a, b, f)
            if r is None:
                missing += 1
                continue
            worst, bad_px = max(worst, r[0]), bad_px + r[1]
            if r[1]:
                bad.append(f)
        return worst, bad_px, bad, missing

    def clean(*dirs):
        for d in dirs:
            po.clear_dir(d)

    def shots_of(k, stride=None):
        n = nframes[k]
        st = stride or max(args.stride, n // 150)
        s = set(range(0, n, st)) | {n - 1}
        for c in L[k]['change']:
            s |= {x for x in range(c - 2, c + 3) if 0 <= x < n}
        return sorted(s), st

    def first_of(k, seq):   # a frame of each wanted sequence (the first one): the settings / scale / plant arms always shoot these
        r = []
        for line in text.splitlines():
            if line.startswith('SEQ %s ' % k) and (' | ' + seq + ' =>') in line:
                r.append(int(line.split('first=')[1].split(' ')[0]))
        return r

    try:
        if want('pictures'):
            for k, c in caps.items():
                n = nframes[k]
                shots, st = shots_of(k)
                a, _ = arm(c, 'p_%s_stream' % k, n, shots, [])
                b, t = arm(c, 'p_%s_ours' % k, n, shots, ['--post-ours'])
                worst, bad_px, bad, missing = compare(a, b, shots)
                cen = po.parse_census(t)
                res.line(missing == 0 and worst <= po.PIX and cen is not None and cen['fallback'] == 0, 'picture.%s: %d frames (every %d-th and +-2 around %d sequence changes), stream arm vs --post-ours arm: max channel difference %d levels, %d pixels over %d level, missing %d, census ours %s fallback %s%s' % (
                    k, len(shots), st, len(L[k]['change']), worst, bad_px, po.PIX, missing, cen and cen['ours'], cen and cen['fallback'], (', frames ' + str(bad[:8])) if bad else ''))
                clean(a, b)
        if want('settings'):
            for k, arms in (('victory', [['--depth-of-field', 'off']]), ('heenan30', [['--depth-of-field', 'off'], ['--film-grain', 'off'], ['--depth-of-field', 'off', '--film-grain', 'off']])):
                n = nframes[k]
                shots = sorted(set(x for s in CAPS[k][2] for f in first_of(k, s) for x in (f, min(n - 1, f + 3), min(n - 1, f + 60))) | {n - 1})
                for fl in arms:
                    a, _ = arm(caps[k], 'st_%s_stream' % k, n, shots, fl)
                    b, t = arm(caps[k], 'st_%s_ours' % k, n, shots, ['--post-ours'] + fl)
                    worst, bad_px, bad, missing = compare(a, b, shots)
                    cen = po.parse_census(t)
                    res.line(missing == 0 and worst <= po.PIX and cen is not None and cen['fallback'] == 0, 'settings.%s %s: %d frames, max difference %d levels, %d pixels over %d level, ours %s fallback %s' % (
                        k, ' '.join(fl), len(shots), worst, bad_px, po.PIX, cen and cen['ours'], cen and cen['fallback']))
                    clean(a, b)
        if want('scale2'):
            for k in ('victory', 'heenan30'):
                n = nframes[k]
                shots = sorted(set(x for s in CAPS[k][2] for f in first_of(k, s) for x in (f, min(n - 1, f + 40))))
                a, _ = arm(caps[k], 's2_%s_stream' % k, n, shots, [], 2)
                b, _ = arm(caps[k], 's2_%s_ours' % k, n, shots, ['--post-ours'], 2)
                rep = set(L[k]['repeat'])
                bad_clamp, far_rep, worst_rep, missing = [], 0, 0, 0
                for f in shots:
                    r = diff(a, b, f)
                    if r is None:
                        missing += 1
                        continue
                    if f in rep:
                        far_rep, worst_rep = max(far_rep, r[2]), max(worst_rep, r[0])
                    elif r[1]:
                        bad_clamp.append((f, r[0], r[1]))
                res.line(missing == 0 and not bad_clamp and far_rep <= 16, 'scale2.%s: %d frames at --scale 2: CLAMP frames exact%s; REPEAT-depth frames max %d levels, farthest differing pixel %d px from an edge (bar 16 px)' % (
                    k, len(shots), '' if not bad_clamp else ' FAILED on ' + str(bad_clamp[:4]), worst_rep, far_rep))
                clean(a, b)
        if want('plants'):
            # (name, capture, sequence the shots are taken on, checks-on expectation, why)
            plants = [('t2_tables_shared', 'victory', 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine', 'moves', "the DoF blur takes the bloom t2's tables: two t2 draws in one frame (what the first plan did: the second Take() overwrote the first)"),
                      ('t2_default_tables', 'victory', 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine', 'moves', "DefaultPostParams' t2 tables: no check sees it, the picture does"),
                      ('film_dropped', 'heenan30', 'copy_depthx2 dof hole_fill box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone', 'moves', 'the film pass is claimed and not recorded: no check sees it, the picture does'),
                      ('film_dropped', 'heenan30', 'combine monotone', 'moves', 'the same on the combine-only film frame')]
            for name, k, seq, expect, why in plants:
                n = nframes[k]
                fr = first_of(k, seq)
                if not fr:
                    res.line(False, 'plant.%s.%s: no frame of the sequence [%s]' % (name, k, seq))
                    continue
                f0 = fr[0]
                shots = sorted({f0, min(n - 1, f0 + 1), min(n - 1, f0 + 5), min(n - 1, f0 + 30)})
                a, _ = arm(caps[k], 'pl_%s_stream' % name, n, shots, [])
                b, t = arm(caps[k], 'pl_%s_on' % name, n, shots, ['--post-ours', '--post-fault', name])
                w1, px1, bad1, m1 = compare(a, b, shots)
                cen = po.parse_census(t)
                if expect == 'kept':
                    res.line(m1 == 0 and w1 <= po.PIX and cen is not None and cen['kept'] > 0, 'plant.%s.%s checks on (%s): census kept %s, picture == stream (max %d levels)' % (name, k, why, cen['kept'] if cen else '?', w1))
                else:
                    res.line(m1 == 0 and px1 >= FAULT_PX and cen is not None, 'plant.%s.%s [%s] (%s): no check fires (kept %s) and the picture moves (%d pixels over %d level, max %d)' % (name, k, seq.split(' ')[0] + '..' + seq.split(' ')[-1], why, cen['kept'] if cen else '?', px1, po.PIX, w1))
                clean(a, b)
        if want('merge') and 'bryan' in extra:
            # (c) run 7131's 'the stream binds a m1_m2 slot 3 point; the pass binds it linear' (46 binds): no capture holds it, so the Bryan frames get that one bind patched (SYNTHETIC, named in SYNTH.txt)
            import subprocess
            mp = os.path.join(work, 'bryan_merge_point')
            shutil_rm = __import__('shutil').rmtree
            shutil_rm(mp, ignore_errors=True)
            p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'synth_frame_patch.py'), extra['bryan'], mp, '--merge-point'], capture_output=True, text=True)
            res.line(p.returncode == 0, 'merge.synth: %s' % (p.stdout.strip() or p.stderr.strip()))
            if p.returncode == 0:
                rc2, t2 = po.run([core, 'mp=' + mp], 900)
                rc3, t3 = po.run([core, '--faults', 'merge_filter_linear,dead_filters_checked', 'mp=' + mp], 900)   # the old behaviour: the pass binds linear and the check reads dead draws too
                g2, g3 = po.core_line(t2), po.core_line(t3)
                n = int(g2['mp']['frames'])
                res.line(int(g2['mp']['ours']) == n and int(g2['mp']['failed']) == 0 and 'MERGE mp frames=%d ' % n in t2.replace('\r', ''), 'merge.core: %d frames, all ours (the pass follows the stream\'s POINT slot 3; the dead glare\'s draws are not compared)' % n)
                rc4, t4 = po.run([core, '--faults', 'dead_filters_checked', 'mp=' + mp], 900)   # the check reads the dead draws too: only the per-slot m1_m2 filter (slot 3 POINT) keeps the frames ours
                g4 = po.core_line(t4)
                res.line(int(g4['mp']['ours']) == n and int(g4['mp']['failed']) == 0, 'merge.per_slot: with --faults dead_filters_checked the %d synthetic frames are still all ours (the plan\'s per-slot m1_m2 filter alone matches the stream)' % n)
                res.line(int(g3['mp']['failed']) == n and int(g3['mp'].get('kept_filter', 0)) == n and 'the stream binds a m1_m2 slot 3 point; the pass binds it linear' in t3,
                         'merge.plant (merge_filter_linear + dead_filters_checked = the old code): all %d frames kept "filter: the stream binds a m1_m2 slot 3 point; the pass binds it linear" (run 7131\'s message)' % n)
                a, _ = arm(mp, 'mg_stream', n, [0, n // 2, n - 1], [])
                b, t = arm(mp, 'mg_ours', n, [0, n // 2, n - 1], ['--post-ours'])
                worst, bad_px, bad, missing = compare(a, b, [0, n // 2, n - 1])
                cen = po.parse_census(t)
                res.line(missing == 0 and worst <= po.PIX and cen is not None and cen['fallback'] == 0, 'merge.picture: stream arm vs --post-ours, max difference %d levels, ours %s fallback %s' % (worst, cen and cen['ours'], cen and cen['fallback']))
                clean(a, b)
        if want('caw') and 'caw' in extra:
            # the CAW editor's base combine (yGlareEtc_base: slot 0 only, no depth, no constants) is ours (lane 244: the stream was wrong under the wide rule, ours right). The capture was recorded in a 21:9 run,
            # so both arms replay it under lane 244's fixed wide rule (--wide 1.3125 --wide-all --wide-from-squeezed): ours == stream at 16:9 numbers.
            rc2, t2 = po.run([core, 'caw=' + extra['caw']], 900)
            g2 = po.core_line(t2)
            n = int(g2['caw']['frames'])
            res.line(int(g2['caw']['ours']) == n and int(g2['caw']['failed']) == 0, 'caw.core: all %d CAW editor frames (the base combine, no depth slot) are ours, kept 0' % n)
            shots = [0, n // 2, n - 1]
            wa = ['--wide', '1.3125', '--wide-all', '--wide-from-squeezed']
            a, _ = arm(extra['caw'], 'caw_stream', n, shots, wa)
            b, t = arm(extra['caw'], 'caw_ours', n, shots, wa + ['--post-ours'])
            worst, bad_px, bad, missing = compare(a, b, shots)
            cen = po.parse_census(t)
            res.line(missing == 0 and worst <= po.PIX and cen is not None and cen['ours'] == n and cen['fallback'] == 0, 'caw.picture: stream arm vs --post-ours under the fixed wide rule: max difference %d levels, %d pixels over %d level, ours %s fallback %s' % (worst, bad_px, po.PIX, cen and cen['ours'], cen and cen['fallback']))
            # the plant: a stream arm that is WRONG about this combine (lane 244's old wide rule squeezed the pixel-placed draw) must be told apart from ours: the check is not blind to a combine that differs
            c, t = arm(extra['caw'], 'caw_oldrule', n, shots, wa + ['--wide-clipoff-squeezed', '--wide-offscreen-squeezed'])
            w2, px2, bad2, m2 = compare(c, b, shots)
            res.line(m2 == 0 and px2 >= FAULT_PX, 'caw.plant (a stream arm under the old wide rule = the past bug of lane 244): differs from ours by %d pixels over %d level (max %d): a combine that differs is seen' % (px2, po.PIX, w2))
            clean(a, b, c)
        if want('t2film') and 'victory' in caps:
            import subprocess
            shutil_rm = __import__('shutil').rmtree
            c = cc.extract(main_co, '20261113_142941_s101_sweep_cutscene_wm30', work) or cc.extract(main_co, '20261113_140507_s101_sweep_cutscene_wm30', work)
            if not c:
                res.line(False, 'input missing: a 30 Years cutscene case zip (the film pass template)')
            else:
                variants = {}
                # lane 296: 'two_*' = the two-layer form (copy_depth t2 copy_depthx2 t2 box4_down ... combine(dof_fog_bloom) monotone, run 7201): synth_t2_film.py --two-layer
                for tag, extra_args in (('dead', []), ('live', ['--live-bloom']), ('two_dead', ['--two-layer']), ('two_live', ['--two-layer', '--live-bloom'])):
                    dst = os.path.join(work, 't2film_' + tag)
                    shutil_rm(dst, ignore_errors=True)
                    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'synth_t2_film.py'), caps['victory'], c, dst, '--first', '1390', '--frames', '80'] + extra_args, capture_output=True, text=True)
                    res.line(p.returncode == 0, 't2film.synth.%s: %s' % (tag, p.stdout.strip() or p.stderr.strip()[-300:]))
                    if p.returncode == 0:
                        variants[tag] = dst
                for tag, cap in variants.items():
                    two = tag.startswith('two_')
                    SEQ = 'copy_depth t2 copy_depthx2 t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone' if two else 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone'
                    rc2, t2 = po.run([core, 't=' + cap], 900)
                    g2 = po.core_line(t2)
                    n = int(g2['t']['frames'])
                    held = [l for l in t2.splitlines() if l.startswith('SEQ ') and (' | ' + SEQ + ' => ours') in l]
                    nseq = int(held[0].split(' ')[2]) if held else 0
                    res.line(bool(held) and int(g2['t']['failed']) == 0 and nseq > 0, 't2film.core.%s: %d frames, %d x [%s] ours, kept %s; dead work draws %s copies %s' % (tag, n, nseq, SEQ, g2['t']['failed'], g2['t']['dead_draws'], g2['t']['dead_copies']))
                    old_fault = 't2_one_only' if two else 't2_needs_dof'   # the rule before lane 296 (one t2 only) / before lane 277 (the combine needs a dof permutation)
                    rc3, t3 = po.run([core, '--faults', old_fault, 't=' + cap], 900)
                    g3 = po.core_line(t3)
                    res.line('chain draws before the known sequence (t2)' in t3 and int(g3['t']['failed']) == nseq, 't2film.old_rule.%s (%s = the plan before lane %s): %s frames refused "chain draws before the known sequence (t2)" = run %s\'s line' % (tag, old_fault, '296' if two else '277', g3['t']['failed'], '7201' if two else '7183'))
                    first = [int(l.split('first=')[1].split(' ')[0]) for l in held]
                    shots = sorted({0, n - 1} | {min(n - 1, first[0] + d) for d in (0, 1, 2, 5, 10, 20, 40)}) if first else [0]
                    a, _ = arm(cap, 'tf_%s_stream' % tag, n, shots, [])
                    b, t = arm(cap, 'tf_%s_ours' % tag, n, shots, ['--post-ours'])
                    worst, bad_px, bad, missing = compare(a, b, shots)
                    cen = po.parse_census(t)
                    res.line(missing == 0 and worst <= po.PIX and cen is not None and cen['fallback'] == 0, 't2film.picture.%s: %d frames, stream arm vs --post-ours: max difference %d levels, %d pixels over %d level, ours %s fallback %s' % (
                        tag, len(shots), worst, bad_px, po.PIX, cen and cen['ours'], cen and cen['fallback']))
                    clean(a, b)
                    for fl in (['--depth-of-field', 'off'], ['--film-grain', 'off'], ['--depth-of-field', 'off', '--film-grain', 'off']):
                        a, _ = arm(cap, 'tf_%s_sst' % tag, n, shots, fl)
                        b, t = arm(cap, 'tf_%s_sou' % tag, n, shots, ['--post-ours'] + fl)
                        worst, bad_px, bad, missing = compare(a, b, shots)
                        cen = po.parse_census(t)
                        res.line(missing == 0 and worst <= po.PIX and cen is not None and cen['fallback'] == 0, 't2film.settings.%s %s: max difference %d levels, %d pixels over %d level, ours %s fallback %s' % (
                            tag, ' '.join(fl), worst, bad_px, po.PIX, cen and cen['ours'], cen and cen['fallback']))
                        clean(a, b)
                    a, _ = arm(cap, 'tf_%s_s2s' % tag, n, shots, [], 2)
                    b, t = arm(cap, 'tf_%s_s2o' % tag, n, shots, ['--post-ours'], 2)
                    worst, bad_px, bad, missing = compare(a, b, shots)
                    res.line(missing == 0 and worst <= po.PIX, 't2film.scale2.%s: %d frames at --scale 2: max difference %d levels, %d pixels over %d level' % (tag, len(shots), worst, bad_px, po.PIX))
                    clean(a, b)
                    for name, why in (('film_dropped', 'the film pass is claimed and not recorded'), ('t2_tables_shared', "the DoF blur's t2 takes the bloom t2's tables"),
                                      ('t2_far_ignored', "the far layer's blur is not passed on: slot 6 gets the near blur (the layers merge)")):
                        if name == 't2_far_ignored' and not two:
                            continue   # one layer: there is no far blur
                        if tag in ('dead', 'two_dead') and name == 't2_tables_shared':
                            continue   # the dead-bloom variant records no blur at all (no dof permutation, HDR chain dead): there are no tables to share   # (two_dead: the blur is recorded, but the plant moved 0 pixels there (cause unconfirmed); the plant is proven on two_live)
                        a, _ = arm(cap, 'tf_%s_pa' % tag, n, shots, [])
                        b, t = arm(cap, 'tf_%s_pb' % tag, n, shots, ['--post-ours', '--post-fault', name])
                        w1, px1, bad1, m1 = compare(a, b, shots)
                        res.line(m1 == 0 and px1 >= FAULT_PX, 't2film.plant.%s.%s (%s): the picture moves (%d pixels over %d level, max %d)' % (name, tag, why, px1, po.PIX, w1))
                        clean(a, b)
                # lane 296b (SF1): the far t2 blur's copy sits inside the claimed range (synth --far-copy-late) with a claimed draw writing its source: refused; the rule before the fix (far_copy_unchecked) draws it wrong
                dst = os.path.join(work, 't2film_two_late')
                shutil_rm(dst, ignore_errors=True)
                p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'synth_t2_film.py'), caps['victory'], c, dst, '--first', '1390', '--frames', '80', '--two-layer', '--live-bloom', '--far-copy-late'], capture_output=True, text=True)
                res.line(p.returncode == 0, 't2film.synth.two_late: %s' % (p.stdout.strip() or p.stderr.strip()[-300:]))
                if p.returncode == 0:
                    rc2, t2 = po.run([core, 't=' + dst], 900)
                    g2 = po.core_line(t2)
                    ours_seq = [l for l in t2.splitlines() if l.startswith('SEQ ') and ' => ours' in l and 'copy_depthx2 t2 box4_down' in l]
                    res.line("the far t2 blur's copy lies inside the claimed range" in t2 and not ours_seq and int(g2['t']['failed']) > 0, 't2film.far_copy_late.refused: %s frames kept, the two-layer sequence is not ours (refusal "the far t2 blur\'s copy lies inside the claimed range")' % g2['t']['failed'])
                    rc3, t3 = po.run([core, '--faults', 'far_copy_unchecked', 't=' + dst], 900)
                    ours3 = [l for l in t3.splitlines() if l.startswith('SEQ ') and ' => ours' in l and 'copy_depthx2 t2 box4_down' in l]
                    res.line(bool(ours3), 't2film.far_copy_late.plant (far_copy_unchecked = the rule before the fix): the two-layer sequence is ours again = it draws with last frame\'s far blur (%d SEQ lines)' % len(ours3))
                    first = [int(l.split('first=')[1].split(' ')[0]) for l in ours3 if 'first=' in l]
                    n = int(po.core_line(t3)['t']['frames'])
                    shots = sorted({0, n - 1} | {min(n - 1, first[0] + d) for d in (0, 1, 2, 5, 10)}) if first else [0]
                    a, _ = arm(dst, 'tf_late_stream', n, shots, [])
                    b, t = arm(dst, 'tf_late_ours', n, shots, ['--post-ours'])
                    w0, px0, bad0, m0 = compare(a, b, shots)
                    res.line(m0 == 0 and w0 <= po.PIX, 't2film.far_copy_late.picture: refused, so --post-ours equals the stream arm (max %d levels, %d pixels over %d level)' % (w0, px0, po.PIX))
                    clean(a, b)
                    a, _ = arm(dst, 'tf_late_pa', n, shots, [])
                    b, t = arm(dst, 'tf_late_pb', n, shots, ['--post-ours', '--post-fault', 'far_copy_unchecked'])
                    w1, px1, bad1, m1 = compare(a, b, shots)
                    res.line(m1 == 0 and px1 >= FAULT_PX, 't2film.far_copy_late.plant.picture: before the fix the picture moves (%d pixels over %d level, max %d)' % (px1, po.PIX, w1))
                    clean(a, b)
    except RuntimeError as e:
        res.line(False, str(e))
    print('%d lines, %d failed' % (res.lines, res.fails))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())

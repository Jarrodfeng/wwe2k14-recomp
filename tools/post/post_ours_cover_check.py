#!/usr/bin/env python3
r"""Lane 140 offline proof (S101, 0 game runs): the post chain's COVERAGE. Every frame shape the Check J runs (7086 / 7087 / 7088) refused and that the captures hold is drawn by our pass and the picture stays inside
the post gate's bar. Docs: docs\renderer\POST_OURS.md section "Coverage (lane 140)".

  py -I tools\post\post_ours_cover_check.py [--root DIR] [--stride N] [--only core,pictures,scale2,plants]

Inputs (a missing one FAILS): the Check J case zips (out\s101\runs\20261112_1723.. / _1724.. / _1725..) and the older runs' case captures that hold the refused shapes (the main checkout's out\s101\runs), the
built rplay.exe + out\tools\post_ours_core_test.exe of this tree.

  core.<cap>      the GPU-free decision over every frame: kept == 0 for the shapes this lane covers.
  picture.<cap>   rplay stream arm vs --post-ours arm on every max(STRIDE, frames // 150)-th frame PLUS the frames within 2 of every change of the chain sequence (review F7): max channel difference <= 1 level.
  picture.scale2  --scale 2 on spaced frames plus one t2 frame and one restore frame: a frame whose dof depth is bound REPEAT may differ within 8 game pixels (16) of an edge; every other frame must be exact (review F6).
  plant.<name>    old_restore_rule, t2_source_stale, t2_slots_foreign, t2_default_tables, claim_from_restore, shape_misclassified: checks on -> the frames stay the stream's (or, for t2_default_tables, which no check
                  sees, the picture moves); ,nocheck -> the picture moves where the plant can reach it.
  limits          --post-limits no_r32f_linear on brock keeps its linear frames the stream's.
"""
import argparse
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import post_ours_check as po   # the helpers (run, pngs_diff, parse_census, ...) are shared

FAULT_PX = 20
# label -> (run dir, why it is here)
CAPS = {
    'j1_brock':   ('20261112_172304_s100_owner_s89_brock', 'Check J1: Brock entrance'),
    'j1b_match':  ('20261112_172456_s100_owner_match', 'Check J1b: in-ring match'),
    'j2_wm30':    ('20261112_172550_s100_owner_s90_wm30', 'Check J2: 30 Years'),
    'a_brock':    ('20261111_174957_s100_owner_s89_brock', "match shape, dof depth bound POINT with no restore (293 frames kept 'filter' before)"),
    'checkB_director': ('checkB_director', "14 'filter' frames (point depth, no restore)"),
    'checkC_director': ('checkC_director', "4911 frames 'copy_depth dof hole_fill combine' (a restore in front of the one layer: 'shape' before)"),
    'a_wm30':     ('20261112_131029_s100_owner_s90_wm30', "the t2 blur shape (163 frames kept 'shape' before)"),
}


def extract(main, run_dir, work):
    d = os.path.join(main, 'out', 's101', 'runs', run_dir)
    zips = sorted(f for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')) if os.path.isdir(d) else []
    if not zips:
        return None
    base = os.path.join(work, run_dir)
    if not os.path.isdir(os.path.join(base, 'capture')):
        z = zipfile.ZipFile(os.path.join(d, zips[0]))
        z.extractall(base, [n for n in z.namelist() if n.startswith('capture/')])
    return os.path.join(base, 'capture')


def lists_of(text):
    """'LIST label name a,b,c' -> {label: {name: [ints]}}"""
    out = {}
    for line in text.splitlines():
        if not line.startswith('LIST '):
            continue
        parts = line.strip().split(' ')
        label, name = parts[1], parts[2]
        vals = [int(x) for x in parts[3].split(',')] if len(parts) > 3 and parts[3] else []
        out.setdefault(label, {})[name] = vals
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=po.ROOT)
    ap.add_argument('--stride', type=int, default=8)
    ap.add_argument('--only', default='')
    ap.add_argument('--work', default='')
    args = ap.parse_args()
    import numpy as np
    root = os.path.abspath(args.root)
    main_co = po.main_checkout(root)
    work = args.work or os.path.join(main_co, 'out', 'post_cover_cache')
    os.makedirs(work, exist_ok=True)
    want = lambda g: not args.only or g in args.only.split(',')
    res = po.Results()
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    core = os.path.join(root, 'out', 'tools', 'post_ours_core_test.exe')
    for exe in (rplay, core):
        if not os.path.isfile(exe):
            res.line(False, 'input missing: %s' % exe)
            return 1
    caps = {}
    for label, (run_dir, why) in CAPS.items():
        c = extract(main_co, run_dir, work)
        if not c:
            res.line(False, 'input missing: the %s case zip (%s)' % (label, run_dir))
            continue
        caps[label] = c
    if not caps:
        return 1
    # ---- core: every frame (always run: the lists feed the picture arms)
    rc, text = po.run([core] + ['%s=%s' % (k, v) for k, v in caps.items()], 1800)
    g = po.core_line(text)
    L = lists_of(text)
    nframes = {}
    for k in caps:
        if k not in g or k not in L:
            res.line(False, 'core.%s: no CAP / LIST line' % k)
            continue
        fr, ours, failed, nochain = int(g[k]['frames']), int(g[k]['ours']), int(g[k]['failed']), int(g[k]['nochain'])
        nframes[k] = fr
        kept = {x: g[k][x] for x in g[k] if x.startswith('kept_')}
        if want('core'):
            res.line(rc == 0 and failed == 0 and ours + nochain == fr, 'core.%s: %d frames, ours %d, no chain %d, kept %d %s (t2 blur frames %d, restore-in-front %d, REPEAT dof depth %d, sequence changes %d)' % (
                k, fr, ours, nochain, failed, kept or '', len(L[k]['t2']), len(L[k]['restore']), len(L[k]['repeat']), len(L[k]['change'])))
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
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))

    def shots_of(k, stride=None):
        n = nframes[k]
        st = stride or max(args.stride, n // 150)
        s = set(range(0, n, st)) | {n - 1}
        for c in L[k]['change']:   # review F7: +-2 frames around every change of the chain sequence
            s |= {x for x in range(c - 2, c + 3) if 0 <= x < n}
        return sorted(s), st

    try:
        if want('pictures'):
            for k, c in caps.items():
                n = nframes[k]
                shots, st = shots_of(k)
                a, _ = arm(c, 'c_%s_stream' % k, n, shots, [])
                b, text = arm(c, 'c_%s_ours' % k, n, shots, ['--post-ours'])
                worst, bad_px, bad, missing = compare(a, b, shots)
                cen = po.parse_census(text)
                res.line(missing == 0 and worst <= po.PIX, 'picture.%s: %d frames (every %d-th and +-2 around %d sequence changes), stream arm vs --post-ours arm: max channel difference %d levels, %d pixels over %d level, missing %d%s' % (
                    k, len(shots), st, len(L[k]['change']), worst, bad_px, po.PIX, missing, (', frames ' + str(bad[:8])) if bad else ''))
                if cen:
                    res.line(True, 'census.%s: ours %d, fallback %d, kept %d (shape %d, slot %d, filter %d), shapes match %d film %d editor %d entrance %d' % (
                        k, cen['ours'], cen['fallback'], cen['kept'], cen['k_shape'], cen['k_slot'], cen['k_filter'], cen['match'], cen['film'], cen['editor'], cen['entrance']))
                else:
                    res.line(False, 'census.%s: no [rdr][post] line' % k)
                clean(a, b)
        if want('scale2'):
            for k in ('a_brock', 'a_wm30', 'checkC_director'):
                n = nframes[k]
                shots = set(range(0, n, max(40, n // 6))) | {n - 1}
                if L[k]['t2']:
                    shots |= {L[k]['t2'][0], L[k]['t2'][len(L[k]['t2']) // 2]}   # review F6: at least one t2 frame
                if L[k]['restore']:
                    shots |= {L[k]['restore'][0], L[k]['restore'][len(L[k]['restore']) // 2]}   # and one restore frame
                shots = sorted(shots)
                a, _ = arm(caps[k], 's2_%s_stream' % k, n, shots, [], 2)
                b, _ = arm(caps[k], 's2_%s_ours' % k, n, shots, ['--post-ours'], 2)
                rep = set(L[k]['repeat'])
                bad_clamp, far_rep, worst_rep, missing, n_rep = [], 0, 0, 0, 0
                for f in shots:
                    r = diff(a, b, f)
                    if r is None:
                        missing += 1
                        continue
                    if f in rep:
                        n_rep += 1
                        far_rep, worst_rep = max(far_rep, r[2]), max(worst_rep, r[0])
                    elif r[1]:
                        bad_clamp.append((f, r[0], r[1]))
                t2set, rset = set(L[k]['t2']), set(L[k]['restore'])
                res.line(missing == 0 and not bad_clamp and far_rep <= 16, 'picture.scale2.%s: %d frames at --scale 2 (%d with the dof depth REPEAT, %d t2, %d restore-in-front): CLAMP frames exact%s; REPEAT frames: max %d levels, farthest differing pixel %d px from an edge (bar 16 px)' % (
                    k, len(shots), n_rep, len([f for f in shots if f in t2set]), len([f for f in shots if f in rset]), '' if not bad_clamp else ' FAILED on ' + str(bad_clamp[:4]), worst_rep, far_rep))
                clean(a, b)
        if want('plants'):
            # (name, capture, checks-on expectation, ,nocheck expectation, why)
            plants = [('old_restore_rule', 'a_brock', 'kept', 'info', "the old rule (depth_linear = restores == 0) against brock's POINT-depth frames"),
                      ('t2_source_stale', 'a_wm30', 'kept', 'kept', "the t2 source = the dropped copy's destination: refused by the plan itself (writer check), not bypassable"),
                      ('t2_slots_foreign', 'a_wm30', 'kept', 'moves', 'slot 6 fed another image, the writer check skipped: the slot 2 == 6 check refuses'),
                      ('t2_default_tables', 'a_wm30', 'ours_moves', None, "DefaultPostParams' t2 tables: no check sees it, the picture does"),
                      ('claim_from_restore', 'checkC_director', 'kept', None, 'the claim starts at the copy_depth: the writes / reads checks refuse'),
                      ('shape_misclassified', 'a_wm30', 'kept', 'moves', 'the t2 source taken for the scene colour')]
            for name, k, on_expect, off_expect, why in plants:
                n = nframes[k]
                shots, _ = shots_of(k, max(args.stride * 2, n // 80))
                a, _ = arm(caps[k], 'pl_%s_stream' % name, n, shots, [])
                b, text = arm(caps[k], 'pl_%s_on' % name, n, shots, ['--post-ours', '--post-fault', name])
                w1, px1, bad1, m1 = compare(a, b, shots)
                cen = po.parse_census(text)
                if on_expect == 'kept':
                    res.line(m1 == 0 and w1 <= po.PIX and cen is not None and cen['kept'] > 0, 'plant.%s.%s checks on (%s): census kept %s, picture == stream (max %d levels)' % (name, k, why, cen['kept'] if cen else '?', w1))
                else:
                    res.line(m1 == 0 and px1 >= FAULT_PX and cen is not None and cen['kept'] == 0, 'plant.%s.%s checks on (%s): no check fires (kept %s) and the picture moves (%d pixels over %d level, max %d)' % (name, k, why, cen['kept'] if cen else '?', px1, po.PIX, w1))
                if off_expect:
                    c, _t = arm(caps[k], 'pl_%s_off' % name, n, shots, ['--post-ours', '--post-fault', name + ',nocheck'])
                    w2, px2, bad2, m2 = compare(a, c, shots)
                    if off_expect == 'moves':
                        res.line(m2 == 0 and px2 >= FAULT_PX, 'plant.%s.%s ,nocheck: the picture moves (%d pixels over %d level, max %d)' % (name, k, px2, po.PIX, w2))
                    elif off_expect == 'info':
                        # reported, not asserted: on every frame on disk (brock's 293, checkB_director's 14) linear vs point on the dof depth moves 0 pixels, so only the check carries this twin
                        res.line(m2 == 0, 'plant.%s.%s ,nocheck: INFO %d pixels over %d level (max %d) vs the stream: no picture signal on these frames, the filter check carries this twin' % (name, k, px2, po.PIX, w2))
                    else:
                        res.line(m2 == 0 and px2 == 0, 'plant.%s.%s ,nocheck: the plan refuses anyway (picture == stream, %d pixels)' % (name, k, px2))
                    clean(c)
                clean(a, b)
            # the device limit arm: a device that cannot filter R32F linearly keeps the linear-depth frames the stream's
            k = 'a_brock'
            n = nframes[k]
            shots, _ = shots_of(k, max(args.stride * 2, n // 80))
            a, _ = arm(caps[k], 'lim_stream', n, shots, [])
            b, text = arm(caps[k], 'lim_ours', n, shots, ['--post-ours', '--post-limits', 'no_r32f_linear'])
            w, px, bad, m = compare(a, b, shots)
            cen = po.parse_census(text)
            res.line(m == 0 and w <= po.PIX and cen is not None and cen['k_filter'] > 0 and cen['ours'] > 0, "limits.no_r32f_linear.%s: the linear-depth frames stay the stream's (filter kept %s), the point-depth ones are ours (%s), picture == stream (max %d levels)" % (
                k, cen['k_filter'] if cen else '?', cen['ours'] if cen else '?', w))
            clean(a, b)
    except RuntimeError as e:
        res.line(False, str(e))
    print('%d lines, %d failed' % (res.lines, res.fails))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())

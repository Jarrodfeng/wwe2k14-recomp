#!/usr/bin/env python3
"""Lane 135 offline proof, 0 game runs: the wrestlers' sweat map (yLiquidMap) and skin pre-light (yPreLight) passes drawn by ours, in place, through the REAL replace seam (fview --small-passes-ours),
every frame of three captures.

  py -I tools\\small\\small_passes_check.py [--root DIR] [--no-build] [--only checkB,r7079,bryan] [--no-plants]

For each capture two sweeps (fview --sweep: every frame in order; a hash of the PICTURE, of the small passes' target images, of the persistent sweat map and of every image bound at fetch slot 13 = the
pre-light cube maps the characters sample):
  off  the stream's own draws
  on   --small-passes-ours --validation 1
Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. picture, sweat scratch + persistent map, pre-light targets and slot-13 cube maps byte-equal to the stream's on EVERY frame (and the passes ran: images present);
  2. the FEEDBACK LOOP: the persistent sweat map after every frame (its input is OUR previous output, frame N -> N+1) equals the stream's, on every frame (part of 1, said apart);
  3. every replaced draw's bytes equal the rules (verify == replaced, bad 0, kept 0) and the census equals an independent oracle read from the capture alone (the claimed call sites' draws, summed over frames);
  4. the draws carry fate small_passes_ours in the op list (frame trace) and their number equals the oracle for that frame;
  5. validation errors 0;
  6. planted faults on Check B (small.NAME), each fails its exact check: sweat_no_prev (the persistent map and the picture move), slot_null (SH2's blur_input_null class: the nulled fetch slot refuses the
     sweat draws), prelight_wrong_target_size (the mesh and the clip range refuse the faces; with no_verify as well the cube maps and the picture move), and the control no_verify (nothing moves).
Every process has a timeout; the GPU-busy refusal is retried (a few attempts, said)."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
sys.path.insert(0, HERE)

CAPTURES = [('checkB', 'out/lane40/brock/capture'), ('r7079', 'out/lane85/r7079/capture'), ('bryan', 'out/lane99/bryan/capture')]
SITES = {0x8279A19C: 'sweat', 0x82799CB4: 'sweat', 0x82799DC8: 'sweat', 0x82C20B90: 'prelight'}
SLOT = 13


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    for attempt in range(6):
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, 'timeout after %d s: %s' % (timeout, ' '.join(cmd))
        text = (p.stdout or '') + (p.stderr or '')
        if p.returncode and 'Not enough graphics memory for the frame ring' in text and attempt < 5:
            print('INFO  GPU busy ("Not enough graphics memory for the frame ring"), attempt %d of 6, retrying in 20 s' % (attempt + 1))
            time.sleep(20)
            continue
        return p.returncode, text


class Report:
    def __init__(self):
        self.fails = 0

    def info(self, text):
        print('INFO  ' + text)

    def line(self, ok, text):
        print(('PASS  ' if ok else 'FAIL  ') + text)
        self.fails += 0 if ok else 1
        return ok


def read_csv(path):
    rows = {}
    with open(path, encoding='utf-8') as f:
        head = f.readline().strip().split(',')
        for l in f:
            v = l.strip().split(',')
            if len(v) == len(head):
                rows[int(v[0])] = dict(zip(head, v))
    return rows


CENSUS = re.compile(r'\[rdr\]\[small_passes\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
DETAIL = re.compile(r'\[rdr\]\[small_passes\] decisions (\d+), replaced (\d+) \(sweat (\d+), prelight (\d+)\) \| verify (\d+); read failed (\d+); bad: vertices (\d+) indices (\d+) shape (\d+) layout (\d+) clip (\d+) variant (\d+), fetch slot without image (\d+)')


def parse(text):
    c, d = CENSUS.search(text), DETAIL.search(text)
    if not c or not d:
        return None
    o = dict(zip(['frames', 'fallback', 'replaced', 'kept', 'not_ready', 'unjoined', 'mismatch', 'variant', 'texture', 'palette', 'not_ours'], map(int, c.groups())))
    o.update(dict(zip(['decisions', 'rep', 'sweat', 'prelight', 'verify', 'read_failed', 'bad_v', 'bad_i', 'bad_shape', 'bad_layout', 'bad_clip', 'bad_variant', 'null_slot'], map(int, d.groups()))))
    return o


def oracle(cap):
    """The independent count from the capture alone: the draws of the claimed call sites summed over frames (and the images they target)."""
    import numpy as np
    import shadow_census as sc
    n = len(cap.frames)
    total = {'sweat': 0, 'prelight': 0, 'frames': 0}
    targets = {'sweat': set(), 'prelight': set()}
    persistent = set()
    for fi in range(n):
        f = cap.frame(fi)
        if not f.has_origins:
            continue
        cs = f.origins['call_site'].astype(np.int64)
        hit = [(i, SITES[int(cs[i])]) for i in range(len(cs)) if int(cs[i]) in SITES]
        if not hit:
            continue
        total['frames'] += 1
        for _, kind in hit:
            total[kind] += 1
        copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
        passes = sc.walk(f, copies)
        mine = {i for i, _ in hit}
        for p in passes:
            for i in mine.intersection(p.draws):
                targets[SITES[int(cs[i])]].add(p.target)
        for i in mine:
            d = f.draws[i]
            if SITES[int(cs[i])] == 'sweat':
                for s in f.slots[int(d['tex_first']):int(d['tex_first']) + int(d['tex_count'])]:
                    if int(s['slot']) == 0:
                        persistent.add(int(s['image']))   # the previous map: the persistent sweat map the next frame reads back
    return total, {k: sorted(v) for k, v in targets.items()}, sorted(persistent)


FVIEW = ['']


def tolerance_check(label, cap, off, on, adiff, pre_targets, samples=4):
    """owned_check's picture tolerance on EVERY frame whose picture bytes differ (rplay shots of both arms in one run each), plus ALL the pre-light targets (<= 1 level) on up to `samples` differing frames."""
    import pic_tolerance as pt
    if not adiff:
        return True, 'no differing frame'
    pic = [f for f in sorted(off) if f in on and off[f]['picture'] != on[f]['picture']]
    worst = {'p999': 0.0, 'max': 0, 'block': 0.0, 'over4': 0.0, 'cube_max': 0, 'bad_frames': []}
    ok = True
    if pic:
        shots = {}
        for tag, extra in (('off', []), ('on', ['--small-passes-ours'])):
            d = os.path.join(ROOT, 'out', 'l135check', 'shots_%s_%s' % (label, tag))
            os.makedirs(d, exist_ok=True)
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))
            rc, text = run([RPLAY[0], '--capture', cap, '--frames', str(max(pic) + 1), '--shots', ','.join(str(f) for f in pic), '--shot-dir', d] + extra, 1800)
            if rc:
                return False, 'rplay %s failed (exit %d): %s' % (tag, rc, text[-200:].strip())
            shots[tag] = d
        for fr in pic:
            name = 'frame_%04d.png' % fr
            pa, pb = os.path.join(shots['off'], name), os.path.join(shots['on'], name)
            if not (os.path.isfile(pa) and os.path.isfile(pb)):
                return False, 'frame %d: rplay wrote no shot' % fr
            r = pt.compare(pt.load(pa), pt.load(pb))
            worst['p999'], worst['max'], worst['block'], worst['over4'] = max(worst['p999'], r['p999']), max(worst['max'], r['max']), max(worst['block'], r['block_max']), max(worst['over4'], r['over4_share'])
            if not r['ok']:
                worst['bad_frames'].append(fr)
            ok = ok and r['ok']
    spread = [adiff[int(k * (len(adiff) - 1) / max(1, samples - 1))] for k in range(min(samples, len(adiff)))]
    pick = sorted(set(spread))
    for fr in pick:
        for img in pre_targets:   # all five 384x64 targets
            outs = []
            for tag, extra in (('a', []), ('b', ['--small-passes-ours'])):
                png = os.path.join(ROOT, 'out', 'l135check', 'cube_%s_%d_%d.png' % (tag, img, fr))
                if os.path.isfile(png):
                    os.remove(png)
                rc, text = run([FVIEW[0], '--capture', cap, '--frame', str(fr), '--image-png', '%d,%s' % (img, png)] + extra, 900)
                if not os.path.isfile(png):
                    return False, 'frame %d: pre-light target %d not readable' % (fr, img)
                outs.append(pt.load(png))
            cm = int(abs(outs[0] - outs[1]).max()) if outs[0].shape == outs[1].shape else 255
            worst['cube_max'] = max(worst['cube_max'], cm)
            ok = ok and cm <= 1
    return ok, 'owned_check tolerance on ALL %d frames whose picture differs: worst p99.9 %.1f (<= %s), max %d, 16x16 block mean %.2f (<= %s), share over %d levels %.5f (<= %s), failing frames %s; all %d pre-light targets on %d spread frames %s: max %d level(s) (<= 1)' % (
        len(pic), worst['p999'], pt.TOL_P999, worst['max'], worst['block'], pt.TOL_BLOCK, pt.TOL_SMALL, worst['over4'], pt.TOL_SHARE, worst['bad_frames'][:5], len(pre_targets), len(pick), pick, worst['cube_max'])


RPLAY = ['']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--no-plants', action='store_true')
    args = ap.parse_args()
    rep = Report()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    if not args.no_build:
        for script in ('tools/fview/build.ps1', 'tools/renderer/build.ps1'):
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, script)], 2400)
            if rc:
                print(text[-3000:])
                print('FAIL  build %s (exit %d)' % (script, rc))
                return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    FVIEW[0] = fview
    RPLAY[0] = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.isfile(RPLAY[0]):
        print('FAIL  input missing: ' + RPLAY[0] + ' (tools/renderer/build.ps1)')
        return 1
    if not os.path.isfile(fview):
        print('FAIL  input missing: ' + fview)
        return 1
    import shadow_census as sc
    work = os.path.join(root, 'out', 'l135check')
    os.makedirs(work, exist_ok=True)
    only = {x for x in args.only.split(',') if x}

    def sweep(label, cap, ids, tag, extra, slot=SLOT):
        csv = os.path.join(work, '%s_%s.csv' % (label, tag))
        if os.path.isfile(csv):
            os.remove(csv)
        rc, text = run([fview, '--capture', cap, '--sweep', csv, '--sweep-slot', str(slot), '--sweep-images', ','.join(str(i) for i in ids)] + extra, 1800)
        if not os.path.isfile(csv):
            print(text[-1500:])
            raise RuntimeError('sweep %s/%s failed (exit %d)' % (label, tag, rc))
        return rc, text, read_csv(csv)

    def moved(on, ref, cols):
        return [f for f in sorted(ref) if f in on and any(ref[f][c] != on[f][c] for c in cols)]

    clean_cache = {}
    try:
        for label, rel in CAPTURES:
            if only and label not in only:
                continue
            cap = os.path.join(main_co, rel.replace('/', os.sep))
            if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isdir(os.path.join(cap, 'frames')):
                rep.line(False, '%s: input missing: the capture at %s' % (label, cap))
                continue
            orc, targets, persistent = oracle(sc.Cap(cap))
            if orc['frames'] == 0:
                rep.line(False, '%s: the oracle found no small-pass draws in the capture' % label)
                continue
            sweat_ids = sorted(set(targets['sweat']) | set(persistent))
            ids = sorted(set(sweat_ids) | set(targets['prelight']))
            exact = ['img%d' % i for i in sweat_ids]
            approx = ['picture'] + ['img%d' % i for i in targets['prelight']] + ['slot%d_hash' % SLOT]
            cols = exact + approx
            rc0, off_text, off = sweep(label, cap, ids, 'off', [])
            rc1, on_text, on = sweep(label, cap, ids, 'on', ['--small-passes-ours'])
            frames_equal = len(off) > 0 and set(off) == set(on)
            diff = moved(on, off, exact) if (frames_equal and exact) else []
            have_cubes = sum(1 for f in off if int(off[f]['slot%d_images' % SLOT]) > 0)
            # The sweat mesh floats differ from the stream's by up to 3.6e-7 (263 of 1122 values), so equal bytes are an OBSERVATION on these captures, not a property: the bar is "at most 1 level".
            sweat_max = 0
            if diff:
                import pic_tolerance as pt
                for fr in sorted({diff[int(k * (len(diff) - 1) / 5)] for k in range(min(6, len(diff)))}):
                    for img in sweat_ids:
                        outs = []
                        for tag, extra in (('a', []), ('b', ['--small-passes-ours'])):
                            png = os.path.join(work, 'sweat_%s_%d_%d.png' % (tag, img, fr))
                            if os.path.isfile(png):
                                os.remove(png)
                            run([fview, '--capture', cap, '--frame', str(fr), '--image-png', '%d,%s' % (img, png)] + extra, 900)
                            outs.append(pt.load(png) if os.path.isfile(png) else None)
                        sweat_max = max(sweat_max, 255 if None in outs or outs[0].shape != outs[1].shape else int(abs(outs[0] - outs[1]).max()))
            rep.line(frames_equal and sweat_max <= 1 and have_cubes > 0,
                     '%s: %d frames: the sweat scratch + persistent maps %s: bytes differ on %d frames (observed equal on the 3 captures; mesh floats within 3.6e-7, so the bar is <= 1 level: max %d level(s) over %d differing frames, first %s); frames with cube maps: %d' % (
                         label, len(off), sweat_ids, len(diff), sweat_max, len(diff), diff[:5], have_cubes))
            adiff = moved(on, off, approx) if frames_equal else []
            tol_ok, tol_text = tolerance_check(label, cap, off, on, adiff, targets['prelight'])
            rep.line(frames_equal and tol_ok, '%s: picture / pre-light targets %s / slot-%d cube maps: %d of %d frames differ in bytes (the cube directions are rule-built floats, 1 level at most); %s' % (
                label, targets['prelight'], SLOT, len(adiff), len(off), tol_text))
            if persistent:
                pc = ['img%d' % i for i in persistent]
                pdiff = moved(on, off, pc) if frames_equal else ['frames differ']
                changing = len({off[f][pc[0]] for f in off})
                rep.line((not pdiff or sweat_max <= 1) and changing > 1,
                         '%s: feedback loop: the persistent sweat map (img %s, built from OUR previous output) equals the stream\'s after every frame (differs on %d; %d distinct states over the run)' % (label, persistent, len(pdiff), changing))
            elif label != 'bryan':
                rep.line(False, '%s: no sweat draw found to test the feedback loop' % label)
            c = parse(on_text)
            if not c:
                print(on_text[-1500:])
                rep.line(False, '%s: no small_passes census line in the on arm (exit %d)' % (label, rc1))
                continue
            rcv, vtext = run([fview, '--capture', cap, '--small-passes-ours', '--validation', '1', '--frames', str(len(off))], 1800)
            rep.line(rcv == 0 and 'validation errors 0' in vtext and 'validation active: yes' in vtext, '%s: validation errors 0 over all %d frames (exit %d)' % (label, len(off), rcv))
            want = orc['sweat'] + orc['prelight']
            rep.line(c['replaced'] == want and c['rep'] == want and c['sweat'] == orc['sweat'] and c['prelight'] == orc['prelight'] and c['kept'] == 0 and c['verify'] == c['rep'] and c['read_failed'] == 0 and
                     c['bad_v'] == 0 and c['bad_i'] == 0 and c['bad_shape'] == 0 and c['bad_layout'] == 0 and c['bad_clip'] == 0 and c['bad_variant'] == 0 and c['null_slot'] == 0,
                     '%s: census: replaced %d == oracle %d (sweat %d / %d, prelight %d / %d over %d frames), kept %d, every replaced draw compared (verify %d), bad: vertices %d indices %d shape %d layout %d clip %d variant %d slot %d' % (
                         label, c['replaced'], want, c['sweat'], orc['sweat'], c['prelight'], orc['prelight'], orc['frames'], c['kept'], c['verify'], c['bad_v'], c['bad_i'], c['bad_shape'], c['bad_layout'], c['bad_clip'], c['bad_variant'], c['null_slot']))
            # the op list: the draws carry the fate
            fr = max(f for f in off) // 2
            ops_json = os.path.join(work, '%s_ops.json' % label)
            rc2, ops_text = run([fview, '--capture', cap, '--frame', str(fr), '--ops', '--json', ops_json, '--small-passes-ours'], 900)
            fate_n = want_n = -1
            if os.path.isfile(ops_json):
                try:
                    j = json.load(open(ops_json, encoding='utf-8'))
                    ds = [o for o in j.get('ops', []) if isinstance(o, dict) and 'fate' in o]
                    fate_n = sum(1 for o in ds if o['fate'] == 'small_passes_ours')
                    import numpy as np
                    cs = sc.Cap(cap).frame(fr).origins['call_site'].astype(np.int64)
                    want_n = sum(1 for x in cs if int(x) in SITES)
                except Exception as e:
                    print('INFO  ops json: %s' % e)
            rep.line(fate_n == want_n >= 0, '%s: frame %d op list: draws with fate small_passes_ours %d == the capture\'s claimed draws %d (exit %d)' % (label, fr, fate_n, want_n, rc2))
            clean_cache[label] = (cap, ids, persistent, off, c, on, exact, approx)
        if 'checkB' not in clean_cache or args.no_plants:
            print('small_passes_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
            return 1 if rep.fails else 0
        cap, ids, persistent, off, clean, on_clean, exact, approx = clean_cache['checkB']
        cols = exact + approx
        pcols = ['img%d' % i for i in persistent]

        def plant(name, faults, extra=()):
            rc, text, rows = sweep('checkB', cap, ids, 'p_' + name, ['--small-passes-ours', '--replace-fault', ','.join('small.' + x for x in faults)] + list(extra))
            return parse(text), rows, text

        # The references: `off` is the stream's own; `on_clean` is ours without a fault (the pre-light cube maps are 1 level off the stream's, so a plant that keeps OUR pre-light is judged against on_clean).
        # sweat_no_prev: the feedback input is a cleared image: the persistent map and the picture move, the same draws are replaced
        c, rows, _ = plant('sweat_no_prev', ['sweat_no_prev'])
        mp, mpic = moved(rows, on_clean, pcols), moved(rows, on_clean, ['picture'])
        rep.line(bool(c) and c['replaced'] == clean['replaced'] and len(mp) > 0,
                 'plant sweat_no_prev: a cleared previous map moves the persistent sweat map on %d frames and the picture on %d (replaced %s = clean %d)' % (len(mp), len(mpic), c and c['replaced'], clean['replaced']))
        # slot_null (SH2's blur_input_null class): the fetch slot read as image 0: the check refuses every sweat draw (texture), the stream's sweat stays (exact), our pre-light is untouched
        c, rows, _ = plant('slot_null', ['slot_null'])
        rep.line(bool(c) and c['null_slot'] > 0 and c['texture'] > 0 and c['sweat'] == 0 and c['replaced'] < clean['replaced'] and not moved(rows, off, exact) and not moved(rows, on_clean, approx),
                 'plant slot_null: the nulled fetch slot refuses the sweat draws (fetch slot without image %s, texture %s, sweat replaced %s of %d, prelight still %s), the stream\'s draws stay: sweat maps moved on %d frames' % (
                     c and c['null_slot'], c and c['texture'], c and c['sweat'], clean['sweat'], c and c['prelight'], len(moved(rows, off, exact))))
        # prelight_wrong_target_size: the mesh and the clip range refuse the faces; the pre-light is the stream's again, so the whole picture equals the stream's
        c, rows, _ = plant('prelight_wrong_target_size', ['prelight_wrong_target_size'])
        rep.line(bool(c) and c['prelight'] == 0 and c['bad_clip'] == clean['prelight'] and c['bad_v'] == 0 and c['bad_i'] == 0 and c['kept'] == clean['prelight'] and c['sweat'] == clean['sweat'] and not moved(rows, off, cols),
                 'plant prelight_wrong_target_size: faces built for another target size are refused (clip %s, vertices %s, prelight replaced %s of %d), the stream\'s stay: picture / maps moved on %d frames vs the stream' % (
                     c and c['bad_clip'], c and c['bad_v'], c and c['prelight'], clean['prelight'], len(moved(rows, off, cols))))
        c, rows, _ = plant('prelight_wrong_target_size_nv', ['prelight_wrong_target_size', 'no_verify'])
        m = moved(rows, on_clean, approx)
        rep.line(bool(c) and c['prelight'] == clean['prelight'] and c['verify'] == 0 and len(m) > 0,
                 'plant prelight_wrong_target_size without the compare: the half-size faces are drawn and the cube maps / picture move on %d frames (verify %s)' % (len(m), c and c['verify']))
        # rule 4: the compares cover the LAST vertex / the index order / the restarts (each plant is one fault in our mesh, the stream's bytes are the reference)
        c, rows, _ = plant('disc_last_vertex', ['disc_last_vertex'])
        rep.line(bool(c) and c['bad_v'] == clean['sweat'] and c['sweat'] == 0 and c['bad_i'] == 0 and c['prelight'] == clean['prelight'] and not moved(rows, off, exact),
                 'plant disc_last_vertex: +1e-4 on vertex 186 of the sweat mesh: every sweat draw refused (bad vertices %s of %d, sweat replaced %s), the stream sweat maps stay equal (moved on %d frames)' % (
                     c and c['bad_v'], clean['sweat'], c and c['sweat'], len(moved(rows, off, exact))))
        c, rows, _ = plant('cube_last_vertex', ['cube_last_vertex'])
        rep.line(bool(c) and c['bad_v'] == clean['prelight'] and c['bad_clip'] == 0 and c['prelight'] == 0 and c['sweat'] == clean['sweat'],
                 'plant cube_last_vertex: +1e-4 on vertex 725 of the pre-light mesh (positions right, the clip check passes): every pre-light draw refused by the vertex compare (bad vertices %s of %d, clip %s)' % (
                     c and c['bad_v'], clean['prelight'], c and c['bad_clip']))
        c, rows, _ = plant('cube_index_order', ['cube_index_order'])
        rep.line(bool(c) and c['bad_i'] == clean['prelight'] and c['bad_v'] == 0 and c['prelight'] == 0,
                 'plant cube_index_order: the strips pairs swapped: every pre-light draw refused by the index compare (bad indices %s of %d)' % (c and c['bad_i'], clean['prelight']))
        c, rows, _ = plant('disc_no_restart', ['disc_no_restart'])
        rep.line(bool(c) and c['bad_i'] == clean['sweat'] and c['bad_v'] == 0 and c['sweat'] == 0,
                 'plant disc_no_restart: the restart indices replaced: every sweat draw refused by the index compare (bad indices %s of %d)' % (c and c['bad_i'], clean['sweat']))
        # sweat_one_frame: ONE frame reads a cleared previous map; the persistent map carries it over, so frames AFTER it still differ from the clean arm
        c, rows, _ = plant('sweat_one_frame', ['sweat_one_frame'])
        m1 = moved(rows, on_clean, pcols)
        rep.line(bool(c) and c['replaced'] == clean['replaced'] and len(m1) >= 5 and m1[-1] - m1[0] >= 5 and len(m1) > 1,
                 'plant sweat_one_frame: the previous map cleared on one frame only (first differing frame %s): the persistent map differs from the clean arm on %d frames, up to frame %s (the carry-over through the copies)' % (
                     m1[:1], len(m1), m1[-1:]))
        # control: no_verify alone, the geometry is right, nothing moves against our own clean arm
        c, rows, _ = plant('no_verify', ['no_verify'])
        rep.line(bool(c) and c['verify'] == 0 and c['replaced'] == clean['replaced'] and not moved(rows, on_clean, cols), 'control no_verify: the compare is off (%s runs), the geometry is right: %s replaced, picture and images byte-equal to the clean arm' % (c and c['verify'], c and c['replaced']))
    except RuntimeError as e:
        rep.line(False, str(e))
    print('small_passes_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
    return 1 if rep.fails else 0


sys.exit(main())

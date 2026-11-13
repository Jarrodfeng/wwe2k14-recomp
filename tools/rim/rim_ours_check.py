#!/usr/bin/env python3
"""Lane 124 (RM) offline proof, 0 game runs: the rim light map's casters drawn by us in place through the REAL seam (fview --rim-ours), every frame of three captures.

  py -I tools\\rim\\rim_ours_check.py [--root DIR] [--no-build] [--only checkB,r7079,bryan] [--no-plants]

For each capture two sweeps (fview --sweep: every frame in order; a hash of the PICTURE, of the rim map target (image 1189, 512x256) and of every image bound at fetch slot 15 per frame = the
blurred rim maps the wrestler materials sample):
  off  the other classes only (--wrestlers-ours --ring-ours; Bryan has no scene records: no classes)
  on   the same + --rim-ours (--validation 1)
Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. picture, rim map target and slot-15 images byte-equal on EVERY frame (and there are slot-15 images);
  2. every replaced draw's bytes equal the stream's (verify bad 0, mismatch 0, layout refused 0, joins dropped 0, verify == replaced);
  3. the census is exact against an independent oracle written here from the capture alone (the rim-site draws of objects whose colour draws are a wrestler's, from the wrestlers' first
     record frame on): replaced == oracle (Bryan has no scene records: 0 replaced, every claimed draw kept);
  4. validation errors 0;
  5. planted faults on Check B (rim.NAME), each fails its exact check: no_pose, first_object, wrong_target; and, as a control of lane 86 rule (not this code), rplay rim_as_shadow at half rate moves the picture; serial_running (review B1).
Every process has a timeout; the GPU-busy refusal is retried (a few attempts, said)."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))

CAPTURES = [
    ('checkB', 'out/lane40/brock/capture', True),
    ('r7079', 'out/lane85/r7079/capture', True),
    ('bryan', 'out/lane99/bryan/capture', False),
]
RIM_IMG = 1189   # the rim light map's target (512x256 RGBA8), the same id in all three captures (tools\rim\rim_targets.py)
SLOT = 15


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

    def info(self, text):   # a measurement reported, never counted as a pass or a failure
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


CENSUS = re.compile(r'\[rdr\]\[rim_map\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
DETAIL = re.compile(r'rim map: claimed (\d+) replaced (\d+) \| kept: not ours (\d+), unjoined (\d+) \| verify (\d+) bad (\d+), layout refused (\d+), rows overridden (\d+), joins dropped (\d+), parts built (\d+) released \d+ \| upload .*? \| frames ready \d+ not ready \d+ \| wrestlers binds (\d+) starts (\d+) early hits (\d+)')
WRESTLER_FB = re.compile(r'\[rdr\]\[wrestlers\] ours frames (\d+), fallback (\d+)')


def parse(text):
    c, d = CENSUS.search(text), DETAIL.search(text)
    if not c or not d:
        return None
    k = ['frames', 'fallback', 'replaced', 'kept', 'not_ready', 'unjoined', 'mismatch', 'variant', 'texture', 'palette', 'not_ours']
    o = dict(zip(k, map(int, c.groups())))
    o.update(dict(zip(['claimed', 'rep', 'not_ours_map', 'unjoined_d', 'verify', 'bad', 'layout_refused', 'rows', 'dropped', 'built', 'binds', 'starts', 'early_hits'], map(int, d.groups()))))
    w = WRESTLER_FB.search(text)
    o['wr_fb'] = int(w.group(2)) if w else 0
    return o


def oracle(cap_path, first_frame):
    """The independent count: rim-site draws (pass site 0x82BFBD58, object != 0) whose object's colour draws are a wrestler's or a piece's (the census tool's class rule: yCh_skin / gear / Aniso / Refl, or all yCh_trans),
    over the frames from the wrestlers' first record frame on. Read from the capture alone: no scene record, no join, no hook."""
    import collections
    import numpy as np
    import shadow_census as sc
    cap = sc.Cap(cap_path)
    n = len(cap.frames)
    total = {'draws': 0, 'wrestler': 0, 'from': 0}
    for fi in range(n):
        f = cap.frame(fi)
        if not f.has_origins:
            continue
        d = f.draws
        o = f.origins
        ps, ob, kl = o['pass_site'].astype(np.int64), o['object'].astype(np.int64), o['klass'].astype(np.int64)
        vs_eff = cap.effs(d['vs_hash'])
        eff = collections.defaultdict(set)
        for i in np.nonzero(((ps == sc.SITE_COLOUR) | (ps == sc.SITE_MIRROR)) & (kl == sc.pcap.K_MODEL_PART))[0]:
            eff[int(ob[i])].add(vs_eff[i])
        for i in np.nonzero((ps == sc.SITE_RIM) & (ob != 0))[0]:
            total['draws'] += 1
            if sc.class_of_effects(eff.get(int(ob[i]), set())) in ('wrestler', 'piece'):   # the wrestlers' hook joins the three yCh_trans pieces a frame too (they draw into the rim map)
                total['wrestler'] += 1
                if fi >= first_frame:
                    total['from'] += 1
    return total


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
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    if not args.no_build:
        for script in ('tools/fview/build.ps1', 'tools/renderer/build.ps1'):
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, script)], 2400)
            if rc:
                print(text[-3000:])
                print('FAIL  build %s (exit %d)' % (script, rc))
                return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    for need in (fview, rplay):
        if not os.path.isfile(need):
            print('FAIL  input missing: ' + need)
            return 1
    work = os.path.join(root, 'out', 'l124check')
    os.makedirs(work, exist_ok=True)
    only = {x for x in args.only.split(',') if x}

    def sweep(label, cap, tag, extra, slot=SLOT):
        csv = os.path.join(work, '%s_%s.csv' % (label, tag))
        if os.path.isfile(csv):
            os.remove(csv)
        rc, text = run([fview, '--capture', cap, '--sweep', csv, '--sweep-slot', str(slot), '--sweep-images', str(RIM_IMG), '--game', game] + extra, 1500)
        if not os.path.isfile(csv):
            print(text[-1500:])
            raise RuntimeError('sweep %s/%s failed (exit %d)' % (label, tag, rc))
        return rc, text, read_csv(csv)

    def moved(on, ref):
        return [f for f in sorted(ref) if f in on and (ref[f]['picture'] != on[f]['picture'] or ref[f]['img%d' % RIM_IMG] != on[f]['img%d' % RIM_IMG] or ref[f]['slot%d_hash' % SLOT] != on[f]['slot%d_hash' % SLOT])]

    clean_cache = {}
    try:
        for label, rel, records in CAPTURES:
            if only and label not in only:
                continue
            cap = os.path.join(main_co, rel.replace('/', os.sep))
            if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isdir(os.path.join(cap, 'frames')):
                rep.line(False, '%s: input missing: the capture at %s' % (label, cap))
                continue
            classes = ['--wrestlers-ours', '--ring-ours'] if records else []
            rc0, off_text, off = sweep(label, cap, 'off', classes)
            rc1, on_text, on = sweep(label, cap, 'on', classes + ['--rim-ours', '--validation', '1'])
            # review B2: both hooks together (both start the wrestlers' frame early) against the stream on every frame; then slot 12 (the shadow maps) for the pair
            rc2, both_text, both = sweep(label, cap, 'both', classes + ['--shadow-casters-ours', '--rim-ours', '--validation', '1'])
            bdiff = moved(both, off) if set(off) == set(both) else ['frames differ']
            rep.line(not bdiff and rc2 == 0 and 'validation errors 0' in both_text, '%s: --shadow-casters-ours --rim-ours together: picture / rim map / slot-%d images differ on %d frames, validation errors 0 (exit %d)' % (label, SLOT, len(bdiff), rc2))
            cb = parse(both_text)
            if records and cb:
                rep.line(cb['starts'] == cb['binds'] and cb['early_hits'] > 0 and cb['bad'] == 0, '%s: both hooks on, the wrestlers start once per bind (binds %d, starts %d, early hits %d), rim verify bad %d' % (label, cb['binds'], cb['starts'], cb['early_hits'], cb['bad']))
            _, _, off12 = sweep(label, cap, 'off12', classes, slot=12)
            _, _, both12 = sweep(label, cap, 'both12', classes + ['--shadow-casters-ours', '--rim-ours'], slot=12)
            d12 = [f for f in sorted(off12) if f in both12 and (off12[f]['picture'] != both12[f]['picture'] or off12[f]['slot12_hash'] != both12[f]['slot12_hash'])]
            rep.line(set(off12) == set(both12) and not d12 and sum(1 for f in off12 if int(off12[f]['slot12_images']) > 0) > 0, '%s: both hooks on: picture and the slot-12 shadow map images byte-equal to the stream on every frame (differ on %d)' % (label, len(d12)))
            n = len(off)
            frames_equal = n > 0 and set(off) == set(on)
            diff = moved(on, off) if frames_equal else []
            have_imgs = sum(1 for f in off if int(off[f]['slot%d_images' % SLOT]) > 0)
            rep.line(frames_equal and not diff and have_imgs > 0,
                     '%s: %d frames: picture / rim map target (img %d) / slot-%d images differ on %d frames (frames with slot-%d images: %d; first differing %s)' % (
                         label, n, RIM_IMG, SLOT, len(diff), SLOT, have_imgs, diff[:5]))
            c = parse(on_text)
            if not c:
                print(on_text[-1500:])
                rep.line(False, '%s: no rim map census line in the on arm (exit %d)' % (label, rc1))
                continue
            rep.line(rc1 == 0 and 'validation errors 0' in on_text, '%s: validation errors 0 (exit %d)' % (label, rc1))
            rep.line(c['bad'] == 0 and c['mismatch'] == 0 and c['layout_refused'] == 0 and c['dropped'] == 0 and c['variant'] == 0 and c['rows'] == 0 and c['verify'] == c['replaced'],
                     '%s: every replaced draw byte-compared with the stream (verify %d of replaced %d), bad %d, mismatch %d, layout refused %d, rows overridden %d, joins dropped %d' % (
                         label, c['verify'], c['replaced'], c['bad'], c['mismatch'], c['layout_refused'], c['rows'], c['dropped']))
            fb = c['wr_fb'] if records else 0
            orc = oracle(cap, fb)
            want = orc['from'] if records else 0
            rep.line(c['replaced'] == want and c['texture'] == 0 and (c['claimed'] > 0 if records else c['kept'] == orc['draws']),
                     '%s: replaced %d == oracle %d (%d of the capture\'s %d rim draws belong to wrestler / piece objects over all frames; the wrestlers\' %d fallback frame(s) leave the stream\'s)%s; kept: not ready %d, not ours %d, unjoined %d' % (
                         label, c['replaced'], want, orc['wrestler'], orc['draws'], fb, '' if records else ' [no scene records: no wrestlers\' joins, every rim draw stays the stream\'s]',
                         c['not_ready'], c['not_ours'], c['unjoined']))
            clean_cache[label] = (cap, classes, off, c, records)
        if 'checkB' not in clean_cache or args.no_plants:
            print('rim_ours_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
            return 1 if rep.fails else 0
        cap, classes, off, clean, records = clean_cache['checkB']

        def plant(name, faults, more=()):
            rc, text, rows = sweep('checkB', cap, 'p_' + name, classes + list(more) + ['--rim-ours', '--replace-fault', ','.join('rim.' + x for x in faults)])
            return parse(text), rows, text

        # no_pose: the replaced draws get the bind pose's rows instead of the frame's palette: the rim map and the picture move, the same draws are replaced
        c, rows, _ = plant('no_pose', ['no_pose'])
        m = moved(rows, off)
        rep.line(bool(c) and c['replaced'] == clean['replaced'] and c['bad'] == 0 and len(m) > 0,
                 'plant no_pose: the bind pose in place of the frame\'s palette moves the rim map / picture on %d frames (replaced %s = clean %d)' % (len(m), c and c['replaced'], clean['replaced']))
        # first_object: one object's join for every object: the byte compare fails it, the stream's draws stay
        c, rows, _ = plant('first_object', ['first_object'])
        rep.line(bool(c) and (c['bad'] > 0 or c['mismatch'] > 0) and c['replaced'] < clean['replaced'] and not moved(rows, off),
                 'plant first_object: the per-draw checks fail it (mismatch %s, verify bad %s, replaced %s of %d clean, the stream\'s draws stay: picture moved on %d frames)' % (c and c['mismatch'], c and c['bad'], c and c['replaced'], clean['replaced'], len(moved(rows, off))))
        c2, rows2, _ = plant('first_object_nv', ['first_object', 'no_verify'])
        m2 = moved(rows2, off)
        rep.info('plant first_object,no_verify (not counted; verify %s): the vertex / index COUNT check alone already refuses the other objects (replaced %s of %d clean); the rim map / picture moves on %d frames (not required: only the first object\'s own draws are replaced, and they are right)' % (c2 and c2['verify'], c2 and c2['replaced'], clean['replaced'], len(m2)))
        # serial_running (review B1): the old early-start dedupe (frame key AND bind number) with the rim hook's bind number running ahead of the shadow hook's: the wrestlers start twice in a bind
        c, rows, _ = plant('serial_running', ['serial_running'], ['--shadow-casters-ours'])
        rep.line(bool(c) and c['starts'] > c['binds'], 'plant serial_running: the old per-hook bind number starts the wrestlers more than once per bind (binds %s, starts %s; clean: binds = starts)' % (c and c['binds'], c and c['starts']))
        rep.line(clean['starts'] == clean['binds'], 'clean rim arm: one start of the wrestlers per bind (binds %d, starts %d)' % (clean['binds'], clean['starts']))
        # wrong_target: the hook claims the shadow maps' draws: it lands in the shadow maps; the census is wrong against the oracle (and the shadow maps are claimed)
        c, rows, _ = plant('wrong_target', ['wrong_target'])
        rep.line(bool(c) and c['replaced'] != clean['replaced'],
                 'plant wrong_target: claiming the shadow maps\' site instead of the rim map\'s fails the census (replaced %s, clean %d / oracle)' % (c and c['replaced'], clean['replaced']))
        # no_verify alone: the geometry is right, nothing moves
        c, rows, _ = plant('no_verify', ['no_verify'])
        rep.line(bool(c) and c['verify'] == 0 and c['replaced'] == clean['replaced'] and not moved(rows, off), 'control no_verify: the compare is off (%s runs), the geometry is right: %s replaced, picture byte-equal' % (c and c['verify'], c and c['replaced']))
        # rim_as_shadow (rplay, half rate): the old half-rate rule skipping the rim map must move the picture; the role rule does not
        shots = os.path.join(work, 'pics')

        def play(tag, extra):
            out = os.path.join(shots, tag)
            os.makedirs(out, exist_ok=True)
            for f in os.listdir(out):
                os.remove(os.path.join(out, f))
            rc, text = run([rplay, '--capture', cap, '--frames', '201', '--shots', '200', '--shot-dir', out, '--game', game, '--wrestlers-ours', '--ring-ours'] + extra, 900)
            png = os.path.join(out, 'frame_0200.png')
            if rc or not os.path.isfile(png):
                print(text[-1500:])
                raise RuntimeError('rplay %s failed (exit %d)' % (tag, rc))
            with open(png, 'rb') as fh:
                return fh.read()

        half_off = play('half_off', ['--shadow-half'])
        half_on = play('half_on', ['--shadow-half', '--rim-ours'])
        half_plant = play('half_plant', ['--shadow-half', '--rim-ours', '--shadow-keying', 'rim_as_shadow'])
        rep.line(half_on == half_off, 'half rate, role keying: --rim-ours picture == stream picture (%d / %d bytes)' % (len(half_on), len(half_off)))
        rep.line(half_plant != half_on, 'control (lane 86 rule, not this code): with rplay --shadow-keying rim_as_shadow at half rate the rim map is skipped like a shadow and the picture moves (differs from the role-keyed ours picture)')
        rc, text = run([fview, '--capture', cap, '--frame', '5', '--ops', '--json', os.path.join(work, 'x.json'), '--rim-ours', '--replace-fault', 'rim.nonsense', '--game', game], 300)
        rep.line(rc != 0, 'loud failure: an unknown rim fault name is refused (exit %d)' % rc)
        rc, text = run([fview, '--capture', cap + '.missing', '--sweep', os.path.join(work, 'x.csv'), '--rim-ours', '--game', game], 300)
        rep.line(rc != 0, 'loud failure: a missing capture is refused (exit %d)' % rc)
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('rim_ours_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    sys.exit(main())

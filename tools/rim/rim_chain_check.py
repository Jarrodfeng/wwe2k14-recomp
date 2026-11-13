#!/usr/bin/env python3
"""Lane 151 (P3) offline proof, 0 game runs: the rim light map's reductions and blur drawn by our own programs (fview --rim-chain-check, headless, two sessions in lockstep), every frame of three captures.

  py -I tools\\rim\\rim_chain_check.py [--root DIR] [--no-build] [--only checkB,r7079,bryan] [--no-plants]

Per capture: A = the stream's draws, B = the same plus --rim-chain-ours (Check B and 7079 also with the wrestlers, the ring and lane 124's rim casters in BOTH arms). After every frame the chain's images
222 (the rim map's copy), 223 (the blurred map the wrestler materials read in slot 15), 224 and 225 (the two coverage maps) and the presented picture are read from both and compared texel by texel.
Bar (PASS / FAIL; a missing input FAILS, never skips):
  1. every image and the picture byte-equal on EVERY frame (the bar is within rounding, 1 level; measured: 0), the compared images hold data (lit texels > 0 in the stream arm), validation 0;
  2. the census equals an independent oracle read from the capture alone: replaced reduce1 == reduce2 == blur == the frames carrying the chain; every kept counter 0; verify bad 0, size unchecked 0;
  3. planted faults on Check B each fail their exact check: reduction_wrong_size (224 / 225 / 223 / picture move), blur_no_input (223 and picture move), blur_input_null (every blur draw refused as texture,
     the stream's blur stays: everything equal), rim_reader_stale (223 and picture move), no_verify (a control: equal); an unknown fault name and a missing capture are refused loudly.
  4. (review S3) a structured input: in BOTH arms reduction 1 reads the shadow coverage reduction's copy (dark and lit areas) instead of the rim map, so the mixed any() and the blur's mask-off cells are
     compared (the real rim map is lit everywhere in all three captures); a render scale of 2; refusal plants that perturb the input: grid_byte (verify bad, the stream's blur stays) and image_info live-fault
     (width + 1: both reductions refused on size, the stream's stay).
  The bar is BYTE-equal on the 4090 (measured: 0 everywhere); on another GPU / driver the bar is "within 1 level" (the 13-tap sum may round differently): the driver's `moved` counts texels over 1 level.
  The 'unmasked' experiment (the blur ignores the coverage maps) is measured and reported, not asserted."""
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

CAPTURES = [('checkB', 'out/lane40/brock/capture', True), ('r7079', 'out/lane85/r7079/capture', True), ('bryan', 'out/lane99/bryan/capture', False)]
IMAGES = ('222', '223', '224', '225')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=1500):
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


def oracle(cap_path):
    """Independent facts from the capture alone: frames, the chain's draws (flag_any at the generic post queue site into a 128x64 / 32x16 target; yBlur/t2 at call 0x82814080), the capture's OWN image ids
    of the chain (the copies' destinations: the rim map's copy, the blurred map, the two coverage maps) and a structured image (the shadow coverage reduction's copy: binary, dark and lit areas)."""
    import shadow_census as sc
    import numpy as np
    cap = sc.Cap(cap_path)
    n = len(cap.frames)
    out = {'frames': n, 'with_origins': 0, 'reduce1': 0, 'reduce2': 0, 'blur': 0, 'ids': {}, 'structured': 0}
    for fi in range(n):
        f = cap.frame(fi)
        if not f.has_origins:
            continue
        out['with_origins'] += 1
        d, o = f.draws, f.origins
        cs = sc.pcap.queue_site(o)
        ps = o['pass_site'].astype(np.int64)
        pe = cap.effs(d['ps_hash'])
        copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
        for p in sc.walk(f, copies):
            if not p.draws:
                continue
            t = cap.imgs[p.target]
            dest = int(copies[p.copies[0]]['d']['image']) if p.copies else 0
            if any(ps[di] == sc.SITE_RIM for di in p.draws) and dest:
                out['ids'].setdefault('rim', dest)
            if not out['structured'] and dest and any(cs[di] == sc.CALL_REDUCE and pe[di] == 'yBlur/flag_any' for di in p.draws):
                out['structured'] = dest
            for di in p.draws:
                if pe[di] == 'yBlur/flag_any' and cs[di] == sc.CALL_RIMRED:
                    if (int(t['w']), int(t['h'])) == (128, 64):
                        out['reduce1'] += 1
                        out['ids'].setdefault('red1', dest)
                    elif (int(t['w']), int(t['h'])) == (32, 16):
                        out['reduce2'] += 1
                        out['ids'].setdefault('red2', dest)
                elif pe[di] == 'yBlur/t2' and cs[di] == sc.CALL_MINBLUR:
                    out['blur'] += 1
                    out['ids'].setdefault('blur', dest)
    return out


CENSUS = re.compile(r'rim chain: decisions (\d+) \(not ours (\d+)\), replaced reduce1 (\d+) reduce2 (\d+) blur (\d+); kept: variant (\d+), geometry (\d+), constants (\d+), texture (\d+), alpha (\d+), size (\d+) \| verify (\d+) bad (\d+), size unchecked (\d+) \| frames (\d+)')


def parse(text):
    m = CENSUS.search(text)
    if not m:
        return None
    k = ['decisions', 'not_ours', 'reduce1', 'reduce2', 'blur', 'variant', 'geometry', 'constants', 'texture', 'alpha', 'size', 'verify', 'bad', 'size_unchecked', 'frames']
    return dict(zip(k, map(int, m.groups())))


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
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools/fview/build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  build fview (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(fview):
        print('FAIL  input missing: ' + fview)
        return 1
    work = os.path.join(root, 'out', 'l151check')
    os.makedirs(work, exist_ok=True)
    only = {x for x in args.only.split(',') if x}

    def arm(label, cap, frames, tag, extra, fault=None, images=None):
        js = os.path.join(work, '%s_%s.json' % (label, tag))
        if os.path.isfile(js):
            os.remove(js)
        cmd = [fview, '--capture', cap, '--rim-chain-check', js, '--frames', str(frames), '--validation', '1', '--game', game] + extra
        if images:
            cmd += ['--rim-chain-images', images]
        if fault:
            cmd += ['--replace-fault', 'rimchain.' + fault]
        rc, text = run(cmd)
        if not os.path.isfile(js):
            print(text[-1500:])
            raise RuntimeError('rim-chain-check %s/%s failed (exit %d)' % (label, tag, rc))
        with open(js, encoding='utf-8') as fh:
            return rc, text, json.load(fh)

    def moved(j):   # the images and the picture that differ from the stream (beyond the 1 level a different GPU may round to: the bar is "within 1 level", measured 0 on the 4090)
        return {k: v['frames_differing'] for k, v in list(j['images'].items()) + [('picture', j['picture'])] if v['over1'] or v['shape_fail'] or (v['max_diff'] > 0 and k == 'picture' and v['frames_differing'] and v['max_diff'] > 1)}

    def exact(j):
        return {k: v['frames_differing'] for k, v in list(j['images'].items()) + [('picture', j['picture'])] if v['frames_differing'] or v['shape_fail']}

    clean = {}
    try:
        for label, rel, records in CAPTURES:
            if only and label not in only:
                continue
            cap = os.path.join(main_co, rel.replace('/', os.sep))
            if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isdir(os.path.join(cap, 'frames')):
                rep.line(False, '%s: input missing: the capture at %s' % (label, cap))
                continue
            orc = oracle(cap)
            frames = orc['frames']
            ids = orc['ids']
            if sorted(ids) != ['blur', 'red1', 'red2', 'rim'] or not orc['structured']:
                rep.line(False, '%s: the chain\'s image ids could not be derived from the capture (%s, structured %s)' % (label, ids, orc['structured']))
                continue
            names = [str(ids[k]) for k in ('rim', 'blur', 'red1', 'red2')]
            image_list = ','.join(names)
            rep.info('%s: chain images (derived from the capture): rim copy %s, blurred %s, coverage maps %s / %s; structured source %d' % (label, names[0], names[1], names[2], names[3], orc['structured']))
            for tag, extra in (('plain', []), ('classes', ['--wrestlers-ours', '--ring-ours', '--rim-ours'] if records else None)):
                if extra is None:
                    continue
                rc, text, j = arm(label, cap, frames, tag, extra, images=image_list)
                c = parse(text)
                eq = not exact(j) and j['compared'] == frames and j['no_image'] == 0
                lit = all(j['images'][k]['lit'] > 0 for k in names) if all(k in j['images'] for k in names) else False
                worst = max([v['max_diff'] for v in j['images'].values()] + [j['picture']['max_diff']])
                rep.line(eq and lit and worst <= 1.0, '%s/%s: %d frames: images %s and the picture byte-equal on every frame (max difference %.0f; moved %s); the stream\'s images hold data (lit frames: %s)' % (
                    label, tag, frames, image_list, worst, exact(j) or 'none', {k: j['images'].get(k, {}).get('frames_lit') for k in names}))
                rep.line(rc == 0 and j['validation_errors'] == 0, '%s/%s: validation errors %d (exit %d)' % (label, tag, j['validation_errors'], rc))
                if not c:
                    print(text[-1500:])
                    rep.line(False, '%s/%s: no rim chain census line' % (label, tag))
                    continue
                n = orc['with_origins']
                rep.line(c['reduce1'] == c['reduce2'] == c['blur'] == orc['reduce1'] == orc['reduce2'] == orc['blur'] == n and c['reduce1'] > 0,
                         '%s/%s: census replaced reduce1 %d reduce2 %d blur %d == the oracle read from the capture alone (%d / %d / %d, frames with names %d)' % (
                             label, tag, c['reduce1'], c['reduce2'], c['blur'], orc['reduce1'], orc['reduce2'], orc['blur'], n))
                rep.line(c['variant'] + c['geometry'] + c['constants'] + c['texture'] + c['alpha'] + c['size'] == 0 and c['bad'] == 0 and c['size_unchecked'] == 0 and c['verify'] == c['blur'],
                         '%s/%s: nothing kept (variant %d geometry %d constants %d texture %d alpha %d size %d), every blur draw compared (verify %d == blur draws), bad %d, size unchecked %d' % (
                             label, tag, c['variant'], c['geometry'], c['constants'], c['texture'], c['alpha'], c['size'], c['verify'], c['bad'], c['size_unchecked']))
                if tag == 'plain':
                    clean[label] = (cap, frames, c, image_list, orc)
            # S3: the rim map is lit everywhere in these captures, so a constant-white program would pass the arms above. Both arms read a STRUCTURED image (the shadow coverage reduction's copy:
            # dark and lit areas) as reduction 1's input instead: the mixed any() and the mask-off cells of the blur are then compared against the stream.
            nov = min(frames, 120)
            js = os.path.join(work, '%s_override.json' % label)
            cmd = [fview, '--capture', cap, '--rim-chain-check', js, '--frames', str(nov), '--validation', '1', '--game', game, '--rim-chain-override', '%d:%d' % (ids['rim'], orc['structured']), '--rim-chain-images', image_list]
            rc, text = run(cmd)
            if not os.path.isfile(js):
                print(text[-1500:])
                raise RuntimeError('override arm %s failed (exit %d)' % (label, rc))
            with open(js, encoding='utf-8') as fh:
                j = json.load(fh)
            c = parse(text)
            mixed = all(0 < j['images'][k]['lit'] < j['images'][k]['texels'] for k in (names[2], names[3])) if all(k in j['images'] for k in names) else False
            rep.line(not exact(j) and mixed and rc == 0 and j['validation_errors'] == 0 and bool(c) and c['reduce1'] == nov and c['blur'] == nov,
                     '%s/structured input (image %d read as reduction 1\'s input in BOTH arms, %d frames): the coverage maps %s / %s are partly lit (lit %s of %s / %s of %s texels) and ours equals the stream (moved %s), blur cells switched off by the mask included; validation %d' % (
                         label, orc['structured'], nov, names[2], names[3], j['images'].get(names[2], {}).get('lit'), j['images'].get(names[2], {}).get('texels'), j['images'].get(names[3], {}).get('lit'), j['images'].get(names[3], {}).get('texels'),
                         exact(j) or 'none', j['validation_errors']))
        if 'checkB' not in clean or args.no_plants:
            print('rim_chain_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
            return 1 if rep.fails else 0
        cap, frames, base, image_list, orc = clean['checkB']
        names = image_list.split(',')
        nplant = min(frames, 120)
        # a render scale of 2: the system block carries the scale, the programs are the same
        rc, text, j = arm('checkB', cap, 40, 'scale2', ['--scale', '2'], images=image_list)
        c = parse(text)
        rep.line(not exact(j) and rc == 0 and j['validation_errors'] == 0 and bool(c) and c['blur'] == 40 and c['reduce1'] == 40, 'checkB/--scale 2 (40 frames): images and the picture equal to the stream (moved %s), census %s/%s/%s, validation %d' % (
            exact(j) or 'none', c and c['reduce1'], c and c['reduce2'], c and c['blur'], j['validation_errors']))

        def plant(name, expect_moved, forbid=(), extra=()):
            rc, text, j = arm('checkB', cap, nplant, 'p_' + name, list(extra), name, images=image_list)
            c = parse(text)
            m = moved(j)
            fe = [names[names.index(x)] if x in names else x for x in expect_moved]
            ok = bool(c) and all(k in m for k in fe) and not any(k in m for k in forbid)
            rep.line(ok, 'plant %s (%d frames): moved %s; must move %s%s; census replaced %s/%s/%s, kept texture %s geometry %s' % (
                name, nplant, m or 'nothing', fe or 'nothing', ' and keep %s equal' % list(forbid) if forbid else '', c and c['reduce1'], c and c['reduce2'], c and c['blur'], c and c['texture'], c and c['geometry']))
            return c, j
        rim, blur, red1, red2 = names
        plant('reduction_wrong_size', [red1, red2, blur, 'picture'], forbid=[rim])
        plant('blur_no_input', [blur, 'picture'], forbid=[rim, red1, red2])
        c, _ = plant('blur_input_null', [], forbid=names + ['picture'])
        rep.line(bool(c) and c['blur'] == 0 and c['texture'] == nplant and c['reduce1'] == nplant, 'plant blur_input_null: every blur draw refused as texture (%s of %d), the stream\'s blur stays, the reductions stay ours (%s)' % (c and c['texture'], nplant, c and c['reduce1']))
        plant('rim_reader_stale', [blur, 'picture'], forbid=[rim, red1, red2])
        c, _ = plant('grid_byte', [], forbid=names + ['picture'])
        rep.line(bool(c) and c['blur'] == 0 and c['bad'] == nplant and c['geometry'] == nplant and c['reduce1'] == nplant, 'plant grid_byte: one wrong grid byte fails the compare on every blur draw (bad %s, kept geometry %s of %d), the stream\'s blur stays, the reductions stay ours' % (c and c['bad'], c and c['geometry'], nplant))
        c, _ = plant('no_verify', [], forbid=names + ['picture'])
        rep.line(bool(c) and c['verify'] == 0 and c['blur'] == nplant, 'control no_verify: the compare is off (%s runs), the geometry is right: %s blur draws replaced, everything equal' % (c and c['verify'], c and c['blur']))
        # a pixel block buffer too small for the blocks (16 B): the pack refuses, nothing is written past the buffer, every chain draw is refused on constants and the stream's stay
        rc, text, j = arm('checkB', cap, nplant, 'p_fs_capacity', ['--rim-chain-fs-capacity', '16'], images=image_list)
        c = parse(text)
        rep.line(bool(c) and rc == 0 and not exact(j) and j['validation_errors'] == 0 and c['constants'] == 3 * nplant and c['reduce1'] + c['reduce2'] + c['blur'] == 0,
                 'plant fs capacity 16 B: the pack refuses (kept constants %s of %d), the stream\'s three draws stay (images and picture equal: moved %s), validation %d' % (c and c['constants'], 3 * nplant, exact(j) or 'none', j['validation_errors']))
        # the live image-info supplier with every width + 1: both reductions refuse (size), the stream's stay
        rc, text, j = arm('checkB', cap, nplant, 'p_live_fault', ['--image-info', 'live-fault'], images=image_list)
        c = parse(text)
        rep.line(bool(c) and not exact(j) and c['size'] == 2 * nplant and c['reduce1'] == 0 and c['reduce2'] == 0 and c['blur'] == nplant,
                 'plant image_info live-fault (width + 1): both reductions refused on size (%s of %d), the stream\'s reductions stay (images and picture equal: moved %s), the blur stays ours (%s)' % (c and c['size'], 2 * nplant, exact(j) or 'none', c and c['blur']))
        rc, text, j = arm('checkB', cap, nplant, 'p_unmasked', [], 'unmasked', images=image_list)
        rep.info('experiment unmasked on the real rim map (lit everywhere): moved %s (reported, not asserted)' % (exact(j) or 'nothing'))
        rc, text = run([fview, '--capture', cap, '--rim-chain-check', os.path.join(work, 'x.json'), '--frames', '2', '--replace-fault', 'rimchain.nonsense', '--game', game], 300)
        rep.line(rc != 0, 'loud failure: an unknown rim chain fault name is refused (exit %d)' % rc)
        rc, text = run([fview, '--capture', cap + '.missing', '--rim-chain-check', os.path.join(work, 'x.json'), '--frames', '2', '--game', game], 300)
        rep.line(rc != 0, 'loud failure: a missing capture is refused (exit %d)' % rc)
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('rim_chain_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    sys.exit(main())

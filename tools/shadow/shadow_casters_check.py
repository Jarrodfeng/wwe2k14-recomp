#!/usr/bin/env python3
"""Lane 99 (SO) offline proof, 0 game runs: the shadow passes' casters drawn by us in place through the REAL seam (fview --shadow-casters-ours), every frame of three captures.

  py -I tools\\shadow\\shadow_casters_check.py [--root DIR] [--no-build] [--only checkB,r7079,bryan] [--no-plants]

For each capture two sweeps (fview --sweep: every frame in order, so the hooks keep their state across frames as live; a hash of the PICTURE and of every image bound at fetch slot 12 per frame = the
shadow maps the wrestler / floor / ring-mat readers sample, the plane mask included):
  off  the other classes only (--wrestlers-ours --ring-ours; Bryan has no scene records: no classes)
  on   the same + --shadow-casters-ours (--validation 1)
Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. picture byte-equal on EVERY frame, and the slot-12 images byte-equal on every frame (and there are some);
  2. every replaced draw's bytes equal the stream's (census: verify bad 0, mismatch 0, layout refused 0, rows overridden 0, joins dropped 0);
  3. the census is exact against an independent oracle: lane SV's out\\tools\\caster_vertex_check.exe (it joins through the COLOUR draws, no scene record) counts the wrestler / ring-956 / chCommon draws
     of the capture; our replaced counts by class equal the oracle's minus the draws of the frames before the wrestlers' and the ring's first record (their fallback frames: the stream's draws stay);
  4. validation errors 0;
  5. planted faults on Check B: each fails its exact check (and the no_verify twin moves the picture or the slot-12 images); its unplanted twin is the clean arm.
Every fview process has a timeout; the GPU-busy refusal is retried (a few attempts, said)."""
import argparse
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# (label, folder under the main checkout's out\, scene records?, arena)
CAPTURES = [
    ('checkB', 'out/lane40/brock/capture', True, 4),
    ('r7079', 'out/lane85/r7079/capture', True, 4),
    ('bryan', 'out/lane99/bryan/capture', False, 4),
]
BRYAN_RUN = 'out/s101/runs/20261111_174549_s100_owner_bryan_entrance'
# plant -> kind: 'bytes' (the byte check catches it: draws stay the stream's), 'part' (the join), 'rows' (palette rows)
WRITER_PLANTS = ('fetch_order', 'weights_as_fill', 'xdword_colour', 'bone_zero')
# The dword-0 u8x4 element (the rigid fill / the plane proxies' xdword) did not move the picture or the reader images in the sweeps below [C: Check B, no_verify twins]. The shadow shaders consume it only
# as a truncated UNORM bone index under Bools[0] / [4] (reviewer's reading [L]; whether a fetch-1-versus-ids confusion could ever make it matter is an open lead [L], not tested here), so for these two plants
# the twin's picture result is REPORTED (INFO), never required either way: the byte compare is what guards the element.
PICTURE_BLIND = ('fetch_order', 'xdword_colour')


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


CENSUS = re.compile(r'\[rdr\]\[shadow_casters\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
DETAIL = re.compile(r'shadow casters: claimed (\d+) replaced (\d+) \(map (\d+), plane (\d+); by class wrestler (\d+) / ring (\d+) / proxy (\d+)\) \| kept: map not ours (\d+), plane unjoined (\d+) \| '
                    r'plane joins ring (\d+) proxy (\d+) \| verify (\d+) bad (\d+), layout refused (\d+), rows overridden (\d+), joins dropped (\d+), parts built (\d+)')
WRESTLER_FB = re.compile(r'\[rdr\]\[wrestlers\] ours frames (\d+), fallback (\d+)')
RING_FB = re.compile(r'\[rdr\]\[ring\] ours frames (\d+), fallback (\d+)')


def parse(text):
    c = CENSUS.search(text)
    d = DETAIL.search(text)
    if not c or not d:
        return None
    k = ['frames', 'fallback', 'replaced', 'kept', 'not_ready', 'unjoined', 'mismatch', 'variant', 'texture', 'palette', 'not_ours']
    o = dict(zip(k, map(int, c.groups())))
    kd = ['claimed', 'rep', 'map', 'plane', 'wrestler', 'ring', 'proxy', 'map_not_ours', 'plane_unjoined', 'joins_ring', 'joins_proxy', 'verify', 'bad', 'layout_refused', 'rows', 'dropped', 'built']
    o.update(dict(zip(kd, map(int, d.groups()))))
    w, r = WRESTLER_FB.search(text), RING_FB.search(text)
    o['wr_fb'] = int(w.group(2)) if w else 0
    o['ring_fb'] = int(r.group(2)) if r else 0
    return o


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
    # Bryan's case zip -> a capture folder (game data: under out\, never committed)
    bryan = os.path.join(main_co, 'out', 'lane99', 'bryan', 'capture')
    if not os.path.isfile(os.path.join(bryan, 'images.bin')) or not os.path.isdir(os.path.join(bryan, 'frames')):
        run_dir = os.path.join(main_co, BRYAN_RUN)
        zips = [os.path.join(run_dir, f) for f in os.listdir(run_dir) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(run_dir) else []
        if not zips:
            print('FAIL  input missing: Bryan\'s case zip under ' + run_dir)
            return 1
        z = zipfile.ZipFile(zips[0])
        z.extractall(os.path.join(main_co, 'out', 'lane99', 'bryan'), [n for n in z.namelist() if n.startswith('capture/')])
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'shadow', 'build_caster_vertex_check.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  caster_vertex_check build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    oracle = os.path.join(root, 'out', 'tools', 'caster_vertex_check.exe')
    for need in (fview, oracle):
        if not os.path.isfile(need):
            print('FAIL  input missing: ' + need)
            return 1
    work = os.path.join(root, 'out', 'l99check')
    os.makedirs(work, exist_ok=True)
    only = {x for x in args.only.split(',') if x}

    def sweep(label, cap, tag, extra):
        csv = os.path.join(work, '%s_%s.csv' % (label, tag))
        if os.path.isfile(csv):
            os.remove(csv)
        rc, text = run([fview, '--capture', cap, '--sweep', csv, '--sweep-slot', '12', '--game', game] + extra, 1500)
        if not os.path.isfile(csv):
            print(text[-1500:])
            raise RuntimeError('sweep %s/%s failed (exit %d)' % (label, tag, rc))
        return rc, text, read_csv(csv)

    def oracle_counts(cap, arena, to=None):
        cmd = [oracle, '--game', game, '--capture', cap, '--arena', str(arena)] + (['--to', str(to)] if to else [])
        rc, text = run(cmd, 1500)
        got = {}
        for key, name in (('wrestler', 'wrestler'), ('ring956', 'ring'), ('chcommon', 'proxy')):
            m = re.search(r'INFO  %s: (\d+) draws' % key, text)
            if not m:
                print(text[-1500:])
                raise RuntimeError('the oracle printed no %s count (exit %d)' % (key, rc))
            got[name] = int(m.group(1))
        return got

    clean_cache = {}
    try:
        for label, rel, records, arena in CAPTURES:
            if only and label not in only:
                continue
            cap = os.path.join(main_co, rel.replace('/', os.sep))
            if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isdir(os.path.join(cap, 'frames')):
                rep.line(False, '%s: input missing: the capture at %s' % (label, cap))
                continue
            classes = ['--wrestlers-ours', '--ring-ours'] if records else []
            rc0, off_text, off = sweep(label, cap, 'off', classes)
            rc1, on_text, on = sweep(label, cap, 'on', classes + ['--shadow-casters-ours', '--validation', '1'])
            n = len(off)
            frames_equal = n > 0 and set(off) == set(on)
            diff_pic = [f for f in sorted(off) if f in on and off[f]['picture'] != on[f]['picture']]
            diff_img = [f for f in sorted(off) if f in on and (off[f]['slot12_hash'] != on[f]['slot12_hash'] or off[f]['slot12_images'] != on[f]['slot12_images'])]
            have_imgs = sum(1 for f in off if int(off[f]['slot12_images']) > 0)
            rep.line(frames_equal and not diff_pic and not diff_img and have_imgs > 0,
                     '%s: %d frames: picture differs on %d, the slot-12 reader images differ on %d (frames with reader images: %d; first differing %s)' % (
                         label, n, len(diff_pic), len(diff_img), have_imgs, (diff_pic + diff_img)[:5]))
            c = parse(on_text)
            if not c:
                print(on_text[-1500:])
                rep.line(False, '%s: no shadow-caster census line in the on arm (exit %d)' % (label, rc1))
                continue
            rep.line(rc1 == 0 and 'validation errors 0' in on_text, '%s: validation errors 0 (exit %d)' % (label, rc1))
            rep.line(c['bad'] == 0 and c['mismatch'] == 0 and c['layout_refused'] == 0 and c['rows'] == 0 and c['dropped'] == 0 and c['variant'] == 0 and c['verify'] == c['replaced'],
                     '%s: every replaced draw byte-compared with the stream (verify %d of replaced %d), bad %d, mismatch %d, layout refused %d, rows overridden %d, joins dropped %d' % (
                         label, c['verify'], c['replaced'], c['bad'], c['mismatch'], c['layout_refused'], c['rows'], c['dropped']))
            # the independent oracle
            total = oracle_counts(cap, arena)
            fb = max(c['wr_fb'], c['ring_fb']) if records else None
            first = oracle_counts(cap, arena, to=fb) if fb else {'wrestler': 0, 'ring': 0, 'proxy': 0}
            if records:
                want = {'wrestler': total['wrestler'] - first['wrestler'], 'ring': total['ring'] - first['ring'], 'proxy': total['proxy']}
            else:
                want = {'wrestler': 0, 'ring': 0, 'proxy': total['proxy']}   # no scene records: the wrestlers' and the ring's joins do not exist; the plane proxy joins from the files alone
            got = {'wrestler': c['wrestler'], 'ring': c['ring'], 'proxy': c['proxy']}
            rep.line(got == want, '%s: replaced by class %s == oracle %s (SV writer check: %s over all frames, minus %d fallback frame(s) of the wrestlers / ring: %s)' % (
                label, got, want, total, fb or 0, first))
            # without scene records there is no ring hook: the ring's rope proxy (956) cannot join and stays the stream's, exactly the oracle's ring count
            want_unjoined = 0 if records else total['ring']
            rep.line(c['unjoined'] == want_unjoined and c['texture'] == 0 and c['plane_unjoined'] == want_unjoined,
                     '%s: unjoined %d, plane unjoined %d (want %d: %s); kept: not ready %d, not ours %d (props, pieces, circles)' % (
                         label, c['unjoined'], c['plane_unjoined'], want_unjoined, 'every wrestler / ring / proxy object joined after the fallback frames' if records else 'no scene records, no ring hook: the 956 draws stay the streams own',
                         c['not_ready'], c['not_ours']))
            clean_cache[label] = (cap, classes, off, c, records)
        if 'checkB' not in clean_cache or args.no_plants:
            print('shadow_casters_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
            return 1 if rep.fails else 0
        cap, classes, off, clean, records = clean_cache['checkB']

        def total_rep(c):
            return c['wrestler'] + c['ring'] + c['proxy']

        def moved(on, ref):
            return [f for f in sorted(ref) if f in on and (ref[f]['picture'] != on[f]['picture'] or ref[f]['slot12_hash'] != on[f]['slot12_hash'])]

        def plant(name, faults):
            rc, text, rows = sweep('checkB', cap, 'p_' + name.replace(',', '_'), classes + ['--shadow-casters-ours', '--replace-fault', ','.join('shadow.' + x for x in faults)])
            return parse(text), rows, text

        results = {}
        for p in WRITER_PLANTS:
            c, rows, _ = plant(p, [p])
            tw, trows, _ = plant(p + '_nv', [p, 'no_verify'])
            if not c or not tw:
                rep.line(False, 'plant %s: no census' % p)
                continue
            m = moved(trows, off)
            ok = c['bad'] > 0 and total_rep(c) < total_rep(clean) and not moved(rows, off)
            rep.line(ok, 'plant %s: the byte check fails it (verify bad %d, replaced %d of %d clean, the stream\'s draws stay: picture moved on %d frames)' % (
                p, c['bad'], total_rep(c), total_rep(clean), len(moved(rows, off))))
            blind = p in PICTURE_BLIND
            rep.line(tw['verify'] == 0 and total_rep(tw) == total_rep(clean) and (blind or len(m) > 0),
                     '%splant %s,no_verify: verify off (%d runs), same %d draws replaced, the PICTURE / reader images move on %d frames (%s)' % (
                         'INFO  (picture not required) ' if blind else '', p, tw['verify'], total_rep(tw), len(m),
                         'consumed only as a truncated UNORM bone index under Bools[0]/[4] [L]: only the byte compare guards it' if blind else 'our geometry is what is drawn'))
        c, rows, _ = plant('proxy_part_shift', ['proxy_part_shift'])
        rep.line(bool(c) and c['proxy'] == 0 and c['plane_unjoined'] > 0 and c['wrestler'] == clean['wrestler'],
                 'plant proxy_part_shift: the chCommon join one mesh off fails on counts: proxy draws replaced %s (clean %d), plane unjoined %s' % (c and c['proxy'], clean['proxy'], c and c['plane_unjoined']))
        c, rows, _ = plant('palette_from_colour', ['palette_from_colour'])
        m = moved(rows, off)
        rep.line(bool(c) and c['rows'] > 0 and len(m) > 0, 'plant palette_from_colour: the colour pass\'s rows override the shadow draw\'s (rows overridden %s, clean 0): the map / picture moves on %d frames' % (c and c['rows'], len(m)))
        c, rows, _ = plant('no_verify', ['no_verify'])
        rep.line(bool(c) and c['verify'] == 0 and total_rep(c) == total_rep(clean) and not moved(rows, off), 'control no_verify: the compare is off (%s runs), the geometry is right: %d draws replaced, picture byte-equal' % (c and c['verify'], total_rep(c) if c else -1))
        rc, text = run([fview, '--capture', cap, '--frame', '5', '--ops', '--json', os.path.join(work, 'x.json'), '--shadow-casters-ours', '--replace-fault', 'shadow.nonsense', '--game', game], 300)
        rep.line(rc != 0, 'loud failure: an unknown shadow fault name is refused (exit %d)' % rc)
        rc, text = run([fview, '--capture', cap + '.missing', '--sweep', os.path.join(work, 'x.csv'), '--shadow-casters-ours', '--game', game], 300)
        rep.line(rc != 0, 'loud failure: a missing capture is refused (exit %d)' % rc)
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('shadow_casters_check: %s' % ('PASS' if not rep.fails else 'FAIL (%d)' % rep.fails))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    sys.exit(main())

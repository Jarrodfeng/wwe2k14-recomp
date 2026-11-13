#!/usr/bin/env python3
"""Lane 82 acceptance test (offline, 0 game runs): the seated floor people (draw site 0x8276ED60) drawn by our crowd through the crowd seam, on Check B's capture.

  py -I tools\\crowd\\floor_check.py [--root DIR] [--no-build] [--frames 341]

Check B's scene records were written before the floor observer existed and hold no floor members, so the REAL seam (CrowdOurs + the binder's crowd origin + the floor switch) is run on
records that fview adds the floor members to (fview --crowd-floor-synth, test only; the members come from tools\\crowd\\floor_join.py --emit: the draws' own c76-c78 words and the
type from the seat join with the player's own seat text). NOT proven here: the hook's register reads on a running game (src\\scene_live\\crowd_floor.h; unit-tested with a fake memory by
tools\\scene\\crowd_make --contract), the pairing of real floor records, live timing, the phone. Items (docs\\renderer\\CROWD_FAMILY_8276ED60.md section 4):
  A  floor_join.py: all 341 frames join (28,496 draws), its six planted twins each FAIL their check
  B  fview --crowd-ours --crowd-floor --crowd-floor-synth, every frame (--replay): floor draws drawn + kept == floor stream draws (the draws of the frames ours drew, from A's file); drawn and kept
     equal an INDEPENDENT count from the records (rsc_dump: does record j-1 carry a LOD 0 palette for the member's type); (class, LOD 0) floor checks all equal, pose residual 0.0000 on
     every logged floor class; every member's slot 0 is its own stream draw's; no bad pairing; validation errors 0. The picture on 3 frames: floor-on vs floor-off (stands ours in both):
     <= PIX_MAX pixels differ by > 12. Plants floor_lod1, floor_type_shift, floor_first_texture, floor_dup, floor_drop_no_draw each FAIL their named check.
     Safety: floor claimed with records that hold no floor members -> every frame falls back to the stream (counted, no hole: the picture equals the stream's).
     Switch off (--crowd-ours alone): the picture md5 and the census numbers are the base's (BASE_CLEAN_MD5, per GPU like crowd_ours_check.py's pin), and the report names no floor.
  D  lane 73's census (tools\\census\\leftovers.py, the shipped aggregation): the 0x8276ED60 row's stream draws drop by exactly the floor members drawn in that frame
A check that cannot read its input fails loudly."""
import argparse
import hashlib
import importlib.util
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FLOOR_SITE = 0x8276ED60
PIX = 12.0
PIX_MAX = 4           # floor-on vs floor-off: pixels differing by > 12 (of 921,600); the lane's bar is 0, measured 1 on frame 200 (a silhouette pixel; see CROWD_FLOOR.md)
# The md5 of the picture of frame 200 with --crowd-ours (no floor), drawn by the BASE (s101-renderer-arc cc639251, before this lane) on the machine it was recorded on:
# RTX 4090, NVIDIA driver 93D28000, Vulkan 1.4.325 (the same machine and driver as crowd_ours_check.py's BASE_STREAM_MD5). Another GPU / driver may draw other bits: re-record from a build of the base.
# lane 170 (2026-11-13): re-pinned, same reason and evidence as crowd_ours_check.py's BASE_STREAM_MD5 (the old pin is the old 16-bit target rule's picture).
BASE_CLEAN_MD5 = 'c16359570b735b269db944a2607157bc'
BASE_CLEAN_MEMBERS = 23000   # the base's census for that run: members drawn == stream draws dropped


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    for attempt in range(4):  # the GPU is shared: only the "frame ring" start failure is retried (docs/agents/s101/common.md)
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if p.returncode == 0 or 'frame ring' not in text:
            return p.returncode, text
        print('[floor_check] "frame ring" error, retry %d' % (attempt + 1), flush=True)
        time.sleep(15)
    return p.returncode, text


class Result:
    def __init__(self):
        self.fails = 0

    def check(self, ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        self.fails += not ok
        return ok


RE_CENSUS = re.compile(r'ours frames (\d+), fallback (\d+), stream draws dropped (\d+), members drawn (\d+)')
RE_PAIR = re.compile(r'pairing (\d+) of (\d+) worlds match the stream \((\d+) stream crowd draws dropped\), (\d+) frames with a bad pairing, (\d+) of (\d+) \(class, LOD\) checks: triangles \+ vertices equal the stream.s, and (\d+) posed vertices match')
RE_FLOOR = re.compile(r'floor members drawn (\d+), floor draws kept (\d+) of (\d+) floor stream draws, floor \(class, LOD 0\) checks: (\d+) of (\d+) equal the stream.s triangles \+ vertices and (\d+) match its posed blob, (\d+) of (\d+) members use the slot-0 picture')
RE_FLOOR_LINE = re.compile(r'class type (\d+) LOD 0 \(floor\):.*max ([0-9.]+) units')


def parse(text):
    c, p, f = RE_CENSUS.search(text), RE_PAIR.search(text), RE_FLOOR.search(text)
    d = {}
    if c:
        d.update(ours=int(c.group(1)), fallback=int(c.group(2)), dropped=int(c.group(3)), members=int(c.group(4)))
    if p:
        d.update(pm=int(p.group(1)), pt=int(p.group(2)), sd=int(p.group(3)), bad=int(p.group(4)), ct=int(p.group(6)), cg=int(p.group(5)), cp=int(p.group(7)))
    if f:
        d.update(F=int(f.group(1)), K=int(f.group(2)), S=int(f.group(3)), fg=int(f.group(4)), ft=int(f.group(5)), fp=int(f.group(6)), mok=int(f.group(7)), mt=int(f.group(8)))
    d['floor_max'] = [float(m.group(2)) for m in RE_FLOOR_LINE.finditer(text)]
    return d


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def palette_types(dump_text):
    """From rsc_dump's text: the person types that carry a LOD 0 palette in the record (any crowd object that is skinned with bones, whose asset is the type's LOD 0 body or its card)."""
    asset_entry = {}
    for m in re.finditer(r'^\s+asset (\d+): kind 0 pac "[^"]*" entry "([^"]*)"', dump_text, re.M):
        asset_entry[int(m.group(1))] = m.group(2)
    have = set()
    for m in re.finditer(r'^\s+object \d+: id "(crowd:[^"]*)" asset (\d+) class \d+ flags 0x([0-9a-f]+) pos \([^)]*\) bones (\d+)\+(\d+)', dump_text, re.M):
        if not (int(m.group(3), 16) & 4) or int(m.group(5)) == 0:
            continue
        e = asset_entry.get(int(m.group(2)), '')
        if e.startswith('card:'):
            have.add(int(e[5:]))
        elif e.isdigit() and 20000 <= int(e) < 20020:
            have.add(int(e) - 20000)
    return have


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=341)
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, 'out', 's101', 'crowd', 'case_brock', 'capture')
    if not os.path.isdir(os.path.join(cap, 'scene')) or not os.path.isdir(game):
        print('FAIL  input missing: Check B\'s extracted capture with scene records (%s) and the game files (%s)' % (cap, game))
        return 1
    out = os.path.join(root, 'out', 's101', 'crowd_floor')
    os.makedirs(out, exist_ok=True)
    R = Result()
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1

    # ---- A
    emit = os.path.join(out, 'floor_members.txt')
    rc, text = run([sys.executable, '-I', os.path.join(HERE, 'floor_join.py'), '--selftest', '--emit', emit], 900)
    R.check(rc == 0 and 'floor_join: PASS' in text, 'A  floor_join.py: every floor draw of all frames joins one seat, vertex counts / images / programs / world y as lane 78 measured, and its six plants each FAIL their check (exit %d)' % rc)
    if rc:
        print(text[-1500:])
    if not os.path.isfile(emit):  # the selftest mode writes no sidecar (plants run); make it from a clean run
        rc, text = run([sys.executable, '-I', os.path.join(HERE, 'floor_join.py'), '--emit', emit], 900)
        if rc:
            print(text[-1500:])
            print('FAIL  floor_join --emit (exit %d)' % rc)
            return 1
    per_frame = {}
    with open(emit) as fh:
        for line in fh:
            if line.startswith('#') or not line.strip():
                continue
            fr, typ = int(line.split()[0]), int(line.split()[1])
            per_frame.setdefault(fr, []).append(typ)
    total_floor = sum(len(v) for v in per_frame.values())

    base = [exe, '--capture', cap, '--crowd-ours', '--game', game]
    synth = ['--crowd-floor', '--crowd-floor-synth', emit]

    # ---- B1: every frame, clean
    census_csv = os.path.join(out, 'census.csv')
    rc, text = run(base + synth + ['--replay', '--frames', str(args.frames), '--validation', '1', '--crowd-census', census_csv], 1800)
    if rc:
        print(text[-1500:])
    r = parse(text)
    R.check(rc == 0 and 'validation errors 0' in text and 'validation active: yes' in text and 'F' in r, 'B  %d-frame replay with the floor on: Vulkan validation errors 0 (exit %d)' % (args.frames, rc))
    if 'F' not in r:
        print('FAIL  no floor census in the clean run')
        return 1
    drawn_frames = set()
    with open(census_csv) as fh:
        for line in fh:
            parts = line.strip().split(',')
            if len(parts) < 2:
                continue
            a, b = parts[:2]
            if a.isdigit() and b.isdigit() and int(b) > 0:
                drawn_frames.add(int(a))
    expect_S = sum(len(v) for fr, v in per_frame.items() if fr in drawn_frames)
    R.check(r['S'] == expect_S and r['S'] > 0, 'B  floor stream draws seen in the frames ours drew: %d == %d from floor_join.py\'s file (%d floor draws in all %d frames; %d frames drawn)' % (r['S'], expect_S, total_floor, args.frames, len(drawn_frames)))
    R.check(r['F'] + r['K'] == r['S'] and r['F'] > 0, 'B  floor members drawn %d + floor draws kept on the stream %d == floor stream draws %d (drawn + dropped-and-kept accounts for every floor draw)' % (r['F'], r['K'], r['S']))
    R.check(r['bad'] == 0 and r['pm'] == r['pt'] and r['pt'] == r['sd'], 'B  pairing: %d of %d member worlds equal a stream crowd draw\'s c76-c78 bit for bit, members == stream draws dropped (%d), bad-pairing frames %d (the floor members pair as the stands do)' % (r['pm'], r['pt'], r['sd'], r['bad']))
    R.check(r['ft'] > 0 and r['fg'] == r['ft'] and r['fp'] == r['ft'] and r['ct'] == r['cg'] == r['cp'], 'B  (class, LOD 0) of the floor members: %d of %d have the stream\'s triangle + vertex counts and %d of %d posed vertices equal its blob (all classes: %d of %d)' % (r['fg'], r['ft'], r['fp'], r['ft'], r['cp'], r['ct']))
    R.check(len(r['floor_max']) > 0 and all(x == 0.0 for x in r['floor_max']), 'B  pose residual of the logged floor classes (%d lines): max %.4f units (must be 0.0000, as the stands at LOD 0)' % (len(r['floor_max']), max(r['floor_max'] or [-1])))
    R.check(r['mt'] > 0 and r['mok'] == r['mt'], 'B  material: %d of %d members use the slot-0 picture of their OWN stream draw' % (r['mok'], r['mt']))

    # independent count of drawn vs kept from the records (rsc_dump): record j-1 carries the type's LOD 0 palette -> drawn, else kept
    rsc_dump = None
    for cand in (os.path.join(root, 'out', 'tools', 'rsc_dump.exe'), os.path.join(main_co, 'out', 'tools', 'rsc_dump.exe')):
        if os.path.isfile(cand):
            rsc_dump = cand
            break
    if rsc_dump is None:
        R.check(False, 'B  independent count: rsc_dump.exe not found (build it: pwsh tools\\scene\\build_rsc.ps1)')
    else:
        exp_F = exp_K = 0
        have_cache = {}
        for fr in sorted(drawn_frames):
            if fr not in per_frame:
                continue
            rec = os.path.join(cap, 'scene', 'frame_%06d.rsc' % (fr - 1))
            p = subprocess.run([rsc_dump, rec], capture_output=True, text=True, errors='replace', timeout=120)
            if p.returncode:
                R.check(False, 'B  independent count: rsc_dump failed on %s' % rec)
                break
            have = palette_types(p.stdout)
            for typ in per_frame[fr]:
                if typ in have:
                    exp_F += 1
                else:
                    exp_K += 1
        R.check(exp_F == r['F'] and exp_K == r['K'], 'B  drawn / kept counted independently from the records (does record j-1 carry the member type\'s LOD 0 palette: rsc_dump text): drawn %d == %d, kept %d == %d' % (exp_F, r['F'], exp_K, r['K']))

    # ---- B2: pictures on 3 frames, floor on vs off
    def arm(name, frame, extra):
        d = os.path.join(out, 'arm_%s_%d' % (name, frame))
        rc, txt = run([exe, '--capture', cap, '--frame', str(frame), '--judge-out', d] + extra, 900)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(txt[-1500:])
            raise RuntimeError('arm %s frame %d failed (exit %d)' % (name, frame, rc))
        return np.asarray(Image.open(png).convert('RGB'), dtype=np.float64), txt, png
    lum = np.array([0.3, 0.59, 0.11])
    L = lambda a: a @ lum
    px = lambda a, b: int((np.abs(L(a) - L(b)) > PIX).sum())
    try:
        worst = 0
        for frame in (120, 200, 260):
            off, _, _ = arm('off', frame, ['--crowd-ours', '--game', game])
            on, txt, _ = arm('on', frame, ['--crowd-ours', '--game', game] + synth)
            n = px(on, off)
            worst = max(worst, n)
            f = parse(txt)
            R.check(n <= PIX_MAX and f.get('F', 0) > 0, 'B  picture frame %d: floor on vs floor off (stands ours in both): %d pixels differ by > %d luminance (bar <= %d; %d floor members drawn, %d kept up to this frame)' % (frame, n, PIX, PIX_MAX, f.get('F', 0), f.get('K', 0)))
        # the planted faults on frame 200
        faults = {
            'floor_lod1': 'floor (class, LOD 0) checks: the (class, LOD) vertex / triangle counts differ from the stream draw\'s',
            'floor_type_shift': 'floor (class, LOD 0) checks: vertex counts / pose differ',
            'floor_first_texture': 'material: members whose slot 0 is not their own stream draw\'s',
            'floor_dup': 'pairing: members != stream draws dropped (bad-pairing frames)',
            'floor_drop_no_draw': 'pairing: members != stream draws dropped (bad-pairing frames)',
        }
        for name, what in faults.items():
            _, txt, _ = arm(name, 200, ['--crowd-ours', '--game', game] + synth + ['--crowd-fault', name])
            f = parse(txt)
            if name in ('floor_lod1', 'floor_type_shift'):
                ok = f.get('ft', 0) > 0 and (f['fg'] < f['ft'] or f['fp'] < f['ft'])
                got = '%d of %d geometry, %d of %d pose' % (f.get('fg', -1), f.get('ft', -1), f.get('fp', -1), f.get('ft', -1))
            elif name == 'floor_first_texture':
                ok = f.get('mt', 0) > 0 and f['mok'] < f['mt']
                got = '%d of %d own picture' % (f.get('mok', -1), f.get('mt', -1))
            else:
                ok = f.get('bad', 0) > 0
                got = '%d bad-pairing frames, %d fallback frames' % (f.get('bad', -1), f.get('fallback', -1))
            R.check(ok, 'B  planted %s must FAIL %s: %s' % (name, what, got))
        # claimed floor, records without floor members: every frame falls back, no hole
        stream, _, _ = arm('stream', 200, [])
        nof, txt, _ = arm('nofloor_records', 200, ['--crowd-ours', '--game', game, '--crowd-floor'])
        f = parse(txt)
        n = px(nof, stream)
        R.check(f.get('bad', 0) > 0 and f.get('members', 1) == 0 and n <= PIX_MAX, 'B  safety: the floor claimed with records that hold no floor members: %d bad-pairing frames, 0 members drawn, the picture is the stream\'s (%d pixels differ by > %d)' % (f.get('bad', -1), n, PIX))
        # seam-wide: a member missing from the record on a frame that is NOT verified leaves its claimed stream draw in place (no hole)
        miss, txt, _ = arm('missing', 200, ['--crowd-ours', '--game', game, '--crowd-fault', 'record_missing_member'])
        base_on, _, _ = arm('clean', 200, ['--crowd-ours', '--game', game])
        kept = re.search(r'kept for members ours could not draw: (\d+)', txt)
        n_miss, n_clean = px(miss, stream), px(base_on, stream)
        R.check(bool(kept) and int(kept.group(1)) > 0 and n_miss <= n_clean + PIX_MAX, 'B  planted record_missing_member (verify off): the missing member\'s stream draw stays the game\'s: %s stream draws kept, %d px differ from the stream (clean ours: %d)' % (kept.group(1) if kept else '?', n_miss, n_clean))
        # switch off
        off200, txt, off_png = arm('off', 200, ['--crowd-ours', '--game', game])
        md5 = hashlib.md5(open(off_png, 'rb').read()).hexdigest()
        f = parse(txt)
        R.check(md5 == BASE_CLEAN_MD5 and f.get('members') == BASE_CLEAN_MEMBERS and 'floor' not in txt.split('crowd ours:')[-1], 'B  switch off: --crowd-ours alone gives the base\'s picture (md5 %s %s %s) and census (members drawn %s, base %d), the report names no floor' % (md5, '==' if md5 == BASE_CLEAN_MD5 else '!=', BASE_CLEAN_MD5, f.get('members'), BASE_CLEAN_MEMBERS))
    except RuntimeError as e:
        R.check(False, str(e))

    # ---- D: lane 73's census row
    try:
        L73 = load_module(os.path.join(root, 'tools', 'census', 'leftovers.py'), 'leftovers')
    except Exception as e:
        R.check(False, 'D  cannot load tools/census/leftovers.py: %s' % e)
        L73 = None
    if L73:
        def census(frame, extra):
            j = os.path.join(out, 'ops_%d_%d.json' % (frame, len(extra)))
            rc, txt = run([exe, '--capture', cap, '--frame', str(frame), '--ops', '--json', j, '--crowd-ours', '--game', game] + extra, 900)
            if rc or not os.path.isfile(j):
                print(txt[-1500:])
                raise RuntimeError('census run failed (exit %d)' % rc)
            draws, _, _ = L73.load_draws(j)
            agg = L73.aggregate(L73.mark_particle_heads(draws), None)
            row = [x for x in agg['by_site'] if x['key'][2] == '0x%08X' % FLOOR_SITE]
            return (row[0] if row else {'draws': 0, 'stream': 0, 'ours': 0}), parse(txt)
        try:
            row_off, _ = census(200, [])
            row_on, f200 = census(200, synth)
            _, f199 = census(199, synth)
            moved = row_off['stream'] - row_on['stream']
            delta = f200['F'] - f199['F']
            R.check(row_off['draws'] > 0 and row_off['ours'] == 0 and moved == delta and moved == row_on['ours'] and row_on['draws'] == row_off['draws'] and delta > 0,
                    'D  lane 73 census, the %s row: %d floor draws, stream %d -> %d with the floor on (ours %d); fview\'s own counters (frame 199 -> 200) say %d floor members drawn in frame 200: the row moved by exactly the drawn count' % ('0x%08X' % FLOOR_SITE, row_off['draws'], row_off['stream'], row_on['stream'], row_on['ours'], delta))
        except RuntimeError as e:
            R.check(False, 'D  ' + str(e))
    print('floor_check: %s' % ('PASS' if not R.fails else 'FAIL (%d)' % R.fails))
    return 1 if R.fails else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Lane 45 (W2) acceptance test, offline, 0 game runs: the wrestlers replaced in place (src\\renderer_scene\\wrestler_ours.*, wrestler_core.h; docs\\renderer\\WRESTLER_PROMOTION_MAP.md
section 11), on Check B's run 7051 case zip.

  py -I tools\\wrestler\\wrestler_ours_check.py [--root DIR] [--no-build] [--quick] [--sheet-dir DIR]

Two halves, one capture:
  CPU   out\\tools\\wrestler_ours_check.exe: the SHIPPED decision code (WrestlerCore) over every frame (stream frame j with record j-1), each replaced draw proven independently
        against the stream's captured vertex bytes, index bytes and palette rows. Clean: Brock (slot 0) palette draws REFUSED (the real bug, counted as Palette), the exact skeletons
        (slots 6, 7, 8) replaced, nothing wrong. Every plant with the checks ON: nothing wrong gets through. Every plant with the checks OFF: the independent proof sees it
        (a positive control for the proof itself).
  PIC   fview --wrestlers-ours (the real binder, hook and renderer), pictures md5 / pixel-compared with the stream arm at two frames: replaced draws are picture-identical; every
        plant with the checks on is the stream's picture; with the checks off it breaks the picture (the uv-halves swap is measured against
        the same arm without the swap).
The sheet (stream | ours | the update-hook palette accepted for Brock | the bind pose), zoomed on the wrestlers, goes to <main checkout>\\out\\s101\\wrestlers_ours\\.
A missing input FAILS (never skips)."""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def md5(path):
    with open(path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


PIECE_PLANTS = ['piece_wrong_owner', 'piece_node_off', 'piece_body_scale']   # S101 lane 48
PLANTS = ['bind_pose', 'record_off_by_one', 'part_off_by_one', 'wrong_attire', 'scale_one', 'lent_constants', 'uv_swap'] + PIECE_PLANTS
# plant -> the independent-proof counter that must be > 0 when the checks are off ("the proof sees it")
SEES = {'bind_pose': ['wrong_rows'], 'record_off_by_one': ['wrong_rows'], 'part_off_by_one': ['wrong_count', 'wrong_vertex', 'wrong_index'],
        'wrong_attire': ['wrong_count', 'wrong_vertex', 'wrong_index'], 'scale_one': ['wrong_vertex'], 'lent_constants': ['wrong_rows'], 'uv_swap': ['wrong_vertex'],
        'piece_wrong_owner': ['wrong_rows'], 'piece_node_off': ['wrong_rows'], 'piece_body_scale': ['wrong_vertex']}
# with the checks ON, why the plant's draws stay the stream's (census / CPU counter that must be > 0)
REFUSED_BY = {'bind_pose': 'palette', 'record_off_by_one': 'palette', 'part_off_by_one': 'mismatch', 'wrong_attire': 'palette', 'scale_one': 'mismatch', 'lent_constants': 'palette', 'uv_swap': 'mismatch',
              'piece_wrong_owner': 'palette', 'piece_node_off': 'palette', 'piece_body_scale': 'mismatch'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true', help='fewer frames per CPU arm (development)')
    ap.add_argument('--sheet-dir', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        print("FAIL  input missing: Check B's case zip under " + runs)
        return 1
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1
    out = os.path.join(root, 'out', 's101', 'wrestlers_ours')
    # S101 lane 100: Check I's WM30 case (run 7084): a body whose part 1 is ANOTHER attire's piece of the same character pac
    wm_runs = os.path.join(main_co, 'out', 's101', 'runs', '20261112_131029_s100_owner_s90_wm30')
    wm_zips = [os.path.join(wm_runs, f) for f in os.listdir(wm_runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(wm_runs) else []
    if not wm_zips:
        print("FAIL  input missing: Check I's WM30 case zip under " + wm_runs)
        return 1
    wm_cap = os.path.join(out, 'case_wm30', 'capture')
    if not os.path.isfile(os.path.join(wm_cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'case_wm30'), exist_ok=True)
        zipfile.ZipFile(wm_zips[0]).extractall(os.path.join(out, 'case_wm30'))
    if not os.path.isfile(os.path.join(wm_cap, 'images.bin')):
        print('FAIL  the WM30 capture did not unpack: ' + wm_cap)
        return 1
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'case_brock'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'case_brock'))
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        print('FAIL  the capture did not unpack: ' + cap)
        return 1
    if not args.no_build:
        for what, script in [('wrestler_ours_check', 'tools/wrestler/build_wrestler_ours_check.ps1'), ('fview', 'tools/fview/build.ps1')]:
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, script)], 1800)
            if rc:
                print(text[-3000:])
                print('FAIL  %s build (exit %d)' % (what, rc))
                return 1
    cpu_exe = os.path.join(root, 'out', 'tools', 'wrestler_ours_check.exe')
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview_exe = _fe.find_or_exit(root=root, kind='dev')
    for p in (cpu_exe, fview_exe):
        if not os.path.isfile(p):
            print('FAIL  missing ' + p)
            return 1

    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    # ---------------- CPU half ----------------
    def cpu(faults, frames):
        cmd = [cpu_exe, '--capture', cap, '--game', game, '--frames', frames]
        if faults:
            cmd += ['--fault', faults]
        rc, text = run(cmd, 1200)
        m = re.search(r'^RESULT (.*)$', text, re.M)
        if rc or not m:
            print(text[-2500:])
            raise RuntimeError('cpu arm %r failed (exit %d)' % (faults, rc))
        r = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())}
        r['slots'] = {s: (int(a), int(b)) for s, a, b in re.findall(r'^SLOT (\S+) replaced=(\d+) palette_refused=(\d+)', text, re.M)}
        r['bodies'] = re.findall(r'^BODY (.*)$', text, re.M)
        pm = re.search(r'^PIECES (.*)$', text, re.M)
        r['pieces'] = {k: int(v) for k, v in (kv.split('=') for kv in pm.group(1).split())} if pm else None   # linked_object_frames; the rest are DRAWS
        r['text'] = text
        return r

    def cpu_live(faults, frames, *extra):
        cmd = [cpu_exe, '--capture', cap, '--game', game, '--frames', frames, '--live', *extra]
        if faults:
            cmd += ['--fault', faults]
        rc, text = run(cmd, 1200)
        m = re.search(r'^RESULT (.*)$', text, re.M)
        if rc or not m:
            print(text[-2500:])
            raise RuntimeError('cpu live arm %r failed (exit %d)' % (faults, rc))
        r = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())}
        cm = re.search(r'^CORRUPT set=(\d+) frame=(\d+) detected_frame=(-?\d+)', text, re.M)
        r['corrupt'] = (int(cm.group(1)), int(cm.group(3))) if cm else None
        return r

    full = '0-340' if not args.quick else '160-200'
    short = '170-180'
    clean = cpu('', full)
    print('CPU clean (frames %s): claimed %d replaced %d refused palette %d not ours %d, proof draws %d; bodies: %s' % (full, clean['claimed'], clean['replaced'], clean['kept_palette'], clean['kept_not_ours'],
                                                                                    clean['proof_draws'], '; '.join(clean['bodies'])))
    cm0 = re.search(r'vertex elements compared (\d+): exact (\d+) near (\d+) bad (\d+)', clean['text'])
    print('CPU clean vertex-compare counts: ' + (('%s elements, %s exact, %s near, %s bad' % cm0.groups()) if cm0 else '(missing)'))
    verdict(cm0 is not None and int(cm0.group(4)) == 0, 'clean: vertex elements compared with the stream: 0 bad')
    verdict(clean['replaced'] > 1000 and clean['proof_draws'] == clean['replaced'], 'clean: %d draws replaced, every one proven' % clean['replaced'])
    verdict(clean['wrong_vertex'] == 0 and clean['wrong_index'] == 0 and clean['wrong_rows'] == 0 and clean['wrong_count'] == 0,
            'clean: nothing wrong (vertex bytes, index bytes, palette rows, counts all equal the stream draw\'s)')
    verdict(clean['objects_joined'] >= 4 and clean['scale_measured'] >= 4 and len(clean['bodies']) >= 4, 'clean: %d bodies joined by content + bytes, one scale measured each' % len(clean['bodies']))
    scales = [float(re.search(r'scale (\S+)', b).group(1)) for b in clean['bodies']]
    expect = {1.0105: 'Brock', 0.9104: 'ring announcer', 0.9209: 'slot 7', 0.9578: 'slot 8'}  # lane 42's measured scales
    for want, who in expect.items():
        verdict(any(abs(s - want) < 2e-4 for s in scales), 'scale %.4f measured by us for %s (lane 42: %.4f)' % (next((s for s in scales if abs(s - want) < 2e-4), 0), who, want))
    slots = clean['slots']
    verdict(all(slots.get(s, (0, 1))[0] > 0 and slots.get(s, (0, 1))[1] == 0 for s in ('6', '7', '8')), 'exact skeletons (slots 6, 7, 8): replaced %s, never refused' % {s: slots.get(s) for s in ('6', '7', '8')})
    b = slots.get('0', (0, 0))
    verdict(b[1] > 0 and clean['kept_palette'] == b[1],
            "Brock (slot 0): %d draws REFUSED as Palette (today's update-hook palette, the real bug re-found by the per-draw check), %d replaced (their rows are exact); no other body is palette-refused" % (b[1], b[0]))
    verdict(clean['kept_mismatch'] == 0 and clean['kept_variant'] == 0, 'clean: no mismatch / variant refusals (the layouts, counts and bytes all fit)')
    verdict(clean['replaced'] + sum(clean['kept_' + k] for k in ('not_ready', 'unjoined', 'mismatch', 'variant', 'texture', 'palette', 'not_ours')) == clean['claimed'], 'clean: replaced + kept == claimed (%d)' % clean['claimed'])

    # S101 lane 48: the pieces (hair, alpha pieces: own 7 / 11-node tables, rows from the owner body's node of the same NAME)
    pc = clean['pieces']
    print('CPU clean pieces: %s' % pc)
    verdict(pc is not None and pc['replaced'] > 0 and pc['linked_object_frames'] > 0 and pc['palette_refused'] == 0 and pc['no_owner'] == 0
            and pc['proof_draws'] == pc['replaced'] and pc['proof_wrong_rows'] == 0 and pc['proof_wrong_vertex'] == 0,
            'pieces: %s (3 piece objects of run 7051 linked to their owner body by node name; all counts are draws except linked_object_frames; every piece draw replaced and proven by the independent proof, 0 wrong; 0 refused, 0 without an owner)' % pc)
    verdict(sum(1 for b in clean['bodies'] if re.search(r'nodes (7|11) ', b)) == 3, 'pieces: the 3 piece models (7 / 11 nodes) are joined by content + bytes, each with its own measured scale')
    base_off = cpu('checks_off', short)   # the checks-off-only baseline: Brock's palette draws are already wrong there; a piece plant must add PIECE wrongness to it
    print('CPU checks_off-only baseline (short window), pieces: %s' % base_off['pieces'])
    verdict(base_off['pieces']['proof_wrong_rows'] == 0 and base_off['pieces']['proof_wrong_vertex'] == 0, 'baseline checks_off alone: 0 wrong rows / vertices on PIECE draws (its wrong draws are Brock\'s: %d wrong rows overall)' % base_off['wrong_rows'])
    for plant in PLANTS:
        on = cpu(plant, short)
        if plant in PIECE_PLANTS and plant != 'piece_body_scale':
            verdict(on['pieces'] is not None and on['pieces']['palette_refused'] > 0, 'plant %-18s checks ON : piece draws refused as Palette (%s)' % (plant, on['pieces']))
        sees_on = [on[k] for k in SEES[plant]]
        verdict(on['wrong_vertex'] == 0 and on['wrong_index'] == 0 and on['wrong_rows'] == 0 and on['wrong_count'] == 0 and on['kept_' + REFUSED_BY[plant]] > 0,
                'plant %-18s checks ON : nothing wrong gets through, refused as %s (%d); replaced %d of %d claimed' % (plant, REFUSED_BY[plant], on['kept_' + REFUSED_BY[plant]], on['replaced'], on['claimed']))
        off = cpu(plant + ',checks_off', short)
        if plant in PIECE_PLANTS:   # judged on piece draws only, against the checks_off-only baseline
            key = 'proof_wrong_vertex' if plant == 'piece_body_scale' else 'proof_wrong_rows'
            verdict(off['pieces'][key] > base_off['pieces'][key], 'plant %-18s checks OFF: the independent proof sees it on PIECE draws (%s %d vs %d in the checks_off-only baseline)' % (plant, key, off['pieces'][key], base_off['pieces'][key]))
        else:
            verdict(any(off[k] > 0 for k in SEES[plant]), 'plant %-18s checks OFF: the independent proof sees it (%s)' % (plant, ', '.join('%s %d' % (k, off[k]) for k in SEES[plant])))
        del sees_on
    acc = cpu('accept_record_palette', short)
    verdict(acc['wrong_rows'] > 0 and acc['wrong_vertex'] == 0, "plant accept_record_palette (today's update-hook palette for Brock accepted): the proof sees %d wrong draws, only the palette rows" % acc['wrong_rows'])

    # S101 lane 100: WM30 (Check I, run 7084). Live said mismatch 722: one body's part 1 (780 vertices) is built from another attire of the same pac (uv and tangent differ in 230 / 53
    # vertices of the object's own model); the object joined on its biggest draw, so every later frame refused that part. The part's source is now settled by the stream's own bytes.
    def cpu_wm(faults, frames):
        cmd = [cpu_exe, '--capture', wm_cap, '--game', game, '--frames', frames, '--refusals']
        if faults:
            cmd += ['--fault', faults]
        rc, text = run(cmd, 1200)
        m = re.search(r'^RESULT (.*)$', text, re.M)
        if rc or not m:
            print(text[-2500:])
            raise RuntimeError('wm30 cpu arm %r failed (exit %d)' % (faults, rc))
        r = {k: int(v) for k, v in (kv.split('=') for kv in m.group(1).split())}
        pa = re.search(r'parts from another attire of the pac (\d+)', text)
        r['other_attire'] = int(pa.group(1)) if pa else -1
        r['refusals'] = re.findall(r'^REFUSAL (.*)$', text, re.M)
        return r
    wm_frames = '0-339' if not args.quick else '200-260'
    wc = cpu_wm('', wm_frames)
    print('CPU WM30 clean (frames %s): claimed %d replaced %d mismatch %d unjoined %d, parts from another attire %d' % (wm_frames, wc['claimed'], wc['replaced'], wc['kept_mismatch'], wc['kept_unjoined'], wc['other_attire']))
    verdict(wc['kept_mismatch'] == 0 and wc['kept_unjoined'] == 0 and wc['kept_variant'] == 0 and wc['replaced'] > 1000 and wc['proof_draws'] == wc['replaced'],
            'WM30 clean: %d draws replaced and proven, mismatch 0 / unjoined 0 / variant 0 (live run 7084 said mismatch 722)' % wc['replaced'])
    verdict(wc['wrong_vertex'] == 0 and wc['wrong_index'] == 0 and wc['wrong_rows'] == 0 and wc['wrong_count'] == 0, 'WM30 clean: nothing wrong (vertex bytes, index bytes, palette rows, counts all equal the stream draw\'s)')
    verdict(wc['other_attire'] >= 1, 'WM30 clean: a part taken from another attire of the character pac (%d), settled by the stream\'s bytes' % wc['other_attire'])
    wp = cpu_wm('own_model_only', wm_frames)
    print('CPU WM30 plant own_model_only: mismatch %d replaced %d; %s' % (wp['kept_mismatch'], wp['replaced'], wp['refusals'][:2]))
    verdict(wp['kept_mismatch'] > 0 and wp['replaced'] < wc['replaced'] and wp['wrong_vertex'] == 0 and wp['wrong_rows'] == 0 and wp['other_attire'] == 0 and any('role uv' in x for x in wp['refusals']),
            'WM30 plant own_model_only (the bug: every part from the one joined model): the real fault re-found, %d draws refused as mismatch on the uv role, none wrong, %d replaced against the clean %d' % (wp['kept_mismatch'], wp['replaced'], wc['replaced']))

    # S101 lane 88: the proof frames (live-style core: pinned parts). Clean: nothing wrong, pins used, and the same replaced / refused counts as the verify-all core on the same frames.
    # A vertex blob inverted after the pin is refused within 32 frames; the planted twin no_proof_frames never sees it (the check requiring detection FAILS).
    live_frames = '150-300'
    base_live = cpu('', live_frames)
    live_clean = cpu_live('', live_frames)
    verdict(live_clean['wrong_vertex'] == 0 and live_clean['wrong_index'] == 0 and live_clean['wrong_rows'] == 0 and live_clean['wrong_count'] == 0
            and all(live_clean[k] == base_live[k] for k in ('claimed', 'replaced', 'kept_palette', 'kept_mismatch', 'kept_not_ours', 'kept_unjoined')),
            'live-style core (pins on, frames %s): replaced %d / refused counts equal the verify-all core\'s (replaced %d), 0 wrong' % (live_frames, live_clean['replaced'], base_live['replaced']))
    cor = cpu_live('', live_frames, '--corrupt-at', '160')
    verdict(cor['corrupt'] is not None and cor['corrupt'][0] == 1 and 0 < cor['corrupt'][1] - 160 <= 32 and cor['kept_mismatch'] > 0,
            'proof frames: a vertex blob inverted after frame 160 is refused (Mismatch %d draws) %s frames later (allowed 32)' % (cor['kept_mismatch'], (cor['corrupt'][1] - 160) if cor['corrupt'] else None))
    twin = cpu_live('no_proof_frames', live_frames, '--corrupt-at', '160')
    verdict(not (twin['corrupt'] is not None and twin['corrupt'][1] > 0 and twin['kept_mismatch'] > 0),
            'plant no_proof_frames: the corrupted blob goes undetected (the check "detected within 32 frames" would FAIL: Mismatch %d draws)' % twin['kept_mismatch'])

    # ---------------- PIC half ----------------
    def fview(name, faults, frame):
        d = os.path.join(out, 'arm_%s_f%d' % (name, frame))
        cmd = [fview_exe, '--capture', cap, '--frame', str(frame), '--judge-out', d, '--root', root]
        if faults is not None:
            cmd += ['--wrestlers-ours', '--game', game]
            if faults:
                cmd += ['--replace-fault', faults]
        for attempt in range(4):
            rc, text = run(cmd, 900)
            if rc and 'Not enough graphics memory for the frame ring' in text and attempt < 3:
                print('  (fview: the shared GPU was full, retry %d)' % (attempt + 1))
                continue
            break
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('fview arm %s failed (exit %d)' % (name, rc))
        m = re.search(r'\[rdr\]\[wrestlers\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)', text)
        c = None
        if m:
            c = dict(zip(['ours_frames', 'fallback', 'replaced', 'kept', 'not_ready', 'unjoined', 'mismatch', 'variant', 'texture', 'palette', 'not_ours'], map(int, m.groups())))
        return png, c, text

    def load(png):
        return np.asarray(Image.open(png).convert('RGB')).astype(int)

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        ys, xs = np.nonzero(d > 0)
        return len(ys), int(d.max()) if len(ys) else 0, (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())) if len(ys) else None

    shots = {}
    for frame in (200, 260):
        s_png, _, _ = fview('stream', None, frame)
        o_png, oc, otext = fview('ours', '', frame)
        shots[frame] = {'stream': s_png, 'ours': o_png}
        A, B = load(s_png), load(o_png)
        n, mx, _ = diff(A, B)
        verdict(oc is not None and n == 0 and md5(s_png) == md5(o_png) and oc['replaced'] > 1000,
                'frame %d: ours == stream picture (%d differing pixels, md5 %s), %d draws replaced in %d frames' % (frame, n, 'equal' if md5(s_png) == md5(o_png) else 'DIFFERENT', oc['replaced'] if oc else -1, oc['ours_frames'] if oc else -1))
        if frame == 200:
            print('  census: ' + re.search(r'\[rdr\]\[wrestlers\] ours frames.*', otext).group(0)[:400])
            rep = re.search(r'\[fview\] \[rdr\]\[wrestlers\] (wrestlers: .*)', otext)
            print('  hook:   ' + (rep.group(1)[:900] if rep else '(no report line)'))
            cm = re.search(r'vertex elements compared (\d+): exact (\d+) near (\d+) bad (\d+)', rep.group(1)) if rep else None
            verdict(cm is not None and int(cm.group(4)) == 0 and int(cm.group(1)) == int(cm.group(2)) + int(cm.group(3)) and int(cm.group(2)) > 0,
                    'vertex elements compared with the stream bytes: %s total, %s bit-exact, %s within tolerance (position ulps / one normal step), %s bad' % (cm.groups() if cm else ('?', '?', '?', '?')) if cm else 'vertex-compare counts missing from the hook report')
            verdict(oc['fallback'] <= 3 and oc['palette'] > 0 and oc['not_ours'] > 0 and oc['mismatch'] == 0 and oc['variant'] == 0,
                    'frame 200 census: fallback frames %d (the first frames have no record), Brock refused %d (palette), %d draws of other classes passed on, no mismatch / variant' % (oc['fallback'], oc['palette'], oc['not_ours']))
            verdict(rep is not None and 'verify %d bad 0' % int(re.search(r'verify (\d+) bad', rep.group(1)).group(1)) in rep.group(1) and 'refused: counts 0 layout 0' in rep.group(1), 'hook report: every replaced draw verified against the stream bytes, 0 bad')
            verdict(re.search(r'replaced (\d+), kept (\d+)', otext) is not None and (int(re.search(r'\[rdr\]\[wrestlers\] ours frames \d+, fallback \d+, replaced (\d+)', otext).group(1)) == int(re.search(r'claimed (\d+) replaced (\d+)', rep.group(1)).group(2))),
                    "the binder's replaced count equals the hook's own count")
    base_png_cache = []

    def checks_off_baseline():   # the picture with every check off and nothing planted (Brock's palette draws are already wrong in it)
        if not base_png_cache:
            base_png_cache.append(fview('checks_off_only', 'checks_off', 200)[0])
        return base_png_cache[0]

    for plant in PLANTS:
        png_on, c_on, _ = fview(plant, plant, 200)
        A, B = load(shots[200]['stream']), load(png_on)
        n, mx, _ = diff(A, B)
        verdict(c_on is not None and n == 0 and c_on[REFUSED_BY[plant]] > 0, 'plant %-18s checks ON : picture == stream (%d differing pixels), refused as %s (%d), replaced %d' % (plant, n, REFUSED_BY[plant], c_on[REFUSED_BY[plant]], c_on['replaced']))
        png_off, c_off, _ = fview(plant + '_off', plant + ',checks_off', 200)
        C = load(png_off)
        n2, mx2, box = diff(A, C)
        if plant == 'uv_swap':
            # the swap alone: against the same arm without it (every check off, nothing planted), so the Brock palette and the forced links are in both
            base_png = checks_off_baseline()
            nuv, mxuv, boxuv = diff(load(base_png), C)
            verdict(c_off['replaced'] > 1000 and nuv > 100, 'plant %-18s checks OFF: the picture breaks (%d pixels differ from the same arm without the swap, max %d, region %s; %d draws drawn)' % (plant, nuv, mxuv, boxuv, c_off['replaced']))
        elif plant in PIECE_PLANTS:   # against the checks_off-only picture, so Brock's refused palette does not count
            nbp, mxbp, boxbp = diff(load(checks_off_baseline()), C)
            verdict(nbp > 100, 'plant %-18s checks OFF: the picture breaks (%d pixels differ from the checks_off-only arm, max %d, region %s)' % (plant, nbp, mxbp, boxbp))
        else:
            verdict(n2 > 100, 'plant %-18s checks OFF: the picture breaks (%d pixels differ, max %d, region %s)' % (plant, n2, mx2, box))
        shots[200][plant + '_off'] = png_off
    shots[200]['accept_record_palette'] = fview('accept_record_palette', 'accept_record_palette', 200)[0]
    n_acc, mx_acc, box_acc = diff(load(shots[200]['stream']), load(shots[200]['accept_record_palette']))
    verdict(n_acc > 100, "plant accept_record_palette (today's update-hook palette for Brock accepted): the picture breaks (%d pixels differ, max %d, region %s)" % (n_acc, mx_acc, box_acc))
    acc260 = fview('accept_record_palette', 'accept_record_palette', 260)[0]
    shots[260]['accept_record_palette'] = acc260

    # ---------------- the sheet ----------------
    sheet_dir = args.sheet_dir or os.path.join(main_co, 'out', 's101', 'wrestlers_ours')
    os.makedirs(sheet_dir, exist_ok=True)
    try:
        from PIL import ImageDraw
        for frame, box in ((200, (240, 340, 660, 720)), (260, (290, 340, 640, 680))):
            tiles = [('STREAM (the game\'s own draws)', shots[frame]['stream']), ('OURS (replaced in place)', shots[frame]['ours']), ("Brock's update-hook palette ACCEPTED (what the check refuses)", shots[frame]['accept_record_palette'])]
            if frame == 200:
                tiles.append(('bind pose, checks off (plant)', shots[200]['bind_pose_off']))
            scale = 2
            crops = []
            for label, png in tiles:
                im = Image.open(png).convert('RGB').crop(box)
                im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
                strip = Image.new('RGB', (im.width, im.height + 26), (20, 20, 20))
                strip.paste(im, (0, 26))
                ImageDraw.Draw(strip).text((6, 6), '%s  [frame %d, x%d, crop %s]' % (label, frame, scale, box), fill=(255, 255, 255))
                crops.append(strip)
            cols = 2
            rows = (len(crops) + 1) // 2
            W, H = crops[0].width, crops[0].height
            sheet = Image.new('RGB', (W * cols, H * rows), (0, 0, 0))
            for i, c in enumerate(crops):
                sheet.paste(c, ((i % cols) * W, (i // cols) * H))
            path = os.path.join(sheet_dir, 'sheet_frame%d.png' % frame)
            sheet.save(path)
            print('SHEET ' + path)
    except Exception as e:
        verdict(False, 'sheet could not be made: %s' % e)

    print('\n%s: %d check(s) failed' % ('FAIL' if fails else 'PASS', fails) if fails else '\nPASS: all wrestlers_ours checks hold')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

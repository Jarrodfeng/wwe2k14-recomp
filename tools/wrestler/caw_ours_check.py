#!/usr/bin/env python3
"""Lane 153 (N3) acceptance test, offline, 0 game runs: the Create a Superstar editor's wrestler replaced in place (src\\renderer_scene\\caw_core.h; docs\\renderer\\CAW_OURS.md), on the
caw_walk case zip (out\\s101\\runs\\20261112_122353_s101_checkB_caw_walk, no scene records).

  py -I tools\\wrestler\\caw_ours_check.py [--root DIR] [--no-build] [--quick]

out\\tools\\caw_ours_check.exe drives the SHIPPED decision code (CawCore) over the capture's frames; every replaced draw is proven independently against the stream's vertex / index bytes.
  clean     300 frames: nothing wrong; 10 of the body's 12 parts are replaced on every frame (the two the editor's morph moves beyond the class tolerance stay the stream's, named).
  MORPH     from frame 101 on one replaced part's vertex array has a NEW OFFSET with vertex 0 moved by 0.5 (geometry is content-addressed: a changed array never keeps its offset): the core must refuse that
            part AT frame 101 and the independent proof must see 0 wrong draws. 4e-4 is accepted, 6e-4 refused.
  MATCH     fview on the 7083 match capture: ring / props / wrestlers fates are EQUAL with and without caw, and caw answers NotOurs for every draw; the count_only plant is caught.
  plants    pinned (a pin that ignores a new offset: the PAST BUG, a stale model after an editor change): the proof sees stale draws and the part is never refused -> the MORPH check FAILS;
            no_verify: the proof sees wrong draws (positive control of the proof itself); wrong_leaf: the first count-fitting leaf unconfirmed -> the proof sees wrong draws.
A missing input FAILS (never skips)."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RUN = '20261112_122353_s101_checkB_caw_walk'


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


def kv(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}


def match_arm(root, main_co, game, problems):
    """fview (headless: --ops --json) on the 7083 match capture, frame 200, the other classes on: ring / props / wrestlers fates must be EQUAL with and without caw, and caw must
    answer NotOurs for everything; the count_only plant (a count-only answer) takes draws away and is caught."""
    import json
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261112_131008_s100_owner_match')
    zs = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fv = _fe.find_or_exit(root=root, kind='release')
    if not zs or not os.path.exists(fv):
        problems.append('match arm: the match case zip or the release fview is missing (build tools/fview/build.ps1 -Release)')
        return False
    out_dir = os.path.join(main_co, 'out', 's101', 'caw_ours')
    os.makedirs(out_dir, exist_ok=True)

    def fate_counts(name, extra):
        js = os.path.join(out_dir, name + '.json')
        rc, text = run([fv, '--case', zs[0], '--root', root, '--game', game, '--frame', '200', '--ops', '--wrestlers-ours', '--ring-ours', '--props-ours'] + extra + ['--json', js], 1500)
        if rc != 0 or not os.path.exists(js):
            problems.append(f'match arm {name}: fview failed (rc {rc}): {text[-300:]}')
            return None, text
        d = json.load(open(js))
        c = {}
        for o in d['ops']:
            if o['kind'] == 'draw':
                c[o['fate']] = c.get(o['fate'], 0) + 1
        return c, text

    base, _ = fate_counts('match_base', [])
    withc, text = fate_counts('match_caw', ['--caw-ours'])
    plant, ptext = fate_counts('match_caw_countonly', ['--caw-ours', '--replace-fault', 'caw.count_only'])
    if base is None or withc is None or plant is None:
        return False
    pat = r'caw: claimed (\d+) replaced (\d+) \| not caw (\d+) loading (\d+) unjoined (\d+) refused layout (\d+) bytes (\d+) other (\d+)'
    m = re.search(pat, text)
    print(f'ARM match without caw: {base}')
    print(f'ARM match with caw:    {withc}  {m.group(0) if m else "(no caw report)"}')
    print(f'ARM match with caw count_only plant: {plant}')
    if base != {k: v for k, v in withc.items() if k != 'caw_ours'} or withc.get('caw_ours', 0):
        problems.append(f'match arm: the fates changed with caw on (ring / props / wrestlers must keep their draws): {base} vs {withc}')
    if not m or any(int(m.group(i)) for i in (2, 5, 6, 7, 8)):
        problems.append(f'match arm: caw must answer NotOurs only on a match capture: {m.group(0) if m else text[-300:]}')
    pm = re.search(pat, ptext)
    if plant == base and not (pm and int(pm.group(5)) > 0):
        problems.append('plant count_only (a count-only answer takes draws away from ring / props) was NOT caught: the fates and the unjoined count did not move')
    return True


def backdrop_arms(root, main_co, game, capdir, exe, frames_n, quick, problems):
    """Lane 169 (N3b): the editor's backdrop (pac\\bg\\bg66.pac: CAS_wall = yBG_Default, CAS_moya = the yAudience / yBG_UVScroll card) drawn by us in place.
      source   caw_ours_check: both objects join bg66.pac (pach 0) entries 0 / 3, replaced on every frame, the independent proof sees 0 wrong draws
      picture  fview --caw-ours vs the stream, headless, on sampled frames: the differing pixels are the body's tolerance (a few hundred, max 1 on the backdrop's own area)
      census   the stream draws at the colour site: 4 draws / 2,601 prims (lane 153) -> 2 draws / 1,718 prims (lane 169) -> 0 draws (lane 179: parts 4 and 9 joined)
      plants   bg_wrong_source (+ no_verify for the independent proof), frozen_scroll (Time constant frozen), wrong_picture (the other piece's slot 0): each caught"""
    import json
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        problems.append('backdrop arms: numpy / Pillow are needed: %s' % e)
        return
    # ---- source and independent proof (every frame) ----
    def chk(name, extra, step_='1'):
        rc, out = run([os.path.join(root, 'out', 'tools', 'caw_ours_check.exe'), '--capture', capdir, '--game', game, '--step', step_] + extra)
        res = kv(next((l for l in out.splitlines() if l.startswith('RESULT')), ''))
        bd = kv(next((l for l in out.splitlines() if l.startswith('BACKDROP')), ''))
        src = {l.split()[1]: l.split(None, 2)[2] for l in out.splitlines() if l.startswith('BGSRC')}
        if rc != 0 or not res:
            problems.append(f'backdrop {name}: the checker failed (rc {rc}): {out[-300:]}')
        print(f'ARM backdrop {name}: backdrop replaced {bd.get("replaced")}, sources {sorted(src.values())}, wrong vertex {res.get("wrong_vertex")} index {res.get("wrong_index")} count {res.get("wrong_count")}')
        return res, bd, src
    res, bd, src = chk('clean', [])
    if res:
        if bd.get('replaced') != 2 * frames_n:
            problems.append(f"backdrop clean: both pieces must be replaced on every frame ({bd.get('replaced')} of {2 * frames_n})")
        if sorted(src.values()) != ['pac=1066 pach=0 entry=0', 'pac=1066 pach=0 entry=3']:
            problems.append(f'backdrop clean: the join must be bg66.pac entry 0 (CAS_wall) and entry 3 (CAS_moya): {src}')
        if res['wrong_vertex'] or res['wrong_index'] or res['wrong_count']:
            problems.append(f'backdrop clean: the independent proof saw wrong draws: {res}')
    res, bd, src = chk('plant bg_wrong_source (the hook alone: the index check refuses it)', ['--fault', 'bg_wrong_source'], '10')
    if res and bd.get('replaced', 0) != 0:
        problems.append(f'plant bg_wrong_source was NOT caught (a wrong source entry still drew): {bd}')
    res, bd, src = chk('plant bg_wrong_source,no_verify (the independent proof alone)', ['--fault', 'bg_wrong_source,no_verify'], '10')
    if res and not (res['wrong_vertex'] or res['wrong_index'] or res['wrong_count']):
        problems.append(f'plant bg_wrong_source,no_verify: the independent proof did not see the wrong source: {res}')
    # ---- pictures and the census (fview, headless) ----
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218
    try:
        fv = _fe.find_fview(root=root, kind='release')
    except _fe.FviewError as e:
        problems.append('backdrop picture arms: ' + str(e))
        return
    work = os.path.join(root, 'out', 'l169')
    os.makedirs(work, exist_ok=True)
    cache = {}

    def pic(tag, frame, extra):
        if (tag, frame) in cache:
            return cache[(tag, frame)]
        d = os.path.join(work, '%s_%d' % (tag, frame))
        rc, out = run([fv, '--capture', capdir, '--frame', str(frame), '--root', root, '--judge-out', d] + extra, 1200)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            raise RuntimeError('fview %s frame %d failed (rc %d): %s' % (tag, frame, rc, out[-300:]))
        cache[(tag, frame)] = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
        return cache[(tag, frame)]

    on = ['--caw-ours', '--game', game]
    frames = [100, 200] if quick else [0, 50, 100, 150, 200, 250, 299]
    for f in frames:
        s, o = pic('s', f, []), pic('c', f, on)
        d = np.abs(o - s).max(2)
        n, big = int((d > 0).sum()), int((d > 8).sum())
        ok = n <= 460 and big <= 16   # <= 0.05% of the 1280x720 picture differs, and almost none by more than 8/255: the body's own tolerance (CAW_OURS.md); the backdrop is byte-equal
        ys, xs = np.nonzero(d > 0)
        print(('PASS' if ok else 'FAIL') + f'  backdrop picture frame {f}: differing px {n}, over 8/255 {big}' + (f', inside x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()} (the wrestler)' if n else ''))
        if not ok:
            problems.append(f'backdrop picture frame {f}: {n} px differ ({big} over 8)')
    for plant, label in (('frozen_scroll', 'a frozen scroll (the Time constant stuck at the first tick)'), ('wrong_picture', 'a wrong picture (the other piece\'s texture)')):
        for f in [100, 200]:
            s, o = pic('s', f, []), pic(plant, f, on + ['--replace-fault', 'caw.' + plant])
            n = int((np.abs(o - s).max(2) > 8).sum())
            print(('PASS' if n > 20000 else 'FAIL') + f'  plant {plant} frame {f}: {n} px move by more than 8/255 (must be caught: > 20000)')
            if n <= 20000:
                problems.append(f'plant {plant} ({label}) was NOT caught on frame {f}: {n} px moved')
    # census: stream draws at the colour site, frame 150, with and without the class
    def site_stream(tag, extra):
        js = os.path.join(work, 'ops_%s.json' % tag)
        rc, out = run([fv, '--capture', capdir, '--frame', '150', '--root', root, '--ops', '--json', js] + extra, 1200)
        if rc or not os.path.isfile(js):
            raise RuntimeError('ops %s failed: %s' % (tag, out[-300:]))
        ops = json.load(open(js, encoding='utf-8'))['ops']
        dr = [o for o in ops if o.get('kind') == 'draw' and (o.get('pass_site', 0) & 0xFFFFFFFF) == 0x82C0A950 and o.get('fate') == 'stream']
        ours = [o for o in ops if o.get('kind') == 'draw' and o.get('fate') == 'caw_ours']
        return len(dr), sum(o.get('prims', 0) for o in dr), len(ours), sorted(set(o.get('effect') for o in ours))
    a, b = site_stream('off', []), site_stream('on', on)
    print(f'ARM census frame 150, stream draws at 0x82C0A950: off {a[0]} draws / {a[1]} prims -> caw on {b[0]} draws / {b[1]} prims; caw_ours draws {b[2]} ({b[3]})')
    if not (b[0] == 0 and b[1] == 0 and b[2] == 15 and a[0] > b[0]):  # lane 179: 0 stream draws of the editor's body (the two backdrop pieces and all 13 body draws are ours)
        problems.append(f'census frame 150: expected 0 stream draws at the colour site and 15 caw_ours draws, got {b}; off {a}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', RUN)
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        print('FAIL  input missing: the caw_walk case zip under ' + runs)
        return 1
    if not os.path.isdir(os.path.join(game, 'pac', 'edit')):
        print('FAIL  input missing: game_data\\extracted\\pac\\edit under ' + main_co)
        return 1
    exe = os.path.join(root, 'out', 'tools', 'caw_ours_check.exe')
    if not args.no_build or not os.path.exists(exe):
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'wrestler', 'build_caw_ours_check.ps1')], 1800)
        if rc != 0 or not os.path.exists(exe):
            print('FAIL  build\n' + out[-1500:])
            return 1
    cap = os.path.join(main_co, 'out', 's101', 'caw_ours', 'cap')
    if not os.path.isdir(os.path.join(cap, 'capture', 'frames')):
        os.makedirs(cap, exist_ok=True)
        with zipfile.ZipFile(zips[0]) as z:
            z.extractall(cap)
    capdir = os.path.join(cap, 'capture')
    step = '3' if args.quick else '1'
    problems = []

    def arm(name, extra, step_=step):
        rc, out = run([exe, '--capture', capdir, '--game', game, '--step', step_] + extra)
        res = next((l for l in out.splitlines() if l.startswith('RESULT')), '')
        mor = next((l for l in out.splitlines() if l.startswith('MORPH')), '')
        if rc != 0 or not res:
            problems.append(f'{name}: the checker failed (rc {rc}): {out[-400:]}')
            return {}, {}
        r, m = kv(res), kv(mor) if mor else {}
        ih = re.search(r'identity hits (\d+)', out)
        r['identity_hits'] = int(ih.group(1)) if ih else 0
        print(f'ARM {name}: {res} {mor}')
        return r, m

    r, _ = arm('clean', [])
    frames = 100 if args.quick else 300
    if r:
        if r['wrong_vertex'] or r['wrong_index'] or r['wrong_count']:
            problems.append(f'clean: the independent proof saw wrong draws: {r}')
        if r['replaced'] != 15 * frames:   # lane 179: all 15 colour draws of the editor (12 body parts + the 2 backdrop pieces + part 11's second draw); nothing is left to the stream
            problems.append(f"clean: all 15 draws a frame must be replaced ({r['replaced']} over {frames} frames, expected {15 * frames})")
        if r['kept_mismatch'] or r['kept_unjoined'] or r['kept_not_ours']:
            problems.append(f"clean: parts never confirmed must answer NotOurs (review B1), got unjoined {r['kept_unjoined']} mismatch {r['kept_mismatch']}")
        if r['identity_hits'] < r['replaced'] * 0.9:
            problems.append(f"clean: the identity fast path was not taken ({r['identity_hits']} hits for {r['replaced']} replaced draws)")
    # the slider move: the part's vertex array has a NEW offset (content-addressed geometry) with moved bytes
    r, m = arm('morph new offset', ['--morph-at', '100'], '1')
    if r and (m.get('set') != 1 or m.get('refused_frame') != 101 or m.get('wrong_after', 1) != 0 or r['wrong_vertex']):
        problems.append(f'morph: the stale part was not refused at frame 101 / the proof saw wrong draws: {m} {r}')
    # the tolerance boundary (review plant d): 4e-4 of max(1, |x|) is the editor's own noise (accepted), 6e-4 is a slider (refused)
    r, m = arm('tolerance 4e-4 accepted', ['--morph-at', '100', '--morph-rel', '0.0004'], '1')
    if r and (m.get('refused_frame') != -1 or r['wrong_vertex'] or r['replaced'] < 15 * 300 * 0.99):
        problems.append(f'tolerance 4e-4 should be accepted: {m} {r}')
    r, m = arm('tolerance 6e-4 refused', ['--morph-at', '100', '--morph-rel', '0.0006'], '1')
    if r and (m.get('refused_frame') != 101 or m.get('wrong_after', 1) != 0):
        problems.append(f'tolerance 6e-4 should be refused at frame 101: {m} {r}')
    # plants
    r, m = arm('plant pinned (a pin that ignores a new offset: the past stale-model bug)', ['--morph-at', '100', '--fault', 'pinned'], '1')
    if r and not (r['wrong_vertex'] > 0 and m.get('refused_frame') != 101):
        problems.append(f'plant pinned (the stale model past bug) was NOT caught: {m} {r}')
    r, m = arm('plant no_verify', ['--morph-at', '100', '--fault', 'no_verify'], '1')
    if r and not r['wrong_vertex'] > 0:
        problems.append(f'plant no_verify: the independent proof did not see it: {r}')
    r, _ = arm('plant wrong_leaf,no_verify (positive control; the hook alone refuses it at the index check)', ['--fault', 'wrong_leaf,no_verify'])
    if r and not (r['wrong_vertex'] > 0 or r['wrong_count'] > 0 or r['wrong_index'] > 0):
        problems.append(f'plant wrong_leaf: the independent proof did not see it: {r}')
    # lane 179 (N3 parts 4 and 9): the sources the two parts join, and the plants of the two causes
    rc, out = run([exe, '--capture', capdir, '--game', game, '--step', '10', '--census'], 900)
    psrc = {l.split()[1]: l.split(None, 2)[2] for l in out.splitlines() if l.startswith('PARTSRC')}
    print('ARM lane 179 sources: part 4 ->', psrc.get('AAB142F0:4'), '| part 9 ->', psrc.get('AAB142F0:9'))
    if psrc.get('AAB142F0:4') != 'pac=2 pach=0 leaf=2002 mesh=1' or psrc.get('AAB142F0:9') != 'pac=9 pach=0 leaf=2007 mesh=0':
        problems.append(f"lane 179: part 4 must join ed02 leaf 2002 mesh 1 (the draw count 1364 = 685 + 678 indices + 1 restart marker, not the ed22 mesh with 1364 indices) and part 9 ed09 leaf 2007 mesh 0: {psrc}")
    r, _ = arm('plant flat_indices (the count-only fit ignores the restart marker: part 4 loses its leaf)', ['--fault', 'flat_indices'], '10')
    if r and not (r['replaced'] < 15 * 30 and r['wrong_vertex'] == 0):
        problems.append(f'plant flat_indices (part 4 joined the wrong leaf) was NOT caught by the byte check: {r}')
    r, _ = arm('plant flat_indices,wrong_leaf,no_verify (positive control for the independent proof: the wrong ed22 leaf, nothing verified)', ['--fault', 'flat_indices,wrong_leaf,no_verify'], '10')
    if r and not (r['wrong_vertex'] > 0 or r['wrong_index'] > 0 or r['wrong_count'] > 0):
        problems.append(f'plant flat_indices,wrong_leaf,no_verify: the independent proof did not see the wrong leaf: {r}')
    # lane 179 review: positive controls for the proof's vertex-content and index-content paths (the right leaf, counts fit, bytes wrong; nothing verified)
    r, _ = arm('plant scale_one,no_verify (the right leaf built at scale 1: counts fit, vertex bytes wrong)', ['--fault', 'scale_one,no_verify'], '10')
    if r and not (r['wrong_vertex'] > 0 and r['wrong_count'] == 0):
        problems.append(f'plant scale_one,no_verify: the independent proof must see wrong VERTEX bytes with the counts right: {r}')
    r, _ = arm('plant index_swap,no_verify (the right leaf with two indices swapped: counts fit, strips wrong)', ['--fault', 'index_swap,no_verify'], '10')
    if r and not (r['wrong_index'] > 0 and r['wrong_count'] == 0):
        problems.append(f'plant index_swap,no_verify: the independent proof must see wrong INDEX bytes with the counts right: {r}')
    r, _ = arm('plant fill0_strict (fill0 compared bit-exactly: part 9 loses its leaf)', ['--fault', 'fill0_strict'], '10')
    if r and not (r['replaced'] < 15 * 30 and r['wrong_vertex'] == 0):
        problems.append(f'plant fill0_strict (part 9 should stay the streams) was NOT caught: {r}')
    # lane 179 review: the wrestlers' class has the same restart-marker rule (wrestler_geom.h DrawIndexCount / BuildIndices); the unit check scans every pac\\ch
    mg = os.path.join(root, 'out', 'tools', 'multigroup_check.exe')
    if not os.path.exists(mg):
        run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'wrestler', 'build_multigroup_check.ps1')], 900)
    rc, out = run([mg, '--game', game], 1500)
    mres = kv(next((l for l in out.splitlines() if l.startswith('RESULT')), ''))
    print('ARM multigroup_check (every pac\\ch):', next((l for l in out.splitlines() if l.startswith('RESULT')), out[-200:]))
    if rc != 0 or mres.get('bad', 1) != 0 or mres.get('multi_group') != 2 or mres.get('plant_differs') != 2:
        problems.append(f'multigroup_check: the restart-marker rule failed or the multi-group census moved (expected 2 multi-group parts, ch103 entry 10500): {mres} {out[-300:]}')
    # plant f: a part index above 255 must not collide with a low one (the key keeps the whole index)
    core_h = open(os.path.join(root, 'src', 'renderer_scene', 'caw_core.h'), encoding='utf-8').read()
    good = 'uint64_t(object) << 32 | part' in core_h and 'static_assert(CawKey(1, 300) != CawKey(1, 44)' in core_h
    bad = core_h.replace('uint64_t(object) << 32 | part', 'uint64_t(object) << 8 | (part & 0xFF)')
    planted_caught = 'uint64_t(object) << 32 | part' not in bad
    print(f'ARM part key > 255: the key keeps the whole part index and a static_assert says so: {good}; the truncating plant is detected: {planted_caught}')
    if not good or not planted_caught:
        problems.append('plant f: the part key is not the whole index (or its static_assert is gone)')
    # lane 169 (N3b): the editor's backdrop pieces (bg66.pac)
    if not args.no_build:
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1'), '-Release'], 2400)
        if rc != 0:
            problems.append('fview build failed: ' + out[-400:])
    backdrop_arms(root, main_co, game, capdir, exe, frames, args.quick, problems)
    # a CAW-shaped draw must not take anything from the other classes in a MATCH (review B1, plants a and b)
    match_arm(root, main_co, game, problems)
    if problems:
        print('FAIL')
        for p in problems:
            print('  ' + p)
        return 1
    print('PASS  caw_ours: clean proof, the slider move refused at the next frame, 3 plants caught')
    return 0


if __name__ == '__main__':
    sys.exit(main())

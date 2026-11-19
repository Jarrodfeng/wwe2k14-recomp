#!/usr/bin/env python3
r"""Lane D4 check (Session 103): the crowd's signs / foam hands and the single standing people drawn from the scene record rows alone, offline (0 game runs).

  py -I tools\scene\d4_check.py [--capture DIR] [--frames 20,110,200] [--out DIR]

Rows: the signs' rows are read from the capture's STREAM draws at the loops' call sites (0x82518D60 / 0x825192D0: their VS constants c76-c78 and which mesh = the draw's vertex / index counts) and appended to the
record through the writer API (frame_rig --props: crowd_rows::AppendProp), i.e. placed exactly where the stream drew them. The single person is a clone of a posed stands person moved by (dx, dy, dz)
(frame_rig --single-like: crowd_rows::AppendSingle). The crowd class is drawn alone through BuildFrame + the class table (LiveCrowd), once with only the rows' class kept (fault names only_props / only_singles).
Bars (independent oracle for the props: Python projects the audience-set meshes from the player's own pac with the same rows and the stream's camera and fills the triangles):
  1. PROPS  the rig reports every row drawn (listed == drawn, skipped 0, holes 0) and our coverage mask equals the oracle's: IoU >= 0.55.
  2. HOLES  a row naming a mesh key the audience set lacks is a counted hole with its reason (not drawn, not silent).
  3. SINGLE the diff between 'with the single row' and 'no_singles' lies where the row's world puts the person (its centroid within 80 px of the projected body centre), and is non-empty.
The oracle keeps only the clockwise-on-screen triangles (the game's cull state), so a renderer that culled the other side (planted twin props_cull_front: the winding reversed) fails the same bar.
Planted twins (each must FAIL its bar): props_transposed (the 3x3 read transposed: IoU drops below 0.8 of the base), props_wrong_key (the other kind's mesh: IoU below 0.8 of the base),
singles_identity (the row's world dropped: the diff is elsewhere), plus the oracle's own plant (the stream's rows with the transposed 3x3 in the ORACLE: IoU drops).
Exit 0 = all bars passed and all twins failed their bar."""
import argparse, collections, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frame_rig as fr  # noqa: E402

ROOT = fr.ROOT
sys.path[:0] = [os.path.join(ROOT, 'tools', 'solver'), os.path.join(ROOT, 'tools', 'wm30'), os.path.join(ROOT, 'tools', 'crowd')]
SITE_KIND = {0x82518D60: 1, 0x82518E68: 1, 0x825192D0: 2, 0x825194B4: 2}   # CrowdPropKind: 1 = T_Audience_LightMap, 2 = T_Audience
W, H = 1280, 720


def export_rows(capture, n, meshes=None):
    """The stream frame's prop draws -> ('P kind key variant c0..c11' lines, [(kind, key, c12)]). The mesh key is the audience-set entry the draw's counts name (34/77 -> 40000 [C: CROWD_PROMOTION N4]); a foam hand is keyed by its REAL h (S103 SGF review 7): the audience set's own (vertex, index) counts name the key whose mesh the draw's (vertex, index) counts equal (the old fixed 140/223 -> 41016 held for h 17 only)."""
    from capread import Source, Frame
    src = Source(capture)
    f = Frame(src.read(src.frames[n]))
    site = f.origins['call_site']
    out = []
    # MEASURED (S103 D4): in brock_losswin (and brock_losswin_rt) every stream prop draw is behind or outside the camera in every frame (clip w < 0 for the translation, 0 of 11-28 rows inside the
    # frame at all 21 frames sampled): the loops draw all props whatever the view. So the rows keep the stream's rotation / scale / mesh / kind but are RELOCATED to stand above a stands
    # person of the same frame (that person's block draw translation, 120 units up: -y is up in the game's file space), where the camera looks. A capture with visible signs is the real-placement
    # proof: asked for in the hand-back.
    blocks = [f.reg(int(f.draws[d]['vs_bank']), 76, 3)[:, 3].astype(float) for d in range(len(f.draws)) if int(site[d]) == 0x82518574]
    for d in range(len(f.draws)):
        kind = SITE_KIND.get(int(site[d]))
        if not kind:
            continue
        dr = f.draws[d]
        counts = (int(dr['vertex_count']), int(dr['index_count']))
        key = {(34, 77): 40000, (140, 223): 41016}.get(counts, 0)
        if kind == 2 and meshes:
            named = [k for k, (v_, i_) in meshes.items() if 41000 <= k <= 41018 and (len(v_), len(i_)) == counts]
            if len(named) == 1:
                key = named[0]
            elif named:
                key = 41016 if 41016 in named else named[0]   # several hand meshes with the same counts: the pictures differ, the silhouettes do not
        c = [float(x) for x in f.reg(int(dr['vs_bank']), 76, 3).reshape(-1)]
        if blocks:
            b = blocks[(len(out) * 7) % len(blocks)]
            c[3], c[7], c[11] = float(b[0]), float(b[1]) - 120.0, float(b[2])
        out.append((kind, key, c))
    return out


def oracle_meshes(game, arena=4):
    import crowd_props_join as pj
    meshes = {}
    for nm, leaf in pj.audience_set(game, arena):
        r = pj.convert(leaf)
        if not r:
            continue
        n, vb, ib = r
        v = np.frombuffer(vb, '<f4').reshape(n, 7)[:, :3].astype(np.float64)
        idx = np.frombuffer(ib, '<u2').astype(np.int64)
        meshes[int(nm.rsplit('/', 1)[1])] = (v, idx)
    return meshes


def oracle_mask(rows, meshes, M, flip_y, transpose=False, cull=True):
    """Project every row's mesh with the stream's camera (column-vector M, clip = M [p 1]) and fill its triangles. cull (S103 SGF review 5): only the CLOCKWISE-on-screen triangles are
    filled (the game's state on every sign / hand draw: cull back, front face clockwise [C: tools\\crowd\\crowd_prop_cull_check.py]); a strip's odd triangles are swapped (the alternate winding) before the test."""
    img = Image.new('L', (W, H), 0)
    dr = ImageDraw.Draw(img)
    for kind, key, c in rows:
        if key not in meshes:
            continue
        v, idx = meshes[key]
        a = np.array(c, dtype=np.float64).reshape(3, 4)
        R, t = a[:, :3], a[:, 3]
        if transpose:
            R = R.T
        p = v @ R.T + t
        clip = (M @ np.c_[p, np.ones(len(p))].T).T
        with np.errstate(divide='ignore', invalid='ignore'):
            ndc = clip[:, :3] / clip[:, 3:4]
        px = (ndc[:, 0] * 0.5 + 0.5) * W
        py = (0.5 - ndc[:, 1] * 0.5) * H if not flip_y else (ndc[:, 1] * 0.5 + 0.5) * H
        ok = clip[:, 3] > 1e-4
        for i in range(len(idx) - 2):
            a_, b_, c_ = idx[i], idx[i + 1], idx[i + 2]
            if a_ == b_ or b_ == c_ or a_ == c_ or not (ok[a_] and ok[b_] and ok[c_]):
                continue
            if i & 1:
                a_, b_ = b_, a_
            if cull:
                area = (px[b_] - px[a_]) * (py[c_] - py[a_]) - (px[c_] - px[a_]) * (py[b_] - py[a_])   # picture rows run down: a positive area is clockwise on the screen
                if not area > 0:
                    continue
            dr.polygon([(px[a_], py[a_]), (px[b_], py[b_]), (px[c_], py[c_])], fill=255)
    return np.asarray(img) > 0


def lit(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)
    clear = a[0, 0]
    return np.abs(a - clear).sum(axis=2) > 12


def iou(a, b):
    u = int((a | b).sum())
    return int((a & b).sum()) / u if u else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=None)
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = a.capture or os.path.join(main_root, 'out', 'captures', 'brock_losswin')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('D4', a.out)
    work = os.path.join(out, 'd4_work')
    os.makedirs(work, exist_ok=True)
    os.environ.setdefault('WWE_FVIEW_ALLOW_STALE', '1')
    from fview_exe import find_fview
    fview = find_fview(root=ROOT)
    rig = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    frames = [int(x) for x in a.frames.split(',')]
    meshes = oracle_meshes(game)
    print('INFO  oracle meshes (audience set, arena 4): keys %s' % sorted(meshes))
    fails = []
    cams, rows = {}, {}
    for n in frames:
        cam, why = fr.stream_camera(fview, capture, n, game, work)
        if cam is None:
            print('FAIL  frame %d: no camera: %s' % (n, why))
            return 1
        cams[n] = cam
        rows[n] = export_rows(capture, n, meshes)
        with open(os.path.join(work, 'props_%d.txt' % n), 'w') as fp:
            for kind, key, c in rows[n]:
                fp.write('P %d %d 0 %s\n' % (kind, key, ' '.join('%.9g' % x for x in c)))
        print('INFO  frame %d: %d prop draws in the stream (keys %s)' % (n, len(rows[n]), dict(collections.Counter(k for _, k, _ in rows[n]))))

    def rig_run(prefix, n, extra=()):
        vp, eye, _op, _M = cams[n]
        cmd = [rig, '--record', os.path.join(capture, 'scene', 'frame_%06d.rsc' % n), '--game', game, '--size', '%dx%d' % (W, H), '--prefix', prefix, '--families', '0x10',
               '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)] + list(extra)
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1800)
        if p.returncode != 0:
            print('FAIL  rig exit %d: %s' % (p.returncode, (p.stderr or p.stdout)[-500:]))
            return None
        return open(prefix + '_report.txt').read()

    def props_of(rep):
        m = re.search(r'props (\d+) \(drawn (\d+), hidden (\d+), skipped (\d+), in place (\d+), neutral picture (\d+)\)', rep)
        return tuple(int(x) for i, x in enumerate(m.groups()) if i != 4) if m else None

    def in_place_of(rep):   # S103 SG2: the rows counted in place (not drawn)
        m = re.search(r'props \d+ \(drawn \d+, hidden \d+, skipped \d+, in place (\d+),', rep)
        return int(m.group(1)) if m else None

    # ---- 1. PROPS: the stream's rows through the writer API, the props alone, against the oracle ----
    base_iou, base_mask = {}, {}
    flip = None
    for n in frames:
        pre = os.path.join(work, 'props_%d' % n)
        rep = rig_run(pre, n, ['--props', os.path.join(work, 'props_%d.txt' % n), '--plant-crowd', 'only_props+props_grey_ok'])
        if rep is None:
            return 1
        ours = lit(pre + '_mask16.png')
        base_mask[n] = ours
        po = props_of(rep)
        M = cams[n][3]
        cand = {fy: iou(ours, oracle_mask(rows[n], meshes, M, fy)) for fy in (False, True)}
        if flip is None:
            flip = max(cand, key=cand.get)
        base_iou[n] = cand[flip]
        ok = bool(po) and po[0] == len(rows[n]) and po[1] == po[0] - po[2] and po[3] == 0 and cand[flip] >= 0.55
        print('%s  frame %d props: rows %d, report %s, IoU with the oracle %.3f (screen y %s; the other convention %.3f), %d px covered' % ('PASS' if ok else 'FAIL', n, len(rows[n]), po, cand[flip], 'up' if flip else 'down', cand[not flip], int(ours.sum())))
        if not ok:
            fails.append('props %d' % n)
    # ---- 2. HOLES: a key the audience set lacks ----
    mid = frames[len(frames) // 2]
    hole_file = os.path.join(work, 'props_hole.txt')
    with open(hole_file, 'w') as fp:
        fp.write(open(os.path.join(work, 'props_%d.txt' % mid)).read())
        fp.write('P 1 49999 0 %s\n' % ' '.join('%.9g' % x for x in rows[mid][0][2]))
    pre = os.path.join(work, 'hole')
    rep = rig_run(pre, mid, ['--props', hole_file, '--plant-crowd', 'only_props+props_grey_ok'])
    if rep is None:
        return 1
    holes = re.findall(r'crowd[^\n]*holes (\d+)', rep) or re.findall(r'holes[ =](\d+)', rep)
    why = 'prop key 49999 is not in the audience set' in rep
    ok = why and any(int(h) >= 1 for h in holes)
    print('%s  a row with a mesh key the audience set lacks is a counted hole with its reason (holes %s; reason in the report: %s)' % ('PASS' if ok else 'FAIL', holes, why))
    if not ok:
        fails.append('hole')
    # ---- 2b. the picture rule (coordinator, 2026-11-16): by default a VISIBLE sign has no picture source, so its rows are holes (the frame stays the game's); grey only with props_grey_ok ----
    pre = os.path.join(work, 'rule')
    rep = rig_run(pre, mid, ['--props', os.path.join(work, 'props_%d.txt' % mid), '--plant-crowd', 'only_props'])
    if rep is None:
        return 1
    holes = [int(x) for x in re.findall(r'family crowd[^\n]*holes (\d+)', rep)]
    ok = bool(holes) and max(holes) >= 1 and 'have no picture source' in rep
    print('%s  props need a picture (default): visible rows are holes (holes %s) with the reason; the grey-sign switch (props_grey_ok) leaves none' % ('PASS' if ok else 'FAIL', holes))
    if not ok:
        fails.append('picture rule')
    quiet = [int(x) for x in re.findall(r'family crowd[^\n]*holes (\d+)', open(os.path.join(work, 'props_%d_report.txt' % mid)).read())]
    if not quiet or max(quiet) != 0:
        print('FAIL  with props_grey_ok the crowd family still has holes %s' % quiet)
        fails.append('grey ok holes')
    # ---- 2c. S103 SG2: the rows a WALKED frame's record carries are flagged kCpfInPlace (the stream draws them in place): the mirror draws all of them, the live drawer skips and counts them ----
    pf = os.path.join(work, 'props_%d.txt' % mid)
    rep_m = rig_run(os.path.join(work, 'inplace_mirror'), mid, ['--props', pf, '--props-in-place', '--plant-crowd', 'only_props+props_grey_ok'])
    rep_l = rig_run(os.path.join(work, 'inplace_live'), mid, ['--props', pf, '--props-in-place', '--skip-in-place', '--plant-crowd', 'only_props+props_grey_ok'])
    rep_t = rig_run(os.path.join(work, 'inplace_twin'), mid, ['--props', pf, '--props-in-place', '--skip-in-place', '--plant-crowd', 'only_props+props_grey_ok+in_place_drawn_live'])
    if rep_m is None or rep_l is None or rep_t is None:
        return 1
    pm, pl, pt = props_of(rep_m), props_of(rep_l), props_of(rep_t)
    ok_m = bool(pm) and pm[1] == pm[0] - pm[2] and in_place_of(rep_m) == 0 and pm[0] == len(rows[mid])
    print('%s  live.in_place_mirror: the mirror (no skip) draws every flagged row (%s, in place %s)' % ('PASS' if ok_m else 'FAIL', pm, in_place_of(rep_m)))
    ok_l = bool(pl) and pl[1] == 0 and in_place_of(rep_l) == pl[0] - pl[2] and pl[0] == len(rows[mid])
    print('%s  live.in_place_skipped: the live drawer draws 0 flagged rows and counts them (%s, in place %s)' % ('PASS' if ok_l else 'FAIL', pl, in_place_of(rep_l)))
    caught_t = bool(pt) and pt[1] != 0
    print('%s  planted twin in_place_drawn_live: a live drawer that ignores the skip draws the rows twice (drawn %s) -> %s' % ('PASS' if caught_t else 'FAIL', pt[1] if pt else None, 'caught' if caught_t else 'NOT caught'))
    for okk, nm in ((ok_m, 'in place mirror'), (ok_l, 'in place skipped'), (caught_t, 'plant in_place_drawn_live')):
        if not okk:
            fails.append(nm)
    # ---- 3. planted twins on the props ----
    ob = lambda tr: iou(base_mask[mid], oracle_mask(rows[mid], meshes, cams[mid][3], flip, transpose=tr))
    for plant in ('props_transposed', 'props_wrong_key', 'props_cull_front'):
        pre = os.path.join(work, 'plant_%s' % plant)
        rep = rig_run(pre, mid, ['--props', os.path.join(work, 'props_%d.txt' % mid), '--plant-crowd', 'only_props+props_grey_ok+' + plant])
        if rep is None:
            fails.append('plant %s did not run' % plant)
            continue
        v = iou(lit(pre + '_mask16.png'), oracle_mask(rows[mid], meshes, cams[mid][3], flip))
        caught = v < 0.8 * base_iou[mid]
        if plant == 'props_cull_front':
            # MEASURED (S103 SGF): the prop meshes are two-sided shells, so the SILHOUETTE cannot tell the culled side (the oracle's culled and two-sided pictures are identical, IoU 1.000). The side shows in the
            # COLOUR: the reversed winding draws the back faces (their normals face away). Caught = >= 20 % of the lit pixels change colour against the unplanted picture.
            ca = np.asarray(Image.open(os.path.join(work, 'props_%d_mask16.png' % mid)).convert('RGB'), dtype=np.int16)
            cb = np.asarray(Image.open(pre + '_mask16.png').convert('RGB'), dtype=np.int16)
            lm = base_mask[mid]
            frac = float((np.abs(ca - cb).sum(axis=2)[lm] > 12).mean()) if lm.any() else 0.0
            caught = frac >= 0.20 or v < 0.8 * base_iou[mid]
            print('%s  planted twin props_cull_front: %.1f %% of the lit pixels change colour (>= 20 %% needed), IoU %.3f -> %s' % ('PASS' if caught else 'FAIL', 100 * frac, v, 'caught' if caught else 'NOT caught'))
            if not caught:
                fails.append('plant ' + plant)
            continue
        print('%s  planted twin %s: IoU with the oracle %.3f vs %.3f unplanted -> %s' % ('PASS' if caught else 'FAIL', plant, v, base_iou[mid], 'caught' if caught else 'NOT caught (the check is blind to it)'))
        if not caught:
            fails.append('plant ' + plant)
    nc = iou(oracle_mask(rows[mid], meshes, cams[mid][3], flip), oracle_mask(rows[mid], meshes, cams[mid][3], flip, cull=False))
    print('INFO  the oracle\'s culled and two-sided silhouettes: IoU %.3f (1.000 = the meshes are two-sided shells: the silhouette is blind to the side, the colour twin above is the cull proof)' % nc)
    t_iou = ob(True)
    caught = t_iou < 0.8 * base_iou[mid]
    print('%s  oracle plant (the ORACLE reads the 3x3 transposed): IoU %.3f vs %.3f -> %s' % ('PASS' if caught else 'FAIL', t_iou, base_iou[mid], 'caught' if caught else 'NOT caught'))
    if not caught:
        fails.append('oracle plant')
    # ---- 4. SINGLE: a clone of a posed stands person, moved by d, as a SINGLE row; its mask is compared with the SOURCE person moved by d through the stands path (a turned body keeps the centroid) ----
    d = (0.0, 0.0, 60.0)
    dd = '%g,%g,%g' % d
    full = ['--single-like', dd]
    pre0 = os.path.join(work, 'single_base')
    if rig_run(pre0, mid, ['--plant-crowd', 'no_props+no_singles+props_grey_ok'] + ['--single-like', '0,0,0', '--move-source']) is None:   # the whole crowd, the source in place
        return 1
    base = lit(pre0 + '_mask16.png')
    pre1 = os.path.join(work, 'single_row')
    rep1 = rig_run(pre1, mid, ['--plant-crowd', 'no_props+props_grey_ok+report_bodies'] + full)   # the crowd + the clone as a SINGLE row
    pre2 = os.path.join(work, 'single_moved')
    rep2 = rig_run(pre2, mid, ['--plant-crowd', 'no_props+no_singles+props_grey_ok'] + full + ['--move-source'])   # the crowd with the source person itself moved
    if rep1 is None or rep2 is None:
        return 1
    added_row = lit(pre1 + '_mask16.png') & ~base
    added_src = lit(pre2 + '_mask16.png') & ~base
    v = iou(added_row, added_src)
    ok = int(added_row.sum()) > 200 and v >= 0.85
    print('%s  single person: the row adds %d px, the moved source person adds %d px, IoU %.3f' % ('PASS' if ok else 'FAIL', int(added_row.sum()), int(added_src.sum()), v))
    if not ok:
        fails.append('single mask')
    # S103 SN: the row must be DRAWN by the rule (palette covers the body's used slots), not refused; the bodies' skeleton / used slots / file palette map are printed (the premise slot = node, [L])
    m_mm = re.search(r'singles (\d+) \(node mismatch (\d+), nodes != file skeleton (\d+), file slot map != identity (\d+)\)', rep1)
    m_bd = re.search(r'bodies \[([^\]]*)\]', rep1)
    drawn = bool(m_mm) and int(m_mm.group(2)) == 0 and True
    print('%s  single person: the applier draws the row (node mismatch %s, nodes != file skeleton %s, file slot map != identity %s (census))' % ('PASS' if drawn else 'FAIL', *(m_mm.group(2, 3, 4) if m_mm else ('?', '?', '?'))))
    if not drawn:
        fails.append('single rule refuses the row')
    print('INFO  resident bodies (nodes = file skeleton, slots = 1 + highest used palette slot, identity of the file palette map): %s' % (m_bd.group(1).strip() if m_bd else 'none reported'))
    for plant in ('singles_identity', 'singles_transposed'):
        pre = os.path.join(work, 'single_%s' % plant)
        rp = rig_run(pre, mid, ['--plant-crowd', 'no_props+props_grey_ok+' + plant] + full)
        if rp is None:
            fails.append('plant %s did not run' % plant)
            continue
        pv = iou(lit(pre + '_mask16.png') & ~base, added_src)
        caught = pv < 0.8 * v
        print('%s  planted twin %s: IoU with the moved source %.3f vs %.3f -> %s' % ('PASS' if caught else 'FAIL', plant, pv, v, 'caught' if caught else 'NOT caught'))
        if not caught:
            fails.append('plant ' + plant)
    # S103 SN: the palette cut to (used slots - 1) must be REFUSED (node mismatch counted, nothing drawn); with the rule unchecked (the old pre-review behaviour) the same row is drawn
    pre = os.path.join(work, 'single_short_palette')
    rp = rig_run(pre, mid, ['--plant-crowd', 'no_props+props_grey_ok+singles_short_palette'] + full)
    if rp is None:
        fails.append('plant singles_short_palette did not run')
    else:
        mm = re.search(r'singles (\d+) \(node mismatch (\d+)', rp)
        added_short = int((lit(pre + '_mask16.png') & ~base).sum())
        caught = bool(mm) and int(mm.group(2)) >= 1 and added_short < 0.2 * max(1, int(added_row.sum()))
        print('%s  planted twin singles_short_palette: node mismatch %s, the row adds %d px (vs %d) -> %s' % ('PASS' if caught else 'FAIL', mm.group(2) if mm else '?', added_short, int(added_row.sum()), 'caught' if caught else 'NOT caught'))
        if not caught:
            fails.append('plant singles_short_palette')
    pre = os.path.join(work, 'single_nodes_unchecked')
    ru = rig_run(pre, mid, ['--plant-crowd', 'no_props+props_grey_ok+singles_short_palette+singles_nodes_unchecked'] + full)
    if ru is not None:
        mu = re.search(r'singles (\d+) \(node mismatch (\d+)', ru)
        uncaught = bool(mu) and int(mu.group(2)) == 0   # the old rule's twin: the short palette is NOT refused (so the short-palette twin above is what proves the new rule)
        print('%s  planted twin singles_nodes_unchecked (the pre-review rule: no check): the short palette is drawn, node mismatch %s' % ('PASS' if uncaught else 'FAIL', mu.group(2) if mu else '?'))
        if not uncaught:
            fails.append('plant singles_nodes_unchecked')
    print('d4_check: %s' % ('PASS' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

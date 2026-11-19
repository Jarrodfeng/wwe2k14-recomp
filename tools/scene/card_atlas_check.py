#!/usr/bin/env python3
"""S103 CB1 acceptance check (offline, 0 game runs, 0 Surface runs): the crowd card bake in ONE pass (src\\renderer_scene\\card_atlas_rule.h, live_crowd.cpp RecordAtlasBake).

  py -I tools\\scene\\card_atlas_check.py [--capture brock_losswin] [--frames 110,200] [--out DIR]      the picture proof (needs out\\tools\\crowd_live_rig.exe: tools\\scene\\build_crowd_live.ps1)
  py -I tools\\scene\\card_atlas_check.py --unit                                                        the pure rule's unit (compiles tools\\scene\\card_atlas_unit.cpp, plants pitch / v must FAIL)

Per frame F the rig draws the crowd's cards (overview camera, cards only) and the whole crowd (the capture's own camera), validation layer on:
  ref    --card-atlas 0   one pass per type (the game's shape)
  one    --card-atlas 1   every picture in one pass into the atlas
  both   --card-atlas 3   + a picture whose inputs equal its last bake's is kept
Bars (every one must hold, else exit 1):
  tiles       every card picture the atlas baked equals the per-type picture except for vertex-snap rounding (alpha equal, <= 12 levels, <= 8 pixels a tile; see tiles_check) (rig --dump-cards: 64 x 64 RGBA8 of each baked type), and the gutter row above / below each tile equals the tile's
              first / last row (the copy that reproduces the sampler's clamp), for frames F1 then F2 in ONE process (the second frame's inputs differ: every picture is baked again)
  pictures    JARROD'S ACCEPTANCE BAR (docs\\DECISIONS.md 2026-11-18 08:55: the ~0.5 % difference of a one-pass atlas is accepted): the crowd picture with the atlas differs from the per-type one on at most 1 % of
              the lit pixels and at most 0.1 % of them by more than 2 levels. Why it is not 0 [L]: the far-card quad samples a 64 x 1320 image, a texture coordinate is rounded at 1/1320 instead of 1/64 (+-1 level
              on ~0.5 % of the pixels, plus a cut-out edge pixel on the 192 alpha threshold); the plants below move 10 % and 100 %.
  problems    no `problem` line in the stderr of any rig run (a Problem is a renderer contract break: e.g. transient_needs_contents)
  partial     frames F2, F2+drop3+palette, F2: the second draw drops one type and changes one palette (the kept tiles make the clear a PARTIAL one: depth + stencil cleared full, colour by rect), the third brings
              the type back: one pass each, pictures equal the per-type ones
  key         F2 then F2 with ONLY the light / ONLY the stage page / ONLY one palette changed: each rebakes (light, page: every tile; palette: that tile alone, the rest kept)
  dropped     F1, a discarded record of F2 (the hook ran, RenderFrame never did), F2: the renderer was told, so F2 rebakes every tile; the planted twin (renderer not told) keeps stale tiles and must fail
  passes      the bake opens one pass (ref: one per type, >= 10); both, the same frame drawn twice in a row: second time 0 passes and every picture kept (same_inputs == bakes of the first)
  kept        the repeated frame's picture equals ref
  plant offset   --plant atlas_offset (the first tile's picture one row low) differs from ref
  plant stale    --plant atlas_stale (a baked picture is never baked again) draws frame F2 after F1 differently from ref
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return REPO


MAIN = main_checkout()
bad = 0


def bar(ok, text):
    global bad
    print(('PASS ' if ok else 'FAIL ') + text)
    bad += 0 if ok else 1


def unit():
    cxx = shutil.which('clang++') or r'C:\Program Files\LLVM\bin\clang++.exe'
    outdir = os.path.join(REPO, 'out', 'tools')
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, 'card_atlas_unit.exe')
    r = subprocess.run([cxx, '-std=c++20', '-O1', os.path.join(HERE, 'card_atlas_unit.cpp'), '-o', exe], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        bar(False, 'card_atlas_unit compiles')
        return
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    bar(r.returncode == 0, 'card_atlas_unit: the rule holds')
    for plant, what in (('pitch', 'tiles without gutter rows'), ('v', 'v without the tile offset')):
        r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True)
        bar(r.returncode != 0, f'planted twin "{what}" must FAIL the unit (exit {r.returncode})')


PROBLEMS = []


def run_rig(capture, frames, view, out_png, extra=(), size='1280x720', dump=None):
    exe = os.path.join(REPO, 'out', 'tools', 'crowd_live_rig.exe')
    cmd = [exe, '--capture', os.path.join(MAIN, 'out', 'captures', capture), '--frames', ','.join(str(f) for f in frames), '--view', view, '--game', os.path.join(MAIN, 'game_data', 'extracted'),
           '--out', out_png, '--size', size] + list(extra) + (['--dump-cards', dump] if dump else [])
    env = dict(os.environ, WWE_RDR_VALIDATION='1')
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    PROBLEMS.extend(ln for ln in p.stderr.splitlines() if re.search(r'problem', ln, re.I))
    per_frame = []   # (frame, bake passes, same inputs, bakes) from the stderr lines, in order
    for ln in p.stderr.splitlines():
        m = re.search(r'\[crowd_live\] frame (\d+): .*?bakes (\d+);.*bake passes (\d+), same inputs (\d+)', ln)
        if m:
            per_frame.append((int(m.group(1)), int(m.group(3)), int(m.group(4)), int(m.group(2))))
    val = [int(x) for x in re.findall(r'validation (\d+)', ' '.join(l for l in p.stdout.splitlines() if l.startswith('RESULT')))]
    return p.returncode, per_frame, val


def png_for(path, frame, nframes):
    if nframes == 1:
        return path
    return path[:path.rfind('.')] + f'_f{frame}' + path[path.rfind('.'):]


def load(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB')).astype(np.int32)


def maxdiff(a, b):
    import numpy as np
    return int(np.abs(a - b).max()), int((np.abs(a - b).sum(axis=2) > 0).sum())


def spread(a, b):
    """(lit pixels of a or b, pixels that differ, pixels that differ by more than 2 levels in a channel, max)"""
    import numpy as np
    clear = np.array([26, 26, 33])
    lit = (np.abs(a - clear).sum(axis=2) > 12) | (np.abs(b - clear).sum(axis=2) > 12)
    df = np.abs(a - b).max(axis=2)
    return int(lit.sum()), int((df > 0).sum()), int((df > 2).sum()), int(df.max())


def read_dump(path):
    out = []
    with open(path, 'rb') as f:
        data = f.read()
    step = 1 + 64 * 64 * 4 + 256 + 256
    for t in range(20):
        b = data[t * step:(t + 1) * step]
        out.append((b[0] == 1, b[1:1 + 16384], b[1 + 16384:1 + 16384 + 256], b[1 + 16384 + 256:]))
    return out


def tiles_check(label, ref_path, atl_path, plant=False):
    import numpy as np
    r, a = read_dump(ref_path), read_dump(atl_path)
    n = exact = gut = 0
    worst_px = worst_lvl = worst_alpha = 0
    total_px = 0
    bad_types = []
    for t in range(20):
        if not (r[t][0] and a[t][0]):
            if r[t][0] != a[t][0]:
                bad_types.append(t)
            continue
        n += 1
        ra = np.frombuffer(r[t][1], dtype=np.uint8).reshape(64, 64, 4).astype(int)
        aa = np.frombuffer(a[t][1], dtype=np.uint8).reshape(64, 64, 4).astype(int)
        df = np.abs(ra - aa)
        px = int((df.max(axis=2) > 0).sum())
        exact += px == 0
        total_px += px
        worst_px, worst_lvl, worst_alpha = max(worst_px, px), max(worst_lvl, int(df.max())), max(worst_alpha, int(df[..., 3].max()))
        gut_ok = a[t][2] == a[t][1][:256] and a[t][3] == a[t][1][-256:]
        gut += gut_ok
        if not gut_ok or px > 8 or int(df.max()) > 12 or int(df[..., 3].max()) > 0:
            bad_types.append(t)
    # A tile's viewport sits up to 1255 rows down the atlas, so the window y of a vertex is a bigger float there (ulp 1.2e-4 px) and the rasteriser's snap to its 1/256 px grid goes the other way for about 1 % of vertex
    # coordinates: the vertex moves 1/256 px and a pixel beside a hard texture edge reads a slightly different texture weight. NOT exactly reproducible by any single-pass atlas [L]. Measured on brock_losswin frames 110 / 200:
    # 0-6 pixels a tile, <= 6 levels (the edge pixels of a texture), alpha (the cut-out silhouette) never. Bar (twice the measured worst, tied to nothing else): alpha identical, <= 12 levels, <= 8 of 4096 pixels a tile;
    # the gutters equal their edge row byte for byte. The plants move 760+ pixels a tile by up to 255.
    print(f'    {label}: {exact} / {n} tiles byte-equal, {total_px} pixels differ in all (worst tile {worst_px}, worst {worst_lvl} level, alpha {worst_alpha}), {gut} / {n} gutter rows equal their edge rows')
    ok = n > 0 and gut == n and not bad_types
    if plant:
        bar(not ok, f'{label}: the tile bar must FAIL on the plant (bad types {bad_types})')
        return
    bar(ok, f'{label}: every card picture within the rounding bar of the per-type picture (alpha equal, <= 12 levels, <= 8 pixels a tile), gutter rows exact (bad types {bad_types})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default='brock_losswin')
    ap.add_argument('--frames', default='110,200')
    ap.add_argument('--out', default=None)
    ap.add_argument('--unit', action='store_true')
    a = ap.parse_args()
    if a.unit:
        unit()
        sys.exit(1 if bad else 0)
    sys.path.insert(0, HERE)
    import run_dirs
    out = run_dirs.run_out('CB1_card_atlas', a.out)
    os.makedirs(out, exist_ok=True)
    f1, f2 = [int(x) for x in a.frames.split(',')][:2]
    seq = [f1, f2]
    views = (('cards', 'overview', ['--only', 'cards']), ('crowd', 'capture', []))
    for vname, view, vextra in views:
        res = {}
        for arm, extra in (('ref', ['--card-atlas', '0']), ('one', ['--card-atlas', '1']), ('both', ['--card-atlas', '3'])):
            png = os.path.join(out, f'{vname}_{arm}.png')
            dump = os.path.join(out, f'{vname}_{arm}_cards') if vname == 'cards' else None
            rc, per, val = run_rig(a.capture, seq, view, png, vextra + extra, dump=dump)
            res[arm] = (rc, per, val, png, dump)
            bar(rc in (0, 4) and val and all(v == 0 for v in val), f'{vname} {arm}: the rig drew {len(per)} frames, validation {val} (exit {rc})')
        for arm in ('one', 'both'):
            for fr in seq:
                lit, n, n2, d = spread(load(png_for(res['ref'][3], fr, len(seq))), load(png_for(res[arm][3], fr, len(seq))))
                bar(n <= 0.01 * lit and n2 <= 0.001 * lit, f'{vname} frame {fr}: --card-atlas {"1" if arm == "one" else "3"} picture vs one pass per type: {n} of {lit} lit pixels differ, {n2} by more than 2 levels, max {d}')
                if vname == 'cards':
                    tiles_check(f'frame {fr} --card-atlas {"1" if arm == "one" else "3"}', res['ref'][4] + f'_f{fr}.bin', res[arm][4] + f'_f{fr}.bin')
        ref_passes = [p[1] for p in res['ref'][1]]
        one_passes = [p[1] for p in res['one'][1]]
        bar(len(ref_passes) == 2 and all(p >= 10 for p in ref_passes) and one_passes == [1, 1], f'{vname} bake passes a frame: one per type {ref_passes} -> atlas {one_passes}')
        if vname == 'cards':
            # the same frame twice in one process: the second is kept whole
            png = os.path.join(out, 'cards_repeat.png')
            rc, per, val = run_rig(a.capture, [f2, f2], 'overview', png, ['--only', 'cards', '--card-atlas', '3'], dump=os.path.join(out, 'cards_repeat_cards'))
            bar(rc == 0 and val == [0, 0], f'both, frame {f2} twice: the rig passes, validation {val} (a frame with no bake pass still samples the atlas in the right layout)')
            bar(len(per) == 2 and per[0][1] == 1 and per[1][1] == 0 and per[1][2] == per[0][3] and per[1][3] == 0, f'both, frame {f2} twice: passes {[p[1] for p in per]}, same inputs {[p[2] for p in per]} (the second keeps all {per[0][3] if per else "?"})')
            tiles_check(f'the kept pictures (frame {f2}, second draw)', res['ref'][4] + f'_f{f2}.bin', os.path.join(out, 'cards_repeat_cards') + f'_f{f2}.bin')
            lit, n, n2, d = spread(load(png_for(png, f2, 2)), load(png_for(res['one'][3], f2, len(seq))))
            bar(n == 0, f'the kept pictures (frame {f2}, second draw) draw the same crowd as a fresh atlas bake: {n} pixels differ (max {d})')
            # partial clear, key coverage, dropped frame
            ref_dump = res['ref'][4] + f'_f{f2}.bin'
            def seqrun(tag, toks, extra=()):
                d = os.path.join(out, f'cards_{tag}_cards')
                rc, per, val = run_rig(a.capture, toks, 'overview', os.path.join(out, f'cards_{tag}.png'), ['--only', 'cards', '--card-atlas', '3'] + list(extra), dump=d)
                return rc, per, val, d + f'_f{f2}.bin'
            rc, per, val, dp = seqrun('partial', [f2, f'{f2}+drop3+palette', f2])
            bar(rc == 0 and len(per) == 3 and [p[1] for p in per] == [1, 1, 1] and per[1][2] > 0 and per[2][2] > 0 and per[2][3] >= 1, f'partial: passes {[p[1] for p in per]}, baked {[p[3] for p in per]}, kept {[p[2] for p in per]} (validation {val})')
            tiles_check('partial, third draw', ref_dump, dp)
            n_all = None
            for tag, tok in (('light', f'{f2}+light'), ('page', f'{f2}+page'), ('palette', f'{f2}+palette')):
                rc, per, val, dp = seqrun('key_' + tag, [f2, tok])
                n_all = per[0][3] if per else 0
                if tag == 'palette':
                    ok = rc == 0 and len(per) == 2 and per[1][1] == 1 and per[1][3] == 1 and per[1][2] == n_all - 1
                else:
                    ok = rc == 0 and len(per) == 2 and per[1][1] == 1 and per[1][3] == n_all and per[1][2] == 0
                bar(ok, f'key: only the {tag} changed between two draws of frame {f2}: baked {[p[3] for p in per]} of {n_all}, kept {[p[2] for p in per]}, passes {[p[1] for p in per]}')
            rc, per, val, dp = seqrun('dropped', [f1, f'd{f2}', f2])
            bar(rc == 0 and len(per) == 2 and per[1][1] == 1 and per[1][2] == 0, f'dropped: the discarded record of frame {f2} (renderer told): the next draw bakes {per[1][3] if len(per) == 2 else "?"} and keeps {per[1][2] if len(per) == 2 else "?"}')
            tiles_check('dropped, frame ' + str(f2), ref_dump, dp)
            rc, per, val, dp = seqrun('dropped_plant', [f1, f'd{f2}', f2], ['--plant', 'drop_unnoted'])
            tiles_check('plant drop_unnoted (the host never told the renderer)', ref_dump, dp, plant=True)
            # planted twins
            png = os.path.join(out, 'cards_plant_offset.png')
            rc, per, val = run_rig(a.capture, [f2], 'overview', png, ['--only', 'cards', '--card-atlas', '1', '--plant', 'atlas_offset'], dump=os.path.join(out, 'cards_plant_offset_cards'))
            tiles_check('plant atlas_offset', res['ref'][4] + f'_f{f2}.bin', os.path.join(out, 'cards_plant_offset_cards') + f'_f{f2}.bin', plant=True)
            lit, n, n2, d = spread(load(png_for(res['ref'][3], f2, len(seq))), load(png))
            bar(rc in (0, 4) and n2 > 0.001 * lit, f'plant atlas_offset (one tile one row low): {n} of {lit} lit pixels differ, {n2} by more than 2 levels (the picture bar is 0.1 %: the plant must exceed it)')
            png = os.path.join(out, 'cards_plant_stale.png')
            rc, per, val = run_rig(a.capture, seq, 'overview', png, ['--only', 'cards', '--card-atlas', '3', '--plant', 'atlas_stale'], dump=os.path.join(out, 'cards_plant_stale_cards'))
            tiles_check('plant atlas_stale', res['ref'][4] + f'_f{f2}.bin', os.path.join(out, 'cards_plant_stale_cards') + f'_f{f2}.bin', plant=True)
            lit, n, n2, d = spread(load(png_for(png, f2, len(seq))), load(png_for(res['ref'][3], f2, len(seq))))
            bar(rc in (0, 4) and n2 > 0.001 * lit and per and per[-1][2] > 0, f'plant atlas_stale (never baked again): frame {f2} after {f1}: {n} of {lit} lit pixels differ, {n2} by more than 2 levels (the plant must exceed the 0.1 % bar; kept {per[-1][2] if per else "?"})')
    bar(not PROBLEMS, f'no renderer problem line in any rig run ({len(PROBLEMS)}{": " + PROBLEMS[0][:160] if PROBLEMS else ""})')
    print('PASS' if not bad else f'FAIL ({bad})')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

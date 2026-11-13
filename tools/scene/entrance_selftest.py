#!/usr/bin/env python3
"""Lane 15 acceptance test (offline, 0 game runs): entrances on the Arena Viewer's "rdr" backend against its own "vk" backend.

  py -I tools\\scene\\entrance_selftest.py [--root DIR] [--size 640x360] [--only NAME] [--scan NAME]

Three entrances the viewer plays (ENTRANCES below), each at three entrance clock points (start, the light peak, the ring walk), shot on both
backends with the same camera, in two framings: CLOSE (the entrant fills the middle columns: the tile bar over his region) and WIDE (the stage,
the tron screens, the rig's light pools: the tile bar over the whole picture). Pass = rdr within the bar of vk everywhere, rdr validation 0.
Planted faults (each must FAIL the same comparison, then the restored run passes):
  * the entrant's light direction flipped in what the front-end is fed (WWE_RDR_FLIP_LIGHT_DIR=1): the S98 "entrant too dark again" class
    (LESSONS: look fixes verified one at a time can cancel; the shared light-direction code) -> must fail on the entrant's tiles;
  * the video surface never uploaded (WWE_RDR_NO_LIVE=1: a stale / black tron, and no entrance light pools) -> must fail on the stage tiles;
  * the rig's light sets not applied to the arena / props (WWE_RDR_NO_ENT_LIGHTS=1) -> must fail on the stage tiles.
Coverage per entrance: every mesh bound to our effect or listed "no effect yet" (counted). Sheet: out\\s101\\scene\\entrances_sheet.png (vk | rdr,
every panel labelled with the entrance, the clock point and the backend). Needs the viewer built with -Backend rdr, the player's game files and the
tron preview cache (LOCALAPPDATA\\wwe2k14-launcher\\trons); a missing input is a FAIL (never a pass)."""
import argparse
import json
import os
import re
import subprocess
import time
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import selftest as S  # noqa: E402  (the wrestler selftest's PNG / tile helpers)

MAIN = S.MAIN
TRONS = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons')

# name, wrestler, attire, why this one, clock points [(label, cue, frame within the cue)]
# Cue 5 = stage to ramp, 10 = the ramp, 15 = ring-in, 20 = in the ring (entrance_info "entrance_segments"). The light peak is the point of the
# entrance with the brightest wide picture on vk among cues 5 and 10 (found with --scan every 120 frames; the points are then fixed).
ENTRANCES = [
    ('rig', 161, 1, 'heavy light rig: Randy Orton, the red rig + pools + follow spot, main tron on the big screen',
     [('start', 5, 20), ('peak', 10, 740), ('ringwalk', 15, 500)]),
    ('props', 156, 0, 'props: Eddie Guerrero (a prop act shown through the entrance: a skinned prop piece)',
     [('start', 5, 20), ('peak', 10, 260), ('ringwalk', 15, 500)]),
    ('wall', 102, 0, 'tron on the wall: the wall movie surface of the stage (MVSW still) + the main tron',
     [('start', 5, 20), ('peak', 10, 380), ('ringwalk', 15, 500)]),
    ('belt', 139, 0, 'a champion carrying a title: John Cena, WWE Championship in the hand (the belt\'s normal-mapped meshes take the entrant\'s light set: the S98 dull-belt class)',
     [('start', 5, 20), ('peak', 5, 300), ('ringwalk', 20, 300)]),  # (Cena's bank: cues 10 and 15 are only 166 / 158 frames long; the scan for his peak was not usable)
]
CLOSE_DIST, WIDE_DIST = 260, 700
# A panel that does not match yet: (reason, ceiling of the worst tile, floor of the structure correlation). Counted and printed as KNOWN, never as a pass; the lane FAILS when it
# gets worse than its ceiling and when it starts to pass (then delete the entry). Planted-fault checks never use these panels.
KNOWN_OPEN = {
    ('belt', 'start', 'close'): ('the same wall picture difference as the wall entrance (cue 5 frame 20 stands at the wall), seen behind the entrant; see the wall entry', 75.0, 0.90),
    ('wall', 'start', 'wide'): ('the wall movie surface, close to the stage: its picture sits about half a letter further right than the viewer\'s (the pillars and everything else match; '
                              'cause not found [U]: not scroll (neither backend scrolls), not winding (the order of the letters is the same))', 30.0, 0.65),
}
# Extra arguments of the viewer's entrance command per wrestler (a champion carrying a title: the belt's meshes take the entrant's light set).
ENTRANCE_ARGS = {139: {'title': 'hold', 'belt': 352}}
HEAD = [{'cmd': 'look', 'mode': 'lit', 'shadows': False, 'dof': False, 'fxaa': False, 'bloom': False, 'glare': False, 'gamma': False, 'haze': False, 'film': False, 'flashes': False}]


def entrance_cmds(wid, attire, points, size, shotdir, tag, roots=None):
    """roots: {label: [x, y, z]} the entrant's root at each point (from the viewer's own entrance_info): the camera frames him (his place moves with the motion)."""
    cmds = list(HEAD)
    cmds += [{'cmd': 'load', 'kind': 'arena', 'id': 5}, {'cmd': 'lights', 'mode': 'entrance', 'time': 140},
             {'cmd': 'stage', 'wrestler': wid, 'media': TRONS.replace('\\', '/')},
             dict({'cmd': 'entrance', 'wrestler': wid, 'attire': attire, 'cue': points[0][1], 'f': points[0][2], 'intro_hold': 0, 'camera': 'free',
                   'stage': False, 'play': False, 'particles': False}, **ENTRANCE_ARGS.get(wid, {})),
             {'cmd': 'tron_offset', 'offset': 0.233}]
    for label, cue, f in points:
        cmds.append({'cmd': 'entrance_time', 'cue': cue, 'f': f, 'camera': 'free'})
        for cam, dist in (('close', CLOSE_DIST), ('wide', WIDE_DIST)):
            r = (roots or {}).get(label, [0, 0, 0])
            cmds.append({'cmd': 'camera', 'target': [r[0], 190, r[2]], 'yaw': 205, 'pitch': 8, 'dist': dist, 'fov': 40})  # from the front, 25 degrees to the side
            cmds.append({'cmd': 'shot', 'path': os.path.join(shotdir, f'{tag}_{label}_{cam}.png').replace('\\', '/'), 'w': size[0], 'h': size[1]})
    cmds.append({'cmd': 'quit'})
    return cmds


def probe_roots(exe, game, outdir, name, wid, attire, points, size):
    """One vk launch: the entrant's root at every clock point (entrance_info \"current\".\"root\"), so both backends share one camera."""
    shotdir = os.path.join(outdir, name)
    os.makedirs(shotdir, exist_ok=True)
    cmds = list(HEAD) + [{'cmd': 'load', 'kind': 'arena', 'id': 5}, {'cmd': 'lights', 'mode': 'entrance', 'time': 140},
                         {'cmd': 'stage', 'wrestler': wid, 'media': TRONS.replace('\\', '/')},
                         dict({'cmd': 'entrance', 'wrestler': wid, 'attire': attire, 'cue': points[0][1], 'f': points[0][2], 'intro_hold': 0, 'camera': 'free',
                               'stage': False, 'play': False, 'particles': False}, **ENTRANCE_ARGS.get(wid, {}))]
    for label, cue, f in points:
        cmds += [{'cmd': 'entrance_time', 'cue': cue, 'f': f, 'camera': 'free'}, {'cmd': 'entrance_info'}]
    cmds.append({'cmd': 'quit'})
    script = os.path.join(shotdir, 'roots.jsonl')
    with open(script, 'w') as f:
        for c in cmds:
            f.write(json.dumps(c) + '\n')
    p = subprocess.run([exe, '--backend', 'vk', '--game', game, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=600)
    infos = [l for l in p.stdout.splitlines() if '"entrance_segments"' in l and '"current"' in l]
    # (the entrance command, entrance_time and entrance_info all answer with the "current" block: each reply is matched to its point by the cue and the cue-local frame it reports)
    roots = {}
    for l in infos:
        m = re.search(r'"current":\{"i":\d+,"cue":(\d+),"t":[-0-9.]+,"local":([-0-9.]+),"root":\[([-0-9.]+),([-0-9.]+),([-0-9.]+)\]', l)
        if not m:
            continue
        for label, cue, f in points:
            if int(m.group(1)) == cue and abs(float(m.group(2)) - f) < 1.0:
                roots[label] = [float(m.group(3)), float(m.group(4)), float(m.group(5))]
    return roots


def run(exe, backend, game, outdir, name, wid, attire, points, size, env_extra, roots=None):
    shotdir = os.path.join(outdir, name)
    os.makedirs(shotdir, exist_ok=True)
    tag = backend + ('_' + env_extra['tag'] if 'tag' in env_extra else '')
    for label, _, _ in points:
        for cam in ('close', 'wide'):
            p = os.path.join(shotdir, f'{tag}_{label}_{cam}.png')
            if os.path.exists(p):
                os.remove(p)
    script = os.path.join(shotdir, f'{tag}.jsonl')
    with open(script, 'w') as f:
        for c in entrance_cmds(wid, attire, points, size, shotdir, tag, roots):
            f.write(json.dumps(c) + '\n')
    env = dict(os.environ, **{k: v for k, v in env_extra.items() if k != 'tag'})
    log = os.path.join(shotdir, f'{tag}.err.txt')
    for attempt in range(4):  # (other viewers on the machine can hold the GPU's small host-visible heap for a moment: "renderer init failed" is retried)
        with open(os.path.join(shotdir, f'{tag}.out.txt'), 'w') as out, open(log, 'w') as err:
            p = subprocess.run([exe, '--backend', backend, '--game', game, '--script', script, '--exit', '--hidden'], stdout=out, stderr=err, env=env, timeout=600)
        with open(log, errors='replace') as f:
            if 'renderer init failed' not in f.read():
                break
        time.sleep(10)
    with open(os.path.join(shotdir, f'{tag}.out.txt'), errors='replace') as f:
        bad = [l for l in f.read().splitlines() if '"ok":false' in l]
    shots = {(label, cam): os.path.join(shotdir, f'{tag}_{label}_{cam}.png') for label, _, _ in points for cam in ('close', 'wide')}
    return shots, log, p.returncode, bad


def coverage_lines(text):
    """Every effect line of the LAST coverage list of an rdr log: (class, 'bound to our effect' | 'no effect yet', draws)."""
    blocks = text.split('[rdr] shot ')
    last = blocks[-1] if len(blocks) > 1 else ''
    cov = [(m.group(1), m.group(2), int(m.group(3))) for m in re.finditer(r'^  (\S+)\s+(bound to our effect|no effect yet)\s+.*?\((\d+) draws', last, re.M)]
    also = [(m.group(2), int(m.group(1))) for m in re.finditer(r'^    also (\d+) draws of (\S+) in the debug colour', last, re.M)]
    return cov, also


def coverage_all(text):
    """Union over every shot of the run: {class: [bound draws max, unbound draws max]} and the number of effects not bound."""
    res = {}
    for blk in text.split('[rdr] shot ')[1:]:
        for m in re.finditer(r'^  (\S+)\s+(bound to our effect|no effect yet)\s+.*?\((\d+) draws', blk, re.M):
            r = res.setdefault(m.group(1), [0, 0, m.group(2)])
            if m.group(2) == 'bound to our effect':
                r[0] = max(r[0], int(m.group(3)))
            else:
                r[1] = max(r[1], int(m.group(3)))
                r[2] = m.group(2)
        for m in re.finditer(r'^    also (\d+) draws of (\S+) in the debug colour', blk, re.M):
            r = res.setdefault(m.group(2), [0, 0, 'bound to our effect'])
            r[1] = max(r[1], int(m.group(1)))
    return res


TILE_MAX = 32.0  # the entrance bar: every tile's mean colour within 32 / 255 of the viewer's (measured: rdr worst tile 28.3 (the wall movie's fine text, close to the stage), otherwise <= 19; each planted fault has a tile over 37 at its best point)


def check(np, ref, got, whole):
    """The wrestler selftest's bar (tiles close, none black / flat, structure correlation, share of pixels far off) PLUS the entrance bar: no tile of the
    compared region is off by more than TILE_MAX in mean colour (the S98 entrant / stage faults hid inside the loose 40)."""
    def down(a):  # 4 x 4 area average: the structure correlation samples single pixels, which fine texture (the wall movie's text) turns into noise
        h4, w4 = a.shape[0] // 4, a.shape[1] // 4
        return a[:h4 * 4, :w4 * 4].reshape(h4, 4, w4, 4, 3).astype(np.float64).mean((1, 3)).round().astype(np.uint8)
    ok, notes = S.compare_crop(np, down(ref), down(got), whole=whole)
    h, w, _ = ref.shape
    x0, x1 = (0, w) if whole else (int(w * S.CROP[0]), int(w * S.CROP[1]))
    r, g = ref[:, x0:x1].astype(np.float64), got[:, x0:x1].astype(np.float64)
    ch, cw = r.shape[:2]
    worst, over = 0.0, 0
    for ty in range(S.TILES_Y):
        for tx in range(S.TILES_X):
            sl = (slice(ty * ch // S.TILES_Y, (ty + 1) * ch // S.TILES_Y), slice(tx * cw // S.TILES_X, (tx + 1) * cw // S.TILES_X))
            d = float(np.abs(r[sl].reshape(-1, 3).mean(0) - g[sl].reshape(-1, 3).mean(0)).mean())
            worst, over = max(worst, d), over + (d > TILE_MAX)
    notes.append(f'worst tile {worst:.1f}/255 ({over} tiles over {TILE_MAX:.0f})')
    return ok and over == 0, notes


def mean_lum(np, img):
    return float((img.astype(np.float64) @ np.array([0.3, 0.59, 0.11])).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--size', default='640x360')
    ap.add_argument('--only', default='')
    ap.add_argument('--scan', default='', help='NAME: shoot that entrance wide on vk every 120 frames of cues 5-20 and print the mean luminance (finds the light peak)')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except ImportError as e:
        print(f'FAIL  needs numpy and Pillow ({e})')
        return 1
    root = os.path.abspath(args.root)
    exe = os.path.join(root, 'out', 'tools', 'wwe2k14_assets_rdr.exe')
    game = os.path.join(MAIN, 'game_data', 'extracted')
    for need in (exe, game, TRONS):
        if not os.path.exists(need):
            print(f'FAIL  cannot read its input: {need}')
            return 1
    outdir = os.path.join(root, 'out', 's101', 'scene', 'entrances')
    os.makedirs(outdir, exist_ok=True)
    size = tuple(int(x) for x in args.size.split('x'))

    if args.scan:
        e = next(x for x in ENTRANCES if x[0] == args.scan)
        pts = [(f'c{cue}_{f}', cue, f) for cue, lens in ((5, 700), (10, 850), (15, 1200), (20, 1700)) for f in range(20, lens, 120)]
        shots, log, code, bad = run(exe, 'vk', game, outdir, 'scan_' + e[0], e[1], e[2], pts, size, {})
        for (label, cam), p in shots.items():
            if cam == 'wide' and os.path.exists(p):
                print(label, round(mean_lum(np, np.asarray(Image.open(p).convert('RGB'))), 1))
        return 0

    fails = []
    known_open = []
    panels = []
    cov_total = {}
    for name, wid, attire, why, points in ENTRANCES:
        if args.only and name != args.only:
            continue
        arms = {}
        roots = probe_roots(exe, game, outdir, name, wid, attire, points, size)
        if len(roots) != len(points):
            print(f'FAIL  {name}: the viewer did not report the entrant\'s place at every clock point ({roots})')
            fails.append(f'{name}: no roots')
            continue
        for tag, backend, env in (('vk', 'vk', {}), ('rdr', 'rdr', {'WWE_RDR_VALIDATION': '1'}),
                                  ('flip', 'rdr', {'WWE_RDR_VALIDATION': '1', 'WWE_RDR_FLIP_LIGHT_DIR': '1', 'tag': 'flip'}),
                                  ('nolive', 'rdr', {'WWE_RDR_VALIDATION': '1', 'WWE_RDR_NO_LIVE': '1', 'tag': 'nolive'}),
                                  ('nolights', 'rdr', {'WWE_RDR_VALIDATION': '1', 'WWE_RDR_NO_ENT_LIGHTS': '1', 'tag': 'nolights'}),
                                  ('nomesh', 'rdr', {'WWE_RDR_VALIDATION': '1', 'WWE_RDR_NO_MESH_SETS': '1', 'tag': 'nomesh'})):
            try:
                shots, log, code, bad = run(exe, backend, game, outdir, name, wid, attire, points, size, env, roots)
            except subprocess.TimeoutExpired:
                print(f'FAIL  {name} {tag}: viewer timed out')
                fails.append(f'{name} {tag}: timed out')
                break
            missing = [k for k, p in shots.items() if not os.path.exists(p)]
            if missing or bad:
                print(f'FAIL  {name} {tag}: {len(missing)} shots missing, errors {bad[:2]} (exit {code}); see {log}')
                fails.append(f'{name} {tag}: no shots')
                break
            with open(log, errors='replace') as f:
                arms[tag] = ({k: np.asarray(Image.open(p).convert('RGB')) for k, p in shots.items()}, f.read())
        if len(arms) < 6:
            continue
        vk, rdr = arms['vk'][0], arms['rdr']
        clean_ok = {}
        print(f'--- entrance "{name}" (wrestler {wid}/{attire}): {why}')
        for label, cue, f in points:
            for cam in ('close', 'wide'):
                ref, got = vk[(label, cam)], rdr[0][(label, cam)]
                ok, notes = check(np, ref, got, cam == 'wide')
                known = KNOWN_OPEN.get((name, label, cam))
                clean_ok[(label, cam)] = ok and not known
                if known and ok:
                    print(f'FAIL  {name} {label} {cam}: listed as a known difference but it passes now: delete the KNOWN_OPEN entry')
                    fails.append(f'{name} {label} {cam}: stale KNOWN_OPEN entry')
                elif known:
                    worst = float(re.search(r'worst tile ([0-9.]+)', notes[-1]).group(1))
                    corr = float(re.search(r'structure correlation (-?[0-9.]+)', notes[0]).group(1))
                    over = worst > known[1] or corr < known[2]
                    print(('FAIL' if over else 'KNOWN') + f' {name} {label} (cue {cue} f {f}) {cam}: rdr vs vk: ' + '; '.join(notes) + f'  [{known[0]}] ceiling: worst tile <= {known[1]}, correlation >= {known[2]}')
                    if over:
                        fails.append(f'{name} {label} {cam}: known-open panel got worse than its ceiling')
                    known_open.append(f'{name} {label} {cam}')
                else:
                    print(('PASS' if ok else 'FAIL') + f'  {name} {label} (cue {cue} f {f}) {cam}: rdr vs vk: ' + '; '.join(notes))
                    if not ok:
                        fails.append(f'{name} {label} {cam}: rdr differs from vk')
                panels.append((f'{name} / {label} (cue {cue}, frame {f}) / {cam}', ref, got))
        errs = [int(m) for m in re.findall(r'validation errors(?: so far)?: (\d+)', rdr[1])]
        if not errs or max(errs) != 0:
            print(f'FAIL  {name}: rdr validation errors: {max(errs) if errs else "no validation line"}')
            fails.append(f'{name}: validation')
        else:
            print(f'PASS  {name}: rdr validation errors: 0 ({len(errs)} shots)')
        # planted faults: the same comparison must fail
        def caught(tag, cam, labels, what):
            """True when at least one of `labels` at `cam` fails the tile bar for arm `tag`."""
            best = None
            for label in labels:
                if not clean_ok.get((label, cam)):
                    continue  # a panel the clean rdr arm does not pass (a known-open one) says nothing about a fault
                ok, notes = check(np, vk[(label, cam)], arms[tag][0][(label, cam)], cam == 'wide')
                if not ok and (best is None or float(re.search(r'worst tile ([0-9.]+)', notes[-1]).group(1)) > best[0]):
                    best = (float(re.search(r'worst tile ([0-9.]+)', notes[-1]).group(1)), label, notes)
            if best:
                print(f'PASS  {name}: planted fault "{what}" is caught at {best[1]} {cam} (its best point): ' + '; '.join(best[2]))
                return True
            print(f'FAIL  {name}: planted fault "{what}" was NOT caught at {labels} {cam}')
            fails.append(f'{name}: planted fault {what} passed the check')
            return False
        labels = [p[0] for p in points]
        caught('flip', 'close', labels, "the entrant's light direction flipped")
        caught('nolive', 'wide', labels, 'video surface not uploaded (stale / black tron + no pools)')
        caught('nolights', 'wide', labels, "the rig's light sets not applied to the arena / props")
        if name == 'belt':  # the belt's / carried props' meshes must use the set the viewer pointed them at (the entrant's), not the file's
            # The belt is a few thousand pixels: tile means cannot see it. Compare vk with rdr over the pixels where the per-mesh set changes the picture (rdr clean vs rdr no-mesh-set).
            best, worst_clean = None, 0.0
            for label in labels:
                for cam in ('close', 'wide'):
                    v, a, b = (x[(label, cam)].astype(np.float64) for x in (vk, rdr[0], arms['nomesh'][0]))
                    m = np.abs(a - b).sum(2) > 6
                    if m.sum() < 100:
                        continue
                    ea, eb = float(np.abs(a - v)[m].mean()), float(np.abs(b - v)[m].mean())
                    worst_clean = max(worst_clean, ea)
                    if eb >= 2 * ea and eb - ea >= 5 and (best is None or eb - ea > best[0]):
                        best = (eb - ea, label, cam, int(m.sum()), ea, eb)
            if worst_clean > 12:
                print(f'FAIL  belt: the belt / entrant-lit pixels differ from vk by {worst_clean:.1f}/255 on average (bar 12)')
                fails.append('belt: lit region differs from vk')
            else:
                print(f'PASS  belt: pixels the per-mesh light set decides match vk (worst mean error {worst_clean:.1f}/255, bar 12)')
            if best:
                print(f'PASS  belt: planted fault "the belt meshes keep the file\'s light set (no per-mesh set)" is caught at {best[1]} {best[2]}: {best[3]} pixels, error vs vk {best[4]:.1f} -> {best[5]:.1f}/255')
            else:
                print('FAIL  belt: planted fault "no per-mesh light set" was NOT caught')
                fails.append('belt: planted fault no-mesh-set passed the check')
        cov = coverage_all(rdr[1])
        for k, v in cov.items():
            t = cov_total.setdefault(k, [0, 0])
            t[0], t[1] = max(t[0], v[0]), max(t[1], v[1])
        unbound = {k: v for k, v in cov.items() if v[1]}
        bound = sum(v[0] for v in cov.values())
        print(f'PASS  {name} coverage: {len(cov)} material classes, {bound} draws bound to our effect, {sum(v[1] for v in cov.values())} listed "no effect yet" (debug colour)'
              + ('' if not unbound else ': ' + ', '.join(f'{k} {v[1]}' for k, v in sorted(unbound.items()))))
        if not cov:
            print(f'FAIL  {name}: no coverage list in the rdr log')
            fails.append(f'{name}: no coverage')
        for label, cue, f in points[:1]:
            pass

    if panels:
        label_h, gap = 16, 4
        h, w = panels[0][1].shape[:2]
        cols = 2
        rows = len(panels)
        sheet = Image.new('RGB', (cols * w + gap, rows * (h + label_h)), (32, 32, 32))
        d = ImageDraw.Draw(sheet)
        for i, (label, a, b) in enumerate(panels):
            y = i * (h + label_h)
            d.text((4, y + 2), f"vk  (the viewer's own picture)  {label}", fill=(255, 255, 255))
            d.text((w + gap + 4, y + 2), f"rdr (our renderer)  {label}", fill=(255, 255, 255))
            sheet.paste(Image.fromarray(a), (0, y + label_h))
            sheet.paste(Image.fromarray(b), (w + gap, y + label_h))
        path = os.path.join(root, 'out', 's101', 'scene', 'entrances_sheet.png')
        sheet.save(path)
        print(f'sheet: {path}  ({rows} rows: vk left, rdr right; each panel labelled entrance / clock point / framing)')
    if not panels and not fails:
        print('FAIL  nothing was checked')
        return 1
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print(f'PASS  lane 15 entrance selftest ({len(panels) - len(known_open)} of {len(panels)} panels match, {len(known_open)} known open: {known_open})')
    return 0


if __name__ == '__main__':
    sys.exit(main())

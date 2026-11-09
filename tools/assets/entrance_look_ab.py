r"""Session 98 (integration): ONE refcheck set for the entrance look (Orton, run 6816 + 6817) and the look-switch A/B, one viewer launch
per item. Replaces the separate setups of the skin / arena / rim / sharpness fixes (out\s98\*\items.json, ab.py, _entrant2.py): the
same placement / camera / entrance time per item for every arm, per-part metrics (face, arm, shirt) from hand-placed boxes (game box,
viewer box: the pose is not pixel-exact) and refcheck's per-region metrics. 0 game runs: it reads screenshots, probe CSVs and the
player's own game files through the viewer.

  py -I tools\assets\entrance_look_ab.py OUTDIR [--items a,b] [--arms default,rig_off,...] [--metrics-only]
OUTDIR/<item>/<arm>.png = the viewer shots, OUTDIR/results.json + OUTDIR/table.txt = the numbers (viewer / game ratios).
--metrics-only re-measures the shots already in OUTDIR (no launch: for new boxes).
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import refcheck as R  # noqa: E402
import refcheck_lib as L  # noqa: E402

CAP = os.path.join(R.MAIN, 'out', 'build', 'win-amd64-release', 'captures')
P1 = os.path.join(R.MAIN, 'out', 's98', 'run1', 'probe.csv')
P2 = os.path.join(R.MAIN, 'out', 's98', 'run2', 'probe.csv')

# name: (screenshot, probe, swap, cue, cue-local frame, parts {part: (game box, viewer box)} in 1280x720 pixels x0, y0, x1, y1)
# The 8341-10741 boxes are _entrant2.py's (skin fix); the rest placed on the game shots / the first viewer shots of this pass.
ITEMS = {
    '8341': ('6816_bookmark_5.png', P1, 8341, 5, 576,
             {'face': ((580, 140, 740, 300), (580, 160, 740, 320)), 'arm': ((410, 480, 540, 710), (410, 500, 550, 710)),
              'shirt': ((520, 340, 800, 520), (500, 350, 820, 520))}),
    '8941': ('6816_bookmark_6.png', P1, 8941, 10, 428,
             {'face': ((490, 150, 620, 300), (550, 150, 670, 300)), 'arm': ((280, 430, 410, 670), (270, 430, 410, 670)),
              'shirt': ((390, 300, 710, 540), (370, 300, 730, 540))}),
    '10141': ('6816_bookmark_8.png', P1, 10141, 15, 766,
              {'face': ((600, 160, 750, 320), (600, 270, 730, 440)), 'arm': ((690, 360, 840, 620), (710, 580, 860, 720))}),
    '10741': ('6816_bookmark_9.png', P1, 10741, 20, 122,
              {'head': ((720, 170, 790, 240), (690, 230, 760, 310)), 'arm': ((540, 350, 610, 510), (540, 410, 610, 550)),
               'shirt': ((620, 240, 800, 400), (620, 300, 800, 460))}),
    '11341': ('6816_bookmark_10.png', P1, 11341, 20, 722, {}),
    '9447': ('6817_r2_ramp_b.png', P2, 9447, 15, 213, {}),  # (run 2's cue times differ from run 1's: f = the viewer root nearest Orton's probe position)
    '11758': ('6817_r2_dump_a.png', P2, 11758, 20, 1416, {}),
}
BOXES_FILE = os.path.join(HERE, 'examples', 'entrance_look_boxes.json')  # boxes added later (item -> part -> [game box, viewer box])
# Session 98 (generalisation): items of other runs / arenas / wrestlers come from --items-file (examples\entrance_look_6819.json):
# name -> {shot (captures-relative or absolute), probe, swap, cue, f, arena, wrestler, parts {part: [game box, viewer box]}}.
# Items without arena / wrestler are Orton in arena 5 (the built-in set).
META = {}  # name -> {'arena', 'wrestler'}


def load_items_file(path):
    global BOXES_FILE
    d = json.load(open(path))
    ITEMS.clear()
    for n, it in d.items():
        if n.startswith('_'):
            continue
        probe = it['probe'] if os.path.isabs(it['probe']) else os.path.join(R.MAIN, it['probe'])
        ITEMS[n] = (it['shot'], probe, it['swap'], it['cue'], it['f'], {p: tuple(tuple(x) for x in gv) for p, gv in it.get('parts', {}).items()})
        META[n] = {'arena': it['arena'], 'wrestler': it['wrestler'], 'entrance': it.get('entrance', {})}
        if 'tron_offset' in it:
            TRON_OFFSET[n] = it['tron_offset']
    BOXES_FILE = path + '.boxes'  # (none: the parts live in the items file)


def meta(n):
    return META.get(n, {'arena': 5, 'wrestler': 161})
# Video seconds the game's big screen showed (the shot's screen correlated against every tron frame, out\s98\integrate\notes.md). Run 1
# items follow the viewer's own rule (tron_video.h offset / loop_gap); run 2's cue times differ from run 1's (its cue 0 is not known), so
# its items get the measured video time through tron_offset (entrance clock 1910 + 213 = 2123 frames -> raw 35.383 s; 12.1 s on the
# second pass = raw 38.64 s with the 26.536 s loop).
TRON_OFFSET = {'9447': 35.383 - 38.64}
# The motion strip: the entrance camera (take 1) at cue 10, frames 10 apart around the 8944 screenshot (cue-local 431).
MOTION = [381, 391, 401, 411, 421, 431]  # (431 = the 8944 bookmark; the clip freezes there, bookmark 7's clip starts ~200 frames later)

OWN_LM_DEFAULT = True  # the viewer's default of nt_own_lm (renderer.h Effects)
# The look switches. Every arm starts from the defaults (RESET after each shot). rig_off = main before the integration fix.
RESET = [{'cmd': 'look', 'skin': 'game', 'rim_light': True, 'pools': True, 'film': True, 'dof': True, 'bloom': True, 'entrance': True, 'cull': 1,
          'flashes': True, 'ent_lm': True, 'ent_lm_bg': False, 'ent_own_lm': OWN_LM_DEFAULT, 'crowd_cards': 1, 'follow_spot': True},
         {'cmd': 'entrance_time', 'rig': True, 'l4': True, 'light_dirs': 'keyed', 'rig_weights': True, 'prop_light': True}]
ARMS = {
    'default': [],
    'rig_off': [{'cmd': 'entrance_time', 'rig': False}],
    'skin_old': [{'cmd': 'look', 'skin': 'old'}],
    'rim_off': [{'cmd': 'look', 'rim_light': False}],
    'l4_off': [{'cmd': 'entrance_time', 'l4': False}],
    'dirs_arena': [{'cmd': 'entrance_time', 'light_dirs': 'arena'}],
    'pools_off': [{'cmd': 'look', 'pools': False}],
    'film_off': [{'cmd': 'look', 'film': False}],
    'bloom_04': [{'cmd': 'look', 'entrance': {'bloom': 0.4, 'glare': 0}}],
    'dof_off': [{'cmd': 'look', 'dof': False}],
    'cull_off': [{'cmd': 'look', 'cull': 0}],     # (stage fix) the movie surfaces two-sided again (main before)
    'cull_other': [{'cmd': 'look', 'cull': 2}],   # the other winding culled (proves the rule's side)
    # (flash fix) main before: no crowd camera flashes, no entrance LM variants; then each alone
    'flash_off': [{'cmd': 'look', 'flashes': False, 'ent_lm': False}],
    'flashes_off': [{'cmd': 'look', 'flashes': False}],
    'ent_lm_off': [{'cmd': 'look', 'ent_lm': False}],
    'ent_lm_bg_on': [{'cmd': 'look', 'ent_lm_bg': True}],  # the entrance LM rule on the yBG_* families too (off by default: ab3)
    # (generalisation 2) yBG_* surfaces with their own light map read the entrance page instead (run 6817 anatomy: VS 4F1118D4 /
    # EFCB021A / 2F1B8D16 / CED7FD64 fetch the page at slot 13, no second map); and both yBG_* rules together
    'own_lm_on': [{'cmd': 'look', 'ent_own_lm': True}],
    'own_lm_off': [{'cmd': 'look', 'ent_own_lm': False}],
    'own_bg_on': [{'cmd': 'look', 'ent_own_lm': True, 'ent_lm_bg': True}],
    # (gen check 3) the crowd card LOD off: every stands member a body with the bodies' gain (main before)
    'cards_off': [{'cmd': 'look', 'crowd_cards': 0}],
    # (spot / beard pass) no follow spot under the entrant on the stage light map page (main before)
    'follow_off': [{'cmd': 'look', 'follow_spot': False}],
    # (sweep look) the entrant's lobe weights always set 10's (main before; the rule: 1.0 unless the driving cue has slot 4)
    'weights_off': [{'cmd': 'entrance_time', 'rig_weights': False}],
    # (sweep look) the belt / hand props on their own default lights (main before)
    'prop_light_off': [{'cmd': 'entrance_time', 'prop_light': False}],
    'before': [{'cmd': 'entrance_time', 'rig_weights': False, 'prop_light': False}],
}

LUM = lambda c: 0.299 * c[..., 0] + 0.587 * c[..., 1] + 0.114 * c[..., 2]  # noqa: E731


def skin(c):
    r, g, b = c[..., 0] + 1e-3, c[..., 1], c[..., 2]
    return (r - b > 8) & (g / r > 0.45) & (g / r < 0.97) & (b / r > 0.2) & (b / r < 0.92) & (LUM(c) > 6)


def part_mean(im, box, part):
    x0, y0, x1, y1 = box
    c = im[y0:y1, x0:x1]
    m = ((LUM(c) > 3) & (LUM(c) < 80) & ~skin(c)) if part == 'shirt' else skin(c)
    return float(LUM(c)[m].mean()) if m.sum() > 30 else float('nan')


def boxes():
    b = {k: dict(v[5]) for k, v in ITEMS.items()}
    if os.path.exists(BOXES_FILE):
        for k, parts in json.load(open(BOXES_FILE)).items():
            b.setdefault(k, {}).update({p: tuple(tuple(x) for x in gv) for p, gv in parts.items()})
    return b


def camera(out, name):
    cache = os.path.join(out, 'cameras.json')
    cams = json.load(open(cache)) if os.path.exists(cache) else {}
    if name not in cams:
        shot, probe, swap = ITEMS[name][:3]
        s, vp, eye = R.camera_from_probe(probe, swap)
        cams[name] = [vp, eye]
        json.dump(cams, open(cache, 'w'))
    return cams[name]


def launch(out, names, arms, motion=False, size=(1280, 720), tag=''):
    """ONE viewer launch per arena + wrestler group (the entrance is moved with entrance_time)."""
    groups = {}
    for n in names:  # (Session 98 sweep look: the group key carries the item's extra entrance arguments, e.g. a champion bank)
        groups.setdefault((meta(n)['arena'], meta(n)['wrestler'], json.dumps(meta(n).get('entrance', {}), sort_keys=True)), []).append(n)
    if len(groups) > 1:
        for k, (g, ns) in enumerate(sorted(groups.items())):
            launch(out, ns, arms, motion and k == 0, size, tag='_%d_%d_%d' % (g[0], g[1], k))
        return
    arena, wid = (meta(names[0])['arena'], meta(names[0])['wrestler']) if names else (5, 161)
    lines = ['{"cmd":"load","kind":"arena","id":%d}' % arena, json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': 140})]
    st = {'cmd': 'stage', 'wrestler': wid}
    if os.path.isdir(R.MEDIA):
        st['media'] = R.MEDIA.replace('\\', '/')
    lines.append(json.dumps(st))
    first = ITEMS[names[0]] if names else ITEMS['8941']
    # attire: the entrance model rule (viewer default); Orton's built-in set keeps its explicit PACH 1
    ent = {'cmd': 'entrance', 'wrestler': wid, 'cue': first[3], 'f': first[4], 'intro_hold': 0, 'camera': 'free', 'stage': False, 'play': False}
    if wid == 161 and not META:
        ent['attire'] = 1
    if names:
        ent.update(meta(names[0]).get('entrance', {}))
    lines.append(json.dumps(ent))
    for name in names:
        shot, probe, swap, cue, f, _ = ITEMS[name]
        vp, eye = camera(out, name)
        d = os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        lines.append(json.dumps({'cmd': 'tron_offset', 'offset': TRON_OFFSET.get(name, 0.233)}))
        lines.append(json.dumps({'cmd': 'entrance_time', 'cue': cue, 'f': f, 'camera': 'free'}))
        lines.append(json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': eye}))
        for a in arms:
            lines += [json.dumps(c) for c in ARMS[a]]
            p = os.path.join(d, a + '.png')
            if os.path.exists(p):
                os.remove(p)
            lines.append(json.dumps({'cmd': 'shot', 'path': p.replace('\\', '/'), 'w': size[0], 'h': size[1]}))
            lines += [json.dumps(c) for c in RESET]
    if motion:
        d = os.path.join(out, 'motion')
        os.makedirs(d, exist_ok=True)
        lines.append(json.dumps({'cmd': 'tron_offset', 'offset': 0.233}))
        for f in MOTION:
            lines.append(json.dumps({'cmd': 'entrance_time', 'cue': 10, 'f': f, 'camera': 'entrance', 'take': 1}))
            lines.append(json.dumps({'cmd': 'shot', 'path': os.path.join(d, 'f%03d.png' % f).replace('\\', '/'), 'w': size[0], 'h': size[1]}))
    script = os.path.join(out, 'viewer%s.jsonl' % tag)
    open(script, 'w').write('\n'.join(lines) + '\n')
    pr = subprocess.run([R.EXE, '--game', R.GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    open(os.path.join(out, 'viewer_out%s.txt' % tag), 'w').write(pr.stdout[-60000:] + '\n--stderr--\n' + pr.stderr[-4000:])
    bad = [l for l in pr.stdout.splitlines() if '"ok":false' in l]
    if bad:
        print('viewer errors', bad[:3])


def measure(out, names, arms):
    bx = boxes()
    res = {}
    for n in names:
        geo = R.arena_geo(meta(n)['arena'])
        shot = os.path.join(CAP, ITEMS[n][0])
        game = R.read_img(shot, (1280, 720))
        vp, eye = camera(out, n)
        regions = L.build_regions(vp, eye, geo, 1280, 720)
        ign = np.zeros((720, 1280), bool)
        res[n] = {}
        for a in arms:
            p = os.path.join(out, n, a + '.png')
            if not os.path.exists(p):
                continue
            view = R.read_img(p, (1280, 720))
            r = {'parts': {}, 'regions': {}}
            for part, (gb, vb) in bx.get(n, {}).items():
                g, v = part_mean(game, gb, part), part_mean(view, vb, part)
                r['parts'][part] = [round(g, 1), round(v / g, 2) if g == g and v == v else None]
            # regions: the entrant's boxes left out (they are the parts' job)
            ign2 = ign.copy()
            for part, (gb, vb) in bx.get(n, {}).items():
                for x0, y0, x1, y1 in (gb, vb):
                    ign2[y0:y1, x0:x1] = True
            cmp_ = L.compare(game, view, regions, ign2)
            for k, s in cmp_['regions'].items():
                if s.get('pixels', 0) > 2000:
                    r['regions'][k.split(' ')[0] if not k.startswith('stage b') else 'backdrop'] = [s['game_lum'], s['ratio'], s['game_rgb'], s['view_rgb']]
            res[n][a] = r
    json.dump(res, open(os.path.join(out, 'results.json'), 'w'), indent=1)
    lines = []
    for n, arms_r in res.items():
        for a, r in arms_r.items():
            pt = '  '.join('%s %s x%s' % (k, v[0], v[1]) for k, v in r['parts'].items())
            rg = '  '.join('%s x%.2f' % (k, v[1]) for k, v in r['regions'].items())
            lines.append('%-6s %-10s | %s | %s' % (n, a, pt, rg))
    open(os.path.join(out, 'table.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--items', default=','.join(ITEMS))
    ap.add_argument('--arms', default=','.join(ARMS))
    ap.add_argument('--metrics-only', action='store_true')
    ap.add_argument('--motion', action='store_true', help='also shoot the motion strip (entrance camera, cue 10, MOTION frames)')
    ap.add_argument('--items-file', help='items of another run / arena / wrestler (examples\\entrance_look_6819.json)')
    a = ap.parse_args()
    if a.items_file:
        load_items_file(a.items_file)
        if a.items == ap.get_default('items'):
            a.items = ','.join(ITEMS)
    names, arms = [n for n in a.items.split(',') if n], a.arms.split(',')
    os.makedirs(a.out, exist_ok=True)
    if not a.metrics_only:
        launch(a.out, names, arms, a.motion)
    measure(a.out, names, arms)


if __name__ == '__main__':
    main()

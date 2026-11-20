#!/usr/bin/env python3
r"""SCU: the stage cull for Jarrod's eyes, offline (0 game runs): every match arena drawn from its own files through our renderer with the stage cull OFF (two-sided, the
draw before SCU) and ON (the game's rule: record word +0x8C bit 0x2, the `r` copy's winding swapped), from the arena sheet's beauty camera of one look (arena_rec
--camera-screens, arena_cams.h BeautyCam). Also the watch list of the bit-set meshes in m(N) motion slots: each such slot drawn ALONE (WWE_RDR_L_SLOTS=N-N) off and on.

  py -I tools\scene\stage_cull_sheet.py [--arenas 4,44] [--look match] [--census out\scu\cull_census.txt] [--out out\scu\sheet] [--sheet-dir MAIN\out\sheets]

Writes (scratch; pictures of game assets are never committed):
  OUT\stage_cull_<look>.json          per arena: covered pixels (not the clear colour) off / on, changed pixels (any channel > 2) and their share of covered
  SHEET_DIR\cull_before_after.png     the ten arenas with the largest changed share of the picture: OFF | ON | changed pixels in red over a dimmed ON, each tile labelled (arena, share)
  OUT\stage_cull_mslots.txt           per bit-set m(N) mesh's slot, drawn alone from five sides framed on its bounds: the best view's coverage off / on, each view's, and VANISHED when the cull-on best view covers under half the cull-off best
The m(N) watch list needs `arena_rec --cull-census` lines (cull_census_m) in --census.
"""
import argparse, json, os, re, subprocess, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'harvest'))
sys.path.insert(0, HERE)
import stage_cull_proof as scp   # look_camera: the game-space camera of the proof (handedness checked there)
SCENE_WORD = {'entrance': 4, 'match': 1}


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.normpath(os.path.dirname(common)) if common else ROOT


def png(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB'))


def rig(exe, rsc, game, prefix, env_add, look, size, arena_only=False, extra=()):
    env = dict(os.environ)
    for k in ('WWE_RDR_STAGE_CULL', 'WWE_RDR_PLANT_STAGE_CULL_NO_FLIP', 'WWE_RDR_L_SLOTS'):
        env.pop(k, None)
    env.update(env_add)
    cmd = [exe, '--record', rsc, '--game', game, '--size', size, '--prefix', prefix, '--no-hooks'] + (['--families', '0x1'] if arena_only else []) + list(extra)
    if look == 'entrance':
        cmd.append('--look-entrance')
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600, env=env)
    if p.returncode != 0:
        raise RuntimeError('frame_rig %s: %s' % (prefix, (p.stdout + p.stderr)[-400:]))
    return png(prefix + ('_mask1.png' if arena_only else '_whole.png'))


def clear_of(img):
    """the clear colour = the most common corner colour (the frame's clear; the arena never covers all four corners in a beauty view)"""
    c = [tuple(img[y, x]) for y in (0, -1) for x in (0, -1)]
    return max(set(c), key=c.count)


def measure(off, on):
    cl = np.array(clear_of(off), np.int16)
    cov_off = np.abs(off.astype(np.int16) - cl).max(axis=-1) > 2
    cov_on = np.abs(on.astype(np.int16) - cl).max(axis=-1) > 2
    chg = np.abs(on.astype(np.int16) - off.astype(np.int16)).max(axis=-1) > 2
    return dict(cov_off=int(cov_off.sum()), cov_on=int(cov_on.sum()), changed=int(chg.sum()), share=float(chg.sum()) / max(1, int(cov_off.sum())),
                frame_share=float(chg.sum()) / chg.size), chg


def main(argv=None):
    import choice_ours
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arenas', default='all'); ap.add_argument('--look', default='match'); ap.add_argument('--size', default='640x360')
    ap.add_argument('--census', default=os.path.join(ROOT, 'out', 'scu', 'cull_census.txt'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'scu', 'sheet'))
    ap.add_argument('--sheet-dir', default=None)
    ap.add_argument('--mslots-only', action='store_true', help='only the m(N) watch list (no whole-arena renders, no sheet)')
    a = ap.parse_args(argv)
    main_root = main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    sheet_dir = a.sheet_dir or os.path.join(main_root, 'out', 'sheets')
    tools = os.path.join(ROOT, 'out', 'tools')
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(sheet_dir, exist_ok=True)
    arenas = choice_ours.MATCH_ARENAS if a.arenas == 'all' else [int(x) for x in a.arenas.split(',')]
    mslots = {}
    if os.path.exists(a.census):
        for ln in open(a.census):
            m = re.search(r'cull_census_m arena (\d+) slot (\d+) mesh (\d+) word (\S+) .* bounds (\S+)', ln)
            if m:
                b = [float(x) for x in m.group(5).split(',')]
                mslots.setdefault((int(m.group(1)), int(m.group(2))), []).append((int(m.group(3)), m.group(4), (np.array(b[:3]), np.array(b[3:]))))
    res, pics = {}, {}
    mlines = []
    for ar in arenas:
        rsc = os.path.join(a.out, 'a%02d_%s.rsc' % (ar, a.look))
        p = subprocess.run([os.path.join(tools, 'arena_rec.exe'), '--game', game, '--arena', str(ar), '--from-provider', '--scene-word', str(SCENE_WORD[a.look]), '--camera-from', 'none',
                            '--camera-screens', a.look, '--out', rsc], capture_output=True, text=True, errors='replace', timeout=300)
        if p.returncode != 0:
            res[ar] = dict(error='arena_rec %d' % p.returncode)
            continue
        pre = os.path.join(a.out, 'a%02d_%s' % (ar, a.look))
        if a.mslots_only and not any(k[0] == ar for k in mslots):
            continue
        if not a.mslots_only:
            try:
                off = rig(os.path.join(tools, 'frame_rig.exe'), rsc, game, pre + '_off', {'WWE_RDR_STAGE_CULL': '0'}, a.look, a.size)
                on = rig(os.path.join(tools, 'frame_rig.exe'), rsc, game, pre + '_on', {}, a.look, a.size)
            except RuntimeError as e:
                res[ar] = dict(error=str(e)[:200])
                continue
            r, chg = measure(off, on)
            res[ar] = r
            pics[ar] = (off, on, chg)
            print('a%02d %s changed %d px = %.4f of the picture' % (ar, a.look, r['changed'], r['frame_share']), flush=True)
        for (mar, slot), meshes in sorted(mslots.items()):
            if mar != ar:
                continue
            # the slot ALONE, framed by its own placed bounds from five sides (x+ x- z+ z- and above; game space, up = -y): a one-sided piece is legitimately gone from its back,
            # so the verdict compares the BEST view of each arm: VANISHED when the cull-on arm's best coverage is under half the cull-off arm's best
            lo = np.min([b[0] for _m, _w, b in meshes], axis=0); hi = np.max([b[1] for _m, _w, b in meshes], axis=0)
            c = (lo + hi) / 2; rad = max(30.0, float(np.linalg.norm(hi - lo)) / 2); dist = rad / np.tan(np.radians(25)) * 1.15
            env = {'WWE_RDR_L_SLOTS': '%d-%d' % (slot, slot)}
            best_off = best_on = 0
            per = []
            for vn, d in (('x+', (1, 0, 0)), ('x-', (-1, 0, 0)), ('z+', (0, 0, 1)), ('z-', (0, 0, -1)), ('up', (0.001, -1, 0.001))):
                eye = c + dist * np.asarray(d, np.float64)
                M = scp.look_camera(eye, c, 50.0, 16 / 9.)
                cam = ['--camera', ','.join('%.9g' % x for x in M.T.reshape(-1)), '--eye', ','.join('%.9g' % x for x in eye)]
                so = rig(os.path.join(tools, 'frame_rig.exe'), rsc, game, pre + '_s%d_%s_off' % (slot, vn), dict(env, WWE_RDR_STAGE_CULL='0'), a.look, a.size, True, cam)
                sn = rig(os.path.join(tools, 'frame_rig.exe'), rsc, game, pre + '_s%d_%s_on' % (slot, vn), env, a.look, a.size, True, cam)
                sr, _ = measure(so, sn)
                best_off, best_on = max(best_off, sr['cov_off']), max(best_on, sr['cov_on'])
                per.append('%s %d/%d' % (vn, sr['cov_off'], sr['cov_on']))
                for suf in ('_s%d_%s_off' % (slot, vn), '_s%d_%s_on' % (slot, vn)):
                    for ext in ('_mask1.png', '_report.txt'):
                        try:
                            os.remove(pre + suf + ext)
                        except OSError:
                            pass
            verdict = ('not drawn (no pixels from any side)' if best_off < 50 else 'VANISHED' if best_on < 0.5 * best_off else 'kept')
            mlines.append('a%02d slot %d meshes %s words %s: best covered off %d on %d (%.2f); off/on per view %s -> %s' % (
                ar, slot, ','.join(str(m) for m, _w, _b in meshes), ','.join(w for _m, w, _b in meshes), best_off, best_on, best_on / max(1, best_off), ', '.join(per), verdict))
            print('  ' + mlines[-1], flush=True)
    if not a.mslots_only:
        json.dump(res, open(os.path.join(a.out, 'stage_cull_%s.json' % a.look), 'w'), indent=1)
    if mlines:
        open(os.path.join(a.out, 'stage_cull_mslots.txt'), 'w').write('\n'.join(mlines) + '\n')
    top = sorted((ar for ar in pics), key=lambda x: -res[x]['frame_share'])[:10]
    if top:
        from PIL import Image, ImageDraw
        H, W = pics[top[0]][0].shape[:2]
        lab = 22
        sheet = Image.new('RGB', (W * 3, (H + lab) * len(top)), (16, 16, 16))
        d = ImageDraw.Draw(sheet)
        for i, ar in enumerate(top):
            off, on, chg = pics[ar]
            mark = (on.astype(np.float32) * 0.4).astype(np.uint8)
            grown = chg.copy()   # grown by 3 px so a few changed pixels are easy to find; the exact pixels inside in bright red
            for dy in range(-3, 4):
                for dx in range(-3, 4):
                    grown |= np.roll(np.roll(chg, dy, axis=0), dx, axis=1)
            mark[grown] = (255, 220, 0)
            mark[chg] = (255, 40, 40)
            y = i * (H + lab)
            for j, (img, t) in enumerate(((off, 'BEFORE: cull off (two-sided)'), (on, 'AFTER: the game\'s cull'), (mark, 'changed pixels: red (yellow halo to find them)'))):
                sheet.paste(Image.fromarray(img), (j * W, y + lab))
                d.text((j * W + 4, y + 4), 'arena %d %s | %s | changed %.1f %% of the picture' % (ar, a.look, t, 100 * res[ar]['frame_share']), fill=(255, 255, 255))
        sheet.save(os.path.join(sheet_dir, 'cull_before_after.png'))
        print('wrote', os.path.join(sheet_dir, 'cull_before_after.png'))
    return 0


if __name__ == '__main__':
    sys.exit(main())

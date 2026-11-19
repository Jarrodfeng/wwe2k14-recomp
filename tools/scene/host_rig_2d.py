#!/usr/bin/env python3
r"""The scene host's 2D rows on F, offline (S103 landing 2, piece P7 H2; docs\renderer\P7_DESIGN.md section 5 H2). 0 game runs.

  py -I tools\scene\host_rig_2d.py [--root DIR] [--quick] [--only menu] [--no-twins] [--out DIR]

The captured scene records were written before the 2D tap, so they carry no rows (every record reads NotTaken). The rows therefore come from the SAME place the P3c / I1 proofs get them: fview's rows
arm builds a Quad2D row from every row-site draw of the stream frame (quads2d_rig.h Truth, through the section-58 writer and parser) and `--quads-2d-dump` writes them, in draw order, as one record.
Per frame of the 2D corpus (out\captures\c2d_{menu,play,options,title,load}):
  reference  fview rows arm (--ui-ours --ui-quads --movie-ours --quads-2d-rows) with the menu backdrop particle draws dropped (--draw-drop site=...: the particles are not rows; they are P6's):
             the stream's own drawing of the same rows over black. Only a frame with NO other draw left (every draw a row-site draw our row replaced) is comparable.
  host       host_rig --mode host --flat --rows-dir: the SAME SceneHost the game runs, the record carrying those rows, a headless offscreen present; the sprites' pictures and the movies' planes come from
             the capture (host_rig_2d.h, a stand-in for the live resolvers). The host draws them with quads2d_draw Append onto F, no binder, no stream scan.
  bar        max channel difference <= 1 level and 0 pixels over 1 (the quads2d rig bar), on every comparable frame; rows drawn == rows in the record on those frames.
c2d_match_hud (a 3D frame: the host's 3D is not the stream's): rows drawn / not drawn by reason only, no picture comparison.
Planted twins (each must be caught): rows_first (rows drawn before the copy of the 3D picture: the order twin), rows_never (no 2D step), stale_record (the rows of the PREVIOUS record on this frame),
stale_planes (a movie row shows the previous frame's planes: run on the captures that hold movie rows).
Pictures, the sheet (reference | host | |diff| x 8) and logs go under <root>\out\s103\p7h2\ . Nothing from the game is written elsewhere or committed.
Exit 0 = pass."""
import argparse
import json
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ROW_SITES = {0x82BC3784, 0x82BC3EE8, 0x82BC484C, 0x82BC5688, 0x82BC8BEC, 0x82BC7E50}
PARTICLE_SITES = (0x82A0DA58, 0x82A0E338, 0x82A0ED2C)
OUR_FATES = ('ui_quads_ours', 'movie_ours')
CAPS = {
    'menu': ('out/captures/c2d_menu', (7, 40, 80)),
    'play': ('out/captures/c2d_play', (5, 30)),
    'options': ('out/captures/c2d_options', (5, 30)),
    'select': ('out/captures/c2d_select', (7, 30)),   # S103 PN4: guest-copied pictures (the second tier's arena pac is not indexed here)
    'title': ('out/captures/c2d_title', (15, 40)),
    'load': ('out/captures/c2d_load', (20, 60)),
    'match_hud': ('out/captures/c2d_match_hud', (60, 90)),
}
QUICK = {'menu': (7,), 'play': (5,), 'options': (5,), 'select': (7,), 'title': (15,), 'load': (20,), 'match_hud': (60,)}
COUNT_ONLY = ('match_hud',)
PIX = 1


def main_checkout(here):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=1500, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e, errors='replace')
    return p.returncode, (p.stdout or '') + (p.stderr or '')


class Report:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        self.fails += 0 if ok else 1
        return ok


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def diff(a, b):
    if a.shape != b.shape:
        return 255, a.shape[0] * a.shape[1], a.shape[0] * a.shape[1]
    d = np.abs(a - b).max(axis=2)
    return int(d.max()), int((d > PIX).sum()), int((d > 0).sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--no-twins', action='store_true')
    ap.add_argument('--out', default='')
    ap.add_argument('--real', action='store_true', help='S103 PN4: the host runs the REAL PictureResolver over forged guest memory + the player\'s file index and the movie planes from the plane store (host_rig --real), not the capture stand-ins')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = a.out or os.path.join(main_co, 'out', 's103', 'pn4' if a.real else 'p7h2')
    cache = os.path.join(main_co, 'out', 's103_i1', 'idx')
    os.makedirs(work, exist_ok=True)
    os.makedirs(cache, exist_ok=True)
    rep = Report()
    host_exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as _fe
    fexe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(host_exe):
        rep.line(False, 'no host_rig.exe (pwsh -NoProfile -File tools\\scene\\build_host_rig.ps1)')
        return 1
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        rep.line(False, 'input missing: the game files ' + game)
        return 1
    env = {'WWE2K14_UI_INDEX_CACHE': cache}
    caps = {}
    for k, (rel, frames) in CAPS.items():
        p = os.path.join(main_co, rel)
        if (a.only and k != a.only) or (k == 'select' and not a.real and a.only != 'select'):   # (select belongs to the --real run: the P7H2 lane's captures are unchanged)
            continue
        if not os.path.isfile(os.path.join(p, 'images.bin')):
            rep.line(False, 'input missing: the capture ' + p)
            return 1
        caps[k] = p
    drops = ['--draw-drop', 'class=particle']   # (the menu backdrop particles are not rows: they are P6's; a `site=` filter drops nothing, the class does)
    rows_args = ['--ui-ours', '--ui-quads', '--movie-ours', '--quads-2d-rows', '--game', game]
    sheet = []
    movie_caps = set()
    comparable_total = 0
    for cap, p in caps.items():
        frames = (QUICK[cap] if a.quick else CAPS[cap][1])
        rows_dir = os.path.join(work, 'rows_' + cap)
        refs = {}
        for f in frames:
            d = os.path.join(work, 'ref_%s_%d' % (cap, f))
            rc, text = run([fexe, '--capture', p, '--frame', str(f), '--root', root, '--judge-out', d, '--quads-2d-dump', os.path.join(rows_dir, 'r%d' % f)] + rows_args + drops, env=env)
            if rc or not os.path.isfile(os.path.join(d, 'picture.png')):
                print(text[-2000:])
                rep.line(False, 'fview rows arm %s frame %d failed (exit %d)' % (cap, f, rc))
                return 1
            oj = d + '_ops.json'
            rc2, text2 = run([fexe, '--capture', p, '--frame', str(f), '--root', root, '--ops', '--json', oj] + rows_args, env=env)
            ops = [o for o in json.load(open(oj, encoding='utf-8'))['ops'] if o.get('kind') == 'draw'] if rc2 == 0 and os.path.isfile(oj) else None
            other = None
            if ops is not None:
                other = sum(1 for o in ops if not (o.get('origin_op') == 19 and o['call_site'] in ROW_SITES and o['fate'] in OUR_FATES) and not (o.get('origin_op') == 19 and o['call_site'] in PARTICLE_SITES))
            nrows = -1
            m = re.search(r'rows dump: (\d+) rows', text)
            if m:
                nrows = int(m.group(1))
            refs[f] = (load(os.path.join(d, 'picture.png')), other, nrows)
        flat_args = (['--real'] if a.real else []) + ['--mode', 'host', '--flat', '--no-curve', '--scene', os.path.join(p, 'scene'), '--capture', p, '--rows-dir', rows_dir, '--game', game, '--frames', ','.join(str(f) for f in frames)]
        hd = os.path.join(work, 'host_' + cap)
        os.makedirs(hd, exist_ok=True)
        rc, text = run([host_exe] + flat_args + ['--out', hd] + (['--once'] if cap in COUNT_ONLY else []), env=env)
        if rc:
            print(text[-2500:])
            rep.line(False, 'host rig %s failed (exit %d)' % (cap, rc))
            return 1
        rowlines = {int(m.group(1)): m for m in re.finditer(r'^rows frame (\d+): (\d+) rows \((\d+) in the record, holes (\d+)\), drawn (\d+), not drawn (.*)$', text, re.M)}
        tot = [l for l in text.splitlines() if l.startswith('rows total')]
        print('INFO  %s: %s' % (cap, tot[-1] if tot else '(no totals)'))
        for l in text.splitlines():
            if l.startswith('real ') or l.startswith('      real resolver'):   # (includes "real pending: N of M frames drew with pictures still arriving": the frames before every picture arrived)
                print('INFO  %s %s' % (cap, l[:420]))
            if l.startswith('[scene][2d] exit'):
                print('INFO  %s census: %s' % (cap, l[:300]))
        if re.search(r'movie|planes [1-9]', ' '.join(tot)) or 'planes' in (tot[-1] if tot else ''):
            movie_caps.add(cap)
        for f in frames:
            ref, other, nrows = refs[f]
            rl = rowlines.get(f)
            drawn = int(rl.group(5)) if rl else -1
            seen = int(rl.group(2)) if rl else -1
            notd = rl.group(6) if rl else '?'
            if cap in COUNT_ONLY:
                rep.line(rl is not None and seen == nrows, '%s frame %d: rows in the record %d, rows seen by the host %d, drawn %d, not drawn %s (a 3D frame: no picture comparison)' % (cap, f, nrows, seen, drawn, notd))
                continue
            if a.real:
                rep.line('picture' not in notd and 'planes' not in notd, '%s frame %d (real resolver): rows dropped `picture` / `planes` = 0 (not drawn %s)' % (cap, f, notd))
            host = load(os.path.join(hd, 'host_%d.png' % f))
            mx, over, npx = diff(host, ref)
            comparable = other == 0
            if comparable:
                comparable_total += 1
                ok = mx <= PIX and over == 0 and drawn == seen == nrows and notd.strip() == '-'
                rep.line(ok, '%s frame %d: host F vs the rows arm, max channel diff %d (%d pixels over %d, %d at 1); rows in the record %d, seen %d, drawn %d, not drawn %s' % (cap, f, mx, over, PIX, npx - over, nrows, seen, drawn, notd))
            else:
                print('INFO  %s frame %d: not comparable (%s draws that are not rows remain in the stream frame); rows %d drawn %d not drawn %s; host vs arm max diff %d, %d px over %d' % (cap, f, 'unknown' if other is None else other, nrows, drawn, notd, mx, over, PIX))
            sheet.append((cap, f, ref, host, comparable))
    rep.line(comparable_total > 0, '%d comparable frames compared (frames whose stream picture is rows only)' % comparable_total)
    if not a.no_twins and comparable_total:
        # twins on the comparable frames of the first capture that has some
        twin_caps = [c for c in caps if c not in COUNT_ONLY]
        # --real: stale_planes belongs to the capture stand-in (the real planes come from the store: the twins no_flush / swap_chroma are theirs); resolver_null / wrong_entry are the resolver's (PN4).
        for plant in ('rows_first', 'rows_never', 'stale_record') + (('resolver_null', 'wrong_entry', 'no_flush', 'swap_chroma') if a.real else ('stale_planes',)):
            caught, why = False, ''
            if plant == 'stale_record' and not any(len(QUICK[c] if a.quick else CAPS[c][1]) >= 2 for c in twin_caps):
                print('INFO  twin stale_record: not run (--quick: one frame per capture; the full run holds it)')
                continue
            for cap in twin_caps:
                frames = (QUICK[cap] if a.quick else CAPS[cap][1])
                if plant == 'stale_record' and len(frames) < 2:
                    continue
                pd = os.path.join(work, 'plant_%s_%s' % (plant, cap))
                os.makedirs(pd, exist_ok=True)
                rc, text = run([host_exe] + (['--real'] if a.real else []) + ['--plant', plant, '--mode', 'host', '--flat', '--no-curve', '--scene', os.path.join(caps[cap], 'scene'), '--capture', caps[cap], '--rows-dir', os.path.join(work, 'rows_' + cap),
                                '--game', game, '--frames', ','.join(str(f) for f in frames), '--out', pd], env=env)
                if rc:
                    caught, why = True, 'host rig failed on %s' % cap
                    break
                for f in frames:
                    ref = None
                    d = os.path.join(work, 'ref_%s_%d' % (cap, f), 'picture.png')
                    if os.path.isfile(d):
                        ref = load(d)
                    hp = os.path.join(pd, 'host_%d.png' % f)
                    if ref is None or not os.path.isfile(hp):
                        continue
                    mx, over, _ = diff(load(hp), ref)
                    if mx > PIX:
                        caught, why = True, '%s frame %d max diff %d' % (cap, f, mx)
                        break
                if caught:
                    break
            rep.line(caught, 'twin %s: %s (%s)' % (plant, 'caught' if caught else 'NOT CAUGHT', why))
    if a.real and 'menu' in caps:   # S103 PN4F: the resolver's lifecycle (rejoin keeps the picture D3, a drawn picture survives a gap C1, a failed picture is retried D4) and its three twins
        mf = (QUICK['menu'] if a.quick else CAPS['menu'][1])[0]
        unit = [host_exe, '--mode', 'resolver', '--scene', os.path.join(caps['menu'], 'scene'), '--capture', caps['menu'], '--rows-dir', os.path.join(work, 'rows_menu'), '--game', game, '--frames', str(mf)]
        rc, text = run(unit, timeout=900, env=env)
        for l in text.splitlines():
            if l.startswith('PASS') or l.startswith('FAIL') or l.startswith('      '):
                print('INFO  resolver unit ' + l[:300])
        rep.line(rc == 0 and 'resolver unit: PASS' in text, 'resolver unit (menu frame %d): rejoin keeps the picture, a first-sight file join draws at once (T2F), a drawn picture survives a gap, a failed picture is retried' % mf)
        if not a.no_twins:
            for plant in ('c1_old', 'd3_old', 'd4_permanent', 't2f_old', 't2f_forever'):
                rc, text = run(unit + ['--plant', plant], timeout=900, env=env)
                bad_lines = [l for l in text.splitlines() if l.startswith('FAIL')]
                rep.line(rc != 0 and bool(bad_lines), 'twin %s: %s (%s)' % (plant, 'caught' if rc != 0 and bad_lines else 'NOT CAUGHT', bad_lines[0][:160] if bad_lines else '-'))
    if comparable_total and 'menu' in caps:   # R8: rows that cannot be drawn are dropped alone and counted by reason, never guessed
        pd = os.path.join(work, 'no_picture')
        os.makedirs(pd, exist_ok=True)
        f = (QUICK['menu'] if a.quick else CAPS['menu'][1])[0]
        rc, text = run([host_exe, '--plant', 'no_picture', '--mode', 'host', '--flat', '--no-curve', '--scene', os.path.join(caps['menu'], 'scene'), '--capture', caps['menu'], '--rows-dir', os.path.join(work, 'rows_menu'), '--game', game, '--frames', str(f), '--out', pd])
        m = re.search(r'rows total: (\d+) seen, (\d+) drawn, not drawn by reason: (.*?)(;|$)', text, re.M)
        ok = rc == 0 and m is not None and 0 < int(m.group(2)) < int(m.group(1)) and 'picture' in m.group(3) and 'planes' in m.group(3)
        by = {k: int(v) for k, v in re.findall(r'(\w+) (\d+)', m.group(3))} if m else {}
        rep.line(ok and sum(by.values()) + int(m.group(2)) == int(m.group(1)), 'R8 no resolver (menu frame %d): %s seen, %s drawn, dropped by reason %s: every row is dropped for exactly one reason, none guessed' % (f, m.group(1) if m else '?', m.group(2) if m else '?', m.group(3) if m else '?'))
    if comparable_total and 'menu' in caps:   # review (c): the GAME's rule for a row whose pass the tap does not know: drawn only when pixel-placed (PosScale 2/w, 2/h), the rest dropped `pass_unknown`
        pd = os.path.join(work, 'game_pass')
        os.makedirs(pd, exist_ok=True)
        f = (QUICK['menu'] if a.quick else CAPS['menu'][1])[0]
        rc, text = run([host_exe, '--game-pass', '--mode', 'host', '--flat', '--no-curve', '--scene', os.path.join(caps['menu'], 'scene'), '--capture', caps['menu'], '--rows-dir', os.path.join(work, 'rows_menu'), '--game', game, '--frames', str(f), '--out', pd])
        m = re.search(r'^rows frame (\d+): (\d+) rows \((\d+) in the record, holes (\d+)\), drawn (\d+), not drawn (.*)$', text, re.M)
        by = {k: int(v) for k, v in re.findall(r'(\w+) (\d+)', m.group(6))} if m else {}
        ok = rc == 0 and m is not None and int(m.group(5)) > 0 and int(m.group(5)) + sum(by.values()) == int(m.group(2)) and set(by) <= {'pass_unknown', 'picture', 'planes'}
        rep.line(ok, 'game rule for Unknown-pass rows (menu frame %d): %s seen, %s drawn, not drawn %s (pixel-placed rows drawn, the rest `pass_unknown`, every row one fate)' % (f, m.group(2) if m else '?', m.group(5) if m else '?', m.group(6) if m else '?'))
        if m and ok:
            print('INFO  FINDING the game rule on the rig rows: %s of %s rows drawn, pass_unknown %s' % (m.group(5), m.group(2), by.get('pass_unknown', 0)))
    if comparable_total and 'menu' in caps:   # the open finding: the rows carry no scissor (Quad2D layout gap), so a game-clipped list is drawn whole
        pd = os.path.join(work, 'noscissor')
        os.makedirs(pd, exist_ok=True)
        f = (QUICK['menu'] if a.quick else CAPS['menu'][1])[0]
        rc, text = run([host_exe, '--no-scissor', '--mode', 'host', '--flat', '--no-curve', '--scene', os.path.join(caps['menu'], 'scene'), '--capture', caps['menu'], '--rows-dir', os.path.join(work, 'rows_menu'), '--game', game, '--frames', str(f), '--out', pd])
        if rc == 0:
            mx, over, _ = diff(load(os.path.join(pd, 'host_%d.png' % f)), load(os.path.join(work, 'ref_menu_%d' % f, 'picture.png')))
            print('INFO  FINDING rows carry no scissor: menu frame %d drawn unclipped differs from the stream in %d pixels (max %d); the game scissors 170 of its 209 rows to the menu panel (q2d::Context::scissor is the seam, a Quad2D layout + tap change fills it)' % (f, over, mx))
    # the sheet: reference | host | |diff| x 8, a few comparable frames
    rows = [s for s in sheet if s[4]][:6] or sheet[:4]
    if rows:
        w, h = 320, 180
        out = Image.new('RGB', (w * 3, h * len(rows)))
        for i, (cap, f, ref, host, comp) in enumerate(rows):
            d = (np.abs(host - ref).max(axis=2) * 8).clip(0, 255).astype(np.uint8) if host.shape == ref.shape else np.zeros(ref.shape[:2], np.uint8)
            for j, im in enumerate((Image.fromarray(ref.astype(np.uint8)), Image.fromarray(host.astype(np.uint8)), Image.fromarray(d).convert('RGB'))):
                out.paste(im.resize((w, h)), (j * w, i * h))
        sp = os.path.join(work, 'sheet.png')
        out.save(sp)
        print('INFO  sheet (rows arm | host F | |diff| x 8): ' + sp)
    print('host rig 2d: %d failed' % rep.fails)
    return 1 if rep.fails else 0


if __name__ == '__main__':
    sys.exit(main())

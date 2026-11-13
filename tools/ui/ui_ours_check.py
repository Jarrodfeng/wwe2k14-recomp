#!/usr/bin/env python3
r"""Lane 66 offline proof (0 game runs): our UI textures (UiOurs, UI_PROMOTION_MAP.md sections 4-5, docs\renderer\UI_OURS.md) on the menu, editor and match case zips.

  py -I tools\ui\ui_ours_check.py [--root DIR] [--no-build] [--only unit|fview] [--quick]

Two halves; every line PASS / FAIL, a missing input FAILS (never skips); exit 1 on any FAIL.
UNIT (out\tools\ui_ours_test.exe: the shipped UiCore / loader / index on the player's pacs and the capture's frames, no GPU):
  1. menu and editor zips: every 2D draw's picture joined (unjoined 0), per-draw checks 0 mismatches, and for every replaced draw OUR upload's bytes equal the capture image's
     bytes (read back from textures.bin). The match zip: the HUD's art is kept and counted (the file side has none of it), nothing replaced wrongly.
  2. the plants: first_texture, icon_as_font, rb_swap, key_shift each make the unit's `checks` FAIL; names_stripped makes `claimed` FAIL (loud: 0 claimed);
     short_mips makes the synthetic multi-mip case's `synth_join` FAIL (no joined picture of the zips has more than one mip: the stream side of that case is synthetic).
FVIEW (out\renderer\dev\fview.exe through the REAL seam, --ui-ours; the GPU is shared: only the "frame ring" refusal is retried):
  3. picture: ours equals the stream's, pixel for pixel (max channel difference 0), at several frames of each zip; census: fallback 0, mismatch 0, kept only as listed
     (menu / editor: nothing kept), index error-free; an 8-frame replay with the Vulkan validation layer on: errors 0.
  4. plants through the seam (--replace-fault unchecked_NAME: the checks only count, the planted texture is drawn): the picture must MOVE from the clean arm for first_texture,
     icon_as_font, rb_swap, key_shift; with the checks on (--replace-fault NAME) the same plants keep the stream's draw (census: mismatch > 0, picture = stream's).
  5. stale_after_join (the slice's own proof): the STREAM's image bytes are replaced after the join (the reader never re-read a CPU rewrite): the stream arm's picture moves
     (--replace-fault stale_stream), OURS does not (--replace-fault stale_after_join: pixel difference 0 against the unplanted ours arm).
Pictures and logs go under the main checkout's out\s101\ui_ours\ ; nothing from the game is written elsewhere."""
import argparse
import glob
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RUNS = os.path.join('out', 's101', 'runs')
ZIPS = {   # kind -> run folder (the gate's ui_join lane uses the same three zips)
    'menu': '20261111_214249_s100_owner_boot_menu_quit',
    'editor': '20261111_223313_s101_checkB_caw_walk',
    'match': '20261111_215201_s100_owner_s89_brock',
}
FRAMES = {'menu': (5, 300, 700, 2300), 'editor': (20, 120, 250), 'match': (100, 250)}
QUICK = {'menu': (700,), 'editor': (120,), 'match': (100,)}
PLANT_FRAMES = {'menu': (700, 2300), 'editor': (120, 250)}
UNIT_PLANTS = {   # plant -> (mode, the unit line that must FAIL)
    'first_texture': ('capture', 'checks'), 'icon_as_font': ('capture', 'checks'), 'rb_swap': ('capture', 'checks'), 'key_shift': ('capture', 'checks'),
    'names_stripped': ('capture', 'claimed'), 'short_mips': ('synth', 'synth_join'),
}
# the plants a kind can show (measured): the editor has no 4-byte picture (rb_swap), the menu frames chosen draw no font000 glyphs (icon_as_font; the unit test shows it on the whole menu zip)
PICTURE_PLANTS = {'menu': ('first_texture', 'rb_swap', 'key_shift'), 'editor': ('first_texture', 'icon_as_font', 'key_shift')}


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


class Report:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        self.fails += 0 if ok else 1
        return ok


def unit_lines(text):
    return {m.group(2): (m.group(1) == 'PASS', m.group(3)) for m in re.finditer(r'^(PASS|FAIL)  (\S+)\s+(.*)$', text, re.M)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    rep = Report()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(main_co, 'out', 's101', 'ui_ours')
    cache = os.path.join(work, 'idx')
    os.makedirs(work, exist_ok=True)
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        return 1 if rep.line(False, 'input missing: the game files ' + game) else 0
    caps = {}
    for kind, run_name in ZIPS.items():
        d = os.path.join(work, 'case_' + kind)
        cap = os.path.join(d, 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            zs = glob.glob(os.path.join(main_co, RUNS, run_name, 'case_*.zip'))
            if not zs:
                rep.line(False, 'input missing: the %s case zip under %s' % (kind, os.path.join(main_co, RUNS, run_name)))
                return 1
            os.makedirs(d, exist_ok=True)
            zipfile.ZipFile(zs[0]).extractall(d)
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            rep.line(False, 'the %s zip has no capture\\images.bin' % kind)
            return 1
        caps[kind] = cap
    do_unit = args.only in ('', 'unit')
    do_fview = args.only in ('', 'fview')
    t0 = time.time()

    if do_unit:
        if not args.no_build:
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'ui', 'build_ui_ours_test.ps1')], 1800)
            if rc:
                print(text[-3000:])
                return 1 if rep.line(False, 'unit test build (exit %d)' % rc) else 0
        exe = os.path.join(root, 'out', 'tools', 'ui_ours_test.exe')
        if not os.path.isfile(exe):
            return 1 if rep.line(False, 'no ' + exe) else 0

        def unit(kind, fault='', mode='capture', step=10, extra=()):
            cmd = [exe, mode, '--game', game, '--cache', cache]
            if mode == 'capture':
                cmd += ['--capture', caps[kind], '--kind', kind, '--step', str(step)]
            cmd += list(extra)
            if fault:
                cmd += ['--fault', fault]
            rc, text = run(cmd, 1500)
            lines = unit_lines(text)
            if rc == 2 or not lines:
                print(text[-2500:])
                raise RuntimeError('unit test could not run (exit %d)' % rc)
            return lines, text

        for kind in ('menu', 'editor', 'match'):
            lines, text = unit(kind, step=10, extra=('--arena', '4') if kind == 'match' else ())   # lane 126: the Brock zip's arena (bg04); the unit fires the second-tier trigger itself
            bad = [k for k, v in lines.items() if not v[0]]
            census = [l.strip() for l in text.splitlines() if 'frames ' in l and '2D draws claimed' in l]
            rep.line(not bad and len(lines) >= 6, 'unit %s zip: %d checks, failing: %s | %s' % (kind, len(lines), ', '.join(bad) or 'none', census[-1] if census else '?'))
            if bad:
                print(text[-2500:])
        for plant, (mode, must_fail) in UNIT_PLANTS.items():
            lines, text = unit('menu', plant, mode)
            ok = must_fail in lines and not lines[must_fail][0]
            rep.line(ok, 'unit plant %s: the %s check fails (%s)' % (plant, must_fail, lines.get(must_fail, (None, 'no such line'))[1][:150]))
        # review F1: an image a GPU copy overwrites after the join must keep the stream's draw (a doctored frame carrying only the copy); the plant = the feed that forgets to erase it
        lines, _ = unit('menu', extra=('--copy-after', '1500'))
        rep.line(all(v[0] for v in lines.values()) and 'gpu_copy' in lines, 'unit gpu_copy_after_join scenario: the copied image keeps the stream\'s draw (%s)' % lines.get('gpu_copy', (0, 'no such line'))[1][:150])
        lines, _ = unit('menu', 'gpu_copy_after_join', extra=('--copy-after', '1500'))
        rep.line('gpu_copy' in lines and not lines['gpu_copy'][0], 'unit plant gpu_copy_after_join: the gpu_copy check fails (%s)' % lines.get('gpu_copy', (0, 'no such line'))[1][:150])
        lines, _ = unit('menu', '', 'synth')
        rep.line(all(v[0] for v in lines.values()) and 'synth_join' in lines, 'unit synth (no plant): the first 3 of 9 mips of a file picture joined and replaced (the mip-prefix join)')

    if do_fview:
        if not args.no_build:
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
            if rc:
                print(text[-3000:])
                return 1 if rep.line(False, 'fview build (exit %d)' % rc) else 0
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
        fexe = _fe.find_or_exit(root=root, kind='dev')
        if not os.path.isfile(fexe):
            return 1 if rep.line(False, 'no ' + fexe) else 0
        try:
            import numpy as np
            from PIL import Image
        except Exception as e:
            return 1 if rep.line(False, 'numpy / Pillow are needed to compare pictures: %s' % e) else 0
        env = {'WWE2K14_UI_INDEX_CACHE': cache}
        cache_pics = {}

        def fview(tag, kind, frame, extra):
            key = (tag, kind, frame)
            if key in cache_pics:
                return cache_pics[key]
            d = os.path.join(work, 'arm_%s_%s_%d' % (tag, kind, frame))
            cmd = [fexe, '--capture', caps[kind], '--frame', str(frame), '--judge-out', d] + extra
            for attempt in range(10):   # the GPU is shared: only the frame-ring refusal is retried
                rc, text = run(cmd, 900, env)
                if 'Not enough graphics memory for the frame ring' in text:
                    print('      (shared GPU: frame ring refused, attempt %d; waiting)' % (attempt + 1), flush=True)
                    time.sleep(20)
                    continue
                break
            png = os.path.join(d, 'picture.png')
            if rc or not os.path.isfile(png):
                print(text[-2500:])
                raise RuntimeError('fview arm %s %s frame %d failed (exit %d)' % (tag, kind, frame, rc))
            img = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
            cache_pics[key] = (img, text)
            return cache_pics[key]

        def diff(a, b):
            d = np.abs(a - b).max(axis=2)
            return int(d.max()), int((d > 0).sum())

        ui = ['--ui-ours', '--game', game]
        frames = QUICK if args.quick else FRAMES
        census_re = re.compile(r'^\[rdr\]\[ui\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)', re.M)
        hook_re = re.compile(r'^\[fview\] \[rdr\]\[ui\] (.*)$', re.M)

        def census(text):
            m = census_re.search(text)
            h = hook_re.search(text)
            return (tuple(map(int, m.groups())) if m else None), (h.group(1) if h else '')

        for kind in ('menu', 'editor', 'match'):
            worst, shown = 0, []
            for f in frames[kind]:
                s, _ = fview('stream', kind, f, [])
                o, text = fview('clean', kind, f, ui)
                mx, npx = diff(o, s)
                c, hook = census(text)
                ok = mx == 0 and c is not None and c[1] == 0 and c[6] == 0 and 'ERRORS' not in hook
                pm = re.search(r'index (\d+) textures in (\d+) pacs', hook)
                if kind != 'match':   # review F3 (lane 126): menu / editor pin the first tier exactly (3605 textures in 10 pacs) and never start the second tier
                    ok = ok and pm is not None and int(pm.group(1)) == 3605 and int(pm.group(2)) == 10 and 'deep (bg' not in hook
                    ok = ok and c is not None and c[2] > 0 and c[3] == 0 and c[5] == 0   # replaced > 0, kept 0, unjoined 0
                else:   # the Brock zip starts the second tier once: bgEtc + its arena's pac (the pac count is computed from the disk), done, nothing kept
                    dm = re.search(r'deep \(bg lightmaps\) done: (\d+) pacs walked \(arena (\d+)\), 0 failed, 0 pending', hook)
                    want = 10 + 1 + (1 if dm and os.path.isfile(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % int(dm.group(2)))) else 0)
                    ok = ok and dm is not None and pm is not None and int(pm.group(2)) == want and int(dm.group(1)) == want - 10
                    ok = ok and c is not None and c[2] > 0 and c[3] <= 12 and c[5] <= 12   # lane 126: the census is cumulative over the replayed frames: the page sprites of the first frames (before the scene record names the arena, measured 9) stay unjoined; the final frame's draws all join (ui_inmatch_check.py reads that from the ops table)
                rep.line(ok, 'fview %s frame %d: ours vs stream max channel diff %d (%d pixels); fallback %s, replaced %s, kept %s (unjoined %s, mismatch %s) | %s' % (
                    kind, f, mx, npx, c[1] if c else '?', c[2] if c else '?', c[3] if c else '?', c[5] if c else '?', c[6] if c else '?', hook[:210]))
                worst = max(worst, mx)
        for kind in ('menu', 'editor'):
            rc, vt = run([fexe, '--capture', caps[kind], '--frames', '8', '--validation', '1', '--replay'] + ui, 900, env)
            rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, 'fview %s: 8-frame replay with the validation layer on, errors 0 (exit %d)' % (kind, rc))
        for kind in ('menu', 'editor'):
            for plant in PICTURE_PLANTS[kind]:
                moved, shown, mism = [], [], []
                for f in PLANT_FRAMES[kind][:1 if args.quick else 2]:
                    s, _ = fview('stream', kind, f, [])
                    clean, _ = fview('clean', kind, f, ui)
                    raw, rtext = fview('raw_' + plant, kind, f, ui + ['--replace-fault', 'unchecked_' + plant])
                    chk, ctext = fview('chk_' + plant, kind, f, ui + ['--replace-fault', plant])
                    moved.append(diff(raw, clean)[1])
                    cm = census(ctext)[0]
                    mism.append((cm[6] if cm else -1, diff(chk, s)[1]))
                ok = max(moved) > 0 and all(m > 0 and p == 0 for m, p in mism)
                rep.line(ok, 'fview %s plant %s: unchecked picture moves from the clean arm by %s pixels; checked: mismatch / pixels from the stream %s' % (kind, plant, moved, mism))
        for kind in ('menu', 'editor'):
            f = PLANT_FRAMES[kind][-1]
            s, _ = fview('stream', kind, f, [])
            clean, _ = fview('clean', kind, f, ui)
            stale_stream, _ = fview('stale_stream', kind, f, ui + ['--replace-fault', 'stale_stream'])
            stale_ours, stext = fview('stale_ours', kind, f, ui + ['--replace-fault', 'stale_after_join'])
            ds = diff(stale_stream, s)
            do = diff(stale_ours, clean)
            c, hook = census(stext)
            rep.line(ds[1] > 0 and do[1] == 0 and c is not None and c[2] > 0, 'fview %s stale_after_join (frame %d): the stream arm moves by %d pixels, ours by %d (bar 0); replaced %s' % (kind, f, ds[1], do[1], c[2] if c else '?'))
        s, _ = fview('stream', 'menu', FRAMES['menu'][-1], [])
        none_arm, ntext = fview('names_stripped', 'menu', FRAMES['menu'][-1], ui + ['--replace-fault', 'names_stripped'])
        c, _ = census(ntext)
        rep.line(c is not None and c[2] == 0 and diff(none_arm, s)[1] == 0, 'fview menu plant names_stripped: replaced %s (the hook claims nothing: loud, the picture is the stream\'s)' % (c[2] if c else '?'))

    print('%s  lane 66 ui_ours: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)

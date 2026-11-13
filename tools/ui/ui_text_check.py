#!/usr/bin/env python3
r"""Lane 89 (UT) offline proof (0 game runs): the game's text laid out by us (UiTextOurs, docs\renderer\UI_TEXT_OURS.md, design UI_PROMOTION_MAP.md sections 6-7) on the menu and
editor case zips that carry the run strings (names.h section 69).

  py -I tools\ui\ui_text_check.py [--root DIR] [--no-build] [--only unit|fview] [--quick]

Two halves; every line PASS / FAIL, a missing input FAILS (never skips); exit 1 on any FAIL.
UNIT (out\tools\ui_text_test.exe: the SHIPPED hook, the player's font files, the zips' frames driven the way the binder drives them, no GPU):
  1. menu and editor zips: every glyph draw accounted for (ours / icon glyph / no run), none refused; every replaced quad's four corners within 0.005 px of the stream's, uv rect within
     1e-6, every other byte equal (checked by code in the test, not the hook); the editor zip shows the narrower-quad factor (handled, counted).
  2. the plants (R = a real past bug): advance_c2 (R: the first pen rule, trim 2 for every font), kern_off, wrong_font, run_shift. Each, as written (the hook's guard on): the `kept`
     check FAILs (the hook refused the segments it could not match). Each as `unchecked_NAME` (the guard off, the planted layout drawn): the `quads` check FAILs.
  3. the twin of the `runs` check: a capture without run strings (lane 66's older menu zip) must FAIL it (loud: 0 runs is not "0 problems").
FVIEW (out\renderer\dev\fview.exe through the REAL seam, --ui-text-ours; the GPU is shared: only the frame-ring refusal is retried):
  4. picture: ours vs the stream at several frames of each zip (the bar: max channel difference <= 2 and <= 0.05 % of the pixels differ; measured 0), census: fallback 0, refused 0,
     glyph draws ours > 0; 8-frame replay with the Vulkan validation layer on: errors 0.
  5. plants through the seam: unchecked_run_shift / unchecked_kern_off (editor also unchecked_advance_c2) move the picture (> 200 pixels) from the clean ours arm; the checked plants keep the stream's draws (refused > 0,
     picture = the stream's within the bar).
Pictures and logs go under the main checkout's out\s101\ui_text\ ; nothing from the game is written elsewhere."""
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
ZIPS = {   # kind -> run folder (labelled zips: built after the label fix; section 69 present)
    'menu': '20261112_122344_s100_owner_boot_menu_quit',
    'editor': '20261112_122353_s101_checkB_caw_walk',
}
OLD_MENU = os.path.join('out', 's101', 'ui_ours', 'case_menu', 'capture')   # lane 66's zip, built before the run strings: no section 69
UNIT_STEP = {'menu': 3, 'editor': 3}
FRAMES = {'menu': (2100, 3240, 3328), 'editor': (20, 120, 250)}
QUICK = {'menu': (3240,), 'editor': (120,)}
PLANTS = ('advance_c2', 'kern_off', 'wrong_font', 'run_shift')
# the plants a kind can show (measured): the menu zip is font000 only (trim 2 already: advance_c2 changes nothing there), font002 is in the editor
PIC_PLANTS = {'menu': ('run_shift', 'kern_off'), 'editor': ('run_shift', 'advance_c2', 'kern_off')}
MAX_DIFF, MAX_FRACTION = 2, 0.0005


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
    work = os.path.join(main_co, 'out', 's101', 'ui_text')
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
                rep.line(False, 'input missing: the %s case zip under %s (the run folders age out of out\\ after 24 h: extract the zip to %s once and keep it)' % (kind, os.path.join(main_co, RUNS, run_name), d))
                return 1
            os.makedirs(d, exist_ok=True)
            zipfile.ZipFile(zs[0]).extractall(d)
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            rep.line(False, 'the %s zip has no capture\\images.bin' % kind)
            return 1
        caps[kind] = cap
    old_menu = os.path.join(main_co, OLD_MENU)
    do_unit = args.only in ('', 'unit')
    do_fview = args.only in ('', 'fview')
    t0 = time.time()

    if do_unit:
        if not args.no_build:
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'ui', 'build_ui_text_test.ps1')], 1800)
            if rc:
                print(text[-3000:])
                return 1 if rep.line(False, 'unit test build (exit %d)' % rc) else 0
        exe = os.path.join(root, 'out', 'tools', 'ui_text_test.exe')
        if not os.path.isfile(exe):
            return 1 if rep.line(False, 'no ' + exe) else 0

        def unit(kind, fault='', capture=None):
            cmd = [exe, 'capture', '--game', game, '--capture', capture or caps[kind], '--kind', kind, '--step', str(UNIT_STEP[kind])]
            if fault:
                cmd += ['--fault', fault]
            rc, text = run(cmd, 1500)
            lines = unit_lines(text)
            if rc == 2 or not lines:
                print(text[-2500:])
                raise RuntimeError('unit test could not run (exit %d)' % rc)
            return lines, text

        for kind in ('menu', 'editor'):
            lines, text = unit(kind)
            bad = [k for k, v in lines.items() if not v[0]]
            census = [l.strip() for l in text.splitlines() if 'glyph draws claimed' in l]
            rep.line(not bad and len(lines) >= 8, 'unit %s zip: %d checks, failing: %s | %s' % (kind, len(lines), ', '.join(bad) or 'none', (census[-1] if census else '?')[:330]))
            if bad:
                print(text[-2500:])
            rep.line(lines.get('quads', (False, ''))[0], 'unit %s quads: %s' % (kind, lines.get('quads', (0, 'no such line'))[1][:260]))
        for plant in PLANTS:
            lines, _ = unit('editor', plant)
            rep.line('kept' in lines and not lines['kept'][0], 'unit plant %s (guard on): the kept check fails, the hook refuses what it cannot match (%s)' % (plant, lines.get('kept', (0, 'no such line'))[1][-120:]))
            lines, _ = unit('editor', 'unchecked_' + plant)
            rep.line('quads' in lines and not lines['quads'][0], 'unit plant unchecked_%s (guard off, planted layout drawn): the quads check fails (%s)' % (plant, lines.get('quads', (0, 'no such line'))[1][:200]))
        if not os.path.isfile(os.path.join(old_menu, 'images.bin')):
            rep.line(False, 'input missing: lane 66\'s older menu zip (no run strings) at ' + old_menu)
        else:
            lines, _ = unit('menu', capture=old_menu)
            rep.line('runs' in lines and not lines['runs'][0], 'unit twin: a capture without run strings fails the runs check (%s)' % lines.get('runs', (0, 'no such line'))[1][:120])

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
        cache_pics = {}

        def fview(tag, kind, frame, extra):
            key = (tag, kind, frame)
            if key in cache_pics:
                return cache_pics[key]
            d = os.path.join(work, 'arm_%s_%s_%d' % (tag, kind, frame))
            cmd = [fexe, '--capture', caps[kind], '--frame', str(frame), '--judge-out', d] + extra
            for attempt in range(10):   # the GPU is shared: only the frame-ring refusal is retried
                rc, text = run(cmd, 900)
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
            return int(d.max()), int((d > 0).sum()), int((d > 0).sum()) / d.size

        ours = ['--ui-text-ours', '--game', game]
        frames = QUICK if args.quick else FRAMES
        binder_re = re.compile(r'^\[rdr\]\[ui_text\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+)', re.M)
        hook_re = re.compile(r'^\[fview\] \[rdr\]\[ui_text\] glyph draws claimed (\d+): ours (\d+), kept \(icon (\d+), no run (\d+), truncated (\d+), count (\d+), font (\d+), geometry (\d+), layout (\d+)\)'
                             r'; runs (\d+) \(ours (\d+), with icons (\d+)\), segments (\d+) \(narrower quads (\d+)\)', re.M)

        def census(text):
            b = binder_re.search(text)
            h = hook_re.search(text)
            return (tuple(map(int, b.groups())) if b else None), (tuple(map(int, h.groups())) if h else None)

        def within(d):
            return d[0] <= MAX_DIFF and d[2] <= MAX_FRACTION

        for kind in ('menu', 'editor'):
            for f in frames[kind]:
                s, _ = fview('stream', kind, f, [])
                o, text = fview('clean', kind, f, ours)
                d = diff(o, s)
                b, h = census(text)
                ok = within(d) and b is not None and h is not None and b[1] == 0 and b[2] > 0 and h[1] > 0 and sum(h[4:9]) == 0 and h[0] == h[1] + h[2] + h[3] + sum(h[4:9])
                rep.line(ok, 'fview %s frame %d: ours vs stream max channel diff %d (%d pixels, %.4f %%); fallback %s, replaced %s; glyph draws %s: ours %s, icon %s, refused %s, narrower segments %s' % (
                    kind, f, d[0], d[1], 100 * d[2], b[1] if b else '?', b[2] if b else '?', h[0] if h else '?', h[1] if h else '?', h[2] if h else '?', sum(h[4:9]) if h else '?', h[13] if h else '?'))
        for kind, frame in (('menu', FRAMES['menu'][1]), ('editor', FRAMES['editor'][1])):
            rc, vt = run([fexe, '--capture', caps[kind], '--frame', str(frame), '--validation', '1', '--judge-out', os.path.join(work, 'arm_val_' + kind)] + ours, 900)
            rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, 'fview %s frame %d with the Vulkan validation layer on, errors 0 (exit %d)' % (kind, frame, rc))
        for kind in ('menu', 'editor'):
            f = FRAMES[kind][1]
            s, _ = fview('stream', kind, f, [])
            clean, _ = fview('clean', kind, f, ours)
            for plant in PIC_PLANTS[kind]:
                raw, rtext = fview('raw_' + plant, kind, f, ours + ['--replace-fault', 'unchecked_' + plant])
                chk, ctext = fview('chk_' + plant, kind, f, ours + ['--replace-fault', plant])
                moved = diff(raw, clean)
                kept = diff(chk, s)
                _, hh = census(ctext)
                refused = sum(hh[4:9]) if hh else -1
                rep.line(moved[1] > 200 and refused > 0 and within(kept), 'fview %s plant %s: unchecked picture moves from the clean arm by %d pixels; checked: %d glyph draws refused, picture vs the stream max diff %d (%d pixels)' % (
                    kind, plant, moved[1], refused, kept[0], kept[1]))

    print('%s  lane 89 ui_text_ours: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)

#!/usr/bin/env python3
"""S102 lane G13 offline check (0 game runs, 0 Surface runs, no window): the draw heights 576 / 600 / 648 for 720p (NATIVE only).

  py -I tools\\g13\\g13_check.py [--root DIR] [--fview]

1. unit: tools\\g13\\g13_unit.cpp against the real src\\gfx_settings.h; planted twin headers must FAIL it (600 accepted for 1440p; 600 missing from the list;
   600 listed under resolution 1; the 1440P fallback by position).
2. scale: the renderer's ceil(float(v) * (float(h) / 720)) equals the exact ceil(v * h / 720) for every extent 1..2048 at 540 / 576 / 600 / 648; PLANT: a truncated
   scale (0.8333) must differ.
3. wiring: reader.h turns any draw height 360..2880 into render_scale = h / 720 (no height list), the launcher's list is built from kResolutions, bench.ps1's
   ValidateSet names the profiles, the profiles are auto.json + draw_height, profile_check.py passes; each link removed in a copy must FAIL.
4. --fview: fview on the Bryan entrance case at each height with the 1280x720 display present (--present-size 1280x720), validation 1: the exit code is 0, the
   log's `validation errors` is 0, the picture size is ceil(1280 h / 720) x h, a sheet under out\\s102\\sheets\\g13 (full pictures + 1:1 crops).
   PLANT: a run at 600's scale checked against 648's size must FAIL.
Exit 0 only when everything holds."""
import argparse
import math
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
HEIGHTS = [540, 576, 600, 648]
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


sys.path.insert(0, os.path.join(ROOT, 'tools', 'split'))
from read_unit import expand_bytes  # a split file is read with its parts expanded (tools/split/read_unit.py)


def read(root, rel):
    return expand_bytes(os.path.join(root, rel)).decode('utf-8', errors='replace').replace('\r\n', '\n')


def unit(root, work, tag, gfx_text):
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'gfx_applied.h'), 'w', newline='', encoding='utf-8') as f:  # a quote include looks beside the includer first: copy it so the twin is the one read
        f.write(read(root, 'src/gfx_applied.h'))
    with open(os.path.join(d, 'gfx_settings.h'), 'w', newline='', encoding='utf-8') as f:
        f.write(gfx_text)
    exe = os.path.join(d, 't.exe')
    c = subprocess.run([CLANG, '-std=c++20', '-w', '-I', d, '-I', os.path.join(root, 'src'), os.path.join(root, 'tools/g13/g13_unit.cpp'), '-o', exe],
                       capture_output=True, text=True)
    if c.returncode:
        print(c.stdout, c.stderr)
        return None
    p = subprocess.run([exe], capture_output=True, text=True)
    if p.returncode:
        print(p.stdout.strip())
    return p.returncode == 0


def scale_ok(heights, scale_of):
    bad = []
    for h in heights:
        s = scale_of(h)
        for v in range(1, 2049):
            if int(np.ceil(np.float32(v) * s)) != -(-v * h // 720):
                bad.append((h, v))
    return bad


def plant_line(what, text, old, new, checker):
    """A real plant: a meaningful change to the source text; the checker must see it. DID NOT APPLY is its own failure."""
    twin = text.replace(old, new, 1)
    if twin == text:
        line(False, 'plant (%s): DID NOT APPLY' % what)
        return
    line(not checker(twin), 'plant: %s is caught' % what)


def wiring(root, work):
    reader = read(root, 'src/renderer_adapter/reader.h')
    launcher = read(root, 'src/launcher/launcher_main.cpp')
    bench = read(root, 'tools/surface/bench.ps1')
    renderer = read(root, 'src/renderer/renderer.cpp')
    checks = [
        ('reader.h: any height 360..2880 below the whole size is render_scale = h / 720', reader,
         r'draw_height >= 360 && draw_height <= 2880 && float\(draw_height\) < options\.render_scale \* 720\.0f\) options\.render_scale = float\(draw_height\) / 720\.0f',
         'float(draw_height) / 720.0f', 'float(draw_height) / 700.0f'),
        ('launcher: the RESOLUTION list is built from kResolutions (native_only entries only with NATIVE)', launcher,
         r'if \(native \|\| !wwe2k14::gfxs::kResolutions\[i\]\.native_only\) entries\.push_back\(i\)',
         'if (native || !wwe2k14::gfxs::kResolutions[i].native_only) entries', 'if (true || !wwe2k14::gfxs::kResolutions[i].native_only) entries'),
        ("launcher: a pick writes the entry's resolution and draw height", launcher, r't\.resolution = n\.resolution, t\.draw_height = n\.draw_height',
         't.draw_height = n.draw_height', 't.draw_height = 0'),
        ('renderer.cpp:663 ScaledExtent is ceil(float(value) * scale)', renderer, r'return uint32_t\(std::ceil\(float\(value\) \* scale\)\);',
         'return uint32_t(std::ceil(float(value) * scale));', 'return uint32_t(std::floor(float(value) * scale));'),
        ('renderer.cpp:2914 a copy origin is the truncation int32_t(float(value) * factor)', renderer, r'return int32_t\(float\(value\) \* factor\);',
         'return int32_t(float(value) * factor);', 'return int32_t(std::round(float(value) * factor));'),
    ] + [('bench.ps1 ValidateSet names auto_dh%d' % h, bench, r"'auto_dh%d'" % h, "'auto_dh%d'" % h, "'auto_dhX%d'" % h) for h in HEIGHTS]
    for what, text, pat, old, new in checks:
        line(re.search(pat, text) is not None, 'wiring: ' + what)
        plant_line(what, text, old, new, lambda t, pat=pat: re.search(pat, t) is not None)
    auto = read(root, 'tools/surface/profiles/auto.json')
    for h in HEIGHTS:
        p = os.path.join(root, 'tools/surface/profiles/auto_dh%d.json' % h)
        want = auto.replace('"draw_height": 0', '"draw_height": %d' % h)
        have = read(root, 'tools/surface/profiles/auto_dh%d.json' % h) if os.path.isfile(p) else None
        line(have == want and want != auto, 'profile auto_dh%d.json = auto.json with draw_height %d' % (h, h))


GFX_EXE = 'out/tools/gfx_applied.exe'


def applied_of(root, path):
    exe = os.path.join(root, GFX_EXE)
    if not os.path.isfile(exe):
        return None
    r = subprocess.run([exe, '--settings', path, '--panel', '2736x1824', '--gpu', 'integrated', '--gpu-name', 'Intel(R) UHD Graphics 620'], capture_output=True, text=True)
    m = re.search(r'tier=(\w+) .*? internal=(\d+)x(\d+) ', r.stdout)
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else None


def profile_ok(root, path, h):
    got = applied_of(root, path)
    return got == ('LOW', -(-1280 * h // 720), h), got


def profiles_applied(root, work):
    """F4: each auto_dh*.json through the game's own reader: tier LOW and internal = ceil(1280 h / 720) x h."""
    for h in HEIGHTS:
        ok, got = profile_ok(root, os.path.join(root, 'tools/surface/profiles/auto_dh%d.json' % h), h)
        line(ok, 'gfx_applied auto_dh%d.json: tier / internal %s (want LOW %dx%d)' % (h, got, -(-1280 * h // 720), h))
    auto = read(root, 'tools/surface/profiles/auto.json')
    bad = os.path.join(work, 'auto_dh601.json')
    with open(bad, 'w', newline='') as f:
        f.write(auto.replace('"draw_height": 0', '"draw_height": 601'))
    ok, got = profile_ok(root, bad, 600)
    line(not ok, 'PLANT: a profile with draw_height 601 (a height nobody lists) is caught as 600 (%s)' % (got,))


def res_entries(gfx_text):
    block = gfx_text[gfx_text.index('kResolutions[] = {'):gfx_text.index('kResolutionCount')]
    return [(m.group(1), int(m.group(2)), int(m.group(3))) for m in re.finditer(r'\{"(\w+)", (\d+), (\d+), (?:true|false)(?:, (?:true|false))?\}', block)]


def menu_lint_ok(menu_text, entries):
    texts = {int(m.group(1)): m.group(2) for m in re.finditer(r'\{kTextVal2 \+ (\d+), "([^"]+)"', menu_text)}
    if len(texts) != len(entries) or any(texts.get(i) != e[0] for i, e in enumerate(entries)):
        return False
    m = re.search(r'g_res_order\[8\] = \{(\d+), (\d+), (\d+)\}', menu_text)
    return m is not None and [entries[int(x)][0] for x in m.groups()] == ['720P', '1440P', '4K']


def menu_lint(root):
    """F2: the in-game RESOLUTION texts follow kResolutions by index; the g_res_order defaults name 720P / 1440P / 4K."""
    entries = res_entries(read(root, 'src/gfx_settings.h'))
    menu = read(root, 'src/gfx_menu.h')
    line(len(entries) >= 10 and menu_lint_ok(menu, entries), 'lint: gfx_menu.h kTextVal2 + N texts == kResolutions[N].name (%d entries), defaults 720P / 1440P / 4K' % len(entries))
    swapped = menu.replace('{kTextVal2 + 4, "600P"', '{kTextVal2 + 4, "@@"').replace('{kTextVal2 + 5, "648P"', '{kTextVal2 + 5, "600P"').replace('{kTextVal2 + 4, "@@"', '{kTextVal2 + 4, "648P"')
    line(swapped != menu and not menu_lint_ok(swapped, entries), 'PLANT: the 600P and 648P text lines swapped is caught')
    plant_line('g_res_order default pointing at 600P', menu, 'g_res_order[8] = {6, 8, 9}', 'g_res_order[8] = {4, 8, 9}', lambda t: menu_lint_ok(t, entries))


def width_of(expr_mul, expr_add, expr_div, h):
    return (h * expr_mul + expr_add) // expr_div


def width_lint_ok(launcher_text, applied_text, entries):
    a = re.search(r'\(e\.draw_height \* (\d+) \+ (\d+)\) / (\d+),', launcher_text)
    b = re.search(r'return \{\(dh \* (\d+) \+ (\d+)\) / (\d+), dh\}', applied_text)
    if not a or not b:
        return False
    for _, _, h in entries:
        if h and width_of(*map(int, a.groups()), h) != width_of(*map(int, b.groups()), h):
            return False
        if h and width_of(*map(int, b.groups()), h) != -(-1280 * h // 720):
            return False
    return True


def width_lint(root):
    """F5: the launcher note's width (what it says the game draws) and the applied line's InternalSize agree with the renderer's ceil(1280 h / 720) for every entry."""
    entries = res_entries(read(root, 'src/gfx_settings.h'))
    launcher = read(root, 'src/launcher/launcher_main.cpp')
    applied = read(root, 'src/gfx_applied.h')
    line(width_lint_ok(launcher, applied, entries), 'lint: the launcher note width formula == InternalSize == ceil(1280 h / 720) for every kResolutions entry')
    plant_line('a floored launcher width (h * 16) / 9 (1066 for 600p)', launcher, '(e.draw_height * 1280 + 719) / 720,', '(e.draw_height * 16 + 0) / 9,',
               lambda t: width_lint_ok(t, applied, entries))


def fview_arms(root, a_scale_for, sheet_dir):
    common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    main_root = os.path.dirname(common)
    z = os.path.join(main_root, 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip')
    game = os.path.join(main_root, 'game_data/extracted')
    fv = os.path.join(root, 'out/renderer/dev/fview.exe')
    for p in (z, game, fv):
        if not os.path.exists(p):
            line(False, 'input missing: ' + p)
            return
    os.makedirs(sheet_dir, exist_ok=True)

    def run(h, effect):
        shot = os.path.join(sheet_dir, 'dh%d_%s.png' % (h, effect))
        scale = '%.9g' % np.float32(np.float32(h) / np.float32(720.0)) if h != 720 else '1'
        cmd = [fv, '--case', z, '--replay', '--frames', '40', '--validation', '1', '--crowd-ours', '--crowd-cards', '--game', game, '--scale', scale,
               '--present-size', '1280x720', '--present-shot', shot]
        if effect == 'sgsr':
            cmd += ['--present-effect', 'sgsr']
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=900)
        return shot, p.returncode, (p.stdout or '') + (p.stderr or '')

    def verdict(h, rc, text, effect):
        m = re.search(r'validation errors (\d+)', text)
        v = int(m.group(1)) if m else -1
        pm = re.search(r'picture=(\d+)x(\d+) sgsr=(\d)', text)
        want = (-(-1280 * h // 720), h)
        size_ok = pm is not None and (int(pm.group(1)), int(pm.group(2))) == want
        sg_ok = pm is not None and (pm.group(3) == '1') == (effect == 'sgsr' and h != 720)
        return rc == 0 and v == 0 and size_ok and sg_ok, 'exit %d, validation errors %d, picture %s (want %dx%d), sgsr %s' % (
            rc, v, '%sx%s' % (pm.group(1), pm.group(2)) if pm else '?', want[0], want[1], pm.group(3) if pm else '?')

    shots = {}
    for h in HEIGHTS + [720]:
        for effect in ('bilinear', 'sgsr'):
            shot, rc, text = run(h, effect)
            ok, what = verdict(h, rc, text, effect)
            line(ok, 'fview Bryan %dp -> 1280x720 present (%s): %s' % (h, effect, what))
            shots[(h, effect)] = shot
            if h == 600 and effect == 'bilinear':  # the planted twin: this run checked against 648's size must FAIL
                ok2, what2 = verdict(648, rc, text, effect)
                line(not ok2, 'PLANT: the 600 run checked as 648 is caught (%s)' % what2)
    # The sheet: full pictures (bilinear, what the bench's LOW draws), then 1:1 crops of the face / jacket and the name plate, every panel labelled.
    heights = [540, 600, 648, 720]
    crops = [('face / hair', (560, 110, 880, 350)), ('name plate', (500, 590, 820, 670))]
    for eff in ('bilinear', 'sgsr'):
        panel_w = 320
        sheet = Image.new('RGB', (len(heights) * (panel_w + 6), 24 + 240 + 6 + 80 + 6 + 2 * 22), (20, 20, 20))
        dr = ImageDraw.Draw(sheet)
        for i, h in enumerate(heights):
            im = Image.open(shots[(h, eff)]).convert('RGB')
            x = i * (panel_w + 6)
            dr.text((x + 4, 6), '%dp (%s) 1:1 crops of 1280x720' % (h, eff), fill=(255, 255, 255))
            y = 24
            for name, box in crops:
                c = im.crop(box)
                sheet.paste(c, (x, y))
                y += c.height + 6
        sheet.save(os.path.join(sheet_dir, 'sheet_crops_%s.png' % eff))
    full = Image.new('RGB', (2 * 646, 2 * 366), (20, 20, 20))
    dr = ImageDraw.Draw(full)
    for i, h in enumerate(heights):
        im = Image.open(shots[(h, 'bilinear')]).convert('RGB').resize((640, 360), Image.LANCZOS)
        full.paste(im, ((i % 2) * 646, (i // 2) * 366))
        dr.text(((i % 2) * 646 + 6, (i // 2) * 366 + 4), '%dp drawn, present 1280x720 (bilinear)' % h, fill=(255, 255, 0))
    full.save(os.path.join(sheet_dir, 'sheet_full.png'))
    for n in ('sheet_crops_bilinear.png', 'sheet_crops_sgsr.png', 'sheet_full.png'):
        line(os.path.getsize(os.path.join(sheet_dir, n)) > 10000, 'sheet written: ' + os.path.join(sheet_dir, n))
    keep = os.path.join(sheet_dir, 'KEEP')
    with open(keep, 'w') as f:
        f.write('S102 lane G13: sheets of Bryan\'s entrance drawn at 540 / 576 / 600 / 648 / 720 for Jarrod to judge (a lower internal resolution stays on the branch until he sees a sheet).\nuntil 2026-11-21\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--fview', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    work = os.path.join(root, 'out', 's102', 'g13', 'check')
    os.makedirs(work, exist_ok=True)
    gfx = read(root, 'src/gfx_settings.h')
    line(unit(root, work, 'real', gfx) is True, 'unit: ValidDrawHeight / ResolutionIndex / list / flags / applied line hold on the real gfx_settings.h')
    plants = [
        ('600 accepted for 1440p (the resolution test dropped)', 'e.draw_height == draw_height && e.resolution == resolution', 'e.draw_height == draw_height'),
        ('600P missing from the list', '{"600P", 0, 600, true, false},  ', ''),
        ('600P listed under resolution 1', '{"600P", 0, 600, true, false}', '{"600P", 1, 600, true, false}'),
        ('a run without NATIVE keeps showing the draw height', '  if (!native) draw_height = 0;', '  if (!native) draw_height = draw_height;'),
        ('720P listed as native-only', '{"720P", 0, 0, false}', '{"720P", 0, 0, true}'),
        ('the third loop (1440P for a resolution nobody lists) replaced by return 4',
         "  for (int i = 0; i < kResolutionCount; ++i)  // S102 G13: by name, not a position (the list grew)\n    if (kResolutions[i].resolution == 1 && !kResolutions[i].draw_height) return i;  // 1440p\n  return 0;\n",
         '  return 4;\n'),
        ('the second loop (the whole size for an unlisted height) deleted',
         '  for (int i = 0; i < kResolutionCount; ++i)\n    if (kResolutions[i].resolution == resolution && !kResolutions[i].draw_height) return i;\n', ''),
    ]
    for name, old, new in plants:
        twin = gfx.replace(old, new, 1)
        if twin == gfx:
            line(False, 'unit twin (%s): DID NOT APPLY' % name)
            continue
        line(unit(root, work, 'twin', twin) is False, 'unit twin: %s fails the test' % name)
    bad = scale_ok(HEIGHTS, lambda h: np.float32(h) / np.float32(720.0))
    line(not bad, 'scale: ceil(float(v) * (float(h) / 720)) == exact ceil(v h / 720) for v in 1..2048 at %s' % HEIGHTS)
    badp = scale_ok([600], lambda h: np.float32(0.8334))
    line(len(badp) > 0, 'scale twin: a rounded-up 0.8334 scale gives %d wrong extents' % len(badp))
    bad_t = 0
    for h in HEIGHTS:  # the copy origin truncates: int32_t(float(v) * scale) == floor(v h / 720)
        sc = np.float32(h) / np.float32(720.0)
        bad_t += sum(1 for v in range(0, 2049) if int(np.float32(v) * sc) != v * h // 720)
    line(bad_t == 0, 'scale: the copy origin int32_t(float(v) * scale) == floor(v h / 720) for v in 0..2048 at %s' % HEIGHTS)
    sc = np.float32(0.8332)
    line(sum(1 for v in range(0, 2049) if int(np.float32(v) * sc) != v * 600 // 720) > 0, 'scale twin: a rounded-down 0.8332 scale breaks the origin')
    r = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools/surface/profile_check.py')], capture_output=True, text=True, cwd=root)
    line(r.returncode == 0, 'profile_check.py passes: ' + (r.stdout.strip().splitlines() or ['(no output)'])[-1][:100])
    wiring(root, work)
    profiles_applied(root, work)
    menu_lint(root)
    width_lint(root)
    if a.fview:
        fview_arms(root, None, os.path.join(root, 'out', 's102', 'sheets', 'g13'))
    print('g13_check: %d failure(s)' % fails)
    return 1 if fails else 0


sys.exit(main())

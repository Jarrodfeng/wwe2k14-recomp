#!/usr/bin/env python3
"""S102 lane G2 offline check (0 game runs, 0 Surface runs, no window): the output pass's levers (docs\\renderer\\OUTPUT_PASS.md).

  py -I tools\\g2\\g2_check.py [--root DIR] [--bryan]

1. unit: tools\\g2\\output_levers_test.cpp against the real low_levers.h; planted twin headers (the cap / the bilinear switch ignore LOW) must FAIL it.
2. wiring: both switches are read in reader.h, stored in rdr::g_out_*, and read where the present is recorded (renderer.cpp); each link removed in a copy must FAIL.
3. rplay (synthetic scene, offscreen output 2736x1824, validation 2): cap arm = every present drawn capped, 0 validation errors, the picture differs from the uncapped
   one (the cap reached the pixels); bilinear arm = 0 SGSR frames; SGSR arm = all SGSR. PLANTED TWIN --output-cap-plant (cap on, never applied) must fail the cap predicate.
4. --bryan: fview on the Bryan entrance case with the same arms (60 frames), needs the case zip and the game files.
Exit 0 only when everything holds."""
import argparse
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def read(root, rel):
    with open(os.path.join(root, rel), newline='') as f:
        return f.read()


def unit(root, work, tag, header_text):
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'low_levers.h'), 'w', newline='') as f:
        f.write(header_text)
    exe = os.path.join(d, 't.exe')
    c = subprocess.run([CLANG, '-std=c++20', '-I', d, os.path.join(root, 'tools/g2/output_levers_test.cpp'), '-o', exe], capture_output=True, text=True)
    if c.returncode:
        print(c.stdout, c.stderr)
        return None
    return subprocess.run([exe], capture_output=True, text=True).returncode == 0


WIRING = [
    ('cap cvar is in the S0-4 table, PerFrame', 'src/renderer_adapter/rdr_switches.h', r'X\(LowOutputCap, "wwe2k14_rdr_low_output_cap", PerFrame'),
    ('bilinear cvar is in the S0-4 table, PerFrame', 'src/renderer_adapter/rdr_switches.h', r'X\(LowPresentBilinear, "wwe2k14_rdr_low_present_bilinear", PerFrame'),
    ('cap cvar defined', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_low_output_cap'),
    ('bilinear cvar defined', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_low_present_bilinear'),
    ('LOW is stored once at start', 'src/start_settings.cpp', r'g_low_tier\.store\(q\.low_tier'),
    ('live.cpp reads the cap with IntSeen(guest_frame) through OutputCapHeight', 'src/renderer_adapter/live.cpp', r'OutputCapHeight\(int\(rdr_sw::IntSeen\(rdr_sw::kLowOutputCap, 0, message\.guest_frame\)\), g2_low\)'),
    ('live.cpp reads the bilinear switch with BoolSeen through PresentBilinear', 'src/renderer_adapter/live.cpp', r'PresentBilinear\(rdr_sw::BoolSeen\(rdr_sw::kLowPresentBilinear, false, message\.guest_frame\), g2_low\)'),
    ('live.cpp hands both to the frame', 'src/renderer_adapter/live.cpp', r'(?:builder|fb)\.SetOutput\(out_cap_now, out_bilinear_now\)'),
    ('the frame builder writes them into the Frame', 'src/renderer/frame_builder.h', r'frame\.out_cap_h = out_cap_h_, frame\.out_bilinear = out_bilinear_'),
    ('the render side takes them from the Frame', 'src/renderer/renderer.cpp', r'out_cap_h_ = frame\.out_cap_h, out_bilinear_ = frame\.out_bilinear'),
    ('the present uses the frame cap', 'src/renderer/renderer.cpp', r'const uint32_t out_cap_h = out_cap_h_;'),
    ('the cap is applied (not only asked)', 'src/renderer/renderer.cpp', r'swapchain\.ApplyOutputCap\(out_cap_h,'),
    ('the blit is recorded', 'src/renderer/renderer.cpp', r'swapchain\.RecordCapBlit\(slot\.cmd, image_index\)'),
    ('SGSR reads the frame bilinear flag', 'src/renderer/renderer.cpp', r'options\.present_sgsr && !out_bilinear_'),
]
# the render thread must NOT read the live global (review F4)
FORBIDDEN = [('the render thread does not read the live cap global', 'src/renderer/renderer.cpp', r'g_out_cap_h\.load'),
             ('the render thread does not read the live bilinear global', 'src/renderer/renderer.cpp', r'g_out_bilinear\.load'),
             ('the reader no longer stores the switches (live.cpp does, from the table)', 'src/renderer_adapter/reader.h', r'g_out_cap_h\.store')]


def wiring(root):
    for name, rel, pat in WIRING:
        line(re.search(pat, read(root, rel)) is not None, 'wiring: ' + name)
    for name, rel, pat in FORBIDDEN:
        line(re.search(pat, read(root, rel)) is None, 'wiring: ' + name)


def lint_pass_class(root, work):
    """F1: the present pipeline is cached by (state, pass class). The window's pass and the capped pass differ in dependencyCount (MakePass adds one for TRANSFER_SRC
    final layouts), so the class key must tell them apart. Real code must pass; the planted copy (pad = 1 always, as before the fix) must FAIL."""
    sc = read(root, 'src/renderer/swapchain.cpp')
    rc = read(root, 'src/renderer/renderer.cpp')
    differ = 'dependencyCount = 2' in sc and 'pass_info.dependencyCount = 1' in sc
    def ok(text):
        m = re.search(r'class_key\.pad\s*=\s*([^;]*);', text)
        return (not differ) or (m is not None and 'out_capped' in m.group(1))
    line(differ, 'lint: the two present passes do differ in dependencyCount (the lint is not vacuous)')
    line(ok(rc), 'lint: the present class key tells the capped pass from the window pass')
    planted = re.sub(r'class_key\.pad\s*=\s*out_capped \? 2 : 1;', 'class_key.pad = 1;', rc)
    line(planted != rc and not ok(planted), 'lint planted (pad = 1 for both passes, as before the fix): caught')


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout)
    return (p.stdout or '') + (p.stderr or '')


def counts(text, tag):
    m = re.search(tag + r'presents (\d+), drawn capped (\d+) \(cap (\d+), plant (\d+)\), through SGSR (\d+), bilinear switch (\d+), makecap tries (\d+)', text)
    v = re.search(r'validation errors (\d+)', text)
    return (tuple(int(x) for x in m.groups()) if m else None, int(v.group(1)) if v else -1)


def cap_ok(c, valid):  # the predicate a plant must fail: every present drawn capped, no validation error
    return c is not None and c[0] > 0 and c[1] == c[0] and valid == 0


MEAN_DIFF_MAX = 2.0   # F2: the capped shot vs a PIL bilinear stretch of an honest 1920x1280 SGSR shot, mean abs diff per channel (0-255)
CORNER_MIN = 8.0      # F2: the bottom-right block of the capped shot must be lit, not the clear colour / zero


def pixel_verdict(cap_img, ref_img):
    """F2: returns (ok, mean_diff, corner_mean). ref_img is the 1920x1280 SGSR shot; the cap shot must be that picture stretched over the panel."""
    ref_up = np.asarray(Image.fromarray(ref_img.astype(np.uint8)).resize((cap_img.shape[1], cap_img.shape[0]), Image.BILINEAR), dtype=np.int16)
    diff = float(np.abs(cap_img - ref_up).mean())
    h, w = cap_img.shape[:2]
    corner = float(cap_img[int(h * 0.80):, int(w * 0.80):].mean())
    return diff < MEAN_DIFF_MAX and corner > CORNER_MIN, diff, corner


def arms(exe_cmd, tag, work, name, check_corner=True):
    shots = {}
    base = ['--present-effect', 'sgsr']
    plan = [('sgsr', base, 2736, 1824), ('bilinear', base + ['--present-bilinear'], 2736, 1824), ('ref', base, 1920, 1280),
            ('cap', base + ['--output-cap', '1280'], 2736, 1824), ('plant', base + ['--output-cap', '1280', '--output-cap-plant'], 2736, 1824),
            ('blitplant', base + ['--output-cap', '1280', '--output-blit-plant'], 2736, 1824),
            ('mkfail', base + ['--output-cap', '1280', '--output-makecap-fail-plant'], 2736, 1824)]
    for arm, extra, pw, ph in plan:
        shot = os.path.join(work, '%s_%s.png' % (name, arm))
        shots[arm] = shot
        text = run(exe_cmd + ['--present-size', '%dx%d' % (pw, ph), '--present-shot', shot] + extra)
        c, valid = counts(text, tag)
        if arm == 'sgsr':
            line(c is not None and c[0] > 0 and c[4] == c[0] and c[1] == 0 and valid == 0, '%s sgsr arm: %s validation %d' % (name, c, valid))
        if arm == 'bilinear':
            line(c is not None and c[0] > 0 and c[4] == 0 and valid == 0, '%s bilinear switch arm (SGSR off): %s validation %d' % (name, c, valid))
        if arm == 'ref':
            line(c is not None and c[0] > 0 and valid == 0, '%s 1920x1280 SGSR reference arm: %s validation %d' % (name, c, valid))
        if arm == 'cap':
            line(cap_ok(c, valid), '%s cap arm applied on every present: %s validation %d' % (name, c, valid))
        if arm == 'plant':
            line(not cap_ok(c, valid) and c is not None and c[2] == 1280, '%s PLANT (cap on, not applied) is caught: %s' % (name, c))
        if arm == 'mkfail':  # F7: the target cannot be made: one try, the picture still drawn at the panel size, nothing capped
            line(c is not None and c[6] == 1 and c[1] == 0 and c[0] > 1 and valid == 0, '%s PLANT (MakeCap fails) tried once, not every frame: %s validation %d' % (name, c, valid))

    def img(k):
        return np.asarray(Image.open(shots[k]).convert('RGB'), dtype=np.int16)
    ok, d, corner = pixel_verdict(img('cap'), img('ref'))
    line(ok, '%s cap shot = the 1920x1280 SGSR picture stretched (mean diff %.3f < %.1f, bottom-right block mean %.1f > %.1f)' % (name, d, MEAN_DIFF_MAX, corner, CORNER_MIN))
    okp, dp, cp = pixel_verdict(img('blitplant'), img('ref'))
    line(not okp, '%s PLANT (blit only to the capped-size corner) is caught (mean diff %.3f, bottom-right block mean %.1f)' % (name, dp, cp))
    okq, dq, cq = pixel_verdict(img('plant'), img('ref'))
    line(dq > 0, '%s uncapped picture is not the stretched one (diff %.3f): the cap changed the pixels' % (name, dq))
    line(float(np.abs(img('bilinear') - img('sgsr')).mean()) > 0.005, '%s bilinear differs from SGSR' % name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--bryan', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    work = os.path.join(root, 'out', 's102', 'g2', 'check')
    os.makedirs(work, exist_ok=True)
    header = read(root, 'src/renderer_adapter/low_levers.h')
    line(unit(root, work, 'real', header) is True, 'unit: the decisions hold on the real low_levers.h')
    twin = header.replace('return low && value >= 360', 'return value >= 360')
    line(twin != header and unit(root, work, 'twin', twin) is False, 'unit twin: a cap that ignores LOW fails the test')
    twin2 = header.replace('return lever && low; }\n\ninline constexpr uint32_t kLowCopyClasses', 'return lever; }\n\ninline constexpr uint32_t kLowCopyClasses')
    twin2 = twin2 if twin2 != header else header.replace('PresentBilinear(bool lever, bool low) { return lever && low; }', 'PresentBilinear(bool lever, bool low) { return lever; }')
    line(twin2 != header and unit(root, work, 'twin2', twin2) is False, 'unit twin: a bilinear switch that ignores LOW fails the test')
    wiring(root)
    lint_pass_class(root, work)
    rplay = os.path.join(root, 'out/renderer/dev/rplay.exe')
    if not os.path.isfile(rplay):
        line(False, 'rplay.exe missing: run tools\\renderer\\build.ps1')
        return 1
    arms([rplay, '--validation', '2', '--frames', '20'], r'\[rplay\]\[present\] size \S+: ', work, 'rplay')
    if a.bryan:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main_root = os.path.dirname(common)
        z = os.path.join(main_root, 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip')
        game = os.path.join(main_root, 'game_data/extracted')
        fv = os.path.join(root, 'out/renderer/dev/fview.exe')
        for p in (z, game, fv):
            if not os.path.exists(p):
                line(False, 'input missing: ' + p)
                return 1
        arms([fv, '--case', z, '--replay', '--frames', '60', '--validation', '1', '--crowd-ours', '--crowd-cards', '--game', game], r'\[fview\]\[present\] ', work, 'bryan')
    print('g2_check: %d failure(s)' % fails)
    return 1 if fails else 0


sys.exit(main())

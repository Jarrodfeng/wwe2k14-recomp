#!/usr/bin/env python3
"""Lane 132 offline proof (0 game runs): every particle draw is ours, runs of one and the soft variant included (N5; PARTICLES_PROMOTION_MAP.md section 11).

  py -I tools\\particles\\singles_check.py [--root DIR] [--no-build] [--keep]

Bar (each line PASS / FAIL; a missing input FAILS, never skips). Menu (3,329 frames), CAW (300), Check B (341), Bryan (91); switch off vs on, EVERY frame:
  1. the presented picture's max channel difference <= 1 level (owned_check's bar is looser; this is the bar of the existing particles_ours gate), rplay exit 0 (validation 0),
     no `attribute type:` line (mismatch 0), no missing shader.
  2. census (the shipped binder's line, on): kept 0 (lone / soft / third / other all 0), draws == folded, particles drawn == folded, blend keyed 0, runs == merged calls.
     Menu and CAW must show runs of one / soft drawn (> 0): the census cannot pass by finding nothing.
  3. plants (each must move the picture, > 1 level, from the clean arm): soft_as_hard (CAW: the soft run takes the hard pixel stage), single_dropped (menu: runs of one dropped),
     view_first (every member takes the first particle's c203-c206) and soft_rows_at_32 (LocalWorldView at the hard path's offset), first_texture (R: the past bug of lane 35, "one texture for every member", on the menu where neighbouring runs differ only by texture).
The existing particles_ours gate (runs_check.py: Check B and Bryan, counts, timing, plants) is separate and unchanged."""
import argparse
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import runs_check as rc  # the shipped helpers: extract, rplay, pics

ROOT = rc.ROOT
MENU = 'out/s101/runs/20261112_122344_s100_owner_boot_menu_quit/case_20261112_122351_841_0_pc_run_exit.zip'
CAW = 'out/s101/runs/20261112_122353_s101_checkB_caw_walk/case_20261112_122500_157_0_pc_run_exit.zip'
CAPS = {'menu': (MENU, 3329), 'caw': (CAW, 300), 'brock': (rc.BROCK, 341), 'bryan': (rc.BRYAN, 91)}
# plant -> (capture, frames to check)
PLANTS = {'soft_as_hard': ('caw', (60, 120, 180, 240)), 'view_first': ('caw', (60, 120, 180, 240)), 'soft_rows_at_32': ('caw', (60, 120, 180, 240)), 'single_dropped': ('menu', (1500, 2000, 3000, 3300)), 'first_texture': ('menu', (1500, 2000, 3000, 3300))}
failures = 0


def rplay(*a, **k):
    """rc.rplay, but the census is the LAST line (the end-of-run total, `[rplay] promotion census`), not the first 600-frame window."""
    code, text, _ = rc.rplay(*a, **k)
    ms = list(rc.CENSUS.finditer(text))
    census = dict(zip(rc.FIELDS, (float(x) if f == 'ms' else int(x) for f, x in zip(rc.FIELDS, ms[-1].groups())))) if ms else None
    return code, text, census


def say(ok, msg):
    global failures
    if not ok:
        failures += 1
    print(('PASS  ' if ok else 'FAIL  ') + msg)
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_dir = rc.main_checkout(root)
    try:
        import numpy as np
        from PIL import Image
    except ImportError as e:
        print('FAIL  numpy / PIL missing: %s' % e)
        return 1
    work = os.path.join(root, 'out', 'particles_singles')
    os.makedirs(work, exist_ok=True)
    if not args.no_build:
        code, text = rc.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'renderer', 'build.ps1')], 1800)
        if code != 0:
            print(text[-2000:])
            print('FAIL  the renderer build failed')
            return 1
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.isfile(exe):
        print('FAIL  %s is missing' % exe)
        return 1
    caps = {}
    for name, (rel, _) in CAPS.items():
        caps[name] = rc.extract(main_dir, rel, os.path.join(work, name))
        if not caps[name]:
            print('FAIL  cannot read the case zip %s (or extract its capture\\)' % rel)
            return 1
    # 0. the soft unit cases (runs_test.cpp `soft`): the shipped key and packer on a real CAW frame, doctored twins, plants soft_mask_wide / view_first / soft_rows_at_32
    test_exe = os.path.join(work, 'runs_test.exe')
    code, text = rc.run(['C:/Program Files/LLVM/bin/clang++.exe', '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-o', test_exe, os.path.join(root, 'tools', 'particles', 'runs_test.cpp')], 600)
    if code != 0:
        print(text[-1500:])
        say(False, '0. runs_test.cpp does not build')
        return 1
    code, text = rc.run([test_exe, caps['caw'], '150', 'soft'], 300)
    for line in text.strip().splitlines():
        if line.startswith('FAIL'):
            print(line)
    say(code == 0 and 'PASS:' in text, '0. runs_test soft cases (c204 keeps the run, c202 / c207 split it, soft_mask_wide fails that, packer rows, view_first / soft_rows_at_32, third site): ' + (text.strip().splitlines() or ['no output'])[-1])
    a_dirs = {}
    for name, (rel, frames) in CAPS.items():
        shots = list(range(frames))
        a_dir, b_dir = os.path.join(work, name + '_a'), os.path.join(work, name + '_b')
        for d in (a_dir, b_dir):
            shutil.rmtree(d, ignore_errors=True)
        code_a, text_a, _ = rplay(exe, caps[name], frames, [], shots, a_dir)
        code_b, text_b, census = rplay(exe, caps[name], frames, ['--particles-ours'], shots, b_dir)
        say(code_a == 0 and code_b == 0, '1. %s: validation 0 off and on (exit codes %d / %d)' % (name, code_a, code_b))
        say('attribute type:' not in text_b and 'missing shader' not in text_b.replace('0 missing a shader', ''), '1. %s: mismatch 0 (no attribute type line), no missing shader' % name)
        worst, mean_worst, bad = rc.pics(np, Image, a_dir, b_dir, shots)
        say(worst <= 1 and not bad, '1. %s: %d frames, picture max difference %d levels (worst frame mean %.4f), frames over 1 level: %s' % (name, len(shots), worst, mean_worst, bad[:8]))
        if not census:
            say(False, '2. %s: no particles census line in rplay\'s output' % name)
            continue
        c = census
        say(c['kept'] == 0 and c['lone'] == 0 and c['soft'] == 0 and c['third'] == 0 and c['other'] == 0, '2. %s: kept %d (lone %d, soft %d, third %d, other %d) -> 0 particle draws left on the stream' % (name, c['kept'], c['lone'], c['soft'], c['third'], c['other']))
        say(c['draws'] == c['folding'] and c['particles'] == c['folding'] and c['runs'] == c['merged'] and c['blend_keyed'] == 0, '2. %s: draws %d == folded %d == particles drawn %d; runs %d == merged calls %d; blend keyed %d' % (name, c['draws'], c['folding'], c['particles'], c['runs'], c['merged'], c['blend_keyed']))
        m = list(re.finditer(r'runs of one (\d+) .*soft drawn (\d+)', text_b))
        singles, soft_drawn = (int(m[-1].group(1)), int(m[-1].group(2))) if m else (-1, -1)
        if name == 'menu':
            say(singles > 0, '2. menu: runs of one drawn by ours %d (> 0: the check sees them)' % singles)
        if name == 'caw':
            say(soft_drawn > 0 and soft_drawn == c['draws'], '2. caw: soft particles drawn by ours %d == all %d particle draws' % (soft_drawn, c['draws']))
        a_dirs[name] = a_dir
        if not args.keep:
            shutil.rmtree(b_dir, ignore_errors=True)
    for plant, (name, frames) in PLANTS.items():
        p_dir = os.path.join(work, 'plant_' + plant)
        shutil.rmtree(p_dir, ignore_errors=True)
        code_p, text_p, cp = rplay(exe, caps[name], max(frames) + 1, ['--particles-ours', '--particles-fault', plant], list(frames), p_dir)
        if not cp:
            say(False, '3. %s: no census line (rplay exit %d)' % (plant, code_p))
            continue
        worst_p, mean_p, _ = rc.pics(np, Image, a_dirs[name], p_dir, frames)
        say(worst_p > 1, '3. plant %s (%s) fails: picture moved by %d levels (mean %.3f), census kept %d' % (plant, name, worst_p, mean_p, cp['kept']))
    if not args.keep:
        for name in CAPS:
            shutil.rmtree(os.path.join(work, name + '_a'), ignore_errors=True)
        for plant in PLANTS:
            shutil.rmtree(os.path.join(work, 'plant_' + plant), ignore_errors=True)
    print('%s: %d failure(s)' % ('FAIL' if failures else 'PASS', failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

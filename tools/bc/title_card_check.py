#!/usr/bin/env python3
"""S102 lane U1: the black title card on a Mali-G615 phone, proved offline (0 game runs, 0 Surface runs, no window shown).
Cause (docs\\renderer\\TITLE_CARD_MALI.md): the phone has no BC texture formats; the owned renderer refused every BC1-BC5 texture by name
(`[rdr][PROBLEM] image_format_unsupported: format 26, usage 1`, the player's own log line), so the title card, the menus and every file picture were missing. The fix
decodes BC on the CPU at upload when the device cannot sample it (src\\renderer\\bc_decode.h, switch --wwe2k14_rdr_bc_decode, default 1 = automatic).

  py -I tools\\bc\\title_card_check.py [--root DIR]

Inputs (a missing one FAILS): out\\renderer\\dev\\rplay.exe + fview.exe built from this tree; the boot / menu case zip of the census lane
(out\\s101\\runs\\20261112_122344_s100_owner_boot_menu_quit, 3,329 frames, the title card is on screen from frame ~800; extracted once into out\\s101\\movie_l131\\case_menu,
read in place). No game files.

Bars (each line PASS / FAIL):
  decoder        tools\\bc\\bc_decode_check.py passes (bit-exact with Pillow's DDS decoder on random blocks of every format; its own planted twins fail).
  picture.forced rplay shots of the title card and the menu, native BC (arm A) vs --bc-decode 2 (arm B): max channel difference <= 8 levels, 0 pixels over 8, <= 2 % of the
                 pixels over 1 level (the GPU's own BC1 palette rounds differently), mean luma shift <= 0.5, and every title frame is not dark (mean > 30).
  picture.caps   --format-caps-mask 40 (this PC pretends it has no BC; automatic mode) == arm B pixel for pixel: the capability path and the forced path are the same decode.
  refind         the same pretend-no-BC device with --bc-decode 0 (the v0.3.2 behaviour) dies on `image_format_unsupported: format 24|25|26` in this dev build: the player's line.
  verdict        the replay log carries `block-compressed textures (BC1-BC5): the device CANNOT sample them ... decoded on the CPU at upload`.
  validation     fview replay of 900 frames with --bc-decode 2 and the validation layer on: errors 0, no PROBLEM line.
  start_failure  rplay --gpu 99 (no such card) refuses to start with the renderer's plain sentence, exit != 0; the message text names the reason, the card and the report button.
  gpu_name       --gpu-name zzz: `no card clears the floor with that name, using <best>`; --gpu-name <part of the real card's name>: `using <that card>`.
  twin.black     the picture comparison can fail: a black copy of a title frame is reported as over the bar.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
results = []
SHOTS = [100, 500, 800, 900, 1000, 1500, 2000]
TITLE_FRAMES = [800, 900, 1000]
FRAMES = 2100


def report(name, ok, evidence):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', str(root), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return root


def run(cmd, timeout=900):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, p.stdout + p.stderr


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16)


def compare(a, b):
    """(max channel difference, pixels over 1, pixels over 8, total, mean luma a, mean luma b)"""
    d = np.abs(a - b).max(axis=2)
    return int(d.max()), int((d > 1).sum()), int((d > 8).sum()), d.size, float(a.mean()), float(b.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(ROOT))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    main_co = main_checkout(root)
    rplay = root / 'out' / 'renderer' / 'dev' / 'rplay.exe'
    fview = root / 'out' / 'renderer' / 'dev' / 'fview.exe'
    for exe in (rplay, fview):
        if not exe.exists():
            report('exe', False, f'missing {exe}: build with tools\\renderer\\build.ps1 and tools\\fview\\build.ps1')
            return 1
    # decoder
    rc, text = run([sys.executable, '-I', root / 'tools' / 'bc' / 'bc_decode_check.py', '--root', root], 600)
    report('decoder', rc == 0 and text.count('PASS') >= 10 and 'FAIL' not in text, f'bc_decode_check exit {rc}, {text.count("PASS")} PASS lines')
    # the capture
    cap = main_co / 'out' / 's101' / 'movie_l131' / 'case_menu' / 'capture'
    if not (cap / 'images.bin').exists():
        run_dir = main_co / 'out' / 's101' / 'runs' / '20261112_122344_s100_owner_boot_menu_quit'
        zips = sorted(run_dir.glob('case_*.zip')) if run_dir.is_dir() else []
        if not zips:
            report('capture', False, f'the boot / menu case zip is missing under {run_dir}')
            return 1
        base = main_co / 'out' / 's101' / 'movie_l131' / 'case_menu'
        base.mkdir(parents=True, exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(base)
    if not (cap / 'images.bin').exists():
        report('capture', False, f'no capture at {cap}')
        return 1
    tmp = Path(tempfile.mkdtemp(prefix='u1_'))
    shots = ','.join(map(str, SHOTS))

    def arm(name, extra):
        d = tmp / name
        d.mkdir()
        rc, text = run([rplay, '--capture', cap, '--frames', FRAMES, '--no-vsync', '--shot-dir', d, '--shots', shots] + extra)
        (tmp / (name + '.log')).write_text(text, encoding='utf-8')
        return d, rc, text

    da, rca, ta = arm('native', [])
    db, rcb, tb = arm('forced', ['--bc-decode', '2'])
    dc, rcc, tc = arm('caps_auto', ['--format-caps-mask', '40'])
    dd, rcd, td = arm('caps_off', ['--format-caps-mask', '40', '--bc-decode', '0'])
    report('arms', rca == 0 and rcb == 0 and rcc == 0, f'rplay exits native {rca}, forced {rcb}, caps_auto {rcc} (logs in {tmp})')
    # picture.forced
    worst, over8, over1, total, shift, dark = 0, 0, 0, 0, 0.0, []
    for s in SHOTS:
        pa, pb = da / f'frame_{s:04d}.png', db / f'frame_{s:04d}.png'
        if not (pa.exists() and pb.exists()):
            report(f'picture.forced.{s}', False, 'a shot is missing')
            continue
        a, b = load(pa), load(pb)
        m, o1, o8, n, ma, mb = compare(a, b)
        worst, over8, over1, total, shift = max(worst, m), over8 + o8, max(over1, o1), n, max(shift, abs(ma - mb))
        if s in TITLE_FRAMES and mb <= 30:
            dark.append(s)
    report('picture.forced', worst <= 8 and over8 == 0 and over1 / max(1, total) <= 0.02 and shift <= 0.5 and not dark,
           f'{len(SHOTS)} shots: max channel difference {worst}, pixels over 8 levels {over8}, worst share over 1 level {over1 / max(1, total):.4f}, mean luma shift {shift:.3f}, dark title frames {dark}')
    # picture.caps
    bad = []
    for s in SHOTS:
        pb, pc = db / f'frame_{s:04d}.png', dc / f'frame_{s:04d}.png'
        if not (pb.exists() and pc.exists()) or compare(load(pb), load(pc))[0] != 0:
            bad.append(s)
    report('picture.caps', not bad, f'{len(SHOTS)} shots, frames that differ: {bad}')
    # refind
    refound = rcd != 0 and re.search(r'image_format_unsupported: format (24|25|26),', td) is not None
    report('refind', refound, f'exit {rcd}; line: {next((l for l in td.splitlines() if "image_format_unsupported" in l), "(none)")}')
    report('verdict', 'the device CANNOT sample them; bc_decode 1 -> decoded on the CPU at upload' in tc and 'the device samples them; bc_decode 1 -> sampled as they are' in ta,
           'native arm says it samples BC; the pretend-no-BC arm says it decodes on the CPU')
    # validation
    rc, text = run([fview, '--root', root, '--capture', cap, '--replay', '--validation', '1', '--bc-decode', '2', '--frames', 900], 900)
    m = re.search(r'validation errors (\d+)', text)
    report('validation', rc == 0 and m is not None and int(m.group(1)) == 0 and '[rdr][PROBLEM]' not in text, f'exit {rc}, validation errors {m.group(1) if m else "?"}')
    # start failure
    rc, text = run([rplay, '--capture', cap, '--frames', 5, '--gpu', 99], 120)
    exe = tmp / 'start_failure_text.exe'
    p = subprocess.run(['C:/Program Files/LLVM/bin/clang++.exe', '-std=c++20', '-O1', str(root / 'tools' / 'bc' / 'start_failure_text_cli.cpp'), '-o', str(exe)], capture_output=True, text=True)
    msg = ''
    if p.returncode == 0:
        msg = subprocess.run([str(exe), 'This graphics hardware is below the minimum. Missing: Vulkan 1.1.', 'Radeon'], capture_output=True, text=True).stdout
        bare = subprocess.run([str(exe), '', ''], capture_output=True, text=True).stdout
    else:
        bare = ''
    ok = (rc != 0 and 'could not start: This graphics hardware is below the minimum.' in text and 'could not start its graphics' in msg and 'below the minimum' in msg and '"Radeon"' in msg
          and 'REPORT A PROBLEM' in msg and 'could not start its graphics' in bare and 'dedicated one' in bare)
    report('start_failure', ok, f'rplay --gpu 99 exit {rc}: "{text.strip().splitlines()[-1] if text.strip() else ""}"; the message carries the reason, the card and the report button: {bool(msg)}')
    # gpu name
    rc1, t1 = run([rplay, '--capture', cap, '--frames', 5, '--gpu-name', 'zzz'], 120)
    m = re.search(r'\[rdr\] device: ([^,]+),', ta)
    real = m.group(1) if m else ''
    part = real.split()[-1] if real else ''
    rc2, t2 = run([rplay, '--capture', cap, '--frames', 5, '--gpu-name', part], 120) if part else (1, '')
    report('gpu_name', rc1 == 0 and 'no card clears the floor with that name, using' in t1 and rc2 == 0 and f"asked for '{part}'" in t2 and f'using {real}' in t2,
           f'zzz -> {next((l for l in t1.splitlines() if "graphics card" in l), "(none)")}; {part!r} -> {next((l for l in t2.splitlines() if "graphics card" in l), "(none)")}')
    # S102 U1 review: the staging stall, the decode counters, the per-window verdict
    match = main_co / 'out' / 'captures' / 's100_match300'
    if not match.exists():
        report('stall', False, f'capture missing: {match}')
    else:
        t0 = time.time()
        rc_fix, tf = run([rplay, '--capture', match, '--frames', 300, '--no-vsync', '--bc-decode', 2, '--staging-cap-mb', 8], 60)
        fixed_s = time.time() - t0
        t0 = time.time()
        try:
            rc_plant, _tp = run([rplay, '--capture', match, '--frames', 300, '--no-vsync', '--bc-decode', 2, '--staging-cap-mb', 8, '--staging-wait-plant'], 30)
            plant_s, plant_timed_out = time.time() - t0, False
        except subprocess.TimeoutExpired:
            plant_s, plant_timed_out = time.time() - t0, True
        report('stall', rc_fix == 0 and fixed_s < 15 and plant_timed_out, f'decoded match case with an 8 MB staging cap: fixed {fixed_s:.1f} s (exit {rc_fix}); the planted pre-fix wait {"did not finish in" if plant_timed_out else "finished in"} {plant_s:.0f} s (2 s steps per upload)')
        m1 = re.search(r'bc decoded (\d+) images, ([\d.]+) MB in all', tf)
        rc_n, tn = run([rplay, '--capture', match, '--frames', 300, '--no-vsync'], 60)
        m0 = re.search(r'bc decoded (\d+) images, ([\d.]+) MB in all', tn)
        report('counters', bool(m1) and int(m1.group(1)) > 0 and float(m1.group(2)) > 20 and 'the first block-compressed level decoded on the CPU' in tf and bool(m0) and int(m0.group(1)) == 0 and float(m0.group(2)) == 0.0,
               f'forced decode: {m1.group(1) if m1 else "?"} images, {m1.group(2) if m1 else "?"} MB; native: {m0.group(1) if m0 else "?"} images, {m0.group(2) if m0 else "?"} MB')
    rc_v, tv = run([rplay, '--capture', cap, '--frames', FRAMES, '--no-vsync', '--movie-ours'], 300)
    vl = [l for l in tv.splitlines() if l.startswith('[rdr][verdict] movie')]
    counts = [int(re.search(r'ours frames (\d+)', l).group(1)) for l in vl if re.search(r'ours frames (\d+)', l)]
    report('verdict.window', rc_v == 0 and len(vl) >= 2 and all('movie ok' in l for l in vl) and all(c <= 600 for c in counts), f'{len(vl)} windows, ours frames per window {counts} (a cumulative count would climb past 600)')
    # twin
    a = load(da / 'frame_0900.png')
    m, o1, o8, n, ma, mb = compare(a, np.zeros_like(a))
    report('twin.black', m > 8 and o8 > n // 2, f'a black copy of the title frame: max {m}, {o8} of {n} pixels over 8 levels')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())

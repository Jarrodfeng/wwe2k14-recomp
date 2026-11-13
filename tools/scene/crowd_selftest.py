#!/usr/bin/env python3
"""Lane 27 acceptance test (offline, 0 game runs): the crowd contributor's record against the viewer's own crowd.

  py -I tools\\scene\\crowd_selftest.py [--root DIR] [--arena 5] [--keep]

Runs tools\\scene\\crowd_make.exe (builds a crowd snapshot from the files, drives the contributor as the game hooks do, writes records), then
rsc_draw.exe on each (WWE_RDR_VALIDATION=1) into out\\s101\\crowd\\ of the MAIN checkout (pictures made from game files never go in a commit):
  ref    the viewer's own crowd: the arena record drawn with --keep-src-kind 3 (crowd meshes only)
  clean  the contributor's record (members + 20 palettes + the far cards as objects)
  fault_palette  the same with every palette at the bind pose (identity): the members stand unposed
  fault_cards    the same with the far cards' objects dropped (the Session 100 far-crowd class: cards that were not there / hid the ones behind)
Bar (the tile check of selftest.py, plus a pixel-difference bar): clean must pass; both planted faults must FAIL it; validation errors 0 in
every draw; the contributor must have emitted objects and the draw must have posed them. A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _main_checkout():
    # The main checkout (where game_data\ and out\ live), also from a worktree: git's common dir is the main .git (as tools\gate\gate.py).
    here = os.path.normpath(os.path.join(HERE, '..', '..'))
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


MAIN = _main_checkout()
os.environ.setdefault('RDR_MAIN_CHECKOUT', MAIN)
sys.path.insert(0, HERE)
import selftest  # noqa: E402  (tile_stats / compare: the same tile bar)
PIX_THRESH = 40.0      # luminance difference counting as "this pixel differs"
CLEAN_MAX_DIFF = 0.020  # clean: at most 2% of the picture's pixels differ by more than PIX_THRESH from the viewer's crowd
FAULT_MIN_DIFF = 0.030  # a planted fault must differ by at least 3% (and fail the tile bar or this bar)
COVER_MAX = 0.030       # clean: at most 3% of the viewer crowd's pixels are missing from the contributor's picture
COVER_FAULT_MIN = 0.080  # a planted fault (the palettes at the bind pose) loses at least 8% of the crowd's pixels


def run(cmd, env=None, timeout=600):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def diff_fraction(np, a, b):
    lum = np.array([0.3, 0.59, 0.11])
    return float((np.abs(a.astype(np.float64) @ lum - b.astype(np.float64) @ lum) > PIX_THRESH).mean())


CLEAR = (0.12 * 255, 0.14 * 255, 0.18 * 255)  # the front-end's clear colour (SceneFrame::clear)


def missing_share(np, ref, got):
    """The share of the reference's crowd pixels (anything but the clear colour) that `got` does not draw: a crowd that is "bare" (the S100
    far-crowd report: far members vanished) loses coverage even when the tile means stay close, because the far people are small."""
    c = np.array(CLEAR)
    r = np.abs(ref.astype(np.float64) - c).sum(axis=2) > 24
    g = np.abs(got.astype(np.float64) - c).sum(axis=2) > 24
    return float((r & ~g).sum()) / max(1, int(r.sum()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--arena', type=int, default=5)
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image
    except ImportError as e:
        print(f'FAIL  crowd: needs numpy and Pillow ({e})')
        return 1
    root = os.path.abspath(args.root)
    game = os.path.join(MAIN, 'game_data', 'extracted')
    exe_make = os.path.join(root, 'out', 'tools', 'crowd_make.exe')
    exe_draw = os.path.join(root, 'out', 'tools', 'rsc_draw.exe')
    for need in (exe_make, exe_draw, game):
        if not os.path.exists(need):
            print(f'FAIL  crowd: missing {need} (pwsh tools\\scene\\build_rsc.ps1; game_data\\extracted)')
            return 1
    out = os.path.join(MAIN, 'out', 's101', 'crowd')
    os.makedirs(out, exist_ok=True)
    fails = []

    code, text = run([exe_make, '--contract'])
    for l in text.splitlines():
        if 'contract' in l or 'AddString' in l:
            print(l.replace('[crowd_make] ', ''))
    if code != 0:
        print('FAIL  contributor contract (stale snapshots, palette ring, unmodelled counts, unique ids, AddString speed)')
        fails.append('contract')

    code, text = run([exe_make, '--game', game, '--arena', str(args.arena), '--out', out])
    sys.stdout.write(''.join(l + '\n' for l in text.splitlines() if l.startswith('[crowd_make]')))
    if code != 0:
        print(f'FAIL  crowd_make exit {code}: {text.strip()[-300:]}')
        return 1
    m = re.search(r'clean\.rsc: objects (\d+), unknown (\d+)', text)
    if not m or int(m.group(1)) < 100:
        print('FAIL  crowd: the contributor emitted fewer than 100 objects (or no census line)')
        fails.append('objects')
    else:
        print(f'PASS  contributor emitted {m.group(1)} crowd objects, {m.group(2)} unknown fields counted')

    env = dict(os.environ, WWE_RDR_VALIDATION='1')
    pics = {}
    for name, extra in (('ref', ['--keep-src-kind', '3']), ('clean', []), ('fault_palette', []), ('fault_cards', []), ('cards_only', [])):
        png = os.path.join(out, name + '.png')
        if os.path.exists(png):
            os.remove(png)
        for attempt in range(12):  # (other tools on the machine can hold the GPU for a moment: "Not enough graphics memory for the frame ring")
            code, text = run([exe_draw, os.path.join(out, name + '.rsc'), '--game', game, '--out', png] + extra, env=env)
            if code == 0 or 'Not enough graphics memory' not in text:
                break
            time.sleep(5)
        if code != 0 or not os.path.exists(png):
            print(f'FAIL  rsc_draw {name} exit {code}: {text.strip()[-300:]}')
            fails.append(f'{name}: no picture')
            continue
        errs = re.findall(r'validation errors: (\d+)', text)
        if not errs or int(errs[-1]) != 0:
            print(f'FAIL  {name}: validation errors {errs[-1] if errs else "(no line)"}')
            fails.append(f'{name}: validation')
        else:
            print(f'PASS  {name}: validation errors 0')
        if name != 'ref':
            cm = re.search(r'crowd: (\d+) objects, (\d+) with a palette', text)
            if not cm or int(cm.group(1)) == 0:
                print(f'FAIL  {name}: rsc_draw drew no crowd objects')
                fails.append(f'{name}: no crowd objects')
        pics[name] = np.asarray(Image.open(png).convert('RGB'))
    if 'ref' not in pics or 'clean' not in pics:
        return 1

    def bar(name):
        a, b = pics['ref'], pics[name]
        ok_tiles, notes = selftest.compare(selftest.read_png(os.path.join(out, 'ref.png')), selftest.read_png(os.path.join(out, name + '.png')))
        d = diff_fraction(np, a, b)
        return ok_tiles, d, notes

    ok, d, notes = bar('clean')
    miss = missing_share(np, pics['ref'], pics['clean'])
    good = ok and d <= CLEAN_MAX_DIFF and miss <= COVER_MAX
    print(('PASS' if good else 'FAIL') + f'  clean crowd vs the viewer\'s crowd: pixels off by > {PIX_THRESH:.0f}: {d * 100:.2f}% (max {CLEAN_MAX_DIFF * 100:.1f}%); '
          f'crowd pixels missing {miss * 100:.2f}% (max {COVER_MAX * 100:.1f}%); ' + '; '.join(notes))
    if not good:
        fails.append('clean differs from the viewer crowd')
    for name, what in (('fault_palette', 'every palette at the bind pose (members unposed)'), ('fault_cards', 'the far cards dropped (the S100 far-crowd class)')):
        if name not in pics:
            continue
        ok, d, notes = bar(name)
        miss = missing_share(np, pics['ref'], pics[name])
        min_miss = COVER_FAULT_MIN
        if name == 'fault_cards' and 'cards_only' in pics:
            # DERIVED bar: the far cards alone cover this share of the viewer crowd's pixels; dropping them must lose at least half of it
            # (the rest is hidden behind other people). Not a measure of the S100 alpha_test class (see below).
            c = np.array(CLEAR)
            share = float((np.abs(pics['cards_only'].astype(np.float64) - c).sum(axis=2) > 24).sum()) / max(1, int((np.abs(pics['ref'].astype(np.float64) - c).sum(axis=2) > 24).sum()))
            min_miss = 0.5 * share
            print(f'      far cards alone cover {share * 100:.2f}% of the crowd pixels: the dropped-cards bar is {min_miss * 100:.2f}% missing')
        caught = (not ok) or d >= FAULT_MIN_DIFF or miss >= min_miss
        print(('PASS' if caught else 'FAIL') + f'  planted fault "{what}" is caught: pixels off {d * 100:.2f}% (min {FAULT_MIN_DIFF * 100:.1f}%), crowd pixels missing '
              f'{miss * 100:.2f}% (min {min_miss * 100:.1f}%), tile bar {"passed" if ok else "failed"} (any one catches it); ' + '; '.join(notes))
        if not caught:
            fails.append(f'{name}: the planted fault passed the check')
    if fails:
        print('FAIL  crowd selftest: ' + '; '.join(fails))
        return 1
    print('PASS  crowd selftest')
    return 0


if __name__ == '__main__':
    sys.exit(main())

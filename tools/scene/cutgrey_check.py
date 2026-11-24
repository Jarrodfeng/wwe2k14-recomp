#!/usr/bin/env python3
r"""Lane CUTGREY's proof (Session 110; docs\research\s110\CUTGREY.md, CUTGREY_BUILD.md): the crowd camera flash is drawn AFTER the post chain, with the colour of its row, lit by the game's rule, hidden whole by the
depth at its centre. Offline, headless (a Vulkan device, no window), 0 game runs, no game files.

  py -I tools\scene\cutgrey_check.py [--no-build]

Builds out\tools\cutgrey_unit.exe (tools\scene\build_cutgrey_unit.ps1: the REAL FrontEnd + PostPass over a hand-made flash quad) and runs it:
  clean run          every check passes: [ORDER] the flash picture minus the post(scene) picture equals the sprite alone on every pixel (depth of field + haze + sepia on); [LIGHT] the sprite's centre is
                     colour x (1 + abs(n.x)) x alpha; [COLOUR] a row coloured (0.6, 0.8, 0.6) draws that tint; [HIDE] a centre beyond the far plane hides the whole sprite; [VAL] 0 validation errors
  planted twins      each must FAIL its named check (the unit's --expect-fail run exits 0 only when the twin was caught, and this script also reads the named token):
                       flash_pre_post     (the old order: the flash drawn into the scene colour BEFORE the post chain)  -> [ORDER]
                       flash_file_colour  (the row's colour ignored)                                                    -> [COLOUR]
                       flash_v_flip       (the centre's depth row read upside down)                                     -> [MAP]
                       flash_fetch_row0   (the centre's depth read from row 0)                                          -> [OCC]
  [OCC] (S110 FLASHOCC): a REAL opaque draw (an arena quad, and a skinned character quad) between a cutscene camera (viewer space, made as the live record frame makes it) and a flash centre 3600 units out,
  at render scale 1, 1.5 and 2 (scaled targets, the post chain on): the picture equals the one without the flash; the same flash with the body moved aside is drawn. [FACE]: the sprite faces the frame's viewer eye
  [MAP] (review): the top half of the scene depth is nearer; a flash centred in the top half is hidden whole, the same flash centred in the bottom half is drawn
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
EXE = os.path.join(ROOT, 'out', 'tools', 'cutgrey_unit.exe')
TWINS = (('flash_pre_post', '[ORDER]'), ('flash_file_colour', '[COLOUR]'), ('flash_v_flip', '[MAP]'), ('flash_fetch_row0', '[OCC]'))


def run(cmd, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, cwd=ROOT)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(HERE, 'build_cutgrey_unit.ps1')], 1800)
        if rc != 0:
            print(text[-3000:])
            print('FAIL  cutgrey_check: the unit did not build')
            return 1
    if not os.path.exists(EXE):
        print('FAIL  cutgrey_check: ' + EXE + ' is not there')
        return 1
    bad = 0
    rc, text = run([EXE], 600)
    for line in text.splitlines():
        if line.startswith(('PASS', 'FAIL', 'info')):
            print(line)
    if rc != 0 or 'PASS: cutgrey_unit' not in text:
        bad += 1
        print('FAIL  cutgrey_check: the clean run (exit %d)' % rc)
    for plant, token in TWINS:
        rc, text = run([EXE, '--plant', plant, '--expect-fail'], 600)
        failed_line = [l for l in text.splitlines() if l.startswith('FAIL') and token in l]
        caught = rc == 0 and 'planted twin CAUGHT' in text and bool(failed_line)
        print(('PASS' if caught else 'FAIL') + '  planted twin %s: %s (%s)' % (plant, 'caught by ' + token if caught else 'NOT caught by ' + token, 'exit %d' % rc))
        if not caught:
            bad += 1
            print('\n'.join(l for l in text.splitlines() if l.startswith(('PASS', 'FAIL', 'info', 'planted')))[:2500])
    print(('FAIL' if bad else 'PASS') + '  cutgrey_check: %d problem(s)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

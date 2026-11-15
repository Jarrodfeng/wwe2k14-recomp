#!/usr/bin/env python3
"""S102 lane K1: builds and runs src\\renderer_adapter\\guard_decay_test.cpp (GPU free, 0 game runs), then its planted twins, each of which must FAIL, then the wiring lints.
  py -I tools\\renderer\\guard_decay_unit.py [--root DIR]
The test (decaying state guard, wwe2k14_rdr_guard_decay, and the flat guard walk, wwe2k14_rdr_skip_lean): the match's moving shadow copies take the image rule after the window and not before; a
state image read every frame never decays; one read every 4th frame is still held by the 16 frame window; a tile atlas never goes per-frame; the flat walk equals the map walk frame by frame.
Planted twins: --plant-fast (a window of 1 frame: a lasting copy is lost), --plant-lean (the flat walk forgets a copy's write), --plant-noclear (the flat branch does not clear the map walk's leftovers on a mid-run flip: review F1), --plant-oldrule (the time-based draft rule: the reader-gap case loses its copies and the stale-read ledger counts); each must exit non-zero AND print a FAIL line (a crash is not a catch).
A twin that no longer applies fails as DID NOT APPLY. Wiring lints: the switches are in the table (PerFrame, default off), the cvars are defined (default false), live.cpp reads them with BoolSeen
(guest frame) before streaks.Note, and logs the [rdr][skip_cost] census line; the live path never reads a cvar copy once at start.
Exit 0 = pass, 1 = fail."""
import argparse, os, re, subprocess, sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ra = os.path.join(root, 'src', 'renderer_adapter')
    out = os.path.join(root, 'out', 'k1')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'guard_decay_test.exe')
    for need in (os.path.join(ra, 'guard_decay_test.cpp'), os.path.join(ra, 'lasting_prune.h')):
        if not os.path.isfile(need):
            print('FAIL: input missing ' + need)
            return 1
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-I', os.path.join(root, 'src'), '-I', ra, os.path.join(ra, 'guard_decay_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode or 'all passed' not in r.stdout:
        print('FAIL: the unit test failed')
        return 1
    for plant in ('--plant-fast', '--plant-lean', '--plant-noclear', '--plant-oldrule'):
        p = subprocess.run([exe, plant], capture_output=True, text=True)
        if p.returncode == 0 or 'FAIL' not in p.stdout:
            print('FAIL: planted twin %s was not caught (exit %d)' % (plant, p.returncode))
            return 1
        print('twin %s: caught (%d FAIL lines)' % (plant, p.stdout.count('FAIL ')))

    def text(rel):
        with open(os.path.join(root, rel), encoding='utf-8') as f:
            return f.read()
    sw, rd, lv, lp = text('src/renderer_adapter/rdr_switches.h'), text('src/renderer_adapter/reader.h'), text('src/renderer_adapter/live.cpp'), text('src/renderer_adapter/lasting_prune.h')
    need = [
        (sw, 'X(GuardDecay, "wwe2k14_rdr_guard_decay", PerFrame, 0)', 'switch table: guard_decay per-frame, default off'),
        (sw, 'X(SkipLean, "wwe2k14_rdr_skip_lean", PerFrame, 0)', 'switch table: skip_lean per-frame, default off'),
        (rd, 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_guard_decay, false', 'reader.h defines guard_decay (default false)'),
        (rd, 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_lean, false', 'reader.h defines skip_lean (default false)'),
        (lv, 'rdr_sw::BoolSeen(rdr_sw::kGuardDecay, false, message.guest_frame)', 'live.cpp reads guard_decay per frame with the guest frame'),
        (lv, 'rdr_sw::BoolSeen(rdr_sw::kSkipLean, false, message.guest_frame)', 'live.cpp reads skip_lean per frame with the guest frame'),
        (lv, '[rdr][skip_cost] frames %llu, skipped %llu, lasting binds %llu, skipped-frame ms mean', 'live.cpp logs the [rdr][skip_cost] census line'),
        (lp, 'return r.clean <= hold;', 'the decay rule (positive evidence: reads after the copy since the last read before it, held 2 x the carry cadence)'),
        (lv, 'streaks.NoteDrawn(frame);', 'live.cpp feeds the stale-read ledger on every drawn frame'),
        (lv, 'if (window.skip_frames || streaks.guard_decay || streaks.lean) {', 'the census line is printed whenever the window had skipped frames (switches off too)'),
        (lv, 'decayed-guard stale reads %llu (must be 0)', 'the census line carries the decayed-guard stale-read count'),
        (lv, 'streaks.CommitOwed();', 'live commits the owed images on the really-skipped branch only (review F4)'),
    ]
    bad = 0
    for src, needle, what in need:
        ok = needle in src
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += not ok
    i_read, i_note = lv.find('rdr_sw::BoolSeen(rdr_sw::kGuardDecay'), lv.find('      streaks.Note(frame);')
    ok = 0 <= i_read < i_note and 0 <= lv.find('rdr_sw::BoolSeen(rdr_sw::kSkipLean') < i_note
    print(('ok   ' if ok else 'FAIL ') + 'live.cpp reads both switches before streaks.Note (every handled frame, drawn or not)')
    bad += not ok
    # a text plant of the rule itself: made to decay at once, the unit test must fail (and the plant must have applied)
    rule = 'return r && Alive(*r);'
    planted = lp.replace(rule, 'return false;')
    if planted == lp:
        print('FAIL: text plant DID NOT APPLY (the decay rule line moved)')
        return 1
    pdir = os.path.join(out, 'plant_rule')
    os.makedirs(pdir, exist_ok=True)
    with open(os.path.join(pdir, 'lasting_prune.h'), 'w', encoding='utf-8', newline='') as f:
        f.write(planted)
    pexe = os.path.join(pdir, 'guard_decay_test_plant.exe')
    import shutil
    shutil.copyfile(os.path.join(ra, 'guard_decay_test.cpp'), os.path.join(pdir, 'guard_decay_test.cpp'))   # a quoted include looks in the including file's own folder first
    b = subprocess.run([CLANG, '-std=c++20', '-Wno-unused-parameter', '-I', pdir, '-I', os.path.join(root, 'src'), '-I', ra, os.path.join(pdir, 'guard_decay_test.cpp'), '-o', pexe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: the text-planted test did not compile\n' + b.stderr[-1500:])
        return 1
    p = subprocess.run([pexe], capture_output=True, text=True)
    if p.returncode == 0 or 'FAIL' not in p.stdout:
        print('FAIL: the text plant (a guard that never holds) was not caught')
        return 1
    print('twin text plant (guard never holds): caught')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

r"""Selftest of the census's thin-capture handling and proof bar (S101 lane 143; gate lane `thin_census`).

Unit level (no GPU, always runs; the SHIPPED functions in leftovers.py / proof_bar.py):
  - thin_frames reads the frame header's flags (kFrameHasThin = 32) of a hand-made capture folder: a capture with flagged frames is thin, one without is not;
  - the planted twin: the same capture with the flag cleared reads as not thin (the detector is what the census relies on);
  - thin_arg keeps only the promoted thin classes;
  - the `[fview] thin frame` line parses;
  - proof_bar: identical pictures pass; a picture with a 200x200 block missing (a ring-sized hole) fails the guard; the planted twin (guard limit raised to 1000) passes it, so the
    check is the guard and not something else; an unreadable picture is a failure, never a pass.
Live level (needs the Check J1 case zip and a built fview.exe with the lane 143 line; SKIPs without them, run directly it fails loudly):
  - the shipped tool on J1's LAST frame with `--plant thin_ignored` (the old behaviour: no --thin) exits 3, says THIN capture / INCOMPLETE and PROOF BAR FAIL: the bug is re-found (R);
  - the honest run (fview --thin) of the same frame has fewer than a third of the plant's not-learned draws (fview prints `not learned`), proving --thin is passed and works.
  py -I tools\census\thin_census_selftest.py [--zip PATH] [--unit-only]
"""
import argparse
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'census'))
import leftovers as L  # noqa: E402
import proof_bar as PB  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

problems = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        problems.append(what)


def fake_capture(root: Path, flags):
    fr = root / 'frames'
    fr.mkdir(parents=True)
    for i, fl in enumerate(flags):
        (fr / f'frame_{i:06d}.bin').write_bytes(struct.pack('<IIIIQII', 0x46524D45, 3, i, 0, 1000 + i, 1, fl) + b'\0' * 64)


def unit():
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        fake_capture(td / 'thin', [35, 35, 3, 35])
        fake_capture(td / 'plain', [3, 3, 3, 3])
        check(L.thin_frames(td / 'thin') == [0, 1, 3], 'unit: thin_frames finds the frames whose header flags carry kFrameHasThin (32)')
        check(L.thin_frames(td / 'plain') == [], 'unit: a capture without the flag is not thin')
        fake_capture(td / 'cleared', [35 & ~32] * 4)   # planted twin: the detector fed a capture whose flag is cleared
        check(L.thin_frames(td / 'cleared') == [], 'unit: planted (flag cleared): the same capture reads as not thin, so the detector is the flag')
        check(L.thin_arg(['crowd', 'arena', 'ring']) == ['arena', 'ring'] and L.thin_arg(['crowd']) == [], 'unit: thin_arg keeps only the promoted thin classes')
        m = L.THIN_LINE.search('x [fview] thin frame 300: 360 lite draws, 4 not learned (drawn by nothing)')
        check(bool(m) and m.groups() == ('300', '360', '4'), 'unit: the "thin frame" line parses (frame, lite, not learned)')
        base = (np.random.RandomState(1).rand(360, 640, 3) * 120).astype(np.uint8)
        Image.fromarray(base).save(td / 'live.png')
        Image.fromarray(base).save(td / 'same.png')
        holed = base.copy()
        holed[100:300, 200:400] = 0   # a ring-sized hole: the redraw missed a whole object
        holed[100:300, 200:400] = 255
        Image.fromarray(holed).save(td / 'holed.png')
        check(L.proof_bar(td / 'same.png', td / 'live.png')['ok'], 'unit: proof bar: an identical redraw passes')
        r = L.proof_bar(td / 'holed.png', td / 'live.png')
        check(not r['ok'] and r['guard_block'] > PB.GUARD_BLOCK, f"unit: proof bar: a redraw with a 200x200 hole fails the guard ({r.get('guard_block', 0):.0f} > {PB.GUARD_BLOCK:g})")
        old = PB.GUARD_BLOCK
        PB.GUARD_BLOCK = 1000.0   # planted twin: with the guard disabled the hole passes: the failure above is the guard's
        r2 = L.proof_bar(td / 'holed.png', td / 'live.png')
        PB.GUARD_BLOCK = old
        check(r2['ok'], 'unit: planted (guard limit 1000): the same hole passes, so the guard is what failed it')
        r3 = L.proof_bar(td / 'missing.png', td / 'live.png')
        check(not r3['ok'] and r3.get('unreadable'), 'unit: proof bar: a missing picture fails, never passes')


def live(zip_path, fview):
    base = Path(tempfile.mkdtemp(prefix='thin_census_selftest_', dir=str(HERE / 'out'))) if (HERE / 'out').is_dir() else Path(tempfile.mkdtemp())
    out = {}
    for plant in ('thin_ignored', 'emu_shared_tables', ''):   # lane 149: + the old fview emulation (the bind the picture comes from lost 47 arena draws, the spotlight beams among them)
        cmd = [sys.executable, '-I', str(HERE / 'tools' / 'census' / 'leftovers.py'), str(zip_path), '--frames', 'last', '--promote', 'crowd,arena,wrestlers,ring', '--grid', '8x4',
               '--out', str(base / (plant or 'thin')), '--name', 'thin_census_selftest', '--fview', str(fview)]
        if plant:
            cmd += ['--plant', plant]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=3000)
        out[plant] = (p.returncode, p.stdout + p.stderr, base / (plant or 'thin'))
    rc, txt, _ = out['thin_ignored']
    check(rc == 3 and 'THIN capture' in txt and 'INCOMPLETE' in txt and 'PROOF BAR FAIL' in txt,
          f'live: planted thin_ignored (the old behaviour) on J1: exit {rc}, says THIN capture / INCOMPLETE / PROOF BAR FAIL (R: the bug of run 7086 is re-found)')
    rc2, txt2, d2 = out['']
    m = re.search(r'frame \d+ INCOMPLETE: (\d+) of (\d+) lite', txt2)
    nl = int(m.group(1)) if m else 0
    check('replaying with fview --thin' in txt2 and nl * 3 < 111, f'live: the honest run passes --thin and learns the lite draws (not learned {nl}, the unthinned replay drops all of them)')
    # lane 149: the honest run's picture (any bind) loses nothing, so the redraw matches J1's picture.png; the planted old emulation is caught by the not-learned guard AND the bar (R: the missing spotlight beams)
    check(rc2 == 0 and 'PROOF BAR PASS' in txt2 and 'INCOMPLETE' not in txt2, f'live: the honest run on J1 passes the proof bar (exit {rc2}; the three spotlight beams are in the redraw)')
    rc3, txt3, _ = out['emu_shared_tables']
    check(rc3 == 3 and 'INCOMPLETE' in txt3 and 'PROOF BAR FAIL' in txt3, f'live: planted emu_shared_tables (the old fview emulation) on J1: exit {rc3}, INCOMPLETE + PROOF BAR FAIL (R: the beams of Check J1 re-found)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip')
    ap.add_argument('--unit-only', action='store_true')
    a = ap.parse_args()
    unit()
    if not a.unit_only:
        main_co = L.main_checkout()
        z = Path(a.zip) if a.zip else next(iter((main_co / 'out' / 's101' / 'runs' / '20261112_172304_s100_owner_s89_brock').glob('case_*.zip')), None)
        sys.path.insert(0, str(HERE / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources); lane 149: the dev build
        import fview_exe as fe
        try:
            fview = Path(fe.find_fview(root=HERE, kind='dev'))
        except fe.FviewError as _e:
            fview = HERE / 'out' / 'renderer' / 'dev' / 'fview.exe'
            problems.append(str(_e))
        if not z or not z.exists():
            print(f'SKIP live level: J1 zip {z} missing')
            if a.zip:
                problems.append('the named zip is missing')
        elif not fview.exists():
            problems.append(f'live level not run: no fview at {fview} (build it: pwsh -NoProfile -File tools\fview\build.ps1); --unit-only skips the live level on purpose')
        else:
            print(f'live level uses fview {fview} (built {time.strftime("%Y-%m-%d %H:%M", time.localtime(fview.stat().st_mtime))})')
            live(z, fview)
    if problems:
        print(f'FAIL: {len(problems)} problem(s)')
        for p in problems:
            print('  -', p)
        sys.exit(1)
    print('thin census selftest: all checks passed')


if __name__ == '__main__':
    main()

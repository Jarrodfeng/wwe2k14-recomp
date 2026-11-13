#!/usr/bin/env python3
"""S101 lane 59: the stage view's offline proof (gate lane fview). 0 game runs.

  py -I tools\\fview\\stage_view_check.py [--root DIR]

Input: the Check E bug-button zip (run 7066, frame 1979), kept in the MAIN checkout at out\\lane59\\case_stage_run7066.zip (a copy of
out\\build\\win-amd64-release\\out\\cases\\case_20261112_100646_780_0_pc_key.zip). In that frame the characters draw into surface 1074 (a SceneColor target
declared by its creator) and the presented surface 1 is a copy of the editor's red background.
 1 without --stage-view: surface 1 is NOT surface 1074's picture (today's picture).
 2 with --stage-view: surface 1's PNG is byte-identical to surface 1074's (md5 equal): the scene target is presented, 1980 frames, 0 missed.
 3 a capture whose images carry no declared role (s100cards, version 1): --stage-view finds no scene target: the picture is unchanged ("missed" > 0, 0 frames).
 4 PAIRING (review): a frame that is not a stage frame but has declared SceneColor targets (out\\lane59\\case_match_nonstage.zip, a match capture from before the stage push existed,
   24 declared SceneColor images): with the director saying "playing" (--stage-playing) but no frame stamped, NOTHING is replaced (frames 0). The planted twin: the same zip with the
   frame treated as stamped (--stage-view) DOES replace it (frames 1), so the per-frame stamp is what protects a real picture.
 5 TARGET PICK twins: --stage-pick-fault first_by_id (the lowest-id declared SceneColor) presents another target (693 / 1194, not 1074 / 1659) and the picture differs from the right one.
   --stage-pick-fault last_by_id picks the same target as the real rule in both zips (1074 and 1659 are the highest ids): no frame of these zips tells that rule apart (reported, not claimed).
Exit 1 on any failure; a missing input or tool FAILS (never passes on an empty read).
"""
import argparse
import hashlib
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
failures = []


def check(name, ok, evidence=''):
    print(('PASS  ' if ok else 'FAIL  ') + name + (': ' + evidence if evidence else ''), flush=True)
    if not ok:
        failures.append(name)


def fview(exe, args, tries=6):
    """Runs fview; retries only the shared-GPU 'Not enough graphics memory for the frame ring' message."""
    for _ in range(tries):
        p = subprocess.run([str(exe)] + [str(a) for a in args], capture_output=True, text=True, timeout=900)
        out = p.stdout + p.stderr
        if 'Not enough graphics memory' not in out:
            return p.returncode, out
        time.sleep(8)
    return 99, out


def md5(path):
    return hashlib.md5(Path(path).read_bytes()).hexdigest() if Path(path).exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(REPO))
    root = Path(ap.parse_args().root).resolve()
    sys.path.insert(0, str(Path(__file__).resolve().parent))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
    import fview_exe as fe
    exe = Path(fe.find_or_exit(root=root, kind='dev'))
    zip_path = MAIN / 'out' / 'lane59' / 'case_stage_run7066.zip'
    match_zip = MAIN / 'out' / 'lane59' / 'case_match_nonstage.zip'
    cards = MAIN / 'out' / 'captures' / 's100cards'
    work = MAIN / 'out' / 'lane59' / 'work'
    work.mkdir(parents=True, exist_ok=True)
    for need in (exe, zip_path, match_zip, cards):
        if not Path(need).exists():
            check('input ' + str(need), False, 'missing')
            return 1
    base = {}
    for tag, extra in (('base', []), ('sv', ['--stage-view'])):
        for sid in (1, 1074):
            out_png = work / f'{tag}_{sid}.png'
            out_png.unlink(missing_ok=True)
            code, out = fview(exe, ['--case', zip_path, '--frame', 1979] + extra + ['--image-png', f'{sid},{out_png}'])
            check(f'fview {tag} surface {sid} readable', code == 0 and out_png.exists(), f'exit {code}')
            base[(tag, sid)] = md5(out_png)
            if tag == 'sv' and sid == 1:
                m = re.search(r'\[rdr\]\[stage_view\] frames (\d+), missed (\d+)', out)
                check('stage view presented the scene target on every frame', bool(m) and int(m.group(1)) == 1980 and int(m.group(2)) == 0, m.group(0) if m else out[-300:])
                check('log names scene target 1074 over surface 1', 'presenting scene target 1074' in out and 'over surface 1 ' in out)
    check('1 without --stage-view surface 1 is not the scene picture (today)', base[('base', 1)] and base[('base', 1)] != base[('base', 1074)], f"{base[('base', 1)]} vs {base[('base', 1074)]}")
    check('2 with --stage-view surface 1 == surface 1074 (md5)', base[('sv', 1)] is not None and base[('sv', 1)] == base[('sv', 1074)] == base[('base', 1074)], str(base[('sv', 1)]))
    # 3. no declared role in the capture: nothing to present, the picture is the game's
    code, out = fview(exe, ['--capture', cards, '--frame', 1, '--stage-view', '--ops', '--json', work / 'cards_ops.json'])
    m = re.search(r'\[rdr\]\[stage_view\] frames (\d+), missed (\d+)', out)
    check('3 capture without declared roles: stage view finds no target, shows the game picture', code == 0 and bool(m) and int(m.group(1)) == 0 and int(m.group(2)) > 0, m.group(0) if m else out[-300:])
    # 4. pairing: playing (director) without a stamp replaces nothing; the stamped twin replaces the real picture
    code, out = fview(exe, ['--case', match_zip, '--frame', 0, '--stage-playing', '--ops', '--json', work / 'm_ops.json'])
    m = re.search(r'\[rdr\]\[stage_view\] frames (\d+), missed (\d+), last target (\d+)', out)
    check('4 director "playing" but the frame is not stamped: nothing replaced', code == 0 and bool(m) and int(m.group(1)) == 0 and int(m.group(3)) == 0, m.group(0) if m else out[-300:])
    code, out = fview(exe, ['--case', match_zip, '--frame', 0, '--stage-view', '--ops', '--json', work / 'm_ops.json'])
    m = re.search(r'\[rdr\]\[stage_view\] frames (\d+), missed (\d+), last target (\d+)', out)
    check('4 planted twin: the same non-stage frame treated as stamped IS replaced (the stamp is what protects it)', code == 0 and bool(m) and int(m.group(1)) == 1 and int(m.group(3)) > 0, m.group(0) if m else out[-300:])
    right_match = m.group(3) if m else None
    # 4b. the header stamp itself (kFrameStagePlaying = 16 in FrameFileHeader::flags, offset 28): a frame that carries it is replaced with the director's flag the only thing on,
    # the same frame without it is not. (The stamp is written by reader.h FrameBytes on the guest thread; here the bit is set in an unpacked copy of the frame file.)
    import os
    import struct
    import shutil
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    cap_dir = work / 'match_unpacked'
    shutil.rmtree(cap_dir, ignore_errors=True)
    cap_dir.mkdir(parents=True)
    r = subprocess.run([tar, '-xf', str(match_zip), '-C', str(cap_dir)], capture_output=True, text=True, timeout=900)
    frame_file = cap_dir / 'capture' / 'frames' / 'frame_000000.bin'
    check('4b unpacked the non-stage zip for the header test', r.returncode == 0 and frame_file.exists(), r.stderr[:200])
    if frame_file.exists():
        data = bytearray(frame_file.read_bytes())
        flags = struct.unpack_from('<I', data, 28)[0]
        for tag, value, want in (('unstamped', flags & ~16, 0), ('stamped', flags | 16, 1)):
            struct.pack_into('<I', data, 28, value)
            frame_file.write_bytes(bytes(data))
            code, out = fview(exe, ['--capture', cap_dir / 'capture', '--frame', 0, '--stage-playing', '--ops', '--json', work / 'm_ops.json'])
            m = re.search(r'\[rdr\]\[stage_view\] frames (\d+)', out)
            check(f'4b {tag} frame (header flags {value:#x}) with only the director flag on: replaced frames = {want}', code == 0 and bool(m) and int(m.group(1)) == want, m.group(0) if m else out[-300:])
    shutil.rmtree(cap_dir, ignore_errors=True)  # game data: not left behind
    # 5. the pick
    for label, zp, frame, right in (('stage zip', zip_path, 1979, '1074'), ('match zip', match_zip, 0, right_match)):
        got = {}
        for fault in ('', 'first_by_id', 'last_by_id'):
            png = work / f'pick_{label.split()[0]}_{fault or "real"}.png'
            png.unlink(missing_ok=True)
            args = ['--case', zp, '--frame', frame, '--stage-view'] + (['--stage-pick-fault', fault] if fault else []) + ['--image-png', f'1,{png}']
            code, out = fview(exe, args)
            m = re.search(r'last target (\d+)', out)
            got[fault] = (m.group(1) if m else None, md5(png))
        check(f'5 {label}: the real rule picks the target the frame drew into ({right})', got[''][0] == right, str(got['']))
        check(f'5 {label}: planted twin "lowest-id declared SceneColor" is caught (other target, other picture)', got['first_by_id'][0] != got[''][0] and got['first_by_id'][1] != got[''][1], str(got['first_by_id']))
        print(f'      note: "highest-id declared SceneColor" picks {got["last_by_id"][0]} here (real rule {got[""][0]}): ' + ('same, this frame cannot tell it apart' if got['last_by_id'][0] == got[''][0] else 'differs, caught'))
    print('FAILED: ' + ', '.join(failures) if failures else 'stage view check: all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

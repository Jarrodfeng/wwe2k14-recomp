#!/usr/bin/env python3
r"""S103 lane WB gate check (the scene-walk bit; docs\research\s103\PM_post_match_menu.md section 5 item 2). 0 game runs, a headless GPU, no game process.

  py -I tools\scene\walk_check.py [--root DIR]

Replays the post-match recording (out\live\main\cases\case_20261112_131244_709_0_pc_run_exit.zip, capture\scene\*.rsc: 782 records; the stream's last 3D frame is record 347, the record keeps a sticky
arena of 251 objects to record 781) through the SAME SceneHost the game runs (out\tools\host_rig.exe built by build_host_rig.ps1), with the stamps the stream-era capture lacks injected by the game's own
rule: records 0-346 carry an OURS Scene3D stamp with the walk bit (a Model / Crowd / Ring ask), records 347+ carry no Scene3D stamp (no hook asks in the menu).
  1. census: records 0 and 346 hold an arena, records 400 and 781 STILL hold the arena (the bug's input: the record never forgets it);
  2. `--walk-until 347 --frames 100,300,360,500,781`: 100 and 300 are DRAWN in 3D (source draw3d), 360 / 500 / 781 are flat (source black, never a drawn dark arena, never hold3d);
  3. twin `--walk-all`: the same records all carrying the walk bit must FAIL at 360 / 500 / 781 (a check that cannot fail proves nothing).
Skips with exit 0 (printed SKIP) when the case zip / game files / host_rig.exe are missing, the way a gate `needs` does.
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

CASE = os.path.join('out', 'live', 'main', 'cases', 'case_20261112_131244_709_0_pc_run_exit.zip')
FRAMES = '100,300,360,500,781'


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else root


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    zpath = os.path.join(main_co, CASE)
    game = os.path.join(main_co, 'game_data', 'extracted')
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    for what, p in (('case zip', zpath), ('game files', game), ('host_rig.exe (build_host_rig.ps1)', exe)):
        if not os.path.exists(p):
            print('SKIP: %s missing: %s' % (what, p))
            return 0
    work = os.path.join(root, 'out', 's103', 'wb')
    scene = os.path.join(work, 'scene')
    os.makedirs(scene, exist_ok=True)
    if len([f for f in os.listdir(scene) if f.endswith('.rsc')]) < 782:
        with zipfile.ZipFile(zpath) as z:
            for n in z.namelist():
                if n.startswith('capture/scene/frame_') and n.endswith('.rsc'):
                    with open(os.path.join(scene, os.path.basename(n)), 'wb') as f:
                        f.write(z.read(n))
    bad = 0

    def check(ok, what):
        nonlocal bad
        print(('PASS ' if ok else 'FAIL ') + what)
        if not ok:
            bad += 1

    rc, out = run([exe, '--mode', 'census', '--scene', scene, '--frames', '0,346,400,781'])
    arena = {}
    for ln in out.splitlines():
        m = re.match(r'rec (\d+) gf \d+ obj (\d+)', ln)
        if m:
            arena[int(m.group(1))] = ('arena:' in ln.split('| pacs:')[0]) if '| pacs:' in ln else ('arena' in ln)
    check(rc == 0 and all(arena.get(n) for n in (0, 346, 400, 781)), 'census: the record holds the arena at 0, 346, 400 and 781 (the input of the bug: it never forgets it): %s' % arena)
    base = [exe, '--mode', 'host', '--scene', scene, '--game', game, '--out', os.path.join(work, 'out'), '--frames', FRAMES, '--size', '640x360']
    rc, out = run(base + ['--walk-until', '347'])
    marks = re.findall(r'walk check frame (\d+) \((walked|no walk)\): (PASS|FAIL) \(source (\S+), wanted (\S+)\)', out)
    print('\n'.join(ln for ln in out.splitlines() if ln.startswith('walk check') or ln.startswith('host frame')))
    got = {int(m[0]): m for m in marks}
    check(rc == 0 and sorted(got) == [100, 300, 360, 500, 781], 'replay: exit 0 and a verdict for each of the five records (rc %d, frames %s)' % (rc, sorted(got)))
    check(all(got.get(n, ('', '', 'FAIL'))[2] == 'PASS' and got[n][3] == 'draw3d' for n in (100, 300)), 'replay: records 100 and 300 (walked) are drawn in 3D')
    check(all(got.get(n, ('', '', 'FAIL'))[2] == 'PASS' and got[n][3] == 'black' for n in (360, 500, 781)), 'replay: records 360, 500 and 781 (no walk, sticky arena) are flat: black under their rows, never a drawn arena, never hold3d')
    rc2, out2 = run(base + ['--walk-until', '347', '--walk-all'])
    fails = [int(m) for m in re.findall(r'walk check frame (\d+) \([^)]*\): FAIL', out2)]
    check(rc2 != 0 and set(fails) >= {360, 500, 781}, 'twin --walk-all (every record walked: the old rule) is caught at 360 / 500 / 781 (failed %s, rc %d)' % (sorted(fails), rc2))
    print('walk_check: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

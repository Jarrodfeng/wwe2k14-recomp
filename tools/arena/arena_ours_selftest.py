# Gate check of lane 40 (Session 101): our arena's unit test (arena_ours_test.exe) with its planted faults and loud failures.
#   py -I tools\arena\selftest.py [--zip CASE.zip] [--step N]
# Runs the SHIPPED core (src\renderer_scene\arena_ours_core.cpp, the code ArenaOurs calls) via out\tools\arena_ours_test.exe (built by
# tools\arena\build_arena_ours_test.ps1, which also compile-checks the GPU side arena_ours.cpp) on the files of arenas 4 and 5 and on Check B's capture
# (case zip of run 7051; the parts the test reads are unpacked once to <main checkout>\out\lane40\brock, never committed).
# Bars: (1) files-only and the clean capture run exit 0; (2) every planted fault exits 1 AND the named exact check is among the failures;
# (3) loud failures: a missing capture and a missing game folder exit 2, an unknown fault 3. Exit 0 = all hold, 1 = a bar failed.
import argparse, glob, re, shutil, subprocess, sys, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


MAIN = main_checkout()
fails = []


def check(name, ok, detail=''):
    print(('PASS  ' if ok else 'FAIL  ') + name + (f'  ({detail})' if detail else ''))
    if not ok:
        fails.append(name)


def run(exe, *args, timeout=900):
    p = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def failed_checks(out):
    return set(re.findall(r'^FAIL\s+(\S+)', out, re.M))


# fault -> a check that must be among its failures (the fault's own exact check)
FAULTS = {
    'vertex_swizzle': 'capture.colour_bytes_equal',
    'no_mirror': 'capture.both_sites',
    'first_texture': 'capture.textures_ours',
    'viewer_space': 'capture.joins_correct',  # nothing joins: the oracle names no draw, so oracle_all_replaced / part checks fail as well
    'file_lightmap': 'capture.textures_ours',
    'join_shift': 'capture.joins_correct',
    'no_group_rule': 'capture.draw_checks',
    'winding_flip': 'capture.triangles_equal_stream',
    'bone_off_by_one': 'capture.skin_bone_ids_equal',  # S101 lane 49
    'bone_zero': 'capture.skin_bone_ids_equal',
}
# viewer_space: with the Y mirror kept no stream position matches any mesh; the oracle finds nothing, so the exact check is "something was compared at all"
FAULTS['viewer_space'] = 'capture.mesh_space'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip', default='')
    ap.add_argument('--step', type=int, default=4)
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'arena_ours_test.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\arena\\build_arena_ours_test.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    cap = MAIN / 'out' / 'lane40' / 'brock' / 'capture'
    if not (cap / 'frames').exists() or not (cap / 'textures.bin').exists():
        zips = [args.zip] if args.zip else sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / '20261111_215201_s100_owner_s89_brock' / 'case_*.zip')))
        if not zips:
            print('FAIL  Check B case zip not found (out\\s101\\runs\\20261111_215201_s100_owner_s89_brock\\case_*.zip)')
            return 1
        with zipfile.ZipFile(zips[-1]) as z:
            names = [n for n in z.namelist() if n.startswith('capture/')]
            if not names:
                print(f'FAIL  {zips[-1]} holds no capture/')
                return 1
            tmp = cap.parent
            shutil.rmtree(tmp, ignore_errors=True)
            tmp.mkdir(parents=True)
            for n in names:
                z.extract(n, tmp)
        print(f'INFO  unpacked {len(names)} capture files to {cap}')
    st = str(args.step)

    rc, out, err = run(exe, '--game', game, '--files')
    check('files.clean', rc == 0 and 'FAIL' not in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--threads')
    check('threads.two_loaders', rc == 0 and 'PASS  threads.two_loaders' in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--files', '--fault', 'viewer_space')
    check('files.viewer_space_fails', rc == 1 and 'FAIL  files.arena4.game_space' in out, f'exit {rc}')

    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st)
    check('capture.clean', rc == 0 and 'RESULT OK' in out, f'exit {rc}')
    if rc != 0:
        print(out)
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--no-image-info')
    check('capture.no_image_info_keeps_stream_image', rc == 0 and 'PASS  capture.no_image_info_keeps_stream_image' in out, f'exit {rc}')
    # S101 lane 47: the live image-info supplier. Clean: it answers exactly what the capture reader answers (every id), survives a concurrent refresh, and `size unchecked` is 0.
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st)
    check('supplier.clean', rc == 0 and 'PASS  capture.supplier_equals_capture' in out and 'PASS  capture.supplier_threads' in out and 'PASS  capture.image_info_checked  (size unchecked 0)' in out, f'exit {rc}')
    # planted twins: a supplier fed a wrong table must fail its own exact check
    for sf in ('shrink', 'drop', 'shift'):
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--supplier-fault', sf)
        got = failed_checks(out)
        check(f'supplier_fault.{sf}', rc == 1 and 'capture.supplier_equals_capture' in got, f'exit {rc}, fails {sorted(got)}')
    # live-style geometry (run 7067: a key is not an address; decoy blobs sit where offset arithmetic would land) must replace with 0 failed checks; one bad draw keeps only itself
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--live-style')
    check('capture.live_style_geometry', rc == 0 and 'RESULT OK' in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--corrupt-one', '--frames', '200-200')
    check('capture.one_bad_draw_keeps_only_itself', rc == 0 and 'PASS  corrupt_one.only_itself' in out, f'exit {rc}')
    # S101 lane 88: the proof frames' planted twin. With them off, a blob corrupted in place after the pin goes undetected and the detection check must FAIL.
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--corrupt-one', '--frames', '200-200', '--fault', 'no_proof_frames')
    got = failed_checks(out)
    check('fault.no_proof_frames', rc == 1 and 'corrupt_one.in_place_detected' in got and 'corrupt_one.only_itself' not in got, f'exit {rc}, fails {sorted(got)}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--corrupt-one', '--frames', '200-200')
    check('capture.proof_frames_detect_in_place', rc == 0 and 'PASS  corrupt_one.in_place_detected' in out and 'PASS  corrupt_one.proof_period_steady' in out, f'exit {rc}')
    for fault, must in FAULTS.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--fault', fault)
        got = failed_checks(out)
        check(f'fault.{fault}', rc == 1 and must in got, f'exit {rc}, fails {sorted(got)}')

    rc, out, err = run(exe, '--game', game, '--capture', MAIN / 'out' / 'lane40' / 'no_such_capture')
    check('loud.missing_capture', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', MAIN / 'out' / 'lane40' / 'no_such_game', '--capture', cap)
    check('loud.missing_game', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--fault', 'nonsense')
    check('loud.unknown_fault', rc == 3, f'exit {rc}')
    print('RESULT ' + ('OK' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

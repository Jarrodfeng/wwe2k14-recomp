# Gate check of lane 49 (Session 101): the skinned prop host vertex writer (src\assets\prop_host_vertex.h) against Check B's vertex bytes.
#   py -I tools\ring\prop_vertex_selftest.py [--step N]
# Runs out\tools\prop_vertex_check.exe (built by tools\ring\build_prop_vertex_check.ps1) on the capture of Check B (case zip of run 7051, unpacked once to <main checkout>\out\lane40\brock,
# the same folder the arena's unit test uses; never committed). Bars: the clean run exits 0; each planted writer fault (colour_order, uv_halves_swapped, bone_zero, weight_word)
# exits 1 AND its own exact check is among the failures; a missing capture / game folder exits 2, an unknown fault 3. Exit 0 = all hold.
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


PLANTS = {'colour_order': 'prop.colour_exact', 'uv_halves_swapped': 'prop.uv_exact', 'bone_zero': 'prop.ids_exact', 'weight_word': 'prop.weights_exact'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', type=int, default=4)
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'prop_vertex_check.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\ring\\build_prop_vertex_check.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    cap = MAIN / 'out' / 'lane40' / 'brock' / 'capture'
    if not (cap / 'frames').exists() or not (cap / 'textures.bin').exists():
        zips = sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / '20261111_215201_s100_owner_s89_brock' / 'case_*.zip')))
        if not zips:
            print('FAIL  Check B case zip not found')
            return 1
        with zipfile.ZipFile(zips[-1]) as z:
            names = [n for n in z.namelist() if n.startswith('capture/')]
            tmp = cap.parent
            shutil.rmtree(tmp, ignore_errors=True)
            tmp.mkdir(parents=True)
            for n in names:
                z.extract(n, tmp)
    st = str(args.step)
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st)
    check('prop.clean', rc == 0 and 'RESULT OK' in out and re.search(r'PASS  prop.compared  \((\d+) draws', out) is not None, f'exit {rc}')
    if rc != 0:
        print(out)
    for fault, must in PLANTS.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--fault', fault)
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
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

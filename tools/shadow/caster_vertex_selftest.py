# Gate check of lane 85 = SV (Session 101): the shadow-layout vertex writers (src\assets\shadow_caster_vertex.h) against the shadow-pass vertex bytes of two captures.
#   py -I tools\shadow\caster_vertex_selftest.py [--step N]
# Runs out\tools\caster_vertex_check.exe (built by tools\shadow\build_caster_vertex_check.ps1) on
#   Check B (case zip of run 7051, unpacked once to <main checkout>\out\lane40\brock; the u of the stream is |u|, before SDK patch 109) and
#   run 7079's case zip (<main checkout>\out\s101\runs\20261112_122623_s100_owner_s89_brock, unpacked once to <main checkout>\out\lane85\r7079; the u is signed).
# Both are game data: they stay under out\, never committed.
# Bars: (1) the clean run of each capture exits 0 and every wrestler / ring-956 / chCommon caster draw is byte-equal; the u-sign rule was LEARNED and differs between the two;
# (2) each planted fault (fetch_order, weights_as_fill, xdword_colour, bone_zero, uv_sign) exits 1 on BOTH captures AND fails exactly its own checks (and not the others);
# (3) loud failures: a missing capture and a missing game folder exit 2, an unknown fault 3. Exit 0 = all hold.
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
    p = subprocess.run([str(exe), *[str(a) for a in args]], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def unpack(dest, run_dir):
    cap = MAIN / 'out' / dest / 'capture'
    if (cap / 'frames').exists() and (cap / 'textures.bin').exists():
        return cap
    zips = sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / run_dir / 'case_*.zip')))
    if not zips:
        return None
    shutil.rmtree(cap.parent, ignore_errors=True)
    cap.parent.mkdir(parents=True)
    with zipfile.ZipFile(zips[-1]) as z:
        names = [n for n in z.namelist() if n.startswith('capture/')]
        for n in names:
            z.extract(n, cap.parent)
    return cap


# fault -> the exact checks that fail (bytes_equal fails with every writer fault that moves a shadow-layout byte; uv_sign moves none: the shadow layouts carry no uv)
PLANTS = {
    'fetch_order': {'sv.fill_exact', 'sv.xdword_exact', 'sv.bytes_equal'},
    'weights_as_fill': {'sv.weights_exact', 'sv.bytes_equal'},
    'xdword_colour': {'sv.xdword_exact', 'sv.bytes_equal'},
    'bone_zero': {'sv.ids_exact', 'sv.bytes_equal'},
    'uv_sign': {'sv.uv_exact'},
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', type=int, default=1, help='frame step of the clean runs (plants use 4x this)')
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'caster_vertex_check.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\shadow\\build_caster_vertex_check.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    caps = {'brock7051': unpack('lane40/brock', '20261111_215201_s100_owner_s89_brock'), 'run7079': unpack('lane85/r7079', '20261112_122623_s100_owner_s89_brock')}
    for k, v in caps.items():
        if v is None:
            print(f'FAIL  case zip for {k} not found')
            return 1
    expect_rule = {'brock7051': '|u|', 'run7079': 'signed'}
    for name, cap in caps.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', args.step)
        m = re.search(r'PASS\s+sv.compared\s+\(wrestler (\d+) / ring956 (\d+) / chcommon (\d+) draws', out)
        check(f'{name}.clean', rc == 0 and 'RESULT OK' in out and m is not None and all(int(x) > 0 for x in m.groups()), f'exit {rc}' + (f', draws {m.groups()}' if m else ''))
        if rc != 0:
            print(out)
        learned = re.search(r'rule learned from the stream: (\S+),', out)
        check(f'{name}.uv_rule_learned', learned is not None and learned.group(1) == expect_rule[name], f'learned {learned.group(1) if learned else None}, expected {expect_rule[name]}')
        for fault, must in PLANTS.items():
            rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', args.step * 4, '--fault', fault)
            got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
            check(f'{name}.fault.{fault}', rc == 1 and got == must, f'exit {rc}, fails {sorted(got)}, expected {sorted(must)}')
    cap = caps['brock7051']
    rc, out, err = run(exe, '--game', game, '--capture', MAIN / 'out' / 'lane85' / 'no_such_capture')
    check('loud.missing_capture', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', MAIN / 'out' / 'lane85' / 'no_such_game', '--capture', cap)
    check('loud.missing_game', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--fault', 'nonsense')
    check('loud.unknown_fault', rc == 3, f'exit {rc}')
    print('RESULT ' + ('OK' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

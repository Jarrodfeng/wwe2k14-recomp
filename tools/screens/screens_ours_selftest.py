# Gate check of lane 115 (S101): the unit test of our stage screens (screens_ours_test.exe) with its planted faults and loud failures.
#   py -I tools\screens\screens_ours_selftest.py [--step N]
# Runs the SHIPPED core (src\renderer_scene\screens_ours_core.cpp, the code ScreensOurs calls) via out\tools\screens_ours_test.exe (built by tools\screens\build_screens_ours_test.ps1, which also compile-checks the GPU
# side screens_ours.cpp) on the arena files and on four captures: Check F (run 7067's case zip, scene records, consumer palettes), Check B (the folder lane 40 unpacked, or run 7051's zip), run 7074 ("h1": the stream
# holds SIGNED u) and run 7079 (the owner Brock run). The captures are unpacked once to <main checkout>\out\lane115\<name>\capture; never committed.
# Bars: (1) the files-only run and every clean capture run exit 0 (4 draws a frame at frame 200, keys joined {802, 804, 805, 807}: the pairs 801/802, 803/804 and 806/807 differ in the uv stream (S2), so content tells them apart);
# the live-style run (key store with decoys) too; (2) each planted fault exits 1 AND its own exact check is among the failures; (3) loud failures: a missing capture and game folder exit 2, an unknown fault 3.
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


def run(exe, *args, timeout=1500):
    p = subprocess.run([str(exe), *[str(a) for a in args]], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def unpack(name, run_dir):
    cap = MAIN / 'out' / 'lane115' / name / 'capture'
    if (cap / 'frames').exists() and (cap / 'textures.bin').exists():
        return cap
    zips = sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / run_dir / 'case_*.zip')))
    if not zips:
        return None
    shutil.rmtree(cap.parent, ignore_errors=True)
    cap.parent.mkdir(parents=True)
    with zipfile.ZipFile(zips[-1]) as z:
        z.extractall(cap.parent)
    return cap


# fault -> checks of which at least one must be among its failures (the core verifies bytes itself, so a writer fault shows as draws kept: oracle_all_replaced / draw_checks)
PLANTS = {
    'colour_order': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'uv_halves_swapped': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'pair_swap': ['capture.draw_checks', 'capture.oracle_all_replaced'],   # 801 joined as 802 (and 804 as 803, 807 as 806): the uv stream differs, the byte compare refuses it
    # no_verify: with the byte compare off a writer / join fault reaches the replaced draws: our bytes differ from the stream's (the independent memcmp)
    'uv_halves_swapped,no_verify': ['capture.vertex_bytes_equal_stream'],
    'pair_swap,no_verify': ['capture.vertex_bytes_equal_stream'],
    'colour_order,no_verify': ['capture.vertex_bytes_equal_stream'],
    'class_refuse': ['capture.draw_checks', 'capture.oracle_all_replaced'],   # lane 115: the stream's fragment forced to yDefault_base_fragment: every draw kept, class refused counted
    'file_effect': ['capture.draw_checks', 'capture.oracle_all_replaced'],    # lane 115: the file's effect read as non-yBG: same
    'frozen_clock': ['clock.foreign_expires'],                                # lane 115 F2
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', type=int, default=12)
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'screens_ours_test.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\screens\\build_screens_ours_test.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    caps = {}
    for name, run_dir in (('checkF', '20261112_101633_s100_owner_s89_brock'), ('brock', '20261111_215201_s100_owner_s89_brock'), ('h1', '20261112_121740_s100_owner_s89_brock'), ('r7079', '20261112_122623_s100_owner_s89_brock')):   # h1 = run 7074, post SDK patch 109: SIGNED u
        cap = unpack(name, run_dir)
        if cap is None:
            print(f'FAIL  capture {name}: case zip not found')
            return 1
        caps[name] = cap
    st = str(args.step)

    rc, out, err = run(exe, '--game', game, '--files', '--arena', 4, '--arena', 29)
    pairs_ok = all(re.search(p, out) for p in (r'pair 801/802 part 0: whole S0 equal yes; differs in: S2\(uv\)', r'pair 806/807 part 0: whole S0 equal yes; differs in: S2\(uv\)', r'pair 803/804 part 0: whole S0 equal no; differs in: S0\(pos/normal/colour\), S2\(uv\)'))
    check('files.clean', rc == 0 and out.count('PASS') >= 6, f'exit {rc}')
    check('files.pairs_measured', pairs_ok, 'the pairs differ in the uv stream (801/802, 806/807) and in normals / colour + uv (803/804)')
    if rc or not pairs_ok:
        print(out)
    for name, cap in caps.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st)
        m = re.search(r'RESULT OK fault=none frames=(\d+) replaced=(\d+)', out)
        check(f'capture.{name}.clean', rc == 0 and m is not None and int(m.group(2)) > 0, f'exit {rc}, {m.group(0) if m else "no RESULT OK"}')
        if rc:
            print(out)
    cap = caps['checkF']
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200')
    check('capture.checkF.frame200', rc == 0 and 'frame 200: 4 draws' in out and 'keys joined: 802 804 805 807' in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '150-250', '--live-style')
    check('capture.checkF.live_style', rc == 0 and 'RESULT OK' in out and 'PASS  capture.live_reads_first_use_only' in out, f'exit {rc}')
    if rc:
        print(out)
    # the u-sign plant: the opposite rule must be refused on both known rules (|u| before patch 109, signed after)
    uv_failed = {}
    for cname in ('checkF', 'brock', 'h1'):
        rc, out, err = run(exe, '--game', game, '--capture', caps[cname], '--frames', '200-200', '--fault', 'uv_sign')
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        rule = re.search(r'u sign rule: ([^;\n]*)', out)
        uv_failed[cname] = (rc == 1 and ('capture.draw_checks' in got or 'capture.oracle_all_replaced' in got), rule.group(1) if rule else '?')
        print(f'INFO  uv_sign plant on {cname}: failed={uv_failed[cname][0]} rule {uv_failed[cname][1]}')
    check('fault.uv_sign', uv_failed['checkF'][0] and uv_failed['h1'][0], f'failed on {[k for k, v in uv_failed.items() if v[0]]}; rules {[v[1] for v in uv_failed.values()]}')
    for fault, must in PLANTS.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200', '--fault', fault)
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        check(f'fault.{fault}', rc == 1 and any(x in got for x in must), f'exit {rc}, fails {sorted(got)}')
    # lane 115 F1: the ribbon band (0xAAE0B0F0, pair 806 / 807) alone, first, on a SIGNED-u capture (run 7074): the rule is learned from the pair without believing 806's clear sign bit at vertex 72
    # (807's u there is +1e-10), and 807 joins. The plant uv_from_pair is the old defect and must fail it.
    rc, out, err = run(exe, '--game', game, '--capture', caps['h1'], '--only-object', '0xAAE0B0F0', '--frames', '200-200', '--expect-uv', 2, '--expect-key', 807)
    check('capture.h1.ribbon_pair_signed', rc == 0 and 'RESULT OK' in out, f'exit {rc}')
    if rc:
        print(out)
    rc, out, err = run(exe, '--game', game, '--capture', caps['h1'], '--only-object', '0xAAE0B0F0', '--frames', '200-200', '--expect-uv', 2, '--expect-key', 807, '--fault', 'uv_from_pair')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.uv_from_pair', rc == 1 and 'expect.uv_rule' in got and 'expect.key' in got, f'exit {rc}, fails {sorted(got)}')
    # lane 115: arena_switch: A -> B -> A rebuilds the joins, the foreign verdicts, the rule and the buffers; the plant stale_switch keeps them
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '150-250', '--arena-switch')
    check('capture.checkF.arena_switch', rc == 0 and 'PASS  switch.state_rebuilt' in out, f'exit {rc}')
    if rc:
        print(out)
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '150-250', '--arena-switch', '--fault', 'stale_switch')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.stale_switch', rc == 1 and 'switch.state_rebuilt' in got, f'exit {rc}, fails {sorted(got)}')
    # lane 115 F5: the census over every bg pac: arenas without screens (23, 31, 32, 66-68, 76-80) are INFO, never FAIL
    rc, out, err = run(exe, '--game', game, '--files', '--all-arenas')
    check('files.all_arenas', rc == 0 and 'INFO  arena 23: no screens' in out and 'INFO  arena 80: no screens' in out and 'FAIL' not in out, f'exit {rc}')
    # wrong_arena: another arena's screens loaded (arena 29 holds 803/804 only): the screens of the capture's arena are not found
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200', '--wrong-arena', 29)
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.wrong_arena', rc == 1 and 'capture.draws_per_frame' in got and 'capture.screens_replaced' in got, f'exit {rc}, fails {sorted(got)}')
    rc, out, err = run(exe, '--game', game, '--capture', MAIN / 'out' / 'lane115' / 'no_such_capture')
    check('loud.missing_capture', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', MAIN / 'out' / 'lane115' / 'no_such_game', '--capture', cap)
    check('loud.missing_game', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--fault', 'nonsense')
    check('loud.unknown_fault', rc == 3, f'exit {rc}')
    print('RESULT ' + ('OK' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

# Gate check of lane 70 (Session 101): our ring's unit test (ring_ours_test.exe) with its planted faults and loud failures.
#   py -I tools\ring\ring_ours_selftest.py [--step N]
# Runs the SHIPPED core (src\renderer_scene\ring_ours_core.cpp, the code RingOurs calls) via out\tools\ring_ours_test.exe (built by tools\ring\build_ring_ours_test.ps1, which also
# compile-checks the GPU side ring_ours.cpp) on the ring files of arenas 4 and 29 and on three captures: Check B (case zip of run 7051, arena 4, scene records), Bryan's entrance (arena 4)
# and wm30 (arena 29). The captures are unpacked once to <main checkout>\out\lane70\<name>\capture (Check B: the folder lane 40 unpacked, out\lane40\brock); never committed.
# Bars: (1) files-only and every clean capture run exit 0, the live-style run (key store with decoys, first-use verification, no tie texture) too; (2) each of the eight planted faults of the map
# section 4 exits 1 AND its own exact check is among the failures; (3) loud failures: a missing capture and a missing game folder exit 2, an unknown fault 3.
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
    cap = MAIN / 'out' / 'lane70' / name / 'capture'
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
    'frozen_clock': ['clock.foreign_expires'],   # lane 115 F2: NoClaimFrame does not tick the core's clock: a foreign verdict never expires on frames where nothing is claimed
    'colour_order': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'uv_halves_swapped': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'uv_sign': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'bone_zero': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'weight_word': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'skirt_tie': ['capture.textures_equal_stream'],
    'part_only_join': ['capture.draw_checks', 'capture.oracle_all_replaced', 'capture.instances'],
    'lod_pin': ['capture.draw_checks', 'capture.oracle_all_replaced', 'capture.instances'],
    # F4: with the byte compare off a writer fault reaches the replaced draws: our bytes differ from the stream's (the independent memcmp)
    'uv_halves_swapped,no_verify': ['capture.vertex_bytes_equal_stream'],
    'colour_order,no_verify': ['capture.vertex_bytes_equal_stream'],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', type=int, default=12)
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'ring_ours_test.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\ring\\build_ring_ours_test.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    caps = {}
    for name, run_dir, arena in (('brock', '20261111_215201_s100_owner_s89_brock', None), ('bryan', '20261111_174549_s100_owner_bryan_entrance', 4), ('wm30', '20261111_174707_s100_owner_s90_wm30', 29), ('h1', '20261112_121740_s100_owner_s89_brock', None), ('wm30b', '20261112_131029_s100_owner_s90_wm30', 29)):   # lane 137: wm30b = run 7084 (the T_Lambert rope proxy, --expect-proxy); h1 = run 7074, post SDK patch 109: the stream holds SIGNED u
        if name == 'brock':
            cap = MAIN / 'out' / 'lane40' / 'brock' / 'capture'
            if not (cap / 'frames').exists():
                cap = unpack('brock', run_dir)
        else:
            cap = unpack(name, run_dir)
        if cap is None:
            print(f'FAIL  capture {name}: case zip not found')
            return 1
        caps[name] = (cap, arena)
    st = str(args.step)

    rc, out, err = run(exe, '--game', game, '--files')
    check('files.clean', rc == 0 and out.count('PASS') >= 8, f'exit {rc}')
    if rc:
        print(out)
    for name, (cap, arena) in caps.items():
        extra = ['--arena', arena] if arena is not None else []
        extra += ['--expect-proxy'] if name == 'wm30b' else []   # lane 137
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, *extra)
        m = re.search(r'RESULT OK fault=none frames=(\d+) replaced=(\d+)', out)
        check(f'capture.{name}.clean', rc == 0 and m is not None and int(m.group(2)) > 0, f'exit {rc}, {m.group(0) if m else "no RESULT OK"}')
        if rc:
            print(out)
    cap, arena = caps['brock']
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--live-style', '--no-content')
    check('capture.brock.live_style', rc == 0 and 'RESULT OK' in out, f'exit {rc}')
    # S101 lane 88: the proof frames. A blob inverted in place after the pin (same key) is found within 32 frames; the planted twin no_proof_frames never finds it, and then ONLY that check fails.
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', '1', '--frames', '100-300', '--live-style', '--no-content', '--corrupt-at', '150')
    check('capture.proof_frames_detect_in_place', 'PASS  capture.in_place_detected' in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', '1', '--frames', '100-300', '--live-style', '--no-content', '--corrupt-at', '150', '--fault', 'no_proof_frames')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.no_proof_frames', rc == 1 and got == {'capture.in_place_detected'}, f'exit {rc}, fails {sorted(got)}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--content-fault')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('plant.content_fault', rc == 1 and 'capture.textures_equal_stream' in got, f'exit {rc}, fails {sorted(got)} (no slot may be ours when no stream image matches its hash)')
    # the u-sign plant in BOTH directions: Check B (|u| stream) and run 7074 (signed stream) each reject the opposite rule
    for cname in ('brock', 'h1'):
        rc, out, err = run(exe, '--game', game, '--capture', caps[cname][0], '--step', st, '--fault', 'uv_sign')
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        check(f'fault.uv_sign.{cname}', rc == 1 and 'capture.draw_checks' in got, f'exit {rc}, fails {sorted(got)}')
        want = 'signed' if cname == 'h1' else '|u|'
        rc, out, err = run(exe, '--game', game, '--capture', caps[cname][0], '--step', st)
        check(f'uv_rule.{cname}', rc == 0 and f'u sign rule: {want}' in out or (cname == 'h1' and rc == 0 and 'u sign rule: signed' in out), f'learned from the stream: want {want}')
    for fault, must in PLANTS.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st, '--fault', fault)
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        check(f'fault.{fault}', rc == 1 and any(x in got for x in must), f'exit {rc}, fails {sorted(got)}')
    # lane 137 (N9) plant: the T_Lambert rope shadow proxy joined to the wrong entry (the low rope): its draws are then not ours and the proxy checks fail
    rc, out, err = run(exe, '--game', game, '--capture', caps['wm30b'][0], '--step', st, '--arena', 29, '--expect-proxy', '--fault', 'proxy_wrong')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.proxy_wrong.wm30b', rc == 1 and ('capture.proxy_lambert' in got or 'capture.oracle_all_replaced' in got), f'exit {rc}, fails {sorted(got)}')
    # lane 137 review 2: the Lambert site is the proxy only, one join per object across both sites. Clean: a canvas draw re-tagged to the Lambert site stays NotOurs and the colour-site draws stay ours.
    w30 = caps['wm30b'][0]
    rc, out, err = run(exe, '--game', game, '--capture', w30, '--step', st, '--arena', 29, '--expect-proxy', '--retag-canvas', '--frames', '132-667')
    check('capture.wm30b.retag_isolated', rc == 0 and 'PASS  capture.retag_isolated' in out, f'exit {rc}')
    for fault, must in (('lambert_guard_off', 'capture.retag_isolated'),   # the guard removed: the re-tagged canvas draw is replaced at the wrong site
                        ('lambert_off', 'capture.proxy_lambert'),          # the real re-find: the Lambert site not claimed (the state before the promotion)
                        ('bone_zero', 'capture.proxy_lambert'),            # a byte-level plant: the proxy's bone ids written 0
                        ('uv_halves_swapped', 'capture.proxy_lambert')):   # the same, uv halves exchanged
        rc, out, err = run(exe, '--game', game, '--capture', w30, '--step', st, '--arena', 29, '--expect-proxy', '--retag-canvas', '--frames', '132-667', '--fault', fault)
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        check(f'fault.{fault}.wm30b', rc == 1 and must in got, f'exit {rc}, fails {sorted(got)}')
    rc, out, err = run(exe, '--game', game, '--capture', MAIN / 'out' / 'lane70' / 'no_such_capture')
    check('loud.missing_capture', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', MAIN / 'out' / 'lane70' / 'no_such_game', '--capture', cap)
    check('loud.missing_game', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--fault', 'nonsense')
    check('loud.unknown_fault', rc == 3, f'exit {rc}')
    print('RESULT ' + ('OK' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

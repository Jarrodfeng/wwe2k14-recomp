# Gate check of lane 84 (S101): the unit test of our props of other pacs (props_ours_test.exe) with its planted faults and loud failures.
#   py -I tools\props\props_ours_selftest.py [--step N]
# Runs the SHIPPED core (src\renderer_scene\props_ours_core.cpp, the code PropsOurs calls) via out\tools\props_ours_test.exe (built by tools\props\build_props_ours_test.ps1, which also compile-checks the GPU
# side props_ours.cpp) on the prop files and on three captures: Check F (run 7067's case zip, scene records, consumer palettes), Check B (the folder lane 40 unpacked) and the post-u-fix run 7074 ("h1": the stream
# holds SIGNED u). The captures are unpacked once to <main checkout>\out\lane84\<name>\capture; never committed.
# Bars: (1) files-only and every clean capture run exit 0 (17 table / 3 microphone / 10 step leaves; per frame table 7 + mic 4 + steps 6 = 17 draws, joins {steps: the 32 family, table: PACH 305, mic: PACH 4}, bytes equal);
# the live-style run (key store with decoys, no content feed) too; (2) each planted fault exits 1 AND its own exact check is among the failures; (3) loud failures: a missing capture and game folder exit 2, an unknown fault 3.
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
    cap = MAIN / 'out' / 'lane84' / name / 'capture'
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
    # bone_zero is not planted: every prop vertex uses palette slot 0 [C: Check F / B / run 7074], so the fault writes the real bytes and cannot fail anything
    'weight_word': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'counts_only_join': ['capture.draw_checks', 'capture.oracle_all_replaced', 'capture.joins_correct'],   # R: the mic joined by counts alone is PACH 301 (positions refuse it)
    'wrong_table_variant': ['capture.textures_equal_stream', 'capture.joins_correct'],
    'steps_position_swap': ['capture.draw_checks', 'capture.oracle_all_replaced'],
    'all_mips': ['capture.textures_equal_stream'],
    'lookalike': ['capture.oracle_all_replaced'],   # + capture.mismatch_bounded must PASS (checked below): an object whose bytes differ is given up, not retried (WM30 run 7084)
    # no_verify: with the byte compare off a writer fault reaches the replaced draws: our bytes differ from the stream's (the independent memcmp)
    'uv_halves_swapped,no_verify': ['capture.vertex_bytes_equal_stream'],
    'steps_position_swap,no_verify': ['capture.vertex_bytes_equal_stream'],
    'counts_only_join,no_verify': ['capture.vertex_bytes_equal_stream', 'capture.joins_correct'],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', type=int, default=12)
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'props_ours_test.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\props\\build_props_ours_test.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    caps = {}
    for name, run_dir in (('checkF', '20261112_101633_s100_owner_s89_brock'), ('brock', '20261111_215201_s100_owner_s89_brock'), ('h1', '20261112_121740_s100_owner_s89_brock'), ('wm30r7084', '20261112_131029_s100_owner_s90_wm30'), ('victory7133', '20261113_063621_s101_sweep_victory')):   # h1 = run 7074, post SDK patch 109: SIGNED u
        if name == 'brock':
            cap = MAIN / 'out' / 'lane40' / 'brock' / 'capture'
            if not (cap / 'frames').exists():
                cap = unpack('brock', run_dir)
        else:
            cap = unpack(name, run_dir)
        if cap is None:
            print(f'FAIL  capture {name}: case zip not found')
            return 1
        caps[name] = cap
    st = str(args.step)

    rc, out, err = run(exe, '--game', game, '--files')
    check('files.clean', rc == 0 and out.count('PASS') >= 5, f'exit {rc}')
    if rc:
        print(out)
    for name, cap in caps.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--step', st)
        m = re.search(r'RESULT OK fault=none frames=(\d+) replaced=(\d+)', out)
        check(f'capture.{name}.clean', rc == 0 and m is not None and int(m.group(2)) > 0, f'exit {rc}, {m.group(0) if m else "no RESULT OK"}')
        if rc:
            print(out)
    cap = caps['checkF']
    # the acceptance frame itself: 17 draws (table 7, mic 4, steps 6)
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200')
    check('capture.checkF.frame200', rc == 0 and 'frame 200: 17 draws' in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200', '--live-style', '--no-content')
    check('capture.checkF.live_style', rc == 0 and 'RESULT OK' in out, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200', '--live-style')   # the live path WITH the content feed (review: the live-style arm used to run only without it)
    check('capture.checkF.live_style_content', rc == 0 and 'RESULT OK' in out and 'content: checks' in out and 'ok 0 ' not in out.split('content: checks')[1][:30], f'exit {rc}')
    if rc:
        print(out)
    # review F3: the sources in the opposite order: the joins must still be {table 305, mic 4, steps 32 family} (the tie-break by texture content decides, not the pac's order)
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200', '--reverse-sources')
    check('capture.checkF.reverse_sources', rc == 0 and 'RESULT OK' in out and '[evtobj PACH 305]' in out and '[evtobj PACH 4]' in out, f'exit {rc}')
    if rc:
        print(out)
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200', '--content-fault')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('plant.content_fault', rc == 1 and 'capture.textures_equal_stream' in got, f'exit {rc}, fails {sorted(got)} (no slot may be ours when no stream image matches its hash: the check wants slots compared, 0 are)')
    # the u-sign plant: the opposite rule must be refused; it must fail on at least one capture (review F4: "not known" no longer passes), and on both known rules (|u| before patch 109, signed after)
    uv_failed = {}
    for cname in ('checkF', 'brock', 'h1'):
        rc, out, err = run(exe, '--game', game, '--capture', caps[cname], '--frames', '200-200', '--fault', 'uv_sign')
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        rule = re.search(r'u sign rule: ([^;]*)', out)
        uv_failed[cname] = (rc == 1 and ('capture.draw_checks' in got or 'capture.oracle_all_replaced' in got), rule.group(1) if rule else '?')
        print(f'INFO  uv_sign plant on {cname}: failed={uv_failed[cname][0]} rule {uv_failed[cname][1]}')
    check('fault.uv_sign', any(v[0] for v in uv_failed.values()) and uv_failed['checkF'][0] and uv_failed['h1'][0], f'failed on {[k for k, v in uv_failed.items() if v[0]]}; rules {[v[1] for v in uv_failed.values()]}')
    for fault, must in PLANTS.items():
        rc, out, err = run(exe, '--game', game, '--capture', cap, '--frames', '200-200' if fault != 'lookalike' else '150-250', '--fault', fault)
        got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
        extra = fault != 'lookalike' or ('PASS  capture.mismatch_bounded' in out and 'lookalike objects given up 1' in out)
        check(f'fault.{fault}', rc == 1 and any(x in got for x in must) and extra, f'exit {rc}, fails {sorted(got)}')
    # lane 137 (N9): WM30 run 7084's scene props (the steel cage, the commentary desk, its monitors, one more prop: 14 draws a frame at frame 300) are ours, bytes equal; the wrong-source plant (a scene prop joined to a step leaf) fails
    # review 1: frames 132-667 at step 12 hit frame 300; capture.scene_props now requires exactly 14 scene draws on EVERY frame run, and names the expected joins (cage twin p1 / p2, desk p9/106, monitors p9/107, p9/14/14)
    w30 = ['--game', game, '--capture', caps['wm30r7084'], '--frames', '132-667', '--step', st, '--expect-scene']
    rc, out, err = run(exe, *w30)
    check('capture.wm30r7084.scene_props', rc == 0 and 'PASS  capture.scene_props' in out and 'PASS  capture.draw_checks' in out and 'frames run 45, frames without exactly 14: 0' in out, f'exit {rc}')
    if rc:
        print(out)
    rc, out, err = run(exe, *w30, '--fault', 'scene_wrong')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.scene_wrong.wm30r7084', rc == 1 and 'capture.scene_props' in got and 'capture.oracle_all_replaced' in got, f'exit {rc}, fails {sorted(got)}')
    # the desk forced to bgEtc p9/105 with its picture forced into slot 0: the file picture is not the stream's
    rc, out, err = run(exe, *w30, '--fault', 'scene_desk_105')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.scene_desk_105.wm30r7084', rc == 1 and 'capture.textures_equal_stream' in got, f'exit {rc}, fails {sorted(got)}')
    # a scene lookalike (the cage twin PACH 1 written with a wrong vertex byte): the cage objects re-join to PACH 2 (joins dropped 3, mismatch bounded, nothing given up); both twins wrong: given up within 4 objects
    rc, out, err = run(exe, *w30, '--fault', 'scene_lookalike_p1')
    check('fault.scene_lookalike_p1.wm30r7084', rc == 1 and 'joins dropped 3,' in out and 'PASS  capture.mismatch_bounded' in out and 'lookalike objects given up 0' in out and '(0 draws kept as mismatch' in out, f'exit {rc}')   # lane 220: a tied candidate that fails its bytes is replaced in the SAME call (was mismatch 3: the failing frame's draws were kept), the twin is joined at once
    rc, out, err = run(exe, *w30, '--fault', 'scene_lookalike_both')
    m = re.search(r'lookalike objects given up (\d+)', out)
    check('fault.scene_lookalike_both.wm30r7084', rc == 1 and m is not None and 1 <= int(m.group(1)) <= 4 and 'PASS  capture.mismatch_bounded' in out, f'exit {rc}, given up {m.group(1) if m else "?"}')
    # the real re-find: the state before the promotion (no scene sources) loses every scene draw
    rc, out, err = run(exe, *w30, '--fault', 'scene_off')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.scene_off.wm30r7084', rc == 1 and 'capture.scene_props' in got and 'capture.oracle_all_replaced' in got, f'exit {rc}, fails {sorted(got)}')
    # lane 220 (S101, coverage sweep run 7133, the victory scene): two vertices of the commentary monitor (bgEtc p9/7/0, p62/7/0) have a file u of -0.0. The loaders tested `u < 0`, so the mesh did not
    # depend on the stream's u-sign rule and was written |u| (+0) while the stream (signed, after SDK patch 109) holds 0x8000: the byte compare refused the monitor (props mismatch 30 on the board).
    # Fixed (std::signbit in the three loaders + the shadow casters). The plant neg_zero_u restores the old rule: the clean arm must replace every oracle-named draw with mismatch 0 and the plant must not.
    v7133 = ['--game', game, '--capture', caps['victory7133'], '--live-style']
    rc, out, err = run(exe, *v7133)
    check('capture.victory7133.neg_zero_u', 'PASS  capture.oracle_all_replaced' in out and 'PASS  capture.mismatch_bounded' in out and 'PASS  capture.draw_checks' in out and '0 draws kept as mismatch' in out and 'bgEtc p9/7/0' in out, f'exit {rc}')
    if 'PASS  capture.oracle_all_replaced' not in out:
        print(out[-3000:])
    rc, out, err = run(exe, *v7133, '--fault', 'neg_zero_u')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.neg_zero_u.victory7133', rc == 1 and 'capture.oracle_all_replaced' in got and 'capture.draw_checks' in got, f'exit {rc}, fails {sorted(got)}')
    # lane 220 follow-up: a -0.0 vertex whose stream sign bit the game cleared must not teach |u|. zero_cleared plants that on the victory capture: the fixed rule refuses (u sign rule not known), draws
    # nothing wrong (0 byte compares failed); teach_abs is the past bug (any clear bit teaches |u|): wrong |u| learned, byte compares fail, draws kept.
    rc, out, err = run(exe, *v7133, '--fault', 'zero_cleared')
    check('fault.zero_cleared.victory7133', 'PASS  capture.vertex_bytes_equal_stream' in out and 'PASS  capture.draw_checks' in out and 'byte checks failed 0' in out and 'kept variant: u sign rule not known' in out, f'exit {rc}')
    rc, out, err = run(exe, *v7133, '--fault', 'zero_cleared,teach_abs')
    got = set(re.findall(r'^FAIL\s+(\S+)', out, re.M))
    check('fault.teach_abs.victory7133', rc == 1 and 'capture.draw_checks' in got and 'capture.oracle_all_replaced' in got, f'exit {rc}, fails {sorted(got)}')
    rc, out, err = run(exe, '--game', game, '--capture', MAIN / 'out' / 'lane84' / 'no_such_capture')
    check('loud.missing_capture', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', MAIN / 'out' / 'lane84' / 'no_such_game', '--capture', cap)
    check('loud.missing_game', rc == 2, f'exit {rc}')
    rc, out, err = run(exe, '--game', game, '--capture', cap, '--fault', 'nonsense')
    check('loud.unknown_fault', rc == 3, f'exit {rc}')
    print('RESULT ' + ('OK' if not fails else 'FAIL ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

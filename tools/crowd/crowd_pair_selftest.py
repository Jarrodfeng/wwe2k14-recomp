# Gate check of lane 36 (Session 101): the crowd's pairing and pose test on Check B's material, with its planted faults.
#   py -I tools\crowd\crowd_pair_selftest.py [--zip CASE.zip]
# Runs the SHIPPED tool (out\tools\crowd_pair.exe, built by tools\crowd\build_crowd_pair.ps1) on the capture inside the case zip of run 7051
# (the Brock replay with scene records on). The zip is read, never written; the parts the tool needs (frames, scene, images.bin,
# geometry.bin: no textures) are unpacked once to <main checkout>\out\lane36\brock (game data stays under out\, never committed).
# Bars: (1) the clean run measures offset +1 on every compared frame, mean residual < 5e-5, no node flagged, all 20 person types compared,
# every block draw's c76-c78 equals a record member's world at offset +1; (2) planted faults: two swapped palette nodes -> MISMATCH naming
# both nodes; the record shifted by -1 / +1 frame moves the best offset to 0 / +2; (3) loud failures: a capture with no scene records, a
# missing capture. Exit 0 = all hold, 1 = a bar failed.
import argparse, glob, os, re, shutil, subprocess, sys, tempfile, time, zipfile
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


def run_tool(exe, cap, game, extra=(), timeout=600):
    t = time.time()
    p = subprocess.run([str(exe), '--capture', str(cap), '--game', str(game), *extra], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr, time.time() - t


def parse(out):
    m = re.search(r'^RESULT offset=([+-]\d+) \((\d+) of (\d+) frames\) node_order=(\w+)(?: nodes=([\d,]+))? residual=([\d.e+-]+)', out, re.M)
    if not m:
        return None
    return dict(offset=int(m.group(1)), n=int(m.group(2)), frames=int(m.group(3)), order=m.group(4), nodes=[int(x) for x in (m.group(5) or '').split(',') if x], residual=float(m.group(6)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip', default='')
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'crowd_pair.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\crowd\\build_crowd_pair.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    zips = [args.zip] if args.zip else sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / '20261111_215201_s100_owner_s89_brock' / 'case_*.zip')))
    if not zips or not Path(zips[0]).exists():
        print('FAIL  the run 7051 case zip is not there (out\\s101\\runs\\20261111_215201_s100_owner_s89_brock\\case_*.zip)')
        return 1
    root = MAIN / 'out' / 'lane36' / 'brock'
    cap = root / 'capture'
    marker = root / 'unpacked.txt'
    if not marker.exists() or marker.read_text() != Path(zips[0]).name:
        shutil.rmtree(root, ignore_errors=True)
        with zipfile.ZipFile(zips[0]) as z:
            for n in z.namelist():
                if n.startswith('capture/') and n != 'capture/textures.bin':
                    z.extract(n, root)
        marker.write_text(Path(zips[0]).name)
    frames = len(list((cap / 'frames').glob('frame_*.bin')))
    scenes = len(list((cap / 'scene').glob('frame_*.rsc')))
    check('input: the case zip holds frames and scene records', frames > 100 and scenes == frames, f'{frames} frames, {scenes} records')

    # 1. the clean run
    rc, out, err, dt = run_tool(exe, cap, game)
    print(out.rstrip())
    r = parse(out)
    check('clean: exit code 0 and a RESULT line', rc == 0 and r is not None, f'exit {rc}, {dt:.1f} s' + (f', stderr: {err.strip()[:200]}' if rc else ''))
    if r:
        check('clean: best offset +1 on every compared frame', r['offset'] == 1 and r['n'] >= 100 and re.search(r'best offset histogram[^\n]*: \+1:%d\s*$' % r['n'], out, re.M) is not None,
              f"offset {r['offset']:+d}, {r['n']} frames")
        check('clean: mean residual below 5e-5 game units and node order ok', r['order'] == 'ok' and r['residual'] < 5e-5, f"{r['order']}, residual {r['residual']:.2e}")
        check('clean: all 20 person types compared', 'person types compared: ' + ' '.join(str(i) for i in range(20)) + '\n' in out)
        m = re.search(r'member worlds, stream offset \+1: block draws (\d+) matched (\d+) \(c76-c78 max diff ([\d.e+-]+).*card draws (\d+) matched (\d+) \(max diff ([\d.e+-]+)', out)
        check('clean: member worlds at +1 equal c76-c78 exactly (blocks and cards)', bool(m) and m.group(1) == m.group(2) and m.group(4) == m.group(5) and float(m.group(3)) < 5e-7 and float(m.group(6)) < 5e-7,
              m.group(0)[:150] if m else 'no line')
        check('clean: no LOD-0 vertex-count mismatch at the best offset', 'another vertex count 0\n' in out)
        check('clean: run time under 60 s', dt < 60, f'{dt:.1f} s')

    # 2. planted faults
    rc, out, err, dt = run_tool(exe, cap, game, ['--swap', '16,15'])
    f = parse(out)
    check('plant: nodes 16 and 15 swapped -> MISMATCH naming both', rc == 0 and f is not None and f['order'] == 'MISMATCH' and 15 in f['nodes'] and 16 in f['nodes'], f"{f}" if f else f'exit {rc} {err.strip()[:150]}')
    rc, out, err, dt = run_tool(exe, cap, game, ['--swap', '2,10'])
    f = parse(out)
    check('plant: two palette nodes swapped -> node-order MISMATCH naming both', rc == 0 and f is not None and f['order'] == 'MISMATCH' and 2 in f['nodes'] and 10 in f['nodes'] and f['residual'] > 1.0,
          f"{f}" if f else f'exit {rc} {err.strip()[:150]}')
    rc, out, err, dt = run_tool(exe, cap, game, ['--shift', '-1'])
    f = parse(out)
    check('plant: record read one frame earlier -> best offset moves +1 to 0', rc == 0 and f is not None and f['offset'] == 0 and f['order'] == 'ok', f"{f}" if f else f'exit {rc}')
    rc, out, err, dt = run_tool(exe, cap, game, ['--shift', '1'])
    f = parse(out)
    check('plant: record read one frame later -> best offset moves +1 to +2', rc == 0 and f is not None and f['offset'] == 2 and f['order'] == 'ok', f"{f}" if f else f'exit {rc}')

    # 3. loud failures
    tmp = Path(tempfile.mkdtemp(prefix='l36_noscene_', dir=str(MAIN / 'out' / 'lane36')))
    try:
        (tmp / 'frames').mkdir()
        shutil.copy(cap / 'images.bin', tmp / 'images.bin')
        for i in range(3):
            shutil.copy(cap / 'frames' / f'frame_{i:06d}.bin', tmp / 'frames' / f'frame_{i:06d}.bin')
        rc, out, err, dt = run_tool(exe, tmp, game)
        check('failure: a capture without scene records fails loudly (exit 2, says so)', rc == 2 and 'no scene records' in err and 'RESULT' not in out, f'exit {rc}: {err.strip()[:160]}')
        rc, out, err, dt = run_tool(exe, MAIN / 'out' / 'lane36' / 'does_not_exist', game)
        check('failure: a missing capture fails loudly (exit 2)', rc == 2 and 'FAIL' in err, f'exit {rc}: {err.strip()[:120]}')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print('crowd_pair selftest:', 'PASS' if not fails else 'FAIL (' + ', '.join(fails) + ')')
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())

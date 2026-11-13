# Gate check of lane 42 (Session 101): the wrestler pairing tool on Check B's material (run 7051), with its planted faults and the REAL bug.
#   py -I tools\wrestler\wrestler_pair_selftest.py [--zip CASE.zip]
# Runs the SHIPPED tool (out\tools\wrestler_pair.exe, built by tools\wrestler\build_wrestler_pair.ps1) on the capture inside run 7051's case zip
# (unpacked once, no textures, to <main checkout>\out\lane36\brock or lane 36's folder if already there). Game data stays under out\.
# Bars: (1) clean: offset +1 on every voting frame; slots 6, 7, 8 palette EXACT over every part and frame; part index = mesh index (vertex and
# index counts and every index value equal) for all four skeleton bodies; the position fit is one uniform scale per body with rms < 1e-4;
# (2) the REAL bug (WRESTLER_PROMOTION_MAP.md section 2): Brock (actor:slot:0) is MISMATCH, only in nodes >= 59, nodes 0-58 exact; the
# stale-register block past a palette equals a record node for most draws (palette length must come from the bone list);
# (3) planted faults: record shifted -1 / +1 -> best offset 0 / +2; two nodes swapped -> EXACT skeletons become MISMATCH naming them; part index off
# by one -> counts MISMATCH; (4) loud failures: no scene records, missing capture. Exit 0 = all hold, 1 = a bar failed.
import argparse, glob, re, shutil, subprocess, sys, tempfile, time, zipfile
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


def run_tool(exe, cap, game, extra=(), timeout=900):
    t = time.time()
    p = subprocess.run([str(exe), '--capture', str(cap), '--game', str(game), *extra], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr, time.time() - t


def skeletons(out):
    r = {}
    for line in out.splitlines():
        if not line.startswith('SKELETON '):
            continue
        kv = dict(t.split('=', 1) for t in line.split()[1:] if '=' in t)
        sc = re.search(r'pos_scale=\(([\d.]+),([\d.]+),([\d.]+)\)', line)
        r[kv['id']] = dict(palette=kv['palette'], bad=kv.get('bad_nodes', ''), count_bad=int(kv['count_bad']), idx_ok=int(kv['index_ok']), idx_bad=int(kv['index_bad']),
                           check_draws=int(kv['check_draws']), cands=int(kv['candidates']), same=int(kv['same_palettes']), distinct=int(kv['distinct_palettes']),
                           scale=(float(sc.group(1)), float(sc.group(2)), float(sc.group(3))) if sc else None,
                           spread=float(kv['pos_scale_spread']) if 'pos_scale_spread' in kv else None, rms=float(kv['pos_rms_max']) if 'pos_rms_max' in kv else None)
    return r


def offset_rows(out):
    return {int(m.group(1)): (int(m.group(2)), int(m.group(3))) for m in re.finditer(r'^  offset ([+-]\d+): rows (\d+) exact (\d+)', out, re.M)}


def result(out):
    m = re.search(r'^RESULT offset=([+-]\d+) \((\d+) of (\d+) frames\) skeletons=(\d+) palette_exact=(\d+) palette_mismatch=(\d+) counts=(\w+) indices=(\w+)', out, re.M)
    return m and dict(offset=int(m.group(1)), n=int(m.group(2)), total=int(m.group(3)), sk=int(m.group(4)), exact=int(m.group(5)), mism=int(m.group(6)), counts=m.group(7), idx=m.group(8))


def nodes_of(spec):
    s = set()
    for part in spec.split(','):
        if not part:
            continue
        a, _, b = part.partition('-')
        s.update(range(int(a), int(b or a) + 1))
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--zip', default='')
    args = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'wrestler_pair.exe'
    if not exe.exists():
        print(f'FAIL  {exe} is not built (tools\\wrestler\\build_wrestler_pair.ps1)')
        return 1
    game = MAIN / 'game_data' / 'extracted'
    if not game.exists():
        print(f'FAIL  no game files at {game}')
        return 1
    zips = [args.zip] if args.zip else sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / '20261111_215201_s100_owner_s89_brock' / 'case_*.zip')))
    if not zips or not Path(zips[0]).exists():
        print('FAIL  the run 7051 case zip is not there (out\\s101\\runs\\20261111_215201_s100_owner_s89_brock\\case_*.zip)')
        return 1
    root = MAIN / 'out' / 'lane36' / 'brock'  # shared with lane 36's selftest (same unpack)
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

    # 1. clean run
    rc, out, err, dt = run_tool(exe, cap, game)
    print(out.rstrip())
    r, sk = result(out), skeletons(out)
    check('clean: exit 0 and a RESULT line', rc == 0 and r is not None, f'exit {rc}, {dt:.1f} s' + (f', stderr: {err.strip()[-200:]}' if rc else ''))
    if r:
        check('clean: pairing offset +1 on every voting frame', r['offset'] == 1 and r['n'] == r['total'] and r['n'] >= 300, f"{r}")
        check('clean: four skeleton bodies resolved by content and linked', set(sk) == {'actor:slot:0', 'actor:slot:6', 'actor:slot:7', 'actor:slot:8'}, f'{sorted(sk)}')
        for s in ('actor:slot:6', 'actor:slot:7', 'actor:slot:8'):
            check(f'clean: {s} palette EXACT (every part, every frame, bit for bit)', sk.get(s, {}).get('palette') == 'EXACT', str(sk.get(s)))
        check('clean: part index = mesh index: every checked draw has equal counts and every index value (idx_ok == draws, 0 misses)',
              r['counts'] == 'ok' and r['idx'] == 'ok' and all(v['idx_ok'] == v['check_draws'] > 0 and v['idx_bad'] == 0 for v in sk.values()), str({k: (v['idx_ok'], v['check_draws']) for k, v in sk.items()}))
        check('clean: each body resolves to exactly ONE roster model (candidates=1, none sharing palettes)', all(v['cands'] == 1 and v['same'] == 0 for v in sk.values()), str({k: v['cands'] for k, v in sk.items()}))
        check('clean: vertex positions = file positions x ONE scale per body (spread of the per-draw scales < 1e-5), rms < 1e-4',
              bool(sk) and all(v['scale'] and v['spread'] is not None and v['spread'] < 1e-5 and v['rms'] is not None and v['rms'] < 1e-4 for v in sk.values()),
              ', '.join(f"{k}: {v['scale']} spread {v['spread']}" for k, v in sk.items()))
        orows = offset_rows(out)
        check('clean: not one palette row is exact at any offset but +1 (and rows were compared there)', set(orows) == {-2, -1, 0, 1, 2} and all(orows[o][0] > 0 and (orows[o][1] == 0) == (o != 1) for o in orows), str(orows))
        check('clean: record i carries frame i guest frame', 'guest frame of record i vs frame i header: 0 mismatches' in out)
        # 2. the real bug
        b = sk.get('actor:slot:0', {})
        bad = nodes_of(b.get('bad', ''))
        want = set(range(59, 83)) | set(range(84, 108))
        check('REAL BUG re-found: Brock (actor:slot:0) palette MISMATCH in exactly nodes 59-82 and 84-107 (every compared node below 59 is exact)', b.get('palette') == 'MISMATCH' and bad == want, f"bad_nodes={b.get('bad')}")
        m = re.search(r'id=actor:slot:0 .*?stale_block_equals_a_node=(\d+)/(\d+)', out)
        check('stale registers past a palette equal a record node for most draws (length must come from the bone list)', bool(m) and int(m.group(2)) > 0 and int(m.group(1)) > 0.5 * int(m.group(2)), m.group(0)[-40:] if m else 'no line')
        check('clean: run time under 120 s', dt < 120, f'{dt:.1f} s')

    # 3. planted faults
    rc, out, err, dt = run_tool(exe, cap, game, ['--shift', '-1'])
    f = result(out)
    check('plant: record read one frame earlier -> best offset moves +1 to 0', rc == 0 and f is not None and f['offset'] == 0, str(f) if f else f'exit {rc} {err.strip()[-150:]}')
    rc, out, err, dt = run_tool(exe, cap, game, ['--shift', '1'])
    f = result(out)
    check('plant: record read one frame later -> best offset moves +1 to +2', rc == 0 and f is not None and f['offset'] == 2, str(f) if f else f'exit {rc} {err.strip()[-150:]}')
    rc, out, err, dt = run_tool(exe, cap, game, ['--swap', '2,10'])
    f, s2 = result(out), skeletons(out)
    check('plant: two palette nodes swapped -> the EXACT skeletons become MISMATCH naming both nodes',
          rc == 0 and f is not None and all(s2.get(k, {}).get('palette') == 'MISMATCH' and {2, 10} <= nodes_of(s2[k]['bad']) for k in ('actor:slot:6', 'actor:slot:7', 'actor:slot:8')), str(f))
    rc, out, err, dt = run_tool(exe, cap, game, ['--part-off', '1', '--frames', '150-200'])
    f = result(out)
    check('plant: part index off by one -> counts MISMATCH', rc == 0 and f is not None and f['counts'] == 'MISMATCH', str(f) if f else f'exit {rc} {err.strip()[-150:]}')

    rc, out, err, dt = run_tool(exe, cap, game, ['--perturb-index', '--frames', '150-200'])
    f = result(out)
    check('plant: one index value of the file strip off by a bit -> indices MISMATCH', rc == 0 and f is not None and f['idx'] == 'MISMATCH', str(f) if f else f'exit {rc} {err.strip()[-150:]}')
    rc, out, err, dt = run_tool(exe, cap, game, ['--perturb-pos', '--frames', '150-200'])
    s3 = skeletons(out)
    check('plant: one file position 1 unit off -> the position fit breaks (rms >= 1e-4 or spread >= 1e-5 on a body)',
          rc == 0 and bool(s3) and any((v['rms'] or 0) >= 1e-4 or (v['spread'] or 0) >= 1e-5 for v in s3.values()), str({k: (v['rms'], v['spread']) for k, v in s3.items()}))

    # 4. loud failures
    tmp = Path(tempfile.mkdtemp(prefix='l42_noscene_', dir=str(MAIN / 'out' / 'lane36')))
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

    print('wrestler_pair selftest:', 'PASS' if not fails else 'FAIL (' + ', '.join(fails) + ')')
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())

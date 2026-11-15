r"""S102 T4 / T5 gate proof (offline, 0 game runs, no window): picture equality of the frame builder's two ways, on real captures, plus our thread's time.
  py -I tools\fb\fb_replay_check.py --lane t4|t5 [--root DIR] [--reps N] [--quick]
Arms (our code against our code: equality is the bar):
  t4: off (reference), --fb-fast, and the planted twin --fb-fast --fb-fast-plant (the table hashes only the first half of each entry and trusts the key: must DIFFER)
  t5: off (reference), --pack-once, --pack-once --fb-fast, and the planted twin --pack-once --pack-once-plant (a block that misses its last constant range: must DIFFER)
Every arm's process prints "[fb] fast frames N, pack-once frames M" (the builder's own count of frames begun in each mode): a fast arm must show N > 0, a pack-once arm M > 0,
and the off arm 0 / 0, so the flag provably reached the builder (a picture equal because the switch did nothing would otherwise pass).
Captures (read in place from the MAIN checkout, never copied):
  Bryan's entrance: the case zip named in tools\gate\lanes\crowd_card_thin.json, replayed by fview (--case ZIP --replay --hash-frames): the FNV hash of the presented picture of EVERY frame
  four rplay captures (s100_heenan_build 100 frames, s100_match300 3, s100_s89k_a 6, brock_s15 16): the presented picture after chosen frames (--shots, PNG, sha256)
Time (not a bar, direction only; the PC hides GPU cost): rplay --timing's bind_us over the steady frames of s100_heenan_build, arms INTERLEAVED, at least 5 reps (--quick: 1), min and median.
A missing input FAILs loudly."""
import argparse, csv, hashlib, json, os, re, statistics, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FB_RE = re.compile(r'\[fb\] fast frames (\d+), pack-once frames (\d+)')

def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root

def fail(msg):
    print('FAIL ' + msg)
    sys.exit(1)

def bryan_zip(root):
    j = json.load(open(os.path.join(root, 'tools', 'gate', 'lanes', 'crowd_card_thin.json')))
    for c in j['checks']:
        for n in c.get('needs', []):
            if 'bryan' in n:
                return n
    fail('no Bryan capture named in tools/gate/lanes/crowd_card_thin.json')

# name, flags, expects fast frames > 0, expects pack-once frames > 0
ARMS = {
    't4': [('off', [], False, False), ('fast', ['--fb-fast'], True, False), ('plant', ['--fb-fast', '--fb-fast-plant'], True, False)],
    't5': [('off', [], False, False), ('pack', ['--pack-once'], False, True), ('pack_fast', ['--pack-once', '--fb-fast'], True, True),
           ('plant', ['--pack-once', '--pack-once-plant'], False, True)],
}

def counts(text, what):
    m = FB_RE.search(text)
    if not m:
        fail(f'{what}: no "[fb] fast frames" line (the tool did not print the builder counts)')
    return int(m.group(1)), int(m.group(2))

def check_counts(what, got, want_fast, want_pack):
    fast, pack = got
    if (fast > 0) != want_fast or (pack > 0) != want_pack:
        fail(f'{what}: builder counts fast={fast} pack-once={pack}, expected fast>0 {want_fast}, pack-once>0 {want_pack} (the flag did not reach the builder, or leaked)')

def run_fview(exe, zip_path, out_dir, name, flags):
    hashes = os.path.join(out_dir, f'bryan_{name}.csv')
    cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--hash-frames', hashes] + flags
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500)
    text = p.stdout + p.stderr
    if p.returncode != 0:
        print(text[-1500:]); fail(f'fview bryan {name}: exit {p.returncode}')
    if not os.path.exists(hashes):
        fail(f'fview bryan {name}: no hash file')
    vm = re.search(r'validation errors (\d+)', text)   # S102 G5 (F1 class fix): the csv is written before exit 4; the count must be read and be 0 in every arm
    if not vm or int(vm.group(1)) != 0:
        print(text[-1500:]); fail(f'fview bryan {name}: validation errors {vm.group(1) if vm else "not printed"} (must be 0)')
    rows = [l.strip() for l in open(hashes) if l.strip() and not l.startswith('format')]
    if len(rows) < 100:
        fail(f'fview bryan {name}: only {len(rows)} frames')
    return rows, counts(text, f'fview bryan {name}')

def run_rplay(exe, cap, out_dir, name, flags, frames, shots, rep):
    tag = os.path.basename(cap)
    shot_dir = os.path.join(out_dir, f'{tag}_{name}_{rep}')
    timing = os.path.join(out_dir, f'{tag}_{name}_{rep}.csv')
    cmd = [exe, '--capture', cap, '--validation', '1', '--frames', str(frames), '--timing', timing, '--shot-dir', shot_dir, '--shots', ','.join(map(str, shots))] + flags
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500)
    text = p.stdout + p.stderr
    if p.returncode != 0:
        print(text[-1500:]); fail(f'rplay {tag} {name}: exit {p.returncode}')
    sums = []
    for s in shots:
        f = os.path.join(shot_dir, f'frame_{s:04d}.png')
        if not os.path.exists(f):
            fail(f'rplay {tag} {name}: no shot {f}')
        sums.append(hashlib.sha256(open(f, 'rb').read()).hexdigest()[:16])
    bind = 0.0
    n = 0
    with open(timing) as fh:
        for r in csv.DictReader(fh):
            if int(r['frame']) == 0:
                continue  # frame 0 builds pipelines and tables: not steady state
            bind += float(r['bind_us']); n += 1
    return sums, bind, n, counts(text, f'rplay {tag} {name}')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lane', required=True, choices=['t4', 't5'])
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--quick', action='store_true', help='one timing rep')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    zip_path = os.path.join(main_co, bryan_zip(root).replace('/', os.sep))
    caps = [('s100_heenan_build', 100), ('s100_match300', 3), ('s100_s89k_a', 6), ('brock_s15', 16)]  # name, frames in the capture
    for p in (fview, rplay, zip_path) + tuple(os.path.join(main_co, 'out', 'captures', c[0]) for c in caps):
        if not os.path.exists(p):
            fail(f'missing {p}')
    reps = 1 if a.quick else max(5, a.reps)
    out_dir = tempfile.mkdtemp(prefix=f'fb_{a.lane}_', dir=os.path.join(root, 'out')) if os.path.isdir(os.path.join(root, 'out')) else tempfile.mkdtemp()
    arms = ARMS[a.lane]
    ok = True
    ref_b = ref_m = None
    for name, flags, want_fast, want_pack in arms:
        rows, got = run_fview(fview, zip_path, out_dir, name, flags)
        check_counts(f'fview bryan {name}', got, want_fast, want_pack)
        sums = []
        for cname, cframes in caps:
            cap = os.path.join(main_co, 'out', 'captures', cname)
            shots = list(range(0, cframes, 1 if cframes <= 16 else 10)) + ([cframes - 1] if cframes > 16 else [])
            s_, _, _, got = run_rplay(rplay, cap, out_dir, name, flags, cframes, shots, 0)
            check_counts(f'rplay {cname} {name}', got, want_fast, want_pack)
            sums += s_
        if ref_b is None:
            ref_b, ref_m = rows, sums
            print(f'[fb] reference arm "{name}": Bryan {len(rows)} frames hashed, {len(sums)} pictures over {len(caps)} captures; builder counts {got}')
            continue
        diff_b = sum(1 for x, y in zip(ref_b, rows) if x != y) + abs(len(ref_b) - len(rows))
        diff_m = sum(1 for x, y in zip(ref_m, sums) if x != y)
        if name == 'plant':
            good = diff_b > 0 or diff_m > 0
            print(f'[fb] planted twin "{name}": Bryan frames differing {diff_b} of {len(rows)}, capture pictures differing {diff_m} of {len(sums)}: ' + ('caught' if good else 'NOT CAUGHT'))
        else:
            good = diff_b == 0 and diff_m == 0
            print(f'[fb] arm "{name}": Bryan frames differing {diff_b} of {len(rows)}, capture pictures differing {diff_m} of {len(sums)}: ' + ('EQUAL' if good else 'DIFFERENT') + f'; builder counts (fast, pack-once) {got}')
        ok = ok and good
    # time: the arms interleaved rep by rep (heenan_build only), planted arms left out
    timed = [(n, f) for n, f, _, _ in arms if n != 'plant']
    series = {n: [] for n, _ in timed}
    nframes = 0
    for rep in range(reps):
        for name, flags in timed:
            _, bind, n, _ = run_rplay(rplay, os.path.join(main_co, 'out', 'captures', caps[0][0]), out_dir, name, flags, caps[0][1], [0], 100 + rep)
            series[name].append(bind)
            nframes = n
    ref_med = statistics.median(series[timed[0][0]])
    for name, _ in timed:
        v = series[name]
        med = statistics.median(v)
        print(f'[fb] time {name:10s}: bind min {min(v) / 1000.0:7.1f} ms, median {med / 1000.0:7.1f} ms over {nframes} steady frames of heenan_build ({med / max(nframes, 1):.1f} us a frame; median vs off {100.0 * (med - ref_med) / ref_med:+.1f} %; {len(v)} interleaved reps) [direction only: a shared PC, no GPU cost]')
    if not ok:
        fail('a frame builder arm differs from the reference, or a planted twin passed')
    print(f'OK fb_replay_check {a.lane}')

main()

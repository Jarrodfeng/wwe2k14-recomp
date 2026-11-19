# S102 main (07:05): read a Surface bench "ab" zip folder (Only=ab: warm-up + A B B A ..., folders ab_<n>_A|B under the run folder) with
# bench_line.py, one line per arm and the mean of each side. bench_line only reads frames_bryan / frames_brock / frames_match folders, so each
# arm folder is copied into a temp folder under the scene's name (with the run's power / counters files) and read there.
#   py -I tools\surface\ab_line.py <unzipped job folder> [--scene frames_bryan] [--split]
# S102 H3: --split also reads the swap-thread split (bench_line.py --split: the fs_ columns of a run with --wwe2k14_frame_split=true) and prints, per arm, each column's
# mean and its mean over the 1 %-low frames, then the mean of each side and B - A (mean / 1 %-low-frame mean). Arms without the columns print 'split: not measured'.
import argparse, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import wer_report   # S102 TS1
LINE = re.compile(r'\w+ \[frames [^\]]+\]: shown ([\d.]+) fps, 1% low ([\d.]+)(?: \(worst-gap-per-row [\d.]+\))? \| gpu ([\d.-]+) render ([\d.]+|not measured) swap ([\d.]+)')   # S102 main 08:5x: R25-C2's worst-gap text broke this match
SPLIT = re.compile(r'^split \[[^\]]*\]: (.*) ms \(mean / mean of the (\d+) 1%-low frames\)', re.M)
SPLIT_CELL = re.compile(r'(\w+) (-|[\d.-]+)/(-|[\d.-]+)')
HEAT = re.compile(r'heat \[([^\]]*)\][^|]*?pkg ([\d.]+) W')
THERMAL = re.compile(r'thermal (SUSTAINED|BURST|MIXED|COOL|NONE)')   # S102 B1: burst_rules.py's state, first window of the line

POWER_W = 1.5

# S103 GT (--roles): bench_line.py --roles prints `roles [bryan frames A..B]: colour 3.64 ... = work 13.4 ms a frame (...) | uploads steady 0.02 MB/frame (n frames), loading 14.1 MB/frame (...)` per window
# (the per-role GPU split from the run log of EITHER build; the stream-free build logs it since S103 GT). Per arm the line is echoed; with both sides it gets one A-vs-B table per role.
ROLES = re.compile(r'^roles \[[^\]]*\]: (.*?) = work ([\d.]+) ms a frame(?: \([^)]*\))?(?: \| uploads steady ([\d.]+) MB/frame \((\d+) frames\), loading ([\d.]+) MB/frame \((\d+) frames)?', re.M)
ROLE_CELL2 = re.compile(r'(\w+) (-?[\d.]+)')


def parse_roles(out):
    """-> {'roles': {name: ms}, 'work': ms, 'steady_mb', 'loading_mb', 'loading_frames', 'steady_frames'} of the FIRST roles line, or None (not measured)."""
    m = ROLES.search(out)
    if not m:
        return None
    d = {'roles': {k: float(v) for k, v in ROLE_CELL2.findall(m.group(1))}, 'work': float(m.group(2))}
    if m.group(3) is not None:
        d.update(steady_mb=float(m.group(3)), steady_frames=int(m.group(4)), loading_mb=float(m.group(5)), loading_frames=int(m.group(6)))
    return d


def roles_table(sides):
    """sides: {'A': [roles dict], 'B': [roles dict]} -> prints a side-by-side table per role (mean over the arms of each side) when both sides have stamps."""
    have = {s: [r for r in rs if r] for s, rs in sides.items()}
    if not have.get('A') or not have.get('B'):
        print('roles: %s' % ', '.join('%s %s' % (s, ('%d arms with stamps of %d' % (len(have.get(s, [])), len(sides.get(s, []))))) for s in 'AB'))
        return
    names = [k for k in ('colour', 'shadow', 'rim', 'plane', 'cards', 'post', 'output', 'ui', 'frame', 'copies', 'prelight', 'feedback', 'other', 'uploads')
             if any(k in r['roles'] for s in 'AB' for r in have[s])]
    mean = lambda s, f: sum(f(r) for r in have[s]) / len(have[s])
    print('roles (GPU ms a frame, mean of %d A / %d B arms; NOT power-matched: read the pairs above for that)   %-9s %8s %8s %8s' % (len(have['A']), len(have['B']), 'role', 'A', 'B', 'B - A'))
    for k in names + ['work']:
        f = (lambda r, k=k: r['work']) if k == 'work' else (lambda r, k=k: r['roles'].get(k, 0.0))
        a_, b_ = mean('A', f), mean('B', f)
        print('roles %36s %8.2f %8.2f %+8.2f' % (k, a_, b_, b_ - a_))
    up = {s: [r for r in have[s] if 'steady_mb' in r] for s in 'AB'}
    if up['A'] and up['B']:
        for lab, key in (('uploads steady MB/frame', 'steady_mb'), ('uploads loading MB/frame', 'loading_mb')):
            a_, b_ = mean_of(up['A'], key), mean_of(up['B'], key)
            print('roles %36s %8.3f %8.3f %+8.3f' % (lab, a_, b_, b_ - a_))
    else:
        print('roles uploads: not measured on both sides')


def mean_of(rs, key):
    return sum(r[key] for r in rs) / len(rs)


def read_arm(arm, tdir, a):
    """One arm folder through bench_line.py: (vals, pkg W, heat, split cells | None, why). vals None when bench_line found no line."""
    t = Path(tdir)
    shutil.copytree(arm, t / a.scene)
    for f in arm.parent.iterdir():
        if f.is_file() and re.search(r'power|counters', f.name):
            shutil.copy2(f, t)
    proc = subprocess.run([sys.executable, '-I', str(HERE / 'bench_line.py'), str(t)] + (['--split'] if a.split else []), capture_output=True, text=True)
    out = proc.stdout
    m, h = LINE.search(out), HEAT.search(out)
    if not m:
        return None, float('nan'), '?', None, (out + proc.stderr).strip()[-300:]
    vals = [float(x) if x not in ('-', 'not measured') else float('nan') for x in m.groups()]
    cells = None
    if a.split:
        sm = SPLIT.search(out)
        if sm:
            cells = {k: (float(x) if x != '-' else float('nan'), float(y) if y != '-' else float('nan')) for k, x, y in SPLIT_CELL.findall(sm.group(1))}
    th = THERMAL.search(out)
    return vals, (float(h.group(2)) if h else float('nan')), ((h.group(1) if h else '?') + ('/' + th.group(1) if th else '')), cells, ''


def run_order(parent, arms):
    """arm folder -> position in time: the line number in ab_order.txt (the run order), else the folder's modification time."""
    pos = {}
    of = parent / 'ab_order.txt'
    if of.is_file():
        for i, line in enumerate(of.read_text(errors='replace').splitlines()):
            pos.setdefault(line.split('\t')[0], i)
    if all(p.name in pos for p in arms):
        return {p: pos[p.name] for p in arms}, 'ab_order.txt'
    return {p: p.stat().st_mtime for p in arms}, 'folder time (no ab_order.txt line for every arm)'


def multi(a, arms):
    """Several B arms (ab_<round>_<arm>): each B arm's run is paired with the nearest A run in time inside its own round; a pair whose package
    power differs by more than POWER_W is left out; per arm the power-matched B - A (and the split B - A with --split), then one table of all arms."""
    if a.roles:
        print('roles: multi-arm not supported (use a plain A / B job, or bench_line.py --roles per arm folder)')
    runs = {}   # (parent, round, arm name) -> dict
    parents = {}
    for p in arms:
        parents.setdefault(p.parent, []).append(p)
    pos, how_all = {}, set()
    for par, lst in parents.items():
        ks, how = run_order(par, lst)
        how_all.add(how)
        pos.update(ks)
    with tempfile.TemporaryDirectory(prefix='ab_line_') as tmp:
        for i, p in enumerate(arms):
            rm = re.match(r'ab_(\d+)_(.+)$', p.name)
            vals, w, heat, cells, why = read_arm(p, Path(tmp) / ('arm%d' % i), a)
            if vals is None:
                print('%s: no line (bench_line said: %s)' % (p.name, why))
                continue
            # a round's repeat base runs are ab_<r>_A2, A3 ...: all of them are arm A
            runs[(str(p.parent), int(rm.group(1)), rm.group(2) if not re.match(r'A\d+$', rm.group(2)) else 'A', p.name)] = dict(vals=vals, w=w, cells=cells, pos=pos[p])
            if a.split:
                if cells is None:
                    print('        split: not measured (no fs_ columns in this arm)')
                else:
                    print('        split (mean/1%-low-frame mean, ms): ' + ' '.join('%s %.2f/%.2f' % (k, *v) for k, v in cells.items()))
            print('%-9s shown %5.1f  1%% low %5.1f  gpu %5.1f  render %5.1f  swap %5.1f  | heat %s, pkg %s W' % (p.name, *vals, heat, '%.1f' % w if w == w else '?'))
    print('pairing order from: %s' % ', '.join(sorted(how_all)))
    names = sorted({k[2] for k in runs if k[2] != 'A'})
    if not any(k[2] == 'A' for k in runs):
        print('REFUSED: no arm A runs (nothing to pair against)')
        return 2
    table = []
    for nm in names:
        print('--- arm %s vs A' % nm)
        matched, pairs, msplit = [], 0, []
        for (par, rd, an, _fn), r in sorted(runs.items()):
            if an != nm:
                continue
            cands = [(abs(ra['pos'] - r['pos']), ra['pos'], ra) for (par2, rd2, an2, _fn2), ra in runs.items() if an2 == 'A' and rd2 == rd and par2 == par]
            if not cands:
                print('pair r%d %s: no A run in this round (left out)' % (rd, nm))
                continue
            _, _, ra = min(cands, key=lambda c: (c[0], c[1]))
            pairs += 1
            va, vb = ra['vals'], r['vals']
            ok = abs(ra['w'] - r['w']) <= POWER_W
            if ok:
                matched.append([vb[i] - va[i] for i in range(5)])
                if a.split and r['cells'] and ra['cells']:
                    msplit.append({k: tuple(r['cells'][k][j] - ra['cells'][k][j] for j in (0, 1)) for k in ra['cells'] if k in r['cells']})
            print('pair r%d %s: B - A shown %+5.1f  swap %+5.1f  gpu %+5.1f | pkg A %.1f W, B %.1f W -> %s' % (rd, nm, vb[0] - va[0], vb[4] - va[4], vb[2] - va[2], ra['w'], r['w'], 'matched' if ok else 'POWER STEP (left out)'))
        if matched and 2 * len(matched) >= pairs:
            d = [sum(m[i] for m in matched) / len(matched) for i in range(5)]
            print('power-matched B - A (%d of %d pairs): shown %+5.1f  1%% low %+5.1f  gpu %+5.1f  render %+5.1f  swap %+5.1f' % (len(matched), pairs, *d))
            verdict = 'ok'
        else:
            d = None
            print('POWER-CONFOUNDED: %d of %d pairs matched within %.1f W: no verdict for arm %s' % (len(matched), pairs, POWER_W, nm))
            verdict = 'CONFOUNDED'
        if a.split and d is not None:
            if msplit:
                print('split B - A (power-matched pairs): ' + ' '.join('%s %+.2f/%+.2f' % (k, sum(x[k][0] for x in msplit if k in x) / len(msplit), sum(x[k][1] for x in msplit if k in x) / len(msplit)) for k in msplit[0]))
            else:
                print('split B - A: not measured')
        table.append((nm, len(matched), pairs, d, verdict))
    print('=== summary (B arm - nearest A in the same round, power-matched within %.1f W)' % POWER_W)
    print('%-9s %-7s %7s %8s %6s %7s %6s  %s' % ('arm', 'pairs', 'shown', '1% low', 'gpu', 'render', 'swap', 'verdict'))
    for nm, m, n, d, v in table:
        if d is None:
            print('%-9s %-7s %7s %8s %6s %7s %6s  %s' % (nm, '%d/%d' % (m, n), '-', '-', '-', '-', '-', v))
        else:
            print('%-9s %-7s %+7.1f %+8.1f %+6.1f %+7.1f %+6.1f  %s' % (nm, '%d/%d' % (m, n), d[0], d[1], d[2], d[3], d[4], v))
    return 0



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--scene', default='frames_bryan', choices=('frames_bryan', 'frames_brock', 'frames_match', 'frames_bryan_heavy'))   # S102 M6
    ap.add_argument('--split', action='store_true')
    ap.add_argument('--roles', action='store_true', help='S103 GT: per arm the per-role GPU split + upload view (bench_line.py --roles), then one A-vs-B table per role when both sides logged stamps')
    ap.add_argument('--flip', action='store_true', help='S102 M7: the folder is an AbFlip job (ab_<n>_A + ab_<n>_B = a complementary pair): read each round with flip_line.py --pair')
    a = ap.parse_args()
    if a.flip:
        codes = []
        for pa in sorted(Path(a.folder).rglob('ab_*_A')):
            pb = pa.with_name(pa.name[:-1] + 'B')
            if pa.is_dir() and pb.is_dir():
                print('=== %s + %s (flip_line.py --pair)' % (pa.name, pb.name), flush=True)
                codes.append(subprocess.run([sys.executable, '-I', str(HERE / 'flip_line.py'), str(pa), '--pair', str(pb)] + (['--quiet'] if not a.split else [])).returncode)
        if not codes:
            print('REFUSED: no ab_<n>_A / ab_<n>_B pair under %s' % a.folder)
            return 2
        bad = [c for c in codes if c]
        if bad:
            print('%d of %d pairs refused or mismatched (exit codes %s)' % (len(bad), len(codes), codes))
        wer_report.print_wer(a.folder)   # S102 TS1
        return max(codes)
    arms = sorted(Path(a.folder).rglob('ab_*_[AB]'))
    arms = [p for p in arms if p.is_dir()]
    # S102 M5: a job run with ArmFlags (several B arms) has folders ab_<round>_<arm>: any arm name besides A / B sends the folder to the multi-arm reader.
    every = sorted(p for p in Path(a.folder).rglob('ab_*') if p.is_dir() and re.match(r'ab_\d+_.+$', p.name))
    if any(not re.match(r'ab_\d+_[AB]$', p.name) for p in every):
        rc = multi(a, every)
        wer_report.print_wer(a.folder)   # S102 TS1
        return rc
    if not arms:
        print('REFUSED: no ab_<n>_A|B folders under %s' % a.folder)
        return 2
    side = {'A': [], 'B': []}
    by_round = {}   # S102 main (21:30): round n -> {'A': (vals, pkg W), 'B': ...}; each round's A and B ran next to each other (A B B A ...)
    split_side = {'A': [], 'B': []}   # S102 H3: per arm {column: (mean, tail mean)}
    role_side = {'A': [], 'B': []}    # S103 GT: per arm parse_roles()
    with tempfile.TemporaryDirectory(prefix='ab_line_') as tmp:
        for arm in arms:
            t = Path(tmp) / arm.name
            shutil.copytree(arm, t / a.scene)
            for f in arm.parent.iterdir():
                if f.is_file() and re.search(r'power|counters', f.name):
                    shutil.copy2(f, t)
            out = subprocess.run([sys.executable, '-I', str(HERE / 'bench_line.py'), str(t)] + (['--split'] if a.split else []) + (['--roles'] if a.roles else []), capture_output=True, text=True).stdout
            m, h = LINE.search(out), HEAT.search(out)
            if a.roles:
                for ln in out.splitlines():
                    if ln.startswith('roles ['):
                        print('        ' + ln)
            if not m:
                print('%s: no line (bench_line said: %s)' % (arm.name, out.strip()[:200]))
                continue
            if a.roles:
                role_side[arm.name[-1]].append(parse_roles(out))   # only an arm whose line was read (review: a skipped arm must not enter the table)
            vals = [float(x) if x not in ('-', 'not measured') else float('nan') for x in m.groups()]
            side[arm.name[-1]].append(vals)
            rm = re.match(r'ab_(\d+)_([AB])$', arm.name)
            if rm:
                by_round.setdefault(int(rm.group(1)), {})[rm.group(2)] = (vals, float(h.group(2)) if h else float('nan'))
            if a.split:
                sm = SPLIT.search(out)
                if not sm:
                    print('        split: not measured (no fs_ columns in this arm)')
                else:
                    cells = {k: (float(x) if x != '-' else float('nan'), float(y) if y != '-' else float('nan')) for k, x, y in SPLIT_CELL.findall(sm.group(1))}
                    split_side[arm.name[-1]].append(cells)
                    print('        split (mean/1%-low-frame mean, ms): ' + ' '.join('%s %.2f/%.2f' % (k, *v) for k, v in cells.items()) + ' (%s tail frames)' % sm.group(2))
            th = THERMAL.search(out)   # S102 B1
            print('%-7s shown %5.1f  1%% low %5.1f  gpu %5.1f  render %5.1f  swap %5.1f  | heat %s, pkg %s W' % (arm.name, *vals, (h.group(1) if h else '?') + ('/' + th.group(1) if th else ''), h.group(2) if h else '?'))
    for s in 'AB':
        if side[s]:
            n = len(side[s])
            mean = [sum(v[i] for v in side[s]) / n for i in range(5)]
            print('mean %s (%d): shown %5.1f  1%% low %5.1f  gpu %5.1f  render %5.1f  swap %5.1f' % (s, n, *mean))
    if side['A'] and side['B']:
        d = [sum(v[i] for v in side['B']) / len(side['B']) - sum(v[i] for v in side['A']) / len(side['A']) for i in range(5)]
        print('B - A: shown %+5.1f  1%% low %+5.1f  gpu %+5.1f  render %+5.1f  swap %+5.1f' % tuple(d))
    # S102 main (21:30, Jarrod: throttling must not make a real cut look like a regression): a power step between an A and its B biases
    # the pair. Pair each round's A with its B (they ran back to back), flag a pair whose package power differs by more than POWER_W, and
    # give the result over the power-matched pairs only. Fewer than half the pairs matched = POWER-CONFOUNDED: no verdict from this job.
    POWER_W = 1.5
    pairs = [(n, r['A'], r['B']) for n, r in sorted(by_round.items()) if 'A' in r and 'B' in r]
    if pairs:
        matched = []
        for n, (va, wa), (vb, wb) in pairs:
            ok = abs(wa - wb) <= POWER_W
            if ok:
                matched.append([vb[i] - va[i] for i in range(5)])
            print('pair %d: B - A shown %+5.1f  swap %+5.1f  gpu %+5.1f | pkg A %.1f W, B %.1f W -> %s' % (n, vb[0] - va[0], vb[4] - va[4], vb[2] - va[2], wa, wb, 'matched' if ok else 'POWER STEP (left out)'))
        if matched and 2 * len(matched) >= len(pairs):
            d = [sum(m[i] for m in matched) / len(matched) for i in range(5)]
            print('power-matched B - A (%d of %d pairs): shown %+5.1f  1%% low %+5.1f  gpu %+5.1f  render %+5.1f  swap %+5.1f' % (len(matched), len(pairs), *d))
        else:
            print('POWER-CONFOUNDED: %d of %d pairs matched within %.1f W: no verdict from this job' % (len(matched), len(pairs), POWER_W))
    if a.roles:
        roles_table(role_side)
    if a.split:
        means = {}
        for sd in 'AB':
            arms_ = split_side[sd]
            if arms_:
                keys = list(arms_[0])
                means[sd] = {k: tuple(sum(x[k][j] for x in arms_ if k in x) / len(arms_) for j in (0, 1)) for k in keys}
                print('split mean %s (%d): ' % (sd, len(arms_)) + ' '.join('%s %.2f/%.2f' % (k, *v) for k, v in means[sd].items()))
        if 'A' in means and 'B' in means:
            print('split B - A: ' + ' '.join('%s %+.2f/%+.2f' % (k, means['B'][k][0] - means['A'][k][0], means['B'][k][1] - means['A'][k][1]) for k in means['A'] if k in means['B']))
    wer_report.print_wer(a.folder)   # S102 TS1: the job's GPU watchdog reports (wer_report.py)
    return 0


if __name__ == '__main__':
    sys.exit(main())

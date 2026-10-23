"""The playtester performance test with the per-pass breakdown (Session 41, low-spec step 0; docs\\native\\LOWSPEC_PLAN.md).

The launcher's PERFORMANCE TEST plays the same Quick Match twice (the LOW preset: the native renderer, fast clears;
then the light renderer; both 720p, frame rate 30) with the route short\\s41_perf_breakdown.txt (shipped as
perf_test.txt). Each game: frame-anatomy captures with GPU timestamps (K = 0 the main menu, 1 Bryan's entrance +440,
2 the same with the native post-processing and after-scene draws (Session 42), 3 the match +600, 4 the same with
them; 2 frames each) and the fast clears switched on / off every 120 frames through both entrances
("ab: mode 1 / 0"). This prints, per game:
  - frames per second per scene (all windows) and the fast clears' effect (each "on" window against the "off"
    windows next to it, as tools\\native\\laptop_ab.py; the light renderer has no fast clears: its estimate is the
    test's own check, about 0);
  - Session 43: the captures' labels come from the log (perf: capture K LABEL; K = 0 menu, 1 Bryan, 2 Bryan + the
    Session 42 pair, 3 Bryan + the Session 43 native set, 4 match, 5 / 6 the same), each native capture compared with
    the capture before it (off -> on per group), and the Session 43 set's own on / off windows in the match (ab2:
    frames per second, paired);
  - per capture and frame: the GPU's busy time and where it goes, by render pass: before the scene (shadow maps =
    passes on small targets, the clears, the rest), the scene (the pass with the most depth-writing draws), after the
    scene (clears, the rest = post-processing and HUD), and the biggest passes.

    py tools\\native\\laptop_breakdown.py performance_test_DATE.zip
    py tools\\native\\laptop_breakdown.py RUN [RUN...]            (runs of this PC)
    py tools\\native\\laptop_breakdown.py LOGS_FOLDER RUN [RUN...]

Numbers only; nothing from the game.
"""
import csv
import os
import re
import statistics
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import laptop_ab  # noqa: E402

CLEAR_VS = '1E6883FCCDE1F688'  # the game's clear rectangle (Session 40)
# Session 42: 2 and 4 = the same with gpu_native_post + gpu_native_after_scene (the light renderer: no change).
CAPTURES = {0: 'main menu', 1: "Bryan's entrance +440", 2: "Bryan's entrance +452, native post + after-scene",
            3: 'match +600', 4: 'match +612, native post + after-scene'}


def u(v):
    try:
        return int(v, 16) if v else 0
    except ValueError:
        return 0


def passes_of(rows):
    """Consecutive draws with the same render-target registers; copies (resolves) join the pass before them."""
    out, cur = [], None
    for r in rows:
        key = (r['rb_surface_info'], r['rb_color_info0'], r['rb_color_info1'], r['rb_color_info2'],
               r['rb_color_info3'], r['rb_depth_info'])
        if r['kind'] == 'draw' and (cur is None or cur['key'] != key):
            cur = {'key': key, 'rows': [], 'draws': 0, 'depth_draws': 0, 'clears': 0, 'copies': 0, 'gpu': 0.0,
                   'clear_gpu': 0.0}
            out.append(cur)
        if cur is None:
            cur = {'key': None, 'rows': [], 'draws': 0, 'depth_draws': 0, 'clears': 0, 'copies': 0, 'gpu': 0.0,
                   'clear_gpu': 0.0}
            out.append(cur)
        cur['rows'].append(int(r['idx']))
        if r['kind'] == 'draw':
            cur['draws'] += 1
            dc = u(r['rb_depthcontrol'])
            if dc & 0b110 == 0b110:  # z enable + z write
                cur['depth_draws'] += 1
            if r['vs_hash'] == CLEAR_VS:
                cur['clears'] += 1
        elif r['kind'] == 'copy':
            cur['copies'] += 1
    return out


def breakdown(csv_path):
    """-> [(frame, busy_ms, groups {name: ms}, passes)] for one capture."""
    base = csv_path[:-4]
    if not all(os.path.exists(p) for p in (csv_path, base + '_gpu.csv', base + '_gpu_frames.csv')):
        return []
    rows = list(csv.DictReader(open(csv_path, newline='')))
    gpu = list(csv.DictReader(open(base + '_gpu.csv', newline='')))
    busy = {int(r['frame']): float(r['busy_union_us']) for r in csv.DictReader(open(base + '_gpu_frames.csv', newline=''))}
    out = []
    for frame in sorted({int(r['frame']) for r in rows}):
        frows = sorted((r for r in rows if int(r['frame']) == frame), key=lambda r: int(r['idx']))
        kind_of = {int(r['idx']): r for r in frows}
        ps = passes_of(frows)
        pass_of = {}
        for i, p in enumerate(ps):
            for idx in p['rows']:
                pass_of[idx] = i
        other = 0.0
        for g in gpu:
            if int(g['frame']) != frame or not g['gpu_row_us']:
                continue
            us = float(g['gpu_row_us'])
            idx = int(g['idx'])
            if g['kind'] == 'cb_end' or idx not in pass_of:
                other += us
                continue
            p = ps[pass_of[idx]]
            p['gpu'] += us
            r = kind_of.get(idx)
            if r is not None and r['kind'] == 'draw' and r['vs_hash'] == CLEAR_VS:
                p['clear_gpu'] += us
        # The scene: the pass with the most depth-writing draws (at least 50; a menu frame has none: all one group).
        scene = max(range(len(ps)), key=lambda i: ps[i]['depth_draws']) if ps else -1
        if scene >= 0 and ps[scene]['depth_draws'] < 50:
            scene = -1
        groups = {'no 3D scene: all passes': 0.0, 'before: shadow maps': 0.0, 'before: clears': 0.0, 'before: other': 0.0, 'scene: clears': 0.0,
                  'scene': 0.0, 'after: clears': 0.0, 'after: post-processing / HUD': 0.0, 'not in a pass': other}
        for i, p in enumerate(ps):
            pitch = u(p['key'][0]) & 0x3FFF if p['key'] else 0
            clears = p['clear_gpu']
            rest = p['gpu'] - clears
            if scene < 0:
                groups['no 3D scene: all passes'] += p['gpu']
            elif i < scene:
                if 0 < pitch <= 128:
                    groups['before: shadow maps'] += p['gpu']
                else:
                    groups['before: clears'] += clears
                    groups['before: other'] += rest
            elif i == scene:
                groups['scene: clears'] += clears
                groups['scene'] += rest
            else:
                groups['after: clears'] += clears
                groups['after: post-processing / HUD'] += rest
        out.append((frame, busy.get(frame, 0.0) / 1000.0, {k: v / 1000.0 for k, v in groups.items()}, ps, scene))
    return out


CAPTURE_LINE = re.compile(r'\[input\] line \d+: perf: capture (\d+) (.*?) \(frame \d+\)')


def capture_names(logs, run):
    """Session 43: the captures' labels from the run's "perf: capture K LABEL" lines (older routes: CAPTURES)."""
    names = {}
    log = os.path.join(logs, f'wwe2k14_{run:03d}.log')
    if os.path.exists(log):
        for line in open(log, encoding='utf-8', errors='replace'):
            m = CAPTURE_LINE.search(line)
            if m:
                names[int(m.group(1))] = m.group(2)
    return names or dict(CAPTURES)


def compare_captures(logs, run, names):
    """Session 43: each native-setting capture against the capture it follows (the same scene, settings off), frame 0:
    GPU ms per group, off -> on."""
    results = {k: breakdown(os.path.join(logs, f'anatomy_{run:03d}_{k}.csv')) for k in names}
    base = None
    lines = []
    for k in sorted(names):
        name = names[k]
        if 'native' not in name:
            base = k
            continue
        if base is None or not results.get(base) or not results.get(k):
            continue
        _, busy0, g0, _, _ = results[base][0]
        _, busy1, g1, _, _ = results[k][0]
        lines.append(f'  capture {k} ({name}) vs {base} ({names[base]}): GPU busy {busy0:.2f} -> {busy1:.2f} ms '
                     f'({100.0 * (busy1 - busy0) / busy0 if busy0 else 0:+.1f}%)')
        for g in g0:
            if g0[g] > 0.0005 or g1.get(g, 0) > 0.0005:
                lines.append(f'      {g:<32} {g0[g]:7.2f} -> {g1.get(g, 0.0):7.2f} ms')
    if lines:
        print('  native settings off -> on (frame 0 of each capture; the captures are 12 frames apart, so a part of a'
              ' change is the shot):')
        for line in lines:
            print(line)


def print_breakdown(logs, run):
    names = capture_names(logs, run)
    for k, name in sorted(names.items()):
        path = os.path.join(logs, f'anatomy_{run:03d}_{k}.csv')
        result = breakdown(path)
        if not result:
            print(f'  capture {k} ({name}): none')
            continue
        for frame, busy, groups, ps, scene in result:
            total = sum(groups.values())
            print(f'  capture {k} ({name}), frame {frame}: GPU busy {busy:.2f} ms, in marked rows {total:.2f} ms, '
                  f'{len(ps)} passes')
            for g, ms in groups.items():
                if ms > 0.0005:
                    print(f'    {g:<32} {ms:7.2f} ms  {100.0 * ms / total if total else 0:5.1f}%')
            top = sorted(range(len(ps)), key=lambda i: -ps[i]['gpu'])[:6]
            for i in top:
                p = ps[i]
                where = '-' if scene < 0 else 'scene' if i == scene else ('before' if i < scene else 'after')
                print(f'      pass {i:3d} ({where:<6}) surface {p["key"][0] if p["key"] else "-":>10} '
                      f'{p["draws"]:5d} draws {p["clears"]:2d} clears {p["copies"]:2d} resolves: {p["gpu"] / 1000:6.2f} ms'
                      f' (clears {p["clear_gpu"] / 1000:.2f})')
    compare_captures(logs, run, names)


def print_run(logs, run, label):
    print(f'run {run}{": " + label if label else ""}')
    r = laptop_ab.analyze(logs, run)
    if not r:
        print('  no "ab: mode" lines or no frame times')
    else:
        device, light, order, seq = r
        for d in device:
            print(f'  {d}')
        print(f'  {"scene":<18} {"all fps":>8} {"clears off":>11} {"clears on":>10} {"estimate":>9} '
              f'{"middle half of the pairs":>26}')
        for scene in order:
            s = seq.get(scene, [])
            off = [f for m, f in s if m == 0]
            on = [f for m, f in s if m == 1]
            pairs = laptop_ab.paired([(2 if m == 1 else m, f) for m, f in s])
            all_fps = statistics.median([f for _, f in s]) if s else 0.0
            if not off or not on or not pairs:
                print(f'  {scene:<18} {all_fps:6.1f}   (not enough fast-clear windows)')
                continue
            lo, hi = laptop_ab.quartiles(pairs)
            print(f'  {scene:<18} {all_fps:6.1f}  {statistics.median(off):7.1f} fps {statistics.median(on):6.1f} fps '
                  f'{statistics.median(pairs):+8.1f}% {f"{lo:+.1f}% .. {hi:+.1f}% ({len(pairs)} pairs)":>26}')
    # Session 43: the Session 43 native set on / off in the match ("ab2" windows), frames per second paired.
    r2 = laptop_ab.analyze(logs, run, 'ab2')
    if r2:
        _, _, order2, seq2 = r2
        for scene in order2:
            s = seq2.get(scene, [])
            pairs = laptop_ab.paired([(2 if m == 1 else m, f) for m, f in s])
            off = [f for m, f in s if m == 0]
            on = [f for m, f in s if m == 1]
            if off and on and pairs:
                lo, hi = laptop_ab.quartiles(pairs)
                print(f'  Session 43 native set, {scene:<14} off {statistics.median(off):6.1f} fps, on '
                      f'{statistics.median(on):6.1f} fps: {statistics.median(pairs):+.1f}% '
                      f'({lo:+.1f}% .. {hi:+.1f}%, {len(pairs)} pairs)')
    # Session 45 (step 2): the native frame (gpu_native_all + gpu_native_resolve) on / off in the match ("ab3").
    r3 = laptop_ab.analyze(logs, run, 'ab3')
    if r3:
        _, _, order3, seq3 = r3
        for scene in order3:
            s = seq3.get(scene, [])
            pairs = laptop_ab.paired([(2 if m == 1 else m, f) for m, f in s])
            off = [f for m, f in s if m == 0]
            on = [f for m, f in s if m == 1]
            if off and on and pairs:
                lo, hi = laptop_ab.quartiles(pairs)
                print(f'  Native frame (step 2), {scene:<13} off {statistics.median(off):6.1f} fps, on '
                      f'{statistics.median(on):6.1f} fps: {statistics.median(pairs):+.1f}% '
                      f'({lo:+.1f}% .. {hi:+.1f}%, {len(pairs)} pairs)')
        print_native_split(logs, run)
    # Session 45: with the native frame on, native blending + fast clears of any size left out (mode 1) / in ("ab4").
    r4 = laptop_ab.analyze(logs, run, 'ab4')
    if r4:
        _, _, order4, seq4 = r4
        for scene in order4:
            s = seq4.get(scene, [])
            pairs = laptop_ab.paired([(2 if m == 1 else m, f) for m, f in s])
            with_parts = [f for m, f in s if m == 0]
            without = [f for m, f in s if m == 1]
            if with_parts and without and pairs:
                lo, hi = laptop_ab.quartiles(pairs)
                print(f'  Native frame without blending / small clears, {scene:<6} with {statistics.median(with_parts):6.1f} '
                      f'fps, without {statistics.median(without):6.1f} fps: {statistics.median(pairs):+.1f}% '
                      f'({lo:+.1f}% .. {hi:+.1f}%, {len(pairs)} pairs)')
    # Session 46: truth frames (gpu_native_truth, the no-EDRAM frame) on / off in the match against LOW ("ab5").
    r5 = laptop_ab.analyze(logs, run, 'ab5')
    if r5:
        _, _, order5, seq5 = r5
        for scene in order5:
            s = seq5.get(scene, [])
            pairs = laptop_ab.paired([(2 if m == 1 else m, f) for m, f in s])
            off = [f for m, f in s if m == 0]
            on = [f for m, f in s if m == 1]
            if off and on and pairs:
                lo, hi = laptop_ab.quartiles(pairs)
                print(f'  Truth frames (no EDRAM), {scene:<12} off {statistics.median(off):6.1f} fps, on '
                      f'{statistics.median(on):6.1f} fps: {statistics.median(pairs):+.1f}% '
                      f'({lo:+.1f}% .. {hi:+.1f}%, {len(pairs)} pairs)')
        print_edram_census(logs, run)
    print_breakdown(logs, run)
    print()


# Session 45 (step 2): the native renderer's own numbers, per 60 frames ([native][frame] census, [native][gpu] GPU ms
# by pass kind). A 60-frame line wholly inside the ab3 "on" windows serves 20+ resolves from its targets; "off" lines
# after the first "on" one serve none (lines straddling a switch are left out).
NATIVE_FRAME = re.compile(r'\[native\]\[frame\] frames (\d+)-(\d+) \((\d+) replaced\).*?from post targets ([\d.]+)'
                          r'.*?write-backs ([\d.]+) \(.*?loads ([\d.]+) \(.*?texture loads ([\d.]+) \(over resolved '
                          r'ranges [\d.]+, ([\d.]+) MB\)')
NATIVE_GPU = re.compile(r'\[native\]\[gpu\] frames (\d+)-(\d+): GPU ms per replaced frame - scene passes ([\d.]+), '
                        r'scene write-backs ([\d.]+), post passes ([\d.]+), post write-backs ([\d.]+)')


# Session 46: the EDRAM census ([native][edram], per 60 frames): lines wholly inside truth frames ("truth" count = the
# line's frames) against lines with none, in the match (after the first truth line).
NATIVE_EDRAM = re.compile(r'\[native\]\[edram\] frames (\d+)-(\d+) \((\d+) replaced, (\d+) truth\): write-backs ([\d.]+), '
                          r'loads ([\d.]+), clear mirrors ([\d.]+), fills ([\d.]+), emulated draws ([\d.]+), resolves '
                          r'reading it ([\d.]+) of ([\d.]+).*?EDRAM accesses ([\d.]+)')


def print_edram_census(logs, run):
    log = os.path.join(logs, f'wwe2k14_{run:03d}.log')
    if not os.path.exists(log):
        return
    lines = []
    for line in open(log, encoding='utf-8', errors='replace'):
        m = NATIVE_EDRAM.search(line)
        if m:
            f0, f1, truth = int(m.group(1)), int(m.group(2)), int(m.group(4))
            lines.append((f0, f1 - f0 + 1, truth, [float(m.group(i)) for i in range(5, 13)]))
    first = min((f0 for f0, n, t, _ in lines if t == n), default=None)
    if first is None:
        return
    arms = {'truth': [v for f0, n, t, v in lines if f0 >= first and t == n],
            'off': [v for f0, n, t, v in lines if f0 >= first and t == 0]}
    print('  Truth frames, the EDRAM census per frame (median of 60-frame lines):')
    print(f'    {"":5} {"lines":>5} {"wbacks":>7} {"loads":>6} {"mirror":>6} {"fills":>6} {"emul":>5} {"res rd":>6} '
          f'{"res":>5} {"access":>7}')
    for arm, vs in arms.items():
        if not vs:
            continue
        med = [statistics.median(v[i] for v in vs) for i in range(8)]
        print(f'    {arm:<5} {len(vs):5d} ' + ' '.join(f'{x:6.1f}' for x in med))


def print_native_split(logs, run):
    log = os.path.join(logs, f'wwe2k14_{run:03d}.log')
    if not os.path.exists(log):
        return
    census, gpu = {}, {}
    for line in open(log, encoding='utf-8', errors='replace'):
        m = NATIVE_FRAME.search(line)
        if m:
            census[int(m.group(1))] = [float(m.group(i)) for i in range(4, 9)]
            continue
        m = NATIVE_GPU.search(line)
        if m:
            gpu[int(m.group(1))] = [float(m.group(i)) for i in range(3, 7)]
    first_on = min((f for f, c in census.items() if c[0] >= 20), default=None)
    if first_on is None:
        return
    arms = {'on': [], 'off': []}
    for f in sorted(census):
        if f < first_on:
            continue
        served = census[f][0]
        arm = 'on' if served >= 20 else 'off' if served == 0 else None
        if arm:
            arms[arm].append(f)
    print('  Native frame (step 2), its own numbers per frame in the match (median of 60-frame lines):')
    print(f'    {"":4} {"lines":>5} {"served":>7} {"wbacks":>7} {"loads":>6} {"tex":>5} {"tex MB":>7} | GPU ms: '
          f'{"scene":>6} {"sc wb":>6} {"post":>6} {"post wb":>7}')
    for arm in ('on', 'off'):
        fs = arms[arm]
        if not fs:
            continue
        med = [statistics.median(census[f][i] for f in fs) for i in range(5)]
        g = [f for f in fs if f in gpu]
        gm = [statistics.median(gpu[f][i] for f in g) for i in range(4)] if g else None
        gtext = ' '.join(f'{v:6.2f}' for v in gm[:3]) + f' {gm[3]:7.2f}' if gm else '(no timestamps)'
        print(f'    {arm:<4} {len(fs):5d} {med[0]:7.1f} {med[1]:7.1f} {med[2]:6.1f} {med[3]:5.1f} {med[4]:7.2f} | '
              f'         {gtext}')


def from_zip(path):
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if '/' not in n and '\\' not in n]
            z.extractall(tmp, names)
        labels = {}
        about = os.path.join(tmp, 'about.txt')
        if os.path.exists(about):
            text = open(about, encoding='utf-8', errors='replace').read()
            print(text)
            for m in re.finditer(r'Run \d+: (.*?), log wwe2k14_(\d+)\.log', text):
                labels[int(m.group(2))] = m.group(1)
        for n in sorted(names):
            m = re.fullmatch(r'wwe2k14_(\d+)\.log', n)
            if m:
                print_run(tmp, int(m.group(1)), labels.get(int(m.group(1)), ''))
    return 0


def main():
    args = sys.argv[1:]
    if len(args) == 1 and args[0].lower().endswith('.zip'):
        return from_zip(args[0])
    if not args:
        print(__doc__)
        return 2
    logs = laptop_ab.DEFAULT_LOGS
    if not args[0].isdigit():
        logs, args = args[0], args[1:]
    for a in args:
        print_run(logs, int(a), '')
    return 0


if __name__ == '__main__':
    sys.exit(main())

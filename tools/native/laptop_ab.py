"""The laptop / playtester test of the native renderer (Session 39): frames per second, native renderer on vs off.

The route short\\s39_laptop_ab.txt (shipped as native_ab.txt; the launcher's PERFORMANCE TEST runs it) switches
gpu_native between 2 (the native renderer draws the scene) and 0 (the emulated renderer alone) every 120 frames and
logs "ab: mode N (frame F)"; --wwe2k14_file_trace=true logs the file opens that mark the scenes (Brock's titantron
343.bik2, Bryan's 324.bik2, the match's M3.PAC). From the run's log and its frames_NNN.csv this measures each window's
frames per second (the first 20 frames after a switch left out) and compares every "on" window with the "off"
windows right before and after it in the same scene (a paired comparison: neighbouring windows show nearly the same
camera shots, so the shots' own cost cancels). Per scene: the median change over the pairs (the estimate) and the
middle half of the pairs' changes (how much one pair varies: a change inside it on both sides of 0 is no change).

    py tools\\native\\laptop_ab.py RUN                        (a run of this PC: out\\build\\win-amd64-release\\logs)
    py tools\\native\\laptop_ab.py LOGS_FOLDER NNN            (a logs folder from another PC, run NNN)
    py tools\\native\\laptop_ab.py performance_test_DATE.zip  (the launcher's performance test)

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
DEFAULT_LOGS = os.path.join(HERE, '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
# Session 43: "ab" (the fast clears) and "ab2" (the Session 43 native set) series; a window of either ends at the next
# switch of any series.
MODE = re.compile(r'\[input\] line \d+: (ab\d*): mode (\d) \(frame (\d+)\)')
OPEN = re.compile(r"\[file\] NtCreateFile '([^']+)'.* frame=(\d+)")
TURBO_MATCH_OFF = re.compile(r'\[turbo\] match off at frame (\d+)')
DEVICE = re.compile(r'\[wwe2k14\]\[(?:gpu|light)\] (.*)')
SCENES = (('343.bik2', 'tron_343 (Brock)'), ('324.bik2', 'tron_324 (Bryan)'), ('m3.pac', 'match'))
SKIP = 20        # frames after a switch left out (the switch, the scene target learned again, first-time pipelines)
MIN_FRAMES = 40  # a window part shorter than this in one scene is left out


def analyze(logs, run, marker='ab'):
    """-> (device lines, light renderer?, scenes in order, {scene: [(mode, fps), ...] in time order}) or None."""
    log = os.path.join(logs, f'wwe2k14_{run:03d}.log')
    frames_csv = os.path.join(logs, f'frames_{run:03d}.csv')
    if not os.path.exists(log) or not os.path.exists(frames_csv):
        return None
    switches, starts, device = [], {}, []
    match_loaded = None  # Session 41: the match's load screen ends here (the load turbo's "match off" line)
    for line in open(log, encoding='utf-8', errors='replace'):
        m = MODE.search(line)
        if m:
            switches.append((int(m.group(3)), int(m.group(2)), m.group(1)))
            continue
        m = TURBO_MATCH_OFF.search(line)
        if m and match_loaded is None:
            match_loaded = int(m.group(1))
            continue
        m = OPEN.search(line)
        if m:
            path = m.group(1).replace('\\', '/').lower()
            for key, name in SCENES:
                if path.endswith(key) and name not in starts:
                    starts[name] = int(m.group(2))
            continue
        m = DEVICE.search(line)
        if m and len(device) < 6:
            device.append(m.group(1)[:160])
    if not any(s[2] == marker for s in switches):
        return None
    order = [n for n, _ in sorted(starts.items(), key=lambda kv: kv[1])]

    match_open = starts.get('match')

    def scene_of(frame):
        name = 'loading'
        for n in order:
            if frame >= starts[n]:
                name = n
        # Session 41: from the match file's open to the end of its load screen is loading, not the match (a window
        # there ran 380 fps against 265 in the match, on both renderers: run 2735 / 2736).
        if name == 'match' and match_loaded is not None and match_open <= frame < match_loaded:
            name = 'loading'
        return name
    times = {}
    with open(frames_csv, newline='') as fh:
        for row in csv.DictReader(fh):
            times[int(row['frame'])] = float(row['frame_ms'])
    seq = {}
    for i, (f0, mode, series) in enumerate(switches):
        f1 = switches[i + 1][0] if i + 1 < len(switches) else max(times) + 1
        if series != marker:
            continue
        parts = {}
        for f in range(f0 + SKIP, f1):
            if f in times:
                parts.setdefault(scene_of(f), []).append(times[f])
        for scene, ms in parts.items():
            if len(ms) >= MIN_FRAMES:
                seq.setdefault(scene, []).append((mode, 1000.0 * len(ms) / sum(ms)))
    light = any('render path host render targets' in d for d in device)
    return device, light, order, seq


def paired(seq):
    """Each "on" window against the mean of the "off" windows next to it -> the changes in per cent."""
    out = []
    for i, (mode, fps) in enumerate(seq):
        if mode != 2:
            continue
        near = [seq[j][1] for j in (i - 1, i + 1) if 0 <= j < len(seq) and seq[j][0] == 0]
        if near:
            out.append(100.0 * (fps / (sum(near) / len(near)) - 1))
    return out


def quartiles(v):
    v = sorted(v)
    if len(v) < 4:
        return v[0], v[-1]
    q = statistics.quantiles(v, n=4)
    return q[0], q[2]


def print_run(run, label, result):
    device, light, order, seq = result
    print(f'run {run}{": " + label if label else ""}')
    for d in device:
        print(f'  {d}')
    if light:
        print('  (light renderer: the native renderer cannot run here, so on and off are the same renderer: a check '
              'of the test itself, the estimate should be about 0)')
    print(f'  {"scene":<18} {"native off":>11} {"native on":>11} {"estimate":>9} {"middle half of the pairs":>26}')
    for scene in order:  # 'loading' left out: a few windows of different screens
        s = seq.get(scene, [])
        off = [f for m, f in s if m == 0]
        on = [f for m, f in s if m == 2]
        p = paired(s)
        if not off or not on or not p:
            print(f'  {scene:<18} (not enough windows)')
            continue
        lo, hi = quartiles(p)
        print(f'  {scene:<18} {statistics.median(off):7.1f} fps {statistics.median(on):7.1f} fps '
              f'{statistics.median(p):+8.1f}% {f"{lo:+.1f}% .. {hi:+.1f}% ({len(p)} pairs)":>26}')
    print('  (estimate: the median change of each "on" window against the "off" windows next to it; fps: medians)')


def from_zip(path):
    """The launcher's performance test .zip: about.txt and its run(s)."""
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if '/' not in n and '\\' not in n]
            z.extractall(tmp, names)
        about = os.path.join(tmp, 'about.txt')
        labels = {}
        if os.path.exists(about):
            text = open(about, encoding='utf-8', errors='replace').read()
            print(text)
            for m in re.finditer(r'Run \d+: (.*?), log wwe2k14_(\d+)\.log', text):
                labels[int(m.group(2))] = m.group(1)
        for n in sorted(names):
            m = re.fullmatch(r'wwe2k14_(\d+)\.log', n)
            if m:
                r = analyze(tmp, int(m.group(1)))
                if r:
                    print_run(int(m.group(1)), labels.get(int(m.group(1)), ''), r)
                    print()
    return 0


def main():
    if len(sys.argv) == 2 and sys.argv[1].lower().endswith('.zip'):
        return from_zip(sys.argv[1])
    if len(sys.argv) == 2:
        logs, run = DEFAULT_LOGS, int(sys.argv[1])
    elif len(sys.argv) == 3:
        logs, run = sys.argv[1], int(sys.argv[2])
    else:
        print(__doc__)
        return 2
    r = analyze(logs, run)
    if not r:
        print(f'run {run}: no "ab: mode" lines or no frames_{run:03d}.csv in {logs}')
        return 1
    print_run(run, '', r)
    return 0


if __name__ == '__main__':
    sys.exit(main())

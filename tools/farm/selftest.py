r"""The replay farm's own check (gate lane `farm`): positive control and planted faults, on two captures of the main checkout's corpus.
  py -I tools\farm\selftest.py
Captures: s100_heenan_build (100 frames, a post / sprite scene: no wrestler skin) and brock_s15 (16 frames, a full scene).
 1. the same rplay against itself (paired): no SLOWER, no DIFFERS, exit 0   (the noise band must not trip the thresholds)
 2. --delay-us 100 on the new arm: SLOWER on both captures, exit 1          (planted slowness)
 3. --isolate "!yCh_skin_" on the new arm (one effect family left out of every draw): "pictures differ" on brock_s15 (it draws skin),
    NOT on s100_heenan_build (it has none), exit 1                           (planted picture fault, with its own control)
Each run fails loudly when it cannot read its input. Results and pictures go to the main checkout's out\farm\selftest\ (local).
"""
import re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'farm'))
import farm  # noqa: E402

EXE = HERE / 'out' / 'renderer' / 'dev' / 'rplay.exe'
ONLY = 's100_heenan_build,brock_s15'
HEENAN, BROCK = 's100_heenan_build', 'brock_s15'
failures = []


def fail(msg):
    print('FAIL ' + msg)
    failures.append(msg)


def run_tree(cmd, timeout_s):
    """subprocess.run, but a timeout kills the whole tree (farm.py AND the rplay it started), not just farm.py."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = proc.communicate(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
        out, _ = proc.communicate()
        out = (out or '') + '\nFAIL timeout'
        proc.returncode = 124
    return subprocess.CompletedProcess(cmd, proc.returncode, out, '')


def synthetic_checks():
    """Logic checks on made-up runs (no rplay): each one FAILS on the code before the lane 22 review fixes."""
    import argparse, json, shutil
    import numpy as np
    from PIL import Image
    thr = argparse.Namespace(thr_median=farm.THR_MEDIAN, thr_p99=farm.THR_P99, thr_gpu=farm.THR_GPU)
    base = dict(frames=100, median_ms=5.0, p99_ms=7.0, first_ms=50.0, cpu_median_ms=4.5, gpu_median_ms=0.8, gpu_ts_ms=0.6)
    # (a) a GPU-only regression: the GPU time doubles (+0.6 ms), the total moves 10 % -> judged SLOWER by its own number
    new = dict(base, median_ms=5.5, p99_ms=7.5, gpu_ts_ms=1.2, gpu_median_ms=1.4)
    v, notes, _ = farm.judge_timing(new, base, thr)
    if v != 'SLOWER':
        fail('a doubled GPU time with a +10 % total must be SLOWER (the total alone does not see it)')
    # (b) the same total move with the GPU time unchanged stays ok (the total is inside the band)
    v, notes, _ = farm.judge_timing(dict(base, median_ms=5.5, p99_ms=7.5), base, thr)
    if v != 'ok':
        fail('+10 % total with an unchanged GPU time must stay ok')
    # (c) a tiny GPU time moving by +30 % (0.10 -> 0.13 ms) is under the 0.05 ms floor
    v, notes, _ = farm.judge_timing(dict(base, gpu_ts_ms=0.13), dict(base, gpu_ts_ms=0.10), thr)
    if v != 'ok':
        fail('a 0.1 ms GPU move is under the floor and must stay ok')
    # (d) picture noise comes from the REFERENCE, a jittery new build does not widen its own tolerance; and a shot neither build wrote is a note
    root = farm.MAIN / 'out' / 'farm' / 'selftest' / 'syn'
    if root.exists():
        shutil.rmtree(root)
    for arm in ('', 'ref'):
        shots = root / 'cap' / arm / 'run0' / 'shots'
        shots.mkdir(parents=True)
        img = np.zeros((64, 64, 3), np.uint8)
        if arm == '':
            img[:, :8] = 40  # 12 % of the pixels differ by 40 levels
        Image.fromarray(img).save(shots / 'frame_0000.png')
    entry = dict(base, status='ok', kind='dir', shot_frames=[0, 5], noise={'0': {'p999': 255.0, 'block': 0.0, 'share': 1.0}},
                 ref=dict(base, noise={'0': {'p999': 0.0, 'block': 0.0, 'share': 0.0}}))
    (root / 'run.json').write_text(json.dumps({'stamp': 'syn', 'rplay_mtime': 'x', 'ref': 'r', 'captures': {'cap': entry}}))
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        farm.cmd_compare(argparse.Namespace(run=str(root), base=None, **vars(thr)))
    text = buf.getvalue()
    if verdict(text, 'cap') != 'DIFFERS':
        fail('a new build with a jittery noise record must still be flagged against the reference (noise from the ref arm)\n' + text)
    if 'no picture in either build at frames 5' not in text:
        fail('a shot neither build presented must be a note, not a difference\n' + text)
    if 'picture missing' in text:
        fail('frame 5 (missing in both arms) must not be reported as missing')
    # S101 lane 47 (c): the clock check. A planted low-clock run (the 0.57 -> 2.06 ms bug: 2565 MHz vs ~1100 vs 300-700) is flagged; a steady set is not; a re-run that lifts it settles.
    steady = {('off', 0): 2565, ('off', 1): 2550, ('crowd', 0): 2520, ('crowd', 1): 2565}
    if farm.flag_low_clocks(steady):
        fail('steady clocks were flagged: %s' % farm.flag_low_clocks(steady))
    planted = {**steady, ('arena', 0): 1100, ('arena', 1): 2540, ('wrestlers', 0): 500, ('wrestlers', 1): 2560}
    if farm.flag_low_clocks(planted) != [('arena', 0), ('wrestlers', 0)]:
        fail('the planted low-clock runs were not flagged: %s' % farm.flag_low_clocks(planted))
    redone = []

    def redo_ok(k):
        redone.append(k)
        return 2540
    if farm.settle_clocks(dict(planted), redo_ok) != ([], []) or sorted(redone) != [('arena', 0), ('wrestlers', 0)]:
        fail('re-running the low runs did not settle them: %s' % redone)
    if farm.settle_clocks(dict(planted), lambda k: 400) != ([('arena', 0), ('wrestlers', 0)], []):
        fail('a clock that stays low after the re-run rounds must stay flagged (the capture FAILs)')
    # an arm low on EVERY run is systematic (the driver clocks it down): not re-run, reported as not comparable
    sysarm = {**steady, ('wrestlers', 0): 1065, ('wrestlers', 1): 675}
    redone.clear()
    if farm.settle_clocks(dict(sysarm), redo_ok) != ([], ['wrestlers']) or redone:
        fail('a systematically low arm must be reported, not re-run: %s' % redone)
    if farm.flag_low_clocks({('a', 0): None, ('a', 1): None}):
        fail('runs with no clock sample must not be judged')
    # D6: a rplay-args string with a quoted path with spaces is one argument
    if farm.split_args('--arena-ours --game "C:\\My Games\\x y"') != ['--arena-ours', '--game', 'C:\\My Games\\x y']:
        fail('split_args mangled a quoted path with spaces: %s' % farm.split_args('--arena-ours --game "C:\\My Games\\x y"'))
    print('synthetic checks done')


def stale_shots_check():
    """A reused --out is cleared per capture: a leftover from an earlier run must be gone."""
    out = farm.FARM / 'selftest' / 'stale'
    leftover = out / 's100cards' / 'run0' / 'shots' / 'frame_0099.png'
    leftover.parent.mkdir(parents=True, exist_ok=True)
    leftover.write_bytes(b'stale')
    p = run_tree([sys.executable, '-I', str(HERE / 'tools' / 'farm' / 'farm.py'), 'run', '--build', str(EXE), '--only', 's100cards', '--runs', '1', '--warm', '0', '--out', str(out)], 300)
    if p.returncode != 0 or leftover.exists():
        fail('a reused --out kept an earlier run\'s shot (or the run failed): ' + p.stdout[-300:])


def run_check(tag, rplay_args=''):
    out = farm.FARM / 'selftest' / tag
    cmd = [sys.executable, '-I', str(HERE / 'tools' / 'farm' / 'farm.py'), 'check', '--build', str(EXE), '--ref', str(EXE), '--only', ONLY,
           '--runs', '3', '--warm', '1', '--timeout', '300', '--out', str(out)]
    if rplay_args:
        cmd += ['--rplay-args', rplay_args]
    p = run_tree(cmd, 800)
    text = p.stdout + p.stderr
    print(f'--- {tag} (exit {p.returncode})')
    print('\n'.join(l for l in text.splitlines() if l.startswith('  ') or l.startswith('compare:') or l.startswith('FAIL')))
    return p.returncode, text


def verdict(text, name):
    m = re.search(r'^  (\S+)\s+' + re.escape(name) + ':', text, re.M)
    return m.group(1) if m else None


def main():
    if not EXE.exists():
        print(f'FAIL no rplay at {EXE} (build: tools\\renderer\\build.ps1)')
        return 1
    for n in (HEENAN, BROCK):
        if not (farm.MAIN / 'out' / 'captures' / n / 'images.bin').exists():
            print(f'FAIL capture {n} missing from {farm.MAIN / "out" / "captures"}')
            return 1

    synthetic_checks()
    stale_shots_check()

    rc, text = run_check('aa')
    if rc != 0 or verdict(text, HEENAN) != 'ok' or verdict(text, BROCK) != 'ok':
        fail('positive control: the same rplay against itself must pass on both captures')

    rc, text = run_check('delay', '--delay-us 100')
    if rc == 0 or verdict(text, HEENAN) != 'SLOWER' or verdict(text, BROCK) != 'SLOWER':
        fail('planted delay (--delay-us 100) must be flagged SLOWER on both captures')

    rc, text = run_check('effect', '--isolate !yCh_skin_')
    vb, vh = verdict(text, BROCK), verdict(text, HEENAN)
    if rc == 0 or vb is None or 'DIFFERS' not in vb:
        fail(f'planted effect fault must be flagged "pictures differ" on {BROCK} (got {vb})')
    if vh is None or 'DIFFERS' in vh:
        fail(f'planted effect fault must NOT flag {HEENAN}, which has no wrestler skin (got {vh})')

    if failures:
        print(f'farm selftest: {len(failures)} failure(s)')
        return 1
    print('farm selftest: PASS (A/A clean; delay flagged on both; effect fault flagged only where drawn)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

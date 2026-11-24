#!/usr/bin/env python3
r"""S103 lane GSTP: offline proof of wwe2k14_rdr_gpu_group_stamps in the STREAM-FREE build (the scene host, tools\scene\host_rig.exe built with -DWWE2K14_KEEP_STREAM=0). 0 game runs, 0 Surface runs.

  py -I tools\scene\gstp_check.py [--root DIR] [--capture a04_match] [--frames 33,56,78] [--job-timeout 180]

  unit     tools\scene\model_group_test.cpp: every loader label format -> its group (the ring per-key models are ring, not arena).  lint: tools\scene\gstp_lint.py --selftest.
  off      host_rig.exe --mode host, no switch: no [rdr][gpu][groups] line.
  on1/on4  --gpu-group-stamps 1 / 4. Hard rules:
             pictures      every presented frame byte-equal to the off run (max |diff| 0)
             sum           the groups' sum equals the pass's OWN stamp span (ticks[end] - ticks[begin], read straight from the stamps by the renderer, an independent reading): per pass, to the
                           table's rounding, and `groups did not add up` = 0 in the header
             step table    the same pass's mean in the per-step table (the same window, the same frames, per-step mean us) equals the pass's own span (5 % + 5 us)
             drop-free     chain broken 0, coverage short 0, not ready 0; the segments hold 100 % of the draws; min run 1: 0 % of the draws in another group's segment
             labels        no draw of the colour pass is labelled `other` / `unnamed`, and the groups this capture draws (arena, ring, props, screens, wrestler, crowd) are all present
  twins    each must FAIL its rule: group_chain (a boundary dropped: chain broken > 0 AND the sum rule), group_cover (coverage short > 0), group_nolabel (every model draw `other`: the labels rule).
  Not exercised by any PC capture (said, never silently passed): weapon, and 2D (text / sprite / movie in the frame pass): no capture in out\captures carries weapon sections or 2D rows; they are covered by the label table
  test and the Draw lint, and are read from the Surface log.
Each host_rig job has a hard timeout (--job-timeout, default 180 s) and a deadline of its own; the tail of a stuck job is printed. Counts and numbers only (hard rule 1); pictures go under out\ only."""
import argparse, concurrent.futures, os, re, subprocess, sys, tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FAILS = []

HEAD_RE = re.compile(r'\[rdr\]\[gpu\]\[groups\] (?P<why>[^:]+): group GPU table over (?P<frames>\d+) frames .*?min run (?P<min>\d+) draws.*?; (?P<stamps>[\d.]+) group stamps a frame;'
                     r' frames dropped: group results not ready (?P<nr>\d+), chain broken (?P<broken>\d+), coverage short (?P<cover>\d+); frames that ran out of stamps (?P<over>\d+);'
                     r' draws held by the segments (?P<held>\d+) of (?P<issued>\d+) the passes issued \((?P<pct>[\d.]+) %\); frames where the groups did not add up to the pass\'s own stamp span (?P<sumbad>\d+)')
PASS_RE = re.compile(r'\[rdr\]\[gpu\]\[groups\] (?P<cls>\w+) (?P<kind>\w+) #(?P<occ>\d+) image (?P<img>\d+) (?P<w>\d+)x(?P<h>\d+) fmt \d+ \[stencil_dead \w+\]: pass (?P<ms>[\d.]+) ms a frame \(its own stamp span (?P<direct>[\d.]+) ms\) over (?P<n>\d+) frames,'
                     r' (?P<draws>[\d.]+) draws a frame \(segments hold (?P<held>[\d.]+)\), (?P<segs>[\d.]+) segments a frame, (?P<mixed>[\d.]+) % of its draws')
GROUP_RE = re.compile(r'\[rdr\]\[gpu\]\[groups\]   (?P<name>\w+): (?P<mean>\d+) us mean / (?P<p90>\d+) us p90, (?P<n>\d+) of (?P<N>\d+) frames, (?P<draws>[\d.]+) draws a frame, (?P<ms>[\d.]+) ms a frame = (?P<share>[\d.]+) % of the pass')
STEP_RE = re.compile(r'\[rdr\]\[gpu\]\[passes\] (?P<cls>\w+) (?P<kind>\w+) #(?P<occ>\d+) image (?P<img>\d+) \d+x\d+ fmt \d+: (?P<mean>\d+) us mean / (?P<p90>\d+) us p90, (?P<n>\d+) of (?P<N>\d+) frames, (?P<ms>[\d.]+) ms a frame')
MUST = ('arena', 'ring', 'props', 'screens', 'wrestler', 'crowd')


def say(ok, msg):
    print(('PASS ' if ok else 'FAIL ') + msg)
    if not ok:
        FAILS.append(msg)


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else root


def parse(text):
    heads = [m for m in HEAD_RE.finditer(text)]
    d = dict(head=None, passes=[], steps=[m.groupdict() for m in STEP_RE.finditer(text)], lines=text.count('[rdr][gpu][groups]'))
    if not heads:
        return d
    head = next((m for m in heads if m.group('why').strip() == 'summary'), heads[0])   # the renderer logs its table when it closes ("last window"): the rig never reads it while the host runs
    d['head'] = head.groupdict()
    nxt = [m.start() for m in heads if m.start() > head.start()]
    tail = text[head.end(): (nxt[0] if nxt else len(text))]
    cur = None
    for line in tail.splitlines():
        m = PASS_RE.search(line)
        if m:
            cur = dict(m.groupdict(), groups=[])
            d['passes'].append(cur)
            continue
        m = GROUP_RE.search(line)
        if m and cur is not None:
            cur['groups'].append(m.groupdict())
    return d


def run(exe, cap, game, od, frames, size, extra, timeout):
    os.makedirs(od, exist_ok=True)
    cmd = [exe, '--mode', 'host', '--scene', os.path.join(cap, 'scene'), '--game', game, '--out', od, '--frames', frames, '--size', size] + extra
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=dict(os.environ, WWE_RIG_DEADLINE_S=str(max(30, timeout - 20))))
    except subprocess.TimeoutExpired as e:
        tail = ((e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')) + ((e.stderr or b'').decode(errors='replace') if isinstance(e.stderr, bytes) else (e.stderr or ''))
        print('TIMEOUT after %d s: %s\n--- tail of the job ---\n%s\n--- end ---' % (timeout, ' '.join(extra) or '(no switch)', '\n'.join(tail.splitlines()[-25:])))
        return -9, tail
    return p.returncode, p.stdout + p.stderr


def max_diff(a, b):
    if not (os.path.isfile(a) and os.path.isfile(b)):
        return None
    x, y = np.asarray(Image.open(a).convert('RGB'), dtype=np.int16), np.asarray(Image.open(b).convert('RGB'), dtype=np.int16)
    return -1 if x.shape != y.shape else int(np.abs(x - y).max())


def unit(root):
    exe = os.path.join(tempfile.gettempdir(), 'gstp_model_group_test_%d.exe' % os.getpid())
    p = subprocess.run(['C:\\Program Files\\LLVM\\bin\\clang++.exe', '-std=c++20', '-Wall', '-Wextra', '-Werror', os.path.join(root, 'tools', 'scene', 'model_group_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        say(False, 'unit: model_group_test does not compile:\n' + p.stdout + p.stderr)
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    say(p.returncode == 0 and 'passed' in p.stdout, 'unit: model_group_test: %s' % (p.stdout.strip().splitlines() or ['(no output)'])[-1])
    for ln in p.stdout.splitlines():
        if ln.startswith('FAIL'):
            print('   ' + ln)
    try:
        os.remove(exe)
    except OSError:
        pass
    p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'gstp_lint.py'), '--root', root, '--selftest'], capture_output=True, text=True, timeout=120)
    say(p.returncode == 0, 'lint: gstp_lint (G1 every Draw names its group, G2 every WaitHandled checked, G3 no LogGpuSummary in the rig): %s' % ((p.stdout.strip().splitlines() or ['(no output)'])[-1]))


def judge(res, tag):
    h = res['head']
    say(h is not None, '%s: the [rdr][gpu][groups] header is logged' % tag)
    if not h:
        return
    say(int(h['frames']) > 0, '%s: the lever read %s frames (%s stamps a frame)' % (tag, h['frames'], h['stamps']))
    say(int(h['nr']) == 0 and int(h['broken']) == 0 and int(h['cover']) == 0, '%s: no frame dropped (not ready %s, chain broken %s, coverage short %s)' % (tag, h['nr'], h['broken'], h['cover']))
    say(h['held'] == h['issued'] and int(h['issued']) > 0, '%s: the segments hold %s of %s draws the passes issued' % (tag, h['held'], h['issued']))
    say(int(h['sumbad']) == 0, '%s: the groups add up to the pass\'s own stamp span in every frame (frames that did not: %s)' % (tag, h['sumbad']))
    for p in res['passes']:
        tot = sum(float(g['ms']) for g in p['groups'])
        direct, pm = float(p['direct']), float(p['ms'])
        say(abs(tot - direct) <= 0.0002 * max(1, len(p['groups'])) + 0.002 * direct, '%s %s #%s image %s: the %d groups add up to %.4f ms, the pass\'s own stamp span (ticks[end] - ticks[begin]) is %.4f ms (to the table\'s rounding)' % (tag, p['cls'], p['occ'], p['img'], len(p['groups']), tot, direct))
        if tag == 'min run 1':
            say(float(p['mixed']) == 0.0, '%s %s #%s: %s %% of the draws sit in a segment of another group (must be 0)' % (tag, p['cls'], p['occ'], p['mixed']))
        step = [s for s in res['steps'] if s['cls'] == p['cls'] and s['occ'] == p['occ'] and s['img'] == p['img'] and s['kind'] == 'pass']
        if step:
            sm = float(step[-1]['mean']) / 1000.0   # per-step MEAN us over the frames that had the step (not the ms-a-frame column, which divides by every frame of the window)
            say(abs(sm - direct) <= 0.05 * max(sm, direct) + 0.005, '%s %s #%s: the per-step table (the same window and frames, per-step mean) reads %.4f ms for the same pass, its own stamp span %.4f ms (5 %% + 5 us)' % (tag, p['cls'], p['occ'], sm, direct))
        else:
            say(False, '%s %s #%s: the per-step table has no row for the pass to compare with' % (tag, p['cls'], p['occ']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture', default='a04_match')
    ap.add_argument('--frames', default='33,56,78')
    ap.add_argument('--job-timeout', type=int, default=180)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    game = os.path.join(main_dir, 'game_data', 'extracted')
    cap = os.path.join(main_dir, 'out', 'captures', a.capture)
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    if not os.path.isfile(exe):
        exe = os.path.join(main_dir, 'out', 'tools', 'host_rig.exe')
    unit(root)
    if not (os.path.isfile(exe) and os.path.isdir(os.path.join(cap, 'scene')) and os.path.isdir(game)):
        print('FAIL: need host_rig.exe (pwsh -NoProfile -File tools\\scene\\build_host_rig.ps1), the capture %s and the game files' % cap)
        return 1
    p = subprocess.run([exe, '--mode', 'sizes', '--capture', cap], capture_output=True, text=True)
    m = re.search(r'distinct sizes: (\d+)x(\d+)', p.stdout + p.stderr)
    size = '%sx%s' % (m.group(1), m.group(2)) if m else '1280x720'
    base = os.path.join(root, 'out', 'gstp', 'run')
    jobs = {'off': [], 'on1': ['--gpu-group-stamps', '1'], 'on4': ['--gpu-group-stamps', '4'],
            'p_chain': ['--plant', 'group_chain'], 'p_cover': ['--plant', 'group_cover'], 'p_nolabel': ['--plant', 'group_nolabel']}
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get('GATE_GPU_PROCS') or 3)) as ex:  # GATE_GPU_PROCS (set by the old gate system, now normally unset): how many to run; default 3
        futs = {k: ex.submit(run, exe, cap, game, os.path.join(base, k), a.frames, size, extra, a.job_timeout) for k, extra in jobs.items()}
        for k, f in futs.items():
            rc, text = f.result()
            res[k] = dict(rc=rc, text=text, **parse(text))
            say(rc == 0, '%s: host_rig exit code %d' % (k, rc))
    if any(res[k]['rc'] != 0 for k in ('off', 'on1', 'on4')):
        print('FAILED: %d' % len(FAILS))
        return 1
    say(res['off']['lines'] == 0, 'off: no [rdr][gpu][groups] line without the switch (%d)' % res['off']['lines'])
    frames = [int(f) for f in a.frames.split(',')]
    for k in ('on1', 'on4'):
        diffs = [max_diff(os.path.join(base, 'off', 'host_%d.png' % f), os.path.join(base, k, 'host_%d.png' % f)) for f in frames]
        say(all(d == 0 for d in diffs), 'pictures %s: max |diff| per presented frame %s against the off run (must all be 0)' % (k, diffs))
    judge(res['on1'], 'min run 1')
    judge(res['on4'], 'min run 4')
    h1, h4 = res['on1']['head'], res['on4']['head']
    if h1 and h4:
        say(float(h4['stamps']) <= float(h1['stamps']), 'runs: min run 4 writes %s stamps a frame, min run 1 writes %s (no more)' % (h4['stamps'], h1['stamps']))
    colour = [p for p in res['on1']['passes'] if p['cls'] == 'colour']
    say(bool(colour), 'table: a colour pass is in the table (%s)' % ', '.join('%s #%s image %s' % (p['cls'], p['occ'], p['img']) for p in res['on1']['passes']))
    if colour:
        top = max(colour, key=lambda p: float(p['ms']))
        names = {g['name']: g for g in top['groups']}
        say(all(n in names for n in MUST), 'labels: the groups this capture draws are all in the colour pass: %s (missing: %s)' % (', '.join('%s %s ms %s %%' % (g['name'], g['ms'], g['share']) for g in sorted(top['groups'], key=lambda g: -float(g['ms']))), [n for n in MUST if n not in names]))
        say('other' not in names and 'unnamed' not in names, 'labels: no draw of the colour pass is labelled other / unnamed (%s)' % [n for n in ('other', 'unnamed') if n in names])
        say(float(names['ring']['draws']) >= 1 if 'ring' in names else False, 'labels: the ring models (per-key "arena N ring K" and the posed pieces) are booked as ring: %s draws a frame (was arena)' % (names['ring']['draws'] if 'ring' in names else 'none'))
    pc, pv, pn = res['p_chain'], res['p_cover'], res['p_nolabel']
    say(pc['head'] is not None and int(pc['head']['broken']) > 0, 'twin group_chain (a boundary stamp written, its segment dropped): chain broken %s (must be > 0: the clean rule FAILS on it)' % (pc['head'] and pc['head']['broken']))
    say(pc['head'] is not None and int(pc['head']['sumbad']) + int(pc['head']['broken']) > 0, 'twin group_chain: the sum rule also sees it (groups did not add up %s, chain broken %s)' % (pc['head'] and pc['head']['sumbad'], pc['head'] and pc['head']['broken']))
    say(pv['head'] is not None and int(pv['head']['cover']) > 0, 'twin group_cover (a draw left out of the counts): coverage short %s (must be > 0)' % (pv['head'] and pv['head']['cover']))
    nc = [p for p in pn['passes'] if p['cls'] == 'colour']
    nn = {g['name'] for p in nc for g in p['groups']}
    say(bool(nc) and ('other' in nn) and not all(n in nn for n in MUST), 'twin group_nolabel (every model draw labelled other): colour-pass groups %s (the labels rule FAILS on it: other present, the named groups missing)' % sorted(nn))
    print('INFO not exercised on the PC: weapon, 2D (text / sprite / movie): no capture carries them; covered by the label table test + the Draw lint')
    print('FAILED: %d' % len(FAILS) if FAILS else 'ALL PASS')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

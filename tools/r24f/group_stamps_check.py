r"""S102 lane R24-F: offline proof of wwe2k14_rdr_gpu_group_stamps (GPU stamps at the change of draw group inside the colour / frame passes). 0 game runs, 0 Surface runs.
  py -I tools\r24f\group_stamps_check.py [--root DIR] [--only unit|bryan|brock]
  unit      tools\r24f\group_stamps_test.cpp: the planner, the budget, the chain check, the coverage count, the aggregation and both planted faults.
  bryan / brock   the gate's Bryan entrance capture (237 frames) and the Brock capture (375), the SHIPPED DEFAULT BUNDLE, replayed by rplay:
      off         no switch: no [rdr][gpu][groups] line at all (the draws carry no group, no stamp is written).
      pictures    shots under min run 1, min run 4 and validation 1 are BYTE-EQUAL to the off run (a stamp changes no output).
      validation  rplay --validation 1 with the switch on: `validation errors 0`, exit 0.
      table       min run 1: frames read > 0 (reports how many frames the lever took, of how many), no frame dropped (not ready / chain broken / coverage short), the segments hold
                  100.00 % of the draws the passes issued, no draw sits in a segment of another group (min run 1), the colour pass has >= 3 groups, the groups of each pass add up to
                  the pass's own ms (and to 100 % of it), and the pass ms equals the per-step table's (--gpu-pass-stamps 1, the independent reading of the same pass) within 5 %.
      runs        min run 4 writes fewer stamps a frame than min run 1 and still holds 100 % of the draws.
      twins       --gpu-group-plant 1 (a stamp written, its segment not recorded): `chain broken` > 0, the clean-run rule would FAIL on it; --gpu-group-plant 2 (a draw left out of the
                  counts): `coverage short` > 0.
Counts and numbers only (hard rule 1); pictures go under out\ only."""
import argparse, concurrent.futures, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'renderer'))
import colour_cuts_check as cc  # BUNDLE, shot_diffs, main_checkout, say, FAILS

FAILS = cc.FAILS
say = cc.say
CAPS = {
    'bryan': ('s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 120, [59, 60, 119]),   # (S102 build fast, prove small: 120 frames keep the whole gate under ~10 min)
    'brock': ('s101/lane92/case_20261113_093550_855_0_pc_run_exit/capture', 120, [60, 61, 119]),
}
HEAD_RE = re.compile(r'\[rdr\]\[gpu\]\[groups\] (?P<why>[^:]+): group GPU table over (?P<frames>\d+) frames .*?min run (?P<min>\d+) draws.*?; (?P<stamps>[\d.]+) group stamps a frame;'
                     r' frames dropped: group results not ready (?P<nr>\d+), chain broken (?P<broken>\d+), coverage short (?P<cover>\d+); frames that ran out of stamps (?P<over>\d+);'
                     r' draws held by the segments (?P<held>\d+) of (?P<issued>\d+) the passes issued \((?P<pct>[\d.]+) %\)')
PASS_RE = re.compile(r'\[rdr\]\[gpu\]\[groups\] (?P<cls>\w+) (?P<kind>\w+) #(?P<occ>\d+) image (?P<img>\d+) (?P<w>\d+)x(?P<h>\d+) fmt \d+ \[stencil_dead \w+\]: pass (?P<ms>[\d.]+) ms a frame over (?P<n>\d+) frames,'
                     r' (?P<draws>[\d.]+) draws a frame \(segments hold (?P<held>[\d.]+)\), (?P<segs>[\d.]+) segments a frame, (?P<mixed>[\d.]+) % of its draws')
GROUP_RE = re.compile(r'\[rdr\]\[gpu\]\[groups\]   (?P<name>\w+): (?P<mean>\d+) us mean / (?P<p90>\d+) us p90, (?P<n>\d+) of (?P<N>\d+) frames, (?P<draws>[\d.]+) draws a frame, (?P<ms>[\d.]+) ms a frame = (?P<share>[\d.]+) % of the pass')
STEP_RE = re.compile(r'\[rdr\]\[gpu\]\[passes\] (?P<cls>\w+) (?P<kind>\w+) #(?P<occ>\d+) image (?P<img>\d+) \d+x\d+ fmt \d+: (?P<mean>\d+) us mean / (?P<p90>\d+) us p90, (?P<n>\d+) of (?P<N>\d+) frames, (?P<ms>[\d.]+) ms a frame')
VAL_RE = re.compile(r'validation errors (\d+)')


def run_rplay(root, capture, game, tag, outdir, extra, frames, shots, timeout=1700):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    shot_dir = os.path.join(outdir, tag + '_shots')
    cmd = [exe, '--capture', capture, '--frames', str(frames), '--game', game, '--shots', ','.join(map(str, shots)), '--shot-dir', shot_dir] + cc.BUNDLE + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    text = p.stdout + p.stderr
    d = dict(tag=tag, rc=p.returncode, text=text, shots=shot_dir)
    v = VAL_RE.findall(text)
    d['val'] = int(v[-1]) if v else -1
    heads = [m for m in HEAD_RE.finditer(text)]
    d['head'] = heads[-1].groupdict() if heads else None
    d['heads'] = len(heads)
    # the table of the LAST header: the pass lines and group rows after it
    d['passes'] = []
    if heads:
        tail = text[heads[-1].end():]
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
    d['groups_lines'] = text.count('[rdr][gpu][groups]')
    d['steps'] = [m.groupdict() for m in STEP_RE.finditer(text)]
    return d


def equal_shots(a, b, shots):
    bad = []
    for f in shots:
        d = cc.shot_diffs(a, b, [f])
        if d is None:
            bad.append((f, 'missing'))
        elif d[0]['nd'] != 0:
            bad.append((f, d[0]['nd']))
    return len(shots) - len(bad), bad


def unit(root):
    exe = os.path.join(tempfile.gettempdir(), 'r24f_group_stamps_test_%d.exe' % os.getpid())
    p = subprocess.run(['C:\\Program Files\\LLVM\\bin\\clang++.exe', '-std=c++20', '-Wall', '-Wextra', '-Werror', os.path.join(root, 'tools', 'r24f', 'group_stamps_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        say(False, 'unit: the test does not compile:\n' + p.stdout + p.stderr)
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    fails = [l for l in p.stdout.splitlines() if l.startswith('FAIL')]
    say(p.returncode == 0 and not fails and 'all passed' in p.stdout, 'unit: group_stamps_test (%d lines, %d failed; both planted faults change an answer)' % (len(p.stdout.splitlines()), len(fails)))
    try:
        os.remove(exe)
    except OSError:
        pass


def capture_checks(name, root, outdir, game, main_co):
    sub, frames, shots = CAPS[name]
    capture = os.path.join(main_co, 'out', *sub.split('/'))
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        say(False, '%s: cannot read the capture %s' % (name, capture))
        return
    # (S102 "build fast, prove small": Bryan carries the full set, Brock only off / min run 1; min run 4 rides with validation)
    jobs = {
        'off': [],
        'on1': ['--gpu-group-stamps', '1', '--gpu-pass-stamps', '1'],
    }
    if name == 'bryan':
        jobs.update({
            'val': ['--gpu-group-stamps', '4', '--validation', '1'],
            'p1': ['--gpu-group-stamps', '1', '--gpu-group-plant', '1'],
            'p2': ['--gpu-group-stamps', '1', '--gpu-group-plant', '2'],
        })
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as ex:
        futs = {k: ex.submit(run_rplay, root, capture, game, '%s_%s' % (name, k), outdir, v, frames, shots) for k, v in jobs.items()}
        for k, f in futs.items():
            res[k] = f.result()
    for k, r in res.items():
        say(r['rc'] == 0, '%s %s: rplay exit code %d' % (name, k, r['rc']))
    if any(r['rc'] != 0 for r in res.values()):
        return
    off, on1 = res['off'], res['on1']
    on4 = res.get('val')
    say(off['groups_lines'] == 0, '%s off: no [rdr][gpu][groups] line without the switch (%d)' % (name, off['groups_lines']))
    for k in [k for k in ('on1', 'val') if k in res]:
        n, bad = equal_shots(off, res[k], shots)
        say(not bad, '%s pictures %s: %d of %d shots byte-equal to the off run%s' % (name, k, n, len(shots), '' if not bad else ' (differ: %s)' % bad))
    if on4:
        say(res['val']['val'] == 0, '%s validation 1 with the switch on: validation errors %d, exit %d' % (name, res['val']['val'], res['val']['rc']))
    h1, h4 = on1['head'], (on4 or on1)['head']
    say(h1 is not None and h4 is not None, '%s table: the [rdr][gpu][groups] header is logged (min run 1: %s, min run 4: %s)' % (name, bool(h1), bool(h4)))
    if not (h1 and h4):
        return
    for tag, h in (('min run 1', h1),) + ((('min run 4', h4),) if on4 else ()):
        say(int(h['frames']) > 0, '%s %s: the lever read %s frames of %d (%s stamps a frame)' % (name, tag, h['frames'], frames, h['stamps']))
        say(int(h['nr']) == 0 and int(h['broken']) == 0 and int(h['cover']) == 0, '%s %s: no frame dropped (not ready %s, chain broken %s, coverage short %s); ran out of stamps in %s frames' % (name, tag, h['nr'], h['broken'], h['cover'], h['over']))
        say(h['held'] == h['issued'] and int(h['issued']) > 0, '%s %s: the segments hold %s of %s draws the passes issued (%s %%)' % (name, tag, h['held'], h['issued'], h['pct']))
    if on4: say(float(h4['stamps']) < float(h1['stamps']), '%s runs: min run 4 writes %s stamps a frame, min run 1 writes %s (fewer, and still 100 %% of the draws)' % (name, h4['stamps'], h1['stamps']))
    # the passes
    colour = [p for p in on1['passes'] if p['cls'] == 'colour']
    say(bool(colour), '%s table: a colour pass is in the table (%d passes: %s)' % (name, len(on1['passes']), ', '.join('%s #%s image %s' % (p['cls'], p['occ'], p['img']) for p in on1['passes'])))
    for p in on1['passes']:
        tot = sum(float(g['ms']) for g in p['groups'])
        shares = sum(float(g['share']) for g in p['groups'])
        pass_ms = float(p['ms'])
        # (the group rows are printed to 4 decimals of ms and 1 of %: allow the rounding of up to ~30 rows)
        say(abs(tot - pass_ms) <= max(0.002, 0.005 * pass_ms) and abs(shares - 100.0) <= 1.0,
            '%s %s #%s image %s: the %d groups add up to %.4f ms of the pass'' %.4f ms (%.1f %%)' % (name, p['cls'], p['occ'], p['img'], len(p['groups']), tot, pass_ms, shares))
        say(float(p['mixed']) == 0.0, '%s %s #%s image %s: min run 1: %s %% of the draws sit in a segment of another group (must be 0)' % (name, p['cls'], p['occ'], p['img'], p['mixed']))
        say(abs(float(p['held']) - float(p['draws'])) < 0.01, '%s %s #%s image %s: segments hold %s of %s draws a frame' % (name, p['cls'], p['occ'], p['img'], p['held'], p['draws']))
    if colour:
        top = max(colour, key=lambda p: float(p['ms']))
        named = [g for g in top['groups'] if g['name'] not in ('unnamed', 'no_draws', 'other')]
        say(len(named) >= 3, '%s colour pass image %s (%s ms a frame): %d named groups: %s' % (name, top['img'], top['ms'], len(named),
            '; '.join('%s %s ms %s %%' % (g['name'], g['ms'], g['share']) for g in sorted(top['groups'], key=lambda g: -float(g['ms']))[:8])))
        # the independent reading of the same pass: the per-step table (--gpu-pass-stamps 1) in the same run
        step = [s for s in on1['steps'] if s['cls'] == top['cls'] and s['occ'] == top['occ'] and s['img'] == top['img'] and s['kind'] == 'pass']
        if step:
            sm = float(step[-1]['ms'])
            pm = float(top['ms'])
            say(abs(sm - pm) <= 0.05 * max(sm, pm) + 0.002, '%s colour pass: the groups add up to %.4f ms, the per-step table reads %.4f ms for the same pass (within 5 %%)' % (name, pm, sm))
        else:
            say(False, '%s colour pass: the per-step table has no row for it to compare with' % name)
    # the twins
    if name != 'bryan':
        return
    p1, p2 = res['p1']['head'], res['p2']['head']
    say(p1 is not None and int(p1['broken']) > 0, '%s twin plant 1 (a stamp written, its segment missing): chain broken %s (must be > 0: the clean rule "chain broken 0" FAILS on it)' % (name, p1 and p1['broken']))
    say(p2 is not None and int(p2['cover']) > 0, '%s twin plant 2 (a draw left out of the counts): coverage short %s (must be > 0: the clean rule "coverage short 0" FAILS on it)' % (name, p2 and p2['cover']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only')
    a = ap.parse_args()
    root = a.root
    main_co = cc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    outdir = os.path.join(root, 'out', 's102', 'r24f')
    os.makedirs(outdir, exist_ok=True)
    if not a.only or a.only == 'unit':
        unit(root)
    names = [n for n in ('bryan', 'brock') if not a.only or a.only == n]
    for name in names:
        capture_checks(name, root, outdir, game, main_co)
    print('FAILED: %d' % len(FAILS) if FAILS else 'ALL PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()

r"""S102 lane G10: the scene colour pass, split by program group (offline, 0 game runs, 0 Surface runs).
  py -I tools\renderer\colour_pass_rank.py [--root DIR] [--capture DIR] [--frames N] [--reps R] [--no-bundle] [--sheet-dir DIR]
rplay (this checkout's out\renderer\dev\rplay.exe) plays the Bryan entrance capture under the shipped default bundle (the --*-ours switches wwe2k14_app.h turns on) with GPU stamps on.
  census   `--draw-census 2`: per program name what the colour passes draw: draws, triangles, area bound (M1), vertex and fragment shader INVOCATIONS (a pipeline-statistics query per run of
           same-program draws: fragment invocations = shaded pixels, overdraw included). Groups below add them up.
  arms     `--arms`: ONE process, frame f drawn under arm f % N (rdr::SetDrawMod), each stamped frame's colour-class ms kept under its arm. Arms: base; SKIP group (every draw of the group left out:
           the group's marginal cost); SCISSOR group (the group's draws drawn with a 1x1 scissor: vertex + setup work only, no fragments). Group cost = base - skip; its fragment part =
           base - scissor; its vertex part = scissor - skip. Interleaving the arms frame by frame (entrance frames move slowly) makes one process a fair ranking; --reps R repeats it with the arm order
           rotated so a position never decides.
The ms are the PC's (RTX 4090, shared with other lanes: read them as a ranking, with the noise printed). The Surface estimate scales by the PC-to-Surface ratio of the stamped colour class (S0-5: 5.3 ms Surface).
Counts and numbers only (hard rule 1)."""
import argparse, collections, json, os, re, statistics, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# name substrings of the program names ("vertex + fragment") that make up each group, evidence: rplay --draw-census 2 under the bundle (docs\renderer\COLOUR_PASS.md)
GROUPS = collections.OrderedDict([
    ('crowd_members', ['yAudDefault', 'yAudience_lit', 'yAudience_lm_uv']),
    ('crowd_cards', ['yAudienceCard']),
    ('arena', ['yBG_']),
    ('ring_props', ['yProp_']),
    ('wrestlers', ['yCh_']),
    ('movie_misc', ['yMovieDefault', 'yPostBase', 'yRect_texcoord']),
])
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--ring-ours', '--flash-ours', '--props-ours', '--particles-ours', '--ui-ours', '--ui-text-ours', '--post-ours',
          '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--rim-ours', '--rim-chain-ours', '--small-passes-ours', '--circles-ours', '--caw-ours', '--movie-ours', '--screens-ours']
ARM_RE = re.compile(r'\[rplay\]\[arm\] (\S+): frames (\d+), mean/median ms: colour ([\d.]+)/([\d.]+) work ([\d.]+)/([\d.]+) total ([\d.]+)/([\d.]+) copies ([\d.]+)/([\d.]+) shadows ([\d.]+)/([\d.]+) rest ([\d.]+)/([\d.]+)')
DRAW_RE = re.compile(r'\[rplay\]\[draws\] ([\d.]+) draws (\d+) tris (\d+) px (\d+) vs (\d+) fs a frame: (.*)$')


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def rplay(root, args, timeout=1500):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    if not os.path.exists(exe):
        sys.exit('FAIL: %s does not exist (build it: pwsh tools\\renderer\\build.ps1)' % exe)
    p = subprocess.run([exe] + args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def group_of(name):
    for g, subs in GROUPS.items():
        if any(s in name for s in subs):
            return g
    return 'other'


def census(root, capture, game, frames, bundle):
    rc, text = rplay(root, ['--capture', capture, '--frames', str(frames), '--gpu-stamps', '1', '--draw-census', '2'] + (BUNDLE + ['--game', game] if bundle else []))
    rows = []
    for line in text.splitlines():
        m = DRAW_RE.search(line)
        if m:
            rows.append(dict(draws=float(m.group(1)), tris=int(m.group(2)), area=int(m.group(3)), vs=int(m.group(4)), fs=int(m.group(5)), name=m.group(6)))
    return rc, rows


def arms(root, capture, game, frames, bundle, specs, skip, scale=1.0):
    spec = '|'.join('%s=%d:%s' % (label, mode, names) for label, mode, names in specs)
    rc, text = rplay(root, ['--capture', capture, '--frames', str(frames), '--gpu-stamps', '1', '--gpu-log', os.path.join(tempfile.gettempdir(), 'g10_arms_%d.csv' % os.getpid()), '--arms', spec, '--arm-skip', str(skip), '--scale', str(scale)] + (BUNDLE + ['--game', game] if bundle else []))
    out = {}
    for line in text.splitlines():
        m = ARM_RE.search(line)
        if m:
            g = m.groups()
            out[g[0]] = dict(frames=int(g[1]), colour=(float(g[2]), float(g[3])), work=(float(g[4]), float(g[5])), total=(float(g[6]), float(g[7])), copies=(float(g[8]), float(g[9])),
                             shadows=(float(g[10]), float(g[11])), rest=(float(g[12]), float(g[13])))
    return rc, out, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture')
    ap.add_argument('--frames', type=int, default=237)
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--skip', type=int, default=4)
    ap.add_argument('--scale', type=float, default=1.0, help='render scale for the arms (3 = 3840x2160: the fragment work lifts above the 4090 noise; the vertex work stays)')
    ap.add_argument('--frag-arms', action='store_true', help='also a 1x1-scissor arm per group (more arms = fewer frames each)')
    ap.add_argument('--no-bundle', action='store_true')
    ap.add_argument('--no-census', action='store_true', help='arms only')
    ap.add_argument('--json')
    a = ap.parse_args()
    main_co = main_checkout(a.root)
    capture = a.capture or os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    bundle = not a.no_bundle
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        sys.exit('FAIL: capture %s cannot be read' % capture)
    rc, rows = (0, [dict(draws=0, tris=0, area=0, vs=0, fs=0, name='-')]) if a.no_census else census(a.root, capture, game, a.frames, bundle)
    if rc != 0 or not rows:
        sys.exit('FAIL: census run rc=%d rows=%d' % (rc, len(rows)))
    by = collections.OrderedDict((g, dict(draws=0, tris=0, area=0, vs=0, fs=0)) for g in list(GROUPS) + ['other'])
    for r in rows:
        g = by[group_of(r['name'])]
        for k in ('draws', 'tris', 'area', 'vs', 'fs'):
            g[k] += r[k]
    tot_fs = sum(g['fs'] for g in by.values()) or 1
    tot_vs = sum(g['vs'] for g in by.values()) or 1
    print('CENSUS (a frame, colour passes, bundle %s, %d frames)' % ('on' if bundle else 'off', a.frames))
    print('%-14s %9s %11s %14s %12s %12s %6s %6s' % ('group', 'draws', 'tris', 'area bound', 'vs inv', 'fs inv', 'fs %', 'vs %'))
    for name, g in by.items():
        print('%-14s %9.1f %11d %14d %12d %12d %5.1f%% %5.1f%%' % (name, g['draws'], g['tris'], g['area'], g['vs'], g['fs'], 100.0 * g['fs'] / tot_fs, 100.0 * g['vs'] / tot_vs))
    print('fragments shaded a frame %d = %.2f per pixel of 1280x720' % (tot_fs, tot_fs / (1280 * 720)))
    result = dict(census={k: v for k, v in by.items()}, rows=rows, arms=[])
    names = list(GROUPS)
    for rep in range(a.reps):
        specs = [('base', 0, '')]
        for g in names:
            specs.append(('skip_' + g, 1, ','.join(GROUPS[g])))
        if a.frag_arms:
            for g in names:
                specs.append(('frag_' + g, 2, ','.join(GROUPS[g])))
        specs.append(('skip_all', 1, '*'))
        specs.append(('frag_all', 2, '*'))
        k = rep % len(specs)
        specs = specs[k:] + specs[:k]  # rotate the arm order
        rc, out, text = arms(a.root, capture, game, a.frames, bundle, specs, a.skip, a.scale)
        if rc != 0 or 'base' not in out:
            sys.exit('FAIL: arms run %d rc=%d arms=%s' % (rep, rc, sorted(out)))
        result['arms'].append(out)
        print('rep %d: base colour %.3f ms (mean) %.3f (median) over %d frames' % (rep, out['base']['colour'][0], out['base']['colour'][1], out['base']['frames']))
    # medians over the reps
    def med(label, field='colour', which=1):
        return statistics.median(r[label][field][which] for r in result['arms'] if label in r)
    def spread(label, field='colour', which=1):
        v = [r[label][field][which] for r in result['arms'] if label in r]
        return (max(v) - min(v)) if v else 0
    base = med('base')
    print('\nRANKING (colour-class ms a frame on the PC, median over %d reps; spread = max - min over reps)' % a.reps)
    print('%-14s %9s %8s %9s %8s %9s %8s' % ('group', 'cost ms', 'spread', 'frag ms', 'spread', 'vert ms', '% base'))
    table = []
    for g in names + ['all']:
        skip = med('skip_' + g)
        frag = med('frag_' + g) if a.frag_arms or g == 'all' else base
        cost, fpart, vpart = base - skip, base - frag, frag - skip
        table.append((cost, g, fpart, vpart, spread('skip_' + g) + spread('base'), spread('frag_' + g) + spread('base')))
    for cost, g, fpart, vpart, s1, s2 in sorted(table, reverse=True):
        print('%-14s %9.4f %8.4f %9.4f %8.4f %9.4f %7.1f%%' % (g, cost, s1, fpart, s2, vpart, 100.0 * cost / base if base else 0))
    print('base colour %.4f ms; stamped work %.4f ms; total %.4f ms (median over the reps)' % (base, med('base', 'work'), med('base', 'total')))
    result['base'] = base
    if a.json:
        json.dump(result, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()

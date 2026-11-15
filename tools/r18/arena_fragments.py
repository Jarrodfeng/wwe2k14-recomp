r"""S102 lane R18-B3: where the arena's shaded fragments go (offline, 0 game runs, 0 Surface runs). Counts only (hard rule 1).
  py -I tools\r18\arena_fragments.py [--root DIR] [--capture DIR] [--frames N] [--from F] [--md FILE] [--reuse]
Two rplay runs under the shipped default bundle (the same BUNDLE colour_pass_rank.py uses; this script CALLS that file's constants, it does not edit it):
  base   `--draw-census 2 --draw-log`                       : every colour-pass draw with its state and its own fragment / vertex invocations (pipeline-statistics query per draw)
  wide   the same + `--colour-cuts 1 --arena-overdraw 1`    : the widest prepass the tree has today (G10 rule + C4 texture proof): the fragments of a draw AFTER its pre-draw = the ones that survive
Keyed by program x blend x depth write x compare x stencil x alpha-proof flag; each family gets the verdict `colour_cuts.h Eligible` gives it (the same order of tests, read from the state in the log) and,
for a draw the binder's alpha proof refused, the binder's dominant refusal reason for that program pair (`[rplay][alpha]` lines of the wide run). Vertex shares come from the same queries (the
brief's `--draw-mod 2` 1x1-scissor run measures the same vertex + setup work; the per-draw queries give it without a second run).
`--from F` keeps only frames >= F (the late stretch of the entrance), `--frames` is how many frames rplay plays."""
import argparse, collections, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'renderer'))
import colour_pass_rank as rank  # BUNDLE, main_checkout, rplay (G10's file, not edited)

EXE = None


def RPLAY(root, args):
    p = subprocess.run([EXE or os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')] + args, capture_output=True, text=True, timeout=3000)
    return p.returncode, p.stdout + p.stderr


LOG_RE = re.compile(r'f (\d+) p (\d+) i (\d+) (.*?) \| blend (\d+) (\d+) (\d+) (\d+) wm (\d+) dt (\d+) dw (\d+) cmp (\d+) st (\d+) alphaop (\d+) tris (\d+) area (\d+) tex (\d+) vb (\d+) vs (\d+) fs (\d+)(?: afoff (\d+))?')
ALPHA_RE = re.compile(r'\[rplay\]\[alpha\] (\d+) taken, (\d+) not alpha-simple, (\d+) product<1, (\d+) per-frame, (\d+) no colour, (\d+) vertex alpha<255: (.*?) \+ (.*?) \((\d+) refused for the texture(?:; (\d+) with a zero constants product)?\)')
FACTORS = {0: 'ZERO', 1: 'ONE', 6: 'SRC_ALPHA', 7: 'ONE_MINUS_SRC_ALPHA'}
REASONS = ['taken', 'not alpha-simple (the fragment multiplies in a texture / second texture / fade alpha, or the variant is not in the registry)', 'MdlAlpha x material alpha < 254.5/255',
           'per-frame vertex stream (colour not readable)', 'no readable vertex colour', 'a vertex alpha < 255', 'the slot-0 texture has alpha (or is not proven alpha-free)']


def load_log(path, first):
    rows = {}
    for line in open(path, errors='replace'):
        m = LOG_RE.match(line)
        if not m:
            continue
        g = m.groups()
        f = int(g[0])
        if f < first:
            continue
        rows[(f, int(g[1]), int(g[2]))] = dict(name=g[3], blend=tuple(map(int, g[4:8])), wm=int(g[8]), dt=int(g[9]), dw=int(g[10]), cmp=int(g[11]), st=int(g[12]), ao=int(g[13]), tris=int(g[14]),
                                               vs=int(g[18]), fs=int(g[19]))
    return rows


def blend_kind(r):
    en, src, dst, op = r['blend']
    if not en:
        return 0, 'off'
    if src == 6 and dst == 7 and op == 0:
        return 1, 'alpha'
    return 2, 'src=%s dst=%s op=%d' % (FACTORS.get(src, src), FACTORS.get(dst, dst), op)


def verdict(r, ao):
    """colour_cuts.h Eligible's order of tests, from the draw's state; ao = the binder's alpha-proof flag (what `named` is)."""
    if not (r['dt'] and r['dw']):
        return 'no depth write' if r['dt'] else 'no depth test'
    if r['st']:
        return 'stencil test'
    bk, _ = blend_kind(r)
    if bk > 1:
        return 'blended (not the plain alpha blend)'
    if not r['wm']:
        return 'no colour written'
    if r['cmp'] != 6:
        return 'other compare op %d' % r['cmp']
    if not ao:
        return 'alpha not proven 1'
    return 'ELIGIBLE (pre-drawn)'


def group_of(name):
    if 'yBG_' in name:
        return 'arena'
    for g, subs in rank.GROUPS.items():
        if any(s in name for s in subs):
            return g
    return 'other'


def run(root, capture, game, tag, outdir, extra, frames):
    log = os.path.join(outdir, tag + '.log')
    draws = os.path.join(outdir, tag + '_draws.txt')
    args = ['--capture', capture, '--frames', str(frames), '--gpu-stamps', '1', '--game', game] + rank.BUNDLE + ['--draw-census', '2', '--draw-log', draws] + extra
    rc, text = RPLAY(root, args)
    open(log, 'w').write(text)
    if rc != 0:
        sys.exit('FAIL: rplay %s exit %d (see %s)' % (tag, rc, log))
    return draws, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture')
    ap.add_argument('--frames', type=int, default=237)
    ap.add_argument('--from', dest='first', type=int, default=0)
    ap.add_argument('--md')
    ap.add_argument('--exe', help="an rplay.exe to use instead of this tree's own")
    ap.add_argument('--reuse', action='store_true', help='read the logs a previous run left in out\\s102\\r18b3')
    a = ap.parse_args()
    global EXE
    EXE = a.exe
    main_co = rank.main_checkout(a.root)
    capture = a.capture or os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        sys.exit('FAIL: capture %s cannot be read' % capture)
    outdir = os.path.join(a.root, 'out', 's102', 'r18b3')
    os.makedirs(outdir, exist_ok=True)
    if a.reuse and os.path.isfile(os.path.join(outdir, 'wide.log')):
        base_path, wide_path = os.path.join(outdir, 'base_draws.txt'), os.path.join(outdir, 'wide_draws.txt')
        wide_text = open(os.path.join(outdir, 'wide.log'), errors='replace').read()
    else:
        base_path, _ = run(a.root, capture, game, 'base', outdir, [], a.frames)
        wide_path, wide_text = run(a.root, capture, game, 'wide', outdir, ['--colour-cuts', '1', '--arena-overdraw', '1'], a.frames)
    base, wide = load_log(base_path, a.first), load_log(wide_path, a.first)
    if not base or not wide:
        sys.exit('FAIL: no draw lines read (base %d, wide %d)' % (len(base), len(wide)))
    frames = sorted({k[0] for k in base})
    nframes = len(frames)
    alpha = {}
    for m in ALPHA_RE.finditer(wide_text):
        alpha[m.group(7) + ' + ' + m.group(8)] = [int(m.group(i) or 0) for i in (1, 2, 3, 4, 5, 6, 9, 10)]
    fam = collections.OrderedDict()
    tot_fs = sum(r['fs'] for r in base.values())
    tot_vs = sum(r['vs'] for r in base.values())
    for key, r in base.items():
        w = wide.get(key)
        if w is None or w['name'] != r['name']:
            sys.exit('FAIL: draw %s differs between the runs (base %s, wide %s): the two runs must replay the same stream' % (key, r['name'], w['name'] if w else None))
        bk, bt = blend_kind(r)
        gname = group_of(r['name'])
        k = (gname, r['name'], bt, 'dw%d' % r['dw'] if r['dt'] else 'no depth test', 'st' if r['st'] else '-', 'ao%d' % w['ao'])
        f = fam.setdefault(k, dict(draws=0, fs=0, fs_wide=0, vs=0, tris=0, ao_base=r['ao'], verdict=verdict(r, w['ao']), verdict_base=verdict(r, r['ao'])))
        f['draws'] += 1
        f['fs'] += r['fs']
        f['fs_wide'] += w['fs']
        f['vs'] += r['vs']
        f['tris'] += r['tris']
    arena = {k: v for k, v in fam.items() if k[0] == 'arena'}
    a_fs = sum(v['fs'] for v in arena.values())
    a_wide = sum(v['fs_wide'] for v in arena.values())
    out = []
    P = out.append
    P('Arena fragments by family, %s frames %d to %d (%d frames), default bundle, per-draw pipeline-statistics queries (counts a frame in brackets)' % (os.path.basename(os.path.dirname(capture)), frames[0], frames[-1], nframes))
    P('all colour-pass fragments %d [%d a frame]; vertex invocations %d' % (tot_fs, tot_fs // nframes, tot_vs))
    P('arena (yBG_*) fragments %d = %.1f %% of the pass; with the widest prepass (G10 + C4) %d (%+.1f %%)' % (a_fs, 100.0 * a_fs / max(1, tot_fs), a_wide, 100.0 * (a_wide - a_fs) / max(1, a_fs)))
    P('')
    by_verdict = collections.OrderedDict()
    for k, v in arena.items():
        e = by_verdict.setdefault(v['verdict'], dict(draws=0, fs=0, fs_wide=0, vs=0))
        for c in ('draws', 'fs', 'fs_wide', 'vs'):
            e[c] += v[c if c != 'fs' else 'fs']
    P('BY VERDICT (colour_cuts.h Eligible, with the wide run\'s alpha proof): draws, fragments before, share of arena, fragments with the prepass, vertex invocations')
    for vd, e in sorted(by_verdict.items(), key=lambda kv: -kv[1]['fs']):
        P('  %-40s draws %7d [%6.1f] fs %10d [%9d] %5.1f %%  with prepass %10d  vs %9d' % (vd, e['draws'], e['draws'] / nframes, e['fs'], e['fs'] // nframes, 100.0 * e['fs'] / max(1, a_fs), e['fs_wide'], e['vs']))
    P('')
    P('BY FAMILY (program x blend x depth write x stencil x the binder\'s alpha-proof flag in the wide run), arena only, biggest first; fs after = with the prepass, share of the arena\'s fragments before')
    P('  %-62s %-26s %-6s %-3s %-4s %7s %10s %6s %10s %9s  %s' % ('program', 'blend', 'depth', 'st', 'proof', 'draws', 'fs', 'share', 'fs after', 'vs', 'verdict'))
    for k, v in sorted(arena.items(), key=lambda kv: -kv[1]['fs']):
        if v['fs'] == 0 and v['fs_wide'] == 0:
            continue
        P('  %-62s %-26s %-6s %-3s %-4s %7d %10d %5.1f%% %10d %9d  %s' % (k[1][:62], k[2][:26], k[3][:6], k[4], k[5], v['draws'], v['fs'], 100.0 * v['fs'] / max(1, a_fs), v['fs_wide'], v['vs'], v['verdict']))
    P('')
    P('WHY THE ALPHA PROOF REFUSED (wide run, the binder\'s own per-draw decisions over the run, per program pair, arena programs that have any refusal; columns = taken / not alpha-simple / product<1 / per-frame / no colour / vertex alpha<255 / texture / of the draws a zero constants product)')
    for name, n in sorted(alpha.items(), key=lambda kv: -sum(kv[1][1:])):
        if 'yBG_' in name and sum(n[1:]) > 0:
            P('  %-100s %s' % (name[:100], n))
    P('')
    P('CHECK: sum of the families %d against the pass total of the arena rows %d' % (sum(v['fs'] for v in arena.values()), a_fs))
    text = '\n'.join(out)
    print(text)
    if a.md:
        open(a.md, 'w').write(text + '\n')


if __name__ == '__main__':
    main()

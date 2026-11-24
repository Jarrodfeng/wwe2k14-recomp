r"""S102 lane R18-B3: offline proof of wwe2k14_rdr_arena_fragments (0 game runs, 0 Surface runs). Counts and pictures only (hard rule 1).
  py -I tools\r18\arena_fragments_check.py [--root DIR] [--smoke] [--sheet]
  --smoke   unit + the Bryan player-config pair (mask off / on) + fview validation: minutes (the first-version check; no plants, no second capture)
  mask 1    blend off for the arena draws the binder proved EXACTLY opaque (alpha constants each >= 1, vertex alpha 255, texture alpha proven): an EXACT lever: the pictures must be BYTE-EQUAL off / on.
            It removes destination reads and, with the benign alpha test dropped, lets early-Z reject hidden fragments: the colour-pass fragment total must not RISE. Bryan, with and without the C4 prepass (arena overdraw), and Brock.
  plants    af_blend_off_all (1): blend off for every arena draw          -> the picture must differ (caught)
            af_widen_translucent (2): ... also the yBG_Default_base alpha overlays (G10's first bug, black polygons on the ramp) -> differs
            af_vertexlit_alpha (3): ... also the vertex-lit family (G10's second bug, ~9 k wrong pixels) -> differs
            af_not_applied (4): the lever reads the mask and applies nothing -> "DID NOT APPLY" (draws blend-off = 0 while the real run takes > 0)
  unit      tools\r18\arena_fragments_test.cpp (the proof's constants, the blend decision, with twins).
  validation fview --validation 1 with the lever on: `validation errors 0` AND exit code 0.
rplay plays the standard Bryan entrance capture (237 frames) and the Brock capture (375 frames) under the SHIPPED DEFAULT BUNDLE (colour_pass_rank.py BUNDLE)."""
import argparse, concurrent.futures, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'renderer'))
import colour_pass_rank as rank
FAILS = []
SHOTS = [60, 120, 180, 236]
BSHOTS = [100, 200, 300, 370]
AF_RE = re.compile(r'\[rplay\]\[arena_fragments\] mask (\d+) plant (\d+): colour passes taken (\d+), draws blend-off (\d+), draws widened (\d+), declined states \(not a plain alpha blend\) (\d+), \(other target blended\) (\d+), \(float target\) (\d+), exact proofs (\d+), inexact (\d+)')
CUT_RE = re.compile(r'\[rplay\]\[colour_cuts\] mask (\d+) plant (\d+): colour passes with a prepass (\d+), draws pre-drawn (\d+)')
STORE_RE = re.compile(r'on-demand builds (\d+) \(([\d.]+) ms\), prebuilt from the list (\d+)')
VAL_RE = re.compile(r'validation errors (\d+)')
LOG_RE = re.compile(r'f (\d+) p (\d+) i (\d+) (.*?) \| blend .* vs (\d+) fs (\d+)(?: afoff (\d+))?')


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def run(root, exe, capture, game, tag, outdir, extra, frames, shots):
    shotdir = os.path.join(outdir, tag + '_shots')
    cmd = [exe, '--capture', capture, '--frames', str(frames), '--gpu-stamps', '1', '--game', game] + rank.BUNDLE + ['--shots', ','.join(map(str, shots)), '--shot-dir', shotdir] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=2400)
    text = p.stdout + p.stderr
    open(os.path.join(outdir, tag + '.log'), 'w').write(text)
    return dict(tag=tag, rc=p.returncode, text=text, shots=shotdir, draws=os.path.join(outdir, tag + '_draws.txt'))


def diffs(a, b, shots):
    import numpy as np
    from PIL import Image
    out = []
    for f in shots:
        pa, pb = os.path.join(a['shots'], 'frame_%04d.png' % f), os.path.join(b['shots'], 'frame_%04d.png' % f)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            return None
        d = np.abs(np.asarray(Image.open(pa).convert('RGB')).astype(int) - np.asarray(Image.open(pb).convert('RGB')).astype(int)).max(axis=2)
        out.append(dict(n0=int((d > 0).sum()), n8=int((d > 8).sum()), n32=int((d > 32).sum()), mx=int(d.max()), px=int(d.size)))
    return out


def af(r):
    m = AF_RE.search(r['text'])
    return dict(zip(('mask', 'plant', 'passes', 'off', 'widened', 'decl_plain', 'decl_multi', 'decl_float', 'exact', 'inexact'), map(int, m.groups()))) if m else None


def draw_totals(path):
    """(fragments of all colour-pass draws, fragments of the blend-off draws, blend-off draws, arena fragments) from a per-draw log."""
    tot = off = noff = arena = 0
    for line in open(path, errors='replace'):
        m = LOG_RE.match(line)
        if not m:
            continue
        fs = int(m.group(6))
        tot += fs
        if 'yBG_' in m.group(4):
            arena += fs
        if m.group(7) == '1':
            off += fs
            noff += 1
    return tot, off, noff, arena


def unit(root):
    exe = os.path.join(tempfile.gettempdir(), 'r18b3_af_test_%d.exe' % os.getpid())
    p = subprocess.run(['C:\\Program Files\\LLVM\\bin\\clang++.exe', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', os.path.join(root, 'src', 'renderer'), os.path.join(root, 'tools', 'r18', 'arena_fragments_test.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        say(False, 'unit: does not compile:\n' + p.stdout + p.stderr)
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    say(p.returncode == 0 and 'all passed' in p.stdout, 'unit: arena_fragments_test (%s)' % p.stdout.strip().splitlines()[-1])
    try:
        os.remove(exe)
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--smoke', action='store_true')
    a = ap.parse_args()
    root = a.root
    main_co = rank.main_checkout(root)
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    capture = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    brock = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    for c in (capture,) + (() if a.smoke else (brock,)):
        if not os.path.isfile(os.path.join(c, 'images.bin')):
            sys.exit('FAIL cannot read the capture %s' % c)
    if not os.path.exists(exe):
        sys.exit('FAIL %s is not built (tools\\renderer\\build.ps1)' % exe)
    outdir = os.path.join(root, 'out', 's102', 'r18b3', 'check')
    os.makedirs(outdir, exist_ok=True)
    unit(root)
    census = ['--draw-census', '2']

    def census_for(tag):
        return census + ['--draw-log', os.path.join(outdir, tag + '_draws.txt')]
    jobs = {
        'base': (capture, 237, SHOTS, census_for('base')),
        'bf': (capture, 237, SHOTS, census_for('bf') + ['--arena-fragments', '1']),
    }
    if not a.smoke:
        jobs.update({
            'od': (capture, 237, SHOTS, ['--arena-overdraw', '1']),
            'od_bf': (capture, 237, SHOTS, ['--arena-overdraw', '1', '--arena-fragments', '1']),
            'p1': (capture, 237, SHOTS, ['--arena-fragments', '1', '--arena-fragments-plant', '1']),
            'p2': (capture, 237, SHOTS, ['--arena-fragments', '1', '--arena-fragments-plant', '2']),
            'p3': (capture, 237, SHOTS, ['--arena-fragments', '1', '--arena-fragments-plant', '3']),
            'p4': (capture, 237, SHOTS, ['--arena-fragments', '1', '--arena-fragments-plant', '4']),
            'g10': (capture, 237, SHOTS, ['--colour-cuts', '1']),
            'g10_bf': (capture, 237, SHOTS, ['--colour-cuts', '1', '--arena-fragments', '1']),
            'order': (capture, 237, SHOTS, ['--arena-fragments', '1', '--arena-overdraw', '0']),
            'b_base': (brock, 375, BSHOTS, []),
            'b_bf': (brock, 375, BSHOTS, ['--arena-fragments', '1']),
        })
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
        futs = {k: ex.submit(run, root, exe, c, game, k, outdir, extra, n, sh) for k, (c, n, sh, extra) in jobs.items()}
        for k, f in futs.items():
            res[k] = f.result()
    for k, r in res.items():
        if r['rc'] != 0:
            say(False, '%s: rplay exit code %d' % (k, r['rc']))
    base, bf = res['base'], res['bf']
    s = af(bf)
    say(bool(s) and s['off'] > 0 and s['passes'] > 0, 'mask 1 took: %s' % (s if s else 'no [rplay][arena_fragments] line'))
    say(bool(s) and s['exact'] > 0, 'the exact proof held for %s draws (inexact %s): 0 = the lever does nothing for players' % (s['exact'] if s else '?', s['inexact'] if s else '?'))
    d = diffs(base, bf, SHOTS)
    say(d is not None and all(x['n0'] == 0 for x in d), 'bryan pictures byte-equal off / on (an exact lever): differing pixels %s' % ([x['n0'] for x in d] if d else None))
    tb, ob, nb, ab = draw_totals(base['draws'])
    tf, of, nf, af_ = draw_totals(bf['draws'])
    say(tf <= tb, 'colour-pass fragments do not rise %d -> %d (%+.1f %%): blend-off removes destination reads; the draws it takes also lose the benign alpha test (a kill in the shader), which lets the early depth test reject the hidden fragments' % (tb, tf, 100.0 * (tf - tb) / max(1, tb)))
    say(nf > 0 and of > 0, 'destination reads removed: %d draws, %d fragments (%.1f %% of the colour pass, %.1f %% of the arena); %d fragments a frame' % (nf, of, 100.0 * of / max(1, tf), 100.0 * of / max(1, af_), of // 237))
    if s:
        print('frames taken under the default bundle: colour passes taken %d of 474 (237 frames x 2 colour passes), draws blend-off %d (%.1f a frame)' % (s['passes'], s['off'], s['off'] / 237.0))
    if not a.smoke:
        od, od_bf = res['od'], res['od_bf']
        d = diffs(od, od_bf, SHOTS)
        say(d is not None and all(x['n0'] == 0 for x in d), 'bryan pictures byte-equal with the C4 prepass on: differing pixels %s' % ([x['n0'] for x in d] if d else None))
        s2 = af(od_bf)
        say(bool(s2) and s2['off'] > 0, 'mask 1 with the prepass took: %s' % (s2 if s2 else None))
        # review F2: G10's own switch (the LOW levers' prepass) with mask 1 must pre-draw exactly what G10 alone does (mask 1 asks for the texture proof, not for C4's widening)
        g, gb = res['g10'], res['g10_bf']
        mg, mgb = CUT_RE.search(g['text']), CUT_RE.search(gb['text'])
        say(bool(mg) and bool(mgb) and mg.group(4) == mgb.group(4), 'F2: --colour-cuts 1 draws pre-drawn %s alone, %s with --arena-fragments 1 (must be equal: no widening by the back door)' % (mg.group(4) if mg else '?', mgb.group(4) if mgb else '?'))
        d = diffs(g, gb, SHOTS)
        sg = af(gb)
        say(d is not None and all(x['n0'] == 0 for x in d) and bool(sg) and sg['off'] > 0, 'G10 on + mask 1: pictures byte-equal to G10 alone %s, blend-off draws %s' % ([x['n0'] for x in d] if d else None, sg['off'] if sg else '?'))
        # review F4: the switch order must not matter (the texture scan flag follows both)
        so = af(res['order'])
        say(bool(so) and bool(s) and so['off'] == s['off'], 'F4: --arena-fragments 1 --arena-overdraw 0 blend-off draws %s equal to --arena-fragments 1 alone %s' % (so['off'] if so else '?', s['off'] if s else '?'))
        # review F3: the blend-off pipelines are listed by the pipeline store: a second run on the same store builds fewer on demand and prebuilds from the list
        sdir = os.path.join(outdir, 'store')
        if os.path.isdir(sdir):
            import shutil
            shutil.rmtree(sdir, ignore_errors=True)
        r1 = run(root, exe, capture, game, 'store1', outdir, ['--arena-fragments', '1', '--pipeline-store', sdir, '--save-ms', '1000'], 60, [])
        r2 = run(root, exe, capture, game, 'store2', outdir, ['--arena-fragments', '1', '--pipeline-store', sdir, '--save-ms', '1000', '--prebuild'], 60, [])
        l1, l2 = STORE_RE.findall(r1['text']), STORE_RE.findall(r2['text'])
        say(bool(l1) and bool(l2) and int(l2[-1][0]) < int(l1[-1][0]) and int(l2[-1][2]) > 0, 'F3: the store lists the blend-off pipelines: on-demand builds %s -> %s, prebuilt on the second run %s' % (l1[-1][0] if l1 else None, l2[-1][0] if l2 else None, l2[-1][2] if l2 else None))
        d = diffs(res['b_base'], res['b_bf'], BSHOTS)
        sb = af(res['b_bf'])
        say(d is not None and all(x['n0'] == 0 for x in d), 'brock pictures byte-equal off / on: differing pixels %s (blend-off draws %s)' % ([x['n0'] for x in d] if d else None, sb['off'] if sb else '?'))
        for k, label in (('p1', 'af_blend_off_all'), ('p2', 'af_widen_translucent (G10 bug 1: the alpha overlays)'), ('p3', 'af_vertexlit_alpha (G10 bug 2: the vertex-lit family)')):
            d = diffs(base, res[k], SHOTS)
            n8 = sum(x['n8'] for x in d) if d else -1
            say(n8 > 100, 'plant %s: pixels differing by >8: %s (bar: more than 100 = caught)' % (label, [x['n8'] for x in d] if d else None))
        p4 = af(res['p4'])
        say(p4 is not None and p4['off'] == 0 and s and s['off'] > 0, 'plant af_not_applied: draws blend-off %s against the real run\'s %s: the check reads "DID NOT APPLY" as caught' % (p4['off'] if p4 else '?', s['off'] if s else '?'))
    # validation (fview, headless): the lever on, errors AND exit code
    if os.path.exists(fview):
        p = subprocess.run([fview, '--capture', capture, '--replay', '--frames', '100', '--validation', '1', '--game', game, '--arena-ours', '--crowd-ours', '--wrestlers-ours', '--ring-ours', '--arena-fragments', '1'],
                           capture_output=True, text=True, timeout=2400)
        t = p.stdout + p.stderr
        mm = VAL_RE.search(t)
        say(p.returncode == 0 and mm is not None and int(mm.group(1)) == 0, 'validation: fview --validation 1 with the lever on: errors %s, exit code %d (bar: 0 and 0)' % (mm.group(1) if mm else '?', p.returncode))
    else:
        say(False, 'validation: fview.exe is not built (tools\\fview\\build.ps1)')
    print('RESULT: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'PASS'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()

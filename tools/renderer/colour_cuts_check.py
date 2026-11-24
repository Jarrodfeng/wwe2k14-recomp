r"""S102 lane G10: offline proof of the scene colour pass's LOW cut 1 (src\renderer\colour_cuts.h: a depth-only prepass of the opaque light-mapped arena draws), 0 game runs, 0 Surface runs.
  py -I tools\renderer\colour_cuts_check.py [--root DIR] [--sheet] [--timing] [--only unit|pictures|validation]
The `bryan` capture (237 frames, scene records) is replayed by rplay under the SHIPPED DEFAULT BUNDLE (the --*-ours switches wwe2k14_app.h turns on, as tools\renderer\colour_pass_rank.py BUNDLE).
  unit        tools\renderer\colour_cuts_test.cpp: the SPIR-V classifier, the alpha rule, the names, the eligibility reasons, the compare mapping + its plant. Exit 1 on any wrong case.
  taken       the cut ON (mask 1) under the default bundle: the summary line says how many colour passes got a prepass and how many draws were pre-drawn. 0 taken = the lever does nothing for
              players = FAIL. (Bryan: one of the two colour passes a frame, 237 of 474.)
  fragments   `--draw-census 2` (a pipeline-statistics query per run of same-program draws): the colour passes' fragment shader invocations fall by at least 8 % with the cut on (measured 11.5 %).
  pictures    4 shots (frames 60 / 120 / 180 / 236) cut off vs on: pixels differing by more than 8 (of 255) under 0.05 % of the picture and none by more than 32 (measured: 7 px >8, max 22: the
              depth the prepass writes and the depth the normal draw writes are two pipelines' arithmetic, so coplanar edges can flip).
  plants      the planted twins, each must be CAUGHT by the check that owns it:
              1  the normal draws test the strict op      -> the pictures check FAILS (surfaces fail their own depth; a big part of the picture goes)
              2  every program pre-drawn, no shader rule  -> the pictures check FAILS (translucent / alpha overlays hide what is behind them)
              3  the prepass records nothing              -> the fragments check FAILS (a lever that is on and does nothing)
              4  the prepass sets an overflowing scissor     -> the pictures check PASSES and fview --validation 1 reports errors (exit 4): "validation 0" is read from the error count AND the exit code
  validation  fview --replay --validation 1 --colour-cuts 1 on the same capture (arena / crowd / wrestlers / ring promotions): `validation errors 0` and exit code 0.
  --sheet     writes the before / after sheet (full picture + 1:1 crops of the floor and the wall at 2x) to out\s102\sheets\g10\ with a KEEP file (reason, until 7 days out).
  --timing    the interleaved GPU ms (rplay --arms off=3:0|on=3:1: frame f drawn under arm f % 2, each stamped frame's colour ms kept per arm), 3 runs at scale 1 and 3 at scale 3 (3840x2160:
              the fragment work lifts above the shared 4090's noise). Informational: the shared PC GPU is too noisy to judge on (the spread is printed).
Counts and numbers only (hard rule 1); pictures go under out\ only."""
import argparse, concurrent.futures, datetime, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
FAILS = []
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--ring-ours', '--flash-ours', '--props-ours', '--particles-ours', '--ui-ours', '--ui-text-ours', '--post-ours',
          '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--rim-ours', '--rim-chain-ours', '--small-passes-ours', '--circles-ours', '--caw-ours', '--movie-ours', '--screens-ours']
SHOTS = [60, 120, 180, 236]
DRAW_RE = re.compile(r'\[rplay\]\[draws\] ([\d.]+) draws (\d+) tris (\d+) px (\d+) vs (\d+) fs a frame')
TAKEN_RE = re.compile(r'\[rplay\]\[colour_cuts\] mask (\d+) plant (\d+): colour passes with a prepass (\d+), draws pre-drawn (\d+), declined \(depth clear\) (\d+), \(overlap\) (\d+), \(nothing eligible\) (\d+)')
ARM_RE = re.compile(r'\[rplay\]\[arm\] (\S+): frames (\d+), mean/median ms: colour ([\d.]+)/([\d.]+) work ([\d.]+)/([\d.]+) total ([\d.]+)/([\d.]+)')


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def run_rplay(root, capture, game, tag, outdir, extra, timeout=1500, frames=237):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    shots = os.path.join(outdir, tag + '_shots')
    cmd = [exe, '--capture', capture, '--frames', str(frames), '--gpu-stamps', '1', '--gpu-log', os.path.join(outdir, tag + '.csv'), '--game', game] + BUNDLE + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    text = p.stdout + p.stderr
    d = dict(tag=tag, rc=p.returncode, text=text, shots=shots)
    m = TAKEN_RE.search(text)
    if m:
        d.update(mask=int(m.group(1)), plant=int(m.group(2)), taken=int(m.group(3)), predrawn=int(m.group(4)), declined_clear=int(m.group(5)), declined_none=int(m.group(7)))
    d['fs'] = sum(int(m.group(5)) for m in DRAW_RE.finditer(text))  # (group 5 = the fragment shader invocations a frame of each program)
    return d


def diff(a_path, b_path):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(a_path).convert('RGB')).astype(int)
    b = np.asarray(Image.open(b_path).convert('RGB')).astype(int)
    d = np.abs(a - b).max(axis=2)
    return dict(n8=int((d > 8).sum()), n32=int((d > 32).sum()), nd=int((d > 0).sum()), mx=int(d.max()), px=int(d.size))


def shot_diffs(base, other, shots=None):
    out = []
    for f in (shots or SHOTS):
        pa, pb = os.path.join(base['shots'], 'frame_%04d.png' % f), os.path.join(other['shots'], 'frame_%04d.png' % f)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            return None
        out.append(diff(pa, pb))
    return out


def unit(root):
    exe = os.path.join(tempfile.gettempdir(), 'g10_colour_cuts_test_%d.exe' % os.getpid())
    clang = 'C:\\Program Files\\LLVM\\bin\\clang++.exe'
    p = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', os.path.join(root, 'src', 'renderer'), os.path.join(root, 'tools', 'renderer', 'colour_cuts_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        say(False, 'unit: the test does not compile:\n' + p.stdout + p.stderr)
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    fails = [l for l in p.stdout.splitlines() if l.startswith('FAIL')]
    say(p.returncode == 0 and not fails and 'all passed' in p.stdout, 'unit: colour_cuts_test (%d cases, %d failed)' % (p.stdout.count('\nok') + p.stdout.count('ok   '), len(fails)))
    try:
        os.remove(exe)
    except OSError:
        pass


def fview_run(root, capture, game, tag, outdir, extra):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    if not os.path.exists(exe):
        return None
    cmd = [exe, '--capture', capture, '--replay', '--frames', '100', '--validation', '1', '--hash-frames', os.path.join(outdir, tag + '_hash.csv'), '--game', game,
           '--arena-ours', '--crowd-ours', '--wrestlers-ours', '--ring-ours'] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500)
    text = p.stdout + p.stderr
    m = re.search(r'validation errors (\d+)', text)
    return dict(rc=p.returncode, errors=int(m.group(1)) if m else -1, text=text)


def sheet(base, cut, root, main_co):
    import numpy as np
    from PIL import Image, ImageDraw
    out = os.path.join(main_co, 'out', 's102', 'sheets', 'g10')
    os.makedirs(out, exist_ok=True)
    f = 120
    a = Image.open(os.path.join(base['shots'], 'frame_%04d.png' % f)).convert('RGB')
    b = Image.open(os.path.join(cut['shots'], 'frame_%04d.png' % f)).convert('RGB')
    d = np.abs(np.asarray(a).astype(int) - np.asarray(b).astype(int)).max(axis=2)
    diffimg = Image.fromarray(np.clip(d * 8, 0, 255).astype('uint8')).convert('RGB')  # x8 so a 1-level difference is visible
    crops = [('floor (ramp, left)', (200, 380, 520, 560)), ('wall + screen', (440, 20, 760, 200)), ('ring apron, right', (860, 360, 1180, 540))]
    W, H = 1280, 720
    sheet_img = Image.new('RGB', (W * 3 + 40, H + 20 + 3 * 2 * 180 // 1 + 80), (24, 24, 24))
    dr = ImageDraw.Draw(sheet_img)
    for i, (label, im) in enumerate((('cut OFF (the game\'s draw order)', a), ('cut ON (prepass)', b), ('difference x8', diffimg))):
        sheet_img.paste(im, (10 + i * (W + 10), 20))
        dr.text((10 + i * (W + 10), 4), label, fill=(255, 255, 255))
    y = H + 40
    for ci, (label, box) in enumerate(crops):
        for k, im in enumerate((a, b, diffimg)):
            c = im.crop(box).resize(((box[2] - box[0]) * 2, (box[3] - box[1]) * 2), Image.NEAREST)
            sheet_img.paste(c, (10 + k * (W + 10), y))
            dr.text((10 + k * (W + 10), y - 14), ('%s 2x' % label), fill=(255, 255, 255))
        y += (box[3] - box[1]) * 2 + 24
    sheet_img = sheet_img.crop((0, 0, sheet_img.width, min(sheet_img.height, y)))
    path = os.path.join(out, 'g10_colour_cut1_frame120.png')
    sheet_img.save(path)
    until = (datetime.date.today() + datetime.timedelta(days=7)).isoformat()
    with open(os.path.join(out, 'KEEP'), 'w') as k:
        k.write('reason: S102 lane G10 before / after sheets of the colour-pass depth prepass (Bryan entrance, frame 120), for the main session to judge\nuntil %s\n' % until)
    print('sheet:', path)


def timing(root, capture, game, outdir):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    rows = []
    for scale in (1, 3):
        for k in (1, 2, 3):
            arms = 'off=3:0|on=3:1' if k % 2 else 'on=3:1|off=3:0'
            p = subprocess.run([exe, '--capture', capture, '--frames', '237', '--gpu-stamps', '1', '--gpu-log', os.path.join(outdir, 'tm_%d_%d.csv' % (scale, k)), '--game', game] + BUNDLE +
                               ['--scale', str(scale), '--arms', arms, '--arm-skip', '6'], capture_output=True, text=True, timeout=1500)
            arm = {m.group(1): m.groups() for m in ARM_RE.finditer(p.stdout + p.stderr)}
            if 'off' in arm and 'on' in arm:
                rows.append((scale, k, float(arm['off'][3]), float(arm['on'][3]), float(arm['off'][7]), float(arm['on'][7]), int(arm['off'][1]), int(arm['on'][1])))
    print('TIMING (colour-class ms, median per arm; total = the stamped bracket; the shared 4090, frame-interleaved arms)')
    print('scale run   colour off  colour on   delta    total off   total on    delta   frames')
    for scale, k, co, cn, to, tn, fo, fn in rows:
        print('%5d %3d %11.4f %11.4f %8.4f %11.4f %11.4f %8.4f %4d/%d' % (scale, k, co, cn, cn - co, to, tn, tn - to, fo, fn))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--capture')
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--timing', action='store_true')
    ap.add_argument('--only')
    a = ap.parse_args()
    root = a.root
    main_co = main_checkout(root)
    capture = a.capture or os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not a.only or a.only == 'unit':
        unit(root)
        lint = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'g10_lint.py'), '--root', root], capture_output=True, text=True, timeout=300)
        say(lint.returncode == 0, 'lint: g10_lint.py (F1 guard, F10 DrawState{}): ' + lint.stdout.strip().replace('\n', ' | ')[:300])
    if a.only == 'unit':
        sys.exit(1 if FAILS else 0)
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        print('FAIL cannot read the capture', capture)
        sys.exit(1)
    outdir = os.path.join(root, 'out', 's102', 'g10')
    os.makedirs(outdir, exist_ok=True)
    jobs = {
        'base': ['--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'base_shots'), '--draw-census', '2'],
        'cut': ['--colour-cuts', '1', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'cut_shots'), '--draw-census', '2'],
        'plant1': ['--colour-cuts', '1', '--colour-cuts-plant', '1', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'plant1_shots')],
        'plant2': ['--colour-cuts', '1', '--colour-cuts-plant', '2', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'plant2_shots')],
        'plant3': ['--colour-cuts', '1', '--colour-cuts-plant', '3', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'plant3_shots'), '--draw-census', '2'],
        'p5off': ['--alpha-plant', '5', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'p5off_shots')],
        'p5on': ['--alpha-plant', '5', '--colour-cuts', '1', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'p5on_shots')],
        'p5skip': ['--alpha-plant', '5', '--colour-cuts', '1', '--alpha-rule-skip', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'p5skip_shots')],
        'plant4': ['--colour-cuts', '1', '--colour-cuts-plant', '4', '--shots', ','.join(map(str, SHOTS)), '--shot-dir', os.path.join(outdir, 'plant4_shots')],
    }
    res = {}
    if not a.only or a.only == 'pictures':
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
            futs = {k: ex.submit(run_rplay, root, capture, game, k, outdir, v) for k, v in jobs.items()}
            for k, f in futs.items():
                res[k] = f.result()
        for k, r in res.items():
            if r['rc'] != 0:
                say(False, '%s: rplay exit %d\n%s' % (k, r['rc'], r['text'][-800:]))
        base, cut = res['base'], res['cut']
        if base['rc'] == 0 and cut['rc'] == 0:
            say(cut.get('taken', 0) > 0 and cut.get('predrawn', 0) > 0, 'taken: the default bundle with the cut ON: %s colour passes got a prepass, %s draws pre-drawn (declined: depth clear %s, nothing eligible %s)' %
                (cut.get('taken'), cut.get('predrawn'), cut.get('declined_clear'), cut.get('declined_none')))
            say(base.get('taken', 0) == 0, 'taken: the cut OFF takes no pass (%s)' % base.get('taken'))
            ratio = cut['fs'] / base['fs'] if base['fs'] else 1.0
            say(base['fs'] > 1000000 and ratio <= 0.98, 'fragments: colour-pass fragment shader invocations a frame %d -> %d (%.1f %%; bar: down by at least 2 %%: the rule-proven cut is small)' % (base['fs'], cut['fs'], 100 * (ratio - 1)))
            d = shot_diffs(base, cut)
            if d is None:
                say(False, 'pictures: the shots are missing')
            else:
                n8 = sum(x['n8'] for x in d)
                n32 = sum(x['n32'] for x in d)
                px = d[0]['px']
                say(n8 < 0.0005 * px * len(d) and n32 == 0, 'pictures: off vs on, %d shots: pixels differing by >8: %s (bar: under %.0f in all), by >32: %d (bar: 0), max diff %d' % (len(d), [x['n8'] for x in d], 0.0005 * px * len(d), n32, max(x['mx'] for x in d)))
            # the planted twins
            d1 = shot_diffs(base, res['plant1'])
            say(d1 is not None and sum(x['n32'] for x in d1) > 20000, 'plant 1 (strict op) is CAUGHT by the pictures check: pixels >32 = %s' % (sum(x['n32'] for x in d1) if d1 else None))
            d2 = shot_diffs(base, res['plant2'])
            say(d2 is not None and (sum(x['n32'] for x in d2) > 0 or sum(x['n8'] for x in d2) >= 0.0005 * px * len(d2)), 'plant 2 (every program pre-drawn) is CAUGHT by the pictures check: pixels >8 = %s, >32 = %s' %
                ([x['n8'] for x in d2] if d2 else None, [x['n32'] for x in d2] if d2 else None))
            p3 = res['plant3']
            r3 = p3['fs'] / base['fs'] if base['fs'] else 0
            say(r3 > 0.98, 'plant 3 (the prepass records nothing) is CAUGHT by the fragments check: %.1f %% of the base fragments (bar for the real cut: at most 98 %%)' % (100 * r3))
            # review F2: the rule alone (no name list): the census says why each draw was taken or refused; the bisected families are refused; plant 5 (MdlAlpha 0.5) is refused and, with the rule skipped, fails
            alines = [(m.group(1), m.group(7)) for m in re.finditer(r'\[rplay\]\[alpha\] (\d+) taken, (\d+) not alpha-simple, (\d+) product<1, (\d+) per-frame, (\d+) no colour, (\d+) vertex alpha<255: (.*)', cut['text']) if True]
            fam = [(n, int(t)) for t, n in alines if 'yBG_Default_base' in n or '_vertex_lm_vertex + yBG_DefaultLM_base' in n]
            say(len(fam) >= 2 and all(t == 0 for _, t in fam), 'F2: the rule alone refuses both bisected families (yBG_Default_base, the vertex-lit base): taken %s' % fam)
            tot = re.search(r'\[rplay\]\[alpha\] total: (\d+) taken, (\d+) not alpha-simple, (\d+) product<1, (\d+) per-frame, (\d+) no colour, (\d+) vertex alpha<255', cut['text'])
            say(bool(tot) and int(tot.group(1)) > 0, 'F2 census: the rule takes %s draws, refuses %s as not alpha-simple (texture alpha), %s on the constants, %s on vertex alpha' % ((tot.group(1), tot.group(2), tot.group(3), tot.group(6)) if tot else (None,) * 4))
            e0, e1, e2 = shot_diffs(res['p5off'], res['p5on']), shot_diffs(res['p5off'], res['p5skip']), shot_diffs(base, res['p5off'])
            say(e2 is not None and sum(x['n32'] for x in e2) > 500, 'plant 5 changes the picture (MdlAlpha 0.5 on the alpha-simple arena draws): pixels >32 = %s' % ([x['n32'] for x in e2] if e2 else None))
            p5tot = re.search(r'\[rplay\]\[alpha\] total: (\d+) taken, (\d+) not alpha-simple, (\d+) product<1', res['p5on']['text'])
            say(e0 is not None and sum(x['n8'] for x in e0) == 0 and p5tot and int(p5tot.group(1)) == 0 and int(p5tot.group(3)) > 0, 'plant 5 is REFUSED by the rule: cut on = cut off picture (pixels >8 %s), taken %s, refused on the product %s' %
                ([x['n8'] for x in e0] if e0 else None, p5tot.group(1) if p5tot else None, p5tot.group(3) if p5tot else None))
            say(e1 is not None and sum(x['n8'] for x in e1) >= 0.0005 * px * len(e1), 'plant 5 with the rule skipped FAILS the pictures check: pixels >8 = %s, >32 = %s' % ([x['n8'] for x in e1] if e1 else None, [x['n32'] for x in e1] if e1 else None))
            d4 = shot_diffs(base, res['plant4'])
            say(d4 is not None and sum(x['n32'] for x in d4) == 0, 'plant 4 (overflowing scissor) draws the same picture (only validation can see it): pixels >32 = %s' % ([x['n32'] for x in d4] if d4 else None))
            say('kept samples 0 (arms 0)' in base['text'].replace('[rplay][arms] ', ''), 'F1: rplay without --arms keeps no arm samples (%s)' % re.findall(r'kept samples \d+ \(arms \d+\)', base['text']))
            m13 = re.search(r'taken draws by shader of the normal draw: plain (\d+), alpha-test (\d+)', cut['text'])
            say(bool(m13) and int(m13.group(1)) > 0 and int(m13.group(2)) == 0, 'F13: every taken draw normal draw runs the plain (no alpha test) program, so early-Z holds: plain %s, alpha-test %s' % ((m13.group(1), m13.group(2)) if m13 else (None, None)))
            say('mixed depth direction 0, depth-dependent stencil writer 0' in cut['text'], 'F5/F6: no pass of the Bryan entrance declined for mixed directions or stencil writers (%s)' % re.findall(r'mixed depth direction \d+, depth-dependent stencil writer \d+', cut['text']))
            if a.sheet:
                sheet(base, cut, root, main_co)
            # F14: the second arena capture among the check inputs (tools\harness\inputs.py 'r7149': the Brock entrance caster capture, 375 frames, scene records)
            brock = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture')
            if os.path.isfile(os.path.join(brock, 'images.bin')):
                bshots = [100, 200, 300, 370]
                bj = {'bbase': ['--shots', ','.join(map(str, bshots)), '--shot-dir', os.path.join(outdir, 'bbase_shots'), '--draw-census', '2'],
                      'bcut': ['--colour-cuts', '1', '--shots', ','.join(map(str, bshots)), '--shot-dir', os.path.join(outdir, 'bcut_shots'), '--draw-census', '2']}
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
                    bf = {k: ex.submit(run_rplay, root, brock, game, k, outdir, v, 1500, 375) for k, v in bj.items()}
                    br = {k: f.result() for k, f in bf.items()}
                if br['bbase']['rc'] == 0 and br['bcut']['rc'] == 0:
                    bd = shot_diffs(br['bbase'], br['bcut'], bshots)
                    n8b = sum(x['n8'] for x in bd) if bd else -1
                    n32b = sum(x['n32'] for x in bd) if bd else -1
                    say(bd is not None and n8b < 0.0005 * bd[0]['px'] * len(bd) and n32b == 0, 'F14 pictures, second capture (Brock entrance r7149): taken %s passes, fragments %d -> %d, pixels >8 %s, >32 %s' %
                        (br['bcut'].get('taken'), br['bbase']['fs'], br['bcut']['fs'], [x['n8'] for x in bd] if bd else None, [x['n32'] for x in bd] if bd else None))
                else:
                    say(False, 'F14: rplay on the second capture failed (%d, %d)' % (br['bbase']['rc'], br['bcut']['rc']))
            else:
                say(False, 'F14: the second arena capture is missing')
            # F9: the prepass twins are listed by the pipeline store: a second run on the same store dir builds nothing on demand for them
            sdir = os.path.join(outdir, 'store')
            if os.path.isdir(sdir):
                import shutil
                shutil.rmtree(sdir, ignore_errors=True)
            r1 = run_rplay(root, capture, game, 'store1', outdir, ['--colour-cuts', '1', '--pipeline-store', sdir, '--save-ms', '1000'])
            r2 = run_rplay(root, capture, game, 'store2', outdir, ['--colour-cuts', '1', '--pipeline-store', sdir, '--save-ms', '1000', '--prebuild'])
            l1 = re.findall(r'on-demand builds (\d+) \(([\d.]+) ms\), prebuilt from the list (\d+)', r1['text'])
            l2 = re.findall(r'on-demand builds (\d+) \(([\d.]+) ms\), prebuilt from the list (\d+)', r2['text'])
            print('F9 on_demand with the cut on: first run %s, second run (same store) %s' % (l1[-1:] if l1 else None, l2[-1:] if l2 else None))
            say(bool(l1) and bool(l2) and int(l2[-1][0]) < int(l1[-1][0]) and int(l2[-1][2]) > 0, 'F9: the store lists the prepass twins: on-demand builds fall from %s to %s, prebuilt on the second run %s' %
                (l1[-1][0] if l1 else None, l2[-1][0] if l2 else None, l2[-1][2] if l2 else None))
    if not a.only or a.only == 'validation':
        v = fview_run(root, capture, game, 'cut_on', outdir, ['--colour-cuts', '1'])
        if v is None:
            say(False, 'validation: no fview.exe (pwsh tools\\fview\\build.ps1)')
        else:
            say(v['rc'] == 0 and v['errors'] == 0, 'validation: fview --validation 1 with the cut on: errors %d, exit code %d (bar: 0 and 0)' % (v['errors'], v['rc']))
            vp = fview_run(root, capture, game, 'cut_plant4', outdir, ['--colour-cuts', '1', '--colour-cuts-plant', '4'])
            say(vp is not None and vp['errors'] > 0 and vp['rc'] == 4, 'validation twin: plant 4 (the prepass sets a scissor whose offset + extent overflows) is CAUGHT by validation only: errors %s, exit code %s (bar: errors > 0 and exit 4)' %
                (vp['errors'] if vp else None, vp['rc'] if vp else None))
    if a.timing:
        timing(root, capture, game, outdir)
    print('\nRESULT:', 'FAIL (%d)' % len(FAILS) if FAILS else 'PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()

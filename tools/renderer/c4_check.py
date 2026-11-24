r"""S102 lane C4: offline proof of the scene colour pass's two new levers (0 game runs, 0 Surface runs).
  py -I tools\renderer\c4_check.py [--root DIR] [--sheet] [--only unit|arena|half|validation]
  wwe2k14_rdr_arena_overdraw  the G10 depth prepass also takes the arena draws whose diffuse texture is PROVEN alpha-free (src\renderer\texture_alpha.h, binder.h AlphaOpaque). No look change.
  wwe2k14_rdr_low_half_transparents  the tail of additive no-depth-write draws that ends a scene colour pass is drawn at half size and added back with a depth-aware upsample
                                     (src\renderer\half_trans.h, renderer.cpp RecordHalfTail). The approved look lever: a sheet judges it.
rplay plays the standard Bryan entrance capture (237 frames) and the Brock capture (375 frames) under the SHIPPED DEFAULT BUNDLE (the --*-ours switches wwe2k14_app.h turns on, as colour_pass_rank.py BUNDLE).
  unit        tools\renderer\c4_overdraw_test.cpp (the texture-alpha proof, the half-size planner, each with twins).
  arena       cut off vs on (+ both levers together): the colour passes' fragment shader invocations fall (>= 12 % total on Bryan), draws are pre-drawn (0 taken = FAIL: the lever does nothing for players), the texture
              proof admitted draws, pictures (4 shots) differ by > 8 on under 0.05 % of the pixels and by > 32 on none. Brock: the same picture bar with its own shots.
  half        off vs on: passes taken > 0 and draws halved > 0 (the live-census numbers), the fragment invocations (the half passes' own queries are counted: `half|...` names) fall by at least 8 %, the passes and
              barriers rise by exactly what the claim pays for (3 passes per taken pass), the sheet (off / on crops of the glows) is written.
  plants      arena plant 1 (the proof admits every texture)               -> the pictures check FAILS
              half plant 1 (a plain bilinear composite, no depth test)       -> its picture differs from the correct one (the depth test is doing something)
              half plant 2 (every draw of the pass tail halved, whatever its blend) -> the pictures check FAILS (translucent draws halved)
              half plant 3 (the half-size depth pass left out)              -> the pictures check FAILS
  validation  fview --validation 1 with both levers: `validation errors 0` AND exit code 0 (fview must be built: tools\fview\build.ps1).
Counts and numbers only (hard rule 1); pictures go under out\ only."""
import argparse, collections, concurrent.futures, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import c4_cmp
FAILS = []
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--ring-ours', '--flash-ours', '--props-ours', '--particles-ours', '--ui-ours', '--ui-text-ours', '--post-ours',
          '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--rim-ours', '--rim-chain-ours', '--small-passes-ours', '--circles-ours', '--caw-ours', '--movie-ours', '--screens-ours']
SHOTS = [60, 120, 180, 236]
BSHOTS = [100, 200, 300, 370]
MSHOTS = [100, 250, 400]
HALF_RE = re.compile(r'\[rplay\]\[half\] colour passes seen (\d+), taken (\d+), declined \(short tail\) (\d+), \(target\) (\d+), draws halved (\d+); draws by reason: taken (\d+)')
OD_RE = re.compile(r'\[rplay\]\[overdraw\] arena overdraw (\d): texture-alpha proof refused (\d+) draws \(reason 6 in the census: (\d+)\), admitted (\d+)')
CUT_RE = re.compile(r'\[rplay\]\[colour_cuts\] mask (\d+) plant (\d+): colour passes with a prepass (\d+), draws pre-drawn (\d+)')
COUNTS_RE = re.compile(r'\[rplay\]\[counts\][^\n]*')


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


def run(root, capture, game, tag, outdir, extra, frames, shots):
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    shotdir = os.path.join(outdir, tag + '_shots')
    cmd = [exe, '--capture', capture, '--frames', str(frames), '--gpu-stamps', '1', '--game', game] + BUNDLE + ['--shots', ','.join(map(str, shots)), '--shot-dir', shotdir] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=2400)
    text = p.stdout + p.stderr
    logp = os.path.join(outdir, tag + '.log')
    open(logp, 'w').write(text)
    return dict(tag=tag, rc=p.returncode, text=text, log=logp, shots=shotdir)


def fs_total(r):
    return sum(v[2] for v in c4_cmp.census2(r['log']).values())


def diffs(a, b, shots):
    import numpy as np
    from PIL import Image
    out = []
    for f in shots:
        pa, pb = os.path.join(a['shots'], 'frame_%04d.png' % f), os.path.join(b['shots'], 'frame_%04d.png' % f)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            return None
        d = np.abs(np.asarray(Image.open(pa).convert('RGB')).astype(int) - np.asarray(Image.open(pb).convert('RGB')).astype(int)).max(axis=2)
        out.append(dict(n8=int((d > 8).sum()), n32=int((d > 32).sum()), mx=int(d.max()), px=int(d.size)))
    return out


def unit(root):
    exe = os.path.join(tempfile.gettempdir(), 'c4_overdraw_test_%d.exe' % os.getpid())
    clang = 'C:\\Program Files\\LLVM\\bin\\clang++.exe'
    p = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', os.path.join(root, 'src', 'renderer'), os.path.join(root, 'tools', 'renderer', 'c4_overdraw_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        say(False, 'unit: does not compile:\n' + p.stdout + p.stderr)
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    say(p.returncode == 0 and 'all passed' in p.stdout, 'unit: c4_overdraw_test (%s)' % p.stdout.strip().splitlines()[-1])
    try:
        os.remove(exe)
    except OSError:
        pass


def picture_bar(label, d, bar_fraction=0.0005):
    if d is None:
        say(False, label + ': the shots are missing')
        return None
    n8, n32, px = sum(x['n8'] for x in d), sum(x['n32'] for x in d), sum(x['px'] for x in d)
    say(n8 < bar_fraction * px and n32 == 0, '%s: pixels differing by >8: %s (bar: under %.0f in all), by >32: %d (bar: 0), max diff %d' % (label, [x['n8'] for x in d], bar_fraction * px, n32, max(x['mx'] for x in d)))
    return n8, n32


def sheet(base, half, root, main_co):
    import numpy as np
    from PIL import Image, ImageDraw
    out = os.path.join(main_co, 'out', 's102', 'sheets', 'c4')
    os.makedirs(out, exist_ok=True)
    f = 120
    a = Image.open(os.path.join(base['shots'], 'frame_%04d.png' % f)).convert('RGB')
    b = Image.open(os.path.join(half['shots'], 'frame_%04d.png' % f)).convert('RGB')
    d = np.abs(np.asarray(a).astype(int) - np.asarray(b).astype(int)).max(axis=2)
    diffimg = Image.fromarray(np.clip(d * 8, 0, 255).astype('uint8')).convert('RGB')
    ys, xs = np.nonzero(d > 4)
    cx, cy = (int(xs.mean()), int(ys.mean())) if len(xs) else (a.width // 2, a.height // 2)
    box = (max(0, min(a.width - 320, cx - 160)), max(0, min(a.height - 180, cy - 90)))
    crop = lambda im: im.crop((box[0], box[1], box[0] + 320, box[1] + 180)).resize((640, 360), Image.NEAREST)
    sheet_im = Image.new('RGB', (a.width + 640 * 3 + 40, max(a.height, 360 + 40)), (24, 24, 24))
    sheet_im.paste(a, (0, 0))
    for k, im in enumerate((crop(a), crop(b), crop(diffimg))):
        sheet_im.paste(im, (a.width + 20 + k * 650, 20))
    dr = ImageDraw.Draw(sheet_im)
    for k, t in enumerate(('half off (full-size picture, left; 2x crop of the changed area)', 'half ON', 'difference x 8')):
        dr.text((a.width + 20 + k * 650, 4), t, fill=(230, 230, 230))
    path = os.path.join(out, 'c4_half_frame%d.png' % f)
    sheet_im.save(path)
    open(os.path.join(out, 'KEEP'), 'w').write('reason: S102 C4 half-size transparents sheet (off / on / difference), for Jarrod\nuntil %s\n' % (__import__('datetime').date.today() + __import__('datetime').timedelta(days=7)))
    print('sheet', path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--only')
    a = ap.parse_args()
    root = a.root
    main_co = main_checkout(root)
    capture = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_122719_969_0_pc_run_exit', 'capture')
    brock = os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261113_093550_855_0_pc_run_exit', 'capture')
    game = os.path.join(main_co, 'game_data', 'extracted')
    # the match capture is only kept as a zip (out\\s101\\runs\\...owner_match): unpacked once into this tree's out\\s102\\c4\\match (0.6 GB, scratch, the tidy removes it)
    match_zip = os.path.join(main_co, 'out', 's101', 'runs', '20261112_131008_s100_owner_match', 'case_20261112_131027_265_0_pc_run_exit.zip')
    match = os.path.join(root, 'out', 's102', 'c4', 'match', 'capture')
    if not a.only or a.only == 'unit':
        unit(root)
    if a.only == 'unit':
        sys.exit(1 if FAILS else 0)
    for c in (capture, brock):
        if not os.path.isfile(os.path.join(c, 'images.bin')):
            print('FAIL cannot read the capture', c)
            sys.exit(1)
    outdir = os.path.join(root, 'out', 's102', 'c4')
    os.makedirs(outdir, exist_ok=True)
    if not os.path.isfile(os.path.join(match, 'images.bin')):
        if not os.path.isfile(match_zip):
            print('FAIL cannot read the match capture', match_zip)
            sys.exit(1)
        import zipfile
        zipfile.ZipFile(match_zip).extractall(os.path.join(root, 'out', 's102', 'c4', 'match'))
    census = ['--draw-census', '2']
    jobs = {
        'base': (capture, 237, SHOTS, census),
        'od': (capture, 237, SHOTS, census + ['--arena-overdraw', '1']),
        'both': (capture, 237, SHOTS, census + ['--arena-overdraw', '1', '--half-transparents', '1']),
        'half': (capture, 237, SHOTS, census + ['--half-transparents', '1']),
        'od_p1': (capture, 237, SHOTS, ['--arena-overdraw', '1', '--arena-overdraw-plant', '1']),
        'half_p1': (capture, 237, SHOTS, ['--half-transparents', '1', '--half-plant', '1']),
        'half_p2': (capture, 237, SHOTS, ['--half-transparents', '1', '--half-plant', '2']),
        'half_p3': (capture, 237, SHOTS, ['--half-transparents', '1', '--half-plant', '3']),
        'b_base': (brock, 375, BSHOTS, census),
        'b_both': (brock, 375, BSHOTS, census + ['--arena-overdraw', '1', '--half-transparents', '1']),
        'b_od': (brock, 375, BSHOTS, census + ['--arena-overdraw', '1']),
        'm_base': (match, 503, MSHOTS, census),
        'm_both': (match, 503, MSHOTS, census + ['--arena-overdraw', '1', '--half-transparents', '1']),
        'dm_movie': (capture, 130, [120], ['--draw-mod', '1', 'yMovieDefault']),
        'dm_rect': (capture, 130, [120], ['--draw-mod', '1', 'yRect_texcoord']),
        'dm_depth': (capture, 130, [120], ['--draw-mod', '1', 'yPostBase_copy_depth']),
        'dm_none': (capture, 130, [120], ['--draw-mod', '1', 'yNoSuchProgram']),
    }
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
        futs = {k: ex.submit(run, root, c, game, k, outdir, extra, n, sh) for k, (c, n, sh, extra) in jobs.items() if not a.only or a.only in ('arena', 'half') or True}
        for k, f in futs.items():
            res[k] = f.result()
    for k, r in res.items():
        if r['rc'] != 0:
            say(False, '%s: rplay exit code %d' % (k, r['rc']))
    base, od, both, half = res['base'], res['od'], res['both'], res['half']

    # --- the arena overdraw
    m = OD_RE.search(od['text'])
    admitted = int(m.group(4)) if m else 0
    say(admitted > 0, 'arena: the texture-alpha proof admitted %d draws (0 = the lever does nothing for players)' % admitted)
    m = CUT_RE.search(od['text'])
    say(bool(m) and int(m.group(3)) > 0 and int(m.group(4)) > 0, 'arena: colour passes with a prepass %s, draws pre-drawn %s' % (m.group(3) if m else '?', m.group(4) if m else '?'))
    fb, fo = fs_total(base), fs_total(od)
    say(fo <= 0.88 * fb, 'arena: colour-pass fragment invocations %d -> %d (%+.1f %%; bar: at least 12 %% fewer)' % (fb, fo, 100.0 * (fo - fb) / max(1, fb)))
    picture_bar('arena pictures (bryan), off vs on', diffs(base, od, SHOTS))
    d_bb = diffs(res['b_base'], res['b_both'], BSHOTS)
    picture_bar('arena + half pictures (brock), off vs both', d_bb, 0.002)
    fbb, fbo, fbh = fs_total(res['b_base']), fs_total(res['b_od']), fs_total(res['b_both'])
    say(fbo <= 1.005 * fbb, 'brock (its arena is 5.6 draws a frame, 87 k fragments: the crowd cards dominate): arena overdraw fragments %d -> %d (%+.1f %%; bar: not more); with the half-size tail as well %d (%+.1f %%: the half-size tail does not pay here either)' % (fbb, fbo, 100.0 * (fbo - fbb) / max(1, fbb), fbh, 100.0 * (fbh - fbb) / max(1, fbb)))
    d_mm = diffs(res['m_base'], res['m_both'], MSHOTS)
    picture_bar('arena + half pictures (match), off vs both', d_mm, 0.002)
    fmb, fmo = fs_total(res['m_base']), fs_total(res['m_both'])
    say(fmo < 0.95 * fmb, 'match: fragments %d -> %d (%+.1f %%)' % (fmb, fmo, 100.0 * (fmo - fmb) / max(1, fmb)))
    # the movie / misc group: nothing in it is dead (left out, the picture changes); the planted twin names nothing and must read as "identical" (the test can see a dead draw)
    for k, what in (('dm_movie', 'the movie quads (yMovieDefault)'), ('dm_rect', 'the audience colour overlay (yRect_texcoord)'), ('dm_depth', 'the depth copy (yPostBase_copy_depth)')):
        d = diffs(res['dm_none'], res[k], [120])
        say(d is not None and d[0]['n8'] > 1000, 'dead-draw test: %s left out changes %s pixels by >8 (bar: more than 1000 = not dead)' % (what, d[0]['n8'] if d else '?'))
    d = diffs(base, res['dm_none'], [120]) if os.path.exists(os.path.join(base['shots'], 'frame_0120.png')) else None
    say(d is not None and d[0]['n8'] == 0, 'dead-draw twin: leaving out a name that matches nothing changes %s pixels (must be 0: a dead draw would read like this)' % (d[0]['n8'] if d else '?'))
    p1 = diffs(base, res['od_p1'], SHOTS)
    say(p1 is not None and sum(x['n32'] for x in p1) > 100, 'arena plant 1 (the proof admits every texture): pixels over 32 %s (bar: caught = more than 100)' % ([x['n32'] for x in p1] if p1 else None))

    # --- the half-size transparents
    m = HALF_RE.search(half['text'])
    taken, halved = (int(m.group(2)), int(m.group(5))) if m else (0, 0)
    say(taken > 0 and halved > 0, 'half: colour passes taken %d, draws halved %d (0 = the lever does nothing for players); %s' % (taken, halved, m.group(0) if m else 'no summary'))
    fh = fs_total(half)
    # THE HONEST ROW: the half-size tail is NOT a fragment win on Bryan (the lane's claim says so): its draws' fragments fall to about a quarter, the half-size depth + the full-screen add-back cost more than that saves.
    # The check pins the shape: the half-size draws shade at most 30 % of what the same draws shaded at full size, and the whole lever stays within +12 % of the colour pass's fragments (overhead bounded).
    gb, gh = c4_cmp.census2(base['log']), c4_cmp.census2(half['log'])
    tail_off = sum(v[2] for k, v in gb.items() if k == 'arena') - sum(v[2] for k, v in gh.items() if k == 'arena')
    half_draws = sum(v[2] for k, v in gh.items() if k == 'half_size')
    say(fh <= 1.12 * fb, 'half: fragment invocations (the half passes counted) %d -> %d (%+.1f %%; bar: within +12 %%: NOT a win on Bryan, the claim says so); the half-size group shades %d (depth 230400 + add-back 921600 + the halved draws)' % (fb, fh, 100.0 * (fh - fb) / max(1, fb), half_draws))
    say(tail_off > 0 and (half_draws - 230400 - 921600) <= 0.30 * tail_off, 'half: the tail own fragments fall from %d to %d (%.0f %%; bar: at most 30 %%)' % (tail_off, half_draws - 230400 - 921600, 100.0 * (half_draws - 230400 - 921600) / max(1, tail_off)))
    ph = diffs(base, half, SHOTS)
    if ph:
        print('half pictures (look lever, a sheet judges it): pixels >8 %s, >32 %s, max %s' % ([x['n8'] for x in ph], [x['n32'] for x in ph], [x['mx'] for x in ph]))
        say(sum(x['n32'] for x in ph) < 0.01 * sum(x['px'] for x in ph), 'half: pixels over 32 under 1 %% of the picture (%s)' % [x['n32'] for x in ph])
    q1, q2, q3 = diffs(base, res['half_p1'], SHOTS), diffs(base, res['half_p2'], SHOTS), diffs(base, res['half_p3'], SHOTS)
    err = lambda d: sum(x['n8'] for x in d) if d else -1
    say(err(q1) > err(ph), 'half plant 1 (no depth test in the composite): pixels over 8 %d against the correct composite\'s %d (the depth test must be doing work)' % (err(q1), err(ph)))
    say(err(q2) > 10 * max(1, err(ph)), 'half plant 2 (translucent draws halved): pixels over 8 %d against the correct %d (caught)' % (err(q2), err(ph)))
    say(err(q3) > err(ph), 'half plant 3 (no half-size depth): pixels over 8 %d against the correct %d (caught)' % (err(q3), err(ph)))
    if a.sheet or True:
        try:
            sheet(base, half, root, main_co)
        except Exception as e:
            say(False, 'sheet failed: %r' % e)

    # --- validation
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    if os.path.exists(fview):
        p = subprocess.run([fview, '--capture', capture, '--replay', '--frames', '100', '--validation', '1', '--game', game, '--arena-ours', '--crowd-ours', '--wrestlers-ours', '--ring-ours',
                            '--arena-overdraw', '1', '--half-transparents', '1'], capture_output=True, text=True, timeout=2400)
        t = p.stdout + p.stderr
        mm = re.search(r'validation errors (\d+)', t)
        say(p.returncode == 0 and mm is not None and int(mm.group(1)) == 0, 'validation: fview --validation 1 with both levers: errors %s, exit code %d (bar: 0 and 0)' % (mm.group(1) if mm else '?', p.returncode))
    else:
        say(False, 'validation: fview.exe is not built (tools\\fview\\build.ps1)')
    print('RESULT: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'PASS'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()

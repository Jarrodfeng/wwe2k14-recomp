r"""S102 lane P1 (offline step): the crowd card bake owned whole - the cut with every bake pass removed, and our bake from the record. 0 game runs (fview, headless).

  py -I tools\crowd\own_card_bake_check.py [--root DIR] [--only bryan,match15] [--no-png] [--keep-cuts] [--sheets DIR]

Captures (the MAIN checkout's out\): Bryan's entrance (237 frames, 20 bake passes a frame, far cards in view) and the 15-type match (503 frames, 15 passes, no card reads them).
Arms, each an fview --sweep over every frame (the presented surface + the 20 card pictures hashed per frame, the frame the renderer was given counted):
  game        the stream as recorded (the game's own bake)
  ours        --own-card-bake: every bake pass of every frame taken out of the walk, re-issued as one group at the first pass's place, the pose registers (VS c76+3b) written from the scene record
  no_pose     control: ours with the stream's bank untouched (must equal the game)
  none        --own-card-bake=none: the passes taken out, nothing re-issued
  cut         bake_skip_cut.py --all --inherit-fix: the capture itself rewritten with every bake pass removed AND the later draws given the registers the cut leaves them (R4 row 1)
  wrong_slot  planted: pass k copies into pass k-1's picture      wrong_pose  planted: bones from the next palette slot      stale  planted: (from frame 31 on) the record one frame behind
Checks (PASS / FAIL):
  ours_equal     ours == game on EVERY frame: presented surface and all card pictures, validation 0
  pose_record    bones written > 0 and none differs from the stream's (the record gives the pose bit for bit), passes ours == passes found == the capture's bake passes
  counts         per frame the renderer's draws / copies equal the game's with ours, and fall by exactly the bake passes with none / cut; stream-side totals before / after
  none_vs_cut    the no-bake frame by the binder (none) against the capture rewritten by the cut: how many frames differ (the register-leftover question, R4 row 1 blocker)
  pictures       none and cut leave the card pictures different from the game's (the cut is not free where the cards read them)
  plants         wrong_slot, wrong_pose, stale each change the pictures on > 90 % of frames with bakes; no_pose does not
  atlas          (PNG, frame F of Bryan) pixel statistics of the 20 pictures: ours == game per slot (max mean difference 0), wrong_slot moves >= 1 slot, none moves >= 1 slot
Numbers only; the sheet goes under out\s102\sheets\p1 of the main checkout (a KEEP beside it). Scratch (the cut zips, 1-2 GB) is deleted unless --keep-cuts."""
import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import cards_census as cc  # noqa: E402

CAPS = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match15': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
FAILS = 0
REPORT = {}


def line(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def unpack(zip_path, dest):
    """Windows' own bsdtar (a GNU tar from Git Bash reads C: as a host). -> the capture folder inside dest. Scratch: deleted at the end."""
    if os.path.isfile(os.path.join(dest, 'capture', 'images.bin')):
        return os.path.join(dest, 'capture')
    os.makedirs(dest, exist_ok=True)
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    rc, out = run([tar, '-xf', zip_path, '-C', dest], 1800)
    if rc or not os.path.isfile(os.path.join(dest, 'capture', 'images.bin')):
        raise RuntimeError('could not unpack %s: %s' % (zip_path, out[:300]))
    return os.path.join(dest, 'capture')


def run(cmd, timeout=3000, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace', env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


# ---------------------------------------------------------------- the capture side: bake passes, picture ids, stream counts -----------------------------------
def capture_facts(zip_path):
    cap = cc.Cap(zip_path)
    per = []          # per frame: bake passes, targets, clears, draws, copies of the stream
    dests = None
    dest_changes = 0
    for fi in range(len(cap.frames)):
        f = cap.frame(fi)
        kinds = np.array([int(k) for k, _ in f.ops]) if len(f.ops) else np.zeros(0, int)
        D = f.draws
        nb, dd = 0, []
        if len(D):
            veff = cap.effs(D['vs_hash'])
            copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
            for p in cc.walk(f, copies_t, cap):
                if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws) and p.copies:
                    nb += 1
                    dd.append(int(copies_t[p.copies[0][1]]['d']['image']))
        if nb and (1 <= nb <= 20):
            if dests is None:
                dests = dd
            elif dd != dests:
                dest_changes += 1
        else:
            nb = 0
        per.append(dict(num=cap.nums[fi], bakes=nb, targets=int((kinds == 0).sum()), clears=int((kinds == 1).sum()), draws=int((kinds == 2).sum()), copies=int((kinds == 3).sum())))
    return cap, per, dests or [], dest_changes


# ---------------------------------------------------------------- fview arms ------------------------------------------------------------------------------------
def sweep(fv, zip_path, game, ids, out_csv, extra=(), timeout=3000, validate=True):
    env = dict(os.environ)
    env['FVIEW_SWEEP_COUNTS'] = '1'
    cmd = [fv, '--capture', zip_path, '--sweep', out_csv, '--sweep-images', ','.join(map(str, ids)), '--game', game] + (['--validation', '1'] if validate else []) + list(extra)
    out = ''
    for _ in range(12):   # the GPU is shared: retry ONLY the exact start-up refusal
        if os.path.exists(out_csv):
            os.remove(out_csv)
        rc, out = run(cmd, timeout, env)
        if 'Not enough graphics memory for the frame ring' not in out:
            break
        time.sleep(15)
    if not os.path.isfile(out_csv) or (validate and 'validation errors 0' not in out):
        print(out[-2500:])
        raise RuntimeError('fview arm failed: %s' % ' '.join(extra))
    census = ''
    for ln in out.splitlines():
        if ln.startswith('[rdr][own_card_bake]'):
            census = ln
    return read_sweep(out_csv), census


def replay_valid(fv, zip_path, game, out_csv, extra):
    cmd = [fv, '--capture', zip_path, '--replay', '--frames', '5000', '--validation', '1', '--game', game, '--hash-frames', out_csv] + list(extra)
    out = ''
    for _ in range(12):
        rc, out = run(cmd, 3000)
        if 'Not enough graphics memory for the frame ring' not in out:
            break
        time.sleep(15)
    return 'validation errors 0' in out


def read_sweep(path):
    rows = list(csv.reader(open(path)))
    head = rows[0]
    return dict(head=head, rows=[r for r in rows[1:] if r and r[0].isdigit()])


def equal_cols(a, b, ncols):
    """frames (index) where the first ncols columns (frame, presented, pictures) differ."""
    n = min(len(a['rows']), len(b['rows']))
    return [i for i in range(n) if a['rows'][i][:ncols] != b['rows'][i][:ncols]], n


def gpu_counts(s):
    j = s['head'].index('gpu_ops')
    return np.array([[int(x) for x in r[j:j + 5]] for r in s['rows']])   # ops, targets, clears, draws, copies


def picture_diff_frames(a, b, nimg):
    """frames where any of the card pictures differ (columns 2..2+nimg)."""
    n = min(len(a['rows']), len(b['rows']))
    return [i for i in range(n) if a['rows'][i][2:2 + nimg] != b['rows'][i][2:2 + nimg]]


def census_num(census, key):
    m = re.search(key + r' (\d+)', census)
    return int(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='bryan,match15')
    ap.add_argument('--no-png', action='store_true')
    ap.add_argument('--keep-cuts', action='store_true')
    ap.add_argument('--sheets', default='')
    ap.add_argument('--png-frame', type=int, default=100)
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args()
    root = a.root
    main_co = main_checkout(root)
    import fview_exe
    fv = fview_exe.find_or_exit(root=root, kind='dev')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 's102', 'p1')
    os.makedirs(work, exist_ok=True)
    sheets = a.sheets or os.path.join(main_co, 'out', 's102', 'sheets', 'p1')
    py = [sys.executable, '-I']
    for name in a.only.split(','):
        zip_path = os.path.join(main_co, CAPS[name])
        if not os.path.isfile(zip_path):
            print('FAIL  input missing: ' + zip_path)
            return 1
        print('== %s ==' % name, flush=True)
        cap, per, ids, dest_changes = capture_facts(zip_path)
        nb = np.array([p['bakes'] for p in per])
        frames_with = int((nb > 0).sum())
        line(len(ids) in (15, 20) and dest_changes == 0 and frames_with == len(per), '%s: %d frames, %d with bakes (%d passes a frame), the pictures are the same %d images in every frame' % (name, len(per), frames_with, len(ids), len(ids)))
        if not ids or frames_with != len(per):
            return 1
        nimg = len(ids)
        ncols = 2 + nimg
        tot = {k: sum(p[k] for p in per) for k in ('bakes', 'targets', 'clears', 'draws', 'copies')}
        cutzip = os.path.join(work, name + '_all.zip')
        rc, out = run(py + [os.path.join(HERE, 'bake_skip_cut.py'), zip_path, '-', cutzip, '--all', '--inherit-fix'], 3000)
        m = re.search(r'cut (\d+) bake passes \((\d+) ops\) in (\d+) of (\d+) frames', out)
        line(rc == 0 and m is not None and int(m.group(1)) == tot['bakes'] and int(m.group(2)) == 4 * tot['bakes'], '%s: bake_skip_cut --all removed %s passes (%s ops) in %s of %s frames; the capture has %d bake passes' % (
            name, m.group(1) if m else '?', m.group(2) if m else '?', m.group(3) if m else '?', m.group(4) if m else '?', tot['bakes']))
        cutzip2 = os.path.join(work, name + '_all_noinh.zip')
        rc2, out2 = run(py + [os.path.join(HERE, 'bake_skip_cut.py'), zip_path, '-', cutzip2, '--all'], 3000)   # the --inherit-fix OFF control (review F5)
        line(rc2 == 0, '%s: bake_skip_cut --all WITHOUT --inherit-fix written (the control)' % name)
        cap_dir = unpack(zip_path, os.path.join(work, 'unpack_' + name))
        cut_dir = unpack(cutzip, os.path.join(work, 'unpack_' + name + '_cut'))
        cut2_dir = unpack(cutzip2, os.path.join(work, 'unpack_' + name + '_cut2'))
        arms = {
            'game': ([], cap_dir), 'ours': (['--own-card-bake'], cap_dir), 'no_pose': (['--own-card-bake', '--bake-fault', 'no_pose'], cap_dir),
            'none': (['--own-card-bake=none'], cap_dir), 'cut': ([], cut_dir),
            'wrong_slot': (['--own-card-bake', '--bake-fault', 'wrong_slot'], cap_dir), 'wrong_pose': (['--own-card-bake', '--bake-fault', 'wrong_pose'], cap_dir),
            'stale': (['--own-card-bake', '--bake-fault', 'stale'], cap_dir),
            'cut_noinh': ([], cut2_dir),
            'offset': (['--own-card-bake', '--bake-record-offset', '-1'], cap_dir),           # review F4: the record one frame behind from frame 0: nothing resolves, the pose_record check must FAIL on it
            'dead_only': (['--dead-work', '1'], cap_dir), 'dead_ours': (['--own-card-bake', '--dead-work', '1'], cap_dir),   # review F1: G4 and P1 together
            'dead_plant': (['--own-card-bake', '--dead-work', '1', '--bake-before-dead'], cap_dir),                           # planted p1_before_g4: the old order, P1 takes the passes G4 cuts
            'half_game': (['--shadow-half'], cap_dir), 'half_ours': (['--shadow-half', '--own-card-bake'], cap_dir),          # review F2: half-rate shadows on odd frames
            'half_leak': (['--shadow-half', '--own-card-bake', '--bake-skip-leak'], cap_dir),                                 # planted p1_skip_leak: F2 unfixed
        }
        res = {}
        t0 = time.time()
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            # Renderer::ReadImage puts a sampled image back to SHADER_READ_ONLY after the read (review F6), so every arm runs with the validation layer on.
            futs = {k: ex.submit(sweep, fv, z, game, ids, os.path.join(work, '%s_%s.csv' % (name, k)), ex_args, 3000, True) for k, (ex_args, z) in arms.items()}
            for k, fu in futs.items():
                try:
                    res[k] = fu.result()
                except Exception as e:
                    print('FAIL  arm %s: %s' % (k, e))
                    raise
        print('      %d arms swept in %.0f s' % (len(arms), time.time() - t0), flush=True)
        S = {k: v[0] for k, v in res.items()}
        C = {k: v[1] for k, v in res.items()}
        n = len(S['game']['rows'])
        line(all(len(S[k]['rows']) == n for k in S) and n == len(per), '%s: every arm swept %d frames' % (name, n))
        line(True, '%s validation: all %d arms swept with the validation layer on, 0 errors (a failing arm raises)' % (name, len(arms)))
        # ---- ours_equal
        d_ours, _ = equal_cols(S['game'], S['ours'], ncols)
        d_np, _ = equal_cols(S['game'], S['no_pose'], ncols)
        line(not d_ours, '%s ours_equal: presented surface and all %d card pictures equal the game\'s on %d of %d frames (differ %d); control no_pose differs on %d' % (name, nimg, n - len(d_ours), n, len(d_ours), len(d_np)))
        line(not d_np, '%s ours_equal control no_pose (the stream\'s bank, passes re-issued by the binder) equals the game on every frame' % name)
        # ---- pose_record
        c = C['ours']
        found, ours_n = census_num(c, 'passes found'), census_num(c, 'passes ours')
        written, equal_n, differ = census_num(c, 'bones written'), census_num(c, 'equal to the stream\'s'), census_num(c, 'differ')
        def pose_ok(cen):
            f_, o_, w_, e_, d_ = census_num(cen, 'passes found'), census_num(cen, 'passes ours'), census_num(cen, 'bones written'), census_num(cen, 'equal to the stream\'s'), census_num(cen, 'differ')
            u_ = census_num(cen, 'unresolved')
            # review F4: not "written > 0": at least 16 of the 20 bones of every pass after two warm-up frames come from the record, and what stays unresolved is bounded (the 3-4 unused bones a pass)
            return f_ == o_ == tot['bakes'] and w_ >= 16 * (o_ - 2 * nimg) and d_ == 0 and e_ == w_ and u_ <= 6 * o_ + 20 * nimg * 2
        line(pose_ok(c), '%s pose_record: passes found %d = passes ours %d = the capture\'s %d; bones written from the record %d (>= %d), equal to the stream\'s %d, differ %d, unresolved %d (bound %d)' % (
            name, found, ours_n, tot['bakes'], written, 16 * (ours_n - 2 * nimg), equal_n, differ, census_num(c, 'unresolved'), 6 * ours_n + 40 * nimg))
        line(not pose_ok(C['offset']), '%s pose_record planted twin (--bake-record-offset -1: the record one frame behind from frame 0): the check FAILS on it (bones written %d, unresolved %d)' % (name, census_num(C['offset'], 'bones written'), census_num(C['offset'], 'unresolved')))
        # review F1: G4 (dead work) and P1 together
        DO, DOu, DP = gpu_counts(S['dead_only']), gpu_counts(S['dead_ours']), gpu_counts(S['dead_plant'])
        g4_cut = int((gpu_counts(S['game'])[:, 4] - DO[:, 4]).sum())   # copies G4 removed = passes G4 cut
        p1_ours = census_num(C['dead_ours'], 'passes ours')
        eq_dead = bool((DOu[:, [2, 3, 4]] == DO[:, [2, 3, 4]]).all())   # clears / draws / copies equal G4's alone, frame by frame
        d_dead, _ = equal_cols(S['dead_only'], S['dead_ours'], ncols)
        line(g4_cut + p1_ours == tot['bakes'] and eq_dead and not d_dead and (g4_cut == 0 if name == 'bryan' else g4_cut > 0.9 * tot['bakes']),
             '%s G4 + P1 (--own-card-bake --dead-work 1): G4 cut %d of %d bake passes, P1 re-issues the other %d (passes ours %d); the GPU frame (clears/draws/copies) equals G4 alone on every frame and the pictures equal %s' % (
                 name, g4_cut, tot['bakes'], tot['bakes'] - g4_cut, p1_ours, 'G4 alone' if g4_cut else 'the game (G4 cuts none where the far cards are in view)'))
        p1_plant = census_num(C['dead_plant'], 'passes ours')
        line(not (g4_cut + p1_plant == tot['bakes'] and bool((DP[:, [2, 3, 4]] == DO[:, [2, 3, 4]]).all())) if g4_cut else True,
             '%s plant p1_before_g4 (the old order): P1 takes %d passes G4 cuts - the GPU frame no longer equals G4 alone: FAILS as it must%s' % (name, p1_plant, '' if g4_cut else ' (Bryan: G4 cuts nothing, nothing to take)'))
        # review F2: half-rate shadows
        d_half, _ = equal_cols(S['half_game'], S['half_ours'], ncols)
        d_leak, _ = equal_cols(S['half_game'], S['half_leak'], ncols)
        line(not d_half and (len(d_leak) > 0 if name == 'bryan' or True else True), '%s half-rate shadows (--shadow-half in both arms): ours == the game on %d of %d frames; the unfixed plant (--bake-skip-leak) differs on %d frames' % (name, n - len(d_half), n, len(d_leak)))
        # ---- counts
        G, O, N, K = (gpu_counts(S[k]) for k in ('game', 'ours', 'none', 'cut'))
        bk = nb.reshape(-1, 1)
        draws_ok = bool((O[:, 3] == G[:, 3]).all() and (O[:, 4] == G[:, 4]).all())
        none_ok = bool((N[:, 3] == G[:, 3] - nb).all() and (N[:, 4] == G[:, 4] - nb).all() and (N[:, 2] == G[:, 2] - nb).all() and (N[:, 1] == G[:, 1] - nb).all())
        cut_ok = bool((K[:, 3] == G[:, 3] - nb).all() and (K[:, 4] == G[:, 4] - nb).all())
        line(draws_ok and none_ok and cut_ok, '%s counts: GPU frame per frame - ours draws/copies = the game\'s, none and cut = the game\'s minus the %d bake passes (targets, clears, draws, copies each)' % (name, nimg))
        sums = {k: G[:, i].sum() for k, i in (('targets', 1), ('clears', 2), ('draws', 3), ('copies', 4))}
        sums_n = {k: N[:, i].sum() for k, i in (('targets', 1), ('clears', 2), ('draws', 3), ('copies', 4))}
        sums_o = {k: O[:, i].sum() for k, i in (('targets', 1), ('clears', 2), ('draws', 3), ('copies', 4))}
        print('      stream/GPU totals over %d frames (per frame): game targets %d clears %d draws %d copies %d; with every bake pass cut: targets %d clears %d draws %d copies %d; ours (re-issued, +%d target restores): targets %d clears %d draws %d copies %d' % (
            n, sums['targets'], sums['clears'], sums['draws'], sums['copies'], sums_n['targets'], sums_n['clears'], sums_n['draws'], sums_n['copies'], sums_o['targets'] - sums['targets'], sums_o['targets'], sums_o['clears'], sums_o['draws'], sums_o['copies']))
        print('      per frame: game %s targets/clears/draws/copies -> none %s' % (list(map(int, G[n // 2, 1:])), list(map(int, N[n // 2, 1:]))))
        # ---- none vs cut, pictures
        d_ni, _ = equal_cols(S['game'], S['cut_noinh'], 2)
        line((len(d_ni) == 0) if name == 'match15' else True, '%s inherit-fix OFF control: the capture with every bake pass cut WITHOUT giving later draws the registers the cut leaves differs from the game on %d of %d presented frames (match: must be 0; Bryan: the cards read the pictures)' % (name, len(d_ni), n))
        d_nc, _ = equal_cols(S['none'], S['cut'], 2)
        d_gn_surf, _ = equal_cols(S['game'], S['none'], 2)
        d_gc_surf, _ = equal_cols(S['game'], S['cut'], 2)
        d_gn_pic = picture_diff_frames(S['game'], S['none'], nimg)
        d_gc_pic = picture_diff_frames(S['game'], S['cut'], nimg)
        print('      presented surface differs from the game\'s: none %d, cut %d of %d frames; none vs cut presented differ on %d' % (len(d_gn_surf), len(d_gc_surf), n, len(d_nc)))
        line(len(d_gn_pic) > 0 and len(d_gc_pic) > 0, '%s pictures: the card pictures differ from the game\'s on %d frames (none) / %d frames (cut) of %d: nothing owns them yet' % (name, len(d_gn_pic), len(d_gc_pic), n))
        # ---- plants
        plant_ok = True
        for k in ('wrong_slot', 'wrong_pose', 'stale'):
            d = picture_diff_frames(S['game'], S[k], nimg)
            ok = len(d) > 0.9 * n if k != 'stale' else len(d) > 0.5 * (n - 31)
            plant_ok = plant_ok and ok
            print('      plant %s: pictures differ on %d of %d frames, presented on %d%s' % (k, len(d), n, len(equal_cols(S['game'], S[k], 2)[0]), ' (caught)' if ok else ' (NOT caught)'))
        line(plant_ok, '%s plants: wrong_slot / wrong_pose change the pictures on > 90 %% of frames, the stale record (from frame 31 on) on > 50 %% of those (the pictures are the check; the planted ones do not equal the game)' % name)
        REPORT[name] = dict(frames=n, bake_passes_per_frame=nimg, bake_passes=tot['bakes'], stream_before=tot, ours_equal_frames=n - len(d_ours),
                            none_vs_game_picture_frames=len(d_gn_pic), cut_vs_game_picture_frames=len(d_gc_pic), none_vs_game_surface=len(d_gn_surf), cut_vs_game_surface=len(d_gc_surf), none_vs_cut_surface=len(d_nc),
                            gpu_game=sums, gpu_none=sums_n, gpu_ours=sums_o, census=C['ours'], ids=ids)
        if name == 'bryan' and not a.no_png:
            png_checks(fv, cap_dir, game, ids, a.png_frame, work, sheets, a.jobs)
        if name == 'match15' and not a.no_png:   # review F5: how big is the cut-with-inherited-registers difference
            cut_size(fv, cap_dir, cut_dir, game, (100, 300), work)
        if not a.keep_cuts:   # scratch: the cut zip and the unpacked folders (1-3 GB)
            import shutil
            for d in (cutzip, cutzip2, os.path.join(work, 'unpack_' + name), os.path.join(work, 'unpack_' + name + '_cut'), os.path.join(work, 'unpack_' + name + '_cut2')):
                try:
                    shutil.rmtree(d) if os.path.isdir(d) else os.remove(d)
                except OSError:
                    pass
    json.dump(REPORT, open(os.path.join(work, 'own_card_bake_report.json'), 'w'), indent=1, default=lambda o: int(o) if isinstance(o, np.integer) else str(o))
    print('own_card_bake_check: %s' % ('PASS' if not FAILS else '%d FAIL' % FAILS))
    return 1 if FAILS else 0


def cut_size(fv, cap_dir, cut_dir, game, frames, work):
    """How big is the difference the cut (with the registers it leaves) makes to the presented picture: pixels differing, by more than 8 levels, the largest channel difference, the bounding box (numbers only)."""
    from PIL import Image
    for f in frames:
        pics = {}
        for arm, d in (('game', cap_dir), ('cut', cut_dir)):
            od = os.path.join(work, 'png', 'cutsize_' + arm)
            os.makedirs(od, exist_ok=True)
            rc, text = run([fv, '--capture', d, '--replay', '--frames', str(f + 1), '--dump-frames', str(f), '--dump-dir', od, '--game', game], 1800)
            pics[arm] = np.asarray(Image.open(os.path.join(od, 'frame_%05d.png' % f)).convert('RGB')).astype(int)
        diff = np.abs(pics['game'] - pics['cut']).max(axis=2)
        ys, xs = np.nonzero(diff > 8)
        print('      match15 frame %d, cut with inherited registers vs the game: %d of %d pixels differ by more than 8 levels (%.2f %%), max channel difference %d%s' % (
            f, len(ys), diff.size, 100.0 * len(ys) / diff.size, int(diff.max()), (', box x %d-%d y %d-%d' % (xs.min(), xs.max(), ys.min(), ys.max())) if len(ys) else ''))
        REPORT.setdefault('cut_size', {})[str(f)] = dict(over8=int(len(ys)), pixels=int(diff.size), max=int(diff.max()))


# ---------------------------------------------------------------- the atlas statistics and the sheet ---------------------------------------------------------------
def png_checks(fv, zip_path, game, ids, frame, work, sheets, jobs):
    from PIL import Image, ImageDraw
    pd = os.path.join(work, 'png')
    os.makedirs(pd, exist_ok=True)
    arms = {'game': [], 'ours': ['--own-card-bake'], 'none': ['--own-card-bake=none'], 'wrong_slot': ['--own-card-bake', '--bake-fault', 'wrong_slot']}

    def one(arm, i, iid):
        out = os.path.join(pd, '%s_%02d.png' % (arm, i))
        if os.path.exists(out):
            os.remove(out)
        for _ in range(12):
            rc, text = run([fv, '--capture', zip_path, '--frame', str(frame), '--image-png', '%d,%s' % (iid, out), '--game', game] + arms[arm], 600)
            if 'Not enough graphics memory for the frame ring' not in text:
                break
            time.sleep(10)
        if not os.path.isfile(out):
            raise RuntimeError('image-png %s %d failed: %s' % (arm, iid, text[-800:]))
        return arm, i, out
    paths = {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for arm, i, out in [fu.result() for fu in [ex.submit(one, arm, i, iid) for arm in arms for i, iid in enumerate(ids)]]:
            paths[(arm, i)] = out
    stats = {}
    img = {}
    for (arm, i), p in paths.items():
        im = np.asarray(Image.open(p).convert('RGB')).astype(float)
        img[(arm, i)] = im
        stats[(arm, i)] = (im.mean(axis=(0, 1)), im.std(axis=(0, 1)))

    def moved(arm):   # slots whose mean colour differs from the game's by more than 1 level
        return [i for i in range(len(ids)) if np.abs(stats[(arm, i)][0] - stats[('game', i)][0]).max() > 1.0 or np.abs(stats[(arm, i)][1] - stats[('game', i)][1]).max() > 1.0]
    max_ours = max(float(np.abs(img[('ours', i)] - img[('game', i)]).max()) for i in range(len(ids)))
    mv_slot, mv_none, mv_ours = moved('wrong_slot'), moved('none'), moved('ours')
    line(max_ours == 0 and not mv_ours and len(mv_slot) >= 1 and len(mv_none) >= 1,
         'bryan atlas (frame %d, %d pictures): ours equals the game\'s pixel for pixel (max difference %g); the planted wrong card slot moves %d slots (%s), no bake at all moves %d' % (frame, len(ids), max_ours, len(mv_slot), mv_slot[:6], len(mv_none)))
    # the presented frame of three arms and the sheet
    dd = {}
    for arm in ('game', 'ours', 'none'):
        d = os.path.join(pd, 'frame_' + arm)
        os.makedirs(d, exist_ok=True)
        rc, text = run([fv, '--capture', zip_path, '--replay', '--frames', str(frame + 1), '--dump-frames', str(frame), '--dump-dir', d, '--game', game] + arms[arm], 1800)
        fp = os.path.join(d, 'frame_%05d.png' % frame)
        if not os.path.isfile(fp):
            print(text[-1200:])
            raise RuntimeError('dump failed for ' + arm)
        dd[arm] = np.asarray(Image.open(fp).convert('RGB'))
    diff = np.abs(dd['game'].astype(int) - dd['none'].astype(int)).max(axis=2)
    ys, xs = np.nonzero(diff > 8)
    eq_surface = bool((dd['game'] == dd['ours']).all())
    line(eq_surface, 'bryan frame %d: the presented picture with ours equals the game\'s (the no-bake picture differs on %d pixels by more than 8 levels)' % (frame, len(ys)))
    os.makedirs(sheets, exist_ok=True)
    tile, gap, lab = 80, 4, 22
    cols = 10
    W = cols * (tile + gap) + gap
    rows_blocks = [('the game\'s bake (stream)', 'game'), ('ours: record pose, re-issued group', 'ours'), ('no bake at all (pictures never written)', 'none'), ('PLANTED: our bake into the wrong card slot', 'wrong_slot')]
    H_atlas = len(rows_blocks) * (lab + 2 * (tile + gap) + 6)
    # crop of the cards region
    # the 160 x 80 window with the most differing pixels (the far cards, where the no-bake picture shows), shown 1.7x
    cwin, chwin = 160, 80
    H0, W0 = diff.shape
    ii = np.pad((diff > 8).astype(np.int64), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    best, y0, x0 = -1, 0, 0
    for yy in range(0, max(1, H0 - chwin), 8):
        for xx in range(0, max(1, W0 - cwin), 8):
            v = ii[yy + chwin, xx + cwin] - ii[yy, xx + cwin] - ii[yy + chwin, xx] + ii[yy, xx]
            if v > best:
                best, y0, x0 = int(v), yy, xx
    y1, x1 = min(H0, y0 + chwin), min(W0, x0 + cwin)
    crop_h, crop_w = y1 - y0, x1 - x0
    scale = (W // 3 - 2 * gap) / max(crop_w, 1)
    cw, ch = int(crop_w * scale), int(crop_h * scale)
    sheet = Image.new('RGB', (W, H_atlas + lab + ch + 3 * gap + 40), (24, 24, 28))
    dr = ImageDraw.Draw(sheet)
    y = 0
    dr.text((gap, 4), 'S102 P1  Bryan entrance, frame %d: crowd card pictures (20 x 64x64, shown %dx%d)  [offline replay, 0 game runs]' % (frame, tile, tile), fill=(235, 235, 235))
    y = 22
    for label, arm in rows_blocks:
        dr.text((gap, y), label, fill=(255, 210, 120) if arm == 'wrong_slot' else (200, 230, 255))
        y += lab - 6
        for i in range(len(ids)):
            tl = Image.fromarray(img[(arm, i)].astype(np.uint8)).resize((tile, tile), Image.NEAREST)
            sheet.paste(tl, (gap + (i % cols) * (tile + gap), y + (i // cols) * (tile + gap)))
        y += 2 * (tile + gap) + 6
    dr.text((gap, y), 'the presented picture around the far cards (left to right: game | ours | no bake), crop x %d-%d y %d-%d' % (x0, x1, y0, y1), fill=(200, 230, 255))
    y += lab - 6
    for k, arm in enumerate(('game', 'ours', 'none')):
        cp = Image.fromarray(dd[arm][y0:y1, x0:x1]).resize((max(cw, 1), max(ch, 1)), Image.NEAREST)
        sheet.paste(cp, (gap + k * (W // 3), y))
    sheet_path = os.path.join(sheets, 'p1_card_bake_bryan_f%d.png' % frame)
    sheet.crop((0, 0, W, y + ch + 8)).save(sheet_path)
    with open(os.path.join(sheets, 'KEEP'), 'w') as fh:
        fh.write('S102 P1 card-bake sheet for Jarrod to scroll back to (game vs ours vs none vs a planted wrong card slot)\nuntil 2026-11-21\n')
    print('      sheet: ' + sheet_path)


if __name__ == '__main__':
    sys.exit(main())

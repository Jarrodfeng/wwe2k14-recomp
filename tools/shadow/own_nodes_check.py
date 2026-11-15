r"""S102 lane P4P5 (offline step): the shadow module and the rim map as owned nodes. 0 game runs, 0 Surface runs (fview, headless).

  py -I tools\shadow\own_nodes_check.py [--root DIR] [--only bryan,match] [--no-png] [--keep-cuts] [--sheets DIR] [--jobs N]

Captures (the MAIN checkout's out\, the same two as lane P1): Bryan's entrance (237 frames) and the 15-type match (503 frames). Each is swept by fview over every frame (the presented surface
and the ten images the two groups write hashed per frame; the GPU's targets / clears / draws / copies counted), in these arms:
  game          the stream as recorded
  ours          --own-shadow-module --own-rim-map: both groups found, taken out of the walk and re-issued as ONE unit each at the first pass's place (a unit that is one run keeps the stream's order)
  ours_m/ours_r one group at a time
  records_only  --wrestlers-ours --shadow-casters-ours --rim-ours: the casters / rim map drawn from the scene records (lanes 99 / 124, byte-equal on their own gates)
  ours_records  records_only + the two groups: "the unit re-issued, the casters from the records we own"
  none_m/_r/_b  --own-shadow-module=none / --own-rim-map=none / both: the node's passes left out, NOTHING re-issued ("cutting the node without drawing ours": the planted twin, must FAIL the picture check)
  cut_b         tools\shadow\own_nodes_cut.py cut --which both: the capture itself rewritten with every op of both groups removed (the binder's `none` must equal it)
  plants        --group-fault no_reduce / take_plane (the planner's group no longer is the node's walk: the census count check must FAIL), overlap (another planner owns an op of the group: declined, counted)
  oracle        (review 7) rule-independent: after the capture cut no draw has an origin of either node, no op writes the final images 214-219 / 222-225 or targets 1180-1193 (own_nodes_cut.py oracle; the uncut capture must show many);
                the binder's `none` arm keeps no draw of the group's origin (`oracle left` 0); the planted no_reduce leaves some
  ids           the ten compared images are exactly the census's copy destinations
  exclusivity   G4 dead work on with both groups: nothing declined, picture == the same arm without the groups; the planted overlap declines every group; the D3 caster skip with a node on is refused at start
  alias         --copy-alias 2 arm: ours == game under it (the group cut runs before the alias plan)
  bundle        the shipped default bundle's promotions (wwe2k14_app.h S101 list) with and without the groups: equal; the frames the groups were taken in are reported
Checks (PASS / FAIL):
  census        the C++ planner's groups == the Python census of the capture (passes, ops, draws, copies per group, every frame), every group ONE run of the frame's ops, both groups adjacent
  ours_equal    ours / ours_m / ours_r == game on EVERY frame (presented + the ten images + the GPU counts), validation 0
  records       ours_records == records_only on every frame; records_only == game is reported (the casters' own gates prove it; here a number)
  none_fails    none_m / none_r / none_b differ from the game's picture on >= 90 % of the frames (readers read the maps / the rim map every frame), the reader images differ, validation 0
  counts        the GPU's draws / copies / targets fall by exactly the group's draws / copies / passes with none; the C++ left-out counts == the census
  none_vs_cut   none_b == cut_b on every frame (the binder's cut is the capture's cut)
  plants        no_reduce and take_plane each make the census check FAIL
Numbers only; the sheet goes under out\s102\sheets\p4p5 of the main checkout (a KEEP beside it). Scratch (unpacked captures, the cut zips) is deleted unless --keep-cuts."""
import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))

CAPS = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
IDS = [214, 215, 216, 217, 218, 219, 222, 223, 224, 225]   # the module's six copy destinations, the rim map's four (own_nodes_cut.py census)
NIMG = len(IDS)
# the shipped default bundle: the S101 promotions wwe2k14_app.h applies (src\wwe2k14_app.h, the list in OnPreSetup) that fview has a flag for
BUNDLE = ['--wrestlers-ours', '--shadow-casters-ours', '--shadows-ours', '--rim-ours', '--circles-ours', '--rim-chain-ours', '--small-passes-ours', '--plane-shadow-ours']
FAILS = 0


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


def run(cmd, timeout=3000, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace', env=env)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


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


def read_sweep(path):
    rows = list(csv.reader(open(path)))
    return dict(head=rows[0], rows=[r for r in rows[1:] if r and r[0].isdigit()])


def sweep(fv, cap_dir, game, out_csv, extra=(), timeout=3000):
    env = dict(os.environ)
    env['FVIEW_SWEEP_COUNTS'] = '1'
    cmd = [fv, '--capture', cap_dir, '--sweep', out_csv, '--sweep-images', ','.join(map(str, IDS)), '--game', game, '--validation', '1'] + list(extra)
    out = ''
    for _ in range(12):   # the GPU is shared: retry ONLY the exact start-up refusal
        if os.path.exists(out_csv):
            os.remove(out_csv)
        rc, out = run(cmd, timeout, env)
        if 'Not enough graphics memory for the frame ring' not in out:
            break
        time.sleep(15)
    if not os.path.isfile(out_csv) or 'validation errors 0' not in out:
        print(out[-2500:])
        raise RuntimeError('fview arm failed: %s' % ' '.join(extra))
    census = ''
    for ln in out.splitlines():
        if ln.startswith('[rdr][own_group]'):
            census = ln
    return read_sweep(out_csv), census


def differing(a, b, ncols):
    n = min(len(a['rows']), len(b['rows']))
    return [i for i in range(n) if a['rows'][i][:ncols] != b['rows'][i][:ncols]]


def counts(s):
    j = s['head'].index('gpu_ops')
    return np.array([[int(x) for x in r[j:j + 5]] for r in s['rows']])   # ops, targets, clears, draws, copies


def cnum(census, group, key):
    """the number after `key` in the `group` half of the census line."""
    half = census.split('; ')[0 if group == 'shadow_module' else 1] if census else ''
    m = re.search(key + r' (\d+)', half)
    return int(m.group(1)) if m else -1


def cleft(census, group, key):
    """the number in `left out ops N (draws D, copies C)` of the group's half."""
    half = census.split('; ')[0 if group == 'shadow_module' else 1] if census else ''
    m = re.search(r'left out ops (\d+) \(draws (\d+), copies (\d+)\)', half)
    return int(m.group({'ops': 1, 'draws': 2, 'copies': 3}[key])) if m else -1


def py_census(root, zip_path, work, name):
    out = os.path.join(work, name + '_census.json')
    rc, text = run([sys.executable, '-I', os.path.join(HERE, 'own_nodes_cut.py'), 'census', zip_path, '--json', out], 1800)
    if rc or not os.path.isfile(out):
        print(text[-1500:])
        raise RuntimeError('own_nodes_cut.py census failed')
    return json.load(open(out)), text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='bryan,match')
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
    work = os.path.join(root, 'out', 's102', 'p4p5')
    os.makedirs(work, exist_ok=True)
    sheets = a.sheets or os.path.join(main_co, 'out', 's102', 'sheets', 'p4p5')
    for name in a.only.split(','):
        zip_path = os.path.join(main_co, CAPS[name])
        for need in (zip_path, game):
            if not os.path.exists(need):
                print('FAIL  input missing: ' + need)
                return 1
        print('== %s ==' % name, flush=True)
        cen, ctext = py_census(root, zip_path, work, name)
        nframes = int(re.search(r': (\d+) frames', ctext).group(1))
        for g in ('module', 'rim'):
            gg = cen[g]
            line(gg['frames'] == nframes and gg['noncontig'] == 0, '%s: the %s group is in %d of %d frames and is ONE run of the frame\'s ops in every one (%d frames with a foreign pass inside)' % (name, g, gg['frames'], nframes, gg['noncontig']))
        line(cen['extra']['both_adjacent'] == cen['extra']['both_frames'] == nframes and cen['extra']['reads_inside'] == 0,
             '%s: the rim map follows the shadow module directly in %d of %d frames; nothing inside a group span reads its images (%d reads)' % (name, cen['extra']['both_adjacent'], nframes, cen['extra']['reads_inside']))
        tot = {g: {k: sum(cen[g][k]) for k in ('passes', 'ops', 'draws', 'copies')} for g in ('module', 'rim')}
        line(sorted(set(cen['module']['dests']) | set(cen['rim']['dests'])) == sorted(IDS), '%s: the %d compared images are exactly the census\'s copy destinations (%s)' % (name, NIMG, sorted(set(cen['module']['dests']) | set(cen['rim']['dests']))))
        cut_zip = os.path.join(work, name + '_cut_b.zip')
        rc, out = run([sys.executable, '-I', os.path.join(HERE, 'own_nodes_cut.py'), 'cut', zip_path, cut_zip, '--which', 'both'], 3000)
        m = re.search(r'cut (\d+) passes \((\d+) ops\)', out)
        line(rc == 0 and m is not None and int(m.group(1)) == tot['module']['passes'] + tot['rim']['passes'] and int(m.group(2)) == tot['module']['ops'] + tot['rim']['ops'],
             '%s: own_nodes_cut.py cut removed %s passes (%s ops); the census holds %d + %d passes, %d + %d ops' % (name, m.group(1) if m else '?', m.group(2) if m else '?', tot['module']['passes'], tot['rim']['passes'], tot['module']['ops'], tot['rim']['ops']))
        rc, oc = run([sys.executable, '-I', os.path.join(HERE, 'own_nodes_cut.py'), 'oracle', cut_zip], 1800)
        rc0, oc0 = run([sys.executable, '-I', os.path.join(HERE, 'own_nodes_cut.py'), 'oracle', zip_path], 1800)
        m1, m0 = re.search(r'ORACLE remaining (\d+)', oc), re.search(r'ORACLE remaining (\d+)', oc0)
        line(rc == 0 and m1 is not None and int(m1.group(1)) == 0 and m0 is not None and int(m0.group(1)) > 0,
             '%s: oracle (no rule shared with the planner): the capture cut leaves %s node-origin draws / image writes; the uncut capture shows %s (the oracle is not vacuous)' % (name, m1.group(1) if m1 else '?', m0.group(1) if m0 else '?'))
        cap_dir = unpack(zip_path, os.path.join(work, 'unpack_' + name))
        cut_dir = unpack(cut_zip, os.path.join(work, 'unpack_' + name + '_cut'))
        rec = ['--wrestlers-ours', '--shadow-casters-ours', '--rim-ours']
        both = ['--own-shadow-module', '--own-rim-map']
        arms = {
            'game': ([], cap_dir), 'ours': (both, cap_dir), 'ours_m': (['--own-shadow-module'], cap_dir), 'ours_r': (['--own-rim-map'], cap_dir),
            'records_only': (rec, cap_dir), 'ours_records': (rec + both, cap_dir),
            'none_m': (['--own-shadow-module=none'], cap_dir), 'none_r': (['--own-rim-map=none'], cap_dir), 'none_b': (['--own-shadow-module=none', '--own-rim-map=none'], cap_dir),
            'cut_b': ([], cut_dir),
            'plant_no_reduce': (['--own-shadow-module', '--group-fault', 'no_reduce'], cap_dir), 'plant_take_plane': (['--own-shadow-module', '--group-fault', 'take_plane'], cap_dir),
            'plant_none_no_reduce': (['--own-shadow-module=none', '--group-fault', 'no_reduce'], cap_dir),
            'plant_overlap': (both + ['--group-fault', 'overlap'], cap_dir),
            'dead_game': (['--dead-work', '3'], cap_dir), 'dead_ours': (['--dead-work', '3'] + both, cap_dir),
            'alias_game': (['--copy-alias', '2'], cap_dir), 'alias_ours': (['--copy-alias', '2'] + both, cap_dir),
            'bundle_game': (BUNDLE, cap_dir), 'bundle_ours': (BUNDLE + both, cap_dir),
        }
        res, cens = {}, {}
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = {k: ex.submit(sweep, fv, d, game, os.path.join(work, '%s_%s.csv' % (name, k)), x) for k, (x, d) in arms.items()}
            for k, fu in futs.items():
                res[k], cens[k] = fu.result()
        nc = 2 + NIMG
        G = res['game']
        line(len(G['rows']) == nframes, '%s: the game arm swept %d frames (the census saw %d)' % (name, len(G['rows']), nframes))
        # census: the planner's numbers against the capture's
        c = cens['ours']
        ok = True
        for grp, g in (('shadow_module', 'module'), ('rim_map', 'rim')):
            for key, k2 in (('passes', 'passes'), ('ops', 'ops'), ('draws', 'draws'), ('copies', 'copies')):
                if cnum(c, grp, key) != tot[g][k2]:
                    ok = False
            if cnum(c, grp, 'taken') != nframes or cnum(c, grp, 'not one run') != 0 or cnum(c, grp, 're-issued ops') != tot[g]['ops']:
                ok = False
        line(ok, '%s: the planner found %s the same passes / ops / draws / copies as the capture\'s census (module %d / %d / %d / %d, rim %d / %d / %d / %d), taken as a unit in every frame, re-issued ops = ops' % (
            name, 'exactly' if ok else 'NOT', tot['module']['passes'], tot['module']['ops'], tot['module']['draws'], tot['module']['copies'], tot['rim']['passes'], tot['rim']['ops'], tot['rim']['draws'], tot['rim']['copies']))
        # ours == game
        for k in ('ours', 'ours_m', 'ours_r'):
            d = differing(G, res[k], nc)
            dc = int((counts(G) != counts(res[k])).any(axis=1).sum())
            line(not d and dc == 0, '%s: %s == game on every frame (presented + %d images; frames differing %d, GPU counts differing %d)' % (name, k, NIMG, len(d), dc))
        # records
        d_rec = differing(res['records_only'], res['ours_records'], nc)
        line(not d_rec, '%s: ours_records == records_only on every frame (the unit re-issued with the casters / rim map from the records changes nothing; frames differing %d)' % (name, len(d_rec)))
        d_g = differing(G, res['records_only'], 2)
        print('      info  records_only vs game (presented): %d of %d frames differ (the casters\' own gates; not this lane\'s bar)' % (len(d_g), nframes))
        # none must fail
        for k, grp in (('none_m', ('module',)), ('none_r', ('rim',)), ('none_b', ('module', 'rim'))):
            dp = differing(G, res[k], 2)
            di = differing(G, res[k], nc)
            line(len(dp) >= 0.9 * nframes and len(di) >= 0.9 * nframes, '%s: %s (the node cut, nothing drawn) differs from the game on %d of %d frames (picture) and %d (picture + images)' % (name, k, len(dp), nframes, len(di)))
        # counts
        cg, cb = counts(G), counts(res['none_b'])
        want_draws = tot['module']['draws'] + tot['rim']['draws']
        want_copies = tot['module']['copies'] + tot['rim']['copies']
        want_targets = tot['module']['passes'] + tot['rim']['passes']
        fell = cg.sum(axis=0) - cb.sum(axis=0)
        lo = cens['none_b']
        line(fell[3] == want_draws and fell[4] == want_copies and fell[1] == want_targets and cleft(lo, 'shadow_module', 'draws') + cleft(lo, 'rim_map', 'draws') == want_draws and cleft(lo, 'shadow_module', 'ops') + cleft(lo, 'rim_map', 'ops') == tot['module']['ops'] + tot['rim']['ops'],
             '%s: with none_b the GPU\'s draws / copies / targets fall by %d / %d / %d over the capture (the groups hold %d / %d / %d)' % (name, fell[3], fell[4], fell[1], want_draws, want_copies, want_targets))
        # none vs cut
        d_nc = differing(res['none_b'], res['cut_b'], nc)
        line(not d_nc, '%s: none_b == cut_b on every frame (the binder\'s cut is the capture\'s cut; frames differing %d)' % (name, len(d_nc)))
        # plants
        for k, what in (('plant_no_reduce', 'the reductions missing from the module group'), ('plant_take_plane', 'the plane chain taken into the module group')):
            cp = cens[k]
            bad = cnum(cp, 'shadow_module', 'passes') != tot['module']['passes'] or cnum(cp, 'shadow_module', 'ops') != tot['module']['ops']
            line(bad, '%s: PLANT %s (%s): the census count check FAILS as it must (found %d passes / %d ops, the capture has %d / %d)' % (name, k, what, cnum(cp, 'shadow_module', 'passes'), cnum(cp, 'shadow_module', 'ops'), tot['module']['passes'], tot['module']['ops']))
        # oracle in the binder's none arm, exclusivity, alias, bundle
        for k, grp in (('none_m', 'shadow_module'), ('none_r', 'rim_map')):
            left = cnum(cens[k], grp, 'oracle left')
            line(left == 0, '%s: %s: the group is cut and no draw of its origin is left in the walk (oracle left %d)' % (name, k, left))
        left = cnum(cens['plant_none_no_reduce'], 'shadow_module', 'oracle left')
        line(left > 0, '%s: PLANT no_reduce with the cut: the oracle sees %d module-origin draws left (the reductions): the oracle check FAILS as it must' % (name, left))
        d_dead = differing(res['dead_game'], res['dead_ours'], nc)
        cd = cens['dead_ours']
        line(not d_dead and cnum(cd, 'shadow_module', 'taken') == nframes and cnum(cd, 'shadow_module', 'declined') == 0 and cnum(cd, 'rim_map', 'declined') == 0,
             '%s: G4 dead work on (--dead-work 3) with both groups: ours == the same arm without them (frames differing %d), taken %d of %d, declined %d / %d' % (
                 name, len(d_dead), cnum(cd, 'shadow_module', 'taken'), nframes, cnum(cd, 'shadow_module', 'declined'), cnum(cd, 'rim_map', 'declined')))
        co = cens['plant_overlap']
        line(cnum(co, 'shadow_module', 'declined') == nframes and cnum(co, 'rim_map', 'declined') == nframes and cnum(co, 'shadow_module', 'taken') == 0 and cnum(co, 'rim_map', 'taken') == 0,
             '%s: PLANT overlap (another planner owns a group op): every frame declined and counted (%d / %d), none taken (%d / %d)' % (
                 name, cnum(co, 'shadow_module', 'declined'), cnum(co, 'rim_map', 'declined'), cnum(co, 'shadow_module', 'taken'), cnum(co, 'rim_map', 'taken')))
        d_al = differing(res['alias_game'], res['alias_ours'], nc)
        line(not d_al, '%s: --copy-alias 2: ours == game on every frame (frames differing %d)' % (name, len(d_al)))
        d_bu = differing(res['bundle_game'], res['bundle_ours'], nc)
        cb2 = cens['bundle_ours']
        line(not d_bu, '%s: the default bundle (%d promotions) with the groups == without them on every frame (frames differing %d); the groups were taken in %d / %d (module / rim) of %d frames' % (
            name, len(BUNDLE), len(d_bu), cnum(cb2, 'shadow_module', 'taken'), cnum(cb2, 'rim_map', 'taken'), nframes))
        rc_r, out_r = run([fv, '--capture', cap_dir, '--own-shadow-module', '--d3', 'casters', '--game', game], 600)
        line(rc_r != 0 and 'refused' in out_r, '%s: the D3 caster skip with --own-shadow-module is refused at start (exit %d, "refused" in the output: %s)' % (name, rc_r, 'yes' if 'refused' in out_r else 'NO'))
        if name == 'bryan' and not a.no_png:
            png_sheet(fv, cap_dir, game, a.png_frame, work, sheets, nframes)
        if not a.keep_cuts:
            for d in ('unpack_' + name, 'unpack_' + name + '_cut'):
                shutil.rmtree(os.path.join(work, d), ignore_errors=True)
            if os.path.exists(cut_zip):
                os.remove(cut_zip)
    print('%d check(s) FAILED' % FAILS if FAILS else 'all checks passed')
    return 1 if FAILS else 0


def png_sheet(fv, cap_dir, game, frame, work, sheets, nframes):
    """The sheet: the presented picture and the two outputs readers use (map B's final image 214, the rim map 223), game | ours | the node cut."""
    from PIL import Image, ImageDraw
    pd = os.path.join(work, 'png')
    os.makedirs(pd, exist_ok=True)
    arms = {'game': [], 'ours': ['--own-shadow-module', '--own-rim-map'], 'none_b': ['--own-shadow-module=none', '--own-rim-map=none']}
    shots, imgs = {}, {}
    for arm, extra in arms.items():
        d = os.path.join(pd, 'frame_' + arm)
        os.makedirs(d, exist_ok=True)
        rc, text = run([fv, '--capture', cap_dir, '--replay', '--frames', str(frame + 1), '--dump-frames', str(frame), '--dump-dir', d, '--game', game] + extra, 1800)
        fp = os.path.join(d, 'frame_%05d.png' % frame)
        if not os.path.isfile(fp):
            print(text[-1200:])
            raise RuntimeError('dump failed for ' + arm)
        shots[arm] = np.asarray(Image.open(fp).convert('RGB'))
        for iid in (214, 223):
            out = os.path.join(pd, '%s_%d.png' % (arm, iid))
            if os.path.exists(out):
                os.remove(out)
            for _ in range(12):
                rc, text = run([fv, '--capture', cap_dir, '--frame', str(frame), '--image-png', '%d,%s' % (iid, out), '--game', game] + extra, 600)
                if 'Not enough graphics memory for the frame ring' not in text:
                    break
                time.sleep(10)
            if not os.path.isfile(out):
                raise RuntimeError('image-png %s %d failed: %s' % (arm, iid, text[-600:]))
            imgs[(arm, iid)] = np.asarray(Image.open(out).convert('RGB'))
    eq = bool((shots['game'] == shots['ours']).all())
    diff = np.abs(shots['game'].astype(int) - shots['none_b'].astype(int)).max(axis=2)
    line(eq and int((diff > 8).sum()) > 0, 'bryan frame %d: the presented picture with ours equals the game\'s; the picture with the node cut differs on %d pixels by more than 8 levels' % (frame, int((diff > 8).sum())))
    os.makedirs(sheets, exist_ok=True)
    W = 1600
    tile_w = W // 3 - 8
    labels = [('game (stream)', 'game'), ('ours: both groups re-issued as units', 'ours'), ('PLANTED: the node cut, nothing drawn', 'none_b')]
    sc = tile_w / shots['game'].shape[1]
    th = int(shots['game'].shape[0] * sc)
    small = 200
    H = 22 + 18 + th + 18 + small + 20
    sheet = Image.new('RGB', (W, H), (24, 24, 28))
    dr = ImageDraw.Draw(sheet)
    dr.text((6, 4), 'S102 P4P5  Bryan entrance, frame %d: the shadow module and the rim map as owned nodes  [offline replay, 0 game runs]' % frame, fill=(235, 235, 235))
    for k, (label, arm) in enumerate(labels):
        x = 4 + k * (tile_w + 8)
        dr.text((x, 22), label, fill=(255, 210, 120) if arm == 'none_b' else (200, 230, 255))
        sheet.paste(Image.fromarray(shots[arm]).resize((tile_w, th), Image.LANCZOS), (x, 40))
        dr.text((x, 40 + th + 2), 'map B final (214) | rim map (223)', fill=(200, 200, 200))
        for j, iid in enumerate((214, 223)):
            im = Image.fromarray(imgs[(arm, iid)]).resize((tile_w // 2 - 4, small), Image.NEAREST)
            sheet.paste(im, (x + j * (tile_w // 2), 40 + th + 18))
    path = os.path.join(sheets, 'p4p5_nodes_bryan_f%d.png' % frame)
    sheet.save(path)
    with open(os.path.join(sheets, 'KEEP'), 'w') as fh:
        fh.write('S102 P4P5 shadow-module / rim-map sheet for Jarrod to scroll back to (game vs ours vs the node cut)\nuntil 2026-11-21\n')
    print('      sheet: ' + path)


if __name__ == '__main__':
    sys.exit(main())

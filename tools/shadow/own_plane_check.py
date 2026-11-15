r"""S102 lane P3 (offline step): the plane shadow chain (mask + downscale + two blurs) owned as ONE pass, proved offline on the plane captures. 0 game runs (fview, headless).

  py -I tools\shadow\own_plane_check.py [--root DIR] [--only brock,bryan] [--jobs 3] [--no-png] [--sheets DIR] [--keep]

Captures (the MAIN checkout's out\, read in place): Check B (lane86 brock, 341 frames, 12,208 mask + 682 blur draws, the chain in every frame) and Bryan (lane86 bryan, 91 frames, 2,912 mask draws).
The record the plane hook is fed is the capture's own plane constants laid out as the game's uploads and read by the SHIPPED writer's tracker (plane_oracle.py + the oracle player in fview): nothing but the capture is read.
Arms, each an fview --sweep over every frame (the presented surface and the chain's two pictures, capture images 220 = the mask, 221 = the blur result, hashed per frame, plus the frame the renderer was given counted):
  game          the stream as recorded (the game's own walk of the chain)
  inplace       --plane-shadow-ours: lane 159's per-draw replacement in the stream's own op order (the reference the group must equal)
  ours          --own-plane: every op of the four passes taken out of the walk and re-issued as ONE group at the first pass's place; the mask and blur draws made from the record by the plane hook
  ours_nv       ours with the plane hook's check against the stream's own constants OFF (plane.no_verify): the picture then rests on the record alone
  none          --own-plane=none: the chain taken out, nothing re-issued
  cut           plane_skip_cut.py: the capture itself rewritten with every op of the chain removed (the game's "the node did not run")
  tostart       plane_poison.py --to-start: every register a later draw inherits from the chain (the census: plane_leftovers.py) is handed the value the game held BEFORE the chain (the exact cut emulation; an upper bound: the capture reader drops
                a re-install that does not change a block); tostart_keep: the same with VS+PS c218 left as the chain leaves it (the register the hook must keep); poison_ctl: a register every colour draw reads poisoned (the control: must move the picture)
  twins         wrong_dest / no_copy / swap_blur (the group's own faults), palette_prev (the wrong proxy's palette), stamp_nv (the record read one frame off, never searched again, no verify), camera_stale (verify on: draws refused)
And the lockstep fview --plane-check --own-plane (arm A the stream, arm B the group, texel by texel: the chain's pictures and the presented picture).
Checks (PASS / FAIL): chain_found, group_census, lockstep (lane 159's bars: images within 2 levels, picture within 2 levels and 1e-7 of its pixels over 1 level), group_equals_inplace, record_alone, counts, cut_equals_none,
none_differs, leftovers (the census + the pre-chain emulation + the c218 arm + its control), plants (each planted twin must be caught), validation 0 on every arm.
Numbers only; the sheet goes under out\s102\sheets\p3 of the main checkout (a KEEP beside it)."""
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
import plane_chain as pc  # noqa: E402

CAPS = {'brock': 'out/lane86/brock/capture', 'bryan': 'out/lane86/bryan/capture'}
ROUNDING = 2.0        # lane 159's bars (plane_shadow_check.py): the chain's RGBA8 images within 2 levels of the stream's ...
PICTURE_MAX, PICTURE_OVER1 = 2.0, 1e-7   # ... the presented picture within 2 levels, and at most 1e-7 of its pixels more than 1 level off
IDS = (220, 221)
FAILS = 0
REPORT = {}
JUDGED = {}   # twin -> the captures it was judged on (review F10a: every twin on a capture whose chain is not blank)
NONBLANK = []


def line(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def main_checkout(here):
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
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


def fview_run(cmd, timeout=3000, env=None, need_valid=True):
    out = ''
    for _ in range(12):   # the GPU is shared: retry ONLY the exact start-up refusal
        rc, out = run(cmd, timeout, env)
        if 'Not enough graphics memory for the frame ring' not in out:
            break
        time.sleep(15)
    return rc, out


def sweep(fv, cap, game, out_csv, extra=(), oracle=None):
    env = dict(os.environ)
    env['FVIEW_SWEEP_COUNTS'] = '1'
    cmd = [fv, '--capture', cap, '--sweep', out_csv, '--sweep-images', ','.join(map(str, IDS)), '--game', game, '--validation', '1'] + (['--plane-oracle', oracle] if oracle else []) + list(extra)
    if os.path.exists(out_csv):
        os.remove(out_csv)
    rc, out = fview_run(cmd, 3000, env)
    if not os.path.isfile(out_csv) or 'validation errors 0' not in out:
        print(out[-2500:])
        raise RuntimeError('fview arm failed: %s' % ' '.join(extra))
    census = ''
    plane = ''
    for ln in out.splitlines():
        if ln.startswith('[rdr][own_plane]'):
            census = ln
        if ln.startswith('[fview] [rdr][plane_shadow]'):
            plane = ln
    return read_sweep(out_csv), census, plane


def read_sweep(path):
    rows = list(csv.reader(open(path)))
    return dict(head=rows[0], rows=[r for r in rows[1:] if r and r[0].isdigit()])


def differ(a, b, ncols=4):
    """frames where the first ncols columns (frame, presented picture, the two chain pictures) differ."""
    n = min(len(a['rows']), len(b['rows']))
    return [i for i in range(n) if a['rows'][i][:ncols] != b['rows'][i][:ncols]]


def gpu_counts(s):
    j = s['head'].index('gpu_ops')
    return np.array([[int(x) for x in r[j:j + 5]] for r in s['rows']])   # ops, targets, clears, draws, copies


def num(census, key):
    m = re.search(key + r' (\d+)', census)
    return int(m.group(1)) if m else -1


def alias_n(census):
    """copies the group aliased instead of issuing (the census's '(+N aliased)')."""
    m = re.search(r'\+(\d+) aliased', census)
    return int(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='brock,bryan')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--no-png', action='store_true')
    ap.add_argument('--sheets', default='')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--png-frame', type=int, default=100)
    a = ap.parse_args()
    root = a.root
    main_co = main_checkout(root)
    import fview_exe
    fv = fview_exe.find_or_exit(root=root, kind='dev')
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(root, 'out', 's102', 'p3')
    os.makedirs(work, exist_ok=True)
    sheets = a.sheets or os.path.join(main_co, 'out', 's102', 'sheets', 'p3')
    py = [sys.executable, '-I']
    scratch = []
    for name in a.only.split(','):
        cap_dir = os.path.join(main_co, CAPS[name])
        if not os.path.isfile(os.path.join(cap_dir, 'images.bin')):
            line(False, 'input missing (a check that reads nothing must fail): ' + cap_dir)
            return 1
        print('== %s ==' % name, flush=True)
        oracle = os.path.join(work, name + '.plane')
        rc, out = run(py + [os.path.join(HERE, 'plane_oracle.py'), 'dump', cap_dir, oracle], 1800)
        line(rc == 0, '%s oracle: %s' % (name, (out.strip().splitlines() or ['?'])[-1][:230]))
        if rc:
            return 1
        cap = pc.sc.Cap(cap_dir)
        F = pc.facts(cap)
        nf = len(F)
        chain_frames = [r for r in F if r['chain']]
        irregular = [r for r in F if not r['chain'] and r['why']]
        line(len(chain_frames) == nf and not irregular, '%s chain_found: the plane chain (mask, downscale, blur x, blur z; targets all role shadow_map) is in %d of %d frames, irregular %d' % (name, len(chain_frames), nf, len(irregular)))
        tot = {k: sum(r[k] for r in chain_frames) for k in ('mask', 'chain_clears', 'chain_ops')}
        # review F10b: the images the checks read (IDS) are the chain's copy destinations in EVERY chain frame, taken from the chain found; planted wrong ids must FAIL
        line(all(pc.ids_ok(r, IDS) for r in chain_frames) and not all(pc.ids_ok(r, (IDS[0], IDS[1] + 1)) for r in chain_frames), '%s image ids: the chain\'s copy destinations are %s (mask) and %s (blur result, three copies) in all %d chain frames; the planted wrong id %d fails the same assertion' % (name, IDS[0], IDS[1], len(chain_frames), IDS[1] + 1))
        # review F10c: no mask / downscale / blur site draw outside the chain found
        line(all(r['outside'] == 0 for r in chain_frames), '%s chain_exclusive: mask / downscale / blur site draws outside the chain found: %d over %d frames' % (name, sum(r['outside'] for r in chain_frames), len(chain_frames)))
        # ---- the cut, the poison, and the planted cut
        cut_dir, keep_dir, poison_dir, ctl_dir, tsk_dir, dup_dir = (os.path.join(work, name + s) for s in ('_cut', '_cut_keep', '_tostart', '_poison_ctl', '_tostart_keep', '_dup'))
        for d in (cut_dir, keep_dir, poison_dir, ctl_dir, tsk_dir, dup_dir):
            shutil.rmtree(d, ignore_errors=True)
            scratch.append(d)
        rc, out = run(py + [os.path.join(HERE, 'plane_skip_cut.py'), cap_dir, cut_dir], 3000)
        m = re.search(r'cut (\d+) plane-chain passes \((\d+) ops\) in (\d+) of (\d+) frames', out)
        line(rc == 0 and m is not None and int(m.group(1)) == 4 * len(chain_frames) and int(m.group(2)) == tot['chain_ops'], '%s plane_skip_cut: removed %s passes (%s ops) in %s of %s frames; the capture has %d chain passes (%d ops)' % (
            name, m.group(1) if m else '?', m.group(2) if m else '?', m.group(3) if m else '?', m.group(4) if m else '?', 4 * len(chain_frames), tot['chain_ops']))
        rc2, out2 = run(py + [os.path.join(HERE, 'plane_skip_cut.py'), cap_dir, keep_dir, '--plant', 'keep_mask'], 3000)
        rc3, out3 = run(py + [os.path.join(HERE, 'plane_poison.py'), cap_dir, poison_dir, '--to-start'], 3000)
        rcd, outd = run(py + [os.path.join(HERE, 'plane_skip_cut.py'), cap_dir, dup_dir, '--plant', 'duplicate_chain'], 3000)
        FD = pc.facts(pc.sc.Cap(dup_dir)) if rcd == 0 else []
        line(rcd == 0 and FD and all(r['chain'] and r['outside'] > 0 for r in FD), '%s planted twin duplicate_chain (the chain\'s ops inserted twice in every frame): the exclusivity count sees %d outside draws a frame' % (name, FD[0]['outside'] if FD and FD[0].get('outside') is not None else -1))
        rc5, out5 = run(py + [os.path.join(HERE, 'plane_poison.py'), cap_dir, tsk_dir, '--to-start', '--keep-reg', '218'], 3000)
        line(rc3 == 0, '%s plane_poison: %s' % (name, out3.strip().splitlines()[-1][:200] if out3.strip() else '?'))
        ctl_frames = '%d:%d' % (min(40, nf - 1), min(60, nf))
        rc4, out4 = run(py + [os.path.join(HERE, 'plane_poison.py'), cap_dir, ctl_dir, '--control-reg', '56', '--frames', ctl_frames], 3000)
        ctl_n = int(re.search(r'banks of (\d+) of', out4).group(1)) if rc4 == 0 and re.search(r'banks of (\d+) of', out4) else 0
        # ---- the arms
        ob = ['--replace-fault']
        arms = {
            'game': (cap_dir, [], None), 'inplace': (cap_dir, ['--plane-shadow-ours'], oracle), 'ours': (cap_dir, ['--own-plane'], oracle),
            'ours_nv': (cap_dir, ['--own-plane', '--replace-fault', 'plane.no_verify'], oracle), 'none': (cap_dir, ['--own-plane=none'], None),
            'cut': (cut_dir, [], None), 'cut_keep': (keep_dir, [], None), 'tostart': (poison_dir, [], None), 'tostart_keep': (tsk_dir, [], None), 'poison_ctl': (ctl_dir, [], None),
            'wrong_dest': (cap_dir, ['--own-plane', '--own-plane-fault', 'wrong_dest'], oracle), 'no_copy': (cap_dir, ['--own-plane', '--own-plane-fault', 'no_copy'], oracle),
            'swap_blur': (cap_dir, ['--own-plane', '--own-plane-fault', 'swap_blur'], oracle),
            'palette_prev': (cap_dir, ['--own-plane', '--replace-fault', 'plane.palette_prev'], oracle),
            'stamp_nv': (cap_dir, ['--own-plane', '--replace-fault', 'plane.stamp_lock,plane.no_verify'], oracle),
            'camera_stale': (cap_dir, ['--own-plane', '--replace-fault', 'plane.camera_stale'], oracle),
            # review F5: the group and the stream's own order with copy-alias mode 2 (the elided copies) must stay byte-equal; the planted no_alias_end must be caught
            'alias_inplace': (cap_dir, ['--plane-shadow-ours', '--copy-alias', '2', '--copy-alias-classes', '63'], oracle), 'alias_ours': (cap_dir, ['--own-plane', '--copy-alias', '2', '--copy-alias-classes', '63'], oracle),
            'alias_plant': (cap_dir, ['--own-plane', '--copy-alias', '2', '--copy-alias-classes', '63', '--own-plane-fault', 'no_alias_end'], oracle),
            # review F5, the exercised case: the plan elides none of the chain's copies on these captures (census above), so the downscale copy is FORCED elided (alias 221 -> 249): the blur copies after it must end the alias
            'alias_force': (cap_dir, ['--own-plane', '--own-plane-fault', 'force_alias'], oracle),
            'alias_force_plant': (cap_dir, ['--own-plane', '--own-plane-fault', 'force_alias', '--own-plane-fault', 'no_alias_end'], oracle),
            # review F4: another planner owns an op of the chain: the group must decline; with the overlap check off (the plant) it issues a dropped op
            'overlap': (cap_dir, ['--own-plane', '--own-plane-fault', 'force_overlap'], oracle),
            'overlap_plant': (cap_dir, ['--own-plane', '--own-plane-fault', 'force_overlap', '--own-plane-fault', 'ignore_overlap'], oracle),
            # review F10c: the chain duplicated in every frame (a second chain): the group must decline
            'dup': (dup_dir, ['--own-plane'], oracle),
        }
        res = {}
        t0 = time.time()
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = {k: ex.submit(sweep, fv, d, game, os.path.join(work, '%s_%s.csv' % (name, k)), extra, orc) for k, (d, extra, orc) in arms.items()}
            for k, fu in futs.items():
                try:
                    res[k] = fu.result()
                except Exception as e:
                    print('FAIL  arm %s: %s' % (k, e))
                    raise
        print('      %d arms swept in %.0f s' % (len(arms), time.time() - t0), flush=True)
        S = {k: v[0] for k, v in res.items()}
        C = {k: v[1] for k, v in res.items()}
        line(all(len(S[k]['rows']) == nf for k in S), '%s every arm swept %d frames; validation: all %d arms ran with the validation layer on, 0 errors (a failing arm raises)' % (name, nf, len(arms)))
        # ---- the group's census
        c = C['ours']
        group_ok = (num(c, 'frames with a chain') == len(chain_frames) and num(c, 'ours') == len(chain_frames) and num(c, 'every mask and blur draw made from the record') == len(chain_frames) and
                    num(c, 'some stayed the stream\'s') == 0 and num(c, 'the stream\'s stayed') == 0 and num(c, 'passes') == 4 * len(chain_frames) and num(c, 'stream ops taken') == tot['chain_ops'] and
                    num(c, 'targets') == 4 * len(chain_frames) and num(c, 'clears') == tot['chain_clears'] and num(c, 'draws') == tot['mask'] + 3 * len(chain_frames) and num(c, 'mask') == tot['mask'] and
                    num(c, 'downscale') == len(chain_frames) and num(c, 'blur') == 2 * len(chain_frames) and num(c, 'copies') == 4 * len(chain_frames) and
                    num(c, 'made by the plane hook') == tot['mask'] + 2 * len(chain_frames) and re.search(r"the stream's 0; dropped-by-another-planner ops issued 0$", c) is not None and num(c, 'overlap with another planner') == 0 and num(c, 'extra chain-site draws counted') == 0)
        line(group_ok, '%s group_census: every one of %d frames: the four passes (%d ops) taken out and re-issued as one group (targets %d, clears %d, draws %d = mask %d + downscale %d + blur %d, copies %d); mask + blur draws made from the record %d, the stream\'s 0' % (
            name, len(chain_frames), tot['chain_ops'], 4 * len(chain_frames), tot['chain_clears'], tot['mask'] + 3 * len(chain_frames), tot['mask'], len(chain_frames), 2 * len(chain_frames), 4 * len(chain_frames), num(c, 'made by the plane hook')))
        REPORT.setdefault(name, {})['census'] = c
        line('[rdr][own_plane]' in c and 'none' not in C['ours'] and num(C['none'], 'frames with a chain') == len(chain_frames) and num(C['none'], 'ours') == len(chain_frames) and num(C['none'], 'stream ops taken') == tot['chain_ops'] and
             num(C['none'], 'targets') == 0 and num(C['none'], 'draws') == 0 and num(C['none'], 'copies') == 0, '%s none: the same %d frames, %d ops taken, nothing re-issued' % (name, len(chain_frames), tot['chain_ops']))
        # ---- the group == the stream's own order with lane 159's per-draw replacement; the record alone
        d_ip = differ(S['inplace'], S['ours'])
        line(not d_ip, '%s group_equals_inplace: the group (--own-plane) equals lane 159\'s per-draw replacement in the stream\'s order on %d of %d frames (presented picture + both chain pictures byte-equal; differ %d)' % (name, nf - len(d_ip), nf, len(d_ip)))
        d_nv = differ(S['ours'], S['ours_nv'])
        line(not d_nv, '%s record_alone: with the plane hook\'s check against the stream\'s own constants OFF the group is byte-equal on %d of %d frames (the record alone carries c0-c16 and the blur rows; differ %d)' % (name, nf - len(d_nv), nf, len(d_nv)))
        d_gp = differ(S['game'], S['ours'])
        print('      game vs ours (hash): differ on %d of %d frames (our programs are the same arithmetic in another order: the lockstep below holds the bars)' % (len(d_gp), nf))
        # ---- counts
        G, O, N, K = (gpu_counts(S[k]) for k in ('game', 'ours', 'none', 'cut'))
        chain_n = np.array([[4 if r['chain'] else 0, r.get('chain_clears', 0), (r.get('mask', 0) + 3) if r['chain'] else 0, 4 if r['chain'] else 0] for r in F])   # targets, clears, draws, copies of the chain
        ours_ok = bool((O[:, 1:] == G[:, 1:]).all())
        none_ok = bool((N[:, 1:] == G[:, 1:] - chain_n).all())
        cut_ok = bool((K[:, 1:] == G[:, 1:] - chain_n).all())
        line(ours_ok and none_ok and cut_ok, '%s counts: GPU frame per frame - ours = the game\'s (targets / clears / draws / copies), none and cut = the game\'s minus the chain (4 targets, %d clears, %d draws, 4 copies over the capture)' % (
            name, tot['chain_clears'], tot['mask'] + 3 * len(chain_frames)))
        sg, sn = G[:, 1:].sum(axis=0), N[:, 1:].sum(axis=0)
        print('      GPU totals over %d frames (targets/clears/draws/copies): game %s -> with the chain cut %s; per frame: game %s -> none %s' % (nf, list(map(int, sg)), list(map(int, sn)), list(map(int, G[nf // 2, 1:])), list(map(int, N[nf // 2, 1:]))))
        REPORT[name]['counts'] = dict(game=list(map(int, sg)), cut=list(map(int, sn)), per_frame_game=list(map(int, G[nf // 2, 1:])), per_frame_none=list(map(int, N[nf // 2, 1:])))
        # ---- the cut == none; none differs from the game
        d_nc = differ(S['none'], S['cut'])
        line(not d_nc, '%s cut_equals_none: the capture rewritten with every chain op removed equals the binder\'s none arm on %d of %d frames (picture + both chain pictures)' % (name, nf - len(d_nc), nf))
        d_gn = differ(S['game'], S['none'], 2)
        d_gn_img = differ(S['game'], S['none'], 4)
        blank = len(set(r[2] for r in S['game']['rows'])) == 1 and len(set(r[3] for r in S['game']['rows'])) == 1   # the chain's two pictures never change: it draws a constant (a blank mask) in every frame
        REPORT[name]['blank_chain_pictures'] = blank
        if not blank:
            NONBLANK.append(name)
        line(((len(d_gn_img) > 0.9 * nf and len(d_gn) > 0.3 * nf) if not blank else (not d_gn_img and not d_gn)), '%s none_differs: with the chain gone the chain pictures differ on %d of %d frames and the presented picture on %d (the ring mat reads the pictures)%s' % (name, len(d_gn_img), nf, len(d_gn), ('; BLANK CHAIN: the two pictures are one constant in all %d frames (a black mask), so removing the chain changes nothing visible here: pure cost, a dead-work candidate (G4), and a capture the planted twins cannot see on' % nf) if blank else ''))
        REPORT[name]['picture_differs'] = dict(none_picture=len(d_gn), none_images=len(d_gn_img))
        d_ck = differ(S['cut'], S['cut_keep'], 4)
        line(rc2 == 0 and (len(d_ck) > 0.9 * nf or blank), '%s plant keep_mask (the cut keeps the mask pass): differs from the cut on %d of %d frames' % (name, len(d_ck), nf))
        # ---- the lockstep with lane 159's bars
        pcj = os.path.join(work, name + '_lockstep.json')
        rc, out = fview_run([fv, '--capture', cap_dir, '--plane-check', pcj, '--plane-oracle', oracle, '--own-plane', '--frames', str(nf), '--check-every', '1', '--game', game, '--validation', '1'], 3000)
        if rc or not os.path.isfile(pcj):
            print(out[-1500:])
            line(False, '%s lockstep did not run' % name)
        else:
            j = json.load(open(pcj))
            imgs = j['images']
            worst = max(v['max_diff'] for v in imgs.values())
            over = sum(v['over1_pixels'] for v in imgs.values())
            pixels = sum(v['pixels'] for v in imgs.values())
            pic = j['picture']
            ok = (len(imgs) == len(IDS) and worst <= ROUNDING and pic['max_diff'] <= PICTURE_MAX and pic['over1_pixels'] <= PICTURE_OVER1 * pic['pixels'] and j['validation_errors'] == 0 and j['no_dest'] == 0 and
                  all(v['shape_fail'] == 0 and v['frames'] > 0 for v in imgs.values()) and 'every mask and blur draw made from the record %d' % len(chain_frames) in j['report'])
            line(ok, '%s lockstep (arm A the stream, arm B the group, %d frames compared texel by texel): chain pictures worst %.0f levels (%d of %d px over 1 level), presented picture max %.0f levels (%d of %d px over 1 level), validation %d' % (
                name, j['compared'], worst, over, pixels, pic['max_diff'], pic['over1_pixels'], pic['pixels'], j['validation_errors']))
            REPORT[name]['lockstep'] = dict(images=imgs, picture=pic)
        # ---- the register-leftover question
        rc, out = run(py + [os.path.join(HERE, 'plane_leftovers.py'), '%s=%s' % (name, cap_dir), '--frames', '0:%d:%d' % (nf, max(1, nf // 40)), '--json', os.path.join(work, name + '_leftovers.json')], 3000)
        lj = json.load(open(os.path.join(work, name + '_leftovers.json')))[name]
        regs = sorted(lj['regs'])
        only_known = all((r.split()[0] == 'vs' and 124 <= int(r.split()[1][1:]) <= 138) or r.split()[1] in ('c218', 'c249', 'c250', 'c34') for r in regs)   # the palette rows past a part's own bone count, NormalFlip, two PS registers, the blur's c34
        print('      leftover census (upper bound, per register the chain changed and a later draw\'s effect reads, %d frames sampled): %s' % (lj['frames'], ', '.join(regs) if regs else 'none'))
        rcp, outp = run(py + [os.path.join(HERE, 'plane_leftovers.py'), '%s=%s' % (name, cap_dir), '--frames', '0:%d:%d' % (nf, max(1, nf // 10)), '--plant', 'inherit'], 3000)
        line(rcp == 0, '%s leftovers planted twin (a later draw made to inherit a register): the census finds it' % name)
        d_ts = differ(S['game'], S['tostart'])
        d_tk = differ(S['game'], S['tostart_keep'])
        d_ctl = differ(S['game'], S['poison_ctl'])
        edits = int(re.search(r'in (\d+) draw banks', out3).group(1)) if re.search(r'in (\d+) draw banks', out3) else -1
        line(rc3 == 0 and rc5 == 0 and rc4 == 0 and ctl_n > 0 and len(d_ctl) >= 0.9 * ctl_n and not d_tk and only_known,
             '%s leftovers (the cut with the registers it leaves): every register a later draw inherits from the chain (census, upper bound: %s) handed its PRE-chain value in every later draw (%d draw banks): the presented picture / chain pictures differ from the game on %d of %d frames; '
             'with only VS+PS c218 (NormalFlip, the chain leaves (1,1,1,0)) left as the chain leaves it: %d of %d; the control (VS c56 poisoned in every draw after the chain, %d frames) moves them on %d' % (
                 name, ', '.join(regs) if regs else 'none', edits, len(d_ts), nf, len(d_tk), nf, ctl_n, len(d_ctl)))
        REPORT[name]['leftovers'] = dict(registers=regs, to_start_differ=len(d_ts), keep218_differ=len(d_tk), control_differ=len(d_ctl), control_frames=ctl_n)
        # review F5: copy-alias mode 2
        d_al = differ(S['alias_inplace'], S['alias_ours'])
        d_alp = differ(S['alias_inplace'], S['alias_plant'])
        line(not d_al, '%s copy_alias: --own-plane --copy-alias 2 equals the stream order with the same flags on %d of %d frames (presented picture + both chain pictures; differ %d); the elided copies in the group: %d' % (name, nf - len(d_al), nf, len(d_al), alias_n(C['alias_ours'])))
        print('      planted no_alias_end (the shared copy helper does not end an alias before a real copy): differs from the stream order on %d of %d frames' % (len(d_alp), nf))
        d_af = differ(S['ours'], S['alias_force'])
        d_afp = differ(S['ours'], S['alias_force_plant'])
        line(not d_af, '%s alias_end (G8 behaviour through IssueCopy): with the downscale copy forced elided the blur copies end the alias and the group equals ours on %d of %d frames (differ %d)' % (name, nf - len(d_af), nf, len(d_af)))
        ends = lambda c: int((re.search(r'(\d+) alias ends', c) or [0, -1])[1])
        line(ends(C['alias_force']) == nf and ends(C['alias_force_plant']) == 0, '%s alias_end census: the group\'s real copies ended the forced alias in %d of %d frames; the planted no_alias_end (a PlaneRun copy without the alias end) ends %d: caught by the count (the stale alias is invisible in the pictures: the copy lands in the aliased source, whose content is read instead)' % (name, ends(C['alias_force']), nf, ends(C['alias_force_plant'])))
        JUDGED.setdefault('no_alias_end', []).append(name)
        REPORT[name]['alias'] = dict(ours_differ=len(d_al), plant_differ=len(d_alp), aliased=alias_n(C['alias_ours']))
        # review F4: overlap with another planner
        co, cp = C['overlap'], C['overlap_plant']
        line(num(co, 'overlap with another planner') == len(chain_frames) and num(co, 'dropped-by-another-planner ops issued') == 0 and num(co, 'stream ops taken') == 0 and num(co, 'targets') == 0,
             '%s overlap: with one chain op forced into another planner\'s drop set the group declines in %d of %d frames, takes 0 ops and re-issues 0 dropped ops' % (name, num(co, 'overlap with another planner'), len(chain_frames)))
        line(num(cp, 'dropped-by-another-planner ops issued') > 0, '%s planted twin ignore_overlap (the overlap check off): the census sees %d dropped ops issued: the "issued 0" bar FAILS as it must' % (name, num(cp, 'dropped-by-another-planner ops issued')))
        cd = C['dup']
        line(num(cd, 'extra chain-site draws counted') == nf and num(cd, 'targets') == 0, '%s duplicate chain: the group declines in %d of %d frames (extra chain-site draws), issues 0 targets' % (name, num(cd, 'extra chain-site draws counted'), nf))
        # ---- the planted twins
        base = S['ours']
        plant_rows = []
        for tw, why in (('wrong_dest', 'the downscale copies into the mask\'s picture'), ('no_copy', 'the mask pass\'s copy is forgotten'), ('swap_blur', 'blur z before blur x'), ('palette_prev', 'the previous proxy\'s palette'),
                        ('stamp_nv', 'the record read one frame off (offset locked), no verify')):
            dd = differ(base, S[tw])
            ok = len(dd) > (0.5 if tw == 'swap_blur' else 0.9) * nf   # swap_blur: the two blurs differ only where the mask has content (a blank mask blurs to blank in either order)
            plant_rows.append((tw, len(dd)))
            JUDGED.setdefault(tw, []).append(name) if not blank else None
            if blank:   # a blank chain's pictures cannot move: the twin is judged on brock, not here (no PASS is claimed)
                print('      planted twin %s on %s: not judged (the chain pictures are blank in every frame; differ %d of %d)' % (tw, name, len(dd), nf))
                continue
            line(ok, '%s planted twin %s (%s): the chain pictures / picture differ from ours on %d of %d frames' % (name, tw, why, len(dd), nf))
        cs = C['camera_stale']
        line(num(cs, 'some stayed the stream\'s') > 0 and num(cs, 'every mask and blur draw made from the record') < len(chain_frames), '%s planted twin camera_stale (a wrong record, verify on): the check against the stream refuses draws, frames fully from the record %d of %d (partial %d): the clean bar FAILS as it must' % (
            name, num(cs, 'every mask and blur draw made from the record'), len(chain_frames), num(cs, 'some stayed the stream\'s')))
        REPORT[name]['plants'] = dict(plant_rows)
        if name == 'brock' and not a.no_png:
            png_sheet(fv, cap_dir, game, oracle, cut_dir, work, sheets, a.png_frame, res)
    # review F10a: the proof needs at least one capture whose chain draws something, and every planted twin judged on it
    line(bool(NONBLANK), 'non-blank capture: %s (the twins are judged only where the chain pictures change)' % (', '.join(NONBLANK) or 'NONE: every capture has a blank chain'))
    for tw in ('wrong_dest', 'no_copy', 'swap_blur', 'palette_prev', 'stamp_nv', 'no_alias_end'):
        line(tw in JUDGED, 'planted twin %s was judged on %s' % (tw, ', '.join(JUDGED.get(tw, [])) or 'NO capture'))
    if not a.keep:
        for d in scratch:
            shutil.rmtree(d, ignore_errors=True)
    json.dump(REPORT, open(os.path.join(work, 'own_plane_report.json'), 'w'), indent=1, default=lambda o: int(o) if isinstance(o, np.integer) else str(o))
    print('own_plane_check: %s' % ('PASS' if not FAILS else '%d FAIL' % FAILS))
    return 1 if FAILS else 0


def png_sheet(fv, cap_dir, game, oracle, cut_dir, work, sheets, frame, res):
    """The sheet: the chain's two pictures (mask 512, blur result 256) and the presented crop, for the game, ours, none and two planted twins (labelled)."""
    from PIL import Image, ImageDraw
    pd = os.path.join(work, 'png')
    shutil.rmtree(pd, ignore_errors=True)
    os.makedirs(pd, exist_ok=True)
    arms = {'game': [], 'ours': ['--own-plane', '--plane-oracle', oracle], 'none': ['--own-plane=none'], 'wrong_dest': ['--own-plane', '--own-plane-fault', 'wrong_dest', '--plane-oracle', oracle],
            'palette_prev': ['--own-plane', '--replace-fault', 'plane.palette_prev', '--plane-oracle', oracle]}
    pic, img = {}, {}
    for arm, extra in arms.items():
        d = os.path.join(pd, 'frame_' + arm)
        os.makedirs(d, exist_ok=True)
        rc, text = fview_run([fv, '--capture', cap_dir, '--replay', '--frames', str(frame + 1), '--dump-frames', str(frame), '--dump-dir', d, '--game', game] + extra, 1800)
        fp = os.path.join(d, 'frame_%05d.png' % frame)
        if not os.path.isfile(fp):
            print(text[-1200:])
            raise RuntimeError('dump failed for ' + arm)
        pic[arm] = np.asarray(Image.open(fp).convert('RGB'))
        for iid in IDS:
            out = os.path.join(pd, '%s_%d.png' % (arm, iid))
            rc, text = fview_run([fv, '--capture', cap_dir, '--frame', str(frame), '--image-png', '%d,%s' % (iid, out), '--game', game] + extra, 600)
            if not os.path.isfile(out):
                print(text[-800:])
                raise RuntimeError('image-png %s %d failed' % (arm, iid))
            img[(arm, iid)] = np.asarray(Image.open(out).convert('RGB'))
    diff = np.abs(pic['game'].astype(int) - pic['none'].astype(int)).max(axis=2)
    ys, xs = np.nonzero(diff > 8)
    n_over = len(ys)
    H, W = diff.shape
    cwin, chwin = 320, 180   # the window with the most differing pixels (the ring mat's shadow)
    ii = np.pad((diff > 8).astype(np.int64), ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    best, y0, x0 = -1, 0, 0
    for yy in range(0, max(1, H - chwin), 10):
        for xx in range(0, max(1, W - cwin), 10):
            v = ii[yy + chwin, xx + cwin] - ii[yy, xx + cwin] - ii[yy + chwin, xx] + ii[yy, xx]
            if v > best:
                best, y0, x0 = int(v), yy, xx
    y1, x1 = min(H, y0 + chwin), min(W, x0 + cwin)
    labels = [('game: the stream\'s own walk', 'game'), ('ours: one group, from the record', 'ours'), ('none: the chain taken out', 'none'), ('PLANTED: downscale into the wrong picture', 'wrong_dest'), ('PLANTED: the previous proxy\'s palette', 'palette_prev')]
    tile, gap, lab = 256, 6, 20
    cw, ch = int((x1 - x0) * 1.6), int((y1 - y0) * 1.6)
    colw = max(tile + tile // 2 + gap, cw) + gap
    sheet = Image.new('RGB', (gap + len(labels) * colw, 24 + lab + tile + gap + ch + 40), (24, 24, 28))
    dr = ImageDraw.Draw(sheet)
    dr.text((gap, 4), 'S102 P3  Check B (lane86 brock) frame %d: the plane chain\'s pictures (mask 512x512 -> 256, blur result 256x256) and the ring mat crop x %d-%d y %d-%d; none differs from the game on %d pixels by more than 8 levels  [offline replay, 0 game runs]' % (frame, x0, x1, y0, y1, n_over), fill=(235, 235, 235))
    for k, (label, arm) in enumerate(labels):
        x = gap + k * colw
        dr.text((x, 26), label, fill=(255, 210, 120) if 'PLANTED' in label else (200, 230, 255))
        m = Image.fromarray(img[(arm, 220)]).resize((tile, tile), Image.NEAREST)
        b = Image.fromarray(img[(arm, 221)]).resize((tile // 2, tile // 2), Image.NEAREST)
        sheet.paste(m, (x, 26 + lab))
        sheet.paste(b, (x + tile + 4, 26 + lab))
        cp = Image.fromarray(pic[arm][y0:y1, x0:x1]).resize((cw, ch), Image.NEAREST)
        sheet.paste(cp, (x, 26 + lab + tile + gap))
    os.makedirs(sheets, exist_ok=True)
    path = os.path.join(sheets, 'p3_plane_chain_brock_f%d.png' % frame)
    sheet.save(path)
    until = time.strftime('%Y-%m-%d', time.localtime(time.time() + 7 * 86400))
    with open(os.path.join(sheets, 'KEEP'), 'w') as fh:
        fh.write('S102 P3 plane-chain sheet for Jarrod to scroll back to (game vs ours vs none vs planted twins)\nuntil %s\n' % until)
    print('      sheet: ' + path)
    same = all(np.array_equal(pic['game'], pic['ours']) for _ in [0])
    REPORT.setdefault('brock', {})['sheet'] = dict(path=path, none_over8=int(n_over), ours_pixel_equal=bool(same))


if __name__ == '__main__':
    sys.exit(main())

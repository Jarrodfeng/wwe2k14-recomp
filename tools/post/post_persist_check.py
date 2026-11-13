#!/usr/bin/env python3
r"""Lane 224 offline proof (S101, 0 game runs): why 30 % of the live post census was "fallback / slot", and the two things that followed. Docs: docs\renderer\POST_OURS.md, section "Fallback census (lane 224)".

  py -I tools\post\post_persist_check.py [--root DIR] [--only core,rplay,plant,combine]

THE CAUSE [C]: live.cpp binds a SKIPPED frame's lasting work (the frame minus its per-frame copies, rendered PersistentOnly, never shown) through the same Bind as a drawn frame. PlanPost then planned the post
chain on it; with the per-frame copies gone every chain input reads "not written in this frame" (writer -2), so the plan refuses: the census "slot" (28-35 % of all binds, every one of them a bind that shows
nothing). The fix: Binder::persist_only_bind (set by live.cpp around that Bind) makes PlanPost plan nothing (PostHook::PersistOnlyFrame), counted apart ("persist-only binds, not planned").

Inputs (a missing one FAILS): the case zips of tonight's sweep, from the main checkout's out\s101\runs: the arena-44 match (20261113_093119_s101_sweep_match_type, run 7148) and a victory run
(20261113_063415_s101_sweep_victory, run 7132); built rplay.exe + out\tools\post_ours_core_test.exe of this tree (tools\renderer\build.ps1, tools\post\build_post_ours_test.ps1).

  core.<run>       the core test with --live 1 --drawn timing.csv --lasting: the run's own drawn / skipped pattern, the skipped frames bound as live.cpp binds them (per-frame copies left out).
                   DRAWN frames: kept_slot == 0.  LASTING binds that carry a chain: kept_slot >= 1 and equal to (lasting binds - ours) minus the shape refusals: the old code's fallbacks, re-found offline
                   (the past-bug re-find: with the guard these binds never reach the decision).
  rplay.persist    rplay on the match capture, stream arm vs --post-ours --persist-only-every 3: the census says persist-only == the frames bound that way, ours == the rest, kept == 0; the presented
                   pictures equal the stream's (<= 1 level) on spaced frames of both kinds.
  plant.guard      the same with --persist-plant (the guard missing = the old behaviour): the census assertion must FAIL (persist-only 0, every frame planned).
  combine.<run>    the combine alone (a fog-only permutation, sequence "combine"): the core accepts it (decision: ok on every such frame), rplay stream vs ours at scale 1 and 2 on those frames and the ones
                   next to them: <= 1 level; the census keeps only the t2 + HDR shape.
"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import post_ours_check as po   # run, pngs_diff, clear_dir, parse_census, Results
from fview_exe import main_checkout

RUNS = {'match': '20261113_093119_s101_sweep_match_type', 'victory': '20261113_063415_s101_sweep_victory'}
PERSIST_RE = re.compile(r'persist-only binds, not planned: (\d+)')


def extract(main, run_dir, work):
    d = os.path.join(main, 'out', 's101', 'runs', run_dir)
    zips = sorted(f for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')) if os.path.isdir(d) else []
    if not zips:
        return None, None
    base = os.path.join(work, run_dir)
    cap = os.path.join(base, 'capture')
    tim = os.path.join(base, 'timing.csv')
    if not (os.path.isdir(os.path.join(cap, 'frames')) and os.path.isfile(tim)):
        z = zipfile.ZipFile(os.path.join(d, zips[0]))
        z.extractall(base, [n for n in z.namelist() if n.startswith('capture/') or n == 'timing.csv'])
    return cap, tim


def kv(line):
    return {k: v for k, v in (p.split('=', 1) for p in line.split() if '=' in p)}


def lasting_line(text):
    m = re.search(r'lasting binds .*?: (\d+), no chain (\d+), ours (\d+)((?:, kept_\w+ \d+)*)', text)
    if not m:
        return None
    kept = {k: int(v) for k, v in re.findall(r'kept_(\w+) (\d+)', m.group(4))}
    return {'binds': int(m.group(1)), 'nochain': int(m.group(2)), 'ours': int(m.group(3)), 'kept': kept}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='core,rplay,plant,combine')
    a = ap.parse_args()
    only = set(a.only.split(','))
    root = os.path.abspath(a.root)
    main_co = str(main_checkout(root))
    work = os.path.join(root, 'out', 's101', 'post_persist')
    os.makedirs(work, exist_ok=True)
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    core = os.path.join(root, 'out', 'tools', 'post_ours_core_test.exe')
    res = po.Results()
    for exe in (rplay, core):
        if not os.path.isfile(exe):
            res.line(False, 'input: %s is missing (build it)' % exe)
            return 1
    caps = {}
    for label, run_dir in RUNS.items():
        cap, tim = extract(main_co, run_dir, work)
        if not cap:
            res.line(False, 'input: no case zip in %s\\out\\s101\\runs\\%s' % (main_co, run_dir))
            return 1
        caps[label] = (cap, tim)

    # ------------------------------------------------------------------ core: the cause, re-found offline ---------------------------------------------------------------------------------------
    if 'core' in only:
        for label, (cap, tim) in caps.items():
            rc, text = po.run([core, '--live', '1', '--drawn', tim, '--lasting', 'c=' + cap], 900)
            c = po.core_line(text).get('c', {})
            ls = lasting_line(text)
            if rc or not c or not ls:
                res.line(False, 'core.%s: the core test gave no usable output (exit %d)' % (label, rc))
                continue
            drawn_slot = int(c.get('kept_slot', 0))
            slot = ls['kept'].get('slot', 0)
            chain_binds = ls['binds'] - ls['nochain']
            share = slot / chain_binds if chain_binds else 0.0
            ok = drawn_slot == 0 and slot >= 1 and share >= 0.4
            res.line(ok, 'core.%s: %d drawn frames, kept_slot %d (must be 0); %d lasting binds with a chain (%d ours, %d slot, %d shape): the old code refused %.0f %% of them as "slot" - the fallbacks the live census counted' % (
                label, int(c['frames']), drawn_slot, chain_binds, ls['ours'], slot, ls['kept'].get('shape', 0), 100 * share))

    # ------------------------------------------------------------------ rplay: the guard, binder level ----------------------------------------------------------------------------------------------
    out = os.path.join(work, 'run')
    os.makedirs(out, exist_ok=True)

    def arm(cap, tag, frames, shots, extra, scale=1):
        sd = os.path.join(out, tag)
        po.clear_dir(sd)
        cmd = [rplay, '--capture', cap, '--frames', str(frames), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, shots))] + (['--scale', str(scale)] if scale != 1 else []) + extra
        rc, text = po.run(cmd, 1800)
        if rc:
            print(text[-2000:])
            raise RuntimeError('rplay %s failed (exit %d)' % (tag, rc))
        return sd, text

    def worst(sd_a, sd_b, shots):
        w, px, missing = 0, 0, 0
        for f in shots:
            pa, pb = os.path.join(sd_a, 'frame_%04d.png' % f), os.path.join(sd_b, 'frame_%04d.png' % f)
            if not (os.path.isfile(pa) and os.path.isfile(pb)):
                missing += 1
                continue
            m, n = po.pngs_diff(pa, pb)
            w, px = max(w, m), px + n
        return w, px, missing

    def persist_ok(text, frames, every):
        """The assertion the guard must satisfy; returned (ok, detail) so the plant can be checked to FAIL it."""
        c = po.parse_census(text)
        m = PERSIST_RE.search(text)
        persist = int(m.group(1)) if m else 0
        want = frames // every if every else 0
        no_chain = int(re.search(r'frames with no post chain: (\d+)', text).group(1)) if re.search(r'frames with no post chain: (\d+)', text) else 0
        if not c:
            return False, 'no census line'
        planned = c['ours'] + c['fallback']
        return (persist == want and planned == frames - want - no_chain and c['fallback'] == 0), 'persist-only %d (want %d), ours %d, fallback %d, no chain %d of %d frames' % (persist, want, c['ours'], c['fallback'], no_chain, frames)

    if 'rplay' in only or 'plant' in only:
        cap = caps['match'][0]
        frames = len([f for f in os.listdir(os.path.join(cap, 'frames')) if f.endswith('.bin')])
        shots = [10, 50, 100, 101, 102, 150, 200, 201, 202, 250, 290]
        try:
            sd_s, _ = arm(cap, 'persist_stream', frames, shots, [])
            sd_o, text_o = arm(cap, 'persist_ours', frames, shots, ['--post-ours', '--persist-only-every', '3'])
            if 'rplay' in only:
                ok, det = persist_ok(text_o, frames, 3)
                res.line(ok, 'rplay.persist: every 3rd bind as a skipped frame\'s lasting work -> %s' % det)
                w, px, miss = worst(sd_s, sd_o, shots)
                res.line(w <= po.PIX and px == 0 and miss == 0, 'rplay.persist.picture: stream arm vs ours arm, %d frames (both kinds): max difference %d levels, %d pixels over, %d shots missing' % (len(shots), w, px, miss))
            if 'plant' in only:
                _, text_p = arm(cap, 'persist_plant', frames, shots[:3], ['--post-ours', '--persist-only-every', '3', '--persist-plant'])
                ok, det = persist_ok(text_p, frames, 3)
                res.line(not ok, 'plant.guard: the guard missing (the old behaviour) must FAIL the census assertion -> %s' % det)
        except RuntimeError as e:
            res.line(False, str(e))

    # ------------------------------------------------------------------ the combine alone ---------------------------------------------------------------------------------------------------------
    if 'combine' in only:
        cap, tim = caps['victory']
        rc, text = po.run([core, '--detail', 'c=' + cap], 900)
        c = po.core_line(text).get('c', {})
        left = re.findall(r'a chain shape that is not known: combine', text)
        bad_other = [l for l in text.splitlines() if l.startswith('  frame') and 'copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine' not in l]
        res.line(rc == 0 and bool(c) and not left and not bad_other, 'combine.core: "combine" alone is no longer refused (%d such refusals left), and the only frames left to the stream are the t2 + HDR + restore chain (%d other refusals)' % (len(left), len(bad_other)))
        # the frames whose decision is the combine alone: the core's --dump prints the sequence; scan the 'change' list of the capture for the frames where the sequence is exactly "combine"
        change = []
        for line in subprocess.run([core, 'c=' + cap], capture_output=True, text=True, errors='replace').stdout.splitlines():
            if line.startswith('LIST c change '):
                change = [int(x) for x in line.split(' ', 3)[3].split(',') if x]
        lone = []
        # find them by asking the core for each change frame (a handful)
        for f in change:
            rc2, t2 = po.run([core, '--dump', str(f), 'c=' + cap], 300)
            if re.search(r'^  D0 combine \[', t2, re.M) and not re.search(r'^  D1 ', t2, re.M):
                lone.append((f, 'decision: ok' in t2))
        res.line(bool(lone) and all(ok for _, ok in lone), 'combine.frames: %d frames hold the combine alone (frames %s), the core accepts %d of them' % (len(lone), [f for f, _ in lone], sum(1 for _, ok in lone if ok)))
        if lone:
            f0 = lone[0][0]
            shots = [f0 - 1, f0, f0 + 1]
            frames = f0 + 3
            for scale in (1, 2):
                sd_s, _ = arm(cap, 'combine_stream_%d' % scale, frames, shots, [], scale)
                sd_o, text_o = arm(cap, 'combine_ours_%d' % scale, frames, shots, ['--post-ours'], scale)
                w, px, miss = worst(sd_s, sd_o, shots)
                c2 = po.parse_census(text_o)
                res.line(w <= po.PIX and px == 0 and miss == 0 and c2 is not None, 'combine.picture scale %d: frames %s stream vs ours: max difference %d levels, %d pixels over, %d missing (census ours %s, fallback %s)' % (
                    scale, shots, w, px, miss, c2['ours'] if c2 else '?', c2['fallback'] if c2 else '?'))

    print('%d lines, %d failed' % (res.lines, res.fails))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())

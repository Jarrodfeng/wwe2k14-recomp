r"""S102 lane K2 (R16 piece C) offline proof: a lasting bind without the model-part group rule, through the real binder (fview), 0 game runs.

  py -I tools\renderer\nogroup_check.py [--root DIR] [--no-build] [--only B,J1] [--frames N]

fview --ring-ours --thin ring --d3 ring (the shipped D3 ring chain, emulated hook on the capture) with D3_SKIP=every2 D3_SKIP_LASTING=prune: every other frame is a skipped frame; one with lasting work is bound
persist-only on the pruner's kept ops (live.cpp's order). D3_NOGROUP=1 is the switch (wwe2k14_rdr_lasting_nogroup): the pruner keeps only the passes that feed a lasting copy, and the chains count their call ordinals,
markers, the calls = runs guard and the per-object call-count guards over the WHOLE frame's ops (binder.h ord_full_ops).

Captures (both REQUIRED: a missing one FAILS by name): B = out\s101\thin\case_brock (Check B, 341 frames), J1 = out\s101\lane92\case_20261112_172341_872_0_pc_run_exit (343 frames).
Arms per capture (pass 1 writes the ring ledger once):
  group     today's shipped lasting bind (the group rule), natural seeds: the reference counts
  nogroup   D3_NOGROUP=1, natural seeds                       bars: validation errors 0, exit 0; calls = runs differ 0; call-count guard trips == the group arm's; holes 0; unconsumed 0; ordinal mismatch 0;
                                                              no sequence 0; gate resets 0; latched 0; ring markers + part draws within the group arm's (the lever may only draw more of the game's draws)
  gstress / nstress   the same two with D3_NOGROUP_STRESS=1 (TEST ONLY: every skipped frame is forced to carry one lasting copy, the f-th copy op, so the lasting bind runs on ~170 frames instead of ~28)
                                                              bars: nstress equals gstress on every count above; the lever TOOK: ordinal view frames > 0 and object passes left out although another was kept > 0
  bundle    nstress + the shipped default bundle (--shadows-ours --shadow-casters-ours --plane-shadow-ours): the same bars and the lever's taken count reported (0 taken = the lever does nothing for players)
Planted twins (on the stress arm, each must TRIP, i.e. a bar above fails: calls = runs differ > 0 or guard trips differ from the group arm's or holes / unconsumed > 0):
  boundord  the ordinals are taken from the bound ops (an ordinal taken from a bound pass)
  droppass  the ordinal view lacks the whole pass of one dropped keyed object draw (one object pass dropped)
  half      the group rule is applied to about half of the object passes only (a hash of pass and frame), with the bound-op ordinals (the group rule half removed)
Each plant checks it applied (ordinal view frames > 0 for droppass; object passes left out > 0 for half / boundord) and fails as DID NOT APPLY otherwise.
Exit 1 on any failed bar or an unreadable input."""
# S102 K4: D3_SKIP=every2 / every3 on a capture recorded live (J1) needs a teach list (D3_SKIP_TEACH_OUT on a no-skip run, D3_SKIP_TEACH_IN on the skip arms), else the ring is never learned and pictures are not a baseline: SKIP_COST.md finding 8. fview prints "WARN K4" when it is missing.
import argparse
import importlib.util
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'fview'))


def load_ring_check():
    spec = importlib.util.spec_from_file_location('d3_ring_check_k2', os.path.join(HERE, '..', 'd3', 'd3_ring_check.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


RC = load_ring_check()
PRUNE = re.compile(r'D3_SKIP_LASTING=prune(?: \(D3_NOGROUP\))?: (\d+) pruned skipped frames so far, (\d+) of them with no D3 / thin begin-end \(no object pass kept\); object passes (\d+), kept (\d+), left out although another was kept (\d+); '
                   r'ordinal view frames (\d+), runs counted not learned (\d+)(?:; Bind\(\) mean ([\d.]+) ms)?')
TRIPS = re.compile(r'call-count guard trips (\d+)')
VALID = re.compile(r'validation errors (\d+)')
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--chains-only', action='store_true', help='only the arena and caster chain arms (a smoke)')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = os.environ.get('RDR_MAIN_CHECKOUT') or main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    caps = {'B': os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture'),
            'J1': os.path.join(main_co, 'out', 's101', 'lane92', 'case_20261112_172341_872_0_pc_run_exit', 'capture')}
    only = set(x for x in args.only.split(',') if x)
    for k, p in caps.items():
        if only and k not in only:
            continue
        if not (os.path.isfile(os.path.join(p, 'images.bin')) and os.path.isdir(os.path.join(p, 'scene'))):
            print('FAIL  input missing: capture %s: %s' % (k, p))
            return 1
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=2400)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            print('FAIL  fview build (exit %d)' % p.returncode)
            return 1
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'k2')
    os.makedirs(out, exist_ok=True)

    RING = ['--ring-ours', '--thin', 'ring', '--d3', 'ring']

    def run_fview(tag, ck, extra, env, frames, chain=None, cap=None):
        d = os.path.join(out, '%s_%s' % (ck, tag))
        cmd = [exe, '--capture', cap or caps[ck], '--game', game, '--frames', str(frames), '--thin-run', d] + (RING if chain is None else chain) + extra
        e = dict(os.environ)
        for k in ('D3_SKIP', 'D3_SKIP_LASTING', 'D3_NOGROUP', 'D3_NOGROUP_STRESS', 'D3_NOGROUP_PLANT', 'D3_NOGROUP_TARGETS', 'D3_GROUP_VIEW'):
            e.pop(k, None)
        e.update(env)
        text, rc = '', 1
        for attempt in range(8):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, env=e)
            text, rc = (p.stdout or '') + (p.stderr or ''), p.returncode
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                print('  (GPU busy: retry %d in 40 s)' % (attempt + 1), flush=True)
                time.sleep(40)
                continue
            break
        m = RC.LINE.search(text)
        c = dict(zip(RC.KEYS, [m.group(1)] + list(map(int, m.groups()[1:])))) if m else None
        pm = PRUNE.findall(text)
        pr = dict(zip(('frames', 'blind', 'obj_passes', 'obj_kept', 'obj_dropped', 'view_frames', 'not_learned'), map(int, pm[-1][:7]))) if pm else None
        if pr is not None:
            pr['bind_ms'] = float(pm[-1][7]) if len(pm[-1]) > 7 and pm[-1][7] else 0.0
        od = re.search(r'proof overdue (\d+)', text)
        gen = {k: int(v) for k, v in zip(('calls_differ_any',), re.search(r'calls = runs checked \d+ differ (\d+)', text).groups())} if re.search(r'calls = runs checked \d+ differ (\d+)', text) else {}
        tm = TRIPS.search(text)
        vm = VALID.search(text)
        csv = os.path.join(d, 'frames.csv')
        rows = RC.read_csv(csv) if os.path.isfile(csv) else {}
        return dict(rc=rc, c=c, pr=pr, trips=int(tm.group(1)) if tm else None, valid=int(vm.group(1)) if vm else -1, text=text, rows=rows, overdue=int(od.group(1)) if od else None, gen=gen)

    def same_as(ref, arm, what, ck=''):
        c, r = arm['c'], ref['c']
        ok = c is not None and r is not None and c['calls_differ'] == r['calls_differ'] == 0 and arm['trips'] == ref['trips'] and c['holes'] == 0 and c['unconsumed'] == 0 and c['ord_mism'] == 0 and c['no_seq'] == 0 \
            and c['gate_resets'] == 0 and c['latched'] == 0 and arm['valid'] == 0 and arm['rc'] == 0
        pic = [f for f in sorted(ref['rows']) if arm['rows'].get(f) != ref['rows'][f] and ref['rows'][f] != '0']   # the drawn frames' pictures (a skipped frame writes hash 0)
        if ck == 'J1':   # INFO, not a bar: J1's offline skip emulation loses the ring from frame ~200 in EVERY variant (see docs/renderer/SKIP_COST.md K2 finding 6); the arms cannot be compared by picture there
            print('INFO  %s: J1 drawn pictures differing from the group arm: %d of %d (first %s) [not a bar: the plain skip arm and the group arm already differ from the no-skip run on J1]' % (what, len(pic), sum(1 for h in ref['rows'].values() if h != '0'), pic[:4]), flush=True)
        else:
            say(ok and not pic and len(arm['rows']) == len(ref['rows']) > 0, '%s: the pictures of the %d drawn frames are byte-equal to the group arm (%d differ%s)' % (what, sum(1 for h in ref['rows'].values() if h != '0'), len(pic), (': ' + str(pic[:8])) if pic else ''))
        if c is not None and r is not None:   # F5: what the chain learned and proved must not move with the lever (the guards see every run, Observe learns the kept ones)
            eq = c['proof_obj'] == r['proof_obj'] and c['marked'] == r['marked'] and c['complete_of'] == r['complete_of'] and arm['overdue'] == ref['overdue']
            if ck == 'J1' and 'stress' in what and not eq:   # INFO: with a lasting bind on EVERY skipped frame the group rule's binds teach the thin store (ThinAfter pins) far more than nogroup's; docs/renderer/SKIP_COST.md K2 finding 7
                print('INFO  %s: proof objects %s / %s, objects marked %s / %s of %s / %s, proof overdue %s / %s (arm / group arm): the lever slows the D3 learning when a lasting bind runs on every skipped frame (not a bar on J1 stress)' % (what, c['proof_obj'], r['proof_obj'], c['marked'], r['marked'], c['complete_of'], r['complete_of'], arm['overdue'], ref['overdue']), flush=True)
            else:
                say(eq, '%s: proof objects %s / %s, objects marked %s / %s of %s / %s, proof overdue %s / %s (arm / group arm: must be equal), learned %s / %s' % (what, c['proof_obj'], r['proof_obj'], c['marked'], r['marked'], c['complete_of'], r['complete_of'], arm['overdue'], ref['overdue'], c['learned'], r['learned']))
        say(ok, '%s: exit %s, validation errors %s, calls = runs checked %s differ %s (group arm: %s), guard trips %s (group arm %s), holes %s, unconsumed %s, ordinal mismatch %s, no sequence %s, gate resets %s, latched %s' % (
            what, arm['rc'], arm['valid'], c and c['calls_checked'], c and c['calls_differ'], r and r['calls_differ'], arm['trips'], ref['trips'], c and c['holes'], c and c['unconsumed'], c and c['ord_mism'], c and c['no_seq'], c and c['gate_resets'], c and c['latched']))
        return ok

    def trips(arm, ref):   # the plant is CAUGHT when a count the honest arm holds at its reference value moved
        c = arm['c']
        if c is None:
            return False, 'no census line'
        moved = [n for n, v in (('calls differ', c['calls_differ']), ('holes', c['holes']), ('unconsumed', c['unconsumed']), ('ordinal mismatch', c['ord_mism'])) if v > 0]
        if arm['trips'] != ref['trips']:
            moved.append('guard trips %s (honest %s)' % (arm['trips'], ref['trips']))
        return bool(moved), ', '.join(moved) if moved else 'nothing moved'

    for ck in caps:
        if (only and ck not in only) or args.chains_only:
            continue
        n = args.frames or len(os.listdir(os.path.join(caps[ck], 'frames')))
        print('--- capture %s (%d frames) ---' % (ck, n), flush=True)
        ledger = os.path.join(out, 'ledger_%s.txt' % ck)
        p1 = run_fview('pass1', ck, ['--d3-ledger-out', ledger], {}, n)
        say(p1['rc'] == 0 and os.path.isfile(ledger), '[%s] pass 1 (the ring ledger): exit %s' % (ck, p1['rc']))
        base = ['--d3-ledger-in', ledger]
        skip = {'D3_SKIP': 'every2', 'D3_SKIP_LASTING': 'prune'}
        group = run_fview('group', ck, base, dict(skip), n)
        nog = run_fview('nogroup', ck, base, dict(skip, D3_NOGROUP='1'), n)
        say(group['pr'] is not None and group['pr']['frames'] > 0, '[%s] group arm: %s pruned skipped frames, object passes %s kept %s' % (ck, group['pr'] and group['pr']['frames'], group['pr'] and group['pr']['obj_passes'], group['pr'] and group['pr']['obj_kept']))
        same_as(group, nog, '[%s] nogroup (natural seeds) equals the group arm' % ck, ck)
        print('INFO  [%s] natural seeds: the lever took %s of %s pruned frames (object passes left out although another was kept %s, runs counted not learned %s)' % (
            ck, nog['pr'] and nog['pr']['view_frames'], nog['pr'] and nog['pr']['frames'], nog['pr'] and nog['pr']['obj_dropped'], nog['pr'] and nog['pr']['not_learned']), flush=True)
        stress = dict(skip, D3_NOGROUP_STRESS='1')
        gs = run_fview('gstress', ck, base, dict(stress), n)
        ns = run_fview('nstress', ck, base, dict(stress, D3_NOGROUP='1'), n)
        same_as(gs, ns, '[%s] stress: nogroup equals the group arm' % ck, ck)
        pr = ns['pr']
        say(pr is not None and pr['view_frames'] > 0 and pr['obj_dropped'] > 0, '[%s] stress: the lever took: ordinal view frames %s of %s pruned frames, object passes left out although another was kept %s of %s, runs counted not learned %s (the group arm keeps %s of %s object passes)' % (
            ck, pr and pr['view_frames'], pr and pr['frames'], pr and pr['obj_dropped'], pr and pr['obj_passes'], pr and pr['not_learned'], gs['pr'] and gs['pr']['obj_kept'], gs['pr'] and gs['pr']['obj_passes']))
        cg, cn = gs['c'], ns['c']
        say(cg is not None and cn is not None and cn['markers'] <= cg['markers'] and cn['parts'] <= cg['parts'], '[%s] stress: ring markers %s / part draws %s against the group arm\'s %s / %s (the lever leaves marker draws to the game\'s own; it may not invent any)' % (
            ck, cn and cn['markers'], cn and cn['parts'], cg and cg['markers'], cg and cg['parts']))
        bun = run_fview('bundle', ck, base + ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours'], dict(stress, D3_NOGROUP='1'), n)
        bun_ref = run_fview('bundle_g', ck, base + ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours'], dict(stress), n)
        same_as(bun_ref, bun, '[%s] default bundle on, stress: nogroup equals the group arm' % ck, ck)
        bp = bun['pr']
        say(bp is not None and bp['view_frames'] > 0, '[%s] default bundle: the lever took %s of %s pruned frames (0 would mean it does nothing for players)' % (ck, bp and bp['view_frames'], bp and bp['frames']))
        for plant in ('boundord', 'droppass', 'half'):
            pa = run_fview('plant_' + plant, ck, base, dict(stress, D3_NOGROUP='1', D3_NOGROUP_PLANT=plant), n)
            ppr = pa['pr']
            applied = ppr is not None and ppr['obj_dropped'] > 0 and (plant != 'droppass' or ppr['view_frames'] > 0)
            if not applied:
                say(False, '[%s] plant %s DID NOT APPLY (pruned %s, left out %s, view frames %s)' % (ck, plant, ppr and ppr['frames'], ppr and ppr['obj_dropped'], ppr and ppr['view_frames']))
                continue
            ok, why = trips(pa, gs)
            say(ok, '[%s] plant %s: CAUGHT (%s)' % (ck, plant, why) if ok else '[%s] plant %s NOT caught (%s)' % (ck, plant, why))
        # F4: the lasting-target sub-switch (D3_NOGROUP_TARGETS): same bars as the nogroup arms, and the cost of a lasting bind (Bind() mean, the ThinBeginFrame + chain End calls included) for the three arms
        nt = run_fview('ntargets', ck, base, dict(skip, D3_NOGROUP='1', D3_NOGROUP_TARGETS='1'), n)
        nts = run_fview('ntargets_s', ck, base, dict(stress, D3_NOGROUP='1', D3_NOGROUP_TARGETS='1'), n)
        same_as(group, nt, '[%s] nogroup + lasting-target sub-switch (natural) equals the group arm' % ck, ck)
        same_as(gs, nts, '[%s] stress: nogroup + lasting-target sub-switch equals the group arm' % ck, ck)

        def cost(label, x):
            q = x['pr']
            return '%s: Bind() mean %.3f ms over %s lasting binds, %s with no object pass (blind)' % (label, q['bind_ms'], q['frames'], q['blind']) if q else label + ': no census'
        print('INFO  [%s] cost of a lasting bind, natural seeds: %s | %s | %s' % (ck, cost('group', group), cost('nogroup', nog), cost('nogroup + targets', nt)), flush=True)
        print('INFO  [%s] cost of a lasting bind, stress: %s | %s | %s' % (ck, cost('group', gs), cost('nogroup', ns), cost('nogroup + targets', nts)), flush=True)
        # plant nolasttarget (the sub-switch on, the rule planted off): the object passes kept fall to the plain nogroup arm's
        pl = run_fview('plant_nolasttarget', ck, base, dict(skip, D3_NOGROUP='1', D3_NOGROUP_TARGETS='1', D3_NOGROUP_PLANT='nolasttarget'), n)
        ppr, hpr = pl['pr'], nt['pr']
        if ppr is None or hpr is None or ppr['view_frames'] == 0:
            say(False, '[%s] plant nolasttarget DID NOT APPLY (view frames %s)' % (ck, ppr and ppr['view_frames']))
        else:
            say(ppr['obj_kept'] < hpr['obj_kept'], '[%s] plant nolasttarget (the sub-switch on, the rule planted off): %s (object passes kept %s against the sub-switch arm: %s)' % (ck, 'CAUGHT' if ppr['obj_kept'] < hpr['obj_kept'] else 'NOT caught', ppr['obj_kept'], hpr['obj_kept']))
        # F1 plant: the attachment-load marking off (INFO here; the semantic twin is the unit test TestAttachmentLoad)
        pa2 = run_fview('plant_noattach', ck, base, dict(stress, D3_NOGROUP='1', D3_NOGROUP_PLANT='noattach'), n)
        ap = pa2['pr']
        print('INFO  [%s] plant noattach (a kept pass own attachments not marked needed): object passes kept %s against the honest arm: %s' % (ck, ap and ap['obj_kept'], ns['pr'] and ns['pr']['obj_kept']), flush=True)
        # F5 twin: Observe skips every run: what the chain learned / proved must fall below the honest arm's
        po = run_fview('plant_noobserve', ck, base, dict(stress, D3_NOGROUP='1', D3_NOGROUP_PLANT='noobserve'), n)
        pc, hc = po['c'], ns['c']
        if pc is None or hc is None:
            say(False, '[%s] plant noobserve: no census line' % ck)
        else:
            moved = pc['proof_obj'] != hc['proof_obj'] or pc['learned'] != hc['learned'] or pc['marked'] != hc['marked']
            say(moved, '[%s] plant noobserve (Observe skips every run): %s (proof objects %s / learned %s / marked %s against the honest arm: %s / %s / %s)' % (ck, 'CAUGHT' if moved else 'NOT caught', pc['proof_obj'], pc['learned'], pc['marked'], hc['proof_obj'], hc['learned'], hc['marked']))
        # F3: D3 off (thin ring only): does the pruning alone change the drawn pictures? (J1: it does; the D3 chain is not the cause: group with D3 on equals group with D3 off)
        if ck == 'J1':
            tg = run_fview('thin_group', ck, [], dict(skip), n, chain=['--ring-ours', '--thin', 'ring'])
            tn = run_fview('thin_nogroup', ck, [], dict(skip, D3_NOGROUP='1'), n, chain=['--ring-ours', '--thin', 'ring'])
            d_pr = [f for f in sorted(tg['rows']) if tn['rows'].get(f) != tg['rows'][f] and tg['rows'][f] != '0']
            d_d3 = [f for f in sorted(tg['rows']) if group['rows'].get(f) != tg['rows'][f] and tg['rows'][f] != '0']
            print('INFO  [J1] D3 OFF (thin ring only): group vs nogroup drawn pictures differ in %d frames (first %s); group arm with D3 ON vs D3 OFF: %d frames differ. So pruning alone moves the J1 pictures; the D3 chain does not.' % (len(d_pr), d_pr[:4], len(d_d3)), flush=True)
        # F3: the SHIPPED group rule with the full ordinal view (D3_GROUP_VIEW): does Observe's marker bank (mk_vs) differ when taken from the full ops? INFO: it names what the shipped lasting bind gets wrong, if anything
        for label, env in (('natural', skip), ('stress', stress)):
            ref = group if label == 'natural' else gs
            gv = run_fview('groupview_' + label, ck, base, dict(env, D3_GROUP_VIEW='1'), n)
            cg2, cv2 = ref['c'], gv['c']
            if cg2 is None or cv2 is None:
                print('INFO  [%s] group + full view (%s): no census' % (ck, label))
                continue
            diffs = [k for k in ('learned', 'proof_obj', 'proof_parts', 'proof_mism', 'marked', 'markers', 'parts', 'holes', 'calls_differ') if cg2[k] != cv2[k]]
            picd = [f for f in sorted(ref['rows']) if gv['rows'].get(f) != ref['rows'][f] and ref['rows'][f] != '0']
            print('INFO  [%s] SHIPPED group rule + full ordinal view (%s): counts that move: %s; guard trips %s vs %s; %d drawn pictures differ' % (ck, label, ', '.join('%s %s->%s' % (k, cg2[k], cv2[k]) for k in diffs) or 'none', ref['trips'], gv['trips'], len(picd)), flush=True)
    # F6: the other D3 chains under the pruned nogroup bind (stress seeds: the lasting bind runs on every skipped frame)
    chain_arms = [('arena', 'B', ['--arena-ours', '--thin', 'arena', '--d3', 'arena'], None),
                  ('casters', None, ['--ring-ours', '--thin', 'ring', '--d3', 'casters'], os.path.join(main_co, 'out', 'd3caster', 'z7147', 'capture'))]
    for cname, ck0, cflags, cpath in chain_arms:
        if only and (ck0 or 'B') not in only:
            continue
        path = cpath or caps[ck0]
        if not (os.path.isfile(os.path.join(path, 'images.bin')) and os.path.isdir(os.path.join(path, 'scene'))):
            say(False, 'input missing for the %s chain arm: %s' % (cname, path))
            continue
        n = args.frames or len(os.listdir(os.path.join(path, 'frames')))
        print('--- chain %s (%d frames) ---' % (cname, n), flush=True)
        led = os.path.join(out, 'ledger_chain_%s.txt' % cname)
        run_fview('pass1', 'chain_' + cname, ['--d3-ledger-out', led], {}, n, chain=cflags, cap=path)
        cbase = ['--d3-ledger-in', led]
        cstress = {'D3_SKIP': 'every2', 'D3_SKIP_LASTING': 'prune', 'D3_NOGROUP_STRESS': '1'}
        cg = run_fview('group', 'chain_' + cname, cbase, dict(cstress), n, chain=cflags, cap=path)
        cn = run_fview('nogroup', 'chain_' + cname, cbase, dict(cstress, D3_NOGROUP='1'), n, chain=cflags, cap=path)

        def cl(x, key):
            line = next((l for l in x['text'].splitlines() if (l.startswith('[rdr][d3] arena markers') if cname == 'arena' else l.startswith('[rdr][d3][casters] mode'))), '')
            m = re.search(key + r' (\d+)', line)
            return int(m.group(1)) if m else None

        keys = ('holes', 'unconsumed', 'ordinal mismatch', 'no sequence')
        vals_g = {k: cl(cg, k) for k in keys}
        vals_n = {k: cl(cn, k) for k in keys}

        def calls(x):
            m = re.search(r'calls = runs checked \d+ differ (\d+)', x['text'])
            return int(m.group(1)) if m else None
        latched = 'latched off' in cn['text']
        ok = cg['rc'] == 0 and cn['rc'] == 0 and cn['valid'] == 0 and cg['pr'] is not None and cn['pr'] is not None and all(v == 0 for v in vals_n.values()) and not latched and calls(cn) == calls(cg)
        say(ok, '[%s chain] nogroup vs group arm (stress): exit %s / %s, validation %s, %s (group arm %s), calls = runs differ %s (group arm %s), latched off: %s' % (cname, cn['rc'], cg['rc'], cn['valid'], vals_n, vals_g, calls(cn), calls(cg), latched))
        q = cn['pr']
        say(q is not None and q['view_frames'] > 0 and q['obj_dropped'] > 0, '[%s chain] the lever took %s of %s pruned frames, object passes left out although another was kept %s' % (cname, q and q['view_frames'], q and q['frames'], q and q['obj_dropped']))
        pic = [f for f in sorted(cg['rows']) if cn['rows'].get(f) != cg['rows'][f] and cg['rows'][f] != '0']
        print('INFO  [%s chain] drawn pictures differing from the group arm: %d of %d' % (cname, len(pic), sum(1 for h in cg['rows'].values() if h != '0')), flush=True)
    print('nogroup_check: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

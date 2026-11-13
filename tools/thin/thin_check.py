#!/usr/bin/env python3
"""Lane 122 (KB) offline proof, 0 game runs: the arena's D1 thin draws through the REAL binder on Check B's capture (run 7051, arena 4, 341 frames).
Lane 129 (KC): the same check for the ring and the wrestlers (`--cls ring|wrestlers`: the reference arm is that class's own --ring-ours / --wrestlers-ours).

  py -I tools\\thin\\thin_check.py [--root DIR] [--cls arena|ring|wrestlers] [--no-build] [--frames N] [--only a,b] [--k K] [--lag L]

One fview process per arm, one pass over the frames (fview --thin-run: frames bound and rendered once each; frames.csv = a 64-bit hash of the presented picture per frame).
The reader is EMULATED on the capture: the pins published `lag` binds ago decide which keyed draws go lite, and the fields a lite reader would not fill (geometry offsets /
bytes / counts, layout; the shader hashes STAY: the program is part of the key) are blanked before the binder sees the draw (binder.h ThinEmulateReader, the same FrameLite header the game's reader runs).

Bars (each line PASS / FAIL; a missing input FAILS):
  ours     the arena's --arena-ours arm: the reference (replaced R, kept 0 unjoined 0 mismatch 0 ...).
  thin     --thin arena: the picture of ALL frames is byte-equal to `ours`; census: thin > 0, holes 0, proof mismatches 0, epoch drops 0, slot missing 0, and
           thin + (the hook's replaced) = ours' replaced (every stream draw the hook replaced in the reference was drawn by template or by the full path).
  churn    --thin-churn 5 (every 5th frame a pinned key is unpinned): holes 0, pictures equal. The handshake under load.
  valid    8 frames with the validation layer: errors 0.
Planted faults (must be DETECTED, by the check named). Lane 129: ring: rope_colour_instance0, skirt_tie; wrestlers: template_rows, cloth_pinned (plus release_without_ack and
read_stream for both); the swap plant is the arena's.
  release_without_ack (+ churn 5)  holes > 0        first_template        proof mismatches > 0 or the picture moves
  wrong_site_template              proof mismatches > 0 or the picture moves      all_mips   proof mismatches > 0 (levels)
  read_stream                      the picture moves (the lite draws' blanked geometry fields are read: layout / counts are wrong)
  no_program_key                   (your own first-run bug) proof mismatches > 0 or the picture moves
  epoch_mid_run / isolate_bind     holes 0 (epoch drops > 0 / the isolate picture equals the isolated ours arm)
  no_ordinal                       reported only (does Check B have repeated triples?)
  no_proof + slot swap             --thin-k 0 with the stream's slot 0 picture swapped from frame S: the picture differs from the swapped reference; with proof frames it heals."""
import argparse
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SWAP = 150


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def read_trace(path):
    """lane 129 review F7: trace.csv (frame, op, fate, keep_why) of the model-part draws -> {(frame, op): (fate, keep_why)}"""
    rows = {}
    with open(path) as fh:
        next(fh)
        for line in fh:
            x = line.strip().split(',')
            rows[(int(x[0]), int(x[1]))] = (int(x[2]), int(x[3])) + tuple(int(v) for v in x[4:])   # (fate, keep_why[, thin, swapped])
    return rows


FATE = {'arena': 3, 'wrestlers': 4, 'ring': 6}   # binder.h kFateArenaOurs / kFateWrestlersOurs / kFateRingOurs
WHY_PALETTE = 6                                   # KeepWhy::Palette (5) + 1


def read_csv(path):
    rows = {}
    with open(path) as fh:
        next(fh)
        for line in fh:
            f, h, d, b = line.strip().split(',')
            rows[int(f)] = (h, int(d), int(b))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--frames', type=int, default=341)
    ap.add_argument('--only', default='')
    ap.add_argument('--cls', default='arena', choices=['arena', 'ring', 'wrestlers'])  # lane 129 (KC)
    ap.add_argument('--k', type=int, default=-1)
    ap.add_argument('--lag', type=int, default=2)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        print("FAIL  input missing: Check B's case zip under " + runs)
        return 1
    if not os.path.isdir(game):
        print('FAIL  input missing: the game files ' + game)
        return 1
    out = os.path.join(main_co, 'out', 's101', 'thin')
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'case_brock'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'case_brock'))
    if not os.path.isdir(os.path.join(cap, 'scene')):
        print('FAIL  the capture has no scene\\ records: ' + cap)
        return 1
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            print('FAIL  fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    os.makedirs(out, exist_ok=True)
    only = set(x for x in args.only.split(',') if x)
    want = lambda n: not only or n in only
    n = args.frames
    cls = args.cls
    ours_flag = '--%s-ours' % cls
    pre = '' if cls == 'arena' else cls + '_'
    thin_re = re.compile(r'\[rdr\]\[thin\] ' + cls + r' thin (\d+), proof (\d+) \(ok (\d+)\), learned (\d+), pinned (\d+), holes (\d+), proof mismatches (\d+) \(program (\d+), layout (\d+), slot image (\d+), '
                         r'slot view (\d+), levels (\d+), geometry (\d+), counts (\d+)\), unpinned (\d+), released (\d+), slot missing (\d+), tween key moved (\d+), full other (\d+), epoch drops (\d+)')
    arena_re = re.compile(r'\[rdr\]\[' + cls + r'\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+)')
    cache = {}

    def arm(tag, extra):
        if tag in cache:
            return cache[tag]
        d = os.path.join(out, 'arm_' + pre + tag)
        cmd = [exe, '--capture', cap, ours_flag, '--game', game, '--frames', str(n), '--thin-run', d] + extra
        text = ''
        for attempt in range(8):  # (the GPU is shared: this exact message is retried, nothing else)
            rc, text = run(cmd, 1500)
            if 'Not enough graphics memory for the frame ring' in text and attempt < 7:
                print("  (GPU busy: frame ring refused, retry %d in 40 s)" % (attempt + 1), flush=True)
                time.sleep(40)
                continue
            break
        csv = os.path.join(d, 'frames.csv')
        if rc not in (0, 4) or not os.path.isfile(csv):
            print(text[-2500:])
            raise RuntimeError('arm %s failed (exit %d)' % (tag, rc))
        m = thin_re.search(text)
        a = arena_re.search(text)
        res = {'rows': read_csv(csv), 'text': text, 'trace': read_trace(os.path.join(d, 'trace.csv')) if os.path.isfile(os.path.join(d, 'trace.csv')) else {},
               'thin': dict(zip(['thin', 'proof', 'proof_ok', 'learned', 'pinned', 'holes', 'mism', 'd_prog', 'd_layout', 'd_image', 'd_view', 'd_levels', 'd_geom', 'd_counts', 'unpinned', 'released',
                                 'slot_missing', 'blend_moved', 'full_other', 'epoch_drops'], map(int, m.groups()))) if m else None,
               'replaced': int(a.group(3)) if a else -1, 'kept': int(a.group(4)) if a else -1,
               'palette_kept': int(pal.group(1)) if (pal := re.search(r'\[rdr\]\[' + cls + r'\] ours frames .*?palette (\d+)', text)) else 0}
        cache[tag] = res
        return res

    def diffs(a, b, lo=0):
        fa, fb = a['rows'], b['rows']
        return [f for f in sorted(fa) if f >= lo and (f not in fb or fa[f][0] != fb[f][0])]

    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        fails += not ok

    thin_args = ['--thin', cls, '--thin-lag', str(args.lag), '--thin-k', str(args.k)]
    try:
        ours = arm('ours', [])
        bad_ref = ours['rows'] and all(h == '0' for h, _, _ in ours['rows'].values())
        verdict(len(ours['rows']) == n and not bad_ref and ours['replaced'] > 0,
                'ours (the reference, ' + ours_flag + '): %d frames, hook replaced %d, kept %d' % (len(ours['rows']), ours['replaced'], ours['kept']))
        if want('thin'):
            t = arm('thin', thin_args)
            th = t['thin']
            if not th:
                verdict(False, 'thin: no [rdr][thin] census line in the output')
            else:
                d = diffs(t, ours)
                verdict(not d and len(t['rows']) == n, 'thin arm: the picture of all %d frames byte-equal to the ours arm (%d differ%s)' % (len(t['rows']), len(d), (': ' + str(d[:8])) if d else ''))
                verdict(th['thin'] > 0, 'thin engaged: %d draws drawn from a template, %d proof compares (%d ok), %d learned, %d pinned at the end' % (th['thin'], th['proof'], th['proof_ok'], th['learned'], th['pinned']))
                verdict(th['holes'] == 0 and th['epoch_drops'] == 0 and th['slot_missing'] == 0, 'holes %d, epoch drops %d, slot missing %d (all required 0)' % (th['holes'], th['epoch_drops'], th['slot_missing']))
                verdict(th['mism'] == 0, 'proof mismatches %d (program %d layout %d slot image %d slot view %d levels %d geometry %d counts %d); tween key moved %d (info)' % (
                    th['mism'], th['d_prog'], th['d_layout'], th['d_image'], th['d_view'], th['d_levels'], th['d_geom'], th['d_counts'], th['blend_moved']))
                tot = th['thin'] + t['replaced']
                if cls == 'wrestlers':
                    # lane 129 (KC): a thin wrestler draw uses the STREAM's palette rows, so a draw the hook refuses on the full path for a palette that differs from the record (Brock's nodes 59+ in this
                    # capture, WRESTLER map 204) is drawn right by its pinned template (the picture is byte-equal to the reference, which draws that stream draw itself). The census therefore
                    # reads: the reference's replaced draws are all covered, and the surplus is at most the reference's palette refusals.
                    verdict(0 <= tot - ours['replaced'] <= ours['palette_kept'], 'census: thin %d + the hook\'s full-path replaced %d = %d; the reference replaced %d (surplus %d <= its palette refusals %d)' % (
                        th['thin'], t['replaced'], tot, ours['replaced'], tot - ours['replaced'], ours['palette_kept']))
                else:
                    verdict(tot == ours['replaced'], 'census: thin %d + the hook\'s full-path replaced %d = %d; the reference replaced %d' % (th['thin'], t['replaced'], tot, ours['replaced']))
                # review F7: the surplus joined draw by draw. The thin arm's class-ours ops that the reference did NOT draw as class-ours, and why the reference's hooks kept them.
                fate = FATE[cls]
                surplus = [k for k, v in t['trace'].items() if v[0] == fate and ours['trace'].get(k, (0, 0))[0] != fate]
                why = {}
                for k in surplus:
                    why[ours['trace'].get(k, (0, 0))[1]] = why.get(ours['trace'].get(k, (0, 0))[1], 0) + 1
                allowed = {WHY_PALETTE} if cls == 'wrestlers' else set()
                verdict(bool(t['trace']) and bool(ours['trace']) and set(why) <= allowed,
                        'surplus by draw: %d thin-drawn ops the reference did not draw as %s, by the reference\'s keep_why (KeepWhy+1): %s (allowed: %s)' % (len(surplus), cls, why or 'none', sorted(allowed) or 'none'))
                lost = [k for k, v in ours['trace'].items() if v[0] == fate and t['trace'].get(k, (0, 0))[0] != fate]
                print('INFO  reference class-ours ops the thin arm drew by the full path (proof, learn, unpinned): %d' % len(lost))
                for l in [l for l in t['text'].splitlines() if 'proof mismatch frame' in l][:12]:
                    print('INFO  ' + l)
                print('INFO  ' + [l for l in t['text'].splitlines() if '[rdr][thin] ' + cls + ' thin' in l][-1])
                print('INFO  thin share of the class: %d of %d replaced draws in the reference = %.1f %% (proof-frame / unlearned draws take the full path: %d)' % (
                    th['thin'], ours['replaced'], 100.0 * th['thin'] / max(1, ours['replaced']), t['replaced']))
        if want('churn'):
            c = arm('churn', thin_args + ['--thin-churn', '5'])
            th = c['thin']
            d = diffs(c, ours)
            verdict(th is not None and th['holes'] == 0 and not d and th['unpinned'] > 10,
                    'churn 5 (a pinned key unpinned every 5th frame): unpinned %s, released %s, holes %s, %d picture frames differ' % (th and th['unpinned'], th and th['released'], th and th['holes'], len(d)))
        if want('valid'):
            rc, text = run([exe, '--capture', cap, ours_flag, '--game', game, '--thin', cls, '--thin-run', os.path.join(out, 'arm_' + pre + 'valid'), '--frames', '12', '--validation', '1'], 900)
            verdict('validation errors 0' in text and 'validation active: yes' in text and rc == 0, 'validation: 12 thin frames with the layer on, errors 0 (exit %d)' % rc)
            if rc:
                print(text[-1500:])

        if want('vswap'):  # review F4: from frame 150 some model parts' stream vertex buffer is another one of the same size (same index buffer, same images); both arms see the same stream
            vs_ref = arm('vs_ref', ['--thin-vertex-swap', '150'])
            vs_thin = arm('vs_thin', thin_args + ['--thin-vertex-swap', '150'])
            d = diffs(vs_thin, vs_ref)
            th = vs_thin['thin']
            stale = [k for k, v in vs_thin['trace'].items() if len(v) >= 4 and v[2] == 1 and v[3] == 1]   # a swapped draw drawn FROM A TEMPLATE = a stale template
            lite_n = sum(1 for v in vs_thin['trace'].values() if len(v) >= 4 and v[2] == 1)
            # Every class, the arena included: the picture of all frames equals the swapped ours arm. (S101 thin vswap: the arena's 5 frames 151-155 were the offline content check dropping the join
            # without marking the failing blob soft, arena_ours_core.cpp; fixed, plant content_no_soft below. THIN_DRAWS.md "The arena vertex_swap difference".)
            ok_pic = not d
            verdict(th is not None and ok_pic and th["holes"] == 0 and th["thin"] > 0,
                    'vertex_swap (frames 150+): swapped draws drawn from a template %d (info: a key the hook accepted after the swap is a template of the new key) of %d template draws; picture frames differing from the swapped ours arm %d%s; holes %s' % (
                        len(stale), lite_n, len(d), (' ' + str(d[:8])) if d else '', th and th['holes']))
            vs_bad = arm('vs_nofact', thin_args + ['--thin-fault', 'no_vb_fact', '--thin-vertex-swap', '150'])
            dn = diffs(vs_bad, vs_ref)
            verdict(len(dn) > 0, 'planted no_vb_fact + vertex_swap (the key without the vertex identity): %d picture frames differ from the swapped ours arm (bar > 0)' % len(dn))
            if cls == 'arena':   # S101 thin vswap: R, the arena vertex_swap bug re-created (the content check's failure leaves the blob free to re-join the object alone while its siblings are lite)
                vs_soft = arm('vs_content_no_soft', thin_args + ['--replace-fault', 'content_no_soft', '--thin-vertex-swap', '150'])
                ds = diffs(vs_soft, vs_ref)
                verdict(len(ds) > 0, 'planted content_no_soft + vertex_swap (R: the offline content check drops the join without a soft blob): %d picture frames differ from the swapped ours arm %s (bar > 0)' % (len(ds), ds[:8]))

        # ---- planted faults: each must be DETECTED ------------------------------------------------------------------------------------------------------------------
        def plant(name, extra, detect):
            if not want(name):
                return
            p = arm('p_' + name, thin_args + ['--thin-fault', name] + extra)
            ok, why = detect(p)
            verdict(ok, 'planted %s: %s' % (name, why))

        def ch(p):
            th = p['thin']
            d = diffs(p, ours)
            return th, d

        def det_release(p):
            th, _ = ch(p)
            return bool(th and th['holes'] > 0), 'holes %s (bar > 0, the honest arm has 0)' % (th and th['holes'])

        def det_mismatch_or_pic(p):
            th, d = ch(p)
            return bool(th and (th['mism'] > 0 or d)), 'proof mismatches %s (slot image %s, levels %s), %d picture frames differ' % (th and th['mism'], th and th['d_image'], th and th['d_levels'], len(d))

        def det_levels(p):
            th, _ = ch(p)
            return bool(th and th['mism'] > 0 and th['d_levels'] > 0), 'proof mismatches %s, of them levels %s (bar > 0)' % (th and th['mism'], th and th['d_levels'])

        def det_pic(p):
            th, d = ch(p)
            return bool(d), '%d picture frames differ from the ours arm (bar > 0); holes %s' % (len(d), th and th['holes'])

        def det_census_or_pic(p):  # lane 129: cloth_pinned draws a part the hook kept: the picture moves and / or thin + replaced exceeds the reference
            th, d = ch(p)
            over = bool(th and th['thin'] + p['replaced'] - ours['replaced'] > ours['palette_kept'])
            return bool(d or over), '%d picture frames differ; thin %s + replaced %s = %s against the reference %s' % (len(d), th and th['thin'], p['replaced'], th and th['thin'] + p['replaced'], ours['replaced'])

        plant('release_without_ack', ['--thin-churn', '5'], det_release)
        plant('read_stream', [], det_pic)

        def det_nothin(p):  # review F1: the table gated narrower than the query: the keys never match, thin falls to ~0 and / or holes appear
            th, d = ch(p)
            return bool(th and (th['holes'] > 0 or th['thin'] < 0.5 * (cache['thin']['thin'] if 'thin' in cache and cache['thin']['thin'] else 1) or d)), 'thin %s, holes %s, %d picture frames differ' % (th and th['thin'], th and th['holes'], len(d))
        if cls != 'arena':   # (the arena's own switch is the narrow gate: the plant changes nothing there)
            plant('facts_table_gate', [], det_nothin)
        if cls == 'arena':
            plant('no_program_key', [], det_mismatch_or_pic)
            plant('first_template', [], det_mismatch_or_pic)
            plant('wrong_site_template', [], det_mismatch_or_pic)
            plant('all_mips', [], det_levels)
        if cls == 'ring':  # lane 129 (KC)
            plant('rope_colour_instance0', [], det_mismatch_or_pic)   # (a comparator check: Check B's ropes share a colour)
            plant('rope_colour_visible', [], det_pic)                  # (the rope's slot 0 takes another ring part's texture: the picture shows it)
            plant('skirt_tie', [], det_mismatch_or_pic)
        if cls == 'wrestlers':
            plant('template_rows', [], det_pic)
            plant('cloth_pinned', [], det_census_or_pic)   # WEAK: 2 frames differ; the strong wrestler plants are template_rows and vertex_swap


        if want('no_ordinal'):  # reported, not judged: does Check B have repeated (site, object, part, program) triples at all?
            p = arm('p_no_ordinal', thin_args + ['--thin-fault', 'no_ordinal'])
            th, d = ch(p)
            print('INFO  no_ordinal (every ordinal 0): proof mismatches %s, %d picture frames differ -> Check B %s repeated keys whose templates differ' % (th and th['mism'], len(d), 'HAS' if (th and th['mism']) or d else 'has NO'))
        if want('epoch'):  # review 4: the arena "changes" at frame 120 (every template void): lite draws of the frames in flight are epoch drops, never holes; the picture heals
            e = arm('epoch', thin_args + ['--thin-epoch-at', '120'])
            th, d = ch(e)
            verdict(th is not None and th['holes'] == 0 and th['epoch_drops'] > 0 and (n - 1) not in d,
                    'epoch_mid_run (templates cleared at frame 120): holes %s, epoch drops %s (bar > 0), %d picture frames differ (all around the change: %s), the last frame right' % (th and th['holes'], th and th['epoch_drops'], len(d), d[:6]))
        if want('isolate') and cls == 'arena':   # (the isolate name filter 'yBG' is an arena object name)  # review 3: isolate binds are read-only for the thin store and keep the lite draws
            iso_ref = arm('iso_ref', ['--thin-isolate', 'yBG', '--thin-isolate-from', '200'])
            iso = arm('iso', thin_args + ['--thin-isolate', 'yBG', '--thin-isolate-from', '200'])
            d = diffs(iso, iso_ref)
            th = iso['thin']
            verdict(th is not None and th['holes'] == 0 and not d and th['thin'] > 0, 'isolate_bind (frames 200+ bound as isolate binds): holes %s, thin %s, %d picture frames differ from the isolated ours arm' % (th and th['holes'], th and th['thin'], len(d)))
        if want('no_proof') and cls == 'arena':
            ref = arm('swap_ref', ['--thin-swap-slot', str(SWAP)])
            noproof = arm('swap_noproof', thin_args + ['--thin-fault', 'no_proof', '--thin-swap-slot', str(SWAP)])
            proof = arm('swap_proof', thin_args + ['--thin-swap-slot', str(SWAP)])
            dn = diffs(noproof, ref, SWAP)
            dp = diffs(proof, ref, SWAP)
            verdict(len(dn) > 0 and (n - 1) in dn, 'planted no_proof + a slot swapped mid-capture (frame %d): the picture differs from the swapped reference on %d frames from the swap, still on the last frame' % (SWAP, len(dn)))
            kk = 32 if args.k < 0 else args.k
            # A key the swap did not reach while it was drawn (off screen) keeps its stale template until its first proof frame after it comes back: wrong pixels for at most K frames of USE.
            # So the bar is per run of consecutive wrong frames (<= K + lag + 2), a healed last frame, and far fewer wrong frames than the no-proof arm.
            runs, cur = [], 0
            for f in range(SWAP, n):
                if f in dp:
                    cur += 1
                elif cur:
                    runs.append(cur)
                    cur = 0
            if cur:
                runs.append(cur)
            tp = proof['thin']
            ok = tp is not None and tp['holes'] == 0 and (n - 1) not in dp and all(r <= kk + args.lag + 2 for r in runs) and len(dp) * 3 < len(dn) and (tp['unpinned'] > 0 or tp['d_image'] > 0)   # lane 142: the swapped slot's picture is now the stream's image (the draw stays ours), so the proof frame's mismatch (not only an unpin) is the detection
            verdict(ok, 'the same swap WITH proof frames (K=%d): wrong frames %d of %d in runs %s (bar: each run <= %d, the last frame right, fewer than a third of the no-proof arm: %d); holes %s, unpinned %s, proof mismatches %s (slot image %s)' % (
                kk, len(dp), n - SWAP, runs, kk + args.lag + 2, len(dn), tp and tp['holes'], tp and tp['unpinned'], tp and tp['mism'], tp and tp['d_image']))
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        return 1
    print('thin_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

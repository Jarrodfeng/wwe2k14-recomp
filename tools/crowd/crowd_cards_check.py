#!/usr/bin/env python3
"""Lane CD (S101) acceptance test, offline, 0 game runs: the stands' far cards drawn by us, one instanced draw per person type (fview --crowd-ours --crowd-cards).

  py -I tools\\crowd\\crowd_cards_check.py [--root DIR] [--no-build] [--leftovers]

Material (the MAIN checkout's out\\ and game_data\\): Check B = run 20261111_215201 (341 frames, scene records), Check F = run 20261112_101633 (337 frames). A missing input FAILS.
Arms: A = fview --crowd-ours, B = A + --crowd-cards, each one replay of the whole capture (--replay --validation 1 --hash-frames: the FNV hash of the presented surface of every frame).
Bar (docs\\renderer\\CROWD_CARDS.md section 4):
  1. picture: B's hash equals A's on EVERY frame (byte-equal), Check B and Check F.
  2. census: cards drawn == the stream's card draws == the binder's dropped card draws == the record's cards; kept 0, unaccounted 0, unjoined 0, picture mismatch 0, holes 0, bad frames 0 (only the first
     frame, which has no record, falls back); instanced draws <= 20 a frame; the alpha-test twin in every draw; validation errors 0.
  3. planted faults (Check B), each must fail its NAMED check and, with --crowd-cards-unchecked, move the picture (the picture plants) or be the stated control:
       cards_one_picture (lane 35's one texture for every member), cards_type_shift, cards_atlas_picture   -> picture check (mismatches, bad-picture frames)
       cards_order_swap                                                                                   -> order check (inversions)
       offset (record j instead of j-1: lane 36's +1 pairing, real past bug), cards_missing_card           -> pairing check (unpaired / unaccounted)
       cards_drop_unjoined                                                                                -> holes
       cards_no_alpha_test (S100's alpha_test false)                                                      -> alpha-test twin count
       cards_uv_swapped, cards_copy_swap_lost, cards_as_bodies (lane 35's first design)                   -> picture vs the stream (B's hash differs)
     twins that must PASS: cards_unjoin_type (a type unjoined and kept the game's: kept == unjoined cards, holes 0, frame 200 within 12 levels of A: the kept cards are drawn after ours, so only overlapping alpha-edge texels move), cards_no_cards (the record holds no cards: every frame stays the stream's, "no cards" counted);
     control: cards_order_reversed (checked: order check; unchecked: how many frames move is reported).
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BROCK = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
CHECK_F = 'out/s101/runs/20261112_101633_s100_owner_s89_brock/case_20261112_101714_809_0_pc_run_exit.zip'
# Lane 219 (S101): tonight's sweep captures whose stands cards fell back on "no light-map page copy" (slot 13 unbound: the page is not written, so no copy order to keep). 121 and 44 card frames.
MATCH_ZIPS = [('M1', 'out/s101/runs/20261113_062508_s101_sweep_match_type/case_20261113_062627_236_0_pc_run_exit.zip'), ('M2', 'out/s101/runs/20261113_062819_s101_sweep_match_type/case_20261113_062908_185_0_pc_run_exit.zip'), ('M3', 'out/s101/runs/20261113_093119_s101_sweep_match_type/case_20261113_093212_244_0_pc_run_exit.zip')]

CARDS_RE = re.compile(r'cards: frames asked (\d+), ours (\d+), fallback (\d+) \(no record (\d+), no cards (\d+), bad pairing (\d+), bad order (\d+), bad picture (\d+), bad state (\d+), other (\d+)\); '
                      r'record cards (\d+), stream card draws (\d+), cards drawn (\d+) in (\d+) instanced draws, kept (\d+) \(unaccounted (\d+), unjoined type (\d+) types / (\d+) cards, picture mismatch (\d+)\), '
                      r'types joined by bake position (\d+), density left out (\d+), record cards without a stream draw (\d+), order inversions (\d+), types with the record in another order than the stream (\d+), holes (\d+), '
                      r'alpha-test twin built in (\d+) of (\d+) draws that ask for it \((\d+) draws\)')
CARDS_KEYS = ['asked', 'ours', 'fallback', 'no_record', 'no_cards', 'bad_pairing', 'bad_order', 'bad_picture', 'bad_state', 'other', 'record', 'stream', 'drawn', 'draws', 'kept', 'unaccounted', 'unjoined_types',
              'unjoined_cards', 'mismatch', 'by_position', 'density', 'unpaired', 'inversions', 'record_order', 'holes', 'alpha_twin', 'alpha_wanted', 'alpha_of']
BINDER_RE = re.compile(r'\[rdr\]\[crowd\] cards: frames ours (\d+), fallback (\d+), card draws dropped (\d+), cards drawn (\d+), kept (\d+)')
MEMBERS_RE = re.compile(r'crowd ours: \d+ frames, (\d+) members \((\d+) cards\)')
VALID_RE = re.compile(r'validation errors (\d+)')


def fail_exit(msg):
    print('FAIL  ' + msg)
    return 1


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                a, b = line.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


class Arm:
    scale = []  # --scale N for every replay arm (the gate at scale 2: --scale 2)

    def __init__(self, exe, zip_path, game, work, name, extra):
        self.name = name
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--crowd-ours', '--game', game, '--hash-frames', hashes] + Arm.scale + extra
        for attempt in range(12):  # the GPU is shared: retry ONLY the exact "Not enough graphics memory" start-up refusal (logged), never another failure
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
            self.rc, self.text = p.returncode, (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            print('NOTE  arm %s: "Not enough graphics memory for the frame ring" (shared GPU), attempt %d of 12' % (name, attempt + 1), flush=True)
            time.sleep(20)
        if not os.path.isfile(hashes):
            print(self.text[-2500:])
            raise RuntimeError('arm %s wrote no hashes (exit %d)' % (name, self.rc))
        self.hashes = read_hashes(hashes)
        m = CARDS_RE.search(self.text)
        self.cards = dict(zip(CARDS_KEYS, map(int, m.groups()))) if m else None
        b = BINDER_RE.search(self.text)
        self.binder = dict(zip(['ours', 'fallback', 'dropped', 'drawn', 'kept'], map(int, b.groups()))) if b else None
        v = VALID_RE.findall(self.text)
        self.valid = int(v[-1]) if v else -1
        mm = MEMBERS_RE.search(self.text)
        self.members, self.card_members = (int(mm.group(1)), int(mm.group(2))) if mm else (0, 0)

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


class Result:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS  ' if ok else 'FAIL  ') + text)
        self.fails += 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--match-only', action='store_true', help='lane 219: only the two sweep match zips (pictures + frames), no Check B / F arms and no planted faults')
    ap.add_argument('--exe', default='', help='fview.exe to use (default out/renderer/dev/fview.exe of --root)')
    ap.add_argument('--scale', default='1', help='render scale of the replay arms (the review asked for the gate once at 2)')
    ap.add_argument('--leftovers', action='store_true', help='also re-run the leftovers census on Check B frame 200 with --promote auto,cards (slow)')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    zip_b, zip_f = os.path.join(main_co, BROCK), os.path.join(main_co, CHECK_F)
    for need in (zip_b, zip_f, game):
        if not os.path.exists(need):
            return fail_exit('input missing: ' + need)
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            return fail_exit('fview build (exit %d)' % p.returncode)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(prefer=args.exe, root=root, kind='dev')   # lane 219's --exe goes through the same staleness check
    if not os.path.isfile(exe):
        return fail_exit('no fview.exe at ' + exe)
    if args.scale != '1':
        Arm.scale = ['--scale', args.scale]
    work = os.path.join(root, 'out', 'crowd_cards_check' + ('' if args.scale == '1' else '_x' + args.scale))
    os.makedirs(work, exist_ok=True)
    R = Result()
    try:
        arms = {}
        for label, z in (() if args.match_only else (('B', zip_b), ('F', zip_f))):
            a = Arm(exe, z, game, work, 'A_' + label, [])
            b = Arm(exe, z, game, work, 'cards_' + label, ['--crowd-cards'])
            arms[label] = (a, b)
            n = len(a.hashes) - 1  # (the 'format' line is not a frame)
            bad = [k for k in a.hashes if a.hashes[k] != b.hashes.get(k)]  # (the format line included)
            R.line(n > 300 and not bad, 'picture, Check %s: %d of %d frames have the same presented-surface hash with --crowd-cards as without (byte-equal)%s' % (label, n - len(bad), n, '' if not bad else ', first differing frames %s' % bad[:8]))
            c, bd = b.cards, b.binder
            if not c or not bd:
                print(b.text[-3000:])
                return fail_exit('no cards census line in the cards arm (Check %s)' % label)
            fast = c['ours'] == c['asked'] - 1 and c['no_record'] == 1 and c['fallback'] == 1 and c['bad_pairing'] + c['bad_order'] + c['bad_picture'] + c['bad_state'] + c['other'] + c['no_cards'] == 0
            R.line(fast, 'frames, Check %s: %d asked, %d ours, %d fallback = the first frame (no record before it); bad pairing / order / picture / state / other / no-cards frames 0' % (label, c['asked'], c['ours'], c['fallback']))
            ok = c['drawn'] == c['stream'] == c['record'] == bd['dropped'] == bd['drawn'] and c['drawn'] > 100000
            R.line(ok, 'cards, Check %s: cards drawn %d == the stream\'s card draws %d == the record\'s cards %d == the binder\'s dropped card draws %d' % (label, c['drawn'], c['stream'], c['record'], bd['dropped']))
            ok = c['kept'] == c['unaccounted'] == c['unjoined_cards'] == c['mismatch'] == c['unpaired'] == c['inversions'] == c['holes'] == 0 and bd['kept'] == 0
            R.line(ok, 'no hole, Check %s: kept %d, unaccounted %d, unjoined %d, picture mismatch %d, record cards without a stream draw %d, order inversions %d, holes %d' %
                   (label, c['kept'], c['unaccounted'], c['unjoined_cards'], c['mismatch'], c['unpaired'], c['inversions'], c['holes']))
            ok = 0 < c['draws'] <= 20 * c['ours'] and c['alpha_twin'] == c['alpha_wanted'] == c['alpha_of'] == c['draws']
            R.line(ok, 'draws, Check %s: %d instanced draws for %d cards over %d frames (%.1f a frame, at most one per person type = 20); the alpha-test twin AS BUILT in %d of the %d draws that ask for it' %
                   (label, c['draws'], c['drawn'], c['ours'], c['draws'] / max(1, c['ours']), c['alpha_twin'], c['alpha_wanted']))
            fa, fb = a.hashes.get('format'), b.hashes.get('format')
            R.line(fa in ('1', '3') and fb in ('1', '3'), 'present surface, Check %s: storage format %s / %s (rdr::Format 1 = RGBA8, 3 = BGRA8: 8-bit, so the read-back hash keeps every bit the picture has, alpha included)' % (label, fa, fb))
            R.line(a.valid == 0 and b.valid == 0 and a.rc == 0 and b.rc == 0, 'validation, Check %s: errors %d (A) / %d (cards)' % (label, a.valid, b.valid))

        for label, rel in MATCH_ZIPS:   # lane 219
            z = os.path.join(main_co, rel)
            if not os.path.exists(z):
                return fail_exit('input missing: ' + z)
            a = Arm(exe, z, game, work, 'A_' + label, [])
            b = Arm(exe, z, game, work, 'cards_' + label, ['--crowd-cards'])
            bad = [k for k in a.hashes if a.hashes[k] != b.hashes.get(k)]
            c = b.cards
            R.line(len(a.hashes) > 100 and not bad, 'picture, %s: %d frames, byte-equal with --crowd-cards (first differing %s)' % (label, len(a.hashes) - 1, bad[:6]))
            R.line(bool(c) and c['asked'] > 20 and c['ours'] == c['asked'] - c['no_record'] and c['bad_state'] + c['bad_pairing'] + c['bad_order'] + c['bad_picture'] + c['other'] == 0 and c['kept'] == c['holes'] == c['mismatch'] == 0 and c['drawn'] == c['stream'] == c['record'],
                   'frames, %s: asked %s, ours %s, fallback only the no-record frames; bad state / pairing / order / picture 0 (before lane 219: all bad state), drawn == stream == record cards' % (label, c and c['asked'], c and c['ours']))

        # Lane 223: frames WITHOUT a bake (the live arena-44 run showed 194 of 364) and frames with only SOME bakes: emulate both on M1 with bake_skip_cut.py (the game's own skip span removed from the ops):
        # 'alt' = every other frame loses all 20 bake passes, 'half' = every frame loses the bakes of types 0-9. The cards of such a frame read the pictures the last bake wrote, in both arms, so the picture must
        # stay byte-equal and every card must be ours (a type's picture = the one its own stream cards show); a picture plant (type_shift) on the 'alt' zip must still be caught.
        for cut_name, cut_mask, cut_even_only in (('alt', 0xFFFFF, True), ('half', 0x003FF, False)):
            cut_zip = os.path.join(work, 'm1_cut_%s.zip' % cut_name)
            dec = os.path.join(work, 'dec_%s.csv' % cut_name)
            with open(dec, 'w') as f:
                f.writelines('%d,%d\n' % (fr, cut_mask if (fr % 2 == 0 or not cut_even_only) else 0) for fr in range(0, 400))
            p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'crowd', 'bake_skip_cut.py'), os.path.join(main_co, MATCH_ZIPS[0][1]), dec, cut_zip], capture_output=True, text=True, timeout=1500)
            if p.returncode or not os.path.isfile(cut_zip):
                print((p.stdout + p.stderr)[-2000:])
                return fail_exit('bake_skip_cut.py failed (exit %d)' % p.returncode)
            try:
                a = Arm(exe, cut_zip, game, work, 'A_cut_' + cut_name, [])
                b = Arm(exe, cut_zip, game, work, 'cards_cut_' + cut_name, ['--crowd-cards'])
                shift = Arm(exe, cut_zip, game, work, 'shift_cut_' + cut_name, ['--crowd-cards', '--crowd-fault', 'cards_type_shift']) if cut_name == 'alt' else None
            finally:
                os.remove(cut_zip)
            bad = [k for k in a.hashes if a.hashes[k] != b.hashes.get(k)]
            c = b.cards
            mm = re.search(r"picture is the stream's own \(no bake join\) (\d+); frames with a bake (\d+), without (\d+)", b.text)
            stream_joined, with_bake, no_bake = map(int, mm.groups()) if mm else (0, 0, 0)
            R.line(len(a.hashes) > 100 and not bad, "picture, M1 cut '%s': %d frames, byte-equal with --crowd-cards (first differing %s)" % (cut_name, len(a.hashes) - 1, bad[:6]))
            want_joined = 20 * no_bake if cut_name == 'alt' else 10 * (with_bake + no_bake)
            R.line(bool(c) and (no_bake > 20 if cut_name == 'alt' else with_bake > 100) and c['ours'] == c['asked'] - c['no_record'] and c['bad_state'] + c['bad_pairing'] + c['bad_order'] + c['bad_picture'] + c['other'] == 0 and c['kept'] == c['holes'] == c['unjoined_cards'] == c['mismatch'] == 0 and stream_joined >= want_joined,
                   "frames, M1 cut '%s': %d frames with a bake scan, %d without any bake; asked %s, ours %s (never half ours); %d type-frames took their picture from the stream's cards (>= %d); bad state 0, kept 0 (before lane 223 every bake-less frame was bad state)" % (cut_name, with_bake, no_bake, c and c['asked'], c and c['ours'], stream_joined, want_joined))
            if shift:
                sc = shift.cards
                R.line(bool(sc) and sc['mismatch'] > 0 and sc['bad_picture'] > 0, "plant type_shift on the bake-cut zip: the stream-joined pictures shifted by one type are caught (picture mismatches %s, bad-picture frames %s)" % (sc and sc['mismatch'], sc and sc['bad_picture']))

        if args.match_only:
            print('%d check(s) failed' % R.fails if R.fails else 'ALL PASS (match zips only)')
            return 1 if R.fails else 0
        # ---- planted faults and twins (Check B)
        A, B = arms['B']
        results = []

        def plant(name, fault, named, expect_fail=True, moves_unchecked=None, picture_named=False):
            fa = ['--crowd-cards', '--crowd-fault', fault] if fault != 'offset' else ['--crowd-cards', '--crowd-fault', 'offset']
            a = Arm(exe, zip_b, game, work, 'plant_' + name, fa)
            c = a.cards
            if not c and fault != 'cards_as_bodies':
                print(a.text[-2000:])
                raise RuntimeError('no cards census in plant ' + name)
            failed_named = named(a)
            moved = a.differing(A)
            if picture_named:
                failed_named = moved > 0
            note = ''
            if moves_unchecked is not None:
                u = Arm(exe, zip_b, game, work, 'plant_' + name + '_unchecked', fa + ['--crowd-cards-unchecked'] if fault != 'cards_as_bodies' else fa)
                um = u.differing(A)
                note = ' with the checks off the picture moves on %d frames' % um
                if moves_unchecked and um == 0:
                    failed_named = False
                    note += ' (EXPECTED to move)'
            if expect_fail:
                R.line(failed_named, 'plant %s: fails its named check (%s)%s' % (name, named.__doc__ or 'picture vs the stream', note))
            else:
                R.line(not failed_named and moved == 0, 'twin %s: passes (%s); picture differs from the stream\'s on %d frames' % (name, named.__doc__ or '', moved))
            return a

        def picture(a): "picture: mismatching cards and bad-picture frames counted"; return a.cards['mismatch'] > 0 and a.cards['bad_picture'] > 0
        def order(a): "order: inversions and bad-order frames counted"; return a.cards['inversions'] > 0 and a.cards['bad_order'] > 0
        def pairing(a): "pairing: unaccounted stream draws / unpaired record cards and bad-pairing frames counted"; return (a.cards['unaccounted'] > 0 or a.cards['unpaired'] > 0) and a.cards['bad_pairing'] > 0
        def holes(a): "holes: stream card draws neither drawn nor kept"; return a.cards['holes'] > 0
        def alpha(a): "alpha-test twin as built: fewer draws with the twin than draws that ask for it, and the picture moves"; return a.cards['alpha_twin'] < a.cards['alpha_wanted'] and a.differing(A) > 0
        def state(a): "state: a card draw's program / state / constants / blob / pass order differs from the first, bad-state frames counted"; return a.cards['bad_state'] > 0
        def moved_pic(a): "picture vs the stream: frames whose hash differs"; return a.differing(A) > 0
        def clean_unjoin(a): "an unjoined type is kept"; return bool(a.cards['holes'] or a.cards['unjoined_cards'] == 0)
        def no_cards(a): "the record holds no cards: every frame falls back, counted as no cards"; return not (a.cards['no_cards'] >= a.cards['asked'] - 1 and a.cards['drawn'] == 0)
        def bodies(a): "cards drawn as bodies: card members counted, picture moves"; return a.card_members > 0 and a.differing(A) > 0

        plant('one_picture', 'cards_one_picture', picture, moves_unchecked=True)
        plant('type_shift', 'cards_type_shift', picture, moves_unchecked=True)
        plant('atlas_picture', 'cards_atlas_picture', picture, moves_unchecked=True)
        plant('order_swap', 'cards_order_swap', order, moves_unchecked=False)
        plant('offset', 'offset', pairing, moves_unchecked=False)
        plant('missing_card', 'cards_missing_card', pairing, moves_unchecked=False)
        plant('drop_unjoined', 'cards_drop_unjoined', holes, moves_unchecked=True)
        plant('no_alpha_test', 'cards_no_alpha_test', alpha, moves_unchecked=None)
        plant('blob_drift', 'cards_blob_drift', state, moves_unchecked=False)
        plant('state_drift', 'cards_state_drift', state, moves_unchecked=False)
        plant('inject_early', 'cards_inject_early', state, moves_unchecked=False)
        plant('card_gap', 'cards_gap', state, moves_unchecked=False)   # lane 223: a non-card op (a picture rewrite) inside the cards must still refuse the frame (contiguity)
        plant('position_reversed', 'cards_position_reversed', picture, moves_unchecked=True)
        plant('uv_swapped', 'cards_uv_swapped', moved_pic, picture_named=True)
        plant('copy_swap_lost', 'cards_copy_swap_lost', moved_pic, picture_named=True)
        a = Arm(exe, zip_b, game, work, 'plant_cards_as_bodies', ['--crowd-cards', '--crowd-fault', 'cards_as_bodies'])
        R.line(bodies(a), 'plant cards_as_bodies (lane 35\'s first design; stays a refused plant): %d card members drawn as bodies, the picture moves on %d frames' % (a.card_members, a.differing(A)))
        a = Arm(exe, zip_b, game, work, 'twin_unjoin_type', ['--crowd-cards', '--crowd-fault', 'cards_unjoin_type'])
        c = a.cards
        R.line(c['holes'] == 0 and c['unjoined_cards'] > 0 and c['kept'] == c['unjoined_cards'] and a.binder['kept'] == c['kept'] and c['unaccounted'] == 0 and c['bad_pairing'] + c['bad_picture'] + c['bad_order'] + c['bad_state'] == 0,
               'twin unjoin_type: a type unjoined every frame (%d cards) stays the game\'s: kept %d == unjoined cards, holes %d, bad frames 0; drawn %d + kept %d == the stream\'s %d' % (c['unjoined_cards'], c['kept'], c['holes'], c['drawn'], c['kept'], c['stream']))
        try:
            import numpy as np
            from PIL import Image
        except Exception as e:
            return fail_exit('numpy / Pillow are needed to read the pictures: %s' % e)
        pics = {}
        injected = {}
        for name, extra in (('A', []), ('clean', ['--crowd-cards']), ('unjoin', ['--crowd-cards', '--crowd-fault', 'cards_unjoin_type']), ('keep_first', ['--crowd-cards', '--crowd-fault', 'cards_keep_first'])):
            d = os.path.join(work, 'f200_' + name)
            js = os.path.join(work, 'ops_f200_' + name + '.json')
            for attempt in range(12):  # (shared GPU: only the exact start-up refusal is retried)
                p = subprocess.run([exe, '--case', zip_b, '--frame', '200', '--judge-out', d, '--crowd-ours', '--game', game] + extra, capture_output=True, text=True, timeout=900, errors='replace')
                if 'Not enough graphics memory for the frame ring' not in (p.stdout or '') + (p.stderr or ''):
                    break
                time.sleep(20)
            png = os.path.join(d, 'picture.png')
            if p.returncode or not os.path.isfile(png):
                return fail_exit('frame 200 picture arm %s failed (exit %d)' % (name, p.returncode))
            for attempt in range(12):
                p = subprocess.run([exe, '--case', zip_b, '--frame', '200', '--ops', '--json', js, '--crowd-ours', '--game', game] + extra, capture_output=True, text=True, timeout=900, errors='replace')
                if 'Not enough graphics memory for the frame ring' not in (p.stdout or '') + (p.stderr or ''):
                    break
                time.sleep(20)
            pics[name] = np.asarray(Image.open(png).convert('RGB'), dtype=np.int32)
            injected[name] = open(js, encoding='utf-8').read().count('"crowd_injected"') if os.path.isfile(js) else -1
        diff = np.abs(pics['A'] - pics['unjoin']).max(axis=2)
        R.line(int((diff > 12).sum()) == 0, 'twin unjoin_type, frame 200: %d of %d pixels differ from the stream\'s, %d by more than 1 level, %d by more than 12 (max %d): only overlapping alpha-edge texels of the kept cards, which are drawn after ours' %
               (int((diff > 0).sum()), diff.size, int((diff > 1).sum()), int((diff > 12).sum()), int(diff.max())))
        kd = np.abs(pics['A'] - pics['keep_first']).max(axis=2)
        R.line(int((kd > 12).sum()) == 0 and injected['clean'] == injected['keep_first'] and injected['clean'] >= 2,
               'plant keep_first, frame 200: the first card stays the game\'s, %d of %d pixels differ from the stream\'s, %d by more than 12 (alpha edges only); the op that ran InjectCards keeps an ours fate: %d crowd_injected ops (clean %d)' %
               (int((kd > 0).sum()), kd.size, int((kd > 12).sum()), injected['keep_first'], injected['clean']))
        plant('no_cards', 'cards_no_cards', no_cards, expect_fail=False)
        a = Arm(exe, zip_b, game, work, 'twin_position_all', ['--crowd-cards', '--crowd-fault', 'cards_position_all'])
        c = a.cards
        R.line(a.differing(A) == 0 and c['by_position'] >= 20 * c['ours'] and c['kept'] == 0 and c['holes'] == 0 and c['bad_picture'] == 0,
               'twin position_all: every type joined by its bake\'s position (%d type-frames) and the picture stays byte-equal to the stream\'s on every frame (differs on %d)' % (c['by_position'], a.differing(A)))
        da = Arm(exe, zip_b, game, work, 'density_A', ['--crowd-density', '50'])
        db = Arm(exe, zip_b, game, work, 'density_cards', ['--crowd-cards', '--crowd-density', '50'])
        c = db.cards
        R.line(db.differing(da) == 0 and c['density'] > 0 and c['holes'] == 0 and c['kept'] == 0 and c['bad_pairing'] + c['bad_picture'] + c['bad_state'] == 0,
               'density 50%%, both arms: the picture is byte-equal on every frame (differs on %d), density left out %d cards, holes %d, kept %d' % (db.differing(da), c['density'], c['holes'], c['kept']))
        ctl = Arm(exe, zip_b, game, work, 'control_order_reversed', ['--crowd-cards', '--crowd-fault', 'cards_order_reversed'])
        ctl_u = Arm(exe, zip_b, game, work, 'control_order_reversed_unchecked', ['--crowd-cards', '--crowd-fault', 'cards_order_reversed', '--crowd-cards-unchecked'])
        R.line(ctl.cards['inversions'] > 0 and ctl.cards['bad_order'] > 0, 'control order_reversed (checked): the order check counts %d inversions, %d bad-order frames; unchecked (every run reversed) the picture differs from the stream\'s on %d of %d frames (a control: only overlapping alpha-edge texels may move)' %
               (ctl.cards['inversions'], ctl.cards['bad_order'], ctl_u.differing(A), len(A.hashes) - 1))
    except RuntimeError as e:
        return fail_exit(str(e))

    if args.leftovers:
        cmd = [sys.executable, '-I', os.path.join(root, 'tools', 'census', 'leftovers.py'), zip_b, '--frames', '200', '--promote', 'auto,cards', '--name', 'cards_check_b200', '--fview', exe]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=3000, errors='replace')
        print((p.stdout or '')[-3500:])
        R.line(p.returncode == 0, 'leftovers.py on Check B frame 200 with --promote auto,cards ran (exit %d)' % p.returncode)
    print('%s: %d check(s) failed' % ('FAIL' if R.fails else 'PASS', R.fails))
    return 1 if R.fails else 0


if __name__ == '__main__':
    sys.exit(main())

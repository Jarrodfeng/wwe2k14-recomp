#!/usr/bin/env python3
"""S103 FOB: the game's draw order of the stage families, asserted on the captures (docs\\research\\s103\\XFO.md section 1, G1-G7). Offline, 0 game runs; reads captures and the player's own arena files.

For every named frame it takes the colour-pass draws (sites 0x82C0A950 / 0x82C0B94C / 0x82C0F4BC and the crowd's class 2) and names each one:
  canvas / skirt / post / turnbuckle / cover / rope   a ring piece (tools\\ring\\ring_census.py joins the capture's objects to the ring's entries; the address -> piece map comes from it)
  arena        a draw whose positions equal a stage slot's mesh (tools\\arena\\join_census.py match_draw), before the crowd;  arena_late = the same, after the crowd (the blended slot group)
  screens      pass site 0x82C0F4BC (sub_82C0CA08 model_pass_stage_screens_movie)
  other        any other colour-pass draw before the crowd (the arena's props, the wrestlers)
  crowd        origin class 2
Consecutive repeats collapse. The game's sequence must be exactly
  canvas skirt arena screens post turnbuckle cover rope other crowd arena_late
(G1 canvas + skirts, G2 the arena's slots, G3 screens, G4 posts / turnbuckles / covers / ropes, G5 props + wrestlers, G6 crowd, G7 the blended slot group) on every frame of every capture.
A planted twin (the stream's screens run and posts run permuted BEFORE naming) is run on brock_losswin f110 and must FAIL, or the check itself fails.
Not asserted: the slot order inside G2 (SOF's rule, tools\\scene\\c_sheet.py) and the lead group (XFO D5: its cause is not read).
  py -I tools\\arena\\family_order.py [--captures DIR] [--only NAME] [--no-plant]
Exit 0 = every frame is the game's order and the plant failed. Numbers only; nothing from the game is written."""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ring'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import arena_files as af  # noqa: E402
import join_census as jc  # noqa: E402
import ring_census as rc  # noqa: E402

SITE_COLOUR, SITE_MIRROR, SITE_SCREENS = 0x82C0A950, 0x82C0B94C, 0x82C0F4BC
GAME_ORDER = ['canvas', 'skirt', 'arena', 'screens', 'post', 'turnbuckle', 'cover', 'rope', 'other', 'crowd', 'arena_late']
FRAMES = (('brock_losswin', (60, 90, 110, 150, 200)), ('brock_losswin_rt', (100,)))
RING_TOKENS = {'canvas', 'skirt', 'post', 'turnbuckle', 'cover', 'rope'}


def frame_index(cap, number):
    for i in range(len(cap.frames)):
        if cap.frame_number(i) == number:
            return i
    raise jc.CensusError('%s: no frame %d' % (cap.path, number))


def run_bounds(keys, name):
    """The first contiguous run of draws with pre-key `name` -> (first, last) index, or None."""
    a = next((i for i, k in enumerate(keys) if k == name), None)
    if a is None:
        return None
    b = a
    while b + 1 < len(keys) and keys[b + 1] == name:
        b += 1
    return a, b


def sequence(path, number, plant=False):
    """-> (collapsed token list, arena id) for one frame. plant = permute the stream's screens run and its first posts run in the frame BEFORE naming (the order check must then fail)."""
    cap = jc.Capture(path)
    idx = frame_index(cap, number)
    R, arena_id, _ = rc.census(cap, None, [idx], None, False, {})
    piece_of = R.lists.get('object_piece', {})
    cap2 = jc.Capture(path)   # a fresh reader: the census advanced the first one's texture state
    f = cap2.frame(idx)
    if not f.has_origins:
        raise jc.CensusError('%s frame %d: no origins (a version-1 capture)' % (path, number))
    fi = jc.FileIndex(af.load_arena(arena_id))
    cache = {}
    toks = []
    in_crowd_or_later = False
    order = list(range(len(f.draws)))
    if plant:   # S103 FOB (review 7): swap two draw runs of the stream itself, found by site / ring object only (no naming yet)
        keys = []
        for i in order:
            o = f.origins[i]
            site, obj = int(o['pass_site']), hex(int(o['object']))
            keys.append('screens' if site == SITE_SCREENS else piece_of.get(obj) if site == SITE_COLOUR else None)
        sa, pb = run_bounds(keys, 'screens'), run_bounds(keys, 'post')
        if sa and pb and sa[1] < pb[0]:
            order = order[:sa[0]] + order[pb[0]:pb[1] + 1] + order[sa[1] + 1:pb[0]] + order[sa[0]:sa[1] + 1] + order[pb[1] + 1:]
    for i in order:
        o = f.origins[i]
        site, klass, obj = int(o['pass_site']), int(o['klass']), hex(int(o['object']))
        if klass == 2:
            tok = 'crowd'
            in_crowd_or_later = True
        elif site not in (SITE_COLOUR, SITE_MIRROR, SITE_SCREENS):
            continue   # the shadow / plane / bake passes, particles, post: not the colour pass's stage families
        elif site == SITE_SCREENS:
            tok = 'screens'
        elif obj in piece_of and site == SITE_COLOUR:
            tok = 'rope' if piece_of[obj].startswith('rope') else piece_of[obj]   # rope_high / rope_low: one family
            if tok == 'proxy':
                continue   # the rope plane-shadow proxy draws in its own passes
        else:
            r = jc.match_draw(cap2, f, i, fi, cache)
            if r['hits'] or r['scan']:
                tok = 'arena_late' if in_crowd_or_later else 'arena'
            elif in_crowd_or_later:
                continue   # after the crowd: particles and the like (not a family of this list)
            else:
                tok = 'other'
        if not toks or toks[-1] != tok:
            toks.append(tok)
    return toks, arena_id


HELD = ('a00_match', 'a04_match', 'a08_match', 'a09_match', 'a16_match', 'a30_match', 'a41_match', 'a44_match', 'a52_match', 'a54_match', 'a55_match', 'd02_match',
        'a00_entrance', 'a04_entrance', 'a08_entrance', 'a16_entrance', 'a30_entrance', 'a41_entrance', 'a44_entrance', 'a52_entrance', 'a54_entrance', 'a55_entrance', 'd02_entrance')


def pair_checks(t):
    """The cross-family claims scored on data the order was NOT read from: D1 canvas + skirts before every arena run, D2 the screens after the arena and before the ring's late pieces,
    D3 posts < turnbuckles < covers < ropes. -> {name: True / False / None (not in this frame)}."""
    def at(n): return [i for i, k in enumerate(t) if k == n]
    arena, early, scr = at('arena'), at('canvas') + at('skirt'), at('screens')
    late = [i for k in ('post', 'turnbuckle', 'cover', 'rope') for i in at(k)]
    res = {'D1': (max(early) < min(arena)) if early and arena else None,
           'D2': (max(arena) < min(scr) and max(scr) < min(late)) if scr and arena and late else None}
    seq = [at(k) for k in ('post', 'turnbuckle', 'cover', 'rope') if at(k)]
    res['D3'] = all(max(a) < min(b) for a, b in zip(seq, seq[1:])) if len(seq) > 1 else None
    return res


def held_out(captures):
    score = {'D1': [0, 0], 'D2': [0, 0], 'D3': [0, 0]}
    for name in HELD:
        path = os.path.join(captures, name)
        if not os.path.isdir(path):
            print('held-out  %s: capture missing' % name)
            continue
        try:
            toks, arena = sequence(path, 60)
        except (jc.CensusError, rc.CensusError, af.FileReadError, OSError, KeyError) as e:
            print('held-out  %s f60: not read (%s)' % (name, str(e)[:90]))
            continue
        pc = pair_checks(toks)
        for k, v in pc.items():
            if v is not None:
                score[k][1] += 1
                score[k][0] += 1 if v else 0
        print('held-out  %s f60 (bg%02d): %s | %s' % (name, arena, ' '.join(toks), ' '.join('%s=%s' % (k, 'agree' if v else 'DISAGREE' if v is False else 'n/a') for k, v in pc.items())))
    print('held-out score (agree / scored): ' + ', '.join('%s %d/%d' % (k, v[0], v[1]) for k, v in score.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--captures', default=os.path.join(jc.MAIN, 'out', 'captures'))
    ap.add_argument('--only', default='')
    ap.add_argument('--no-plant', action='store_true')
    ap.add_argument('--held-out', action='store_true', help='score D1 / D2 / D3 on the other arenas captures (report only; a disagreement is said, never tuned, never a failure)')
    a = ap.parse_args()
    bad = 0
    for name, frames in FRAMES:
        if a.only and a.only != name:
            continue
        path = os.path.join(a.captures, name)
        if not os.path.isdir(path) and not os.path.exists(path + '.zip'):
            print('FAIL  %s: capture missing (%s)' % (name, path))
            bad += 1
            continue
        for number in frames:
            t0 = time.time()
            try:
                toks, arena = sequence(path, number)
            except (jc.CensusError, rc.CensusError, af.FileReadError, OSError) as e:
                print('FAIL  %s f%d: %s' % (name, number, e))
                bad += 1
                continue
            ok = toks == GAME_ORDER
            bad += 0 if ok else 1
            print('%s  %s f%d (arena bg%02d, %.0f s): %s%s' % ('PASS' if ok else 'FAIL', name, number, arena, time.time() - t0, ' '.join(toks), '' if ok else '   (the game: ' + ' '.join(GAME_ORDER) + ')'))
    if not a.no_plant:
        pname, pnum = FRAMES[0][0], FRAMES[0][1][2]
        try:
            pt, _ = sequence(os.path.join(a.captures, pname), pnum, plant=True)
            detected = pt != GAME_ORDER
        except (jc.CensusError, rc.CensusError, af.FileReadError, OSError) as e:
            pt, detected = [str(e)], False
        bad += 0 if detected else 1
        print('%s  planted twin (the streams screens run and posts run permuted before naming): %s -> %s' % ('PASS' if detected else 'FAIL', ' '.join(pt), 'FAILS the order check as required' if detected else 'NOT DETECTED'))
    if a.held_out:
        held_out(a.captures)
    print('family_order:', 'FAIL (%d)' % bad if bad else 'PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

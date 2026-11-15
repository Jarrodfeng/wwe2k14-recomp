r"""S102 lane UQ1 acceptance, offline, 0 game runs: wwe2k14_rdr_record_skip bit 1 (the far card's op-21 is not recorded when the card cut drops its op-15; docs\renderer\RECORD_SKIP.md).

  py -I tools\crowd\record_skip_check.py [--root DIR] [--captures bryan,checkB,...] [--step N] [--lane133]

What a capture CAN and CANNOT show. A capture records our side: every draw carries its full constant bank, an op-21 is not an op of it. So the picture proof of dropping op-15s (lane 175 / T2) cannot show what
dropping an op-21 does to the draws AFTER it: a later draw that does not set c76-c78 itself would inherit the last op-21's world, and with the op-21 of a skipped card gone it would inherit an older one.
This check asks the capture the one question it can answer: in the full stream, after the cards of a frame (every card but the first of each type is skipped), is there ANY non-card draw whose c76-c78 equal the
world of the last skipped card (the only way it could be reading that op-21)? Count must be 0, over the draws that follow skipped cards (printed, must be > 0: a vacuous check fails).
  1. CENSUS (every capture): draws per frame by record site (cards 0x825130C4, stands blocks 0x82518574, floor 0x8276ED60, everything else), card op-21 skippable per frame (cards - person types), types per frame.
  2. INHERITANCE (every capture): the question above. Plant `inherit`: the first non-card draw after a skipped card gets that card's world: must be counted (>= 1), or the check is blind.
  3. WIRING lint (text): in src\scene_crowd.h the observer (crowd_live::OnOp21) is called BEFORE card_thin::SkipCardOp21 in the op-21 hook; the skip tests !t_run.first_valid; the op-15 hook's pairing path
     (t_dec.op21_skipped) sits BEFORE the tiled test in card_thin_hook.h DropCard. Planted: the two call lines swapped must be flagged.
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import crowd_cards_check as ccc  # noqa: E402
import card_thin_check as ctc    # noqa: E402
import cards_census as cc        # noqa: E402

SITE_FLOOR = 0x8276ED60
LANE133 = r'out\s101\lane133\bryan\capture'


def world(f, idx):
    d = f.draws[int(idx)]
    return tuple(int(x) for x in f.bank(int(d['vs_bank']))[304:316])


def frame_pass(cap, f, plant=None):
    """-> dict of counts for one frame: sites, cards, types, skipped, checked (non-card draws following a skipped card), inheritors."""
    D = f.draws
    out = dict(draws=0, cards=0, blocks=0, floor=0, other=0, types=0, skipped=0, checked=0, inheritors=0, planted=0)
    if not len(D) or not f.has_origins:
        return out
    seen = set()
    last_skipped = None   # world of the most recent card IF it was skipped; None once a kept card or a later draw has passed
    planted_done = False
    sl, tf, tc = f.slots, D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
    for kind, idx in f.ops:
        if int(kind) != 2:
            continue
        idx = int(idx)
        site = int(f.origins['call_site'][idx])
        out['draws'] += 1
        if site == cc.SITE_CARD:
            out['cards'] += 1
            im = 0
            for r in sl[int(tf[idx]):int(tf[idx]) + int(tc[idx])]:
                if int(r['slot']) == 0:
                    im = int(r['image'])
            if im in seen:
                out['skipped'] += 1
                last_skipped = world(f, idx)
            else:
                seen.add(im)
                last_skipped = None   # a kept card records its own op-21: the register is its own
            continue
        out['blocks'] += site == cc.SITE_BLOCK
        out['floor'] += site == SITE_FLOOR
        out['other'] += site not in (cc.SITE_BLOCK, SITE_FLOOR)
        if last_skipped is not None:
            out['checked'] += 1
            w = world(f, idx)
            if plant == 'inherit' and not planted_done:
                w, planted_done = last_skipped, True
                out['planted'] += 1
            if w == last_skipped:
                out['inheritors'] += 1
    out['types'] = len(seen)
    return out


def rd(p):
    return open(p, encoding='utf-8', errors='replace').read()


def wiring(root, planted=False):
    """-> list of problems (text lint of the two hook files)."""
    bad = []
    sc = open(os.path.join(root, 'src', 'scene_crowd.h'), encoding='utf-8', errors='replace').read()
    th = open(os.path.join(root, 'src', 'scene_live', 'card_thin_hook.h'), encoding='utf-8', errors='replace').read()
    if planted:   # the skip asked before the observer: a copy of the skip statement placed in front of the file
        sc = 'wwe2k14::card_thin::SkipCardOp21(\n' + sc
    a, b = sc.find('wwe2k14::crowd_live::OnOp21(ctx, base);'), sc.find('wwe2k14::card_thin::SkipCardOp21(')
    if a < 0 or b < 0:
        bad.append('scene_crowd.h: the observer call or the SkipCardOp21 call is missing')
    elif not a < b:
        bad.append('scene_crowd.h: the skip is asked BEFORE the observer read the card world')
    if 'TakeReplay()' not in sc or sc.count('ReplayOp21(ctx, base') < 2:
        bad.append('scene_crowd.h: the op-15 hook must replay the op-21 of a card / member the cut keeps after its op-21 was skipped (review F1)')
    if sc.count('ctx.r4.u32, ctx.r6.u32') < 2:
        bad.append('scene_crowd.h: the card and member op-21 skips must pass r4 / r6 (only (76, 3) may be skipped; review F2)')
    m21 = sc.find('wwe2k14::member_thin::SkipMemberOp21(')
    if m21 < 0 or (a >= 0 and not a < m21):
        bad.append('scene_crowd.h: the member op-21 skip (UQ2) is missing or comes before the observer')
    if sc.find('TakeMemberTail(wwe2k14::member_thin::kStands') < 0 or sc.find('TakeMemberTail(wwe2k14::member_thin::kFloor') < 0:
        bad.append('scene_crowd.h: a member loop exit does not record the tail op-21 (UQ2 F1)')
    mh = open(os.path.join(root, 'src', 'scene_live', 'member_thin_hook.h'), encoding='utf-8', errors='replace').read()
    if 'MemberOp15Pair(' not in mh or 'MemberOp21Step(' not in mh:
        bad.append('member_thin_hook.h: DropMember / SkipMemberOp21 must call the shipped MemberOp15Pair / MemberOp21Step')
    ko = sc.find('KnockOut(uint32_t(ctx.lr), true)')
    if ko < 0 or (a >= 0 and not a < ko):
        bad.append('scene_crowd.h: the test-only knock-out of the op-21 must come after the observer')
    if b >= 0 and '!t_run.first_valid' not in sc[b - 200:b + 100]:
        bad.append('scene_crowd.h: the skip does not test !t_run.first_valid (the legacy runs path)')
    core = open(os.path.join(root, 'src', 'scene_live', 'card_thin_core.h'), encoding='utf-8', errors='replace').read()
    i = core.find('inline bool Op15Step(')
    k, j = core.find('d.op21_skipped', i), core.find('if (tiled) {', i)
    if i < 0 or j < 0 or k < 0 or not k < j:
        bad.append('card_thin_core.h: Op15Step must test d.op21_skipped (the pairing) before the tiled early return')
    if '= Op15Step(' not in th or 'return Op21Step(' not in th:
        bad.append('card_thin_hook.h: DropCard / SkipCardOp21 must call the shipped Op15Step / Op21Step (no private copy of the decision)')
    c1, c2 = sc.find('__imp__sub_82512F70(ctx, base);'), sc.find('TakeTail(')
    if c1 < 0 or c2 < 0 or not c1 < c2:
        bad.append('scene_crowd.h: the card loop hook must record the loop-exit op-21 (TakeTail) after the original returns (review F1)')
    return bad


# (function, address of the op-21 `bl 0x82bb27d8`, the three constants scene_crowd.h must carry: (frame size, scratch r3 offset, matrix r5 offset))
FRAMES = [('82518340', 0x82518554, ('kFrameSize', 'kScratch', 'kMatrix')),
          ('82512F70', 0x825130AC, ('kCardFrameSize', 'kCardScratch', 'kCardMatrix')),
          ('8276EAA0', 0x8276ED38, ('kFloorFrameSize', 'kFloorScratch', 'kFloorMatrix'))]


def listing(main_co, fn):
    import subprocess
    r = subprocess.run([sys.executable, '-I', os.path.join(main_co, 'tools', 'analysis', 'list_function.py'), fn], capture_output=True, text=True, cwd=main_co, timeout=300)
    if r.returncode or not r.stdout.strip():
        raise RuntimeError('list_function.py %s failed: %s' % (fn, (r.stderr or r.stdout)[-300:]))
    return r.stdout


def frames_lint(root, main_co, planted=False):
    """The frame size / scratch / matrix constants scene_crowd.h uses to record a loop-exit or replayed op-21 must be what the listing shows: `stwu r1,-N(r1)`, and the last `addi r3,r1,A` / `addi r5,r1,B` before the op-21 call."""
    bad = []
    sc = rd(os.path.join(root, 'src', 'scene_crowd.h'))
    if planted:
        sc = re.sub(r'(kFloorScratch\s*=\s*)272', r'\g<1>273', sc)
    for fn, call, names in FRAMES:
        txt = listing(main_co, fn)
        rows = [(int(m.group(1), 16), m.group(2)) for m in re.finditer(r'^0x([0-9A-F]{8})\s+(.*)$', txt, re.M)]
        frame = next((int(re.search(r'stwu r1,-(\d+)\(r1\)', t).group(1)) for a, t in rows if t.startswith('stwu r1,')), None)
        before = [(a, t) for a, t in rows if a < call]
        r3 = next((int(re.search(r'addi r3,r1,(\d+)', t).group(1)) for a, t in reversed(before) if re.match(r'addi r3,r1,\d+', t)), None)
        r5 = next((int(re.search(r'addi r5,r1,(\d+)', t).group(1)) for a, t in reversed(before) if re.match(r'addi r5,r1,\d+', t)), None)
        have = []
        for nm in names:
            m = re.search(r'constexpr uint32_t %s\s*=\s*(\d+)' % nm, sc)
            have.append(int(m.group(1)) if m else None)
        if have != [frame, r3, r5]:
            bad.append('sub_%s: scene_crowd.h has (frame, scratch, matrix) = %s, the listing shows %s' % (fn, have, [frame, r3, r5]))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--captures', default='bryan,checkB,wm30')
    ap.add_argument('--step', type=int, default=3, help='every Nth frame with cards')
    ap.add_argument('--lane133', action='store_true', help='also the R25-B stretch (out\\s101\\lane133\\bryan\\capture)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = ccc.main_checkout(root)
    zips = {n: os.path.join(main_co, p) for n, p in ctc.CAPTURES}
    items = [(n, zips[n]) for n in a.captures.split(',') if n]
    if a.lane133:
        items.append(('lane133', os.path.join(main_co, LANE133)))
    problems = 0
    # --- wiring lint and its planted twin
    w = wiring(root)
    print(('PASS' if not w else 'FAIL') + ' wiring lint: %s' % ('observer before skip, first_valid test, pairing before tiled' if not w else w))
    problems += bool(w)
    wp = wiring(root, planted=True)
    print(('PASS' if wp else 'FAIL') + ' wiring plant (observer / skip swapped): %s' % ('caught: ' + wp[0] if wp else 'NOT caught'))
    problems += not wp
    fl = frames_lint(root, main_co)
    print(('PASS' if not fl else 'FAIL') + ' frame lint (stands 368/192/128, cards 224/80/112, floor 480/272/112 against list_function.py): %s' % ('all three match the listings' if not fl else fl))
    problems += bool(fl)
    fp = frames_lint(root, main_co, planted=True)
    print(('PASS' if fp else 'FAIL') + ' frame lint plant (floor scratch 273): %s' % ('caught' if fp else 'NOT caught'))
    problems += not fp
    tot_checked = 0
    for name, path in items:
        if not os.path.exists(path):
            print('FAIL input missing: ' + path)
            return 1
        cap = cc.Cap(path)
        rows, plant_rows = [], []
        n = 0
        for fi in range(len(cap.frames)):
            f = cap.frame(fi)
            pos, _ = __import__('card_thin_cut').card_runs(cap, f)
            if not pos:
                continue
            n += 1
            if (n - 1) % a.step:
                continue
            rows.append(frame_pass(cap, f))
            if len(plant_rows) < 1 and rows[-1]['checked']:
                plant_rows.append(frame_pass(cap, f, 'inherit'))
        if not rows:
            print('FAIL %s: no frame with cards' % name)
            problems += 1
            continue
        s = {k: sum(r[k] for r in rows) for k in rows[0]}
        m = len(rows)
        print('%-8s frames sampled %d: draws/frame %.0f = cards %.0f + stands blocks %.0f + floor %.0f + other %.0f; person types %.1f; card op-21 skippable/frame %.0f; '
              'draws following a skipped card checked %d, inheriting its world %d' % (name, m, s['draws'] / m, s['cards'] / m, s['blocks'] / m, s['floor'] / m, s['other'] / m, s['types'] / m,
                                                                                    s['skipped'] / m, s['checked'], s['inheritors']))
        tot_checked += s['checked']
        if s['inheritors']:
            print('FAIL %s: %d draws read a skipped card\'s world' % (name, s['inheritors']))
            problems += 1
        if not s['checked']:
            print('FAIL %s: nothing followed a skipped card: the inheritance check is vacuous' % name)
            problems += 1
        if plant_rows:
            pr = plant_rows[0]
            ok = pr['inheritors'] >= 1 and pr['planted'] == 1
            print(('PASS' if ok else 'FAIL') + ' %s plant inherit: a later draw given the skipped card\'s world is %s' % (name, 'counted (%d)' % pr['inheritors'] if ok else 'NOT counted'))
            problems += not ok
    print(('PASS' if not problems else 'FAIL %d problem(s)' % problems) + ' record_skip_check (draws checked %d)' % tot_checked)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())

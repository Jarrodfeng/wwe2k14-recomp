#!/usr/bin/env python3
"""S102 lane U1 check (offline, 0 game runs): wwe2k14_rdr_tail_release (docs\\renderer\\SWAP_TAIL.md).

  py -I tools\\tail\\tail_order_check.py [--root DIR] [--frames N]

Builds tools\\tail\\tail_order_test.cpp with -DRDR_TAIL_PLANTS against the tree's own src\\renderer_scene\\live_scene.cpp, record_io.cpp and the real
contributors (crowd, arena, wrestlers; the header-only lights / circles / plane / arena worlds / ring rows / casters / spheres), runs its arms and requires:
  - for each crowd path (ids, plain, runs, pool = ids + T11b's string pool): the records of `tail` (the emit after the release, the update thread's whole next frame recorded first),
    `tail_race` (no wait: real overlap) and `tail_flip` (deferred every other frame) are byte-equal to `serial` (today's order), every frame deferred,
    0 violations, 0 late latches, every pending finish taken at the release;
  - `tail_extra` (a contributor that cannot latch) equals `serial_extra`, with every frame's emit run at the latch (not_latchable = presents);
  - the planted twins are caught: `plant_release_first` (the update thread released before the present's latch) -> records differ AND late_latch = presents;
    `plant_latch_skip` (every contributor claims a latch it did not take) -> records differ AND violations > 0; `plant_skip_I` for arena / wrestlers /
    crowd / lights alone -> records differ AND the first violation names that contributor.
A check that cannot read its input fails loudly: no compiler, a build error, an arm with no records, a missing stats line."""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FAILS = []


def fail(msg):
    FAILS.append(msg)
    print('FAIL: ' + msg)


def ok(msg):
    print('PASS: ' + msg)


def run_arm(exe, arm, crowd='ids', frames=300, allow_empty=False):
    r = subprocess.run([exe, arm, crowd, str(frames)], capture_output=True, text=True, timeout=600)
    if r.returncode:
        fail('%s %s: exit %d %s' % (arm, crowd, r.returncode, r.stderr[-400:]))
        return None
    recs, stats, latch, events, tlatch = [], {}, {}, [], {}
    for line in r.stdout.splitlines():
        p = line.split()
        if not p:
            continue
        if p[0] == 'rec':
            recs.append((int(p[1]), p[2], int(p[3])))
        elif p[0] == 'ev':
            events.append((p[1], int(p[2]), int(p[3]), int(p[4])))   # kind, frame, index, the keeper's record frame (-1 none)
        elif p[0] in ('stats', 'latch', 'totals', 'trackerlatch'):
            d = stats if p[0] == 'stats' else (latch if p[0] == 'latch' else (tlatch if p[0] == 'trackerlatch' else {}))
            for i in range(1, len(p) - 1, 2):
                try:
                    d[p[i]] = int(p[i + 1])
                except ValueError:
                    d[p[i]] = p[i + 1]
    if not recs and not allow_empty:
        fail('%s %s: no records (the test made nothing to compare)' % (arm, crowd))
        return None
    if 'presents' not in stats or 'violations' not in latch:
        fail('%s %s: no stats / latch line' % (arm, crowd))
        return None
    if recs and len(set(h for _, h, _ in recs)) < len(recs) // 2:
        fail('%s %s: the records barely change frame to frame (%d distinct of %d): the test would not see a frame leaking' % (arm, crowd, len(set(h for _, h, _ in recs)), len(recs)))
    if not events or not tlatch:
        fail('%s %s: no hand-off events / tracker latch line' % (arm, crowd))
        return None
    return {'recs': recs, 'stats': stats, 'latch': latch, 'events': events, 'tlatch': tlatch}


def handoff_problems(events):
    """Review F1: every frame part (a lock read-back) of frame P must come after frame P-1 was submitted and take record P-1; indices strictly rise."""
    probs, submitted, last_idx = [], set(), -1
    first_part = min((f for k, f, _, _ in events if k == 'part'), default=None)
    for kind, frame, idx, rec in events:
        if idx <= last_idx:
            probs.append('index %d after %d' % (idx, last_idx))
        last_idx = idx
        if kind == 'submit':
            submitted.add(frame)
        elif frame != first_part:
            if frame - 1 not in submitted:
                probs.append('part of frame %d drawn before frame %d was submitted' % (frame, frame - 1))
            if rec != frame - 1:
                probs.append('part of frame %d took record %d (not %d)' % (frame, rec, frame - 1))
    return probs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--frames', type=int, default=300)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not os.path.isfile(CLANG):
        fail('%s not found' % CLANG)
        return 1
    out_dir = os.path.join(root, 'out', 'tail')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'tail_order_test.exe')
    srcs = [os.path.join(root, 'tools', 'tail', 'tail_order_test.cpp')] + [os.path.join(root, 'src', *p.split('/')) for p in (
        'renderer_scene/live_scene.cpp', 'renderer_scene/record_io.cpp', 'scene_live/crowd_contrib.cpp', 'scene_live/arena_contrib.cpp', 'scene_live/wrestler_contrib.cpp')]
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DRDR_TAIL_PLANTS', '-DNOMINMAX', '-I', os.path.join(root, 'src'), '-o', exe] + srcs,
                       capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        fail('build: ' + (b.stderr or b.stdout)[-3000:])
        return 1
    ok('built tail_order_test.exe (-DRDR_TAIL_PLANTS, the tree\'s live_scene.cpp / record_io.cpp / contributors)')
    n = a.frames

    for crowd in ('ids', 'plain', 'runs', 'pool'):
        ref = run_arm(exe, 'serial', crowd, n)
        if not ref:
            continue
        if ref['stats']['deferred'] != 0:
            fail('serial %s deferred %d frames' % (crowd, ref['stats']['deferred']))
        for arm in ('tail', 'tail_race', 'tail_flip'):
            r = run_arm(exe, arm, crowd, n)
            if not r:
                continue
            s, l = r['stats'], r['latch']
            want_def = n if arm != 'tail_flip' else n // 2
            if r['recs'] != ref['recs']:
                diff = next((i for i, (x, y) in enumerate(zip(r['recs'], ref['recs'])) if x != y), min(len(r['recs']), len(ref['recs'])))
                fail('%s %s: records differ from serial (first at record %d of %d / %d)' % (arm, crowd, diff, len(r['recs']), len(ref['recs'])))
            elif s['deferred'] != want_def or s['emit_deferred'] != want_def or s['finished_at_release'] != want_def:
                fail('%s %s: deferred %d / emit %d / finished at release %d, expected %d each' % (arm, crowd, s['deferred'], s['emit_deferred'], s['finished_at_release'], want_def))
            elif l['violations'] or s['late_latch'] or l['not_latchable'] or l['registry_changed']:
                fail('%s %s: violations %d, late latches %d, not latchable %d, registry changed %d (all must be 0)' % (arm, crowd, l['violations'], s['late_latch'], l['not_latchable'], l['registry_changed']))
            else:
                ok('%s %s: %d records byte-equal to serial; %d emits after the release, 0 violations' % (arm, crowd, len(r['recs']), s['emit_deferred']))

    ref = run_arm(exe, 'serial', 'ids', n)
    # the fallback: a contributor that cannot latch
    se, te = run_arm(exe, 'serial_extra', 'ids', n), run_arm(exe, 'tail_extra', 'ids', n)
    if se and te:
        if te['recs'] != se['recs']:
            fail('tail_extra: records differ from serial_extra')
        elif te['latch']['not_latchable'] != n or te['stats']['emit_deferred'] != 0:
            fail('tail_extra: not_latchable %d (expected %d), emit_deferred %d (expected 0)' % (te['latch']['not_latchable'], n, te['stats']['emit_deferred']))
        elif ref and se['recs'] == ref['recs']:
            fail('serial_extra equals serial: the extra contributor added nothing (the arm proves nothing)')
        else:
            ok('tail_extra: a contributor that cannot latch keeps the whole emit at the latch (%d frames), records byte-equal' % n)
    # planted twins
    if ref:
        r = run_arm(exe, 'plant_release_first', 'ids', n)
        if r:
            if r['recs'] == ref['recs']:
                fail('plant_release_first: records equal serial: the test cannot see the update thread leaking the next frame in')
            elif r['stats']['late_latch'] != n:
                fail('plant_release_first: late_latch %d, expected %d (the window check is blind)' % (r['stats']['late_latch'], n))
            else:
                ok('plant_release_first caught: records differ, late_latch %d (never deferred)' % r['stats']['late_latch'])
        r = run_arm(exe, 'plant_latch_skip', 'ids', n)
        if r:
            if r['recs'] == ref['recs'] or not r['latch']['violations']:
                fail('plant_latch_skip NOT caught: records %s, violations %d' % ('equal' if r['recs'] == ref['recs'] else 'differ', r['latch']['violations']))
            else:
                ok('plant_latch_skip caught: records differ, violations %d (first %s)' % (r['latch']['violations'], r['latch'].get('first')))
        for i, name in enumerate(('arena', 'wrestlers', 'crowd', 'lights')):
            r = run_arm(exe, 'plant_skip_%d' % i, 'ids', n)
            if not r:
                continue
            if r['recs'] == ref['recs']:
                fail('plant_skip_%d (%s): records equal serial: this contributor\'s latch is not proven to matter' % (i, name))
            elif not r['latch']['violations'] or r['latch'].get('first') != name:
                fail('plant_skip_%d (%s): violations %d, first %s' % (i, name, r['latch']['violations'], r['latch'].get('first')))
            else:
                ok('plant_skip_%d caught: %s alone not latched -> records differ, violation named %s' % (i, name, name))
    # review F1: a read-back lock on the update thread right after the release, before the swap thread's finish
    sl, tlk = run_arm(exe, 'serial_lock', 'ids', n), run_arm(exe, 'tail_lock', 'ids', n)
    if sl and tlk:
        p1, p2 = handoff_problems(sl['events']), handoff_problems(tlk['events'])
        if p1:
            fail('serial_lock: the reference itself breaks the hand-off order: ' + '; '.join(p1[:3]))
        elif p2:
            fail('tail_lock: ' + '; '.join(p2[:3]))
        elif tlk['recs'] != sl['recs'] or tlk['events'] != sl['events']:
            fail('tail_lock: records or hand-off events differ from serial_lock')
        elif tlk['stats']['lock_in_window'] == 0 or tlk['stats']['finished_at_lock'] + tlk['stats']['finished_at_release'] != tlk['stats']['deferred']:
            fail('tail_lock: locks in the window %d, finished at lock %d + at release %d != deferred %d' % (tlk['stats']['lock_in_window'], tlk['stats']['finished_at_lock'], tlk['stats']['finished_at_release'], tlk['stats']['deferred']))
        else:
            ok('tail_lock: %d read-back locks inside the window waited for / ran the finish; frames and parts in the serial order, every part took record P-1' % tlk['stats']['lock_in_window'])
    r = run_arm(exe, 'plant_lock_nowait', 'ids', n)
    if r:
        pr = handoff_problems(r['events'])
        if not pr:
            fail('plant_lock_nowait NOT caught: a lock that does not wait for the finish keeps the hand-off order (the check is blind)')
        else:
            ok('plant_lock_nowait caught: %s' % pr[0])
    # review F2: a contributor before the trackers throws in every Emit: the trackers' latches are never consumed
    for arm, frames_t in (('tail_throw', 1000), ('plant_throw_nocap', 1000)):
        r = run_arm(exe, arm, 'ids', frames_t, allow_empty=True)
        if not r:
            continue
        tlat = r['tlatch']
        bounded = tlat['kept'] <= tlat['cap'] + 1 and tlat['held'] <= tlat['cap']
        if arm == 'tail_throw':
            if not bounded or not tlat['dropped']:
                fail('tail_throw: tracker latch kept %d held %d dropped %d (cap %d): unbounded' % (tlat['kept'], tlat['held'], tlat['dropped'], tlat['cap']))
            else:
                ok('tail_throw: 1000 refused records, the tracker latch holds %d of cap %d (%d buffers), %d frames dropped and counted' % (tlat['held'], tlat['cap'], tlat['kept'], tlat['dropped']))
        elif bounded:
            fail('plant_throw_nocap NOT caught: the uncapped latch stayed within the cap (kept %d)' % tlat['kept'])
        else:
            ok('plant_throw_nocap caught: the uncapped latch grew to %d buffers' % tlat['kept'])
    if FAILS:
        print('%d problem(s)' % len(FAILS))
        return 1
    print('all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

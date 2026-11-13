r"""S101 lane 130: Check J1's analysis of the kScArenaWorlds field (section 15). Offline, 0 game runs.

  py -I tools\skip\arena_world_records.py CAPTURE_OR_CASE [--frames A:B:STEP] [--out SOURCED.txt] [--expect-offset N]

CAPTURE has frames\ AND scene\ (a J1 case zip unpacked, or a capture folder). It
  1. reads every record's section 15 (ledger_io Record.arena_worlds) into a table keyed by the HOOK's frame stamp (never by the record index);
  2. MEASURES the hook-to-stream frame offset on the moving pieces and the placed props: for each offset -4..+4 the number of stream draws whose c76..c78 equal the rows of
     the entry of the same engine object stamped (guest frame - offset); the best offset is printed with its votes (a tie, or no votes, is a FAIL: nothing is joined blind);
  3. joins every placed / moving / animated draw at that offset and prints, per family: draws, rows equal (c76..c78), the mismatches NAMED (absent: no entry for the
     object; palette: the entry was a foreign palette (r5 != [model+200]); node: the entry's node was assumed (no part descriptor); unexplained: node or palette), and for the
     animated family c186 against the clock entry (x1, or x2 for the beam cones) after the offset;
  4. the census: the frames that carry any entry, and the share of frames in which EVERY arena object drawn has an entry;
  5. writes the sourced file (`S fi i regs`) that `arena_ready.py --worlds` reads, so the families are sourced from the RECORDS, not fitted.
The pure parts (best_offset, join) are unit-tested by tools\scene\arena_world_selftest.py with synthetic records."""
import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ledger as L  # noqa: E402
import arena_world_oracle as O  # noqa: E402

TOL = 2e-3
OFFSETS = range(-4, 5)


def fl(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def build_table(records):
    """records: iterable of Record (or objects with .arena_worlds()). -> ({frame: {object: [entries]}}, {frame: clock entry})"""
    objs = collections.defaultdict(lambda: collections.defaultdict(list))
    clocks = {}
    for rec in records:
        if rec is None:
            continue
        for e in rec.arena_worlds():
            if e['kind'] == 1:
                objs[e['frame']][e['object']].append(e)
            else:
                clocks[e['frame']] = e
    return objs, clocks


def rows_equal(a, b, tol=TOL):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def best_offset(draws, objs):
    """draws: [(fi, guest, i, object, family, rows(12 floats), c186)]. -> (offset, votes dict). Votes count draws of the moving / placed families whose rows equal an entry's."""
    votes = collections.Counter()
    for off in OFFSETS:
        votes[off] = 0
    for fi, guest, i, obj, fam, rows, c186 in draws:
        if fam not in ('moving', 'placed'):
            continue
        for off in OFFSETS:
            ents = objs.get(guest - off, {}).get(obj, ())
            if any(rows_equal(e['world'], rows) for e in ents):
                votes[off] += 1
    best = max(votes.values())
    top = [o for o, n in votes.items() if n == best]
    if best == 0 or len(top) != 1:
        return None, dict(votes)
    return top[0], dict(votes)


def join(draws, objs, clocks, off):
    """-> (sourced {(fi, i): {regs}}, stats {family: Counter}, present {frame: [objects seen, objects with an entry]})"""
    sourced = {}
    stats = collections.defaultdict(collections.Counter)
    present = collections.defaultdict(lambda: [0, 0])
    seen_obj = set()
    for fi, guest, i, obj, fam, rows, c186 in draws:
        ents = objs.get(guest - off, {}).get(obj, ())
        if (fi, obj) not in seen_obj:
            seen_obj.add((fi, obj))
            present[fi][0] += 1
            present[fi][1] += 1 if ents else 0
        S = stats[fam]
        S['draws'] += 1
        if fam == 'animated':
            ck = clocks.get(guest - off)
            if ck is None:
                S['absent'] += 1
            elif abs(c186 - ck['clock']) < 0.5 or abs(c186 - 2 * ck['clock']) < 0.5:
                S['clock_equal'] += 1
                sourced[(fi, i)] = {186}
            else:
                S['clock_differs'] += 1
            continue
        if fam not in ('moving', 'placed'):
            continue
        if not ents:
            S['absent'] += 1
        elif any(rows_equal(e['world'], rows) for e in ents):
            S['equal'] += 1
            sourced[(fi, i)] = {76, 77, 78}
        elif any(e['origin'] & 1 for e in ents):
            S['mismatch_palette'] += 1
        elif any(e['origin'] & 2 for e in ents):
            S['mismatch_node'] += 1
        else:
            S['mismatch_unexplained'] += 1
    return sourced, stats, present


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--expect-offset', type=int, default=None)
    x = ap.parse_args(argv)
    try:
        cap = L.Capture(x.capture)
        if not cap.has_scene:
            print('FAIL  %s has no scene\\ folder (no records)' % x.capture)
            return 1
        frames = L.parse_frames(x.frames, cap.n)
        records = [cap.record(i) for i in range(cap.n)]
        objs, clocks = build_table(records)
        if not objs and not clocks:
            print('FAIL  none of the %d records carries kScArenaWorlds (section 15): a capture taken without --wwe2k14_scene_light_states, or before lane 130' % cap.n)
            return 1
        draws = [(fi, g, i, obj, fam, tuple(fl(b) for b in rows), fl(c)) for fi, g, i, obj, fam, rows, c in O.iter_draws(cap, frames, all_objects=True)]
        off, votes = best_offset(draws, objs)
        print('hook-to-stream frame offset (stream guest frame - entry stamp), votes: %s' % ', '.join('%+d: %d' % (o, votes[o]) for o in sorted(votes)))
        if off is None:
            print('FAIL  no unique best offset: nothing is joined blind')
            return 1
        print('measured offset: %+d' % off)
        sourced, stats, present = join(draws, objs, clocks, off)
    except (L.LedgerError, OSError, KeyError, ValueError) as e:
        print('FAIL  %s: %s' % (x.capture, e))
        return 1
    bad = 0
    for fam in ('placed', 'moving', 'animated'):
        S = stats.get(fam)
        if not S:
            continue
        if fam == 'animated':
            print('  animated  draws %6d  c186 equals the clock after the offset %6d (%.1f%%), differs %d, no clock entry %d' % (S['draws'], S['clock_equal'], 100.0 * S['clock_equal'] / S['draws'], S['clock_differs'], S['absent']))
        else:
            mism = ' '.join('%s %d' % (k[9:], S[k]) for k in ('mismatch_palette', 'mismatch_node', 'mismatch_unexplained') if S[k])
            print('  %-9s draws %6d  rows equal %6d (%.1f%%), absent %d, mismatches: %s' % (fam, S['draws'], S['equal'], 100.0 * S['equal'] / S['draws'], S['absent'], mism or 'none'))
            bad += 1 if S['equal'] < S['draws'] else 0
    frames_with = sum(1 for fr, (n, k) in present.items() if n)
    full = sum(1 for fr, (n, k) in present.items() if n and k == n)
    print('census: %d frames with arena draws, %d of them have an entry for EVERY object drawn (%.1f%%); entries: %d object-frames, %d clock frames' % (
        frames_with, full, 100.0 * full / max(1, frames_with), sum(len(v) for v in objs.values()), len(clocks)))
    if x.expect_offset is not None and off != x.expect_offset:
        print('FAIL  measured offset %+d, expected %+d' % (off, x.expect_offset))
        bad += 1
    if x.out:
        os.makedirs(os.path.dirname(os.path.abspath(x.out)), exist_ok=True)
        with open(x.out, 'w', encoding='utf-8') as f:
            for (fi, i), regs in sorted(sourced.items()):
                f.write('S %d %d %s\n' % (fi, i, ','.join(str(r) for r in sorted(regs))))
        print('sourced file: %s (arena_ready.py CAPTURE --worlds %s)' % (x.out, x.out))
    print('arena_world_records: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

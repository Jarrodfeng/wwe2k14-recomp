r"""S101 lane 194 (Ring D3 R2, docs\renderer\SKIP_GAME_WORK_MAP.md 16.6 step 2 / 16.7): the ring's per-call node rows against the stream, on the zip of the ONE live verify run. Offline, 0 game runs, read-only.

  py -I tools\ring\ring_rows_check.py CAPTURE_OR_CASE --arena N [--frames A:B:STEP] [--expect-covered PCT] [--plant NAME]

CAPTURE has frames\ AND scene\ (the verify run's case zip unpacked: banks of the ring's draws + light states + kScRingRows / kScRingNodes). For every ring draw of the stream (found by content as
tools\ring\ring_ledger.py does) it takes the RingRows entry of (hook frame, object, call = the draw's ordinal among its object's calls) and compares the stream's bank registers c76..c138 with the entry's rows
over the mesh's PALETTE COUNT only (registers c76 + 3 * npal .. c138 are the previous draw's stale rows: don't care, ring_rows_core.h RowsRegisterDontCare):
  covered      the share of ring draws that have an entry with rows (the verify arm's "would cover")
  rows equal   the draws whose every cared register equals the entry's rows (tolerance 1e-6 relative: the record copies the floats the packer reads, so bit equality is expected)
  mismatches   named: absent (no entry), unreadable / count unknown (the entry says so), node (a palette slot's node is outside the entry's rows), rows (the values differ)
The slot -> node base is not known from the listings (the file's palette lists node indices; whether slot k reads rows[node] or rows[node - 1] is [U]): both are tried over the whole run and the votes are
printed; the winner must be unique and must be the one the packer's `r5 + 64 (n - 1)` implies (1-based nodes -> rows[node - 1]), else FAIL.
The hook-to-stream frame offset is measured the same way (-4..+4, unique best, +1 expected: J1). Plants (tests only): no_npal_cut (the stale slots counted: the PAST BUG of the wrestlers' palette, the
never-moving posts "mismatch"), wrong_offset0 (joined at offset 0), no_call_index (the call index dropped: every call takes the object's first entry).
The pure parts (compare_draw, vote_base, vote_offset) are unit-tested by tools\ring\ring_rows_selftest.py on synthetic data; the capture path (main) has NO real capture to run on until the verify run exists
(SKIP_GAME_WORK_MAP.md 16.7: unproven)."""
import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
sys.path.insert(0, HERE)

PLANTS = ('no_npal_cut', 'wrong_offset0', 'no_call_index')
OFFSETS = range(-4, 5)
TOL = 1e-6
FIRST_REG, LAST_REG = 76, 138


def fl(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def close(a, b, tol=TOL):
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def build_table(records):
    """records: iterable of Record (or objects with .ring_rows()). -> {frame: {object: [entries sorted by call]}}"""
    t = collections.defaultdict(lambda: collections.defaultdict(list))
    for rec in records:
        if rec is None:
            continue
        for e in rec.ring_rows():
            t[e['frame']][e['object']].append(e)
    for fr in t.values():
        for lst in fr.values():
            lst.sort(key=lambda e: e['call'])
    return t


def dont_care(npal, reg):
    """ring_rows_core.h RowsRegisterDontCare: c76 + 3 * npal .. c138; npal 0 = unknown = care."""
    return npal > 0 and FIRST_REG <= reg <= LAST_REG and reg >= FIRST_REG + 3 * npal


def compare_draw(entry, bank, palette, base, plants=()):
    """One draw: `entry` = a RingRows dict (rows = n_nodes x 12 floats in the record's order: row k = rows[12 * 0 + 4k..4k+3]... see below), `bank` = callable reg -> 4 floats of the stream's c<reg>,
    `palette` = the mesh's palette (node index of each slot), `base` = 0 or 1 (slot k reads entry rows[palette[k] - base]). -> (verdict, cared registers compared, registers equal)
    A record node holds 12 floats = c(76+3k) .. c(76+3k+2) of a slot whose node it is: float 4*row + i is component i of row `row`."""
    npal = len(palette) if 'no_npal_cut' not in plants else (LAST_REG - FIRST_REG + 1) // 3
    if entry is None:
        return 'absent', 0, 0
    if not palette:
        return 'nopalette', 0, 0   # the mesh's palette is unknown: nothing to compare (counted, never "equal")
    if entry['flags'] & 4:
        return 'count_unknown', 0, 0
    if entry['flags'] & 16 or entry['n_nodes'] == 0:
        return 'unreadable', 0, 0
    cared = eq = 0
    bad = None
    for slot in range(npal):
        node = palette[slot] - base if slot < len(palette) else 0
        for row in range(3):
            reg = FIRST_REG + 3 * slot + row
            if reg > LAST_REG:
                break
            if 'no_npal_cut' not in plants and dont_care(len(palette), reg):
                continue
            if node < 0 or node >= entry['n_nodes']:
                bad = bad or 'node'
                cared += 1
                continue
            want = entry['rows'][node][4 * row:4 * row + 4]
            got = bank(reg)
            cared += 1
            if all(close(a, b) for a, b in zip(want, got)):
                eq += 1
            else:
                bad = bad or 'rows'
    return (bad or 'equal'), cared, eq


def vote_base(draws, plants=()):
    """draws: [(entry, bank, palette)]. -> (best base or None, votes {base: draws that compared fully equal})"""
    votes = {0: 0, 1: 0}
    for entry, bank, palette in draws:
        for base in (0, 1):
            if compare_draw(entry, bank, palette, base, plants)[0] == 'equal':
                votes[base] += 1
    best = max(votes.values())
    top = [b for b, n in votes.items() if n == best]
    return (top[0] if best > 0 and len(top) == 1 else None), votes


def vote_offset(draws_by_frame, table, base, palette_of, bank_of, plants=()):
    """draws_by_frame: {guest_frame: [(object, call, key)]}. -> (best offset or None, votes {offset: fully equal draws})"""
    votes = {o: 0 for o in OFFSETS}
    for g, lst in draws_by_frame.items():
        for obj, call, key in lst:
            for off in OFFSETS:
                ents = table.get(g - off, {}).get(obj, ())
                entry = next((e for e in ents if e['call'] == call), None) if ents else None
                if compare_draw(entry, bank_of(key), palette_of(key), base, plants)[0] == 'equal':
                    votes[off] += 1
    best = max(votes.values())
    top = [o for o, n in votes.items() if n == best]
    return (top[0] if best > 0 and len(top) == 1 else None), votes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--frames', default='')
    ap.add_argument('--expect-covered', type=float, default=None)
    ap.add_argument('--plant', default='', choices=('',) + PLANTS)
    x = ap.parse_args(argv)
    import ledger as L  # noqa: E402
    import ring_ledger as RL  # noqa: E402
    import ring_files as rf  # noqa: E402
    plants = (x.plant,) if x.plant else ()
    try:
        cap = L.Capture(x.capture)
        if not cap.has_scene:
            print('FAIL  %s has no scene\\ folder (no records)' % x.capture)
            return 1
        frames = L.parse_frames(x.frames, cap.n)
        table = build_table(cap.record(i) for i in range(cap.n))
        if not table:
            print('FAIL  none of the %d records carries kScRingRows (section 20): a run taken without --wwe2k14_rdr_ring_d3, or before lane 194' % cap.n)
            return 1
        reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
        joiner = RL.Joiner(x.arena)
        ring = rf.load_ring(x.arena, textures=False)
        pals = {e: [list(m.palette) for m in sorted(ms, key=lambda m: m.index)] for e, ms in ring.entries.items()}   # the file's palette: node index of each bone slot, per mesh
        joined = {}
        for fi in frames:   # the pre-pass of ring_ledger: an object's join over the whole capture (thin-recorded draws carry counts only in proof frames)
            f = cap.frame(fi)
            by = collections.defaultdict(dict)
            for i in range(f.ndraws):
                o = f.origin(i)
                if o and o['klass'] == 1 and o['pass_site'] == RL.SITE_A:
                    vc, ic = RL.draw_counts(f, i)
                    if vc:
                        by[o['object']].setdefault(o['part'], (vc, ic))
            for obj, parts in by.items():
                ents = joiner.join(parts)
                if ents:
                    joined[obj] = ents
        draws = []   # (guest frame, object, call, entry-less key (fi, i), palette, bank values)
        banks = {}
        for fi in frames:
            f = cap.frame(fi)
            seen = collections.Counter()
            for i in range(f.ndraws):
                o = f.origin(i)
                if not o or o['klass'] != 1 or o['pass_site'] != RL.SITE_A or o['object'] not in joined:
                    continue
                call = 0 if 'no_call_index' in plants else seen[(o['object'], o['part'])]
                seen[(o['object'], o['part'])] += 1
                d = f.draw(i)
                vb = f.bank(d['vs_bank'])
                pal = joiner_palette(pals, joined[o['object']], o['part'])
                banks[(fi, i)] = (tuple(fl(b) for b in vb), pal)
                draws.append((f.guest_frame, o['object'], call, (fi, i)))
    except (L.LedgerError, OSError, KeyError, ValueError) as e:
        print('FAIL  %s: %s' % (x.capture, e))
        return 1

    def bank_of(key):
        v = banks[key][0]
        return lambda r: v[4 * r:4 * r + 4]

    def palette_of(key):
        return banks[key][1]
    if not draws:
        print('FAIL  no ring draw found in the selected frames')
        return 1
    by_frame = collections.defaultdict(list)
    for g, obj, call, key in draws:
        by_frame[g].append((obj, call, key))
    sample = [(next((e for e in table.get(g - 1, {}).get(obj, ()) if e['call'] == call), None), bank_of(key), palette_of(key)) for g, obj, call, key in draws]
    base, bvotes = vote_base(sample, plants)
    print('slot -> node base votes (draws fully equal at offset +1): base 0: %d, base 1: %d' % (bvotes[0], bvotes[1]))
    if base != 1:
        print('FAIL  the unique best base must be 1 (rows[node - 1], the packer\'s r5 + 64 (n - 1)); got %s' % base)
        return 1
    off, ovotes = vote_offset(by_frame, table, base, palette_of, bank_of, plants)
    print('hook-to-stream frame offset votes: %s' % ', '.join('%+d: %d' % (o, ovotes[o]) for o in sorted(ovotes)))
    if 'wrong_offset0' in plants:
        off = 0
    if off is None:
        print('FAIL  no unique best offset: nothing is joined blind')
        return 1
    print('measured offset: %+d' % off)
    stats = collections.defaultdict(collections.Counter)
    for g, obj, call, key in draws:
        ents = table.get(g - off, {}).get(obj, ())
        entry = next((e for e in ents if e['call'] == call), None) if ents else None
        verdict, cared, eq = compare_draw(entry, bank_of(key), palette_of(key), base, plants)
        cls = RL.piece_name(joined[obj])
        s = stats[cls]
        s['draws'] += 1
        s[verdict] += 1
        s['covered'] += 1 if verdict in ('equal', 'rows', 'node') else 0
    bad = 0
    tot = collections.Counter()
    for cls in sorted(stats):
        s = stats[cls]
        tot.update(s)
        print('  %-26s draws %6d  covered %6d (%.1f%%)  rows equal %6d  mismatches: %s' % (cls, s['draws'], s['covered'], 100.0 * s['covered'] / s['draws'], s['equal'],
              ' '.join('%s %d' % (k, s[k]) for k in ('absent', 'unreadable', 'count_unknown', 'node', 'rows') if s[k]) or 'none'))
        bad += 1 if s['equal'] < s['draws'] else 0
    cov = 100.0 * tot['covered'] / max(1, tot['draws'])
    print('total: %d ring draws, the record would cover %d (%.1f%%), rows equal %d' % (tot['draws'], tot['covered'], cov, tot['equal']))
    if x.expect_covered is not None and cov < x.expect_covered:
        print('FAIL  covered %.1f%% < expected %.1f%%' % (cov, x.expect_covered))
        bad += 1
    print('ring_rows_check: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


def joiner_palette(pals, entries, part):
    """The mesh palette (node indices) of mesh `part`, taken from the first joined entry that has it (counts joined the object to its entries)."""
    for e in entries:
        pal = pals.get(e)
        if pal is not None and part < len(pal):
            return pal[part]
    return []


if __name__ == '__main__':
    sys.exit(main())

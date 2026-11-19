r"""S103 lane E7: do the record's kScPartClocks rows (section 50) equal the stream's VS c186? Offline, 0 game runs.

  py -I tools\skip\part_clock_join.py CAPTURE_OR_CASE [--frames A:B:STEP] [--plant NAME] [--expect-equal PCT] [--min-draws N]

For every main-pass arena draw (yProp_ / yBG_, pass 0x82C0A950 / 0x82C0B94C) the stream binds c186 (bit pattern). The record of frame i carries PartClock rows
{frame (hook guest frame), object (model address), slot, part, time}. The join (docs\renderer\LIVE_MAPS.md "Lane E5"): the draw's engine object + its part index
(r16 = DrawOrigin::part) -> the row of that object with the largest part <= r16, read from the record stamped (guest frame - offset). The offset is MEASURED
(-4..+4; a tie or no vote = FAIL, nothing is joined blind). Per object it prints equal / differ / unjoined (no row, or no record of that stamp); `differ` is split
into x2 (c186 == 2 * time) and other. SLOT (--speeds LIST = `uvscroll_check.exe --arena 4` output): a UVScroll draw's object gives the row's `slot`; (slot, part) must give the loader's speeds equal to the draw's c203 / c204.
Plants: offset0 (the lag ignored), part0 (every part asked for 0), slot_off (the row's slot +1, needs --speeds). Each must make the verdict fail. Exit 1 on FAIL."""
import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ledger as L  # noqa: E402
from ledger_io import Registry  # noqa: E402

SITES = (L.SITE_A, L.SITE_B)
OFFSETS = range(-4, 5)
PLANTS = ('offset0', 'part0', 'slot_off')


def fl(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xFFFFFFFF))[0]


def part_clock_full(rec):
    if 50 not in rec.S:
        return []
    es, cnt, so = rec.S[50]
    out = []
    for i in range(cnt):
        ver, frame, obj, part, t, per, step, slot = struct.unpack_from('<IIIIfffI', rec.b, so + es * i)
        out.append(dict(frame=frame, object=obj, part=part, time=t, period=per, step=step, slot=slot))
    return out


def build_table(rows_per_record):
    """-> {frame stamp: {object: [row dict ascending by part]}}"""
    tab = collections.defaultdict(lambda: collections.defaultdict(list))
    for rows in rows_per_record:
        seen = set()
        for r in rows:
            k = (r['frame'], r['object'], r['part'])
            if k in seen:
                continue
            seen.add(k)
            tab[r['frame']][r['object']].append(r)
    for f in tab.values():
        for o in f.values():
            o.sort(key=lambda r: r['part'])
    return tab


def clock_of(rows, part):
    """The row with the largest part <= `part` (part_clock_lookup.h ClockOf), or None."""
    best = None
    for r in rows:
        if r['part'] <= part:
            best = r
        else:
            break
    return best


def best_offset(draws, tab, plants=()):
    """draws: [(guest, object, part, c186 bits)]. -> (offset, votes). A vote = an exact bit-equal clock at that offset."""
    votes = collections.Counter({o: 0 for o in OFFSETS})
    for guest, obj, part, bits in draws:
        for off in OFFSETS:
            rows = tab.get(guest - off, {}).get(obj)
            if not rows:
                continue
            r = clock_of(rows, 0 if 'part0' in plants else part)
            if r is not None and fl(bits) == r['time']:
                votes[off] += 1
    best = max(votes.values())
    top = [o for o, n in votes.items() if n == best]
    if best == 0 or len(top) != 1:
        return None, dict(votes)
    return top[0], dict(votes)


def join(draws, tab, off, plants=()):
    """-> {object: Counter(equal, equal_x2, differ, unjoined)}"""
    res = collections.defaultdict(collections.Counter)
    for guest, obj, part, bits in draws:
        C = res[obj]
        rows = tab.get(guest - off, {}).get(obj)
        if not rows:
            C['unjoined'] += 1
            continue
        r = clock_of(rows, 0 if 'part0' in plants else part)
        if r is None:
            C['unjoined'] += 1
        elif fl(bits) == r['time']:
            C['equal'] += 1
        elif fl(bits) == 2 * r['time']:
            C['equal_x2'] += 1
        else:
            C['differ'] += 1
    return res


def read_speeds(path):
    """uvscroll_check.exe output lines (a file, or 'game:DIR' = run out/tools/uvscroll_check.exe --game DIR --arena 4: read-only on the player's files) -> {(slot, rec): (U V U2 V2 U3 V3 U4 V4)}"""
    out = {}
    if path.startswith('game:'):
        import subprocess
        game = path[5:]
        if not os.path.isdir(game):
            game = os.path.join(L.main_checkout(), game)
        exe = os.path.join(L.ROOT, 'out', 'tools', 'uvscroll_check.exe')
        lines = subprocess.run([exe, '--game', game, '--arena', '4'], capture_output=True, text=True, timeout=300).stdout.splitlines()
    else:
        with open(path, encoding='utf-8') as fh:
            lines = fh.read().splitlines()
    for ln in lines:
        m = ln.split()
        if len(m) < 12 or m[0] != 'arena':
            continue
        slot, rec = int(m[3]), int(m[5])
        a, b = ln.split('UVScroll0')[1].split('have')[0].split('|')
        out[(slot, rec)] = tuple(float(v) for v in a.split()) + tuple(float(v) for v in b.split())
    return out


def slot_check(cap, frames, tab, off, speeds, plants=()):
    """The slot join, per draw: a UVScroll draw's engine object -> the PartClock row's `slot`; (slot, DrawOrigin::part) -> the loader's speeds (E6, by the mesh's "slot N" and its
    record index) must equal the draw's VS c203 / c204. This is what front_end.cpp does in OURS (ClockOfSlot(slot_no, src_record)) with the speeds of the same mesh."""
    c = collections.Counter()
    bad = {}
    reg = Registry(os.path.join(L.ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    for fi in frames:
        f = cap.frame(fi)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] != L.SITE_A:
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            if not vn or 'UVScro' not in vn:
                continue
            vb = f.bank(d['vs_bank'])
            sp = tuple(fl(vb[4 * r + k]) for r in (203, 204) for k in range(4))
            rows = tab.get(f.guest_frame - off, {}).get(o['object'])
            if not rows:
                c['no_row'] += 1
                continue
            slot = rows[0]['slot'] + (1 if 'slot_off' in plants else 0)
            want = speeds.get((slot, o['part']))
            if want is None:
                c['no_mesh'] += 1
                bad.setdefault((hex(o['object']), slot, o['part']), 0)
            elif all(abs(a - b) <= 1e-4 for a, b in zip(sp, want)):
                c['equal'] += 1
            else:
                c['differ'] += 1
                bad.setdefault((hex(o['object']), slot, o['part']), 0)
    return c, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', default='')
    ap.add_argument('--expect-fail', action='store_true', help='a planted twin: exit 0 only when the verdict is FAIL')
    ap.add_argument('--expect-equal', type=float, default=None)
    ap.add_argument('--min-draws', type=int, default=1)
    ap.add_argument('--speeds', default='', help='uvscroll_check.exe output (arena 4): turns on the slot check')
    x = ap.parse_args(argv)
    plants = (x.plant,) if x.plant else ()
    if x.plant and x.plant not in PLANTS:
        print('FAIL unknown plant %s' % x.plant)
        return 1
    try:
        cap = L.Capture(x.capture)
        if not cap.has_scene:
            print('FAIL  %s has no scene folder' % x.capture)
            return 1
        frames = L.parse_frames(x.frames, cap.n)
        tab = build_table(part_clock_full(cap.record(i)) for i in range(cap.n) if cap.record(i) is not None)
        if not tab:
            print('FAIL  no record carries kScPartClocks (section 50)')
            return 1
        reg = Registry(os.path.join(L.ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
        draws = []
        meta = {}
        for fi in frames:
            f = cap.frame(fi)
            for i in range(f.ndraws):
                o = f.origin(i)
                if not o or o['klass'] != 1 or o['pass_site'] not in SITES:
                    continue
                d = f.draw(i)
                vn = reg.variant_of(d['vs'])
                if not vn or not (vn.startswith('yProp_') or vn.startswith('yBG_')):
                    continue
                vb = f.bank(d['vs_bank'])
                gpart = 0 if 'part0' in plants else o['part']
                draws.append((f.guest_frame, o['object'], gpart, vb[4 * 186]))
                meta.setdefault(o['object'], set()).add(vn)
        off, votes = best_offset(draws, tab, plants)
        if 'offset0' in plants:
            off = 0
        print('draws %d (arena main-pass), objects %d, record stamps %d' % (len(draws), len(meta), len(tab)))
        print('hook-to-stream offset (stream guest frame - row stamp), votes: %s' % ', '.join('%+d: %d' % (o, votes[o]) for o in sorted(votes)))
        if off is None:
            print('FAIL  no unique best offset: nothing is joined blind')
            return 1
        print('measured offset: %+d' % off)
        res = join(draws, tab, off, plants)
    except (L.LedgerError, OSError, KeyError, ValueError) as e:
        print('FAIL  %s: %s' % (x.capture, e))
        return 1
    tot = collections.Counter()
    print('per object: equal / equal_x2 / differ / unjoined (variants)')
    for obj in sorted(res):
        C = res[obj]
        tot.update(C)
        if C['differ'] or C['unjoined'] or C['equal_x2']:
            print('  %08x  eq %5d  x2 %5d  differ %5d  unjoined %5d  %s' % (obj, C['equal'], C['equal_x2'], C['differ'], C['unjoined'], ','.join(sorted(meta[obj]))[:60]))
    n_all_eq = sum(1 for C in res.values() if C['equal'] and not (C['differ'] or C['unjoined'] or C['equal_x2']))
    print('objects: %d all-equal, %d with equal_x2/differ/unjoined' % (n_all_eq, len(res) - n_all_eq))
    nd = sum(tot.values())
    joined = tot['equal'] + tot['equal_x2'] + tot['differ']
    print('TOTAL draws %d: equal %d, equal_x2 %d, differ %d, unjoined %d (equal of joined: %.2f%%)' % (nd, tot['equal'], tot['equal_x2'], tot['differ'], tot['unjoined'], 100.0 * tot['equal'] / max(1, joined)))
    bad = 0
    multi = sum(1 for f in tab.values() for o in f.values() if len(o) > 1)
    print('part runs: %d (frame, object) sets have more than one row; %s' % (multi, 'the part index is tested' if multi else 'every model has ONE clock run in this case: the part index (r16) is NOT tested here'))
    if tot['differ'] or not tot['equal']:
        print('FAIL  c186 differs from the joined row on %d draws (equal %d)' % (tot['differ'], tot['equal']))
        bad += 1
    if nd < x.min_draws:
        print('FAIL  fewer than %d draws' % x.min_draws)
        bad += 1
    if x.expect_equal is not None and 100.0 * tot['equal'] / max(1, nd) < x.expect_equal:
        print('FAIL  equal share of ALL draws %.2f%% < %.2f%%' % (100.0 * tot['equal'] / max(1, nd), x.expect_equal))
        bad += 1
    if x.speeds:
        sc, sbad = slot_check(cap, frames, tab, off, read_speeds(x.speeds), plants)
        print('SLOT join (UVScroll draw object -> row slot, (slot, part) -> loader speeds vs c203/c204): equal %d, differ %d, no_mesh %d, no_row %d' % (sc['equal'], sc['differ'], sc['no_mesh'], sc['no_row']))
        for k in list(sbad)[:8]:
            print('   object %s slot %d part %d' % k)
        if sc['differ'] or sc['no_mesh'] or sc['no_row'] or not sc['equal']:   # review A2: a UVScroll draw with no row is a failure, not a skip
            bad += 1
    print('part_clock_join: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return (0 if bad else 1) if x.expect_fail else (1 if bad else 0)


if __name__ == '__main__':
    sys.exit(main())

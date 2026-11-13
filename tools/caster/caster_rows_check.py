r"""S101 lane 202 (Caster D3 E0-E3 reader side, docs\renderer\SKIP_GAME_WORK_MAP.md 17.7): the caster pass entries against the stream. Offline, 0 game runs, read-only.

  py -I tools\caster\caster_rows_check.py --census CAPTURE [--frames A:B:STEP] [--plant colour_off|offset0]
  py -I tools\caster\caster_rows_check.py --zip CASE [--frames A:B:STEP] [--plant camera_prev_frame|wrong_offset0|no_ordinal]

--census (kept captures; they hold NO pass-entry records, 17.4 item 3): the entry structure the reader will see, read from the stream the way the hooks will meet it (a caster draw's pass site names its pass; a run
of draws of one object and one pass, restarted when a part repeats, is ONE entry; the k-th entry of (pass, object) in a frame is its ordinal), plus the ledger's readiness numbers through the reader's own
sources (tools\skip\caster_ledger.py: trivial / pass bank [= the entry's camera] / record + colour [= the wrestler record's palette]). Prints entries a frame by pass, map ordinal 0 / 1 (map A / B) and the
share of draws ready; with --plant colour_off (the palette source off) or offset0 (records read at offset 0, lane 36's +1) it prints the drop the selftest asserts.
--zip (the ONE live verify run's case zip unpacked: frames + scene records with kScCasterCalls / kScCasterStates; NOT runnable until that run exists): for every stream entry it takes the CasterCall of (hook
frame = stream frame - offset, pass, object, ordinal), and compares the entry's camera state with the stream's registers (map / rim: VS c56-c59 of the entry's first draw, either matrix orientation, which of
the three shadow matrices it equals is VOTED: R+912 ViewProj, +976, +1040 Trapezoidal are the layout of 17.2 item 2, the mapping of the rim pass is [U]); verdicts per entry: equal, camera (differs), no_state,
absent. The hook-to-stream offset is measured (-2..+3, unique best, +1 expected). Plants (tests only): camera_prev_frame (the camera of the entry of the frame before), wrong_offset0, no_ordinal.
The pure parts (stream_entries, build_table, compare_entry, vote_offset) are unit-tested by tools\caster\caster_rows_selftest.py on synthetic data."""
import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
sys.path.insert(0, HERE)

PLANTS_ZIP = ('camera_prev_frame', 'wrong_offset0', 'no_ordinal')
PLANTS_CENSUS = ('colour_off', 'offset0')
OFFSETS = range(-2, 4)
PASS_OF_SUB = {'map': 1, 'plane': 2, 'rim': 3}
PASS_NAME = {1: 'map', 2: 'plane', 3: 'rim'}
QUAD_BASE = {1: 56, 2: 13, 3: 56}     # VS ViewProj c56-c59 (map, rim); the plane's CameraViewProj c13-c16 (SHADOW_PLANE.md)


def stream_entries(draws, no_ordinal=False):
    """draws: iterable of dict(pass_, object, part, quad) in stream order. -> [dict(pass_, object, ordinal, first_quad, parts, draws)].
    One entry = a run of consecutive draws of one (pass, object), restarted when a part repeats (map A then map B of one object back to back)."""
    out, cur, seen_parts = [], None, set()
    ords = collections.Counter()
    for d in draws:
        key = (d['pass_'], d['object'])
        if cur is None or (cur['pass_'], cur['object']) != key or d['part'] in seen_parts:
            cur = dict(pass_=key[0], object=key[1], ordinal=0 if no_ordinal else ords[key], first_quad=d['quad'], parts=[], draws=0)
            ords[key] += 1
            out.append(cur)
            seen_parts = set()
        cur['parts'].append(d['part'])
        cur['draws'] += 1
        seen_parts.add(d['part'])
    return out


def build_table(records):
    """records: iterable of Record (or objects with .caster_calls()). -> {hook_frame: {(pass, object): [calls sorted by ordinal]}}"""
    t = collections.defaultdict(lambda: collections.defaultdict(list))
    for rec in records:
        if rec is None:
            continue
        for c in rec.caster_calls():
            t[c['frame']][(c['pass_'], c['object'])].append(c)
    for fr in t.values():
        for lst in fr.values():
            lst.sort(key=lambda c: c['ordinal'])
    return t


def quads_of(camera):
    """The (matrix index, orientation) -> 4 vec4 of a camera state's three shadow matrices: orientation 'r' = rows of the flat 16 floats, 'c' = columns."""
    out = {}
    for k, m in enumerate(camera['shadow_m']):
        out[(k, 'r')] = tuple(tuple(m[4 * r:4 * r + 4]) for r in range(4))
        out[(k, 'c')] = tuple(tuple(m[c::4]) for c in range(4))
    return out


def same_f32(a, b):
    return all(struct.pack('<f', x) == struct.pack('<f', y) for ra, rb in zip(a, b) for x, y in zip(ra, rb))


def compare_entry(call, quad):
    """-> (verdict, matches): 'absent' (no call), 'no_state' (the call holds no camera), 'equal' (the stream's quad equals one of the call's shadow matrices, matches = the set of (k, orientation)), 'camera'."""
    if call is None:
        return 'absent', ()
    if call['camera'] is None:
        return 'no_state', ()
    m = tuple(k for k, q in quads_of(call['camera']).items() if same_f32(q, quad))
    return ('equal', m) if m else ('camera', ())


def vote_offset(entries_by_frame, table, plants=()):
    """entries_by_frame {stream_frame: [stream entries]}; -> (best offset or None when not unique, votes {offset: entries that find a call of that hook frame, pass, object, ordinal})."""
    votes = collections.Counter({o: 0 for o in OFFSETS})
    for s, ents in entries_by_frame.items():
        for e in ents:
            for o in OFFSETS:
                lst = table.get(s - o, {}).get((e['pass_'], e['object']), [])
                if any(c['ordinal'] == e['ordinal'] for c in lst):
                    votes[o] += 1
    best = votes.most_common(2)
    return (best[0][0] if best[0][1] > 0 and (len(best) < 2 or best[0][1] > best[1][1]) else None), votes


def join_call(table, frame, off, e, plants=()):
    f = frame - off
    if 'camera_prev_frame' in plants:
        f -= 1   # planted: the camera of the entry one frame older
    lst = table.get(f, {}).get((e['pass_'], e['object']), [])
    for c in lst:
        if c['ordinal'] == e['ordinal']:
            return c
    return None


def run_zip(path, frames_spec, plants):
    import ledger as L
    from ledger_io import CLASS_NAMES
    import caster_ledger as CL
    cap = L.Capture(path)
    if not cap.has_scene:
        print('FAIL  %s has no scene records' % path)
        return 1
    reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = L.parse_frames(frames_spec, cap.n)
    table = build_table(cap.record(i) for i in range(cap.n))
    if not table:
        print('FAIL  no record of this capture carries kScCasterCalls (was the run made with --wwe2k14_rdr_caster_d3?)')
        return 1
    by_frame = {}
    for fi in frames:
        f = cap.frame(fi)
        draws = []
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o:
                continue
            d = f.draw(i)
            sub = CL.sub_class(CLASS_NAMES[o['klass']], o, reg.variant_of(d['vs']), 'casters')
            if sub not in PASS_OF_SUB:
                continue
            p = PASS_OF_SUB[sub]
            vb = f.bank(d['vs_bank'])
            b = QUAD_BASE[p]
            quad = tuple(tuple(vb[4 * (b + r):4 * (b + r) + 4]) for r in range(4))
            draws.append(dict(pass_=p, object=o['object'], part=o['part'], quad=quad))
        by_frame[f.guest_frame] = stream_entries(draws, 'no_ordinal' in plants)
    off, votes = vote_offset(by_frame, table)
    print('hook-to-stream offset votes: %s -> %s' % (dict(votes), off))
    if off is None:
        print('FAIL  the offset is not unique')
        return 1
    if 'wrong_offset0' in plants:
        off = 0
    res = {p: collections.Counter() for p in PASS_NAME}
    mvotes = {p: collections.Counter() for p in PASS_NAME}
    collapse = collections.Counter()
    for s, ents in sorted(by_frame.items()):
        for e in ents:
            c = join_call(table, s, off, e, plants)
            v, m = compare_entry(c, e['first_quad'])
            res[e['pass_']][v] += 1
            for k in m:
                mvotes[e['pass_']][k] += 1
            if c is not None and e['pass_'] == 1:
                collapse[(e['ordinal'], c['collapse'])] += 1
    bad = 0
    for p, c in res.items():
        tot = sum(c.values())
        print('%-5s entries %6d  equal %6d  camera %6d  no_state %6d  absent %6d   matrix votes %s' % (PASS_NAME[p], tot, c['equal'], c['camera'], c['no_state'], c['absent'], dict(mvotes[p])))
        bad += c['camera'] + c['no_state'] + c['absent']
    print('map entries by (ordinal, collapse flag): %s' % dict(collapse))
    print('PASS' if bad == 0 else 'FAIL  %d entries without an equal camera (named above)' % bad)
    return 0 if bad == 0 else 1


def run_census(path, frames_spec, plants):
    import ledger as L
    from ledger_io import CLASS_NAMES
    import caster_ledger as CL
    cap = L.Capture(path)
    reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = L.parse_frames(frames_spec, cap.n)
    per = {p: [] for p in PASS_NAME}
    ord01 = {1: collections.Counter()}
    draws_per_entry = {p: [] for p in PASS_NAME}
    for fi in frames:
        f = cap.frame(fi)
        draws = []
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o:
                continue
            d = f.draw(i)
            sub = CL.sub_class(CLASS_NAMES[o['klass']], o, reg.variant_of(d['vs']), 'casters')
            if sub not in PASS_OF_SUB:
                continue
            p = PASS_OF_SUB[sub]
            vb = f.bank(d['vs_bank'])
            b = QUAD_BASE[p]
            draws.append(dict(pass_=p, object=o['object'], part=o['part'], quad=tuple(tuple(vb[4 * (b + r):4 * (b + r) + 4]) for r in range(4))))
        ents = stream_entries(draws)
        cnt = collections.Counter(e['pass_'] for e in ents)
        for p in PASS_NAME:
            per[p].append(cnt[p])
        for e in ents:
            draws_per_entry[e['pass_']].append(e['draws'])
            if e['pass_'] == 1:
                ord01[1][min(e['ordinal'], 2)] += 1
    ledger = CL.run(path, 'casters', frames_spec, 1, tuple(plants))
    out = {}
    for p, name in PASS_NAME.items():
        n = len(per[p])
        out[name] = dict(entries_per_frame=sum(per[p]) / n if n else 0.0, draws_per_entry=(sum(draws_per_entry[p]) / len(draws_per_entry[p])) if draws_per_entry[p] else 0.0)
        s = ledger['sub'].get(name)
        out[name]['ready_pct'] = 100.0 * s['ready'] / s['draws'] if s and s['draws'] else None
        out[name]['draws_per_frame'] = s['per_frame'] if s else 0.0
    out['map_ordinals'] = dict(ord01[1])
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description='caster pass entries against the stream (S101 lane 202)')
    ap.add_argument('--census')
    ap.add_argument('--zip')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', action='append', default=[])
    a = ap.parse_args(argv)
    try:
        if a.census:
            bad = [p for p in a.plant if p not in PLANTS_CENSUS]
            if bad:
                print('FAIL  unknown plant for --census: %s' % bad)
                return 1
            r = run_census(a.census, a.frames, a.plant)
            for name in ('map', 'plane', 'rim'):
                x = r[name]
                print('%-5s entries a frame %6.1f  draws a frame %6.1f  draws an entry %5.1f  ready %s' % (name, x['entries_per_frame'], x['draws_per_frame'], x['draws_per_entry'], '%.1f%%' % x['ready_pct'] if x['ready_pct'] is not None else '-'))
            print('map entries by ordinal (0 = map A or a props object, 1 = map B, 2 = more): %s' % r['map_ordinals'])
            return 0
        if a.zip:
            bad = [p for p in a.plant if p not in PLANTS_ZIP]
            if bad:
                print('FAIL  unknown plant for --zip: %s' % bad)
                return 1
            return run_zip(a.zip, a.frames, tuple(a.plant))
    except (OSError, ValueError, KeyError, IndexError) as e:
        print('FAIL  %s: %s' % (type(e).__name__, e))
        return 1
    except Exception as e:   # the ledger's own LedgerError
        print('FAIL  %s: %s' % (type(e).__name__, e))
        return 1
    ap.print_help()
    return 1


if __name__ == '__main__':
    sys.exit(main())

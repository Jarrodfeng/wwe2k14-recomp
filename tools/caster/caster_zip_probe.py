#!/usr/bin/env python3
"""Lane 211 (Caster D3): what a live verify zip says about the caster palette and camera sources. Offline, read-only, numbers only.
  py -I tools\\caster\\caster_zip_probe.py CASE_DIR [--frames 100:340:40] [--calls-offset 0] [--bones-offsets -1,0,1]
CASE_DIR = the unpacked case zip of a run with --wwe2k14_rdr_caster_d3 (it has kScCasterCalls and the wrestler bones). For each sampled stream frame i the CALLS come from record (i + calls-offset)
[run 7138: offset 0 = the record that carries the calls stamped guest(i) - 1] and the BONES are looked up SEPARATELY in record (i + bones-offset) for every listed bones offset (-1 = the record with header
guest(i) - 1, the call's stamp):
  palette  for every draw at a colour site (0x82C0A950 / 0x82C0B94C), the map site (0x82C04634), the rim site (0x82BFBD58) and the plane site (0x82BF4D38): how many of its 21 palette slots (VS c76 + 3k ..
           +2) equal, bit for bit, the three rows of SOME bone of the bones record. Printed as draws / slots equal / slots possible (draws x 21), per bones offset.
  hash     for every CasterCall with a palette (n_nodes > 0): does the FNV-1a 64 of the first min(n_nodes, 96) 64-byte matrices of ANY object of the bones record equal CasterCall.pal_hash
           (caster_core.h HashPalette, host floats' bytes; a zero hash is 1)? Printed per bones offset.
Lane 217: a record written by a build with the per-node hash (CasterCall flag 256, section 24) also gets a NODES report per bones offset: for every call with node hashes, the entry's node k hash against the
same-object bone k hash of the bones record (FNV-1a 32 of the 64 matrix bytes, caster_core.h HashNode; object = the call's engine object, else NONE and counted). Printed: calls equal / differing /
without bones, the nodes that differ (per node index, as ranges) and per object the ranges that differ in at least half of its calls. `--selftest` runs it on a synthetic pair (a planted difference
in nodes 60 and 71 must be located exactly) without any capture.
Run 7138 (case_20261113_065857_672, frames 100:340:40, calls-offset 0): rim slots 6120 / 6426 at bones offset -1 and 0 at 0 and +1; colour 29407 (-1) against 8485 (0, +1); map 7536 (-1) against 1284 (0) and
84 (+1); plane 0; hash equal 0 of 30 calls at every bones offset (SKIP_GAME_WORK_MAP.md 17.8b; reproduced by this tool, output kept in the lane report). The camera check is tools\\caster\\caster_cam_vectors.py --offset 0 + d3_caster_test.exe cam."""
import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
import ledger as L  # noqa: E402

KINDS = {0x82C0A950: 'colour', 0x82C0B94C: 'colour', 0x82C04634: 'map', 0x82BFBD58: 'rim', 0x82BF4D38: 'plane'}


def fnv_palette(mats):
    h = 0xCBF29CE484222325
    for m in mats:
        for x in struct.pack('<16f', *m):
            h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h or 1


def fnv32_mat(m):
    h = 2166136261
    for x in struct.pack('<16f', *m):
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def ranges(idx):
    out, s = [], sorted(idx)
    i = 0
    while i < len(s):
        j = i
        while j + 1 < len(s) and s[j + 1] == s[j] + 1:
            j += 1
        out.append('%d' % s[i] if i == j else '%d-%d' % (s[i], s[j]))
        i = j + 1
    return ','.join(out) or '-'


def node_compare(calls, bones, objs, engine, cnt):
    """calls = caster_calls() dicts with node_hashes; bones/objs/engine = the bones record's bones(), objects(), engine_objects(). Adds to cnt:
    ('calls',) ('equal',) ('differ',) ('no_object',) ('short',) ('node', k) = calls where node k differs, ('obj', engine, k) the same per object, ('obj_calls', engine)."""
    by_eng = {e: o for e, o in zip(engine, objs)}
    for c in calls:
        nh = c.get('node_hashes')
        if not nh:
            continue
        cnt[('calls',)] += 1
        o = by_eng.get(c['object'])
        if o is None or o['bones_count'] == 0:
            cnt[('no_object',)] += 1
            continue
        n = min(len(nh), o['bones_count'])
        if n < len(nh):
            cnt[('short',)] += 1
        cnt[('obj_calls', c['object'])] += 1
        bad = [k for k in range(n) if nh[k] != fnv32_mat(bones[o['bones_first'] + k])]
        cnt[('differ' if bad else 'equal',)] += 1
        for k in bad:
            cnt[('node', k)] += 1
            cnt[('obj', c['object'], k)] += 1


def print_nodes(off, cnt):
    print('  NODES bones offset %+d: calls with node hashes %d, equal %d, differ %d, object not in the bones record %d, record has fewer bones than the call has nodes %d' %
          (off, cnt[('calls',)], cnt[('equal',)], cnt[('differ',)], cnt[('no_object',)], cnt[('short',)]))
    nodes = sorted(k[1] for k in cnt if k[0] == 'node')
    print('    nodes that differ in at least one call: %s' % ranges(nodes))
    for e in sorted({k[1] for k in cnt if k[0] == 'obj'}):
        n = cnt[('obj_calls', e)]
        half = [k[2] for k in cnt if k[0] == 'obj' and k[1] == e and cnt[k] * 2 >= n]
        print('    object 0x%08X: %d calls, nodes differing in >= half of them: %s' % (e, n, ranges(half)))


def selftest():
    mats = [[float(100 * j + i) for i in range(16)] for j in range(100)]
    bones = [list(m) for m in mats]
    objs = [dict(bones_first=0, bones_count=100)]
    entry = [list(m) for m in mats]
    for k in (60, 71):
        entry[k] = list(entry[k])
        entry[k][3] += 1.0   # planted: the entry's palette differs from the record's bones at nodes 60 and 71
    calls = [dict(object=0xA0, node_hashes=[fnv32_mat(m) for m in entry[:96]])] * 3
    cnt = collections.Counter()
    node_compare(calls, bones, objs, [0xA0], cnt)
    got = sorted(k[1] for k in cnt if k[0] == 'node')
    ok = got == [60, 71] and cnt[('node', 60)] == 3 and cnt[('differ',)] == 3 and cnt[('equal',)] == 0
    cnt2 = collections.Counter()
    node_compare([dict(object=0xA0, node_hashes=[fnv32_mat(m) for m in mats[:96]])], bones, objs, [0xA0], cnt2)
    ok = ok and cnt2[('equal',)] == 1 and not [k for k in cnt2 if k[0] == 'node']
    print('probe selftest: planted nodes 60 and 71 located as %s, an unplanted call equal: %s' % (ranges(got), 'PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


def bone_rows(rec):
    rows = set()
    for b in rec.bones():
        for j in range(3):
            rows.add(tuple(struct.unpack('<I', struct.pack('<f', b[c * 4 + j]))[0] for c in range(4)))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('--frames', default='100:340:40')
    ap.add_argument('--calls-offset', type=int, default=0)
    ap.add_argument('--bones-offsets', default='-1,0,1')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        selftest()
    cap = L.Capture(os.path.join(a.case, 'capture') if os.path.isdir(os.path.join(a.case, 'capture')) else a.case)
    lo, hi, st = [int(x) for x in a.frames.split(':')]
    offs = [int(x) for x in a.bones_offsets.split(',')]
    tot = collections.Counter()
    ncnt = {off: collections.Counter() for off in offs}
    for fi in range(lo, hi, st):
        f = cap.frame(fi)
        calls = [c for c in cap.record(fi + a.calls_offset).caster_calls() if c['n_nodes']]
        for off in offs:
            rec = cap.record(fi + off)
            node_compare(calls, rec.bones(), rec.objects(), rec.engine_objects(), ncnt[off])
            rows = bone_rows(rec)
            bones, objs = rec.bones(), rec.objects()
            for i in range(f.ndraws):
                o = f.origin(i)
                kind = KINDS.get(o['pass_site'])
                if not kind or not o['object']:
                    continue
                vb = f.bank(f.draw(i)['vs_bank'])
                n = 0
                for s in range(21):
                    n += all(tuple(vb[(76 + 3 * s + j) * 4 + k] & 0xFFFFFFFF for k in range(4)) in rows for j in range(3))
                tot[(off, kind, 'draws')] += 1
                tot[(off, kind, 'slots_equal_a_bone')] += n
                tot[(off, kind, 'slots_possible')] += 21
            for c in calls:
                nn = min(c['n_nodes'], 96)
                hit = False
                for ob in objs:
                    if ob['bones_count'] >= nn:
                        if fnv_palette([bones[ob['bones_first'] + k] for k in range(nn)]) == c['pal_hash']:
                            hit = True
                            break
                tot[(off, 'hash', 'equal_some_object' if hit else 'equal_no_object')] += 1
    print('bones offset, kind, counter -> value (calls from record i%+d):' % a.calls_offset)
    for k in sorted(tot, key=lambda t: (t[0], str(t[1]), t[2])):
        print('  ', k, tot[k])
    for off in offs:
        if ncnt[off][('calls',)]:
            print_nodes(off, ncnt[off])


if __name__ == '__main__':
    main()

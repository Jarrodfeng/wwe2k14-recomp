#!/usr/bin/env python3
r"""Lane 221 (docs\renderer\SKIP_GAME_WORK_MAP.md 17.10): the CasterExtra table (scene record section 25) against the STREAM of the frame each call belongs to. 0 game runs, numbers only.

  py -I tools\caster\caster_extras_check.py CAPTURE [--frames A:B:STEP] [--emulate] [--plant NAME]

What a CasterExtra says (SKIP_GAME_WORK_MAP.md 17.10), and what this tool compares for every sampled record, every caster call that carries one, against the capture frame the call belongs to (stamp + 1,
the +1 pairing of lanes 152 / 211):
  rim call (pass 3)      product -> the rim draws' VS c56-c59: register c(56 + i) component k == product[4 k + i] (the twin transposes the product, listing 0x82BD8C24-0x82BD8C64); depth -> c162.xy.
                         Every rim draw of the frame must carry the same camera (the rim pass renders from the main camera, one per frame) [C: run 7147, 51 of 51 draws].
  map call with collapse  pose_t -> every COLLAPSED palette row of the object's map-A draws is (0, 0, 0, t_j) (rows j = 0..2; a zero is a zero: +0.0 and -0.0 both count); at least one such row must exist.
  every call with a product, a state and the R camera (needs the record's caster states): the product against the camera rows built from R: informational (maps: equal expected, the rim: differ).
Verdicts: equal / differ per class; the exit code is 1 when anything differs or no call carries an extra (a record that predates lane 221).

--emulate  build the extras FROM THE STREAM first (what fview's D3_EMULATE_EXTRAS does), then compare: the tool's own compare and layout are self-tested (equal expected) and --plant breaks the emulation:
           product_next_frame (the product of the NEXT frame's rim draws: the wrong-frame camera), product_transposed (c56 + i taken as ROW i), pose_row2 (row 2 of the pose instead of row 3's value) -> differ > 0.
The live reader itself is proved by caster_rows_test.exe (hook.extra_per_entry, hook.pose_point) and, live, by the verify run's census `product_eq_r` (maps equal to R's camera).
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'skip'))
import ledger as L  # noqa: E402

SITE_MAP, SITE_RIM = 0x82C04634, 0x82BFBD58
PLANTS = ('product_next_frame', 'product_transposed', 'pose_row2')


def fl(w):
    return struct.unpack('<f', struct.pack('<I', w & 0xFFFFFFFF))[0]


def is_zero(w):
    return (w & 0x7FFFFFFF) == 0


def rim_draws(f, obj=None):
    out = []
    for i in range(f.ndraws):
        o = f.origin(i)
        if o and o['pass_site'] == SITE_RIM and (obj is None or o['object'] == obj):
            out.append(i)
    return out


def emulate(cap, base, calls, plant):
    """The extras the hook WOULD read, built from the stream (the same rules as fview's EmulateCasterExtras): [{flags, product, depth, pose_t}] parallel to calls."""
    res = []
    for c in calls:
        x = dict(flags=0, product=[0.0] * 16, depth=[0.0, 0.0], pose_t=[0.0] * 4)
        idx = c['frame'] + 1 - base + (1 if plant == 'product_next_frame' and c['pass_'] == 3 else 0)
        if 0 <= idx < cap.n:
            f = cap.frame(idx)
            if c['pass_'] == 3:
                ds = rim_draws(f, c['object']) or rim_draws(f)
                if ds:
                    vb = f.bank(f.draw(ds[0])['vs_bank'])
                    for i in range(4):
                        for k in range(4):
                            if plant == 'product_transposed':
                                x['product'][i * 4 + k] = fl(vb[(56 + i) * 4 + k])
                            else:
                                x['product'][k * 4 + i] = fl(vb[(56 + i) * 4 + k])
                    x['depth'] = [fl(vb[162 * 4]), fl(vb[162 * 4 + 1])]
                    x['flags'] |= 1 | 2
            elif c['pass_'] == 1 and c['collapse']:
                for i in range(f.ndraws):
                    o = f.origin(i)
                    if not o or o['pass_site'] != SITE_MAP or o['object'] != c['object'] or (x['flags'] & 4):
                        continue
                    vb = f.bank(f.draw(i)['vs_bank'])
                    for slot in range(21):
                        rows = [vb[(76 + 3 * slot + j) * 4:(76 + 3 * slot + j) * 4 + 4] for j in range(3)]
                        if all(is_zero(rows[j][k]) for j in range(3) for k in range(3)):
                            x['pose_t'] = [fl(rows[j][3]) for j in range(3)] + [1.0]
                            if plant == 'pose_row2':
                                x['pose_t'][0] += 1.0
                            x['flags'] |= 4
                            break
        res.append(x)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--emulate', action='store_true')
    ap.add_argument('--plant', default='', choices=('',) + PLANTS)
    a = ap.parse_args()
    cap = L.Capture(a.capture)
    base = cap.frame(0).guest_frame
    lo, hi, step = 40, cap.n - 3, 20
    if a.frames:
        p = [int(v) for v in a.frames.split(':')]
        lo, hi, step = p[0], p[1], p[2] if len(p) > 2 else 20
    tally = {k: [0, 0] for k in ('rim_product', 'rim_depth', 'rim_one_camera', 'map_a_pose')}
    any_extra = False
    r_vs_product = {1: [0, 0], 2: [0, 0], 3: [0, 0]}
    for ri in range(lo, min(hi, cap.n), step):
        rec = cap.record(ri)
        if rec is None:
            continue
        calls = rec.caster_calls()
        extras = emulate(cap, base, calls, a.plant) if a.emulate else [c.get('extra') for c in calls]
        for c, x in zip(calls, extras):
            if not x or not x['flags']:
                continue
            any_extra = True
            idx = c['frame'] + 1 - base
            if not 0 <= idx < cap.n:
                continue
            f = cap.frame(idx)
            if c['pass_'] == 3 and x['flags'] & 1:
                ds = rim_draws(f)
                cams = set()
                for d in ds:
                    vb = f.bank(f.draw(d)['vs_bank'])
                    cams.add(tuple(vb[56 * 4:60 * 4]))
                    ok = all(vb[(56 + i) * 4 + k] == struct.unpack('<I', struct.pack('<f', x['product'][4 * k + i]))[0] for i in range(4) for k in range(4))
                    tally['rim_product'][0 if ok else 1] += 1
                    if x['flags'] & 2:
                        okd = vb[162 * 4] == struct.unpack('<I', struct.pack('<f', x['depth'][0]))[0] and vb[162 * 4 + 1] == struct.unpack('<I', struct.pack('<f', x['depth'][1]))[0]
                        tally['rim_depth'][0 if okd else 1] += 1
                tally['rim_one_camera'][0 if len(cams) <= 1 else 1] += 1
            if c['pass_'] == 1 and c['collapse'] and x['flags'] & 4:
                seen = bad = 0
                for i in range(f.ndraws):
                    o = f.origin(i)
                    if not o or o['pass_site'] != SITE_MAP or o['object'] != c['object']:
                        continue
                    vb = f.bank(f.draw(i)['vs_bank'])
                    for slot in range(21):
                        rows = [vb[(76 + 3 * slot + j) * 4:(76 + 3 * slot + j) * 4 + 4] for j in range(3)]
                        if all(is_zero(rows[j][k]) for j in range(3) for k in range(3)):
                            seen += 1
                            bad += not all(fl(rows[j][3]) == x['pose_t'][j] for j in range(3))
                    if seen:
                        break   # the first draw of the object's map-A run that has collapsed rows decides (one point per call)
                # a slot past the part's own list holds the PREVIOUS draw's rows (stale, possibly another object's collapse point): the point must be the one of MORE THAN HALF of the collapsed rows
                tally['map_a_pose'][0 if seen and (seen - bad) * 2 > seen else 1] += 1
            if c.get('camera') and x['flags'] & 1:
                import math  # noqa: F401
                m = c['camera']['shadow_m'][0]
                eq = all(m[k * 4 + i] == x['product'][k * 4 + i] for i in range(4) for k in range(4))
                r_vs_product[c['pass_']][0 if eq else 1] += 1
    print('CasterExtra check on %s (%s)' % (a.capture, 'EMULATED from the stream' + (', plant ' + a.plant if a.plant else '') if a.emulate else 'the capture\'s own table'))
    if not any_extra:
        print('FAIL  no call carries a CasterExtra (a record that predates lane 221, or the reader never read)')
        return 1
    bad_total = 0
    for k, (eq, df) in tally.items():
        print('  %-16s equal %d, differ %d' % (k, eq, df))
        bad_total += df
    print('  product vs the R camera (info) map %s plane %s rim %s  [equal, differ]' % (r_vs_product[1], r_vs_product[2], r_vs_product[3]))
    n_eq = sum(v[0] for v in tally.values())
    ok = bad_total == 0 and n_eq > 0
    print('%s  %d comparisons equal, %d differ' % ('PASS' if ok else 'FAIL', n_eq, bad_total))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

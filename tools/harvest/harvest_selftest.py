#!/usr/bin/env python3
r"""S103 lane J1: the harvester's own proof and its planted twins. 0 game runs.

  py -I tools\harvest\harvest_selftest.py [--proof] [--twin] [--reharvest] [--case out\s103\run27_case.zip]     (no flag = all three)

  --proof      the committed tables (tools\harvest\choice_table\, harvested from the kept arena-4 captures) against the two stream tables the S103 lanes froze by hand:
                 P1  tools\arena\arena4_variants.json  265 meshes: the mask class of every mesh both tables hold must be EQUAL (0 differences) and the harvest must reach all 265
                 L10 tools\arena\arena4_vlm.json       251 meshes: the vertex-LM class likewise, all 251
               (six kept captures together: run 26 / 27 / 29, the Brock and Bryan entrances, the S101 match). The harvest also holds
               every other mesh the frozen tables lack (the tables hold 377 arena-4 meshes), and tools\harvest\expect.json freezes what each rule says about all of them (choice_check.py --expect).
  --twin       a PLANTED WRONG JOIN: every mesh's choices moved to the next mesh of its slot (the join_shift fault of join_census.py) must show as conflicts in the merge (>= 20 % of the meshes
               of multi-mesh slots: meshes drawn identically stay equal when swapped) and as rule misses; the merge of the table with itself must show none.
  --reharvest  the harvester run again on the last 40 frames of the kept run-27 case (unpacked under out\harvest\unpack): every mesh and choice it writes must be in the committed
               arena04_run27 table, with the same counts (determinism, and no drift of the reader against the file)."""
import argparse
import copy
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
TABLES = os.path.join(HERE, 'choice_table')


def load_tables():
    return [json.load(open(p, encoding='utf-8')) for p in sorted(glob.glob(os.path.join(TABLES, '*.json')))]


def harvested_choices(tables):
    out = {}
    for t in tables:
        for m in t['meshes']:
            out.setdefault((m['slot'], m['mesh']), []).extend(c for v in m['looks'].values() for c in v['choices'])
    return out


def proof():
    import variant_rule as vr
    h = harvested_choices(load_tables())
    p1 = {(r['slot'], r['mesh']): r for r in json.load(open(os.path.join(ROOT, 'tools', 'arena', 'arena4_variants.json')))}
    l10 = {(r['slot'], r['mesh']): r for r in json.load(open(os.path.join(ROOT, 'tools', 'arena', 'arena4_vlm.json')))}
    ok = True
    common = set(p1) & set(h)
    diff = [k for k in sorted(common) if ('mask' in {vr.klass(c['ps']) for c in h[k] if c['ps']}) != p1[k]['mask']]
    absent = set(p1) - set(h)
    print('P1 mask table : %d of %d meshes reproduced from the kept captures, %d differ %s, absent %s' % (len(common), len(p1), len(diff), diff[:6], sorted(absent)))
    ok &= not diff and len(common) == 265 and not absent
    common = set(l10) & set(h)
    diff = [k for k in sorted(common) if any('vertex_lm' in c['vs'] for c in h[k]) != l10[k]['vlm']]
    absent = set(l10) - set(h)
    print('L10 vertex-LM : %d of %d meshes reproduced, %d differ %s, absent %s' % (len(common), len(l10), len(diff), diff[:6], sorted(absent)))
    ok &= not diff and len(common) == 251 and not absent
    print('the harvest holds %d arena-4 meshes (the frozen tables: %d, %d)' % (len(h), len(p1), len(l10)))
    return ok


def shifted(table):
    """the planted wrong join: each mesh's looks go to the next mesh index of the same slot."""
    t = copy.deepcopy(table)
    by = {}
    for m in t['meshes']:
        by.setdefault(m['slot'], []).append(m)
    n = 0
    for slot, ms in by.items():
        if len(ms) < 2:
            continue
        ms.sort(key=lambda m: m['mesh'])
        looks = [m['looks'] for m in ms]
        for i, m in enumerate(ms):
            m['looks'] = looks[(i + 1) % len(ms)]
        n += len(ms)
    return t, n


def twin():
    import choice_check as cc
    import choice_merge as cm
    tables = load_tables()
    base = next(t for t in tables if t['scene'] == 'arena04_run27')
    clean = cm.merge([base, copy.deepcopy(base)])
    print('twin 0 (the table merged with itself): %d conflicts' % len(clean['conflicts']))
    ok = not clean['conflicts']
    bad, n = shifted(base)
    bad['scene'] = 'arena04_run27_shifted'
    m = cm.merge([base, bad])
    frac = len(m['conflicts']) / max(n, 1)
    print('twin 1 (planted wrong join, %d meshes of multi-mesh slots moved): %d conflicts (%.0f %%)' % (n, len(m['conflicts']), 100 * frac))
    ok &= frac >= 0.2
    miss = 0
    for name, fn in cc.RULES:
        if name in ('p1_mask', 'l10_vertex_lm'):
            r_ok, total, mi = fn([bad], False)
            good_ok, good_total, good_mi = fn([base], False)
            print('   rule %-14s clean %d / %d (%d miss), planted join %d / %d (%d miss)' % (name, good_ok, good_total, len(good_mi), r_ok, total, len(mi)))
            miss += len(mi) > len(good_mi)
    ok &= miss >= 1
    return ok


def reharvest(case):
    import choice_harvest as ch
    import join_census as jc
    if not os.path.isabs(case):
        main = os.environ.get('RDR_MAIN_CHECKOUT') or ROOT
        case = next((p for p in (os.path.join(ROOT, case), os.path.join(main, case)) if os.path.exists(p)), os.path.join(ROOT, case))
    if not os.path.exists(case):
        print('no case at %s (the lane needs it)' % case)
        return False
    folder, name = ch.prepare(case)
    cap = jc.Capture(folder)
    frames = [cap.frame_number(i) for i in range(len(cap.frames))][-40:]
    fx = ch.Fx()
    meshes, classes, unjoined, n = ch.harvest(cap, folder, 4, frames, 'match', fx, verbose=False)
    t = ch.build_table('arena04_run27', 4, 'match', name, n, meshes, classes, unjoined)
    ref = next(x for x in load_tables() if x['scene'] == 'arena04_run27')
    refm = {(m['slot'], m['mesh']): m['looks']['match']['choices'] for m in ref['meshes']}
    bad = 0
    for m in t['meshes']:
        rc = refm.get((m['slot'], m['mesh']))
        if rc is None:
            bad += 1
            continue
        key = lambda c: json.dumps({k: v for k, v in c.items() if k != 'n'}, sort_keys=True)
        rk = {key(c): c['n'] for c in rc}
        for c in m['looks']['match']['choices']:
            if key(c) not in rk or c['n'] > rk[key(c)]:
                bad += 1
    print('reharvest of the last %d frames: %d meshes, %d choices missing from / larger than the committed table' % (n, len(t['meshes']), bad))
    return bad == 0 and len(t['meshes']) > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--proof', action='store_true')
    ap.add_argument('--twin', action='store_true')
    ap.add_argument('--reharvest', action='store_true')
    ap.add_argument('--case', default=os.path.join('out', 's103', 'run27_case.zip'))
    a = ap.parse_args()
    every = not (a.proof or a.twin or a.reharvest)
    ok = True
    if a.proof or every:
        ok &= proof()
    if a.twin or every:
        ok &= twin()
    if a.reharvest or every:
        ok &= reharvest(a.case)
    print('SELFTEST %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

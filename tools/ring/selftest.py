r"""Lane RJ acceptance test (S101; offline, 0 game runs): the ring join census and the files-only ring census.

  py -I tools\ring\selftest.py [--root DIR]

Every process step runs the SHIPPED command line (tools\ring\ring_census.py as a process), never a copy of its logic; the unit steps call the shipped functions on bytes of the real captures.
1. clean: ring_census on Check B's zip (arena 4), Bryan's entrance zip (arena 4) and the wm30 zip (arena 29), sampled frames, exit 0, every check PASS.
2. arena found by the join rate: Brock without an arena id reports bg04.
3. planted faults, each must exit 1 AND name its check as FAIL, while step 1 (same frames) passed:
     join_shift -> content_equal, colour_order -> colour_order, skirt_tie -> skirt_tie, lod_swap -> content_equal, part_only_join -> join_unique, lod_pin -> lod, no_names -> names, uv_sign -> uv.
4. inputs that cannot be read fail loudly: a missing zip, a zip with no frame, an arena with no ring (bg66), a missing expect file.
5. files-only census: clean exit 0; plants files_set -> files_ring_set, files_skin_bad -> files_skin; a doctored expect file (one count changed) -> files_contents FAIL.
6. unit: the vertex rules on real captured bytes (Check B frame 200: post 951 mesh 0, frame 0: high rope 960 mesh 0): clean = 0 differing vertices in every role; one byte flipped in a role's
   field = exactly that role counts it; half_trunc rounds toward zero (not to nearest); snorm_field's end points.
Exit 0 only when all hold."""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import ring_census as rc  # noqa: E402
import ring_files as rf  # noqa: E402
import join_census as jc  # noqa: E402

BROCK = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
BRYAN = 'out/s101/runs/20261111_174549_s100_owner_bryan_entrance/case_20261111_174607_799_0_pc_run_exit.zip'
WM30 = 'out/s101/runs/20261111_174707_s100_owner_s90_wm30/case_20261111_174739_724_0_pc_run_exit.zip'
PLANTS = {'join_shift': 'content_equal', 'colour_order': 'colour_order', 'skirt_tie': 'skirt_tie', 'lod_swap': 'content_equal', 'part_only_join': 'join_unique',
          'lod_pin': 'lod', 'no_names': 'names', 'uv_sign': 'uv', 'uv_abs_off': 'uv_sign', 'site_flip': 'mirror', 'drop_952': 'instances', 'drop_952_mesh1': 'unjoined'}
fails = []


def ok(cond, msg):
    print(('PASS  ' if cond else 'FAIL  ') + msg)
    if not cond:
        fails.append(msg)


def run(args, root, timeout=1200):
    p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'ring', 'ring_census.py')] + args, capture_output=True, text=True, timeout=timeout, cwd=root)
    return p.returncode, p.stdout + p.stderr


def failed_checks(out):
    return set(re.findall(r'^\s+FAIL\s+(\w+)\s', out, re.M))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = jc.MAIN
    for rel in (BROCK, BRYAN, WM30):
        if not os.path.isfile(os.path.join(main_co, rel)):
            print('FAIL  input missing: %s (a check that cannot read its input fails)' % rel)
            return 1
    # 1. clean
    for cap, fr in ((BROCK + ':4', '0:341:12'), (BRYAN + ':4', '0:91:6'), (WM30 + ':29', '0:423:20')):
        rc_, out = run([cap, '--frames', fr, '-q'], root)
        ok(rc_ == 0 and 'ring_census: PASS' in out and not failed_checks(out) and out.count('PASS  ') >= 24,
           'clean %s frames %s: exit 0, every check PASS (%d PASS lines)' % (os.path.basename(cap)[:30], fr, out.count('PASS  ')))
    # 2. detection
    rc_, out = run([BROCK, '--frames', '100:341:80', '-q'], root)
    ok(rc_ == 0 and 'arena bg04' in out, 'the arena is found by the join rate: bg04 (no id given)')
    # 3. planted faults
    for plant, want in PLANTS.items():
        rc_, out = run([BROCK + ':4', '--frames', '0:341:12', '--plant', plant, '-q'], root)
        ok(rc_ == 1 and want in failed_checks(out), 'plant %-14s -> exit 1 and check %s FAIL (failed: %s)' % (plant, want, ', '.join(sorted(failed_checks(out))) or 'none'))
    # 4. unreadable inputs
    rc_, out = run(['out/s101/runs/does_not_exist.zip:4', '-q'], root)
    ok(rc_ == 1 and 'FAIL' in out, 'a missing capture fails loudly')
    with tempfile.TemporaryDirectory(prefix='ring_rj_selftest_') as td:
        z = os.path.join(td, 'empty.zip')
        with zipfile.ZipFile(z, 'w') as zf:
            zf.writestr('manifest.json', '{}')
        rc_, out = run([z + ':4', '-q'], root)
        ok(rc_ == 1 and 'no frame files' in out, 'a zip with no frame fails loudly (a checker that reads nothing)')
        rc_, out = run([BROCK + ':66', '--frames', '0:341:80', '-q'], root)
        ok(rc_ == 1, 'an arena without a ring (bg66) fails: %s' % (out.strip().splitlines()[-1] if out.strip() else ''))
        rc_, out = run([BROCK + ':4', '--frames', '0:341:80', '--expect', os.path.join(td, 'nope.json'), '-q'], root)
        ok(rc_ == 1 and 'not found' in out, 'a missing expect file fails loudly')
        # 5. files
        rc_, out = run(['--files', '-q'], root)
        ok(rc_ == 0 and 'ring_census: PASS' in out and out.count('PASS  ') == 7, 'files-only census: exit 0, 7 checks PASS')
        for plant, want in (('files_set', 'files_ring_set'), ('files_pairs_bad', 'files_skin'), ('files_slot_bad', 'files_skin'), ('files_weight_bad', 'files_skin'),
                            ('files_two_pair_bad', 'files_two_pair'), ('files_colour_bad', 'files_colour')):
            rc_, out = run(['--files', '--plant', plant, '-q'], root)
            ok(rc_ == 1 and want in failed_checks(out), 'files plant %-14s -> exit 1 and check %s FAIL (failed: %s)' % (plant, want, ', '.join(sorted(failed_checks(out))) or 'none'))
        ex = json.load(open(os.path.join(root, 'tools', 'ring', 'expect.json')))
        ex['files']['contents']['960'] = 5
        bad = os.path.join(td, 'expect_bad.json')
        json.dump(ex, open(bad, 'w'))
        rc_, out = run(['--files', '--expect', bad, '-q'], root)
        ok(rc_ == 1 and 'files_contents' in failed_checks(out), 'a doctored expect file (960 contents 4 -> 5) fails files_contents')
    # 6. unit, on the captured bytes
    cap = jc.Capture(os.path.join(main_co, BROCK))
    ring = rf.load_ring(4, textures=False)
    dc = rc.DrawCache(cap, rc.RingIndex(ring), None)

    def find(fn, entry, mesh):
        cap.catch_up(fn)
        f = cap.frame(fn)
        cap.advance_contents(f, fn)
        for i in range(len(f.draws)):
            r = dc.get(f, i)
            if r['hits'] and r['hits'][0].entry == entry and r['hits'][0].index == mesh and f.origins[i]['pass_site'] == rc.SITE_COLOUR:
                return r, r['hits'][0]
        raise AssertionError('no draw of %d/%d in frame %d' % (entry, mesh, fn))
    cases = [(200, 951, 0, ['pos', 'ids', 'fill', 'normal', 'colour', 'uv']), (0, 960, 0, ['weights'])]
    for fn, entry, mesh, roles in cases:
        r, m = find(fn, entry, mesh)
        rows, els = r['rows'].copy(), r['els']
        roleoff = rc.roles_of(els)
        base = rc.check_vertex(rows, m.n, r['stride'], els, m, None)
        ok(all(base.get(k, 0) == 0 for k in ('pos', 'ids', 'weights', 'fill', 'normal', 'colour', 'uv')), 'unit: entry %d mesh %d clean bytes: every role has 0 differing vertices' % (entry, mesh))
        for role in roles:
            bump = {'pos': ('pos', 4), 'ids': ('ids', 0), 'fill': ('fill', 0), 'normal': ('normal', 0), 'colour': ('colour', 1), 'uv': ('uv', 0), 'weights': ('weights', 0)}[role]
            if bump[0] not in roleoff:
                ok(False, 'unit: entry %d has no %s element' % (entry, role))
                continue
            row2 = rows.copy()
            row2[7, roleoff[bump[0]] + bump[1]] ^= 0x5A
            res = rc.check_vertex(row2, m.n, r['stride'], els, m, None)
            others = [k for k in ('pos', 'ids', 'weights', 'fill', 'normal', 'colour', 'uv') if k != role and res.get(k, 0)]
            ok(res.get(role, 0) == 1 and not others, 'unit: entry %d one byte of the %s field flipped -> exactly the %s role counts 1 vertex (others %s)' % (entry, role, role, others))
    # the plane-shadow proxy's xdword (0, bytes 1..3 of x): a byte flip counts exactly that role
    cap.catch_up(200)
    f = cap.frame(200)
    cap.advance_contents(f, 200)
    for i in range(len(f.draws)):
        r = dc.get(f, i)
        if r['hits'] and r['hits'][0].entry == 956 and r['hits'][0].index == 0:
            m = r['hits'][0]
            roleoff = rc.roles_of(r['els'])
            base = rc.check_vertex(r['rows'], m.n, r['stride'], r['els'], m, None)
            ok(base['xdword'] == 0 and base['ids'] == 0 and base['fill'] == 0, 'unit: proxy 956/0 clean bytes: xdword, ids, fill all 0 differing')
            for off, nm in ((0, 'low byte non-zero'), (2, 'a copied x byte')):
                row2 = r['rows'].copy()
                row2[5, roleoff['xdword'] + off] ^= 0x21
                res = rc.check_vertex(row2, m.n, r['stride'], r['els'], m, None)
                ok(res['xdword'] == 1 and res['ids'] == 0 and res['fill'] == 0, 'unit: proxy xdword, %s flipped -> exactly the xdword role counts 1' % nm)
            break
    else:
        ok(False, 'unit: no proxy draw in frame 200')
    one = np.array([1.0 + 1.9 * 2 ** -10], np.float32)
    ok(int(rc.half_trunc(one)[0]) == 0x3C01 and int(np.float16(one[0]).view(np.uint16)) == 0x3C02, 'unit: half_trunc rounds toward zero (0x3C01), numpy rounds to nearest (0x3C02)')
    ok(list(rc.half_trunc(np.array([1.0, -1.0, 0.0], np.float32))) == [0x3C00, 0xBC00, 0], 'unit: half_trunc of 1, -1, 0')
    ok(list(rc.snorm_field(np.array([1.0, -1.0, 0.0, 2.0]), 10)) == [32767, -32767, 0, 32767], 'unit: snorm_field end points')
    print('ring selftest: %s' % ('FAIL (%d)' % len(fails) if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

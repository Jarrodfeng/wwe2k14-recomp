r"""Lane 39 acceptance test (S101; offline, 0 game runs): the arena join census and the files-only duplicate census.

  py -I tools\arena\selftest.py [--root DIR]

Every step runs the SHIPPED command line (tools\arena\join_census.py / files_dupes.py as processes), never a copy of their logic.
1. clean: join_census on Check B's zip (arena 4, scene records), Bryan's entrance zip (arena 4) and the wm30 zip (arena 29), sampled frames, exit 0, every check PASS.
2. arena found by the join rate: Brock without an arena id reports bg04.
3. planted faults, each must exit 1 AND name its check as FAIL, while step 1 (same frames) passed:
     join_shift -> content_equal, no_group_rule -> group_rule, no_mirror -> mirror, no_names -> names, no_flags -> mirror, tex_flip (one byte of every file texture's mip 0 flipped) -> texture_bytes.
4. inputs that cannot be read fail loudly: a missing zip, a zip with no frame, a wrong arena id.
5. files_dupes over all 57 arenas exits 0; the capture's tied objects (join_census --json) are each an exact K3 slot class of files_dupes (two independent
   measures of the same thing); a planted clone of a slot is found as a K3 class member, a clone with one vertex moved is not (analyse() = the shipped function).
Exit 0 only when all hold."""
import argparse
import copy
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import arena_files as af  # noqa: E402
import files_dupes  # noqa: E402
import join_census as jc  # noqa: E402

BROCK = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
BRYAN = 'out/s101/runs/20261111_174549_s100_owner_bryan_entrance/case_20261111_174607_799_0_pc_run_exit.zip'
WM30 = 'out/s101/runs/20261111_174707_s100_owner_s90_wm30/case_20261111_174739_724_0_pc_run_exit.zip'
PLANTS = {'join_shift': 'content_equal', 'no_group_rule': 'group_rule', 'no_mirror': 'mirror', 'no_names': 'names', 'no_flags': 'mirror', 'tex_flip': 'texture_bytes'}

fails = []


def ok(cond, msg):
    print(('PASS  ' if cond else 'FAIL  ') + msg)
    if not cond:
        fails.append(msg)


def run(args, root, timeout=900):
    p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'arena', 'join_census.py')] + args, capture_output=True, text=True, timeout=timeout, cwd=root)
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
    samples = [(BROCK + ':4', '0:341:12'), (BRYAN + ':4', '0:91:6'), (WM30 + ':29', '0:423:20')]
    for cap, fr in samples:
        rc, out = run([cap, '--frames', fr, '-q'], root)
        ok(rc == 0 and 'join_census: PASS' in out and not failed_checks(out) and out.count('PASS  ') >= 9, 'clean %s frames %s: exit 0, every check PASS (%d PASS lines)' % (os.path.basename(cap)[:30], fr, out.count('PASS  ')))
    # 2. detection
    rc, out = run([BROCK, '--frames', '100:341:80', '-q'], root)
    ok(rc == 0 and 'arena bg04' in out, 'the arena is found by the join rate: bg04 (no id given)')
    # 3. planted faults
    for plant, want in PLANTS.items():
        rc, out = run([BROCK + ':4', '--frames', '0:341:12', '--plant', plant, '-q'], root)
        ok(rc == 1 and want in failed_checks(out), 'plant %-13s -> exit 1 and check %s FAIL (failed: %s)' % (plant, want, ', '.join(sorted(failed_checks(out))) or 'none'))
    # 4. unreadable inputs
    rc, out = run(['out/s101/runs/does_not_exist.zip:4', '-q'], root)
    ok(rc == 1 and 'FAIL' in out, 'a missing capture fails loudly')
    with tempfile.TemporaryDirectory(prefix='arena_join_l39_selftest_') as td:
        z = os.path.join(td, 'empty.zip')
        with zipfile.ZipFile(z, 'w') as zf:
            zf.writestr('manifest.json', '{}')
        rc, out = run([z + ':4', '-q'], root)
        ok(rc == 1 and 'no frame files' in out, 'a zip with no frame fails loudly (a checker that reads nothing)')
        rc, out = run([BROCK + ':5', '--frames', '0:341:80', '-q'], root)
        ok(rc == 1, 'a wrong arena id (bg05 for a bg04 capture) fails: %s' % (out.strip().splitlines()[-1] if out.strip() else ''))
        js = os.path.join(td, 'brock.json')
        rc, out = run([BROCK + ':4', '--frames', '0:341:12', '--json', js, '-q'], root)
        ok(rc == 0 and os.path.isfile(js), 'json written')
        data = json.load(open(js))
    # 5. files_dupes
    p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'arena', 'files_dupes.py'), '-q'], capture_output=True, text=True, timeout=900, cwd=root)
    ok(p.returncode == 0 and '57 arenas read' in p.stdout, 'files_dupes: %s' % p.stdout.strip().splitlines()[-1][:160])
    arena = af.load_arena(4)
    res = files_dupes.analyse(arena)
    k3 = [tuple(c) for c in res['slot_classes_k3']]
    tied = []
    for r in data.values():
        for row in r['lists'].get('tied_objects', []):
            m = re.search(r'interchangeable slots (\[[0-9, ]+\])', row[2])
            if m:
                tied.append(tuple(json.loads(m.group(1))))
    ok(tied and all(t in k3 for t in tied), 'every tied object of the capture (%d, %d distinct sets) is an exact K3 slot class of the files-only census' % (len(tied), len(set(tied))))
    # planted clone: slot 240's meshes copied to slot 9990 (same), and to 9991 with one vertex moved
    ar2 = copy.copy(arena)
    ar2.slots = dict(arena.slots)
    for new, moved in ((9990, False), (9991, True)):
        ms = []
        for m in arena.slots[240]:
            c = copy.copy(m)
            c.slot = new
            c.positions = m.positions.copy()
            if moved:
                c.positions[0, 0] += 1.0
            ms.append(c)
        ar2.slots[new] = ms
    r2 = files_dupes.analyse(ar2)
    k3b = [c for c in r2['slot_classes_k3'] if 240 in c]
    k2b = [c for c in r2['slot_classes_k2'] if 240 in c]
    ok(k3b and 9990 in k3b[0] and 9991 not in k3b[0] and k2b and 9990 in k2b[0] and 9991 not in k2b[0],
       'planted clone of slot 240 joins its class (9990 in), a clone with one vertex moved does not (9991 out)')
    ok(not any(9990 in c or 9991 in c for c in res['slot_classes_k3']), 'the clean analysis has neither planted slot')
    # 6. compare_texture, the shipped function, with planted twins: a padded image with equal overlapping rows, the same with one byte flipped, a small mip 0
    def tex(w, h, mips=1):
        t = af.Tex()
        t.name, t.width, t.height, t.mips, t.fourcc, t.dir = 'x', w, h, mips, b'DXT1', 400
        t.blob = bytes(128) + bytes(range(1, 1 + sum(((max(w >> k, 1) + 3) // 4) * ((max(h >> k, 1) + 3) // 4) * 8 for k in range(mips))))
        return t
    t = tex(12, 8)  # 3 x 2 blocks, 24-byte rows
    pay = t.payload
    padded = b''.join(pay[r * 24:(r + 1) * 24] + bytes([238]) * 8 for r in range(2))  # 16 x 8 image: 4 blocks, 32-byte rows, the extra block is padding
    ok(jc.compare_texture(jc.TexData(padded, [(0, 0, 16, 8, 32, 2, 0)]), t) == 'padded', 'compare_texture: a padded image with equal overlapping rows is padded')
    bad = bytearray(padded); bad[5] ^= 1
    ok(jc.compare_texture(jc.TexData(bytes(bad), [(0, 0, 16, 8, 32, 2, 0)]), t) == 'padded_differ', 'compare_texture planted: one byte flipped in the overlap -> padded_differ')
    same = bytearray(pay); same[3] ^= 1
    ok(jc.compare_texture(jc.TexData(bytes(same), [(0, 0, 12, 8, 24, 2, 0)]), t) == 'differ', 'compare_texture planted: one byte flipped in mip 0 -> differ')
    ok(jc.compare_texture(jc.TexData(pay, [(0, 0, 12, 8, 24, 2, 0)]), t) == 'equal', 'compare_texture: equal data is equal')
    t2 = tex(8, 8, 2)
    p2 = bytearray(t2.payload)
    p2[-1] ^= 1  # mip 1 (4 x 4, one block) differs: the tail rule allows it
    ok(jc.compare_texture(jc.TexData(bytes(p2), [(0, 0, 8, 8, 16, 2, 0), (1, 0, 4, 4, 8, 1, 32)]), t2) == 'tail', 'compare_texture: a differing mip of at most 8 texels (mip > 0) is tail')
    t3 = tex(4, 4)
    p3 = bytearray(t3.payload); p3[0] ^= 1
    ok(jc.compare_texture(jc.TexData(bytes(p3), [(0, 0, 4, 4, 8, 1, 0)]), t3) == 'differ', 'compare_texture: a differing mip 0 is never tail, however small')
    print('arena selftest: %s' % ('FAIL (%d)' % len(fails) if fails else 'PASS'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

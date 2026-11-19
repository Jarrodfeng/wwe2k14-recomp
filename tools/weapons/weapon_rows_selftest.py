r"""S103 lane PW-2 gate check: the weapons' per-call rows (src\scene_live\weapon_rows_*.h; contract src\renderer_scene\weapon_rows.h). Offline, 0 game runs, headless.

  py -I tools\weapons\weapon_rows_selftest.py [--no-build] [--capture DIR]

  1. builds out\weapons\weapon_rows_test.exe (tools\weapons\build_weapon_rows.ps1) and runs it clean: two chairs (one model, one r5, the buffer rewritten between the calls: both rows kept,
     instance 0 / 1, each with the buffer of its own call), the steps shape, a secondary model, the per-frame membership (a freed slot's address stops being a weapon), the census,
     the contributor's pool rebase, the record round trip (sections 60 / 61), an old record, a bad node range, and (when --capture is given and exists) the ring steps' call structure replayed
     from a kept capture's LightRefs;
  2. the planted twins, each of which must fail its NAMED check: dedupe_object_r5, late_read, node_count_plus1, cached_members (the capture arm's `capture.instances` is named for the dedupe plant too
     when a capture is given).
Exit 0 = the clean run passes and every plant fails the check it names."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TEST = os.path.join(ROOT, 'out', 'weapons', 'weapon_rows_test.exe')
bad = 0

PLANTS = [('dedupe_object_r5', ['hook.two_chairs']), ('late_read', ['hook.two_chairs']), ('node_count_plus1', ['hook.two_chairs']), ('count_every_submit', ['submit.extra_passes']), ('rows_after_ours_check', ['hook.ours_mode_records']), ('shadow_as_colour', ['shadow.tagged_not_colour']), ('no_frame', ['row.frame']), ('no_id_at_call', ['row.id_at_call']), ('rigid_as_unknown', ['rigid.one_row']), ('strict_forward', ['wn.data_leaf_rows']), ('cached_members', ['hook.freed_slot_not_weapon', 'hook.one_read_per_frame'])]


def check(name, ok, why=''):
    global bad
    print('%s  %-44s %s' % ('PASS' if ok else 'FAIL', name, why))
    bad += 0 if ok else 1


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def failed_names(out):
    return {ln.split()[1] for ln in out.splitlines() if ln.startswith('FAIL ')}


def main():
    argv = sys.argv[1:]
    capture = []
    if '--capture' in argv:
        i = argv.index('--capture')
        if i + 1 < len(argv) and os.path.isdir(argv[i + 1]):
            capture = ['--capture', argv[i + 1]]
        else:
            print('SKIP  capture arm: no such capture folder (%s)' % (argv[i + 1] if i + 1 < len(argv) else ''))
    if '--no-build' not in argv:
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(HERE, 'build_weapon_rows.ps1')])
        check('build', rc == 0, '' if rc == 0 else out[-1500:])
        if rc:
            return 1
    rc, out = run([TEST] + capture)
    print(out.rstrip())
    check('unit.clean', rc == 0 and 'FAIL' not in out, '%d failing' % len(failed_names(out)))
    for plant, named in PLANTS:
        rc, pout = run([TEST, '--plant', plant] + capture)
        f = failed_names(pout)
        check('plant.%s' % plant, rc == 1 and all(n in f for n in named), 'fails %s' % ', '.join(sorted(f)))
    if capture:
        rc, pout = run([TEST, '--plant', 'dedupe_object_r5'] + capture)
        check('plant.dedupe_object_r5.capture', 'capture.instances' in failed_names(pout), 'the capture arm sees the dropped second instance')
    print('weapon_rows_selftest: %s' % ('ALL PASS' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

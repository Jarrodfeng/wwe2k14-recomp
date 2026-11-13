r"""S101 lane 202 gate check: Caster D3 pieces E0-E2 (the caster pass entries in the scene record; docs\renderer\SKIP_GAME_WORK_MAP.md 17.7). Offline, 0 game runs, headless.

  py -I tools\caster\caster_rows_selftest.py [--no-build]

  1. builds out\caster\caster_rows_test.exe (tools\caster\build_caster_rows.ps1) and runs the unit test: the tracker, the contributor and the record code against a fake guest BUILT FROM THE LISTINGS TABLE
     tools\caster\caster_call_sites.tsv (not the stream's values), the game's frames replayed in live order; the null-edit self-test (unedited = 0 differences, one flipped bit = exactly 1);
  2. the planted twins, each of which must fail its NAMED check: late_record, offset0, endless_retry, dropped_frame_failure (lane 152's four), ordinal_merge, stale_slots, camera_prev_frame, late_read,
     map_a_key_ignored;
  3. the listings: the three methods' r4 stores / twin calls and the collapse flag's slot load are the instructions the table cites, in the generated code of the main checkout (SKIP without generated\);
  4. the Python reader (ledger_io Record.caster_calls) on the record the unit test wrote;
  5. caster_rows_check.py's pure parts on synthetic data (entry grouping with a restarted part, the offset vote, the camera compare in either orientation) and its plants (camera_prev_frame, wrong_offset0,
     no_ordinal);
  6. the census on the kept captures through the reader's own sources (SKIP when the case folders are absent): Check B / run 7079 entries a frame, the ledger's numbers reproduced (map 93 draws a frame and
     62.4 % ready, rim 51 and 100 %, plane 0 %), and lane 196's two plants on the palette source: colour_off (rim under 70 %) and offset0 (rim under 90 %)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
sys.path.insert(0, HERE)
import ledger_io  # noqa: E402
import caster_rows_check as CC  # noqa: E402

TEST = os.path.join(ROOT, 'out', 'caster', 'caster_rows_test.exe')
SITES = os.path.join(ROOT, 'tools', 'caster', 'caster_call_sites.tsv')
SAMPLE = os.path.join(ROOT, 'out', 'caster', 'caster_rows_sample.rsc')
bad = 0


def check(name, ok, why=''):
    global bad
    print('%s  %-42s %s' % ('PASS' if ok else 'FAIL', name, why))
    bad += 0 if ok else 1


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def cam(k, f=0):
    return dict(shadow_m=[[float(100 * k + 16 * m + i + f) for i in range(16)] for m in range(3)], shadow_f=[0.0] * 16, shadow_x=[[0.0] * 8, [0.0] * 8])


def call(frame, p, obj, ordinal, k):
    return dict(pass_=p, object=obj, frame=frame, ordinal=ordinal, camera=cam(k, frame), collapse=0)


class FakeRec:
    def __init__(self, calls):
        self.calls = calls

    def caster_calls(self):
        return self.calls


def quad_of(camera, k, orient):
    m = camera['shadow_m'][k]
    return tuple(tuple(m[4 * r:4 * r + 4]) for r in range(4)) if orient == 'r' else tuple(tuple(m[c::4]) for c in range(4))


def main():
    if '--no-build' not in sys.argv:
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'caster', 'build_caster_rows.ps1')], 900)
        check('build', rc == 0, out.strip().splitlines()[-1] if out.strip() else '')
        if rc != 0:
            print(out[-1500:])
            return 1
    # 1. the unit test
    rc, out = run([TEST, '--sites', SITES, '--write-record', SAMPLE], 300)
    check('unit', rc == 0 and ' 0 failed' in out, out.strip().splitlines()[-1])
    # 2. the planted twins
    plants = (('late_record', ['contrib.live_order_join']), ('offset0', ['contrib.live_order_join']), ('endless_retry', ['hook.unreadable_once']),
              ('dropped_frame_failure', ['hook.old_calls_not_failures']), ('ordinal_merge', ['hook.ordinals']), ('stale_slots', ['hook.palette_hash']),
              ('camera_prev_frame', ['hook.camera_per_entry']), ('late_read', ['hook.camera_per_entry']), ('map_a_key_ignored', ['hook.collapse']), ('node_shift', ['hook.node_located']),
              ('product_slot0', ['hook.extra_per_entry']), ('product_prev_frame', ['hook.extra_per_entry']), ('pose_row_off', ['hook.pose_point']),
              ('plane_r5_palette', ['hook.plane_mats']), ('plane_pos_dropped', ['hook.plane_pos']), ('plane_mats_shift', ['hook.plane_mats']))
    for plant, named in plants:
        rc, out = run([TEST, '--sites', SITES, '--plant', plant], 300)
        check('unit plant ' + plant, rc == 1 and all(('FAIL  ' + n) in out for n in named), 'exit %d, names %s' % (rc, ', '.join(named)))
    # 3. the listings
    main_co = main_checkout()
    lister = os.path.join(main_co, 'tools', 'analysis', 'list_function.py')
    if not os.path.isdir(os.path.join(main_co, 'generated', 'default')):
        print('SKIP  listings                                 (no generated\\default in %s)' % main_co)
    else:
        rows = [l.rstrip('\r\n').split('\t') for l in open(SITES, encoding='utf-8') if l.strip() and not l.startswith('#')]
        for r in rows:
            if r[0] == 'pass' and r[3] != '0x00000000':
                fn, addr, rx = r[2], r[3], r[4].replace('\\', '')
            elif r[0] == 'twin':
                fn, addr, rx = r[2], r[3], 'bl ' + r[4].lower()
            elif r[0] == 'collapse':
                fn, addr, rx = '0x82BA7808', r[3], 'li r9,1048'
            elif r[0] == 'cite':   # lane 221
                fn, addr, rx = r[2], r[3], r[4]
            else:
                continue
            p = subprocess.run([sys.executable, '-I', lister, fn], capture_output=True, text=True, cwd=main_co, timeout=120)
            lines = {}
            for l in p.stdout.splitlines():
                m = re.match(r'(0x[0-9A-Fa-f]{8})\s+(.*)', l)
                if m:
                    lines[int(m.group(1), 16)] = m.group(2)
            got = lines.get(int(addr, 16), '')
            check('listing %s @%s' % (r[0] + ' ' + r[1], addr), bool(re.match(re.escape(rx).replace(r'\ ', ' '), got.strip())) or got.strip().startswith(rx), '%s' % (got.strip() or 'no line'))
    # 4. the Python reader
    try:
        rec = ledger_io.Record(SAMPLE)
        cc = rec.caster_calls()
        passes = {c['pass_'] for c in cc}
        check('reader.sample', len(cc) > 0 and passes == {1, 2, 3} and all(c['camera'] is not None for c in cc) and any(c['collapse'] == 1 for c in cc) and any(c['pal_hash'] for c in cc),
              '%d entries, passes %s, cameras decoded, collapse and palette hashes present' % (len(cc), sorted(passes)))
        # lane 217: every call that has a palette hash carries n_nodes per-node hashes (flag 256), and the reader's FNV-32 of the record's own matrices is what the tracker wrote only when they are the same rows
        # (here: the sample's calls are the fake guest's, the record's bones are the same rows of the SAME frame in the test's scenario, so only the shape is asserted)
        hashed = [c for c in cc if c['pal_hash'] and not (c['flags'] & 512)]   # (lane 226: a plane call carries its matrices, flag 512, not node hashes)
        planes = [c for c in cc if c['flags'] & 512]
        check('reader.node_hashes', bool(hashed) and all(c['node_hashes'] is not None and len(c['node_hashes']) == c['n_nodes'] for c in hashed) and all(c['node_hashes'] is None for c in cc if not c['pal_hash']),
              '%d calls with a palette: n_nodes node hashes each (section 24); calls without one: none' % len(hashed))
        check('reader.plane_mats', bool(planes) and all(c['mats'] is not None and len(c['mats']) == c['n_nodes'] and c['node_hashes'] is None for c in planes),
              '%d plane calls carry n_nodes matrices each (section 26, flag 512), no node hashes' % len(planes))
    except (OSError, ValueError, KeyError, IndexError) as e:
        check('reader.sample', False, str(e))
    # 4b. lane 217: the zip probe's per-node reading: a planted difference in nodes 60 and 71 is located exactly
    rc, out = run([sys.executable, '-I', os.path.join(HERE, 'caster_zip_probe.py'), '--selftest', 'x'], 120)
    check('probe.node_selftest', rc == 0 and 'PASS' in out, out.strip().splitlines()[-1] if out.strip() else 'no output')
    # 5. caster_rows_check's pure parts
    d = lambda p, o, part, q=None: dict(pass_=p, object=o, part=part, quad=q)  # noqa: E731
    ents = CC.stream_entries([d(1, 7, 0), d(1, 7, 1), d(1, 7, 0), d(1, 7, 1), d(1, 8, 0), d(3, 7, 0), d(3, 7, 1)])
    check('check.entry_grouping', [(e['pass_'], e['object'], e['ordinal'], e['draws']) for e in ents] == [(1, 7, 0, 2), (1, 7, 1, 2), (1, 8, 0, 1), (3, 7, 0, 2)],
          'a repeated part restarts the entry (map A then map B of one object back to back); ordinals count per (pass, object)')
    flat = CC.stream_entries([d(1, 7, 0), d(1, 7, 0)], no_ordinal=True)
    check('check.plant no_ordinal', [e['ordinal'] for e in flat] == [0, 0], 'the planted ordinal drop gives every entry 0 (the join then takes the first call for map B)')
    # frames 100..103: for each stream frame s the hook frame is s - 1 (offset +1); the map object has two entries (A, B) with different cameras
    recs, by_frame = [], {}
    for s in range(100, 104):
        ob = 7 + 16 * s   # an object address that differs per frame, so only the right offset joins
        calls = [call(s - 1, 1, ob, 0, 1), call(s - 1, 1, ob, 1, 2), call(s - 1, 3, ob, 0, 3)]
        recs.append(FakeRec(calls))
        by_frame[s] = [dict(pass_=1, object=ob, ordinal=0, first_quad=quad_of(cam(1, s - 1), 0, "r"), draws=2), dict(pass_=1, object=ob, ordinal=1, first_quad=quad_of(cam(2, s - 1), 2, 'c'), draws=2),
                       dict(pass_=3, object=ob, ordinal=0, first_quad=quad_of(cam(3, s - 1), 0, 'c'), draws=2)]
    table = CC.build_table(recs)
    off, votes = CC.vote_offset(by_frame, table)
    check('check.offset_vote', off == 1 and votes[1] == 12 and votes[0] == 0, 'measured +1: %s' % {k: v for k, v in votes.items() if v})

    def verdicts(plants=(), o=1):
        out = []
        for s, es in by_frame.items():
            for e in es:
                out.append(CC.compare_entry(CC.join_call(table, s, o, e, plants), e['first_quad'])[0])
        return out
    v = verdicts()
    check('check.all_equal', all(x == 'equal' for x in v), 'every entry equal (either matrix orientation, any of the three matrices): %d of %d' % (v.count('equal'), len(v)))
    vp = verdicts(('camera_prev_frame',))
    check('check.plant camera_prev_frame', any(x != 'equal' for x in vp), 'the camera one frame old: %d of %d entries no longer equal' % (sum(1 for x in vp if x != 'equal'), len(vp)))
    v0 = verdicts((), 0)
    check('check.plant wrong_offset0', not any(x == 'equal' for x in v0), 'joined at offset 0 no entry is equal (%d absent)' % v0.count('absent'))
    check('check.named_refusals', CC.compare_entry(None, ())[0] == 'absent' and CC.compare_entry(dict(camera=None), ())[0] == 'no_state', 'absent / no_state are verdicts of their own')
    rc, out = run([sys.executable, '-I', os.path.join(HERE, 'caster_rows_check.py'), '--zip', os.path.join(ROOT, 'out', 'caster', 'does_not_exist')])
    check('check.missing input fails loudly', rc == 1 and 'FAIL' in out, 'exit %d' % rc)
    # 6. the census on the kept captures
    mc = os.path.join(main_co, 'out', 's101', 'lane92')
    cb = os.path.join(mc, 'case_20261111_215233_217_0_pc_run_exit')
    c79 = os.path.join(mc, 'case_20261112_122658_040_0_pc_run_exit')
    if not (os.path.isdir(os.path.join(cb, 'capture', 'frames')) and os.path.isdir(os.path.join(c79, 'capture', 'frames'))):
        print('SKIP  census on kept captures                  (the Check B / run 7079 case folders are absent)')
    else:
        def census(path, *pl):
            cmd = [sys.executable, '-I', os.path.join(HERE, 'caster_rows_check.py'), '--census', path, '--frames', '150:200']
            for p in pl:
                cmd += ['--plant', p]
            rc_, o_ = run(cmd, 600)
            res = {}
            for ln in o_.splitlines():
                m = re.match(r'(map|plane|rim)\s+entries a frame\s+([\d.]+)\s+draws a frame\s+([\d.]+)\s+draws an entry\s+([\d.]+)\s+ready\s+([\d.]+|-)', ln)
                if m:
                    res[m.group(1)] = dict(entries=float(m.group(2)), draws=float(m.group(3)), ready=float(m.group(5)) if m.group(5) != '-' else None)
            return rc_, res
        for label, path in (('Check B', cb), ('run 7079', c79)):
            rc_, r = census(path)
            check('census %s' % label, rc_ == 0 and r['map']['draws'] == 93.0 and r['rim']['draws'] == 51.0 and r['rim']['ready'] == 100.0 and 60.0 <= r['map']['ready'] <= 65.0 and r['plane']['ready'] == 0.0
                  and r['map']['entries'] == 11.0 and r['rim']['entries'] == 7.0,
                  'map %.1f draws / %.1f entries a frame, %.1f%% ready; rim %.1f / %.1f, %.1f%%; plane %.1f%% ready (ledger: 93 / 62.4%%, 51 / 100%%, 0%%)' % (
                      r['map']['draws'], r['map']['entries'], r['map']['ready'], r['rim']['draws'], r['rim']['entries'], r['rim']['ready'], r['plane']['ready'] or 0.0))
        rc_, r1 = census(cb, 'colour_off')
        check('census plant colour_off', rc_ == 0 and r1['rim']['ready'] < 70.0, 'the palette source off: rim falls to %.1f%% ready (< 70)' % r1['rim']['ready'])
        rc_, r2 = census(cb, 'offset0')
        check('census plant offset0', rc_ == 0 and r2['rim']['ready'] < 90.0, 'records read at offset 0: rim falls to %.1f%% ready (< 90)' % r2['rim']['ready'])
    print('caster_rows selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

r"""S101 lane 130 gate check: the arena's D3 gaps (placed world, moving world, stage clock) in the scene record. Offline, 0 game runs.

  py -I tools\scene\arena_world_selftest.py [--no-build] [--frames 0:380:6]

  1. builds out\scene\arena_world_test.exe + arena_world_oracle.exe (tools\scene\build_arena_world.ps1) and runs the unit test: the pure functions, the model-entry tracker
     against a fake guest (r5 always read, the node from the part descriptor, the origin snapshot), the contributor's Emit, the record round trip, an older record without
     the section, an unknown layout that drops only its section;
  2. the Python reader (ledger_io Record.arena_worlds) on the record the C++ test wrote, and the J1 analysis (arena_world_records.py best_offset / join) on synthetic
     records: the measured offset is found and a shifted join is caught;
  3. the capture-side oracle on Check B (run 7051), run 7079 and h1: the field's FITTED values against the stream's registers (a separate, not-provable column);
  4. planted twins, each of which must fail its NAMED check: placed_identity, moving_static, clock_frozen, stamp_off_by_one, refuse_foreign_palette, node1_assumed,
     origin_after_reload (unit test) and placed_identity, moving_static, clock_frozen, stamp_off_by_one (oracle).
A missing input FAILS loudly (this machine's gate lane lists the captures under needs)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L  # noqa: E402
import ledger_io  # noqa: E402
import arena_world_records as R  # noqa: E402

MAIN = L.main_checkout()
CAPS = {'checkB': 'out/s101/lane92/case_20261111_215233_217_0_pc_run_exit', 'r7079': 'out/s101/lane92/case_20261112_122658_040_0_pc_run_exit', 'h1': 'out/lane115/h1'}
TEST = os.path.join(ROOT, 'out', 'scene', 'arena_world_test.exe')
SAMPLE = os.path.join(ROOT, 'out', 'scene', 'arena_world_sample.rsc')
bad = 0


def check(name, ok, why=''):
    global bad
    print('%s  %-36s %s' % ('PASS' if ok else 'FAIL', name, why))
    bad += 0 if ok else 1


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def oracle(cap, frames, extra):
    return run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'skip', 'arena_world_oracle.py'), os.path.join(MAIN, CAPS[cap]), '--frames', frames] + extra)


def synthetic():
    """A fake capture's draws and records: a turning object and a placed one over 8 frames; entries stamped (guest - shift)."""
    def rows(k):
        return (1.0, 0.0, 0.0, 10.0 * k, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 5.0)
    draws, ents = [], []
    for k in range(8):
        g = 1000 + k
        draws.append((k, g, 0, 0xAB0F3310, 'moving', rows(k), 0.0))
        draws.append((k, g, 1, 0xAB1196E0, 'placed', rows(100), 0.0))
        draws.append((k, g, 2, 0xAB083340, 'animated', rows(0), float(g - 900)))
    return draws, rows


class FakeRec:
    def __init__(self, ents):
        self.ents = ents

    def arena_worlds(self):
        return self.ents


def entries_for(draws, shift, rows):
    ents = []
    for fi, g, i, obj, fam, r, c in draws:
        stamp = g - shift
        if fam in ('moving', 'placed'):
            ents.append(dict(kind=1, layout=1, slot=1, object=obj, frame=stamp, origin=0, clock=0.0, world=list(r)))
        else:
            ents.append(dict(kind=2, layout=1, slot=0xFFFF, object=0, frame=stamp, origin=900, clock=float(g - 900), world=[0.0] * 12))
    return ents


def main():
    frames = '0:380:6'
    if '--frames' in sys.argv:
        frames = sys.argv[sys.argv.index('--frames') + 1]
    if '--no-build' not in sys.argv:
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'scene', 'build_arena_world.ps1')], 900)
        check('build', rc == 0, out.strip().splitlines()[-1] if out.strip() else '')
        if rc != 0:
            print(out[-1500:])
            return 1
    # 1. the unit test and its planted twins
    rc, out = run([TEST, '--write-record', SAMPLE], 300)
    check('unit', rc == 0 and ' 0 failed' in out, out.strip().splitlines()[-1])
    for plant, named in (('placed_identity', ['hook.placed_translation']), ('moving_static', ['hook.moving_changes']), ('clock_frozen', ['contrib.clock_advances']),
                         ('stamp_off_by_one', ['hook.moving_exact', 'join.by_stamp']), ('refuse_foreign_palette', ['hook.foreign_palette_recorded']),
                         ('node1_assumed', ['hook.node_from_part']), ('origin_after_reload', ['contrib.clock_origin_snapshot'])):
        rc, out = run([TEST, '--plant', plant], 300)
        check('unit plant ' + plant, rc == 1 and all(('FAIL  ' + n) in out for n in named), 'exit %d, names %s' % (rc, ', '.join(named)))
    # 2. the Python reader and the J1 analysis
    try:
        rec = ledger_io.Record(SAMPLE)
        w = rec.arena_worlds()
        objs = [e for e in w if e['kind'] == 1]
        clk = [e for e in w if e['kind'] == 2]
        n2 = [e for e in objs if e['object'] == 0xAB0FAAAA]
        fo = [e for e in objs if e['object'] == 0xAB0F9999]
        check('reader.sample', len(objs) == 24 and len(clk) == 4 and n2 and n2[0]['slot'] == 2 and n2[0]['world'][3] == 100.0 and fo and fo[0]['origin'] & 1 and clk[0]['origin'] == 3805,
              '%d objects, %d clocks, node 2 read, foreign palette flagged' % (len(objs), len(clk)))
    except (OSError, ValueError, KeyError, IndexError) as e:
        check('reader.sample', False, str(e))
    draws, rows = synthetic()
    for shift in (0, 1):
        o, c = R.build_table([FakeRec(entries_for(draws, shift, rows))])
        off, votes = R.best_offset(draws, o)
        sourced, stats, present = R.join(draws, o, c, off if off is not None else 0)
        ok = off == shift and stats['moving']['equal'] == 8 and stats['placed']['equal'] == 8 and stats['animated']['clock_equal'] == 8
        check('records.offset_%+d' % shift, ok, 'measured %s, votes %s' % (off, votes))
    o, c = R.build_table([FakeRec(entries_for(draws, 1, rows))])
    _, stats0, _ = R.join(draws, o, c, 0)  # joined blind at offset 0: the neighbour frame's rows / no entry
    check('records.blind_join_caught', stats0['moving']['equal'] < 8 and stats0['animated']['clock_equal'] < 8, 'moving equal %d of 8, clock equal %d of 8 at offset 0' % (stats0['moving']['equal'], stats0['animated']['clock_equal']))
    # 3. the oracle on the three captures
    for cap in CAPS:
        if not os.path.isdir(os.path.join(MAIN, CAPS[cap])):
            check('input ' + cap, False, 'missing: ' + CAPS[cap])
            continue
        rc, out = oracle(cap, frames, ['--expect', 'world_placed', '--expect', 'world_moving', '--expect', 'world_clock'])
        lines = [l for l in out.splitlines() if l.startswith('PASS  expect') or l.startswith('FAIL  expect')]
        check('oracle ' + cap, rc == 0 and 'arena_world_oracle: PASS' in out, ' | '.join(l.strip()[:90] for l in lines))
        rise = [l for l in out.splitlines() if 'arena-wide projected' in l]
        if rise:
            vals = [float(v) for v in re.findall(r'(\d+\.\d+)%', rise[0])]
            # honest projected unchanged by the fitted field (rule 4), the fitted one higher
            check('fitted not counted ' + cap, len(vals) >= 3 and abs(vals[0] - vals[1]) < 0.01 and vals[2] > vals[1], rise[0].strip().split(': ', 1)[-1][:120])
    # 4. the planted twins at oracle level (r7079 only)
    for plant, expect, must_fail in (('placed_identity', 'world_placed', True), ('moving_static', 'world_moving', True), ('clock_frozen', 'world_clock', True),
                                     ('stamp_off_by_one', 'world_moving', True), ('stamp_off_by_one', 'world_clock', True), ('stamp_off_by_one', 'world_placed', False)):
        rc, out = oracle('r7079', '0:380:20', ['--plant', plant, '--expect', expect])
        ok = (rc == 1 and 'FAIL  expect' in out) if must_fail else (rc == 0 and 'PASS  expect' in out)
        check('oracle plant %s/%s' % (plant, expect), ok, 'exit %d (%s)' % (rc, 'must fail' if must_fail else 'documented to pass'))
    rc, out = run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'skip', 'arena_world_oracle.py'), os.path.join(MAIN, CAPS['r7079'] + '_does_not_exist')])
    check('missing input fails loudly', rc == 1 and 'FAIL' in out, 'exit %d' % rc)
    print('arena_world selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

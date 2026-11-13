r"""S101 lane 130: the capture-side oracle for the arena's D3 gaps (placed world, moving world, stage clock). Offline, 0 game runs.

  py -I tools\skip\arena_world_oracle.py CAPTURE [--frames A:B:STEP] [--arena 4] [--plant NAME] [--expect NAME ...] [--work DIR]

What it does, in order:
  1. reads the capture's stream draws (ledger_io) and writes, for every placed / moving / animated arena draw, its engine object, family, VS c76..c78 and c186 as bits;
  2. runs out\scene\arena_world_oracle.exe (C++, the SHIPPED pure functions of src\scene_live\arena_world.h + the player's own pac entry 30001): the value the
     kScArenaWorlds field would hold for each draw, joined to the draw by the hook's frame stamp, compared with the stream's register;
  3. runs arena_ready twice: without the field (today) and with the oracle's values fed in (the 'world' source), and prints the families' shares.
THESE VALUES ARE FITTED ON THE SAME CAPTURE (the clock origin, the moving pieces' phase): they prove the structure, not the live source. arena_ready reports them in their
own column ("fitted, pending J1") and never counts them as provable or in the projected share (rule 4); the real proof is tools\skip\arena_world_records.py on J1's records.
--expect world_placed | world_moving | world_clock: assertions on the field's share. --plant: placed_identity, moving_static, clock_frozen, stamp_off_by_one (each must make its
expect fail; stamp_off_by_one passes world_placed, the placed worlds being constant). Exit 1 on a failed expect or an unreadable input."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ledger as L  # noqa: E402
import arena_ready as AR  # noqa: E402
from ledger_io import Registry  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PLANTS = ('placed_identity', 'moving_static', 'clock_frozen', 'stamp_off_by_one')


def iter_draws(cap, frames, all_objects=False):
    """Yields (fi, guest_frame, draw index, engine object, family code, rows bits (12), c186 bits) for the arena's yProp_ / yBG_ draws at the main pass site.
    family code: placed / moving / animated, or (all_objects) other."""
    reg = Registry(os.path.join(L.ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    fam_fn = AR.make_family(())
    for fi in frames:
        f = cap.frame(fi)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] != L.SITE_A:
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            if not vn or not (vn.startswith('yProp_') or vn.startswith('yBG_')):
                continue
            vb = f.bank(d['vs_bank'])
            code = {'placed': 'placed', 'moving_A': 'moving', 'animated': 'animated'}.get(fam_fn(o, vn, vb, None))
            if not code and not all_objects:
                continue
            yield fi, f.guest_frame, i, o['object'], code or 'other', tuple(vb[4 * r + k] for r in (76, 77, 78) for k in range(4)), vb[4 * 186]


def extract(cap, frames, path):
    n = 0
    last = None
    with open(path, 'w', encoding='utf-8') as out:
        for fi, guest, i, obj, code, rows, c186 in iter_draws(cap, frames):
            if fi != last:
                out.write('F %d %d\n' % (fi, guest))
                last = fi
            out.write('D %d %08x %s %s %08x\n' % (i, obj, code, ' '.join('%08x' % b for b in rows), c186))
            n += 1
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--plant', default='', choices=('',) + PLANTS)
    ap.add_argument('--expect', action='append', default=[])
    ap.add_argument('--work', default='')
    x = ap.parse_args(argv)
    exe = os.path.join(ROOT, 'out', 'scene', 'arena_world_oracle.exe')
    if not os.path.isfile(exe):
        print('FAIL  %s is missing: build it with tools\\scene\\build_arena_world.ps1' % exe)
        return 1
    main_co = L.main_checkout()
    work = x.work or os.path.join(main_co, 'out', 'lane130')
    os.makedirs(work, exist_ok=True)
    try:
        cap = L.Capture(x.capture)
        frames = L.parse_frames(x.frames, cap.n)
        tag = os.path.basename(os.path.normpath(os.path.dirname(cap.dir) if os.path.basename(cap.dir) == 'capture' else cap.dir))
        draws = os.path.join(work, '%s_draws.txt' % tag)
        sourced = os.path.join(work, '%s_sourced%s.txt' % (tag, '_' + x.plant if x.plant else ''))
        n = extract(cap, frames, draws)
        print('oracle input: %d placed / moving / animated draws over %d frames (%s)' % (n, len(frames), draws))
        cmd = [exe, '--game', os.path.join(main_co, 'game_data', 'extracted'), '--arena', str(x.arena), '--in', draws, '--out', sourced]
        if x.plant:
            cmd += ['--plant', x.plant]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        sys.stdout.write(p.stdout)
        if p.returncode != 0:
            print('FAIL  arena_world_oracle.exe: %s' % (p.stderr or p.stdout).strip()[:300])
            return 1
        world = {}
        for ln in open(sourced, encoding='utf-8'):
            if ln.startswith('S '):
                _, fi_, i_, regs_ = ln.split()
                world[(int(fi_), int(i_))] = {int(r) for r in regs_.split(',')}
        spec = x.frames
        before = AR.analyse(x.capture, spec, ())
        after = AR.analyse(x.capture, spec, (), world)
    except (L.LedgerError, OSError, KeyError, ValueError) as e:
        print('FAIL  %s: %s' % (x.capture, e))
        return 1
    print('--- arena_ready with the fitted field (a separate column, not provable, pending J1) ---')
    for l in AR.report(after):
        print(l)

    def proj(a, key='projected_full_pct'):
        return 100.0 * sum(a['families'][n_][key] * a['families'][n_]['draws'] / 100.0 for n_ in AR.FAMILIES if a['families'][n_]['draws']) / max(1, a['arena_draws'])
    print('--- before -> after ---')
    for n_ in ('placed', 'moving_A', 'animated'):
        b, a_ = before['families'][n_], after['families'][n_]
        if b['draws']:
            print('  %-9s provable (no template, no fitted field) %5.1f%% -> %5.1f%%   projected %5.1f%% -> %5.1f%%   projected with the FITTED field %5.1f%%' % (
                n_, b['provable_pct'], a_['provable_pct'], b['projected_full_pct'], a_['projected_full_pct'], a_['projected_fitted_full_pct']))
    print('  arena-wide projected (draws 100%% sourced once the light-state rows exist): %.2f%% -> %.2f%%; with the fitted field (pending J1): %.2f%%' % (
        proj(before), proj(after), proj(after, 'projected_fitted_full_pct')))
    bad = 0
    for ok, msg in AR.expect(after, x.expect):
        print('%s  expect %s' % ('PASS' if ok else 'FAIL', msg))
        bad += 0 if ok else 1
    print('arena_world_oracle: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

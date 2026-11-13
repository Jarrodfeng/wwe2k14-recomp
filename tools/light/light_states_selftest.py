r"""S101 lane 109 (LS0-LS3) gate check: the per-draw light state, offline, 0 game runs.

  py -I tools\light\light_states_selftest.py [--frames 0:340:20] [--no-build]

What it runs (every check prints PASS / FAIL; a missing input FAILS loudly, never an empty pass):
  1. builds out\light\light_states_test.exe (tools\light\build_light_states.ps1) and runs it: the record round trip (LS0), the hook's tracker against a fake
     guest (LS1: two thread slots, an install between two calls, a restore, a shadow change without an install, the guards), the contributor's Emit,
     the rows builder's hand-checked TexProjLight (LS3);
  2. the rows builder against the STREAM'S BANKS of Check B (run 7051) and run 7079 (--banks): c197 / c199 / KeyLight / DifLghVec / DifLghCol bit-equal;
  3. the Python port of the rows (tools\skip\light_rows.py, used by the ledger's light_state column) against the C++ builder's rows on a synthetic
     record the C++ test wrote (bit for bit, and the Python record reader reads the C++ writer's tables);
  4. the ledger's light_state column on states built from a real draw's banks (sources the light registers), and dormant behaviour on a capture without tables
     (the column says dormant; --light-state fails loudly);
  5. planted twins, each of which must fail its NAMED check: slot0_always, no_compare, stale_state, state_prev_object (the hook), w_dropped, negate_dirs (the
     rows, against real banks), and at ledger level state_prev_object (refs shifted to the previous object), w_dropped, negate_dirs.
  pre_attach (the dirs read from the light object's +960 copy instead of R: re-finds lane 33's world-fixed lights) needs a capture that carries the light
  object's second direction copy AND the tables: neither exists yet, so it is reported as SKIP (not run), never as a pass."""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger  # noqa: E402
import light_rows  # noqa: E402
from ledger_io import Registry, Record  # noqa: E402

B_ZIP = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
R_ZIP = 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip'
EXE = os.path.join(ROOT, 'out', 'light', 'light_states_test.exe')
OUT = os.path.join(ROOT, 'out', 'light')
bad = 0


def check(name, ok, why=''):
    global bad
    print('%s  %-30s %s' % ('PASS' if ok else 'FAIL', name, why))
    bad += 0 if ok else 1


def skip(name, why):
    print('SKIP  %-30s %s' % (name, why))


def run_exe(args):
    p = subprocess.run([EXE] + args, capture_output=True, text=True, timeout=300)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def extract_banks(cap_dir, frames_spec, path, reg):
    """The stream's own banks of wrestler draws: col rows c0-c4, dir rows c8-c12, c197, c199 (PS) and VS c4 / c12, for light_states_test --banks."""
    cap = ledger.Capture(cap_dir)
    frames = ledger.parse_frames(frames_spec, cap.n)
    recs = []
    for fi in frames:
        f = cap.frame(fi)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] not in (ledger.SITE_A, ledger.SITE_B):
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            if not vn or not vn.startswith('yCh_') or not d['ps']:
                continue
            pn = reg.variant_of(d['ps'])
            if not pn:
                continue
            pr, vr = reg.read_regs(pn), reg.read_regs(vn)
            if not {0, 1, 2, 3, 4, 8, 9, 10, 11, 12} <= pr:
                continue
            pb, vb = f.bank(d['ps_bank']), f.bank(d['vs_bank'])
            row = lambda b, r: tuple(b[4 * r:4 * r + 4])
            has = (1 if 197 in pr else 0) | (2 if 199 in pr else 0) | (4 if (4 in vr and 12 in vr) else 0)
            rec = []
            for r in range(5):
                rec += row(pb, r)
            for r in range(8, 13):
                rec += row(pb, r)
            rec += row(pb, 197) + row(pb, 199) + row(vb, 4) + row(vb, 12)
            recs.append(rec + [has])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as fh:
        fh.write(struct.pack('<I', len(recs)))
        for r in recs:
            fh.write(struct.pack('<57I', *r))
    return len(recs)


def state_from_banks(cols, dirs, drop_w=False, negate=False):
    """A ledger-style state dict built from a draw's PS bank rows (u32 bits): the 'perfect' state the hook would have read."""
    fl = light_rows.fl
    light = [0.0] * 224
    for k in range(5):
        for c in range(4):
            v = fl(cols[k][c])
            if drop_w and c == 3:
                v = 0.0
            light[24 + 4 * k + c] = v
            dv = fl(dirs[k][c])
            if negate and c < 3:
                dv = -dv
            light[4 + 4 * k + c] = dv
    return dict(light=light, shadow_m=[[0.0] * 16] * 3, shadow_f=[0.0] * 16, shadow_x=[[0.0] * 8] * 2, map_wh=(0.0, 0.0), slot=0, flags=0)


class FakeRec:
    def __init__(self, states, refs):
        self._s, self._r = states, refs

    def count(self, sid):
        return 1

    def light_states(self):
        return self._s

    def light_refs(self):
        return self._r


def main():
    global bad
    frames = '0:340:20'
    if '--frames' in sys.argv:
        frames = sys.argv[sys.argv.index('--frames') + 1]
    main_co = ledger.main_checkout()
    zips = {'checkB': os.path.join(main_co, B_ZIP), 'r7079': os.path.join(main_co, R_ZIP)}
    for z in zips.values():
        if not os.path.isfile(z):
            print('FAIL  input missing: ' + z)
            return 1
    if '--no-build' not in sys.argv:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'light', 'build_light_states.ps1')], capture_output=True, text=True, timeout=900)
        check('build', p.returncode == 0, (p.stdout + p.stderr)[-300:].replace('\n', ' | ') if p.returncode else 'light_states_test.exe')
        if p.returncode:
            return 1
    if not os.path.isfile(EXE):
        print('FAIL  ' + EXE + ' not built')
        return 1
    reg = Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))

    # 1. the C++ test (record, hook, contributor, hand rows)
    rc, out = run_exe([])
    check('cpp.plain', rc == 0 and 'SKIP  rows.bank_proof' in out, '%d checks passed' % out.count('PASS  ') if rc == 0 else out[-600:])
    # 2. the banks of both captures
    banks = {}
    for tag, z in zips.items():
        cap_dir = ledger.find_capture(z)
        path = os.path.join(OUT, 'banks_%s.bin' % tag)
        n = extract_banks(cap_dir, frames, path, reg)
        banks[tag] = path
        rc, out = run_exe(['--banks', path])
        line = [l for l in out.splitlines() if 'rows.c197_c199' in l]
        check('%s.rows_vs_banks' % tag, rc == 0 and bool(line) and 'FAIL' not in out, '%d wrestler draws; %s' % (n, line[0].split('rows.c197_c199')[1].strip() if line else out[-300:]))
        for plant, named in (('w_dropped', 'rows.c197_c199'), ('negate_dirs', 'rows.dif_lgh_vec')):
            rc, out = run_exe(['--banks', path, '--plant', plant])
            check('%s.plant.%s' % (tag, plant), rc == 1 and ('FAIL  ' + named) in out, 'fails %s' % named if rc == 1 and ('FAIL  ' + named) in out else 'DID NOT FAIL %s: rc %d' % (named, rc))
    # hook plants
    for plant, named in (('slot0_always', 'hook.slot1_state'), ('no_compare', 'hook.dedupe_count'), ('stale_state', 'hook.install_between'), ('state_prev_object', 'hook.refs_exact')):
        rc, out = run_exe(['--plant', plant])
        check('plant.%s' % plant, rc == 1 and ('FAIL  ' + named) in out, 'fails %s' % named if rc == 1 and ('FAIL  ' + named) in out else 'DID NOT FAIL %s: rc %d' % (named, rc))
    skip('plant.pre_attach', 'needs a capture carrying the light object\'s +960 direction copy and the tables (neither exists yet)')

    # 3. the Python port vs the C++ rows, on the record the C++ test wrote
    rsc = os.path.join(OUT, 'synth.rsc')
    rc, out = run_exe(['--write-rsc', rsc])
    check('cpp.write_rsc', rc == 0 and os.path.isfile(rsc) and os.path.isfile(rsc + '.rows'), out[-200:] if rc else rsc)
    try:
        rec = Record(rsc)
        states, refs = rec.light_states(), rec.light_refs()
        check('py.reads_cpp_record', len(states) == 2 and refs == [(0x100, 0, 0, 5), (0x100, 1, 1, 5), (0x200, 0, 1, 5)], '%d states, refs %s' % (len(states), refs))
        raw = open(rsc + '.rows', 'rb').read()
        per = 20 + 20 + 4 * 6 + 1 + 16 + 12 + 16 * 3
        fl = struct.unpack('<%dI' % (2 * per), raw)
        okall, why = True, ''
        for si, st in enumerate(states):
            c = fl[si * per:(si + 1) * per]
            t = light_rows.rows_from_state(st)
            exp_vec = [t[8 + k]['*'][0] for k in range(5)]
            exp_col = [t[k]['*'][0] for k in range(5)]
            ok = all(tuple(c[4 * k:4 * k + 4]) == exp_vec[k] for k in range(5)) and all(tuple(c[20 + 4 * k:20 + 4 * k + 4]) == exp_col[k] for k in range(5))
            o = 40
            ok = ok and tuple(c[o:o + 4]) == exp_vec[4] and tuple(c[o + 4:o + 8]) == exp_col[4]
            ok = ok and tuple(c[o + 8:o + 12]) == t[197]['p'][0] and tuple(c[o + 12:o + 16]) == t[199]['p'][0]
            ok = ok and tuple(c[o + 16:o + 20]) in t[181]['*'] and tuple(c[o + 20:o + 24]) in t[181]['*']
            o += 24
            valid = struct.unpack('<f', struct.pack('<I', c[o]))[0] == 1.0
            ok = ok and valid == (68 in t)
            if valid:
                ok = ok and all(tuple(c[o + 1 + 4 * j:o + 5 + 4 * j]) == t[68 + j]['v'][0] for j in range(4))
            ok = ok and all(tuple(c[o + 17 + 4 * j:o + 21 + 4 * j]) == t[168 + j]['p'][0] for j in range(3))
            if not ok:
                okall, why = False, 'state %d differs' % si
        check('py.port_vs_cpp_rows', okall, why or '2 states: DifLghVec/Col, KeyLight, c197, c199, sky/ground, TexProjLight (one with the map size unknown), ShadowPrm bit-equal')
    except (OSError, ValueError, struct.error) as e:
        check('py.port_vs_cpp_rows', False, 'cannot read: %s' % e)

    # 4/5. the ledger's light_state column on states built from real draws (the perfect state), and its plants
    cap_dir = ledger.find_capture(zips['r7079'])
    cap = ledger.Capture(cap_dir)
    f = cap.frame(200)
    draw_info = None
    for i in range(f.ndraws):
        o = f.origin(i)
        if not o or o['klass'] != 1 or o['pass_site'] != ledger.SITE_A:
            continue
        d = f.draw(i)
        vn = reg.variant_of(d['vs'])
        if not vn or not vn.startswith('yCh_') or not d['ps']:
            continue
        pn = reg.variant_of(d['ps'])
        pr = reg.read_regs(pn) if pn else set()
        if {0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 197, 199} <= pr:
            draw_info = (o, f.bank(d['ps_bank']))
            break
    if draw_info is None:
        check('ledger.light_state_draw', False, 'no wrestler draw with all the light registers in frame 200 of run 7079')
    else:
        o, pb = draw_info
        row = lambda r: tuple(pb[4 * r:4 * r + 4])
        cols = [row(r) for r in range(5)]
        dirs = [row(r) for r in range(8, 13)]
        regs = [('p', r) for r in (0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 197, 199) if not set(row(r)) <= ledger.TRIVIAL]  # trivial values are never credited

        def sourced(idx, obj):
            return [(s, r) for s, r in regs if idx.match(obj, s, r, row(r))]

        idx = ledger.LightStateIndex(FakeRec([state_from_banks(cols, dirs)], [(o['object'], 0, 0, 1)]))
        got = sourced(idx, o['object'])
        check('ledger.light_state_sources', got == regs, 'sourced %d of %d non-trivial light registers of a real wrestler draw (of c0-c4, c8-c12, c197, c199)' % (len(got), len(regs)))
        lfi = ledger.LightFrameIndex([FakeRec([state_from_banks(cols, dirs)], [(o['object'], 0, 0, 100)])])
        check('ledger.frame_join', lfi.match(100, o['object'], 'p', 8, row(8)) and not lfi.match(101, o['object'], 'p', 8, row(8)) and not lfi.match(100, o['object'] + 4, 'p', 8, row(8)),
              'states are joined by the hook frame (LightRef.frame) and object: the right frame sources, the next frame and another object do not')
        check('ledger.trivial_excluded', not lfi.match(100, o['object'], 'p', 8, (0, 0, 0, 0)), 'an all-zero value is never credited to the light_state column')
        idx = ledger.LightStateIndex(FakeRec([state_from_banks(cols, dirs)], [(o['object'] + 4, 0, 0, 1)]))
        check('ledger.plant.state_prev_object', sourced(idx, o['object']) == [], 'refs on another object source nothing for this draw (the column joins by engine object)')
        idx = ledger.LightStateIndex(FakeRec([state_from_banks(cols, dirs, drop_w=True)], [(o['object'], 0, 0, 1)]))
        got = sourced(idx, o['object'])
        check('ledger.plant.w_dropped', ('p', 197) not in got and ('p', 199) not in got and ('p', 0) not in got, 'colour w dropped: c0..c4 w, c197, c199 stay unsourced')
        idx = ledger.LightStateIndex(FakeRec([state_from_banks(cols, dirs, negate=True)], [(o['object'], 0, 0, 1)]))
        got = sourced(idx, o['object'])
        check('ledger.plant.negate_dirs', not any(r in (8, 9, 10, 11, 12) for _, r in got), 'negated directions: c8-c12 stay unsourced')

    # dormant on a capture without tables
    rc = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'skip', 'ledger.py'), cap_dir, '--frames', '100:140:20', '-q'], capture_output=True, text=True, timeout=600)
    check('ledger.dormant', rc.returncode == 0 and 'light_state column: dormant' in rc.stdout, 'a capture without tables: the column says dormant')
    rc = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'skip', 'ledger.py'), cap_dir, '--frames', '100:140:20', '-q', '--light-state'], capture_output=True, text=True, timeout=600)
    check('ledger.light_state_loud', rc.returncode == 1 and 'carries light tables' in rc.stdout, 'asked for the column on a capture without tables: FAIL, exit 1 (%s)' % rc.stdout.strip()[:80])
    real_run = ledger.run
    try:
        def boom(*a, **k):
            raise ValueError('LightState element_size 1000, this reader knows 1232')
        ledger.run = boom
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc2 = ledger.main(['x'])
        check('ledger.value_error_fails', rc2 == 1 and buf.getvalue().startswith('FAIL'), 'a record reader ValueError is a FAIL line, exit 1, not a traceback')
    finally:
        ledger.run = real_run
    print('light_states_selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

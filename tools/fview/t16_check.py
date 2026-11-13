"""Session 101 lane 170: the 16-bit target rule check (docs\\renderer\\CARRY_DARKENING.md section 12). fview headless, 0 game runs.

THE RULE: our float targets hold the value the shader wrote. The shadow-map pass stores (d, d, 1, 1) with d in 0..1; a reader of any copy of it gets d back, whatever the fetch's
sign. (The 360's fixed-point storage, v / 32 in 16 bits and v / 64 read back as UNORM, is not reproduced: until lane 170 the fx build divided by 64 and every wrestler shadow map reached
the skin / gear shaders ~64x too faint while its clear colour (1.0) stayed 1.0: one target, two scales.)

Checks, on the match capture (frames 488 and 113 of 20261112_172456, CPU v CPU, the wrestlers 42 and 59 units apart):
  R1  the compile rule: tools\\renderer\\compile_shaders.py fx_target16_divisor('unsigned' / 'signed') == 1.0 (env FX_T16_DIVISOR unset)
  R2  the map pass's stored texel at a Brock pixel (surface 1184, frame 488, pixel 116,237, op 251): blue 250..255 (the shader's constant 1.0; the old rule read 3), red = green in 40..75 (d = 0.22)
  R3  the precondition: no draw of the frame has a colour exp bias, no fetch has an exp_adjust or a non-unsigned sign (tools\\fview\\target16_audit.py census); if a capture ever shows one, the rule is
      not proven for it and this fails loudly
  R4  the carry bar, frame 488 (W0, far weight 0.253): the lowered wrestler within 8% of its own w = 1 arm (the old rule: 38.7%), the other within 0.5 luma
  R5  normal distance, frame 113 (far weight 0.8 / 1.0): the wrestler map's own contribution at w = 1, (E - B) / E on W0, at least 1% (the old rule: 0.00%; 360 footage: wrestlers shadow each other)
      and the lowered wrestler within 3% of its w = 1 arm
  --plant divisor64 (the old rule back: R1, R2, R4, R5 fail)   --plant twice (the rule applied twice, divisor 1/64: washed-out maps, R1, R2, R5 fail)
  --selftest runs both plants and exits 0 only when each is caught.

  py -I tools\\fview\\t16_check.py [--fview EXE] [--case ZIP | --capture DIR] [--plant NAME] [--selftest]
"""
import argparse, glob, json, os, pathlib, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
HERE = pathlib.Path(__file__).resolve().parents[2]
from carry_sheet import luma, THRESH
from PIL import Image


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return pathlib.Path(common).parent
    except Exception:
        pass
    return HERE


def find_capture(a):
    if a.capture:
        return pathlib.Path(a.capture)
    zips = [a.case] if a.case else sorted(glob.glob(str(main_checkout() / 'out' / 's101' / 'runs' / '20261112_172456_s100_owner_match' / 'case_*.zip')))
    if not zips:
        sys.exit('t16_check: the match capture zip is missing (out\\s101\\runs\\20261112_172456_s100_owner_match): FAIL, not SKIP')
    dest = HERE / 'out' / 't16_check' / pathlib.Path(zips[0]).stem
    if not (dest / 'capture' / 'images.bin').exists():
        dest.mkdir(parents=True, exist_ok=True)
        subprocess.run([r'C:\Windows\System32\tar.exe', '-xf', zips[0], '-C', str(dest)], check=True, timeout=600)
    return dest / 'capture'


class Runner:
    def __init__(self, fview, cap, plant):
        self.fview, self.cap, self.plant = str(pathlib.Path(fview).resolve()), str(cap), plant
        self.env = dict(os.environ)
        self.plant_args = []
        if plant:
            self.env['FX_T16_DIVISOR'] = {'divisor64': '64', 'twice': '0.015625'}[plant]
            self.plant_args = ['--apply-reload', 'yShadowMap', '--root', str(HERE)]
        else:
            self.env.pop('FX_T16_DIVISOR', None)
        self.tmp = tempfile.mkdtemp(prefix='t16_', dir=str(HERE / 'out')) if (HERE / 'out').exists() else tempfile.mkdtemp(prefix='t16_')

    def picture(self, frame, extra, tag):
        out = pathlib.Path(self.tmp) / ('f%d_%s' % (frame, tag))
        out.mkdir(parents=True, exist_ok=True)
        cmd = [self.fview, '--capture', self.cap, '--frame', str(frame)] + extra + self.plant_args + ['--judge-out', str(out)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=self.env)
        if r.returncode != 0 or not (out / 'picture.png').exists():
            sys.exit('t16_check: fview failed (%d): %s\n%s' % (r.returncode, ' '.join(cmd), (r.stdout + r.stderr)[-600:]))
        return np.asarray(Image.open(out / 'picture.png').convert('RGB')).astype(np.float64)

    def pixel(self, frame, x, y, surface, through):
        js = pathlib.Path(self.tmp) / 'px.json'
        cmd = [self.fview, '--capture', self.cap, '--frame', str(frame), '--pixel', '%d,%d' % (x, y), '--surface', str(surface), '--through-op', str(through)] + self.plant_args + ['--json', str(js)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=self.env)
        if not js.exists():
            sys.exit('t16_check: pixel history failed: ' + (r.stdout + r.stderr)[-400:])
        return json.load(open(js))


def run_checks(a):
    cap = find_capture(a)
    fails, notes = [], []
    def check(name, ok, text):
        (notes if ok else fails).append('%s %s: %s' % ('PASS' if ok else 'FAIL', name, text))
        print(notes[-1] if ok else fails[-1])

    import importlib
    sys.path.insert(0, str(HERE / 'tools' / 'renderer'))
    saved = os.environ.get('FX_T16_DIVISOR')
    if a.plant:
        os.environ['FX_T16_DIVISOR'] = {'divisor64': '64', 'twice': '0.015625'}[a.plant]
    else:
        os.environ.pop('FX_T16_DIVISOR', None)
    cs = importlib.import_module('compile_shaders')
    d_u, d_s = cs.fx_target16_divisor('unsigned'), cs.fx_target16_divisor('signed')
    check('R1 compile rule', d_u == 1.0 and d_s == 1.0, 'divisor unsigned %g, signed %g (rule: 1, the value the shader wrote)' % (d_u, d_s))
    if saved is not None: os.environ['FX_T16_DIVISOR'] = saved

    run = Runner(a.fview, cap, a.plant)
    hist = run.pixel(488, 116, 237, 1184, 296)
    texel = next((e['after'] for e in hist['entries'] if e['op'] == 251), None)
    ok = texel is not None and 250 <= texel[2] <= 255 and abs(texel[0] - texel[1]) <= 1 and 40 <= texel[0] <= 75
    check('R2 map pass texel', ok, 'surface 1184 pixel (116,237) after op 251 = %s (want blue 250-255 = the shader\'s 1.0, red = green = d 40-75)' % (texel,))

    census = subprocess.run([sys.executable, '-I', str(HERE / 'tools' / 'fview' / 'target16_audit.py'), '--fview', str(pathlib.Path(a.fview).resolve()), '--capture', str(cap), '--frames', '488'], capture_output=True, text=True, timeout=600)
    line = next((l for l in census.stdout.splitlines() if 'census:' in l), '')
    ok = 'exp bias != 0 on 0 draws' in line and 'exp_adjust (word4 13-18 / word3 13-18) != 0 on 0 slots' in line and 'sign != 0 on 0 slots' in line
    check('R3 precondition (no exp bias / exp_adjust / sign in the frame)', ok, line.strip() or census.stdout[-200:] + census.stderr[-200:])

    arms = {'A': [], 'B': ['--const-override', 'ps:170.w=1'], 'E': ['--const-override', 'ps:168.w=0'],
            'M0': ['--draw-drop', '@class=wrestlers@group=0'], 'M1': ['--draw-drop', '@class=wrestlers@group=1']}
    res = {}
    for f in (488, 113):
        im = {k: run.picture(f, e, '%s' % k) for k, e in arms.items()}
        r = {}
        for g, mk in ((0, 'M0'), (1, 'M1')):
            m = np.abs(im[mk] - im['A']).max(axis=2) > THRESH
            la, lb, le = (float(luma(im[k])[m].mean()) for k in 'ABE')
            r[g] = dict(A=la, B=lb, E=le, lowered=100 * (lb - la) / lb, map_pct=100 * (le - lb) / le)
        res[f] = r
    w0, w1 = res[488][0], res[488][1]
    check('R4 carry bar f488', w0['lowered'] < 8.0 and abs(w1['A'] - w1['B']) <= 0.5,
          'W0 (w 0.253) lowered vs its w=1 arm %.2f%% (A %.1f B %.1f; want < 8%%), W1 |A-B| %.2f luma (want <= 0.5)' % (w0['lowered'], w0['A'], w0['B'], abs(w1['A'] - w1['B'])))
    n0 = res[113][0]
    check('R5 normal-distance shadow f113', n0['map_pct'] >= 1.0 and res[113][1]['lowered'] < 3.0 and n0['lowered'] < 3.0,
          'wrestler map contribution at w=1 on W0 %.2f%% (E %.1f B %.1f; want >= 1%%), lowered %.2f%% / %.2f%% (want < 3%%)' % (n0['map_pct'], n0['E'], n0['B'], n0['lowered'], res[113][1]['lowered']))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fview')   # lane 218: default = fview_exe.find_fview (the newest own build; refuses a stale one)
    ap.add_argument('--case'); ap.add_argument('--capture'); ap.add_argument('--plant', choices=['divisor64', 'twice'])
    ap.add_argument('--selftest', action='store_true')
    import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
    fe.add_allow_stale(ap)
    a = ap.parse_args()
    a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev')
    if a.selftest:
        bad = []
        for plant in ('divisor64', 'twice'):
            cmd = [sys.executable, '-I', os.path.abspath(__file__), '--fview', a.fview, '--plant', plant] + (['--case', a.case] if a.case else []) + (['--capture', a.capture] if a.capture else [])
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
            caught = r.returncode == 1
            print('plant %-10s: %s (exit %d)\n%s' % (plant, 'caught' if caught else 'NOT CAUGHT', r.returncode, '\n'.join('   ' + l for l in r.stdout.splitlines() if l.startswith(('PASS', 'FAIL')))))
            if not caught: bad.append(plant)
        print('t16_check selftest: %s' % ('FAIL, not caught: ' + ', '.join(bad) if bad else 'PASS (both plants caught)'))
        sys.exit(1 if bad else 0)
    fails = run_checks(a)
    print('t16_check: %s' % ('FAIL (%d)' % len(fails) if fails else 'PASS'))
    sys.exit(1 if fails else 0)


main()

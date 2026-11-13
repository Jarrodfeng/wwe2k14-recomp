r"""Lane SN (S101 lane 86, SH0) gate driver for src\renderer_adapter\shadow_roles.h: shadow casters chosen by draw origin and surface role, not by shader hash.

  py -I tools\shadow\shadow_roles.py check      build tools\shadow\shadow_roles_check.cpp (the shipped header) and run it on Check B (brock), Bryan and wm30:
                                                clean (exit 0), plants hash_only and rim_as_shadow (each exit 1 with the named checks failing), a missing input (exit 1)
  py -I tools\shadow\shadow_roles.py pictures   rplay (this checkout's out\renderer\dev\rplay.exe, built by tools\renderer\build.ps1) plays Check B and Bryan with SHADOWS off,
                                                SHADOWS on and the half-rate maps, once keyed by origin / role and once by the old hash lists (--shadow-keying hash):
                                                the pictures must be byte-equal; the SHADOWS-off picture must differ from the on picture; the planted rim_as_shadow picture must
                                                differ from the clean one (the controls: an A/B that cannot differ proves nothing)

0 game runs. Captures: the case zips of the three S100 owner runs (the same ones the shadow census reads), extracted to <main checkout>\out\lane86\<name>\capture
(game imagery: local only, never committed)."""
import os
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    try:
        common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()
RUNS = MAIN / 'out' / 's101' / 'runs'
ZIPS = {
    'brock': RUNS / '20261111_215201_s100_owner_s89_brock' / 'case_20261111_215233_217_0_pc_run_exit.zip',
    'bryan': RUNS / '20261111_174549_s100_owner_bryan_entrance' / 'case_20261111_174607_799_0_pc_run_exit.zip',
    'wm30': RUNS / '20261111_174707_s100_owner_s90_wm30' / 'case_20261111_174739_724_0_pc_run_exit.zip',
}
WORK = MAIN / 'out' / 'lane86'
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
EXE = ROOT / 'out' / 'l86' / 'shadow_roles_check.exe'
RPLAY = ROOT / 'out' / 'renderer' / 'dev' / 'rplay.exe'
# plant -> checks that must fail
PLANTS = {'hash_only': {'casters', 'targets'}, 'rim_as_shadow': {'casters', 'targets', 'rim', 'squares'}}


def capture(name):
    folder = WORK / name / 'capture'
    if (folder / 'images.bin').exists():
        return folder
    z = ZIPS[name]
    if not z.exists():
        print('FAIL  cannot read %s (a check that reads nothing must fail)' % z)
        sys.exit(1)
    (WORK / name).mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(z) as zf:
        for m in zf.namelist():
            if m.startswith('capture/') and not m.endswith('/'):
                zf.extract(m, WORK / name)
    if not (folder / 'images.bin').exists():
        print('FAIL  %s holds no capture\\images.bin' % z)
        sys.exit(1)
    return folder


def build_checker():
    src = HERE / 'shadow_roles_check.cpp'
    deps = [src, ROOT / 'src' / 'renderer_adapter' / 'shadow_roles.h', ROOT / 'src' / 'renderer_adapter' / 'names.h', ROOT / 'src' / 'renderer_adapter' / 'surface_role_table.h',
            ROOT / 'src' / 'renderer_adapter' / 'capture_reader.h', ROOT / 'src' / 'renderer_adapter' / 'adapter_frame.h']
    if EXE.exists() and EXE.stat().st_mtime > max(d.stat().st_mtime for d in deps):
        return
    EXE.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(ROOT / 'src'), str(src), '-o', str(EXE)],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        print('FAIL  build of shadow_roles_check.cpp')
        sys.exit(1)


def run_checker(args, names):
    r = subprocess.run([str(EXE)] + args + ['-q'] + ['%s=%s' % (n, capture(n)) for n in names], capture_output=True, text=True, timeout=900)
    failed = {l.split()[1].split('.')[-1].rstrip(':') for l in r.stdout.splitlines() if l.startswith('FAIL') and len(l.split()) > 1}
    return r.returncode, failed, r.stdout


def cmd_check():
    build_checker()
    bad = 0
    names = ['brock', 'bryan', 'wm30']
    code, failed, out = run_checker([], names)
    ok = code == 0 and not failed
    print('%s  clean: exit %d, failed checks %s' % ('PASS' if ok else 'FAIL', code, sorted(failed) or 'none'))
    if not ok:
        print(out)
    bad += not ok
    for plant, want in PLANTS.items():
        code, failed, out = run_checker(['--plant', plant], ['brock'])
        ok = code == 1 and want <= failed
        print('%s  plant %-14s: exit %d, failed checks %s (must include %s)' % ('PASS' if ok else 'FAIL', plant, code, sorted(failed), sorted(want)))
        if not ok:
            print(out)
        bad += not ok
    r = subprocess.run([str(EXE), '-q', 'brock=' + str(WORK / 'no_such_capture')], capture_output=True, text=True, timeout=120)
    ok = r.returncode == 1 and 'cannot open' in r.stdout
    print('%s  missing input: exit %d (must fail loudly)' % ('PASS' if ok else 'FAIL', r.returncode))
    bad += not ok
    print('%d problems' % bad)
    return 1 if bad else 0


def play(name, tag, shot, frames, extra):
    out = WORK / 'pics' / ('%s_%s' % (name, tag))
    if out.exists():
        for f in out.glob('*.png'):
            f.unlink()
    r = subprocess.run([str(RPLAY), '--capture', str(capture(name)), '--frames', str(frames), '--shots', str(shot), '--shot-dir', str(out)] + extra, capture_output=True, text=True, timeout=600)
    png = out / ('frame_%04d.png' % shot)
    if r.returncode != 0 or not png.exists():
        print((r.stdout + r.stderr)[-1500:])
        print('FAIL  rplay %s %s: exit %d, picture %s' % (name, tag, r.returncode, 'written' if png.exists() else 'missing'))
        sys.exit(1)
    return png.read_bytes()


def cmd_pictures():
    if not RPLAY.exists():
        print('FAIL  no rplay at %s (build it: pwsh -NoProfile -File tools\\renderer\\build.ps1)' % RPLAY)
        return 1
    bad = 0
    # name -> (frames played, the shot frame: even = a half-rate skip frame, frame_number_ odd and > 8)
    for name, frames, shot in (('brock', 201, 200), ('bryan', 91, 90)):
        pic = {}
        for tag, extra in (('on_role', []), ('on_hash', ['--shadow-keying', 'hash']),
                           ('off_role', ['--shadows', 'off']), ('off_hash', ['--shadows', 'off', '--shadow-keying', 'hash']),
                           ('half_role', ['--shadow-half']), ('half_hash', ['--shadow-half', '--shadow-keying', 'hash']),
                           ('off_plant', ['--shadows', 'off', '--shadow-keying', 'rim_as_shadow']),
                           ('half_plant', ['--shadow-half', '--shadow-keying', 'rim_as_shadow'])):
            pic[tag] = play(name, tag, shot, frames, extra)
        for label, a, b, equal in (('shadows on: role == hash', 'on_role', 'on_hash', True),
                                   ('shadows OFF: role == hash', 'off_role', 'off_hash', True),
                                   ('half rate: role == hash', 'half_role', 'half_hash', True),
                                   ('control: shadows off != on', 'off_role', 'on_role', False),
                                   ('control: half rate != full rate', 'half_role', 'on_role', False),
                                   ('control: planted rim_as_shadow (shadows off) != clean', 'off_plant', 'off_role', False),
                                   ('control: planted rim_as_shadow (half rate) != clean', 'half_plant', 'half_role', False)):
            same = pic[a] == pic[b]
            ok = same == equal
            print('%s  %s %s: %s' % ('PASS' if ok else 'FAIL', name, label, ('byte-equal' if same else 'differ') + ' (%d / %d bytes)' % (len(pic[a]), len(pic[b]))))
            bad += not ok
    print('%d problems' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    if mode == 'check':
        sys.exit(cmd_check())
    if mode == 'pictures':
        sys.exit(cmd_pictures())
    print(__doc__)
    sys.exit(2)

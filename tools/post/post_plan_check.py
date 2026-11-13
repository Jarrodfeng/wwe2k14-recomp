r"""Lane PS (S101): runs the post plan's unit test (out\tools\post_plan_test.exe, built by tools\post\build_post_plan_test.ps1) on the 10 captures lane PC's census reads.

  py -I tools\post\post_plan_check.py [--plant NAME] [--exe PATH] [-q]

The three case zips are unpacked first (frames and images.bin only, nothing else) into <main checkout>\out\post_plan_cache\<label>\ - a capture folder the C++ reader opens; an
unpack that is already complete is reused. The seven version-1 captures are read in place. 0 game runs. Exit code = the test's (1 on any FAIL, and when an input cannot be read)."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


M = main_checkout()
RUNS = os.path.join(M, 'out', 's101', 'runs')
ZIPS = [('brock', os.path.join(RUNS, '20261111_215201_s100_owner_s89_brock', 'case_20261111_215233_217_0_pc_run_exit.zip')),
        ('bryan', os.path.join(RUNS, '20261111_174549_s100_owner_bryan_entrance', 'case_20261111_174607_799_0_pc_run_exit.zip')),
        ('wm30', os.path.join(RUNS, '20261111_174707_s100_owner_s90_wm30', 'case_20261111_174739_724_0_pc_run_exit.zip'))]
FOLDERS = [('match300', 's100_match300'), ('s89k_a', 's100_s89k_a'), ('wm30_300', 's100_wm30_300'), ('brock_tron', 'brock_tron'), ('brock_s15_300', 'brock_s15_300'),
           ('heenan', 's100_heenan_build'), ('cards', 's100cards')]
CACHE = os.path.join(M, 'out', 'post_plan_cache')


def unpack(label, path):
    dest = os.path.join(CACHE, label)
    z = zipfile.ZipFile(path)
    names = z.namelist()
    frames = sorted(n for n in names if re.search(r'(^|/)frames/frame_\d+\.bin$', n))
    images = next((n for n in names if n.endswith('images.bin')), None)
    if not frames or not images:
        raise SystemExit('FAIL  %s: no frames / images.bin in %s' % (label, path))
    os.makedirs(os.path.join(dest, 'frames'), exist_ok=True)
    have = set(os.listdir(os.path.join(dest, 'frames')))
    if len(have) != len(frames) or not os.path.exists(os.path.join(dest, 'images.bin')):
        for n in frames:
            open(os.path.join(dest, 'frames', os.path.basename(n)), 'wb').write(z.read(n))
        open(os.path.join(dest, 'images.bin'), 'wb').write(z.read(images))
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plant', default='')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'out', 'tools', 'post_plan_test.exe'))
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    if not os.path.exists(a.exe):
        print('FAIL  %s does not exist (build it: pwsh -NoProfile -File tools\\post\\build_post_plan_test.ps1)' % a.exe)
        return 1
    caps = []
    for label, path in ZIPS:
        if not os.path.exists(path):
            print('FAIL  cannot read %s (a check that reads nothing must fail)' % path)
            return 1
        caps.append('%s=%s' % (label, unpack(label, path)))
    for label, folder in FOLDERS:
        p = os.path.join(M, 'out', 'captures', folder)
        if not os.path.isdir(p):
            print('FAIL  cannot read %s' % p)
            return 1
        caps.append('%s=%s' % (label, p))
    cmd = [a.exe, '--expect', os.path.join(ROOT, 'tools', 'post', 'expect.json')] + (['--plant', a.plant] if a.plant else []) + (['-q'] if a.q else []) + caps
    r = subprocess.run(cmd, text=True, capture_output=True, timeout=1500)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())

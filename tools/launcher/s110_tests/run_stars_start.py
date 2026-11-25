"""S110 HELPERSTART: build and run stars_start_test.cpp. The real header must pass; every planted copy of it (one rule changed each) must FAIL,
else the test cannot see the fault it exists for. Exit 0 = all as required.
    py -I tools\\launcher\\s110_tests\\run_stars_start.py [--root DIR]
"""
import argparse, os, subprocess, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')))
a = ap.parse_args()
root = a.root
src = os.path.join(root, 'tools', 'launcher', 's110_tests', 'stars_start_test.cpp')
hdr_path = os.path.join(root, 'src', 'launcher', 'stars_start.h')
if not os.path.isfile(src) or not os.path.isfile(hdr_path):
    print('run_stars_start: FAIL: test source or src/launcher/stars_start.h missing under', root)
    sys.exit(1)
clang = next((c for c in (r'C:\Program Files\LLVM\bin\clang++.exe', 'clang++') if c == 'clang++' or os.path.isfile(c)), 'clang++')
tmp = tempfile.mkdtemp(prefix='stars_start_')


def build_run(tag, inc):
    exe = os.path.join(tmp, tag + '.exe')
    b = subprocess.run([clang, '-std=c++20', '-I', inc, src, '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('run_stars_start: FAIL: compile', tag, b.stderr[-800:])
        sys.exit(1)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


rc, out = build_run('real', os.path.join(root, 'src', 'launcher'))
print(out.strip().splitlines()[-1] if out.strip() else '(no output)')
if rc != 0:
    print(out)
    print('run_stars_start: FAIL: the real header fails its own test')
    sys.exit(1)

hdr = open(hdr_path, encoding='utf-8').read().replace('\r\n', '\n')
PLANTS = {
    'a running build no longer waits': ('  if (a.build_running) return Step::kWait;', ''),
    'a test run builds (review S2 dropped)': (' || a.built_for_this || a.test_run)', ' || a.built_for_this)'),
    'built-for-this ignored (a build loop)': (' || a.built_for_this || a.test_run', ' || a.test_run'),
    'the check read before the filters': ('  if (a.build_running) return Step::kWait;', '  const bool need = check();\n  if (a.build_running) return Step::kWait;'),
    'a helper offered PLAY WITHOUT THEM': ('return t == Then::kPlay; }', 'return true; }'),
    "PLAY's wait line changed": ('default: return "Your superstars are being built into the game: the game starts when that is done";',
                                 'default: return "Your superstars are being built into the game";'),
}
bad_count = 0
for name, (old, new) in PLANTS.items():
    if hdr.count(old) != 1:
        print(f'run_stars_start: FAIL: planted edit "{name}" does not apply (the header changed shape)')
        sys.exit(1)
    pdir = os.path.join(tmp, 'inc_' + str(bad_count))
    os.makedirs(pdir)
    open(os.path.join(pdir, 'stars_start.h'), 'w', encoding='utf-8').write(hdr.replace(old, new))
    rc2, out2 = build_run('plant_' + str(bad_count), pdir)
    bad_count += 1
    last = out2.strip().splitlines()[-1] if out2.strip() else '(no output)'
    print(f'planted "{name}": {last}')
    if rc2 == 0:
        print(f'run_stars_start: FAIL: the planted fault "{name}" was NOT caught')
        sys.exit(1)
print(f'run_stars_start: PASS (the real header passes; {bad_count} planted faults each caught)')
sys.exit(0)

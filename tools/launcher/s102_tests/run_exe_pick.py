"""Session 102 lane W1: build and run exe_pick_test.cpp. The real header must pass; every planted copy of it (one rule removed each) must FAIL,
else the test cannot see the fault it exists for. Exit 0 = all as required.
    py -I tools\\launcher\\s102_tests\\run_exe_pick.py [--root DIR]
"""
import argparse, os, subprocess, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')))
a = ap.parse_args()
root = a.root
src = os.path.join(root, 'tools', 'launcher', 's102_tests', 'exe_pick_test.cpp')
hdr_path = os.path.join(root, 'src', 'launcher', 'exe_pick.h')
if not os.path.isfile(src) or not os.path.isfile(hdr_path):
    print('run_exe_pick: FAIL: test source or src/launcher/exe_pick.h missing under', root)
    sys.exit(1)
clang = next((c for c in (r'C:\Program Files\LLVM\bin\clang++.exe', 'clang++') if c == 'clang++' or os.path.isfile(c)), 'clang++')
tmp = tempfile.mkdtemp(prefix='exe_pick_')


def build_run(tag, inc):
    exe = os.path.join(tmp, tag + '.exe')
    b = subprocess.run([clang, '-std=c++20', '-I', inc, src, '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('run_exe_pick: FAIL: compile', tag, b.stderr[-800:])
        sys.exit(1)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


rc, out = build_run('real', os.path.join(root, 'src', 'launcher'))
print(out.strip().splitlines()[-1] if out.strip() else '(no output)')
if rc != 0:
    print(out)
    print('run_exe_pick: FAIL: the real header fails its own test')
    sys.exit(1)

hdr = open(hdr_path, encoding='utf-8').read().replace('\r\n', '\n')
PLANTS = {
    'no AVX2 check': ('c.bmi1 && c.avx2 && c.bmi2', 'c.bmi1 && c.bmi2'),
    'no F16C check (review H)': (' && c.f16c;', ';'),
    'no version match (review F)': ('v3_file_present && versions_match && V3Capable(cpu)', 'v3_file_present && V3Capable(cpu)'),
    'explicit v3 path kept on a CPU without v3 (review E)': ('leaf == "wwe2k14_v3.exe" && !V3Capable(cpu)', 'false'),
    'extra flags ignored (review D)': ('UseV3(ParseBuildChoice(extra_flags), v3_file_present', 'UseV3(BuildChoice::kAuto, v3_file_present'),
    'base override ignored': ('if (choice == BuildChoice::kBase) return false;', ''),
}
bad_count = 0
for name, (old, new) in PLANTS.items():
    if hdr.count(old) != 1:
        print(f'run_exe_pick: FAIL: planted edit "{name}" does not apply (the header changed shape)')
        sys.exit(1)
    pdir = os.path.join(tmp, 'inc_' + str(bad_count))
    os.makedirs(pdir)
    open(os.path.join(pdir, 'exe_pick.h'), 'w', encoding='utf-8').write(hdr.replace(old, new))
    rc2, out2 = build_run('plant_' + str(bad_count), pdir)
    bad_count += 1
    last = out2.strip().splitlines()[-1] if out2.strip() else '(no output)'
    print(f'planted "{name}": {last}')
    if rc2 == 0:
        print(f'run_exe_pick: FAIL: the planted fault "{name}" was NOT caught')
        sys.exit(1)
print(f'run_exe_pick: PASS (real header passes; {len(PLANTS)} planted faults all caught)')

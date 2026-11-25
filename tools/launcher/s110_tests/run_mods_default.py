"""S110 HELPERSTART review: build and run mods_default_test.cpp against src/dlc_catalog.h (LauncherModsDefault). The real header must
pass; every planted copy (one rule changed each) must FAIL. Exit 0 = all as required.
    py -I tools\\launcher\\s110_tests\\run_mods_default.py [--root DIR]
"""
import argparse, os, subprocess, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')))
root = ap.parse_args().root
src = os.path.join(root, 'tools', 'launcher', 's110_tests', 'mods_default_test.cpp')
hdr_path = os.path.join(root, 'src', 'dlc_catalog.h')
if not os.path.isfile(src) or not os.path.isfile(hdr_path):
    print('run_mods_default: FAIL: test source or src/dlc_catalog.h missing under', root)
    sys.exit(1)
clang = next((c for c in (r'C:\Program Files\LLVM\bin\clang++.exe', 'clang++') if c == 'clang++' or os.path.isfile(c)), 'clang++')
tmp = tempfile.mkdtemp(prefix='mods_default_')


def build_run(tag, inc):
    exe = os.path.join(tmp, tag + '.exe')
    b = subprocess.run([clang, '-std=c++20', '-I', inc, src, '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('run_mods_default: FAIL: compile', tag, b.stderr[-800:])
        sys.exit(1)
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.returncode, r.stdout


rc, out = build_run('real', os.path.join(root, 'src'))
print(out.strip().splitlines()[-1] if out.strip() else '(no output)')
if rc != 0:
    print(out)
    print('run_mods_default: FAIL: the real header fails its own test')
    sys.exit(1)

hdr = open(hdr_path, encoding='utf-8').read().replace('\r\n', '\n')
PLANTS = {
    # the code before the review: no launcher default, the game's <game data>\..\mods in a release too
    'no launcher default (the unfixed rule)': ('  return exe_dir / "mods";\n}', '  return {};\n}'),
    'the dev layout ignored': ('    if (fs::exists(d / "game_data" / "extracted" / "default.xex", ec)) return {};',
                               '    if (false) return {};'),
    'the folder itself not looked at': ('  for (fs::path d = exe_dir; !d.empty(); d = d.parent_path()) {',
                                        '  for (fs::path d = exe_dir.parent_path(); !d.empty(); d = d.parent_path()) {'),
    'any game_data folder counts': ('    if (fs::exists(d / "game_data" / "extracted" / "default.xex", ec)) return {};',
                                    '    if (fs::exists(d / "game_data" / "extracted", ec)) return {};'),
}
n = 0
for name, (old, new) in PLANTS.items():
    if hdr.count(old) != 1:
        print(f'run_mods_default: FAIL: planted edit "{name}" does not apply (the header changed shape)')
        sys.exit(1)
    pdir = os.path.join(tmp, 'inc_' + str(n))
    os.makedirs(pdir)
    open(os.path.join(pdir, 'dlc_catalog.h'), 'w', encoding='utf-8').write(hdr.replace(old, new))
    rc2, out2 = build_run('plant_' + str(n), pdir)
    n += 1
    last = out2.strip().splitlines()[-1] if out2.strip() else '(no output)'
    print(f'planted "{name}": {last}')
    if rc2 == 0:
        print(f'run_mods_default: FAIL: the planted fault "{name}" was NOT caught')
        sys.exit(1)
print(f'run_mods_default: PASS (the real header passes; {n} planted faults each caught)')
sys.exit(0)

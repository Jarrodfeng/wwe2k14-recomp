"""S101 lane 118: shared helpers of the N2 selftests (where the kept walks are, the C++ test exe, the gate-style report). 0 game runs."""
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)

# The two kept CAW walk runs (Check B, built with names.h section 69), docs/research/s101/N2_EDITOR_DRIVER.md.
WALKS = ['20261112_122353_s101_checkB_caw_walk', '20261112_121944_s101_checkB_caw_walk']
# Lane 117 extracted the first one under the folder name "case"; accepted when its manifest names the same zip.
LEGACY = {'20261112_122353_s101_checkB_caw_walk': 'case'}

results = []


def report(name, ok, evidence):
    results.append(ok)
    print(('PASS' if ok else 'FAIL') + '  ' + name + ': ' + evidence, flush=True)


def main_checkout():
    if os.path.isdir(os.path.join(REPO, 'out', 's101', 'runs')):
        return REPO
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return REPO


def capture_dir(main, run):
    """-> the extracted capture folder of a kept walk (extracting the case zip once, under out\\s101\\n2_editor\\, when it is not there). Raises loudly."""
    runs = os.path.join(main, 'out', 's101', 'runs', run)
    zips = sorted(f for f in os.listdir(runs) if f.endswith('.zip')) if os.path.isdir(runs) else []
    if not zips:
        raise SystemExit('FAIL  the kept walk %s has no case zip under %s' % (run, runs))
    stem = zips[0][:-4]
    base = os.path.join(main, 'out', 's101', 'n2_editor')
    names = [run[:15]] + ([LEGACY[run]] if run in LEGACY else [])
    for n in names:
        cap = os.path.join(base, n, 'capture')
        man = os.path.join(base, n, 'manifest.json')
        if os.path.isdir(os.path.join(cap, 'frames')) and os.path.exists(man) and stem in open(man, encoding='utf-8').read():
            return cap
    dest = os.path.join(base, run[:15])
    with zipfile.ZipFile(os.path.join(runs, zips[0])) as z:
        z.extractall(dest)
    return os.path.join(dest, 'capture')


def test_exe():
    exe = os.path.join(REPO, 'out', 'tools', 'caw_text_test.exe')
    if not os.path.exists(exe):
        raise SystemExit('FAIL  %s is missing: run tools\\n2\\build_caw_text_test.ps1 first (gate check n2_caw_text.build)' % exe)
    return exe


def cpp_json_lines(cap):
    """The shipped C++ Json() (what `!caw since` answers) per frame of a capture, as a list of str."""
    p = subprocess.run([test_exe(), cap, '--json'], capture_output=True, timeout=600)
    if p.returncode != 0:
        raise SystemExit('FAIL  caw_text_test --json exit %d: %s' % (p.returncode, p.stderr.decode('utf-8', 'replace')[:300]))
    return [x for x in p.stdout.decode('utf-8').replace('\r', '').split('\n') if x]


def cpp_lines(cap, plant=None):
    """The shipped C++ reader's line per frame (tools\\n2\\caw_text_test.cpp), as a list of str."""
    cmd = [test_exe(), cap] + (['--plant', plant] if plant else [])
    p = subprocess.run(cmd, capture_output=True, timeout=600)
    if p.returncode != 0:
        raise SystemExit('FAIL  caw_text_test %s exit %d: %s' % (' '.join(cmd[1:]), p.returncode, p.stderr.decode('utf-8', 'replace')[:300]))
    return [x for x in p.stdout.decode('utf-8').replace('\r', '').split('\n') if x]

#!/usr/bin/env python3
"""S102 lane T19 check (offline, 0 game runs): wwe2k14_rdr_crowd_run_markers (one marker object per run of same-type stands members, expanded by our renderer).

  py -I tools\\crowd\\crowd_run_markers_check.py [--root DIR] [--syntax | --fview [--no-build]]

Default: builds tools\\crowd\\crowd_run_markers_test.cpp (with -DRDR_CROWD_RUNS_PLANTS) against the tree's own record_io.cpp, live_scene.cpp and crowd_contrib.cpp and runs it:
the expanded marker list must equal the plain list object for object (synthetic walk, switch flipping, T11 ids on, pose objects, EndFrame between two contributors with engine
objects), the file form (version 2 for markers, version 1 untouched, old records read, bad markers refused), Compact then Expand = identity, the microbenchmark, the 4 planted twins.
Then the capture arms: every 5th scene record of the Bryan and match captures (case zips of the MAIN checkout) and out\\lane86\\brock\\capture are fed through the contributor's hooks, plain
vs markers; and 3 planted twins run on each capture must FAIL there.
--syntax: clang -fsyntax-only of the game's translation units that include the touched headers, with the main build's flags (as crowd_ids_check.py).
--fview: fview replays (hash per frame, validation 1, exit code) of the Brock, Bryan and match captures, the switch --crowd-run-markers off vs on, under --crowd-ours alone, with the
default bundle's crowd flags (--crowd-floor --crowd-cards), and Brock with T13's member cut (the thin capture): every frame's hash must be equal, validation errors 0, exit 0.
A check that cannot read its input fails loudly."""
import argparse
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
TUS = ['src/main.cpp', 'src/renderer_adapter/live.cpp', 'src/scene_live/crowd_contrib.cpp', 'src/renderer_scene/record_io.cpp', 'src/renderer_scene/crowd_ours.cpp', 'src/renderer_scene/live_scene.cpp', 'src/renderer_scene/live_sinks.cpp', 'src/case/case_scene.cpp']
GAME_FLAGS = ['-DNOMINMAX', '-DRDR_HAS_STREAMLINE=1', '-DREXGLUE_BUILD_CONFIG="Release"', '-DREX_HAS_D3D12=1', '-DREX_HAS_VULKAN=1', '-DSPDLOG_COMPILED_LIB',
              '-DSPDLOG_FMT_EXTERNAL', '-DWIN32_LEAN_AND_MEAN', '-DWWE2K14_HAS_ICON=1', '-msse4.1', '-D_DLL', '-D_MT', '-O1', '-std=gnu++23', '-fsyntax-only', '-Wno-everything']
CAPS = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
BROCK_DIR = 'out/lane86/brock/capture/scene'
BROCK_ZIP = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'


def main_checkout(root):
    out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True)
    if out.returncode:
        return None
    return os.path.dirname(os.path.abspath(os.path.join(root, out.stdout.strip())))


def fail(msg):
    print('FAIL: ' + msg)
    return 1


def extract_rsc(zpath, out_dir, step):
    """Every `step`th scene record of a case zip into out_dir (frame_NNNNNN.rsc)."""
    os.makedirs(out_dir, exist_ok=True)
    zf = zipfile.ZipFile(zpath)
    names = sorted(n for n in zf.namelist() if n.endswith('.rsc'))
    if not names:
        return 0
    n = 0
    for i, name in enumerate(names):
        if i % step:
            continue
        with open(os.path.join(out_dir, os.path.basename(name)), 'wb') as f:
            f.write(zf.read(name))
        n += 1
    return n


def sample_dir(src, out_dir, step):
    os.makedirs(out_dir, exist_ok=True)
    names = sorted(f for f in os.listdir(src) if f.endswith('.rsc'))
    n = 0
    for i, name in enumerate(names):
        if i % step:
            continue
        with open(os.path.join(src, name), 'rb') as f, open(os.path.join(out_dir, name), 'wb') as g:
            g.write(f.read())
        n += 1
    return n


def python_readers_refuse_v2(root, exe, rsc_dir, name):
    """F1 plant (f): the capture's sampled records written as version 2 files; the python readers must REFUSE them (a control: a version 1 file is read)."""
    import importlib.util
    import pathlib
    v2 = os.path.join(root, 'out', 't19', 'v2_' + name)
    r = subprocess.run([exe, '--rsc-dir', rsc_dir, '--step', '1', '--write-v2', v2], capture_output=True, text=True, timeout=600)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stdout)
    if r.returncode != 0:
        return fail('could not write version 2 records for %s' % name)
    files = sorted(f for f in os.listdir(os.path.join(v2, 'scene')) if f.endswith('.rsc'))
    plain = sorted(f for f in os.listdir(rsc_dir) if f.endswith('.rsc'))
    if not files or not plain:
        return fail('no records to feed the python readers (%s)' % name)

    def load(rel, mod):
        spec = importlib.util.spec_from_file_location(mod, os.path.join(root, rel))
        m = importlib.util.module_from_spec(spec)
        sys.modules[mod] = m
        sys.path.insert(0, os.path.dirname(os.path.join(root, rel)))
        spec.loader.exec_module(m)
        return m
    led = load('tools/skip/ledger_io.py', 'ledger_io_t19')
    try:
        led.Record(os.path.join(rsc_dir, plain[0]))   # the control: version 1 is read
    except Exception as e:
        return fail('ledger_io cannot read a version 1 record (%s)' % e)
    try:
        led.Record(os.path.join(v2, 'scene', files[0]))
        return fail('ledger_io READ a version 2 record silently (the crowd worlds would be dropped)')
    except ValueError as e:
        print('PASS: ledger_io refuses a version 2 record: %s' % str(e)[:90])
    lo = load('tools/census/leftovers.py', 'leftovers_t19')
    cap_v1 = pathlib.Path(os.path.join(root, 'out', 't19', 'v1_' + name))
    os.makedirs(cap_v1 / 'scene', exist_ok=True)
    for f in plain[:3]:
        open(cap_v1 / 'scene' / f, 'wb').write(open(os.path.join(rsc_dir, f), 'rb').read())
    lo.records_hold_floor(cap_v1)   # the control: a version 1 capture answers (True or False)
    try:
        lo.records_hold_floor(pathlib.Path(v2))
        return fail('leftovers.records_hold_floor ANSWERED for a version 2 capture folder (a silent False)')
    except ValueError as e:
        print('PASS: leftovers refuses a version 2 capture folder: %s' % str(e)[-70:])
    for f in os.listdir(cap_v1 / 'scene'):
        os.remove(cap_v1 / 'scene' / f)
    for f in os.listdir(os.path.join(v2, 'scene')):
        os.remove(os.path.join(v2, 'scene', f))
    for dpath in (cap_v1 / 'scene', cap_v1, pathlib.Path(v2) / 'scene', pathlib.Path(v2)):
        os.rmdir(dpath)
    return 0


def run_test(root):
    if not os.path.isfile(CLANG):
        return fail('%s not found' % CLANG)
    src = os.path.join(root, 'tools', 'crowd', 'crowd_run_markers_test.cpp')
    if not os.path.isfile(src):
        return fail('missing ' + src)
    out_dir = os.path.join(root, 'out', 't19')
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'crowd_run_markers_test.exe')
    if os.path.exists(exe):
        os.remove(exe)
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-D_CRT_SECURE_NO_WARNINGS', '-DRDR_CROWD_RUNS_PLANTS', '-DNOMINMAX', '-o', exe, src,
                        os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'live_sinks.cpp'), os.path.join(root, 'src', 'scene_live', 'crowd_contrib.cpp')], capture_output=True, text=True)
    if b.returncode or not os.path.isfile(exe):
        return fail('crowd_run_markers_test did not compile\n' + b.stdout + b.stderr)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=900)
    text = r.stdout
    print(text.rstrip())
    passes = len(re.findall(r'^PASS ', text, re.M))
    fails = len(re.findall(r'^FAIL ', text, re.M))
    twins = len(re.findall(r'^PASS planted twin', text, re.M))
    if r.returncode != 0 or fails or 'all checks passed' not in text:
        return fail('crowd_run_markers_test exit %d, %d FAIL lines' % (r.returncode, fails))
    if passes < 30 or twins != 9:
        return fail('expected >= 30 PASS lines and 9 planted twins caught (1, 2, 3, 4, b, c, d, 5, 8), got %d and %d (a check that reads nothing proves nothing)' % (passes, twins))
    if len(re.findall(r'^PASS expanded marker list equals the plain list', text, re.M)) != 8 or len(re.findall(r'^PASS EndFrame with a contributor', text, re.M)) != 2:
        return fail('the 8 synthetic arms and 2 EndFrame arms did not all run')
    main = main_checkout(root)
    caps = []
    for name, rel in CAPS.items():
        z = os.path.join(main, rel)
        caps.append((name, z, 'zip'))
    caps.append(('brock', os.path.join(main, BROCK_DIR), 'dir'))
    for name, path, kind in caps:
        if not os.path.exists(path):
            return fail('capture %s cannot be read: %s' % (name, path))
        d = os.path.join(out_dir, 'rsc_' + name)
        n = extract_rsc(path, d, 5) if kind == 'zip' else sample_dir(path, d, 5)
        if n == 0:
            return fail('no scene records in capture %s: the capture arm would prove nothing' % name)
        c = subprocess.run([exe, '--rsc-dir', d, '--step', '1'], capture_output=True, text=True, timeout=900)
        print(c.stdout.rstrip())
        if c.returncode != 0 or 'capture arm passed' not in c.stdout or 'FAIL' in c.stdout:
            return fail('the capture arm failed on %s' % name)
        if 'RENDER-SIDE full records' not in c.stdout:
            return fail('the capture arm on %s printed no render-side numbers for the full records' % name)
        for plant in (1, 2, 3, 4):
            p = subprocess.run([exe, '--rsc-dir', d, '--step', '1', '--plant', str(plant)], capture_output=True, text=True, timeout=900)
            if p.returncode == 0 or 'FAIL' not in p.stdout:
                return fail('planted twin %d was NOT caught on the %s capture' % (plant, name))
            print('PASS: planted twin %d is caught on the %s capture' % (plant, name))
        rc = python_readers_refuse_v2(root, exe, d, name)
        if rc:
            return rc
        for f in os.listdir(d):   # (scratch: read, then gone)
            os.remove(os.path.join(d, f))
        os.rmdir(d)
    print('PASS: crowd_run_markers_test: %d checks, %d planted twins caught, 3 capture arms with 4 twins each and the python readers refusing version 2, exit 0' % (passes, twins))
    return 0


def run_syntax(root):
    main = main_checkout(root)
    if not main or not os.path.isdir(os.path.join(main, 'generated', 'default')):
        return fail('main checkout with generated\\default not found (%s): the game units cannot be syntax-checked' % main)
    wt = os.path.abspath(root).replace('\\', '/')
    m = main.replace('\\', '/')
    inc = ['-I' + wt, '-I' + wt + '/src', '-I' + m, '-I' + m + '/src', '-I' + m + '/generated/default', '-I' + m + '/thirdparty/streamline/sdk/include',
           '-isystem', m + '/thirdparty/rexglue-sdk-vulkan/include', '-isystem', m + '/thirdparty/rexglue-sdk-vulkan/include/dxc']
    bad = 0
    for tu in TUS:
        p = os.path.join(wt, tu)
        if not os.path.isfile(p):
            return fail('missing ' + p)
        r = subprocess.run([CLANG] + GAME_FLAGS + inc + [p], capture_output=True, text=True)
        print('%s %s' % ('ok  ' if r.returncode == 0 else 'FAIL', tu))
        if r.returncode:
            bad += 1
            print((r.stdout + r.stderr)[:1500])
    if bad:
        return fail('%d game unit(s) do not compile' % bad)
    print("PASS: %d game translation units compile (-fsyntax-only, the main build's flags)" % len(TUS))
    return 0


def run_fview(root, no_build):
    sys.path.insert(0, HERE)
    import crowd_cards_check as ccc
    import member_thin_check as mtc
    main = main_checkout(root)
    game = os.path.join(main, 'game_data', 'extracted')
    zips = {'brock': os.path.join(main, BROCK_ZIP), 'bryan': os.path.join(main, CAPS['bryan']), 'match': os.path.join(main, CAPS['match'])}
    for need in list(zips.values()) + [game]:
        if not os.path.exists(need):
            return fail('input missing: ' + need)
    if not no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            return fail('fview build (exit %d)' % p.returncode)
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 't19', 'fview')
    os.makedirs(work, exist_ok=True)
    R = ccc.Result()
    arms = []   # (label, zip, flags)
    for n, z in zips.items():
        arms.append((n + ' crowd-ours', z, []))
        arms.append((n + ' default bundle (floor + cards)', z, ['--crowd-floor', '--crowd-cards']))
    zt = os.path.join(work, 'brock_thin.zip')
    mtc.cut(root, zips['brock'], zt)
    thin = ['--crowd-members-thin', '--crowd-members-thin-counts', zt[:-4] + '.counts.csv']
    arms.append(('brock with T13 member cut (thin capture)', zt, thin))
    for i, (label, z, flags) in enumerate(arms):
        a = ccc.Arm(exe, z, game, work, 't19_%d_off' % i, flags)
        b = ccc.Arm(exe, z, game, work, 't19_%d_on' % i, flags + ['--crowd-run-markers'])
        diff = b.differing(a)
        R.line(a.rc == 0 and b.rc == 0 and a.valid == 0 and b.valid == 0 and len(a.hashes) > 1 and len(a.hashes) == len(b.hashes) and diff == 0,
               '%-44s picture hash equal on every frame with markers off / on (%d frames, %d differ), validation errors %d / %d, exit %d / %d' % (label, len(a.hashes) - 1, diff, a.valid, b.valid, a.rc, b.rc))
    return 1 if R.fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--fview', action='store_true')
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.syntax:
        return run_syntax(root)
    if a.fview:
        return run_fview(root, a.no_build)
    return run_test(root)


if __name__ == '__main__':
    sys.exit(main())

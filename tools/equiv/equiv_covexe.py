"""S111 EQUIVTIER: the one exe with the stub seam AND branch counts (tools\\equiv\\equiv.py cov-build; docs\\research\\s111\\EQUIV.md).

A test whose closure needs stubs runs only in the coverage tier (the win-amd64-coverage-off preset: src\\coverage_prologue.h's slow path calls
wwe2k14::equiv::OnEntry), and PROVEN needs clang's branch counts of the target's generated file (the covmap variant). This builds both into
one exe the normal way, for every test (with stubs or without):
  1. the commit's coverage lane exe (tools\\exe\\lane_exe.ps1 -Preset win-amd64-coverage-off): a lane slot's build folder then holds that
     commit's coverage objects (skipped when a slot's coverage build is already stamped with the commit);
  2. the slot is claimed the way lane_exe claims it (an open handle on <slot>.owner, read-share only), so no other lane checks it out or
     builds in it while it is read; its stamp (stamp\\wwe2k14_build_stamp.h) must name the commit, clean;
  3. tools\\perf\\guest_variant_exe.py --variant covmap --build-dir <that folder> --tus <the closures' files>: the closures' generated files
     (the slot's own copy, the one its precompiled header includes) with -fprofile-instr-generate -fcoverage-mapping, linked with the slot
     build's other objects (the seam included). <out>\\exe\\variant.json tells equiv.py run which generated copy the counts map to.
The exe is built from a COMMIT (HEAD of this checkout by default): uncommitted changes under src\\ or CMakeLists.txt are refused.
"""
import ctypes
import json
import os
import re
import subprocess
import sys
from ctypes import wintypes

MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
PRESET = 'win-amd64-coverage-off'
STAMP = re.compile(r'#define WWE2K14_BUILD_GIT "([0-9a-f]+)"')
DIRTY = re.compile(r'#define WWE2K14_BUILD_DIRTY (\d+)')


def slot_dir(n):
    return os.path.join(MAIN, 'out', 'wt', f'exe-slot{n}')


def build_dir(n):
    return os.path.join(slot_dir(n), 'out', 'build', PRESET)


def stamp(bd):
    """(git hash prefix, dirty) of a build folder's last build, or None."""
    p = os.path.join(bd, 'stamp', 'wwe2k14_build_stamp.h')
    if not os.path.exists(p) or not os.path.exists(os.path.join(bd, 'wwe2k14.exe')):
        return None
    text = open(p, encoding='utf-8', errors='replace').read()
    g, d = STAMP.search(text), DIRTY.search(text)
    return (g.group(1), d.group(1) if d else '?') if g else None


def stamped_with(bd, full):
    s = stamp(bd)
    return bool(s) and full.startswith(s[0]) and s[1] == '0'


# ---------------------------------------------------------------------------------------------------------------- the slot claim
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
for _f, _a in (('ReadFile', [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]),
               ('WriteFile', [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]),
               ('SetFilePointer', [wintypes.HANDLE, wintypes.LONG, wintypes.LPVOID, wintypes.DWORD]),
               ('SetEndOfFile', [wintypes.HANDLE]), ('FlushFileBuffers', [wintypes.HANDLE]), ('CloseHandle', [wintypes.HANDLE]),
               ('GetExitCodeProcess', [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)])):
    getattr(k32, _f).argtypes = _a
INVALID = wintypes.HANDLE(-1).value


def pid_alive(pid):
    h = k32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = wintypes.DWORD()
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(ok) and code.value == 259   # STILL_ACTIVE


def claim(n, name):
    """The slot's owner handle (tools\\exe\\lane_exe_lib2.ps1 Open-LaneSlotClaim: read/write, read-share only), or None when busy (another
    holder, or a recorded build child still alive)."""
    owner = slot_dir(n) + '.owner'
    h = k32.CreateFileW(owner, 0xC0000000, 0x1, None, 4, 0x80, None)   # GENERIC_READ|WRITE, FILE_SHARE_READ, OPEN_ALWAYS
    if h in (None, INVALID):
        return None
    buf = ctypes.create_string_buffer(4096)
    got = wintypes.DWORD()
    k32.ReadFile(h, buf, 4096, ctypes.byref(got), None)
    m = re.search(rb'child=(\d+)', buf.raw[:got.value])
    if m and pid_alive(int(m.group(1))):
        k32.CloseHandle(h)
        return None
    k32.SetFilePointer(h, 0, None, 0)
    k32.SetEndOfFile(h)
    data = f'pid={os.getpid()}\r\nname={name}\r\n'.encode()
    k32.WriteFile(h, data, len(data), ctypes.byref(got), None)
    k32.FlushFileBuffers(h)
    return h


# ---------------------------------------------------------------------------------------------------------------- the build
def git(repo, *args):
    r = subprocess.run(['git', '-C', repo] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ''


def lane_build(full, log):
    """The commit's coverage lane exe; returns the slot number it was built in. A folder of an earlier cov-build is reused by lane_exe
    without a build (its slot may hold another commit since), so the name takes the first free suffix."""
    c9 = full[:9]
    names = [f'equivcov-{c9}'] + [f'equivcov-{c9}-{k}' for k in range(2, 10)]
    name = next((nm for nm in names if not os.path.exists(os.path.join(MAIN, 'out', 'exe', nm))), None)
    if not name:
        raise SystemExit(f'cov-build: out\\exe\\equivcov-{c9}* folders 1..9 all exist (tidy them)')
    cmd = ['pwsh', '-NoProfile', '-File', os.path.join(MAIN, 'tools', 'exe', 'lane_exe.ps1'), '-Name', name, '-Commit', full,
           '-Preset', PRESET]
    log(f'lane exe: {" ".join(cmd)}')
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=3600)
    out = r.stdout + r.stderr
    for line in out.splitlines()[-14:]:
        log(f'  {line}')
    m = re.search(r'^LANE_EXE (.+)$', out, re.M)
    if r.returncode or not m:
        raise SystemExit(f'cov-build: lane_exe.ps1 exit {r.returncode} (no LANE_EXE line)')
    info = json.load(open(os.path.join(os.path.dirname(m.group(1).strip()), 'exe_info.json'), encoding='utf-8-sig'))
    return int(info['slot'])


def build(tests, tus, repo, name, dry, log=print):
    full = git(repo, 'rev-parse', 'HEAD')
    if not full:
        raise SystemExit(f'cov-build: no HEAD in {repo}')
    dirty = git(repo, 'status', '--porcelain', '--', 'src', 'CMakeLists.txt')
    if dirty:
        raise SystemExit(f'cov-build: the exe is built from the commit {full[:9]}, but src\\ / CMakeLists.txt have uncommitted changes:\n{dirty}')
    name = name or 'equiv_' + '_'.join(t.name.replace('.', '_') for t in tests) + '_' + full[:9]
    slots = [n for n in (1, 2, 3) if stamped_with(build_dir(n), full)]
    log(f'commit {full[:9]}; covmap files {tus}; coverage builds stamped with it: {["slot%d" % n for n in slots] or "none"}')
    if dry:
        log(f'plan: {"slot%d" % slots[0] if slots else "lane_exe.ps1 -Preset " + PRESET + " first"}, then guest_variant_exe.py --variant covmap '
            f'--build-dir <slot>\\out\\build\\{PRESET} --tus {",".join(map(str, tus))} --name {name}')
        return 0
    n = slots[0] if slots else lane_build(full, log)
    h = claim(n, f'equiv cov-build {name}')
    if h is None:
        raise SystemExit(f'cov-build: slot {n} is busy (another lane claimed it after its build): run cov-build again when it is free')
    try:
        bd = build_dir(n)
        if not stamped_with(bd, full):
            raise SystemExit(f'cov-build: slot {n}\'s coverage build is stamped {stamp(bd)}, not {full[:12]} (another lane built in it first)')
        cmd = ['pwsh', '-NoProfile', '-File', os.path.join(MAIN, 'tools', 'runlock.ps1'), 'run', '-Build', '-Name', name, '-TimeoutMinutes', '60',
               '--', sys.executable, os.path.join(repo, 'tools', 'perf', 'guest_variant_exe.py'), '--name', name, '--variant', 'covmap',
               '--build-dir', bd, '--tus', ','.join(map(str, tus))]
        log(f'slot {n} claimed; build: ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
        return subprocess.run(cmd).returncode
    finally:
        k32.CloseHandle(h)

#!/usr/bin/env python3
"""Refuse a game run on an exe built before HEAD's code (S103 H3, audit section 2; run 15 ran an exe built before the fix merged).

  py -I tools\\bench\\exe_vs_head.py --exe PATH [--repo DIR] [--selftest]      exit 0 = fresh (or only docs / tools changed since), 3 = stale, 4 = cannot tell

HOW IT READS THE EXE'S COMMIT (nothing is run): every build writes <build dir>\\stamp\\wwe2k14_build_stamp.h (tools\\build_stamp.cmake: `#define WWE2K14_BUILD_GIT
"<12-hex short HEAD>"` and WWE2K14_BUILD_DIRTY) and links src\\build_stamp.cpp, which embeds that string in the exe. The tool reads the header beside the exe and
CONFIRMS the same string occurs in the exe's bytes (a header from a later build than the exe would otherwise lie); no header, or a hash not found in the exe =
"cannot tell" (4: refused, with the cause). Then `git diff --name-only <exe commit> HEAD -- <code paths>`: any path under src/, config/, patches/, CMakeLists.txt,
tools/build_stamp.cmake, tools/sdk_pins.json (what the exe is built from; docs, tools and notes do not make an exe stale) = stale. An exe commit git does not know = cannot
tell. A dirty exe (WWE2K14_BUILD_DIRTY 1) is judged on its commit and the output says it was dirty (its uncommitted edits cannot be seen).
run_list.ps1 calls this before the run lock; override: -AllowStaleExe or WWE_ALLOW_STALE_EXE=1 (it prints the reason either way)."""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CODE_PATHS = ['src', 'config', 'patches', 'CMakeLists.txt', 'tools/build_stamp.cmake', 'tools/sdk_pins.json']


def git(repo, *a):
    p = subprocess.run(['git', '-C', str(repo), '--no-optional-locks'] + list(a), capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout.strip(), p.stderr.strip()


def exe_stamp(exe):
    """-> (git12, dirty, None) or (None, None, why)."""
    exe = Path(exe)
    hdr = exe.parent / 'stamp' / 'wwe2k14_build_stamp.h'
    if not exe.is_file():
        return None, None, f'{exe} does not exist'
    if not hdr.is_file():
        return None, None, f'no {hdr} beside the exe (a copied or old exe has no readable stamp)'
    text = hdr.read_text(encoding='utf-8', errors='replace')
    m = re.search(r'WWE2K14_BUILD_GIT\s+"([0-9a-f]{7,40})"', text)
    if not m:
        return None, None, f'{hdr} has no WWE2K14_BUILD_GIT (the build had no git: "unknown")'
    d = re.search(r'WWE2K14_BUILD_DIRTY\s+(\d)', text)
    h = m.group(1).encode() + b'\0'
    found = False
    tail = b''
    with open(exe, 'rb') as f:
        for blk in iter(lambda: f.read(8 << 20), b''):
            if h in tail + blk:
                found = True
                break
            tail = blk[-len(h):]
    if not found:
        return None, None, f'the hash {m.group(1)} from {hdr.name} is not in {exe.name}: the header is from a later or different build than this exe'
    return m.group(1), bool(d and d.group(1) == '1'), None


def judge(exe, repo):
    """-> (code, text)."""
    c, dirty, why = exe_stamp(exe)
    if why:
        return 4, f'cannot tell how old the exe is: {why}'
    rc, head, err = git(repo, 'rev-parse', 'HEAD')
    if rc:
        return 4, f'git rev-parse HEAD failed in {repo}: {err[:150]}'
    rc, full, err = git(repo, 'rev-parse', '--verify', '-q', c + '^{commit}')
    if rc or not full:
        return 4, f'the exe was built at {c}, a commit this repository does not know'
    if full == head:
        return 0, f'exe commit {c} == HEAD{" (exe was built dirty: its uncommitted edits cannot be seen)" if dirty else ""}'
    rc, names, err = git(repo, 'diff', '--name-only', c, head, '--', *CODE_PATHS)
    if rc:
        return 4, f'git diff {c} HEAD failed: {err[:150]}'
    files = [n for n in names.splitlines() if n.strip()]
    rc, n_between, _ = git(repo, 'rev-list', '--count', f'{c}..{head}')
    if files:
        return 3, (f'the exe was built at {c} ({n_between} commit(s) before HEAD {head[:12]}) and {len(files)} code file(s) changed since: '
                   f'{", ".join(files[:6])}{" ..." if len(files) > 6 else ""}. Build it (tools\\build.ps1) or override (-AllowStaleExe / WWE_ALLOW_STALE_EXE=1)')
    return 0, f'exe commit {c} is {n_between} commit(s) behind HEAD but no code path changed (docs / tools only)'


def selftest():
    d = Path(tempfile.mkdtemp(prefix='exe_vs_head_'))
    repo = d / 'repo'
    (repo / 'src').mkdir(parents=True)
    (repo / 'docs').mkdir()

    def sh(*a):
        subprocess.run(['git', '-C', str(repo)] + list(a), check=True, capture_output=True)
    sh('init', '-q', '-b', 'trunk')
    sh('config', 'user.email', 'a@b')
    sh('config', 'user.name', 'a')
    (repo / 'src' / 'a.cpp').write_text('v1')
    sh('add', '-A')
    sh('commit', '-q', '-m', 'one')
    c1 = git(repo, 'rev-parse', '--short=12', 'HEAD')[1]
    build = d / 'build'
    (build / 'stamp').mkdir(parents=True)
    exe = build / 'wwe2k14.exe'
    exe.write_bytes(b'MZ....' + c1.encode() + b'\0....')
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text(f'#define WWE2K14_BUILD_GIT "{c1}"\n#define WWE2K14_BUILD_DIRTY 0\n')
    res = []
    res.append(('exe at HEAD is fresh', judge(exe, repo)[0] == 0))
    (repo / 'docs' / 'n.md').write_text('x')
    sh('add', '-A')
    sh('commit', '-q', '-m', 'docs')
    res.append(('a docs-only commit since the exe stays fresh', judge(exe, repo)[0] == 0))
    (repo / 'src' / 'a.cpp').write_text('v2')
    sh('add', '-A')
    sh('commit', '-q', '-m', 'fix')
    code, text = judge(exe, repo)
    res.append(('STALE-EXE TWIN: a src commit after the exe is refused and names the file', code == 3 and 'src/a.cpp' in text))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text('#define WWE2K14_BUILD_GIT "000000000000"\n#define WWE2K14_BUILD_DIRTY 0\n')
    res.append(('a header hash that is not in the exe (header newer than the exe) is "cannot tell"', judge(exe, repo)[0] == 4))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').unlink()
    res.append(('no stamp header = cannot tell', judge(exe, repo)[0] == 4))
    fail = 0
    for n, ok in res:
        print(('ok   ' if ok else 'FAIL ') + n)
        fail += not ok
    return 1 if fail else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default='')
    ap.add_argument('--repo', default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.exe:
        print('FAIL: --exe is required')
        return 2
    code, text = judge(a.exe, a.repo)
    print(('ok   ' if code == 0 else 'STALE ' if code == 3 else 'UNKNOWN ') + text)
    return code


if __name__ == '__main__':
    sys.exit(main())

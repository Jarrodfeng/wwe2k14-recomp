#!/usr/bin/env python3
"""Refuse a game run on an exe built before HEAD's code (S103 H3, audit section 2; run 15 ran an exe built before the fix merged).

  py -I tools\\bench\\exe_vs_head.py --exe PATH [--repo DIR] [--selftest]      exit 0 = fresh (or only docs / tools changed since), 3 = stale, 4 = cannot tell

HOW IT READS THE EXE'S COMMIT (nothing is run): every build writes <build dir>\\stamp\\wwe2k14_build_stamp.h (tools\\build_stamp.cmake: `#define WWE2K14_BUILD_GIT
"<12-hex short HEAD>"` and WWE2K14_BUILD_DIRTY) and links src\\build_stamp.cpp, which embeds that string in the exe. The tool reads the header beside the exe and
CONFIRMS the same string occurs in the exe's bytes (a header from a later build than the exe would otherwise lie); no header, or a hash not found in the exe =
"cannot tell" (4: refused, with the cause). Then `git diff --name-only <exe commit> HEAD -- <code paths>`: any path under src/, config/, patches/, CMakeLists.txt,
tools/build_stamp.cmake, tools/sdk_pins.json, third_party/, CMakePresets.json, VERSION, the keep_stream source lists, generated/rexglue.cmake, game_logo.ico,
assets/ (CODE_PATHS: what the exe is built from; docs, tools and notes do not make an exe stale) = stale. An exe commit git does not know = cannot
tell. A dirty exe (WWE2K14_BUILD_DIRTY 1) is judged on its commit and the output says it was dirty (its uncommitted edits cannot be seen).
run_list.ps1 calls this before the run lock; override: -AllowStaleExe or WWE_ALLOW_STALE_EXE=1 (it prints the reason either way)."""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# S109 WARM review R3: + every other tracked file CMakeLists.txt reads (vendored C code, the OFF build's source lists, the presets' cache variables, the version
# string, the included rexglue.cmake, the icon and the embedded pictures): a commit changing only one of them changes the exe.
CODE_PATHS = ['src', 'config', 'patches', 'CMakeLists.txt', 'tools/build_stamp.cmake', 'tools/sdk_pins.json', 'third_party', 'CMakePresets.json', 'VERSION',
              'tools/scene/keep_stream_sources.txt', 'tools/scene/keep_stream_off_sources.txt', 'generated/rexglue.cmake', 'game_logo.ico', 'assets']


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


def exe_info_commit(exe):
    """S106 G2L: the commit lane_exe.ps1 recorded in <folder>\\exe_info.json -> (commit or None, why or None)."""
    info = Path(exe).parent / 'exe_info.json'
    if not info.is_file():
        return None, f'no {info} beside the exe'
    try:
        c = json.loads(info.read_text(encoding='utf-8-sig')).get('commit')
    except (OSError, ValueError) as e:
        return None, f'{info} unreadable: {e}'
    if not isinstance(c, str) or not re.fullmatch(r'[0-9a-f]{7,40}', c):
        return None, f'{info} has no usable "commit"'
    return c, None


def judge_want(exe, want, c, dirty, repo=None):
    """S106 G2L: a run queued for commit `want` (not HEAD): the exe is right when its stamp IS that commit, wrong (3) otherwise. HEAD is never read.
    An exe_info.json beside the exe that names another commit than the stamp is also wrong (3). Any folder name is accepted: the commit comes from the stamp."""
    want = want.strip().lower()
    if not re.fullmatch(r'[0-9a-f]{7,40}', want):
        return 4, f'--want {want!r} is not a commit hash'
    if repo is not None:   # S106 G2LF: refuse an unknown or ambiguous want (rev-parse --verify want^{commit})
        rc, full, err = git(repo, 'rev-parse', '--verify', '-q', want + '^{commit}')
        if rc or not full:
            return 4, f'--want {want} is not a commit this repository knows (or is ambiguous)'
        want = full.lower()
    n = min(len(want), len(c))
    if want[:n] != c[:n]:
        return 3, f'exe is {c}, the run was queued for {want[:12]}: wrong exe for this run'
    if dirty:   # S106 G2LF F8: uncommitted edits cannot be seen, so a dirty exe is not "commit X"
        return 3, f'exe at {c} was built dirty (uncommitted edits): it is not commit {want[:12]}, build it clean'
    m = re.search(r'-([0-9a-f]{9,12})$', Path(exe).parent.name.lower())   # S106 G2LF F7: a folder named ...-<hash> must hold that commit
    if m and not c.startswith(m.group(1)) and not m.group(1).startswith(c):
        return 3, f'folder {Path(exe).parent.name} names commit {m.group(1)} but the stamp says {c}: wrong exe for this folder'
    ic, _iwhy = exe_info_commit(exe)
    if ic is not None:
        k = min(len(ic), len(c))
        if ic[:k] != c[:k]:
            return 3, f'exe_info.json says {ic[:12]} but the stamp says {c}: the folder holds a mixed build'
    return 0, f'exe commit {c} is the commit this run was queued for' + (' (exe was built dirty: its uncommitted edits cannot be seen)' if dirty else '')


def judge(exe, repo, want=None, info=False):
    """-> (code, text). want=SHA (S106 G2L): judge against the commit the run was queued for, not HEAD. info=True: want = the commit exe_info.json names (4 when it has none)."""
    c, dirty, why = exe_stamp(exe)
    if why:
        return 4, f'cannot tell how old the exe is: {why}'
    if info and not want:
        want, iwhy = exe_info_commit(exe)
        if iwhy:
            return 4, f'cannot tell which commit this exe is for: {iwhy}'
    if want:
        return judge_want(exe, want, c, dirty, repo)
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
    # S109 WARM review R3: inputs the build reads outside src/config (the OFF build's source list, the vendored C code) stale the exe too.
    for rel in ('tools/scene/keep_stream_off_sources.txt', 'third_party/audio/x.c'):
        (repo / rel).parent.mkdir(parents=True, exist_ok=True)
        (repo / rel).write_text('x')
        sh('add', '-A')
        sh('commit', '-q', '-m', rel)
        code, text = judge(exe, repo)
        res.append(('STALE-EXE TWIN: a commit changing only %s is stale (3) and names it' % rel, code == 3 and rel in text))
    (repo / 'src' / 'a.cpp').write_text('v2')
    sh('add', '-A')
    sh('commit', '-q', '-m', 'fix')
    code, text = judge(exe, repo)
    res.append(('STALE-EXE TWIN: a src commit after the exe is refused and names the file', code == 3 and 'src/a.cpp' in text))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text('#define WWE2K14_BUILD_GIT "000000000000"\n#define WWE2K14_BUILD_DIRTY 0\n')
    res.append(('a header hash that is not in the exe (header newer than the exe) is "cannot tell"', judge(exe, repo)[0] == 4))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').unlink()
    res.append(('no stamp header = cannot tell', judge(exe, repo)[0] == 4))
    # S106 G2L: runs judged against the commit they were queued for (out\exe\<any name>), never HEAD.   # entry: judge
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text(f'#define WWE2K14_BUILD_GIT "{c1}"\n#define WWE2K14_BUILD_DIRTY 0\n')
    res.append(('S105 PAST FAULT (SESSION105_SUMMARY.md:113): exe at A, HEAD moved to B by a src commit: refused today (3)', judge(exe, repo)[0] == 3))
    res.append(('  ... the same run queued for A (want=A) is accepted (0)', judge(exe, repo, want=c1)[0] == 0))
    res.append(('  ... the full sha of A is accepted too', judge(exe, repo, want=git(repo, 'rev-parse', c1)[1])[0] == 0))
    c2 = git(repo, 'rev-parse', '--short=12', 'HEAD')[1]
    res.append(('UNSAFE TWIN: stamp A, want=B refused (3), never 0', judge(exe, repo, want=c2)[0] == 3))
    res.append(('UNSAFE TWIN: a want that is not a hash is 4, never 0', judge(exe, repo, want='zzzz')[0] == 4))
    info = build / 'exe_info.json'
    info.write_text(json.dumps({'commit': 'f' * 40}))
    res.append(('UNSAFE TWIN: exe_info.json names another commit than the stamp: 3 (mixed folder), even with the right want', judge(exe, repo, want=c1)[0] == 3))
    info.write_text(json.dumps({'commit': git(repo, 'rev-parse', c1)[1]}))
    res.append(('exe_info.json agreeing with the stamp: info mode accepts (0)', judge(exe, repo, info=True)[0] == 0))
    res.append(('  ... a folder name without a trailing hash (plain-name) is judged by its stamp', judge(shutil.copytree(build, d / 'plain-name') / 'wwe2k14.exe', repo, want=c1)[0] == 0))
    # S106 G2LF F7: a folder whose last '-' part is 9-12 hex names a commit: the stamp must start with it, else 3 (a lane-x-<B> folder holding A).
    res.append(('  ... F7: folder lane-x-<A9> holding A is accepted', judge(shutil.copytree(build, d / f'lane-x-{c1[:9]}') / 'wwe2k14.exe', repo, want=c1)[0] == 0))
    res.append(('UNSAFE TWIN F7: folder lane-x-<other 9 hex> holding A is 3 (with want=A)', judge(shutil.copytree(build, d / 'lane-x-123456789') / 'wwe2k14.exe', repo, want=c1)[0] == 3))
    res.append(('UNSAFE TWIN F7: the same folder in info mode (no -ForCommit) is 3', judge(d / 'lane-x-123456789' / 'wwe2k14.exe', repo, info=True)[0] == 3))
    # S106 G2LF: want must be a commit this repository knows (rev-parse --verify want^{commit}); an unknown / ambiguous hash is 4.
    res.append(('UNSAFE TWIN: an unknown 12-hex want is 4, never 0', judge(exe, repo, want='abcdef123456')[0] == 4))
    # S106 G2LF F8: a dirty exe cannot be said to be commit X: with a want it is 3.
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text(f'#define WWE2K14_BUILD_GIT "{c1}"\n#define WWE2K14_BUILD_DIRTY 1\n')
    res.append(('UNSAFE TWIN F8: a dirty exe with want = its own commit is 3, never 0', judge(exe, repo, want=c1)[0] == 3))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text(f'#define WWE2K14_BUILD_GIT "{c1}"\n#define WWE2K14_BUILD_DIRTY 0\n')
    info.unlink()
    res.append(('UNSAFE TWIN: info mode with no exe_info.json = 4 (never accepted)', judge(exe, repo, info=True)[0] == 4))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').unlink()
    res.append(('UNSAFE TWIN: want given but no stamp = 4, never 0', judge(exe, repo, want=c1)[0] == 4))
    (build / 'stamp' / 'wwe2k14_build_stamp.h').write_text('#define WWE2K14_BUILD_GIT "000000000000"\n#define WWE2K14_BUILD_DIRTY 0\n')
    res.append(('UNSAFE TWIN: a stamp whose hash is not in the exe, want = that hash: 4, never 0', judge(exe, repo, want='000000000000')[0] == 4))
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
    ap.add_argument('--want', default='', help='S106 G2L: the commit the run was queued for (judged instead of HEAD)')
    ap.add_argument('--want-info', action='store_true', help='S106 G2L: want = the commit in <exe folder>\\exe_info.json')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.exe:
        print('FAIL: --exe is required')
        return 2
    code, text = judge(a.exe, a.repo, want=a.want or None, info=a.want_info)
    print(('ok   ' if code == 0 else 'STALE ' if code == 3 else 'UNKNOWN ') + text)
    return code


if __name__ == '__main__':
    sys.exit(main())

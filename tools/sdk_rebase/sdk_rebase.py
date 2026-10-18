"""Rebase our ReXGlue SDK changes (branch wwe2k14-local) onto an upstream tag/commit in a SCRATCH worktree, build it
into a scratch install, run tools\\regress and a codegen compare with it, report (Session 8, task 17).
Docs: docs\\harness\\sdk_rebase.md. Nothing is fetched: --onto must already be in the local SDK repo.

    py tools\\sdk_rebase\\sdk_rebase.py                         onto the newest upstream ref present locally (see --onto)
    py tools\\sdk_rebase\\sdk_rebase.py --onto v0.11.0          a tag / commit already in thirdparty\\rexglue-sdk-src
    py tools\\sdk_rebase\\sdk_rebase.py --regress-only selftest,lesnar   fewer regress routes (default: all)
    py tools\\sdk_rebase\\sdk_rebase.py --no-build              conflicts report only (no lock, no build, no game)
    py tools\\sdk_rebase\\sdk_rebase.py --source patches        apply patches\\rexglue-sdk\\*.patch instead of the commits
    py tools\\sdk_rebase\\sdk_rebase.py --synthetic conflict    self-test: a scratch "upstream" commit that edits a line
                                                               our commits change (expect a conflict); 'clean' = edits
                                                               a file we never touch (expect none). Implies --no-build.
    py tools\\sdk_rebase\\sdk_rebase.py --clean STAMP           delete what --keep or an interrupted run left: the worktree,
                                                               the exe copy, the CMake package registry value

What it does, in order (report: out\\harness\\sdk_rebase\\<stamp>\\report.md + report.json):
  1. upstream: --onto resolved to a commit (default: refs/remotes/origin/main, origin/HEAD, the newest remote-tracking
     ref, else the newest tag); our base = merge-base(wwe2k14-local, onto); upstream commits ahead of our base.
  2. worktree thirdparty\\sdk-wt\\rebase-<stamp>, detached at onto. Units = the first-parent commits base..wwe2k14-local
     (each merge is cherry-picked with -m 1, i.e. as the change it brought in; 36 units at 728667b), or with
     --source patches the patch files in order (git apply --3way --index). A unit that conflicts is recorded (files,
     unmerged status, conflict hunks; the conflicted diff is saved) and aborted; the next units are still tried
     (their result may cascade from the skipped one). Nothing is resolved or adopted; wwe2k14-local is never moved.
  3. net-diff check: same tree as wwe2k14-local (when onto = base), else per file: is our change (git diff
     base..wwe2k14-local) the same patch (git patch-id --stable) after the rebase (git diff onto..rebased)?
  4. build (only when every unit applied): tools\\sdk_rebase\\build_scratch_sdk.ps1 under the run lock -> scratch
     install <worktree>\\out\\install\\win-amd64 (never REXSDK). The SDK's install step registers every prefix in the
     CMake user package registry (HKCU); the build script removes the scratch prefix's value right after the install.
  5. ABI: every file of the scratch install vs the SDK the game was built with (rexglue_DIR in
     out\\build\\win-amd64-release\\CMakeCache.txt), classified by install_kind(): headers, CMake package configs,
     share\\rexglue\\*.cpp (compiled INTO the exe) and anything unlisted decide; bin\\ is what regress/codegen test;
     licenses\\, share\\vulkan\\, lib\\pkgconfig\\ are not used by the game build; lib\\*.lib differ in bytes anyway.
     Equal = same bytes, or same text after replacing the install/source root paths, or (STAMP_FILES only) after
     stamp_normalize. Equal -> the game exe can be reused: out\\harness\\sdk_rebase\\<stamp>\\exe\\ gets a copy of the
     current wwe2k14.exe/.map and the rebased rex*.dll/.pdb (unpinned on purpose; the main exe folder is never
     written). Different -> the exe must be rebuilt against the rebased SDK: reported, regress skipped (no scratch game
     build in this version).
  6. regress: py tools\\regress\\regress.py --exe <scratch exe> [--only ...] (its own run-lock hold), only on an exe
     staged by THIS run (a leftover copy from an earlier run with the same stamp is deleted at the start).
  7. codegen: py tools\\codegen_repro\\run_codegen.py --rexglue <scratch rexglue.exe> --stamp sdk_rebase_<stamp>
     (compares with generated\\default: function set, sizes, code). Its 363 MB output folder is deleted afterwards
     (the compare\\ folder and run.json stay) unless --keep-codegen.
  8. cleanup (unless --keep): worktree + its build/install via build_scratch_sdk.ps1 -Clean (junction-safe; also
     removes any registry value pointing inside it), the exe copy and DLLs (logs\\ and captures\\ of the regress runs
     stay in the report folder).
  9. safety checks (before vs after): wwe2k14-local head, the installed SDK's bin\\rex*.dll/exe, the main exe folder's
     rex*.dll/exe, and the rexglue values in the CMake user package registry.
Exit: 0 = every step that ran passed, 1 = conflicts / build / ABI / regress / codegen difference / safety, 2 = error
(bad arguments, or an exception during the run: the report is still written).
Game-derived output (regress logs, captures, codegen compare) stays under out\\harness\\ (gitignored).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
SDK = os.path.join(REPO, 'thirdparty', 'rexglue-sdk-src')
WT_ROOT = os.path.join(REPO, 'thirdparty', 'sdk-wt')
OUT_ROOT = os.path.join(REPO, 'out', 'harness', 'sdk_rebase')
PATCHES = os.path.join(REPO, 'patches', 'rexglue-sdk')
MAIN_EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_PS1 = os.path.join(HERE, 'build_scratch_sdk.ps1')
RUNLOCK_PS1 = os.path.join(REPO, 'tools', 'runlock.ps1')
REGRESS = os.path.join(REPO, 'tools', 'regress', 'regress.py')
CODEGEN = os.path.join(REPO, 'tools', 'codegen_repro', 'run_codegen.py')
UNMERGED = {'DD', 'AU', 'UD', 'UA', 'DU', 'AA', 'UU'}
# Stamps CMake bakes in at configure time (seen in the 2026-10-09 dry runs). They are normalised ONLY in these files
# (paths relative to the install root); every other installed file is compared exactly (after the install/source root
# paths are replaced). The major.minor.patch part of a version is kept: an upstream version bump counts as a change.
STAMP_FILES = {'include/rex/version.h', 'include/SDL3/SDL_revision.h',
               'lib/cmake/rexglue/rexglueConfig.cmake', 'lib/cmake/rexglue/rexglueConfigVersion.cmake'}
STAMPS = [
    (re.compile(r'\b(\d+\.\d+\.\d+)\.\d+(?:-dev\.g[0-9a-f]{7,40})?'), r'\1.TWEAK'),   # 0.10.0.82-dev.g728667b
    (re.compile(r'(REXGLUE_VERSION_TWEAK)\s+\d+\b'), r'\1 N'),                       # commits since the tag
    (re.compile(r'(REXGLUE_BUILD_TIMESTAMP\s+)"\d{8}_\d{4,6}"'), r'\1"TIMESTAMP"'),
    (re.compile(r'(SDL-\d+\.\d+\.\d+-)(?:[0-9a-f]{7,40}|HEAD-HASH-NOTFOUND)\b'), r'\1HASH'),   # SDL-3.5.0-8bf3b72
]
# How each installed file (path relative to the install root, '/' separated) takes part in the ABI decision.
#   runtime: bin\ (rex*.dll are what regress tests, rexglue.exe is what the codegen compare tests): not compared here
#   unused:  not read by the game's CMake build (checked 2026-10-09: no reference in out\build\win-amd64-release\
#            build.ninja): listed when different, never decides
#   lib:     lib\*.lib: other bytes are expected (debug info holds the build path); added/removed decides. The exe links
#            SDL3-static/fmt/spdlog (submodules: the scratch build stops if upstream re-pins one) + rexruntime.lib
#            (import library of the DLL that regress tests).
#   everything else decides: include\ (headers), share\rexglue\ (rex_app.cpp + windowed_app_main_sdl.cpp, compiled INTO
#   wwe2k14.exe by rexglue_helpers.cmake), every CMake package config (lib\cmake\, cmake\, share\cmake\,
#   share\utf8cpp\cmake\), and any file not listed here ("other").
UNUSED_PREFIXES = ('licenses/', 'share/vulkan/', 'lib/pkgconfig/')


def stamp_normalize(line):
    for rx, rep in STAMPS:
        line = rx.sub(rep, line)
    return line


def install_kind(rel):
    lo = rel.lower()
    if lo.startswith('bin/'):
        return 'runtime'
    if lo.startswith(UNUSED_PREFIXES):
        return 'unused'
    if lo.startswith('include/'):
        return 'header'
    if lo.startswith('share/rexglue/'):
        return 'app_src'
    if lo.startswith('lib/') and lo.endswith('.lib'):
        return 'lib'
    if lo.endswith('.cmake') or '/cmake/' in '/' + lo:
        return 'config'
    return 'other'


# ------------------------------------------------------------------------------------------------------- helpers
def git(*args, cwd=SDK, check=True, inp=None):
    p = subprocess.run(['git', '-C', cwd] + list(args), capture_output=True, text=True, encoding='utf-8',
                       errors='replace', input=inp)
    if check and p.returncode != 0:
        raise RuntimeError('git %s failed (%d): %s' % (' '.join(args), p.returncode, (p.stderr or p.stdout).strip()))
    return p


def out(*args, cwd=SDK):
    return git(*args, cwd=cwd).stdout.strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def hashes(folder, pattern=re.compile(r'(?i)^rex.*\.(dll|exe)$')):
    if not os.path.isdir(folder):
        return {}
    return {n: sha256(os.path.join(folder, n)) for n in sorted(os.listdir(folder))
            if pattern.match(n) and os.path.isfile(os.path.join(folder, n))}


class Report:
    def __init__(self, stamp, folder):
        self.stamp, self.folder = stamp, folder
        self.data = dict(stamp=stamp, started=datetime.datetime.now().isoformat(timespec='seconds'), steps={})
        self.lines = []
        os.makedirs(folder, exist_ok=True)
        self.logf = open(os.path.join(folder, 'sdk_rebase.log'), 'a', encoding='utf-8')

    def say(self, s=''):
        print(s, flush=True)
        self.logf.write(s + '\n')
        self.logf.flush()

    def md(self, s=''):
        self.lines.append(s)

    def step(self, name, **kw):
        self.data['steps'].setdefault(name, {}).update(kw)

    def write(self):
        self.data['finished'] = datetime.datetime.now().isoformat(timespec='seconds')
        with open(os.path.join(self.folder, 'report.json'), 'w', encoding='utf-8') as f:
            json.dump(self.data, f, indent=1)
        with open(os.path.join(self.folder, 'report.md'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(self.lines) + '\n')


def commit_info(rev):
    sha, date, subj = out('log', '-1', '--format=%H%x00%cI%x00%s', rev).split('\x00')
    return dict(sha=sha, date=date, subject=subj)


# ------------------------------------------------------------------------------------------------------ upstream
def pick_onto(explicit):
    """(ref name, why) of the upstream ref to rebase onto. Nothing is fetched: only refs already in the repo."""
    if explicit:
        return explicit, 'given with --onto'
    refs = out('for-each-ref', '--format=%(refname)%00%(committerdate:unix)%00%(objecttype)%00%(*objecttype)',
               'refs/remotes', 'refs/tags').splitlines()
    remotes, tags = [], []
    for line in refs:
        name, ts, typ, deref = (line.split('\x00') + ['', '', '', ''])[:4]
        if name.endswith('/HEAD'):
            continue
        (remotes if name.startswith('refs/remotes/') else tags).append((int(ts or 0), name))
    have = {n for _, n in remotes}
    for pref in ('refs/remotes/origin/main', 'refs/remotes/origin/master'):
        if pref in have:
            return pref, 'the remote-tracking ref %s' % pref
    head = git('symbolic-ref', '-q', 'refs/remotes/origin/HEAD', check=False).stdout.strip()
    if head:
        return head, 'origin/HEAD -> %s' % head
    if remotes:
        return max(remotes)[1], 'the newest remote-tracking ref'
    if tags:
        # Tag dates: annotated tags carry their own date; for a lightweight tag the commit's date is used.
        best = max(tags, key=lambda t: int(out('log', '-1', '--format=%ct', t[1] + '^{commit}') or 0))
        return best[1], 'no remote-tracking refs in the repo (fetch spec: %s): the newest tag' % (
            git('config', '--get-all', 'remote.origin.fetch', check=False).stdout.strip() or 'none')
    raise RuntimeError('no upstream ref in the local SDK repo (no remote-tracking refs, no tags): pass --onto')


def synthetic_upstream(wt, base, branch, kind, rep):
    """Make a scratch 'upstream' commit on top of base inside the worktree (detached; no ref is created anywhere).
    conflict: change a line that our net diff also changes; clean: append to a file our commits never touch."""
    ours = set(out('diff', '--name-only', base, branch).splitlines())
    if kind == 'clean':
        cands = [f for f in out('ls-tree', '-r', '--name-only', base).splitlines()
                 if f not in ours and re.search(r'\.(md|txt)$', f, re.I)]
        if not cands:
            raise RuntimeError('synthetic clean: no untouched .md/.txt file')
        target = sorted(cands, key=len)[0]
        with open(os.path.join(wt, target), 'a', encoding='utf-8', newline='\n') as f:
            f.write('\nsdk_rebase synthetic upstream change (self-test, scratch worktree only)\n')
        what = 'appended a line to %s (not touched by wwe2k14-local)' % target
    else:
        # The first file wwe2k14-local MODIFIES (exists in base) and the first line its diff removes/replaces.
        target, line_no = None, None
        for f in out('diff', '--diff-filter=M', '--name-only', base, branch).splitlines():
            if not f.endswith(('.cpp', '.h')):
                continue
            d = out('diff', '-U0', base, branch, '--', f)
            m = re.search(r'^@@ -(\d+)(?:,(\d+))? \+', d, re.M)
            if m and (m.group(2) is None or int(m.group(2)) > 0):
                target, line_no = f, int(m.group(1))
                break
        if not target:
            raise RuntimeError('synthetic conflict: no modified file with a changed line')
        p = os.path.join(wt, target)
        with open(p, 'rb') as f:
            lines = f.read().split(b'\n')
        lines[line_no - 1] = lines[line_no - 1].rstrip(b'\r') + b' /* sdk_rebase synthetic upstream change */' + (
            b'\r' if lines[line_no - 1].endswith(b'\r') else b'')
        with open(p, 'wb') as f:
            f.write(b'\n'.join(lines))
        what = 'edited line %d of %s (a line wwe2k14-local changes)' % (line_no, target)
    git('add', '-A', cwd=wt)
    git('-c', 'user.name=sdk_rebase', '-c', 'user.email=sdk_rebase@localhost', 'commit', '-q', '-m',
        'sdk_rebase synthetic upstream (%s)' % kind, cwd=wt)
    sha = out('rev-parse', 'HEAD', cwd=wt)
    rep.say('synthetic upstream commit %s: %s' % (sha[:9], what))
    return sha, what


# --------------------------------------------------------------------------------------------------------- units
def list_units(source, base, branch):
    if source == 'patches':
        names = sorted(n for n in os.listdir(PATCHES) if n.endswith('.patch'))
        return [dict(kind='patch', id=n, subject=n, path=os.path.join(PATCHES, n)) for n in names]
    units = []
    for sha in out('rev-list', '--first-parent', '--reverse', base + '..' + branch).splitlines():
        parents = out('log', '-1', '--format=%P', sha).split()
        units.append(dict(kind='merge' if len(parents) > 1 else 'commit', id=sha,
                          subject=out('log', '-1', '--format=%s', sha)))
    return units


def conflict_details(wt):
    st = git('status', '--porcelain=v1', '-z', cwd=wt).stdout.split('\x00')
    files = []
    for e in st:
        if len(e) > 3 and e[:2] in UNMERGED:
            f = e[3:]
            hunks = 0
            p = os.path.join(wt, f)
            if os.path.isfile(p):
                with open(p, 'rb') as fh:
                    hunks = sum(1 for ln in fh if ln.startswith(b'<<<<<<< '))
            files.append(dict(file=f, status=e[:2], hunks=hunks))
    return files


def apply_units(wt, units, rep, xopt):
    results, first_conflict = [], None
    for i, u in enumerate(units, 1):
        tag = '%02d %s' % (i, u['id'][:9] if u['kind'] != 'patch' else u['id'])
        if u['kind'] == 'patch':
            p = git('apply', '--3way', '--index', '--whitespace=nowarn', u['path'], cwd=wt, check=False)
        else:
            cmd = ['-c', 'user.name=sdk_rebase', '-c', 'user.email=sdk_rebase@localhost', 'cherry-pick',
                   '--allow-empty', '--keep-redundant-commits']
            if xopt:
                cmd += ['-X', xopt]
            if u['kind'] == 'merge':
                cmd += ['-m', '1']
            # Keep the original author; the committer is 'sdk_rebase' (scratch commits on a detached HEAD only).
            p = git(*(cmd + [u['id']]), cwd=wt, check=False)
        msg = ((p.stdout or '') + (p.stderr or '')).strip()
        r = dict(n=i, kind=u['kind'], id=u['id'], subject=u['subject'], after_conflict=first_conflict is not None)
        if p.returncode == 0:
            if u['kind'] == 'patch':
                c = git('-c', 'user.name=sdk_rebase', '-c', 'user.email=sdk_rebase@localhost', 'commit', '-q',
                        '--allow-empty', '-m', 'sdk_rebase: ' + u['id'], cwd=wt, check=False)
                if c.returncode:
                    r.update(result='error', message=(c.stderr or c.stdout).strip()[:2000])
                    results.append(r)
                    rep.say('  %s ERROR committing: %s' % (tag, r['message'][:200]))
                    continue
            r.update(result='clean', new=out('rev-parse', 'HEAD', cwd=wt),
                     files=len(out('diff', '--name-only', 'HEAD~1', 'HEAD', cwd=wt).splitlines()))
            rep.say('  %s clean (%d files)  %s' % (tag, r['files'], u['subject'][:90]))
        else:
            # git's own output is kept for every unit that did not apply (report.json 'message' + conflicts\NN_*.log).
            cdir = os.path.join(rep.folder, 'conflicts')
            os.makedirs(cdir, exist_ok=True)
            base_name = '%02d_%s' % (i, re.sub(r'[^A-Za-z0-9_.-]', '_', u['id'][:40]))
            with open(os.path.join(cdir, base_name + '.log'), 'w', encoding='utf-8', errors='replace') as f:
                f.write(msg + '\n')
            r.update(message=msg[:4000], log=os.path.relpath(os.path.join(cdir, base_name + '.log'), rep.folder),
                     git_errors=[x.strip() for x in msg.splitlines() if re.match(r'\s*(error|fatal):', x)][:20])
            files = conflict_details(wt)
            # "git apply --3way" is atomic: when one file cannot be applied at all (e.g. "error: F: does not exist in
            # index", a file an earlier, skipped patch would have created) NOTHING is written, although it has already
            # printed "Applied patch to 'X' with conflicts." for other files. No unmerged entries + an unchanged work
            # tree = the apply aborted: an error (with git's error lines), not a conflict in the files it named.
            touched = git('status', '--porcelain=v1', '--untracked-files=no', cwd=wt, check=False).stdout.strip()
            if files:
                r.update(result='conflict', conflicts=files)
                dpath = os.path.join(cdir, base_name + '.diff')
                with open(dpath, 'w', encoding='utf-8', errors='replace') as f:
                    f.write(git('diff', cwd=wt, check=False).stdout)
                r['diff'] = os.path.relpath(dpath, rep.folder)
                if first_conflict is None:
                    first_conflict = i
                rep.say('  %s CONFLICT in %d file(s): %s  %s' % (tag, len(files), ', '.join(
                    '%s [%s, %d hunk(s)]' % (x['file'], x['status'], x['hunks']) for x in files), u['subject'][:60]))
            else:
                aborted = u['kind'] == 'patch' and not touched
                r.update(result='error', aborted=aborted)
                if first_conflict is None:
                    first_conflict = i
                rep.say('  %s ERROR%s: %s' % (tag, ' (git apply aborted, nothing applied)' if aborted else '',
                                              ' / '.join(r['git_errors'][:3]) or msg[:300]))
            # Drop the attempt; the next unit starts from the last clean state.
            if u['kind'] == 'patch':
                git('reset', '-q', '--hard', 'HEAD', cwd=wt, check=False)
            else:
                git('cherry-pick', '--abort', cwd=wt, check=False)
                git('reset', '-q', '--hard', 'HEAD', cwd=wt, check=False)
        results.append(r)
    return results


def net_diff_check(base, branch, onto, head, rep):
    same_tree = out('rev-parse', branch + '^{tree}') == out('rev-parse', head + '^{tree}')
    res = dict(same_tree_as_branch=same_tree)
    ours = out('diff', '--name-only', base, branch).splitlines()
    now = set(out('diff', '--name-only', onto, head).splitlines())
    same, changed, missing = [], [], []
    for f in ours:
        a = git('patch-id', '--stable', inp=git('diff', base, branch, '--', f).stdout).stdout.split()
        if f not in now:
            missing.append(f)
            continue
        b = git('patch-id', '--stable', inp=git('diff', onto, head, '--', f).stdout).stdout.split()
        (same if a[:1] == b[:1] else changed).append(f)
    extra = sorted(now - set(ours))
    res.update(files=len(ours), same_patch=len(same), changed_patch=changed, missing=missing, extra=extra)
    rep.say('net diff: %d files in wwe2k14-local\'s change; %d identical as a patch after the rebase, %d changed%s%s; '
            'same tree as wwe2k14-local: %s' % (len(ours), len(same), len(changed),
                                                ', %d missing' % len(missing) if missing else '',
                                                ', %d extra' % len(extra) if extra else '', same_tree))
    return res


# ------------------------------------------------------------------------------------------------------ build/ABI
def game_sdk_root():
    cache = os.path.join(MAIN_EXE_DIR, 'CMakeCache.txt')
    with open(cache, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('rexglue_DIR:PATH='):
                return os.path.normpath(os.path.join(line.split('=', 1)[1].strip(), '..', '..', '..'))
    raise RuntimeError('rexglue_DIR not in %s' % cache)


def tree_files(root):
    res = {}
    for d, _, fs in os.walk(root):
        for n in fs:
            p = os.path.join(d, n)
            res[os.path.relpath(p, root).replace('\\', '/')] = p
    return res


def _root_spellings(roots):
    """Regexes for the absolute install/source roots (backslash and slash spellings, any case), longest first."""
    forms = set()
    for r in roots:
        if not r:
            continue
        r = os.path.normpath(r)
        forms.update({r, r.replace('\\', '/'), r.replace('\\', '\\\\')})
    return [re.compile(re.escape(f), re.I) for f in sorted(forms, key=len, reverse=True)]


def _text(path, root_rx):
    with open(path, 'rb') as f:
        t = f.read().decode('utf-8', 'replace').replace('\r\n', '\n')
    for rx in root_rx:
        t = rx.sub('<ROOT>', t)
    return t.split('\n')


def abi_compare(new_root, old_root, rep, extra_roots=()):
    """Can the current exe (built against old_root) run on the rebased install new_root? Every installed file is
    classified by install_kind(); see the table above it. A file counts as unchanged when its bytes are equal, or its
    text is equal after CRLF -> LF and replacing the install/source root paths ("path only"), or, for STAMP_FILES only,
    after stamp_normalize ("version stamp only"). abi_same = nothing that decides was added, removed or changed."""
    keys = ('header', 'config', 'app_src', 'other')
    res = dict(new=new_root, old=old_root, libs_changed=[], libs_added=[], libs_removed=[], unused_changed=[],
               path_only=[], stale_in_old=[])
    for k in keys:
        res.update({k + '_added': [], k + '_removed': [], k + '_changed': [], k + '_version_only': []})
    # Files an older install left in the installed SDK (not in the main SDK build's install_manifest.txt) are stale,
    # not part of the interface the game was built against: listed separately, never a removal.
    manifest = os.path.join(SDK, 'out', 'build', 'win-amd64', 'install_manifest.txt')
    installed = None
    if os.path.exists(manifest):
        with open(manifest, encoding='utf-8', errors='replace') as f:
            installed = {os.path.normcase(os.path.normpath(x.strip())) for x in f if x.strip()}
        root = os.path.normcase(os.path.normpath(old_root)) + os.sep
        if not any(x.startswith(root) for x in installed):
            installed = None   # that manifest is another install's: no stale detection
    res['old_install_manifest'] = manifest if installed is not None else None
    root_rx = _root_spellings([new_root, old_root] + list(extra_roots))
    a, b = tree_files(old_root), tree_files(new_root)
    for f in sorted(set(a) | set(b)):
        kind = install_kind(f)
        if kind == 'runtime':
            continue
        if f not in a or f not in b:
            if f not in b and installed is not None and os.path.normcase(os.path.normpath(a[f])) not in installed:
                res['stale_in_old'].append(f)
            elif kind == 'unused':
                res['unused_changed'].append(f + (' (added)' if f in b else ' (removed)'))
            elif kind == 'lib':
                res['libs_added' if f in b else 'libs_removed'].append(f)
            else:
                res[kind + ('_added' if f in b else '_removed')].append(f)
            continue
        if sha256(a[f]) == sha256(b[f]):
            continue
        if kind == 'lib':
            res['libs_changed'].append(f)
            continue
        if kind == 'unused':
            res['unused_changed'].append(f)
            continue
        la, lb = _text(a[f], root_rx), _text(b[f], root_rx)
        if la == lb:
            res['path_only'].append(f)
        elif f in STAMP_FILES and [stamp_normalize(x) for x in la] == [stamp_normalize(x) for x in lb]:
            res[kind + '_version_only'].append(f)
        else:
            res[kind + '_changed'].append(f)
    # Older field names (header_stale_in_old, cmake_changed) kept for readers of report.json.
    res['header_stale_in_old'] = [f[len('include/'):] for f in res['stale_in_old'] if f.startswith('include/')]
    res['cmake_changed'] = res['config_added'] + res['config_removed'] + res['config_changed']
    reasons = []
    for k, what in (('header', 'headers'), ('config', 'CMake package config'),
                    ('app_src', 'share/rexglue sources compiled into the exe'), ('other', 'other installed files')):
        for op in ('added', 'removed', 'changed'):
            if res['%s_%s' % (k, op)]:
                reasons.append('%s %s: %s' % (what, op, ', '.join(res['%s_%s' % (k, op)][:8])))
    for op in ('added', 'removed'):
        if res['libs_' + op]:
            reasons.append('libraries %s: %s' % (op, ', '.join(res['libs_' + op][:8])))
    res['rebuild_reasons'] = reasons
    res['abi_same'] = not reasons
    rep.say('ABI: headers +%d -%d changed %d (version stamp only %d); CMake config +%d -%d changed %d (stamp only %d); '
            'share/rexglue (compiled into the exe) +%d -%d changed %d; other +%d -%d changed %d; path-only %d; stale '
            'leftovers in the old install %d; not used by the game build, differing %d; libs with other bytes %d '
            '(expected: debug info holds the build path), added %d removed %d -> %s' % (
                len(res['header_added']), len(res['header_removed']), len(res['header_changed']),
                len(res['header_version_only']), len(res['config_added']), len(res['config_removed']),
                len(res['config_changed']), len(res['config_version_only']), len(res['app_src_added']),
                len(res['app_src_removed']), len(res['app_src_changed']), len(res['other_added']),
                len(res['other_removed']), len(res['other_changed']), len(res['path_only']), len(res['stale_in_old']),
                len(res['unused_changed']), len(res['libs_changed']), len(res['libs_added']), len(res['libs_removed']),
                'same interface: the current exe is reused' if res['abi_same'] else
                'INTERFACE CHANGED: the exe must be rebuilt against the rebased SDK (%s)' % '; '.join(reasons)))
    return res


def stage_exe(install, wt, exe_dir, rep):
    if not os.path.isfile(os.path.join(MAIN_EXE_DIR, 'wwe2k14.exe')):
        raise RuntimeError('no wwe2k14.exe in %s: build the game first (nothing to run the rebased DLLs with)' % MAIN_EXE_DIR)
    os.makedirs(exe_dir, exist_ok=True)
    staged = {}
    for n in ('wwe2k14.exe', 'wwe2k14.map'):
        src = os.path.join(MAIN_EXE_DIR, n)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(exe_dir, n))
            staged[n] = dict(src=src, sha256=sha256(src))
    names = [n for n in os.listdir(MAIN_EXE_DIR) if re.match(r'(?i)^rex.*\.dll$', n)]
    for n in names:
        dll = os.path.join(install, 'bin', n)
        if not os.path.exists(dll):
            raise RuntimeError('%s not in the scratch install %s' % (n, os.path.join(install, 'bin')))
        shutil.copy2(dll, os.path.join(exe_dir, n))
        staged[n] = dict(src=dll, sha256=sha256(dll))
        # PDB next to the built DLL (the install has none): <wt>\out\win-amd64\Release or anywhere under <wt>\out.
        pdb = os.path.join(wt, 'out', 'win-amd64', 'Release', n[:-4] + '.pdb')   # where the main SDK tree has them
        if not os.path.exists(pdb):
            pdb = None
            for d, _, fs in os.walk(os.path.join(wt, 'out')):
                if n[:-4] + '.pdb' in fs and 'install' not in d.lower():
                    pdb = os.path.join(d, n[:-4] + '.pdb')
                    break
        if pdb:
            shutil.copy2(pdb, os.path.join(exe_dir, os.path.basename(pdb)))
            staged[os.path.basename(pdb)] = dict(src=pdb, sha256=sha256(pdb))
    for n, v in staged.items():
        rep.say('  staged %-18s sha256 %s  from %s' % (n, v['sha256'][:12], v['src']))
    return staged


def run_logged(cmd, path, rep, cwd=REPO):
    rep.say('$ ' + subprocess.list2cmdline(cmd))
    t0 = time.time()
    lines = []
    with open(path, 'w', encoding='utf-8', errors='replace') as f:
        p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             encoding='utf-8', errors='replace')
        for line in p.stdout:
            f.write(line)
            f.flush()
            lines.append(line.rstrip('\n'))
        rc = p.wait()
    return rc, lines, time.time() - t0


# ---------------------------------------------------------------------------------------------------------- main
PKG_REG = r'Software\Kitware\CMake\Packages\rexglue'


def pkg_registry():
    """The CMake user package registry values for rexglue (HKCU; the SDK's install step adds one per install prefix:
    thirdparty\\rexglue-sdk-src\\cmake\\rexglue_install.cmake). {value name: path}; {} when the key is absent."""
    try:
        import winreg
        vals = {}
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, PKG_REG) as k:
            i = 0
            while True:
                try:
                    n, v, _ = winreg.EnumValue(k, i)
                except OSError:
                    break
                vals[n] = v
                i += 1
        return vals
    except OSError:
        return {}


def clean_worktree(wt, rep):
    """build_scratch_sdk.ps1 -Clean: junction-safe delete of the worktree (if present), git worktree prune, and removal
    of any CMake package registry value that points inside it. Runs even when the folder is gone (registry leftovers)."""
    rc, lines, _ = run_logged(['pwsh', '-NoProfile', '-File', BUILD_PS1, '-Clean', '-Worktree', wt,
                               '-Log', os.path.join(rep.folder, 'clean.log')], os.path.join(rep.folder, 'clean.txt'), rep)
    ok = rc == 0 and any(x.startswith('SDKREBASE_CLEAN ok') for x in lines)
    regs = [x for x in lines if x.startswith('removed CMake package registry value')]
    rep.say('worktree cleanup: %s%s' % ('ok (deleted or absent, pruned, no registry value left)' if ok else
                                        'FAILED: see clean.txt (use build_scratch_sdk.ps1 -Clean; never git worktree remove)',
                                        '; ' + '; '.join(regs) if regs else ''))
    return ok


def remove_exe_copy(exe_dir, rep, why):
    removed = []
    if os.path.isdir(exe_dir):
        for n in os.listdir(exe_dir):
            p = os.path.join(exe_dir, n)
            if os.path.isfile(p) and re.search(r'(?i)\.(exe|dll|pdb|map)$', n):
                os.remove(p)
                removed.append(n)
    if removed:
        rep.say('removed the exe copy + DLLs from %s (%s; logs\\ and captures\\ kept): %s' % (exe_dir, why, ', '.join(removed)))
    return removed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--onto', help='upstream tag/commit/ref already in the local SDK repo (default: auto, see docs)')
    ap.add_argument('--branch', default='wwe2k14-local')
    ap.add_argument('--base', help='our upstream base (default: merge-base of --branch and --onto)')
    ap.add_argument('--source', choices=('commits', 'patches'), default='commits')
    ap.add_argument('-X', dest='xopt', default='diff-algorithm=patience', help='cherry-pick -X option ("" = none)')
    # SDK_REBASE_STAMP: set before the run-lock re-exec, so the re-run keeps the same default stamp.
    ap.add_argument('--stamp', default=os.environ.get('SDK_REBASE_STAMP') or time.strftime('%Y%m%d_%H%M%S'))
    ap.add_argument('--synthetic', choices=('conflict', 'clean'), help='self-test against a scratch upstream commit')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--no-regress', action='store_true')
    ap.add_argument('--regress-only', default='', help='routes for regress.py --only (default: all routes)')
    ap.add_argument('--no-codegen', action='store_true')
    ap.add_argument('--keep', action='store_true', help='keep the worktree (+ build/install) and the exe copy')
    ap.add_argument('--keep-codegen', action='store_true', help='keep the 363 MB codegen output folder')
    ap.add_argument('--no-lock-all', action='store_true',
                    help='take the run lock per step (build, regress, codegen) instead of one hold for all of them')
    ap.add_argument('--clean', metavar='STAMP', help='only delete thirdparty\\sdk-wt\\rebase-STAMP (junction-safe), the '
                                                     'exe copy in its report folder and its CMake package registry value')
    a = ap.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', a.clean or a.stamp) or (a.clean or a.stamp) in ('.', '..'):
        print('sdk_rebase: bad stamp (letters, digits, _ . - only)')
        return 2
    stamp = a.clean or a.stamp
    folder = os.path.join(OUT_ROOT, stamp)
    wt = os.path.join(WT_ROOT, 'rebase-' + stamp)
    exe_dir = os.path.join(folder, 'exe')
    if a.synthetic:
        a.no_build = True
    if a.clean:
        rep = Report(stamp, folder)
        ok = clean_worktree(wt, rep)
        remove_exe_copy(exe_dir, rep, '--clean')
        return 0 if ok else 2
    # Cheap checks first, so a typo does not wait in the run-lock queue (they run again after the re-exec).
    if os.path.exists(wt):
        print('sdk_rebase: %s exists (stamp in use; --clean %s deletes it)' % (wt, stamp))
        return 2
    onto_ref, why = pick_onto(a.onto)
    if git('rev-parse', '--verify', '-q', onto_ref + '^{commit}', check=False).returncode:
        print('sdk_rebase: --onto %s is not a commit in %s (nothing is fetched)' % (onto_ref, SDK))
        return 2
    if a.base and git('rev-parse', '--verify', '-q', a.base + '^{commit}', check=False).returncode:
        print('sdk_rebase: --base %s is not a commit in %s' % (a.base, SDK))
        return 2
    if not a.base and not git('merge-base', a.branch, onto_ref, check=False).stdout.strip():
        print('sdk_rebase: no merge base between %s and %s (shallow clone?): pass --base' % (a.branch, onto_ref))
        return 2
    if not a.no_build and not a.no_lock_all:
        # One hold of the run lock for build + regress + codegen (they re-enter it through the inherited token), so
        # the scratch build is not separated from its test runs by other agents' queue turns. Re-execs this script.
        os.environ['SDK_REBASE_STAMP'] = stamp
        sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
        from runlock import ensure_lock
        ensure_lock('sdk_rebase ' + stamp)
    rep = Report(stamp, folder)
    # Outputs an earlier run with this stamp left (exe copy after --keep or an interruption, old conflict files) would
    # be mistaken for this run's: removed before anything else. Regress only ever runs on an exe staged by THIS run.
    remove_exe_copy(exe_dir, rep, 'left by an earlier run with this stamp')
    if os.path.isdir(os.path.join(folder, 'conflicts')):
        shutil.rmtree(os.path.join(folder, 'conflicts'))
        rep.say('removed conflicts\\ left by an earlier run with this stamp')

    fails = []
    errored = False
    branch_head = out('rev-parse', a.branch)
    try:
        sdk_root = game_sdk_root()
    except (OSError, RuntimeError) as e:
        sdk_root = None
        rep.say('WARNING: cannot read the game build\'s SDK: %s' % e)
    guard = dict(branch_head=branch_head, sdk_bin=hashes(os.path.join(sdk_root, 'bin')) if sdk_root else {},
                 exe_dir=hashes(MAIN_EXE_DIR), pkg_registry=pkg_registry())
    rep.data.update(branch=a.branch, branch_head=branch_head, source=a.source, xopt=a.xopt, synthetic=a.synthetic,
                    game_sdk=sdk_root, worktree=wt, our_repo_head=out('rev-parse', 'HEAD', cwd=REPO))
    rep.say('sdk_rebase %s: %s (%s) onto upstream; report folder %s' % (stamp, a.branch, branch_head[:9], folder))

    # 1. upstream
    onto = out('rev-parse', '--verify', onto_ref + '^{commit}')
    base = a.base and out('rev-parse', '--verify', a.base + '^{commit}')
    if not base:
        mb = git('merge-base', a.branch, onto, check=False).stdout.strip()
        if not mb:
            rep.say('no merge base between %s and %s (shallow clone?): pass --base' % (a.branch, onto_ref))
            return 2
        base = mb
    shallow = out('rev-parse', '--is-shallow-repository') == 'true'
    ahead = int(out('rev-list', '--count', base + '..' + onto))
    ours_n = int(out('rev-list', '--count', base + '..' + a.branch))
    up = dict(ref=onto_ref, why=why, onto=commit_info(onto), base=commit_info(base), ahead_of_base=ahead,
              our_commits=ours_n, our_first_parent=int(out('rev-list', '--count', '--first-parent', base + '..' + a.branch)),
              shallow_repo=shallow, base_tags=out('tag', '--points-at', base).split(),
              onto_tags=out('tag', '--points-at', onto).split())
    rep.step('upstream', **up)
    rep.say('upstream: %s = %s (%s, %s) chosen because: %s' % (onto_ref, onto[:9], up['onto']['date'],
                                                                   up['onto']['subject'][:70], why))
    rep.say('our base: %s %s (%s); upstream is %d commit(s) ahead of it; %s has %d commits (%d first-parent units)%s'
            % (base[:9], ' '.join(up['base_tags']), up['base']['date'], ahead, a.branch, ours_n,
               up['our_first_parent'], '; NOTE: shallow repo, counts only cover the local history' if shallow else ''))

    # 2. worktree + units
    os.makedirs(WT_ROOT, exist_ok=True)
    git('worktree', 'add', '--detach', wt, onto)
    rep.say('worktree %s at %s' % (wt, onto[:9]))
    try:
        target = onto
        if a.synthetic:
            target, what = synthetic_upstream(wt, onto, a.branch, a.synthetic, rep)
            rep.step('synthetic', commit=target, what=what, on_top_of=onto)
        units = list_units(a.source, base, a.branch)
        rep.say('applying %d unit(s) (%s%s):' % (len(units), a.source, ', -X ' + a.xopt if a.xopt and a.source == 'commits' else ''))
        results = apply_units(wt, units, rep, a.xopt if a.source == 'commits' else '')
        n_conf = sum(1 for r in results if r['result'] == 'conflict')
        n_err = sum(1 for r in results if r['result'] == 'error')
        head = out('rev-parse', 'HEAD', cwd=wt)
        rep.step('apply', units=results, clean=sum(1 for r in results if r['result'] == 'clean'), conflicts=n_conf,
                 errors=n_err, rebased_head=head)
        rep.say('apply: %d clean, %d conflict, %d error; rebased head %s' % (
            len(results) - n_conf - n_err, n_conf, n_err, head[:9]))
        if n_conf or n_err:
            fails.append('apply (%d conflict, %d error)' % (n_conf, n_err))
        else:
            rep.step('net_diff', **net_diff_check(base, a.branch, target, head, rep))

        # 4-7. build, ABI, stage, regress, codegen
        built = None
        if a.no_build:
            rep.step('build', skipped='--no-build' + (' (implied by --synthetic)' if a.synthetic else ''))
        elif n_conf or n_err:
            rep.step('build', skipped='units did not all apply')
            rep.say('build: skipped (units did not all apply)')
        else:
            blog = os.path.join(folder, 'sdk_build.log')
            cmd = ['pwsh', '-NoProfile', '-File', RUNLOCK_PS1, 'run', '-Name', 'sdk_rebase build ' + stamp, '--',
                   'pwsh', '-NoProfile', '-File', BUILD_PS1, '-Worktree', wt, '-Log', blog]
            rc, lines, el = run_logged(cmd, os.path.join(folder, 'sdk_build_stdout.txt'), rep)
            inst = next((x.split(' ', 1)[1].strip() for x in lines if x.startswith('SDKREBASE_INSTALL ')), None)
            res = next((x for x in lines if x.startswith('SDKREBASE_RESULT ')), 'SDKREBASE_RESULT (none)')
            belapsed = next((x.split()[1] for x in lines if x.startswith('SDKREBASE_ELAPSED ')), '?')
            errs = [x.strip() for x in lines if x.startswith('  ') and 'error' in x.lower()][:30]
            rep.step('build', exit=rc, result=res, install=inst, build_s=belapsed, wall_s=round(el), errors=errs,
                     log=os.path.relpath(blog, folder))
            rep.say('build: exit %d, %s, %s s building (%d s incl. lock wait), install %s' % (rc, res, belapsed, el, inst))
            if rc == 0 and inst and os.path.isfile(os.path.join(inst, 'bin', 'rexglue.exe')):
                built = inst
            else:
                fails.append('build')
        abi, staged = None, None
        if built and sdk_root:
            abi = abi_compare(built, sdk_root, rep, extra_roots=(SDK, wt))
            rep.step('abi', **abi)
        elif built:
            fails.append('abi (the game build\'s SDK is unknown: nothing to compare with)')
        if built and abi and abi['abi_same']:
            staged = stage_exe(built, wt, exe_dir, rep)
            rep.step('stage', exe_dir=exe_dir, files=staged, method='copy of the current exe + rebased DLLs (same SDK '
                                                                    'headers, CMake config and share/rexglue sources)')
        elif built and abi:
            rep.step('stage', skipped='SDK interface changed (%s): a scratch game build against the rebased SDK is '
                                      'needed (not automated in this version)' % '; '.join(abi['rebuild_reasons']))
            fails.append('abi (exe not reusable)')
        exe = os.path.join(exe_dir, 'wwe2k14.exe')
        if a.no_regress:
            rep.step('regress', skipped='--no-regress')
        elif not staged or 'wwe2k14.exe' not in staged:
            # Only an exe staged by THIS run is ever tested (never a copy an earlier run left behind).
            rep.step('regress', skipped='no exe staged by this run' + (
                '' if built else ' (no scratch build: %s)' % ('--no-build' if a.no_build else 'see build')))
            if built and not any(f.startswith('abi') for f in fails):
                fails.append('regress (expected but not run)')
        else:
            cmd = [sys.executable, REGRESS, '--exe', exe] + (['--only', a.regress_only] if a.regress_only else [])
            rc, lines, el = run_logged(cmd, os.path.join(folder, 'regress.txt'), rep)
            verdict = next((x for x in reversed(lines) if x.startswith('REGRESS ')), 'REGRESS (no verdict line)')
            per_route = [x for x in lines if re.match(r'^\S+\s+(PASS|FAIL)\b', x) or ' run ' in x and ('PASS' in x or 'FAIL' in x)]
            details = next((x.split(' ', 1)[1] for x in lines if x.startswith('details: ')), None)
            rep.step('regress', exit=rc, verdict=verdict, routes=a.regress_only or 'all', lines=per_route[:20],
                     details=details, wall_s=round(el))
            rep.say('regress: exit %d, %s (%d s)' % (rc, verdict, el))
            if rc != 0:
                fails.append('regress')
        if built and not a.no_codegen:
            cstamp = 'sdk_rebase_' + stamp
            cmd = [sys.executable, CODEGEN, '--rexglue', os.path.join(built, 'bin', 'rexglue.exe'), '--stamp', cstamp]
            rc, lines, el = run_logged(cmd, os.path.join(folder, 'codegen.txt'), rep)
            keep = [x for x in lines if re.match(r'^(rexglue:|codegen exit|function set|recompiled bodies|  guest bytes|'
                                                 r'switch tables|partition|files:|RESULT|details)', x)]
            cdir = os.path.join(REPO, 'out', 'harness', 'codegen_repro', cstamp)
            rep.step('codegen', exit=rc, summary=keep, folder=cdir, wall_s=round(el),
                     meaning={0: 'identical (set, sizes, code)', 1: 'function set or sizes differ', 2: 'error',
                              3: 'same set and sizes, code differs'}.get(rc, '?'))
            rep.say('codegen: exit %d (%s)' % (rc, rep.data['steps']['codegen']['meaning']))
            if rc != 0:
                fails.append('codegen (exit %d)' % rc)
            gen = os.path.join(cdir, 'default')
            if not a.keep_codegen and os.path.isdir(gen) and os.path.isdir(os.path.join(cdir, 'compare')):
                shutil.rmtree(gen)
                rep.say('codegen output folder deleted (compare\\ and run.json kept): %s' % gen)
    except Exception as e:   # recorded; cleanup, safety checks and the report still run
        import traceback
        rep.say('ERROR: %s\n%s' % (e, traceback.format_exc()))
        rep.step('error', message=str(e))
        fails.append('error: %s' % e)
        errored = True
    finally:
        # 8. cleanup
        if a.keep:
            rep.say('--keep: worktree %s and %s left in place (delete both with --clean %s)' % (wt, exe_dir, stamp))
            rep.step('cleanup', kept=True)
        else:
            ok = clean_worktree(wt, rep)
            removed = remove_exe_copy(exe_dir, rep, 'cleanup')
            rep.step('cleanup', worktree_deleted=ok, exe_files_removed=removed)
            if not ok:
                fails.append('cleanup')
    # 9. safety checks
    after = dict(branch_head=out('rev-parse', a.branch), sdk_bin=hashes(os.path.join(sdk_root, 'bin')) if sdk_root else {},
                 exe_dir=hashes(MAIN_EXE_DIR), pkg_registry=pkg_registry())
    safe = {k: guard[k] == after[k] for k in guard}
    rep.step('safety', unchanged=safe, branch_head=after['branch_head'],
             pkg_registry_added=sorted('%s=%s' % (k, v) for k, v in after['pkg_registry'].items()
                                       if guard['pkg_registry'].get(k) != v),
             pkg_registry_removed=sorted('%s=%s' % (k, v) for k, v in guard['pkg_registry'].items()
                                         if after['pkg_registry'].get(k) != v))
    rep.say('safety: %s unchanged: %s; installed SDK bin unchanged: %s; main exe folder rex*.dll unchanged: %s; CMake '
            'package registry (HKCU\\%s, %d value(s)) unchanged: %s' % (
                a.branch, safe['branch_head'], safe['sdk_bin'], safe['exe_dir'], PKG_REG, len(after['pkg_registry']),
                safe['pkg_registry']))
    if not all(safe.values()):
        fails.append('safety')
    rep.data['fails'] = fails
    rep.data['verdict'] = 'SDK_REBASE %s' % ('PASS' if not fails else ('ERROR: ' if errored else 'FAIL: ') + ', '.join(fails))
    write_markdown(rep)
    rep.write()
    rep.say(rep.data['verdict'])
    rep.say('report: %s' % os.path.join(folder, 'report.md'))
    return 2 if errored else (0 if not fails else 1)


def write_markdown(rep):
    d, s = rep.data, rep.data['steps']
    m = rep.md
    m('# SDK rebase dry run %s' % d['stamp'])
    m('')
    m('%s. Started %s, finished %s. Branch `%s` at `%s`, source: %s.' % (
        d.get('verdict', '?'), d['started'], datetime.datetime.now().isoformat(timespec='seconds'), d['branch'],
        d['branch_head'][:9], d['source']))
    up = s.get('upstream')
    if up:
        m('')
        m('## Upstream')
        m('- onto: `%s` = `%s` (%s) "%s"; chosen because: %s' % (up['ref'], up['onto']['sha'][:9], up['onto']['date'],
                                                                 up['onto']['subject'], up['why']))
        m('- our base: `%s` %s (%s); upstream is **%d** commit(s) ahead of it; our branch: %d commits, %d first-parent units%s' % (
            up['base']['sha'][:9], ' '.join(up['base_tags']), up['base']['date'], up['ahead_of_base'], up['our_commits'],
            up['our_first_parent'], ' (shallow repo: counts cover the local history only)' if up['shallow_repo'] else ''))
    if s.get('synthetic'):
        m('- synthetic upstream (self-test): `%s` %s' % (s['synthetic']['commit'][:9], s['synthetic']['what']))
    ap = s.get('apply')
    if ap:
        m('')
        m('## Units (%d clean, %d conflict, %d error)' % (ap['clean'], ap['conflicts'], ap['errors']))
        m('| # | Unit | Result | Files / conflicts |')
        m('|---|---|---|---|')
        for r in ap['units']:
            if r['result'] == 'clean':
                det = '%d files' % r.get('files', 0)
            elif r['result'] == 'conflict':
                det = '; '.join('`%s` %s %d hunk(s)' % (c['file'], c['status'], c['hunks']) for c in r['conflicts'])
                det += ' (diff: %s)' % r['diff']
            else:
                det = ('git apply aborted, nothing applied: ' if r.get('aborted') else '') + (
                    ' / '.join(r.get('git_errors') or []) or r.get('message', ''))[:300].replace('|', '/').replace('\n', ' ')
                if r.get('log'):
                    det += ' (git output: %s)' % r['log']
            m('| %d | `%s` %s | %s%s | %s |' % (r['n'], r['id'][:9] if r['kind'] != 'patch' else r['id'],
                                               r['subject'][:70].replace('|', '/'), r['result'],
                                               ' (after an earlier conflict)' if r['after_conflict'] and r['result'] != 'clean' else '',
                                               det))
    nd = s.get('net_diff')
    if nd:
        m('')
        m('Net diff: %d files; identical as a patch: %d; changed: %s; missing: %s; extra: %s; same tree as `%s`: %s.' % (
            nd['files'], nd['same_patch'], ', '.join(nd['changed_patch']) or 'none', ', '.join(nd['missing']) or 'none',
            ', '.join(nd['extra']) or 'none', d['branch'], nd['same_tree_as_branch']))
    for name in ('build', 'abi', 'stage', 'regress', 'codegen', 'cleanup', 'safety'):
        v = s.get(name)
        if not v:
            continue
        m('')
        m('## %s' % name.capitalize())
        if name == 'abi':
            m('- new `%s` vs the game\'s SDK `%s`; abi_same: **%s**' % (v['new'], v['old'], v['abi_same']))
            for k, what in (('header', 'headers (include)'), ('config', 'CMake package config'),
                            ('app_src', 'share/rexglue (compiled into wwe2k14.exe)'), ('other', 'other installed files')):
                m('- %s: added %s; removed %s; changed %s; version stamp only %s' % (
                    what, v[k + '_added'] or 'none', v[k + '_removed'] or 'none', v[k + '_changed'] or 'none',
                    v[k + '_version_only'] or 'none'))
            m('- equal after replacing the install/source root paths: %s; stale leftovers in the old install: %s; '
              'not used by the game build, differing: %s' % (v['path_only'] or 'none', v['stale_in_old'] or 'none',
                                                            v['unused_changed'] or 'none'))
            m('- libraries: %d with other bytes (expected: debug info paths), added %s, removed %s' % (
                len(v['libs_changed']), v['libs_added'] or 'none', v['libs_removed'] or 'none'))
            if v['rebuild_reasons']:
                m('- the exe must be rebuilt: %s' % '; '.join(v['rebuild_reasons']))
            continue
        for k, x in v.items():
            if isinstance(x, list):
                m('- %s:' % k)
                for y in x:
                    m('  - `%s`' % y if isinstance(y, str) else '  - %s' % json.dumps(y))
            elif isinstance(x, dict):
                m('- %s: %s' % (k, ', '.join('%s=%s' % (kk, (vv['sha256'][:12] if isinstance(vv, dict) and 'sha256' in vv else vv))
                                          for kk, vv in x.items())))
            else:
                m('- %s: %s' % (k, x))


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('sdk_rebase: ERROR: %s' % e)
        sys.exit(2)

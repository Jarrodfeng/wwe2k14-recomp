# Tool exes for the gate (Session 101 lane 163): are fview.exe / rplay.exe fresh, who builds them, and what does a build failure say.
# Why: lanes run their check with --no-build on whatever exe sits in the checkout. Measured 2026-11-12: movie_ours FAILed on an exe older than the
# merge (fresh build: 0 failing lines); ui_ours printed "FAIL fview build" inside a PASS; lane 153's wrestler gates got "link failed: permission
# denied" because another gate run in the same worktree had the dev fview.exe open. A gate that is red or green for a reason other than the code
# wastes runs.
#   fresh   = the exe exists and is not older than any source it is built from (the spec's source dirs/files; a build stamps the exe with its end time)
#   lock    = <exe>.lock holds the pid of the process building it (O_EXCL create; a dead pid's lock is stale and removed); a second gate run waits for
#             the lock, then re-checks freshness (so two concurrent gate runs build once)
#   held    = the build failed because a running process has the exe open: wait up to GATE_EXE_WAIT_S (default 600 s), then fail naming the pid and its
#             command line. The other process is never killed.
# Library for tools\gate\gate.py; `py -I tools\gate\exes.py ensure out/renderer/dev/fview.exe [--root DIR]` does one by hand.
import ctypes, json, os, re, subprocess, sys, time
from pathlib import Path

SRC_EXT = ('.cpp', '.h', '.hpp', '.inl', '.c', '.cc', '.hlsl', '.ps1', '.txt', '.py', '.glsl', '.vert', '.frag', '.comp')
HELD_RE = re.compile(r'permission denied|cannot open output file|being used by another process|access is denied|unable to open output', re.I)
# a build or link error in a check's own output (a nested build inside the check, or its build step)
BUILD_RE = re.compile(r'link failed|file\(s\) failed to compile|(fview|renderer|rplay) build \(exit \d+\)|cannot open output file|unable to open output|'
                      r'error: linker command failed|ld\.lld: error|clang(\+\+)?: error|missing objects', re.I)

_FVIEW_SRC = {'dirs': ['src/renderer', 'src/fview', 'src/renderer_scene', 'src/assets'],
              'files': ['tools/fview/session_deps.txt'], 'skip': ['src/renderer/player']}
_RPLAY_SRC = {'dirs': ['src/renderer', 'src/renderer_scene', 'src/assets', 'src/fview'],
              'files': ['tools/fview/session_deps.txt'], 'skip': []}


def registry():
    # normalised repo-relative exe path -> spec. build: argv run from the root. A lane can also give a spec inline (see spec_for).
    def mk(path, script, release, src):
        build = ['pwsh', '-NoProfile', '-File', script] + (['-Release'] if release else [])
        return {'path': path, 'build': build, 'timeout_s': 1800, 'sources': dict(src, files=src['files'] + [script])}
    r = {}
    for kind, rel in (('dev', False), ('release', True)):
        r[f'out/renderer/{kind}/fview.exe'] = mk(f'out/renderer/{kind}/fview.exe', 'tools/fview/build.ps1', rel, _FVIEW_SRC)
        r[f'out/renderer/{kind}/rplay.exe'] = mk(f'out/renderer/{kind}/rplay.exe', 'tools/renderer/build.ps1', rel, _RPLAY_SRC)
    return r


def norm(p):
    p = str(p).replace(chr(92), '/')
    return p[2:] if p.startswith('./') else p


def spec_for(entry, reg):
    # entry: a registry key (string) or an inline spec {"path", "build", "sources": {"dirs": [], "files": []}, "timeout_s"}
    if isinstance(entry, str):
        return reg.get(norm(entry))
    s = dict(entry)
    s['path'] = norm(s['path'])
    s.setdefault('timeout_s', 1800)
    s.setdefault('sources', {'dirs': [], 'files': []})
    return s


def infer(check, root, reg):
    # The exes a check needs: its explicit "exes", else (for a --no-build check or a check calling build.ps1) the exes its script names.
    if 'exes' in check:
        return list(check['exes'])
    cmd = check.get('cmd', [])
    if '--no-build' not in cmd:
        return []
    text = ''
    for a in cmd:
        p = Path(root) / a
        if a.endswith('.py') and p.is_file():
            try:
                text += p.read_text(encoding='utf-8', errors='replace')
            except OSError:
                pass
    found = []
    for tool in ('fview', 'rplay'):
        if re.search(r'\b' + tool + r'(\.exe)?\b', text):
            # release only when the script names the release folder next to the tool (crowd_props_check: 'release', 'fview.exe')
            rel = re.search(r'release[' + chr(39) + chr(34) + r'/\, ]+' + tool, text)
            found.append(f'out/renderer/{"release" if rel else "dev"}/{tool}.exe')
    return found


def newest_source(root, src):
    root = Path(root)
    skip = [(root / s).resolve() for s in src.get('skip', [])]
    best, who = 0.0, None
    for d in src.get('dirs', []):
        base = root / d
        for dp, dn, fn in os.walk(base):
            dpp = Path(dp).resolve()
            if any(dpp == s or s in dpp.parents for s in skip):
                dn[:] = []
                continue
            for f in fn:
                if f.endswith(SRC_EXT):
                    try:
                        m = os.stat(os.path.join(dp, f)).st_mtime
                    except OSError:
                        continue
                    if m > best:
                        best, who = m, os.path.join(dp, f)
    for f in src.get('files', []):
        try:
            m = os.stat(root / f).st_mtime
        except OSError:
            continue
        if m > best:
            best, who = m, str(root / f)
    return best, who


def is_fresh(root, spec):
    exe = Path(root) / spec['path']
    if not exe.is_file():
        return False, 'missing'
    newest, who = newest_source(root, spec['sources'])
    if newest > exe.stat().st_mtime:
        return False, f'sources newer than the exe ({os.path.relpath(who, root) if who else "?"})'
    return True, 'fresh'


def pid_alive(pid):
    if pid <= 0:
        return False
    if os.name == 'nt':
        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
        return bool(ok) and code.value == 259   # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _lock_pid(lock):
    try:
        return int(json.loads(Path(lock).read_text(encoding='utf-8')).get('pid', 0))
    except Exception:
        return 0


def acquire_lock(lock, deadline, poll=0.5):
    # True when this process now holds the lock; False when the deadline passed with a live holder.
    lock = str(lock)
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, json.dumps({'pid': os.getpid(), 'cmd': ' '.join(sys.argv)[:200], 'when': time.strftime('%H:%M:%S')}).encode())
            os.close(fd)
            return True
        except FileExistsError:
            pid = _lock_pid(lock)
            if not pid_alive(pid):
                try:
                    os.remove(lock)   # stale: its builder is gone
                except OSError:
                    pass
                continue
            if time.time() > deadline:
                return False
            time.sleep(poll)


def holders(exe):
    # (pid, command line) of running processes whose image is this exe; [] when none is found (an open handle without a process, or no PowerShell).
    if os.name != 'nt':
        return []
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and ($_.ExecutablePath -ieq '%s') } | "
          "ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }" % str(Path(exe).resolve()).replace("'", "''"))
    try:
        t = subprocess.run(['pwsh', '-NoProfile', '-Command', ps], capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return []
    out = []
    for line in t.splitlines():
        if '\t' in line:
            a, b = line.split('\t', 1)
            if a.strip().isdigit():
                out.append((int(a), b.strip()))
    return out


def ensure(root, spec, wait_s=None, poll=0.5, log=print, retry_s=15.0):
    """-> (state, text). state: 'fresh' (not rebuilt), 'built', 'BUILD' (failed; text names the exe and the cause)."""
    root = Path(root)
    exe = root / spec['path']
    ok, why = is_fresh(root, spec)
    if ok:
        return 'fresh', f'{spec["path"]} is fresh'
    wait_s = float(os.environ.get('GATE_EXE_WAIT_S', 600)) if wait_s is None else wait_s
    exe.parent.mkdir(parents=True, exist_ok=True)
    lock = str(exe) + '.lock'
    if not acquire_lock(lock, time.time() + spec['timeout_s'] + wait_s, poll):
        return 'BUILD', f'{spec["path"]}: another build holds {lock} (pid {_lock_pid(lock)}) past the wait'
    try:
        ok, why = is_fresh(root, spec)   # a concurrent gate run may have built it while this one waited
        if ok:
            return 'fresh', f'{spec["path"]} is fresh (built by another run)'
        held_deadline = time.time() + wait_s
        last_log = 0.0
        while True:
            t0 = time.time()
            try:
                p = subprocess.run(spec['build'], cwd=root, capture_output=True, text=True, timeout=spec['timeout_s'])
                rc, text = p.returncode, (p.stdout + p.stderr)
            except subprocess.TimeoutExpired:
                return 'BUILD', f'{spec["path"]}: build timed out after {spec["timeout_s"]} s'
            except OSError as e:
                return 'BUILD', f'{spec["path"]}: cannot start the build ({e})'
            if rc == 0 and exe.is_file():
                now = time.time()
                os.utime(exe, (now, now))   # stamp: "built from the sources as of the end of this build"
                return 'built', f'built fresh {spec["path"]} ({why})'
            lines = [l for l in text.strip().splitlines() if l.strip()]
            last = ' | '.join(lines[-3:]) if lines else '(no output)'
            if rc and HELD_RE.search(text):
                if time.time() < held_deadline:
                    if time.time() - last_log > 14:
                        last_log = time.time()
                        log(f'[gate] {spec["path"]} is held by a running process; waiting ({int(held_deadline - time.time())} s left)')
                    time.sleep(max(0.05, min(retry_s, held_deadline - time.time())))   # (a retry is a whole build run: not every poll)
                    continue
                h = holders(exe)
                who = '; '.join(f'pid {a} ({b[:120]})' for a, b in h) if h else 'no running process found with that image: an open handle'
                return 'BUILD', f'{spec["path"]}: exe held by {who} (link: permission denied after {int(wait_s)} s)'
            return 'BUILD', f'{spec["path"]}: build failed (exit {rc}): {last[:300]}'
    finally:
        try:
            os.remove(lock)
        except OSError:
            pass


def build_cause(output):
    # The line in a check's output that names a build or link error, or None.
    for line in output.splitlines():
        if BUILD_RE.search(line):
            return line.strip()[:200]
    return None


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['ensure'])
    ap.add_argument('exe')
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    a = ap.parse_args()
    sp = spec_for(a.exe, registry())
    if not sp:
        print('unknown exe', a.exe)
        sys.exit(2)
    st, tx = ensure(a.root, sp)
    print(f'[gate] {tx}')
    sys.exit(1 if st == 'BUILD' else 0)

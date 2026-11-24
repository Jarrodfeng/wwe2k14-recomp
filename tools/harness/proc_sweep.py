"""proc_sweep: find (and with --kill, remove) orphaned build / q processes. QTF, S103 WTA fix 7; rewritten after the Opus review (C3, H1, M1).

    py tools\\harness\\proc_sweep.py [--kill] [--min-age 30] [--json]      (dry run unless --kill)
    py tools\\harness\\proc_sweep.py --selftest

OPT-IN OWNERSHIP ONLY. A process is a candidate only when its environment carries  WWE_SWEEP_OWNER=<pid>:<create_time_epoch_seconds>  (set by
tools\\compile\\proc.run for every child it starts, by q.py / build_ours.py for their workers and compilers, and by build_rig.ps1) AND the named owner
process is gone (no such pid, or a pid reused by a newer process) AND it is older than --min-age minutes. A process without the variable is never
touched, whatever it looks like (the earlier name / command-line matching could hit an unrelated python or compiler).

Never touched, even with the variable: this process and its ancestors; ^wwe2k14.*\\.exe$ and our tool exes (frame_rig, fview, rplay, host_rig,
record_frame_test, ...); the run lock's holder pids; anything whose tree holds one of those or a `git` process; tidy.py / runlock / sampler command
lines; a process whose working directory is inside a worktree that is still in use (a hook `worktree_busy(path)`: tidy passes its agent_state check;
the default treats a worktree directory touched in the last 30 min as in use). The protected set is re-computed inside kill_tree, and a tree that
turns out to hold a protected process is left whole. The root of a tree is killed first, then what is left of its children.

REPORT-ONLY (QTF round 3): nothing calls --kill automatically. tidy may print the dry-run list; kill is a manual decision. Additional protections
(reviews 2 / 3): proc.DAEMONS plus git / ssh / sccache / gh / adb are protected BY NAME, root included, with their ancestors and subtrees (a shared daemon
that inherited the owner variable is never an orphan); a process whose working directory cannot be read is skipped; "worktree in use" covers
.claude\\worktrees\\X, thirdparty\\sdk-wt\\X, out\\wt\\X and bisect worktrees, honours tidy's `<worktree>.owner` file (pid=N alive = in use), and the
tidy-supplied agent_state hook.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ENV_VAR = 'WWE_SWEEP_OWNER'
PROTECT_RE = re.compile(r'^(wwe2k14.*|frame_rig|fview.*|rplay.*|host_rig.*|record_frame_test|nr_unit|owned_check.*|wwe2k14_launcher)\.exe$', re.I)
PROTECT_BASENAMES = {'git', 'ssh', 'sccache', 'gh', 'adb', 'ssh-agent'}
try:
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'compile'))
    import proc as _proc
    PROTECT_BASENAMES |= set(_proc.DAEMONS)
except Exception:        # noqa: BLE001 - the static list above already covers the real daemon names
    pass
SKIP_CMD = re.compile(r'tidy\.py|runlock|util_sample|sampler|session_start|upload_playbook', re.I)


def _psutil():
    import psutil
    return psutil


def owner_string():
    """the value a tool puts in WWE_SWEEP_OWNER for its children: this process's pid and create time"""
    ps = _psutil()
    me = ps.Process()
    return '%d:%.0f' % (me.pid, me.create_time())


def main_root():
    try:
        out = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        cd = os.path.abspath(os.path.join(ROOT, out))
        return os.path.dirname(cd) if os.path.basename(cd) == '.git' else ROOT
    except Exception:
        return ROOT


def lock_pids():
    pids = set()
    for base in {ROOT, main_root()}:
        for dp, _dn, fn in os.walk(os.path.join(base, 'out', 'harness', 'runlock')):
            for f in fn:
                if f.endswith('.json'):
                    try:
                        txt = open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
                    except OSError:
                        continue
                    pids.update(int(x) for x in re.findall(r'"pid"\s*:\s*(\d+)', txt))
    return pids


def is_protected_name(name):
    n = (name or '').lower()
    return bool(PROTECT_RE.match(n)) or n.removesuffix('.exe') in PROTECT_BASENAMES


def protected_set(ps):
    """pids never to be killed: us + ancestors, protected exes (games, tool exes), lock holders, and the ancestors / descendants of those"""
    prot = {os.getpid()}
    me = ps.Process()
    try:
        prot.update(a.pid for a in me.parents())
    except Exception:
        pass
    roots = set(lock_pids())
    for p in ps.process_iter(['name']):
        if is_protected_name(p.info['name']):
            roots.add(p.pid)
    for pid in roots:
        try:
            p = ps.Process(pid)
        except Exception:
            continue
        prot.add(pid)
        try:
            prot.update(a.pid for a in p.parents())
            prot.update(c.pid for c in p.children(recursive=True))
        except Exception:
            pass
    return prot


def owner_gone(ps, owner):
    """owner = 'pid:create_time' -> True when that process no longer exists (or the pid belongs to a newer process)"""
    try:
        pid_s, ct_s = owner.split(':', 1)
        pid, ct = int(pid_s), float(ct_s)
    except ValueError:
        return False          # a malformed value is not ours: leave it
    try:
        p = ps.Process(pid)
        return abs(p.create_time() - ct) > 3.0 and p.create_time() > ct
    except ps.NoSuchProcess:
        return True
    except Exception:
        return False


def default_worktree_busy(path):
    """in use when tidy's `<worktree>.owner` file names a live pid, or the directory was touched in the last 30 min (tidy's wiring adds agent_state)"""
    try:
        import psutil
        txt = open(path + '.owner', encoding='utf-8', errors='replace').read()
        m = re.search(r'pid=(\d+)', txt)
        if m and psutil.pid_exists(int(m.group(1))):
            return True
    except OSError:
        pass
    try:
        return time.time() - os.path.getmtime(path) < 1800
    except OSError:
        return False


WT_RE = re.compile(r'^(.*?[\\/](?:\.claude[\\/]worktrees|sdk-wt|out[\\/]wt|out[\\/]bisect|bisect)[\\/][^\\/]+)', re.I)


def worktree_of(cwd):
    m = WT_RE.search(cwd or '')
    return m.group(1) if m else None


def _has_git(tree):
    """a git / ssh / daemon process anywhere in the tree (ROOT INCLUDED: pass [p] + children)"""
    for c in tree:
        try:
            if (c.name() or '').lower().removesuffix('.exe') == 'git':
                return True
        except Exception:
            pass
    return False


def candidates(min_age_min=30.0, now=None, only=None, worktree_busy=None):
    """-> list of dicts. `only(rec)` (optional) is ANDed with every other rule, never ORed."""
    ps = _psutil()
    now = now or time.time()
    prot = protected_set(ps)
    busy = worktree_busy or default_worktree_busy
    out = []
    for p in ps.process_iter():
        try:
            if p.pid in prot or p.pid == 0:
                continue
            if is_protected_name(p.name()):
                continue
            try:
                env = p.environ()
            except Exception:
                continue                       # unreadable environment (another user, protected): not ours to judge
            owner = env.get(ENV_VAR)
            if not owner:
                continue                       # opt-in only
            cmd = ' '.join(p.cmdline())
            if SKIP_CMD.search(cmd):
                continue
            age = (now - p.create_time()) / 60.0
            if age < min_age_min:
                continue
            if not owner_gone(ps, owner):
                continue
            try:
                cwd = p.cwd()
            except Exception:
                continue                       # an unreadable working directory: cannot tell whose it is, so skip
            wt = worktree_of(cwd)
            if wt and os.path.isdir(wt) and busy(wt):
                continue
            tree = [p] + p.children(recursive=True)
            if any(c.pid in prot or is_protected_name(c.name()) for c in tree):
                continue
            if _has_git(tree):
                continue                       # M1: never cut a git operation in half (the root's own name counts)
            rec = {'pid': p.pid, 'name': (p.name() or '').lower(), 'age_min': round(age, 1), 'owner': owner, 'cmd': cmd[:200],
                   'create_time': p.create_time()}
            if only is not None and not only(rec):
                continue
            out.append(rec)
        except (ps.NoSuchProcess, ps.AccessDenied, ps.ZombieProcess):
            continue
    return sorted(out, key=lambda r: -r['age_min'])


def kill_tree(rec):
    """root first, then what is left of its children. The protected set is re-checked NOW: a tree that holds a protected process, git, or a pid
    that has been reused since the scan is left whole. -> number killed"""
    ps = _psutil()
    try:
        p = ps.Process(rec['pid'])
        if abs(p.create_time() - rec['create_time']) > 0.5:
            return 0                           # the pid was reused after the scan
    except Exception:
        return 0
    prot = protected_set(ps)
    tree = p.children(recursive=True)
    if p.pid in prot or any(c.pid in prot or is_protected_name(c.name()) for c in [p] + tree) or _has_git(tree + [p]):
        return 0
    n = 0
    for k in [p] + tree:                       # the root first
        try:
            if k.pid not in prot and not is_protected_name(k.name()):
                k.kill()
                n += 1
        except Exception:
            pass
    ps.wait_procs([p] + tree, timeout=5)
    return n


def sweep(kill=False, min_age=30.0, log=None, quiet=False, only=None, worktree_busy=None):
    cands = candidates(min_age, only=only, worktree_busy=worktree_busy)
    for c in cands:
        line = f'proc_sweep: {"KILL" if kill else "orphan"} pid {c["pid"]} {c["name"]} age {c["age_min"]:.0f} min, owner {c["owner"]} is gone: {c["cmd"][:120]}'
        if kill:
            c['killed'] = kill_tree(c)
            if log:
                log(line)
        if not quiet:
            print(line)
    if not cands and not quiet:
        print('proc_sweep: no orphaned build / q processes (opt-in owner variable only)')
    return cands


def selftest():
    import tempfile
    ps = _psutil()
    marker = 'proc_sweep_selftest_marker_%d' % os.getpid()
    d = tempfile.mkdtemp(prefix='proc_sweep_st_')
    dead_owner = '999999:1000000000'                      # no such pid: an owner that is gone
    sleeper = f'import time; time.sleep(2e2)  # {marker}'
    launcher = os.path.join(d, 'launch.py')
    pidf = os.path.join(d, 'orphan.pid')
    with open(launcher, 'w') as f:
        f.write('import subprocess, sys, os\n'
                f'env = dict(os.environ, {ENV_VAR}={dead_owner!r})\n'
                f'p = subprocess.Popen([sys.executable, "-c", {sleeper!r}], env=env, creationflags=0x00000008 | 0x00000200 if sys.platform == "win32" else 0, close_fds=True)\n'
                f'open({pidf!r}, "w").write(str(p.pid))\n')
    subprocess.run([sys.executable, launcher], check=True, timeout=60)
    orphan = int(open(pidf).read())
    env_live = dict(os.environ, **{ENV_VAR: owner_string()})                                   # owner = THIS process: alive
    live = subprocess.Popen([sys.executable, '-c', f'import time; time.sleep(2e2)  # {marker}_live'], env=env_live)
    noenv = subprocess.Popen([sys.executable, '-c', f'import time; time.sleep(2e2)  # {marker}_noenv'],
                             env={k: v for k, v in os.environ.items() if k != ENV_VAR})       # orphan-looking but never opted in
    only = lambda r: marker in r['cmd']                                                        # ANDed with every rule
    nobusy = lambda path: False
    try:
        time.sleep(1.0)
        assert ps.pid_exists(orphan)
        assert orphan not in [c['pid'] for c in candidates(30.0, only=only, worktree_busy=nobusy)], 'younger than 30 min must not be listed'
        found = {c['pid'] for c in candidates(0.0, only=only, worktree_busy=nobusy)}
        assert orphan in found, f'orphan {orphan} not found: {found}'
        assert live.pid not in found, 'a process whose owner is alive must be left alone'
        assert noenv.pid not in found, 'a process without the owner variable must never be listed'
        sweep(kill=False, min_age=0.0, quiet=True, only=only, worktree_busy=nobusy)
        assert ps.pid_exists(orphan), 'dry run must not kill'
        sweep(kill=True, min_age=0.0, quiet=True, only=only, worktree_busy=nobusy)
        time.sleep(0.5)
        assert not ps.pid_exists(orphan), 'orphan survived --kill'
        assert live.poll() is None and noenv.poll() is None, 'a process that should be left alone was killed'
        assert os.getpid() in protected_set(ps)
        assert all(is_protected_name(n) for n in ('git.exe', 'sccache.exe', 'ssh.exe', 'adb.exe')), 'daemons / git / ssh are protected by name'
        assert is_protected_name('wwe2k14.exe') and is_protected_name('wwe2k14_launcher.exe') and is_protected_name('frame_rig.exe') and not is_protected_name('python.exe')
        print(f'proc_sweep selftest OK: opted-in orphan pid {orphan} found and killed; live-owner pid {live.pid} and no-variable pid {noenv.pid} left alone; '
              'young not listed; dry run kills nothing; games / tool exes protected')
        return 0
    finally:
        for k in (live, noenv):
            k.kill()
        try:
            ps.Process(orphan).kill()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kill', action='store_true')
    ap.add_argument('--min-age', type=float, default=30.0, help='minutes')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.json:
        print(json.dumps(sweep(a.kill, a.min_age, quiet=True), indent=1))
    else:
        sweep(a.kill, a.min_age)
    return 0


if __name__ == '__main__':
    sys.exit(main())

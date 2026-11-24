r"""S110 QREBUILD: put a rebuilt index file in place without ever forcing it over a file a reader holds open; say which complete copy a reader opens;
one builder at a time; and a record of the last attempt, so a failed build is not repeated by every lookup.

Why (docs\research\s110\QREBUILD.md): on Windows every sqlite reader holds its file handle (no FILE_SHARE_DELETE) for the connection's life, and os.replace onto
a held file raises PermissionError. build_index.py retried that for 30 s, then raised and threw the finished build away; q.py ran that rebuild INSIDE the lookup
and recorded nothing, so every later stale lookup paid the whole rebuild again (and several lookups built side by side).

  publish(tmp, path)   ONE os.replace attempt onto `path`; refused (a reader holds it) -> the finished file is renamed to a NEW name `<path>.pending-<ns>-<pid>`
                       (nobody can hold a name that did not exist, so that rename succeeds) and that name is returned. Never waits, never throws the work away.
  newest(path)         the file a reader opens: the newest pending copy when it is newer than `path`, after ONE attempt to promote it over `path` (it succeeds the
                       first moment nobody holds either file), else `path`. Older pending copies are removed best-effort (a held one goes on a later call).
  acquire(path) / release(lock) / holder(path)   one builder at a time: `<path>.lock`, created exclusively, holding the pid; a dead (or hung) holder's lock is taken over.
  record(path, key, result, **kw) / last(path)   `<path>.attempt.json`: the last build's input key, result (spawned / running / ok / failed), pid, time, note.
  kick(path, key, cmd, log)   ONE line for a stale index, and at most one background build: a build runs -> say so; the last build for these inputs failed (or died)
                       within BACKOFF_S -> say so loudly and do not start another; else start `cmd` detached, outside the caller's process tree (start), and say so.
  sweep(path)          remove `<path>.<pid>.tmp` files of builders that are gone (a killed build never runs its cleanup).

    py -I tools\q\qpublish_selftest.py
"""
import glob
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_fields import pid_alive  # noqa: E402  (Windows: OpenProcess + exit code)

BACKOFF_S = 600.0       # a failed build for the same inputs is not started again by a lookup for this long
LOCK_IDLE_S = 1800.0    # a live holder older than this is taken for hung (a build takes well under a minute)
SPAWN_GRACE_S = 60.0    # a build started this recently that has not taken its lock yet is counted as running


def _remove(p):
    try:
        os.remove(p)
        return True
    except OSError:
        return False


def _pending(path):
    """[(ns, file)] of the pending copies of `path`, oldest first"""
    out = []
    for p in glob.glob(glob.escape(path) + '.pending-*'):
        try:
            out.append((int(p.rsplit('.pending-', 1)[1].split('-')[0]), p))
        except (ValueError, IndexError):
            continue
    return sorted(out)


def publish(tmp, path):
    """-> the file now holding the new data: `path`, or `<path>.pending-<ns>-<pid>` when a reader holds `path` open"""
    try:
        os.replace(tmp, path)
    except OSError:
        if not os.path.exists(tmp):
            raise
        pend = f'{path}.pending-{time.time_ns():020d}-{os.getpid()}'
        os.replace(tmp, pend)
        for _, p in _pending(path):
            if p != pend:
                _remove(p)
        return pend
    for _, p in _pending(path):
        _remove(p)
    return path


def newest(path):
    """-> the file a reader opens (see the module docstring)"""
    pend = _pending(path)
    if not pend:
        return path
    ns, best = pend[-1]
    for _, p in pend[:-1]:
        _remove(p)
    try:
        cur = os.stat(path).st_mtime_ns
    except OSError:
        cur = -1
    if cur >= ns:           # `path` was written after this copy was made: the copy is the old one
        _remove(best)
        return path
    try:
        os.replace(best, path)
        return path
    except OSError:
        return best if os.path.exists(best) else path


def holder(path):
    """the pid of a LIVE builder holding `<path>.lock` (-1: a lock being created this instant), else None (no lock, a dead pid, or idle past LOCK_IDLE_S)"""
    lk = path + '.lock'
    try:
        with open(lk) as f:
            txt = f.read().strip()
        age = time.time() - os.path.getmtime(lk)
    except OSError:
        return None
    if not txt:
        return -1 if age < 5 else None
    try:
        pid = int(txt)
    except ValueError:
        return None
    return pid if pid_alive(pid) and age < LOCK_IDLE_S else None


def acquire(path):
    """-> the lock file (now ours) or None when a live builder holds it"""
    lk = path + '.lock'
    os.makedirs(os.path.dirname(os.path.abspath(lk)), exist_ok=True)
    for _ in range(4):
        try:
            fd = os.open(lk, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            return lk
        except FileExistsError:
            if holder(path):
                return None
            mine = f'{lk}.stale.{os.getpid()}'
            try:
                os.rename(lk, mine)      # of several takers of a dead holder's lock only one wins the rename
            except OSError:
                continue
            _remove(mine)
    return None


def release(lk):
    if not lk:
        return
    try:
        with open(lk) as f:
            mine = f.read().strip() == str(os.getpid())
    except OSError:
        return
    if mine:
        for _ in range(5):          # a reader of the lock (holder()) can refuse the remove for a moment
            if _remove(lk) or not os.path.exists(lk):
                return
            time.sleep(0.1)


def record(path, key, result, **kw):
    """write `<path>.attempt.json` (advisory: a reader holding it this instant leaves the previous record)"""
    d = {'key': key, 'result': result, 'pid': os.getpid(), 'when': time.time(), 'at': time.strftime('%Y-%m-%d %H:%M:%S')}
    d.update(kw)
    tmp = f'{path}.attempt.{os.getpid()}.tmp'
    try:
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(d, f)
        for _ in range(3):
            try:
                os.replace(tmp, path + '.attempt.json')
                return
            except OSError:
                time.sleep(0.05)
    except OSError:
        pass
    _remove(tmp)


def last(path):
    try:
        with open(path + '.attempt.json', encoding='utf-8') as f:
            d = json.load(f)
        return d if isinstance(d, dict) else None
    except (OSError, ValueError):
        return None


def sweep(path):
    for p in glob.glob(glob.escape(path) + '.*.tmp'):
        try:
            pid = int(p[len(path) + 1:-4])
        except ValueError:
            continue
        if pid != os.getpid() and not pid_alive(pid):
            _remove(p)


def start(cmd, log):
    """start `cmd` so that it outlives this process AND is not in its process tree; output to `log`; -> its pid.
    S110 review: q's watchdog (qtimeout.kill_children) kills every descendant it finds by parent pid (psutil children(recursive=True)). A child started
    directly, even with DETACHED_PROCESS, still names this process as its parent, so a `q multi` that timed out killed the build it had started; the record
    then said `running` with a dead pid and every lookup backed off for 10 min. A launcher (this file, --_launch) starts the build and exits at once: the
    build's parent is then a process that is gone, and no tree walk from here reaches it."""
    env = dict(os.environ)
    env.pop('WWE_SWEEP_OWNER', None)       # not this q call's child to sweep when the call ends (tools/q/qtimeout.mark_owner)
    os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
    r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--_launch', log] + list(cmd), env=env, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, timeout=60)
    try:
        return int(r.stdout.strip())
    except ValueError:
        raise OSError(f'the build launcher failed (exit {r.returncode}): {(r.stderr or r.stdout).strip()[-300:]}') from None


def _launch(log, cmd):
    """the launcher's own process: start `cmd` detached, print its pid, exit (see start).
    S110 review: a hidden console (CREATE_NO_WINDOW), not DETACHED_PROCESS: a process with no console gives every console child it starts flags 0 (the
    build's multiprocessing Pool workers) a NEW, visible console window; under a hidden console they share it and nothing shows."""
    kw = {'creationflags': subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP} if sys.platform == 'win32' else {'start_new_session': True}
    with open(log, 'wb') as lf:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=lf, stderr=subprocess.STDOUT, close_fds=True, **kw)
    print(p.pid, flush=True)
    return 0


def kick(path, key, cmd, log, spawn=True):
    """-> one line saying what is being done about the stale index at `path` (see the module docstring)"""
    pid = holder(path)
    if pid:
        return f'a rebuild is running (pid {pid if pid > 0 else "?"}): a call after it ends reads the new index'
    rec = last(path) or {}
    if rec.get('key') == key:
        age = time.time() - float(rec.get('when') or 0)
        res = rec.get('result')
        try:
            rpid = int(rec.get('pid') or 0)
        except (TypeError, ValueError):
            rpid = 0
        if res == 'spawned' and age < SPAWN_GRACE_S and pid_alive(rpid):
            return f'a rebuild was started {age:.0f} s ago (pid {rpid}): a call after it ends reads the new index'
        if age < BACKOFF_S:
            why = rec.get('note') or {'running': 'the builder died before it finished', 'spawned': 'the builder never took its lock',
                                      'ok': 'it finished, yet the index still reads stale'}.get(res, str(res))
            nxt = time.strftime('%H:%M:%S', time.localtime(float(rec.get('when') or 0) + BACKOFF_S))
            return (f'the last rebuild for these inputs FAILED at {rec.get("at")} ({why}); no automatic retry before {nxt}; '
                    f'by hand: py tools\\q\\build_index.py (log {log})')
    if not spawn:
        return 'automatic rebuild off (WWE_Q_NO_AUTOUPDATE=1): py tools\\q\\build_index.py'
    try:
        child = start(cmd, log)
    except (OSError, subprocess.SubprocessError) as e:      # the lookup still answers; the next stale lookup tries again
        return f'could not start a background rebuild ({e}); by hand: py tools\\q\\build_index.py'
    record(path, key, 'spawned', pid=child)
    return f'rebuilding in the background (pid {child}, below-normal priority, well under a minute; log {log}): a call after it ends reads the new index'


if __name__ == '__main__':
    if len(sys.argv) > 3 and sys.argv[1] == '--_launch':
        sys.exit(_launch(sys.argv[2], sys.argv[3:]))
    print(__doc__)

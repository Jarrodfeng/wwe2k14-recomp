"""GTW2: one goto per live name. A pid lock file out/live/<name>.goto.lock, taken before the plan runs, refused while a live
process holds it, removed on exit. A stale file (dead pid) is replaced."""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'live'))
import live  # noqa: E402


class Held(Exception):
    pass


def pid_alive(pid):
    if pid <= 0:
        return False
    if os.name != 'nt':
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    try:
        code = ctypes.c_ulong()
        return bool(k.GetExitCodeProcess(h, ctypes.byref(code))) and code.value == 259  # STILL_ACTIVE
    finally:
        k.CloseHandle(h)


def lock_path(name):
    return os.path.join(live.OUT, '%s.goto.lock' % name)


def take(name, pid=None):
    path = lock_path(name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pid = pid or os.getpid()
    for _ in range(3):
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                with open(path, encoding='utf-8') as f:
                    holder = int(f.read().strip() or 0)
            except (OSError, ValueError):
                holder = 0
            if holder and holder != pid and pid_alive(holder):
                raise Held('another goto (pid %d) is driving live name "%s" (%s); one goto per name' % (holder, name, path))
            try:
                os.remove(path)   # stale
            except OSError:
                pass
            continue
        with os.fdopen(fd, 'w') as f:
            f.write(str(pid))
        return path
    raise Held('could not take %s' % path)


def release(path):
    try:
        os.remove(path)
    except OSError:
        pass

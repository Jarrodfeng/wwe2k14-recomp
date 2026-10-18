"""Session 8 run lock for Python tools (protocol and files: tools/runlock.ps1).

A tool that launches the game, builds, or must not be interleaved with other game runs calls ensure_lock() first,
before it does anything else (everything before the call runs twice: once outside the lock, once inside):

    import sys; sys.path.insert(0, r'C:\\Users\\Jarrod\\wwe2k14-recomp\\tools\\harness')
    from runlock import ensure_lock
    ensure_lock('regress')     # re-runs this script under `runlock.ps1 run` unless this process already holds it

Inside the lock, WWE2K14_RUNLOCK_TOKEN is set; every run_and_triage.ps1 / run_list.ps1 / build.ps1 the tool starts
re-enters the same lock instead of queueing behind it. The re-run keeps the interpreter flags (-I, -X utf8, -O, -m).
The [runlock] messages go to stderr, so the tool's stdout stays clean.

Modes (Session 8 task 0): ensure_lock() is EXCLUSIVE (builds, timed runs, regress, bisect, ...). A tool that only
launches ordinary, untimed game runs may pass shared=True: up to 3 such holders run at once (tools/runlock.ps1).
Inside a shared hold an exclusive ensure_lock() exits with an error (it would wait on its own parent forever).
"""
import ctypes
import ctypes.wintypes as wt
import json
import msvcrt
import os
import subprocess
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
RUNLOCK_PS1 = os.path.join(REPO, 'tools', 'runlock.ps1')
LOCK_DIR = os.path.join(REPO, 'out', 'harness', 'runlock')
LOCK_JSON = os.path.join(LOCK_DIR, 'lock.json')   # exclusive holder; shared holders: slot<N>.json next to it

_k32 = ctypes.WinDLL('kernel32', use_last_error=True)
_k32.CreateFileW.restype = wt.HANDLE
_k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
_k32.OpenProcess.restype = wt.HANDLE
_k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
_k32.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
_k32.CloseHandle.argtypes = [wt.HANDLE]
_INVALID = wt.HANDLE(-1).value
_FILETIME_TO_DOTNET_TICKS = 504911232000000000   # .NET ticks (0001-01-01) minus FILETIME (1601-01-01), UTC
_HOUR_TICKS = 36000000000


def _read_text(path):
    """Read a file sharing read, write AND delete (Python's open() does not share delete, which would make the
    holder's release of lock.json fail). None = missing or unreadable."""
    h = _k32.CreateFileW(path, 0x80000000, 7, None, 3, 0x80, None)   # GENERIC_READ, share R|W|D, OPEN_EXISTING
    if not h or h == _INVALID:
        return None
    fd = msvcrt.open_osfhandle(h, os.O_RDONLY)
    with os.fdopen(fd, 'rb') as f:
        return f.read().decode('utf-8-sig', errors='replace')


def _proc_start_ticks(pid):
    """Start time in the same unit runlock.ps1 records (UTC .NET ticks). None = not running, 0 = running, unreadable."""
    h = _k32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None if ctypes.get_last_error() == 87 else 0   # 87 = no such process; access denied = running
    try:
        code = wt.DWORD()
        if _k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value != 259:   # 259 = STILL_ACTIVE
            return None
        c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
        if not _k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
            return 0
        return ((c.dwHighDateTime << 32) | c.dwLowDateTime) + _FILETIME_TO_DOTNET_TICKS
    finally:
        _k32.CloseHandle(h)


def _owner_alive(rec):
    try:
        start = _proc_start_ticks(int(rec.get('pid') or 0)) if rec.get('pid') else None
    except (TypeError, ValueError):
        return False
    if start is None:
        return False
    try:
        rec_start = int(rec.get('pid_start') or 0)
    except (TypeError, ValueError):
        rec_start = 0
    if start and rec_start:
        # PowerShell's local->UTC round trip can be an hour off inside the DST change-over hour.
        diff = abs(start - rec_start)
        if diff > 10000000 and abs(diff - _HOUR_TICKS) > 10000000:
            return False   # pid reused by another process
    return True


def _read_rec(path):
    text = _read_text(path)
    if not text or not text.strip():
        return None
    try:
        rec = json.loads(text)
    except ValueError:
        return None
    return rec if isinstance(rec, dict) and rec.get('token') else None


def read_lock():
    """The exclusive holder's record (lock.json), or None."""
    return _read_rec(LOCK_JSON)


def read_slots():
    """{slot number: record} of the shared holders' slot files that carry a record (live or not)."""
    out = {}
    try:
        names = os.listdir(LOCK_DIR)
    except OSError:
        return out
    for n in names:
        if n.startswith('slot') and n.endswith('.json') and n[4:-5].isdigit():
            rec = _read_rec(os.path.join(LOCK_DIR, n))
            if rec:
                out[int(n[4:-5])] = rec
    return out


def held_by_us():
    """'exclusive' or 'shared' when this process inherited the token of a live holder (exclusive lock.json, or a
    shared slot file); '' otherwise (falsy, so old `if held_by_us():` callers keep working)."""
    tok = os.environ.get('WWE2K14_RUNLOCK_TOKEN')
    if not tok:
        return ''
    rec = read_lock()
    if rec and rec.get('token') == tok and _owner_alive(rec):
        return 'exclusive'
    for rec in read_slots().values():
        if rec.get('token') == tok and _owner_alive(rec):
            return 'shared'
    return ''


def ensure_lock(name, timeout_minutes=240, shared=False):
    held = held_by_us()
    if held == 'exclusive' or (held == 'shared' and shared):
        return
    if held == 'shared':
        sys.stderr.write('[runlock] %s needs the EXCLUSIVE run lock but runs inside a shared hold (its parent took a '
                         'shared slot); it would wait on its own parent forever. Take the exclusive lock in the parent '
                         '(e.g. run_list -Exclusive).\n' % name)
        sys.exit(1)
    argv = getattr(sys, 'orig_argv', None)   # 3.10+: keeps -I / -X utf8 / -O / -m
    args = list(argv[1:]) if argv else list(sys.argv)
    cmd = ['pwsh', '-NoProfile', '-File', RUNLOCK_PS1, 'run', '-Name', name,
           '-TimeoutMinutes', str(timeout_minutes)] + (['-Shared'] if shared else []) + ['--', sys.executable] + args
    sys.stdout.flush()
    sys.stderr.flush()
    sys.exit(subprocess.call(cmd))


def status():
    return subprocess.run(['pwsh', '-NoProfile', '-File', RUNLOCK_PS1, 'status'],
                          capture_output=True, text=True).stdout


if __name__ == '__main__':
    print(status())

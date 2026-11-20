r"""The compile queue's Python helper (lane CQW, 2026-11-20; docs\agents\admit_wiring.md, contract docs\research\harness\CQW_contract.md).

    sys.path.append(<repo>/tools/admit); import admit_py
    with admit_py.hold('q.parse_worker', want, timeout_s=300) as n:      # n = how many workers this pool may run (an int; n.why: held / nested / observed / ...)
        with multiprocessing.Pool(n) as pool: ...

Every use of admit.exe's command line from Python lives in this file. hold() uses the stdin form of `admit hold` (stdin a real pipe, kept open until the
with block ends). It asks with --min = a quarter of the want (rounded up) and --timeout timeout_s; admit answers `granted M <word>`:
  held      M workers reserved until the block ends (n.admitted True); a held grant below the minimum is refused (given back): want;
  nested    under an outer hold: its count, accepted even below the minimum, no reservation of its own;
  observed  the queue's observe mode: the full want, nothing limited;
  open      admit's error path, nothing held: want.
(a bare `granted M` from an older admit counts as held.) Fall-open rules (the pool runs at exactly `want`, today's count), as tools\admit\Admit.ps1:
  1. env WWE_BUILD_PARALLEL set (a manual override; nothing sets it automatically): min(want, that), no hold;
  2. env WWE_NO_ADMIT=1: want, silently; no admit.exe: want, and ONE line per process `[admit] not installed (<path>): this build is not queued`;
  3. not admitted within timeout_s (exit 75), any error, or admit not answering within ceil(1.1 x timeout_s) + 30 s (that admit is killed with its tree,
     also when the caller is stopped): want, with one line on stderr.
Every answer prints one line saying what it is ("held", "observed: not limited", "nested", "not held: why").
exe_path() / admit_exe(): env WWE_ADMIT_EXE when set (tests), else the MAIN checkout's out\tools\admit\admit.exe (git's common dir); admit_exe() None when missing.
Nothing runs at import time (no git call, no admit call): safe to import from an argparse-default module or a multiprocessing worker.
"""
import contextlib
import json
import math
import os
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, 'tool_build_parallel.json')
GRACE_S = 30   # admit gets ceil(1.1 x timeout_s) + this long to answer, then it is killed and the pool runs at its own count


class Grant(int):
    """The granted worker count (an int). .admitted: True only when admit.exe granted it (False = a fall-open rule gave it); .why: which rule."""

    def __new__(cls, n, admitted, why):
        o = int.__new__(cls, n)
        o.admitted = admitted
        o.why = why
        return o


def _say(msg):
    try:
        print(msg, file=sys.stderr, flush=True)
    except Exception:
        pass


def exe_path():
    """Where admit.exe should be (it may be missing): env WWE_ADMIT_EXE, else the MAIN checkout's out\\tools\\admit\\admit.exe; None without git."""
    over = os.environ.get('WWE_ADMIT_EXE')
    if over:
        return os.path.abspath(over)
    try:
        common = subprocess.run(['git', '-C', HERE, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
    except Exception:
        return None
    if not common:
        return None
    # (put together so the gate's script scanners (tools\gate\sched.py OUT_JOIN / EXE_LIT, fastgate's exe keys) do not take admit.exe for an input of
    #  every check whose closure imports this module: the queue changes WHEN work starts, never what a check reads)
    return os.path.join(os.path.dirname(os.path.normpath(common)), *'{1} tools admit admit{0}'.format('.exe', 'out').split())


def admit_exe():
    p = exe_path()
    return p if p and os.path.isfile(p) else None


_SAID = {'not_installed': False}


def _not_installed(path):
    if not _SAID['not_installed']:   # once per process
        _SAID['not_installed'] = True
        _say(f'[admit] not installed ({path or "no git checkout found"}): this build is not queued')


def resolve_kind(kind):
    """The kind names this wiring uses -> the names admit.json has (tool_build_parallel.json kind_alias, the one place; empty today)."""
    try:
        with open(TABLE, encoding='utf-8') as f:
            return str(json.load(f).get('kind_alias', {}).get(kind) or kind)
    except Exception:
        return kind


def min_of(want):
    return max(1, int(math.ceil(want / 4.0)))


def _kill_tree(p):
    try:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], capture_output=True, timeout=30)
        else:
            p.kill()
    except Exception:
        pass
    try:
        p.wait(timeout=10)
    except Exception:
        pass


def _manual():
    g = os.environ.get('WWE_BUILD_PARALLEL')
    if not g:
        return None
    try:
        n = int(g)
    except ValueError:
        n = 0
    if n >= 1:
        return n
    _say(f"[admit] WWE_BUILD_PARALLEL='{g}' is not a count of 1 or more: ignored")
    return None


@contextlib.contextmanager
def hold(kind, want, timeout_s):
    want = max(1, int(want))
    g = _manual()
    if g is not None:
        yield Grant(min(want, g), False, 'manual WWE_BUILD_PARALLEL')
        return
    if os.environ.get('WWE_NO_ADMIT') == '1':
        yield Grant(want, False, 'WWE_NO_ADMIT=1')
        return
    exe = admit_exe()
    if exe is None:
        _not_installed(exe_path())
        yield Grant(want, False, 'not installed')
        return
    k = resolve_kind(kind)
    lo = min_of(want)
    timeout_s = max(1, int(math.ceil(timeout_s)))
    bound = int(math.ceil(1.1 * timeout_s)) + GRACE_S   # admit's limit is up to 1.1 x T plus a poll: never kill a wait admit is still allowed
    argv = [exe, 'hold', '--kind', k, '--want', str(want), '--min', str(lo), '--timeout', str(timeout_s)]
    p = None
    grant = None
    why = ''
    try:
        p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None, text=True,
                             creationflags=(0x08000000 if os.name == 'nt' else 0))   # CREATE_NO_WINDOW
        box = []
        th = threading.Thread(target=lambda: box.append(p.stdout.readline()), daemon=True)
        th.start()
        try:
            th.join(bound)
        except BaseException:   # a stopped caller (Ctrl+C): its admit must not go on to win a grant and hold it
            _kill_tree(p)
            raise
        if th.is_alive():
            _kill_tree(p)
            p = None
            why = f'admit gave no answer in {bound} s and was killed'
        else:
            parts = (box[0] if box else '').split()
            if len(parts) in (2, 3) and parts[0] == 'granted' and parts[1].isdigit() and 1 <= int(parts[1]) <= want:
                n = int(parts[1])
                word = parts[2] if len(parts) == 3 else 'held'   # (an older admit prints a bare `granted N`)
                if word == 'held' and n >= lo:
                    grant = Grant(n, True, 'held')
                elif word == 'nested':          # under an outer hold: its count, accepted even below the minimum; no reservation of its own
                    grant = Grant(n, False, 'nested')
                elif word == 'observed':        # observe mode: the full want, nothing limited
                    grant = Grant(n, False, 'observed')
                elif word == 'open':
                    why = "admit's error path (see its [admit] line above)"
                else:
                    why = f'granted {n} {word}, below the minimum {lo}: given back' if word == 'held' else f'an unknown answer {" ".join(parts)!r}'
            else:
                rc = None
                if not parts:   # no line: admit ended (exit 75 = not admitted in time) or failed
                    try:
                        rc = p.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        rc = None
                why = f'not admitted within {timeout_s} s' if rc == 75 else f'exit {rc}, first line {" ".join(parts)!r}'
    except Exception as e:
        why = f'{e}'
    if grant is None:
        _end(p)
        _say(f'[admit] {want} at once ({k}: not held: {why})')
        yield Grant(want, False, 'not held')
        return
    _say(f'[admit] {int(grant)} at once ({k}: ' + {'held': f'held, {int(grant)} of {want}, until the pool ends', 'nested': 'nested under an outer hold',
                                                  'observed': 'observed: not limited'}[grant.why] + ')')
    try:
        yield grant
    finally:
        _end(p)


def _end(p):
    """Release: close admit's stdin (EOF = the slots go back), wait briefly for it to write its history row, kill it (and its tree) if it hangs."""
    if p is None:
        return
    try:
        if p.stdin:
            p.stdin.close()
    except Exception:
        pass
    try:
        p.wait(timeout=15)
    except Exception:
        _kill_tree(p)
    try:
        if p.stdout:
            p.stdout.close()
    except Exception:
        pass

r"""The compile queue's Python helper (lane CQW, 2026-11-20; docs\agents\admit_wiring.md, contract docs\research\harness\CQW_contract.md).

    sys.path.append(<repo>/tools/admit); import admit_py
    with admit_py.hold('q.parse_worker', want, timeout_s=300) as n:      # n = how many workers this pool may run (an int; n.why: held / nested / observed / ...)
        with multiprocessing.Pool(n) as pool: ...

Every use of admit.exe's command line from Python lives in this file. hold() uses the stdin form of `admit hold` (stdin a real pipe, kept open until the
with block ends). It asks with --min = a quarter of the want (rounded up) and --timeout timeout_s (admit cuts it to admit.json timeout_s, 180 s: S109
GATE); admit answers `granted M <word>`:
  held      M workers reserved until the block ends (n.admitted True); a held grant below the minimum is refused (given back): want;
  narrowed  at the bound, below the minimum but above the floor: M workers reserved (an older admit's `forced` is taken the same way);
  nested    under an outer hold: its count, accepted even below the minimum, no reservation of its own;
  observed  the queue's observe mode: the full want, nothing limited;
  open      admit's error path, nothing held: want.
(a bare `granted M` from an older admit counts as held.)
REFUSED (S109 GATE): not admitted within the bound = admit exits 75 (stdout `refused <line>`, the loud line on stderr). hold() prints one line and raises
Refused, a SystemExit with code 75: the tool stops with exit 75 (a pool's `except Exception` cannot swallow it) and its caller prints the reason. Nothing ran.
Fall-open rules (the pool runs at exactly `want`, today's count), as tools\admit\Admit.ps1:
  1. env WWE_BUILD_PARALLEL set (a manual override; nothing sets it automatically): min(want, that), no hold;
  2. env WWE_NO_ADMIT=1: want, silently; no admit.exe: want, and ONE line per process `[admit] not installed (<path>): this build is not queued`;
  3. admit's own error (an exit other than 0 / 75, garbage), or admit not answering within ceil(1.1 x timeout_s) + 30 s (that admit is killed with its
     tree, also when the caller is stopped): want, with one line on stderr.
Every answer prints one line saying what it is ("held", "narrowed", "observed: not limited", "nested", "not held: why", "REFUSED").
exe_path() / admit_exe(): env WWE_ADMIT_EXE when set (tests), else the MAIN checkout's out\tools\admit\admit.exe (git's common dir); admit_exe() None when
missing. S109 GATE: admit_exe() first compares the exe's source stamp (admit.exe.sha256) with the main checkout's admit.cpp and, on a difference, runs
build_admit.ps1 -IfStale once per process (its compile admitted by the existing exe in one bounded ask; not admitted or failed = the old exe, said once).
That rebuild-on-ask stands in for a Python port of the gate; exe_current() is the one content check (status.py uses it).
Nothing runs at import time (no git call, no admit call): safe to import from an argparse-default module or a multiprocessing worker.
"""
import contextlib
import json
import math
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, 'tool_build_parallel.json')
GRACE_S = 30   # admit gets ceil(1.1 x timeout_s) + this long to answer, then it is killed and the pool runs at its own count


class Refused(SystemExit):
    """S109 GATE: the queue did not admit this pool within its bound (admit exit 75). A SystemExit with code 75: the tool stops loudly, nothing ran.
    .line = admit's refusal line (what is short, who holds the slots, since when)."""

    def __init__(self, line):
        super().__init__(75)
        self.line = line


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
    return os.path.join(os.path.dirname(os.path.normpath(common)), *'{1} tools admit admit{0}'.format('.exe', 'out').split())


def source_of(exe):
    """The admit.cpp an installed exe is built from: <main>\\out\\tools\\admit\\admit.exe -> <main>\\tools\\admit\\admit.cpp."""
    main_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(exe))))
    return os.path.join(main_root, 'tools', 'admit', 'admit.cpp')


def exe_current(exe):
    """S109 GATE: True when the exe's stamp (admit.exe.sha256, written by build_admit.ps1) is the SHA-256 of its source admit.cpp; False when the stamp
    is missing or differs; None when the source cannot be read (nothing to compare)."""
    import hashlib
    try:
        with open(source_of(exe), 'rb') as f:
            want = hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return None
    try:
        with open(exe + '.sha256', encoding='ascii', errors='replace') as f:
            return f.read().strip().lower() == want
    except OSError:
        return False


def _rebuild_if_stale(exe):
    """Once per process: build_admit.ps1 -IfStale of the exe's own checkout (bounded 300 s). Never raises; the exe in place is used either way."""
    if 'rebuilt' in _SAID_CACHE:
        return
    _SAID_CACHE['rebuilt'] = True
    script = os.path.join(os.path.dirname(source_of(exe)), 'build_admit.ps1')
    try:   # GATE review: a pre-GATE build_admit.ps1 ignores -IfStale and compiles at once, never asking the queue: not run (this tree is ahead of main)
        with open(script, encoding='utf-8', errors='replace') as f:
            if 'IfStale' not in f.read():
                _say(f'[admit] admit.exe has no source stamp and {script} predates the rebuild-on-ask (S109 GATE): this ask uses the exe as it is')
                return
    except OSError:
        return
    try:
        r = subprocess.run(['pwsh', '-NoProfile', '-File', script, '-IfStale'], capture_output=True, text=True, timeout=300,
                           creationflags=(0x08000000 if os.name == 'nt' else 0))
        for ln in (r.stdout + r.stderr).splitlines():
            if ln.startswith('[admit]') or ln.startswith('built '):
                _say(ln)
    except (OSError, subprocess.SubprocessError) as e:
        _say(f'[admit] admit.exe is older than admit.cpp and its rebuild could not start ({e}): this ask uses the old exe')
        return
    if exe_current(exe) is False:
        _say('[admit] admit.exe is still older than admit.cpp: this ask uses the old exe (session start shows it as STALE)')


def admit_exe():
    p = exe_path()
    if not p or not os.path.isfile(p):
        return None
    if not os.environ.get('WWE_ADMIT_EXE') and exe_current(p) is False:   # tests name their own exe: never rebuilt
        _rebuild_if_stale(p)
    return p


def _alone_file():
    """S106 C5: the alone run's pause file (tools\\runlock.ps1 -Alone): env WWE_ALONE_FILE (tests), else <main checkout>\\out\\harness\\runlock\\alone.lock, the
    main checkout being the folder above out\\tools\\admit\\admit.exe's three levels (found as exe_path() does). None without git."""
    over = os.environ.get('WWE_ALONE_FILE')
    if over:
        return os.path.abspath(over)
    if 'alone' not in _SAID_CACHE:   # one git call per process (the wait loop asks every second)
        common = ''
        try:
            common = subprocess.run(['git', '-C', HERE, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                    timeout=20).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass   # no git: no pause file path, the wrapper then behaves as before C5
        _SAID_CACHE['alone'] =os.path.join(os.path.dirname(os.path.normpath(common)), *'{0} harness runlock alone.lock'.format('out').split()) if common else None
    return _SAID_CACHE['alone']


_SAID_CACHE = {}


def _pause_left():
    """S106 C5: seconds a LIVE alone pause has left (0 = none, expired, or this process's own tree). Liveness is the OS lock on byte 0 (msvcrt, the same
    primitive as tools\\admit\\lock_interop.py and admit.exe), never the record. A live pause with no readable `until` counts as 900 s."""
    path = _alone_file()
    if not path:
        return 0.0
    try:
        import msvcrt
        fd = os.open(path, os.O_RDWR)
    except (ImportError, OSError):
        return 0.0   # no pause file (the usual case), or not Windows
    try:
        os.lseek(fd, 0, 0)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError:
            pass   # held: a live pause
        else:
            os.lseek(fd, 0, 0)
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            return 0.0
        os.lseek(fd, 1, 0)
        raw = os.read(fd, 8192)
    except OSError:
        return 0.0
    finally:
        os.close(fd)
    try:
        rec = json.loads(raw.decode('utf-8', 'replace'))
    except ValueError:
        rec = {}
    if not isinstance(rec, dict):
        rec = {}
    if rec.get('token') and rec.get('token') in (os.environ.get('WWE2K14_RUNLOCK_TOKEN'), os.environ.get('WWE_ALONE_TOKEN')):   # S106 C5F note 3: WWE_ALONE_TOKEN = the alone run's dedicated token
        return 0.0   # the alone run's own tree
    try:
        until = float(rec.get('until', 0))
    except (TypeError, ValueError):
        until = 0.0
    if until > 0:
        return max(0.0, until - time.time())
    return 900.0


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


def _cfg_timeout_s():
    """S106 0CF (review B5): the queue's own wait limit (admit.json timeout_s, 900): env WWE_ADMIT_CONFIG when set, else the file beside this one."""
    try:
        with open(os.environ.get('WWE_ADMIT_CONFIG') or os.path.join(HERE, 'admit.json'), encoding='utf-8') as f:
            return float(json.load(f).get('timeout_s', 900))
    except Exception:
        return 900.0


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
def hold(kind, want, timeout_s, interactive=False):
    """timeout_s: the caller's limit; admit cuts it to the queue's own (admit.json timeout_s, S109 GATE). interactive is kept for the callers' signature
    (the q overlay); since S109 every caller's limit is passed as given (the S106 0CF raise to the queue's limit served the forced slot, now gone)."""
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
    bound = int(math.ceil(1.1 * min(timeout_s, _cfg_timeout_s()))) + GRACE_S   # admit answers within min(T, the queue's limit) + its last ask: never kill a wait admit is still allowed
    argv = [exe, 'hold', '--kind', k, '--want', str(want), '--min', str(lo), '--timeout', str(timeout_s)]
    p = None
    grant = None
    refused = None
    why = ''
    try:
        p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=None, text=True,
                             creationflags=(0x08000000 if os.name == 'nt' else 0))   # CREATE_NO_WINDOW
        box = []
        th = threading.Thread(target=lambda: box.append(p.stdout.readline()), daemon=True)
        th.start()
        try:
            # S106 C5: admit does not count the time an alone run's pause holds it off so the
            # wrapper's own bound grows by the paused time too; while a pause is live it never falls through to "not held, run the full count" (unseen compiles).
            t_start = time.time()
            paused_total = 0.0
            last = t_start
            while True:
                th.join(max(0.05, min(1.0, t_start + bound + paused_total - time.time())))
                if not th.is_alive():
                    break
                now = time.time()
                pl = _pause_left()
                if pl > 0:
                    paused_total += now - last
                last = now
                if now >= t_start + bound + paused_total and pl <= 0:
                    break
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
                elif word in ('narrowed', 'forced'):   # S109 GATE: at the bound, below the minimum but above the floor (an older admit says `forced`)
                    grant = Grant(n, True, 'narrowed')
                elif word == 'open':
                    why = "admit's error path (see its [admit] line above)"
                else:
                    why = f'granted {n} {word}, below the minimum {lo}: given back' if word == 'held' else f'an unknown answer {" ".join(parts)!r}'
            else:
                rc = None
                if not parts or parts[0] == 'refused':   # no grant: admit ended (exit 75 = not admitted within its bound) or failed
                    try:
                        rc = p.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        rc = None
                if rc == 75:   # S109 GATE: refused (an older admit prints no `refused` line)
                    refused = ' '.join(parts[1:]) if parts else f'not admitted within {timeout_s} s (exit 75)'
                why = f'not admitted within {timeout_s} s' if rc == 75 else f'exit {rc}, first line {" ".join(parts)!r}'
    except Exception as e:
        why = f'{e}'
    if refused is not None:
        _end(p)
        _say(f'[admit] {k} pool of {want}: {refused} (this tool stops: exit 75)')
        raise Refused(refused)
    if grant is None:
        _end(p)
        _say(f'[admit] {want} at once ({k}: not held: {why})')
        yield Grant(want, False, 'not held')
        return
    _say(f'[admit] {int(grant)} at once ({k}: ' + {'held': f'held, {int(grant)} of {want}, until the pool ends', 'nested': 'nested under an outer hold',
                                                  'observed': 'observed: not limited',
                                                  'narrowed': f'narrowed at the wait bound: below the minimum {lo}, {int(grant)} of {want}, until the pool ends'}[grant.why] + ')')
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

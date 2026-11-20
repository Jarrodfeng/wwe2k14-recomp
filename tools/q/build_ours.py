"""Build / update the structural index of OUR code (out/harness/index/ours.db), queried by `q ours ...` (tools/q/qours.py).

    py tools/q/build_ours.py [--full] [-j N] [--no-tools] [--limit N] [--db PATH] [--root DIR]

What it does (S103 lane QO; docs/agents/q_ours.md):
  1. compile databases of both builds, configured only (never built, never in out/build): out/index/cc_on (default) and
     out/index/cc_off (-DWWE2K14_KEEP_STREAM=OFF -DWWE2K14_KEEP_STREAM_RUN=ON). Missing ones are configured here (~13 s each).
  2. one libclang parse per translation unit (tools/q/ours_clang.py): every src/*.cpp with the flags of the build(s) that compile it
     (ON, OFF); the src files in neither (launcher, fview, viewer ...) and every tools/**/*.cpp (units, rigs) with the ON flags.
  3. the per-unit rows are cached in the db by content hash of the unit and every file of ours it includes; an update re-parses only
     the units whose files changed, then re-merges (the merged tables are rebuilt every time, ~seconds).
Only OUR files are indexed (src, tools, probes): nothing from the game, nothing generated (rule 1). The index lives in out/ (scratch).
"""
import argparse
import hashlib
import json
import multiprocessing
import os
import pickle
import re
import shlex
import sqlite3
import glob
import shutil
import subprocess
import threading
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.append(os.path.join(os.path.dirname(HERE), 'admit'))
import admit_py as _admit  # noqa: E402   CQW: the machine-wide compile queue for the parse pool (nothing runs at import)

B_ON, B_OFF, B_TOOL, B_OTHER = 1, 2, 4, 8
BUILD_NAMES = {B_ON: 'ON', B_OFF: 'OFF', B_TOOL: 'tool', B_OTHER: 'other'}
EXTS = ('.h', '.hpp', '.cpp', '.inl', '.cc')
SCHEMA_VERSION = 3


def db_path(root):
    return os.path.join(root, 'out', 'harness', 'index', 'ours.db')


def rel_norm(p, root):
    n = os.path.abspath(p).replace('\\', '/')
    r = os.path.abspath(root).replace('\\', '/').rstrip('/') + '/'
    return n[len(r):] if n.lower().startswith(r.lower()) else None


def canon_result(r, lc):
    """map every file name of a unit's rows to the file's real case (clang reports an include as it was written)"""
    if 'error' in r:
        return r
    c = lambda p: lc.get(p.lower(), p) if p else p
    r['incs'] = sorted({c(x) for x in r['incs']})
    r['syms'] = [(s[0], s[1], s[2], s[3], c(s[4]), s[5], s[6]) for s in r['syms']]
    r['refs'] = list({(x[0], x[1], c(x[2]), x[3], x[4], x[5]) for x in r['refs']})
    r['funcs'] = {u: (c(v[0]), v[1], v[2]) for u, v in r['funcs'].items()}
    return r


def ensure_cc(root, name, extra):
    d = os.path.join(root, 'out', 'index', name)
    cc = os.path.join(d, 'compile_commands.json')
    if os.path.exists(cc):
        return cc
    os.makedirs(os.path.dirname(d), exist_ok=True)
    cmd = (f'. tools/env.ps1 *> $null; cmake -S . -B out/index/{name} -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang '
           f'-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON {extra}')
    print(f'build_ours: configuring {name} (configure only, ~13 s)')
    r = subprocess.run(['pwsh', '-NoProfile', '-Command', cmd], cwd=root, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(cc):
        raise SystemExit(f'build_ours: cmake configure of {name} failed:\n{r.stdout[-1500:]}\n{r.stderr[-800:]}')
    return cc


def load_cc(path, root):
    """-> {rel src file: cleaned libclang args}"""
    import ours_clang
    out = {}
    with open(path, encoding='utf-8') as f:
        for e in json.load(f):
            rel = rel_norm(e['file'], root)
            if not rel or not rel.startswith(('src/', 'probes/')):
                continue
            args = e['arguments'] if 'arguments' in e else shlex.split(e['command'])
            out[rel] = ours_clang.clean_args(args)
    return out


def sha_file(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        h.update(f.read())
    return h.hexdigest()


def list_files(root):
    res = {}
    for top in ('src', 'tools', 'probes'):
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules')]
            for f in fn:
                if f.endswith(EXTS):
                    p = os.path.join(dp, f)
                    res[rel_norm(p, root)] = p
    return res


def worker(job):
    key, root, rel, args = job
    import ours_clang
    try:
        r = ours_clang.parse_tu(root, os.path.join(root, rel), args)
    except Exception as e:   # a walker bug in one unit must not lose the others
        import traceback
        r = {'error': f'{e!r}\n{traceback.format_exc()[-600:]}', 'secs': 0}
    return key, r


def merge(rows_by_tu, files_sha, con, meta):
    """rows_by_tu: {(rel, build): (build_bit, result)}; rebuilds the merged tables."""
    syms, refs, funcs = {}, {}, {}
    cur = con.cursor()
    cur.executescript('''DROP TABLE IF EXISTS tus; DROP TABLE IF EXISTS incs; DROP TABLE IF EXISTS syms; DROP TABLE IF EXISTS refs;
        DROP TABLE IF EXISTS funcs; DROP TABLE IF EXISTS files;
        CREATE TABLE tus(id INTEGER PRIMARY KEY, file TEXT, build INTEGER, errors INTEGER, first_err TEXT, secs REAL, cursors INTEGER);
        CREATE TABLE incs(tu INTEGER, file TEXT);
        CREATE TABLE files(path TEXT PRIMARY KEY, sha TEXT, size INTEGER, mtime_ns INTEGER);
        CREATE TABLE syms(usr TEXT PRIMARY KEY, qname TEXT, name TEXT, kind INTEGER, file TEXT, line INTEGER, isdef INTEGER, builds INTEGER);
        CREATE TABLE refs(usr TEXT, kind TEXT, file TEXT, line INTEGER, col INTEGER, func TEXT, builds INTEGER);
        CREATE TABLE funcs(usr TEXT PRIMARY KEY, file TEXT, line INTEGER, endline INTEGER, builds INTEGER, cvars TEXT, logs TEXT);''')
    tid = 0
    for (rel, bkey), (bit, r) in sorted(rows_by_tu.items()):
        tid += 1
        cur.execute('INSERT INTO tus VALUES(?,?,?,?,?,?,?)', (tid, rel, bit, r.get('errors', -1), json.dumps(r.get('first_err') or r.get('error') or ''),
                                                             r.get('secs', 0), r.get('cursors', 0)))
        if 'error' in r:
            continue
        cur.executemany('INSERT INTO incs VALUES(?,?)', [(tid, f) for f in r['incs']])
        for s in r['syms']:
            old = syms.get(s[0])
            # builds = the units that saw the DEFINITION (or, with no definition seen, any declaration)
            if old is None:
                syms[s[0]] = [s[1], s[2], s[3], s[4], s[5], s[6], bit if s[6] else 0, bit]
            else:
                if s[6] and not old[5]:
                    old[3], old[4], old[5] = s[4], s[5], 1
                if s[6]:
                    old[6] |= bit
                old[7] |= bit
        for x in r['refs']:
            refs[x] = refs.get(x, 0) | bit
        for usr, (f, a, b) in r['funcs'].items():
            o = funcs.get(usr)
            ex = r['extra'].get(usr, ([], []))
            if o is None:
                funcs[usr] = [f, a, b, bit, ex[0], ex[1]]
            else:
                o[3] |= bit
    cur.executemany('INSERT INTO syms VALUES(?,?,?,?,?,?,?,?)',
                    [(u, v[0], v[1], v[2], v[3], v[4], v[5], v[6] or v[7]) for u, v in syms.items()])
    cur.executemany('INSERT INTO refs VALUES(?,?,?,?,?,?,?)', [(k[0], k[1], k[2], k[3], k[4], k[5], m) for k, m in refs.items()])
    cur.executemany('INSERT INTO funcs VALUES(?,?,?,?,?,?,?)',
                    [(u, v[0], v[1], v[2], v[3], json.dumps(v[4]), json.dumps(v[5])) for u, v in funcs.items()])
    cur.executemany('INSERT INTO files VALUES(?,?,?,?)', files_sha)
    cur.executescript('''CREATE INDEX refs_usr ON refs(usr); CREATE INDEX refs_func ON refs(func); CREATE INDEX refs_file ON refs(file);
        CREATE INDEX syms_name ON syms(name); CREATE INDEX syms_q ON syms(qname); CREATE INDEX syms_file ON syms(file);
        CREATE INDEX incs_file ON incs(file);''')
    cur.execute('DROP TABLE IF EXISTS meta')
    cur.execute('CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)')
    cur.executemany('INSERT INTO meta VALUES(?,?)', [(k, json.dumps(v)) for k, v in meta.items()])
    con.commit()
    return {'syms': len(syms), 'refs': len(refs), 'funcs': len(funcs), 'tus': tid}


def plan_units(root, files, no_tools=False, cc_root=None):
    """-> [(rel, build bit, libclang args)]: every unit of the index. cc_root = the checkout whose compile databases (out/index/cc_*)
    are read (default root; the branch overlay passes the MAIN checkout and rewrites the paths itself)."""
    cc_root = cc_root or root
    cc_on = load_cc(ensure_cc(cc_root, 'cc_on', ''), cc_root)
    cc_off = load_cc(ensure_cc(cc_root, 'cc_off', '-DWWE2K14_KEEP_STREAM=OFF -DWWE2K14_KEEP_STREAM_RUN=ON'), cc_root)
    base = list(cc_on.get('src/main.cpp') or next(iter(cc_on.values())))
    for i in range(len(base) - 2, -1, -1):          # units that are not in the exe are not compiled with the exe's precompiled header
        if base[i] == '-include' and base[i + 1].replace('\\', '/').endswith('cmake_pch.hxx'):
            del base[i:i + 2]
    units = []      # (rel, bit, args)
    for rel, args in cc_on.items():
        units.append((rel, B_ON if rel.startswith('src/') else B_OTHER, args))
    for rel, args in cc_off.items():
        if rel.startswith('src/'):
            units.append((rel, B_OFF, args))
    for rel in files:
        if rel.endswith(('.cpp', '.cc')):
            if rel.startswith('src/') and rel not in cc_on:
                units.append((rel, B_OTHER, base))
            elif rel.startswith('tools/') and not no_tools:
                units.append((rel, B_TOOL, base))
    return units


CANCEL = threading.Event()   # QTF review C2: set by qtimeout.run_with_deadline when the refresh ran over its budget; checked before every commit, the merge and the swap


class Cancelled(Exception):
    pass


def check_cancel():
    if CANCEL.is_set():
        raise Cancelled()


def heal_db(path, wait=30):
    """QTF review C2: a killed in-place writer leaves a HOT journal (path + '-journal'); a read-only open then fails with 'attempt to write a readonly
    database'. One read-write open rolls it back (BEGIN IMMEDIATE + a read does the recovery, ROLLBACK keeps nothing). -> True when a journal was there."""
    jp = path + '-journal'
    if not os.path.exists(jp):
        return False
    try:
        c = sqlite3.connect(path, timeout=wait, isolation_level=None)
        try:
            c.execute('BEGIN IMMEDIATE')
            c.execute('SELECT count(*) FROM sqlite_master').fetchall()
            c.execute('ROLLBACK')
        finally:
            c.close()
        return True
    except sqlite3.Error:
        return False


class SwapTimeout(Exception):
    pass


def seed_cache(con, seed):
    """QGL review: copy the tu_cache rows of a killed refresh's leftover db (its parse progress) into this build's db. Rows are validated on use (flags + dep hashes), so a stale
    one is simply re-parsed. Never raises: a damaged leftover seeds nothing. -> rows copied."""
    try:
        con.execute('ATTACH DATABASE ? AS seed', (seed,))
        try:
            n = con.execute('SELECT count(*) FROM seed.tu_cache').fetchone()[0]
            con.execute('INSERT OR REPLACE INTO main.tu_cache SELECT key, flags, deps, rows FROM seed.tu_cache')
            con.commit()
            return n
        finally:
            con.execute('DETACH DATABASE seed')
    except sqlite3.Error as e:
        print(f'build_ours: could not seed from {seed} ({e})', file=sys.stderr)
        try:
            con.rollback()
        except sqlite3.Error:
            pass
        return 0


def _bg_timer(final, secs):
    """QGL review: a --bg refresh stops itself after `secs`: it writes the back-off marker `<db>.bgbackoff` (qours.bg_refresh honours it for an hour, so stale readers do not
    restart an over-long parse every 90 s), kills its parse pool and exits 124. The parse progress stays in the leftover ours.db.tmp* (the next run seeds tu_cache from it)."""
    def fire():
        time.sleep(secs)
        try:
            with open(final + '.bgbackoff', 'w') as f:
                f.write('timed out after %d s at %s\n' % (secs, time.strftime('%Y-%m-%d %H:%M')))
            sys.stderr.write('build_ours --bg: TIMEOUT after %d s: backed off for an hour; the parse progress is kept for the next run\n' % secs)
            sys.stderr.flush()
            import qtimeout
            qtimeout.kill_children()
        finally:
            os._exit(124)
    threading.Thread(target=fire, name='bg-timer', daemon=True).start()


def snapshot_good(final, retry_s=30):
    """QGL: after a swap, keep `<final>.good` = a whole copy of the index just swapped in (copied to <final>.tmpgood<pid> so the leftover sweep (`<final>.tmp*`) cleans a crashed
    copy, then os.replace: never half-written). A reader that meets the NEXT swap's write lock answers from it instead of waiting (qours.open_ro). os.replace is refused while
    a reader has `.good` open: retried for retry_s seconds; a failure is logged (readers then wait as before), never silent."""
    tmp = final + '.tmpgood%d' % os.getpid()
    try:
        shutil.copyfile(final, tmp)
    except OSError as e:
        print(f'build_ours: could not copy the last-good snapshot ({e})', file=sys.stderr)
        return False
    t_end = time.time() + retry_s
    while True:
        try:
            os.replace(tmp, final + '.good')
            return True
        except OSError as e:
            if time.time() >= t_end:
                print(f'build_ours: could not replace {final}.good within {retry_s} s ({e}); readers wait for swaps until the next refresh', file=sys.stderr)
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
                return False
            time.sleep(0.5)


def swap_in(tmp, final, wait_s=120):
    """QTF review round 3 (#3): replace the live index's CONTENT with the finished copy through the SQLite backup API (one write transaction on `final`):
    readers see the old index or the new one, never a mix, and wait on their busy timeout like any SQLite client, so a stream of readers cannot starve
    the swap the way an os.replace (refused while ANY reader has the file open) could. A kill mid-swap leaves a hot journal that the next reader rolls back
    (heal_db). -> True when swapped, False when the writer lock was not obtained within wait_s (the old index is untouched)."""
    check_cancel()
    t_end = time.time() + wait_s

    def progress(status, remaining, total):
        check_cancel()
        if time.time() > t_end:
            raise SwapTimeout()
    src = sqlite3.connect(tmp)
    dst = sqlite3.connect(final, timeout=wait_s)
    try:
        src.backup(dst, pages=-1, progress=progress, sleep=0.1)
        return True
    except SwapTimeout:
        return False
    except sqlite3.OperationalError as e:
        if 'lock' in str(e).lower() or 'busy' in str(e).lower():
            return False
        raise
    finally:
        for c in (src, dst):
            try:
                c.close()
            except sqlite3.Error:
                pass


class BuildLock:
    """QTF review round 3 (#6): an exclusive lock around the WHOLE ours build (<db>.lock, msvcrt.locking / flock): a second builder waits (checking the
    cancel flag) up to wait_s and then gives up; no two builders copy, parse and swap at once, so no last-swap-wins. The OS drops it when the process dies."""

    def __init__(self, path, wait_s=900):
        self.path, self.wait_s, self.fh = path + '.lock', wait_s, None

    def acquire(self):
        self.fh = open(self.path, 'a+b')
        if os.fstat(self.fh.fileno()).st_size == 0:
            self.fh.write(b'x')
            self.fh.flush()
        t_end = time.time() + self.wait_s
        announced = False
        while True:
            check_cancel()
            try:
                self.fh.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(self.fh.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self.fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                return True
            except OSError:
                if time.time() >= t_end:
                    self.fh.close()
                    self.fh = None
                    return False
                if not announced:
                    print('build_ours: another index build holds the lock; waiting for it (then this one only has the delta to do)', file=sys.stderr)
                    announced = True
                time.sleep(0.5)

    def release(self):
        if self.fh is not None:
            try:
                self.fh.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(self.fh.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self.fh.fileno(), fcntl.LOCK_UN)
            except OSError:
                pass
            self.fh.close()
            self.fh = None


def main(argv=None):
    """QTF review C2: the whole build works on a COPY (ours.db.tmpPID) and swaps it in (SQLite backup API, one write transaction) when it is complete and
    VACUUMed. Readers always see a whole index; a kill / timeout / cancel before the swap loses only the copy (never a hot journal on the live db, never an
    empty index). One build at a time (BuildLock).   py tools/q/build_ours.py --selftest"""
    if (argv if argv is not None else sys.argv[1:])[:1] == ['--selftest']:
        return selftest()
    state = {}
    try:
        import qtimeout
        qtimeout.mark_owner()
        return _main(argv, state)
    except Cancelled:
        print('build_ours: cancelled (over its time budget): the live index is untouched', file=sys.stderr)
        return 3
    finally:
        lk = state.get('lock')
        if lk is not None:
            lk.release()
        c = state.get('con')
        if c is not None:
            try:
                c.close()
            except sqlite3.Error:
                pass
        t = state.get('tmp')
        if t:
            for suffix in ('', '-journal'):
                try:
                    os.unlink(t + suffix)
                except OSError:
                    pass


def _free_ram_gb():
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                        ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                        ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys / 2**30
    except Exception:
        return None


def _default_jobs():
    cpu = max(2, (os.cpu_count() or 8) - 4)
    free = _free_ram_gb()
    by_ram = 8 if free is None else int((free - 6) // 2.5)
    return max(1, min(cpu, 8, by_ram))


def _main(argv, state):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--full', action='store_true', help='re-parse every unit (ignore the cache)')
    # S103 23:4x: each libclang worker holds ~1.7 GB; the old default (cpu_count - 4 = 28) committed 48 GB after a post-merge refresh and
    # starved an Android build (clang "Allocation failed"). Workers are capped by FREE RAM (2.5 GB each, 6 GB left over) and at 8.
    ap.add_argument('-j', type=int, default=_default_jobs())
    ap.add_argument('--no-tools', action='store_true', help='src units only (skip tools/**/*.cpp)')
    ap.add_argument('--limit', type=int, default=0, help='parse at most N units (smoke tests)')
    ap.add_argument('--db', default=None)
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--bg', action='store_true', help='QGL: a background refresh started by a q reader: gives up at once when another build holds the lock, and stops itself after 25 minutes')
    ap.add_argument('--only', default=None, help='regex: only units whose path matches (smoke tests)')
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    import qours   # QTF (S103 WTA fix 1): never build an index inside a worktree (a private 200 MB copy, then 10+ minute re-indexes): borrow the main one. --db does not lift it.
    msg = qours.refuse_update_in_worktree(root)
    if msg:
        print(msg, file=sys.stderr)
        return 2
    final = a.db or db_path(root)
    os.makedirs(os.path.dirname(final), exist_ok=True)
    lock = BuildLock(final, wait_s=0 if a.bg else 900)
    state['lock'] = lock
    if a.bg:
        _bg_timer(final, 1500)
    if not lock.acquire():
        print(f'build_ours: another index build holds {final}.lock{"" if a.bg else f" (waited {lock.wait_s} s)"}: skipped (its result is the index)', file=sys.stderr)
        state['lock'] = None
        return 1
    heal_db(final)
    now_t = time.time()
    # leftovers of killed builds (we hold the build lock: none is a live build). The NEWEST non-snapshot one keeps the parse progress of a refresh that was killed
    # (QGL review: a 25-minute refresh that timed out lost everything and stale readers restarted it): its tu_cache seeds this build; the others go. Older than 6 h: gone.
    left = []
    for old in glob.glob(final + '.tmp*'):
        try:
            age = now_t - os.path.getmtime(old)
            if old.endswith('-journal'):
                continue
            if '.tmpgood' in old:
                if age > 3600:
                    os.unlink(old)
            elif age > 21600:
                for x in (old, old + '-journal'):
                    if os.path.exists(x):
                        os.unlink(x)
            else:
                left.append((os.path.getmtime(old), old))
        except OSError:
            pass
    left.sort(reverse=True)
    seed = left[0][1] if left else None
    for _, old in left[1:]:
        for x in (old, old + '-journal'):
            try:
                os.unlink(x)
            except OSError:
                pass
    state['seed'] = seed
    dbp = final + '.tmp%d' % os.getpid()          # all writes go to this copy
    state['tmp'] = dbp                            # set BEFORE the copy: a kill / cancel during it still cleans the half-copied file
    if os.path.exists(final):
        shutil.copyfile(final, dbp)
    t0 = time.time()
    files = list_files(root)
    units = plan_units(root, files, a.no_tools)
    if a.only:
        units = [u for u in units if re.search(a.only, u[0])]
    if a.limit:
        units = units[:a.limit]

    con = sqlite3.connect(dbp, timeout=120)   # S103: concurrent refreshes (post-merge hook + an auto-refresh) wait, not fail
    state["con"] = con
    con.execute('CREATE TABLE IF NOT EXISTS tu_cache(key TEXT PRIMARY KEY, flags TEXT, deps BLOB, rows BLOB)')
    if seed:
        seeded = seed_cache(con, seed)
        if seeded:
            print(f'build_ours: seeded {seeded} parsed units from the killed refresh {os.path.basename(seed)}')
    cache = {}
    if not a.full:
        for k, fl, deps, rows in con.execute('SELECT key, flags, deps, rows FROM tu_cache'):
            cache[k] = (fl, pickle.loads(zlib.decompress(deps)), rows)
    shas = {}

    def sha(rel):
        if rel not in shas:
            try:
                shas[rel] = sha_file(files[rel]) if rel in files else None
            except OSError:
                shas[rel] = None
        return shas[rel]

    lc = {k.lower(): k for k in files}
    rows_by_tu, todo = {}, []
    for rel, bit, args in units:
        key = f'{rel}|{bit}'
        flags = hashlib.sha1(json.dumps(args).encode()).hexdigest()
        c = cache.get(key)
        if c and c[0] == flags and all(sha(lc.get(d.lower(), d)) == h for d, h in c[1].items()):
            rows_by_tu[(rel, str(bit))] = (bit, c[2])      # the raw blob: decoded only when a merge is needed
        else:
            todo.append((key, root, rel, args, bit, flags))
    print(f'build_ours: {len(units)} units, {len(units) - len(todo)} cached, {len(todo)} to parse (-j {a.j})')
    done = 0
    if todo:
        order = sorted(todo, key=lambda t: -os.path.getsize(os.path.join(root, t[2])))   # big units first
        with _admit.hold('q.parse_worker', a.j, timeout_s=300) as nj, multiprocessing.Pool(nj) as pool:   # CQW: the machine-wide compile queue (a.j = the want and the fall-open value)
            for key, r in pool.imap_unordered(worker, [(t[0], t[1], t[2], t[3]) for t in order]):
                done += 1
                t = next(x for x in todo if x[0] == key)
                rel, bit, flags = t[2], t[4], t[5]
                r = canon_result(r, lc)
                rows_by_tu[(rel, str(bit))] = (bit, r)
                if 'error' not in r:
                    deps = {d: sha(d) for d in set(r['incs']) | {rel}}
                    con.execute('INSERT OR REPLACE INTO tu_cache VALUES(?,?,?,?)',
                                (key, flags, zlib.compress(pickle.dumps(deps)), zlib.compress(pickle.dumps(r))))
                check_cancel()
                if done % 25 == 0 or done == len(todo):
                    con.commit()
                    print(f'  parsed {done}/{len(todo)}  ({time.time() - t0:.0f} s)')
                if 'error' in r:
                    print(f'  ERROR {rel}: {r["error"][:200]}')
    live_keys = {f'{rel}|{bit}' for rel, bit, _ in units}
    if not a.limit and not a.only:
        for (k,) in con.execute('SELECT key FROM tu_cache').fetchall():
            if k not in live_keys:
                con.execute('DELETE FROM tu_cache WHERE key = ?', (k,))
    con.commit()
    fsha = []
    for rel, p in sorted(files.items()):
        st = os.stat(p)
        fsha.append((rel, sha(rel) or '', st.st_size, st.st_mtime_ns))
    try:
        old = {k: json.loads(v) for k, v in con.execute('SELECT key, value FROM meta')}
        old_files = {r[0]: (r[1], r[2], r[3]) for r in con.execute('SELECT path, sha, size, mtime_ns FROM files')}
    except sqlite3.Error:
        old, old_files = {}, {}
    if (not todo and not a.full and not a.limit and not a.only and old.get('schema') == SCHEMA_VERSION and old.get('units') == len(units)
            and old_files == {r[0]: (r[1], r[2], r[3]) for r in fsha}):
        print(f'build_ours: up to date ({len(units)} units cached, nothing changed) in {time.time() - t0:.1f} s')
        con.close()
        try:
            fresh = os.path.getmtime(final + '.good') >= os.path.getmtime(final)
        except OSError:
            fresh = False
        if not fresh:      # QGL: the first run of this build (or one after a failed / older snapshot) gives readers their fallback copy even when nothing changed
            snapshot_good(final)
        return 0
    for k, (bit, v) in list(rows_by_tu.items()):
        if isinstance(v, (bytes, bytearray)):
            rows_by_tu[k] = (bit, canon_result(pickle.loads(zlib.decompress(v)), lc))
    meta = {'schema': SCHEMA_VERSION, 'built': time.strftime('%Y-%m-%d %H:%M'), 'root': root, 'units': len(units),
            'reparsed': len(todo), 'cc_on': os.path.join(root, 'out', 'index', 'cc_on', 'compile_commands.json'),
            'cc_off': os.path.join(root, 'out', 'index', 'cc_off', 'compile_commands.json'),
            'cc_on_mtime': os.stat(os.path.join(root, 'out', 'index', 'cc_on', 'compile_commands.json')).st_mtime_ns}
    check_cancel()
    stats = merge(rows_by_tu, fsha, con, meta)
    check_cancel()
    con.execute('VACUUM')
    con.close()
    state['con'] = None
    if not swap_in(dbp, final):
        print(f'build_ours: could not swap in the new index (a reader held {final} for 120 s); the old index is untouched: rerun later', file=sys.stderr)
        return 1
    sd = state.get('seed')
    if sd:
        for x in (sd, sd + '-journal'):
            try:
                os.unlink(x)
            except OSError:
                pass
    snapshot_good(final)
    print(f'build_ours: done in {time.time() - t0:.0f} s: {stats}  db {os.path.getsize(final) / 1e6:.1f} MB  {final}')
    return 0


def selftest():
    import tempfile
    d = tempfile.mkdtemp(prefix='build_ours_selftest_')
    final, tmp = os.path.join(d, 'x.db'), os.path.join(d, 'x.db.tmp1')

    def mk(path, n):
        c = sqlite3.connect(path)
        c.execute('CREATE TABLE syms(a INTEGER)')
        c.executemany('INSERT INTO syms VALUES(?)', [(i,) for i in range(n)])
        c.commit()
        c.close()
    mk(final, 100)
    mk(tmp, 5000)
    # (1) swap_in under a STREAM of short readers (os.replace would have been refused the whole time): completes, readers see old or new, never a mix / empty
    stop = threading.Event()
    seen, errs = set(), []

    def reader():
        while not stop.is_set():
            try:
                c = sqlite3.connect('file:%s?mode=ro' % final.replace('\\', '/'), uri=True, timeout=30)
                seen.add(c.execute('SELECT count(*) FROM syms').fetchone()[0])
                c.close()
            except sqlite3.Error as e:
                errs.append(repr(e))
    ts = [threading.Thread(target=reader) for _ in range(4)]
    for t in ts:
        t.start()
    time.sleep(0.5)
    t0 = time.time()
    ok = swap_in(tmp, final, wait_s=30)
    dt = time.time() - t0
    time.sleep(0.3)
    stop.set()
    for t in ts:
        t.join()
    assert ok and dt < 20, (ok, dt)
    assert seen <= {100, 5000}, f'a reader saw a mixed / empty index: {seen}'
    c = sqlite3.connect(final)
    assert c.execute('SELECT count(*) FROM syms').fetchone()[0] == 5000
    c.close()
    assert not errs or all('locked' in e for e in errs), errs
    # (1b) a writer lock that never frees: swap_in gives up at wait_s and leaves the old index whole
    hold = sqlite3.connect(final, isolation_level=None)
    hold.execute('BEGIN EXCLUSIVE')
    mk(tmp + 'b', 7)
    t0 = time.time()
    assert swap_in(tmp + 'b', final, wait_s=2) is False
    assert 1.5 < time.time() - t0 < 10
    hold.execute('ROLLBACK')
    hold.close()
    c = sqlite3.connect(final)
    assert c.execute('SELECT count(*) FROM syms').fetchone()[0] == 5000
    c.close()
    # (2) the build lock: a second process cannot take it while the first holds it; it can once the first lets go
    lk = BuildLock(final, wait_s=1)
    assert lk.acquire()
    code = ('import sys; sys.path.insert(0, %r); import build_ours as b\nl = b.BuildLock(%r, wait_s=2)\nprint("got" if l.acquire() else "refused")' % (HERE, final))
    r = subprocess.run([sys.executable, '-c', code], capture_output=True, text=True, timeout=60)
    assert r.stdout.strip() == 'refused', (r.stdout, r.stderr)
    lk.release()
    r = subprocess.run([sys.executable, '-c', code], capture_output=True, text=True, timeout=60)
    assert r.stdout.strip() == 'got', (r.stdout, r.stderr)
    print(f'build_ours selftest OK: swap under 4 reader threads took {dt:.1f} s, readers only ever saw {sorted(seen)}; a held writer lock times out and leaves the old index; '
          'the build lock refuses a second holder and frees on release')
    return 0


if __name__ == '__main__':
    multiprocessing.freeze_support()
    sys.exit(main())

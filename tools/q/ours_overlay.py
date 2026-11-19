"""Branch overlay for `q ours` (S103 lane QB): query a LANE BRANCH's code exactly, in seconds, without a cold index in the worktree.

    py tools/q/q.py ours --branch REF CMD ...        (REF = a branch / commit; its files are read with git, nothing is checked out)
    py tools/q/q.py ours --worktree PATH CMD ...     (a worktree's files, uncommitted edits included)
    py tools/q/q.py ours --branch REF diff           changed symbols, their callers / readers / writers in BASE and BRANCH, ON / OFF flags

How it works (docs/agents/q_ours.md):
  * BASE = the MAIN checkout's out/harness/index/ours.db (never written here).
  * the branch's OWN changes are the files git says differ between its merge base with the main checkout's HEAD and its tip (a worktree: its working
    tree, untracked files too); a changed file equal to the base's content (CRLF-insensitive) is dropped. Everything else is taken as the base has it.
  * only the translation units those files touch are re-parsed (the unit's own file, or any unit whose `incs` list a changed file, from the base index),
    with the MAIN checkout's compile databases (out/index/cc_on, cc_off) and a clang VFS overlay (-ivfsoverlay) that shows the branch's content of the
    changed files at the main checkout's own paths: no tree is copied, nothing is checked out. New .cpp files are parsed with the ON flags.
  * the re-parsed units go into a small overlay db  out/harness/index/overlay/<key>.db  (key = the branch tip; a worktree: tip + hash of its
    changed files), validated on open (schema, key, tip, base fingerprint, base HEAD, complete) and rebuilt when any of them differs; old overlays are pruned.
  * queries run on a connection where syms / refs / funcs / tus / incs / files are views / temp tables over base + overlay (rows of the changed files
    and of the re-parsed units' own files come from the overlay, everything else from the base); every row carries `origin` = base | branch
    (a re-parsed row identical to a base row is tagged base).
Known limits (said in every answer's header): CMake changes (a new file added to a build list, new flags) are not re-read: a new .cpp is
parsed as ON only; a row of an UNCHANGED header seen by a re-parsed unit keeps the base's build mask; when the main checkout moved on since the
base was built, the base is stale for those files (the header says how many: refresh with q ours update in the main checkout).
Rule 1: only our sources (src, tools, probes) are read; the temp tree of a --branch run is deleted at once."""
import hashlib
import json
import multiprocessing
import os
import pickle
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_ours as bo  # noqa: E402

SCHEMA = 4
ID_OFFSET = 1 << 24
KEEP_EXT = bo.EXTS + ('.inc', '.hxx', '.def')
KEEP_OVERLAYS = 6
MAX_AGE_DAYS = 7


class OverlayError(Exception):
    pass


class OverlayTooBig(OverlayError):
    """a worktree overlay would re-parse too many changed files (QTF): the caller answers from the borrowed base index instead"""


MAX_WT_FILES = int(os.environ.get('WWE_Q_OVERLAY_MAX', '40') or 40)


def worktree_key(wt, tip, changes):
    """QTF: the cache key of a worktree overlay = tip + the hash of every changed file's working-tree bytes (no staging, no parsing needed)."""
    h = hashlib.sha1()
    for st, p in changes:
        h.update(f'{st}|{p}|'.encode())
        try:
            with open(os.path.join(wt, p), 'rb') as fh:
                h.update(sha_raw(norm_bytes(fh.read())).encode())
        except OSError:
            h.update(b'-')
    return f'{tip[:12]}-wt-{h.hexdigest()[:10]}'


def git(repo, *a, timeout=180):
    r = subprocess.run(['git', '-C', repo] + list(a), capture_output=True, text=True, errors='replace', timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def git_out(repo, *a):
    rc, out, err = git(repo, *a)
    if rc != 0:
        raise OverlayError(f'git {" ".join(a)} failed in {repo}: {err.strip()[:300]}')
    return out.strip()


def main_checkout(root):
    """the MAIN checkout (the one whose out\\ holds the base index) from any checkout / worktree of the repo."""
    rc, out, _ = git(root, 'rev-parse', '--git-common-dir')
    if rc != 0 or not out.strip():
        return os.path.abspath(root)
    cd = os.path.abspath(os.path.join(root, out.strip()))
    return os.path.dirname(cd) if os.path.basename(cd) == '.git' else os.path.abspath(root)


def overlay_dir(main_root):
    return os.path.join(main_root, 'out', 'harness', 'index', 'overlay')


def fingerprint(base_db):
    st = os.stat(base_db)
    con = sqlite3.connect(f'{Path(base_db).as_uri()}?mode=ro', uri=True)
    try:
        built = con.execute("SELECT value FROM meta WHERE key = 'built'").fetchone()
        sch = con.execute("SELECT value FROM meta WHERE key = 'schema'").fetchone()
    finally:
        con.close()
    return f'{built[0] if built else "?"}|{sch[0] if sch else "?"}|{st.st_size}|{st.st_mtime_ns}'


def norm_bytes(b):
    return b.replace(b'\r\n', b'\n')


def sha_raw(b):
    return hashlib.sha1(b).hexdigest()


# ---------------------------------------------------------------- trees

def branch_changes(main_root, base_head, tip, worktree=None):
    """-> (merge base, [(status A/M/D, path)]) of the files our indexer knows that the branch changed vs its merge base with the main checkout's HEAD
    (a worktree: its working tree, uncommitted and untracked files included)."""
    mb = git_out(main_root, 'merge-base', base_head, tip)
    if worktree:
        raw = git_out(worktree, 'diff', '--name-status', '--no-renames', mb, '--', 'src', 'tools', 'probes')
        raw += '\n' + '\n'.join('A\t' + x for x in git_out(worktree, 'ls-files', '--others', '--exclude-standard', '--', 'src', 'tools', 'probes').split('\n') if x)
    else:
        raw = git_out(main_root, 'diff', '--name-status', '--no-renames', mb, tip, '--', 'src', 'tools', 'probes')
    out = []
    for ln in raw.split('\n'):
        parts = ln.split('\t')
        if len(parts) == 2 and parts[1].endswith(KEEP_EXT) and parts[0][:1] in 'AMD':
            out.append((parts[0][:1], parts[1]))
    return mb, sorted(set(out), key=lambda x: x[1])


def cat_blobs(repo, names):
    """{name: bytes or None} for `rev:path` names through ONE git cat-file --batch"""
    out = {}
    if not names:
        return out
    pr = subprocess.run(['git', '-C', repo, 'cat-file', '--batch'], input=''.join(n + '\n' for n in names).encode(), capture_output=True)
    buf, pos = pr.stdout, 0
    for n in names:
        nl = buf.index(b'\n', pos)
        head = buf[pos:nl].decode().split()
        if len(head) == 2 and head[1] == 'missing':
            out[n] = None
            pos = nl + 1
            continue
        if len(head) != 3 or head[1] != 'blob':
            raise OverlayError(f'git cat-file {n}: {head}')
        size = int(head[2])
        out[n] = buf[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1
    return out


def stage_changes(main_root, dest, changes, tip, mb, worktree=None):
    """the branch's changed / added files, as they would be once MERGED into the main checkout, written under dest -> ({rel: bytes}, info).
    A modified file the main checkout has not touched since the merge base is the branch's version; one main HAS touched since is a 3-way merge
    (git merge-file: main's current text + the branch's edits; a hunk both sides changed takes both sides' lines (git merge-file --union) and is counted as a conflict), so
    the overlay differs from the base index in the branch's own hunks only. Nothing is checked out; the main checkout is never written."""
    info = {'merged': [], 'conflicts': 0}
    modified = [p for st, p in changes if st == 'M']
    got = cat_blobs(main_root, ([] if worktree else [f'{tip}:{p}' for st, p in changes if st in 'AM']) + [f'{mb}:{p}' for p in modified])
    blobs = {}
    for st, p in changes:
        if st not in 'AM':
            continue
        if worktree:
            try:
                with open(os.path.join(worktree, p), 'rb') as f:
                    mine = f.read()
            except OSError:
                continue
        else:
            mine = got.get(f'{tip}:{p}')
            if mine is None:
                continue
        if st == 'A':
            blobs[p] = mine
            continue
        base_c = got.get(f'{mb}:{p}')
        try:
            with open(os.path.join(main_root, p), 'rb') as f:
                main_c = f.read()
        except OSError:
            main_c = None
        if main_c is None or base_c is None or norm_bytes(main_c) == norm_bytes(base_c) or norm_bytes(main_c) == norm_bytes(mine):
            blobs[p] = mine
            continue
        md = os.path.join(dest, '_merge')
        os.makedirs(md, exist_ok=True)
        paths = []
        for nm, data in (('cur', main_c), ('base', base_c), ('other', mine)):
            fp = os.path.join(md, nm + '.txt')
            with open(fp, 'wb') as f:
                f.write(norm_bytes(data))
            paths.append(fp)
        r = subprocess.run(['git', 'merge-file', '-p'] + paths, capture_output=True)
        if r.returncode > 0 and r.returncode < 128:      # hunks both sides changed (two appends at one spot are the usual case): keep both sides' lines
            r2 = subprocess.run(['git', 'merge-file', '-p', '--union'] + paths, capture_output=True)
            if r2.returncode == 0 or r2.returncode < 128:
                r = r2
                info['conflicts'] += 1
                info.setdefault('conflict_files', []).append(p)
        if 0 <= r.returncode < 128:
            blobs[p] = r.stdout
            info['merged'].append(p)
        else:
            blobs[p] = mine
    for p, data in blobs.items():
        dst = os.path.join(dest, p)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)
    return blobs, info


def vfs_file(main_root, dest, blobs):
    """a clang VFS overlay (-ivfsoverlay): the main checkout's path of every changed / added file -> the branch's content under dest."""
    m = os.path.abspath(main_root).replace('\\', '/').rstrip('/')
    roots = [{'name': f'{m}/{p}', 'type': 'file', 'external-contents': os.path.abspath(os.path.join(dest, p)).replace('\\', '/')} for p in sorted(blobs)]
    path = os.path.join(dest, 'vfs.json')
    with open(path, 'w') as f:
        json.dump({'version': 0, 'case-sensitive': 'false', 'use-external-names': 'false', 'roots': roots}, f)
    return path.replace('\\', '/')


def avail_gb():
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('l', ctypes.c_ulong), ('load', ctypes.c_ulong), ('tp', ctypes.c_ulonglong), ('ap', ctypes.c_ulonglong), ('tv', ctypes.c_ulonglong),
                        ('av', ctypes.c_ulonglong), ('te', ctypes.c_ulonglong), ('ae', ctypes.c_ulonglong), ('x', ctypes.c_ulonglong)]
        m = MS()
        m.l = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ap / 2**30
    except Exception:
        return 16.0


def ovl_worker(job):
    key, root, rel, args, overrides = job
    import ours_clang
    try:
        r = ours_clang.parse_tu(root, os.path.join(root, rel), args, overrides)
    except Exception as e:
        import traceback
        r = {'error': f'{e!r}\n{traceback.format_exc()[-600:]}', 'secs': 0}
    return key, r


def classify(base_con, changes, blobs, main_root):
    """-> (changed, added, removed): the branch's OWN changes (git, vs its merge base) kept only where they really differ from what the base index holds.
    A file equal to the base after CRLF normalisation is the same, but only when the main checkout's copy still IS the file the base was built from."""
    have = {r[0]: r[1] for r in base_con.execute('SELECT path, sha FROM files')}
    changed, added, removed = [], [], []
    for st, p in changes:
        if st == 'D':
            if p in have:
                removed.append(p)
            continue
        data = blobs.get(p)
        if data is None:
            continue
        if p not in have:
            added.append(p)
        elif have[p] != sha_raw(data):
            same = False
            try:
                with open(os.path.join(main_root, p), 'rb') as f:
                    mb = f.read()
                same = sha_raw(mb) == have[p] and norm_bytes(mb) == norm_bytes(data)
            except OSError:
                pass
            if not same:
                changed.append(p)
    return sorted(changed), sorted(added), sorted(removed)


# ---------------------------------------------------------------- build

def body_hash(lines, a, b):
    seg = lines[max(a - 1, 0):min(b, len(lines))]
    t = '\n'.join(x.rstrip() for x in seg if x.strip())
    return hashlib.sha1(t.encode('utf-8', 'replace')).hexdigest()


def merge_rows(rows_by_tu):
    syms, refs, funcs, tus, incs = {}, {}, {}, [], []
    tid = ID_OFFSET
    for (rel, bkey), (bit, r) in sorted(rows_by_tu.items()):
        tid += 1
        tus.append((tid, rel, bit, r.get('errors', -1), json.dumps(r.get('first_err') or r.get('error') or ''), r.get('secs', 0), r.get('cursors', 0)))
        if 'error' in r:
            continue
        incs += [(tid, f) for f in r['incs']]
        for s in r['syms']:
            old = syms.get(s[0])
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
    return syms, refs, funcs, tus, incs


def build_overlay(path, *, main_root, base_db, vfs, overrides, blobs, changed, added, removed, meta, jobs, log):
    t0 = time.time()
    hit = set(changed) | set(added) | set(removed)
    base = sqlite3.connect(f'{Path(base_db).as_uri()}?mode=ro', uri=True)
    q = ','.join('?' * len(hit)) if hit else "''"
    params = list(hit) * 2
    aff = {}   # (file, build) -> base tu id
    if hit:
        for tid, f, b in base.execute(f'SELECT DISTINCT t.id, t.file, t.build FROM tus t LEFT JOIN incs i ON i.tu = t.id '
                                      f'WHERE t.file IN ({q}) OR i.file IN ({q})', params):
            aff[(f, b)] = tid
    files = bo.list_files(main_root)                  # the main checkout's files, then the branch's changes over them
    for p in removed:
        files.pop(p, None)
    for p in list(changed) + list(added):
        files[p] = overrides[p]
    plan = bo.plan_units(main_root, files, False, cc_root=main_root)
    added_set = set(added)
    want = {(f, b) for (f, b) in aff} | {(rel, bit) for rel, bit, _ in plan if rel in added_set}
    units = []
    for rel, bit, args in plan:
        if (rel, bit) in want and rel in files:
            units.append((rel, bit, list(args) + ['-ivfsoverlay', vfs]))
    log(f'overlay: {len(hit)} changed files ({len(changed)} changed, {len(added)} added, {len(removed)} removed): '
        f'{len(aff)} base units affected, {len(units)} to re-parse (-j {jobs})')
    lc = {k.lower(): k for k in files}
    rows_by_tu = {}
    work = [(f'{rel}|{bit}', main_root, rel, args, overrides) for rel, bit, args in sorted(units, key=lambda u: -os.path.getsize(files[u[0]]))]
    jobs = max(1, min(jobs, 16, int(avail_gb() / 1.6)))    # a big unit takes ~1-3 GB: LLVM aborts with "out of memory" when too many run at once
    results = []
    if len(work) <= 2 or jobs <= 1:
        results = [ovl_worker(j) for j in work]
    elif work:
        log(f'overlay: parsing with {jobs} workers')
        with multiprocessing.Pool(min(jobs, len(work))) as pool:
            results = list(pool.imap_unordered(ovl_worker, work))
    res = dict(results)
    retry = [j for j in work if 'error' in res[j[0]]]
    if retry:   # a worker that ran out of memory / crashed: once more, one at a time
        log(f'overlay: {len(retry)} units failed in the pool, retrying them one at a time')
        res.update(dict(ovl_worker(j) for j in retry))
    results = list(res.items())
    by_key = {f'{rel}|{bit}': (rel, bit) for rel, bit, _ in units}
    for key, r in results:
        rel, bit = by_key[key]
        rows_by_tu[(rel, str(bit))] = (bit, bo.canon_result(r, lc))
        if 'error' in r:
            log(f'  ERROR {rel}: {r["error"][:200]}')
    t_parse = time.time() - t0
    syms, refs, funcs, tus, incs = merge_rows(rows_by_tu)
    cx = hit | {rel for rel, _ in by_key.values()}
    tmp = path + '.tmp'
    if os.path.exists(tmp):
        os.remove(tmp)
    con = sqlite3.connect(tmp, uri=True)
    con.executescript('''
        CREATE TABLE r_syms(usr TEXT PRIMARY KEY, qname TEXT, name TEXT, kind INTEGER, file TEXT, line INTEGER, isdef INTEGER, builds INTEGER);
        CREATE TABLE r_refs(usr TEXT, kind TEXT, file TEXT, line INTEGER, col INTEGER, func TEXT, builds INTEGER);
        CREATE TABLE r_funcs(usr TEXT PRIMARY KEY, file TEXT, line INTEGER, endline INTEGER, builds INTEGER, cvars TEXT, logs TEXT);
        CREATE TABLE tus(id INTEGER PRIMARY KEY, file TEXT, build INTEGER, errors INTEGER, first_err TEXT, secs REAL, cursors INTEGER);
        CREATE TABLE incs(tu INTEGER, file TEXT);
        CREATE TABLE aff(id INTEGER PRIMARY KEY);
        CREATE TABLE cx(path TEXT PRIMARY KEY);
        CREATE TABLE files(path TEXT PRIMARY KEY, sha TEXT, size INTEGER, mtime_ns INTEGER);
        CREATE TABLE rm_files(path TEXT PRIMARY KEY);
        CREATE TABLE changed(path TEXT PRIMARY KEY, status TEXT);
        CREATE TABLE body(usr TEXT PRIMARY KEY, hash TEXT);
        CREATE TABLE ovmeta(key TEXT PRIMARY KEY, value TEXT);''')
    con.executemany('INSERT INTO r_syms VALUES(?,?,?,?,?,?,?,?)', [(u, v[0], v[1], v[2], v[3], v[4], v[5], v[6] or v[7]) for u, v in syms.items()])
    con.executemany('INSERT INTO r_refs VALUES(?,?,?,?,?,?,?)', [(k[0], k[1], k[2], k[3], k[4], k[5], m) for k, m in refs.items()])
    con.executemany('INSERT INTO r_funcs VALUES(?,?,?,?,?,?,?)', [(u, v[0], v[1], v[2], v[3], json.dumps(v[4]), json.dumps(v[5])) for u, v in funcs.items()])
    con.executemany('INSERT INTO tus VALUES(?,?,?,?,?,?,?)', tus)
    con.executemany('INSERT INTO incs VALUES(?,?)', incs)
    con.executemany('INSERT INTO aff VALUES(?)', [(i,) for i in aff.values()])
    con.executemany('INSERT INTO cx VALUES(?)', [(p,) for p in cx])
    con.executemany('INSERT INTO files VALUES(?,?,?,?)', [(rel, sha_raw(blobs[rel]), len(blobs[rel]), 0) for rel in list(changed) + list(added)])
    con.executemany('INSERT INTO rm_files VALUES(?)', [(p,) for p in removed])
    con.executemany('INSERT INTO changed VALUES(?,?)', [(p, 'changed') for p in changed] + [(p, 'added') for p in added] + [(p, 'removed') for p in removed])
    lines_cache = {}
    bodies = []
    for u, v in funcs.items():
        f = v[0]
        if f not in blobs:            # only the functions of the branch's own files are compared (cmd_diff)
            continue
        if f not in lines_cache:
            lines_cache[f] = norm_bytes(blobs[f]).decode('utf-8', 'replace').split('\n')
        bodies.append((u, body_hash(lines_cache[f], v[1], v[2])))
    con.executemany('INSERT INTO body VALUES(?,?)', bodies)
    con.commit()
    con.execute(f"ATTACH DATABASE '{Path(base_db).as_uri()}?mode=ro' AS base")
    con.executescript('''
        CREATE TABLE gone AS SELECT usr FROM base.syms WHERE file IN (SELECT path FROM cx) AND usr NOT IN (SELECT usr FROM r_syms);
        CREATE TABLE syms AS SELECT o.* FROM r_syms o LEFT JOIN base.syms b ON b.usr = o.usr
            WHERE b.usr IS NULL OR o.file IN (SELECT path FROM cx) OR b.file IN (SELECT path FROM cx) OR (o.isdef = 1 AND b.isdef = 0);
        CREATE TABLE refs AS SELECT o.* FROM r_refs o WHERE o.file IN (SELECT path FROM cx) OR o.usr NOT IN (SELECT usr FROM base.syms);
        CREATE TABLE funcs AS SELECT o.* FROM r_funcs o WHERE o.file IN (SELECT path FROM cx) OR o.usr NOT IN (SELECT usr FROM base.funcs);
        DROP TABLE r_syms; DROP TABLE r_refs; DROP TABLE r_funcs;
        CREATE INDEX o_refs_usr ON refs(usr); CREATE INDEX o_refs_func ON refs(func); CREATE INDEX o_syms_usr ON syms(usr);
        CREATE INDEX o_funcs_usr ON funcs(usr); CREATE INDEX o_incs_tu ON incs(tu);''')
    # a re-parsed row that is IDENTICAL to a base row (a changed header makes every unit that includes it re-parse) is tagged base, not branch
    for tbl, cols in (('refs', 'usr, kind, file, line, col, func'), ('syms', 'usr, file, line'), ('funcs', 'usr, file, line, endline')):
        con.execute(f"ALTER TABLE {tbl} ADD COLUMN tag TEXT DEFAULT 'branch'")
        basekeys = set(tuple(r) for r in con.execute(f'SELECT {cols} FROM base.{tbl} WHERE file IN (SELECT path FROM cx)'))
        ids = [(r[0],) for r in con.execute(f'SELECT rowid, {cols} FROM {tbl}') if tuple(r[1:]) in basekeys]
        con.executemany(f"UPDATE {tbl} SET tag = 'base' WHERE rowid = ?", ids)
    con.commit()
    meta = dict(meta, schema=SCHEMA, built=time.strftime('%Y-%m-%d %H:%M:%S'), units=len(units), base_units=len(aff),
                secs_parse=round(t_parse, 1), changed=len(changed), added=len(added), removed=len(removed),
                n_syms=con.execute('SELECT COUNT(*) FROM syms').fetchone()[0], n_refs=con.execute('SELECT COUNT(*) FROM refs').fetchone()[0],
                parse_errors=sum(1 for t in tus if t[3] != 0), failed_units=sum(1 for t in tus if t[3] == -1),
                incomplete=any(t[3] == -1 for t in tus))     # a unit that crashed / ran out of memory: the overlay is used once, never reused
    meta['secs_total'] = round(time.time() - t0, 1)
    con.executemany('INSERT INTO ovmeta VALUES(?,?)', [(k, json.dumps(v)) for k, v in meta.items()])
    con.commit()
    con.execute('DETACH DATABASE base')
    con.close()
    base.close()
    os.replace(tmp, path)
    return meta


def read_meta(path):
    try:
        con = sqlite3.connect(f'{Path(path).as_uri()}?mode=ro', uri=True)
        try:
            return {k: json.loads(v) for k, v in con.execute('SELECT key, value FROM ovmeta')}
        finally:
            con.close()
    except sqlite3.Error:
        return {}


def valid(path, want):
    """an overlay is reused only when its recorded schema / key / tip / base fingerprint / base HEAD / mode all equal what is wanted now and it is complete"""
    if not os.path.exists(path):
        return False
    m = read_meta(path)
    return all(m.get(k) == v for k, v in want.items()) and not m.get('incomplete')


def prune(odir, keep_path):
    try:
        dbs = sorted((p for p in Path(odir).glob('*.db')), key=lambda p: p.stat().st_mtime, reverse=True)
    except OSError:
        return
    now = time.time()
    for i, p in enumerate(dbs):
        if str(p) == str(keep_path):
            continue
        if i >= KEEP_OVERLAYS or now - p.stat().st_mtime > MAX_AGE_DAYS * 86400:
            try:
                p.unlink()
            except OSError:
                pass
    for p in Path(odir).glob('*.tmp'):
        if now - p.stat().st_mtime > 3600:
            try:
                p.unlink()
            except OSError:
                pass


def get_overlay(root, branch=None, worktree=None, base_db=None, jobs=None, log=None):
    """-> (connection with the merged views, info). Builds (or rebuilds when stale) the overlay of `branch` / `worktree` as needed."""
    log = log or (lambda s: print(s, file=sys.stderr))
    jobs = jobs or max(2, (os.cpu_count() or 8) - 4)
    t0 = time.time()
    main_root = main_checkout(worktree or root)
    base_db = base_db or bo.db_path(main_root)
    if not os.path.exists(base_db):
        raise OverlayError(f'no base index at {base_db}: build it in the main checkout (py tools\\q\\build_ours.py)')
    bo.heal_db(base_db)    # QTF review C2: roll back a hot journal a killed writer left (read-only opens would crash on it)
    base_head = git_out(main_root, 'rev-parse', 'HEAD')
    odir = overlay_dir(main_root)
    os.makedirs(odir, exist_ok=True)
    fp = fingerprint(base_db)
    status, tmp_tree = 'reused', None
    try:
        if worktree:
            wt = os.path.abspath(worktree)
            tip = git_out(wt, 'rev-parse', 'HEAD')
            mode, label = 'worktree', wt
        else:
            tip = git_out(main_root, 'rev-parse', '--verify', f'{branch}^{{commit}}')
            mode, label = 'branch', branch
        want = {'schema': SCHEMA, 'tip': tip, 'base_fp': fp, 'base_head': base_head, 'mode': mode}
        mb = changes = None
        if mode == 'branch':
            key = tip[:16]
            want['key'] = key
            path = os.path.join(odir, key + '.db')
            ok = valid(path, want)
            status = 'reused' if ok else ('rebuilt (stale: tip / base changed)' if os.path.exists(path) else 'built')
        wt_changes = None
        if mode == 'worktree':
            # QTF: look the cache up BEFORE staging / parsing: key = tip + hashes of the dirty files (as --branch keys by tip)
            mb, wt_changes = branch_changes(main_root, base_head, tip, wt)
            key = worktree_key(wt, tip, wt_changes)
            want['key'] = key
            path = os.path.join(odir, key + '.db')
            if valid(path, want):
                status = 'reused'
            else:
                status = 'rebuilt (stale)' if os.path.exists(path) else 'built'
                if len(wt_changes) > MAX_WT_FILES:
                    raise OverlayTooBig(f'{len(wt_changes)} changed files (> {MAX_WT_FILES}, WWE_Q_OVERLAY_MAX): a worktree overlay would re-parse too many units')
        if status != 'reused':
            t1 = time.time()
            if mode == 'worktree':
                changes = wt_changes
            else:
                mb, changes = branch_changes(main_root, base_head, tip, None)
            tmp_tree = tempfile.mkdtemp(prefix='tree_', dir=odir)
            blobs, merge_info = stage_changes(main_root, tmp_tree, changes, tip, mb, wt if mode == 'worktree' else None)
            base = sqlite3.connect(f'{Path(base_db).as_uri()}?mode=ro', uri=True)
            base.row_factory = sqlite3.Row
            try:
                changed, added, removed = classify(base, changes, blobs, main_root)
                if True:   # how stale the base is for the main checkout (not the branch's doing): the answer says so
                    import qours
                    stl = qours.staleness(base, main_root)
                    base_stale = len(stl['changed']) + len(stl['added']) + len(stl['removed'])
            finally:
                base.close()
            log(f'overlay: {label} ({tip[:10]}): {len(changes)} files changed by the branch vs merge base {mb[:10]} ({len(changed)} differ from the base index, '
                f'{len(added)} new, {len(removed)} removed; {len(merge_info["merged"])} 3-way merged with newer main edits), read in {time.time() - t1:.1f} s')
        if status != 'reused':
            gch = sorted({p for _, p in changes})
            meta = dict(want, branch=label, merge_base=mb, git_changed=gch, base_built=fp.split('|')[0], base_stale_files=base_stale, merged_files=merge_info['merged'], merge_conflicts=merge_info['conflicts'])
            overrides = {p: os.path.join(tmp_tree, p) for p in blobs}
            m = build_overlay(path, main_root=main_root, base_db=base_db, vfs=vfs_file(main_root, tmp_tree, blobs), overrides=overrides, blobs=blobs, changed=changed, added=added,
                              removed=removed, meta=meta, jobs=jobs, log=log)
            log(f'overlay: {status} in {time.time() - t0:.1f} s ({m["units"]} units re-parsed in {m["secs_parse"]} s, {m["n_syms"]} symbols, {m["n_refs"]} refs)')
            prune(odir, path)
    finally:
        if tmp_tree:
            shutil.rmtree(tmp_tree, ignore_errors=True)
    con = open_views(base_db, path)
    meta = read_meta(path)
    return con, {'status': status, 'path': path, 'meta': meta, 'main_root': main_root, 'secs': round(time.time() - t0, 1), 'base_db': base_db}


def open_views(base_db, ov_path):
    con = sqlite3.connect(':memory:', uri=True, check_same_thread=False)   # QTF: built in a deadline thread (qtimeout.run_with_deadline)
    con.row_factory = sqlite3.Row
    con.execute(f"ATTACH DATABASE '{Path(base_db).as_uri()}?mode=ro' AS base")
    con.execute(f"ATTACH DATABASE '{Path(ov_path).as_uri()}?mode=ro' AS ov")
    con.executescript('''
        CREATE TEMP TABLE syms AS
          SELECT usr, qname, name, kind, file, line, isdef, builds, 'base' AS origin FROM base.syms
            WHERE usr NOT IN (SELECT usr FROM ov.syms) AND usr NOT IN (SELECT usr FROM ov.gone)
          UNION ALL SELECT usr, qname, name, kind, file, line, isdef, builds, tag FROM ov.syms;
        CREATE TEMP VIEW refs AS
          SELECT usr, kind, file, line, col, func, builds, 'base' AS origin FROM base.refs
            WHERE file NOT IN (SELECT path FROM ov.cx) AND usr NOT IN (SELECT usr FROM ov.gone)
          UNION ALL SELECT usr, kind, file, line, col, func, builds, tag FROM ov.refs;
        CREATE TEMP TABLE funcs AS
          SELECT usr, file, line, endline, builds, cvars, logs, 'base' AS origin FROM base.funcs
            WHERE file NOT IN (SELECT path FROM ov.cx) AND usr NOT IN (SELECT usr FROM ov.gone) AND usr NOT IN (SELECT usr FROM ov.funcs)
          UNION ALL SELECT usr, file, line, endline, builds, cvars, logs, tag FROM ov.funcs;
        CREATE TEMP VIEW tus AS
          SELECT id, file, build, errors, first_err, secs, cursors FROM base.tus WHERE id NOT IN (SELECT id FROM ov.aff)
          UNION ALL SELECT id, file, build, errors, first_err, secs, cursors FROM ov.tus;
        CREATE TEMP VIEW incs AS
          SELECT tu, file FROM base.incs WHERE tu NOT IN (SELECT id FROM ov.aff)
          UNION ALL SELECT tu, file FROM ov.incs;
        CREATE TEMP VIEW files AS
          SELECT path, sha, size, mtime_ns FROM base.files WHERE path NOT IN (SELECT path FROM ov.files) AND path NOT IN (SELECT path FROM ov.rm_files)
          UNION ALL SELECT path, sha, size, mtime_ns FROM ov.files;
        CREATE TEMP VIEW meta AS SELECT key, value FROM base.meta;
        CREATE INDEX temp.t_syms_usr ON syms(usr); CREATE INDEX temp.t_syms_name ON syms(name); CREATE INDEX temp.t_syms_q ON syms(qname);
        CREATE INDEX temp.t_syms_file ON syms(file); CREATE INDEX temp.t_funcs_usr ON funcs(usr);''')
    return con


# ---------------------------------------------------------------- diff

ON, OFF = 1, 2
FUNC_K = (8, 21, 24, 25, 26, 30)
VAR_K = (9, 6, 7)


def on_off(m):
    import qours
    return qours.bstr(m)


def access_of(con, usr, file):
    """globals / fields a function reads and writes: {gusr: [qname, mode set, builds, line]}; only refs in the function's own file
    (a USR shared by several units' static helpers / `main` would otherwise merge their bodies)."""
    out = {}
    for r in con.execute("SELECT y.usr, y.qname, y.builds AS sb, r.kind, r.line, r.file, r.builds FROM refs r JOIN syms y ON y.usr = r.usr WHERE r.func = ? AND r.file = ? "
                         "AND y.kind IN (9, 6) AND r.kind IN ('read','write','rw','addr')", (usr, file)):
        e = out.setdefault(r['usr'], [r['qname'], set(), 0, r['line'], r['file'], r['sb']])
        e[1].add('write' if r['kind'] in ('write', 'rw') else ('addr' if r['kind'] == 'addr' else 'read'))
        if r['kind'] == 'rw':
            e[1].add('read')
        e[2] |= r['builds']
    return out


def site_groups(con, usr, kinds):
    q = ','.join('?' * len(kinds))
    rows = con.execute(f'SELECT * FROM refs WHERE usr = ? AND kind IN ({q}) ORDER BY file, line', (usr, *kinds)).fetchall()
    import qours
    g = {}
    for e in qours.sites_grouped(con, rows):
        g[(e['func'], e['file'])] = e
    return g


def compare_sites(old, new, what, sym_builds):
    """base groups vs branch groups -> flat list of {where, builds_base, builds_branch, change, flag}"""
    out = []
    for k in sorted(set(old) | set(new)):
        a, b = old.get(k), new.get(k)
        ma, mb = (a['builds'] if a else None), (b['builds'] if b else None)
        ref = b or a
        row = {'what': what, 'func': k[0], 'file': k[1], 'lines': ref['lines'][:6], 'kinds': ref['kinds'],
               'base': on_off(ma) if a else None, 'branch': on_off(mb) if b else None, 'builds_base': ma, 'builds_branch': mb}
        row['change'] = 'new in branch' if not a else ('gone in branch' if not b else ('builds changed' if (ma & 3) != (mb & 3) else ''))
        cur = mb if b else ma
        # a cross-build FLAG: the symbol is compiled in BOTH builds but this site exists in only one of them
        row['flag'] = f'{what} exists in {'ON' if cur & 1 else 'OFF'} only' if ((cur & 3) in (1, 2) and (sym_builds & 3) == 3) else ''
        out.append(row)
    return out


def file_lines(path, cache):
    if path not in cache:
        try:
            with open(path, 'rb') as f:
                cache[path] = norm_bytes(f.read()).decode('utf-8', 'replace').split('\n')
        except OSError:
            cache[path] = None
    return cache[path]


def cmd_diff(base_con, br_con, info, limit=40):
    """the "what does this change touch" answer. base_con = the base index alone, br_con = base + overlay views."""
    import qours
    meta = info['meta']
    gch = set(meta.get('git_changed', []))
    main_root = info['main_root']
    ov = sqlite3.connect(f'{Path(info["path"]).as_uri()}?mode=ro', uri=True)
    ov.row_factory = sqlite3.Row
    body_new = {r['usr']: r['hash'] for r in ov.execute('SELECT usr, hash FROM body')}
    ov.close()
    ph = ','.join('?' * len(gch)) if gch else "''"
    gl = sorted(gch)

    def defs(con, kinds):
        k = ','.join('?' * len(kinds))
        return {r['usr']: r for r in con.execute(f'SELECT y.usr, y.qname, y.kind, y.file, y.line, y.builds, f.endline, f.file AS ffile, f.line AS fline FROM syms y '
                                                 f'LEFT JOIN funcs f ON f.usr = y.usr WHERE y.kind IN ({k}) AND y.isdef = 1 AND y.file IN ({ph})', (*kinds, *gl))}
    a_f, b_f = defs(base_con, FUNC_K), defs(br_con, FUNC_K)
    a_v, b_v = defs(base_con, VAR_K), defs(br_con, VAR_K)
    cache, warn = {}, []
    base_files = {r[0]: r[1] for r in base_con.execute(f'SELECT path, sha FROM files WHERE path IN ({ph})', gl)}
    for p, sha in base_files.items():
        try:
            with open(os.path.join(main_root, p), 'rb') as f:
                if sha_raw(f.read()) != sha:
                    warn.append(p)
        except OSError:
            warn.append(p)

    def base_body(r):
        L = file_lines(os.path.join(main_root, r['ffile']), cache)
        return body_hash(L, r['fline'], r['endline']) if L else None
    entries = []
    names_b = {}
    for u, r in b_f.items():
        names_b.setdefault(r['qname'], []).append(u)
    names_a = {}
    for u, r in a_f.items():
        names_a.setdefault(r['qname'], []).append(u)

    def func_entry(change, ua, ub):
        ra, rb = a_f.get(ua) if ua else None, b_f.get(ub) if ub else None
        r = rb or ra
        e = {'kind': 'function', 'change': change, 'qname': r['qname'], 'file': r['file'], 'line': r['line'],
             'builds_base': on_off(ra['builds']) if ra else None, 'builds_branch': on_off(rb['builds']) if rb else None}
        ca = site_groups(base_con, ua, ('call', 'ref')) if ua else {}
        cb = site_groups(br_con, ub, ('call', 'ref')) if ub else {}
        e['callers'] = compare_sites(ca, cb, 'caller', (rb or ra)['builds'])
        ga = access_of(base_con, ua, ra['ffile'] or ra['file']) if ua else {}
        gb = access_of(br_con, ub, rb['ffile'] or rb['file']) if ub else {}
        acc = []
        for g in sorted(set(ga) | set(gb), key=lambda x: (gb.get(x) or ga.get(x))[0]):
            a, b = ga.get(g), gb.get(g)
            ma, mb = (a[1] if a else set()), (b[1] if b else set())
            if ma == mb:
                continue
            ref = b or a
            added = sorted(mb - ma)
            dropped = sorted(ma - mb)
            builds = (b[2] if b else a[2])
            acc.append({'global': ref[0], 'usr': g, 'new_access': added, 'dropped_access': dropped, 'file': ref[4], 'line': ref[3],
                        'builds': on_off(builds), 'flag': (f'{",".join(added) or "access"} of {ref[0]} in {'ON' if builds & 1 else 'OFF'} only' if ((builds & 3) in (1, 2) and (ref[5] & 3) == 3) else '')})
        e['globals_changed'] = acc
        return e
    for u, r in b_f.items():
        if r['qname'] == 'main':      # every tool unit's main() shares one USR: its rows are merged across units, nothing exact to say
            continue
        if u not in a_f:
            ua = next((x for x in names_a.get(r['qname'], []) if x not in b_f), None)
            entries.append(func_entry('signature changed' if ua else 'added', ua, u))
        elif r['fline'] is not None and u in body_new:
            hb = base_body(a_f[u])
            if hb is not None and hb != body_new[u]:
                entries.append(func_entry('body changed', u, u))
    for u, r in a_f.items():
        if r['qname'] != 'main' and u not in b_f and not any(x not in a_f for x in names_b.get(r['qname'], [])):
            entries.append(func_entry('removed', u, None))

    entries = [e for e in entries if not e['qname'].startswith('FLAGS_')]       # REXCVAR macro storage: noise
    for u, r in b_v.items():
        if r['qname'].startswith('FLAGS_'):
            continue
        if u not in a_v:
            entries.append(var_entry('added', None, u, base_con, br_con, a_v, b_v))
    for u, r in a_v.items():
        if u not in b_v and not r['qname'].startswith('FLAGS_'):
            entries.append(var_entry('removed', u, None, base_con, br_con, a_v, b_v))
    entries.sort(key=lambda e: not (any(g['flag'] for g in e.get('globals_changed', [])) or any(s['flag'] for k in ('callers', 'writers', 'readers') for s in e.get(k, []))))
    # touched globals: the full writer / reader picture (base vs branch) of every global whose access set changed
    touched = {}
    for e in entries:
        for g in e.get('globals_changed', []):
            touched.setdefault(g['usr'], g['global'])
    tg = []
    for usr, qn in sorted(touched.items(), key=lambda x: x[1])[:limit]:
        wa, wb = site_groups(base_con, usr, ('write', 'rw')), site_groups(br_con, usr, ('write', 'rw'))
        ra, rb = site_groups(base_con, usr, ('read', 'rw')), site_groups(br_con, usr, ('read', 'rw'))
        sb = (br_con.execute('SELECT builds FROM syms WHERE usr = ?', (usr,)).fetchone() or {'builds': 0})['builds']
        tg.append({'global': qn, 'builds': on_off(sb), 'writers': compare_sites(wa, wb, 'writer', sb), 'readers': compare_sites(ra, rb, 'reader', sb)})
    flags = []
    for e in entries:
        for kind in ('callers', 'writers', 'readers'):
            for s in e.get(kind, []):
                if s['flag']:
                    flags.append(f'{e["qname"]}: {s["flag"]} ({s["func"]} {s["file"]}:{s["lines"][0] if s["lines"] else "?"}){"  [" + s["change"] + "]" if s["change"] else ""}')
        for g in e.get('globals_changed', []):
            if g['flag']:
                flags.append(f'{e["qname"]}: {g["flag"]} ({g["file"]}:{g["line"]})')
    for t in tg:
        for kind in ('writers', 'readers'):
            for s in t[kind]:
                if s['flag'] and s['change']:
                    flags.append(f'{t["global"]}: {s["flag"]} ({s["func"]} {s["file"]}:{s["lines"][0] if s["lines"] else "?"})  [{s["change"]}]')
    return {'branch': meta.get('branch'), 'tip': meta.get('tip'), 'merge_base': meta.get('merge_base'), 'overlay': info['status'],
            'overlay_secs': info['secs'], 'units_reparsed': meta.get('units'), 'git_changed_files': len(gch),
            'base_drift_files': warn, 'changed': entries, 'touched_globals': tg, 'flags': sorted(set(flags)),
            'limits': ['a changed variable TYPE / initial value keeps its USR: not reported as changed', 'a function body is compared as text (blank lines and trailing '
                       'spaces ignored); a body edit only inside a macro argument still counts', 'CMake changes are not re-read']}


def var_entry(change, ua, ub, base_con, br_con, a_v, b_v):
    r = (b_v.get(ub) if ub else None) or a_v.get(ua)
    e = {'kind': qours_kind(r['kind']), 'change': change, 'qname': r['qname'], 'file': r['file'], 'line': r['line'],
         'builds_base': on_off(a_v[ua]['builds']) if ua else None, 'builds_branch': on_off(b_v[ub]['builds']) if ub else None}
    wa = site_groups(base_con, ua, ('write', 'rw')) if ua else {}
    wb = site_groups(br_con, ub, ('write', 'rw')) if ub else {}
    ra = site_groups(base_con, ua, ('read', 'rw')) if ua else {}
    rb = site_groups(br_con, ub, ('read', 'rw')) if ub else {}
    sb = r['builds']
    e['writers'] = compare_sites(wa, wb, 'writer', sb)
    e['readers'] = compare_sites(ra, rb, 'reader', sb)
    return e


def qours_kind(k):
    return {9: 'variable', 6: 'field', 7: 'enumerator'}.get(k, str(k))


def print_diff(d, limit=40):
    print(f'q ours diff {d["branch"]}  tip {str(d["tip"])[:10]}  merge-base {str(d["merge_base"])[:10]}   overlay {d["overlay"]} ({d["units_reparsed"]} units re-parsed, '
          f'{d["overlay_secs"]} s)   {d["git_changed_files"]} files changed by the branch')
    if d['base_drift_files']:
        print(f'  NOTE base index differs from the main checkout for {len(d["base_drift_files"])} changed files (e.g. {d["base_drift_files"][0]}): body compares there are unreliable; q ours update')
    kinds = {}
    for e in d['changed']:
        kinds[e['change']] = kinds.get(e['change'], 0) + 1
    print(f'  {len(d["changed"])} changed symbols ({", ".join(f"{v} {k}" for k, v in sorted(kinds.items())) or "none"}); '
          f'{len(d["touched_globals"])} touched globals; {len(d["flags"])} cross-build flags')
    print(f'CROSS-BUILD FLAGS ({len(d["flags"])}): a symbol compiled in BOTH builds with a caller / reader / writer in only one' if d['flags'] else 'CROSS-BUILD FLAGS: none')
    for f in d['flags'][:limit * 2]:
        print(f'  {f}')
    if not d['changed']:
        print('  no changed functions / variables found in the branch\'s changed files')

    def sites(rows, indent):
        shown = rows[:limit]
        for s in shown:
            tag = f'{s["change"]}' if s['change'] else 'both'
            ln = ','.join(str(x) for x in s['lines'][:4])
            b = s['branch'] or f'(gone; was {s["base"]})'
            fl = f'   <== FLAG {s["flag"]}' if s['flag'] else ''
            print(f'{indent}{s["what"]:<7} {s["func"]}  {s["file"]}:{ln}  base: {s["base"] or "-"}  branch: {s["branch"] or "-"}  [{tag}]{fl}')
        if len(rows) > limit:
            print(f'{indent}... {len(rows) - limit} more')
    for e in d['changed'][:limit * 3]:
        print(f'{e["change"].upper():<18} {e["kind"]} {e["qname"]}  {e["file"]}:{e["line"]}   builds base: {e["builds_base"] or "-"}  branch: {e["builds_branch"] or "-"}')
        gl = sorted(e.get('globals_changed', []), key=lambda g: (not g['flag'], g['global']))
        for g in gl[:8]:
            fl = f'   <== FLAG {g["flag"]}' if g['flag'] else ''
            what = '; '.join(x for x in (f'new {",".join(g["new_access"])}' if g['new_access'] else '', f'dropped {",".join(g["dropped_access"])}' if g['dropped_access'] else '') if x)
            print(f'    globals: {what}  {g["global"]}  ({g["file"]}:{g["line"]}, {g["builds"]}){fl}')
        if len(gl) > 8:
            print(f'    globals: ... {len(gl) - 8} more changed accesses (--json has all)')
        sites(e.get('callers', []), '    ')
        sites(e.get('writers', []), '    ')
        sites(e.get('readers', []), '    ')
    if d['touched_globals']:
        print('TOUCHED GLOBALS (every writer / reader, base vs branch):')
        for t in d['touched_globals']:
            print(f'  {t["global"]}')
            sites(t['writers'], '    ')
            sites(t['readers'], '    ')
    for lim in d['limits']:
        print(f'  limit: {lim}')

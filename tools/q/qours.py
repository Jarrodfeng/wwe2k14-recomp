"""q ours: the structural index of OUR code (src\\, tools\\, probes\\), the counterpart of q's game-code callers / callees / fn.

    py tools/q/q.py ours COMMAND ...        (db: out/harness/index/ours.db, built by tools/q/build_ours.py; exact, from the clang AST)

  def SYMBOL          where a function / variable / enumerator / type is declared and defined (file:line)
  refs SYMBOL         every reference: calls, reads, writes, plain references, with the function each sits in
  callers FUNC        who calls (or takes the address of) FUNC, grouped by function, with the builds that compile each site
  callees FUNC        what FUNC calls (ours and the SDK's), grouped
  writes GLOBAL       who stores a global / static / atomic / field (=, op=, ++, .store / .exchange / .fetch_*, container mutators)
  reads GLOBAL        who loads it
  fn FUNC             one screen: where, which BUILDS compile it, callers, callees, globals read / written, switches (REXCVAR_GET), log lines
  covers FILE         which gate lanes / checks name the file (affects / needs / cmd), which unit tests and rigs compile it
  rows [REGEX]        record row kinds (scene_record.h enumerators kSl*, kCp* ...): who writes (constructs / assigns), who reads (compares)
  lanes-of SCRIPT     (S103 QX2) gate checks naming a tools\\ script (affects / needs / cmd, one line each) + what it invokes + who invokes / names it;
                      `covers` does the same for a python / ps1 / json file; def / refs / callers / callees / fn / writes / reads fall back to the tools\\ python + ps1
                      index when the C++ index has no such symbol (qualify it: gate_wait.main, gdeps.Table.idx, gate.dtab; --tools asks it first)
  line "<tag or log line>"  (QX3) the src printf / Log site(s) that print a log line and every tools\\ regex / string that parses or names it
  lit VALUE           (QX4) every src / tools / probes file holding the literal (integer: hex + decimal forms, word-bounded; else fixed text)
  includes FILE / includers FILE   (QX4) the direct #include lines of a C++ file / every file that includes it
  cvar NAME           (QX5) a cvar: every REXCVAR_DEFINE_* (ours + the SDK's) with type / default / help / the #if block (stream, stream-free, Android), every REXCVAR_GET / SET /
                      by-name string use, every `--NAME=value` in tools / lane json / routes; a part of a name lists the matches; def / refs on a cvar name answer the same
  (QX5) writes / reads / refs also list the lines inside `#if defined(__ANDROID__)` branches (text match: the ON / OFF index never compiles them)
  lit VALUE --all     every line in every file (default: 3 per file);  --tools: only tools / android / config python, ps1, json ... text, each hit marked string / comment / code
  glsl NAME           (QGL) which .glsl / .fx.json shader sources read / write / declare / #define / #if-test NAME (a uniform, an interpolator component
                      inUvFog.w, a field fc.Sweat.y, a define FX_WRINKLES): file:line, kind, the #if guard each line sits under
  fxreg [--dupes]     (QGL) fx_registry.h: counts; --dupes = duplicate hashes grouped by owner (cross-owner flagged); twins (X_fragment / X_pl_fragment)
                      with a constants table on one side only; variants no hash row names
  str LITERAL         (QGL) where our code uses a string literal containing LITERAL ("g_fWklLv%d", "g_mNormalWklPrm"): file:line, the literal's line, the enclosing function;
                      a printf-style LITERAL also matches by the text before its first %-spec and split literals; --tools adds script / json / toml text
  census FIELD [--in PATH] [--run N]   (QCF) the counter behind a census / log field: the format literal holding FIELD -> the argument it prints for it (printf spec
                      N -> arg N; `" field " + to_string(x)`; `<< x`) -> the counter (member / global / element; a local is expanded) -> every write to it
                      (++ / += / = / .store / .fetch_*) with file:line and the enclosing function, + the declaration. Text level, always current. --run also prints
                      the field's values per window from a run log (= `q runlog field FIELD --run N`). Limits: tools/q/qcensus.py header
  info                what the index was built from, counts, staleness
  (worktree, QX1) the borrowed main index is remapped to the branch's edited lines in seconds (budget WWE_Q_FRESH_BUDGET, default 8 s; `stale: N files`
  when over; symbols the branch ADDED show as a text match); lanes-of / covers / line / lit / includes / tools symbols read the checkout's own files: always current
  update [-j N]       rebuild incrementally (re-parses only units whose files changed); --full re-parses everything
  diff REF            (S103 QB) what a branch changes: changed / added / removed functions and variables, each with its callers / readers / writers in the BASE
                      index and in the BRANCH, and every caller / reader / writer that exists in only one of the ON / OFF builds (a cross-build FLAG)

BRANCH OVERLAY (tools/q/ours_overlay.py, docs/agents/q_ours.md): put `--branch REF` or `--worktree PATH` before any command above and it answers for that
branch's code: the MAIN checkout's index is the base, only the units the branch's changes touch are re-parsed (~30-50 s the first time, cached by the branch
tip, 1-5 s after), every row is tagged <base> or <branch>. `q ours diff REF` is the same with REF as the branch.

SYMBOL: a qualified name or its tail (presence::g_crowd, rdr::g_low_tier, FadeAlpha255). Several matches are all shown.
Builds: ON = the game exe (preset win-amd64-release), OFF = the stream-free exe (WWE2K14_KEEP_STREAM=OFF), tool = a unit / rig under
tools\\, other = a src file in neither exe (launcher, fview, viewer). Android compiles the ON source list (CMakeLists shares it).
A site's builds = the units that compiled that exact line (a header function is in every build that includes the header).
Writes are decided from the AST context; a variable passed by non-const reference / pointer shows as 'addr', not 'write'.
An enumerator counts as read when compared / switched on / masked, as written when it is constructed, assigned or passed.
--json on any command; --limit N (default 30 groups).
"""
import contextlib
import argparse
import fnmatch
import glob
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FUNC_KINDS = (8, 21, 24, 25, 26, 30)
VAR_KINDS = (9, 6)
KIND_NAMES = {2: 'struct', 3: 'union', 4: 'class', 5: 'enum', 6: 'field', 7: 'enumerator', 8: 'function', 9: 'variable', 21: 'method',
              24: 'constructor', 25: 'destructor', 26: 'conversion', 30: 'function template', 31: 'class template'}
B = {1: 'ON', 2: 'OFF', 4: 'tool', 8: 'other'}


class QOError(Exception):
    pass


def bstr(m):
    if m is None:
        return '?'
    names = []
    if m & 1 and m & 2:
        names.append('ON+OFF')
    elif m & 1:
        names.append('ON')
    elif m & 2:
        names.append('OFF')
    if m & 4:
        names.append('tool')
    if m & 8:
        names.append('other')
    return '+'.join(names) or 'none'


def _main_checkout(root):
    """The main checkout of a git worktree (via the shared .git dir), or None."""
    import subprocess
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:
        return None
    if not common:
        return None
    common = os.path.abspath(os.path.join(root, common))
    main = os.path.dirname(common) if os.path.basename(common) == '.git' else None
    return main if main and os.path.normcase(main) != os.path.normcase(os.path.abspath(root)) else None


def is_worktree(root=None):
    """True inside a git worktree (not the main checkout)."""
    return _main_checkout(root or ROOT) is not None


def db_path(root=None):
    p = os.path.join(root or ROOT, 'out', 'harness', 'index', 'ours.db')
    # QTF (S103 WTA fix 1): inside a worktree the main checkout's index is ALWAYS borrowed, even when the worktree holds an ours.db of its own
    # (16 worktrees did: `q ours update` built each a private 200 MB index, then every call re-indexed it for 10+ minutes).
    if not os.path.exists(p) or is_worktree(root or ROOT):
        # S103: builders in worktrees had NO q ours index ("no q ours index exists in a worktree") and fell back to grep. A worktree now
        # reads the main checkout's index; rows of files this branch changed may be out of date (q ours --worktree . overlays them).
        main = _main_checkout(root or ROOT)
        mp = os.path.join(main, 'out', 'harness', 'index', 'ours.db') if main else None
        if mp and os.path.exists(mp):
            print('q ours: worktree: using the main checkout\'s index (rows of files this branch changed may be stale; '
                  'exact for them: q ours --worktree . CMD)', file=sys.stderr)
            global BORROWED_INDEX
            BORROWED_INDEX = True   # never auto-refresh a borrowed index from a worktree (it would rebuild a whole index: minutes)
            global BORROWED_PATH
            BORROWED_PATH = mp
            return mp
    return p


def refuse_update_in_worktree(root=None):
    """QTF: `q ours update` / build_ours in a worktree builds a whole private index. Returns the refusal text, or None in the main checkout."""
    if is_worktree(root or ROOT):
        return ('q ours: update refused in a worktree: it would build a whole private index (10+ minutes, out of memory under load). '
                "A worktree reads the main checkout's index; for your own changed files use  py tools/q/q.py ours --worktree . CMD")
    return None


BORROWED_INDEX = False
BORROWED_PATH = None


def connect(path):
    if not os.path.exists(path):
        raise QOError(f'no index at {path}: build it with  py tools\\q\\build_ours.py  (a full build is a few minutes, then incremental)')
    return open_ro(path)


def db_wait():
    """QGL: how long a READER waits for a writer (a refresh swapping the index in) before it answers from the last good copy: 2 s (WWE_Q_DB_WAIT)"""
    try:
        return max(0.1, float(os.environ.get('WWE_Q_DB_WAIT', '2')))
    except ValueError:
        return 2.0


def slow_wait():
    try:
        return max(0.1, float(os.environ.get('WWE_Q_DB_SLOW_WAIT', '20')))
    except ValueError:
        return 20.0


def _is_busy(e):
    t = str(e).lower()
    return 'locked' in t or 'busy' in t


LAST_OPENED = None     # QGL review: the path open_ro ACTUALLY opened (the live db, or its `.good` copy); worktree / overlay readers attach THIS, never the live path


def _today_note(good):
    t = os.path.getmtime(good)
    fmt = '%H:%M' if time.strftime('%Y-%m-%d', time.localtime(t)) == time.strftime('%Y-%m-%d') else '%Y-%m-%d %H:%M'
    return time.strftime(fmt, time.localtime(t))


def _open_good(good, wait, why):
    try:
        gc = sqlite3.connect(f'file:{good}?mode=ro', uri=True, timeout=wait)
        gc.row_factory = sqlite3.Row
        gc.execute('SELECT count(*) FROM sqlite_master').fetchone()
    except sqlite3.Error:
        return None
    global LAST_OPENED
    LAST_OPENED = good
    gc.execute('PRAGMA busy_timeout = 60000')
    print(f'q ours: {why}; answering from the last good copy ({_today_note(good)}), which is stale by whatever changed since', file=sys.stderr)
    return gc


def readable_path(path):
    """QGL review: the path a reader should attach (qfresh.wrap, the overlay base): the live db, or `<db>.good` while a swap holds the live one."""
    con = open_ro(path)
    p = LAST_OPENED or path
    con.close()
    return p


def open_ro(path):
    """QTF review C2: read-only open that survives a HOT journal (a writer killed mid-transaction): one read-write open rolls it back, then reopen read-only.
    Never the raw 'attempt to write a readonly database' crash: an unreadable / empty index gets a clear message.
    QGL (S103, probes hit 'refresh timed out on the index lock' and fell back to grep): a reader never waits on a writer for more than db_wait() (2 s). The swap
    of a finished refresh holds the write lock while it copies the whole 220 MB db; a reader that meets it answers from `<db>.good`, the copy build_ours keeps of the
    last swapped-in index, and says so. Only when there is no such copy does it wait longer (20 s, and it says so). After the probe query the connection gets a
    60 s busy timeout (a swap that starts mid-command must not fail the command), and LAST_OPENED names the file that was opened."""
    sys.path.insert(0, HERE)
    import build_ours
    global LAST_OPENED
    last = None
    busy = False
    good = path + '.good'
    wait = db_wait()
    healed = False
    slow = False
    while True:
        con = sqlite3.connect(f'file:{path}?mode=ro', uri=True, timeout=slow_wait() if slow else wait)
        con.row_factory = sqlite3.Row
        try:
            n = con.execute("SELECT count(*) FROM sqlite_master WHERE name IN ('syms', 'refs', 'funcs', 'meta')").fetchone()[0]
            if n < 4:
                con.close()
                raise QOError(f'the index at {path} is empty or incomplete (a refresh was killed mid-merge by an older build_ours): '
                              'run  py tools' + BS + 'q' + BS + 'build_ours.py  in the MAIN checkout')
            con.execute('PRAGMA busy_timeout = 60000')      # the probe got in: a later SELECT may wait out a swap that starts now
            LAST_OPENED = path
            return con
        except sqlite3.DatabaseError as e:
            last = e
            con.close()
            if _is_busy(e):
                busy = True
                if slow:
                    break
                if os.path.exists(good):
                    gc = _open_good(good, wait, 'index busy (a refresh is swapping a new one in)')
                    if gc is not None:
                        return gc
                slow = True
                print(f'q ours: index busy (a refresh is swapping it in) and there is no last-good copy yet: waiting up to {slow_wait():g} s', file=sys.stderr)
                continue
            if not healed:
                healed = True
                if build_ours.heal_db(path, wait=wait):
                    continue
            break
    if os.path.exists(good):         # a killed writer / a failed heal: the last good copy is still a whole index
        gc = _open_good(good, wait, f'the live index is not readable ({last})')
        if gc is not None:
            return gc
    if busy:
        raise QOError(f'the index at {path} is busy: a refresh is swapping a new one in (it holds the file for a few seconds to a minute) and there is no last-good copy yet; '
                      'retry in a minute. Nothing is broken: do not rebuild it.')
    raise QOError(f'the index at {path} is not readable ({last}); a refresh was probably killed mid-write: '
                  'run  py tools' + BS + 'q' + BS + 'build_ours.py  in the MAIN checkout (it rebuilds into a copy and swaps it in)')


def meta(con):
    return {r['key']: json.loads(r['value']) for r in con.execute('SELECT key, value FROM meta')}


def staleness(con, root):
    """changed / added / removed files since the index was built (size + mtime first, then a content hash), and the units hit."""
    stale = {'changed': [], 'added': [], 'removed': []}
    have = {r['path']: (r['sha'], r['size'], r['mtime_ns']) for r in con.execute('SELECT path, sha, size, mtime_ns FROM files')}
    seen = set()
    import hashlib
    for top in ('src', 'tools', 'probes'):
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules')]
            for f in fn:
                if f.endswith(('.h', '.hpp', '.cpp', '.inl', '.cc')):
                    p = os.path.join(dp, f)
                    rel = os.path.relpath(p, root).replace('\\', '/')
                    seen.add(rel)
                    if rel not in have:
                        stale['added'].append(rel)
                        continue
                    st = os.stat(p)
                    sha, size, mt = have[rel]
                    if st.st_size != size or st.st_mtime_ns != mt:
                        with open(p, 'rb') as fh:
                            if hashlib.sha1(fh.read()).hexdigest() != sha:
                                stale['changed'].append(rel)
    stale['removed'] = [p for p in have if p not in seen]
    hit = set(stale['changed']) | set(stale['removed'])
    units = set()
    if hit:
        q = ','.join('?' * len(hit))
        for r in con.execute(f'SELECT DISTINCT t.file FROM tus t LEFT JOIN incs i ON i.tu = t.id WHERE t.file IN ({q}) OR i.file IN ({q})',
                             list(hit) * 2):
            units.add(r['file'])
    stale['units'] = sorted(units)
    return stale


def stale_warning(st):
    n = len(st['changed']) + len(st['added']) + len(st['removed'])
    if not n:
        return None
    return (f'{len(st["changed"])} files changed, {len(st["added"])} added, {len(st["removed"])} removed since the index was built '
            f'({len(st["units"])} units affected: e.g. {", ".join((st["changed"] + st["added"] + st["removed"])[:3])}); '
            f'refresh: py tools\\q\\q.py ours update')


# ---------------------------------------------------------------- symbol lookup

def find_syms(con, name, kinds=None):
    name = name.strip()
    rows = con.execute('SELECT * FROM syms WHERE qname = ?', (name,)).fetchall()
    if not rows:
        rows = con.execute("SELECT * FROM syms WHERE qname LIKE ? ESCAPE '\\'", ('%::' + name.replace('_', '\\_').replace('%', '\\%'),)).fetchall()
    if not rows:
        rows = con.execute('SELECT * FROM syms WHERE name = ?', (name,)).fetchall()
    if kinds:
        rows = [r for r in rows if r['kind'] in kinds]
    # one entry per USR is already guaranteed; sort definitions first
    rows.sort(key=lambda r: (r['qname'], r['file'] or '', r['line'] or 0))
    return rows


def need_syms(con, name, kinds=None, what='symbol'):
    rows = find_syms(con, name, kinds)
    if not rows:
        any_ = find_syms(con, name)
        if any_:
            raise QOError(f'{name}: found only as {", ".join(sorted({KIND_NAMES.get(r["kind"], str(r["kind"])) for r in any_}))}, not a {what} '
                          f'(q ours def {name})')
        raise QOError(f'{name}: no such symbol in our code (a name tail works: presence::g_crowd)')
    return rows


def qn(con, usr, cache={}):
    if not usr:
        return '(file scope)'
    if usr not in cache:
        r = con.execute('SELECT qname FROM syms WHERE usr = ?', (usr,)).fetchone()
        cache[usr] = r['qname'] if r else usr
    return cache[usr]


def sites_grouped(con, rows, func_col=True):
    """rows of refs -> [{func, file, lines, kinds, builds}] grouped by (function, file)"""
    g = {}
    for r in rows:
        k = (qn(con, r['func']), r['file'])
        e = g.setdefault(k, {'func': k[0], 'file': k[1], 'lines': [], 'kinds': set(), 'builds': 0, 'usrs': set(), 'origins': set()})
        e['usrs'].add(r['func'])
        if 'origin' in r.keys():     # a branch overlay connection (tools/q/ours_overlay.py): base | branch
            e['origins'].add(r['origin'])
        e['lines'].append(r['line'])
        e['kinds'].add(r['kind'])
        e['builds'] |= r['builds']
    out = []
    for e in g.values():
        e['lines'] = sorted(set(e['lines']))
        e['kinds'] = sorted(e['kinds'])
        e['usrs'] = sorted(e['usrs'])
        e['origins'] = sorted(e['origins'])
        out.append(e)
    out.sort(key=lambda e: (e['file'], e['lines'][0]))
    return out


def caller_tree(con, usrs, depth, seen=None):
    """the callers of the functions `usrs`, `depth` levels up: [{func, file, lines, builds, callers: [...]}]"""
    seen = set() if seen is None else seen
    out = []
    for u in usrs:
        if not u or u in seen:
            continue
        seen.add(u)
        rows = con.execute("SELECT * FROM refs WHERE usr = ? AND kind IN ('call','ref') ORDER BY file, line", (u,)).fetchall()
        for g in sites_grouped(con, rows):
            g['callers'] = caller_tree(con, g['usrs'], depth - 1, seen) if depth > 1 else []
            out.append(g)
    return out


def print_tree(nodes, indent, limit):
    for n in nodes[:limit]:
        tag = f'  <{"+".join(n["origins"])}>' if n.get('origins') else ''
        print(f'{indent}called from {n["func"]}  {n["file"]}:{n["lines"][0]}  {bstr(n["builds"])}{tag}')
        print_tree(n['callers'], indent + '  ', limit)


def print_sites(groups, args, indent='  '):
    for e in groups[:args.limit]:
        ls = ','.join(str(x) for x in e['lines'][:6]) + ('...' if len(e['lines']) > 6 else '')
        tag = f'  <{"+".join(e["origins"])}>' if e.get('origins') else ''
        print(f'{indent}{e["file"]}:{ls}  in {e["func"]}  [{"/".join(e["kinds"])}]  {bstr(e["builds"])}{tag}')
    if len(groups) > args.limit:
        print(f'{indent}... {len(groups) - args.limit} more (--limit)')


def off_summary(groups):
    n_off = sum(1 for e in groups if e['builds'] & 2)
    n_on = sum(1 for e in groups if e['builds'] & 1)
    return f'{len(groups)} sites: in the ON build {n_on}, in the OFF build {n_off}'


# ---------------------------------------------------------------- commands

def cmd_def(con, name, args):
    out = []
    for s in find_syms(con, name):
        decls = [d for d in con.execute("SELECT file, line FROM refs WHERE usr = ? AND kind = 'def'", (s['usr'],))]
        out.append({'qname': s['qname'], 'kind': KIND_NAMES.get(s['kind'], s['kind']), 'file': s['file'], 'line': s['line'],
                    'defined': bool(s['isdef']), 'builds': bstr(s['builds']), 'usr': s['usr']})
    if not out:
        raise QOError(f'{name}: no such symbol in our code')
    return {'symbol': name, 'matches': out}


def print_def(r, args):
    for m in r['matches'][:args.limit]:
        print(f'{m["kind"]:<12} {m["qname"]}  {m["file"] or "(SDK / system)"}:{m["line"] or "?"}  '
              f'{"defined" if m["defined"] else "declared only"}  {m["builds"]}')


def cmd_refs(con, name, args):
    res = []
    for s in need_syms(con, name):
        rows = con.execute('SELECT * FROM refs WHERE usr = ? AND kind != ? ORDER BY file, line', (s['usr'], 'def')).fetchall()
        res.append({'qname': s['qname'], 'kind': KIND_NAMES.get(s['kind'], s['kind']), 'file': s['file'], 'line': s['line'],
                    'sites': sites_grouped(con, rows), 'n': len(rows)})
    return {'symbol': name, 'matches': res}


def print_refs(r, args):
    for m in r['matches']:
        print(f'{m["kind"]} {m["qname"]}  ({m["file"] or "SDK"}:{m["line"]})  {m["n"]} references')
        print_sites(m['sites'], args)


def cmd_callers(con, name, args):
    res = []
    for s in need_syms(con, name, FUNC_KINDS, 'function'):
        rows = con.execute("SELECT * FROM refs WHERE usr = ? AND kind IN ('call','ref') ORDER BY file, line", (s['usr'],)).fetchall()
        res.append({'qname': s['qname'], 'file': s['file'], 'line': s['line'], 'builds': bstr(s['builds']), 'summary': None,
                    'sites': sites_grouped(con, rows), 'n': len(rows)})
        res[-1]['summary'] = off_summary(res[-1]['sites'])
    return {'symbol': name, 'matches': res}


def print_callers(r, args):
    for m in r['matches']:
        print(f'{m["qname"]}  ({m["file"] or "SDK"}:{m["line"]}, compiled in {m["builds"]})  {m["summary"]}')
        print_sites(m['sites'], args)


def cmd_callees(con, name, args):
    res = []
    for s in need_syms(con, name, FUNC_KINDS, 'function'):
        rows = con.execute("SELECT r.usr, r.kind, r.file, r.line, r.builds, y.qname, y.kind AS tk, y.file AS tfile, y.builds AS tb "
                           "FROM refs r JOIN syms y ON y.usr = r.usr WHERE r.func = ? AND r.kind IN ('call','ref') AND y.kind IN (8,21,24,25,26,30) "
                           "ORDER BY r.line", (s['usr'],)).fetchall()
        g = {}
        for r in rows:
            e = g.setdefault(r['usr'], {'callee': r['qname'], 'file': r['tfile'], 'lines': [], 'kinds': set(), 'builds': 0})
            e['lines'].append(r['line'])
            e['kinds'].add(r['kind'])
            e['builds'] |= r['builds']
        callees = [{**e, 'lines': sorted(set(e['lines'])), 'kinds': sorted(e['kinds'])} for e in g.values()]
        callees.sort(key=lambda e: e['lines'][0])
        res.append({'qname': s['qname'], 'file': s['file'], 'line': s['line'], 'callees': callees})
    return {'symbol': name, 'matches': res}


def print_callees(r, args):
    for m in r['matches']:
        print(f'{m["qname"]}  ({m["file"]}:{m["line"]})  calls {len(m["callees"])} functions')
        for e in m['callees'][:args.limit * 2]:
            print(f'  {e["callee"]:<48} {e["file"] or "SDK":<44} at {",".join(str(x) for x in e["lines"][:5])}  [{"/".join(e["kinds"])}]  {bstr(e["builds"])}')


def cmd_access(mode):
    kinds = ('write', 'rw') if mode == 'writes' else ('read', 'rw')

    def run(con, name, args):
        res = []
        for s in need_syms(con, name, VAR_KINDS, 'variable / field'):
            q = ','.join('?' * len(kinds))
            rows = con.execute(f'SELECT * FROM refs WHERE usr = ? AND kind IN ({q}) ORDER BY file, line', (s['usr'], *kinds)).fetchall()
            addr = con.execute("SELECT * FROM refs WHERE usr = ? AND kind = 'addr' ORDER BY file, line", (s['usr'],)).fetchall()
            res.append({'qname': s['qname'], 'file': s['file'], 'line': s['line'], 'mode': mode, 'sites': sites_grouped(con, rows),
                        'address_taken': sites_grouped(con, addr), 'n': len(rows)})
            res[-1]['summary'] = off_summary(res[-1]['sites'])
            if args.via:
                for g in res[-1]['sites']:
                    g['callers'] = caller_tree(con, g['usrs'], args.via)
        return {'symbol': name, 'matches': res}
    return run


def print_access(r, args):
    for m in r['matches']:
        print(f'{m["qname"]}  ({m["file"]}:{m["line"]})  {m["mode"]}: {m["summary"]}')
        print_sites(m['sites'], args)
        if args.via:
            print(f'  who reaches the {"writing" if m["mode"] == "writes" else "reading"} functions (--via {args.via}):')
            for g in m['sites'][:args.limit]:
                print(f'    {g["func"]} ({g["file"]}, {bstr(g["builds"])}):')
                print_tree(g.get('callers', []), '      ', 8)
        if m['address_taken']:
            print('  address / reference taken (may write through it):')
            print_sites(m['address_taken'], args, '    ')


def read_log(rows):
    return rows


def cmd_fn(con, name, args):
    res = []
    for s in need_syms(con, name, FUNC_KINDS, 'function'):
        usr = s['usr']
        f = con.execute('SELECT * FROM funcs WHERE usr = ?', (usr,)).fetchone()
        callers = sites_grouped(con, con.execute("SELECT * FROM refs WHERE usr = ? AND kind IN ('call','ref') ORDER BY file, line", (usr,)).fetchall())
        callee_rows = con.execute("SELECT y.qname, y.file, r.line, r.builds, r.kind FROM refs r JOIN syms y ON y.usr = r.usr "
                                  "WHERE r.func = ? AND r.kind IN ('call','ref') AND y.kind IN (8,21,24,25,26,30) ORDER BY r.line", (usr,)).fetchall()
        cal = {}
        for r in callee_rows:
            e = cal.setdefault(r['qname'], {'callee': r['qname'], 'file': r['file'], 'n': 0})
            e['n'] += 1
        gl = {}
        for r in con.execute("SELECT y.qname, y.kind, r.kind AS rk, y.file FROM refs r JOIN syms y ON y.usr = r.usr WHERE r.func = ? AND y.kind IN (9, 6) "
                             "AND r.kind IN ('read','write','rw','addr')", (usr,)):
            e = gl.setdefault(r['qname'], {'name': r['qname'], 'file': r['file'], 'read': 0, 'write': 0, 'addr': 0, 'field': r['kind'] == 6})
            e['write' if r['rk'] in ('write', 'rw') else ('addr' if r['rk'] == 'addr' else 'read')] += 1
            if r['rk'] == 'rw':
                e['read'] += 1
        units = []
        if f:
            units = [r['file'] for r in con.execute('SELECT DISTINCT t.file FROM tus t JOIN incs i ON i.tu = t.id WHERE t.build = 4 AND i.file = ? ORDER BY t.file',
                                                    (f['file'],))]
        builds = f['builds'] if f else s['builds']
        android = ''
        if s['file'] and os.path.basename(s['file']).startswith('android'):
            android = 'Android-only file'
        elif builds & 1:
            android = 'Android: the ON source list (CMakeLists shares it)'
        res.append({'qname': s['qname'], 'kind': KIND_NAMES.get(s['kind'], s['kind']), 'file': s['file'], 'line': s['line'],
                    'endline': f['endline'] if f else None, 'defined': bool(f), 'builds': bstr(builds), 'android': android,
                    'compiled_by_units': units[:12], 'callers': callers, 'callees': sorted(cal.values(), key=lambda e: -e['n']),
                    'globals': sorted(gl.values(), key=lambda e: (-e['write'], e['name'])),
                    'switches': json.loads(f['cvars']) if f else [], 'logs': json.loads(f['logs']) if f else []})
    return {'symbol': name, 'matches': res}


def print_fn(r, args):
    for m in r['matches']:
        end = f'-{m["endline"]}' if m['endline'] else ''
        print(f'== {m["kind"]} {m["qname"]}   {m["file"] or "SDK"}:{m["line"]}{end}{"" if m["defined"] else "  (declared only, no body seen)"}')
        print(f'  builds: {m["builds"]}   {m["android"]}')
        if m['compiled_by_units']:
            print(f'  units / rigs that compile it: {", ".join(m["compiled_by_units"][:8])}{" ..." if len(m["compiled_by_units"]) > 8 else ""}')
        print(f'  callers ({off_summary(m["callers"])}):')
        print_sites(m['callers'], argparse.Namespace(limit=8), '    ')
        print(f'  callees ({len(m["callees"])}):')
        print('    ' + ', '.join(f'{e["callee"]}{" x" + str(e["n"]) if e["n"] > 1 else ""}' for e in m['callees'][:14]) + (' ...' if len(m['callees']) > 14 else ''))
        w = [g for g in m['globals'] if g['write']]
        rd = [g for g in m['globals'] if not g['write']]
        print('  globals written: ' + (', '.join(f'{g["name"]}{" (field)" if g["field"] else ""}' for g in w[:12]) or '-'))
        print('  globals read:    ' + (', '.join(g['name'] for g in rd[:14]) + (' ...' if len(rd) > 14 else '') or '-'))
        print('  switches read:   ' + (', '.join(m['switches']) or '-'))
        if m['logs']:
            print('  log / print lines:')
            for t in m['logs'][:6]:
                print(f'    "{t}"')


# ---- covers

def load_lanes(root):
    lanes = []
    for p in sorted(glob.glob(os.path.join(root, 'tools', 'gate', 'lanes', '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                lanes.append((os.path.basename(p), json.load(f)))
        except (OSError, ValueError):
            pass
    return lanes


def pat_match(pat, rel):
    p = pat.replace('\\', '/').lstrip('./') if pat.startswith('./') else pat.replace('\\', '/')
    if p == rel:
        return True
    if any(c in p for c in '*?['):
        return fnmatch.fnmatch(rel, p)
    return p.endswith('/') and rel.startswith(p)


def lanes_for(root, rel):
    hits = []
    needle = re.compile(re.escape(rel).replace('/', r'[/\\]+'))
    for lane, d in load_lanes(root):
        top = d.get('affects', [])
        for c in d.get('checks', []):
            via = []
            ps = [p for p in list(c.get('affects', [])) + list(top) if isinstance(p, str) and pat_match(p, rel)]
            if ps:
                via.append('affects' if any(p.replace('\\', '/') == rel for p in ps) else 'affects-glob')
            for p in c.get('needs', []):
                if isinstance(p, str) and pat_match(p, rel):
                    via.append('needs')
                    break
            cmdtxt = ' '.join(str(x) for x in c.get('cmd', []))
            if needle.search(cmdtxt):
                via.append('cmd')
            if via:
                hits.append({'lane': lane[:-5], 'check': c.get('name'), 'via': via, 'retired': bool(c.get('retired') or d.get('retired'))})
    return hits


def scripts_naming(root, rel):
    pat = re.compile(re.escape(rel).replace('/', r'[/\\]+'))
    out = []
    for dp, dn, fn in os.walk(os.path.join(root, 'tools')):
        dn[:] = [d for d in dn if d not in ('__pycache__', 'lanes')]
        for f in fn:
            if f.endswith(('.py', '.ps1', '.txt', '.cmake')):
                p = os.path.join(dp, f)
                try:
                    with open(p, encoding='utf-8', errors='replace') as fh:
                        if pat.search(fh.read()):
                            out.append(os.path.relpath(p, root).replace('\\', '/'))
                except OSError:
                    pass
    return sorted(out)


def cmd_covers(con, rel, args, root=ROOT):
    rel = rel.replace('\\', '/')
    if rel.startswith(root.replace('\\', '/').rstrip('/') + '/'):
        rel = rel[len(root.rstrip('/\\')) + 1:]
    r0 = con.execute('SELECT 1 FROM files WHERE path = ?', (rel,)).fetchone()
    if not r0:
        raise QOError(f'{rel}: not a file of ours in the index (paths are repo-relative: src/... or tools/...)')
    direct = lanes_for(root, rel)
    units = [r['file'] for r in con.execute('SELECT DISTINCT t.file FROM tus t LEFT JOIN incs i ON i.tu = t.id WHERE t.build = 4 AND (t.file = ? OR i.file = ?) ORDER BY t.file', (rel, rel))]
    builds = [r['build'] for r in con.execute('SELECT DISTINCT t.build FROM tus t LEFT JOIN incs i ON i.tu = t.id WHERE t.file = ? OR i.file = ?', (rel, rel))]
    m = 0
    for b in builds:
        m |= b
    via_units = {}
    for u in units:
        for h in lanes_for(root, u):
            via_units.setdefault((h['lane'], h['check']), []).append(u)
    return {'file': rel, 'builds': bstr(m), 'lanes': direct, 'units': units,
            'lanes_via_units': [{'lane': k[0], 'check': k[1], 'units': v[:4]} for k, v in via_units.items()],
            'scripts_naming_it': scripts_naming(root, rel)}


def print_covers(r, args):
    print(f'{r["file"]}  compiled in: {r["builds"]}')
    named = [h for h in r['lanes'] if h['via'] != ['affects-glob']]
    globs = [h for h in r['lanes'] if h['via'] == ['affects-glob']]
    print(f'  gate checks that name it (affects / needs / cmd): {len(named)} checks in {len({h["lane"] for h in named})} lanes')
    for h in named[:args.limit]:
        print(f'    {h["lane"]} / {h["check"]}  via {"+".join(h["via"])}{"  [RETIRED]" if h["retired"] else ""}')
    print(f'  lanes naming it: {", ".join(sorted({h["lane"] for h in named})) or "NONE"}')
    if globs:
        print(f'  matched only by a glob in affects: {len(globs)} checks in lanes {", ".join(sorted({h["lane"] for h in globs})[:12])}'
              f'{" ..." if len({h["lane"] for h in globs}) > 12 else ""}')
    print(f'  unit tests / rigs that compile it ({len(r["units"])}): {", ".join(r["units"][:10])}{" ..." if len(r["units"]) > 10 else ""}')
    if r['lanes_via_units']:
        print(f'  lanes covering those units ({len(r["lanes_via_units"])}): ' + ', '.join(sorted({f'{x["lane"]}' for x in r['lanes_via_units']})[:14]))
    if r['scripts_naming_it']:
        print(f'  scripts that name it: {", ".join(r["scripts_naming_it"][:8])}{" ..." if len(r["scripts_naming_it"]) > 8 else ""}')


# ---- rows

def cmd_rows(con, pat, args):
    rx = re.compile(pat) if pat else re.compile(r'^k(Sl|Cp|Ml|Fx|Aw|Rc|Qk|Qp|Qb|Run)')
    syms = [s for s in con.execute("SELECT * FROM syms WHERE kind = 7 AND file LIKE '%scene_record.h'") if rx.search(s['name'])]
    out = []
    for s in syms:
        rows = con.execute("SELECT * FROM refs WHERE usr = ? AND kind IN ('read','write') ORDER BY file, line", (s['usr'],)).fetchall()
        wr = sites_grouped(con, [r for r in rows if r['kind'] == 'write'])
        rd = sites_grouped(con, [r for r in rows if r['kind'] == 'read'])
        out.append({'row': s['qname'], 'name': s['name'], 'line': s['line'], 'writers': wr, 'readers': rd})
    out.sort(key=lambda e: (e['line']))
    return {'pattern': pat, 'rows': out}


def print_rows(r, args):
    for e in r['rows'][:args.limit * 2]:
        def fl(gs):
            seen = []
            for g in gs:
                t = f'{g["func"]} ({os.path.basename(g["file"])}:{g["lines"][0]}, {bstr(g["builds"])})'
                if t not in seen:
                    seen.append(t)
            return seen
        w, rd = fl(e['writers']), fl(e['readers'])
        print(f'{e["name"]}  (scene_record.h:{e["line"]})  writers {len(w)}  readers {len(rd)}')
        for t in w[:6]:
            print(f'    writes: {t}')
        for t in rd[:6]:
            print(f'    reads:  {t}')


def cmd_info(con, args, root=ROOT):
    m = meta(con)
    st = staleness(con, root)
    counts = {t: con.execute(f'SELECT COUNT(*) FROM {t}').fetchone()[0] for t in ('files', 'tus', 'syms', 'refs', 'funcs')}
    by_build = {B.get(r['build'], r['build']): r['n'] for r in con.execute('SELECT build, COUNT(*) AS n FROM tus GROUP BY build')}
    errs = [dict(file=r['file'], build=B.get(r['build']), errors=r['errors']) for r in con.execute('SELECT file, build, errors FROM tus WHERE errors > 0 ORDER BY errors DESC LIMIT 6')]
    return {**m, 'counts': counts, 'units_by_build': by_build, 'units_with_parse_errors': errs, 'stale': stale_warning(st), 'stale_files': st['changed'][:10] + st['added'][:10]}


def print_info(r, args):
    for k, v in r.items():
        print(f'{k}: {v}')


# ---------------------------------------------------------------- main

BS = chr(92)
BACKOFF_S = 3600
MIN_FREE_GB = 12.0

SPAWN_CODE = (
    "import json, subprocess, sys\n"
    "d = json.loads(sys.argv[1])\n"
    "lf = open(d['log'], 'ab')\n"
    "base = 0x08000000 | 0x00000200 | 0x00004000   # CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS\n"
    "for fl in ((base | 0x01000000, base) if sys.platform == 'win32' else (0,)):   # 0x01000000 = CREATE_BREAKAWAY_FROM_JOB (tools/live/live.py does the same)\n"
    "    try:\n"
    "        kw = {'creationflags': fl} if sys.platform == 'win32' else {'start_new_session': True}\n"
    "        subprocess.Popen(d['argv'], stdin=subprocess.DEVNULL, stdout=lf, stderr=subprocess.STDOUT, cwd=d['cwd'], **kw)\n"
    "        break\n"
    "    except OSError:\n"
    "        continue\n"
    "else:\n"
    "    sys.exit(3)\n")


def free_ram_gb():
    sys.path.insert(0, HERE)
    import build_ours
    return build_ours._free_ram_gb()


def run_lock_held():
    """the project run lock (tools/harness/runlock.py: exclusive lock.json or a live shared slot): a game run is going, do not start a 6-process parse"""
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'harness'))
        import runlock
        recs = [runlock.read_lock()] + list(runlock.read_slots().values())
        return any(r and runlock._owner_alive(r) for r in recs)
    except Exception:    # noqa: BLE001 - an unreadable lock must never stop a query
        return False


def bg_refresh(root, final, cmd=None, throttle_s=90):
    """QGL: start ONE detached background incremental refresh unless one runs, just started, backed off, or the PC is busy. -> (state, one line for the reader).
    state: 'started' | 'running' (the build lock is held) | 'recent' (started < throttle_s ago) | 'backoff' (a run timed out < 1 h ago: the marker
    <db>.bgbackoff) | 'deferred' (the run lock is held or free RAM < 12 GB: nothing is spawned) | 'failed'.
    The child is `build_ours.py --bg` (-j min(6, the free-RAM cap), gives up on a held lock, stops itself after 25 min) started through a short-lived
    intermediate process (so q's own watchdog / kill_children and the caller's job object do not see or kill it) with CREATE_NO_WINDOW | NEW_PROCESS_GROUP |
    BELOW_NORMAL, breakaway first; output to <db dir>/ours_update.log. `cmd` is a test hook (the argv to spawn instead)."""
    import subprocess
    sys.path.insert(0, HERE)
    import build_ours
    log = os.path.join(os.path.dirname(final), 'ours_update.log')
    try:
        age = time.time() - os.path.getmtime(final + '.bgbackoff')
        if age < BACKOFF_S:
            return 'backoff', f'The last background refresh timed out {int(age // 60)} min ago and is backed off for {int((BACKOFF_S - age) // 60)} more min (log: {log}); run  py tools{BS}q{BS}q.py ours update  yourself when the PC is quiet.'
    except OSError:
        pass
    stamp = final + '.bgstamp'
    try:
        if time.time() - os.path.getmtime(stamp) < throttle_s:
            return 'recent', f'A background refresh was started under {throttle_s} s ago (log: {log}).'
    except OSError:
        pass
    if cmd is None:
        free = free_ram_gb()
        if run_lock_held() or (free is not None and free < MIN_FREE_GB):
            return 'deferred', ('No background refresh now: ' + ('a game run holds the run lock' if run_lock_held() else f'free RAM is {free:.0f} GB (< {MIN_FREE_GB:g})')
                                + '; it starts on a later query when the PC is quiet.')
    probe = build_ours.BuildLock(final, wait_s=0)
    try:
        got = probe.acquire()
    except OSError:
        got = False
    if not got:
        return 'running', 'A refresh holds the build lock (it is running); its result becomes the index.'
    probe.release()
    argv = cmd or [sys.executable, os.path.join(HERE, 'build_ours.py'), '--bg', '-j', str(min(6, build_ours._default_jobs())), '--root', root]
    env = {k: v for k, v in os.environ.items() if k != 'WWE_SWEEP_OWNER'}
    env['WWE_Q_TIMEOUT'] = '0'
    spec = json.dumps({'argv': argv, 'log': log, 'cwd': root})
    try:
        r = subprocess.run([sys.executable, '-I', '-c', SPAWN_CODE, spec], env=env, timeout=30, capture_output=True,
                           creationflags=(0x08000000 if os.name == 'nt' else 0))
        if r.returncode != 0:
            raise OSError(f'the spawn helper exited {r.returncode}')
        with open(stamp, 'w') as f:
            f.write(str(time.time()))
    except (OSError, subprocess.SubprocessError) as e:
        return 'failed', f'Could not start a background refresh ({e}); run  py tools{BS}q{BS}q.py ours update  yourself.'
    return 'started', f'A background refresh was started (log: {log}); ask again in a few minutes for the fresh rows.'


def _refresh(build_ours):
    with contextlib.redirect_stdout(sys.stderr):
        build_ours.main(['-j', '6'])   # S103: capped: a full -j 28 refresh under load ran LLVM out of memory

TEXT_CMDS = ('cvar', 'line', 'lit', 'includes', 'includers', 'hooks', 'text', 'affects', 'glsl', 'fxreg', 'str', 'census')


def lazy_encl(args):
    """enclosing-function lookup for lit / line hits, opening the index only when a C++ hit asks (S103 QX5 fix 6: lit opened it, and in a worktree remapped it, for every call).
    A file that differs from the index (size / sha) answers '' (unknown): a borrowed or stale index must never name the wrong function."""
    st = {}

    def encl(rel, ln):
        if 'con' in st and st['con'] is None:    # QGL: no index to name functions from (a fixture / a checkout without one): unknown, never a KeyError
            return ''
        try:
            if 'con' not in st:
                st['con'] = connect(args.db or db_path(args.root))
                st['files'] = {r['path']: (r['sha'], r['size']) for r in st['con'].execute('SELECT path, sha, size FROM files')}
                st['ok'] = {}
            if rel not in st['ok']:
                import hashlib
                have = st['files'].get(rel)
                ok = False
                if have:
                    try:
                        with open(os.path.join(args.root, rel), 'rb') as fh:
                            b = fh.read()
                        ok = len(b) == have[1] and hashlib.sha1(b).hexdigest() == have[0]
                    except OSError:
                        ok = False
                st['ok'][rel] = ok
            if not st['ok'][rel]:
                return ''
            con = st['con']
            f = con.execute('SELECT usr FROM funcs WHERE file = ? AND line <= ? AND endline >= ? ORDER BY line DESC LIMIT 1', (rel, ln, ln)).fetchone()
            return qn(con, f['usr']) if f else ''
        except (sqlite3.Error, QOError):
            st.setdefault('con', None)
            return ''
    return encl


def text_cmd_x6(args):
    import qx6
    try:
        if args.cmd == 'hooks':
            r = qx6.cmd_hooks(args.root, args.arg)
            pr = lambda r, a: qx6.print_hooks(r, a.limit)
        elif args.cmd == 'text':
            r = qx6.cmd_text(args.root, [w for w in re.split(r'\s+', args.arg.strip()) if w], limit_files=args.limit, allhits=args.all)
            pr = lambda r, a: qx6.print_text(r)
        else:
            r, pr = qx6.cmd_affects(args.root, args.arg), lambda r, a: qx6.print_affects(r)
    except qx6.XError as e:
        raise QOError(str(e))
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
    return 0


def text_cmd_gl(args):
    """S103 QGL: glsl NAME / fxreg [--dupes] / str LITERAL (tools/q/qgl.py): this checkout's own files, no index, always current"""
    import qgl
    try:
        if args.cmd == 'glsl':
            r, pr = qgl.cmd_glsl(args.root, args.arg), qgl.print_glsl
        elif args.cmd == 'fxreg':
            r, pr = qgl.cmd_fxreg(args.root, dupes=args.dupes or args.arg == '--dupes'), qgl.print_fxreg
        else:
            r, pr = qgl.cmd_str(args.root, args.arg, lazy_encl(args), tools=args.tools), qgl.print_str
    except qgl.GError as e:
        raise QOError(str(e))
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
    return 0


def text_cmd_census(args):
    """S103 QCF: census FIELD (tools/q/qcensus.py): the format literal -> the counter it prints -> every write to it; this checkout's own files, no index"""
    import qcensus
    import qrunlog
    try:
        r = qcensus.cmd_census(args.root, args.arg, only_in=getattr(args, 'in_path', None), run=getattr(args, 'run', None))
    except (qcensus.CError, qrunlog.RunlogError) as e:
        raise QOError(str(e))
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        qcensus.print_census(r, args)
    return 0


def text_cmd(args):
    if args.cmd in ('hooks', 'text', 'affects'):
        return text_cmd_x6(args)
    if args.cmd in ('glsl', 'fxreg', 'str'):
        return text_cmd_gl(args)
    if args.cmd == 'census':
        return text_cmd_census(args)
    import qtext
    encl = lazy_encl(args)
    if args.cmd == 'cvar':
        import qx5
        r, pr = qx5.cmd_cvar(args.root, args.arg), qx5.print_cvar
    elif args.cmd == 'line':
        r, pr = qtext.cmd_line(args.root, args.arg, encl), qtext.print_line
        if r is None:
            raise QOError('line: give a log tag like [scene][maps][e2] and / or the words of the line')
    elif args.cmd == 'lit':
        r, pr = qtext.cmd_lit(args.root, args.arg, encl, tools=args.tools), qtext.print_lit
    else:
        rel = qtext.find_cpp(args.root, args.arg)
        if not rel or isinstance(rel, list):
            raise QOError(f'{args.arg}: ' + ('ambiguous: ' + ', '.join(rel[:6]) if rel else 'no such C++ file under src / tools / probes'))
        r, pr = (qtext.cmd_includes(args.root, rel), qtext.print_includes) if args.cmd == 'includes' else (qtext.cmd_includers(args.root, rel), qtext.print_includers)
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
    return 0


def main(argv=None, root=ROOT, db=None):
    argv = list(argv or [])
    if argv and argv[0] == 'update':
        msg = refuse_update_in_worktree(root)
        if msg:
            print(msg, file=sys.stderr)
            return 2
        sys.path.insert(0, HERE)
        import build_ours
        return build_ours.main(argv[1:])
    branch = worktree = None          # the branch overlay (tools/q/ours_overlay.py): --branch REF | --worktree PATH, anywhere before the command
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--branch', '--worktree') and i + 1 < len(argv):
            if a == '--branch':
                branch = argv[i + 1]
            else:
                worktree = argv[i + 1]
            del argv[i:i + 2]
        elif a.startswith(('--branch=', '--worktree=')):
            k, v = a.split('=', 1)
            if k == '--branch':
                branch = v
            else:
                worktree = v
            del argv[i]
        else:
            i += 1
    ap = argparse.ArgumentParser(prog='q ours', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true')
    common.add_argument('--limit', type=int, default=30)
    common.add_argument('--db', default=db)
    common.add_argument('--tools', action='store_true', help='look in the tools python / ps1 index first (default: only when the C++ index has no such symbol)')
    common.add_argument('--all', action='store_true', help='lit / cvar: every line of every file (default 3 per file for lit, 6 line numbers per file for cvar)')
    common.add_argument('--root', default=root)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for n in ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn', 'covers'):
        p = sub.add_parser(n, parents=[common]); p.add_argument('arg')
        if n in ('writes', 'reads'):
            p.add_argument('--via', type=int, nargs='?', const=2, default=0, metavar='N',
                           help='also show who calls the writing / reading functions, N levels up (default 2), with the builds of each caller')
    for n in ('lanes-of', 'line', 'lit', 'includes', 'includers', 'cvar', 'affects', 'glsl', 'str'):
        p = sub.add_parser(n, parents=[common]); p.add_argument('arg')
    p = sub.add_parser('census', parents=[common]); p.add_argument('arg', nargs='+')
    p.add_argument('--in', dest='in_path', default=None, help='census: keep only writers whose file path contains this')
    p.add_argument('--run', default=None, help='census: also print the field\'s values per window from this run log (number or path)')
    p = sub.add_parser('hooks', parents=[common]); p.add_argument('arg', nargs='?', default='')
    p = sub.add_parser('fxreg', parents=[common]); p.add_argument('arg', nargs='?', default=''); p.add_argument('--dupes', action='store_true')
    p = sub.add_parser('text', parents=[common]); p.add_argument('arg', nargs='+')
    p = sub.add_parser('rows', parents=[common]); p.add_argument('arg', nargs='?', default='')
    p = sub.add_parser('diff', parents=[common]); p.add_argument('arg', nargs='?', default='')
    sub.add_parser('info', parents=[common])
    args = ap.parse_args(argv)
    if isinstance(getattr(args, 'arg', None), list):
        args.arg = ' '.join(args.arg)
    if isinstance(getattr(args, 'arg', None), str) and args.arg.startswith('$') and args.cmd in ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn', 'rows'):
        args.arg = args.arg.lstrip('$')   # a PowerShell variable written with its sigil: the index holds the bare name
    try:
        if args.cmd in TEXT_CMDS:     # S103 QX5 fix 6: text answers read this checkout's own files; no index connect, no worktree remap, no stale refresh
            if branch:
                raise QOError(f'{args.cmd} reads the files of a checkout, not a git ref: --branch is not supported here; use --worktree PATH (or run it in that checkout)')
            if worktree:
                if not os.path.isdir(worktree):
                    raise QOError(f'--worktree {worktree}: not a folder')
                args.root = os.path.abspath(worktree)
            return text_cmd(args)
        if args.cmd == 'diff' and not (branch or worktree):
            branch = args.arg or None
            if not branch:
                raise QOError('diff needs the branch: q ours diff REF  (or --branch REF / --worktree PATH)')
        overlay = None
        fresh_info = None
        if branch or worktree:
            sys.path.insert(0, HERE)
            import ours_overlay as ovl
            import qtimeout
            budget = max(10.0, qtimeout.timeout_secs() * 0.6) if qtimeout.timeout_secs() > 0 else None   # QTF: the overlay build gets 60 % of the q budget
            try:
                if budget:
                    base_db = readable_path(args.db or db_path(args.root))
                    done, got = qtimeout.run_with_deadline(lambda: ovl.get_overlay(args.root, branch, worktree, base_db=base_db), budget)
                    if not done:
                        raise ovl.OverlayTooBig(f'the overlay build ran over {budget:g} s (WWE_Q_TIMEOUT) and was killed')
                    con, overlay = got
                else:
                    con, overlay = ovl.get_overlay(args.root, branch, worktree, base_db=readable_path(args.db or db_path(args.root)))
            except ovl.OverlayTooBig as e:
                # QTF: answer from the base index (the main checkout's) instead of re-parsing a huge diff, loudly
                print(f'q ours: overlay skipped: {e}; answering from the base index WITHOUT this branchs changes '
                      f'(rows of the files it changed are the bases, not yours)', file=sys.stderr)
                overlay = None
                con = connect(args.db or db_path(args.root))
                if args.cmd == 'diff':
                    raise QOError('diff needs the overlay, which was skipped (see above)')
            except ovl.OverlayError as e:
                raise QOError(str(e))
            m = overlay['meta'] if overlay else None
            if overlay:
                print(f'q ours: overlay of {m.get("branch")} tip {str(m.get("tip"))[:10]}: {overlay["status"]} in {overlay["secs"]} s, {m.get("units")} units re-parsed, '
                      f'{m.get("changed", 0)} changed + {m.get("added", 0)} new + {m.get("removed", 0)} removed files differ from the base index (built {m.get("base_built")}, '
                      f'itself stale for {m.get("base_stale_files", "?")} main-checkout files); every row is tagged <base> or <branch>', file=sys.stderr)
            if m and m.get('incomplete'):
                print(f'q ours: WARNING {m.get("failed_units")} units failed to parse (crash / out of memory): their rows are missing; rerun when memory is free', file=sys.stderr)
        else:
            con = connect(args.db or db_path(args.root))
            if BORROWED_INDEX and BORROWED_PATH and args.cmd != 'info' and os.environ.get('WWE_Q_FRESH') != '0':
                # QX1: a worktree answers with ITS lines for the files the branch changed (seconds, hard budget), never a refresh
                sys.path.insert(0, HERE)
                import qfresh
                base_con = con
                con, fresh_info = qfresh.wrap(base_con, args.root, LAST_OPENED or BORROWED_PATH)
                base_con.close()
                if fresh_info['note']:
                    print(fresh_info['note'], file=sys.stderr)
            if args.cmd != 'info' and not fresh_info:
                st = staleness(con, args.root)
                w = stale_warning(st)
                if w and BORROWED_INDEX:
                    n = len(st['changed']) + len(st['added']) + len(st['removed'])
                    print(f'q ours: index is {n} files behind this worktree (borrowed from the main checkout; answering from it anyway; '
                          f'your own changes: q ours --worktree . CMD)', file=sys.stderr)
                    w = None
                if w and not args.db and not BORROWED_INDEX and not is_worktree(args.root) and os.environ.get('WWE_Q_OURS_NO_AUTOUPDATE') != '1':
                    # QGL (S103): a reader never refreshes in the foreground any more. Every reader of a stale index used to copy the 220 MB db, queue on
                    # the build lock for up to 72 s and then give up ("refresh timed out on the index lock"), while the swap blocked every other reader.
                    # Now: answer at once from the index there is ("stale by N files"), and make sure ONE detached background refresh is running.
                    n = len(st['changed']) + len(st['added']) + len(st['removed'])
                    state, why = bg_refresh(args.root, db_path(args.root))
                    print(f'q ours: index is stale by {n} files ({len(st["units"])} units; e.g. {", ".join((st["changed"] + st["added"] + st["removed"])[:3])}); '
                          f'answering from it now. {why}', file=sys.stderr)
                    w = None
                if w:
                    print(f'q ours: WARNING index stale: {w}', file=sys.stderr)
        if args.cmd == 'diff':
            r = ovl.cmd_diff(connect(overlay['base_db']), con, overlay, args.limit)
            if args.json:
                print(json.dumps(r, indent=1, default=str))
            else:
                ovl.print_diff(r, args.limit)
            return 0
        if overlay and args.cmd == 'info':
            print(json.dumps(overlay['meta'], indent=1, default=str) if args.json else '\n'.join(f'{k}: {v}' for k, v in overlay['meta'].items() if k != 'git_changed'))
            return 0
        table = {'def': (cmd_def, print_def), 'refs': (cmd_refs, print_refs), 'callers': (cmd_callers, print_callers),
                 'callees': (cmd_callees, print_callees), 'writes': (cmd_access('writes'), print_access),
                 'reads': (cmd_access('reads'), print_access), 'fn': (cmd_fn, print_fn), 'rows': (cmd_rows, print_rows)}
        import qtools
        if args.cmd == 'info':
            r, pr = cmd_info(con, args, args.root), print_info
        elif args.cmd == 'lanes-of':
            rel = qtools.find_file(args.root, args.arg)
            if not rel or isinstance(rel, list):
                raise QOError(f'{args.arg}: ' + ('ambiguous: ' + ', '.join(rel[:6]) if rel else 'no such file under tools / probes / src'))
            r, pr = qtools.cmd_lanes_of(rel, args.root, lanes_for, load_lanes, scripts_naming), qtools.print_lanes_of
        elif args.cmd == 'covers':
            tf = qtools.find_file(args.root, args.arg)
            in_cpp = con.execute('SELECT 1 FROM files WHERE path = ?', (args.arg.replace('\\', '/'),)).fetchone()
            if tf and not isinstance(tf, list) and not in_cpp and not tf.endswith(('.h', '.hpp', '.cpp', '.inl', '.cc')):
                r, pr = qtools.cmd_lanes_of(tf, args.root, lanes_for, load_lanes, scripts_naming), qtools.print_lanes_of
            else:
                r, pr = cmd_covers(con, args.arg, args, args.root), print_covers
        else:
            fn, pr = table[args.cmd]
            if getattr(args, 'tools', False) or qtools.looks_scripty(args.root, args.arg, args.cmd):
                got = qtools.answer(args.cmd, args.arg, args, args.root)
                if got:
                    r, pr = got
                    if args.json:
                        print(json.dumps(r, indent=1, default=str))
                    else:
                        pr(r, args)
                    return 0
            if args.cmd in ('refs', 'def'):   # QX4: a literal address given for a symbol: the named constants that hold it (kCharSlotTable = 0x836F38D0), then their refs
                import qx6
                if qx6.is_addr_arg(args.arg):
                    hold = qx6.addr_holders(args.root, args.arg)
                    if hold:
                        names = []
                        for hn, hf, hl, htx in hold:
                            if hn not in names:
                                names.append(hn)
                        hr = {'address': args.arg, 'held_in': [{'name': a_, 'file': b_, 'line': c_, 'text': d_} for a_, b_, c_, d_ in hold], 'matches': []}
                        for hn in names[:12]:
                            try:
                                hr['matches'].append(fn(con, hn, args))
                            except QOError as e2:
                                hr['matches'].append({'symbol': hn, 'error': str(e2)})
                        if args.json:
                            print(json.dumps(hr, indent=1, default=str))
                            return 0
                        print(f'{args.arg} is held in {len(hold)} named constant(s) in {len({h[1] for h in hold})} file(s) (a `NAME = {args.arg}` line; `q ours lit {args.arg}` = every literal use):')
                        for hn, hf, hl, htx in hold:
                            print(f'  {hn}  {hf}:{hl}   {htx}')
                        for mr in hr['matches']:
                            if 'error' in mr:
                                print(f'  ({mr["symbol"]}: {mr["error"]})')
                            else:
                                pr(mr, args)
                        if len(names) > 12:
                            print(f'  ... {len(names) - 12} more constant name(s): q ours refs NAME')
                        return 0
            try:
                r = fn(con, args.arg, args)
            except QOError as e:
                msg = str(e)
                if args.cmd in ('def', 'refs', 'writes', 'reads') and 'no such symbol' in msg:   # QX5: a cvar is a macro, not a symbol
                    import qx5
                    if qx5.is_cvar_name(args.root, args.arg):
                        cr = qx5.cmd_cvar(args.root, args.arg)
                        tl = [f'{rf}:{d[3]} ({d[2]} {d[0]})' for rf, d in qtools.resolve(qtools.load(args.root), args.arg)[0]]
                        if args.json:
                            cr['also_tools_symbol'] = tl
                            print(json.dumps(cr, indent=1, default=str))
                        else:
                            print(f'q ours: ({args.arg} is a cvar: REXCVAR_DEFINE_*, not a C++ symbol)', file=sys.stderr)
                            if tl:
                                print(f'q ours: also a tools symbol: {"; ".join(tl[:4])}{" ..." if len(tl) > 4 else ""}  (--tools asks the tools python / ps1 index first)', file=sys.stderr)
                            qx5.print_cvar(cr, args)
                        return 0
                if fresh_info and 'no such symbol' in msg:   # QX1: the branch's own text first: a symbol it added / renamed is a real C++ answer
                    import qfresh
                    hits = qfresh.text_hits(args.root, fresh_info, args.arg)
                    if hits:
                        print(f"q ours: {args.arg}: not in the index (this branch added or renamed it); text matches in the branch's changed files ({len(hits)}):", file=sys.stderr)
                        for rel, ln, tx in hits:
                            print(f'  {rel}:{ln}: {tx}')
                        return 0
                    nn = qfresh.near_names(args.root, fresh_info, args.arg)
                    if nn:
                        print(f"q ours: {args.arg}: no word match in the branch's changed files; names there that contain it ignoring case / underscores:", file=sys.stderr)
                        for w, rel, ln in nn:
                            print(f'  {w}   {rel}:{ln}')
                        return 0
                # the tools index only when the C++ index has NO such symbol (a "found only as" error is a real C++ answer), or a dotted name / --tools
                dotted = '.' in args.arg and '::' not in args.arg
                if args.cmd in qtools.DISPATCH and ('no such symbol' in msg or (dotted and 'found only as' in msg)):
                    got = qtools.answer(args.cmd, args.arg, args, args.root)
                    if got:
                        r, pr = got
                        if args.json:
                            print(json.dumps(r, indent=1, default=str))
                        else:
                            print('q ours: (a script symbol: tools python / ps1 index)', file=sys.stderr)
                            pr(r, args)
                        return 0
                if args.cmd in ('def', 'refs') and 'no such symbol' in msg:   # QX3 / review fix 7: LAST fallback, after the branch text and the tools index: a shader block field (Param1)
                    import qx6
                    sr = qx6.shader_hits(args.root, args.arg)
                    if sr['n']:
                        if args.json:
                            print(json.dumps(sr, indent=1, default=str))
                        else:
                            qx6.print_shader_hits(sr, args.limit)
                        return 0
                raise
        android = []
        if args.cmd in ('refs', 'writes', 'reads') and isinstance(r, dict):   # QX5: writes inside #if __ANDROID__ branches (the AST index never compiles them)
            import qx5
            android = qx5.inactive_sites(args.root, args.arg, args.cmd)
        if args.json:
            if android:
                r['android_branches'] = android
            print(json.dumps(r, indent=1, default=str))
        else:
            pr(r, args)
            if android:
                import qx5
                qx5.print_inactive(android, args.cmd, args.limit)
        if fresh_info and fresh_info.get('approx'):
            print(f'q ours: ~approx: {fresh_info["approx"]} row lines sit in a region this branch rewrote and their name was not found there (lines are the region start)', file=sys.stderr)
        return 0
    except QOError as e:
        if getattr(args, 'json', False):
            print(json.dumps({'error': str(e)}))
        else:
            print(f'q ours: {e}', file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0


if __name__ == '__main__':
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    sys.exit(main(sys.argv[1:]))

"""Precompiled header for game_syntax's per-unit syntax check (S105 PCH; design + numbers: docs/research/s105/CM.md section 3).

src/pch_units.h (STL only, no Windows headers; S106 G4-4: the SAME file CMakeLists.txt force-includes into every plain game unit, so check == build) is precompiled once per (build flags, tree root, header closure, clang) and stored under
<syntax store>/pch/<key>.pch (own caps: GAME_SYNTAX_PCH_ENTRIES default 24, GAME_SYNTAX_PCH_MB default 2048; the .json store's caps are untouched). Atomic writes (temp + os.replace).
The key is content: sha256(`clang -E -P` of the header closure with the reference flags) + the placeholder-normalised flags + the clang id + the raw tree root (the .pch embeds absolute paths).
A unit uses the PCH ONLY when its flag list equals the reference list exactly (so clang's own flag check passes and the parse order is the key's) and it defines / undefines nothing
before its first #include (the PCH puts the STL first; a macro set before an STL include would change that include's meaning). Everything else skips the PCH and says so.
A failed PCH build, or a PCH clang rejects (a touched header: mtime / size), is rebuilt once; if that fails too the unit is parsed without the PCH (loud line) - never a pass without a parse.
The result-store key of a PCH unit is the `-E -P` text of the unit with `-include <src/pch_units.h>` (the exact text the parse sees); game_syntax does that. The caller owns --no-cache."""
import hashlib
import os
import re
import threading
import time

SALT = 'syntax-pch-v2'
BAD_RX = re.compile(r'precompiled header|precompiled file|between the precompiled|PCH file|AST file|has been modified since|malformed or corrupted|module file|was compiled with|different (?:target|language|command)', re.I)
_DIRECTIVE = re.compile(r'^\s*#\s*(\w+)')


def header_path(root):
    return os.path.join(os.path.abspath(root), 'src', 'pch_units.h').replace('\\', '/')


def pch_dir(store_dir):
    return os.path.join(store_dir, 'pch')


def unit_ok(args, ref_args, unit_path, force=False):
    """(True, '') or (False, reason). Quiet reasons start with '-' (not worth a line: main.cpp has its own PCH pair).
    force = the unit is a game unit the real build force-includes src/pch_units.h into (S106 G4-4F, review F4): the STL is ahead of everything in the unit there too, so a #define before its first #include
    changes nothing the build does not do, and the skip does not apply."""
    if '-include' in args or '-include-pch' in args:
        return False, '-unit has its own -include (main.cpp / pch_main.h)'
    if list(args) != list(ref_args):
        return False, 'its flags differ from the build\'s reference flags'
    if force:
        return True, ''
    try:
        with open(unit_path, 'r', encoding='utf-8', errors='replace') as f:
            for ln in f:
                m = _DIRECTIVE.match(ln)
                if not m:
                    continue
                d = m.group(1)
                if d == 'include':
                    break
                if d in ('define', 'undef'):
                    return False, 'it #%s-s something before its first #include (the PCH would put the STL ahead of that)' % d
    except OSError:
        return False, 'unit unreadable'
    return True, ''


def make_ref(root, ref_args, header=None):
    return {'args': list(ref_args), 'header': header or header_path(root), 'root': os.path.abspath(root).replace('\\', '/'),
            'path': None, 'tried': False, 'note': '', 'lock': threading.RLock(), 'builds': 0, 'gen': 0, 'dead': False, 'extra': [], 'build_s': 0.0}   # extra = flags of the PCH BUILD only (S110 SYNTAXPCH: -fpch-instantiate-templates for main.cpp's)


def _key(ref, run, clang, clang_id, norm, roots, timeout):
    pp = run([clang] + ref['args'] + ['-E', '-P', '-dD', '-w', '-x', 'c++-header', ref['header']], timeout=timeout, text=False)
    if pp.returncode != 0:
        return None, 'preprocess of %s failed: %s' % (ref['header'], (pp.stderr or b'')[:300] if isinstance(pp.stderr, str) else '')
    d = hashlib.sha256(pp.stdout)
    d.update(('\0'.join(norm(ref['args'], roots)) + '\0' + clang_id + '\0' + SALT + '\0' + ref['root'] + '\0' + ' '.join(ref['extra'])).encode())   # extra is in the key only when set: the unit PCHs' keys do not move
    return d.hexdigest(), ''


def _rm(path):
    try:
        os.remove(path)
    except OSError:
        pass


def ensure(ref, store_dir, run, clang, clang_id, norm, roots, timeout, force=False, seen_gen=None):
    """(path or None, gen): the .pch of _ensure and the generation that path belongs to, read together under the ref's lock (S106 G4-4F m1: a worker reading ref['gen'] afterwards could see another worker's newer build)."""
    with ref['lock']:
        p = _ensure(ref, store_dir, run, clang, clang_id, norm, roots, timeout, force, seen_gen)
        return p, ref['gen']


def _ensure(ref, store_dir, run, clang, clang_id, norm, roots, timeout, force=False, seen_gen=None):
    """The path of a good .pch for `ref`, building it if the store lacks it (once per ref per process; the lock serialises the pool's workers). None = no PCH (ref['note'] says why).
    S106 G4-4 (review_PCH 4 / 5): force = a parse rejected the .pch; it rebuilds only when ref['gen'] is still `seen_gen` (no other worker rebuilt it since that parse's .pch was handed out), else it
    returns the newer file. A file another run's prune removed is rebuilt once (note says so); a file another process finished first is used (never os.replace over a .pch a clang has open)."""
    with ref['lock']:
        if ref.get('dead'):
            return None
        if ref['path'] and os.path.isfile(ref['path']) and (not force or (seen_gen is not None and seen_gen != ref['gen'])):
            return ref['path']
        if ref['path'] and not os.path.isfile(ref['path']) and not force:
            if ref.get('evictions', 0) >= 1:
                ref['note'] = 'the PCH file vanished twice (a prune by another run, or a cleaner, removed it)'
                return None
            ref['evictions'] = ref.get('evictions', 0) + 1
            ref['note'] = 'the PCH file was evicted by another run; rebuilt once'
        elif ref['tried'] and not force and not ref['path']:
            return None
        ref['tried'] = True
        ref['path'] = None
        key, why = _key(ref, run, clang, clang_id, norm, roots, timeout)
        if key is None:
            ref['note'] = why
            return None
        d = pch_dir(store_dir)
        p = os.path.join(d, key + '.pch')
        if force:
            _rm(p)   # removing works while a clang has it open; the other workers' parses that open it in the gap are rejected and rebuild through seen_gen once
        if os.path.isfile(p):
            try:
                os.utime(p, None)
            except OSError:
                pass
            ref['path'] = p.replace('\\', '/')
            ref['gen'] += 1
            return ref['path']
        tmp = '%s.%d.%d.tmp' % (p, os.getpid(), threading.get_ident())
        try:
            os.makedirs(d, exist_ok=True)
        except OSError as e:
            ref['note'] = 'cannot create %s: %s' % (d, e)
            return None
        t_b = time.time()
        r = run([clang] + ref['args'] + ref['extra'] + ['-w', '-x', 'c++-header', ref['header'], '-o', tmp], timeout=timeout)
        ref['builds'] += 1
        ref['build_s'] += time.time() - t_b
        if r.returncode != 0 or getattr(r, 'timed_out', False) or not os.path.isfile(tmp):
            ref['note'] = 'PCH build failed (rc %s): %s' % (r.returncode, ((r.stdout or '') + (r.stderr or ''))[:400].replace('\n', ' | '))
            _rm(tmp)
            return None
        for _ in range(25):
            if os.path.isfile(p):   # another process finished the same key first (os.replace over a file a clang has open fails): use its file
                _rm(tmp)
                break
            try:
                os.replace(tmp, p)
                break
            except OSError:
                time.sleep(0.1)
        else:
            _rm(tmp)
            ref['note'] = 'could not move the built PCH into place'
            return None
        ref['path'] = p.replace('\\', '/')
        ref['gen'] += 1
        prune(store_dir, keep=p)
        return ref['path']


def prune(store_dir, keep=None, max_entries=None, cap_mb=None):
    """Oldest .pch first (LRU by mtime) while over the entry or size cap; never `keep`; stale temp files over an hour old go. Returns the number removed."""
    if max_entries is None:
        try:
            max_entries = int(os.environ.get('GAME_SYNTAX_PCH_ENTRIES', '24'))
        except ValueError:
            max_entries = 24
    if cap_mb is None:
        try:
            cap_mb = float(os.environ.get('GAME_SYNTAX_PCH_MB', '2048'))
        except ValueError:
            cap_mb = 2048.0
    items, total = [], 0
    try:
        for e in os.scandir(pch_dir(store_dir)):
            try:
                st = e.stat()
            except OSError:
                continue
            if e.name.endswith('.tmp'):
                if time.time() - st.st_mtime > 3600:
                    try:
                        os.remove(e.path)
                    except OSError:
                        pass
                continue
            if e.name.endswith('.pch'):
                items.append((st.st_mtime, st.st_size, e.path))
                total += st.st_size
    except OSError:
        return 0
    items.sort()
    n, left = 0, len(items)
    for _m, size, path in items:
        if total <= cap_mb * 1048576 and left <= max_entries:
            break
        if keep and os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(keep)):
            continue
        try:
            os.remove(path)
            total -= size
            left -= 1
            n += 1
        except OSError:
            pass
    return n

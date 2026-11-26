r"""S105 H: the cvars that exist only in the stream (ON) build, read from the code (never a typed list).

A route made for the stream-free exe (run_list -Exe ...\win-amd64-release-off\wwe2k14.exe, or a shipped player exe) that carries a
`cvar NAME ...` line for one of these is refused by that exe ("cvar ... was not set") and the route aborts at that line (S105 sweep runs
045 / 047 / 048 / 052). A --NAME command-line flag is NOT refused (the SDK stashes unknown options): the switch is silently dropped.
Source of truth: every REXCVAR_DEFINE_* in our src\, tagged by the same build logic as `py tools\q\q.py ours cvar NAME` (tools\q\qx5.py:
the line's #if WWE2K14_STREAM() chain plus the file's build). A name is stream-only when EVERY definition of it is ON-only; one definition
that also compiles in the OFF build (or an OFF twin) means the stream-free exe has it.
The scan is cached in out\harness\stream_cvars_cache.json (key: newest mtime under src\ and the two keep lists); qx5 is imported lazily.
"""
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))

STREAM_FREE_EXE_RX = re.compile(r'release-off|[\\/]win-amd64-[\w-]*-off[\\/]', re.I)   # the OFF build folder; shipped player exes are tested by route_lint.is_release
ROUTE_CVAR_RX = re.compile(r'^\s*cvar\s+(\S+)', re.M)
INCLUDE_RX = re.compile(r'^\s*include\s+("?)([^"#\r\n]+?)\1\s*(?:#.*)?$', re.M)
_CACHE = {}


def _own_keep_stream(exe):
    """S111: what the exe's OWN folder says it was built as: 'OFF' / 'ON' from exe_info.json options (a lane exe) or CMakeCache.txt (an out\\build folder), else ''. Never the folder's name: Jarrod's
    play folder out\\build\\win-amd64-release is stream-free since 2026-11-26 and a name test called it the stream build."""
    d = os.path.dirname(os.path.abspath(exe))
    try:
        with open(os.path.join(d, 'exe_info.json'), encoding='utf-8') as fh:
            v = str(((json.load(fh) or {}).get('options') or {}).get('WWE2K14_KEEP_STREAM', '')).upper()
        if v in ('ON', 'OFF'):
            return v
    except (OSError, ValueError, AttributeError):
        pass
    try:
        with open(os.path.join(d, 'CMakeCache.txt'), encoding='utf-8', errors='replace') as fh:
            m = re.search(r'^WWE2K14_KEEP_STREAM:BOOL=(\w+)', fh.read(), re.M)
        return m.group(1).upper() if m else ''
    except OSError:
        return ''


def is_stream_free_exe(exe):
    if not exe:
        return False
    own = _own_keep_stream(exe)
    if own:
        return own == 'OFF'
    return bool(STREAM_FREE_EXE_RX.search(exe))


def _fingerprint(root):
    """newest mtime (ns) over src\\ and the two keep lists: the cache key."""
    newest = 0
    for d, _dirs, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            try:
                newest = max(newest, os.stat(os.path.join(d, f)).st_mtime_ns)
            except OSError:
                pass
    for k in ('keep_stream_sources.txt', 'keep_stream_off_sources.txt'):
        try:
            newest = max(newest, os.stat(os.path.join(root, 'tools', 'scene', k)).st_mtime_ns)
        except OSError:
            pass
    return str(newest)


def stream_only_cvars(root):
    """{name: 'file:line'} of the cvars defined only in the ON build (ours; the SDK's are in both builds)."""
    root = os.path.abspath(root)
    if root in _CACHE:
        return _CACHE[root]
    cache_file = os.path.join(root, 'out', 'harness', 'stream_cvars_cache.json')
    key = _fingerprint(root)
    try:   # without the cache: 4 s warm / 25 s cold per stream-free run, with the run lock held
        with open(cache_file, encoding='utf-8') as fh:
            c = json.load(fh)
        if c.get('key') == key:
            _CACHE[root] = c['cvars']
            return _CACHE[root]
    except (OSError, ValueError, KeyError):
        pass
    sys.path.insert(0, os.path.join(_HERE, '..', 'q'))
    import qx5   # lazy: a broken q module must not break every run (route_lint turns the failure into a WARN)
    ft = qx5.FileTags(root)
    per = {}   # name -> [is_on_only, ...]
    where = {}
    for p in qx5.walk_files(root, qx5.CPP, ('src',)):
        text = qx5.read_text(p)
        if 'REXCVAR_DEFINE' not in text:
            continue
        rel = qx5.rel_of(root, p)
        cm = qx5.mask_comments(text)
        cml = cm.split('\n')
        conds = qx5.pp_conds(cml)
        ftags = set(ft.tags(rel))
        for m, ln in qx5.def_matches(cm, cml):
            tg = qx5.tags_of(conds[ln - 1]) | ftags
            on_only = 'stream' in tg and 'stream-free' not in tg
            per.setdefault(m.group(2), []).append(on_only)
            where.setdefault(m.group(2), f'{rel}:{ln}')
    out = {n: where[n] for n, v in per.items() if v and all(v)}
    _CACHE[root] = out
    try:
        os.makedirs(os.path.dirname(cache_file), exist_ok=True)
        with open(cache_file, 'w', encoding='utf-8') as fh:
            json.dump({'key': key, 'cvars': out}, fh)
    except OSError:
        pass
    return out


def route_cvar_names(text):
    """every cvar a route sets live: its `cvar NAME` lines."""
    return ROUTE_CVAR_RX.findall(text)


def route_cvar_names_deep(path, depth=0, seen=None):
    """`cvar NAME` lines of a route AND of the routes it `include`s (beside it, in nav\\, or in tools\\input_scripts), like src\\input_script.h FindIncludeFile."""
    seen = set() if seen is None else seen
    path = os.path.abspath(path)
    if path in seen or depth > 8:
        return []
    seen.add(path)
    try:
        text = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return []
    names = route_cvar_names(text)
    here = os.path.dirname(path)
    base = here
    while os.path.basename(base) != 'input_scripts' and os.path.dirname(base) != base:
        base = os.path.dirname(base)
    for m in INCLUDE_RX.finditer(text):
        nm = m.group(2).strip()
        found = None
        for d in (here, os.path.join(base, 'nav'), base):
            for cand in (nm, nm + '.txt'):
                f = os.path.join(d, cand)
                if os.path.isfile(f):
                    found = f
                    break
            if found:
                break
        if found:
            names += route_cvar_names_deep(found, depth + 1, seen)
    return names

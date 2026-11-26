#!/usr/bin/env python3
"""syntax_cfg: game_syntax's own configure-only flags tree (S106 GSMAIN, docs/research/s106/GSMAIN.md).

game_syntax used to read its compile flags from the MAIN checkout's play folder (out\\build\\win-amd64-release[-off]\\build.ninja). When the checked tree's CMakeLists.txt
differed (or main's build.ninja was older than it) every `--changed` run exited 4 (NEEDS-MAIN). Now the flags come from a CONFIGURE-ONLY CMake run on the tree being checked:

  * a placeholder source folder: an EMPTY file for every tracked + untracked path of the tree under src\\ assets\\ third_party\\ (+ game_logo.ico); REAL copies of the files configure
    reads (CMakeLists.txt, CMakePresets.json, VERSION, generated/rexglue.cmake, the opus lists, the two keep_stream lists); generated/default/sources.cmake copied from MAIN;
    empty placeholders for the imgui sources / streamline sl.h CMakeLists names when MAIN has them (presence changes flags);
  * `cmake -S <placeholder tree> -B <key>\\<build>` with main's own cache entries (compilers, RC compiler, SDK dir, every WWE2K14_* option) and the preset's cacheVariables
    (resolved here, never `cmake --preset`: that would put the build in the play folder). Configure runs no codegen and writes only into the new build folder;
  * `ninja -t commands` there, then the placeholder root is rewritten to the tree's real roots by game_syntax's own flag handling (the root is replaced by MAIN's root, exactly
    like the old main build.ninja lines, so flags_for / tree_args / with_pch work unchanged).

Key = sha256 over the configure inputs (the bytes above, the placeholder path list, main's presence bits and cache entries, the resolved presets, cmake's version, the SDK's *.cmake):
a hit costs ~0 s, a fresh tree about 5 s (8 try_compile probes). Store: <main>\\out\\harness\\syntax_cfg\\<key>\\{src,on,off} (SYNTAX_CFG_DIR overrides); one lock per key, a `.done`
marker written last. It never touches out\\build, generated\\ or the run lock (compile queue only).

Notes (S106 GSMAINF): the WWE2K14_* cache values of MAIN's build win over the tree's own option() defaults (this matches main's incremental build, not a fresh configure of the tree); the
placeholder tree has no binary-dir `stamp` include folder (build_stamp.cpp's generated stamp header is behind __has_include, so its flags are unaffected; game_syntax drops the folder anyway);
the store is <main>\\out\\harness\\syntax_cfg (rebuilt in ~5 s, so a clear of out\\ costs one configure). SYNTAX_CFG_OVERRIDE_JSON replaces tree files (selftest twins) and prints OVERRIDE ACTIVE
whenever it is set.

Failure kinds: cmake ran and the tree's CMakeLists does not configure = ConfigureFailed (a real FAIL with cmake's last lines, never a pass); cmake / ninja / main's cache missing =
Unavailable (game_syntax falls back to main's out\\build with a loud note).

  py -I tools\\compile\\syntax_cfg.py [--root DIR] [--build on|off] [--show UNIT]     prints the key, configure seconds (fresh | hit) and the command count
"""
# entry: commands
import argparse
import contextlib
import ctypes
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc as _proc   # every child under a Job Object + hard timeout
sys.path.append(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'admit'))
import admit_py as _admit   # the machine-wide compile queue

T_GIT, T_CFG, T_NINJA, T_VER = 60, 300, 120, 30
PRESETS = {'on': 'win-amd64-release', 'off': 'win-amd64-release-off'}
# What makes a variant that variant: set last, over main's cache AND the preset. Main's play folder (out\build\win-amd64-release) is Jarrod's stream-free player build since
# S111 (2026-11-26), so its cache says KEEP_STREAM=OFF; without this the 'on' check configured stream-free. Main's cache still supplies compilers / SDK / the other options.
VARIANT = {'on': {'WWE2K14_KEEP_STREAM': 'ON'},
           'off': {'WWE2K14_KEEP_STREAM': 'OFF', 'WWE2K14_KEEP_STREAM_RUN': 'ON'}}
OVERRIDE_ENV = 'SYNTAX_CFG_OVERRIDE_JSON'   # selftest planted twins: {"CMakeLists.txt": "<path of a file holding its replacement bytes>"} (part of the key)
# the in-repo files configure READS (build.ninja's RERUN_CMAKE deps + the two file(STRINGS) / presets readers, GSMAIN E5): real copies, everything else is an empty placeholder
REAL_INPUTS = ('CMakeLists.txt', 'CMakePresets.json', 'VERSION', 'generated/rexglue.cmake', 'third_party/audio/opus/opus_sources.cmake',
               'third_party/audio/opus/sources.txt', 'third_party/libwebp/libwebp.cmake', 'tools/scene/keep_stream_sources.txt', 'tools/scene/keep_stream_off_sources.txt')
LIST_DIRS = ('src', 'assets', 'third_party', 'probes', 'game_logo.ico')   # probes/: the wwe2k14_probes target's sources (CMakeLists.txt add_library)
SOURCES_CMAKE = 'generated/default/sources.cmake'
SL_H = 'thirdparty/streamline/sdk/include/sl.h'
IMGUI_DIR = 'thirdparty/rexglue-sdk-src/thirdparty/imgui'
CARRY = ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_RC_COMPILER', 'CMAKE_AR', 'CMAKE_LINKER', 'CMAKE_MAKE_PROGRAM', 'rexglue_DIR', 'CMAKE_BUILD_TYPE',
         'CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_C_FLAGS_RELEASE', 'CMAKE_CXX_FLAGS_RELEASE')

STATS = {'fresh': [], 'hit': 0, 'last': None}   # for game_syntax's note line
_HOOKS = {}   # selftest only (S106 GSMAINF N1): a callable run right after the hit check, to simulate another process's prune


def _hook(name):
    h = _HOOKS.get(name)
    if h:
        h()


class Unavailable(RuntimeError):
    """cmake / ninja / main's cache missing: the caller may fall back to main's out\\build, saying so."""


class ConfigureFailed(RuntimeError):
    """cmake ran and this tree does not configure (or configure read a file syntax_cfg gave as a placeholder): a real FAIL."""


def _git(root, *args):
    r = _proc.run(['git', '-C', root] + list(args), timeout=T_GIT, text=False)
    return r.returncode, (r.stdout or b'')


class WorkTree:
    """The checked tree: path list from git (tracked + untracked, not ignored), file bytes from disk. `overrides` ({rel: bytes}) replaces a file (planted twins)."""

    def __init__(self, root, overrides=None):
        self.root = os.path.abspath(root)
        self.overrides = dict(overrides or {})
        if not self.overrides and os.environ.get(OVERRIDE_ENV):
            print('OVERRIDE ACTIVE: %s replaces tree files (selftest planted twins only; a real run must not set it)' % OVERRIDE_ENV)
            for rel, p in json.load(open(os.environ[OVERRIDE_ENV], encoding='utf-8')).items():
                self.overrides[rel] = open(p, 'rb').read()

    def paths(self):
        rc, out = _git(self.root, 'ls-files', '-co', '--exclude-standard', '-z', '--', *LIST_DIRS)
        if rc:
            raise Unavailable('git ls-files failed in %s (not a git tree?)' % self.root)
        ps = [p for p in out.decode('utf-8', 'replace').split('\0') if p and os.path.isfile(os.path.join(self.root, p))]
        return sorted(set(ps))

    def read(self, rel):
        if rel in self.overrides:
            return self.overrides[rel]
        try:
            return open(os.path.join(self.root, rel), 'rb').read()
        except OSError:
            return None


def parse_cache(path):
    d = {}
    try:
        for ln in open(path, encoding='utf-8', errors='replace'):
            m = re.match(r'^([A-Za-z0-9_\-+.]+):([A-Z]+)=(.*)$', ln.rstrip('\n'))
            if m:
                d[m.group(1)] = (m.group(2), m.group(3))
    except OSError:
        return None
    return d


UNSUPPORTED_PRESET_KEYS = ('toolchainFile', 'environment', 'architecture', 'toolset')   # forms whose effect configure here would not apply: refused, never ignored


def resolve_preset(presets_bytes, name):
    """cacheVariables of a configure preset with `inherits` merged (parents first, the child wins). A preset using a form this resolver does not apply (toolchainFile, environment, architecture,
    toolset, a generator other than Ninja, a macro in a cache value) is refused with ConfigureFailed (S106 GSMAINF N11)."""
    try:
        j = json.loads((presets_bytes or b'').decode('utf-8'))
    except ValueError as e:
        raise ConfigureFailed('CMakePresets.json is not valid JSON: %s' % e)
    by = {p['name']: p for p in j.get('configurePresets', [])}

    def rec(n, seen=()):
        if n not in by or n in seen:
            raise ConfigureFailed('CMakePresets.json: no configure preset %r (or an inherits loop)' % n)
        p, out = by[n], {}
        for k in UNSUPPORTED_PRESET_KEYS:
            if k in p:
                raise ConfigureFailed('CMakePresets.json: preset %r uses %s, which syntax_cfg does not apply (it would give flags the real build does not have): support it in tools/compile/syntax_cfg.py' % (n, k))
        if p.get('generator', 'Ninja') != 'Ninja':
            raise ConfigureFailed('CMakePresets.json: preset %r uses generator %r (syntax_cfg configures with Ninja only)' % (n, p.get('generator')))
        inh = p.get('inherits', [])
        for par in ([inh] if isinstance(inh, str) else inh):
            out.update(rec(par, seen + (n,)))
        for k, v in (p.get('cacheVariables') or {}).items():
            if isinstance(v, dict):
                v = v.get('value', '')
            out[k] = 'ON' if v is True else 'OFF' if v is False else str(v)
            if '$' in out[k]:
                raise ConfigureFailed('CMakePresets.json: preset %r cache variable %s = %r uses a macro, which syntax_cfg does not expand' % (n, k, out[k]))
        return out
    return rec(name)


def check_compiler(preset_args, cache):
    """The compilers come from MAIN's cache (full paths). A preset naming a different compiler (by name, ignoring the path and .exe) would be silently overridden: refuse it."""
    def stem(v):
        return re.sub(r'\.exe$', '', os.path.basename(str(v).replace('\\', '/')), flags=re.I).lower()
    for k in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER'):
        if k in preset_args and k in cache and stem(preset_args[k]) != stem(cache[k][1]):
            raise ConfigureFailed('the preset names %s = %s but main\'s cache has %s: syntax_cfg would silently keep main\'s compiler (use main\'s build, or support it here)' % (k, preset_args[k], cache[k][1]))


def _cmake_exe(cache):
    c = (cache or {}).get('CMAKE_COMMAND')
    for p in ([c[1]] if c else []) + [shutil.which('cmake') or '']:
        if p and os.path.isfile(p):
            return p
    hits = glob.glob(r'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
    if hits:
        return hits[0]
    raise Unavailable('cmake.exe not found (main\'s cache, PATH, the Visual Studio copy)')


def _ninja_exe(cache):
    c = (cache or {}).get('CMAKE_MAKE_PROGRAM')
    for p in ([c[1]] if c else []) + [shutil.which('ninja') or '']:
        if p and os.path.isfile(p):
            return p
    raise Unavailable('ninja.exe not found (main\'s cache, PATH)')


class RevTree(WorkTree):
    """S106 GSN N7: a COMMITTED tree (git rev `rev` of `root`): path list from git ls-tree, bytes from git show. game_syntax compares its configure with the checked tree's to find the units a
    CMake-input change moves. No overrides (SYNTAX_CFG_OVERRIDE_JSON is for the checked tree only)."""

    def __init__(self, root, rev):
        self.root = os.path.abspath(root)
        self.rev = rev
        self.overrides = {}

    def paths(self):
        rc, out = _git(self.root, 'ls-tree', '-r', '--name-only', '-z', self.rev, '--', *LIST_DIRS)
        if rc:
            raise Unavailable('git ls-tree %s failed in %s' % (self.rev, self.root))
        return sorted(p for p in out.decode('utf-8', 'replace').split('\0') if p)

    def read(self, rel):
        rc, out = _git(self.root, 'show', '%s:%s' % (self.rev, rel))
        return out if rc == 0 else None


def cfg_root(main_dir):
    return os.environ.get('SYNTAX_CFG_DIR') or os.path.join(main_dir, 'out', 'harness', 'syntax_cfg')


def _imgui_files(cmake_text):
    return sorted(set(re.findall(r'\$\{WWE2K14_IMGUI\}/([\w./]+\.cpp)', cmake_text)))


def plan(tree, main_dir, build):
    """Everything configure depends on: (key, dict). Raises Unavailable for a missing main input."""
    cache_p = os.path.join(main_dir, 'out', 'build', PRESETS[build], 'CMakeCache.txt')
    cache = parse_cache(cache_p)
    if cache is None:
        raise Unavailable('main has no %s (never configured): no cache entries to carry' % cache_p)
    sources_cmake = _read(os.path.join(main_dir, SOURCES_CMAKE))
    if sources_cmake is None:
        raise Unavailable('main has no %s (codegen never ran there)' % SOURCES_CMAKE)
    cmake_exe = _cmake_exe(cache)
    ver = _proc.run([cmake_exe, '--version'], timeout=T_VER)
    real = {rel: tree.read(rel) for rel in REAL_INPUTS}   # S106 GSMAINF N10: read once; the key, the compiler-free scans and materialise all use these bytes
    cmake_txt = (real['CMakeLists.txt'] or b'').decode('utf-8', 'replace')
    imgui = _imgui_files(cmake_txt) if os.path.isfile(os.path.join(main_dir, IMGUI_DIR, 'imgui.cpp')) else []
    has_sl = os.path.isfile(os.path.join(main_dir, SL_H))
    preset = resolve_preset(real['CMakePresets.json'], PRESETS[build])
    check_compiler(preset, cache)
    args = {}
    for k, (_t, v) in cache.items():
        if k in CARRY or k.startswith('WWE2K14_'):
            args[k] = v
    for k, v in preset.items():   # the preset wins, except a bare compiler name main's cache has resolved to a full path
        if k in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER') and k in args:
            continue
        args[k] = v
    args.update(VARIANT[build])   # S111: the variant's own identity, never main's cache (the play folder became stream-free on 2026-11-26 and its OFF reached the 'on' flags)
    sdk = args.get('rexglue_DIR', '')
    sdk_bytes = [(os.path.basename(p), _read(p) or b'') for p in sorted(glob.glob(os.path.join(sdk, '*.cmake')))] if sdk else []
    h = hashlib.sha256()
    h.update(('build=%s\n' % build).encode())
    for rel in REAL_INPUTS:
        b = real[rel]
        h.update(b'F:' + rel.encode() + b'\0' + (b'<absent>' if b is None else hashlib.sha256(b).digest()) + b'\n')
    h.update(b'main-sources.cmake\0' + hashlib.sha256(sources_cmake).digest())
    paths = tree.paths()
    h.update(b'paths\0' + hashlib.sha256('\n'.join(paths).encode()).digest())
    h.update(('imgui=%s sl=%d\n' % (','.join(imgui), has_sl)).encode())
    h.update(json.dumps(args, sort_keys=True).encode())
    h.update((ver.stdout or '').encode())
    h.update(cmake_exe.lower().encode())
    for n, b in sdk_bytes:
        h.update(n.encode() + b'\0' + hashlib.sha256(b).digest())
    return h.hexdigest()[:24], dict(cache=cache, cmake=cmake_exe, ninja=_ninja_exe(cache), args=args, build=build, paths=paths, imgui=imgui, has_sl=has_sl, real=real, sources_cmake=sources_cmake)


def _read(p):
    try:
        return open(p, 'rb').read()
    except OSError:
        return None


if os.name == 'nt':
    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _k32.OpenProcess.restype = ctypes.c_void_p
    _k32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    _k32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    _k32.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
    _k32.CloseHandle.argtypes = [ctypes.c_void_p]


def _start_ticks(pid):
    """None = no such process; 0 = running but its start time is unreadable; else the process creation time (FILETIME)."""
    if os.name != 'nt':
        try:
            os.kill(pid, 0)
            return 0
        except OSError:
            return None
    h = _k32.OpenProcess(0x1000, 0, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None if ctypes.get_last_error() == 87 else 0   # 87 = no such process; access denied = running
    try:
        code = ctypes.c_uint32()
        if _k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value != 259:   # 259 = STILL_ACTIVE
            return None
        c, e, k, u = (ctypes.c_uint64() for _ in range(4))
        return c.value if _k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)) else 0
    finally:
        _k32.CloseHandle(h)


def _lock_record():
    return '%d %d' % (os.getpid(), _start_ticks(os.getpid()) or 0)


def _holder_dead(text, age):
    """True only when the process that wrote the lock is gone (pid dead, or the pid reused: a different start time). Never by age, except a lock file that is still EMPTY / unreadable after 30 s
    (its writer died between creating and writing it: a write takes microseconds)."""
    parts = text.split()
    try:
        pid = int(parts[0])
    except (IndexError, ValueError):
        return age > 30
    st = _start_ticks(pid)
    if st is None:
        return True
    try:
        rec = int(parts[1]) if len(parts) > 1 else 0
    except ValueError:
        rec = 0
    return bool(st and rec and st != rec)


def _steal(path, seen):
    """Take a dead holder's lock out of the way: rename it aside (atomic), and put it back if what we moved is not what we judged dead (a fresh lock won the race)."""
    aside = '%s.dead.%d' % (path, os.getpid())
    try:
        os.replace(path, aside)
    except OSError:
        return
    got = _read(aside)
    if got is not None and got.decode('utf-8', 'replace') != seen:
        try:
            os.rename(aside, path)
        except OSError:
            pass
        return
    try:
        os.remove(aside)
    except OSError:
        pass


@contextlib.contextmanager
def _lock(path, timeout=900):
    """One lock file per key. Stale only when its holder process is dead (pid + start time), never by how old it is: a configure that waits on the compile queue can legitimately hold it
    for minutes (S106 GSMAINF N2)."""
    t0 = time.time()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mine = _lock_record()
    while True:
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, mine.encode())
            os.close(fd)
            break
        except FileExistsError:
            try:
                age = time.time() - os.path.getmtime(path)
            except OSError:
                continue
            raw = _read(path)
            if raw is not None and _holder_dead(raw.decode('utf-8', 'replace'), age):
                _steal(path, raw.decode('utf-8', 'replace'))
                continue
            if time.time() - t0 > timeout:
                raise Unavailable('syntax_cfg lock %s held for %d s by a live process' % (path, timeout))
            time.sleep(0.5)
    try:
        yield
    finally:
        got = _read(path)
        if got is not None and got.decode('utf-8', 'replace') == mine:
            try:
                os.remove(path)
            except OSError:
                pass


def _rmtree(path, base):
    """Delete a folder that is inside the cfg store only."""
    ap, ab = os.path.abspath(path), os.path.abspath(base)
    if os.path.commonpath([ap, ab]) != ab or ap == ab:
        raise RuntimeError('refusing to delete %s (outside %s)' % (ap, ab))
    shutil.rmtree(ap, ignore_errors=True)


def materialise(tree, main_dir, pl, src):
    os.makedirs(src, exist_ok=True)
    made = set()

    def touch(rel):
        p = os.path.join(src, rel)
        d = os.path.dirname(p)
        if d not in made:
            os.makedirs(d, exist_ok=True)
            made.add(d)
        open(p, 'wb').close()
    for rel in pl['paths']:
        touch(rel)
    for f in pl['imgui']:
        touch(IMGUI_DIR + '/' + f)
    if pl['has_sl']:
        touch(SL_H)
    for rel in REAL_INPUTS:
        b = pl['real'].get(rel)
        if b is not None:
            p = os.path.join(src, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(b)
    p = os.path.join(src, SOURCES_CMAKE)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(pl['sources_cmake'])


# ---- N5: what CMake reads from the tree must be a REAL copy (or a placeholder that is enough), checked statically so a future reader cannot silently get an empty file
OUTSIDE_VARS = {   # S106 GSN: var -> (the read kinds that are fine through it, why); any other kind of read through it is a problem
    'WWE2K14_FFX_DLL': (('exists',), 'a file outside the tree: only its presence is tested (a copy step)'),
    'WWE2K14_ADRENOTOOLS': (('exists', 'read'), 'a folder outside the tree (Android): presence + add_subdirectory'),
    'REXSDK_DIR': (('read',), 'the SDK, outside the tree: add_subdirectory'),
    'png': (('exists',), 'EXISTS on an assets/launcher PNG: assets/ is mirrored as placeholder files, only presence is read')}
KNOWN_EXEC = {'git': 'commit stamp for the exe VERSIONINFO: not a compile flag (build_stamp.cpp reads it behind __has_include)'}   # execute_process commands that may run on the placeholder tree
KNOWN_NO_FLAG = {'generated/default/dll_targets.cmake': 'EXISTS + include of a codegen file main has no copy of in the store: adds no compile flags to the game units',
                 'metadata/icons': 'EXISTS on a folder (exe icon resources): no compile flags'}
_ROOT_PREFIXES = ('${CMAKE_CURRENT_SOURCE_DIR}/', '${CMAKE_SOURCE_DIR}/', '${PROJECT_SOURCE_DIR}/')
_LIST_DIR_PREFIXES = ('${CMAKE_CURRENT_LIST_DIR}/', '${CMAKE_CURRENT_FUNCTION_LIST_DIR}/')   # relative to the folder of the .cmake file that names them (S106 GSN)


def _strip_comments(text):
    return re.sub(r'(?m)(^\s*#.*$)|(\s#[^"\n]*$)', '', text)


def _calls(text, names):
    """(name, [args]) of every `name(...)` call in CMake text (nested parentheses balanced, quoted args unquoted)."""
    out = []
    for m in re.finditer(r'\b(%s)\s*\(' % '|'.join(names), text, re.I):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {'(': 1, ')': -1}.get(text[i], 0)
            i += 1
        body = text[m.end():i - 1]
        out.append((m.group(1).lower(), [a or b for a, b in re.findall(r'"((?:[^"\\]|\\.)*)"|([^\s")]+)', body)]))
    return out


def _classify(path, kind, base=''):
    """None = fine, else the reason it is a problem. `base` = the folder (repo-relative) of the file that names the path: what ${CMAKE_CURRENT_LIST_DIR} means there."""
    p = path.replace('\\', '/')
    for pre in _ROOT_PREFIXES:
        if p.startswith(pre):
            p = p[len(pre):]
            break
    else:
        for pre in _LIST_DIR_PREFIXES:
            if p.startswith(pre):
                p = (base + '/' if base else '') + p[len(pre):]
                break
    m = re.match(r'\$\{(\w+)\}/?(.*)$', p)
    if m:
        var, rest = m.groups()
        if var in OUTSIDE_VARS:
            kinds, _why = OUTSIDE_VARS[var]
            return None if kind in kinds else 'a %s through ${%s}, which is classified for %s only (%s)' % (kind, var, '/'.join(kinds), _why)
        if var == 'WWE2K14_IMGUI':
            p = IMGUI_DIR + '/' + rest
        elif var == '_dir':   # third_party/audio/opus/opus_sources.cmake: set(_dir ${CMAKE_CURRENT_LIST_DIR})
            p = (base + '/' if base else 'third_party/audio/opus/') + rest
        else:
            return 'a path led by the variable ${%s}: classify it in OUTSIDE_VARS (outside the tree) or mirror what it reads' % var
    if '${' in p:
        return 'a path with a variable (%s): resolve it or classify it' % path
    p = p.rstrip('/')
    if p in KNOWN_NO_FLAG:
        return None
    real = set(REAL_INPUTS) | {SOURCES_CMAKE}
    if p in real:
        return None
    if kind in ('exists', 'glob') and (p == SL_H or p.startswith(IMGUI_DIR) or any(p == d or p.startswith(d + '/') for d in LIST_DIRS)):
        return None   # a placeholder file / folder exists for every tracked path there: presence is all that is read
    return '%s is read by CMake but is not in REAL_INPUTS (it would be an EMPTY placeholder): add it to REAL_INPUTS or mirror it' % p


_FILE_READS = ('SIZE', 'TIMESTAMP', 'MD5', 'SHA1', 'SHA224', 'SHA256', 'SHA384', 'SHA512', 'SHA3_224', 'SHA3_256', 'SHA3_384', 'SHA3_512', 'REAL_PATH', 'READ_SYMLINK', 'COPY_FILE', 'RENAME', 'ARCHIVE_EXTRACT')


def scan_texts(texts):
    """Problems for {rel: CMake text}: every file(READ|STRINGS|GLOB) / configure_file / include / add_subdirectory / if(EXISTS) path must be a real copy or a placeholder that is enough."""
    probs = []
    for rel, text in sorted(texts.items()):
        t = _strip_comments(text)
        base = os.path.dirname(rel).replace('\\', '/')
        for name, args in _calls(t, ('file', 'configure_file', 'include', 'add_subdirectory', 'execute_process')):
            sub = args[0].upper() if args else ''
            if name == 'execute_process':   # S106 GSN: fails closed
                cmd = args[args.index('COMMAND') + 1] if 'COMMAND' in args and args.index('COMMAND') + 1 < len(args) else ''
                if cmd not in KNOWN_EXEC:
                    probs.append('%s: execute_process(%s ...): runs on the placeholder tree: add the command to KNOWN_EXEC (with why it adds no compile flag) or mirror what it reads' % (rel, cmd or '?'))
                continue
            if name == 'file' and sub in ('READ', 'STRINGS') and len(args) > 1:
                paths, kind = [args[1]], 'read'
            elif name == 'file' and sub in ('GLOB', 'GLOB_RECURSE'):
                paths, kind = [a for a in args[2:] if '/' in a or '*' in a], 'glob'
            elif name == 'file' and sub in _FILE_READS and len(args) > 1:   # S106 GSN: SIZE / TIMESTAMP / hashes / REAL_PATH ... read what a placeholder lacks
                paths, kind = [args[1]], 'read'
            elif name == 'file' and sub == 'COPY' and 'DESTINATION' in args:
                paths, kind = [a for a in args[1:args.index('DESTINATION')] if '/' in a], 'read'
            elif name == 'configure_file' and args:
                paths, kind = [args[0]], 'read'
            elif name == 'include' and args and ('/' in args[0] or args[0].lower().endswith('.cmake')):
                paths, kind = [args[0]], 'read'
            elif name == 'add_subdirectory' and args:
                paths, kind = [args[0]], 'read'
            else:
                continue
            for pa in paths:
                why = _classify(pa, kind, base)
                if why:
                    probs.append('%s: %s(%s): %s' % (rel, name, pa, why))
        for m in re.finditer(r'\bEXISTS\s+("(?:[^"\\]|\\.)*"|[^\s)]+)', t):
            why = _classify(m.group(1).strip('"'), 'exists', base)
            if why:
                probs.append('%s: EXISTS %s: %s' % (rel, m.group(1), why))
        for m in re.finditer(r'\b(IS_DIRECTORY|IS_SYMLINK|IS_NEWER_THAN)\s+("(?:[^"\\]|\\.)*"|[^\s)]+)', t):   # S106 GSN
            if m.group(1) == 'IS_NEWER_THAN':   # the placeholders' file times mean nothing
                probs.append('%s: IS_NEWER_THAN %s: file times in the placeholder tree mean nothing' % (rel, m.group(2)))
                continue
            why = _classify(m.group(2).strip('"'), 'exists', base)
            if why:
                probs.append('%s: %s %s: %s' % (rel, m.group(1), m.group(2), why))
    return probs


def scan_reads(tree):
    """scan_texts over CMakeLists.txt and every .cmake in REAL_INPUTS (the files configure runs)."""
    texts = {}
    for rel in ('CMakeLists.txt',) + tuple(r for r in REAL_INPUTS if r.endswith('.cmake')):
        b = tree.read(rel)
        if b is not None:
            texts[rel] = b.decode('utf-8', 'replace')
    return scan_texts(texts)


def _ninja_deps(build_ninja_text):
    m = re.search(r'^build build\.ninja: RERUN_CMAKE \|(.*?)(?:\n\n|\n(?=\S))', build_ninja_text, re.S | re.M)
    if not m:
        return []
    t = re.sub(r'\$\n\s*', ' ', m.group(1)).replace('$ ', '\x00')
    out = []
    for tok in t.split():
        out.append(tok.replace('$:', ':').replace('$$', '$').replace('\x00', ' '))
    return out


def _sig(path):
    """[size, mtime_ns] of a file, None when it is missing."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns]


def _compiler_sig(args):
    """The C++ compiler the configure uses: its path, [size, mtime_ns] and `--version`'s first line (a compiler upgrade changes every flag's meaning, and the key does not see it)."""
    c = args.get('CMAKE_CXX_COMPILER', '')
    if c and not os.path.isabs(c):
        c = shutil.which(c) or c
    sig = _sig(c) if c else None
    ver = ''
    if sig:
        r = _proc.run([c, '--version'], timeout=T_VER)
        ver = ((r.stdout or '').strip().splitlines() or [''])[0]
    return {'path': c, 'sig': sig, 'version': ver or ('unreadable' if sig else 'missing')}


def _external_deps(bn_text, src, bdir):
    """Every RERUN_CMAKE dependency OUTSIDE the placeholder tree and the build folder (the SDK's fmt / spdlog / SDL3 / VulkanHeaders / VMA / utf8cpp config files, CMake's own modules):
    {path: [size, mtime_ns]}. They add defines and include paths, so a change must reconfigure (S106 GSMAINF N4)."""
    srcn, bdn = os.path.normcase(os.path.normpath(src)), os.path.normcase(os.path.normpath(bdir))
    out = {}
    for dep in _ninja_deps(bn_text):
        ap = os.path.normpath(dep if os.path.isabs(dep) else os.path.join(bdir, dep))
        n = os.path.normcase(ap)
        if n.startswith(srcn + os.sep) or n.startswith(bdn + os.sep):
            continue
        sg = _sig(ap)
        if sg is not None:
            out[ap] = sg
    return out


def done_valid(done, pl):
    """A `.done` marker still describes the world: it records the external dependencies and the compiler, and none of them changed. An old-format marker is not valid (reconfigure once)."""
    try:
        d = json.loads(open(done, encoding='utf-8').read())
    except (OSError, ValueError):
        return False
    deps, comp = d.get('deps'), d.get('compiler')
    if not isinstance(deps, dict) or not isinstance(comp, dict):
        return False
    for path, sg in deps.items():
        if _sig(path) != sg:
            return False
    return _compiler_sig(pl['args']) == comp


def _cmake_cmd(pl, src, bdir):
    cmd = [pl['cmake'], '-S', src, '-B', bdir, '-G', 'Ninja', '-Wno-dev', '--no-warn-unused-cli']
    for k, v in sorted(pl['args'].items()):
        cmd.append('-D%s=%s' % (k, v))
    return cmd


def _ninja_c_lines(pl, bdir):
    r = _proc.run([pl['ninja'], '-C', bdir, '-t', 'commands'], timeout=T_NINJA)
    if r.returncode:
        raise ConfigureFailed('ninja -t commands failed in %s: %s' % (bdir, (r.stderr or '')[:500]))
    return [ln for ln in r.stdout.splitlines() if ' -c ' in ln]


def configure(pl, src, bdir, build, store):
    """Run cmake (the caller holds the admit slot and the key lock). Returns {path: sig} of the external dependencies."""
    if os.path.isdir(bdir):
        _rmtree(bdir, store)
    r = _proc.run(_cmake_cmd(pl, src, bdir), timeout=T_CFG)
    text = (r.stdout or '') + (r.stderr or '')
    if r.returncode:
        tail = '\n'.join(text.strip().splitlines()[-25:])
        raise ConfigureFailed('cmake could not configure this tree for the %s build (rc %d):\n%s' % (build, r.returncode, tail))
    m = re.search(r'Build files have been written to: (.+)', text)
    if not m or os.path.normcase(os.path.normpath(m.group(1).strip())) != os.path.normcase(os.path.normpath(bdir)):
        raise ConfigureFailed('configure did not write into %s (said: %s)' % (bdir, m.group(1).strip() if m else 'nothing'))
    # the guard: every in-tree file configure depends on must be one of the REAL copies, never a placeholder
    bn = _read(os.path.join(bdir, 'build.ninja'))
    if bn is None:
        raise ConfigureFailed('configure wrote no build.ninja in %s' % bdir)
    bnt = bn.decode('utf-8', 'replace')
    real = {r.lower() for r in REAL_INPUTS} | {SOURCES_CMAKE}
    srcn = os.path.normcase(os.path.normpath(src))
    for dep in _ninja_deps(bnt):
        d = os.path.normcase(os.path.normpath(dep if os.path.isabs(dep) else os.path.join(bdir, dep)))
        if d.startswith(srcn + os.sep):
            rel = d[len(srcn) + 1:].replace(os.sep, '/')
            if rel not in real:
                raise ConfigureFailed('configure read %s, which syntax_cfg gave as a placeholder: add it to REAL_INPUTS (tools/compile/syntax_cfg.py)' % rel)
    return _external_deps(bnt, src, bdir)


def reference_args(pl):
    """S106 GSN note 1: the -D list of the P1 reference, built from EVERY non-INTERNAL / non-STATIC entry of main's CMakeCache (not plan()'s CARRY / WWE2K14_* filter, so a cache variable that filter
    misses still reaches the reference), then the resolved preset on top (same rule as plan: a bare compiler name keeps main's full path)."""
    args = {k: v for k, (t, v) in pl['cache'].items() if t not in ('INTERNAL', 'STATIC')}
    for k, v in resolve_preset(pl['real']['CMakePresets.json'], PRESETS[pl['build']]).items():
        if k in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER') and k in args:
            continue
        args[k] = v
    args.update(VARIANT[pl['build']])   # S111: the same variant identity as plan(), or the reference inherits the play folder's stream-free cache
    return args


def reference_lines(main_dir, build, workdir):
    """Selftest P1 only: the compile lines of a configure-only run on the REAL main tree (its real sources, never out\\build) in `workdir`, ninja's own lines (the real root, no rewrite).
    Unavailable when cmake / ninja / main's cache are missing; ConfigureFailed when main's own tree does not configure."""
    key, pl = plan(WorkTree(main_dir), main_dir, build)
    pl = dict(pl, args=reference_args(pl))
    bdir = os.path.join(workdir, 'ref_' + build)
    if os.path.isdir(bdir):
        _rmtree(bdir, workdir)
    os.makedirs(workdir, exist_ok=True)
    with _admit.hold('syntax.worker', 1, timeout_s=120):
        r = _proc.run(_cmake_cmd(pl, main_dir, bdir), timeout=T_CFG)
        if r.returncode:
            raise ConfigureFailed('the reference configure of main\'s own tree failed (rc %d):\n%s' % (r.returncode, '\n'.join(((r.stdout or '') + (r.stderr or '')).strip().splitlines()[-15:])))
    return _ninja_c_lines(pl, bdir)


def _key_age_stamp(store, name):
    """When a key was last used or built: the newest `*.done` marker (a hit touches it), the folder's own time when it has none."""
    kd = os.path.join(store, name)
    ts = []
    try:
        ts = [e.stat().st_mtime for e in os.scandir(kd) if e.name.endswith('.done')]
        if not ts:
            ts = [os.stat(kd).st_mtime]
    except OSError:
        return None
    return max(ts)


def _sweep_leftovers(store, older=3600):
    """S106 GSN note 4: `<key>.del.<pid>` (a prune that died between the rename and the delete) and `<key>.lock.dead.<pid>` (a steal that died) older than `older` s are removed;
    only names of exactly those two shapes, only inside the store."""
    now = time.time()
    try:
        for e in os.scandir(store):
            if re.fullmatch(r'[0-9a-f]{24}\.del\.\d+', e.name) or re.fullmatch(r'[0-9a-f]{24}\.lock\.dead\.\d+', e.name):
                try:
                    if now - e.stat().st_mtime <= older:
                        continue
                    if e.is_dir():
                        _rmtree(e.path, store)
                    else:
                        os.remove(e.path)
                except OSError:
                    pass
    except OSError:
        pass


def prune(store, keep=12, keep_key=''):
    """Delete the least recently USED key folders over `keep` (by `*.done` mtime, not folder creation; never the one in use, never one with a lock file, never one used in the last hour). The
    folder is renamed aside first (atomic): a reader that already passed its hit check finds its `.lines` file gone and reconfigures instead of failing (S106 GSMAINF N1)."""
    _sweep_leftovers(store)
    try:
        items = []
        for e in os.scandir(store):
            if e.is_dir() and re.fullmatch(r'[0-9a-f]{24}', e.name) and e.name != keep_key:
                if not os.path.exists(os.path.join(store, e.name + '.lock')):
                    ts = _key_age_stamp(store, e.name)
                    if ts is not None:
                        items.append((ts, e.name))
        items.sort()
        for ts, name in items[:max(0, len(items) - keep)]:
            if time.time() - ts > 3600 and not os.path.exists(os.path.join(store, name + '.lock')):
                aside = os.path.join(store, '%s.del.%d' % (name, os.getpid()))
                try:
                    os.replace(os.path.join(store, name), aside)
                except OSError:
                    continue
                _rmtree(aside, store)
    except OSError:
        pass


def _read_lines(path):
    t = _read(path)
    return None if t is None else t.decode('utf-8', 'replace').split('\n') if t else []


def _hit(pl, kdir, build):
    """The stored compile lines when this key + build is built and still valid, else None (never an exception: a key deleted under us is a miss)."""
    bdir = os.path.join(kdir, build)
    done, linesf = os.path.join(kdir, build + '.done'), os.path.join(kdir, build + '.lines')
    if not (os.path.isfile(done) and os.path.isfile(os.path.join(kdir, 'src.done')) and os.path.isfile(linesf) and os.path.isfile(os.path.join(bdir, 'build.ninja'))):
        return None
    if not done_valid(done, pl):
        return None
    try:
        os.utime(done, None)
    except OSError:
        pass
    _hook('after_hit')
    return _read_lines(linesf)


def ensure(tree, main_dir, build):
    """The compile lines (raw, placeholder root) for this tree + build, configuring when the key has no valid done marker. Returns (bdir, src, key, fresh_seconds or None, lines).
    Each build's `-c` lines are written to <key>\\<build>.lines under the lock and READ from that file, so a prune can never delete what a reader still needs."""
    key, pl = plan(tree, main_dir, build)
    store = cfg_root(main_dir)
    kdir = os.path.join(store, key)
    src, bdir = os.path.join(kdir, 'src'), os.path.join(kdir, build)
    done, src_done, linesf = os.path.join(kdir, build + '.done'), os.path.join(kdir, 'src.done'), os.path.join(kdir, build + '.lines')
    lines = _hit(pl, kdir, build)
    if lines is not None:
        return bdir, src, key, None, lines
    t0 = time.time()
    with _admit.hold('syntax.worker', 1, timeout_s=120):   # S106 GSMAINF N2: the queue slot first, THEN the key lock (a lock held while waiting for a slot made others steal it)
        with _lock(os.path.join(store, key + '.lock')):
            if not os.path.isfile(src_done):
                if os.path.isdir(src):
                    _rmtree(src, store)
                materialise(tree, main_dir, pl, src)
                open(src_done, 'w').write(json.dumps({'files': len(pl['paths'])}))
                STATS['src_s'] = time.time() - t0
            if not (done_valid(done, pl) and os.path.isfile(linesf) and os.path.isfile(os.path.join(bdir, 'build.ninja'))):
                t1 = time.time()
                try:
                    if os.path.isfile(done):
                        os.remove(done)
                except OSError as e:   # S106 GSN note 5: a file system error is "tool unavailable" (game_syntax falls back, loudly), never a traceback
                    raise Unavailable('syntax_cfg store: cannot remove %s: %s' % (done, e))
                deps = configure(pl, src, bdir, build, store)
                clines = _ninja_c_lines(pl, bdir)
                tmp = linesf + '.tmp'
                open(tmp, 'w', encoding='utf-8', newline='').write('\n'.join(clines))
                try:
                    os.replace(tmp, linesf)
                except OSError as e:   # note 5
                    raise Unavailable('syntax_cfg store: cannot write %s: %s' % (linesf, e))
                open(done, 'w').write(json.dumps({'key': key, 'build': build, 'configure_s': round(time.time() - t1, 2), 'deps': deps, 'compiler': _compiler_sig(pl['args'])}))
            lines = _read_lines(linesf) or []
    prune(store, keep_key=key)
    return bdir, src, key, time.time() - t0, lines


def _rewrite(lines, src, main_dir):
    fwd = src.replace('\\', '/')
    pats = [re.compile(re.escape(fwd), re.I), re.compile(re.escape(src.replace('/', '\\')), re.I), re.compile(re.escape(src.replace('/', '\\\\')), re.I)]
    to = main_dir.replace('\\', '/').rstrip('/')
    out = []
    for ln in lines:
        for p in pats:
            ln = p.sub(lambda _m: to, ln)
        out.append(ln)
    return out


def commands(root, main_dir, build, overrides=None):
    """The compile command lines (` -c ` lines of `ninja -t commands`) of the tree's OWN configure, the placeholder root rewritten to main's root (so they read like the lines of
    main's out\\build). Raises Unavailable (fall back, loudly) or ConfigureFailed (a real FAIL)."""
    if build not in PRESETS:
        raise ValueError('build must be on or off')
    tree = root if isinstance(root, WorkTree) else WorkTree(root, overrides)
    bdir, src, key, fresh, raw = ensure(tree, main_dir, build)
    if fresh is None:
        STATS['hit'] += 1
    else:
        STATS['fresh'].append((build, key, fresh))
    STATS['last'] = (build, key, fresh)
    return _rewrite(raw, src, main_dir)


def note_line():
    """One status line for game_syntax's output: which keys were configured fresh (seconds) and how many hit."""
    fresh = ', '.join('%s %s %.1f s' % (b, k[:8], s) for b, k, s in STATS['fresh'])
    return 'syntax_cfg: %d hit%s%s' % (STATS['hit'], 's' if STATS['hit'] != 1 else '', ('; configured fresh: ' + fresh) if fresh else '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--build', choices=('on', 'off'), default='off')
    ap.add_argument('--main', default='')
    ap.add_argument('--show', default='', help='print the rewritten command of this unit (e.g. src/main.cpp)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = a.main
    if not main_dir:
        rc, out = _git(root, 'rev-parse', '--git-common-dir')
        main_dir = os.path.dirname(os.path.abspath(os.path.join(root, out.decode().strip())))
    t0 = time.time()
    try:
        lines = commands(root, main_dir, a.build)
    except (Unavailable, ConfigureFailed) as e:
        print('%s: %s' % (type(e).__name__, e))
        return 1
    print('%s; %d compile commands, %.1f s total' % (note_line(), len(lines), time.time() - t0))
    if a.show:
        want = (main_dir.replace('\\', '/') + '/' + a.show).lower()
        for ln in lines:
            m = re.search(r' -c (\S+)\s*$', ln)
            if m and m.group(1).replace('\\', '/').lower() == want:
                print(ln)
    return 0


if __name__ == '__main__':
    sys.exit(main())

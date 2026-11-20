# The fast gate's library (S102 lane M4): which checks a change can affect, what a result is cached by, and whether a program is stale.
# Used by tools\gate\gate.py (--changed [BASE], --jobs N, --no-cache, --build-stale); selftest: tools\gate\fastgate_selftest.py.
#
# A lane json (or one check in it) may declare  "affects": ["src/renderer/**", "tools/crowd/*.py"]  (globs, posix slashes, relative to the repo root;
# ** crosses folders, * does not). A change (git diff --name-only BASE, plus untracked files) selects a check when any changed path matches one of
# its globs, or is the lane's own json. A check with NO "affects" always runs and is never cached (nothing says what it reads).
# A check may also declare  "programs": [{"path": "out/caster/caster_rows_test.exe", "sources": ["tools/caster/**", "src/renderer/**"]}]:
# the program must exist and be at least as new as every file its globs match, else the check does not run: FAIL "stale build" (the S101 lesson:
# a --no-build check on an old exe gave a false gate result three times in one day). The exes the gate already knows (tools\gate\exes.py) are
# held to the same rule in --changed mode (the fast gate never rebuilds; --build-stale restores the old build-once behaviour).
# Cache key = sha256(lane json bytes, check name + cmd + timeout, content hash of every file the affects globs match, content hash of every
# program). Only a PASS is cached; a SKIP, FAIL or BUILD never is. times.csv gets one row per check and one for the whole gate.
# S103 PS2: cache_key_ex() (the key gate.py uses) adds content-not-mtime for every capture / case folder, the script closure, the toolchain and the paired
# real check of a planted-fault twin, and returns WHY when a check cannot be keyed (tools\gate\proofkey.py); gate.py re-runs a hit for real with probability
# GATE_VERIFY_P (default 0.05) or always with --verify-cache and FAILs loudly on a different verdict. serial_needed() below is no longer used by the
# gate (tools\gate\sched.py decides by paths); it stays for old callers.
import csv, hashlib, os, re, subprocess, threading, time
from pathlib import Path
import proofkey as pk   # S103 PS2: the honest proof cache's input enumeration


import functools


@functools.lru_cache(maxsize=None)
def glob_re(pat):   # S103 PS2: compiled once per glob (it was recompiled for every file x glob: ~0.36 s of key cost per check)
    pat = pat.replace('\\', '/')
    out, i = '', 0
    while i < len(pat):
        c = pat[i]
        if pat.startswith('**/', i):
            out += '(?:.*/)?'
            i += 3
        elif pat.startswith('**', i):
            out += '.*'
            i += 2
        elif c == '*':
            out += '[^/]*'
            i += 1
        elif c == '?':
            out += '[^/]'
            i += 1
        else:
            out += re.escape(c)
            i += 1
    return re.compile('^' + out + '$')


def matches(path, globs):
    p = path.replace('\\', '/')
    return any(glob_re(g).match(p) for g in globs)


def _git(root, *a):
    p = subprocess.run(['git', '-C', str(root)] + list(a), capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f'git {" ".join(a)} failed: {(p.stderr or p.stdout).strip()[:200]}')
    return p.stdout


def changed_files(root, base):
    """Paths (posix, relative to root) that differ from BASE in the working tree, plus untracked files. Raises when BASE is not a revision."""
    # --no-renames: a file moved OUT of a declared folder must list its OLD path too (a rename would show only the new one and the lane would not run)
    names = set(_git(root, 'diff', '--name-only', '--no-renames', base).split('\n'))
    names |= set(_git(root, 'ls-files', '--others', '--exclude-standard').split('\n'))
    return sorted(n.strip().replace('\\', '/') for n in names if n.strip())


def tree_files(root):
    """Every file in the checkout (tracked + untracked, not ignored), posix relative paths. Falls back to a walk without git."""
    try:
        names = set(_git(root, 'ls-files').split('\n')) | set(_git(root, 'ls-files', '--others', '--exclude-standard').split('\n'))
        return sorted(n.strip().replace('\\', '/') for n in names if n.strip())
    except Exception:
        out = []
        for dp, dn, fn in os.walk(root):
            dn[:] = [d for d in dn if d not in ('.git', 'out')]
            out += [os.path.relpath(os.path.join(dp, f), root).replace('\\', '/') for f in fn]
        return sorted(out)


def lane_affects(check):
    a = check.get('affects')
    return [a] if isinstance(a, str) else a


def selected(check, changed, lane_rel, derived=None, idx=None, root=None):
    """True when this check should run for the changed paths. Selection = union(affects, derived deps) (S103 DIB; tools\\gate\\deps.py):
      - the lane json changed: yes;  a changed path matches `affects`: yes;  a changed path is in the derived dependency set: yes;
      - nothing matched and the check has `affects`: no (as before);
      - nothing matched and it has NO `affects`: it used to run always. It is skipped only when its derived deps are EXACT (tier A / B: compiled or named
        by its own script, nothing scanned, nothing unknown) and the index they came from is not stale for the closure; otherwise it still always runs.
    No derived argument = the old rule (no 'affects' -> always)."""
    aff = lane_affects(check)
    dh = None
    if derived is not None:
        import deps as gdeps
        dh = gdeps.deps_hit(derived, changed)
    if aff is None and derived is None:
        return True, 'no affects declared: always runs'
    for p in changed:
        if p == lane_rel:
            return True, 'its lane json changed'
        if aff is not None and matches(p, aff):
            return True, f'{p} matches'
    if dh is not None:
        return True, f'derived deps (tier {derived["tier"]}): {dh} is in its dependency set'
    if aff is None:
        if derived['exact'] and not derived.get('writes'):
            import deps as gdeps
            st = gdeps.stale_index(derived, changed, idx, root) if root is not None else []
            if not st:
                return False, f'no affects; its derived deps (tier {derived["tier"]}, {len(derived["files"])} files) are exact and none changed'
            return True, f'no affects; index stale for {st[0]}: runs'
        if derived['exact']:
            return True, f'no affects; its derived deps are exact but it writes files (a consumer may need them): always runs'
        return True, f'no affects and its deps are not exact (tier {derived["tier"]}: {(derived["notes"] or ["?"])[0][:60]}): always runs'
    return False, 'no changed path matches its affects or derived deps'


_MEMO = {}


def file_hash(path):
    p = str(path)
    try:
        st = os.stat(p)
    except OSError:
        return 'missing'
    key = (p, st.st_mtime_ns, st.st_size)
    h = _MEMO.get(key)
    if h is None:
        d = hashlib.blake2b(digest_size=16)
        with open(p, 'rb') as f:
            for blk in iter(lambda: f.read(1 << 16), b''):
                d.update(blk)
        h = _MEMO[key] = d.hexdigest()
    return h


def input_hash(root, check, files=None):
    """Content hash over every file the check's affects globs match (sorted by path)."""
    aff = lane_affects(check) or []
    files = files if files is not None else tree_files(root)
    d = hashlib.sha256()
    n = 0
    for f in files:
        if matches(f, aff):
            d.update(f.encode() + b'\0' + file_hash(Path(root) / f).encode() + b'\n')
            n += 1
    return d.hexdigest(), n


def program_list(check):
    return [{'path': p['path'].replace('\\', '/'), 'sources': p.get('sources', [])} for p in check.get('programs', [])]


def stale_program(root, prog, files=None):
    """None when the program exists and is not older than any source its globs match; else the reason (a 'stale build' text)."""
    root = Path(root)
    exe = root / prog['path']
    if not exe.is_file():
        return f"{prog['path']}: missing (build it first)"
    t = exe.stat().st_mtime
    files = files if files is not None else tree_files(root)
    newest, who = 0.0, None
    for f in files:
        if matches(f, prog['sources']):
            try:
                m = os.stat(root / f).st_mtime
            except OSError:
                continue
            if m > newest:
                newest, who = m, f
    if newest > t:
        return f"{prog['path']}: older than its source {who} (rebuild it)"
    return None


ENV_ALLOW = ('GATE_EXE_WAIT_S', 'GATE_NO_RESTORE', 'WWE_FVIEW', 'WWE_FVIEW_ALLOW_STALE', 'PYTHONHASHSEED')   # plus any RDR_* / WWE2K14_*


def env_part(env=None):
    env = os.environ if env is None else env
    keys = sorted(k for k in env if k in ENV_ALLOW or k.startswith(('RDR_', 'WWE2K14_')))
    return repr([(k, env[k]) for k in keys])


def path_state(p):
    """A needs path or cmd argument: a file by content; a folder by its files' relative path + size + mtime (a capture folder is too big to read);
    missing = 'missing'."""
    p = Path(p)
    if p.is_file():
        return 'f:' + file_hash(p)
    if p.is_dir():
        d = hashlib.sha256()
        for dp, dn, fn in os.walk(p):
            dn.sort()
            for f in sorted(fn):
                try:
                    st = os.stat(os.path.join(dp, f))
                except OSError:
                    continue
                d.update(f'{os.path.relpath(os.path.join(dp, f), p)}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode())
        return 'd:' + d.hexdigest()
    return 'missing'


def arg_files(root, main, cmd):
    """Existing files named by a command's arguments (a trailing :N / :name suffix is allowed), resolved under root, else under the main checkout."""
    out = []
    for a in cmd:
        if not isinstance(a, str) or a.startswith('-'):
            continue
        base = a.split(':')[0] if not re.match(r'^[A-Za-z]:[\\/]', a) else a
        for cand in (Path(root) / base, Path(main) / base, Path(base)):
            try:
                if cand.is_file():
                    out.append(str(cand))
                    break
            except OSError:
                pass
    return out


def cache_key(root, check, lane_bytes, prog_paths, files=None, main=None):
    """-> key, or None when the check cannot be cached (no affects declared, or "cache": false). Besides the lane json, the cmd and the files its
    affects globs match, the key holds: every `needs` path (under the main checkout; a file by content, a folder by file sizes and times), every cmd
    argument that is an existing file (by content: a script a check runs is an input), the Python version and an allow-list of environment variables."""
    import sys
    if lane_affects(check) is None or check.get('cache') is False:
        return None
    main = main or root
    ih, _ = input_hash(root, check, files)
    d = hashlib.sha256()
    d.update(lane_bytes)
    d.update(repr((check.get('name'), check.get('cmd'), check.get('timeout_s'), check.get('needs'))).encode())
    d.update(ih.encode())
    for n in sorted(check.get('needs', [])):
        d.update(b'N' + n.encode() + b'\0' + path_state(Path(main) / n).encode())
    for f in sorted(arg_files(root, main, check.get('cmd', []))):
        d.update(b'A' + f.encode() + b'\0' + file_hash(f).encode())
    d.update(b'PY' + sys.version.encode())
    d.update(b'ENV' + env_part().encode())
    for p in sorted(prog_paths):
        d.update(p.encode() + b'\0' + file_hash(Path(root) / p).encode())
    return d.hexdigest()


KEY_VERSION = 'ps2-f1-dib1'   # bump when the key's recipe changes (an old PASS can never match a new recipe)
ENGINE_FILES = ('gate.py', 'fastgate.py', 'proofkey.py', 'sched.py', 'exes.py', 'deps.py', 'toolcache.py')   # the files that decide what the key holds and what a check runs under
_EXE_DIRS = ('out/tools', 'out/renderer/dev', 'out/renderer/release', 'out/release/build')
_SUBST = re.compile(r'^@main/')


def engine_bytes():
    d = hashlib.sha256()
    here = Path(__file__).resolve().parent
    for n in ENGINE_FILES:
        d.update(n.encode() + b'\0' + file_hash(here / n).encode())
    return d.hexdigest()


def script_inputs(root, main, check):
    """The inputs a check's SCRIPTS name in their own code, not on the command line (F1 of the S103 PS2 review): every out / game_data / generated path any
    script of the closure names (a capture literal, the tool exes it runs, MAIN's out/build/... build.ninja, a case folder), the tool exes a python script
    runs by name, and the exes / reads the lane declares. -> ([(display, real path)], why-not-cacheable or '', [(label, digest) of dependency manifests]).
    A .ps1 reached from a -Command body, or one that dot-sources / runs another script, cannot be enumerated: uncacheable unless the lane declares `exes` or
    `reads` (then those are the inputs the lane vouches for)."""
    import sched as gs
    root, main = Path(root), Path(main)
    cmd = [a for a in check.get('cmd', []) if isinstance(a, str)]
    skip = {gs.norm(w) for w in (check.get('writes') or [])}   # the lane calls these its OUTPUTS
    found = {}

    def add(rel):
        rel = _SUBST.sub('', rel)
        for b in (root, main):
            c = b / rel
            try:
                if c.exists():
                    found.setdefault(os.path.normcase(str(c.resolve())), (rel, str(c)))
            except OSError:
                pass

    declared = [gs.norm(x) for x in list(check.get('exes') or []) + list(check.get('reads') or []) if isinstance(x, str)]
    for rel in declared:
        add(rel)
    scanned, ps1_funny = set(), False
    ps1_all, exe_rels = {}, []
    for a in cmd:
        direct = a.lower().endswith(('.py', '.ps1'))
        names = [a] if direct else [m.group(0) for m in gs._SCRIPT_LIT.finditer(a)] if len(a) > 24 else []
        for nm in names:
            nm = nm.replace('\\', '/')
            if nm in scanned:
                continue
            scanned.add(nm)
            files = gs.closure_files(root, nm)
            nps1 = sum(1 for f in files if f[1] == 'ps1')
            if nps1 and (not direct or nps1 > 1):
                ps1_funny = True
            for _p, kind, text, top in files:
                if kind == 'ps1':
                    ps1_all[os.path.relpath(_p, root).replace('\\', '/')] = text
                touched, _w, ro = gs.script_paths(text, kind, cmd, top=top)
                for rel in sorted(touched | ro):
                    if rel and not any(rel == w or rel.startswith(w + '/') for w in skip) and not rel.startswith(('out/gate/chk', 'out/cases')):   # out/cases = case folders the check itself saves (see sched.script_paths: --save-case)
                        if rel.endswith('.exe'):
                            exe_rels.append(rel)
                        else:
                            add(rel)
                if kind == 'py':
                    for m in gs.EXE_LIT.finditer(text):
                        for dd in _EXE_DIRS:
                            exe_rels.append(dd + '/' + m.group(1))
    extra = []
    if ps1_funny and not declared:
        # S103 DIB: a build .ps1 in the closure is keyed by its DEPENDENCY MANIFEST (the exact files its last build compiled: tools\cxx_cache.ps1 writes it)
        # instead of refusing. Only manifest-writing build scripts that have a manifest, and the known helpers, are allowed; any other .ps1 still refuses.
        extra, why = ps1_manifest_inputs(root, main, ps1_all)
        if why:
            return [], why, []
    import deps as gdeps
    for rel in exe_rels:   # exes ALWAYS by bytes (S103 DIB review bug 1: skipping the bytes of an exe because SOME manifest existed let a rebuilt exe ride on an old PASS)
        add(rel)
        for b in (root, main):   # ... and, beside the bytes, the digest of the exe's own manifest (never instead of them)
            mf = gdeps.read_manifest(b / (_SUBST.sub('', rel) + '.deps.txt'), root, main)
            if mf:
                extra.append(('MX:' + rel, gdeps.digest(mf, root, main)))
                break
    return sorted(found.values()), '', extra


HELPER_PS1 = ('tools/env.ps1', 'tools/cxx_cache.ps1', 'tools/cxx_cc.ps1', 'tools/wrestler/atomic_build.ps1',
              'tools/admit/Admit.ps1')   # CQW: the compile queue's helper (it changes how many compiles run at once, never what a build reads or writes)


def ps1_manifest_inputs(root, main, ps1_all):
    """For the .ps1 files of a check's closure -> ([(label, digest)] of the builder scripts' dependency manifests, why). why != '' = it cannot be keyed."""
    import deps as gdeps
    store = Path(main) / 'out' / 'toolcache' / '_scripts'
    out = []
    for rel, text in sorted(ps1_all.items()):
        if rel in HELPER_PS1:
            continue
        if not re.search(r'cxx_cache\.ps1', text):
            return [], (f'{rel} is a .ps1 that does not write dependency manifests (it does not dot-source tools\\cxx_cache.ps1): its inputs cannot be enumerated; '
                        'declare "exes" / "reads" in the lane')
        ents = gdeps.script_manifest(root, main, rel, store)
        if not ents:
            return [], f'{rel} has no dependency manifest yet (a build through tools\\cxx_cache.ps1 writes it: run the gate once, or `pwsh -File {rel}`)'
        out.append(('M:' + rel, gdeps.digest(ents, root, main)))
    return out, ''


def derived_hash(root, derived, files=None):
    """Content hash of a check's derived dependency set: every derived file, and for a derived directory every tracked file under it (name + content)."""
    import deps as gdeps
    root = Path(root)
    d = hashlib.sha256()
    for f in derived.get('files', []):
        d.update(b'F' + f.encode() + b'\0' + file_hash(root / f).encode() + b'\n')
    dirs = gdeps.effective_dirs(derived)   # (S1: only an exact set adds directories; a declared scan always does)
    if dirs:
        files = files if files is not None else tree_files(root)
        for f in files:
            if any(f == x or f.startswith(x + '/') for x in dirs):
                d.update(b'D' + f.encode() + b'\0' + file_hash(root / f).encode() + b'\n')
    return d.hexdigest()


class _Trace:
    """A hashlib object that also appends every update's first 60 bytes and sha1 to <main>/out/gate/keytrace/<check>.txt (GATE_KEY_TRACE=1)."""

    def __init__(self, h, name):
        self.h, self.lines = h, []
        self.path = Path(os.environ.get('GATE_KEY_TRACE_DIR') or (Path(__file__).resolve().parents[2] / 'out' / 'gate' / 'keytrace')) / (re.sub(r'[^\w.-]', '_', name) + '.txt')

    def update(self, b):
        self.h.update(b)
        self.lines.append(f'{hashlib.sha1(b).hexdigest()[:12]} {b[:70]!r}')

    def hexdigest(self):
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text('\n'.join(self.lines) + '\n', encoding='utf-8')
        except OSError:
            pass
        return self.h.hexdigest()


def cache_key_ex(root, check, lane_bytes, prog_paths, files=None, main=None, stamp=None, pair_key=None, derived=None, pure=False, meta=None):
    """-> (key, why). key None = run it, never cache; why names the reason ('' with a key). The honest proof cache (S103 PS2): everything cache_key() holds
    (lane json, cmd, affects content, programs / exes by bytes, python, env) plus the CONTENT (not size + mtime) of every needs path and every out / game_data
    path the command names, the script and its import closure, the toolchain stamp and, for a planted-fault twin, the paired real check's key (pair_key;
    a twin without one is never cached). A check whose inputs cannot be enumerated returns None + why, never a guess."""
    import sys
    # S103 DIB: a check whose derived deps are EXACT (tools\gate\deps.py: tier B = its script names everything it reads; tier A = compiled / run exes with a
    # dependency manifest) is keyed on the content of that derived set. Without `affects` that is what makes it cacheable at all (it never was) - only when it
    # writes nothing (`pure`: a cache hit must not skip a producer) and is not a build .ps1 (the exe it builds must exist afterwards). With `affects` and tier B the
    # derived set REPLACES the affects-glob content (a glob that matches 300 files for a script that reads 3 no longer misses on every merge).
    # Narrowing (derived set replaces the affects-glob content) is OPT-IN until the observed-read recorder has verified a check's derived set: a lane says
    # "narrow": true on the check, or GATE_NARROW_KEYS=1 turns it on for every exact tier-B check. The default keeps the lane author's affects in the key.
    exact_b = (derived is not None and derived.get('exact') and derived.get('tier') == 'B'
               and (check.get('narrow') is True or os.environ.get('GATE_NARROW_KEYS') == '1'))
    exact_any = derived is not None and derived.get('exact') and derived.get('tier') in ('A', 'B') and not derived.get('writes')
    builds = any(isinstance(a, str) and a.lower().endswith('.ps1') for a in check.get('cmd', []))
    if lane_affects(check) is None and not (exact_any and pure and not builds):
        return None, 'no affects: inputs cannot be enumerated' + ('' if derived is None else ' (derived deps: tier ' + derived.get('tier', '?') + (', not exact' if not derived.get('exact') else ', not a pure non-build check') + ')')
    if check.get('cache') is False:
        return None, '"cache": false'
    if isinstance(check.get('uncacheable'), str) and check['uncacheable'].strip():   # S103 DIB review bug 6: a declared "uncacheable" takes effect
        return None, 'uncacheable: ' + check['uncacheable'].strip()
    if pk.is_plant(check) and pair_key is None:
        return None, 'planted-fault twin without a paired real check'
    main = main or root
    if lane_affects(check) is None or exact_b:
        ih = 'DERIVED:' + derived_hash(root, derived, files)
    else:
        ih, _ = input_hash(root, check, files)
        if derived is not None:   # S103 DIB review bug 4: a check selected by a derived dependency is keyed on it too (add, never replace)
            ih += '|DERIVED+:' + derived_hash(root, derived, files)
    d = _Trace(hashlib.sha256(), check['name']) if os.environ.get('GATE_KEY_TRACE') else hashlib.sha256()   # GATE_KEY_TRACE=1: out/gate/keytrace/<check>.txt lists what each key part was (diff two runs to see what moved)
    d.update(lane_bytes)
    d.update(repr((check.get('name'), check.get('cmd'), check.get('timeout_s'), check.get('needs'))).encode())
    d.update(ih.encode())
    for n in sorted(check.get('needs', [])):
        d.update(b'N' + n.encode() + b'\0' + pk.content_state(Path(main) / n).encode())
    d.update(b'KV' + KEY_VERSION.encode() + engine_bytes().encode())
    for disp, path in pk.named_inputs(root, main, check.get('cmd', [])):
        d.update(b'A' + disp.encode() + b'\0' + pk.content_state(path).encode())
    sin, why_s, sextra = script_inputs(root, main, check)
    if why_s:
        return None, why_s
    if meta is not None:
        meta['manifest'] = bool(sextra)   # the key depends on build manifests: the gate re-keys after the check ran (bug 5)
    for lab, dg in sextra:
        d.update(b'M' + lab.encode() + b'\0' + dg.encode())
    declared_real = set()   # GKL fix: what the lane declares (`exes` / `reads`) is an input it vouches for: full content, never a listing
    for x in list(check.get('exes') or []) + list(check.get('reads') or []):
        if isinstance(x, str):
            for b in (root, main):
                try:
                    declared_real.add(os.path.normcase(str((Path(b) / _SUBST.sub('', x.replace('\\', '/'))).resolve())))
                except OSError:
                    pass
    for disp, path in sin:
        d.update(b'I' + disp.encode() + b'\0' + pk.content_state(path, listing=os.path.normcase(str(Path(path).resolve())) not in declared_real).encode())   # GKL: a folder a script names is a listing, not 10^4 files read
    for sc in [a for a in check.get('cmd', []) if isinstance(a, str) and a.lower().endswith('.py')]:
        fl, probs = pk.script_closure(root, sc)
        if probs:
            return None, 'script inputs cannot be enumerated: ' + probs[0]
        for f in fl:
            d.update(b'S' + os.path.relpath(f, root).replace('\\', '/').encode() + b'\0' + file_hash(f).encode())
    import sched as _gs
    for a in [a for a in check.get('cmd', []) if isinstance(a, str)]:
        names = [a] if a.lower().endswith(('.py', '.ps1')) else [m.group(0) for m in _gs._SCRIPT_LIT.finditer(a)] if len(a) > 24 else []
        for nm in names:
            for f, kind, _t, _top in _gs.closure_files(root, nm.replace('\\', '/')):
                d.update(b'C' + os.path.relpath(f, root).replace('\\', '/').encode() + b'\0' + file_hash(f).encode())
    d.update(b'PY' + sys.version.encode())
    d.update(b'ENV' + env_part().encode())
    if stamp:
        d.update(b'TC' + stamp.encode())
    for p in sorted(prog_paths):
        d.update(p.encode() + b'\0' + file_hash(Path(root) / p).encode())
    if pair_key:
        d.update(b'PAIR' + pair_key.encode())
    return d.hexdigest(), ''


def validate_affects(root, checks, files=None):
    """-> list of problems: an `affects` that is not a non-empty list of strings, or a glob that matches no file in the tree. A silent never-selected
    (or always-selected) lane is worse than a loud one."""
    files = files if files is not None else tree_files(root)
    seen, bad = set(), []
    for c in checks:
        aff = lane_affects(c)
        if aff is None or (c['lane'], repr(aff)) in seen:
            continue
        seen.add((c['lane'], repr(aff)))
        if not isinstance(aff, list) or not aff or not all(isinstance(g, str) and g for g in aff):
            bad.append(f"{c['lane']}: 'affects' must be a non-empty list of globs, got {aff!r}")
            continue
        for g in aff:
            if not any(matches(f, [g]) for f in files):
                bad.append(f"{c['lane']}: affects glob {g!r} matches no file")
    return bad


def serial_needed(check):
    """Checks that must not run beside others: no `affects` (nothing says what they touch) or a PowerShell build (.ps1 in the cmd), unless the check or
    its lane says "parallel_safe": true."""
    if check.get('parallel_safe') is True:
        return False
    cmd = check.get('cmd', [])
    if any(isinstance(a, str) and a.lower().endswith('.ps1') for a in cmd):
        return True
    return lane_affects(check) is None


def default_base(root, branch='main'):
    """merge-base of HEAD with main (S103 H3; it named s102-arc-pt2); raises with a clear message when the branch does not exist."""
    try:
        return _git(root, 'merge-base', 'HEAD', branch).strip()
    except RuntimeError as e:
        raise RuntimeError(f'no BASE given and the branch {branch} is not here ({e}); pass --changed BASE')


class FileLock:
    """One OS lock file per shared gate file (cache.json.lock, times.csv.lock): every gate on this PC takes it around its load-merge-replace / append, so two gates never lose each other's
    updates or tear a row (S103 GST review 5, 7, 12). An OS byte-range lock on an open file: a process that dies, however it dies, frees it. Raises TimeoutError after `wait_s`."""

    def __init__(self, path, wait_s=30.0):
        self.path, self.wait_s, self.fd = str(path), wait_s, None

    def _try(self, fd):
        try:
            if os.name == 'nt':
                import msvcrt
                os.lseek(fd, 0, 0)
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    def __enter__(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT)
        t0 = time.time()
        while not self._try(fd):
            if time.time() - t0 > self.wait_s:
                os.close(fd)
                raise TimeoutError(f'could not take {self.path} in {self.wait_s:.0f} s')
            time.sleep(0.05)
        self.fd = fd
        return self

    def __exit__(self, *a):
        fd, self.fd = self.fd, None
        if fd is None:
            return
        try:
            if os.name == 'nt':
                import msvcrt
                os.lseek(fd, 0, 0)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
        os.close(fd)


def _replace(tmp, path, tries=20, delay=0.05):
    """os.replace that rides out a reader holding the target open on Windows (PermissionError)."""
    for i in range(tries):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(delay)


def load_cache(path, tries=5, delay=0.1):
    """The result cache. ONLY a missing file is empty ({}): any other failure (a reader / replace race, a torn file) is retried and then RAISED, never turned into {} (a per-row save
    that merged into {} wiped every other gate's entries: S103 GST review 4)."""
    import json
    last = None
    for i in range(tries):
        try:
            return json.loads(Path(path).read_text(encoding='utf-8'))
        except FileNotFoundError:
            return {}
        except Exception as e:
            last = e
            time.sleep(delay)
    raise last


def save_cache(path, cache, drop=()):
    """Merge into whatever another gate run saved meanwhile, then replace the file atomically (write a temp file, os.replace), all under cache.json.lock so two gates never lose each
    other's entries. `cache` = only what THIS gate adds (never its whole start-time copy). `drop` = keys a cache audit found wrong: removed from the file even if another gate run saved
    them meanwhile. Raises when the file cannot be read or written (the caller logs and skips)."""
    import json
    with FileLock(str(path) + '.lock'):
        cur = load_cache(path)
        cur.update(cache)
        for k in drop:
            cur.pop(k, None)
        tmp = Path(str(path) + f'.{os.getpid()}.{threading.get_ident()}.tmp')
        tmp.write_text(json.dumps(cur, indent=0), encoding='utf-8')
        _replace(tmp, path)


TIMES_HEAD = ['when', 'root', 'mode', 'lane', 'check', 'verdict', 'seconds', 'cache', 'run']


def append_times(path, rows):
    """Append rows to times.csv under times.csv.lock, each batch in ONE write (no torn rows across gates). A file written with the old 8-column header is migrated once (rows get an empty
    `run`); every row carries the gate's RUN_ID in its last column (the caller passes 9 fields; a shorter row is padded)."""
    import io
    p = Path(path)
    with FileLock(str(p) + '.lock'):
        new = not p.exists() or p.stat().st_size == 0
        if not new:
            with open(p, newline='', encoding='utf-8', errors='replace') as f:
                head = f.readline().strip()
            if head and 'run' not in head.split(','):
                old = p.read_text(encoding='utf-8', errors='replace').splitlines()
                tmp = Path(str(p) + f'.{os.getpid()}.tmp')
                tmp.write_text(','.join(TIMES_HEAD) + chr(10) + ''.join(ln + ',' + chr(10) for ln in old[1:]), encoding='utf-8')
                _replace(tmp, p)
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator=chr(10))
        if new:
            w.writerow(TIMES_HEAD)
        w.writerows([list(r) + [''] * (len(TIMES_HEAD) - len(r)) for r in rows])
        with open(p, 'a', newline='', encoding='utf-8') as f:
            f.write(buf.getvalue())


def last_seconds(path):
    """{check name: seconds of its newest PASS row in times.csv} (what a check takes when it runs; {} when the file is missing / unreadable)."""
    out = {}
    try:
        with open(path, newline='', encoding='utf-8', errors='replace') as f:
            for r in csv.DictReader(f):
                if r.get('verdict') == 'PASS' and r.get('check') and r.get('cache') not in ('hit',) and not str(r.get('lane', '')).startswith('('):
                    try:
                        out[r['check']] = float(r['seconds'])
                    except (TypeError, ValueError):
                        pass
    except OSError:
        pass
    return out


def only_base(root, branch='main'):
    """S103 OMIT: the base the --only omission warning diffs against. The session branch is whatever the MAIN checkout has checked out (as
    tools/harness/tidy.py session_branch()), never a hard-coded name. HEAD != session (a worktree branch, detached too): merge-base(HEAD, session).
    HEAD == session: HEAD^1 when HEAD is a merge commit (what the merge brought), else HEAD (uncommitted changes only)."""
    session = ''
    try:
        common = _git(root, 'rev-parse', '--path-format=absolute', '--git-common-dir').strip()
        session = _git(os.path.dirname(common.rstrip('/\\')), 'symbolic-ref', '--short', '-q', 'HEAD').strip()
    except RuntimeError:
        pass
    if not session:
        return default_base(root, branch)
    head = _git(root, 'rev-parse', 'HEAD').strip()
    if _git(root, 'rev-parse', session).strip() == head:
        parents = _git(root, 'rev-list', '--parents', '-n', '1', 'HEAD').split()
        return head + '^1' if len(parents) > 2 else head
    return _git(root, 'merge-base', 'HEAD', session).strip()


def omitted_affected(checks, only, changed):
    r"""S103 OMIT (docs\research\s103\F5S.md: an --only list left out s103_F5 although the change edited a file in its affects). `checks` = every
    check of every lane, `only` = the names / lanes the gate keeps, `changed` = changed paths. Returns {lane: [path, ...]} for the lanes with a check
    that is NOT in `only` and whose lane json or `affects` globs match a changed path (same matcher as --changed: matches()). A check with no
    `affects` is never listed (it has no declared files) and neither is a retired one."""
    out = {}
    for c in checks:
        if c['name'] in only or c['lane'] in only or c.get('retired'):
            continue
        aff = lane_affects(c)
        for p in changed:
            hit = p == c.get('_lane_rel') or (aff is not None and matches(p, aff))
            if hit:
                lst = out.setdefault(c['lane'], [])
                if p not in lst:
                    lst.append(p)
                break
    return out


def omitted_text(om, cap=15):
    items = [f'{ln} ({ps[0]})' for ln, ps in sorted(om.items())]
    return ', '.join(items[:cap]) + (f', +{len(items) - cap} more' if len(items) > cap else '')

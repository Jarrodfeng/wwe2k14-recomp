# Content-addressed tool-exe cache for the gate (Session 103, lane GX).
# Why: an agent worktree starts without out\, so every gate run rebuilt out\renderer\dev\fview.exe / rplay.exe and the rig exes (out\tools\frame_rig.exe,
# host_rig.exe, the unit tests): 68 gate runs x ~220 s = 4.2 h (out\gate\times.csv, "(exe step)" rows). The same sources always give the same exe, so the
# first build is published here and every other checkout COPIES it.
#
# Key = sha256 of EVERYTHING an exe is built from, never a subset we hope is enough:
#   - the content of every build-relevant file under src\ and tools\ (C++ sources and headers, shaders, build scripts, source lists, flags files; see
#     SRC_EXT). That is the whole tree, not the per-tool source list: the per-tool list is only what the mtime check trusts (and it was incomplete once,
#     S102 08:56), a whole-tree hash cannot miss an include. It caches less (an edit anywhere in src\ or tools\ makes a new key) and is correct.
#   - the tool's own identity (output path and the exact build argv, so dev / release and a -Release flag differ)
#   - the toolchain: clang++ --version, the newest VulkanSDK folder name (the build scripts pick it), tools\sdk_pins.json (the SDK / imgui / stb pins the
#     build scripts include from thirdparty\), the platform
#   Python files are NOT hashed (a build never reads them; they would only lower the hit rate); neither are lane jsons or docs.
# Layout: <cache>/<name>/<key>/{ok.json, files/<repo-relative path>...}; ok.json lists each file with size + sha256 and is written last (inside the temp
# folder, which is renamed into place as the final step): a half-written entry is never visible. A restore re-checks every sha256 and refuses on a mismatch.
# Keep: the newest KEEP keys per name (a hit refreshes the entry's age).
import hashlib, json, os, platform, shutil, subprocess, sys, threading, time, uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deps as gdeps   # S103 DIB: the compiler's own dependency manifests

SRC_EXT = ('.cpp', '.h', '.hpp', '.inl', '.c', '.cc', '.hlsl', '.glsl', '.vert', '.frag', '.comp', '.spv', '.ps1', '.txt', '.flags', '.cmake', '.def', '.rc')
TREES = ('src', 'tools')
SKIP_DIRS = {'__pycache__', '.git', 'out', 'lanes'}   # tools\gate\lanes holds lane jsons only (not read by a build)
OUT_EXT = ('.exe', '.pdb', '.dll', '.deps.txt')   # S103 DIB: <exe>.deps.txt and out\deps\<script>.deps.txt (tools\cxx_cache.ps1) travel with the exe
KEEP = 6


def _sha_file(p, _open=open):
    """sha256 of a file, streamed through ONE reusable 64 KB buffer (readinto): nothing accumulates and nothing is allocated per block. The old loop allocated a
    fresh 1 MB bytes object per read; with many gate threads and the PC's commit charge nearly used (other builders' games / rigs) that allocation raised
    MemoryError at toolcache.py:30 with ~15 GB of RAM free (S103 DIB2). A MemoryError is retried a few times (the commit charge moves), never swallowed."""
    h = hashlib.sha256()
    buf = bytearray(1 << 16)
    mv = memoryview(buf)
    for attempt in range(6):
        h = hashlib.sha256()
        try:
            with _open(p, 'rb') as f:
                while True:
                    n = f.readinto(mv)
                    if not n:
                        break
                    h.update(mv[:n])
            return h.hexdigest()
        except MemoryError:
            if attempt == 5:
                raise
            time.sleep(0.5 * (attempt + 1))
    return h.hexdigest()


def tree_digest(root):
    """sha256 over (relative path, content hash) of every build-relevant file under src\\ and tools\\ (sorted)."""
    root = Path(root)
    h = hashlib.sha256()
    for t in TREES:
        base = root / t
        entries = []
        for dp, dn, fn in os.walk(base):
            dn[:] = sorted(d for d in dn if d not in SKIP_DIRS)
            for f in sorted(fn):
                if f.lower().endswith(SRC_EXT):
                    entries.append(os.path.join(dp, f))
        for p in entries:
            try:
                h.update(os.path.relpath(p, root).replace('\\', '/').encode() + b'\0' + _sha_file(p).encode() + b'\n')
            except OSError:
                h.update(b'unreadable ' + p.encode() + b'\n')
    return h.hexdigest()


_STAMP = {}


def toolchain_stamp(root):
    root = Path(root)
    k = str(root)
    if k in _STAMP:
        return _STAMP[k]
    parts = [platform.platform(), platform.machine()]
    try:
        parts.append(subprocess.run(['clang++', '--version'], capture_output=True, text=True, timeout=30).stdout.strip())
    except Exception:
        parts.append('no clang++ on PATH')
    try:
        vk = sorted(p.name for p in Path('C:/VulkanSDK').iterdir() if p.is_dir())
        parts.append('vulkan ' + (vk[-1] if vk else 'none'))
    except OSError:
        parts.append('vulkan none')
    try:
        parts.append((root / 'tools' / 'sdk_pins.json').read_text(encoding='utf-8', errors='replace'))
    except OSError:
        parts.append('no sdk_pins.json')
    _STAMP[k] = '\n'.join(parts)
    return _STAMP[k]


class ToolCache:
    def __init__(self, root, cache_dir, keep=KEEP, log=print, main=None):
        self.root = Path(root)
        self.dir = Path(cache_dir)
        self.main = Path(main) if main else self.root   # where thirdparty\ lives for a worktree
        self.keep = keep
        self.log = log
        self._digest = None
        self._mu = threading.Lock()
        self.how = {}   # identity -> 'manifest' | 'tree': which key recipe the last key() used

    # ---- S103 DIB: keys from the build's own dependency manifest -------------------------------------------------------------------------------------
    # The old key hashed ALL of src\ + tools\ (any edit anywhere = a new key). A build now leaves <exe>.deps.txt (the exact files it was compiled from);
    # note_manifest() stores the union of a published build's manifests under its identity, and key() uses the CONTENT of those files (+ directory names:
    # tools\gate\deps.py digest) instead. No stored manifest (a build script that does not write them, an INVALID one) = the old whole-tree key.
    def _store_path(self, identity):
        return self.dir / '_deps' / (hashlib.sha256(identity.encode()).hexdigest()[:40] + '.deps.txt')

    def note_manifest(self, identity, relfiles):
        """Remember the dependency manifest of the build that just produced relfiles (the exe manifests among them). The script-level manifest
        (out\\deps\\...) is also copied to <cache>\\_scripts for the proof keys of other checkouts (gdeps.script_manifest). -> True when stored."""
        ents, scripts = [], []
        for r in relfiles:
            r = str(r).replace('\\', '/')
            if not r.endswith('.deps.txt'):
                continue
            m = gdeps.read_manifest(self.root / r, self.root, self.main)
            if m is None:
                continue
            (scripts if r.startswith('out/deps/') else ents).append((r, m))
        if not ents:
            return False
        try:
            self._last_ents = gdeps.union(*[m for _r, m in ents])   # kept in hand: rekey() keys from it, never from a re-read of the shared store (bug 8)
            gdeps.write_manifest(self._store_path(identity), self._last_ents, self.root, self.main)
            for r, m in scripts:
                gdeps.write_manifest(self.dir / '_scripts' / Path(r).name, m, self.root, self.main)
        except OSError as e:
            self.log(f'[gate] tool cache: cannot store the dependency manifest ({e})')
            return False
        return True

    def deps_key(self, identity, ents=None):
        ents = ents or gdeps.read_manifest(self._store_path(identity), self.root, self.main)
        if not ents:
            return None
        h = gdeps.digest(ents, self.root, self.main)
        return hashlib.sha256('\n'.join(['toolcache-v2-deps', identity, h, toolchain_stamp(self.root)]).encode()).hexdigest()

    def key(self, identity, ents=None):
        """identity: text naming the tool (output path + build argv). -> 64 hex chars. From the stored manifest when there is one (or `ents` in hand), else the
        whole-tree key."""
        k = self.deps_key(identity, ents)
        if k:
            self.how[identity] = 'manifest'
            return k
        self.how[identity] = 'tree'
        with self._mu:
            if self._digest is None:
                self._digest = tree_digest(self.root)
        return hashlib.sha256('\n'.join(['toolcache-v1', identity, self._digest, toolchain_stamp(self.root)]).encode()).hexdigest()

    def rekey(self, identity, relfiles):
        """After a build: store its manifest, then the key the entry is published under (from that fresh manifest when the build wrote one)."""
        self._last_ents = None
        stored = self.note_manifest(identity, relfiles)
        return self.key(identity, self._last_ents if stored else None)

    def entry(self, name, key):
        return self.dir / name / key

    def restore(self, name, key):
        """Copy a cached entry's files into the checkout. -> list of repo-relative files, or None (miss / refused). Every file is verified by sha256 first
        (a copy of the wrong exe is worse than a rebuild). Files already identical in place are left alone."""
        ent = self.entry(name, key)
        try:
            man = json.loads((ent / 'ok.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return None
        if man.get('key') != key or man.get('format') != 1:
            return None   # a stale-cache plant: an entry filed under another key is never used
        files = man.get('files', [])
        if not files:
            return None
        for f in files:
            src = ent / 'files' / f['path']
            try:
                if src.stat().st_size != f['size'] or _sha_file(src) != f['sha256']:
                    self.log(f'[gate] tool {name}: cache entry {key[:8]} is corrupt ({f["path"]}), ignored')
                    return None
            except OSError:
                return None
        done = []
        for f in files:
            src, dst = ent / 'files' / f['path'], self.root / f['path']
            try:
                if dst.is_file() and dst.stat().st_size == f['size'] and _sha_file(dst) == f['sha256']:
                    now = time.time()
                    os.utime(dst, (now, now))   # content identical (e.g. the main checkout after a merge touched sources): stamp it so the mtime check agrees
                    done.append(f['path'])
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                tmp = dst.with_name(dst.name + f'.tc{os.getpid()}')
                shutil.copy2(src, tmp)
                os.replace(tmp, dst)
                now = time.time()
                os.utime(dst, (now, now))   # "as new as the checkout": the mtime backstop (exes.is_fresh) then passes
                done.append(f['path'])
            except OSError as e:
                self.log(f'[gate] tool {name}: cannot place {f["path"]} ({e}); building instead')
                return None
        try:
            os.utime(ent, None)   # the entry was just used: newest for the prune
        except OSError:
            pass
        return done

    def publish(self, name, key, relfiles):
        """Atomically add an entry. relfiles: repo-relative output files (must exist). -> True when the entry exists afterwards."""
        rels = [r for r in dict.fromkeys(str(r).replace('\\', '/') for r in relfiles) if (self.root / r).is_file()]
        if not rels:
            return False
        ent = self.entry(name, key)
        if (ent / 'ok.json').is_file():
            return True
        parent = ent.parent
        try:
            parent.mkdir(parents=True, exist_ok=True)
            tmp = parent / f'.tmp-{os.getpid()}-{uuid.uuid4().hex[:8]}'
            (tmp / 'files').mkdir(parents=True)
            man = {'format': 1, 'key': key, 'name': name, 'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'from': str(self.root), 'files': []}
            for r in rels:
                dst = tmp / 'files' / r
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(self.root / r, dst)
                man['files'].append({'path': r, 'size': dst.stat().st_size, 'sha256': _sha_file(dst)})
            (tmp / 'ok.json').write_text(json.dumps(man, indent=1), encoding='utf-8')
            try:
                os.rename(tmp, ent)   # atomic; fails (and we drop ours) when another checkout published the same key first
            except OSError:
                shutil.rmtree(tmp, ignore_errors=True)
                return (ent / 'ok.json').is_file()
        except OSError as e:
            self.log(f'[gate] tool {name}: cannot publish to the cache ({e})')
            return False
        self.prune(name)
        return True

    def prune(self, name):
        parent = self.dir / name
        try:
            ents = [p for p in parent.iterdir() if p.is_dir() and not p.name.startswith('.tmp-')]
            for p in parent.iterdir():   # an abandoned temp folder (a crashed publisher) older than 1 h
                if p.name.startswith('.tmp-') and time.time() - p.stat().st_mtime > 3600:
                    shutil.rmtree(p, ignore_errors=True)
            ents.sort(key=lambda p: p.stat().st_mtime, reverse=True)
            for p in ents[self.keep:]:
                shutil.rmtree(p, ignore_errors=True)
        except OSError:
            pass


def scan_outputs(root):
    """{repo-relative path: (mtime_ns, size)} of every exe / pdb / dll under out\\ (depth <= 3; obj folders and the big non-tool trees skipped)."""
    root = Path(root)
    base = root / 'out'
    skip = {'build', 'wt', 'sdk-wt', 'gate', 'harness', 'toolcache', 'bench', 'release', 'android', 'replays', 'scenarios', 'recordings', 'backup',
            'glslang-build', 'pgo', 'tidy', 'captures', 'cases', 'runs', 's101', 'lane86', 'bisect'}
    res = {}
    if not base.is_dir():
        return res
    for dp, dn, fn in os.walk(base):
        depth = Path(dp).relative_to(base).parts
        dn[:] = [d for d in dn if not (len(depth) == 0 and d in skip) and not d.endswith(('_obj', 'obj')) and len(depth) < 3]
        for f in fn:
            if f.lower().endswith(OUT_EXT):
                p = os.path.join(dp, f)
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                res[os.path.relpath(p, root).replace('\\', '/')] = (st.st_mtime_ns, st.st_size)
    return res


def new_outputs(before, after):
    """Files the build created. A pre-existing file that changed makes the entry unsafe to publish (we cannot tell it belongs to this build): -> (new, changed)."""
    new = sorted(p for p in after if p not in before)
    changed = sorted(p for p in after if p in before and after[p] != before[p])
    return new, changed


def build_script_of(cmd):
    """The tools\\...\\build*.ps1 a check runs (pwsh -NoProfile -File <script> [flags]) -> (script, flags) or None."""
    cmd = [str(a) for a in cmd]
    if not cmd or Path(cmd[0]).stem.lower() not in ('pwsh', 'powershell'):
        return None
    for i, a in enumerate(cmd):
        if a.lower() == '-file' and i + 1 < len(cmd):
            s = cmd[i + 1].replace('\\', '/')
            if s.startswith('tools/') and Path(s).name.lower().startswith('build') and s.lower().endswith('.ps1'):
                return s, cmd[i + 2:]
            return None
    return None


# ---- self test: py -I tools\gate\toolcache.py selftest ----
def selftest():
    import tempfile
    fails = []

    def ok(c, t):
        print(('ok    ' if c else 'FAIL  ') + t)
        if not c:
            fails.append(t)
    base = Path(tempfile.mkdtemp(prefix='toolcache_selftest_'))
    try:
        def tree(name, src='int a;'):
            r = base / name
            (r / 'src').mkdir(parents=True)
            (r / 'tools').mkdir()
            (r / 'out' / 'x').mkdir(parents=True)
            (r / 'src' / 'a.cpp').write_text(src)
            (r / 'tools' / 'sdk_pins.json').write_text('{}')
            (r / 'out' / 'x' / 'tool.exe').write_bytes(b'EXE-' + src.encode())
            (r / 'out' / 'x' / 'tool.pdb').write_bytes(b'PDB-' + src.encode())
            return r
        quiet = lambda *_a, **_k: None
        a = tree('a')
        ca = ToolCache(a, base / 'cache', log=quiet)
        k = ca.key('out/x/tool.exe|build')
        ok(ca.restore('tool', k) is None, 'empty cache: miss')
        ok(ca.publish('tool', k, ['out/x/tool.exe', 'out/x/tool.pdb']), 'publish')
        # a twin checkout, same sources, no out\: a hit places the exe and pdb byte-identical
        b = tree('b')
        shutil.rmtree(b / 'out')
        cb = ToolCache(b, base / 'cache', log=quiet)
        kb = cb.key('out/x/tool.exe|build')
        ok(kb == k, 'same sources -> same key in another checkout')
        got = cb.restore('tool', kb)
        ok(got and (b / 'out' / 'x' / 'tool.exe').read_bytes() == b'EXE-int a;', 'hit copies the exe')
        # planted twin: a source change must change the key (and so miss)
        c = tree('c', src='int b;')
        shutil.rmtree(c / 'out')
        cc = ToolCache(c, base / 'cache', log=quiet)
        kc = cc.key('out/x/tool.exe|build')
        ok(kc != k and cc.restore('tool', kc) is None and not (c / 'out').exists(), 'planted source change -> new key, miss, nothing copied')
        # a different build argv (release) is a different key; a header-only change counts too
        ok(cb.key('out/x/tool.exe|build -Release') != k, 'different build argv -> different key')
        (b / 'src' / 'h.h').write_text('x')
        ok(ToolCache(b, base / 'cache', log=quiet).key('out/x/tool.exe|build') != k, 'a new header changes the key')
        (b / 'src' / 'notes.py').write_text('x')
        # stale-cache plant: an entry copied under another key's folder (filed wrong) must be refused, never copied
        shutil.copytree(ca.entry('tool', k), cc.entry('tool', kc))
        ok(cc.restore('tool', kc) is None and not (c / 'out').exists(), 'stale-cache plant (entry filed under the wrong key) is caught: refused, nothing copied')
        # a corrupted payload is refused
        (ca.entry('tool', k) / 'files' / 'out' / 'x' / 'tool.exe').write_bytes(b'EXE-int a;')
        (ca.entry('tool', k) / 'files' / 'out' / 'x' / 'tool.exe').write_bytes(b'EXE-zzzzz')
        d = tree('d')
        shutil.rmtree(d / 'out')
        ok(ToolCache(d, base / 'cache', log=quiet).restore('tool', k) is None and not (d / 'out').exists(), 'a corrupted cached exe is refused')
        # no half entries: a temp folder is not an entry
        (base / 'cache' / 'tool' / '.tmp-1-x' / 'files').mkdir(parents=True)
        ok(ToolCache(d, base / 'cache', log=quiet).restore('tool', '0' * 64) is None, 'a temp folder is not an entry')
        # prune keeps the newest N
        cp = ToolCache(a, base / 'cache', keep=2, log=quiet)
        for i in range(4):
            cp.publish('p', f'{i:064x}', ['out/x/tool.exe'])
            time.sleep(0.05)
        left = sorted(p.name for p in (base / 'cache' / 'p').iterdir())
        ok(len(left) == 2 and left[-1] == f'{3:064x}', f'prune keeps the newest 2 ({len(left)} left)')
        # output diff + script parse
        ok(build_script_of(['pwsh', '-NoProfile', '-File', 'tools/scene/build_rig.ps1']) == ('tools/scene/build_rig.ps1', []), 'build script parsed')
        ok(build_script_of(['pwsh', '-NoProfile', '-File', 'tools/x/run_it.ps1']) is None, 'a non-build script is not cached')
        # S103 DIB2: _sha_file streams (one reusable buffer), matches hashlib on a multi-block file, and retries a MemoryError instead of dying
        big = base / 'big.bin'
        big.write_bytes(bytes(range(256)) * 2000 + b'tail')   # 512 KB + 4: spans several 64 KB reads
        ok(_sha_file(big) == hashlib.sha256(big.read_bytes()).hexdigest(), '_sha_file: same sha256 as hashlib on a multi-block file')

        class _Flaky:
            n = 0

            def __init__(self, p, m):
                _Flaky.n += 1
                if _Flaky.n <= 2:
                    raise MemoryError('planted')
                self.f = open(p, m)

            def __enter__(self):
                return self.f

            def __exit__(self, *x):
                self.f.close()
        ok(_sha_file(big, _open=_Flaky) == hashlib.sha256(big.read_bytes()).hexdigest() and _Flaky.n == 3, '_sha_file: a MemoryError is retried, not fatal')
        bef = scan_outputs(a)
        time.sleep(0.02)
        (a / 'out' / 'x' / 'new.exe').write_bytes(b'n')
        new, ch = new_outputs(bef, scan_outputs(a))
        ok(new == ['out/x/new.exe'] and not ch, 'new outputs found by the snapshot diff')
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print('PASS toolcache selftest' if not fails else f'FAIL toolcache selftest ({len(fails)})')
    return 1 if fails else 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'selftest':
        sys.exit(selftest())
    print('usage: py -I tools\\gate\\toolcache.py selftest')
    sys.exit(2)

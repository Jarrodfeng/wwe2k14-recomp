#!/usr/bin/env python3
"""syntax_cfg selftest (S106 GSMAIN): the tree's own configure gives correct flags, and the planted twins FAIL where the old stale-flags path passed.

  py -I tools\\compile\\syntax_cfg_selftest.py [--root .]                 P1 parity + the twins + the out\\build mtime guard (about 20 s, one fresh configure per twin the first time)
  py -I tools\\compile\\syntax_cfg_selftest.py --mutant stale             the same twins against the OLD behaviour (main's out\\build lines): each must be CAUGHT (the check fails the old way)
  py -I tools\\compile\\syntax_cfg_selftest.py --a7 DIR                   P2 past fault: A7's tree (S106 time-trial lane A7, exit 4 at 13:45 on 2026-11-21) now gives game_syntax rc 0 (about 3 min)

Exit 0 = every row as expected; 1 = a row wrong; 3 = BLOCKED (main's build folder / a tree the proof needs is missing: nothing was judged).

Rows (each prints `ok` / `FAIL` / `NOT JUDGED`):
  P1 parity   syntax_cfg flags of the MAIN checkout's own tree == a FRESH configure of main's real tree in a scratch folder (never out\\build: S106 GSMAINF N3), every src unit, both builds
              (-MT/-MF/-o/-c/-include dropped as game_syntax does, the binary-dir `stamp` include dropped). A reference that cannot be built prints NOT JUDGED (exit 3), never ok.
  N1-N12      the GSMAINF fix rows (`--only fix` runs just these): prune racing a reader, lock staleness by a dead pid, NOT JUDGED, deps recorded + re-checked, static read scan + guard twin,
              cmake_state over every real input + fixed-list NEEDS-MAIN, OVERRIDE ACTIVE, T3 only ConfigureFailed, plan bytes read once, unsupported preset forms refused, cvar_pp's tree CMakeLists.
  T1 define   a CMakeLists that REMOVES `_CRT_SECURE_NO_WARNINGS` from the game target: a unit that #errors without it must FAIL with the twin's flags and PASS with the real ones.
  T2 keep     keep_stream_sources.txt without a unit the stream build compiles: the stream-FREE (off) build must now compile that unit (the old path never saw the change).
  T3 broken   a CMakeLists that does not configure: ConfigureFailed (never lines), and `game_syntax.py --changed` exits 1 with a FAIL line (never PASS).
  T4 mtimes   out\\build\\*\\{build.ninja, CMakeCache.txt, rules.ninja} unchanged (path, mtime, size) across this whole run; the detector itself is shown to see a planted change in a scratch copy.
  T5 fallback syntax_cfg.Unavailable (cmake missing) makes game_syntax print FALLING BACK and use main's out\\build lines: never silent.
"""
# entry: commands
import argparse
import contextlib
import glob
import io
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault('GAME_SYNTAX_NO_CACHE', '1')   # twins are scratch units: no store writes
import game_syntax as gs   # noqa: E402
import syntax_cfg as cfg   # noqa: E402
import proc as _proc   # noqa: E402

CRT = '_CRT_SECURE_NO_WARNINGS'
CRT_BLOCK = '    target_compile_definitions(wwe2k14 PRIVATE _CRT_SECURE_NO_WARNINGS)\n'


def norm(lines, main):
    """unit -> set of flag tuples (what game_syntax's flags_for keeps), for every unit under <main>/src/."""
    pre = (main.replace('\\', '/') + '/src/').lower()
    out = {}
    for ln in lines:
        m = re.search(r' -c (\S+)\s*$', ln)
        if not m or not m.group(1).replace('\\', '/').lower().startswith(pre):
            continue
        toks = ln.replace('\\"', '"').split()[1:]
        keep, i = [], 0
        while i < len(toks):
            t = toks[i]
            if t in ('-MT', '-MF', '-o', '-c', '-include', '-Xclang'):
                i += 2
                continue
            if t in ('-MD', '-MMD', '-Winvalid-pch') or t == m.group(1) or (t.startswith('-I') and t.replace('\\', '/').rstrip('/').endswith('/stamp')):
                i += 1
                continue
            keep.append(t.replace('\\', '/').lower() if t.startswith(('-I', '-isystem')) else t)
            i += 1
        out.setdefault(m.group(1).replace('\\', '/').lower(), set()).add(tuple(keep))
    return out


def snap_build(main):
    d = os.path.join(main, 'out', 'build')
    s = {}
    try:
        for e in sorted(os.scandir(d), key=lambda e: e.name):
            s['<dir>/' + e.name] = None
            if e.is_dir():
                for f in ('build.ninja', 'CMakeCache.txt', 'rules.ninja'):
                    try:
                        st = os.stat(os.path.join(e.path, f))
                        s[e.name + '/' + f] = (st.st_mtime_ns, st.st_size)
                    except OSError:
                        pass
    except OSError:
        pass
    return s


def p1_parity(main, scratch):
    """P1: the configure of the MAIN checkout's own tree through syntax_cfg == a fresh configure of main's REAL tree in a scratch folder (S106 GSMAINF N3: never out\\build, so it is always judged;
    a reference that cannot be built says NOT JUDGED). Returns (problems, notes, {build: judged})."""
    probs, notes, judged = [], [], {}
    tree = cfg.WorkTree(main)
    for b in ('on', 'off'):
        try:
            ref_lines = cfg.reference_lines(main, b, os.path.join(scratch, 'p1ref'))
        except cfg.Unavailable as e:
            notes.append('P1 [%s] NOT JUDGED: no reference could be configured (%s)' % (b, e))
            judged[b] = False
            continue
        except cfg.ConfigureFailed as e:
            probs.append('P1 [%s] the reference configure of main\'s own tree failed: %s' % (b, str(e)[:300]))
            judged[b] = True
            continue
        judged[b] = True
        mine = norm(cfg.commands(tree, main, b), main)
        ref = norm(ref_lines, main)
        if not ref:   # S106 GSN note 2: an empty reference compares nothing
            notes.append('P1 [%s] NOT JUDGED: the reference configure produced no src compile line' % b)
            judged[b] = False
            continue
        if not mine:
            probs.append('P1 [%s] syntax_cfg gave no src compile line (the reference has %d units)' % (b, len(ref)))
            continue
        same = [u for u in ref if u in mine and mine[u] == ref[u]]
        for u in sorted(set(ref) | set(mine)):
            if u not in mine:
                probs.append('P1 [%s] %s: in the reference configure, not in the tree\'s syntax_cfg lines' % (b, u))
            elif u not in ref:
                probs.append('P1 [%s] %s: in the tree\'s syntax_cfg lines, not in the reference configure' % (b, u))
            elif mine[u] != ref[u]:
                a, c = sorted(mine[u])[0], sorted(ref[u])[0]
                probs.append('P1 [%s] %s: flags differ: only syntax_cfg %s | only reference %s' % (b, u, sorted(set(a) - set(c))[:4], sorted(set(c) - set(a))[:4]))
        notes.append('P1 [%s] parity: %d of %d units identical (%d in syntax_cfg\'s lines; reference = a fresh configure of main\'s real tree)' % (b, len(same), len(ref), len(mine)))
    return probs, notes, judged


def p1_status(probs, judged):
    """`FAIL` when a difference was found, `NOT JUDGED` when any build was not compared (never `ok`), else `ok`."""
    if probs:
        return 'FAIL'
    if not judged or not all(judged.values()):
        return 'NOT JUDGED'
    return 'ok'


def p1_detector(main):
    """The parity comparison itself must see a planted difference (a define dropped from one unit's line), never say `identical` over it."""
    ref = gs.build_commands(main, gs.BUILD_DIRS['off'])
    bent = list(ref)
    for i, ln in enumerate(bent):
        if ' -DNOMINMAX ' in ln and ('/src/' in ln.replace('\\', '/')) and ' -c ' in ln:
            bent[i] = ln.replace(' -DNOMINMAX ', ' ', 1)
            break
    else:
        return ['P1 detector cannot be built: no unit line with -DNOMINMAX']
    return [] if norm(bent, main) != norm(ref, main) else ['P1 the parity comparison did not see a planted missing define']


def compile_probe(root, main, lines, path):
    fl = gs.flags_for(lines, main.replace('\\', '/'), 'src/start_settings.cpp')
    label, rc, text, _k, _h = gs.compile_unit(root, main, fl, path, 'probe')
    return rc, text


def twins(root, main, scratch, commands):
    """Each twin: [problems]. `commands(tree_or_root, main, build)` is the function under test (the mutant swaps it)."""
    res = {}
    base_text = (cfg.WorkTree(root).read('CMakeLists.txt') or b'').decode('utf-8', 'replace')
    probe = os.path.join(scratch, 'probe_crt.cpp')
    open(probe, 'w').write('#ifndef %s\n#error planted: %s is missing from the flags\n#endif\nint syntax_cfg_probe;\n' % (CRT, CRT))
    # T1
    p = []
    if CRT_BLOCK not in base_text.replace('\r\n', '\n'):
        p.append('T1 cannot be built: CMakeLists.txt has no `%s` block to remove' % CRT_BLOCK.strip())
    else:
        ok = commands(cfg.WorkTree(root), main, 'on')
        rc, text = compile_probe(root, main, ok, probe)
        if rc:
            p.append('T1 control: the real tree\'s flags do not carry %s (probe failed: %s)' % (CRT, (re.findall(r'error: [^\n]*', text) or ['?'])[0]))
        tw = commands(cfg.WorkTree(root, {'CMakeLists.txt': base_text.replace('\r\n', '\n').replace(CRT_BLOCK, '').encode()}), main, 'on')
        rc, text = compile_probe(root, main, tw, probe)
        if rc == 0:
            p.append('T1 the removed define still PASSES: the flags came from somewhere that did not follow the tree\'s CMakeLists.txt')
    res['T1 define'] = p
    # T2
    p = []
    tree = cfg.WorkTree(root)
    ks = (tree.read('tools/scene/keep_stream_sources.txt') or b'').decode().replace('\r\n', '\n')
    on = commands(tree, main, 'on')
    off = commands(tree, main, 'off')
    pre = main.replace('\\', '/').lower() + '/'

    def has(lines, u):
        return any((re.search(r' -c (\S+)\s*$', ln) or [0, ''])[1].replace('\\', '/').lower() == pre + u.lower() for ln in lines)
    pick = [u.strip() for u in ks.splitlines() if u.strip().startswith('src/') and has(on, u.strip()) and not has(off, u.strip())]
    if not pick:
        p.append('T2 cannot be built: no unit of keep_stream_sources.txt that the stream build compiles and the stream-free build does not')
    else:
        u = pick[0]
        tw = commands(cfg.WorkTree(root, {'tools/scene/keep_stream_sources.txt': ('\n'.join(l for l in ks.splitlines() if l.strip() != u) + '\n').encode()}), main, 'off')
        if not has(tw, u):
            p.append('T2 %s left the stream-only list but the stream-free build still does not compile it: the unit moved and nobody noticed' % u)
    res['T2 keep_stream'] = p
    res['T3 broken (function)'] = t3_function(root, main, commands)
    return res


def t3_function(root, main, commands):
    """A CMakeLists that does not configure must raise ConfigureFailed: any other exception (even Unavailable) is a problem (S106 GSMAINF N9)."""
    base_text = (cfg.WorkTree(root).read('CMakeLists.txt') or b'').decode('utf-8', 'replace')
    broken = (base_text.replace('\r\n', '\n') + '\nmessage(FATAL_ERROR "planted: this CMakeLists does not configure")\n').encode()
    try:
        commands(cfg.WorkTree(root, {'CMakeLists.txt': broken}), main, 'off')
        return ['T3 a CMakeLists that does not configure produced flags (no ConfigureFailed)']
    except cfg.ConfigureFailed:
        return []
    except Exception as e:   # noqa: BLE001
        return ['T3 wrong failure kind for a broken CMakeLists: %s: %s' % (type(e).__name__, str(e)[:120])]


def t3_end_to_end(root, scratch):
    base = (cfg.WorkTree(root).read('CMakeLists.txt') or b'').decode('utf-8', 'replace').replace('\r\n', '\n')
    bp = os.path.join(scratch, 'broken_cmakelists.txt')
    open(bp, 'w', newline='').write(base + '\nmessage(FATAL_ERROR "planted: this CMakeLists does not configure")\n')
    js = os.path.join(scratch, 'override.json')
    json.dump({'CMakeLists.txt': bp}, open(js, 'w'))
    env = dict(os.environ, **{cfg.OVERRIDE_ENV: js})
    r = _proc.run([sys.executable, '-I', os.path.join(HERE, 'game_syntax.py'), '--root', root, '--changed', '--build', 'off'], timeout=300, env=env)
    out = (r.stdout or '') + (r.stderr or '')
    p = []
    if r.returncode != 1 or not re.search(r'^FAIL: \[off\]', out, re.M) or 'PASS' in out:
        p.append('T3 end to end: game_syntax --changed on a CMakeLists that does not configure gave rc %s (want 1 + a FAIL line, no PASS): %s' % (r.returncode, out.strip().splitlines()[-1:] ))
    return p


def t4_detector(scratch):
    d = os.path.join(scratch, 'fakebuild', 'out', 'build', 'x')
    os.makedirs(d, exist_ok=True)
    f = os.path.join(d, 'build.ninja')
    open(f, 'w').write('a')
    before = snap_build(os.path.join(scratch, 'fakebuild'))
    time.sleep(0.05)
    open(f, 'w').write('ab')
    return [] if snap_build(os.path.join(scratch, 'fakebuild')) != before else ['T4 the mtime detector did not see a planted change']


def t5_fallback(root, main):
    orig = cfg.commands
    buf = io.StringIO()

    def boom(*a, **k):
        raise cfg.Unavailable('planted: cmake.exe not found')
    cfg.commands = boom
    try:
        with contextlib.redirect_stdout(buf):
            lines = gs.build_lines_for(root, main, 'off', [])
    finally:
        cfg.commands = orig
    p = []
    if 'FALLING BACK' not in buf.getvalue():
        p.append('T5 the fallback to main\'s out\\build was silent')
    if not lines:
        p.append('T5 the fallback returned no lines')
    return p


class CountingTree(cfg.WorkTree):
    """A WorkTree that counts reads per file (S106 GSMAINF N10: each input is read once per ensure)."""

    def __init__(self, *a, **k):
        self.reads = {}
        super().__init__(*a, **k)

    def read(self, rel):
        self.reads[rel] = self.reads.get(rel, 0) + 1
        return super().read(rel)


def _guard(fn, *a):
    """A row that cannot run (an API the fix adds, an exception) is a FAIL row, never a pass."""
    try:
        return fn(*a)
    except Exception as e:   # noqa: BLE001
        return ['%s raised %s: %s' % (fn.__name__, type(e).__name__, str(e)[:240])]


def _key_dirs(store):
    return sorted(d for d in glob.glob(os.path.join(store, '[0-9a-f]' * 24)) if os.path.isfile(os.path.join(d, 'src.done')))


def _age(path, secs):
    t = time.time() - secs
    os.utime(path, (t, t))


def store_rows(root, main, scratch):
    """The rows that need one fresh configure in a SCRATCH store (SYNTAX_CFG_DIR; the real store and out\\build are never touched): N10 reads, N2 order, N4 deps recorded + re-checked,
    N1 lines file + a prune racing a reader. Each result is a list of problems."""
    res = {}
    store = os.path.join(scratch, 'store')
    if os.path.isdir(store):
        cfg._rmtree(store, scratch)
    os.makedirs(store)
    old_env = os.environ.get('SYNTAX_CFG_DIR')
    os.environ['SYNTAX_CFG_DIR'] = store
    base = (cfg.WorkTree(root).read('CMakeLists.txt') or b'').decode('utf-8', 'replace').replace('\r\n', '\n')
    tree = CountingTree(root, {'CMakeLists.txt': (base + '\n# nonce %d\n' % time.time_ns()).encode()})
    events = []
    oh, ol = cfg._admit.hold, cfg._lock

    @contextlib.contextmanager
    def hold(*a, **k):
        events.append('admit')
        with oh(*a, **k) as x:
            yield x

    @contextlib.contextmanager
    def lock(*a, **k):
        events.append('lock')
        with ol(*a, **k):
            yield
    cfg._admit.hold, cfg._lock = hold, lock
    try:
        try:
            lines = cfg.commands(tree, main, 'on')
        finally:
            cfg._admit.hold, cfg._lock = oh, ol
        # N10
        res['N10 plan bytes carried'] = ['%s was read %d times in one configure (want 1: plan and materialise share the bytes)' % (k, n) for k, n in sorted(tree.reads.items()) if n > 1]
        # N2
        p = []
        if 'admit' not in events or 'lock' not in events:
            p.append('N2 the fresh configure did not take both the admit slot and the key lock (%s)' % events)
        elif events.index('admit') > events.index('lock'):
            p.append('N2 the key lock was taken BEFORE the admit slot (%s): a waiter on the queue holds the lock' % events)
        res['N2 admit before lock'] = p
        kd = _key_dirs(store)
        # N4 recorded
        p = []
        try:
            d = json.load(open(os.path.join(kd[0], 'on.done')))
            deps, comp = d.get('deps'), d.get('compiler')
            if not isinstance(deps, dict) or not any(re.search(r'spdlog|fmt|SDL3', k, re.I) for k in deps):
                p.append('N4 on.done records no SDK config dependency (spdlog / fmt / SDL3): %s' % (list(deps)[:3] if isinstance(deps, dict) else deps))
            if not isinstance(comp, dict) or not comp.get('version'):
                p.append('N4 on.done records no compiler version: %s' % comp)
        except (IndexError, OSError, ValueError) as e:
            p.append('N4 cannot read on.done: %s' % e)
        res['N4 deps recorded'] = p
        # N1 lines file
        lf = os.path.join(kd[0], 'on.lines') if kd else ''
        res['N1 lines file'] = [] if lf and os.path.isfile(lf) and len(open(lf, encoding='utf-8').read().splitlines()) == len(lines) else ['N1 no on.lines with the %d compile lines next to on.done' % len(lines)]
        # N1 race: a prune (another run) deletes the key between the hit check and the read
        for i in range(13):
            fk = os.path.join(store, '%024x' % (i + 1))
            os.makedirs(fk, exist_ok=True)
            open(os.path.join(fk, 'on.done'), 'w').write('{}')
        for e in os.scandir(kd[0]):
            _age(e.path, 7200)
        _age(kd[0], 7200)

        def prune_race():
            for f in glob.glob(os.path.join(kd[0], '*.done')):
                _age(f, 7200)
            cfg.prune(store)
        cfg._HOOKS['after_hit'] = prune_race
        p = []
        try:
            again = cfg.commands(CountingTree(root, dict(tree.overrides)), main, 'on')
            if again != lines:
                p.append('N1 after the raced prune the lines differ from the first run (%d vs %d)' % (len(again), len(lines)))
        except Exception as e:   # noqa: BLE001
            p.append('N1 a prune racing a reader gave %s: %s' % (type(e).__name__, str(e)[:160]))
        finally:
            cfg._HOOKS.pop('after_hit', None)
        res['N1 prune races reader'] = p
        # N4 re-check on a hit: a planted dependency outside the tree (a copy standing for an SDK config file) touched -> reconfigure; a changed compiler version -> reconfigure
        p = []
        kd = _key_dirs(store)
        done = os.path.join(kd[0], 'on.done') if kd else ''
        if not done or not os.path.isfile(done):
            p.append('N4 no key after the race row: cannot plant')
        else:
            fake = os.path.join(scratch, 'planted_sdk_config.cmake')
            open(fake, 'w').write('# planted\n')
            d = json.load(open(done))
            d.setdefault('deps', {})[fake] = [os.path.getsize(fake), os.stat(fake).st_mtime_ns]
            json.dump(d, open(done, 'w'))
            n0 = len(cfg.STATS['fresh'])
            cfg.commands(CountingTree(root, dict(tree.overrides)), main, 'on')
            if len(cfg.STATS['fresh']) != n0:
                p.append('N4 control: an unchanged planted dependency reconfigured')
            os.utime(fake, ns=(os.stat(fake).st_atime_ns, os.stat(fake).st_mtime_ns + 5 * 10 ** 9))
            cfg.commands(CountingTree(root, dict(tree.overrides)), main, 'on')
            if len(cfg.STATS['fresh']) == n0:
                p.append('N4 a touched SDK config file (planted copy) did not reconfigure')
            n1 = len(cfg.STATS['fresh'])
            d = json.load(open(done))
            d['compiler']['version'] = 'planted: another compiler version'
            json.dump(d, open(done, 'w'))
            cfg.commands(CountingTree(root, dict(tree.overrides)), main, 'on')
            if len(cfg.STATS['fresh']) == n1:
                p.append('N4 a changed compiler version did not reconfigure')
        res['N4 re-checked on a hit'] = p
    finally:
        cfg._admit.hold, cfg._lock = oh, ol
        if old_env is None:
            os.environ.pop('SYNTAX_CFG_DIR', None)
        else:
            os.environ['SYNTAX_CFG_DIR'] = old_env
    return res


def n2_lock_rows(scratch):
    """A lock is stale only when its holder is dead, never by age."""
    d = os.path.join(scratch, 'lk')
    os.makedirs(d, exist_ok=True)
    live, dead = os.path.join(d, 'live.lock'), os.path.join(d, 'dead.lock')
    open(live, 'w').write(str(os.getpid()))
    _age(live, 700)
    p = []
    try:
        with cfg._lock(live, timeout=2):
            p.append('N2 a lock held by a LIVE process for 700 s was stolen')
    except cfg.Unavailable:
        pass
    finally:
        try:
            os.remove(live)
        except OSError:
            pass
    sp = subprocess.Popen([sys.executable, '-I', '-c', 'pass'])
    sp.wait()
    open(dead, 'w').write(str(sp.pid))   # a fresh lock whose holder died
    try:
        with cfg._lock(dead, timeout=3):
            pass
    except cfg.Unavailable:
        p.append('N2 a fresh lock whose holder is DEAD was not taken over')
    return p


def n11_rows():
    base = {'configurePresets': [{'name': 'a', 'cacheVariables': {'X': 'Y'}}]}
    p = []
    forms = {'toolchainFile': {'toolchainFile': 'tc.cmake'}, 'environment': {'environment': {'CC': 'gcc'}}, 'architecture': {'architecture': 'x64'}, 'toolset': {'toolset': 'v143'},
             'generator': {'generator': 'Unix Makefiles'}, 'macro': {'cacheVariables': {'X': '${sourceDir}/y'}}, 'env macro': {'cacheVariables': {'X': '$env{FOO}'}}}
    for name, extra in forms.items():
        j = json.loads(json.dumps(base))
        j['configurePresets'][0].update(extra)
        try:
            cfg.resolve_preset(json.dumps(j).encode(), 'a')
            p.append('N11 a preset with %s was accepted (its effect is not applied here)' % name)
        except cfg.ConfigureFailed:
            pass
    j = json.loads(json.dumps(base))
    j['configurePresets'] = [{'name': 'p', 'hidden': True, 'toolchainFile': 'tc.cmake'}, {'name': 'a', 'inherits': 'p'}]
    try:
        cfg.resolve_preset(json.dumps(j).encode(), 'a')
        p.append('N11 an unsupported form inherited from a parent was accepted')
    except cfg.ConfigureFailed:
        pass
    try:
        cfg.resolve_preset(json.dumps(base).encode(), 'a')
    except cfg.ConfigureFailed as e:
        p.append('N11 control: a plain preset was refused: %s' % e)
    try:
        cfg.check_compiler({'CMAKE_CXX_COMPILER': 'gcc'}, {'CMAKE_CXX_COMPILER': ('STRING', 'C:/Program Files/LLVM/bin/clang++.exe')})
        p.append('N11 a preset naming another compiler than main\'s cache was accepted')
    except cfg.ConfigureFailed:
        pass
    cfg.check_compiler({'CMAKE_CXX_COMPILER': 'clang++'}, {'CMAKE_CXX_COMPILER': ('STRING', 'C:/Program Files/LLVM/bin/clang++.exe')})   # the same compiler by name: fine
    return p


def n5_rows(root, scratch, main, store_env):
    p = []
    tree = cfg.WorkTree(root)
    real = cfg.scan_reads(tree)
    if real:
        p.append('N5 the real tree has reads syntax_cfg does not mirror: %s' % real[:3])
    base = (tree.read('CMakeLists.txt') or b'').decode('utf-8', 'replace').replace('\r\n', '\n')
    plants = {'file(STRINGS on an unlisted path': 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/tools/zz/unlisted.txt" ZZ)\n',
              'file(READ': 'file(READ "tools/zz/unlisted_read.txt" ZZ)\n', 'configure_file': 'configure_file(tools/zz/unlisted_cf.in out.h)\n',
              'file(GLOB': 'file(GLOB ZZ "${CMAKE_CURRENT_SOURCE_DIR}/zz_unlisted/*.txt")\n', 'if(EXISTS': 'if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zz_unlisted/f.txt")\nendif()\n',
              'include': 'include(zz_unlisted/x.cmake)\n'}
    for name, line in plants.items():
        got = cfg.scan_reads(cfg.WorkTree(root, {'CMakeLists.txt': (base + line).encode()}))
        if not got:
            p.append('N5 a planted %s was not reported' % name)
    # the guard itself: include(src/<tracked file>) reads a placeholder -> ConfigureFailed (about 5 s)
    os.environ['SYNTAX_CFG_DIR'] = store_env
    try:
        cfg.commands(cfg.WorkTree(root, {'CMakeLists.txt': (base + '\ninclude(src/main.cpp)\n').encode()}), main, 'on')
        p.append('N5 guard twin: include(src/main.cpp) (a placeholder) produced flags')
    except cfg.ConfigureFailed as e:
        if 'placeholder' not in str(e):
            p.append('N5 guard twin: ConfigureFailed for another reason: %s' % str(e)[:160])
    finally:
        os.environ.pop('SYNTAX_CFG_DIR', None)
    return p


def n6_rows(scratch):
    p = []
    for rel in cfg.REAL_INPUTS:
        a, b = os.path.join(scratch, 'n6a'), os.path.join(scratch, 'n6b')
        for d in (a, b):
            for r in cfg.REAL_INPUTS:
                os.makedirs(os.path.dirname(os.path.join(d, r)), exist_ok=True)
                open(os.path.join(d, r), 'w').write('same\n')
        open(os.path.join(a, rel), 'w').write('different\n')
        why = gs.cmake_state(a, b, [])
        if not any(rel in w for w in why):
            p.append('N6 cmake_state did not see a difference in %s' % rel)
    why = ['planted: syntax_cfg unavailable']
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gs.fixed_list_verdict(why, 2)
    if rc != 4 or 'NEEDS-MAIN' not in buf.getvalue():
        p.append('N6 the fixed-list mode on the fallback gave rc %s (want 4 NEEDS-MAIN)' % rc)
    with contextlib.redirect_stdout(io.StringIO()):
        rc = gs.fixed_list_verdict([], 2)
    if rc != 0:
        p.append('N6 control: the fixed-list mode with no reasons gave rc %s' % rc)
    return p


def n8_row(root, scratch):
    js = os.path.join(scratch, 'ov8.json')
    open(os.path.join(scratch, 'ov8.txt'), 'w').write('# x\n')
    json.dump({'CMakeLists.txt': os.path.join(scratch, 'ov8.txt')}, open(js, 'w'))
    old = os.environ.get(cfg.OVERRIDE_ENV)
    os.environ[cfg.OVERRIDE_ENV] = js
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            cfg.WorkTree(root)
    finally:
        if old is None:
            os.environ.pop(cfg.OVERRIDE_ENV, None)
        else:
            os.environ[cfg.OVERRIDE_ENV] = old
    return [] if 'OVERRIDE ACTIVE' in buf.getvalue() else ['N8 SYNTAX_CFG_OVERRIDE_JSON replaced a file without saying OVERRIDE ACTIVE']


def n9_row(root, main):
    p = []

    def boom_unavailable(*a, **k):
        raise cfg.Unavailable('planted: not a configure failure')
    if not t3_function(root, main, boom_unavailable):
        p.append('N9 T3 accepted an Unavailable (any exception passed)')

    def boom_failed(*a, **k):
        raise cfg.ConfigureFailed('planted')
    if t3_function(root, main, boom_failed):
        p.append('N9 control: T3 rejected a ConfigureFailed')
    return p


def n12_row(scratch, main):
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'handback'))
    import cvar_pp   # noqa: E402
    tree = os.path.join(scratch, 'n12tree')
    os.makedirs(tree, exist_ok=True)
    open(os.path.join(tree, 'CMakeLists.txt'), 'w').write('# TREE_MARK_N12\n')
    o1, o2 = gs.main_checkout, gs.build_lines_for
    gs.main_checkout, gs.build_lines_for = (lambda r: main), (lambda *a, **k: [])
    try:
        fl = cvar_pp.Flags(tree)
    finally:
        gs.main_checkout, gs.build_lines_for = o1, o2
    return [] if 'TREE_MARK_N12' in fl.main_cmake else ['N12 cvar_pp judges "does the configured build skip it" against main\'s CMakeLists.txt, not the tree\'s']


def n3_row():
    p = []
    if cfg_status([], {'on': False, 'off': False}) != 'NOT JUDGED':
        p.append('N3 a P1 with nothing judged does not say NOT JUDGED')
    if cfg_status([], {'on': True, 'off': True}) != 'ok':
        p.append('N3 control: a judged, clean P1 is not ok')
    if cfg_status(['x'], {'on': True, 'off': True}) != 'FAIL':
        p.append('N3 control: a P1 with a difference is not FAIL')
    return p


def cfg_status(probs, judged):
    return p1_status(probs, judged)


def fix_rows(root, main, scratch):
    res = {}
    res['N2 stale only when dead'] = _guard(n2_lock_rows, scratch)
    res['N3 NOT JUDGED is said'] = _guard(n3_row)
    res['N6 cmake_state + fixed-list'] = _guard(n6_rows, scratch)
    res['N8 override is loud'] = _guard(n8_row, root, scratch)
    res['N9 T3 only ConfigureFailed'] = _guard(n9_row, root, main)
    res['N11 preset forms refused'] = _guard(n11_rows)
    res['N12 cvar_pp tree CMakeLists'] = _guard(n12_row, scratch, main)
    try:
        res.update(store_rows(root, main, scratch))
    except Exception as e:   # noqa: BLE001
        res['store rows'] = ['store_rows raised %s: %s' % (type(e).__name__, str(e)[:240])]
    sd = os.path.join(scratch, 'store5')
    os.makedirs(sd, exist_ok=True)
    res['N5 reads mirrored + guard twin'] = _guard(n5_rows, root, scratch, main, sd)
    return res


def run_a7(a7):
    if not os.path.isdir(os.path.join(a7, 'src')):
        print('BLOCKED: %s is not a tree (the A7 time-trial worktree is gone; nothing judged)' % a7)
        return 3
    main = gs.main_checkout(a7)
    t0 = time.time()
    r = _proc.run([sys.executable, '-I', os.path.join(HERE, 'game_syntax.py'), '--root', a7, '--changed'], timeout=900)
    out = (r.stdout or '') + (r.stderr or '')
    tail = [l for l in out.strip().splitlines() if not l.startswith('[admit]')][-3:]
    print('A7 game_syntax --changed: rc %s in %.0f s; last lines: %s' % (r.returncode, time.time() - t0, ' | '.join(tail)))
    probs = []
    if r.returncode != 0:
        probs.append('P2 A7: rc %s, want 0 (it was 4 before syntax_cfg)' % r.returncode)
    if 'NEEDS-MAIN' in out:
        probs.append('P2 A7: NEEDS-MAIN still printed')
    if not re.search(r'^PASS: \d+ unit-builds', out, re.M):
        probs.append('P2 A7: no `PASS: N unit-builds` line')
    # the flags are A7's own: its CMakeLists lacks the CRT define the s106 tip has (git diff s106 tt/A7), so its game-unit flags must lack it, the main tree's must carry it
    a7_on = norm(cfg.commands(a7, main, 'on'), main)
    me_on = norm(cfg.commands(cfg.WorkTree(main), main, 'on'), main)
    key = (main.replace('\\', '/') + '/src/start_settings.cpp').lower()
    a7_has = any(CRT in ' '.join(t) for t in a7_on.get(key, []))
    me_has = any(CRT in ' '.join(t) for t in me_on.get(key, []))
    cm = (cfg.WorkTree(a7).read('CMakeLists.txt') or b'').decode('utf-8', 'replace')
    has_block = CRT_BLOCK.strip() in cm.replace('\r\n', '\n')   # the GAME target's define block (the launcher target names the define too)
    print('A7 flags carry %s: %s (its CMakeLists %s the game target block); main tree: %s' % (CRT, a7_has, 'has' if has_block else 'lacks', me_has))
    if a7_has != has_block:
        probs.append('P2 A7: its flags and its CMakeLists.txt disagree about %s' % CRT)
    for q in probs:
        print('FAIL: ' + q)
    print('A7: %s' % ('FAIL' if probs else 'ok'))
    return 1 if probs else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--mutant', choices=('stale',), default='')
    ap.add_argument('--a7', default='')
    ap.add_argument('--only', choices=('fix',), default='', help='fix = only the GSMAINF fix rows (N1-N12)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.a7:
        return run_a7(os.path.abspath(a.a7))
    main_dir = gs.main_checkout(root)
    if not main_dir or not os.path.isfile(os.path.join(main_dir, gs.BUILD_DIRS['on'], 'build.ninja')):
        print('BLOCKED: the main checkout / its configured out\\build was not found (%s): nothing judged' % main_dir)
        return 3
    scratch = os.path.join(root, 'out', 'syntax_cfg_selftest')
    os.makedirs(scratch, exist_ok=True)
    before = snap_build(main_dir)
    t0 = time.time()
    bad = 0
    if a.mutant:   # the OLD behaviour: flags from main's out\build whatever the tree says
        def stale(_root, main, build, overrides=None):
            return gs.build_commands(main, gs.BUILD_DIRS[build])
        res = twins(root, main_dir, scratch, stale)
        for name, probs in res.items():
            if probs and not probs[0].startswith(name.split()[0] + ' cannot'):
                print('ok   mutant caught: %s (%s)' % (name, probs[0][:110]))
            else:
                print('FAIL mutant NOT caught: %s %s' % (name, probs[:1]))
                bad += 1
        print('mutant stale: %s' % ('every twin fails the old way: ok' if not bad else 'a twin passes the old way: FAIL'))
        return 1 if bad else 0
    if a.only == 'fix':
        allp = fix_rows(root, main_dir, scratch)
        for name, ps in allp.items():
            print('%s %s' % ('ok  ' if not ps else 'FAIL', name))
            for q in ps[:6]:
                print('       ' + q)
            bad += bool(ps)
        print('fix rows: %s (%.0f s)' % ('FAIL' if bad else 'ok', time.time() - t0))
        return 1 if bad else 0
    probs, notes, judged = p1_parity(main_dir, scratch)
    for n in notes:
        print(n)
    allp = {'P1 parity': probs}
    allp['P1 detector'] = p1_detector(main_dir)
    allp.update(twins(root, main_dir, scratch, cfg.commands))
    allp['T3 end to end'] = t3_end_to_end(root, scratch)
    allp['T4 mtimes'] = t4_detector(scratch)
    after = snap_build(main_dir)
    allp['T4 mtimes (out\\build)'] = [] if after == before else ['T4 out\\build changed during the run: %s' % [k for k in set(before) | set(after) if before.get(k) != after.get(k)][:5]]
    allp['T5 fallback'] = t5_fallback(root, main_dir)
    allp.update(fix_rows(root, main_dir, scratch))
    unjudged = 0
    for name, ps in allp.items():
        st = p1_status(ps, judged) if name == 'P1 parity' else ('FAIL' if ps else 'ok')
        print('%-11s %s' % (st if st != 'ok' else 'ok', name))
        for q in ps[:6]:
            print('       ' + q)
        bad += st == 'FAIL'
        unjudged += st == 'NOT JUDGED'
    print(cfg.note_line())
    print('syntax_cfg selftest: %s (%.0f s)' % ('FAIL' if bad else 'NOT JUDGED (a reference could not be built: nothing compared)' if unjudged else 'ok', time.time() - t0))
    return 1 if bad else 3 if unjudged else 0


if __name__ == '__main__':
    sys.exit(main())

r"""S103 lane H1: the shared compile cache under MASS PARALLELISM (the old poison appeared when many worktrees built at once).

  py -I tools\compile\cxx_cache_conc.py [--trees 4] [--keep]     exit 1 on any failed claim

Four copies of the renderer sources (src\, tools\scene, the helper, thirdparty headers) are built AT THE SAME TIME with tools\scene\build_rsc.ps1 against ONE
shared cache dir (cold), then again all at once (warm). The trees differ: w1 = plain, w2 / w3 = a header (record_frame.h) with a different extern constant each,
w4 = record_frame.cpp with its own global. Afterwards every tree's 36 objects are compared (code, data, relocations, symbols; tools\compile\cxx_cache_proof.py sig)
with the same tree built UNCACHED (WWE_NO_CCACHE=1, one tree at a time). Claims: no cross-contamination (cached == own uncached, per object, per tree), the twins'
objects differ from the plain tree's where they must, both rounds exit 0, and the sccache server reports 0 cache read / write / other errors."""
import argparse, ctypes, importlib.util, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('proof', str(Path(__file__).with_name('cxx_cache_proof.py')))
proof = importlib.util.module_from_spec(spec); spec.loader.exec_module(proof)
FAILS = []
TOTAL = [8]
PER_TREE = [16]   # clang jobs per build_rsc.ps1 (its ThrottleLimit), set by cap_concurrency()


def free_ram_gb():
    try:
        class MS(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                        ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                        ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        m = MS(); m.dwLength = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys / 2**30
    except Exception:
        return None


def cap_concurrency(ntrees):
    """Total clang++ at once for the whole test: at most 8, one per 2.5 GB of FREE RAM above a 6 GB reserve (like game_syntax._syntax_workers), at least ntrees
    (each tree needs one). The per-tree share is written into each tree's OWN copy of build_rsc.ps1 (ThrottleLimit 16 x 4 trees was up to 64 clang at once, H1C)."""
    free = free_ram_gb()
    total = 8 if free is None else int((free - 6) // 2.5)
    total = max(ntrees, min(8, total))
    TOTAL[0] = total
    PER_TREE[0] = max(1, total // ntrees)
    print('concurrency cap: free RAM %s GB -> at most %d clang at once in the cached rounds (%d trees x %d); the serial uncached reference runs one tree of %d' % (
        'unknown' if free is None else '%.1f' % free, PER_TREE[0] * ntrees, ntrees, PER_TREE[0], total), flush=True)
    return total


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def thirdparty():
    t = REPO / 'thirdparty' / 'rexglue-sdk-src' / 'thirdparty'
    if not (t / 'stb' / 'stb_image.h').exists():
        common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        t = Path(common).parent / 'thirdparty' / 'rexglue-sdk-src' / 'thirdparty'
    return t


def make_tree(root, i, tp):
    shutil.copytree(REPO / 'src', root / 'src', ignore=shutil.ignore_patterns('*.spv'))
    (root / 'tools').mkdir(parents=True)
    shutil.copytree(REPO / 'tools' / 'scene', root / 'tools' / 'scene', ignore=shutil.ignore_patterns('*.exe', '*.o'))
    for f in ('cxx_cache.ps1', 'env.ps1'):
        shutil.copy(REPO / 'tools' / f, root / 'tools' / f)
    for d in ('stb', 'vulkan-headers', 'xxHash'):
        if (tp / d).exists():
            shutil.copytree(tp / d, root / 'thirdparty' / 'rexglue-sdk-src' / 'thirdparty' / d)
    bp = root / 'tools' / 'scene' / 'build_rsc.ps1'
    txt = bp.read_text()
    assert 'ThrottleLimit 16' in txt
    bp.write_text(txt.replace('ThrottleLimit 16', 'ThrottleLimit %d' % PER_TREE[0]))
    if i in (2, 3):
        with open(root / 'src' / 'renderer_scene' / 'record_frame.h', 'a') as f:
            f.write('\n__attribute__((used)) static const int h1_twin_hdr = %d;\n' % i)
    if i == 4:
        with open(root / 'src' / 'renderer_scene' / 'record_frame.cpp', 'a') as f:
            f.write('\n__attribute__((used)) static int h1_twin_cpp = 4;\n')


def build_all(trees, env_extra, serial=False):
    env = dict(os.environ, PATH=proof.LLVM + os.pathsep + os.environ['PATH'], SCCACHE_IDLE_TIMEOUT='30')
    env.pop('WWE_NO_CCACHE', None); env.pop('SCCACHE_DIRECT', None); env.update(env_extra)
    cmd = lambda t: ['pwsh', '-NoProfile', '-File', str(t / 'tools' / 'scene' / 'build_rsc.ps1'), '-Clean']
    if serial:   # the uncached reference: one tree at a time (64 plain clang++ at once ran the PC out of RAM, H1C)
        for t in trees:   # a lone tree may use the whole budget
            bp = t / 'tools' / 'scene' / 'build_rsc.ps1'
            bp.write_text(re.sub(r'ThrottleLimit \d+', 'ThrottleLimit %d' % TOTAL[0], bp.read_text()))
        done = [subprocess.run(cmd(t), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1500) for t in trees]
        return [p.returncode for p in done], [p.stdout for p in done]
    ps = [subprocess.Popen(cmd(t), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) for t in trees]
    outs = [p.communicate(timeout=1500)[0] for p in ps]
    return [p.returncode for p in ps], outs


def show_fails(trees, rc, outs):
    for t, r, o in zip(trees, rc, outs):
        if r:
            lines = [l for l in o.splitlines() if l.startswith('FAILED') or ': error:' in l or 'out of memory' in l.lower() or 'failed' in l]
            print('  %s build FAILED:\n    %s' % (t.name, '\n    '.join(lines[:40]) if lines else o[-1500:]))


def objsigs(t, d):
    return {p.name: proof.sig(p) for p in sorted((t / 'out' / 'tools' / d).glob('*.o'))}


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--trees', type=int, default=4); ap.add_argument('--keep', action='store_true'); a = ap.parse_args()
    (REPO / 'out').mkdir(exist_ok=True)
    base = Path(tempfile.mkdtemp(prefix='cxxconc_', dir=str(REPO / 'out')))
    try:
        cap_concurrency(a.trees)
        tp = thirdparty(); cache = base / 'cache'
        trees = [base / ('w%d' % i) for i in range(1, a.trees + 1)]
        for i, t in enumerate(trees, 1):
            make_tree(t, i, tp)
        env = {'WWE_CCACHE_DIR': str(cache)}
        rc, outs = build_all(trees, env)
        say(all(r == 0 for r in rc), 'cold round: %d concurrent build_rsc.ps1 against one empty cache, exit codes %s' % (len(trees), rc))
        show_fails(trees, rc, outs)
        for t, o in zip(trees, outs):
            m = re.search(r'\[cxx-cache\] on: (\d+) compiles, (\d+) hits, (\d+) misses', o.replace('\n', ' '))
            print('  %s: %s' % (t.name, ('shared-server delta while all trees build: ' + m.group(0)) if m else 'NO CACHE LINE: ' + o[-300:]))
            shutil.move(str(t / 'out' / 'tools' / 'rsc_obj'), str(t / 'out' / 'tools' / 'obj_cold'))
        rc, outs = build_all(trees, env)   # warm
        say(all(r == 0 for r in rc), 'warm round: all at once again, exit codes %s' % rc)
        show_fails(trees, rc, outs)
        for t, o in zip(trees, outs):
            m = re.search(r'\[cxx-cache\] on: (\d+) compiles, (\d+) hits, (\d+) misses', o.replace('\n', ' '))
            print('  %s: %s' % (t.name, ('shared-server delta while all trees build: ' + m.group(0)) if m else 'NO CACHE LINE'))
            shutil.move(str(t / 'out' / 'tools' / 'rsc_obj'), str(t / 'out' / 'tools' / 'obj_warm'))
        # server health
        r = subprocess.run(['pwsh', '-NoProfile', '-Command', '$repo = "%s"; . "%s"; & $script:cxxSccache --show-stats' % (trees[0], REPO / 'tools' / 'cxx_cache.ps1')],
                           env=dict(os.environ, PATH=proof.LLVM + os.pathsep + os.environ['PATH'], WWE_CCACHE_DIR=str(cache)), capture_output=True, text=True)
        errs = {m.group(1): int(m.group(2)) for m in re.finditer(r'^(Cache read errors|Cache write errors|Cache errors|Non-cacheable compilations|Unsupported compiler calls|Compile requests executed|Cache hits)\s+(\d+)', r.stdout, re.M)}
        print('  server stats:', errs)
        say(errs.get('Compile requests executed', 0) > 0 and errs.get('Cache hits', 0) > 0 and errs.get('Cache read errors', 1) == 0 and errs.get('Cache write errors', 1) == 0 and errs.get('Cache errors', 1) == 0,
            'the one sccache server (read right after the warm round, while alive) served %d concurrent clients: compile requests > 0, hits > 0, 0 read / write / other errors' % len(trees))
        rc, outs = build_all(trees, {'WWE_NO_CCACHE': '1', 'WWE_CCACHE_DIR': str(cache)}, serial=True)   # the reference: uncached, one tree at a time
        say(all(r == 0 for r in rc), 'uncached reference builds, exit codes %s' % rc)
        show_fails(trees, rc, outs)
        ref = {}
        for t in trees:
            ref[t.name] = objsigs(t, 'rsc_obj')
        for rnd in ('obj_cold', 'obj_warm'):
            bad = []
            for t in trees:
                got = objsigs(t, rnd)
                want = ref[t.name]
                miss_ref = sorted(set(got) - set(want)); miss_got = sorted(set(want) - set(got))
                if miss_ref:
                    bad.append('%s: %d object(s) missing from the UNCACHED reference (a reference build failure, not a cache verdict): %s' % (t.name, len(miss_ref), ', '.join(miss_ref[:4])))
                if miss_got:
                    bad.append('%s: %d object(s) missing from the cached round: %s' % (t.name, len(miss_got), ', '.join(miss_got[:4])))
                bad += ['%s/%s DIFFERS' % (t.name, k) for k in got if k in want and got[k] != want[k]]
            n = sum(len(v) for v in ref.values())
            say(not bad, '%s: %d objects over %d trees, each equals the SAME tree built uncached%s' % (rnd, n, len(trees), '' if not bad else ' - DIFFERENT: ' + ', '.join(bad[:8])))
        # the twins really differ (so equality above is not vacuous): record_frame.o and a dependent of record_frame.h
        k = 'src_renderer_scene_record_frame.cpp.o'
        s = {t.name: ref[t.name][k] for t in trees}
        say(len(set(s.values())) == len(trees) or a.trees < 4, 'the four trees\' record_frame.o are all different (plain / hdr 2 / hdr 3 / cpp 4): %s' % s)
    finally:
        if not a.keep:
            shutil.rmtree(base, ignore_errors=True)
    print('\n%s: %d claim(s) failed' % ('FAIL' if FAILS else 'PASS', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

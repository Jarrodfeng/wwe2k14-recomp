# entry: gate
# Selftest for the fast gate (S102 lane M4). py -I tools\gate\fastgate_selftest.py
# A scratch git repo in a temp folder, fake lanes whose checks only append to a counter file, the real gate.py run on it (--root TMP --out TMP/out):
#   1 a change to one renderer file runs only the lanes that declare it (and the lane without "affects"), not the others
#   2 a second run with no change: every cacheable check is a cache hit (its counter does not move); a check without "affects" runs again
#   3 changing a file inside "affects" invalidates the cache; changing one outside it does not
#   4 PLANTED: a program older than its sources is refused, FAIL "stale build", the check does not run (the S101 movie_ours bug, 2026-11-12: a --no-build
#     check ran on an exe older than the merge and gave a false result); a fresh program passes; --build-stale rebuilds a stale registered exe instead
#   5 a FAIL is never cached; a SKIP (missing input) is never cached
#   6 lanes run in parallel (two 3 s lanes: --jobs 2 is at least 2 s faster than --jobs 1) and times.csv holds one row per check plus the whole gate
#   7 glob matching: ** crosses folders, * does not
import csv, json, os, subprocess, sys, tempfile, time, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fastgate as fg

GATE = HERE / 'gate.py'
PY = sys.executable
FAILS = []


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def sh(root, *a):
    subprocess.run(['git', '-C', str(root)] + list(a), check=True, capture_output=True, text=True)


COUNT_PY = '''
import sys, time
name, secs, rc = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
t0 = time.time()
time.sleep(secs)
open("counts_" + name + ".txt", "a").write("x")
open("span_" + name + ".txt", "w").write("%f %f" % (t0, time.time()))
sys.exit(rc)
'''
BUILD_PY = '''
import os
os.makedirs("out", exist_ok=True)
open("out/reg.exe", "wb").write(b"built")
open("builds.txt", "a").write("x")
'''


def count(root, name):
    p = Path(root) / f'counts_{name}.txt'
    return len(p.read_text()) if p.exists() else 0


def chk(name, cnt, secs=0.0, rc=0, **extra):
    d = {'name': name, 'cmd': [PY, '-I', 'count.py', cnt, str(secs), str(rc)], 'timeout_s': 60, 'writes': [f'counts_{cnt}.txt', f'span_{cnt}.txt']}   # S103 PS2: the script's output names are built at run time: declared
    d.update(extra)
    return d


def write_lane(root, name, checks, **top):
    (Path(root) / 'tools' / 'gate' / 'lanes' / f'{name}.json').write_text(json.dumps(dict(top, checks=checks)), encoding='utf-8')


def gate(root, *flags):
    env = dict(os.environ, GATE_NO_RESTORE='1', GATE_VERIFY_P='0')   # S103 PS2: no random cache audit inside the counting tests (sched_selftest.py tests the audit)
    p = subprocess.run([PY, '-I', str(GATE), '--root', str(root), '--out', str(Path(root) / 'out_gate'), '--no-toolcache', '--gpu-slots', '0'] + list(flags), capture_output=True, text=True, env=env)
    return p.returncode, p.stdout + p.stderr


def run():
    base = Path(tempfile.mkdtemp(prefix='fastgate_selftest_'))
    try:
        root = base / 'repo'
        (root / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
        for d in ('src/renderer', 'src/other', 'out'):
            (root / d).mkdir(parents=True)
        (root / 'count.py').write_text(COUNT_PY)
        (root / 'build.py').write_text(BUILD_PY)
        (root / 'src/renderer/a.cpp').write_text('v1')
        (root / 'src/renderer/sub').mkdir()
        (root / 'src/renderer/sub/deep.cpp').write_text('v1')
        (root / 'src/other/b.cpp').write_text('v1')
        write_lane(root, 'l_a', [chk('l_a.one', 'a')], affects=['src/renderer/**'])
        write_lane(root, 'l_b', [chk('l_b.one', 'b')], affects=['src/other/**'])
        write_lane(root, 'l_c', [chk('l_c.one', 'c')])                                    # no affects: always runs, never cached
        sh(root, 'init', '-q', '-b', 'trunk')
        sh(root, 'config', 'user.email', 't@t')
        sh(root, 'config', 'user.name', 't')
        (root / '.gitignore').write_text('out_gate/\ncounts_*.txt\nbuilds.txt\nout/\n')
        sh(root, 'add', '-A')
        sh(root, 'commit', '-q', '-m', 'base')

        # 1 ---- selection
        (root / 'src/renderer/a.cpp').write_text('v2')
        rc, out = gate(root, '--changed', 'HEAD')
        ok(rc == 0 and count(root, 'a') == 1 and count(root, 'c') == 1 and count(root, 'b') == 0, f'1 one renderer file changed -> l_a and the lane without affects ran, l_b did not (rc {rc}; {out.strip().splitlines()[0]})')
        ok('2 of 3 checks selected' in out, '1b the gate says how many checks were selected')
        # 2 ---- cache
        rc, out = gate(root, '--changed', 'HEAD')
        ok(count(root, 'a') == 1 and count(root, 'c') == 2, f'2 second run, no change: l_a is a cache hit (still 1), l_c ran again (now {count(root, "c")})')
        ok('cache hit' in out, '2b the output says cache hit')
        # 3 ---- invalidation
        (root / 'src/renderer/sub/deep.cpp').write_text('v2')
        gate(root, '--changed', 'HEAD')
        ok(count(root, 'a') == 2, '3 a content change in a nested file under affects -> re-run')
        (root / 'src/other/b.cpp').write_text('v2')
        gate(root, '--changed', 'HEAD')
        ok(count(root, 'a') == 2 and count(root, 'b') == 1, '3b a change outside l_a\'s affects keeps its cache and runs l_b')
        gate(root, '--changed', 'HEAD', '--no-cache')
        ok(count(root, 'a') == 3, '3c --no-cache re-runs')
        # 4 ---- stale program
        exe = root / 'out' / 'tool.exe'
        exe.write_bytes(b'tool')
        old = time.time() - 3600
        os.utime(exe, (old, old))
        write_lane(root, 'l_a', [chk('l_a.one', 'a', programs=[{'path': 'out/tool.exe', 'sources': ['src/renderer/**']}])], affects=['src/renderer/**'])
        n0 = count(root, 'a')
        rc, out = gate(root, '--changed', 'HEAD')
        line = [l for l in out.splitlines() if 'l_a.one' in l]
        ok(rc == 3 and line and line[0].startswith('BLOCKED') and 'stale build' in line[0] and count(root, 'a') == n0, f'4 PLANTED stale program -> BLOCKED stale build, check not run ({line[:1]})')
        missing = base / 'x'
        write_lane(root, 'l_a', [chk('l_a.one', 'a', programs=[{'path': 'out/nope.exe', 'sources': ['src/renderer/**']}])], affects=['src/renderer/**'])
        rc, out = gate(root, '--changed', 'HEAD')
        ok(rc == 3 and 'missing' in out and 'stale build' in out and 'BLOCKED' in out, '4b PLANTED missing program -> BLOCKED stale build (missing)')   # S105 E: could not run = BLOCKED, exit 3
        write_lane(root, 'l_a', [chk('l_a.one', 'a', programs=[{'path': 'out/tool.exe', 'sources': ['src/renderer/**']}])], affects=['src/renderer/**'])
        now = time.time() + 5
        os.utime(exe, (now, now))
        rc, out = gate(root, '--changed', 'HEAD')
        ok(rc == 0 and count(root, 'a') == n0 + 1, '4c a fresh program passes and the check runs')
        gate(root, '--changed', 'HEAD')
        ok(count(root, 'a') == n0 + 1, '4d ... and the next run is a cache hit (the program hash is part of the key)')
        exe.write_bytes(b'tool rebuilt, new bytes')
        os.utime(exe, (now + 10, now + 10))
        gate(root, '--changed', 'HEAD')
        ok(count(root, 'a') == n0 + 2, '4e a rebuilt program (new bytes) invalidates the cache')
        # registered exe (the gate's own build-once exes): stale -> FAIL in --changed, rebuilt with --build-stale
        spec = {'path': 'out/reg.exe', 'build': [PY, 'build.py'], 'timeout_s': 60, 'sources': {'dirs': ['src/renderer'], 'files': []}}
        write_lane(root, 'l_a', [chk('l_a.one', 'a', exes=[spec])], affects=['src/renderer/**'])
        reg = root / 'out' / 'reg.exe'
        reg.write_bytes(b'old')
        os.utime(reg, (old, old))
        n1 = count(root, 'a')
        rc, out = gate(root, '--changed', 'HEAD')
        ok(rc == 3 and 'BLOCKED' in out and 'stale build' in out and count(root, 'a') == n1 and not (root / 'builds.txt').exists(), '4f stale registered exe in --changed: FAIL stale build, not built, check not run')
        rc, out = gate(root, '--changed', 'HEAD', '--build-stale')
        ok(rc == 0 and (root / 'builds.txt').exists() and count(root, 'a') == n1 + 1, '4g --build-stale builds it once and the check runs')
        # 5 ---- failures and skips are never cached
        write_lane(root, 'l_a', [chk('l_a.one', 'a')], affects=['src/renderer/**'])
        write_lane(root, 'l_f', [chk('l_f.one', 'f', rc=1)], affects=['src/renderer/**'])
        write_lane(root, 'l_s', [chk('l_s.one', 's', needs=['out/does/not/exist'])], affects=['src/renderer/**'])
        gate(root, '--changed', 'HEAD')
        rc, out = gate(root, '--changed', 'HEAD')
        ok(count(root, 'f') == 2 and rc == 1, '5 a failing check runs again (never cached) and the gate exits 1')
        ok(count(root, 's') == 0 and 'SKIP' in out, '5b a SKIP does not run and is not a pass')
        # 5c S103 GRF: a "retired" check is a SKIP with its reason (never a pass, never run); the same check without the key runs
        write_lane(root, 'l_r', [dict(chk('l_r.one', 'r'), retired='tests a path that no longer ships')], affects=['src/renderer/**'])
        rc, out = gate(root, '--changed', 'HEAD', '--no-cache')
        ok(count(root, 'r') == 0 and 'retired: tests a path that no longer ships' in out, '5c a retired check does not run and reads SKIP with its reason')
        write_lane(root, 'l_r', [chk('l_r.one', 'r')], affects=['src/renderer/**'])
        gate(root, '--changed', 'HEAD', '--no-cache')
        ok(count(root, 'r') == 1, '5d control: without "retired" the same check runs')
        (root / 'tools' / 'gate' / 'lanes' / 'l_r.json').unlink()
        # 6 ---- parallel + times
        for n in ('f', 's'):
            (root / 'tools' / 'gate' / 'lanes' / f'l_{n}.json').unlink()
        write_lane(root, 'p1', [chk('p1.one', 'p1', 3.0)], affects=['src/renderer/**'])
        write_lane(root, 'p2', [chk('p2.one', 'p2', 3.0)], affects=['src/renderer/**'])
        (root / 'src/renderer/a.cpp').write_text('v3')
        def spans():
            return [tuple(float(x) for x in (root / f'span_{n}.txt').read_text().split()) for n in ('p1', 'p2')]
        overlap = lambda s: min(s[0][1], s[1][1]) > max(s[0][0], s[1][0])   # the two checks' [start, end] intervals intersect
        rc1, out1 = gate(root, '--changed', 'HEAD', '--jobs', '1', '--no-cache')
        s1 = spans()
        rc, out = gate(root, '--changed', 'HEAD', '--jobs', '2', '--no-cache')
        s2 = spans()
        ok(rc == 0 and rc1 == 0 and count(root, 'p1') == 2 and count(root, 'p2') == 2, f'6 both slow lanes ran in both modes (rc {rc1}, {rc})')
        ok(not overlap(s1) and overlap(s2), f'6b --jobs 1: the two checks run one after the other; --jobs 2: their run intervals overlap (a timing-free test)')
        rows = list(csv.DictReader(open(root / 'out_gate' / 'times.csv', newline='')))
        whole = [r for r in rows if r['lane'] == '(whole gate)']
        ok(rows and whole and len(whole) >= 5 and any(r['check'] == 'p1.one' for r in rows) and float(whole[-1]['seconds']) > 0,
           f'6c times.csv: {len(rows)} rows, {len(whole)} whole-gate rows, per-check rows present')
        # 7 ---- globs
        ok(fg.matches('src/renderer/sub/x.cpp', ['src/renderer/**']) and fg.matches('src/renderer/x.cpp', ['src/renderer/*'])
           and not fg.matches('src/renderer/sub/x.cpp', ['src/renderer/*']) and fg.matches('tools/a/b/c.py', ['tools/**/*.py'])
           and not fg.matches('src/renderers/x.cpp', ['src/renderer/**']), '7 glob matching: ** crosses folders, * does not, prefixes are exact')
        # ---- review round 1 (2026-11-14): every finding that can be a check is one
        (root / 'tools' / 'gate' / 'lanes' / 'p1.json').unlink()
        (root / 'tools' / 'gate' / 'lanes' / 'p2.json').unlink()
        write_lane(root, 'l_a', [chk('l_a.one', 'a')], affects=['src/renderer/**'])
        sh(root, 'add', '-A')
        sh(root, 'commit', '-q', '-m', 'round 1')
        # 9 --no-renames: a file moved OUT of a declared folder must select the lane (the old path is in the list)
        sh(root, 'mv', 'src/renderer/a.cpp', 'src/other/a.cpp')
        ch = fg.changed_files(root, 'HEAD')
        ok('src/renderer/a.cpp' in ch, f'9 git mv out of a declared folder: the OLD path is in the changed list ({ch})')
        n0 = count(root, 'a')
        gate(root, '--changed', 'HEAD', '--no-cache')
        ok(count(root, 'a') == n0 + 1, '9b ... and the lane that declares that folder runs')
        sh(root, 'reset', '-q', '--hard', 'HEAD')
        # 10 serial worker: two lanes without affects writing one file must not interleave under --jobs 4; the control (parallel_safe) does
        (root / 'shared.py').write_text('import sys, time\nn = sys.argv[1]\nf = open("shared.txt", "a")\nf.write(n + " start\\n"); f.flush()\ntime.sleep(2.0)\n'
                                        'f.write(n + " end\\n"); f.close()\n')
        for n in ('s1', 's2'):
            write_lane(root, n, [{'name': f'{n}.one', 'cmd': [PY, '-I', 'shared.py', n], 'timeout_s': 60}])
        gate(root, '--changed', 'HEAD', '--jobs', '4', '--no-cache')
        lines = (root / 'shared.txt').read_text().split('\n')[:4]
        ok([l.split()[1] for l in lines] == ['start', 'end', 'start', 'end'], f'10 two no-affects lanes under --jobs 4 do not interleave ({lines})')
        (root / 'shared.txt').unlink()
        for n in ('s1', 's2'):
            write_lane(root, n, [{'name': f'{n}.one', 'cmd': [PY, '-I', 'shared.py', n], 'timeout_s': 60, 'parallel_safe': True}])
        gate(root, '--changed', 'HEAD', '--jobs', '4', '--no-cache')
        lines = (root / 'shared.txt').read_text().split('\n')[:4]
        ok([l.split()[1] for l in lines if l.strip()] != ['start', 'end', 'start', 'end'], f'10b control: with parallel_safe they DO overlap, so the test can see interleaving ({lines})')
        for n in ('s1', 's2'):
            (root / 'tools' / 'gate' / 'lanes' / f'{n}.json').unlink()
        (root / 'bld.ps1').write_text('x')
        ok(fg.serial_needed({'cmd': ['pwsh', '-File', 'bld.ps1'], 'affects': ['a']}) and not fg.serial_needed({'cmd': [PY, 'x.py'], 'affects': ['a']})
           and fg.serial_needed({'cmd': [PY, 'x.py']}), '10c a .ps1 build check and a check without affects are serial; an affects check with a script is not')
        # 11 cache key: needs, cmd-argument files, env, opt-out
        main = base / 'main'
        (main / 'out').mkdir(parents=True)
        (main / 'out' / 'need.bin').write_bytes(b'aaaa')
        (main / 'out' / 'capdir').mkdir()
        (main / 'out' / 'capdir' / 'f.bin').write_bytes(b'1234')
        ck = chk('k.one', 'k', affects=['src/other/**'], needs=['out/need.bin', 'out/capdir'])
        k0 = fg.cache_key(root, ck, b'lane', [], None, main)
        ok(k0 and fg.cache_key(root, ck, b'lane', [], None, main) == k0, '11 the key is stable for unchanged inputs')
        (main / 'out' / 'need.bin').write_bytes(b'aaab')
        k1 = fg.cache_key(root, ck, b'lane', [], None, main)
        ok(k1 != k0, '11b one byte changed in a needs FILE changes the key')
        (main / 'out' / 'capdir' / 'f.bin').write_bytes(b'12345')
        ok(fg.cache_key(root, ck, b'lane', [], None, main) != k1, '11c a changed file in a needs FOLDER changes the key')
        k2 = fg.cache_key(root, ck, b'lane', [], None, main)
        (root / 'count.py').write_text(COUNT_PY + '\n# edited\n')
        ok(fg.cache_key(root, ck, b'lane', [], None, main) != k2, '11d an edited script named in the cmd changes the key')
        k3 = fg.cache_key(root, ck, b'lane', [], None, main)
        os.environ['RDR_SELFTEST_X'] = '1'
        ok(fg.cache_key(root, ck, b'lane', [], None, main) != k3, '11e an allow-listed environment variable changes the key')
        del os.environ['RDR_SELFTEST_X']
        ok(fg.cache_key(root, dict(ck, cache=False), b'lane', [], None, main) is None, '11f "cache": false means no key at all')
        write_lane(root, 'l_a', [chk('l_a.one', 'a', cache=False)], affects=['src/renderer/**'])
        (root / 'src/renderer/sub/deep.cpp').write_text('v9')
        gate(root, '--changed', 'HEAD')
        n1 = count(root, 'a')
        gate(root, '--changed', 'HEAD')
        ok(count(root, 'a') == n1 + 1, '11g end to end: a check with "cache": false runs again on an unchanged tree')
        # 12 affects validation at load
        write_lane(root, 'l_e', [chk('l_e.one', 'e')], affects=[])
        rc, out = gate(root, '--changed', 'HEAD')
        ok(rc == 1 and 'non-empty list' in out and count(root, 'e') == 0, '12 an EMPTY affects fails at load, nothing runs')
        write_lane(root, 'l_e', [chk('l_e.one', 'e')], affects=['src/nowhere/**'])
        rc, out = gate(root, '--changed', 'HEAD')
        ok(rc == 1 and 'matches no file' in out and count(root, 'e') == 0, '12b an affects glob that matches no file fails at load')
        (root / 'tools' / 'gate' / 'lanes' / 'l_e.json').unlink()
        # 13 default BASE = merge-base with main
        rc, out = gate(root, '--changed')
        ok(rc == 1 and 'main' in out, '13 no BASE and no session branch: FAIL naming the branch')
        sh(root, 'branch', 'main')
        (root / 'src/other/b.cpp').write_text('v99')
        rc, out = gate(root, '--changed', '--no-cache')
        ok('--changed ' in out and 'changed path(s)' in out and 'l_b.one' in out, f'13b default BASE is the merge-base with main ({out.splitlines()[0][:80]})')
        # 13c S103 H3: the cache is on for --only and for a full run too (it used to need --changed)
        gate(root, '--only', 'l_b')
        nb = count(root, 'b')
        rc, out = gate(root, '--only', 'l_b')
        ok(count(root, 'b') == nb and 'cache hit' in out, '13c --only: the second run is a cache hit (no --changed needed)')
        rc, out = gate(root, '--only', 'l_b', '--no-cache')
        ok(count(root, 'b') == nb + 1, '13d --only --no-cache still re-runs')
        # 14 the cache file is replaced atomically and merges
        cp = base / 'cache.json'
        fg.save_cache(cp, {'a': {'verdict': 'PASS'}})
        fg.save_cache(cp, {'b': {'verdict': 'PASS'}})
        ok(set(fg.load_cache(cp)) == {'a', 'b'} and not list(base.glob('cache.json.*.tmp')), '14 save_cache merges a second writer\'s keys and leaves no temp file')
        # a bad BASE is a loud FAIL
        rc, out = gate(root, '--changed', 'no-such-rev')
        ok(rc == 1 and 'FAIL' in out and '--changed' in out, '8 an unknown BASE fails loudly')
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print(f'fastgate selftest: {"FAIL, " + str(len(FAILS)) + " check(s)" if FAILS else "all checks hold"}')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run())

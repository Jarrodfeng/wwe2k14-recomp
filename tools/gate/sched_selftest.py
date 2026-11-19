r"""Selftest for the gate's scheduler and honest proof cache (S103 lane PS2). py -I tools\gate\sched_selftest.py

  F  footprints: a declared "writes" wins; a .py that only RUNS an exe reads it; a script that writes and names no path is an UNKNOWN barrier; a pure lint
     writes nothing; --out X is a write; the GPU slot is inferred from code, not from a docstring
  L  PathLocks: two writers of one path never overlap; readers share; a reader waits for a writer; disjoint paths overlap; unknown conflicts with everything
  G  GpuSlots: N holders at most; a holder process KILLED frees its slot at once (an OS lock: nothing can go stale)
  P  END TO END, real gate.py on a scratch repo: two lanes writing the same path serialise under --jobs 4; the control with disjoint writes overlaps
  K  proof cache keys: a source edit, a capture edit that keeps size AND mtime, an imported module edit change the key; a touch alone does not; a
     check with no affects / an import we cannot find / a planted-fault twin without its pair returns WHY and no key
  C  proof cache end to end: a FAIL is never served; a PASS is a hit; PLANTED hole (an input outside the key changes the verdict): without the audit the
     hit stays wrong, with --verify-cache it is a loud FAIL "CACHE MISMATCH" and the entry is dropped
  T  planted-fault twin: cached only together with its real check, under a key bound to the real check's; alone it runs every time
  R  the Opus review of PS2: a rebuilt exe / a child script's capture literal / out/build/build.ninja / a same-size re-capture change the key; a wrapper and a direct
     frame_rig check serialise on out/cases; --check is no shortcut with a subprocess; a .ps1 in a -Command body is uncacheable; invalidate() for F4
  GST S103 GST (docs/research/s103/GST.md): a build .ps1's footprint is its exe; honest slot holders; path-wait log + writer preference; a killed gate keeps its rows and cache;
     --deadline NOT RUN; the stall watchdog; GPU timeout +50 %; the twin's timeouts
  J  default jobs for a 5950X (32 threads) and for small machines
Every test that can plant a fault does, and expects the check to see it.

S103 PS3 (speed: ~165 s -> ~21 s wall, the same 132 cases): every group (GROUPS) has its own scratch repo / folder and runs on a thread pool; sleeps and deadlines are the smallest that
still show the behaviour (a deadline / stall / min-left case sets the window by env and checks both sides with seconds of slack, so a loaded PC cannot flip it); a "check took 30 s"
case adds the times.csv row instead of waiting 30 s. test_jobs runs first and alone (it sets GATE_JOBS in os.environ). `SCHED_SELFTEST_WORKERS` = pool size (default 16);
the run prints the slowest groups and the slowest cases ("(s)").  The lane runs only when tools/gate/** (or the twin scripts it reads) change.
Run one group alone: import sched_selftest, call one of GROUPS with a scratch Path."""
import json, os, re, subprocess, sys, tempfile, threading, time, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sched as gs
import proofkey as pk
import fastgate as fg
import fastgate_selftest as fst

FAILS = []
PY = sys.executable


TIMES = []   # (seconds since this thread's previous case ended, case id): the per-case table printed at the end
_TL = threading.local()


def ok(cond, text):
    now = time.time()
    TIMES.append((now - getattr(_TL, 'last', now), text.split(' ', 1)[0]))
    _TL.last = now
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


MEET_PY = """
import os, sys, time
me, other = sys.argv[1], sys.argv[2]
t0 = time.time()
open('span_m_' + me + '.txt', 'w').write('1')
while not os.path.exists('span_m_' + other + '.txt') and time.time() - t0 < 50:
    time.sleep(0.02)
time.sleep(0.05)
open('span_' + me + '.txt', 'w').write('%f %f' % (t0, time.time()))
"""


def meet_chk(root, n, **extra):
    """a check that ends only after the OTHER lane's check has started: both are alive at once whatever the load (a serialised pair would wait 50 s: never used in the negative cases)"""
    (Path(root) / 'meet.py').write_text(MEET_PY)
    other = 'w2' if n == 'w1' else 'w1'
    d = {'name': f'{n}.one', 'cmd': [PY, '-I', 'meet.py', n, other], 'timeout_s': 120, 'writes': [f'span_{n}.txt', f'span_m_{n}.txt']}
    d.update(extra)
    return d


def span_overlap(root, a, b):
    s = [tuple(float(x) for x in (Path(root) / f'span_{n}.txt').read_text().split()) for n in (a, b)]
    return min(s[0][1], s[1][1]) > max(s[0][0], s[1][0])


# ---------------------------------------------------------------- F
def test_footprints(base):
    root = base / 'fp'
    (root / 'tools' / 'x').mkdir(parents=True)
    (root / 'tools' / 'x' / 'writer.py').write_text("import os\nos.makedirs('out/s103/w', exist_ok=True)\nopen(os.path.join('out','s103','w','a.txt'),'w').write('x')\n")
    (root / 'tools' / 'x' / 'runner.py').write_text("import subprocess\nsubprocess.run(['out/tools/frame_rig.exe'])\nOUT = 'out/s103/r'\n")
    (root / 'tools' / 'x' / 'lint.py').write_text("import sys\nprint('ok')\n")
    (root / 'tools' / 'x' / 'blind.py').write_text("open('somewhere.txt', 'w').write('x')\n")
    (root / 'tools' / 'x' / 'doc.py').write_text('"""runs fview and rplay"""\nprint(1)\n')
    fp = lambda cmd, **k: gs.footprint(dict({'name': 'n', 'cmd': cmd}, **k), root, root)
    f = fp([PY, '-I', 'tools/x/writer.py'])
    ok('out/s103/w' in f['writes'] and not f['unknown'], f'F1 a script that writes names its out path: {sorted(f["writes"])}')
    f = fp([PY, '-I', 'tools/x/runner.py'])
    ok('out/tools/frame_rig.exe' in f['reads'] and 'out/tools/frame_rig.exe' not in f['writes'] and f['gpu'], f'F2 an exe a script only RUNS is a read, and a rig exe takes a GPU slot ({sorted(f["reads"])})')
    f = fp([PY, '-I', 'tools/x/lint.py'])
    ok(not f['writes'] and not f['unknown'] and not f['gpu'], 'F3 a pure lint writes nothing, is not unknown, and takes no GPU slot')
    f = fp([PY, '-I', 'tools/x/blind.py'])
    ok(f['unknown'], 'F4 PLANTED: a script that writes and names no output path is an UNKNOWN barrier (never assumed safe)')
    f = fp([PY, '-I', 'tools/x/blind.py'], writes=['out/x/blind'])
    ok(not f['unknown'] and f['writes'] == {'out/x/blind'}, 'F5 a declared "writes" makes the same check known')
    f = fp(['tools/x/prog.exe', '--out', 'out/s103/prog'])
    ok('out/s103/prog' in f['writes'], 'F6 --out X is a write')
    f = fp([PY, '-I', 'tools/x/doc.py'])
    ok(not f['gpu'], 'F7 a docstring that names fview does not take a GPU slot')
    f = fp([PY, '-I', 'tools/x/lint.py'], gpu=True)
    ok(f['gpu'], 'F8 "gpu": true forces a slot')
    f = fp(['pwsh', '-NoProfile', '-Command', 'clang++ a.cpp -o out/tools/z_unit.exe'])
    ok('out/tools/z_unit.exe' in f['writes'], 'F9 a compile command: the -o file is a write')


# ---------------------------------------------------------------- L
def run_threads(pl, jobs, secs=0.2):
    """jobs: [(name, footprint)] run on threads; -> {name: (start, end)}"""
    span = {}

    def go(name, fp):
        tok = pl.acquire(fp, name)
        t0 = time.time()
        time.sleep(secs)
        span[name] = (t0, time.time())
        pl.release(tok)
    ts = [threading.Thread(target=go, args=j) for j in jobs]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return span


def ov(span, a, b):
    return min(span[a][1], span[b][1]) > max(span[a][0], span[b][0])


def fpd(r=(), w=(), u=False):
    return {'reads': set(r), 'writes': set(w), 'unknown': u, 'gpu': False}


def test_locks():
    s = run_threads(gs.PathLocks(), [('w1', fpd(w=['out/a'])), ('w2', fpd(w=['out/a']))])
    ok(not ov(s, 'w1', 'w2'), 'L1 two writers of one path never overlap')
    s = run_threads(gs.PathLocks(), [('r1', fpd(r=['out/a'])), ('r2', fpd(r=['out/a']))])
    ok(ov(s, 'r1', 'r2'), 'L2 two readers of one path run together')
    s = run_threads(gs.PathLocks(), [('w', fpd(w=['out/tools/x.exe'])), ('r', fpd(r=['out/tools/x.exe']))])
    ok(not ov(s, 'w', 'r'), 'L3 a reader waits for a writer of the same exe')
    s = run_threads(gs.PathLocks(), [('w', fpd(w=['out/tools'])), ('r', fpd(r=['out/tools/x.exe']))])
    ok(not ov(s, 'w', 'r'), 'L4 a writer of a folder conflicts with a reader of a file inside it')
    s = run_threads(gs.PathLocks(), [('w1', fpd(w=['out/a'])), ('w2', fpd(w=['out/b']))])
    ok(ov(s, 'w1', 'w2'), 'L5 writers of disjoint paths overlap')
    s = run_threads(gs.PathLocks(), [('w1', fpd(w=['out/ab'])), ('w2', fpd(w=['out/a']))])
    ok(ov(s, 'w1', 'w2'), 'L6 out/ab is not inside out/a (a prefix is a path prefix, not a string prefix)')
    s = run_threads(gs.PathLocks(), [('u', fpd(u=True)), ('r', fpd(r=['out/zzz']))])
    ok(not ov(s, 'u', 'r'), 'L7 an unknown-writes check conflicts even with a reader of an unrelated path')
    # all-or-nothing: two checks each needing two paths in opposite order cannot deadlock
    done = []
    pl = gs.PathLocks()

    def two(name, ps):
        tok = pl.acquire(fpd(w=ps), name)
        time.sleep(0.05)
        pl.release(tok)
        done.append(name)
    ts = [threading.Thread(target=two, args=('a', ['out/p', 'out/q'])), threading.Thread(target=two, args=('b', ['out/q', 'out/p']))]
    for t in ts:
        t.start()
    for t in ts:
        t.join(10)
    ok(sorted(done) == ['a', 'b'], 'L8 two checks taking the same two paths in opposite order do not deadlock')


# ---------------------------------------------------------------- G
HOLD = r'''
import sys, time
sys.path.insert(0, sys.argv[1])
import sched as gs
s = gs.GpuSlots(sys.argv[2], 1)
t = s.acquire('holder')
open(sys.argv[3], 'w').write('held')
time.sleep(60)
'''


def test_slots(base):
    d = base / 'slots'
    s = gs.GpuSlots(d, 2, poll=0.05)
    a, b = s.acquire('a'), s.acquire('b')
    got = []
    th = threading.Thread(target=lambda: got.append(s.acquire('c')))
    th.start()
    time.sleep(0.2)
    ok(not got, 'G1 with 2 slots both taken, a third check waits')
    s.release(a)
    th.join(5)
    ok(len(got) == 1, 'G2 a released slot is taken by the waiter')
    s.release(b)
    s.release(got[0])
    # a holder in ANOTHER process, killed: the slot frees at once (OS byte-range lock: nothing to go stale)
    d1 = base / 'slots1'
    flag = base / 'held.flag'
    hs = base / 'hold.py'
    hs.write_text(HOLD)
    p = subprocess.Popen([PY, '-I', str(hs), str(HERE), str(d1), str(flag)])
    t0 = time.time()
    while not flag.exists() and time.time() - t0 < 20:
        time.sleep(0.05)
    s1 = gs.GpuSlots(d1, 1, poll=0.05)
    res = []
    th = threading.Thread(target=lambda: res.append(s1.acquire('after')))
    th.start()
    time.sleep(0.3)
    ok(flag.exists() and not res, 'G3 a slot held by another process blocks this gate')
    p.kill()
    p.wait()
    t1 = time.time()
    th.join(10)
    ok(len(res) == 1 and time.time() - t1 < 5, f'G4 PLANTED: the holder was killed (no release call): its slot is free within {time.time() - t1:.2f} s, no stale lock to clean')
    if res:
        s1.release(res[0])
    s2 = gs.GpuSlots(d1, 1, poll=0.05)
    tok = s2.acquire('zed_check')
    ok('zed_check' in s2.holders(), f'G5 holders() names the holder of a LOCKED slot (it read byte 0 and failed on Windows): {s2.holders()}')
    s2.release(tok)
    # S103 GSP2: a check that runs N GPU processes holds N slots (acquire(n=N): a list token, gathered under the ticket)
    d3 = base / 'slots_n'
    s3 = gs.GpuSlots(d3, 4, poll=0.05)
    one = s3.acquire('single')
    big = s3.acquire('big', n=3)
    ok(isinstance(big, list) and len({i for _, i in big}) == 3 and one[1] not in {i for _, i in big}, 'G6 acquire(n=3) of 4 slots gives 3 distinct free slots')
    got = []
    th = threading.Thread(target=lambda: got.append(s3.acquire('big2', n=3)))
    th.start()
    time.sleep(0.2)
    ok(not got, 'G7 a second 3-slot ask waits while only 1 slot is free')
    s3.release(big)
    th.join(5)
    ok(len(got) == 1 and isinstance(got[0], list) and len(got[0]) == 3, 'G8 the waiting 3-slot ask gets 3 slots once they are free')
    s3.release(got[0])
    s3.release(one)
    big = s3.acquire('over', n=9)
    ok(len(big) == 4, f'G9 an ask for more slots than exist is clamped to the pool ({len(big)} of 4)')
    s3.release(big)
    # no starvation: a gatherer waiting for the last slot of a 2-slot pool is not overtaken by a single asker
    d4 = base / 'slots_s'
    s4 = gs.GpuSlots(d4, 2, poll=0.05)
    hold = s4.acquire('holder')
    gath, late = [], []
    tg = threading.Thread(target=lambda: gath.append(s4.acquire('gatherer', n=2)))
    tg.start()
    time.sleep(0.2)
    tl = threading.Thread(target=lambda: late.append(s4.acquire('late_single')))
    tl.start()
    time.sleep(0.2)
    s4.release(hold)
    tg.join(5)
    time.sleep(0.2)
    ok(len(gath) == 1 and not late, 'G10 a single asker arriving while a gatherer waits does not overtake it (no starvation)')
    if gath:
        s4.release(gath[0])
    tl.join(5)
    ok(len(late) == 1, 'G11 the single asker runs once the gatherer is done')
    if late:
        s4.release(late[0])


# ---------------------------------------------------------------- scratch repo
def make_repo(base, name='repo'):
    root = base / name
    (root / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
    for d in ('src/a', 'src/b', 'out'):
        (root / d).mkdir(parents=True)
    (root / 'count.py').write_text(fst.COUNT_PY)
    (root / 'src/a/x.cpp').write_text('v1')
    (root / 'src/b/y.cpp').write_text('v1')
    fst.sh(root, 'init', '-q', '-b', 'trunk')
    fst.sh(root, 'config', 'user.email', 't@t')
    fst.sh(root, 'config', 'user.name', 't')
    (root / '.gitignore').write_text('out_gate/\ncounts_*.txt\nspan_*.txt\nout/\n')
    fst.sh(root, 'add', '-A')
    fst.sh(root, 'commit', '-q', '-m', 'base')
    return root


def test_parallel(base):
    root = make_repo(base, 'par')
    for n in ('w1', 'w2'):
        fst.write_lane(root, n, [fst.chk(f'{n}.one', n, 0.7, writes=['out/shared/file'])], affects=['src/a/**'])
    fst.gate(root, '--jobs', '4', '--no-cache')
    ok(not span_overlap(root, 'w1', 'w2'), 'P1 two lanes that both write out/shared/file run one after the other under --jobs 4 (a real path conflict)')
    for n in ('w1', 'w2'):
        fst.write_lane(root, n, [meet_chk(root, n, writes=[f'out/{n}/file', f'span_{n}.txt', f'span_m_{n}.txt'])], affects=['src/a/**'])
    fst.gate(root, '--jobs', '4', '--no-cache')
    ok(span_overlap(root, 'w1', 'w2'), 'P2 control: the same two lanes with disjoint writes overlap (so P1 can see a serialisation)')


def test_parallel_noaffects(base):
    root = make_repo(base, 'par3')
    # a lane without affects used to force the serial worker; with its writes declared it runs beside the others
    for n in ('w1', 'w2'):
        c = meet_chk(root, n, writes=[f'out/{n}/file', f'span_{n}.txt', f'span_m_{n}.txt'])
        fst.write_lane(root, n, [c])
    fst.gate(root, '--jobs', '4', '--no-cache')
    ok(span_overlap(root, 'w1', 'w2'), 'P3 two lanes WITHOUT affects but with declared writes overlap (the old serial worker is gone)')


def test_parallel_blind(base):
    root = make_repo(base, 'par4')
    # an undeclared writer is an unknown barrier: it does not run beside another check
    (root / 'blind.py').write_text('import sys, time\nt0 = time.time()\ntime.sleep(0.7)\nopen("span_" + sys.argv[1] + ".txt", "w").write("%f %f" % (t0, time.time()))\n')
    fst.write_lane(root, 'w1', [{'name': 'w1.one', 'cmd': [PY, '-I', 'blind.py', 'w1'], 'timeout_s': 60}])
    fst.write_lane(root, 'w2', [fst.chk('w2.one', 'w2', 0.7, writes=['out/w2/file'])])
    fst.gate(root, '--jobs', '4', '--no-cache')
    ok(not span_overlap(root, 'w1', 'w2'), 'P4 PLANTED: a check whose writes cannot be derived is a barrier: it runs alone')


def test_parallel_env(base):
    root = make_repo(base, 'par5')
    # per-check output folder: GATE_CHECK_DIR is set and differs per check
    (root / 'envdump.py').write_text('import os, sys\nopen("env_" + sys.argv[1] + ".txt", "w").write(os.environ.get("GATE_CHECK_DIR", "NONE"))\n')
    for n in ('w1', 'w2'):
        fst.write_lane(root, n, [{'name': f'{n}.one', 'cmd': [PY, '-I', 'envdump.py', n], 'timeout_s': 60, 'writes': [f'env_{n}.txt']}])
    fst.gate(root, '--jobs', '4', '--no-cache')
    e1, e2 = (root / 'env_w1.txt').read_text(), (root / 'env_w2.txt').read_text()
    ok(e1 != e2 and e1 != 'NONE' and str(root) in e1 and e1.endswith('w1.one'), f'P5 every check gets its own output folder in THIS checkout (GATE_CHECK_DIR: {e1[-40:]} / {e2[-40:]})')


# ---------------------------------------------------------------- K
def test_keys(base):
    root = make_repo(base, 'keys')
    main = base / 'keys_main'
    (main / 'out' / 'cap').mkdir(parents=True)
    cap = main / 'out' / 'cap' / 'f.bin'
    cap.write_bytes(b'A' * 100 + b'x' + b'B' * 100)
    (root / 'helper.py').write_text('X = 1\n')
    (root / 'proof.py').write_text('import helper\nprint(helper.X)\n')
    chk = {'name': 'k.one', 'cmd': [PY, '-I', 'proof.py'], 'needs': ['out/cap'], 'affects': ['src/a/**'], 'timeout_s': 60}
    key = lambda c=chk: fg.cache_key_ex(root, c, b'lane', [], None, main, 'tc')[0]
    k0 = key()
    ok(k0 and key() == k0, 'K1 the key is stable for unchanged inputs')
    (root / 'src/a/x.cpp').write_text('v2')
    k1 = key()
    ok(k1 != k0, 'K2 a source edit under affects changes the key')
    # a capture edit that keeps the size AND the mtime (a mid-file byte): the old size+mtime key could not see it
    st = os.stat(cap)
    cap.write_bytes(b'A' * 100 + b'y' + b'B' * 100)
    os.utime(cap, ns=(st.st_atime_ns, st.st_mtime_ns))
    pk._DIR_STATE.clear()
    k2 = key()
    ok(k2 != k1, 'K3 PLANTED: a capture edit that keeps size and mtime changes the key (content, not mtime)')
    old = fg.path_state(main / 'out' / 'cap')
    cap.write_bytes(b'A' * 100 + b'y' + b'B' * 100)
    os.utime(cap, ns=(st.st_atime_ns + 5, st.st_mtime_ns + 5_000_000_000))
    pk._DIR_STATE.clear()
    ok(key() == k2, 'K4 a touch alone (same bytes, new mtime: a tidy, a copy) keeps the key')
    ok(fg.path_state(main / 'out' / 'cap') != old, 'K4b ... which the old size+mtime state called a change')
    (root / 'helper.py').write_text('X = 2\n')
    pk._CLOSURE.clear()
    ok(key() != k2, 'K5 an edit to a module the script imports changes the key')
    (root / 'proof.py').write_text('import helper\nimport nonexistent_module_for_ps2\nprint(helper.X)\n')
    pk._CLOSURE.clear()
    k, why = fg.cache_key_ex(root, chk, b'lane', [], None, main, 'tc')
    ok(k is None and 'nonexistent_module_for_ps2' in why, f'K6 an import we cannot find: NO key, and it says why ({why})')
    k, why = fg.cache_key_ex(root, dict(chk, affects=None), b'lane', [], None, main, 'tc')
    k_na, why_na = fg.cache_key_ex(root, {k2_: v for k2_, v in chk.items() if k2_ != 'affects'}, b'lane', [], None, main, 'tc')
    ok(k_na is None and 'no affects' in why_na, f'K7 no affects: NO key, and it says why ({why_na})')
    tw = dict(chk, name='k.plant', cmd=[PY, '-I', 'proof.py', '--plant', 'x', '--expect-fail', 'T'])
    (root / 'proof.py').write_text('import helper\nprint(helper.X)\n')
    pk._CLOSURE.clear()
    k, why = fg.cache_key_ex(root, tw, b'lane', [], None, main, 'tc')
    ok(k is None and 'twin' in why, f'K8 a planted-fault twin without its pair: NO key ({why})')
    k, why = fg.cache_key_ex(root, tw, b'lane', [], None, main, 'tc', pair_key='real-key-1')
    k3, _ = fg.cache_key_ex(root, tw, b'lane', [], None, main, 'tc', pair_key='real-key-2')
    ok(k and k3 and k != k3, 'K9 a twin\'s key is bound to its real check\'s key (the real check changed: the twin misses)')
    ok(fg.cache_key_ex(root, chk, b'lane', [], None, main, 'tc')[0] != fg.cache_key_ex(root, chk, b'lane', [], None, main, 'other toolchain')[0], 'K10 the toolchain stamp is in the key')
    ok(pk.strip_plant(['pwsh', '-NoProfile', '-Command', 'out/tools/l8_unit.exe --plant own_fade_clock; if ($LASTEXITCODE -eq 0) { exit 1 } else { exit 0 }']) == ['out/tools/l8_unit.exe']
       and pk.strip_plant([PY, 'x.py', '--plant', 'old', '--expect-fail', 'PAGE PIXELS']) == [PY, 'x.py'] and pk.strip_plant([PY, 'x.py']) is None,
       'K11 plant flags are stripped to find the real check\'s command (python form and the pwsh exe form)')


# ---------------------------------------------------------------- C / T
def test_cache(base):
    root = make_repo(base, 'cache')
    chk = fst.chk
    fst.write_lane(root, 'ok_l', [chk('ok_l.one', 'o')], affects=['src/a/**'])
    fst.write_lane(root, 'bad_l', [chk('bad_l.one', 'f', rc=1)], affects=['src/a/**'])
    fst.gate(root)
    fst.gate(root)
    ok(fst.count(root, 'o') == 1 and fst.count(root, 'f') == 2, 'C1 a PASS is a hit (ran once in two gates); a FAIL is never served (ran twice)')


def test_cache_flip(base):
    root = make_repo(base, 'cflip')
    # PLANTED hole: the script's verdict depends on a file that is in no key (not named in cmd, not under affects)
    (root / 'flip.py').write_text('import os, sys\nsys.exit(1 if os.path.exists("fl" + "ip.txt") else 0)\n')
    fst.write_lane(root, 'flip_l', [{'name': 'flip_l.one', 'cmd': [PY, '-I', 'flip.py'], 'timeout_s': 60, 'affects': ['src/a/**'], 'writes': []}])
    rc, out = fst.gate(root, '--only', 'flip_l')
    ok(rc == 0, 'C2 the flip check passes at first')
    (root / 'flip.txt').write_text('1')
    rc, out = fst.gate(root, '--only', 'flip_l')
    ok(rc == 0 and 'cache hit' in out, 'C3 PLANTED hole: the input changed outside the key, and the cache (audit off) still says PASS: this is the wrong answer the audit exists to catch')
    rc, out = fst.gate(root, '--only', 'flip_l', '--verify-cache')
    ok(rc == 1 and 'CACHE MISMATCH' in out and 'FAIL' in out, 'C4 --verify-cache re-runs the hit, sees FAIL, and fails LOUDLY: CACHE MISMATCH')
    rc, out = fst.gate(root, '--only', 'flip_l')
    ok(rc == 1 and 'cache hit' not in out and 'MISMATCH' not in out.split('flip_l.one')[0], 'C5 the wrong entry was dropped: the next plain gate runs the check for real and FAILs')


def test_cache_verify(base):
    root = make_repo(base, 'cverify')
    fst.write_lane(root, 'ok_l', [fst.chk('ok_l.one', 'o')], affects=['src/a/**'])
    fst.gate(root, '--only', 'ok_l')
    n = fst.count(root, 'o')
    rc, out = fst.gate(root, '--only', 'ok_l', '--verify-cache')
    ok(fst.count(root, 'o') == n + 1 and 'hit-verified' in (Path(root) / 'out_gate' / 'last.json').read_text() and rc == 0, 'C6 --verify-cache on a TRUE hit re-runs it and passes (label hit-verified)')
    rc, out = fst.gate(root, '--only', 'ok_l', '--verify-p', '1.0')
    ok(fst.count(root, 'o') == n + 2, 'C7 --verify-p 1.0 re-runs every hit (the probability knob works); p 0 re-runs none (the other tests)')


def test_cache_uncacheable(base):
    root = make_repo(base, 'cuncache')
    # an uncacheable check says why in times.csv and in last.json, and is never cached
    fst.write_lane(root, 'na_l', [{'name': 'na_l.one', 'cmd': [PY, '-I', 'count.py', 'n', '0', '0'], 'timeout_s': 60, 'writes': ['counts_n.txt', 'span_n.txt']}])
    fst.gate(root, '--only', 'na_l')
    fst.gate(root, '--only', 'na_l')
    last = json.loads((Path(root) / 'out_gate' / 'last.json').read_text())
    ok(fst.count(root, 'n') == 2 and last['results'][0]['cache'].startswith('uncacheable: no affects'), f'C8 a check with no affects runs every time and is marked ({last["results"][0]["cache"]})')



def test_twins(base):
    root = make_repo(base, 'twins')
    chk = fst.chk
    # T ---- planted-fault twins
    fst.write_lane(root, 'tw', [chk('tw.real', 'r1'), dict(chk('tw.plant_x', 'r1'), cmd=[PY, '-I', 'count.py', 'r1', '0.0', '0', '--plant', 'x', '--expect-fail', 'T'])], affects=['src/b/**'])
    fst.gate(root, '--only', 'tw')
    fst.gate(root, '--only', 'tw')
    ok(fst.count(root, 'r1') == 2, f'T1 real check and its twin are both cached together: 2 runs in the first gate, 0 in the second (count {fst.count(root, "r1")})')
    (root / 'src/b/y.cpp').write_text('v2')
    fst.gate(root, '--only', 'tw')
    ok(fst.count(root, 'r1') == 4, 'T2 an input edit: both miss')
    fst.write_lane(root, 'tw2', [dict(chk('tw2.plant_alone', 'r2'), cmd=[PY, '-I', 'count.py', 'r2', '0.0', '0', '--plant', 'x', '--expect-fail', 'T'])], affects=['src/b/**'])
    fst.gate(root, '--only', 'tw2')
    fst.gate(root, '--only', 'tw2')
    ok(fst.count(root, 'r2') == 2, 'T3 a twin with no real check in its lane is never cached (ran in both gates)')
    # the real check FAILs: the twin's PASS must not be cached
    (root / 'pf.py').write_text('import os, sys\nopen("counts_r3.txt", "a").write("x")\nsys.exit(1 if os.path.exists("pf.txt") and "--plant" not in sys.argv else 0)\n')
    (root / 'pf.txt').write_text('1')
    real3 = {'name': 'tw3.real', 'cmd': [PY, '-I', 'pf.py'], 'timeout_s': 60, 'writes': ['counts_r3.txt']}
    twin3 = dict(real3, name='tw3.plant_x', cmd=[PY, '-I', 'pf.py', '--plant', 'x', '--expect-fail', 'T'])
    fst.write_lane(root, 'tw3', [real3, twin3], affects=['src/b/**'])
    fst.gate(root, '--only', 'tw3')
    fst.gate(root, '--only', 'tw3')
    ok(fst.count(root, 'r3') == 4, 'T4 PLANTED: the real check fails, so its twin\'s PASS is never cached (4 runs in 2 gates: the twin passes, its real check fails, nothing is cached)')


# ---------------------------------------------------------------- R (the Opus review of PS2: F1 .. F4)
def test_review(base):
    root = base / 'rv'
    main = base / 'rv_main'
    (root / 'tools' / 'x').mkdir(parents=True)
    (main / 'out' / 'captures' / 'a04').mkdir(parents=True)
    (main / 'out' / 'build' / 'win-amd64-release').mkdir(parents=True)
    (root / 'out' / 'tools').mkdir(parents=True)
    cap = main / 'out' / 'captures' / 'a04' / 'f.bin'
    cap.write_bytes(b'1234')
    bn = main / 'out' / 'build' / 'win-amd64-release' / 'build.ninja'
    bn.write_text('rule cc\n')
    exe = root / 'out' / 'tools' / 'frame_rig.exe'
    exe.write_bytes(b'MZ-one')
    X = root / 'tools' / 'x'
    # a child script names the capture and the build folder by literals; the wrapper names nothing
    (X / 'child.py').write_text("import os\nCAP = os.path.join(MAIN, 'out', 'captures', 'a04')\nBD = os.path.join('out', 'build', 'win-amd64-release')\n"
                                "RIG = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')\n")
    (X / 'wrap.py').write_text("import child\nprint(1)\n")
    chk = {'name': 'r.one', 'cmd': [PY, '-I', 'tools/x/wrap.py'], 'affects': ['tools/x/**'], 'timeout_s': 60}
    key = lambda c=chk: fg.cache_key_ex(root, c, b'lane', [], None, main, 'tc')
    pk._CLOSURE.clear()
    k0, why = key()
    ok(k0 and key()[0] == k0, f'R0 the wrapper with a child script is cacheable and stable ({why})')
    exe.write_bytes(b'MZ-two')
    k1, _ = key()
    ok(k1 != k0, 'R1 (F1) a rebuilt tool exe the script runs (frame_rig.exe) changes the key')
    cap.write_bytes(b'1235')
    pk._DIR_STATE.clear()
    k2, _ = key()
    ok(k2 != k1, 'R2 (F1) a capture named by a literal in a CHILD script changes the key')
    bn.write_text('rule cc2\n')
    pk._DIR_STATE.clear()
    k3, _ = key()
    ok(k3 != k2, 'R3 (F1) MAIN out/build/.../build.ninja changes the key')
    (main / 'out' / 'build' / 'win-amd64-release' / 'x.obj').write_bytes(b'object')
    pk._DIR_STATE.clear()
    ok(key()[0] == k3, 'R3b ... but an object file in the build folder does not (a build output is not an input)')
    old_v = fg.KEY_VERSION
    fg.KEY_VERSION = 'other'
    ok(key()[0] != k3, 'R4 (F1) the key recipe version / engine bytes are in the key')
    fg.KEY_VERSION = old_v
    (X / 'ps.ps1').write_text('. ./tools/env.ps1\nclang++ a.cpp -o out/tools/z.exe\n')
    c2 = {'name': 'r.ps', 'cmd': ['pwsh', '-NoProfile', '-Command', '. tools/x/ps.ps1; out/tools/frame_rig.exe --x y'], 'affects': ['tools/x/**'], 'timeout_s': 60}
    k, why = fg.cache_key_ex(root, c2, b'lane', [], None, main, 'tc')
    ok(k is None and '.ps1' in why, f'R5 (F1) a .ps1 inside a -Command body: uncacheable unless the lane declares exes / reads ({why})')
    k, why = fg.cache_key_ex(root, dict(c2, exes=['out/tools/frame_rig.exe']), b'lane', [], None, main, 'tc')
    ok(k, 'R6 ... and cacheable when the lane declares them')
    # F2: wrapper + direct frame_rig check serialise on out/cases
    (X / 'rig.py').write_text("import glob, os, subprocess\nbefore = set(glob.glob(os.path.join(ROOT, 'out', 'cases', 'fview_*')))\n"
                              "subprocess.run(['out/tools/fview.exe', '--save-case'])\nnew = set(glob.glob(os.path.join(ROOT, 'out', 'cases', 'fview_*'))) - before\n"
                              "OUT = os.path.join(ROOT, 'out', 's103', 'R2')\nopen(os.path.join(OUT, 'x.json'), 'w').write('{}')\n")
    (X / 'wrap_rig.py').write_text("import rig\nprint(rig)\n")
    fw = gs.footprint({'name': 'w', 'cmd': [PY, '-I', 'tools/x/wrap_rig.py']}, root, main)
    fd = gs.footprint({'name': 'd', 'cmd': [PY, '-I', 'tools/x/rig.py']}, root, main)
    ok('out/cases' in fw['writes'] and 'out/cases' in fd['writes'] and fw['gpu'], f'R7 (F2) the wrapper inherits its child\'s out/cases write and GPU slot: {sorted(fw["writes"])}')
    s = run_threads(gs.PathLocks(), [('w', fw), ('d', fd)])
    ok(not ov(s, 'w', 'd'), 'R8 (F2) a wrapper and a direct frame_rig check never overlap (two GPU checks cannot take each other\'s fview case)')
    # --check is read-only only when nothing in the closure starts a subprocess
    (X / 'chk.py').write_text("import sys, subprocess\nap.add_argument('--check')\nOUTP = 'out/atlas_check'\nif '--check' in sys.argv: pass\nelse: open(OUTP + '/x', 'w')\nsubprocess.run(['pipelines'])\n")
    f = gs.footprint({'name': 'c', 'cmd': [PY, '-I', 'tools/x/chk.py', '--check']}, root, main)
    ok('out/atlas_check' in f['writes'] or f['unknown'], 'R9 (F2) `--check` is no read-only shortcut when the script starts a subprocess (atlas pipelines.py writes under --check)')
    # F3: a re-capture with the same size, a changed middle byte and a new mtime: the key changes (full content at any size)
    big = main / 'out' / 'captures' / 'a04' / 'big.bin'
    big.write_bytes(b'A' * (5 << 20))
    pk._DIR_STATE.clear()
    pk._MEMO.clear()
    kb0 = key()[0]
    st = os.stat(big)
    data = bytearray(b'A' * (5 << 20))
    data[2 << 20] = 66
    big.write_bytes(bytes(data))
    os.utime(big, ns=(st.st_atime_ns, st.st_mtime_ns + 7_000_000_000))
    pk._DIR_STATE.clear()
    ok(key()[0] != kb0, 'R10 (F3) a re-capture with the same size and a changed middle byte changes the key (full content, no sample)')
    # F4: a folder state read before the locks is read AGAIN under them (invalidate)
    d = main / 'out' / 'captures' / 'a04'
    s0 = pk.content_state(d)
    (d / 'new.bin').write_bytes(b'late')
    ok(pk.content_state(d) == s0, 'R11 the folder state is memoised within a process (cheap for the other checks)')
    pk.invalidate([d])
    ok(pk.content_state(d) != s0, 'R12 (F4) invalidate() makes the state under the locks fresh: a write by an earlier check shows')


def test_stale_hit(base):
    """F4 end to end: lane a_l (re)builds out/x/data.txt from src/a/x.cpp; lane b_l reads it and passes only for v1. The gate that changes the producer's input must
    make b_l FAIL: its key was taken BEFORE a_l rebuilt the file in the old design, so the old PASS was served; now the key is taken under the locks."""
    root = make_repo(base, 'stale')
    (root / 'w.py').write_text("import os\nos.makedirs('out/x', exist_ok=True)\nopen('out/x/data.txt', 'w').write(open('src/a/x.cpp').read())\n")
    (root / 'rd.py').write_text("import sys\nsys.exit(0 if open(sys.argv[1]).read() == 'v1' else 1)\n")
    fst.write_lane(root, 'a_l', [{'name': 'a_l.build', 'cmd': [PY, '-I', 'w.py'], 'timeout_s': 60, 'writes': ['out/x/data.txt']}])
    fst.write_lane(root, 'b_l', [{'name': 'b_l.read', 'cmd': [PY, '-I', 'rd.py', 'out/x/data.txt'], 'timeout_s': 60, 'affects': ['src/b/**'], 'writes': []}])
    rc, out = fst.gate(root, '--jobs', '1')
    ok(rc == 0, 'S1 v1: the producer writes v1, the reader passes (and its PASS is cached under the v1 data)')
    (root / 'src/a/x.cpp').write_text('v2')
    rc, out = fst.gate(root, '--jobs', '1')
    ok(rc == 1 and 'b_l.read' in out and 'cache hit' not in out.split('b_l.read')[1].split('\n')[0], 'S2 PLANTED (F4): the producer now writes v2; the reader is NOT served its v1 PASS: it runs and FAILs')


def test_gate_serialise(base):
    """F2 end to end: a wrapper and a direct check that both glob out/cases never overlap, under --jobs 4."""
    root = make_repo(base, 'cases')
    (root / 'rig.py').write_text("import glob, os, sys, time\nt0 = time.time()\nbefore = set(glob.glob(os.path.join('out', 'cases', 'fview_*')))\n"
                                 "time.sleep(0.7)\nopen('span_' + sys.argv[1] + '.txt', 'w').write('%f %f' % (t0, time.time()))\n")
    (root / 'wrap.py').write_text("import os, sys\nsys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))\nimport rig\n")
    # rig.py runs its body on import / run: both take sys.argv[1] as the span name
    fst.write_lane(root, 'w1', [{'name': 'w1.one', 'cmd': [PY, '-I', 'wrap.py', 'w1'], 'timeout_s': 60}])
    fst.write_lane(root, 'w2', [{'name': 'w2.one', 'cmd': [PY, '-I', 'rig.py', 'w2'], 'timeout_s': 60}])
    fst.gate(root, '--jobs', '4', '--no-cache')
    ok((root / 'span_w1.txt').exists() and (root / 'span_w2.txt').exists() and not span_overlap(root, 'w1', 'w2'),
       'S3 (F2) end to end: a wrapper of rig.py and a direct rig.py run one after the other under --jobs 4 (the old scan read only the top script: both ran at once)')


# ---------------------------------------------------------------- GST (S103: why a main gate "never finished": docs\research\s103\GST.md)
def gate_cmd(root, *flags):
    return [PY, '-I', str(fst.GATE), '--root', str(root), '--out', str(Path(root) / 'out_gate'), '--no-toolcache', '--gpu-slots', '0'] + list(flags)


def gate_env(**extra):
    return dict(os.environ, GATE_NO_RESTORE='1', GATE_VERIFY_P='0', **extra)


def times_rows(root):
    p = Path(root) / 'out_gate' / 'times.csv'
    return p.read_text(encoding='utf-8').splitlines() if p.exists() else []


def test_gst(base):
    # F10 / F11: a build .ps1's write footprint is its output exe, not the folder it quotes as $outDir; with no exe named the folder stays and the footprint SAYS so
    root = base / 'gstfp'
    (root / 'tools' / 'x').mkdir(parents=True)
    (root / 'tools' / 'x' / 'b.ps1').write_text("$repo = 'x'\n$outDir = Join-Path $repo 'out\\tools'\n& clang++ a.cpp -o (Join-Path $outDir 'zz_rig.exe')\n")
    (root / 'tools' / 'x' / 'c.ps1').write_text("$repo = 'x'\n$outDir = Join-Path $repo 'out\\tools'\nNew-Item -ItemType Directory $outDir\nSet-Content (Join-Path $outDir 'a.txt') 'x'\n")
    fp = lambda s: gs.footprint({'name': 'n', 'cmd': ['pwsh', '-NoProfile', '-File', s]}, root, root)
    f = fp('tools/x/b.ps1')
    ok('out/tools/zz_rig.exe' in f['writes'] and 'out/tools' not in f['writes'], f'GST1 a build .ps1 writes its exe, not the whole out/tools folder: {sorted(f["writes"])}')
    f2 = gs.footprint({'name': 'n', 'cmd': [PY, '-I', 'tools/x/reader.py'], 'reads': ['out/tools/host_rig.exe'], 'writes': []}, root, root)
    s = run_threads(gs.PathLocks(), [('b', {'reads': set(), 'writes': set(f['writes']), 'unknown': False, 'gpu': False}), ('r', f2)])
    ok(ov(s, 'b', 'r'), 'GST2 a build of zz_rig.exe and a rig that reads host_rig.exe now run together (they waited for each other before)')
    f = fp('tools/x/c.ps1')
    ok('out/tools' in f['writes'] and 'whole-folder' in f['why'], f'GST3 PLANTED: a .ps1 that writes and names no exe keeps the whole folder AND says so: {f["why"][:140]}')
    # holders(): free slots are not holders; another gate's slot says whose
    d = base / 'gst_slots'
    a = gs.GpuSlots(d, 3, poll=0.05, who={'root': 'C:/gate_a', 'gate': 'A'})
    b = gs.GpuSlots(d, 3, poll=0.05, who={'root': 'C:/gate_b', 'gate': 'B'})
    ta = a.acquire('chk_a')
    h = b.holders()
    ok('0:chk_a' in h and 'other gate C:/gate_a' in h and 'pid %d' % os.getpid() in h, f'GST4 a slot held by ANOTHER gate says so (root, pid): {h}')
    ok('this gate' in a.holders(), f'GST5 the holder\'s own gate reads "this gate": {a.holders()}')
    a.release(ta)
    h = b.holders()
    ok('free (last: chk_a)' in h and 'other gate' not in h, f'GST6 a RELEASED slot prints "free (last: X)", never as a holder: {h}')
    ok(gs.GpuSlots(base / 'gst_slots_new', 2).holders() == 'all free', 'GST7 a never-used pool reads "all free"')
    # path waits: a log line after warn_s naming the holder; writer preference
    logs = []
    pl = gs.PathLocks(warn_s=0.15)
    t1 = pl.acquire(fpd(w=['out/a']), 'holder_chk')
    got = []
    th = threading.Thread(target=lambda: got.append(pl.acquire(fpd(r=['out/a']), 'waiter_chk', log=logs.append)))
    th.start()
    time.sleep(0.8)   # the waiter re-checks every 0.5 s: its warn line is out by then
    pl.release(t1)
    th.join(5)
    ok(len(logs) == 1 and 'waiter_chk' in logs[0] and 'holder_chk' in logs[0] and 'paths' in logs[0] and logs[0].startswith('[gate] ') and logs[0][7:9].isdigit(),
       f'GST8 a path wait past the warn time logs ONCE with a clock time, naming the holder: {logs}')
    pl.release(got[0])
    pl = gs.PathLocks()
    order = []
    r1 = pl.acquire(fpd(r=['out/tools/x.exe']), 'r1')

    def worker(name, fp_):
        tk = pl.acquire(fp_, name)
        order.append(name)
        time.sleep(0.1)
        pl.release(tk)
    tw = threading.Thread(target=worker, args=('writer', fpd(w=['out/tools/x.exe'])))
    tw.start()
    time.sleep(0.15)
    tr2 = threading.Thread(target=worker, args=('reader2', fpd(r=['out/tools/x.exe'])))
    tr2.start()
    time.sleep(0.25)
    ok(order == [], f'GST9 a later reader does NOT slip in beside a running reader while a writer is queued (writer preference): {order}')
    pl.release(r1)
    tw.join(5)
    tr2.join(5)
    ok(order == ['writer', 'reader2'], f'GST10 the queued writer gets in before the later reader: {order}')
    pl = gs.PathLocks()
    t1 = pl.acquire(fpd(w=['out/a']), 'h')
    err = []

    def late():
        try:
            pl.acquire(fpd(r=['out/a']), 'late', until=time.time() + 0.4)
        except gs.WaitDeadline as e:
            err.append(e)
    th = threading.Thread(target=late)
    th.start()
    th.join(5)
    ok(len(err) == 1, 'GST11 a path wait past its deadline raises WaitDeadline (the gate marks the check NOT RUN)')
    pl.release(t1)
    # a waiting writer that gives up does not leave readers blocked behind it
    ok(pl.acquire(fpd(r=['out/a']), 'after') is not None, 'GST12 a writer that left the queue (deadline) holds nobody back')



def test_gst_kill(base):
    # end to end, real gate.py on a scratch repo
    root = make_repo(base, 'gst')
    fst.write_lane(root, 'a_fast', [fst.chk('k.fast', 'kf', 0.2)], affects=['src/a/**'])
    fst.write_lane(root, 'b_slow', [fst.chk('k.slow', 'ks', 40)], affects=['src/a/**'])
    p = subprocess.Popen(gate_cmd(root, '--jobs', '2'), cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=gate_env())
    t0 = time.time()
    while time.time() - t0 < 180 and not any('k.fast' in r for r in times_rows(root)):
        time.sleep(0.2)
    subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], capture_output=True)   # the outer `timeout 3600` of the main gate
    p.wait(20)
    rows = times_rows(root)
    ck = fg.load_cache(Path(root) / 'out_gate' / 'cache.json')
    ok(any('k.fast' in r and 'PASS' in r for r in rows) and not any('(whole gate)' in r for r in rows), f'GST13 a KILLED gate keeps its finished rows in times.csv (no end-of-gate write ran): {[r[-40:] for r in rows][-3:]}')
    ok(any(v.get('check') == 'k.fast' for v in ck.values()), f'GST14 a KILLED gate keeps its finished PASS in cache.json ({len(ck)} entries)')
    rc, out = fst.gate(root, '--jobs', '2', '--only', 'a_fast')
    ok(rc == 0 and 'cache hit' in out, 'GST15 the next run serves the killed gate\'s PASS from the cache')
    m = [ln for ln in out.splitlines() if ln.startswith('PASS')]
    ok(bool(m) and re.match(r'^PASS\s+\S+\s+\S+\s+[\d.]+ s  @\d\d:\d\d:\d\d  ', m[0]), f'GST16 every row carries an @HH:MM:SS clock time after its seconds (verdict column still first): {m[:1]}')


def test_gst_deadline(base):
    root = make_repo(base, 'gstdl')
    # --deadline: nothing new after S; the running check is timed out at the deadline; the rest is NOT RUN; times / cache / summary still written
    fst.write_lane(root, 'a_fast', [fst.chk('k.fast', 'kf', 0.2)], affects=['src/a/**'])
    fst.write_lane(root, 'b_slow', [fst.chk('k.slow', 'ks', 40)], affects=['src/a/**'])
    fst.write_lane(root, 'c_after', [fst.chk('k.after1', 'k1', 0.1)], affects=['src/a/**'])
    fst.write_lane(root, 'd_after', [fst.chk('k.after2', 'k2', 0.1)], affects=['src/a/**'])
    shutil.rmtree(Path(root) / 'out_gate', ignore_errors=True)
    t0 = time.time()
    p = subprocess.run(gate_cmd(root, '--jobs', '1', '--deadline', '20'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_MIN_LEFT_S='0.2'))
    wall = time.time() - t0
    out = p.stdout + p.stderr
    rows = times_rows(root)
    ok(p.returncode == 5 and out.count('NOT RUN') >= 2 and 'k.after1' in out and fst.count(root, 'k1') == 0, f'GST17 --deadline: the checks after it are NOT RUN (exit 5 = only NOT RUN rows, review 10), wall {wall:.1f} s')
    ok(wall < 120 and re.search(r'^NOT RUN\s+\S+\s+k\.slow .*killed at the --deadline after \d+ s', out, re.M), 'GST18 the running check is killed at the deadline (never 40 s of it, never the 300 s default) and is NOT RUN, not a FAIL (review 3)')
    ok(any('(whole gate)' in r for r in rows) and any('k.after1' in r and 'NOT RUN' in r for r in rows) and re.search(r'^gate: .*3 of them NOT RUN', out, re.M),
       'GST19 --deadline still writes times.csv (NOT RUN rows + the whole-gate row) and the summary line')
    ok(any(v.get('check') == 'k.fast' for v in fg.load_cache(Path(root) / 'out_gate' / 'cache.json').values()), 'GST20 --deadline still writes cache.json (the finished PASS)')


def test_gst_stall(base):
    root = make_repo(base, 'gststall')
    # watchdog: a planted stall (a check that runs longer than the window): the report names what is running
    fst.write_lane(root, 'a_fast', [fst.chk('k.stall', 'kst', 1.5)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root, '--jobs', '2', '--no-cache'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_STALL_S='0.5'))
    out = p.stdout + p.stderr
    wd = [ln for ln in out.splitlines() if 'no check finished in' in ln]
    ok(p.returncode == 0 and wd and re.search(r'no check finished in 0.5 s: running k\.stall \(since \d\d:\d\d\)', wd[0]) and 'waiting for paths: none' in wd[0], f'GST21 the stall watchdog fires on a planted stall and names the running check: {wd[:1]}')
    fst.write_lane(root, 'a_fast', [fst.chk('k.stall', 'kst', 0.3)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root, '--jobs', '2', '--no-cache'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_STALL_S='30'))
    ok('no check finished' not in p.stdout + p.stderr, 'GST22 control: no stall report when checks finish inside the window')
    # DPC (QX4 / RFL stalls: a check held its path locks while its cache key hung, the stall report said "blocked by ?", and the writers queued behind it waited 20 min):
    # the hang is state 'key' in the report and times out (GATE_KEY_TIMEOUT) to an uncached run
    fst.write_lane(root, 'a_fast', [fst.chk('k.stall', 'kst', 0.2)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root, '--jobs', '2'), cwd=root, capture_output=True, text=True,
                       env=gate_env(GATE_STALL_S='1', GATE_KEY_TIMEOUT='3', GATE_TEST_KEY_HANG='k.stall', GATE_TEST_KEY_HANG_S='30'))
    out = p.stdout + p.stderr
    ok(p.returncode == 0 and 'k.stall: computing cache key since' in out and 'blocked by ?' not in out and 'did not finish in 3 s' in out
       and re.search(r'^PASS\s+\S+\s+k\.stall', out, re.M) is not None, f'DPC1 a hung cache key shows as "computing cache key since", times out and the check runs uncached: {out[-300:]!r}')

    # DPC2 (the root of the QX4 / RFL "blocked by ?" stalls, read from the RFL log: compute_key raised PermissionError on a locked file; the lane thread died
    # holding its path locks and every check queued behind it waited out the 20-minute deadline): a raising key = uncached run, and the locks are never leaked
    fst.write_lane(root, 'a_fast', [fst.chk('k.stall', 'kst', 0.2), fst.chk('k.next', 'knx', 0.2)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root, '--jobs', '2'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_TEST_KEY_RAISE='k.stall'))
    out = p.stdout + p.stderr
    ok(p.returncode == 0 and 'cache key of k.stall failed (PermissionError' in out and re.search(r'^PASS\s+\S+\s+k\.stall', out, re.M) is not None
       and re.search(r'^PASS\s+\S+\s+k\.next', out, re.M) is not None and 'Traceback' not in out, f'DPC2 a cache key that raises makes the check uncached; it neither kills the lane nor leaks its path locks: {out[-300:]!r}')


def test_gkl_key_once(base):
    """GKL (the L10.draw stall of 2026-11-19 05:53-06:03: a key that hashes huge out\\ folders was computed under the path locks, again after the GPU-slot wait, again ...):
    a cacheable GPU check misses, gives its paths up for a slot and takes them again: its key is still computed ONCE; a slow key is named in the stall report with its count."""
    root = make_repo(base, 'gklkey')
    cnt = Path(base) / 'gklkey_count.txt'
    cnt.write_text('', encoding='utf-8')
    fst.write_lane(root, 'a_gpu', [fst.chk('k.gpuk', 'kgk', 0.2, gpu=True)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root, '--jobs', '2', '--gpu-slots', '2'), cwd=root, capture_output=True, text=True,
                       env=gate_env(GATE_STALL_S='1', GATE_TEST_KEY_COUNT='k.gpuk', GATE_TEST_KEY_COUNT_FILE=str(cnt), GATE_TEST_KEY_SLOW_S='2.5', GATE_SLOTS_DIR=str(Path(root) / 'slots')))
    out = p.stdout + p.stderr
    n = len(cnt.read_text(encoding='utf-8').split())
    ok(p.returncode == 0 and re.search(r'^PASS\s+\S+\s+k\.gpuk', out, re.M) is not None and n == 1,
       f'GKL1 PLANT: a slow key of a cacheable GPU check (miss -> slot wait -> paths again) is computed once, not once per turn of the acquire loop: computed {n} time(s): {out[-300:]!r}')
    ok(re.search(r'k\.gpuk: computing cache key since \d\d:\d\d \(\d+ s so far, compute #1, 0 s in earlier ones\)', out) is not None,
       f'GKL2 the stall report names the key compute, its elapsed seconds and its count: {[l for l in out.splitlines() if "computing cache key" in l][:1]}')
    # GKL fix: after a REAL slot wait (>= 1 s: slots are shared across gates, the holder may be writing MAIN's out\) the key is read again
    cnt.write_text('', encoding='utf-8')
    root2 = make_repo(base, 'gklkey2')
    fst.write_lane(root2, 'a_hold', [fst.chk('k.hold', 'khold', 3.0, gpu=True)], affects=['src/a/**'])
    fst.write_lane(root2, 'b_wait', [fst.chk('k.waits', 'kwait', 0.2, gpu=True)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root2, '--jobs', '2', '--gpu-slots', '1'), cwd=root2, capture_output=True, text=True,
                       env=gate_env(GATE_TEST_KEY_COUNT='k.waits', GATE_TEST_KEY_COUNT_FILE=str(cnt), GATE_TEST_KEY_SLOW_S='0.5', GATE_SLOTS_DIR=str(Path(root2) / 'slots')))
    n2 = len(cnt.read_text(encoding='utf-8').split())
    ok(p.returncode == 0 and n2 == 2, f'GKL6 PLANT: a key taken before a real slot wait is read again after it (computed {n2} time(s), want 2): {(p.stdout + p.stderr)[-300:]!r}')
    # a write between the compute and the retake moves the epoch: the key is read again then (never a stale PASS key)
    d = Path(base) / 'gklkey_dir'
    shutil.rmtree(d, ignore_errors=True)
    (d / 'out' / 'cases' / 'c1').mkdir(parents=True)
    (d / 'out' / 'cases' / 'c1' / 'a.bin').write_bytes(b'x' * 100)
    s0 = pk.content_state(d / 'out' / 'cases' / 'c1', listing=True)
    (d / 'out' / 'cases' / 'c1' / 'a.bin').write_bytes(b'y' * 100)
    os.utime(d / 'out' / 'cases' / 'c1' / 'a.bin', ns=(1, 1))
    pk.invalidate([d])
    ok(pk.content_state(d / 'out' / 'cases' / 'c1', listing=True) != s0, 'GKL3 an out\\ folder is keyed by a (path, size, mtime) listing: a rewrite with another mtime changes it, without reading the files')
    pk.invalidate([d])
    seen = {'n': 0}
    orig = pk.content_hash
    pk.content_hash = lambda f: seen.__setitem__('n', seen['n'] + 1) or orig(f)
    try:
        pk.content_state(d / 'out' / 'cases' / 'c1', listing=True)
    finally:
        pk.content_hash = orig
    # GKL fix: invalidate() of the folder ITSELF and of a SUBFOLDER drops its listing memo too (the memo key carries '|L')
    sub = d / 'out' / 'cases' / 'c1' / 'sub'
    sub.mkdir(exist_ok=True)
    for tgt, lab in ((d / 'out' / 'cases' / 'c1', 'the folder itself'), (sub, 'a subfolder')):
        pk.invalidate([d])
        a0 = pk.content_state(d / 'out' / 'cases' / 'c1', listing=True)
        (sub / ('w_' + lab[:6].strip().replace(' ', '') + '.bin')).write_bytes(os.urandom(8))
        ok(pk.content_state(d / 'out' / 'cases' / 'c1', listing=True) == a0, 'GKL5 setup: a listing is memoised')
        pk.invalidate([tgt])
        ok(pk.content_state(d / 'out' / 'cases' / 'c1', listing=True) != a0, f'GKL5 PLANT: invalidate() of {lab} drops the listing memo (a write under the folder shows)')
    ok(seen['n'] == 0, f'GKL4 PLANT: hashing an out\\ folder reads none of its files (it read {seen["n"]})')


def test_gst_gpu(base):
    root = make_repo(base, 'gstgpu')
    # the doubled GPU timeout is +50 %
    fst.write_lane(root, 'a_fast', [dict(fst.chk('k.gpu', 'kg', 8), timeout_s=2, gpu=True)], affects=['src/a/**'])
    p = subprocess.run(gate_cmd(root, '--jobs', '2', '--no-cache'), cwd=root, capture_output=True, text=True, env=gate_env())
    ok(re.search(r'timeout after 3 s', p.stdout + p.stderr) is not None, 'GST23 a GPU check\'s timeout under --jobs > 1 is +50 % (2 s -> 3 s), not doubled')
    # the twin has its own timeout and the rig child gets a deadline
    src = (HERE.parent / 'scene' / 'host_rig_serial_twin.py').read_text(encoding='utf-8')
    ok(src.count('= run_rig(cmd)') == 2 and 'timeout=tmo' in src and 'subprocess.run(cmd, capture_output=True, text=True, errors=\'replace\')' not in src, 'GST24 both twin runs of host_rig.py have a timeout')
    src = (HERE.parent / 'scene' / 'host_rig.py').read_text(encoding='utf-8')
    ok('WWE_RIG_DEADLINE_S' in src, 'GST25 host_rig.py gives its child rig a deadline')


# ---------------------------------------------------------------- GST2 (the Opus review of GST: "review N" in each text is its finding number)
WORKER = r'''
import sys
sys.path.insert(0, sys.argv[1])
import fastgate as fg
mode, path, tag, n = sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
for i in range(n):
    if mode == 'times':
        fg.append_times(path, [['2026-01-01 00:00:00', 'r', 'full', 'lane', tag + str(i), 'PASS', 1.0, 'miss', tag]])
    else:
        fg.save_cache(path, {tag + str(i): {'verdict': 'PASS', 'check': tag}})
'''


def read_csv(p):
    import csv
    with open(p, newline='', encoding='utf-8') as f:
        return list(csv.reader(f))


def test_gst2(base):
    import tiers as gt
    # review 1: a build .ps1's exe brings its link outputs with it; the output attribution counts <stem>.* of a written exe as the build's
    root = base / 'g2fp'
    (root / 'tools' / 'x').mkdir(parents=True)
    (root / 'tools' / 'x' / 'b.ps1').write_text("$repo = 'x'\n$outDir = Join-Path $repo 'out\\tools'\n& clang++ a.cpp -o (Join-Path $outDir 'zz_rig.exe')\n")
    f = gs.footprint({'name': 'n', 'cmd': ['pwsh', '-NoProfile', '-File', 'tools/x/b.ps1']}, root, root)
    ok({'out/tools/zz_rig.exe', 'out/tools/zz_rig.pdb', 'out/tools/zz_rig.ilk', 'out/tools/zz_rig.exp', 'out/tools/zz_rig.lib', 'out/tools/zz_rig.exe.deps.txt'} <= f['writes'],
       f'GG1 a build naming zz_rig.exe writes its .pdb / .ilk / .exp / .lib / .exe.deps.txt too: {sorted(f["writes"])}')
    w = {'out/tools/zz_rig.exe'}
    ok(gs.owns('out/tools/zz_rig.pdb', w) and gs.owns('out/tools/zz_rig.exe.deps.txt', w) and not gs.owns('out/tools/zz_rig_other.exe', w) and not gs.owns('out/tools/other.pdb', w),
       'GG2 PLANTED: <stem>.* of a written exe is the build\'s, a name that only STARTS with the stem (zz_rig_other.exe) or another stem is not')
    # review 4: only a missing cache file is empty; review 6+: a failed read never wipes the file; review 12: a replace rides out a reader
    c = base / 'g2lc'
    c.mkdir()
    ok(fg.load_cache(c / 'none.json') == {}, 'GG3 a missing cache.json is {}')
    (c / 'bad.json').write_text('{not json')
    try:
        fg.load_cache(c / 'bad.json', tries=2, delay=0.01)
        raised = False
    except Exception:
        raised = True
    ok(raised, 'GG4 PLANTED: an unreadable (torn) cache.json RAISES; it used to read as {} and the next save wiped everyone\'s entries')
    try:
        fg.save_cache(c / 'bad.json', {'k': {'verdict': 'PASS'}})
        raised = False
    except Exception:
        raised = True
    ok(raised and (c / 'bad.json').read_text() == '{not json', 'GG5 a save onto an unreadable file raises and leaves it untouched')
    tgt, tmp = c / 't.json', c / 't.tmp'
    tgt.write_text('old')
    tmp.write_text('new')
    rd = open(tgt)
    threading.Timer(0.4, rd.close).start()
    fg._replace(tmp, tgt)
    ok(tgt.read_text() == 'new', 'GG6 os.replace is retried while a reader holds the file open (PermissionError on Windows)')
    # reviews 5 + 7: cross-process load-merge-replace and appends under one OS lock: nothing lost, no torn row
    wk = base / 'g2worker.py'
    wk.write_text(WORKER)
    for mode, fn, nproc, nrow in (('times', 'times.csv', 4, 40), ('cache', 'cache.json', 4, 40)):
        procs = [subprocess.Popen([PY, '-I', str(wk), str(HERE), mode, str(c / fn), f'p{i}_', str(nrow)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) for i in range(nproc)]
        errs = [p.communicate(timeout=120)[0] for p in procs]
        if mode == 'times':
            rows = read_csv(c / fn)
            ok(rows[0] == fg.TIMES_HEAD and len(rows) == 1 + nproc * nrow and all(len(r) == 9 for r in rows), f'GG7 {nproc} gates appending to times.csv at once: {len(rows) - 1} of {nproc * nrow} rows, every row whole, one header {"".join(errs)[-200:]}')
        else:
            got = fg.load_cache(c / fn)
            ok(len(got) == nproc * nrow, f'GG8 PLANTED: {nproc} gates saving cache.json at once lose nothing ({len(got)} of {nproc * nrow} entries; without the lock the last writer wins) {"".join(errs)[-200:]}')
    # review 11: the run column; an old 8-column file is migrated once
    old = c / 'old_times.csv'
    old.write_text('when,root,mode,lane,check,verdict,seconds,cache\n2026-01-01 00:00:00,r,full,l,old.one,PASS,1.0,miss\n')
    fg.append_times(old, [['2026-01-02 00:00:00', 'r', 'full', 'l', 'new.one', 'PASS', 2.0, 'miss', 'RUN-X']])
    rows = read_csv(old)
    ok(rows[0][-1] == 'run' and rows[1][-1] == '' and rows[2][-1] == 'RUN-X' and all(len(r) == 9 for r in rows), f'GG9 an old 8-column times.csv is migrated: old rows get an empty run, the new row its RUN_ID ({rows})')


def test_gst2_twin(base):
    # review 9: the twin's rig run kills the whole tree on a timeout and hands the child a deadline
    sys.path.insert(0, str(HERE.parent / 'scene'))
    import host_rig_serial_twin as twin
    pidf = base / 'g2child.pid'
    kid = base / 'g2kid.py'
    kid.write_text("import subprocess, sys, time\np = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])\nopen(sys.argv[1], 'w').write(str(p.pid))\ntime.sleep(120)\n")
    os.environ['WWE_TWIN_TIMEOUT_S'] = '2'
    t0 = time.time()
    r = twin.run_rig([PY, '-I', str(kid), str(pidf)])
    dt = time.time() - t0
    del os.environ['WWE_TWIN_TIMEOUT_S']
    time.sleep(0.5)
    gone = True
    if pidf.exists():
        tl = subprocess.run(['tasklist', '/FI', 'PID eq ' + pidf.read_text().strip()], capture_output=True, text=True).stdout
        gone = pidf.read_text().strip() not in tl
    ok(r.returncode == 124 and dt < 25 and gone, f'GG10 PLANTED: the twin\'s timeout returns 124 in {dt:.1f} s and the GRANDCHILD is dead too (it was left running holding GPU slots)')
    os.environ['WWE_TWIN_TIMEOUT_S'] = '100'
    r = twin.run_rig([PY, '-I', '-c', "import os; print(os.environ.get('WWE_RIG_DEADLINE_S'))"])
    del os.environ['WWE_TWIN_TIMEOUT_S']
    ok(r.returncode == 0 and r.stdout.strip() == '70', f'GG11 the rig child gets WWE_RIG_DEADLINE_S = timeout - 30 ({r.stdout.strip()})')


def test_gst2_tiers_unit(base):
    import tiers as gt
    # tiers
    ok(gt.derive({'name': 'l.build'}) == 'cheap' and gt.derive({'name': 'l.x_lint'}) == 'cheap' and gt.derive({'name': 'l.selftest'}) == 'cheap' and gt.derive({'name': 'l.x_unit', 'needs': ['out/captures/a']}) == 'cheap',
       'GG12 builds / lints / selftests / units derive cheap (a unit that reads a capture too)')
    ok(gt.derive({'name': 'l.anything'}, gpu=True) == 'rig' and gt.derive({'name': 'l.a_unit'}, gpu=True) == 'rig' and gt.derive({'name': 'l.zz', 'needs': ['out/captures/a']}) == 'rig' and gt.derive({'name': 'l.rig_arms'}) == 'rig' and gt.derive({'name': 'l.q'}) == 'cheap',
       'GG13 a GPU check / a capture proof / a rig-named check derive rig; an unclassified check is cheap')
    ok(gt.derive({'name': 'l.rig_arms', 'tier': 'cheap'}) == 'cheap' and gt.derive({'name': 'l.build', 'tier': 'rig'}) == 'rig', 'GG14 a declared tier wins both ways')
    cs = [{'name': 'a.build', 'lane': 'a'}, {'name': 'a.rig_arms', 'lane': 'a'}, {'name': 'a.picture_proof', 'lane': 'a'}]
    kept, skipped = gt.split(cs, 'cheap', {'a'}, lambda c: False)
    ok([c['name'] for c in kept] == ['a.build'] and len(skipped) == 2, 'GG15 --only LANE under --tier cheap keeps the lane\'s cheap checks and skips its rig ones')
    kept, skipped = gt.split(cs, 'cheap', {'a', 'a.picture_proof'}, lambda c: False)
    ok([c['name'] for c in kept] == ['a.build', 'a.picture_proof'] and [c['name'] for c in skipped] == ['a.rig_arms'], 'GG16 a check NAMED in --only (lane.check) always runs, whatever its tier')
    ok(len(gt.split(cs, 'all', set(), lambda c: False)[0]) == 3, 'GG17 --tier all keeps everything')


def test_gst2_tiers_e2e(base):
    # end to end: tiers
    root = make_repo(base, 'g2tier')
    fst.write_lane(root, 'a', [fst.chk('a.build', 'tb', 0.1), fst.chk('a.rig_arms', 'tr', 0.1), dict(fst.chk('a.own_proof', 'to', 0.1), tier='rig')], affects=['src/a/**'])
    rc, out = fst.gate(root, '--only', 'a', '--tier', 'cheap')
    ok(rc == 0 and fst.count(root, 'tb') == 1 and fst.count(root, 'tr') == 0 and fst.count(root, 'to') == 0 and 'rig skipped: 2' in out, f'GG18 gate --tier cheap --only a: builds run, 2 rig checks skipped and counted ("rig skipped: 2")')
    rc, out = fst.gate(root, '--only', 'a,a.own_proof', '--tier', 'cheap')
    ok(rc == 0 and fst.count(root, 'to') == 1 and fst.count(root, 'tr') == 0 and 'rig skipped: 1' in out, 'GG19 --only a,a.own_proof: the named rig check runs, the other still skipped')
    rc, out = fst.gate(root)
    ok(rc == 0 and fst.count(root, 'tr') == 1 and 'rig skipped' not in out, 'GG20 the default (--tier all) runs everything')
    gw = HERE / 'gate_wait.py'
    envw = gate_env()
    shutil.rmtree(Path(root) / 'out_gate', ignore_errors=True)
    p = subprocess.run([PY, '-I', str(gw), '--root', str(root), '--out', str(Path(root) / 'out_gate'), '--log', str(base / 'g2w.log'), '--only', 'a', '--no-toolcache', '--gpu-slots', '0', '--jobs', '2'], capture_output=True, text=True, env=envw)
    ok(p.returncode == 0 and 'rig skipped: 2' in p.stdout, f'GG21 gate_wait --only passes --tier cheap by default ({p.returncode}: {p.stdout[-200:]})')


def test_gst2_notrun(base):
    gw = HERE / 'gate_wait.py'
    # end to end: NOT RUN exit codes through gate_wait (2), the exit step
    root = make_repo(base, 'g2nr')
    fst.write_lane(root, 'a', [fst.chk('a.slow', 'ts', 30)])
    p = subprocess.run([PY, '-I', str(gw), '--root', str(root), '--out', str(Path(root) / 'out_gate'), '--log', str(base / 'g2n.log'), '--only', 'a', '--no-toolcache', '--gpu-slots', '0', '--deadline', '4'], capture_output=True, text=True, env=gate_env(GATE_MIN_LEFT_S='0.2'))
    ok(p.returncode == 2 and 'NOT RUN' in p.stdout and 'timed out' in p.stdout, f'GG22 a gate whose only non-pass row is a deadline-killed check: gate_wait exits 2 (timed out), not 1 ({p.returncode}: {p.stdout[-300:]})')


def test_gst2_left(base):
    # review 3: a check is not started with < MIN left or < its last seconds in times.csv left
    root = make_repo(base, 'g2left')
    fst.write_lane(root, 'a', [fst.chk('a.after', 'la', 0.1)])
    # no clock: MIN left is 1000 s, the deadline 600 s, so there is always less left than the minimum and the check must not be started
    p = subprocess.run(gate_cmd(root, '--jobs', '1', '--no-cache', '--deadline', '600'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_MIN_LEFT_S='1000'))
    out = p.stdout + p.stderr
    ok(p.returncode == 5 and re.search(r'^NOT RUN\s+\S+\s+a\.after .*not started: \d+ s left', out, re.M) and fst.count(root, 'la') == 0, f'GG23 a check is NOT started with under 10 s left before the deadline (a.after, ran {fst.count(root, "la")}x)')
    fst.write_lane(root, 'a', [fst.chk('a.four', 'l4', 0.1)])
    p = subprocess.run(gate_cmd(root, '--jobs', '1', '--no-cache'), cwd=root, capture_output=True, text=True, env=gate_env())
    # the check's newest PASS row says it took 1000 s, more than the 600 s deadline (the row is added, no real wait)
    fg.append_times(Path(root) / 'out_gate' / 'times.csv', [[time.strftime('%Y-%m-%d %H:%M:%S'), str(root), 'full', 'a', 'a.four', 'PASS', 1000.0, 'miss', 'FAKE']])
    p = subprocess.run(gate_cmd(root, '--jobs', '1', '--no-cache', '--deadline', '600'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_MIN_LEFT_S='0.1'))
    out = p.stdout + p.stderr
    ok(re.search(r'^NOT RUN\s+\S+\s+a\.four .*needs ~1000 s', out, re.M) and fst.count(root, 'l4') == 1, f'GG24 nor with less than its LAST run took (times.csv: 1000 s, deadline 600 s) left ({fst.count(root, "l4")} runs)')


def test_gst2_slot(base):
    # reviews 2 + 8 + 3: a slot is not held idle behind a path wait; the deadline kills a running check as NOT RUN; end to end on ONE slot
    root = make_repo(base, 'g2slot')
    fst.write_lane(root, 'b', [fst.chk('s.bpre', 'sbp', 0.3), fst.chk('s.b', 'sb', 0.3, gpu=True, reads=['out/p'])])
    fst.write_lane(root, 'g1', [fst.chk('s.g1', 'sg1', 6.0, gpu=True)])
    fst.write_lane(root, 'w', [fst.chk('s.wpre', 'swp', 0.8), fst.chk('s.w', 'sw', 30, writes=['out/p', 'counts_sw.txt', 'span_sw.txt'])])
    fst.write_lane(root, 'x', [fst.chk('s.xpre', 'sxp', 0.5), fst.chk('s.x', 'sx', 0.3, gpu=True)])
    p = subprocess.run(gate_cmd(root, '--jobs', '4', '--gpu-slots', '1', '--no-cache', '--deadline', '25'), cwd=root, capture_output=True, text=True,
                       env=gate_env(GATE_MIN_LEFT_S='1', GATE_SLOTS_DIR=str(Path(root) / 'slots')))
    out = p.stdout + p.stderr
    ok(re.search(r'^PASS\s+\S+\s+s\.x ', out, re.M) and re.search(r'^NOT RUN\s+\S+\s+s\.b ', out, re.M) and re.search(r'^NOT RUN\s+\S+\s+s\.w .*killed at the --deadline', out, re.M) and p.returncode == 5,
       f'GG25 PLANTED: s.b took the one GPU slot, found its paths held by s.w and let the slot go: s.x got it (PASS) while s.b ended NOT RUN, s.w killed at the deadline is NOT RUN ({p.returncode}): '
       + ' | '.join(ln[:70] for ln in out.splitlines() if re.match(r'^(PASS|FAIL|NOT RUN)', ln)))


def test_gst2_cachefile(base):
    # reviews 4 + 5: a cache.json that cannot be read or written never kills a finished gate before its summary
    root = make_repo(base, 'g2cache')
    fst.write_lane(root, 'a', [fst.chk('a.one', 'c1', 0.1)], affects=['src/a/**'])
    (Path(root) / 'out_gate' / 'cache.json').mkdir(parents=True)
    p = subprocess.run(gate_cmd(root, '--jobs', '1'), cwd=root, capture_output=True, text=True, env=gate_env())
    out = p.stdout + p.stderr
    ok(p.returncode == 0 and re.search(r'^gate: 1 of 1 pass or skip, 0 fail', out, re.M) and 'could not' in out, f'GG26 PLANTED: an unreadable / unwritable cache.json is said and the gate still ends with its summary ({p.returncode}: {out[-250:]})')


def test_gst2_dirty(base):
    # review 6: the end save writes only this gate's own entries (an entry another gate dropped meanwhile stays dropped)
    root = make_repo(base, 'g2dirty')
    slow = {'name': 'a.slow', 'timeout_s': 60, 'writes': ['started_ds.txt'], 'cmd': [PY, '-I', '-c', "import time; open('started_ds.txt', 'w').write('1'); time.sleep(2.0)"]}
    fst.write_lane(root, 'a', [slow], affects=['src/a/**'])
    cp = Path(root) / 'out_gate' / 'cache.json'
    cp.parent.mkdir(parents=True)
    cp.write_text(json.dumps({'OLDKEY': {'verdict': 'PASS', 'check': 'old'}}))
    pr = subprocess.Popen(gate_cmd(root, '--jobs', '1'), cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=gate_env())
    t0 = time.time()
    while time.time() - t0 < 180 and not (Path(root) / 'started_ds.txt').exists():
        time.sleep(0.1)
    cp.write_text(json.dumps({}))   # another gate dropped OLDKEY meanwhile
    pr.communicate(timeout=60)
    got = fg.load_cache(cp)
    ok('OLDKEY' not in got and any(v.get('check') == 'a.slow' for v in got.values()), f'GG27 PLANTED: the end save writes only what this gate added: an entry another gate dropped is not resurrected ({list(got)[:3]})')


def test_gst2_mismatch(base):
    # review 6: a CACHE MISMATCH entry leaves the file at once (a killed gate keeps nothing wrong)
    root = make_repo(base, 'g2mis')
    (root / 'flip.py').write_text('import os, sys\nsys.exit(1 if os.path.exists("fl" + "ip.txt") else 0)\n')
    fst.write_lane(root, 'flip_l', [{'name': 'flip_l.one', 'cmd': [PY, '-I', 'flip.py'], 'timeout_s': 60, 'affects': ['src/a/**'], 'writes': []}])
    rc, out = fst.gate(root, '--only', 'flip_l')
    (root / 'flip.txt').write_text('1')
    fst.write_lane(root, 'slow_l', [fst.chk('slow_l.one', 'sl', 40)], affects=['src/b/**'])
    pr = subprocess.Popen(gate_cmd(root, '--jobs', '2', '--verify-cache'), cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=gate_env())
    t0 = time.time()
    while time.time() - t0 < 180 and not any('flip_l.one' in r and 'FAIL' in r for r in times_rows(root)):
        time.sleep(0.2)
    time.sleep(0.3)
    subprocess.run(['taskkill', '/F', '/T', '/PID', str(pr.pid)], capture_output=True)
    pr.wait(20)
    got = fg.load_cache(Path(root) / 'out_gate' / 'cache.json')
    ok(rc == 0 and not any(v.get('check') == 'flip_l.one' for v in got.values()), f'GG28 PLANTED: the wrong entry is gone from cache.json although the gate was killed before its end ({len(got)} entries)')


def test_gst2_stall0(base):
    # review 14: GATE_STALL_S=0 is off (it used to report every 0.2 s); review 11: every row carries this gate's RUN_ID
    root = make_repo(base, 'g2stall')
    fst.write_lane(root, 'a', [fst.chk('a.one', 'z1', 0.8)])
    p = subprocess.run(gate_cmd(root, '--jobs', '1', '--no-cache'), cwd=root, capture_output=True, text=True, env=gate_env(GATE_STALL_S='0'))
    ok(p.returncode == 0 and 'no check finished' not in p.stdout + p.stderr, 'GG29 PLANTED: GATE_STALL_S=0 switches the stall watchdog off')
    p = subprocess.run(gate_cmd(root, '--jobs', '1', '--no-cache'), cwd=root, capture_output=True, text=True, env=gate_env())
    rows = read_csv(Path(root) / 'out_gate' / 'times.csv')
    ids = [r[8] for r in rows[1:]]
    ok(rows[0][-1] == 'run' and ids and all(ids) and len(set(ids)) == 2 and len(rows[1:]) == len(ids), f'GG30 every times.csv row has a run id, one per gate ({set(ids)})')
    src = (HERE / 'gate.py').read_text(encoding='utf-8')
    ok('log=glog' in src and src.count('int(deadline_at - time.time()) + 1') >= 2, 'GG31 path-wait lines go through the print lock; the shared game_syntax run honours the deadline too')


def test_jobs():
    ok(gs.default_jobs(32) == 8 and gs.default_jobs(16) == 4 and gs.default_jobs(4) == 2 and gs.default_jobs(128) == 8, 'J1 default jobs: 32 threads (5950X) -> 8, 16 -> 4, 4 -> 2, capped at 8')
    os.environ['GATE_JOBS'] = '3'
    ok(gs.default_jobs(32) == 3, 'J2 GATE_JOBS overrides')
    del os.environ['GATE_JOBS']


def keys_and_review(base):
    # one thread: both clear / read the process-global proofkey memos (pk._CLOSURE, _DIR_STATE, _MEMO) and fastgate.KEY_VERSION
    test_keys(base)
    test_review(base)


def test_unit_misc(base):
    test_footprints(base)
    test_locks()


def run_group(fn, base, secs):
    sub = base / fn.__name__
    sub.mkdir()
    _TL.last = time.time()
    t0 = time.time()
    try:
        fn(sub)
    except BaseException:
        import traceback
        FAILS.append(f'{fn.__name__} raised')
        print('FAIL  group ' + fn.__name__ + ' raised:\n' + traceback.format_exc(), flush=True)
    secs[fn.__name__] = time.time() - t0


# every group has its own scratch folder (own repo, own GATE_SLOTS_DIR where it uses slots), so the groups run side by side on a pool; the longest first.
# test_jobs runs BEFORE the pool: it sets GATE_JOBS in os.environ, which every gate subprocess started meanwhile would inherit.
GROUPS = [test_cache, test_gkl_key_once, test_gst_kill, test_gst2_dirty, test_gst2_slot, test_gst2_mismatch, test_gst_deadline, test_cache_flip, test_gst2_notrun, test_gst_stall,
          test_gst2_stall0, keys_and_review, test_gst2_tiers_e2e, test_gst2_left, test_parallel, test_twins, test_stale_hit, test_gst2_cachefile,
          test_cache_verify, test_cache_uncacheable, test_parallel_noaffects, test_parallel_blind, test_parallel_env, test_gate_serialise, test_gst_gpu,
          test_slots, test_gst, test_gst2, test_gst2_twin, test_gst2_tiers_unit, test_unit_misc]


def run():
    base = Path(tempfile.mkdtemp(prefix='sched_selftest_'))
    secs = {}
    t_all = time.time()
    try:
        test_jobs()
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=int(os.environ.get('SCHED_SELFTEST_WORKERS') or 16)) as ex:
            for f in [ex.submit(run_group, fn, base, secs) for fn in GROUPS]:
                f.result()
        print('groups (s): ' + ', '.join(f'{n} {t:.1f}' for n, t in sorted(secs.items(), key=lambda kv: -kv[1])[:10]) + f'; wall {time.time() - t_all:.1f}')
    finally:
        shutil.rmtree(base, ignore_errors=True)
    top = sorted(TIMES, reverse=True)[:12]
    print('slowest cases (s): ' + ', '.join(f'{n} {t:.1f}' for t, n in top) + f'; total {sum(t for t, _ in TIMES):.1f}')
    print(f'sched selftest: {"FAIL, " + str(len(FAILS)) + " check(s)" if FAILS else "all checks hold"}')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(run())

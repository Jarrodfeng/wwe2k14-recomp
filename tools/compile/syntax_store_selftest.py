# Selftest for game_syntax's per-key result store (S105 CC). py -I tools\compile\syntax_store_selftest.py
# Scratch folders + real clang -fsyntax-only on tiny units (a few seconds), a scratch store (GAME_SYNTAX_CACHE_DIR); no game, no build tree.
#   1 a second identical run is a hit with the same verdict
#   2 a header edit that changes the preprocessed output misses
#   3 a comment-only header edit MISSES by design (clang -E -P leaves blank lines / trailing blanks where comments were; those are not normalised away because a raw string literal could depend on them): safe, costs one re-parse
#   4 a failing unit is stored as a failure: the replay is a failure with its diagnostics, never a pass; fixing the header misses (then passes); breaking it again replays the failure
#   5 two "worktrees" with different header content never share a hit; with identical content they do (positive control)
#   6 --no-cache (GAME_SYNTAX_NO_CACHE) neither reads nor writes
#   7 prune() removes the oldest entries first (LRU: a touched entry survives) and only over the cap
import os, shutil, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, os.environ.get('GS_DIR', str(HERE)))
TMP = Path(tempfile.mkdtemp(prefix='gs_store_selftest_'))
os.environ['GAME_SYNTAX_CACHE_DIR'] = str(TMP / 'store')
os.environ.pop('GAME_SYNTAX_NO_CACHE', None)
import game_syntax as gs
import syntax_store as st

FAILS = []


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def tree(name, header):
    d = TMP / name
    (d / 'src').mkdir(parents=True)
    (d / 'src' / 'h.h').write_text(header)
    (d / 'src' / 'u.cpp').write_text('#include "h.h"\nint use() { return f(); }\n')
    return d


def check(d):
    wt = str(d).replace('\\', '/')
    return gs.syntax_check(['-std=c++17', '-x', 'c++'], wt + '/src/u.cpp', roots=((wt, '<ROOT>'),))


def main():
    if not os.path.isfile(gs.CLANG):
        print('SKIP: clang not found')
        return 0
    try:
        A = tree('wtA', 'inline int f() { return 1; }\n')
        r1 = check(A)
        ok(r1[0] == 0 and r1[2] and not r1[3], '1 first run parses and passes (miss)')
        r2 = check(A)
        ok(r2[3] and r2[0] == 0 and r2[2] == r1[2], '1 second identical run is a hit with the same verdict and key')
        (A / 'src' / 'h.h').write_text('inline int f() { return 2; }\n')
        r3 = check(A)
        ok(not r3[3] and r3[0] == 0 and r3[2] != r1[2], '2 a header edit that changes the preprocessed output misses')
        (A / 'src' / 'h.h').write_text('// a new comment\ninline int f() { return 2; }   // another\n')
        r4 = check(A)
        ok(not r4[3] and r4[0] == 0 and r4[2] != r3[2], '3 a comment-only header edit misses (strict: preprocessed text differs in blank lines) and still passes')
        # 4 failures
        (A / 'src' / 'h.h').write_text('inline int f() { return missing_name; }\n')
        b1 = check(A)
        ok(b1[0] != 0 and not b1[3] and 'missing_name' in b1[1], '4 a broken header fails and the diagnostics name it')
        b2 = check(A)
        ok(b2[3] and b2[0] != 0 and 'missing_name' in b2[1], '4 the failure is REPLAYED as a failure with its diagnostics (hit, rc != 0, never a pass)')
        (A / 'src' / 'h.h').write_text('inline int f() { return 7; }\n')
        f1 = check(A)
        ok(not f1[3] and f1[0] == 0, '4 fixing the header misses (the stored failure is not served for fixed text) and passes')
        (A / 'src' / 'h.h').write_text('inline int f() { return missing_name; }\n')
        b3 = check(A)
        ok(b3[3] and b3[0] != 0, '4 breaking it the same way again replays the failure, not a pass')
        # 5 worktrees
        (A / 'src' / 'h.h').write_text('inline int f() { return 1; }\n')
        B = tree('wtB', 'inline int f() { return 99; }\n')
        ra, rb = check(A), check(B)
        ok(not rb[3] and ra[2] != rb[2], '5 a second worktree with different header content does not share the hit')
        C = tree('wtC', 'inline int f() { return 1; }\n')
        rc_ = check(C)
        ok(rc_[3] and rc_[2] == ra[2], '5 positive control: a third worktree with identical content shares the hit (paths are placeholders in the key)')
        # 6 no cache
        os.environ['GAME_SYNTAX_NO_CACHE'] = '1'
        n0 = sum(1 for _ in Path(os.environ['GAME_SYNTAX_CACHE_DIR']).rglob('*.json'))
        D = tree('wtD', 'inline int f() { return 5; }\n')
        rn = check(D)
        n1 = sum(1 for _ in Path(os.environ['GAME_SYNTAX_CACHE_DIR']).rglob('*.json'))
        rn2 = check(A)
        os.environ.pop('GAME_SYNTAX_NO_CACHE')
        ok(rn[0] == 0 and rn[2] is None and n0 == n1 and not rn2[3], '6 --no-cache: no key, nothing written, a stored unit is not read')
        # 7 prune
        for k in ('aa1', 'bb2', 'cc3'):
            st.put(k, 0, 'x' * 4000)
            time.sleep(0.05)
        os.utime(st._path('aa1'), None)   # touch the oldest: it must survive
        removed = st.prune(cap_mb=0)   # cap 0 would clear all; use a real cap next
        ok(removed > 0, '7 prune over the cap removes entries')
        for k in ('aa1', 'bb2', 'cc3'):
            st.put(k, 0, 'x' * 4000)
            os.utime(st._path(k), (time.time() - {'aa1': 10, 'bb2': 30, 'cc3': 20}[k],) * 2)
        total = sum(Path(p).stat().st_size for p in Path(os.environ['GAME_SYNTAX_CACHE_DIR']).rglob('*.json'))
        st.prune(cap_mb=(total - 3000) / 1048576.0)
        left = {k for k in ('aa1', 'bb2', 'cc3') if os.path.exists(st._path(k))}
        ok('bb2' not in left and 'aa1' in left, '7 the least recently used entry goes first, a recent one stays (left: %s)' % sorted(left))
        n = st.prune(cap_mb=1000)
        ok(n == 0, '7 under the cap nothing is removed')
        # ---- S105 CC fix round ----
        import threading
        sdir = Path(os.environ['GAME_SYNTAX_CACHE_DIR'])

        def nfiles():
            return sum(1 for _ in sdir.rglob('*.json'))

        # 8 an entry reached disk; get() touches mtime (LRU)
        st.prune(cap_mb=0)
        E1 = tree('wtE', 'inline int f() { return 11; }\n')
        r = check(E1)
        ok(r[0] == 0 and nfiles() == 1 and os.path.exists(st._path(r[2])), '8 an entry reached disk under the store folder after a miss')
        os.utime(st._path(r[2]), (time.time() - 5000,) * 2)
        before = os.stat(st._path(r[2])).st_mtime
        check(E1)
        ok(os.stat(st._path(r[2])).st_mtime > before + 1000, '8 a hit touches the entry (get() refreshes mtime: LRU)')
        # 9 a timeout / a crash / an exit-1 with no diagnostic is NOT stored; a real diagnostic IS (F2)
        real_run = gs._proc.run

        class Fake:
            def __init__(s, rc, out='', err='', to=False):
                s.returncode, s.stdout, s.stderr, s.timed_out = rc, out, err, to

        def fake(kind):
            def run(cmd, *a, **k):
                if '-fsyntax-only' in cmd:
                    return kind
                return real_run(cmd, *a, **k)
            return run

        for name, kind in (('a timeout', Fake(-1, '', '', True)), ('a crash (Stack dump)', Fake(1, '', 'x.cpp:1:1: error: boom\nStack dump:\n0. Program arguments')),
                           ('an exit-1 with no diagnostic', Fake(1, '', 'clang: killed')), ('a bad_alloc exit', Fake(3, '', 'bad_alloc')),
                           ('an OOM', Fake(1, '', 'x.cpp:1:1: error: out of memory allocating'))):
            st.prune(cap_mb=0)
            Fz = tree('wtF', 'inline int f() { return 12; }\n')
            gs._proc.run = fake(kind)
            try:
                r = check(Fz)
            finally:
                gs._proc.run = real_run
            ok(nfiles() == 0, '9 %s is not stored' % name)
            shutil.rmtree(Fz)
        st.prune(cap_mb=0)
        Fz = tree('wtF', 'inline int f() { return 12; }\n')
        gs._proc.run = fake(Fake(1, '', 'u.cpp:3:5: error: use of undeclared identifier x'))
        try:
            check(Fz)
        finally:
            gs._proc.run = real_run
        ok(nfiles() == 1, '9 an exit-1 with a real file:L:C: error: diagnostic is stored')
        shutil.rmtree(Fz)
        # 10 the second-preprocess race guard (moved here from keep_stream_syntax_twin): a header FIXED between the first preprocess and the parse is not stored under the broken text's key
        st.prune(cap_mb=0)
        G = tree('wtG', 'inline int f() { return missing_g; }\n')
        gh = G / 'src' / 'h.h'
        fired, n_before = [], len(gs.NOT_STORED)

        def racing(cmd, *a, **k):
            r_ = real_run(cmd, *a, **k)
            if '-E' in cmd and not fired:
                fired.append(1)
                gh.write_text('inline int f() { return 1; }\n')   # fixed right after the first preprocess read the broken text
            return r_
        gs._proc.run = racing
        try:
            rr = check(G)
        finally:
            gs._proc.run = real_run
        ok(rr[0] == 0 and rr[2] is not None and st.get(rr[2]) is None and len(gs.NOT_STORED) == n_before + 1, '10 race guard: the parse saw the fixed text, nothing is stored under the broken text\'s key, NOT_STORED +1')
        gh.write_text('inline int f() { return missing_g; }\n')
        rb = check(G)
        ok(rb[0] != 0 and not rb[3], '10 the broken text still FAILS afterwards (not served the raced pass)')
        # 11 clang's default-error diagnostics are not silenced (-w, not -Wno-everything): a narrowing conversion in braces fails
        N = tree('wtN', 'inline int f() { double d = 1.5; int x{d}; return x; }\n')
        rn = check(N)
        ok(rn[0] != 0 and 'narrow' in rn[1], '11 a narrowing conversion in braces (a default error in the real build) FAILS the syntax check')
        wtn = str(N).replace(chr(92), '/')
        rn2 = gs.syntax_check(['-std=c++17', '-x', 'c++', '-Wno-everything'], wtn + '/src/u.cpp', roots=((wtn, '<ROOT>'),))
        ok(rn2[0] != 0, '11 a caller passing -Wno-everything (keep_stream_off_link --syntax) does not hide the default error either')
        # 12 the old -Wno-everything stored passes must not be served: the key carries a salt
        ok(gs.KEY_SALT and 'v2' in gs.KEY_SALT, '12 the key carries a salt that changed with the parse flags')
        # 13 --no-cache flag exists and sets the switch
        old_argv, old_env = sys.argv, os.environ.get('GAME_SYNTAX_NO_CACHE')
        sys.argv = ['game_syntax.py', '--no-cache', '--root', str(TMP)]
        os.environ.pop('GAME_SYNTAX_NO_CACHE', None)
        try:
            gs.main()
            flag_set = os.environ.get('GAME_SYNTAX_NO_CACHE') == '1'
        except SystemExit:
            flag_set = False
        finally:
            sys.argv = old_argv
            os.environ.pop('GAME_SYNTAX_NO_CACHE', None)
            if old_env:
                os.environ['GAME_SYNTAX_NO_CACHE'] = old_env
        ok(flag_set, '13 --no-cache sets GAME_SYNTAX_NO_CACHE (every unit is parsed, nothing read or written)')
        # 14 a pool whose jobs are ALL store hits never asks the compile queue; a miss does (F-follow-up)
        st.prune(cap_mb=0)
        H = tree('wtH', 'inline int f() { return 21; }\n')
        check(H)
        wt = str(H).replace('\\', '/')
        job = ('u [on]', ['-std=c++17', '-x', 'c++'], wt + '/src/u.cpp')
        held = []

        class Hold:
            def __init__(s, *a, **k):
                held.append(1)

            def __enter__(s):
                return 1

            def __exit__(s, *a):
                return False
        real_hold = gs._admit.hold
        gs._admit.hold = Hold
        try:
            res = gs.run_jobs([job], wt, wt)
            ok(res[0][1] == 0 and res[0][4] and not held, '14 an all-hit pool does not take compile-queue admission (hold never called)')
            (H / 'src' / 'h.h').write_text('inline int f() { return 22; }\n')
            res = gs.run_jobs([job], wt, wt)
            ok(res[0][1] == 0 and not res[0][4] and held, '14 a pool with a miss does take admission and parses it')
        finally:
            gs._admit.hold = real_hold
        # 15 concurrent writers of one key (F3: the temp name carries the thread id) and the entry-count cap (F5)
        errs = []

        def w():
            try:
                for _ in range(20):
                    st.put('dd9', 0, 'x')
            except Exception as e:
                errs.append(e)
        ts = [threading.Thread(target=w) for _ in range(8)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        ok(not errs and st.get('dd9') == (0, 'x') and not list(sdir.rglob('*.tmp')), '15 eight threads writing one key leave a good entry and no temp file')
        st.prune(cap_mb=0)
        for i in range(6):
            st.put('ee%d' % i, 0, 'y')
            os.utime(st._path('ee%d' % i), (time.time() - 100 + i,) * 2)
        n = st.prune(cap_mb=1000, max_entries=3)
        left = sorted(k for k in ('ee%d' % i for i in range(6)) if os.path.exists(st._path(k)))
        ok(n == 3 and left == ['ee3', 'ee4', 'ee5'], '15 an entry-count cap removes the oldest entries (left %s)' % left)
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    print('FAIL: %d' % len(FAILS) if FAILS else 'PASS: syntax_store selftest')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

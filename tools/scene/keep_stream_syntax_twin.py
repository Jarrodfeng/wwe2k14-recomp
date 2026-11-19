#!/usr/bin/env python3
r"""Twin of keep_stream_off_link.py --syntax's verdict cache (S103 GSP). 0 game runs, one scratch unit (never a game unit).

  py -I tools\scene\keep_stream_syntax_twin.py [--root DIR]

A scratch unit out\gsp_twin\scratch.cpp includes out\gsp_twin\scratch.h; keep_stream_off_link.py --syntax --extra compiles it with the main.cpp flags:
  1. first run: compiles (rc 0);
  2. same text again: served by the preprocess hash (1 of 1 hit), rc 0;
  3. the HEADER edited (a new declaration): the unit is NOT served (0 of 1), rc 0: the edit reached the preprocessed text;
  4. the header made broken (#error): NOT served and the check FAILS (a stale pass can never hide it);
  5. the header restored to step 1's text: served again (1 of 1);
  6. (GSP2 review bug 6) a header that preprocesses but does not compile: FAILS, and is not served on a second run;
  7. the header FIXED between the first preprocess and the parse (planted): the PASS is not stored under the broken text's key, and the broken text still FAILS afterwards;
  8. the flags' tree paths are placeholders (a worktree inside the main checkout, either slash style).
Exit 0 = all hold. Needs the main checkout's configured release build (the same as keep_stream_off_link.py)."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    d = os.path.join(root, 'out', 'gsp_twin')
    os.makedirs(d, exist_ok=True)
    hdr, cpp = os.path.join(d, 'scratch.h'), os.path.join(d, 'scratch.cpp')
    open(cpp, 'w').write('#include "scratch.h"\nint scratch_main() { return kScratch; }\n')
    base = 'constexpr int kScratch = 1;\n'

    def run():
        p = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'keep_stream_off_link.py'), '--root', root, '--syntax', '--only', 'none.cpp', '--extra', cpp,
                            '--out', os.path.join(d, 'out'), '-j', '1'], capture_output=True, text=True, errors='replace')
        txt = p.stdout + p.stderr
        m = re.search(r'pp-cache: (\d+) of (\d+) units', txt)
        return p.returncode, int(m.group(1)) if m else -1, int(m.group(2)) if m else -1, txt

    bad = []

    def expect(name, ok, why):
        print('%s %s: %s' % ('PASS' if ok else 'FAIL', name, why))
        if not ok:
            bad.append(name)

    # a header text that has never been cached: stamp it with this run's time so step 1 is a real miss
    import time
    stamp = 'constexpr long long kStamp = %dLL;\n' % time.time_ns()   # (a comment would not do: the preprocessed text the key hashes has none)
    open(hdr, 'w').write(stamp + base)
    rc, hit, n, txt = run()
    expect('first_run', rc == 0 and hit == 0 and n == 1, 'rc %d, served %d of %d%s' % (rc, hit, n, '' if rc == 0 else '\n' + txt[-800:]))
    rc, hit, n, txt = run()
    expect('same_text_served', rc == 0 and hit == 1 and n == 1, 'rc %d, served %d of %d' % (rc, hit, n))
    open(hdr, 'w').write(stamp + base + 'constexpr int kOther = 2;\n')
    rc, hit, n, txt = run()
    expect('header_edit_recompiles', rc == 0 and hit == 0 and n == 1, 'rc %d, served %d of %d' % (rc, hit, n))
    open(hdr, 'w').write(stamp + base + '#error planted header break\n')
    rc, hit, n, txt = run()
    expect('header_break_fails', rc != 0 and hit == 0 and 'planted header break' in txt, 'rc %d, served %d of %d (must fail, not served)' % (rc, hit, n))
    open(hdr, 'w').write(stamp + base)
    rc, hit, n, txt = run()
    expect('restored_served', rc == 0 and hit == 1 and n == 1, 'rc %d, served %d of %d' % (rc, hit, n))
    # GSP2 review bug 6, step 6: a header that PREPROCESSES but does not compile must FAIL, and must not be served on a second run (a failure is never stored)
    open(hdr, 'w').write(stamp + 'constexpr int kScratch = "x";\n')
    rc, hit, n, txt = run()
    expect('compile_error_fails', rc != 0 and hit == 0, 'rc %d, served %d of %d (must fail, not served)' % (rc, hit, n))
    rc, hit, n, txt = run()
    expect('compile_error_not_served_again', rc != 0 and hit == 0, 'rc %d, served %d of %d (a second run must still fail, never be served)' % (rc, hit, n))
    open(hdr, 'w').write(stamp + base)
    # step 7: the tree changes between the preprocess and the parse (planted: the header is FIXED right after the first -E): the PASS must not be stored under the broken text's key
    sys.path.insert(0, os.path.join(root, 'tools', 'gate'))
    import game_syntax as gs
    rd = os.path.join(d, 'race')
    os.makedirs(rd, exist_ok=True)
    rh, rc_cpp = os.path.join(rd, 'race.h'), os.path.join(rd, 'race.cpp')
    open(rc_cpp, 'w').write('#include "race.h"\nint race_main() { return kRace; }\n')
    broken_text = 'constexpr int kRace = "x";   // %d\n' % time.time_ns()
    open(rh, 'w').write(broken_text)
    flags = ['-std=c++20']
    k_bad = gs.pp_key(flags, rc_cpp, ())
    real_run = gs._proc.run
    fired = []

    def racing(cmd, *args, **kw):
        r = real_run(cmd, *args, **kw)
        if '-E' in cmd and not fired:
            fired.append(1)
            open(rh, 'w').write('constexpr int kRace = 1;\n')   # fixed right after the first preprocess read the broken text
        return r
    gs._proc.run = racing
    gs.PP_CACHE.clear()
    del gs.NOT_STORED[:]
    try:
        rc2, text2, key2, hit2 = gs.syntax_check(flags, rc_cpp, ())
    finally:
        gs._proc.run = real_run
    expect('race_not_stored', rc2 == 0 and k_bad is not None and key2 != gs.pp_key(flags, rc_cpp, ()) and key2 not in gs.PP_CACHE and len(gs.NOT_STORED) == 1,
           'parse after the fix rc %d, stored under the broken text key: %s, not-stored count %d (want 0 stored, 1 not stored)' % (rc2, key2 in gs.PP_CACHE, len(gs.NOT_STORED)))
    open(rh, 'w').write(broken_text)
    rc3, text3, key3, hit3 = gs.syntax_check(flags, rc_cpp, ())
    expect('race_broken_text_fails', rc3 != 0 and not hit3, 'rc %d, served %s (the broken text must fail, not be served the raced PASS)' % (rc3, hit3))
    got = gs.norm_args(['-IC:/a/wt/src', '-I' + 'C:/a', '-IC:\\a\\wt'], (('C:/a', '<MAIN>'), ('C:/a/wt', '<ROOT>')))
    expect('path_placeholders', got == ['-I<ROOT>/src', '-I<MAIN>', '-I<ROOT>'], 'flags with the tree paths: %s (a worktree inside the main checkout, either slash style, must read the same in every worktree)' % got)
    print('keep_stream syntax cache twin: %d failed' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

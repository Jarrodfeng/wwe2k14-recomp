#!/usr/bin/env python3
r"""Twin of keep_stream_off_link.py --syntax's verdict cache (S103 GSP). 0 game runs, one scratch unit (never a game unit).

  py -I tools\scene\keep_stream_syntax_twin.py [--root DIR]

A scratch unit out\gsp_twin\scratch.cpp includes out\gsp_twin\scratch.h; keep_stream_off_link.py --syntax --extra compiles it with the main.cpp flags:
  1. first run: compiles (rc 0);
  2. same text again: served by the preprocess hash (1 of 1 hit), rc 0;
  3. the HEADER edited (a new declaration): the unit is NOT served (0 of 1), rc 0: the edit reached the preprocessed text;
  4. the header made broken (#error): NOT served and the check FAILS (a stale pass can never hide it);
  5. the header restored to step 1's text: served again (1 of 1);
  6. (GSP2 review bug 6; S105 CC) a header that preprocesses but does not compile: FAILS, and the second run REPLAYS the failure from the store (served, still rc != 0, diagnostics kept);
  7. (moved to syntax_store_selftest.py row 10) the header FIXED between the first preprocess and the parse is not stored under the broken text's key;
  8. the flags' tree paths are placeholders (a worktree inside the main checkout, either slash style).
Exit 0 = all hold. Needs the main checkout's configured release build (the same as keep_stream_off_link.py)."""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# S105 CC F1: the verdict store is per-key files under %USERPROFILE%\.cache; this twin uses a scratch one (set before game_syntax is imported here or in the children it starts)
os.environ['GAME_SYNTAX_CACHE_DIR'] = tempfile.mkdtemp(prefix='gsp_twin_store_')


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
    expect('compile_error_replayed_as_failure', rc != 0 and hit == 1 and 'kScratch' in txt, 'rc %d, served %d of %d (a second run must replay the FAILURE with its diagnostics, never a pass)' % (rc, hit, n))
    open(hdr, 'w').write(stamp + base)
    # step 7 (the second-preprocess race guard) moved to tools\compile\syntax_store_selftest.py row 10 (S105 CC F1): it exercises game_syntax's own store directly
    sys.path.insert(0, os.path.join(root, 'tools', 'compile'))
    import game_syntax as gs
    got = gs.norm_args(['-IC:/a/wt/src', '-I' + 'C:/a', '-IC:\\a\\wt'], (('C:/a', '<MAIN>'), ('C:/a/wt', '<ROOT>')))
    expect('path_placeholders', got == ['-I<ROOT>/src', '-I<MAIN>', '-I<ROOT>'], 'flags with the tree paths: %s (a worktree inside the main checkout, either slash style, must read the same in every worktree)' % got)
    import shutil
    shutil.rmtree(os.environ['GAME_SYNTAX_CACHE_DIR'], ignore_errors=True)
    print('keep_stream syntax cache twin: %d failed' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

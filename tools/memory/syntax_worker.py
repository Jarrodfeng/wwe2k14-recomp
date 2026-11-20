"""syntax_worker.py: ONE tools\\gate\\game_syntax.py worker (one unit, one build), for measure_peak.py (CQM, 2026-11-20).

    py -I tools\\memory\\measure_peak.py --label U5 -- py -I tools\\memory\\syntax_worker.py --build on|off --unit src/main.cpp [--root .]

Runs exactly what game_syntax's --changed pool runs per job (game_syntax.compile_unit: `clang -E -P` for the cache key, `clang -fsyntax-only`, and on a pass
`clang -E -P` again), with the same flags (the main checkout's build.ninja read through `ninja -t commands`, pch dropped, main.cpp gets src\\pch_main.h).
Differences, on purpose: the pp cache is neither read nor written (GAME_SYNTAX_NO_CACHE=1 and PP_CACHE left empty; nothing under the main checkout's
out\\game_syntax is touched), so the parse always runs; no planted twin. Read-only toward the build trees.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'gate'))
os.environ['GAME_SYNTAX_NO_CACHE'] = '1'
import game_syntax as gs  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', choices=('on', 'off'), default='on')
    ap.add_argument('--unit', default='src/main.cpp')
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = gs.main_checkout(root)
    lines = gs.build_commands(main_dir, gs.BUILD_DIRS[a.build])
    fl = gs.flags_for(lines, main_dir.replace('\\', '/'), a.unit)
    if a.unit == 'src/main.cpp':
        fl = fl + ['-include', os.path.abspath(os.path.join(root, 'src', 'pch_main.h')).replace('\\', '/')]
    t0 = time.time()
    label, rc, text, key, hit = gs.compile_unit(root, main_dir, fl, os.path.join(root, a.unit), '%s [%s]' % (a.unit, a.build))
    print('%s: rc %d, %.1f s, cache %s' % (label, rc, time.time() - t0, 'hit' if hit else 'not used'))
    if rc:
        print(text[-2000:])
    return rc


if __name__ == '__main__':
    sys.exit(main())

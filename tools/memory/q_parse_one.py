"""q_parse_one.py: ONE libclang parse of the q "ours" index (build_ours.worker -> ours_clang.parse_tu) on one unit, in THIS process, for measure_peak.py
(CQM, 2026-11-20). No database is opened or written: the worker returns rows and this script prints only their counts.

    py -I tools\\memory\\measure_peak.py --label U3 -- py -I tools\\memory\\q_parse_one.py --compdb FILE --unit src/main.cpp [--root MAIN]

--compdb: a compile_commands.json (e.g. `ninja -t compdb` of out\\build\\win-amd64-release, saved outside the build tree). build_ours itself reads
out\\index\\cc_on (its own configure of the same CMake project, Release); that folder may not exist, and making it would run a configure, so the release
tree's lines are used and go through the SAME build_ours.load_cc / ours_clang.clean_args the index uses. --root: the checkout the sources live in
(default: the main checkout of this tree).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'q'))
import build_ours  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compdb', required=True)
    ap.add_argument('--unit', default='src/main.cpp')
    ap.add_argument('--root')
    a = ap.parse_args()
    root = a.root
    if not root:
        g = subprocess.run(['git', 'rev-parse', '--git-common-dir'], capture_output=True, text=True, cwd=HERE, timeout=60).stdout.strip()
        root = os.path.dirname(os.path.abspath(os.path.join(HERE, g)))
    cc = build_ours.load_cc(a.compdb, root)
    if a.unit not in cc:
        print(f'{a.unit}: not in {a.compdb} under {root}')
        return 2
    key, r = build_ours.worker(('k', root, a.unit, cc[a.unit]))
    if 'error' in r:
        print('parse error:', str(r['error'])[:800])
        return 1
    print(f'{a.unit}: parsed in {r.get("secs", 0):.1f} s; ' + ', '.join(f'{k} {len(v)}' for k, v in r.items() if isinstance(v, (list, dict))))
    return 0


if __name__ == '__main__':
    sys.exit(main())

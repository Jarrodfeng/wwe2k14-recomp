#!/usr/bin/env python3
r"""Lane 238: which post-chain sequences do the case zips of the runs hold, and which are ours? (0 game runs, offline.)

  py -I tools\post\post_scan_zips.py --core out\tools\post_ours_core_test.exe --out SEQ.txt [--glob 20261113_*] [--runs DIR] [--work DIR] [--keep-frames]

For each run folder matching --glob under --runs (default: the main checkout's out\s101\runs) that holds a case_*.zip: extract only capture/ into --work, run the GPU-free core test over it
(one label per run folder), append its CAP / SEQ lines to --out, delete the extraction. A zip that cannot be read, a core test that fails to open a capture: the line says so (a scan that reads
nothing must not look like a pass). The SEQ lines (frames per chain sequence and whether it is ours) are what post_ours_core_test prints.
"""
import argparse
import fnmatch
import os
import shutil
import subprocess
import sys
import zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--core', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--glob', default='*')
    ap.add_argument('--runs', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\s101\runs')
    ap.add_argument('--work', default=r'C:\Users\Jarrod\wwe2k14-recomp\out\lane238\scan')
    ap.add_argument('--skip', default='', help='comma list of folder names to skip')
    args = ap.parse_args()
    skip = set(x for x in args.skip.split(',') if x)
    n = 0
    with open(args.out, 'a') as out:
        for name in sorted(os.listdir(args.runs)):
            if not fnmatch.fnmatch(name, args.glob) or name in skip:
                continue
            d = os.path.join(args.runs, name)
            zips = sorted(f for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')) if os.path.isdir(d) else []
            if not zips:
                continue
            n += 1
            work = os.path.join(args.work, name)
            shutil.rmtree(work, ignore_errors=True)
            try:
                z = zipfile.ZipFile(os.path.join(d, zips[0]))
                z.extractall(work, [m for m in z.namelist() if m.startswith('capture/')])
            except Exception as e:  # noqa
                out.write('FAIL %s: cannot extract (%s)\n' % (name, e))
                out.flush()
                shutil.rmtree(work, ignore_errors=True)
                continue
            r = subprocess.run([args.core, '%s=%s' % (name, os.path.join(work, 'capture'))], capture_output=True, text=True)
            lines = [l for l in r.stdout.splitlines() if l.startswith(('CAP ', 'SEQ ', 'MERGE ', 'FAIL'))]
            out.write('\n'.join(lines) + ('\n' if lines else 'FAIL %s: no output (rc %d)\n' % (name, r.returncode)))
            out.flush()
            shutil.rmtree(work, ignore_errors=True)
    print('scanned %d run folders' % n)
    return 0 if n else 1


if __name__ == '__main__':
    sys.exit(main())

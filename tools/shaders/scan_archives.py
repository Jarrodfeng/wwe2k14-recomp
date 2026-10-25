"""Session 61: which of the player's game files hold shader containers (read-only; game_data\\extracted).

    py tools\\shaders\\scan_archives.py [--jobs 12] [--out out\\shaders\\scan.json] [--only NAME ...]

Every pac is unpacked the way tools\\wm30\\pac.py does (XCTD via out\\tools\\xctd_unpack.exe, EPAC / PACH, BPE,
ZLIB), every leaf searched for containers (tools\\shaders\\shaderlib.py); the XEX image (out\\harness\\index\\image.bin,
the D3D library's own shaders) too. The JSON lists per file: leaves with containers (path, VS / PS count, XXH3 of each
microcode) and unpack failures. Hashes and counts only: no game bytes in the output.
"""

import argparse
import json
import multiprocessing
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'wm30'))
import pac  # noqa: E402
import shaderlib  # noqa: E402

IMAGE = os.path.join(shaderlib.REPO, 'out', 'harness', 'index', 'image.bin')


def scan_bytes(data):
    found = []
    for off, kind, code, mc in shaderlib.containers(data):
        found.append({'off': off, 'kind': kind, 'dwords': len(mc) // 4, 'hash': '%016X' % shaderlib.xxh3(mc)})
    return found


def scan_file(path):
    rel = os.path.relpath(path, shaderlib.GAME).replace(os.sep, '/')
    t = time.time()
    res = {'file': rel, 'leaves': [], 'error': None}
    try:
        if rel == 'default.xex':
            data = open(IMAGE, 'rb').read()
            hits = scan_bytes(data)
            if hits:
                res['leaves'].append({'leaf': 'image@0x82000000', 'shaders': hits})
        else:
            data = pac.load(path)
            n = 0
            for name, dec in pac.entries(data):
                n += 1
                if b'\x10\x2a\x11' in dec:
                    hits = scan_bytes(dec)
                    if hits:
                        res['leaves'].append({'leaf': name, 'shaders': hits})
            res['leaf_count'] = n
            # a pac that is not EPAC / PACH: search the raw bytes too
            if n == 0 and b'\x10\x2a\x11' in data:
                hits = scan_bytes(data)
                if hits:
                    res['leaves'].append({'leaf': 'raw', 'shaders': hits})
    except BaseException as e:  # noqa: BLE001 - one bad file must not stop the scan
        res['error'] = '%s: %s' % (type(e).__name__, str(e)[:200])
    res['seconds'] = round(time.time() - t, 1)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--out', default=os.path.join(shaderlib.REPO, 'out', 'shaders', 'scan.json'))
    ap.add_argument('--only', nargs='*')
    a = ap.parse_args()
    files = [os.path.join(shaderlib.GAME, 'default.xex')]
    for root, _, fs in os.walk(os.path.join(shaderlib.GAME, 'pac')):
        files += [os.path.join(root, f) for f in sorted(fs)]
    if a.only:
        files = [f for f in files if any(o.lower() in f.lower() for o in a.only)]
    shaderlib.xxh3(b'')  # fail early without the DLL
    t = time.time()
    results = []
    with multiprocessing.Pool(a.jobs) as pool:
        for i, r in enumerate(pool.imap_unordered(scan_file, files), 1):
            results.append(r)
            k = sum(len(l['shaders']) for l in r['leaves'])
            if k or r['error']:
                print('[%d/%d] %s: %d containers%s' % (i, len(files), r['file'], k,
                                                       ('  ERROR ' + r['error']) if r['error'] else ''), flush=True)
    results.sort(key=lambda r: r['file'])
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(results, open(a.out, 'w'), indent=1)
    uniq = {}
    for r in results:
        for l in r['leaves']:
            for s in l['shaders']:
                uniq.setdefault(s['hash'], s['kind'])
    errs = [r for r in results if r['error']]
    print('%d files in %.0f s: %d with shaders, %d unique shaders (%d VS, %d PS), %d failed to unpack -> %s' % (
        len(results), time.time() - t, sum(1 for r in results if r['leaves']), len(uniq),
        sum(1 for k in uniq.values() if k == 'VS'), sum(1 for k in uniq.values() if k == 'PS'), len(errs), a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())

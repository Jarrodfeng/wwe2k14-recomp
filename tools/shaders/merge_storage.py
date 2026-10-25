"""Session 61: merge the shader storage of many runs (run_list.ps1 -KeepShaderStorage DIR) into one, and write the list.

    py tools\\shaders\\merge_storage.py DIR [DIR ...] [--out out\\shaders\\merged] [--list out\\shaders\\shader_list.txt]

Each DIR is one run's `cache\\shaders\\shareable` copy (or a folder of such copies: every sub-folder with an .xsh is
taken, in name order). Merged by hash, first seen wins: `545408B2.xsh` (shaders), `545408B2.fsi.vk.xpso` /
`.fbo.vk.xpso` (the emulated path's pipelines, same header kept), `545408B2.native.fsi|fbo.vk.xnps` (Session 61, the
native renderer's pipelines). Per run: how many shaders / pipelines it added that the runs before it had not (a sweep stops
when runs from different families add nothing). `--list`: the shipped list (docs\\systems\\shaders.md, "The list"):
`S` lines from tools\\shaders\\locate.py (hashes, sources, XOR masks), then `P fsi|fbo <record hex>` and
`N fsi|fbo <record hex>` (pipeline descriptions: hashes, modification bits, state; no shader code).
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderlib  # noqa: E402
import xsh  # noqa: E402

TITLE = '545408B2'
XNPS_MAGIC = 0x53504E58
XNPS_RECORD = 8 + 64


def storage_dirs(paths):
    out = []
    for p in paths:
        if os.path.exists(os.path.join(p, TITLE + '.xsh')):
            out.append(p)
            continue
        for d in sorted(os.listdir(p)):
            q = os.path.join(p, d)
            if os.path.isdir(q) and os.path.exists(os.path.join(q, TITLE + '.xsh')):
                out.append(q)
    return out


def read_xsh_raw(path):
    """[(hash, raw record bytes)] (header excluded), as the SDK writes them."""
    data = open(path, 'rb').read()
    out, p = [], 8
    while p + 12 <= len(data):
        h, bits = struct.unpack_from('<QI', data, p)
        n = 12 + (bits & 0x7FFFFFFF) * 4
        if p + n > len(data):
            break
        out.append((h, data[p:p + n]))
        p += n
    return data[:8], out


def read_records(path, header_size, record_size):
    if not os.path.exists(path):
        return None, []
    data = open(path, 'rb').read()
    head = data[:header_size]
    recs = []
    p = header_size
    while p + record_size <= len(data):
        recs.append(data[p:p + record_size])
        p += record_size
    return head, recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--out', default=os.path.join(shaderlib.REPO, 'out', 'shaders', 'merged'))
    ap.add_argument('--list')
    a = ap.parse_args()
    dirs = storage_dirs(a.dirs)
    if not dirs:
        print('no storage folders found')
        return 1
    xsh_head, shaders = None, {}
    pso = {'fsi': [None, {}], 'fbo': [None, {}]}
    native = {'fsi': [None, {}], 'fbo': [None, {}]}
    print('%-46s %8s %8s %8s %8s' % ('run', '+shaders', '+fsi', '+fbo', '+native'))
    for d in dirs:
        added = [0, 0, 0, 0]
        head, recs = read_xsh_raw(os.path.join(d, TITLE + '.xsh'))
        xsh_head = xsh_head or head
        for h, raw in recs:
            if h not in shaders:
                shaders[h] = raw
                added[0] += 1
        for k, api in enumerate(('fsi', 'fbo')):
            head, recs = read_records(os.path.join(d, '%s.%s.vk.xpso' % (TITLE, api)), 12, 68)
            if head is None:
                continue
            if pso[api][0] is None:
                pso[api][0] = head
            elif head != pso[api][0]:
                print('  %s: %s header differs (another SDK version?): skipped' % (d, api))
                continue
            for r in recs:
                h = struct.unpack_from('<Q', r, 0)[0]
                if h not in pso[api][1]:
                    pso[api][1][h] = r
                    added[1 + k] += 1
        for api in ('fsi', 'fbo'):
            head, recs = read_records(os.path.join(d, '%s.native.%s.vk.xnps' % (TITLE, api)), 16, XNPS_RECORD)
            if head is None:
                continue
            if native[api][0] is None:
                native[api][0] = head
            if head != native[api][0]:
                print('  %s: native %s header differs: skipped' % (d, api))
                continue
            for r in recs:
                h = struct.unpack_from('<Q', r, 0)[0]
                if h not in native[api][1]:
                    native[api][1][h] = r
                    added[3] += 1
        print('%-46s %8d %8d %8d %8d' % (os.path.basename(d)[:46], *added))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, TITLE + '.xsh'), 'wb') as o:
        o.write(xsh_head)
        for raw in shaders.values():
            o.write(raw)
    for api, (head, recs) in pso.items():
        if head is not None:
            with open(os.path.join(a.out, '%s.%s.vk.xpso' % (TITLE, api)), 'wb') as o:
                o.write(head)
                for r in recs.values():
                    o.write(r)
    for api, (head, recs) in native.items():
        if head is not None:
            with open(os.path.join(a.out, '%s.native.%s.vk.xnps' % (TITLE, api)), 'wb') as o:
                o.write(head)
                for r in recs.values():
                    o.write(r)
    print('merged: %d shaders, %d fsi + %d fbo pipelines, %d + %d native (fsi + fbo) pipelines -> %s' % (
        len(shaders), len(pso['fsi'][1]), len(pso['fbo'][1]), len(native['fsi'][1]), len(native['fbo'][1]), a.out))
    if a.list:
        import subprocess
        tmp = a.list + '.shaders'
        rc = subprocess.call([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'locate.py'),
                              'match', os.path.join(a.out, TITLE + '.xsh'), '--list', tmp])
        with open(a.list, 'w', newline='\n') as o:
            o.write(open(tmp).read())
            o.write('H xsh %s\n' % xsh_head.hex().upper())
            for api, (head, recs) in pso.items():
                if head is not None:
                    o.write('H %s %s\n' % (api, head.hex().upper()))
                    for r in recs.values():
                        o.write('P %s %s\n' % (api, r.hex().upper()))
            for api, (head, recs) in native.items():
                if head is not None:
                    o.write('H native-%s %s\n' % (api, head.hex().upper()))
                    for r in recs.values():
                        o.write('N %s %s\n' % (api, r.hex().upper()))
        os.remove(tmp)
        print('list -> %s (locate exit %d: 1 = some shaders not found in the files)' % (a.list, rc))
    return 0


if __name__ == '__main__':
    sys.exit(main())

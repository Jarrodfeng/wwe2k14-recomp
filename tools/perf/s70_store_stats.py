"""Session 70: what a NATIVE pipeline store holds (545408B2.native.fsi|fbo.vk.xnps).

Usage: py tools\\perf\\s70_store_stats.py STORE_OR_USERDATA_DIR [...]
Per file: records, how many carry the Session 69 clamp bit (state bit 14 = clip_disable), the ext bits (0 = an older
record: prewarmed in both epilogue variants by v0.2.8), native pixel / twin / geometry flags, distinct shader pairs.
Record layout: native_renderer.h PipelineRecord (64 bytes) after a 16-byte header, each with its LE64 XXH3 first.
"""
import collections
import os
import struct
import sys


def files(arg):
    if os.path.isfile(arg):
        return [arg]
    out = []
    for root, _, names in os.walk(arg):
        out += [os.path.join(root, n) for n in names if n.endswith('.xnps')]
    return out


def stats(path):
    d = open(path, 'rb').read()
    magic, version, modv, size = struct.unpack_from('<4I', d, 0)
    recs = []
    p = 16
    while p + 8 + size <= len(d):
        r = d[p + 8:p + 8 + size]
        vs, vm, ps, pm, tint, state, blend, variant = struct.unpack_from('<5Q3I', r, 0)
        flags, quant, prim, ext = r[60], r[61], r[62], r[63]
        recs.append(dict(vs=vs, ps=ps, state=state, flags=flags, ext=ext, variant=variant))
        p += 8 + size
    c = collections.Counter()
    for r in recs:
        c['clamp'] += (r['state'] >> 14) & 1
        c['ext_known'] += 1 if r['ext'] & 1 else 0
        c['ext_zero'] += 1 if r['ext'] == 0 else 0
        c['native_pixel'] += r['flags'] & 1
        c['twin'] += (r['flags'] >> 2) & 1
        c['post'] += 1 if r['variant'] & 0xF else 0
    pairs = len({(r['vs'], r['ps']) for r in recs})
    print(f'{path}: header v{version:08X} mod {modv} size {size}; {len(recs)} records, {pairs} shader pairs; '
          f'clamp bit {c["clamp"]}, ext known {c["ext_known"]}, ext 0 (old format) {c["ext_zero"]}, '
          f'native pixel {c["native_pixel"]}, twin {c["twin"]}, post {c["post"]}')


for a in sys.argv[1:]:
    for f in files(a):
        stats(f)

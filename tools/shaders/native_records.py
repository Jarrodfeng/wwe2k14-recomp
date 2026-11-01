"""Session 80 (HD 520 arc, first launch): what makes the shipped list's NATIVE pipelines differ.

Reads the `N fsi|fbo <72-byte hex>` records of a shader list (docs\\systems\\shaders.md section 6: XXH3 + the SDK's
64-byte NativeRenderer::PipelineRecord) and prints how many distinct shaders, shader variants (modifications) and
state values they span, and for the (vertex, pixel) pairs with several records which fields tell them apart.

    py tools\\shaders\\native_records.py db\\shaders\\shader_list.txt [--path fsi] [--top 15]
"""
import argparse
import collections
import struct

FIELDS = ('vs', 'vs_mod', 'ps', 'ps_mod', 'tint', 'state', 'blend', 'variant', 'layout', 'flags', 'quantize',
          'host_prim', 'ext')


def records(path, which):
    out = []
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) != 3 or p[0] != 'N' or p[1] != which:
                continue
            b = bytes.fromhex(p[2])
            if len(b) != 72:
                continue
            v = struct.unpack('<5Q3I4H4B', b[8:])
            out.append(dict(zip(FIELDS, v[:8] + (v[8:12],) + v[12:])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('list')
    ap.add_argument('--path', default='fsi')
    ap.add_argument('--top', type=int, default=15)
    a = ap.parse_args()
    rs = records(a.list, a.path)
    print('%d %s records' % (len(rs), a.path))
    for f in FIELDS:
        print('  distinct %-9s %d' % (f, len({r[f] for r in rs})))
    print('  distinct vertex translations (vs, vs_mod): %d' % len({(r['vs'], r['vs_mod']) for r in rs}))
    print('  distinct pixel translations (ps, ps_mod):  %d' % len({(r['ps'], r['ps_mod']) for r in rs}))
    print('  distinct shader pairs (vs, ps):            %d' % len({(r['vs'], r['ps']) for r in rs}))
    print('  distinct translation pairs:                %d' % len(
        {(r['vs'], r['vs_mod'], r['ps'], r['ps_mod']) for r in rs}))
    # Which fields separate the records of one shader pair (the pair's records compared with each other).
    sep = collections.Counter()
    pairs = collections.defaultdict(list)
    for r in rs:
        pairs[(r['vs'], r['ps'])].append(r)
    extra = 0
    for recs in pairs.values():
        if len(recs) < 2:
            continue
        extra += len(recs) - 1
        for f in FIELDS:
            if f in ('vs', 'ps'):
                continue
            if len({r[f] for r in recs}) > 1:
                sep[f] += len({r[f] for r in recs}) - 1
    print('records beyond one per shader pair: %d; fields that differ inside a pair (extra values summed):' % extra)
    for f, n in sep.most_common():
        print('  %-9s %d' % (f, n))
    # The pairs with the most records.
    print('pairs with the most records:')
    for (vs, ps), recs in sorted(pairs.items(), key=lambda kv: -len(kv[1]))[:a.top]:
        diff = [f for f in FIELDS if f not in ('vs', 'ps') and len({r[f] for r in recs}) > 1]
        print('  vs %016X ps %016X: %3d records, differ in %s' % (vs, ps, len(recs), ', '.join(diff) or '-'))


if __name__ == '__main__':
    main()

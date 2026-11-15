"""S102 W3 step 1: rank the game's own (recompiled) functions on the VdSwap / render-command thread from a sampling-profiler
stacks file, self and inclusive, over a guest-frame window. Frames are mapped to guest functions by their generated-source line
(generated\\default\\wwe2k14_recomp.N.cpp:LINE -> the enclosing DEFINE_REX_FUNC). Read-only; reads generated code from --gen.

  py -I tools\\w3\\rank_guest.py STACKS --gen DIR [--lo 5900 --hi 10400] [--thread "main thread"] [--top 40]
"""
import argparse, bisect, os, re, sys
from collections import Counter


def func_index(gen, n, cache):
    if n in cache:
        return cache[n]
    p = os.path.join(gen, 'wwe2k14_recomp.%d.cpp' % n)
    starts, names = [], []
    pat = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
    with open(p, encoding='utf-8', errors='replace') as f:
        for i, l in enumerate(f, 1):
            m = pat.match(l)
            if m:
                starts.append(i)
                names.append(m.group(1))
    cache[n] = (starts, names)
    return cache[n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stacks')
    ap.add_argument('--gen', required=True)
    ap.add_argument('--lo', type=int, default=5900)
    ap.add_argument('--hi', type=int, default=10400)
    ap.add_argument('--thread', default='main thread')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    names, stacks, slines, lines = {}, {}, {}, {}
    samples = Counter()
    with open(a.stacks, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('n '):
                _, i, nm = line.split(' ', 2)
                names[int(i)] = nm
            elif line.startswith('s '):
                _, i, key = line.split(' ', 2)
                p = key.split(',')
                stacks[int(i)] = (p[0], [int(x) for x in p[1:] if x])
            elif line.startswith('sl '):
                _, i, ids = line.split(' ', 2)
                slines[int(i)] = [None if x == '-' else int(x) for x in ids.split(',')]
            elif line.startswith('l '):
                _, i, num, file = line.split(' ', 3)
                lines[int(i)] = (file, int(num))
            elif line and line[0].isdigit():
                fr, sid = line.split(' ')[:2]
                if a.lo <= int(fr) <= a.hi:
                    samples[int(sid)] += 1
    cache = {}

    def guest(file, num):
        m = re.search(r'wwe2k14_recomp\.(\d+)\.cpp$', file)
        if not m:
            return None
        st, nm = func_index(a.gen, int(m.group(1)), cache)
        k = bisect.bisect_right(st, num) - 1
        return nm[k] if k >= 0 else None

    selfc, incl, pure, hostleaf = Counter(), Counter(), Counter(), Counter()
    total = 0
    nframes = a.hi - a.lo + 1
    for sid, cnt in samples.items():
        lab, ids = stacks[sid]
        if lab != a.thread:
            continue
        total += cnt
        lids = slines.get(sid) or [None] * len(ids)
        gl = []  # leaf first
        for nid, lid in zip(ids, lids):
            g = None
            if lid is not None and lid in lines:
                g = guest(*lines[lid])
            gl.append(g)
        first = next((g for g in gl if g), None)
        if gl and gl[0]:
            pure[gl[0]] += cnt   # the sampled instruction itself is inside this function's generated code
        elif first:
            hostleaf[(first, names[ids[0]].split('!')[0].split('+')[0], ('line' if lids and lids[0] is not None and lids[0] in lines else 'noline'))] += cnt
        selfc[first or '(none)'] += cnt
        seen = set()
        for g in gl:
            if g and g not in seen:
                seen.add(g)
                incl[g] += cnt
    print('thread %r, frames %d-%d: %d samples (%.2f per frame)' % (a.thread, a.lo, a.hi, total, total / nframes))
    print('%-34s | %s' % ('by self* (share, samples/frame)', 'by inclusive'))
    s = selfc.most_common(a.top)
    i = incl.most_common(a.top)
    for k in range(a.top):
        l = '%-14s %7.2f%% %8.3f' % (s[k][0], 100 * s[k][1] / total, s[k][1] / nframes) if k < len(s) else ''
        r = '%-14s %7.2f%% %8.3f' % (i[k][0], 100 * i[k][1] / total, i[k][1] / nframes) if k < len(i) else ''
        print('%-34s | %s' % (l, r))
    print()
    print("PURE self (the sampled instruction is in the function's own generated code), per frame; share of thread")
    for g, c in pure.most_common(a.top):
        print('  %-14s %7.2f%% %8.3f' % (g, 100 * c / total, c / nframes))
    print('host-leaf samples charged to the nearest guest caller: (guest fn, module of leaf frame, line known)')
    for k, c in hostleaf.most_common(30):
        print('  %-14s %-22s %-7s %7.2f%% %8.3f' % (k[0], k[1][:22], k[2], 100 * c / total, c / nframes))
    print('* self = the leaf-most guest function on the stack (host callees below it: memcmp, heap, our hooks, count to it)')

    if total == 0 or not incl or incl.most_common(1)[0][0] != 'sub_82BB5CC0':
        print('rank_guest: the profile holds no samples of the render-command thread (or its root is not sub_82BB5CC0): not a result')
        sys.exit(1)


main()

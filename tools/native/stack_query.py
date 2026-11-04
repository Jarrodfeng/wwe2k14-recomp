r"""Ask one thread's profiled stacks a question over a frame range (Session 89; promoted from Session 87's stk.py).
Reads the stacks file of --wwe2k14_profile_stacks (the same file tools\native\stack_range.py reads).

    py -I tools\native\stack_query.py STACKS F0 F1 MODE REGEX [--top 25] [--thread "GPU Commands"]

modes (REGEX matches function names):
  callers  stacks whose LEAF matches: count the nearest frame of ours (rexgpu, rex::) with its line - who calls it
  callees  stacks containing the function: count the frame directly below it (its child) + line
  parent   the frame directly above the outermost match
  chain    the 4 frames above the outermost match, joined
  incl     inclusive share of stacks containing the function
Shares are of the thread's samples in F0-F1; "per frame" = samples / frames.
"""
import argparse
import re
from collections import Counter


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stacks'); ap.add_argument('f0', type=int); ap.add_argument('f1', type=int)
    ap.add_argument('mode', choices=['callers', 'callees', 'parent', 'chain', 'incl']); ap.add_argument('regex')
    ap.add_argument('--top', type=int, default=25); ap.add_argument('--thread', default='GPU Commands')
    a = ap.parse_args()
    names, lines, stacks, hits, last = {}, {}, {}, Counter(), None
    with open(a.stacks, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('n '):
                _, nid, name = line.rstrip('\n').split(' ', 2)
                names[int(nid)] = name; last = int(nid)
            elif line.startswith('l '):
                _, nid, ln, fl = line.rstrip('\n').split(' ', 3)
                lines[last] = (int(ln), fl.rsplit('\\', 1)[-1])
            elif line.startswith('s '):
                _, sid, key = line.rstrip('\n').split(' ', 2)
                parts = key.split(',')
                stacks[int(sid)] = (parts[0], [int(x) for x in parts[1:] if x])
            elif line and line[0].isdigit():
                fr, sid = line.split(' ')[:2]
                if a.f0 <= int(fr) <= a.f1:
                    hits[int(sid)] += 1
    rx = re.compile(a.regex)

    def nm(i):
        s = names.get(i, '?').split('!', 1)[-1]
        return s.replace('rex::graphics::vulkan::', 'v::').replace('rex::graphics::', 'g::')

    total, res = 0, Counter()
    for sid, n in hits.items():
        th, ids = stacks[sid]
        if th != a.thread:
            continue
        total += n
        fn = [names.get(i, '?') for i in ids]
        if a.mode == 'callers':
            if fn and rx.search(fn[0]):
                for j, i in enumerate(ids):
                    if 'rexgpu' in fn[j] and 'rex::' in fn[j]:
                        res[(nm(i), lines.get(i, ('?', ''))[0])] += n
                        break
        elif a.mode == 'callees':
            for j, i in enumerate(ids):
                if rx.search(fn[j]):
                    res[(nm(ids[j - 1]), lines.get(ids[j - 1], ('?', ''))[0]) if j > 0 else ('<self>', 0)] += n
                    break
        else:
            idx = [j for j, x in enumerate(fn) if rx.search(x)]
            if not idx:
                continue
            j = idx[-1]
            if a.mode == 'parent':
                res[(nm(ids[j + 1]), 0) if j + 1 < len(ids) else ('<top>', 0)] += n
            elif a.mode == 'chain':
                res[(' < '.join(nm(i) for i in ids[j + 1:j + 5]), 0)] += n
            else:
                res[('match', 0)] += n
    if not total:
        raise SystemExit(f'stack_query: no samples of thread {a.thread!r} in frames {a.f0}-{a.f1}')
    print(f'{a.thread}: {total} samples, {total / (a.f1 - a.f0 + 1):.2f} per frame')
    for (k, ln), v in res.most_common(a.top):
        print(f'{100 * v / total:6.2f}%  {k[:110]}  line {ln}')


if __name__ == '__main__':
    main()

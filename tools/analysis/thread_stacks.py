"""Whole call stacks of one sampled thread over a frame range, grouped, with host frames squeezed out.

Usage: py tools/analysis/thread_stacks.py STACKS_FILE --thread NAME [--frames A-B] [--depth 14] [--top 12]
       [--keep-host]

Reads a stacks file of the sampling profiler (logs/stacks_NNN.txt or a slow-frame frame_<N>_stacks.txt; format in
tools/analysis/profile_stacks.py). For each distinct stack (leaf first): sample count and share, the leaf, then the
guest frames (sub_XXXXXXXX, with db/*.json names in brackets) up to --depth; host frames between them are dropped
unless --keep-host (the leaf is always kept, so a host wait or sleep stays visible with its guest callers).
Thread names come from --wwe2k14_profile_threads (e.g. the barrier probe's bclient_<lr>_<tid> names).
Session 8 task B. Reads only our own run output.
"""
import argparse
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(__file__))
import profile_stacks  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def db_names():
    names = {}
    ddir = os.path.join(ROOT, 'db')
    for fn in os.listdir(ddir):
        if fn.endswith('.json'):
            try:
                d = json.load(open(os.path.join(ddir, fn), encoding='utf-8'))
            except Exception:
                continue
            ents = d if isinstance(d, list) else d.get('functions', d.get('entries', []))
            for e in ents if isinstance(ents, list) else []:
                if isinstance(e, dict) and 'address' in e and 'name' in e:
                    names[e['address'].upper().replace('0X', '')] = e['name']
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stacks')
    ap.add_argument('--thread', required=True)
    ap.add_argument('--frames', default=None)
    ap.add_argument('--depth', type=int, default=14)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--keep-host', action='store_true')
    a = ap.parse_args()
    lo, hi = (0, 1 << 62)
    if a.frames:
        p = a.frames.split('-')
        lo, hi = int(p[0]), int(p[-1])
    stacks, per_thread, chosen = profile_stacks.load(a.stacks, a.thread, lo, hi)
    dbn = db_names()

    def nm(s):
        base = s.replace('__imp__', '')
        if base.startswith('sub_') and base[4:12].upper() in dbn:
            return f'{base} [{dbn[base[4:12].upper()]}]'
        return s

    groups = Counter()
    for names, _lines, count in stacks:
        if not names:
            continue
        out = [names[0]]
        for s in names[1:]:
            if 'sub_8' in s or a.keep_host:
                b = s.replace('__imp__', '')
                if b != out[-1].replace('__imp__', ''):
                    out.append(s)
        groups[tuple(nm(x) for x in out[:a.depth])] += count
    total = sum(groups.values())
    print(f'{total} samples ({", ".join(sorted(chosen))}, frames {lo}-{hi if hi < 1 << 62 else "end"})')
    for st, c in groups.most_common(a.top):
        print(f'{100.0 * c / max(1, total):5.1f}% {c:4d}  ' + ' <- '.join(st))


if __name__ == '__main__':
    main()

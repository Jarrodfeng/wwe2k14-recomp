"""Where the game's CPU time goes across ALL its threads, by guest function (Session 95: every CPU watt is a GPU watt on
the 15 W Surface). Answers: which threads are busy, what each one is (its outermost guest function), the top guest
functions over all threads, and which host helpers the guest code spends in.

Each sample is charged to its innermost guest function (sub_XXXXXXXX or __imp__sub_XXXXXXXX), host helpers included;
samples whose leaf is a wait are left out. "core" = samples / sampling rounds (rounds = the most samples of any thread),
so 1.00 = one thread busy the whole range. Run the game with:
  --wwe2k14_profile_gpu_commands=true --wwe2k14_profile_stacks=true --wwe2k14_profile_lines=false
  --wwe2k14_profile_max_named=40 --wwe2k14_profile_threads=XThread,AK::,Audio

    py tools\\perf\\guest_cpu.py logs\\stacks_NNN.txt --frames A-B [--top 30] [--thread-top 8] [--leafs 25] [--json OUT]
"""
import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'analysis'))
import profile_stacks  # noqa: E402

GUEST = re.compile(r'^(?:__imp__)?(sub_[0-9A-Fa-f]{8})(?:\+.*)?$')
WAITS = ('NtWaitForSingleObject', 'NtWaitForMultipleObjects', 'ZwWaitForAlertByThreadId', 'NtDelayExecution',
         'RtlSleepConditionVariableSRW', 'NtWaitForAlertByThreadId', 'ZwDelayExecution', 'NtYieldExecution',
         'SwitchToThread', 'ZwWaitForSingleObject', 'ZwWaitForMultipleObjects', 'NtRemoveIoCompletion',
         'ZwRemoveIoCompletion', 'NtSignalAndWaitForSingleObject', 'ZwSignalAndWaitForSingleObject',
         'NtWaitForWorkViaWorkerFactory', 'ZwWaitForWorkViaWorkerFactory', 'ZwYieldExecution')


def leaf_class(leaf):
    """Which kind of code a sample's leaf is."""
    if guest(leaf):
        return 'guest code'
    if re.match(r'(__imp__)?__(save|rest)(gpr|fpr|vmx)', leaf):
        return 'guest register save / restore helpers'
    if leaf.endswith('!fma') or 'fma' == leaf:
        return 'ucrt fma (guest fmadd family)'
    if leaf.startswith(('ntdll', 'KERNEL', 'kernel', 'win32u', 'USER32')):
        return 'Windows (ntdll / kernel32)'
    if leaf.startswith(('VCRUNTIME', 'ucrtbase', 'MSVCP')):
        return 'C runtime (memcpy, memchr, locks)'
    if leaf.startswith(('rexruntime', 'rexgpu')):
        return 'SDK DLLs (kernel exports, memory)'
    return 'our hooks (src\\*.h native code)'


def guest(name):
    m = GUEST.match(name)
    return m.group(1) if m else None


def is_wait(leaf):
    # a stack of RtlUserThreadStart alone = a thread created suspended, never run (two such XThreads in a match)
    return any(w in leaf for w in WAITS) or leaf.endswith('RtlUserThreadStart')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stacks')
    ap.add_argument('--frames', required=True, help='A-B guest frames')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--thread-top', type=int, default=8)
    ap.add_argument('--leafs', type=int, default=25)
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.frames.split('-'))
    _, per_thread, _ = profile_stacks.load(a.stacks, 'all', lo, hi)
    rounds = max(per_thread.values()) if per_thread else 1
    own = defaultdict(Counter)      # thread -> guest function -> busy samples
    incl = defaultdict(Counter)     # thread -> guest function -> busy samples anywhere on the stack
    roots = defaultdict(Counter)    # thread -> outermost guest function
    leafs = Counter()               # leaf, for samples whose leaf is host code
    leafs_by_guest = defaultdict(Counter)
    selfs = Counter()               # guest functions, samples with the function itself as the leaf
    classes = Counter()
    busy = Counter()
    by_label = rows_by_label(a.stacks, lo, hi)
    for label, rows in by_label.items():
        for names, count in rows:
            gs = [guest(n) for n in names]
            order = [g for g in gs if g]
            if order:
                # sub_822674A8 = the guest's common thread start; the thread's own entry is the frame inside it
                roots[label][order[-2] if len(order) > 1 and order[-1] == 'sub_822674A8' else order[-1]] += count
            if is_wait(names[0]):
                continue
            busy[label] += count
            classes[leaf_class(names[0])] += count
            if gs[0]:
                selfs[gs[0]] += count
            first = order[0] if order else '(host only)'
            own[label][first] += count
            for f in set(order):
                incl[label][f] += count
            if not gs[0]:
                leafs[names[0]] += count
                leafs_by_guest[names[0]][first] += count
    total_busy = sum(busy.values())
    frames = hi - lo + 1
    print(f'frames {lo}-{hi} ({frames}), rounds {rounds}, threads {len(per_thread)}, busy samples {total_busy} '
          f'= {total_busy / rounds:.2f} cores')
    print(f'\n{"core":>6} {"alive":>6}  {"root (outermost guest fn)":<14} thread')
    for label, c in busy.most_common():
        root = roots[label].most_common(1)[0][0] if roots[label] else '-'
        print(f'{c / rounds:6.3f} {per_thread[label] / rounds:6.2f}  {root:<14} {label}')
    print('\n== what kind of code the samples are in')
    for k, c in classes.most_common():
        print(f'{100.0 * c / total_busy:6.2f} {c / rounds:6.3f}  {k}')
    print(f'\n== top {a.top} guest functions over all threads (own = innermost guest function, host helpers included)')
    allown, allincl = Counter(), Counter()
    for t in own:
        allown.update(own[t])
        allincl.update(incl[t])
    print(f'{"own%":>6} {"core":>6} {"incl%":>6}  function       threads')
    out = []
    for f, c in allown.most_common(a.top):
        split = sorted(((own[t][f], t) for t in own if own[t][f]), reverse=True)[:3]
        where = ', '.join(f'{t} {100.0 * n / c:.0f}%' for n, t in split)
        print(f'{100.0 * c / total_busy:6.2f} {c / rounds:6.3f} {100.0 * allincl[f] / total_busy:6.1f}  {f:<14} {where}')
        out.append({'fn': f, 'own_pct': 100.0 * c / total_busy, 'cores': c / rounds, 'incl_pct': 100.0 * allincl[f] / total_busy,
                    'threads': [[t, n] for n, t in split]})
    print(f'\n== host leafs under guest code (samples whose leaf is not guest code), share of all busy')
    for leaf, c in leafs.most_common(a.leafs):
        top = ', '.join(f'{g} {100.0 * n / c:.0f}%' for g, n in leafs_by_guest[leaf].most_common(3))
        print(f'{100.0 * c / total_busy:6.2f} {c / rounds:6.3f}  {profile_stacks.short(leaf, 60):<60} {top}')
    print(f'\n== per thread, top {a.thread_top} own and inclusive')
    for label, c in busy.most_common(14):
        print(f'-- {label}: {c / rounds:.3f} core')
        for f, n in own[label].most_common(a.thread_top):
            print(f'   {100.0 * n / c:5.1f}%  incl {100.0 * incl[label][f] / c:5.1f}%  {f}')
        hi_incl = [f for f, _ in incl[label].most_common(a.thread_top)]
        print('   inclusive: ' + ', '.join(f'{f} {100.0 * incl[label][f] / c:.0f}%' for f in hi_incl))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'frames': [lo, hi], 'rounds': rounds, 'top': out, 'self': selfs, 'classes': classes, 'total_busy': total_busy, 'self': selfs, 'classes': classes, 'total_busy': total_busy,
                       'threads': {t: {'cores': busy[t] / rounds, 'root': roots[t].most_common(1)[0][0] if roots[t] else ''}
                                   for t in busy}}, f, indent=1)


def rows_by_label(path, lo, hi):
    """{thread label: [(names leaf first, count)]} in one pass over the file."""
    names, stacks = {}, {}
    samples = Counter()
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('n '):
                _, nid, name = line.split(' ', 2)
                names[int(nid)] = name
            elif line.startswith('s '):
                _, sid, key = line.split(' ', 2)
                parts = key.split(',')
                stacks[int(sid)] = (parts[0], [int(x) for x in parts[1:] if x])
            elif line and line[0].isdigit():
                frame, sid = line.split(' ')[:2]
                if lo <= int(frame) <= hi:
                    samples[int(sid)] += 1
    out = defaultdict(list)
    for sid, count in samples.items():
        label, ids = stacks[sid]
        out[label].append(([names[i] for i in ids], count))
    return out


if __name__ == '__main__':
    main()

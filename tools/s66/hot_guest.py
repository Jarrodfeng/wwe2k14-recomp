"""The native function factory's hot list (Session 66, track F2): which recompiled guest functions the game's threads
spend their CPU time in, from a profiled run's call stacks.

Each sample is charged to its innermost guest function (sub_XXXXXXXX or __imp__sub_XXXXXXXX: the time a native version
of that function would replace, host helpers it calls included); samples whose leaf is a wait (the thread asleep) are
left out. Also printed: the inclusive share (the function and everything it calls).

Run: a launched route with --wwe2k14_profile_gpu_commands=true --wwe2k14_profile_stacks=true
--wwe2k14_profile_threads=*F800002C,*F8000054,*F8000070 (the game threads; '*' = anywhere in the thread's name).

    py tools\\s66\\hot_guest.py logs\\stacks_NNN.txt --frames A-B [--thread "*F8000070"] [--top 25]
"""
import argparse
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'analysis'))
import profile_stacks  # noqa: E402

GUEST = re.compile(r'^(?:__imp__)?(sub_[0-9A-F]{8})$')
WAITS = ('NtWaitForSingleObject', 'NtWaitForMultipleObjects', 'ZwWaitForAlertByThreadId', 'NtDelayExecution',
         'RtlSleepConditionVariableSRW', 'NtWaitForAlertByThreadId', 'ZwDelayExecution', 'NtYieldExecution',
         'SwitchToThread', 'Sleep')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stacks')
    ap.add_argument('--frames', required=True, help='A-B guest frames')
    ap.add_argument('--thread', default='')
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.frames.split('-'))
    frames = hi - lo + 1
    labels = [a.thread] if a.thread else None
    if labels is None:
        _, per_thread, _ = profile_stacks.load(a.stacks, 'GPU Commands', lo, hi)
        labels = sorted(per_thread)
    for label in labels:
        out, per_thread, wanted = profile_stacks.load(a.stacks, label, lo, hi)
        total = sum(c for _, _, c in out)
        if not total:
            continue
        busy, own, incl = 0, Counter(), Counter()
        for names, _, count in out:
            if any(w in names[0] for w in WAITS):
                continue
            busy += count
            seen = set()
            first = None
            for n in names:
                m = GUEST.match(n)
                if not m:
                    continue
                f = m.group(1)
                if first is None:
                    first = f
                if f not in seen:
                    seen.add(f)
                    incl[f] += count
            own[first or '(host only)'] += count
        print(f'== {", ".join(sorted(wanted))}: frames {lo}-{hi}, samples {total}, busy (not waiting) {busy} '
              f'({100.0 * busy / total:.0f}%); per guest frame at 1 ms sampling: busy {busy / frames:.3f} ms')
        print(f'{"own %busy":>9} {"own ms/f":>8} {"incl %":>7}  function')
        for f, c in own.most_common(a.top):
            print(f'{100.0 * c / busy:9.1f} {c / frames:8.3f} {100.0 * incl.get(f, 0) / busy:7.1f}  {f}')


if __name__ == '__main__':
    main()

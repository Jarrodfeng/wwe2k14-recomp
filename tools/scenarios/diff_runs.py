"""Three-way compare of guest-memory snapshots (--wwe2k14_scenario_observe, src/scenario.h): which words do two
menu-driven runs agree on that a launcher run has differently? Those are the candidates for state the menus set and
a --wwe2k14_launch launch misses. Prints addresses and big-endian word values only.

    py tools\\scenarios\\diff_runs.py MENU_A.bin MENU_B.bin LAUNCH.bin [--base 0x835C0000] [--max N]

--base = the guest address of the snapshot's first byte (launch_entry_D: 0x835C0000; for launch_entry_P give P from
the log line, or 0 for offsets). Words that also differ between the two menu runs (timers, heap addresses, random
state) are counted and left out.
"""
import argparse
import struct
import sys


def words(path):
    d = open(path, 'rb').read()
    return struct.unpack('>%dI' % (len(d) // 4), d[:len(d) // 4 * 4])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('menu_a')
    ap.add_argument('menu_b')
    ap.add_argument('launch')
    ap.add_argument('--base', default='0')
    ap.add_argument('--max', type=int, default=300)
    a = ap.parse_args()
    base = int(a.base, 0)
    wa, wb, wc = words(a.menu_a), words(a.menu_b), words(a.launch)
    n = min(len(wa), len(wb), len(wc))
    noisy = sum(1 for i in range(n) if wa[i] != wb[i])
    hits = [i for i in range(n) if wa[i] == wb[i] and wc[i] != wa[i]]
    print('%d words compared; %d differ between the menu runs (left out); %d candidates (menus agree, launch differs)'
          % (n, noisy, len(hits)))
    # Group consecutive candidate words.
    shown = 0
    run = []
    for i in hits + [None]:
        if run and (i is None or i != run[-1] + 1):
            if shown < a.max:
                addr = base + run[0] * 4
                menu = ' '.join('%08X' % wa[k] for k in run[:6])
                launch = ' '.join('%08X' % wc[k] for k in run[:6])
                more = ' ...' if len(run) > 6 else ''
                print('0x%08X %3d words  menus %s%s   launch %s%s' % (addr, len(run), menu, more, launch, more))
            shown += 1
            run = []
        if i is not None:
            run.append(i)
    if shown > a.max:
        print('... %d more groups (raise --max)' % (shown - a.max))
    return 0


if __name__ == '__main__':
    sys.exit(main())

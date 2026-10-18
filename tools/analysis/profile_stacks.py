"""Inclusive / self time per function, and per source line, from the profiler's call stacks
(--wwe2k14_profile_stacks=true).

    py tools/analysis/profile_stacks.py <logs/stacks_NNN.txt> [--frames A-B] [--thread "GPU Commands"]
                                        [--top 40] [--focus NAME] [--match TEXT] [--lines]

inclusive % = share of samples with the function anywhere on the stack (counted once per sample);
self %      = share of samples with the function as the leaf.
--thread NAME: which sampled thread (default "GPU Commands"; also "main thread", "GPU Submit", ... as set by
              --wwe2k14_profile_threads): the label NAME exactly, else every label starting with NAME
              (case-insensitive); "all" = every thread together. With no match the threads present are listed.
--focus NAME: for samples passing through a function whose name contains NAME, breaks its time down by the
              function it called next (its direct callees), so you can drill into a hot path.
--match TEXT: only list functions whose name contains TEXT.
--lines:      source lines (needs a stacks file written with --wwe2k14_profile_lines=true, the default, and the
              PDBs next to the DLLs). Alone: the hottest lines by self time (the line each sample was executing).
              With --focus NAME: NAME's time per line of its own source. self = executing that line;
              incl = that line plus everything called from it (for a call site, the callee it called most is
              shown).
Frames are the game's guest frame numbers (VdSwap count) at sample time; the GPU thread works ~2-3 frames
behind the game, which doesn't matter over a range of a hundred frames.
"""
import argparse
from collections import Counter


def pick_threads(per_thread, thread):
    if not thread or thread.lower() == 'all':
        return set(per_thread)
    if thread in per_thread:
        return {thread}
    return {t for t in per_thread if t.lower().startswith(thread.lower())}


def load(path, thread, lo, hi):
    """Returns ([(names, source lines or None, count)], samples per thread label in range, chosen labels).
    names / source lines are leaf first; a source line is (file, line number) or None."""
    names, stacks, stack_lines, lines = {}, {}, {}, {}
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
            elif line.startswith('sl '):
                _, sid, ids = line.split(' ', 2)
                stack_lines[int(sid)] = [None if x == '-' else int(x) for x in ids.split(',')]
            elif line.startswith('l '):
                _, lid, number, file = line.split(' ', 3)
                lines[int(lid)] = (file, int(number))
            elif line and line[0].isdigit():
                frame, sid = line.split(' ')[:2]
                frame = int(frame)
                if lo <= frame <= hi:
                    samples[int(sid)] += 1
    per_thread = Counter()
    for sid, count in samples.items():
        per_thread[stacks[sid][0]] += count
    wanted = pick_threads(per_thread, thread)
    out = []
    for sid, count in samples.items():
        label, ids = stacks[sid]
        if label not in wanted:
            continue
        lids = stack_lines.get(sid)
        out.append(([names[i] for i in ids],
                    [lines.get(l) if l is not None else None for l in lids] if lids else None,
                    count))
    return out, per_thread, wanted


def short(name, width=110):
    return name if len(name) <= width else name[:width - 3] + '...'


def where(loc, width=64):
    """'graphics\\vulkan\\command_processor.cpp:7124' (path after the last \\src\\, else its last 3 parts)."""
    if loc is None:
        return '(no line info)'
    file, number = loc
    path = file.replace('/', '\\')
    cut = path.lower().rfind('\\src\\')
    path = path[cut + 5:] if cut >= 0 else '\\'.join(path.split('\\')[-3:])
    text = f'{path}:{number}'
    return text if len(text) <= width else '...' + text[-(width - 3):]


def line_at(locs, i):
    return locs[i] if locs and i < len(locs) else None


def report_lines(data, total, args):
    if not any(locs for _, locs, _ in data):
        print('\nno source lines in this file (needs --wwe2k14_profile_lines=true and the PDBs next to the DLLs)')
        return
    if not args.focus:
        # Hottest lines overall: the line each sample was executing.
        by_line = Counter()
        for names, locs, count in data:
            if args.match and args.match not in names[0]:
                continue
            by_line[(names[0], line_at(locs, 0))] += count
        print(f'\nhottest source lines (self time){" in functions matching " + repr(args.match) if args.match else ""}:')
        print(f'{"self %":>7}  {"line":<64}  function')
        for (name, loc), count in by_line.most_common(args.top):
            print(f'{100 * count / total:7.1f}  {where(loc):<64}  {short(name, 70)}')
        return

    incl, self_, callees, focus_total = Counter(), Counter(), {}, 0
    for names, locs, count in data:
        idx = [i for i, n in enumerate(names) if args.focus in n]
        if not idx:
            continue
        i = idx[-1]  # outermost frame matching --focus, as for the callee breakdown
        focus_total += count
        key = (names[i], line_at(locs, i))
        incl[key] += count
        if i > 0:
            callees.setdefault(key, Counter())[names[i - 1]] += count
        if idx[0] == 0:
            self_[(names[0], line_at(locs, 0))] += count
    functions = {name for name, _ in set(incl) | set(self_)}
    print(f'\n"{args.focus}" by source line: {100 * focus_total / total:.1f}% of samples '
          f'(self {100 * sum(self_.values()) / total:.1f}%); incl = the line and its calls, self = executing it')
    print(f'{"incl %":>7} {"self %":>7} {"% focus":>7}  {"line":<64}  calls (share of the line)')
    keys = sorted(set(incl) | set(self_), key=lambda k: (incl[k], self_[k]), reverse=True)
    for key in keys[:args.top]:
        name, loc = key
        calls = ''
        if key in callees:
            callee, n = callees[key].most_common(1)[0]
            calls = f'{short(callee, 60)} ({100 * n / incl[key]:.0f}%)'
            if len(callees[key]) > 1:
                calls += f' +{len(callees[key]) - 1} more'
        if len(functions) > 1:
            calls = f'[{short(name, 40)}] {calls}'
        print(f'{100 * incl[key] / total:7.1f} {100 * self_[key] / total:7.1f} '
              f'{100 * incl[key] / max(1, focus_total):7.1f}  {where(loc):<64}  {calls}'.rstrip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stacks')
    ap.add_argument('--frames', default='')
    ap.add_argument('--thread', default='GPU Commands')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--focus', default='')
    ap.add_argument('--match', default='')
    ap.add_argument('--lines', action='store_true')
    args = ap.parse_args()
    lo, hi = (int(x) for x in args.frames.split('-')) if args.frames else (0, 1 << 62)

    data, per_thread, wanted = load(args.stacks, args.thread, lo, hi)
    total = sum(c for _, _, c in data)
    if not total:
        if per_thread and not wanted:
            print(f'no thread matching "{args.thread}"; threads with samples in range: '
                  + ', '.join(f'"{t}" ({n})' for t, n in per_thread.most_common()))
        else:
            print('no samples in range')
        return
    inclusive, self_ = Counter(), Counter()
    for names, _, count in data:
        self_[names[0]] += count
        for n in set(names):
            inclusive[n] += count
    print(f'{total} samples ({", ".join(sorted(wanted))}, frames {args.frames or "all"})')
    print(f'\n{"incl %":>7} {"self %":>7}  function')
    shown = 0
    for name, count in inclusive.most_common():
        if args.match and args.match not in name:
            continue
        print(f'{100 * count / total:7.1f} {100 * self_[name] / total:7.1f}  {short(name)}')
        shown += 1
        if shown >= args.top:
            break

    if args.focus:
        callees, focus_total = Counter(), 0
        for names, _, count in data:
            # names is leaf first; find the outermost frame matching --focus, take the frame below it.
            idx = [i for i, n in enumerate(names) if args.focus in n]
            if not idx:
                continue
            i = idx[-1]
            focus_total += count
            callees[names[i - 1] if i > 0 else '(self)'] += count
        print(f'\nunder "{args.focus}": {100 * focus_total / total:.1f}% of samples; direct callees:')
        for name, count in callees.most_common(args.top):
            print(f'{100 * count / total:7.1f}% of all  {100 * count / max(1, focus_total):5.1f}% of focus  '
                  f'{short(name)}')

    if args.lines:
        report_lines(data, total, args)


if __name__ == '__main__':
    main()

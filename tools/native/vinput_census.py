#!/usr/bin/env python3
"""Session 95 (P0 of out\\s95\\android_structural.md): sums a run's [vinput] census lines (--gpu_native_vinput_census=true) and prints the
decision table: DMA draws with reset values, converted draws, the pool's peak, callback calls a window, trivial clamps, and the top shaders
that are not ours. Log only; works on a PC run or a phone report's log.

    py tools\\native\\vinput_census.py LOG [LOG ...] [--from FRAME] [--top N]

--from FRAME drops the windows that end before FRAME (boot and menu), for a scene's own share.
"""
import argparse, collections, re, sys

NUM = r'(\d+)'
LINE = re.compile(r'\[vinput\] census frames (\d+)-(\d+): draws (\d+) \(typed (\d+), not typed (\d+)\); class \{ours (\d+), translated (\d+), ring (\d+) \(ours (\d+)\), crowd (\d+)\}; '
                  r'index \{none (\d+), dma (\d+), converted (\d+), builtin-auto (\d+), builtin-dma (\d+)\}; crowd index \{none (\d+), dma (\d+), converted (\d+), builtin-auto (\d+), builtin-dma (\d+)\}; '
                  r'restart on (\d+), base nonzero (\d+), clamp trivial (\d+); dma asked (\d+): found (\d+), restart on (\d+), reset values (\d+) \(triangle strip (\d+), line strip (\d+)\), '
                  r'identity refused (\d+), refused \{([^}]*)\}; no-index identity refused (\d+); stream refused \{type (\d+), size (\d+), endian (\d+), range (\d+)\}; '
                  r'storage always \{memexport (\d+), ps-fetch (\d+), vs-expand (\d+), line-loop (\d+)\}; pool \{entries (\d+), hits (\d+), converts (\d+) \((\d+) KiB\), marks cpu (\d+) gpu (\d+), '
                  r'callback calls cpu (\d+) gpu (\d+) watch-cpu (\d+), peak bytes (\d+), window bytes (\d+), evictions (\d+), busy steps (\d+)\}')
NAMES = ('first last draws typed not_typed ours translated ring ring_ours crowd none dma converted bauto bdma c_none c_dma c_converted c_bauto c_bdma restart base clamp '
         'asked found dma_restart reset reset_tri reset_line identity refused plain_identity f_type f_size f_endian f_range memexport ps_fetch vs_expand line_loop '
         'entries hits converts convert_kib marks_cpu marks_gpu calls_cpu calls_gpu calls_watch peak window_bytes evictions busy').split()
TOP = re.compile(r'\[vinput\] census frames (\d+)-(\d+): (\d+) shaders not ours, top: (.*)$')
MAX = {'peak', 'window_bytes', 'entries'}


def pct(a, b):
    return '%.2f%%' % (100.0 * a / b) if b else 'n/a'


def read(path, start):
    total = collections.Counter()
    peak_calls = peak_cpu_calls = windows = 0
    refused = collections.Counter()
    top = collections.Counter()
    per_window = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m:
            w = dict(zip(NAMES, m.groups()))
            why = w.pop('refused')
            w = {k: int(v) for k, v in w.items()}
            if w['last'] < start:
                continue
            windows += 1
            for k, v in w.items():
                if k in MAX:
                    total[k] = max(total[k], v)
                elif k not in ('first', 'last'):
                    total[k] += v
            calls = w['calls_cpu'] + w['calls_gpu']
            peak_calls = max(peak_calls, calls)
            peak_cpu_calls = max(peak_cpu_calls, w['calls_watch'])
            if why != 'none':
                for item in why.split(', '):
                    name, n = item.rsplit(' ', 1)
                    refused[name] += int(n)
            per_window.append(w)
            continue
        m = TOP.search(line)
        if m and int(m.group(2)) >= start:
            for item in m.group(4).split(', '):
                h, n = item.split(' ')
                top[h] += int(n)
    return total, peak_calls, peak_cpu_calls, windows, refused, top, per_window


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--from', dest='start', type=int, default=0)
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    for path in a.logs:
        t, peak_calls, peak_watch, windows, refused, top, per_window = read(path, a.start)
        print('== %s: %d windows, %d draws' % (path, windows, t['draws']))
        if not windows:
            continue
        noncrowd = t['draws'] - t['crowd']
        print('  draws: typed %d, not typed %d; ours %d (%s), translated %d (%s), ring %d (%s; ours %d), crowd %d (%s)' % (
            t['typed'], t['not_typed'], t['ours'], pct(t['ours'], t['draws']), t['translated'], pct(t['translated'], t['draws']), t['ring'], pct(t['ring'], t['draws']), t['ring_ours'],
            t['crowd'], pct(t['crowd'], t['draws'])))
        print('  index, non-crowd: none %d, dma %d, converted %d (%s), builtin-auto %d, builtin-dma %d; crowd: dma %d, converted %d, none %d, builtin-auto %d, builtin-dma %d' % (
            t['none'], t['dma'], t['converted'], pct(t['converted'], noncrowd), t['bauto'], t['bdma'], t['c_dma'], t['c_converted'], t['c_none'], t['c_bauto'], t['c_bdma']))
        print('  restart on %d (%s of non-crowd), base nonzero %d (%s), clamp trivial %d (%s)' % (t['restart'], pct(t['restart'], noncrowd), t['base'], pct(t['base'], noncrowd), t['clamp'], pct(t['clamp'], noncrowd)))
        print('  dma asked %d, found %d (refused %s), restart on %d; WITH RESET VALUES %d (%s of asked; triangle strip %d, line strip %d); identity refused %d (%s)' % (
            t['asked'], t['found'], dict(refused) or 'none', t['dma_restart'], t['reset'], pct(t['reset'], t['asked']), t['reset_tri'], t['reset_line'], t['identity'], pct(t['identity'], t['asked'])))
        print('  no-index identity refused %d; stream refused: type %d, size %d, endian %d, range %d; storage always: memexport %d, ps-fetch %d, vs-expand %d, line-loop %d' % (
            t['plain_identity'], t['f_type'], t['f_size'], t['f_endian'], t['f_range'], t['memexport'], t['ps_fetch'], t['vs_expand'], t['line_loop']))
        print('  pool: most entries %d, hits %d, converts %d (%d KiB), marks cpu %d gpu %d, evictions %d, busy steps %d; PEAK %.2f MiB, most used in a window %.2f MiB' % (
            t['entries'], t['hits'], t['converts'], t['convert_kib'], t['marks_cpu'], t['marks_gpu'], t['evictions'], t['busy'], t['peak'] / 1048576.0, t['window_bytes'] / 1048576.0))
        print('  callback calls: most in a window %d (cpu + gpu), watch by cpu writes most in a window %d; totals cpu %d, gpu %d, watch-cpu %d' % (
            peak_calls, peak_watch, t['calls_cpu'], t['calls_gpu'], t['calls_watch']))
        print('  top not ours (a share of the draws not ours, %d in all): %s' % (t['translated'] + t['ring'] - t['ring_ours'], ', '.join('%s %d' % kv for kv in top.most_common(a.top))))


if __name__ == '__main__':
    main()

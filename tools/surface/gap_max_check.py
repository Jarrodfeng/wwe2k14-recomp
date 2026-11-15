#!/usr/bin/env python3
"""S102 R25-C2 piece A: the per-row max present gap (frames csv column `present_gap_max_ms`), offline.

  py -I tools\\surface\\gap_max_check.py [--root .]

1. bench_line on synthetic csvs: a row with two presents keeps its larger gap in `present_gap_max_ms`; the headline `low1` (the last gap of a row) is unchanged,
   `low1_max` (the worst-gap-per-row figure) is lower where a 2-present row hid a long first gap. A csv WITHOUT the column (every csv before R25-C2) gives
   low1_max None and the same low1 as before. CSV_ORDER is accepted by check_header with the new column in the base part.
2. The wiring, read as text: NotePresent raises rdr::g_present_gap_max_us with a compare-exchange loop; the row writer takes it with exchange(0) EVERY row (a 0-present
   row too: no stale gap leaks), writes it after device_lost and before the fs_ columns, header literal and row format agree.
3. Planted twins on copies (each must be flagged, 'DID NOT APPLY' fails when the plant no longer matches): the CAS loop replaced by a plain store (the row would keep the
   LAST gap, the bug this column exists to fix), the exchange(0) made a load (stale gaps leak), the column written in front of device_lost.
A check that cannot read its input fails loudly."""
import argparse, os, re, sys

p = argparse.ArgumentParser()
p.add_argument('--root', default='.')
a = p.parse_args()
root = os.path.abspath(a.root)
sys.path.insert(0, os.path.join(root, 'tools', 'surface'))
import bench_line


class Fail(Exception):
    pass


def read(rel):
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        raise Fail('cannot read ' + rel)
    return open(path, encoding='utf-8', newline='').read()


def synth_rows(with_max):
    rows = []
    for i in range(300):
        r = {c: '' for c in bench_line.CSV_ORDER if not c.startswith('fs_')}
        r.update({'frame': str(1000 + i), 't_seconds': '%.3f' % (i * 0.0167), 'frame_ms': '16.7', 'gpu_ms': '12.0', 'presented': '1', 'present_gap_ms': '16.7',
                  'render_thread_ms': '10.0', 'scene': 'match', 'device_lost': '0'})
        if with_max:
            r['present_gap_max_ms'] = '16.7'
        rows.append(r)
    # every 20th row presents twice: its LAST gap is short (6 ms), its FIRST (hidden) gap long (27 ms)
    for i in range(10, 300, 20):
        rows[i]['presented'] = '2'
        rows[i]['present_gap_ms'] = '6.0'
        if with_max:
            rows[i]['present_gap_max_ms'] = '27.0'
    if not with_max:
        for r in rows:
            r.pop('present_gap_max_ms', None)
    return rows


def part1():
    new = bench_line.window_numbers(synth_rows(True), 1000, 1299)
    old = bench_line.window_numbers(synth_rows(False), 1000, 1299)
    if old.get('low1_max') is not None:
        raise Fail('a csv without present_gap_max_ms must give low1_max None, got %r' % old.get('low1_max'))
    if abs(new['low1'] - old['low1']) > 1e-9:
        raise Fail('the headline low1 changed with the column present: %.3f vs %.3f' % (new['low1'], old['low1']))
    if new['low1_max'] is None or not (new['low1_max'] < new['low1'] - 5):
        raise Fail('low1_max must show the hidden long gaps (27 ms -> 37 fps) below the headline: low1 %.1f low1_max %s' % (new['low1'], new['low1_max']))
    print('ok   bench_line: low1 %.1f unchanged, low1_max %.1f sees the 2-present rows\' hidden 27 ms gaps; an old csv (no column) gives low1_max None' % (new['low1'], new['low1_max']))
    bench_line.check_header(bench_line.CSV_ORDER, 'CSV_ORDER')
    i = bench_line.CSV_ORDER.index('present_gap_max_ms')
    if i != bench_line.CSV_ORDER.index('device_lost') + 1 or bench_line.CSV_ORDER[i + 1] != 'fs_reader_draw_ms':
        raise Fail('present_gap_max_ms must sit after device_lost and before the fs_ columns')
    print('ok   CSV_ORDER: present_gap_max_ms after device_lost, before the fs_ columns; check_header accepts it')


def wiring_problems(stats, live, overlay):
    pr = []
    if 'g_present_gap_max_us' not in overlay:
        pr.append('overlay.h: no rdr::g_present_gap_max_us')
    if not re.search(r'compare_exchange_weak\(seen, gap_us', live) or 'g_present_gap_max_us' not in live:
        pr.append('live.cpp NotePresent: the max is not raised with a compare-exchange loop (a plain store would keep the LAST gap)')
    if 'g_present_gap_max_us.exchange(0' not in stats:
        pr.append('frame_stats.h: the row writer does not take-and-clear the max with exchange(0)')
    if not re.search(r'"device_lost,present_gap_max_ms"', stats):
        pr.append('frame_stats.h: the header literal lacks device_lost,present_gap_max_ms in that order')
    m = re.search(r'std::fprintf\(csv_, "(%llu[^"]*)"', stats)
    if not m or len(re.findall(r'%[-+ #0-9.]*(?:ll)?[a-zA-Z]', m.group(1))) != len(bench_line.CSV_ORDER) - 7 + 1 or not m.group(1).endswith('%s%s\\n'):
        pr.append('frame_stats.h: the row format is not the base conversions (incl. the new column) + the gap-max cell + the split cells')
    if not re.search(r'lost_now \? 1 : 0, gapmax_text, split_text\)', stats):
        pr.append('frame_stats.h: gapmax_text is not written between device_lost and split_text')
    return pr


def part2():
    stats, live, overlay = read('src/frame_stats.h'), read('src/renderer_adapter/live.cpp'), read('src/renderer/overlay.h')
    pr = wiring_problems(stats, live, overlay)
    if pr:
        raise Fail('wiring: ' + '; '.join(pr))
    print('ok   wiring: CAS max in NotePresent, exchange(0) every row, column after device_lost, header and row format agree')
    plants = [
        ('plain store instead of the compare-exchange loop (last gap wins)', 'live', live,
         'for (uint32_t seen = rdr::g_present_gap_max_us.load(std::memory_order_relaxed); gap_us > seen && !rdr::g_present_gap_max_us.compare_exchange_weak(seen, gap_us, std::memory_order_relaxed);) {}',
         'rdr::g_present_gap_max_us.store(gap_us, std::memory_order_relaxed);'),
        ('exchange(0) made a load (a stale gap leaks into later rows)', 'stats', stats, 'g_present_gap_max_us.exchange(0, std::memory_order_relaxed)', 'g_present_gap_max_us.load(std::memory_order_relaxed)'),
        ('column written in front of device_lost', 'stats', stats, '"device_lost,present_gap_max_ms"', '"present_gap_max_ms,device_lost"'),
    ]
    for name, which, text, old, new in plants:
        planted = text.replace(old, new)
        if planted == text:
            raise Fail('plant %r: DID NOT APPLY (the code changed)' % name)
        pr = wiring_problems(planted if which == 'stats' else stats, planted if which == 'live' else live, overlay)
        if not pr:
            raise Fail('plant %r: NOT caught' % name)
        print('ok   plant caught: %s' % name)


try:
    part1()
    part2()
except Fail as e:
    print('FAIL', e)
    sys.exit(1)
print('PASS: present_gap_max_ms (writer wiring, bench_line second figure, old csvs unchanged); 3 planted twins caught')

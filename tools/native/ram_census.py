"""Session 49 (the RAM ties): sum a run's gpu_ram_stats census by phase of the s49_ram_census route.

Usage: py tools\native\ram_census.py RUN [RUN...]

Phases come from the route's "census: ..." log lines (menus, entrances, match); every census line is put in the
phase its timestamp falls in. Prints per phase the means of:
  [native][ram]    resolve write-backs per frame (staged / direct, KB), GPU ms of write-backs / fills / other stores,
                   the fill refusals by reason
  [s49][ram]       requests overlapping resolve-written pages by caller, pages dropped by CPU writes / uploads
  [s49][faults]    write-watch faults per frame, handler us per frame, threads, faulting code
  [s7][UPSKIP]     pages uploaded / skipped by hash per frame
  [native][truth]  truth stores GPU ms (for reference)
"""
import collections
import glob
import os
import re
import sys

LOG_DIR = r'out\build\win-amd64-release\logs'
TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')
NUM = r'([0-9.]+)'


def phases_of(lines):
    marks = []
    for line in lines:
        m = TS.match(line)
        if m and 'census: ' in line:
            marks.append((m.group(1), line.split('census: ', 1)[1].strip()))
    return marks


def phase_at(marks, ts):
    name = 'before'
    for t, label in marks:
        if ts >= t:
            name = label
    if name.startswith('main menu'):
        return 'menus'
    if name.startswith('match launched'):
        return 'loading'
    if name.startswith('entrances'):
        return 'entrances'
    if name.startswith('match clock'):
        return 'match'
    return name.split(' ')[0]


def main():
    for run in sys.argv[1:]:
        path = os.path.join(LOG_DIR, f'wwe2k14_{run}.log')
        lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
        marks = phases_of(lines)
        data = collections.defaultdict(lambda: collections.defaultdict(list))
        texts = collections.defaultdict(lambda: collections.defaultdict(list))
        for line in lines:
            m = TS.match(line)
            if not m:
                continue
            ph = phase_at(marks, m.group(1))
            d = data[ph]
            if '[native][ram] frames' in line:
                r = re.search(r'staged ' + NUM + r' \(' + NUM + r' KB\), direct ' + NUM + r' \(' + NUM +
                              r' KB\); GPU ms per replaced frame: write-backs ' + NUM + r', texture fills ' + NUM +
                              r' \(truth stores outside them ' + NUM + r'\); untimed segments (\d+)', line)
                if r:
                    for k, v in zip(['wb_staged', 'wb_staged_kb', 'wb_direct', 'wb_direct_kb', 'gpu_wb', 'gpu_fill',
                                     'gpu_stores_other', 'untimed'], r.groups()):
                        d[k].append(float(v))
                texts[ph]['refusals'].append(line.split('fills refused by reason: ', 1)[-1])
            elif '[s49][ram] frames' in line:
                r = re.search(r'resolves noted per frame ' + NUM + r' \(' + NUM + r' pages\)', line)
                if r:
                    d['noted'].append(float(r.group(1)))
                    d['noted_pages'].append(float(r.group(2)))
                for name, a, b, p in re.findall(r'(other|vertex|index|texture|memexport|resolve dest) ' + NUM +
                                                r' of ' + NUM + r' \(' + NUM + r' pages\)', line):
                    d[f'read_{name}'].append(float(a))
                    d[f'req_{name}'].append(float(b))
                    d[f'read_pages_{name}'].append(float(p))
                r = re.search(r'by CPU writes ' + NUM + r' in ' + NUM + r' faults, by uploads ' + NUM +
                              r', by memexport ' + NUM, line)
                if r:
                    for k, v in zip(['cpu_pages', 'cpu_faults', 'upload_pages', 'memexport_pages'], r.groups()):
                        d[k].append(float(v))
            elif '[s49][faults] frames' in line:
                r = re.search(r'write-watch faults ' + NUM + r'/frame \((\d+) raced\), handler ' + NUM +
                              r' us/frame \(mean ' + NUM + r' us, max ' + NUM + r' us\)', line)
                if r:
                    for k, v in zip(['faults', 'raced', 'fault_us', 'fault_mean_us', 'fault_max_us'], r.groups()):
                        d[k].append(float(v))
                texts[ph]['fault_code'].append(line.split('faulting code: ', 1)[-1])
                texts[ph]['fault_threads'].append(line.split('threads: ', 1)[-1].split(';')[0])
            elif '[s7][UPSKIP] frames' in line:
                r = re.search(r'pages uploaded \(' + NUM + r'/frame\), \d+ pages skipped \(' + NUM + r'/frame\)', line)
                if r:
                    d['up_pages'].append(float(r.group(1)))
                    d['skip_pages'].append(float(r.group(2)))
            elif '[native][wb] frames' in line:
                r = re.search(r'texture-only resolves per frame ' + NUM + r' \(' + NUM + r' KB; direct ' + NUM +
                              r'\), replaced by a later resolve ' + NUM + r', written from their textures ' + NUM +
                              r' \(' + NUM + r' KB', line)
                if r:
                    for k, v in zip(['wb_skip', 'wb_skip_kb', 'wb_skip_direct', 'wb_replaced', 'wb_mat', 'wb_mat_kb'],
                                    r.groups()):
                        d[k].append(float(v))
                for name, v in re.findall(r'(other|vertex|index|texture|memexport|resolve over part|age|skipping '
                                          r'stopped) ' + NUM + r'[,)]', line.split('KB: ', 1)[-1].split(';')[0]):
                    d['mat_' + name.replace(' ', '_')].append(float(v))
                kept = line.split('write-backs kept per frame: ', 1)[-1]
                for name, v in re.findall(r'(no texture|not readable|not inside|not a plain fill) ' + NUM, kept):
                    d['kept_' + name.replace(' ', '_')].append(float(v))
            elif '[native][truth] frames' in line:
                r = re.search(r'truth stores ' + NUM, line)
                if r:
                    d['truth_stores'].append(float(r.group(1)))
        print(f'== run {run}: phases {[label for _, label in marks]}')
        for ph in ['menus', 'loading', 'entrances', 'match']:
            if ph not in data:
                continue
            d = data[ph]
            mean = {k: sum(v) / len(v) for k, v in d.items() if v}
            print(f'-- {ph}: ' + ', '.join(f'{k} {v:.3g}' for k, v in sorted(mean.items())))
            for k, v in texts[ph].items():
                c = collections.Counter(v)
                print(f'   {k}: {c.most_common(2)}')


if __name__ == '__main__':
    main()

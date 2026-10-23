"""Session 43 (stretch S1): per run, how the native paths held - launches, native errors, what the last report lines
say (replaced frames, scene draws native, post draws native and what was left emulated), and the run's end.

    py tools\\native\\sweep_report.py RUN [RUN ...]
"""
import os
import re
import sys

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'build', 'win-amd64-release',
                    'logs')
PROBLEMS = re.compile(r'\[native\].*(lost|failed|could not|no write-back|no load|not replacing|after the last resolve|'
                      r'differs)', re.I)


def last_num(pattern, text, default='-'):
    m = re.findall(pattern, text)
    return m[-1] if m else default


def main():
    for run in sys.argv[1:]:
        path = os.path.join(LOGS, f'wwe2k14_{int(run):03d}.log')
        if not os.path.exists(path):
            print(f'{run}: no log')
            continue
        problems, native_lines, post_lines, launches, flags = [], [], [], [], ''
        end = ''
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'launch' in line and '[scenario]' in line:
                    launches.append(line.strip()[-120:])
                if '[native] frames' in line:
                    native_lines.append(line)
                elif '[native][post] frames' in line:
                    post_lines.append(line)
                elif PROBLEMS.search(line):
                    problems.append(line.strip()[-200:])
                if 'command line' in line.lower() and not flags:
                    flags = line.strip()[-300:]
                end = line
        print(f'== run {run}: {len(native_lines)} native reports, {len(post_lines)} post reports, '
              f'{len(problems)} problem lines, {len(launches)} launch lines')
        if native_lines:
            nl = native_lines[-1]
            print('   last [native]: replaced', last_num(r'\((\d+) read, (\d+) replaced\)', nl),
                  '| scene draws', last_num(r'scene draws (\d+)/frame', nl), 'native', last_num(r'native (\d+)/frame', nl),
                  '| passes', last_num(r'passes ([\d.]+), loads', nl))
        if post_lines:
            pl = post_lines[-1]
            m = re.search(r'post draws native ([\d.]+)/frame \(depth ([\d.]+)\)', pl)
            left = re.search(r'left emulated per frame: (.*); targets', pl)
            nonzero = ', '.join(x for x in (left.group(1).split(', ') if left else []) if not x.endswith(' 0.0'))
            print(f'   last [native][post]: post draws {m.group(1) if m else "-"} (depth {m.group(2) if m else "-"}); '
                  f'left emulated: {nonzero or "none"}')
        for p in problems[:8]:
            print('   problem:', p)
        print('   end:', end.strip()[-160:])


if __name__ == '__main__':
    main()

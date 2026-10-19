"""Session 32: results of DLC wrestler runs (tools\\input_scripts\\short\\dlc_pair.txt / dlc_six.txt).

    py tools\\dlc\\sweep_results.py RUN [RUN ...] [--logs DIR]

Per run: the participants, each DLC character archive (pac\\ch\\chNNN_dlcPP.pac) and titantron the game opened (and
from which dlcN: root), whether the match scene loaded (M3.PAC), how far the route got, the screenshots, and any
crash line. Reads the run logs only.
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')

OPEN = re.compile(r"\[file\] NtCreateFile '([^']+)'.*-> 0x00000000")


def one(run, logs):
    path = os.path.join(logs, 'wwe2k14_%d.log' % run)
    if not os.path.exists(path):
        return '%d: no log' % run
    parts, chars, trons, m3, last, shots, crash = '', {}, {}, False, '', [], ''
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'modified: wwe2k14_launch_participants' in line:
                parts = line.split("= '", 1)[1].split("'", 1)[0]
            m = OPEN.search(line)
            if m:
                p = m.group(1).lower()
                cm = re.match(r'(dlc\d+):\\pac\\ch\\ch(\d+)_dlc(\d+)\.pac', p)
                if cm:
                    chars.setdefault(int(cm.group(2)), '%s pack %d' % (cm.group(1), int(cm.group(3))))
                tm = re.match(r'(dlc\d+|game):\\movies\\titantron\\(\d+)\.bik2', p)
                if tm:
                    trons.setdefault(int(tm.group(2)), tm.group(1))
                if p.endswith('\\m3.pac'):
                    m3 = True
            if '[wwe2k14][input]' in line and ('pair: ' in line or 'six: ' in line):
                last = line.split('[wwe2k14][input] ', 1)[1].strip()
            if 'screenshot saved:' in line:
                shots.append(os.path.basename(line.strip().split('screenshot saved: ', 1)[1].split(' (')[0]))
            if '[crash]' in line or 'script FAILED' in line:
                crash = crash or line.strip()[-160:]
    out = ['%d: participants %s, match scene %s' % (run, parts or '?', 'yes' if m3 else 'NO')]
    out.append('   DLC archives: ' + (', '.join('ch%d (%s)' % kv for kv in sorted(chars.items())) or 'none'))
    out.append('   titantrons: ' + (', '.join('%d (%s)' % kv for kv in sorted(trons.items())) or 'none'))
    out.append('   last step: ' + last)
    out.append('   shots: ' + ', '.join(shots))
    if crash:
        out.append('   !! ' + crash)
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('runs', nargs='+', type=int)
    ap.add_argument('--logs', default=LOGS)
    a = ap.parse_args()
    for r in a.runs:
        print(one(r, a.logs))
    return 0


if __name__ == '__main__':
    sys.exit(main())

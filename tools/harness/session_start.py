"""Session 29: the start-of-session state check in one command (memory "rebuild state after a rewind").

  py tools/harness/session_start.py

Prints, compactly: the time; git branch, uncommitted files, the last commits and branches not merged into main; the
run lock and any running game; the newest run of each build with its times and what it checked; the newest START
HERE heading of PLAYBOOK.md; free disk space. Read-only (git, files, runlock.ps1 status).
"""
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def run(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, encoding='utf-8', errors='replace',
                           timeout=timeout)
        return p.stdout.rstrip()  # rstrip: git status lines start with a status column
    except (OSError, subprocess.TimeoutExpired) as e:
        return '(%s)' % e


def newest_log(build):
    d = os.path.join(REPO, 'out', 'build', build, 'logs')
    best, num = None, -1
    try:
        for name in os.listdir(d):
            m = re.fullmatch(r'wwe2k14_(\d+)\.log', name)
            if m and int(m.group(1)) > num:
                best, num = os.path.join(d, name), int(m.group(1))
    except OSError:
        pass
    return best, num


def log_times(path):
    stamp = re.compile(r'^\[\d{4}-(\d\d-\d\d \d\d:\d\d:\d\d)')
    first = last = None
    with open(path, 'rb') as f:
        head = f.readline().decode('utf-8', 'replace')
        f.seek(max(0, os.path.getsize(path) - 4096))
        tail = f.read().decode('utf-8', 'replace').splitlines()
    m = stamp.match(head)
    first = m.group(1) if m else '?'
    for line in reversed(tail):
        m = stamp.match(line)
        if m:
            last = m.group(1)
            break
    return first, last or '?'


def main():
    print('== session start check, %s' % time.strftime('%Y-%m-%d %H:%M'))
    branch = run(['git', 'branch', '--show-current'])
    dirty = [l for l in run(['git', 'status', '--short']).splitlines() if l.strip()]
    print('git: branch %s, %d uncommitted file(s)%s' % (branch, len(dirty), (': ' + ', '.join(l[3:] for l in dirty[:6])) if dirty else ''))
    for line in run(['git', 'log', '--format=%h %ad %s', '--date=format:%m-%d %H:%M', '-3']).splitlines():
        print('  ' + line[:150])
    unmerged = run(['git', 'branch', '--no-merged', 'main', '--sort=-committerdate',
                    '--format=%(refname:short) (%(committerdate:format:%m-%d %H:%M))']).splitlines()
    if unmerged:
        print('branches not merged into main (newest first): ' + ', '.join(u.strip() for u in unmerged[:5]) +
              (' ... (%d)' % len(unmerged) if len(unmerged) > 5 else ''))
    lock = run(['pwsh', '-NoProfile', '-File', os.path.join(REPO, 'tools', 'runlock.ps1'), 'status'])
    keep = [l.strip() for l in lock.splitlines() if re.match(r'\s*(mode|lock|slot\d|queue|game processes)', l)]
    print('run lock: ' + ' | '.join(k for k in keep if 'free' not in k or k.startswith(('mode', 'lock'))))
    for build in ('win-amd64-release', 'win-amd64-coverage'):
        path, num = newest_log(build)
        if not path:
            continue
        first, last = log_times(path)
        checks = run([sys.executable, os.path.join(REPO, 'tools', 'harness', 'run_verdict.py'), str(num),
                      '--exe-dir', os.path.join(REPO, 'out', 'build', build), '--no-sheet'])
        print('newest run (%s): %03d, %s -> %s; %s' % (build.replace('win-amd64-', ''), num, first, last[6:], checks))
    try:
        with open(os.path.join(REPO, 'PLAYBOOK.md'), encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('## START HERE'):
                    print('PLAYBOOK: ' + line.strip()[3:])
                    break
    except OSError:
        pass
    free = shutil.disk_usage(REPO).free / 2**30
    print('disk: %.0f GB free on %s' % (free, os.path.splitdrive(REPO)[0] or '/'))
    print('MCP: %s' % ('wwe2k14-live in .mcp.json' if os.path.exists(os.path.join(REPO, '.mcp.json')) else 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main())

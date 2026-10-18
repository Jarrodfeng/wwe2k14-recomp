"""Session 29: follow a running game's log and print only what needs attention, one line each, as it happens.

  py tools/live/watch.py [--name main] [--run NNN] [--all-errors]

For the Monitor tool while Jarrod plays (each printed line is a notification): bookmarks (his F11 or an automatic one),
on-screen check answers, script failures, crashes and GPU device losses, flight-recorder dumps, scan / trace summaries,
and new warning / error lines (each distinct text at most once a minute, known noise skipped unless --all-errors).
Finds the run through the live channel's !status (or --run), in the release or the coverage build's logs folder.
Ends when the game exits (its log says so, or its process is gone; the live channel is not polled: it serves
one client at a time, and a long batch must not look like a closed game).
"""
import argparse
import glob
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live  # noqa: E402

REPO = live.REPO
LOG_DIRS = [os.path.join(REPO, 'out', 'build', p, 'logs') for p in ('win-amd64-release', 'win-amd64-coverage')]
ALWAYS = re.compile(r'\[wwe2k14\]\[(bookmark|check|crash|scan|live)\]|script FAILED|device lost|Failed to await|'
                    r'flight recorder dump|\[wwe2k14\]\[trace\] .*(callers over|done)')
BAD = re.compile(r'\] \[(warning|error|critical)\] ')
NOISE = re.compile(r'NtQueryInformationFile\(XFileXctdCompressionInformation\)|NtCreateFile\] FAILED: path=.cache:|'
                   r'gamecontrollerdb|Vulkan draw resolution scaling|BaseHeap::Alloc failed to find contiguous|'
                   r'\[wwe2k14\]\[live\] batch \d+(:| done)')
EXIT = re.compile(r'Title terminated|\[wwe2k14\]\[input\] .*quit|ReXApp::OnClosing|exiting')


def pid_alive(pid):
    import ctypes
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = k.GetExitCodeProcess(h, ctypes.byref(code))
    k.CloseHandle(h)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


def find_log(run):
    for d in LOG_DIRS:
        p = os.path.join(d, 'wwe2k14_%03d.log' % run)
        if os.path.exists(p):
            return p
    cands = [p for d in LOG_DIRS for p in glob.glob(os.path.join(d, 'wwe2k14_%d.log' % run))]
    return cands[0] if cands else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', default='main')
    ap.add_argument('--run', type=int)
    ap.add_argument('--all-errors', action='store_true')
    a = ap.parse_args()
    run = a.run
    if run is None:
        try:
            fields, _ = live.parse(live.request(a.name, '!status', connect_wait=30))
            run = int(fields['run'])
        except (live.NoGame, KeyError, ValueError):
            print('watch: no game answers on the live channel %s' % a.name, flush=True)
            return 2
    path = find_log(run)
    if not path:
        print('watch: no log for run %03d' % run, flush=True)
        return 2
    pid = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'run log number: \d+ \(pid (\d+)\)', line)
            if m:
                pid = int(m.group(1))
                break
    print('watch: following %s (game pid %s)' % (path, pid), flush=True)
    seen = {}
    last_ping = time.time()
    with open(path, encoding='utf-8', errors='replace') as f:
        f.seek(0, os.SEEK_END)
        buf = ''
        while True:
            chunk = f.read()
            if chunk:
                buf += chunk
                *lines, buf = buf.split('\n')
                for line in lines:
                    s = live.short(line.rstrip('\r'))
                    if ALWAYS.search(line) and not NOISE.search(line):
                        print(s[:400], flush=True)
                    elif BAD.search(line) and (a.all_errors or not NOISE.search(line)):
                        key = s[11:]
                        now = time.time()
                        if now - seen.get(key, 0) >= 60:
                            seen[key] = now
                            print(s[:400], flush=True)
                    if EXIT.search(line) and 'Title terminated' in line:
                        print('watch: the game exited', flush=True)
                        return 0
                continue
            time.sleep(0.25)
            if time.time() - last_ping > 5:
                last_ping = time.time()
                if pid and not pid_alive(pid):
                    print('watch: the game exited (pid %d)' % pid, flush=True)
                    return 0


if __name__ == '__main__':
    sys.exit(main())

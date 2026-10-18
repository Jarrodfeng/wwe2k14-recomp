"""Session 29: client for the game's live control channel (--wwe2k14_live=NAME, src/live_control.h).

One game stays open; each `run` sends a batch of input-script lines (every command of src/input_script.h: press, set,
wait_state, log_state, cvar, screenshot, check, ...) and prints the answer when the batch has ended.

  py tools/live/live.py start [--name main] [--script menu|idle|FILE] [--launch SCENARIO] [--fast] [--extra "FLAGS"]
        [--window behind|visible|windowed|foreground]
        boots the game through tools/bench/run_list.ps1 (frozen user data, run lock, a test settings file under
        out/live/), in the background; returns once the channel answers. Normal speed unless --fast (Jarrod watches).
        Session 30: the window is fullscreen but shown without taking the focus (behind = behind other windows, the
        default; visible = on top; windowed = a 1280x720 window, on top; foreground = the old activated window). A
        `check` brings it forward and flashes its taskbar button; Jarrod clicks it (or its taskbar button) to play.
  py tools/live/live.py run "press A" "wait_frames 30" "log_state menu" [--shot] [--timeout S] [--log ours|files|all|none]
  py tools/live/live.py run -f FILE        (or '-' for stdin)
  py tools/live/live.py goto NAME          Session 30: a navigation snippet (tools/input_scripts/nav/NAME.txt, e.g.
        menu_to_options; its header says where it starts and ends) = a batch with `include NAME`
  py tools/live/live.py shot [LABEL] [--width 1280]      screenshot + a small JPEG copy under out/live/ to look at
  py tools/live/live.py check "Is the GRAPHICS item after GAMEPLAY?"   on-screen question, Jarrod answers F1/F2/F5
  py tools/live/live.py bookmark      the F11 bundle now (screenshot, states, last 5 s of frame times, recent warnings),
        e.g. when Jarrod says "that, just now"; Jarrod's own F11 presses land in captures/NNN_bookmark_K.txt
  py tools/live/live.py status | stop | quit     (status: run, frame, fps now, menu / scene states, text, bookmarks)
  py tools/live/live.py start --coverage ...   Session 30: the coverage build with recording on (--wwe2k14_coverage=true)
  py tools/live/live.py window LABEL     close the coverage window that ran since the last one, under LABEL (a `mark`)
  py tools/live/live.py diffcov B [A]    functions that ran in window B but not in A (tools/coverage/diffcov.py; rarest
        first), e.g. window idle / press A / window press_a / diffcov press_a idle; then --wwe2k14_trace the top ones
  py tools/live/live.py struct NAME [ADDR]   Session 30: every field of a known object (tools/input_scripts/structs.toml)
  py tools/live/live.py text      Session 30: the text on screen now (what the game fetched through its text manager)
        and the 40 newest distinct strings (src/screen_text.h)
  py tools/live/live.py save-route NAME [--run NNN] [--from B] [--to B] [--drop-inspect] [--keep-failed] [--force]
        Session 30: the live session as a route: the batches of run NNN (default: the live game's run, else the newest
        run that has batches) as sent, in order, into tools/input_scripts/short/NAME.txt, after the start script's part
        (live/menu.txt -> `include boot_to_menu`; another --script -> `include` of it), with `on_fail quit` and a final
        `quit`. Failed / timed-out batches are left out (--keep-failed keeps them); --drop-inspect also leaves out
        inspection-only lines (log_state, scan_*, watch, snap, screenshot, check, mark, log; assert_state stays). Comments name the run, batch and time of each part. Needs a game
        built in Session 30 or later (the run log keeps each batch's lines: "[live] batch N> LINE").

Output: the status line, the frames the batch took, the batch's log lines (ours: [wwe2k14] lines without file opens,
plus warnings and errors; timestamps cut to HH:MM:SS) and, per screenshot, the PNG path and the small copy.
Exit code 0 = ok, 1 = failed / timeout / busy / error, 2 = no game answering.
"""
import argparse
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')
OUT = os.path.join(REPO, 'out', 'live')
RUN_LIST = os.path.join(REPO, 'tools', 'bench', 'run_list.ps1')
SCRIPTS = {'menu': os.path.join(REPO, 'tools', 'input_scripts', 'live', 'menu.txt'),
           'idle': os.path.join(REPO, 'tools', 'input_scripts', 'live', 'idle.txt')}

PREFIX = re.compile(r'^\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)\.\d+\] \[(\w)\w*\] \[\w+\] \[t\d+\] ')
SHOT = re.compile(r'screenshot saved: (.+?) \((\d+)x(\d+)\)')


class NoGame(Exception):
    pass


def pipe_path(name):
    return r'\\.\pipe\wwe2k14_live_' + name


def request(name, text, connect_wait=10.0):
    """Sends one request (text lines; '.' terminator added) and returns the raw answer text."""
    path = pipe_path(name)
    deadline = time.time() + connect_wait
    while True:
        try:
            f = open(path, 'r+b', buffering=0)
            break
        except OSError:
            # Not there yet (booting, or between two clients) or busy with another client.
            if time.time() >= deadline:
                raise NoGame('no game answers on %s' % path)
            time.sleep(0.05)
    with f:
        body = text if text.endswith('\n') else text + '\n'
        f.write((body + '.\n').encode('utf-8'))
        chunks = []
        while True:
            try:
                data = f.read(65536)
            except OSError:  # the game closed its end (answer complete, or the game quit)
                break
            if not data:
                break
            chunks.append(data)
            if b''.join(chunks[-2:]).endswith(b'\n.\n'):
                break
    answer = b''.join(chunks).decode('utf-8', errors='replace').replace('\r', '')
    if answer.endswith('\n.\n'):
        answer = answer[:-2]
    return answer


def parse(answer):
    """-> (fields dict, log lines list)."""
    fields, log = {}, []
    lines = answer.split('\n')
    for i, line in enumerate(lines):
        if line == 'log:':
            log = [x for x in lines[i + 1:] if x]
            break
        if ': ' in line:
            k, v = line.split(': ', 1)
            fields[k] = v
    return fields, log


def short(line):
    m = PREFIX.match(line)
    return ('%s %s %s' % (m.group(1), m.group(2).upper(), line[m.end():])) if m else line


def filter_log(log, mode='ours'):
    if mode == 'none':
        return []
    if mode == 'all':
        return [short(x) for x in log]
    out, seen = [], {}
    for line in log:
        ours = '[wwe2k14]' in line
        bad = re.search(r'\] \[(warning|error|critical)\] ', line) is not None
        # Session 30: the batch's own echo ("batch N> LINE") and result line are for save-route, not for the answer.
        if mode == 'ours' and re.search(r'\[wwe2k14\]\[live\] batch \d+(> | result: )', line):
            continue
        if ours and ('[wwe2k14][frames]' in line or (mode == 'ours' and '[wwe2k14][file]' in line)):
            if not bad:
                continue
        if not (ours or bad):
            continue
        s = short(line)
        if bad and not ours:  # SDK warnings repeat a lot (one line each, the count at its first place)
            key = s[11:]
            if key in seen:
                seen[key][1] += 1
                continue
            seen[key] = [len(out), 1]
        out.append(s)
    for i, n in seen.values():
        if n > 1:
            out[i] += '   (x%d in this batch)' % n
    return out


def screenshots(log):
    return [m.group(1) for m in (SHOT.search(x) for x in log) if m]


def thumbnail(png, width=1280):
    from PIL import Image
    os.makedirs(OUT, exist_ok=True)
    im = Image.open(png).convert('RGB')
    if im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    dst = os.path.join(OUT, os.path.splitext(os.path.basename(png))[0] + '.jpg')
    im.save(dst, 'JPEG', quality=88)
    return dst


def run_batch(name, lines, timeout=120, shot=False, width=1280):
    """Runs one batch. -> (fields, log, shots [(png, jpg)])."""
    lines = list(lines)
    if shot:
        lines.append('screenshot live_%s' % time.strftime('%H%M%S'))
    text = '!timeout %d\n' % timeout + '\n'.join(lines)
    fields, log = parse(request(name, text))
    pairs = []
    for png in screenshots(log):
        try:
            pairs.append((png, thumbnail(png, width) if os.path.exists(png) else None))
        except OSError:
            pairs.append((png, None))
    return fields, log, pairs


def describe(fields, log, pairs, mode='ours'):
    out = []
    head = 'status: %s' % fields.get('status', '?')
    for key in ('run', 'batch', 'frames', 'running', 'error'):
        if key in fields:
            head += '   %s: %s' % (key, fields[key])
    out.append(head)
    out.extend(filter_log(log, mode))
    for png, jpg in pairs:
        out.append('shot: %s%s' % (png, ('   look: %s' % jpg) if jpg else ''))
    return '\n'.join(out)


def bookmark(name='main', width=1280):
    """The F11 bundle, taken now (src/bookmark.h). -> (the bundle's text, small JPEG of its screenshot or None)."""
    fields, log = parse(request(name, '!bookmark'))
    m = re.search(r'\(last: (.+?\.txt)\)', fields.get('bookmark', ''))
    if not m or not os.path.exists(m.group(1)):
        return 'bookmark: %s' % fields.get('bookmark', '?'), None
    with open(m.group(1), encoding='utf-8', errors='replace') as f:
        text = f.read()
    png = m.group(1)[:-4] + '.png'
    for _ in range(100):  # the screenshot is written on its own thread
        if os.path.exists(png):
            break
        time.sleep(0.05)
    look = thumbnail(png, width) if os.path.exists(png) else None
    return text.rstrip(), look


def start(name='main', script='menu', launch='', fast=False, extra='', wait_s=7200, boot_wait=180, exe='',
          window='behind', coverage=False):
    """Starts a game with the live channel through run_list (background). -> text."""
    try:
        fields, _ = parse(request(name, '!status', connect_wait=0.3))
        return 'already running: run %s, frame %s (live channel %s)' % (fields.get('run'), fields.get('frame'), name)
    except NoGame:
        pass
    os.makedirs(OUT, exist_ok=True)
    path = SCRIPTS.get(script, script)
    if not os.path.isabs(path):
        path = os.path.join(REPO, path)
    if not os.path.exists(path):
        return 'no such script: %s' % path
    flags = '--wwe2k14_live=%s' % name
    if launch:
        flags += ' --wwe2k14_launch=%s' % launch
    # Session 29 part 3: live sessions catch hitches by themselves (a bookmark at a 50 ms frame during steady play) with
    # the last 10 s as a clip (no cost measured at 60 fps, runs 2202 / 2203); --extra can set other values.
    if '--wwe2k14_bookmark_auto_ms=' not in extra:
        flags += ' --wwe2k14_bookmark_auto_ms=50'
    if '--wwe2k14_bookmark_clip_fps=' not in extra:
        flags += ' --wwe2k14_bookmark_clip_fps=10'
    if '--wwe2k14_settings_file=' not in extra:
        # CLAUDE.md: test runs that can reach the GRAPHICS menu never touch Jarrod's launcher settings.
        flags += ' --wwe2k14_settings_file=%s' % os.path.join(OUT, 'settings_%s.json' % name)
    if coverage:  # Session 30 (#6): the coverage build, recording which functions run per window (`window LABEL`)
        exe = 'coverage'
        if '--wwe2k14_coverage=' not in extra:
            flags += ' --wwe2k14_coverage=true'
    if extra:
        flags += ' ' + extra
    q = lambda s: "'" + s.replace("'", "''") + "'"
    if exe == 'coverage':  # Session 29: the trace build (--wwe2k14_trace)
        exe = os.path.join(REPO, 'out', 'build', 'win-amd64-coverage', 'wwe2k14.exe')
    win = {'visible': ' -Visible', 'windowed': ' -Visible -Windowed',  # Session 30: run_list's window
           'foreground': ' -Foreground'}.get(window, '')
    cmd = "& %s -Scripts @(%s) -Extra %s -WaitSeconds %d%s%s%s" % (q(RUN_LIST), q(path), q(flags), wait_s,
                                                                  '' if fast else ' -RealTime',
                                                                  (' -Exe %s' % q(exe)) if exe else '', win)
    log_path = os.path.join(OUT, 'start_%s.log' % name)
    # A hidden console, not DETACHED_PROCESS: pwsh without a console exits at once without running anything (Session
    # 29, first try). Break away from the caller's job when allowed, so the game outlives the command that started it.
    base = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    with open(log_path, 'w', encoding='utf-8') as lf:
        for flags_try in (base | 0x01000000, base):  # 0x01000000 = CREATE_BREAKAWAY_FROM_JOB
            try:
                subprocess.Popen(['pwsh', '-NoProfile', '-Command', cmd], cwd=REPO, stdout=lf,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags_try)
                break
            except OSError:  # the job does not allow breakaway
                continue
    t0 = time.time()
    while time.time() - t0 < boot_wait:
        try:
            fields, _ = parse(request(name, '!status', connect_wait=1.0))
            return ('started: run %s, live channel %s, %s (%.0f s; run_list output in %s)' %
                    (fields.get('run'), name, 'fast-forward' if fast else 'normal speed', time.time() - t0, log_path))
        except NoGame:
            pass
    return 'no answer after %d s; see %s' % (boot_wait, log_path)


# Session 30 (#1): save a live session as a route.
INSPECT = {'log_state', 'scan_start', 'scan_eq', 'scan_ne', 'scan_gt', 'scan_lt', 'scan_changed', 'scan_unchanged',
           'scan_inc', 'scan_dec', 'scan_show', 'scan_end', 'scan_range', 'watch', 'unwatch', 'snap', 'unsnap',
           'watch_zero', 'screenshot', 'screenshot_every', 'check', 'mark', 'log'}
BATCH_LINE = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+\].*\[wwe2k14\]\[live\] batch (\d+)> (.*)$')
BATCH_RESULT = re.compile(r'\[wwe2k14\]\[live\] batch (\d+) result: (\w+)')
START_SCRIPT = re.compile(r"\[wwe2k14\] modified: wwe2k14_input_script = '([^']*)'")
LAUNCH_FLAG = re.compile(r"\[wwe2k14\] modified: wwe2k14_launch = '([^']*)'")


def run_log(run):
    return os.path.join(EXE_DIR, 'logs', 'wwe2k14_%03d.log' % int(run))


def read_batches(run):
    """-> (batches {n: {'time', 'lines', 'result'}}, start script, launch scenario) from run NNN's log."""
    batches, start, launch = {}, '', ''
    with open(run_log(run), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = BATCH_LINE.search(line)
            if m:
                b = batches.setdefault(int(m.group(2)), {'time': m.group(1)[11:], 'lines': [], 'result': ''})
                b['lines'].append(m.group(3).rstrip())
                continue
            m = BATCH_RESULT.search(line)
            if m and int(m.group(1)) in batches:
                batches[int(m.group(1))]['result'] = m.group(2)
                continue
            m = START_SCRIPT.search(line)
            if m and not start:
                start = m.group(1)
            m = LAUNCH_FLAG.search(line)
            if m and not launch:
                launch = m.group(1)
    return batches, start, launch


def newest_run_with_batches():
    logs = sorted((f for f in os.listdir(os.path.join(EXE_DIR, 'logs')) if re.match(r'wwe2k14_\d+\.log$', f)),
                  key=lambda f: int(re.findall(r'\d+', f)[-1]), reverse=True)
    for f in logs[:40]:
        with open(os.path.join(EXE_DIR, 'logs', f), encoding='utf-8', errors='replace') as h:
            if '[wwe2k14][live] batch 1> ' in h.read():
                return int(re.findall(r'\d+', f)[-1])
    return None


def save_route(name, run=None, first=None, last=None, drop_inspect=False, keep_failed=False, force=False, live_name='main'):
    if run is None:
        try:
            fields, _ = parse(request(live_name, '!status', connect_wait=0.3))
            run = int(fields.get('run'))
        except (NoGame, TypeError, ValueError):
            run = newest_run_with_batches()
    if run is None:
        return 1, 'no run with live batches found (games built before Session 30 do not log them)'
    batches, start, launch = read_batches(run)
    if not batches:
        return 1, 'run %s has no live batches in its log' % run
    out_path = os.path.join(REPO, 'tools', 'input_scripts', 'short', name if name.endswith('.txt') else name + '.txt')
    if os.path.exists(out_path) and not force:
        return 1, '%s exists (--force to overwrite)' % out_path
    lines = ['# Saved from live session run %03d (%s) with tools/live/live.py save-route, %s.' % (
                 run, os.path.basename(start.replace('\\', '/')) or 'no start script', time.strftime('%Y-%m-%d %H:%M')),
             '# Each part below is one live batch (number, time, result). Failed batches %s; inspection lines %s.' % (
                 'kept' if keep_failed else 'left out', 'left out' if drop_inspect else 'kept')]
    if launch:
        lines.append('# Use with --wwe2k14_launch=%s (the live game was started with it).' % launch)
    lines += ['on_fail quit', '']
    base = os.path.basename(start.replace('\\', '/')).lower()
    if base == 'menu.txt' and 'live' in start.lower():
        lines += ['include boot_to_menu   # the live start (live/menu.txt): boot to the main menu', '']
    elif start and base != 'idle.txt':
        lines += ['include %s   # the live start script' % os.path.relpath(
            os.path.normpath(os.path.join(EXE_DIR, start)), os.path.dirname(out_path)).replace('\\', '/'), '']
    kept = dropped_lines = dropped_batches = 0
    for n in sorted(batches):
        if (first is not None and n < first) or (last is not None and n > last):
            continue
        b = batches[n]
        if b['result'] != 'ok' and not keep_failed:
            dropped_batches += 1
            continue
        body = []
        for ln in b['lines']:
            words = ln.split()
            if words and words[0].lower() in INSPECT and drop_inspect:
                dropped_lines += 1
                continue
            body.append(ln)
        if not body:
            continue
        lines.append('# batch %d (%s, %s)' % (n, b['time'], b['result'] or 'no result'))
        lines += body
        kept += 1
    lines += ['', 'quit', '']
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    checks = sum(1 for ln in lines if ln.split()[:1] == ['check'])
    return 0, 'saved %s: %d batches of run %03d (%d failed batches and %d inspection lines left out)%s' % (
        out_path, kept, run, dropped_batches, dropped_lines,
        '; note: %d `check` line(s) wait for Jarrod (up to 300 s each) when the route runs' % checks if checks else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['start', 'run', 'goto', 'shot', 'check', 'status', 'stop', 'quit', 'bookmark',
                                    'save-route', 'text', 'struct', 'window', 'diffcov'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--name', default='main')
    ap.add_argument('-f', '--file')
    ap.add_argument('--timeout', type=int, default=120)
    ap.add_argument('--log', default='ours', choices=['ours', 'files', 'all', 'none'])
    ap.add_argument('--shot', action='store_true')
    ap.add_argument('--width', type=int, default=1280)
    ap.add_argument('--script', default='menu')
    ap.add_argument('--launch', default='')
    ap.add_argument('--fast', action='store_true')
    ap.add_argument('--extra', default='')
    ap.add_argument('--exe', default='', help='another wwe2k14.exe, or "coverage" (the trace build)')
    ap.add_argument('--window', default='behind', choices=['behind', 'visible', 'windowed', 'foreground'])
    ap.add_argument('--coverage', action='store_true', help='start: the coverage build with recording on')
    ap.add_argument('--top', type=int, default=40, help='diffcov: how many functions to list')
    ap.add_argument('--run', type=int)
    ap.add_argument('--from', dest='first', type=int)
    ap.add_argument('--to', dest='last', type=int)
    ap.add_argument('--drop-inspect', action='store_true')
    ap.add_argument('--keep-failed', action='store_true')
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'diffcov':  # Session 30 (#6)
        try:
            fields, _ = parse(request(a.name, '!status', connect_wait=0.5))
            run = ['--run', str(int(fields.get('run')))]
        except (NoGame, TypeError, ValueError):
            run = ['--run', str(a.run)] if a.run else []
        return subprocess.call([sys.executable, os.path.join(REPO, 'tools', 'coverage', 'diffcov.py')] + a.args + run + ['--top', str(a.top)])
    if a.cmd == 'save-route':
        if not a.args:
            print('save-route needs a NAME')
            return 1
        code, msg = save_route(a.args[0], a.run, a.first, a.last, a.drop_inspect, a.keep_failed, a.force, a.name)
        print(msg)
        return code
    try:
        if a.cmd == 'start':
            text = start(a.name, a.script, a.launch, a.fast, a.extra, exe=a.exe, window=a.window, coverage=a.coverage)
            print(text)
            return 0 if text.startswith(('started', 'already')) else 1
        if a.cmd == 'text':  # Session 30
            print(request(a.name, '!text').replace('\n.\n', '\n').rstrip())
            return 0
        if a.cmd in ('status', 'stop'):
            fields, log = parse(request(a.name, '!' + a.cmd))
            print('  '.join('%s: %s' % kv for kv in fields.items()))
            return 0
        if a.cmd == 'bookmark':
            text, look = bookmark(a.name, a.width)
            print(text)
            if look:
                print('look: %s' % look)
            return 0
        if a.cmd == 'quit':
            try:
                request(a.name, 'quit', connect_wait=2.0)
            except NoGame:
                pass
            print('quit sent (the game closes like Alt+F4; run_list then releases the run lock)')
            return 0
        if a.cmd == 'shot':
            lines = ['screenshot %s' % (a.args[0] if a.args else 'live_%s' % time.strftime('%H%M%S'))]
        elif a.cmd == 'window':  # Session 30 (#6): a coverage window edge
            if not a.args:
                print('window needs a LABEL')
                return 1
            lines = ['mark %d %s' % (int(time.time() * 10) % 1000000, '_'.join(a.args))]
        elif a.cmd == 'struct':  # Session 30
            lines = ['dump_struct %s' % ' '.join(a.args)]
        elif a.cmd == 'goto':
            lines = ['include %s' % n for n in a.args]
        elif a.cmd == 'check':
            lines = ['check %s' % ' '.join(a.args)]
            a.timeout = max(a.timeout, 330)
        elif a.file:
            src = sys.stdin if a.file == '-' else open(a.file, encoding='utf-8')
            lines = src.read().splitlines()
        else:
            lines = a.args
        fields, log, pairs = run_batch(a.name, lines, a.timeout, a.shot, a.width)
        print(describe(fields, log, pairs, a.log))
        return 0 if fields.get('status') == 'ok' else 1
    except NoGame as e:
        print('no game: %s (start one with: py tools\\live\\live.py start)' % e)
        return 2


if __name__ == '__main__':
    sys.exit(main())

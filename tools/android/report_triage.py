#!/usr/bin/env python3
"""Session 95 (LADDER): name the class of a phone report in one line, offline.

  py tools\\android\\report_triage.py <report folder or .zip> [more ...]

A report is what the launcher's SHARE makes (logs/startup.txt, last_game_exit.txt, stderr.txt, frames_NNN.csv, launcher_settings.json,
wwe2k14_NNN.log). The rules are the same ones the launcher uses (src\\launcher_mobile\\ladder.h Classify / MeanFps); keep them in step.
Classes: CRASH, CRASH-ON-QUIT, FATAL, HANG, BLACK, ANR, LOW-MEMORY, NORMAL-QUIT, NORMAL-QUIT-SLOW, UNKNOWN, NO-RECORD.
Nothing from the game is read: only our own log files.
"""
import csv
import re
import statistics
import sys
import zipfile
from pathlib import Path

CRASH_SIGNALS = {4: 'SIGILL', 5: 'SIGTRAP', 6: 'SIGABRT', 7: 'SIGBUS', 8: 'SIGFPE', 11: 'SIGSEGV', 31: 'SIGSYS'}


def read_files(src: Path):
    """name -> text for the files the triage needs."""
    wanted = ('startup.txt', 'stderr.txt', 'last_game_exit.txt', 'launcher_settings.json', 'report.txt')
    out = {}
    if src.is_dir():
        for p in src.rglob('*'):
            if p.is_file() and (p.name in wanted or re.fullmatch(r'frames_\d+\.csv', p.name) or re.fullmatch(r'wwe2k14_\d+\.log', p.name)):
                out[p.name] = p.read_text(encoding='utf-8', errors='replace')
    else:
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                base = n.rsplit('/', 1)[-1]
                if base in wanted or re.fullmatch(r'frames_\d+\.csv', base) or re.fullmatch(r'wwe2k14_\d+\.log', base):
                    out[base] = z.read(n).decode('utf-8', errors='replace')
    return out


def line_time(line):
    m = re.match(r'\s*([0-9]+\.[0-9]+)\s+(.*)', line)
    return (float(m.group(1)), m.group(2)) if m else (-1.0, line.strip())


def mean_fps(text):
    rows = list(csv.DictReader(text.splitlines()))
    ms = [float(r['frame_ms']) for r in rows if r.get('frame_ms') and r.get('t_seconds') and float(r['t_seconds']) > 10]
    normal = [m for m in ms if m >= 10]
    return 1000.0 / (sum(normal) / len(normal)) if len(normal) >= 3000 else 0.0


def triage(src: Path):
    f = read_files(src)
    box = f.get('startup.txt')
    if box is None:
        return 'NO-RECORD', 'no startup.txt in the report'
    played = boot = quit_marker = precompile = False
    t_first = t_quit = -1.0
    crash = fatal = hang = black = gpu_fatal = ''
    memory_lows = 0
    for raw in box.splitlines():
        t, text = line_time(raw)
        if text.startswith('step: '):
            boot = True
        if 'first frame' in text:
            played, t_first = True, t
        if 'quit to the launcher' in text:
            quit_marker, t_quit = True, t
            precompile = precompile or 'shader precompile ended' in text
        if 'CRASH: ' in text:
            crash = text.split('CRASH: ', 1)[1]
        if 'HANG: ' in text:
            hang = text.split('HANG: ', 1)[1]
        if 'BLACK: ' in text:
            black = text.split('BLACK: ', 1)[1]
        if 'FATAL: GPU lacks ' in text:
            gpu_fatal = text.split('FATAL: GPU lacks ', 1)[1]
        if 'memory low' in text:
            memory_lows += 1
    for raw in f.get('stderr.txt', '').splitlines():
        if 'FATAL ERROR: ' in raw:
            fatal = raw.split('FATAL ERROR: ', 1)[1].strip()
    reason, status, importance = '', 0, 1000
    for raw in f.get('last_game_exit.txt', '').splitlines():
        if raw.startswith('reason: '):
            reason = raw[8:].strip()
        elif raw.startswith('status: '):
            status = int(re.match(r'-?\d+', raw[8:].strip() or '0').group(0) or 0)
        elif raw.startswith('importance: '):
            importance = int(re.match(r'-?\d+', raw[12:].strip() or '1000').group(0) or 1000)
    fps = 0.0
    for name, text in f.items():
        if name.startswith('frames_'):
            fps = mean_fps(text)
    where = 'while playing' if played else 'at start-up'
    if precompile:
        return 'PRECOMPILE', 'the shader precompile run, not a play'
    if gpu_fatal:
        return 'FATAL', 'this GPU lacks ' + gpu_fatal
    if hang:
        return 'HANG', hang
    if black:
        return 'BLACK', black
    if quit_marker:
        # r014: a CRASH line after the game was running for a long time with no quit marker is a different class; with the marker the
        # run ended by the player's choice (SIGKILL 9 / UNKNOWN 0 afterwards are the game ending its own process).
        early = played and t_first >= 0 and t_quit >= t_first and (t_quit - t_first) < 120
        if crash:
            return 'CRASH-ON-QUIT', crash
        slow = f' - ran at {fps:.0f} fps: the 30 FPS card would show' if 0 < fps < 52 else ''
        if early:
            return 'NORMAL-QUIT', f'left {t_quit - t_first:.0f} s after the first frame: the "did the picture look right?" card would show'
        return ('NORMAL-QUIT-SLOW' if slow else 'NORMAL-QUIT'), 'the player quit' + slow
    if fatal:
        return 'FATAL', fatal
    log_tail = ''
    for name, text in f.items():
        if re.fullmatch(r'wwe2k14_\d+\.log', name):
            log_tail = ' | '.join(text.splitlines()[-40:])
    if crash and ('Execution complete' in log_tail or 'window closing' in log_tail):
        # r014: the in-game QUIT ran the SDK's shutdown to its end ("Execution complete") and the process aborted in libc
        return 'CRASH-ON-QUIT', f'{crash.split(". ")[0]}; the log ends at the SDK shutdown'
    if crash:
        # a CRASH line well into a play with no quit marker: the SDK's own abort (r014: SIGABRT 80 s in, exit status 6)
        return 'CRASH', f'{crash.split(". ")[0]} {where}'
    if (reason.startswith('SIGNALED') or reason.startswith('CRASH')) and status in CRASH_SIGNALS:
        extra = f', with {memory_lows} low-memory warnings before it (not a low-memory kill: the signal is a crash)' if memory_lows else ''
        return 'CRASH', f'{CRASH_SIGNALS[status]} {where}, no CRASH line was written{extra}'
    if reason.startswith('ANR'):
        return 'ANR', 'Android says the game stopped responding'
    if reason.startswith('LOW_MEMORY'):
        return 'LOW-MEMORY', 'Android closed the game ' + ('in front' if importance <= 125 else 'in the background (normal)')
    return 'UNKNOWN', f'no named end (exit reason "{reason or "none"}", status {status}); never counts on the ladder'


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for a in argv[1:]:
        cls, why = triage(Path(a))
        print(f'{Path(a).name}: {cls}: {why}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

"""Where a session's time went (Session 92; Jarrod: "asking about timing has helped us optimize quite a bit in the past").

  py -I tools\\harness\\session_time.py                         the newest Claude Code session of this project, from its start
  py -I tools\\harness\\session_time.py --since "2026-11-05 10:00"   only from that local time on
  py -I tools\\harness\\session_time.py --session <id> -v          another session; -v lists every agent

What it reads (nothing is changed):
  - the game run logs (out\\build\\win-amd64-release\\logs and out\\harness\\bisect\\exe\\*\\logs, wwe2k14_*.log): a run = its log's
    first line time -> the file's last write. The run figures come from here, not from transcripts (a builder's background
    runs leave no clean trace in its transcript).
  - the session transcript (%USERPROFILE%\\.claude\\projects\\<project>\\<id>.jsonl) and its subagents\\ folder (each agent's
    description from its .meta.json: SON / OPUS prefixes and words like builder / review / design / shot-check give the role).
Figures:
  - "summed" = every agent's own time added up (agents overlap, so this is more than the clock);
  - "on the clock" = how long at least one agent of that role was working (the union of their spans);
  - builders' "waiting" = gaps over 4 min in their transcript (a background check running, or stopped for reviews / the go);
    "compiling" = their foreground build calls (fast_build, sdk_check, build_sdk, build.ps1, the Android builds, proof builds).
"""
import datetime as dt
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT = os.path.join(os.path.expanduser('~'), '.claude', 'projects', re.sub(r'[:\\/]', '-', ROOT))
LOCAL = dt.datetime.now().astimezone().utcoffset()
GAP = 240
BUILD = re.compile(r'fast_build|sdk_check|build_sdk|build\.ps1|build_game_android|build_sdk_android|proof_tno|wt_build', re.I)
READONLY = re.compile(r'^\s*(cd [^;&]*[;&]+\s*)?(grep|sed|cat|head|tail|ls|Get-Content|Select-String|echo|wc|git )', re.I)


def hm(sec):
    sec = int(sec)
    return '%dh%02d' % (sec // 3600, (sec % 3600) // 60)


def union(iv):
    tot, cur = 0.0, None
    for a, b in sorted(iv):
        if cur and a <= cur[1]:
            cur[1] = max(cur[1], b)
        else:
            if cur:
                tot += (cur[1] - cur[0]).total_seconds()
            cur = [a, b]
    if cur:
        tot += (cur[1] - cur[0]).total_seconds()
    return tot


def clip(iv, since):
    return [(max(a, since), b) for a, b in iv if b > since]


def stamp(s):
    return dt.datetime.strptime(s[:19], '%Y-%m-%dT%H:%M:%S') + LOCAL


def read_transcript(path, since):
    """-> sorted local timestamps, compile intervals (foreground build calls)."""
    ts, comp, pending = [], [], {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith('{'):
                continue
            try:
                j = json.loads(line)
            except Exception:
                continue
            t = j.get('timestamp')
            if not t:
                continue
            t = stamp(t)
            if t < since:
                continue
            ts.append(t)
            content = (j.get('message') or {}).get('content')
            if not isinstance(content, list):
                continue
            for c in content:
                if not isinstance(c, dict):
                    continue
                if c.get('type') == 'tool_use' and c.get('name') in ('Bash', 'PowerShell'):
                    inp = c.get('input') or {}
                    cmd = inp.get('command', '')
                    if not inp.get('run_in_background') and not READONLY.search(cmd) and BUILD.search(cmd):
                        pending[c['id']] = t
                elif c.get('type') == 'tool_result' and c.get('tool_use_id') in pending:
                    comp.append((pending.pop(c['tool_use_id']), t))
    ts.sort()
    return ts, comp


def role(d):
    s = d.lower()
    if 'review' in s or 'skeptic' in s:
        return 'review'
    if 'shot' in s or 'picture' in s:
        return 'pictures'
    if 'design' in s or s.startswith('opus read') or 'read:' in s or 'research' in s or 'scout' in s or 'map' in s:
        return 'design / reading'
    if s.startswith('son') and ('read' in s.split()[:3] or 'explore' in s):
        return 'design / reading'
    if s.startswith('son') or 'builder' in s or 'build' in s or 'harness' in s or 'tool' in s:
        return 'building'
    return 'other'


def runs(since, until):
    groups, allv = {}, []
    pats = [os.path.join(ROOT, r'out\build\win-amd64-release\logs\wwe2k14_*.log'),
            os.path.join(ROOT, r'out\harness\bisect\exe\*\logs\wwe2k14_*.log')]
    for lg in [p for pat in pats for p in glob.glob(pat)]:
        end = dt.datetime.fromtimestamp(os.path.getmtime(lg))
        if end < since:
            continue
        try:
            with open(lg, encoding='utf-8', errors='replace') as f:
                m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)', f.readline())
        except OSError:
            continue
        if not m:
            continue
        beg = dt.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S')
        if beg < since or end <= beg or beg > until:
            continue
        parts = os.path.normpath(lg).split(os.sep)
        g = 'main exe' if 'win-amd64-release' in parts else parts[parts.index('exe') + 1]
        groups.setdefault(g, []).append((beg, end))
        allv.append((beg, end))
    return groups, allv


def main(argv):
    since = None
    session = None
    verbose = '-v' in argv
    for i, a in enumerate(argv):
        if a == '--since' and i + 1 < len(argv):
            since = dt.datetime.strptime(argv[i + 1], '%Y-%m-%d %H:%M')
        elif a == '--session' and i + 1 < len(argv):
            session = argv[i + 1]
    if session:
        main_path = os.path.join(PROJECT, session + '.jsonl')
    else:
        cands = glob.glob(os.path.join(PROJECT, '*.jsonl'))
        if not cands:
            print('session_time: no transcripts in %s' % PROJECT)
            return 2
        main_path = max(cands, key=os.path.getmtime)
    sub = main_path[:-6] + os.sep + 'subagents'
    main_ts, main_comp = read_transcript(main_path, since or dt.datetime(2000, 1, 1))
    if not main_ts:
        print('session_time: no entries after the start time in %s' % main_path)
        return 1
    since = since or main_ts[0]
    until = main_ts[-1]
    clock = (until - since).total_seconds()
    print('SESSION %s: %s -> %s, %s on the clock' % (os.path.basename(main_path)[:8], since.strftime('%Y-%m-%d %H:%M'),
                                                     until.strftime('%H:%M'), hm(clock)))

    groups, allv = runs(since, until)
    if allv:
        d = sorted((b - a).total_seconds() for a, b in allv)
        ev = sorted([(a, 1) for a, b in allv] + [(b, -1) for a, b in allv])
        cur = peak = 0
        for _, x in ev:
            cur += x
            peak = max(peak, cur)
        on = union(allv)
        print('GAME RUNS: %d runs (median %ds, 90%% under %ds); a game running %s of the clock (%d%%); game time %s (up to %d at once)' % (
            len(allv), d[len(d) // 2], d[int(len(d) * 0.9)], hm(on), 100 * on / max(clock, 1), hm(sum(d)), peak))
        for g, iv in sorted(groups.items(), key=lambda kv: -union(kv[1])):
            print('  %-22s %4d runs, %s on the clock (%s-%s)' % (g, len(iv), hm(union(iv)), min(iv)[0].strftime('%H:%M'),
                                                                 max(b for a, b in iv).strftime('%H:%M')))
        hours = {}
        for a, b in allv:
            t = a
            while t < b:
                h = t.replace(minute=0, second=0, microsecond=0)
                nxt = min(b, h + dt.timedelta(hours=1))
                hours.setdefault(h, []).append((t, nxt))
                t = nxt
        print('  minutes of each hour with a game running: ' + ', '.join('%s %d' % (h.strftime('%H'), union(v) // 60)
                                                                         for h, v in sorted(hours.items())))
    else:
        print('GAME RUNS: none')

    by, rows = {}, []
    for mp in glob.glob(os.path.join(sub, 'agent-*.meta.json')):
        try:
            meta = json.load(open(mp, encoding='utf-8'))
        except Exception:
            continue
        jl = mp[:-10] + '.jsonl'
        if not os.path.exists(jl):
            continue
        ts, comp = read_transcript(jl, since)
        if len(ts) < 2:
            continue
        d = meta.get('description', '?')
        r = role(d)
        span = (ts[-1] - ts[0]).total_seconds()
        wait = sum((b - a).total_seconds() for a, b in zip(ts, ts[1:]) if (b - a).total_seconds() > GAP)
        e = by.setdefault(r, {'n': 0, 'span': 0.0, 'wait': 0.0, 'comp': 0.0, 'iv': []})
        e['n'] += 1
        e['span'] += span
        e['wait'] += wait
        e['comp'] += union(comp)
        e['iv'].append((ts[0], ts[-1]))
        rows.append((ts[0], ts[-1], r, d, meta.get('model', '?'), wait, union(comp)))
    if by:
        allag = [iv for e in by.values() for iv in e['iv']]
        print('AGENTS: %d, %s summed, at least one working %s of the clock' % (sum(e['n'] for e in by.values()),
                                                                              hm(sum(e['span'] for e in by.values())), hm(union(allag))))
        for r, e in sorted(by.items(), key=lambda kv: -kv[1]['span']):
            line = '  %-17s %3d  %s summed, %s on the clock' % (r, e['n'], hm(e['span']), hm(union(e['iv'])))
            if r == 'building':
                line += ' | waiting %s (background checks, stopped for reviews), compiling %s, the rest %s (reading, writing, foreground runs)' % (
                    hm(e['wait']), hm(e['comp']), hm(e['span'] - e['wait'] - e['comp']))
            print(line)
        if 'building' in by and allv:
            bexe = [iv for g, ivs in groups.items() if g != 'main exe' for iv in ivs]
            print('  builders\' own builds (live_*) had a game running %s of the clock' % hm(union(bexe)))
    if verbose:
        for a, b, r, d, model, wait, comp in sorted(rows):
            print('    %s-%s %-16s %-7s %-46s %s (waiting %s, compiling %s)' % (a.strftime('%H:%M'), b.strftime('%H:%M'), r, model, d[:46],
                                                                         hm((b - a).total_seconds()), hm(wait), hm(comp)))
    print('MAIN SESSION: compiling (foreground builds, pins) %s; the rest is designs, contracts, merges, reports and the runs it drove' % hm(
        union(main_comp)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

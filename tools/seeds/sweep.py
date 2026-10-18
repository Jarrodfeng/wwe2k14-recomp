"""Session 16 item 4: seed sweeper, the first step towards the match director. Runs a scenario once per seed in
replay mode (deterministic: src/replay.h), fast-forwarded, CPU vs CPU, to the finish, and records the outcome.

    py tools/seeds/sweep.py SCENARIO --seeds 1..20 [--seeds 86401234,...] [--res 1] [--entrances] [--wait 1500]
    py tools/seeds/sweep.py SCENARIO --report            (table of what is already in the CSV)
    py tools/seeds/sweep.py SCENARIO --director NAME --seeds 1..10 [--parallel 3]

Session 17: --director NAME runs a director script (out/director/NAME.txt, src/director.h) in every seed (its events
change the match, so they are part of the search); results go to out/seeds/SCENARIO+NAME.csv (find.py takes the same
SCENARIO+NAME). --parallel N runs N seeds at once in run_list's shared lanes (each game is deterministic on its own:
replay mode); a row's wall_s is then that run's own time.

Per seed (one game at a time, tools/bench/run_list.ps1 with tools/seeds/seed_match.txt; flags
--wwe2k14_launch=SCENARIO --wwe2k14_replay_mode=true --wwe2k14_fixed_seed=N, entrances skipped unless --entrances,
--resolution_scale=1 (720p internal) unless --res, and peeks on the finish type, winner / loser and the referee's
count), it appends one row to out/seeds/SCENARIO.csv:
  seed, run, status (ok / no_finish / failed), finish (G+472 0x8376ACCC: 1 pinfall, 6 seen at a tag finish, 7 Rumble;
  others unknown), winner, loser (slots, bytes at 0x8376E3EC / +1, -1 none), clock (match clock in frames at the
  finish; /60 = seconds of match time), finish_frame (guest frame), pins (times the referee's count 0x83767B6C>80
  started), near_falls (pins that reached a count of 2 without a finish), wall_s (run length).
Seeds already in the CSV are skipped (rerun with --force). Output stays under out/ (game-derived).
Seed space (src/determinism.h): srand gets N % 1000, MT #1 / cursor A (N % 1000) & 0xFF, MT #2 a lossy pack of the
time of day N % 86,400,000, the audio LCG and clock-shaped script reseeds all 32 bits of N. Consecutive seeds below
256 differ in every source.
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN_LIST = os.path.join(REPO, 'tools', 'bench', 'run_list.ps1')
ROUTE = os.path.join(REPO, 'tools', 'seeds', 'seed_match.txt')
EXE = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'wwe2k14.exe')
LOGS = os.path.join(os.path.dirname(EXE), 'logs')
OUT = os.path.join(REPO, 'out', 'seeds')
PEEKS = 'fin=0x8376ACCC,win=0x8376E3EC,jcnt=0x83767B6C>80'
FIELDS = ['seed', 'run', 'status', 'finish', 'winner', 'loser', 'clock', 'clock_s', 'finish_frame', 'pins',
          'near_falls', 'wall_s', 'flags']
FINISH = {1: 'pinfall', 6: 'tag finish (6)', 7: 'rumble (7)'}


def parse_seeds(specs):
    out = []
    for spec in specs:
        for part in spec.split(','):
            part = part.strip()
            if not part:
                continue
            if '..' in part:
                a, b = part.split('..')
                out.extend(range(int(a), int(b) + 1))
            else:
                out.append(int(part))
    seen, res = set(), []
    for s in out:
        if s not in seen:
            seen.add(s)
            res.append(s)
    return res


def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def s8(v):
    return v - 256 if v >= 128 else v


def parse_log(run):
    path = os.path.join(LOGS, 'wwe2k14_%03d.log' % int(run))
    if not os.path.exists(path):  # e.g. a seed run with the main build while the sweep used a snapshot exe
        path = os.path.join(os.path.dirname(EXE), 'logs', 'wwe2k14_%03d.log' % int(run))
    r = dict(status='failed', finish='', winner='', loser='', clock='', clock_s='', finish_frame='', pins=0,
             near_falls=0)
    if not os.path.exists(path):
        return r
    jcnt_prev, jmax, fin_frame = 0, 0, None
    decided = False
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'\[peek\] frame=(\d+) (\w+)=0x([0-9A-F]+)', line)
            if m:
                frame, name, val = int(m.group(1)), m.group(2), int(m.group(3), 16)
                if name == 'jcnt':
                    if val > 0 and jcnt_prev == 0:
                        r['pins'] += 1
                        jmax = 0
                    jmax = max(jmax, val)
                    if val == 0 and jcnt_prev > 0 and jmax >= 2 and fin_frame is None:
                        r['near_falls'] += 1
                    jcnt_prev = val
                elif name == 'fin' and val != 0 and fin_frame is None:
                    fin_frame = frame
                continue
            m = re.search(r'line \d+(?: \([^)]*\))?: state (\S+) = 0x([0-9A-F]+) \(', line)
            if m:
                name, raw = m.group(1).lower(), int(m.group(2), 16)
                if name == '0x8376accc':
                    r['finish'] = raw
                elif name == '0x8376e3ec':
                    r['winner'], r['loser'] = s8(raw >> 24 & 0xFF), s8(raw >> 16 & 0xFF)
                elif name == 'match_clock':
                    r['clock'] = raw
                    r['clock_s'] = round(raw / 60.0, 1)
            if 'seeds: decided' in line:
                decided = True
            if 'seeds: done' in line:
                r['status'] = 'ok' if decided and r['finish'] not in ('', 0) else 'no_finish'
    r['finish_frame'] = fin_frame if fin_frame is not None else ''
    return r


def load(csv_path):
    if not os.path.exists(csv_path):
        return []
    with open(csv_path, newline='') as f:
        return list(csv.DictReader(f))


def report(rows, scenario):
    lines = ['seed sweep %s: %d seeds' % (scenario, len(rows)),
             '%10s %6s %-9s %-15s %6s %5s %8s %5s %5s %6s' % ('seed', 'run', 'status', 'finish', 'winner', 'loser',
                                                              'clock_s', 'pins', 'nearf', 'wall_s')]
    for r in rows:
        fin = r['finish']
        fin_t = FINISH.get(int(fin), 'type %s' % fin) if fin not in ('', None) else '-'
        lines.append('%10s %6s %-9s %-15s %6s %5s %8s %5s %5s %6s' % (r['seed'], r['run'], r['status'], fin_t,
                                                                      r['winner'], r['loser'], r['clock_s'], r['pins'],
                                                                      r['near_falls'], r['wall_s']))
    ok = [r for r in rows if r['status'] == 'ok']
    if ok:
        wins = {}
        for r in ok:
            wins[r['winner']] = wins.get(r['winner'], 0) + 1
        walls = [float(r['wall_s']) for r in rows if r['wall_s']]
        lines.append('decided: %d / %d; wins by slot: %s; mean run %.0f s (%.1f seeds per hour, one game at a time)' % (
            len(ok), len(rows), ', '.join('%s: %d' % kv for kv in sorted(wins.items())), sum(walls) / len(walls),
            3600.0 / (sum(walls) / len(walls))))
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scenario')
    ap.add_argument('--seeds', action='append', default=[])
    ap.add_argument('--res', default='1', help='resolution_scale (default 1 = 720p internal)')
    ap.add_argument('--entrances', action='store_true', help='play the entrances (default: skipped)')
    ap.add_argument('--extra', default='', help='more flags, one string')
    ap.add_argument('--wait', type=int, default=1500, help='run_list -WaitSeconds per seed')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--reparse', action='store_true', help='re-read every row from its run log (same --exe)')
    ap.add_argument('--exe', default=EXE, help='game exe (e.g. a snapshot copy, so builds can go on meanwhile)')
    ap.add_argument('--director', default='', help='director script name (out/director/NAME.txt) for every seed')
    ap.add_argument('--parallel', type=int, default=1, help='seeds at once (run_list shared lanes, max 3)')
    a = ap.parse_args()
    global LOGS
    LOGS = os.path.join(os.path.dirname(os.path.abspath(a.exe)), 'logs')
    os.makedirs(OUT, exist_ok=True)
    key = a.scenario + ('+' + a.director if a.director else '')
    csv_path = os.path.join(OUT, key + '.csv')
    rows = load(csv_path)
    if a.reparse:
        for r in rows:
            if r['run']:
                r.update({k: v for k, v in parse_log(int(r['run'])).items()})
        with open(csv_path, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)
        a.report = True
    if a.report:
        print(report(rows, key))
        return
    done = {r['seed'] for r in rows}
    todo = []
    for seed in parse_seeds(a.seeds):
        if str(seed) in done and not a.force:
            print('seed %d: already in %s (skip; --force reruns)' % (seed, csv_path))
            continue
        todo.append(seed)

    def seed_flags(seed):
        flags = ('--wwe2k14_launch=%s --wwe2k14_launch_skip_entrances=%s --wwe2k14_replay_mode=true '
                 '--wwe2k14_fixed_seed=%d --resolution_scale=%s --wwe2k14_peek=%s' % (
                     a.scenario, 'false' if a.entrances else 'true', seed, a.res, PEEKS))
        if a.director:
            flags += ' --wwe2k14_director=%s' % a.director
        if a.extra:
            flags += ' ' + a.extra
        return flags

    def save():
        with open(csv_path, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)

    n = max(1, min(3, a.parallel))
    for b in range(0, len(todo), n):
        batch = todo[b:b + n]
        t0 = time.time()
        if len(batch) == 1:
            cmd = '& %s -Scripts @(%s) -Extra %s -WaitSeconds %d -Exe %s' % (
                ps_quote(RUN_LIST), ps_quote(ROUTE), ps_quote(seed_flags(batch[0])), a.wait,
                ps_quote(os.path.abspath(a.exe)))
        else:
            cmd = '& %s -Parallel %d -Scripts @(%s) -ScriptExtras @(%s) -WaitSeconds %d -Exe %s' % (
                ps_quote(RUN_LIST), len(batch), ','.join(ps_quote(ROUTE) for _ in batch),
                ','.join(ps_quote(seed_flags(sd)) for sd in batch), a.wait, ps_quote(os.path.abspath(a.exe)))
        p = subprocess.run(['pwsh', '-NoProfile', '-Command', cmd], capture_output=True, text=True, cwd=REPO)
        wall = round(time.time() - t0, 1)
        lines = p.stdout.splitlines()
        if len(batch) > 1:  # the result lines in input order follow this header
            k = next((i for i, l in enumerate(lines) if 'result lines in input order' in l), len(lines))
            lines = lines[k + 1:]
        results = [l for l in lines if ' -> ' in l][:len(batch)]
        for seed, line in zip(batch, results + [''] * (len(batch) - len(results))):
            m = re.search(r'-> wwe2k14_(\d+)', line)
            run = int(m.group(1)) if m else ''
            mw = re.search(r'after ([0-9.]+)s', line)
            r = parse_log(run) if run else dict(status='failed')
            row = dict(seed=seed, run=run, wall_s=float(mw.group(1)) if mw else wall, flags=seed_flags(seed), **r)
            if run and 'exit code 0' not in line and r.get('status') == 'ok':
                row['status'] = 'ok_unclean_exit'
            rows.append({k2: row.get(k2, '') for k2 in FIELDS})
            save()
            print('seed %d: run %s %s finish %s winner %s loser %s clock %s s, pins %s near-falls %s, %s s' % (
                seed, run, row['status'], row.get('finish'), row.get('winner'), row.get('loser'), row.get('clock_s'),
                row.get('pins'), row.get('near_falls'), row['wall_s']), flush=True)
    txt = report(rows, key)
    with open(os.path.join(OUT, key + '.txt'), 'w') as f:
        f.write(txt + '\n')
    print(txt)


if __name__ == '__main__':
    sys.exit(main())

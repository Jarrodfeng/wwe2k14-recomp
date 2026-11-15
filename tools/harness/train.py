"""Session 102 lane F1: three merge trains and the waiting board (RENDERER_ARC_PT2.md section 9 item 8).

  py -I tools\\harness\\train.py land LANE_BRANCH --track rt|game|gpu --lane NAME [--also GATE_LANE,...]
  py -I tools\\harness\\train.py up --track rt|game|gpu
  py -I tools\\harness\\train.py stamp LANE STAGE [--track T] [--at "YYYY-MM-DD HH:MM[:SS]"]
  py -I tools\\harness\\train.py board
  options on every command: --repo DIR  --base BRANCH (default s102-arc-pt2)  --csv FILE  --gate-cmd "TEMPLATE"  --dry

Tracks: s102-track-rt (render thread: T*, M1), s102-track-game (P*, G4, H*), s102-track-gpu (G1-G3, G5, G6, W*), each created off the
base branch on first use. NEVER pushes, and NEVER resets or merges inside a checkout it does not own (review T1):
  every merge happens in a DETACHED scratch worktree (out\\s102\\train_wt\\cand_*) cut from the branch being advanced; the gate runs
  there; only on green is the branch advanced: `git merge --ff-only <candidate>` in the checkout where the branch is checked out
  (redone from the new tip when the branch moved meanwhile), else a compare-and-swap `update-ref`. The scratch worktree is always
  removed, also on Ctrl-C or an exception (T2): an ungated merge can never be left behind.
`land` = candidate off the track + merge the lane (--no-ff), gate lanes = tools\\gate\\lanes\\*.json the lane added or changed relative
to <base>...<lane> (T4) plus --also (default s102_budget, M1's lane; a missing default lane is printed). A lane already an ancestor of the
track is "already landed" (no csv write). The gate runs with its own results folder out\\gate_train\\<track> and must show at least one
PASS (T7). --lane is required and must already have a csv row (stamp it first) (T6). `up` = the same for track -> base, then stamps
landed_session on that track's landed lanes.

Timestamps per lane per stage go to <repo>\\out\\s102\\trains.csv (M3's board reads it; writes are locked, T5). Stages in order:
  brief_written, agent_started, handback, review_started, review_done, landed_track, landed_session
`handback` keeps the FIRST stamp (B3); later rounds are appended to the `rounds` column. `board` (B1): per lane the waits between stages,
and "wait to land" = (landed_track or now) - first handback, as a share of the lane's life (agent_started -> landed_track or now);
lanes handed back and not landed are listed as "waiting now" (B2). This is NOT the S101 figure ("builders waited 32 %"): that one counts
every gap over 4 min in a builder's transcript and is computed by tools\\harness\\session_time.py from transcripts; trains.csv has no
transcript, so the board cannot compute it (run session_time.py for that). Bad cells are skipped with a warning (B4).
"""
import argparse, csv, datetime as dt, json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

STAGES = ['brief_written', 'agent_started', 'handback', 'review_started', 'review_done', 'landed_track', 'landed_session']
COLS = ['lane', 'track'] + STAGES + ['rounds', 'gate', 'note']
TRACKS = {'rt': 's102-track-rt', 'game': 's102-track-game', 'gpu': 's102-track-gpu'}
FMT = '%Y-%m-%d %H:%M:%S'
DEFAULT_GATE = 'py -I tools/gate/gate.py --root . --only {lanes} --out {out}'
DEFAULT_ALSO = ['s102_budget']  # M1's budget lane: tools\gate\lanes\s102_budget.json


def now():
    return dt.datetime.now().strftime(FMT)


def norm_time(s):
    for f in (FMT, '%Y-%m-%d %H:%M'):
        try:
            return dt.datetime.strptime(s.strip(), f).strftime(FMT)
        except ValueError:
            pass
    raise SystemExit('bad time %r: use "YYYY-MM-DD HH:MM" or "YYYY-MM-DD HH:MM:SS"' % s)


def git(repo, *a, check=True):
    r = subprocess.run(['git', '-C', str(repo)] + list(a), capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit('git %s failed in %s: %s' % (' '.join(a), repo, (r.stderr or r.stdout).strip()))
    return r


def main_checkout():
    here = Path(__file__).resolve().parents[2]
    c = subprocess.run(['git', '-C', str(here), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return Path(c).parent if c else here


# ---- csv (T5: lock + unique temp name) ----------------------------------------------------------------------------------------
class Lock:
    def __init__(self, path, timeout=60.0):
        self.path, self.timeout = str(path) + '.lock', timeout

    def __enter__(self):
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        while True:
            try:
                self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(self.fd, str(os.getpid()).encode())
                return self
            except FileExistsError:
                try:  # a lock older than 2 minutes belongs to a dead process
                    if time.time() - os.path.getmtime(self.path) > 120:
                        os.unlink(self.path)
                        continue
                except OSError:
                    pass
                if time.time() - t0 > self.timeout:
                    raise SystemExit('cannot lock %s (held for over %d s)' % (self.path, self.timeout))
                time.sleep(0.02)

    def __exit__(self, *a):
        os.close(self.fd)
        try:
            os.unlink(self.path)
        except OSError:
            pass


def read_csv(p):
    if not Path(p).exists():
        return []
    with open(p, newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))


def write_csv(p, rows):
    Path(p).parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='trains.', suffix='.tmp', dir=str(Path(p).parent))
    with os.fdopen(fd, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=COLS)
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, '') for c in COLS})
    os.replace(tmp, p)


def update_row(p, lane, fn):
    """Locked read-modify-write of one lane's row; fn(row) mutates it."""
    with Lock(p):
        rows = read_csv(p)
        row = next((r for r in rows if r['lane'] == lane), None)
        if row is None:
            row = {'lane': lane}
            rows.append(row)
        fn(row)
        write_csv(p, rows)


def stamp(p, lane, stage, track='', at=None, **extra):
    if stage not in STAGES:
        raise SystemExit('unknown stage %r (stages: %s)' % (stage, ', '.join(STAGES)))
    t = norm_time(at) if at else now()

    def f(row):
        if track:
            row['track'] = track
        if stage == 'handback' and row.get('handback'):   # B3: keep the first, record each later round
            row['rounds'] = (row.get('rounds', '') + ';' if row.get('rounds') else '') + t
        else:
            row[stage] = t
        row.update(extra)
    update_row(p, lane, f)


# ---- git plumbing: candidates, never resets of a foreign checkout ---------------------------------------------------------------
def branch_checkout(repo, branch):
    cur = None
    for line in git(repo, 'worktree', 'list', '--porcelain').stdout.splitlines():
        if line.startswith('worktree '):
            cur = line[9:].strip()
        elif line.startswith('branch ') and line[7:].strip() == 'refs/heads/' + branch:
            return Path(cur)
    return None


def ensure_track(repo, base, track):
    br = TRACKS[track]
    if git(repo, 'rev-parse', '--verify', '--quiet', 'refs/heads/' + br, check=False).returncode:
        git(repo, 'branch', br, base)
    return br


class Candidate:
    """A detached scratch worktree cut from `rev`; always removed on exit (also on KeyboardInterrupt / exceptions)."""
    def __init__(self, repo, rev):
        self.repo, self.rev = Path(repo), rev

    def __enter__(self):
        self.dir = self.repo / 'out' / 's102' / 'train_wt' / ('cand_%d_%d' % (os.getpid(), int(time.time() * 1000) % 10**9))
        self.dir.parent.mkdir(parents=True, exist_ok=True)
        git(self.repo, 'worktree', 'add', '--detach', str(self.dir), self.rev)
        return self.dir

    def __exit__(self, *a):
        # the scratch worktree is train-owned: no junctions, nothing else points into it
        git(self.dir, 'reset', '--hard', '-q', check=False)
        git(self.dir, 'clean', '-fdxq', check=False)
        r = git(self.repo, 'worktree', 'remove', str(self.dir), check=False)
        if r.returncode and self.dir.exists():
            def onerr(f, p, e):
                os.chmod(p, 0o700)
                f(p)
            shutil.rmtree(self.dir, onerror=onerr)
            git(self.repo, 'worktree', 'prune', check=False)


def advance(repo, branch, old, new):
    """Move `branch` from old to new (a descendant). True on success, False when the branch moved meanwhile."""
    wt = branch_checkout(repo, branch)
    if wt is not None:
        if git(wt, 'rev-parse', 'HEAD').stdout.strip() != old:
            return False
        r = git(wt, 'merge', '--ff-only', '-q', new, check=False)
        if r.returncode:
            print('[train] cannot fast-forward %s in %s: %s' % (branch, wt, (r.stderr or r.stdout).strip()[-300:]))
        return r.returncode == 0
    return git(repo, 'update-ref', 'refs/heads/' + branch, new, old, check=False).returncode == 0


def gate_lanes(repo, base, lane_rev, also):
    d = git(repo, 'diff', '--name-only', '--diff-filter=AM', '%s...%s' % (base, lane_rev)).stdout.split()
    names = [Path(f).stem for f in d if f.replace('\\', '/').startswith('tools/gate/lanes/') and f.endswith('.json')]
    for a in also:
        if not a:
            continue
        if git(repo, 'cat-file', '-e', '%s:tools/gate/lanes/%s.json' % (lane_rev, a), check=False).returncode == 0:
            if a not in names:
                names.append(a)
        else:
            print('[train] note: default extra gate lane %s has no tools/gate/lanes/%s.json yet: not run' % (a, a))
    return names


def run_gate(wt, lanes, template, out_dir):
    """True only when the gate exits 0 AND its results file shows at least one PASS (T7)."""
    out_dir = Path(out_dir)
    shutil.rmtree(out_dir, ignore_errors=True)
    cmd = template.replace('{lanes}', ','.join(lanes)).replace('{out}', '"%s"' % out_dir)
    print('[train] gate: %s   (in %s)' % (cmd, wt))
    r = subprocess.run(cmd, shell=True, cwd=str(wt), capture_output=True, text=True)
    sys.stdout.write(r.stdout[-3000:])
    sys.stderr.write(r.stderr[-1500:])
    if r.returncode:
        return False
    try:
        res = json.loads((out_dir / 'last.json').read_text(encoding='utf-8'))['results']
    except Exception as e:
        print('[train] gate exited 0 but its results file %s is unreadable (%s): refusing' % (out_dir / 'last.json', e))
        return False
    if not any(x.get('verdict') == 'PASS' for x in res):
        print('[train] gate exited 0 but no check PASSED (%d skipped or missing inputs): refusing' % len(res))
        return False
    return True


def merge_and_gate(repo, branch, src, label, base, also, template, track_key, dry):
    """Candidate = branch + merge of src, gated. Returns (state, lanes, why). state: 'landed' | 'already' | 'refused' | 'dry'."""
    for attempt in range(3):
        old = git(repo, 'rev-parse', 'refs/heads/' + branch).stdout.strip()
        if git(repo, 'merge-base', '--is-ancestor', src, old, check=False).returncode == 0:
            return 'already', [], 'already_landed'
        lanes = gate_lanes(repo, base, src, also)
        if not lanes:
            print('[train] FAIL: %s touches no gate lane (tools/gate/lanes/*.json) and no default extra lane exists: nothing proves it.' % label)
            return 'refused', [], 'no_gate'
        with Candidate(repo, old) as cand:
            m = git(cand, 'merge', '--no-ff', '--no-edit', '-m', 'train: land %s' % label, src, check=False)
            if m.returncode:
                print('[train] CONFLICT merging %s: %s' % (label, (m.stdout + m.stderr).strip()[-400:]))
                return 'refused', lanes, 'conflict'
            ok = run_gate(cand, lanes, template, repo / 'out' / 'gate_train' / track_key)
            if not ok:
                print('[train] %s: gate FAILED; nothing changed' % label)
                return 'refused', lanes, 'gate_failed'
            if dry:
                print('[train] %s: dry run, gate green; nothing changed' % label)
                return 'dry', lanes, 'dry'
            new = git(cand, 'rev-parse', 'HEAD').stdout.strip()
        if advance(repo, branch, old, new):
            return 'landed', lanes, 'green'
        print('[train] %s moved while gating; redoing from its new tip (attempt %d)' % (branch, attempt + 2))
    return 'refused', lanes, 'branch_kept_moving'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['land', 'up', 'stamp', 'board'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--track', choices=list(TRACKS))
    ap.add_argument('--lane', default='')
    ap.add_argument('--also', default=','.join(DEFAULT_ALSO))
    ap.add_argument('--repo', default='')
    ap.add_argument('--base', default='s102-arc-pt2')
    ap.add_argument('--csv', default='')
    ap.add_argument('--gate-cmd', default=DEFAULT_GATE)
    ap.add_argument('--at', default='')
    ap.add_argument('--dry', action='store_true')
    a = ap.parse_args(argv)
    repo = Path(a.repo) if a.repo else main_checkout()
    csvp = Path(a.csv) if a.csv else repo / 'out' / 's102' / 'trains.csv'
    also = [x for x in a.also.split(',') if x]

    if a.cmd == 'stamp':
        if len(a.args) != 2:
            raise SystemExit('usage: stamp LANE STAGE')
        stamp(csvp, a.args[0], a.args[1], a.track or '', a.at or None)
        print('[train] %s %s %s' % (a.args[0], a.args[1], a.at or 'now'))
        return 0
    if a.cmd == 'board':
        return board(csvp)

    if not a.track:
        raise SystemExit('--track is required for %s' % a.cmd)
    br = ensure_track(repo, a.base, a.track)

    if a.cmd == 'land':
        if len(a.args) != 1 or not a.lane:
            raise SystemExit('usage: land LANE_BRANCH --track T --lane NAME   (--lane is required)')
        lane_br, lane = a.args[0], a.lane
        if not any(r['lane'] == lane for r in read_csv(csvp)):
            raise SystemExit('no csv row named %r in %s: stamp the lane first (train.py stamp %s agent_started) or fix --lane' % (lane, csvp, lane))
        if git(repo, 'rev-parse', '--verify', '--quiet', 'refs/heads/' + lane_br, check=False).returncode:
            raise SystemExit('no such branch %s' % lane_br)
        state, lanes, why = merge_and_gate(repo, br, lane_br, lane, a.base, also, a.gate_cmd, a.track, a.dry)
        if state == 'already':
            print('[train] %s is already on %s: nothing to do' % (lane, br))
            return 0
        if state == 'dry':
            return 0
        if state != 'landed':
            update_row(csvp, lane, lambda r: r.update(track=a.track, gate='FAIL:' + why, note=','.join(lanes)))
            return 1
        stamp(csvp, lane, 'landed_track', a.track, gate='PASS', note=','.join(lanes))
        print('[train] LANDED %s on %s (gate lanes: %s)' % (lane, br, ', '.join(lanes)))
        return 0

    if a.cmd == 'up':
        state, lanes, why = merge_and_gate(repo, a.base, br, 'track ' + a.track, a.base, also, a.gate_cmd, 'up_' + a.track, a.dry)
        if state in ('already', 'dry'):
            print('[train] track %s: %s' % (a.track, 'already on ' + a.base if state == 'already' else 'dry run green'))
            return 0
        if state != 'landed':
            return 1
        t = now()
        for r in read_csv(csvp):
            if r.get('track') == a.track and r.get('landed_track') and not r.get('landed_session'):
                stamp(csvp, r['lane'], 'landed_session', a.track, t)
        print('[train] track %s landed on %s (gate lanes: %s)' % (a.track, a.base, ', '.join(lanes)))
        return 0


def board(p, now_s=None):
    rows = read_csv(p)
    if not rows:
        print('[train] board: %s is missing or empty: nothing to show' % p)
        return 1
    nowt = dt.datetime.strptime(now_s, FMT) if now_s else dt.datetime.now()

    def t(r, k):
        v = r.get(k, '')
        if not v:
            return None
        try:
            return dt.datetime.strptime(v, FMT)
        except ValueError:
            print('WARN: %s.%s=%r is not a time: skipped' % (r['lane'], k, v))
            return None
    pairs = list(zip(STAGES, STAGES[1:]))
    tot = {pr: 0.0 for pr in pairs}
    cnt = {pr: 0 for pr in pairs}
    life = wait = 0.0
    waiting_now = []
    print('%-10s %-5s ' % ('lane', 'track') + ' '.join('%-12s' % ('%s>%s' % (x[:5], y[:5])) for x, y in pairs) + '  wait/life')
    for r in rows:
        cells = []
        for pr in pairs:
            a_, b_ = t(r, pr[0]), t(r, pr[1])
            if a_ and b_ and b_ < a_:
                print('WARN: %s: %s is before %s (out of order): skipped' % (r['lane'], pr[1], pr[0]))
                a_ = None
            if a_ and b_:
                m = (b_ - a_).total_seconds() / 60
                tot[pr] += m
                cnt[pr] += 1
                cells.append('%-12s' % ('%.0f min' % m))
            else:
                cells.append('%-12s' % '-')
        s, h, l = t(r, 'agent_started'), t(r, 'handback'), t(r, 'landed_track')
        share = ''
        if s and h:
            end = l or nowt
            if h >= s and end >= h:
                w, span = (end - h).total_seconds(), (end - s).total_seconds()
                if span > 0:
                    wait += w
                    life += span
                    share = '%.0f%%%s' % (100 * w / span, '' if l else ' (now)')
                if not l:
                    waiting_now.append((r['lane'], w / 60))
            else:
                print('WARN: %s: stamps out of order (agent_started/handback/landed_track): not counted' % r['lane'])
        print('%-10s %-5s ' % (r['lane'][:10], r.get('track', '')) + ' '.join(cells) + '  ' + share)
    print()
    for pr in pairs:
        if cnt[pr]:
            print('  %-28s total %7.0f min over %d lanes (mean %.0f)' % ('%s -> %s' % pr, tot[pr], cnt[pr], tot[pr] / cnt[pr]))
    if life:
        pct = 100 * wait / life
        print('\nfinished-lane wait to land, %% of lane life (first handback -> landed or now, over agent_started -> landed or now): %.1f %%  target < 10 %%  %s'
              % (pct, 'OK' if pct < 10 else 'OVER'))
    else:
        print('\nno lane has agent_started and handback yet: share not computable')
    if waiting_now:
        print('waiting now (handed back, not landed): ' + ', '.join('%s %.0f min' % x for x in sorted(waiting_now, key=lambda x: -x[1])))
    print('not the S101 figure (builders waited 32 %): that counts transcript gaps over 4 min and comes from py -I tools\\harness\\session_time.py; '
          'trains.csv has no transcripts, so the board cannot compute it')
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""Session 102 lanes F1 + F2 proof (gate lane s102_flow): ready_lanes.py, stop_hook.py and train.py, each with planted-fault twins.

  py -I tools\\harness\\flow_selftest.py

Everything runs in a scratch repo / scratch folder under out\\s102_flow_selftest (gitignored); no real branch, hook state or READY.md is touched.
Each twin copies the tool, plants one fault and requires the same scenario to FAIL (a check that cannot fail proves nothing).
Review findings covered: S1 S2 S3 S4 (stop hook), T1-T7 (train), B1-B4 (board).
"""
import contextlib, csv, importlib.util, io, json, os, re, shutil, stat, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SCR = ROOT / 'out' / 's102_flow_selftest'
PY = sys.executable


def rmtree(p):
    def onerr(f, path, exc):
        os.chmod(path, stat.S_IWRITE)
        f(path)
    if Path(p).exists():
        shutil.rmtree(p, onerror=onerr)


def sh(cmd, cwd=None, inp=None, env=None):
    e = dict(os.environ)
    e.update(env or {})
    return subprocess.run(cmd, cwd=cwd, input=inp, capture_output=True, text=True, env=e)


def table(rows):
    s = '# fixture\n\n| Lane | State | Brief | Agent | Started | Notes |\n|---|---|---|---|---|---|\n'
    return s + ''.join('| %s | %s | b.md | | | %s |\n' % (r + ('',) if len(r) == 2 else r) for r in rows)


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# ---- F2 / stop hook -----------------------------------------------------------------------------------------------------------
def hook_scenario(hook, work, real=False):
    """Return a list of failure strings for the recorded Stop inputs."""
    bad = []
    sys.path.insert(0, str(Path(hook).parent))
    mod = None if real else load(hook, 'hook_mut_%d' % abs(hash(str(hook))))
    sys.path.pop(0)
    rmtree(work)
    work.mkdir(parents=True)
    ready = work / 'READY.md'
    state = work / 'state'
    env = {'S102_READY_FILE': str(ready), 'S102_HOOK_DIR': str(state)}

    def run(msg='All done, summary below.', rows=None, reset=True, active=False, sid='s1'):
        if reset:
            rmtree(state)
        if rows is not None:
            ready.write_text(table(rows), encoding='utf-8')
        data = {'session_id': sid, 'hook_event_name': 'Stop', 'stop_hook_active': active, 'last_assistant_message': msg}
        rc, out = call(json.dumps(data))
        if rc != 0:
            bad.append('hook exit code %s (must always be 0)' % rc)
        return json.loads(out) if out else None

    def call(stdin_text):
        """Real subprocess for the shipped hook (real=True); in-process for planted-fault twins (speed: py starts slowly here)."""
        if real:
            r = sh([PY, '-I', str(hook)], inp=stdin_text, env=env)
            return r.returncode, r.stdout.strip()
        old = {k: os.environ.get(k) for k in env}
        os.environ.update(env)
        oldin = sys.stdin
        sys.stdin = type('In', (), {'buffer': io.BytesIO(stdin_text.encode('utf-8'))})()
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = mod.main()
        finally:
            sys.stdin = oldin
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        return rc, buf.getvalue().strip()

    def expect(name, got, want_block, must=''):
        blocked = bool(got and got.get('decision') == 'block')
        if blocked != want_block or (want_block and must not in got.get('reason', '')):
            bad.append('%s: want block=%s got %s' % (name, want_block, got))

    four = [('R%d' % i, 'running') for i in range(4)]
    clean = [('R', 'running'), ('D', 'done')]
    expect('ready=3 running=4', run(rows=[('A1', 'ready'), ('A2', 'ready'), ('A3', 'ready')] + four), True, 'ready lane')
    expect('ready=3 running=6', run(rows=[('A1', 'ready'), ('A2', 'ready'), ('A3', 'ready')] + [('R%d' % i, 'running') for i in range(6)]), False)
    expect('ready=0 running=4', run(rows=four + [('W', 'waiting-input'), ('D', 'done')]), False)
    # S3(d): the narrowed ask rule
    for name, msg, want in (
            ('ends asking to merge', 'T1 is reviewed and green. Should I merge T1?', True),
            ('ends asking to start lane', 'Built. Want me to start the next lane?', True),
            ('release ask passes', 'Checks passed. Release v0.3.3?', False),
            ('push ask passes', 'Ready to push main to GitHub, ok to push?', False),
            ('default ask passes', 'Should the new default for players be ON?', False),
            ('runs beyond grant passes', 'I need 3 extra runs beyond the plan, ok?', False),
            ('delete ask passes', 'May I delete the old captures outside the tidy?', False),
            ('clarifying passes', 'There are two lanes called G1. Which one did you mean?', False),
            ('picture judgement passes', 'The sheet is posted. Does the crowd look right to you?', False),
            ('quoted question passes', 'I merged T1 and T2 and kept the build lane going, as the plan says: "should I merge T1?"', False),
            ('question not at the end passes', 'Should I merge T1? I did, it is green and landed. Moving on to T3.', False),
            ('plain statement passes', 'Merged T1 and T2. Gate green. Moving on to T3.', False),
            ('allowed + forbidden in two sentences blocks', 'Release v0.3.3? Also should I merge T1?', True)):
        expect(name, run(msg, clean), want, 'Jarrod' if want else '')
    # transcript path form (no last_assistant_message)
    tr = work / 't.jsonl'
    tr.write_text(json.dumps({'type': 'assistant', 'message': {'content': [{'type': 'text', 'text': 'Should I merge T9?'}]}}) + '\n', encoding='utf-8')
    rmtree(state)
    ready.write_text(table(clean), encoding='utf-8')
    r = sh([PY, '-I', str(hook)], inp=json.dumps({'hook_event_name': 'Stop', 'session_id': 'tt', 'transcript_path': str(tr)}), env=env)
    expect('transcript form', json.loads(r.stdout) if r.stdout.strip() else None, True, 'Jarrod')
    # no-forever: 3 blocks with the (lane, state) key unchanged -> pass, and (S3c) keep passing until the key changes
    rows = [('A1', 'ready')] + four
    seq = [run(rows=rows)] + [run(reset=False) for _ in range(5)]
    for i, (g, w) in enumerate(zip(seq, [True, True, True, False, False, False])):
        expect('repeat %d' % (i + 1), g, w)
    log = state / 'stop_hook.log'
    if not log.exists() or 'PASS after 3 blocks' not in log.read_text(encoding='utf-8'):
        bad.append('limit pass was not logged')
    expect('after limit, state changed -> blocks again', run(rows=rows + [('A2', 'ready')], reset=False), True)
    # S1: Notes edits do not reset the count (stop_hook_active false, so the second counter is out of play)
    seq = [run(rows=[('A1', 'ready', 'edit %d' % i)] + four, reset=(i == 0)) for i in range(5)]
    if not (seq[0] and seq[1] and seq[2]) or seq[3]:
        bad.append('S1 notes-only edits: want block,block,block,pass got %s' % [bool(x) for x in seq])
    # S1: the second counter: stop_hook_active true, the (lane, state) key changes every call -> passes on or before call 6
    res = []
    for i in range(8):
        res.append(run(rows=[('A%d' % j, 'ready') for j in range(i + 1)] + four, reset=(i == 0), active=True))
    first_pass = next((i + 1 for i, g in enumerate(res) if not g), None)
    if first_pass is None or first_pass > 6:
        bad.append('S1 active cap: first pass at call %s, want <= 6' % first_pass)
    # S3(b): state is per session: B's first call blocks although A is already passing on the same key
    run(rows=rows, reset=True, sid='A')
    for _ in range(4):
        run(reset=False, sid='A')
    expect('S3b other session blocks', run(reset=False, sid='B'), True)
    expect('S3b session A still passing', run(reset=False, sid='A'), False)
    # S3(a): the off switch
    (state / 'stop_hook.off').write_text('off', encoding='utf-8')
    expect('S3a off switch', run(rows=rows, reset=False, sid='C'), False)
    (state / 'stop_hook.off').unlink()
    expect('S3a switch removed', run(rows=rows, reset=False, sid='C'), True)
    # S4: a READY.md with a 0x96 byte -> exit 0 and a log line; garbage stdin -> exit 0
    ready.write_bytes(b'| Lane | State |\n|---|---|\n| A\x96 | ready |\n')
    rmtree(state)
    got = run(reset=False)
    lg = state / 'stop_hook.log'
    if not lg.exists() or 'cannot read/parse READY.md' not in lg.read_text(encoding='utf-8'):
        bad.append('S4: 0x96 byte left no log line')
    r = sh([PY, '-I', str(hook)], inp='not json{{', env=env)
    if r.returncode != 0:
        bad.append('S4: garbage stdin exit %s' % r.returncode)
    # S2: the exec-form wrapper exits 0 even when the script is missing (python itself exits 2 then)
    wrapper = ['-I', '-c', "import runpy,sys\ntry:\n runpy.run_path(sys.argv[1],run_name='__main__')\nexcept BaseException:\n pass\nsys.exit(0)", str(work / 'missing.py')]
    if sh([PY] + wrapper).returncode != 0:
        bad.append('S2: wrapper did not exit 0 on a missing script')
    if sh([PY, '-I', str(work / 'missing.py')]).returncode == 0:
        bad.append('S2: precondition: plain python on a missing script should be non-zero')
    return bad


def parser_checks():
    bad = []
    m = load(HERE / 'ready_lanes.py', 'ready_lanes_t')
    p = SCR / 'parse'
    p.mkdir(parents=True, exist_ok=True)
    f = p / 'READY.md'
    f.write_text(table([('A', 'ready'), ('B', 'running'), ('C', 'running'), ('D', 'done'), ('E', 'waiting-input')]), encoding='utf-8')
    rows, prob = m.parse(f)
    c = m.counts(rows)
    if prob or c['ready'] != 1 or c['running'] != 2 or c['done'] != 1 or c['waiting-input'] != 1:
        bad.append('parser counts wrong: %s %s' % (c, prob))
    for body, why in (('no table here\n', 'no table'), (table([('A', 'redy')]), 'unknown state'), (table([('A', 'ready'), ('A', 'ready')]), 'duplicate')):
        f.write_text(body, encoding='utf-8')
        if sh([PY, '-I', str(HERE / 'ready_lanes.py'), '--check', '--file', str(f)]).returncode == 0:
            bad.append('--check passed a bad file (%s)' % why)
    if sh([PY, '-I', str(HERE / 'ready_lanes.py'), '--check', '--file', str(p / 'missing.md')]).returncode == 0:
        bad.append('--check passed a missing file')
    return bad


# ---- F1 / train ---------------------------------------------------------------------------------------------------------------
STUB_GATE = r"""import json,sys
from pathlib import Path
a=sys.argv
names=a[a.index('--only')+1].split(',')
out=Path(a[a.index('--out')+1]) if '--out' in a else Path('out/gate'); out.mkdir(parents=True,exist_ok=True)
res=[];bad=0
for n in names:
    j=json.loads(Path('tools/gate/lanes/%s.json'%n).read_text())
    if j.get('touch'):
        with open(j['touch'],'a') as f: f.write('gate was here\n')
    v='SKIP' if j.get('skip') else ('PASS' if j['pass'] else 'FAIL')
    bad+=v=='FAIL'; res.append({'lane':n,'verdict':v}); print(n,v)
(out/'last.json').write_text(json.dumps({'results':res}))
sys.exit(1 if bad else 0)
"""


class Scratch:
    def __init__(self, work, train):
        rmtree(work)
        self.repo, self.csv, self.train = work / 'repo', work / 'trains.csv', train
        self.repo.mkdir(parents=True)
        self.g('init', '-q', '-b', 's102-arc-pt2')
        self.g('config', 'user.name', 't')
        self.g('config', 'user.email', 't@t')
        self.g('config', 'commit.gpgsign', 'false')
        (self.repo / 'tools' / 'gate' / 'lanes').mkdir(parents=True)
        (self.repo / 'tools' / 'gate' / 'gate.py').write_text(STUB_GATE, encoding='utf-8')
        (self.repo / '.gitignore').write_text('out/\n')
        (self.repo / 'README').write_text('base\n')
        (self.repo / 'STATUS.md').write_text('status\n')
        self.g('add', '-A')
        self.g('commit', '-qm', 'base')

    def g(self, *a):
        r = sh(['git', '-C', str(self.repo)] + list(a))
        if r.returncode:
            raise SystemExit('scratch git %s failed: %s' % (a, r.stderr))
        return r.stdout.strip()

    def lane(self, name, j, extra_files=()):
        self.g('checkout', '-q', '-b', name, 's102-arc-pt2')
        (self.repo / 'tools' / 'gate' / 'lanes').mkdir(parents=True, exist_ok=True)
        (self.repo / 'tools' / 'gate' / 'lanes' / (name.lower() + '.json')).write_text(json.dumps(j))
        (self.repo / ('file_' + name)).write_text(name)
        for fn in extra_files:
            (self.repo / 'tools' / 'gate' / 'lanes' / fn).write_text(json.dumps({'pass': True}))
        self.g('add', '-A')
        self.g('commit', '-qm', name)
        self.g('checkout', '-q', 's102-arc-pt2')

    def t(self, *a):
        return sh([PY, '-I', str(self.train), '--repo', str(self.repo), '--csv', str(self.csv)] + list(a))

    def land(self, lane, track='rt', stamp=True, *extra):
        if stamp:
            self.t('stamp', lane, 'agent_started', '--track', track, '--at', '2026-11-14 10:00')
            self.t('stamp', lane, 'handback', '--at', '2026-11-14 11:00')
        return self.t('land', lane, '--track', track, '--lane', lane, *extra)

    def rows(self):
        return {x['lane']: x for x in csv.DictReader(open(self.csv, encoding='utf-8'))} if self.csv.exists() else {}

    def files(self, br):
        return set(self.g('ls-tree', '-r', '--name-only', br).split())


def train_scenario(train, work):
    bad = []
    S = Scratch(work / 'x', train)
    S.lane('laneA', {'pass': True})
    S.lane('laneB', {'pass': False, 'touch': 'README'})
    S.lane('laneC', {'pass': True}, extra_files=('s102_budget.json',))
    S.lane('laneN', {'pass': True})  # json removed below: touches no gate json
    S.g('checkout', '-q', 'laneN')
    (S.repo / 'tools' / 'gate' / 'lanes' / 'lanen.json').unlink()
    (S.repo / 'orphan').write_text('x')
    S.g('add', '-A')
    S.g('commit', '-qm', 'laneN no gate')
    S.g('checkout', '-q', 's102-arc-pt2')
    S.lane('laneS', {'pass': True, 'skip': True})  # T7: its only check SKIPs
    ra = S.land('laneA')
    if 'default extra gate lane s102_budget' not in ra.stdout:
        bad.append('T3: absent default lane s102_budget was not announced')
    rc = {'laneA': ra.returncode}
    for lane in ('laneB', 'laneC', 'laneN', 'laneS'):
        rc[lane] = S.land(lane).returncode
    files = S.files('s102-track-rt')
    if rc != {'laneA': 0, 'laneB': 1, 'laneC': 0, 'laneN': 1, 'laneS': 1}:
        bad.append('land exit codes %s' % rc)
    if not {'file_laneA', 'file_laneC'} <= files or files & {'file_laneB', 'orphan', 'file_laneS'}:
        bad.append('track contents wrong: %s' % sorted(files))
    d = S.rows()
    for lane, want in (('laneA', 'PASS'), ('laneC', 'PASS'), ('laneB', 'FAIL:gate_failed'), ('laneN', 'FAIL:no_gate'), ('laneS', 'FAIL:gate_failed')):
        if d.get(lane, {}).get('gate') != want:
            bad.append('%s gate column %r want %r' % (lane, d.get(lane, {}).get('gate'), want))
    if 's102_budget' not in d.get('laneC', {}).get('note', ''):
        bad.append('T3: laneC note lacks s102_budget: %r' % d.get('laneC', {}).get('note'))
    for lane in ('laneB', 'laneN', 'laneS'):
        if d.get(lane, {}).get('landed_track'):
            bad.append('%s was stamped landed' % lane)
    # T4: already an ancestor -> no csv write; a later commit not touching its json lands again
    before = S.rows()['laneA']['landed_track']
    r = S.land('laneA', stamp=False)
    if r.returncode or 'already on' not in r.stdout or S.rows()['laneA']['landed_track'] != before:
        bad.append('T4: re-land of an ancestor wrong: %s' % r.stdout[-200:])
    S.g('checkout', '-q', 'laneA')
    (S.repo / 'extra_file').write_text('more')
    S.g('add', '-A')
    S.g('commit', '-qm', 'laneA round 2')
    S.g('checkout', '-q', 's102-arc-pt2')
    r = S.land('laneA', stamp=False)
    if r.returncode or 'extra_file' not in S.files('s102-track-rt'):
        bad.append('T4: second round of laneA did not land: rc=%s %s' % (r.returncode, r.stdout[-300:]))
    # T6: --lane required, and a name with no csv row is refused
    if S.t('land', 'laneC', '--track', 'rt').returncode == 0:
        bad.append('T6: land without --lane passed')
    if S.t('land', 'laneC', '--track', 'rt', '--lane', 'nosuchrow').returncode == 0:
        bad.append('T6: land with an unknown csv row passed')
    # T1: `up` never touches the base checkout. Red gate first: a dirty tracked edit + a gate that edits a tracked file.
    S.g('branch', 's102-track-game', 'laneB')               # a track whose gate is red
    (S.repo / 'tools' / 'gate' / 'lanes').mkdir(exist_ok=True)
    (S.repo / 'STATUS.md').write_text('status\nJARROD UNCOMMITTED EDIT\n')
    head0 = S.g('rev-parse', 'HEAD')
    r = S.t('up', '--track', 'game')
    if r.returncode == 0 or S.g('rev-parse', 'HEAD') != head0 or 'JARROD UNCOMMITTED EDIT' not in (S.repo / 'STATUS.md').read_text():
        bad.append('T1 red: the dirty edit or HEAD changed, or up passed: rc=%s' % r.returncode)
    if 'gate was here' in (S.repo / 'README').read_text():
        bad.append('T1 red: the gate ran in the base checkout')
    # green gate, still dirty: HEAD advances (fast-forward), the dirty file is untouched
    r = S.t('up', '--track', 'rt')
    base_files = S.files('s102-arc-pt2')
    if r.returncode or not {'file_laneA', 'file_laneC'} <= base_files or 'file_laneB' in base_files:
        bad.append('up failed or wrong: rc=%s %s %s' % (r.returncode, sorted(base_files), r.stdout[-300:]))
    if S.g('rev-parse', 'HEAD') == head0 or 'JARROD UNCOMMITTED EDIT' not in (S.repo / 'STATUS.md').read_text():
        bad.append('T1 green: HEAD did not advance or the dirty file changed')
    if not (S.repo / 'file_laneA').exists():
        bad.append('T1 green: the checkout was not updated')
    rows2 = S.rows()
    if not rows2.get('laneA', {}).get('landed_session') or rows2.get('laneB', {}).get('landed_session'):
        bad.append('landed_session stamps wrong')
    leftovers = [l for l in S.g('worktree', 'list').splitlines() if 'cand_' in l]
    if leftovers:
        bad.append('candidate worktrees left behind: %s' % leftovers)
    return bad


def interrupt_check(train, work):
    """T2: an exception or Ctrl-C between the merge and the gate result leaves the track exactly as it was, no scratch worktree."""
    bad = []
    S = Scratch(work / 'x', train)
    S.lane('laneA', {'pass': True})
    S.t('stamp', 'laneA', 'agent_started', '--track', 'rt')
    S.g('branch', 's102-track-rt', 's102-arc-pt2')
    before = S.g('rev-parse', 's102-track-rt')
    m = load(train, 'train_t2')

    def boom(*a, **k):
        raise KeyboardInterrupt()
    m.run_gate = boom
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            m.main(['land', 'laneA', '--track', 'rt', '--lane', 'laneA', '--repo', str(S.repo), '--csv', str(S.csv)])
        bad.append('T2: KeyboardInterrupt did not propagate')
    except KeyboardInterrupt:
        pass
    if S.g('rev-parse', 's102-track-rt') != before:
        bad.append('T2: the track moved after an interrupted gate')
    if [l for l in S.g('worktree', 'list').splitlines() if 'cand_' in l]:
        bad.append('T2: scratch worktree left behind')
    return bad


def csv_checks(train, work):
    bad = []
    rmtree(work)
    work.mkdir(parents=True)
    c = work / 'p.csv'
    procs = [subprocess.Popen([PY, '-I', str(train), '--csv', str(c), 'stamp', 'lane%02d' % i, 'brief_written'], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL) for i in range(20)]
    for p in procs:
        p.wait()
    n = len(list(csv.DictReader(open(c, encoding='utf-8')))) if c.exists() else 0
    if n != 20:
        bad.append('T5: 20 parallel stamps gave %d rows' % n)
    return bad


def board_check(train, work):
    bad = []
    rmtree(work)
    work.mkdir(parents=True)
    c = work / 'b.csv'
    m = load(train, 'train_board')

    def T(*a):
        return sh([PY, '-I', str(train), '--csv', str(c)] + list(a))

    def board(now=None):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            m.board(c, now)
        return buf.getvalue()

    # B3 + B4: first handback kept, later rounds recorded, "YYYY-MM-DD HH:MM" accepted: 10:00 start, 11:00 / 11:50 handbacks, 12:00 landed -> 50 %
    for stage, at in (('agent_started', '10:00'), ('handback', '11:00'), ('handback', '11:50'), ('landed_track', '12:00')):
        T('stamp', 'multi', stage, '--at', '2026-11-14 ' + at)
    row = next(csv.DictReader(open(c, encoding='utf-8')))
    if row['handback'] != '2026-11-14 11:00:00' or row['rounds'] != '2026-11-14 11:50:00':
        bad.append('B3: first handback not kept / round not recorded: %s %s' % (row['handback'], row['rounds']))
    out = board()
    mm = re.search(r'landed or now\): ([\d.]+) %', out)
    if not mm or abs(float(mm.group(1)) - 50.0) > 0.1:
        bad.append('B3: share should be 50 %%: %s' % out[-500:])
    if 'wait to land' not in out or 'session_time.py' not in out:
        bad.append('B1: board does not label the measure / point at session_time.py')
    if T('stamp', 'x', 'handback', '--at', 'yesterday').returncode == 0:
        bad.append('B4: a bad --at was accepted')
    # B2: handed back, not landed: counted to now, listed as waiting now
    T('stamp', 'stuck', 'agent_started', '--at', '2026-11-14 10:00')
    T('stamp', 'stuck', 'handback', '--at', '2026-11-14 11:00')
    out = board('2026-11-14 12:00:00')
    if not re.search(r'waiting now.*stuck 60 min', out):
        bad.append('B2: stuck lane not listed as waiting now: %s' % out[-300:])
    mm = re.search(r'landed or now\): ([\d.]+) %', out)
    if not mm or abs(float(mm.group(1)) - 50.0) > 0.1:   # both lanes 60/120
        bad.append('B2: the stuck lane is not in the share: %s' % (mm.group(1) if mm else out[-200:]))
    # B4: a bad cell and out-of-order stamps: warnings, no crash, never over 100 %
    rows = list(csv.DictReader(open(c, encoding='utf-8')))
    rows[0]['review_done'] = 'garbage'
    m.write_csv(c, rows + [{'lane': 'backwards', 'agent_started': '2026-11-14 12:00:00', 'handback': '2026-11-14 11:00:00', 'landed_track': '2026-11-14 13:00:00'}])
    out = board('2026-11-14 12:00:00')
    if 'WARN' not in out or 'garbage' not in out or 'out of order' not in out:
        bad.append('B4: no warning for bad cell / out-of-order: %s' % out[-400:])
    for v in re.findall(r'([\d.]+) %', out):
        if float(v) > 100:
            bad.append('B4: a share over 100 %')
    if m.board(work / 'nope.csv') == 0:
        bad.append('board on a missing csv returned 0')
    return bad


def twin(src_name, old, new, scenario_fn, label, work):
    src = (HERE / src_name).read_text(encoding='utf-8')
    if old not in src:
        return ['twin %s: fault anchor not found in %s' % (label, src_name)]
    rmtree(work)
    work.mkdir(parents=True)
    mut = work / ('mut_' + src_name)
    mut.write_text(src.replace(old, new, 1), encoding='utf-8')
    shutil.copy(HERE / 'ready_lanes.py', work / 'ready_lanes.py')
    return [] if scenario_fn(mut, work / 'run') else ['twin %s: the planted fault was NOT caught' % label]


def main():
    rmtree(SCR)
    SCR.mkdir(parents=True)
    results = []

    def step(name, bad):
        print('%-44s %s' % (name, 'PASS' if not bad else 'FAIL'))
        for b in bad:
            print('    -', b)
        results.append(bool(bad))

    caught_h = lambda m, w: bool(hook_scenario(m, w))
    caught_t = lambda m, w: bool(train_scenario(m, w))
    step('ready_lanes parser', parser_checks())
    step('stop_hook scenarios (S1-S4)', hook_scenario(HERE / 'stop_hook.py', SCR / 'hook'))
    step('train land/up scenario (T1 T3 T4 T6 T7)', train_scenario(HERE / 'train.py', SCR / 'train'))
    step('train interrupt (T2)', interrupt_check(HERE / 'train.py', SCR / 'int'))
    step('train csv lock (T5)', csv_checks(HERE / 'train.py', SCR / 'csv'))
    step('train board (B1-B4)', board_check(HERE / 'train.py', SCR / 'board'))
    for label, old, new in (
            ('hook ignores running count', "c['running'] < MIN_RUNNING", 'False'),
            ('hook never checks the ask', 'if not ASK.search(s):\n        return None', 'if False:\n        return None'),
            ('hook blocks forever on one key', 'if n > MAX_BLOCKS:', 'if False:'),
            ('hook has no stop_hook_active cap', 'if consec >= MAX_ACTIVE_BLOCKS:', 'if False:'),
            ('hook key includes Notes', 'ready_lanes.digest_states(rows)', 'ready_lanes.digest(rf)'),
            ('hook keeps counting after the limit pass', "if s.get('passing_key') == key:", 'if False:'),
            ('hook ignores the off switch', "if (hook_dir() / 'stop_hook.off').exists():", 'if False:'),
            ('hook shares state across sessions', "sid = str(data.get('session_id') or 'nosession')", "sid = 'x'"),
            ('hook blocks quoted questions', "t = QUOTED.sub(' ', t)", 't = t'),
            ('hook blocks clarifying questions', 'or CLARIFY.search(s)', 'or False')):
        step('twin: ' + label, twin('stop_hook.py', old, new, caught_h, label, SCR / ('tw_' + re.sub(r'\W', '', label)[:24])))
    for label, old, new, fn in (
            ('train ignores a red gate', 'if not ok:\n                print', 'if False:\n                print', caught_t),
            ('train accepts a gate with no PASS', "if not any(x.get('verdict') == 'PASS' for x in res):", 'if False:', caught_t),
            ('train: handback overwritten (B3)', "if stage == 'handback' and row.get('handback'):", 'if False:', lambda m, w: bool(board_check(m, w))),
            ('train: csv not locked (T5)', 'with Lock(p):', 'if True:', lambda m, w: bool(csv_checks(m, w))),
            ('train: interrupt leaves track moved (T2)', 'ok = run_gate(cand, lanes, template, repo / \'out\' / \'gate_train\' / track_key)',
             'advance(repo, branch, old, git(cand, \'rev-parse\', \'HEAD\').stdout.strip()); ok = run_gate(cand, lanes, template, repo / \'out\' / \'gate_train\' / track_key)',
             lambda m, w: bool(interrupt_check(m, w)))):
        step('twin: ' + label, twin('train.py', old, new, fn, label, SCR / ('tw_' + re.sub(r'\W', '', label)[:24])))
    rmtree(SCR)
    print('flow selftest: %s' % ('FAIL' if any(results) else 'PASS'))
    return 1 if any(results) else 0


if __name__ == '__main__':
    sys.exit(main())

"""Session 102 lanes F1 + F2 proof: ready_lanes.py and stop_hook.py, each with planted-fault twins.

  py -I tools\\harness\\flow_selftest.py

Everything runs in a scratch repo / scratch folder under out\\s102_flow_selftest (gitignored); no real branch, hook state or READY.md is touched.
Each twin copies the tool, plants one fault and requires the same scenario to FAIL (a check that cannot fail proves nothing).
Review findings covered: S1 S2 S3 S4 (stop hook).
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
    step('ready_lanes parser', parser_checks())
    step('stop_hook scenarios (S1-S4)', hook_scenario(HERE / 'stop_hook.py', SCR / 'hook'))
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
    rmtree(SCR)
    print('flow selftest: %s' % ('FAIL' if any(results) else 'PASS'))
    return 1 if any(results) else 0


if __name__ == '__main__':
    sys.exit(main())

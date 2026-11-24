r"""Plants for tools\harness\board.py (Session 106 lane B1). Small synthetic inputs, no transcript, no game, no network. Exit 1 on any failed plant.

    py -I tools\harness\board_selftest.py

Each verdict the board can give is planted in its UNSAFE direction (can it say "free" / "counted" / "no problem" when the thing is wrong?) and
with a positive control (the same input made right must give the right answer):
  rows      a row whose need is unmerged is NOT free (control: merged need -> FREE from that merge's time; a running agent -> not FREE)
  lanes     a lane whose only hand-back is a failed status SHOWS it (control: a completed hand-back is not shown as failed)
  logs      a game log outside the window is not counted (control: inside counted; a name that is not wwe2k14_NNN.log is not counted)
  fixed     --write-fixed leaves every byte outside the markers identical, in CRLF and LF files, with and without markers; a second write is stable
  stream    a task notification with status failed read from a transcript file shows as failed; an Agent start with no notification is still counted
  kinds     "fix round" / "adversary ... fix" / "builder" descriptions get the right kind
  time      utc_to_local goes through the OS zone rules
"""
# entry: write_fixed
import json
import os
import sys
import tempfile
import time
from datetime import datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import board  # noqa: E402

FAILS = []


def check(name, cond, detail=''):
    print('%s  %s%s' % ('ok  ' if cond else 'FAIL', name, ('  ' + detail) if (detail and not cond) else ''))
    if not cond:
        FAILS.append(name)


T0 = datetime(2026, 11, 21, 10, 0, 0)


def agent(aid, desc, start, done=(), send=(), kind=None, tag=None):
    return {'id': aid, 'start': start, 'desc': desc, 'type': '', 'wf': False, 'done': list(done), 'send': list(send), 'tool_use': 'tu_' + aid,
            'branch': 'worktree-agent-' + aid, 'model': '', 'worktree': True, 'finished_meta': False, 'mtime': None,
            'kind': kind or board.kind_of(desc), 'tag': tag if tag is not None else board.tag_of(desc), 'merges': [], 'first_commit': None, 'merged_in': []}


def rows_spec():
    spec = {'rows': [
        {'id': 'X1', 'needs': [], 'match': [board.re.compile(r'(?<![A-Za-z0-9])X1(?![A-Za-z0-9])')], 'settle': 'merge'},
        {'id': 'X2', 'needs': ['X1'], 'match': [board.re.compile(r'(?<![A-Za-z0-9])X2(?![A-Za-z0-9])')], 'settle': 'merge'},
        {'id': 'X3', 'needs': ['X1', 'X2'], 'match': [board.re.compile(r'(?<![A-Za-z0-9])X3(?![A-Za-z0-9])')], 'settle': 'merge'},
        {'id': 'P1', 'needs': ['X1|go:P1'], 'match': [board.re.compile(r'(?<![A-Za-z0-9])P1(?![A-Za-z0-9])')], 'settle': 'merge'},
    ]}
    for r in spec['rows']:
        r['_rx'] = r['match']
    return spec


def run_rows(agents, merges, go=()):
    spec = rows_spec()
    ag = {a['id']: a for a in agents}
    board.assign_rows(spec['rows'], ag, merges)
    board.compute_rows(spec['rows'], ag, merges, T0, T0 + timedelta(hours=3), go)
    return {r['id']: r for r in spec['rows']}


def t_rows():
    now = T0 + timedelta(hours=3)
    # X1 handed back but NOT merged: X2 and X3 must not be free (unsafe direction)
    a1 = agent('a1', 'SON builder X1 thing', T0 + timedelta(minutes=5), done=[(T0 + timedelta(minutes=30), 'completed')])
    r = run_rows([a1], [])
    check('rows: unmerged need -> dependent row not FREE', r['X2']['state'] != 'FREE' and r['X2']['free_from'] is None, r['X2']['state'])
    check('rows: unmerged need -> blocked_on names it', r['X2']['blocked_on'] == ['X1'], str(r['X2']['blocked_on']))
    check('rows: X1 itself (no needs, started) is started, not free', r['X1']['state'] == 'started', r['X1']['state'])
    r0 = run_rows([], [])
    check('rows: no agent, no needs -> FREE since the session start', r0['X1']['state'] == 'FREE' and r0['X1']['free_from'] == T0, r0['X1']['state'])
    # control: X1 merged at T0+50 -> X2 FREE from then, X3 still blocked on X2
    mg = [{'t': T0 + timedelta(minutes=50), 'sha': 'abc', 'src': 'worktree-agent-a1', 'first_commit': None}]
    r = run_rows([a1], mg)
    check('rows: merged need -> row FREE from the merge time', r['X2']['state'] == 'FREE' and r['X2']['free_from'] == T0 + timedelta(minutes=50), r['X2']['state'])
    check('rows: two needs, one unmerged -> not FREE', r['X3']['state'] == 'blocked' and r['X3']['blocked_on'] == ['X2'], r['X3']['state'])
    # a row an agent already started is not free
    a2 = agent('a2', 'SON builder X2 thing', T0 + timedelta(minutes=60))
    r = run_rows([a1, a2], mg)
    check('rows: a row with a running agent is running, not FREE', r['X2']['state'] == 'running', r['X2']['state'])
    check('rows: FREE list excludes started rows', 'X2' not in [x['id'] for x in board.free_rows(list(r.values()))])
    # a running agent matching X1 keeps X1 from counting as done even after a merge
    a1r = agent('a1', 'SON builder X1 thing', T0 + timedelta(minutes=5), done=[(T0 + timedelta(minutes=30), 'completed')], send=[T0 + timedelta(minutes=55)])
    r = run_rows([a1r], mg)
    check('rows: a follow-up in flight keeps the need unmet', r['X2']['state'] != 'FREE', r['X2']['state'])
    # go:P1 alternative
    r = run_rows([], [], go=['P1'])
    check('rows: a go: need is satisfied by --go', r['P1']['state'] == 'FREE')
    r = run_rows([a1], [])
    check('rows: an alternative need unmet stays blocked', r['P1']['state'] == 'blocked', r['P1']['state'])


def t_lanes():
    now = T0 + timedelta(hours=2)
    failed = agent('f1', 'SON builder F thing', T0, done=[(T0 + timedelta(minutes=20), 'failed')])
    st, wait, ws = board.lane_state(failed, now, None, None, None)
    check('lanes: a failed hand-back is shown FAILED', 'FAILED' in st, st)
    ok = agent('f2', 'SON builder G thing', T0, done=[(T0 + timedelta(minutes=20), 'completed')])
    st, wait, ws = board.lane_state(ok, now, None, None, None)
    check('lanes: a completed hand-back is not shown failed', 'FAILED' not in st, st)
    check('lanes: completed, no review -> waits for a review', st == 'handed back, not reviewed', st)
    run = agent('f3', 'SON builder H thing', T0)
    st, wait, ws = board.lane_state(run, now, None, None, None)
    check('lanes: no hand-back yet -> running', st == 'running', st)
    run2 = agent('f4', 'SON builder I thing', T0, done=[(T0 + timedelta(minutes=20), 'completed')], send=[T0 + timedelta(minutes=30)])
    st, wait, ws = board.lane_state(run2, now, None, None, None)
    check('lanes: a follow-up after the hand-back -> running again', st == 'running', st)
    mg = {'t': T0 + timedelta(minutes=90)}
    st, wait, ws = board.lane_state(ok, now, mg, None, [])
    check('lanes: merged with a check log and no check line -> not checked', st == 'merged, not checked', st)
    st, wait, ws = board.lane_state(ok, now, mg, None, [T0 + timedelta(minutes=100)])
    check('lanes: merged and a later check line -> checked', st == 'merged, checked', st)
    st, wait, ws = board.lane_state(ok, now, mg, None, [T0 + timedelta(minutes=10)])
    check('lanes: a check line BEFORE the merge does not count', st == 'merged, not checked', st)
    st, wait, ws = board.lane_state(ok, now, mg, None, None)
    check('lanes: merged with no check log says so (not "checked")', st == 'merged' and 'no check log' in wait, st)
    rv = {'started': T0 + timedelta(minutes=25), 'done': T0 + timedelta(minutes=45)}
    st, wait, ws = board.lane_state(ok, now, None, rv, None)
    check('lanes: review done, no follow-up -> fix not started', st == 'review done, fix not started', st)


def t_logs():
    with tempfile.TemporaryDirectory() as d:
        logs = os.path.join(d, 'out', 'build', 'x', 'logs')
        os.makedirs(logs)
        t0 = datetime(2026, 11, 20, 16, 30)
        t1 = datetime(2026, 11, 21, 2, 10)

        def mk(name, when):
            p = os.path.join(logs, name)
            open(p, 'w').write('x')
            ts = time.mktime(when.timetuple())
            os.utime(p, (ts, ts))
        mk('wwe2k14_001.log', datetime(2026, 11, 20, 12, 0))      # before the window
        mk('wwe2k14_002.log', datetime(2026, 11, 20, 18, 0))      # inside
        mk('wwe2k14_003.log', datetime(2026, 11, 21, 1, 0))       # inside
        mk('wwe2k14_004.log', datetime(2026, 11, 21, 3, 0))       # after the window
        mk('events_005.jsonl', datetime(2026, 11, 20, 18, 0))     # not a game log
        mk('wwe2k14_006.log.1', datetime(2026, 11, 20, 18, 0))    # not a game log
        per = board.count_logs(d, t0, t1)
        total, parts = board.describe_logs(per)
        check('logs: only the two inside the window are counted', total == 2, '%d %s' % (total, parts))
        check('logs: outside-window logs are not counted', all(n in (2, 3) for v in per.values() for n, _ in v))


def t_fixed():
    block = 'line one\nline two'
    for nl in ('\r\n', '\n'):
        base = nl.join(['# Summary', '', 'body 1', 'body 2', '']) + 'tail' + nl
        out = board.write_fixed(base, block)
        check('fixed(%r): markers added at the top, the original follows untouched' % nl, out.endswith(base) and out.startswith(board.MARK_A), out[:60])
        # now with markers present and different content inside, plus text before and after
        doc = nl.join(['# Top', '', board.MARK_A, 'old a', 'old b', 'old c', board.MARK_B, '', 'after 1', 'after 2', '']) + 'last'
        out2 = board.write_fixed(doc, block)
        lines_in = doc.splitlines(keepends=True)
        lines_out = out2.splitlines(keepends=True)
        ia = [i for i, l in enumerate(lines_out) if l.strip() == board.MARK_A][0]
        ib = [i for i, l in enumerate(lines_out) if l.strip() == board.MARK_B][0]
        outside_in = lines_in[:3] + lines_in[6:]
        outside_out = lines_out[:ia + 1] + lines_out[ib:]
        check('fixed(%r): every line outside the markers is byte-identical' % nl, outside_in == outside_out)
        check('fixed(%r): the inside is the new block' % nl, ''.join(lines_out[ia + 1:ib]) == nl.join(block.split('\n')) + nl, repr(lines_out[ia + 1:ib]))
        check('fixed(%r): a second write is stable' % nl, board.write_fixed(out2, block) == out2)
        check('fixed(%r): same line count outside the markers' % nl, len(outside_in) == len(outside_out))
    # planted fault: a file whose markers are inverted is treated as unmarked (never edits between a wrong pair)
    # (S106 B1F: inverted markers now REFUSE instead of being treated as unmarked: see t_f6_write_fixed)
    # the file writer end to end, byte-level, on a CRLF file
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 's.md')
        raw = b'# S\r\n\r\nstate\r\n' + board.MARK_A.encode() + b'\r\nold\r\n' + board.MARK_B.encode() + b'\r\nafter\r\n'
        open(p, 'wb').write(raw)
        with open(p, encoding='utf-8', newline='') as f:
            text = f.read()
        new = board.write_fixed(text, block)
        open(p, 'w', encoding='utf-8', newline='').write(new)
        got = open(p, 'rb').read()
        check('fixed: bytes outside the markers survive a CRLF file round trip', got.startswith(b'# S\r\n\r\nstate\r\n') and got.endswith(b'\r\nafter\r\n') and b'\n' not in got.replace(b'\r\n', b''))


def t_stream():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 't.jsonl')
        lines = []
        for i, (tu, desc) in enumerate((('toolu_1', 'SON builder S1 x'), ('toolu_2', 'SON builder S2 y'))):
            lines.append(json.dumps({'type': 'assistant', 'timestamp': '2026-11-20T06:%02d:00.000Z' % i, 'message': {'id': 'msg_%d' % i, 'content': [
                {'type': 'tool_use', 'id': tu, 'name': 'Agent', 'input': {'description': desc, 'subagent_type': 'general-purpose', 'prompt': 'SECRET TEXT'}}]}}, separators=(',', ':')))
        lines.append(json.dumps({'type': 'queue-operation', 'operation': 'enqueue', 'timestamp': '2026-11-20T06:30:00.000Z',
                                 'content': '<task-notification><task-id>aid1</task-id><tool-use-id>toolu_1</tool-use-id><status>failed</status></task-notification>'}, separators=(',', ':')))
        open(p, 'w').write('\n'.join(lines) + '\n')
        tr = board.read_transcript(p)
        ag = board.build_agents(tr, {})
        check('stream: both Agent starts are agents (one has no notification)', len(ag) == 2, str(list(ag)))
        check('stream: the notified one shows status failed', ag['aid1']['done'][0][1] == 'failed', str(ag['aid1']['done']))
        st, wait, ws = board.lane_state(ag['aid1'], datetime(2026, 11, 21, 12, 0), None, None, None)
        check('stream: ... and the lane state says FAILED', 'FAILED' in st, st)
        check('stream: the un-notified one is running', board.agent_running([a for a in ag.values() if a['id'] != 'aid1'][0], datetime(2026, 11, 21, 12, 0)))
        check('stream: prompt text is not kept', 'SECRET' not in json.dumps({k: str(v) for k, v in tr['uses'].items()}))


def t_kinds():
    k = board.kind_of
    check('kinds: fix round', k('SON fix round A6S counters') == 'fix')
    check('kinds: adversary with the word fix in it', k('OPUS adversary A5d Threads fix') == 'adversary')
    check('kinds: builder', k('SON builder A7 mod attires') == 'builder')
    check('kinds: review', k('OPUS review C0') == 'review')
    check('kinds: probe', k('OPUS probe A1 resolution') == 'probe')
    check('tag: builder tag', board.tag_of('SON builder A4B previews') == 'A4B')
    check('tag: fix round tag', board.tag_of('SON fix round C2L-A materials') == 'C2L-A')


def t_time():
    s = '2026-11-20T05:33:47.123Z'
    want = datetime.fromtimestamp(datetime.strptime(s[:19], '%Y-%m-%dT%H:%M:%S').replace(tzinfo=board.timezone.utc).timestamp())
    check('time: utc_to_local equals the OS conversion', board.utc_to_local(s) == want, '%s vs %s' % (board.utc_to_local(s), want))


# ================================================================ S106 B1F: plants for the review's 12 BLOCK findings (docs\research\s106\review_B1.md)
import subprocess  # noqa: E402


def _git(d, *a, env=None, check=True):
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run(['git', '-C', d] + list(a), capture_output=True, text=True, env=e)
    if check and r.returncode != 0:
        raise RuntimeError('git %s: %s' % (a, r.stderr))
    return r.stdout.strip()


def _at(h, m, day=21, month=11):
    s = '2026-%02d-%02dT%02d:%02d:00Z' % (month, day, h, m)
    return {'GIT_AUTHOR_DATE': s, 'GIT_COMMITTER_DATE': s}


def _utc(h, m, day=21):
    return board.utc_to_local('2026-11-%02dT%02d:%02d:00' % (day, h, m))


def _commit(d, name, when, msg=None):
    open(os.path.join(d, name), 'w').write(name)
    _git(d, 'add', name)
    _git(d, 'commit', '-q', '-m', msg or name, env=when)


def t_f1_merges():
    """BLOCK 1 (+12): merges without a 'Merge branch X into s106' commit are seen; first commits ignore history older than the lane."""
    with tempfile.TemporaryDirectory() as d:
        _git(d, 'init', '-q', '-b', 's106')
        _git(d, 'config', 'user.email', 'a@b.c')
        _git(d, 'config', 'user.name', 'T')
        _commit(d, 'c0', _at(9, 0))
        c0 = _git(d, 'rev-parse', 'HEAD')
        # A: fast-forward
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-aaa')
        _commit(d, 'a1', _at(9, 10))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--ff-only', 'worktree-agent-aaa', env=_at(9, 30))
        # B: merged into a staging branch, then s106 fast-forwards to it
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-bbb')
        _commit(d, 'b1', _at(9, 35))
        _git(d, 'checkout', '-q', '-b', 'stg', 's106')
        _git(d, 'merge', '-q', '--no-ff', 'worktree-agent-bbb', '-m', "Merge branch 'worktree-agent-bbb' into stg", env=_at(9, 40))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--ff-only', 'stg', env=_at(9, 50))
        # C: --no-ff merge whose message names no target
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-ccc')
        _commit(d, 'c1', _at(9, 55))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--no-ff', 'worktree-agent-ccc', '-m', "Merge branch 'worktree-agent-ccc'", env=_at(10, 0))
        # P: carries a commit from last month (public-history style) before its own work
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-ppp', c0)
        _commit(d, 'old', _at(9, 0, 18, 10))
        _commit(d, 'p1', _at(10, 10))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--no-ff', 'worktree-agent-ppp', '-m', "Merge branch 'worktree-agent-ppp' into s106", env=_at(10, 20))
        # D: never merged
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-ddd')
        _commit(d, 'd1', _at(10, 25))
        _git(d, 'checkout', '-q', 's106')
        into, _ = board.read_merges('s106', _utc(8, 0), _utc(12, 0), repo=d)
        by = {m['src']: m for m in into}
        check('f1: fast-forwarded lane A is seen', 'worktree-agent-aaa' in by, str(list(by)))
        check('f1: lane B (merged into a staging branch, s106 fast-forwarded) is seen', 'worktree-agent-bbb' in by, str(list(by)))
        check('f1: lane C (merge message names no target) is seen', 'worktree-agent-ccc' in by, str(list(by)))
        check('f1: a never-merged lane D is not a merge', 'worktree-agent-ddd' not in by, str(list(by)))
        check('f1: A lands at the fast-forward time (reflog 09:30)', by.get('worktree-agent-aaa', {}).get('t') == _utc(9, 30), str(by.get('worktree-agent-aaa')))
        check('f1: B lands when s106 took it (09:50)', by.get('worktree-agent-bbb', {}).get('t') == _utc(9, 50), str(by.get('worktree-agent-bbb')))
        check('f1: C lands at its merge (10:00)', by.get('worktree-agent-ccc', {}).get('t') == _utc(10, 0), str(by.get('worktree-agent-ccc')))
        check('f1: the staging / base branches are not lane merges', 'stg' not in by and 's106' not in by, str(list(by)))
        # finding 12: first commit ignores commits older than the lane's start
        fc = board.lane_first_commit('worktree-agent-ppp', 's106', _utc(9, 55), repo=d)
        check('f12: first commit ignores the month-old commit (10:10)', fc == _utc(10, 10), str(fc))
        fc2 = board.lane_first_commit('worktree-agent-ppp', 's106', _utc(11, 0), repo=d)
        check('f12: no commit after the start -> None, never a negative day', fc2 is None, str(fc2))
        fc3 = board.lane_first_commit('worktree-agent-aaa', 's106', _utc(9, 5), repo=d)
        check('f1: first commit of a fast-forwarded lane (09:10)', fc3 == _utc(9, 10), str(fc3))


def t_f1b_landings():
    """S106 B1F2: landing times come from the session branch's reflog (review_round2 B1 / B2 + 2 FOLLOW)."""
    # (a) main --no-ff merges the session branch, then the session branch fast-forwards to main: lanes keep their earlier times
    with tempfile.TemporaryDirectory() as d:
        _git(d, 'init', '-q', '-b', 'main')
        _git(d, 'config', 'user.email', 'a@b.c')
        _git(d, 'config', 'user.name', 'T')
        _commit(d, 'c0', _at(9, 0))
        _git(d, 'checkout', '-q', '-b', 's106')
        for nm, hh, mm in (('aaa', 9, 10), ('bbb', 9, 40)):
            _git(d, 'checkout', '-q', '-b', 'worktree-agent-' + nm, 's106')
            _commit(d, nm + '1', _at(hh, mm))
            _git(d, 'checkout', '-q', 's106')
            _git(d, 'merge', '-q', '--no-ff', 'worktree-agent-' + nm, '-m', "Merge branch 'worktree-agent-%s' into s106 (fix round; x)" % nm, env=_at(hh, mm + 5))
        _git(d, 'checkout', '-q', 'main')
        _git(d, 'merge', '-q', '--no-ff', 's106', '-m', "Merge branch 's106'", env=_at(11, 0))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--ff-only', 'main', env=_at(11, 5))
        into, _ = board.read_merges('s106', _utc(8, 0), _utc(12, 0), repo=d)
        by = {m['src']: m for m in into}
        check('f1b(a): lane A keeps its own landing (09:15), not the main merge', by.get('worktree-agent-aaa', {}).get('t') == _utc(9, 15), str(by.get('worktree-agent-aaa')))
        check('f1b(a): lane B keeps 09:45', by.get('worktree-agent-bbb', {}).get('t') == _utc(9, 45), str(by.get('worktree-agent-bbb')))
        ch = board.session_chain('s106', _utc(8, 0), repo=d)
        h = next((x for x, t in ch if t <= _utc(9, 20)), None)
        def anc(x, y):
            return subprocess.run(['git', '-C', d, 'merge-base', '--is-ancestor', x, y]).returncode == 0
        check('f1b(a): session_chain at 09:20 is the state with lane A only', h is not None and anc('worktree-agent-aaa', h) and not anc('worktree-agent-bbb', h), str(ch))
    # (b) the staging-branch shape: staging merges A then B, the session branch ff-only later: both land at the ff time
    with tempfile.TemporaryDirectory() as d:
        _git(d, 'init', '-q', '-b', 's106')
        _git(d, 'config', 'user.email', 'a@b.c')
        _git(d, 'config', 'user.name', 'T')
        _commit(d, 'c0', _at(9, 0))
        for nm, hh, mm in (('aaa', 9, 10), ('bbb', 9, 12)):
            _git(d, 'checkout', '-q', '-b', 'worktree-agent-' + nm, 's106')
            _commit(d, nm + '1', _at(hh, mm))
        _git(d, 'checkout', '-q', '-b', 'stg', 's106')
        _git(d, 'merge', '-q', '--no-ff', 'worktree-agent-aaa', '-m', "Merge branch 'worktree-agent-aaa' into stg", env=_at(9, 20))
        _git(d, 'merge', '-q', '--no-ff', 'worktree-agent-bbb', '-m', "Merge branch 'worktree-agent-bbb' into stg", env=_at(9, 25))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--ff-only', 'stg', env=_at(9, 50))
        into, _ = board.read_merges('s106', _utc(8, 0), _utc(12, 0), repo=d)
        by = {m['src']: m for m in into}
        check('f1b(b): both staged lanes are seen', 'worktree-agent-aaa' in by and 'worktree-agent-bbb' in by, str(list(by)))
        check('f1b(b): lane A lands at the ff time 09:50', by.get('worktree-agent-aaa', {}).get('t') == _utc(9, 50), str(by.get('worktree-agent-aaa')))
        check('f1b(b): lane B lands at the ff time 09:50', by.get('worktree-agent-bbb', {}).get('t') == _utc(9, 50), str(by.get('worktree-agent-bbb')))
    # (c) a reset in the reflog (containment going backwards)
    with tempfile.TemporaryDirectory() as d:
        _git(d, 'init', '-q', '-b', 's106')
        _git(d, 'config', 'user.email', 'a@b.c')
        _git(d, 'config', 'user.name', 'T')
        _commit(d, 'c0', _at(9, 0))
        c0 = _git(d, 'rev-parse', 'HEAD')
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-aaa')
        _commit(d, 'a1', _at(9, 10))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--ff-only', 'worktree-agent-aaa', env=_at(9, 30))
        _git(d, 'reset', '-q', '--hard', c0, env=_at(9, 40))
        _git(d, 'checkout', '-q', '-b', 'worktree-agent-bbb')
        _commit(d, 'b1', _at(9, 45))
        _git(d, 'checkout', '-q', 's106')
        _git(d, 'merge', '-q', '--ff-only', 'worktree-agent-bbb', env=_at(10, 0))
        into, _ = board.read_merges('s106', _utc(8, 0), _utc(12, 0), repo=d)
        by = {m['src']: m for m in into}
        check('f1b(c): lane B lands 10:00 after a reset', by.get('worktree-agent-bbb', {}).get('t') == _utc(10, 0), str(by.get('worktree-agent-bbb')))
        check('f1b(c): lane A (reset away, no longer on the branch) is not a merge', 'worktree-agent-aaa' not in by, str(list(by)))


def _write_transcript(p, rows):
    open(p, 'w', encoding='utf-8').write('\n'.join(json.dumps(r, separators=(',', ':')) for r in rows) + '\n')


def t_f2_session():
    """BLOCK 2: the default session is chosen by the last INTERNAL timestamp, preferring the rows' branch; two live transcripts refuse."""
    with tempfile.TemporaryDirectory() as d:
        def mk(name, ts_last, branch, tail_no_ts=False):
            rows = [{'type': 'user', 'timestamp': '2026-11-21T01:00:00.000Z', 'gitBranch': branch},
                    {'type': 'assistant', 'timestamp': ts_last, 'gitBranch': branch}]
            if tail_no_ts:
                rows.append({'type': 'cost-state', 'x': 1})   # timestamp-less line: bumps the file's write time only
            _write_transcript(os.path.join(d, name + '.jsonl'), rows)
        mk('old1', '2026-11-20T10:00:00.000Z', 's105', tail_no_ts=True)
        time.sleep(0.05)
        mk('mine', '2026-11-21T01:00:30.000Z', 's106')
        time.sleep(0.05)
        mk('old2', '2026-11-20T10:05:00.000Z', 's101', tail_no_ts=True)   # the newest file by write time, but old inside
        now = board.utc_to_local('2026-11-21T12:30:00')
        got = board.pick_session(d, 's106', now=now)
        check('f2: newest by INTERNAL timestamp, not by write time', got == 'mine', str(got))
        got2 = board.pick_session(d, 's105', now=now)
        check("f2: the rows' branch is preferred", got2 == 'old1', str(got2))
        mk('liveA', '2026-11-21T01:20:00.000Z', 'main')
        mk('liveB', '2026-11-21T01:25:00.000Z', 'main')
        try:
            board.pick_session(d, 'zzz', now=board.utc_to_local('2026-11-21T01:30:00'))
            refused = False
        except SystemExit:
            refused = True
        check('f2: two transcripts written in the last 30 min and no matching branch -> refuses', refused)


def t_f3_clock():
    """BLOCK 3: an idle main session does not end the window early."""
    real = datetime(2026, 11, 21, 12, 0)
    last = real - timedelta(minutes=40)
    live, now = board.clock_state(last, real, [], None)
    check('f3: no fixed window, transcript idle 40 min -> now is the real now', now == real, str(now))
    check('f3: ... and the label is idle, not live', live is False)
    live2, now2 = board.clock_state(last, real, [real - timedelta(minutes=3)], None)
    check('f3: a subagent written 3 min ago keeps it LIVE', live2 is True and now2 == real)
    with tempfile.TemporaryDirectory() as d:
        logs = os.path.join(d, 'out', 'build', 'x', 'logs')
        os.makedirs(logs)
        p = os.path.join(logs, 'wwe2k14_001.log')
        open(p, 'w').write('x')
        ts = time.mktime((real - timedelta(minutes=5)).timetuple())
        os.utime(p, (ts, ts))
        per = board.count_logs(d, last - timedelta(hours=3), now)
        check('f3: a game log 5 min old is counted (was 0 with now = last write)', board.describe_logs(per)[0] == 1)


def _wf_tree(base, session, run, agents):
    wd = os.path.join(base, session, 'subagents', 'workflows', run)
    os.makedirs(wd)
    jl = []
    for aid, label, wt in agents:
        json.dump({'agentType': 'workflow-subagent', 'worktreePath': wt, 'spawnedWithWorktree': True, 'description': label, 'model': 'sonnet'},
                  open(os.path.join(wd, 'agent-%s.meta.json' % aid), 'w'))
        open(os.path.join(wd, 'agent-%s.jsonl' % aid), 'w').write('{}\n')
        jl.append({'type': 'started', 'agentId': aid, 'label': label, 'phase': 'Check'})
        jl.append({'type': 'result', 'agentId': aid, 'result': {}})
    open(os.path.join(wd, 'journal.jsonl'), 'w').write('\n'.join(json.dumps(x) for x in jl) + '\n')


def t_f4_f10_workflow():
    """BLOCK 4 (workflow lanes) and 10 (a failed Agent launch is not alive)."""
    with tempfile.TemporaryDirectory() as d:
        sess = 'sess1'
        _wf_tree(d, sess, 'wf_b5d2733e-0b0', [('aa39', 'b2-check', 'C:\\x\\.claude\\worktrees\\wf_b5d2733e-0b0-1')])
        wfres = ('Workflow launched in background. Task ID: wz44\nSummary: B2 check: does a workflow worktree start at the tip\n'
                 'Transcript dir: ' + os.path.join(d, sess, 'subagents', 'workflows', 'wf_b5d2733e-0b0') + '\nRun ID: wf_b5d2733e-0b0\nYou will be notified')
        rows = [
            {'type': 'assistant', 'timestamp': '2026-11-20T23:40:16.000Z', 'message': {'id': 'msg_1', 'content': [
                {'type': 'tool_use', 'id': 'toolu_wf', 'name': 'Workflow', 'input': {'script': 'SECRET SCRIPT'}}]}},
            {'type': 'user', 'timestamp': '2026-11-20T23:40:17.000Z', 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'toolu_wf', 'content': wfres, 'is_error': False}]}},
            {'type': 'queue-operation', 'operation': 'enqueue', 'timestamp': '2026-11-20T23:40:33.000Z',
             'content': '<task-notification><task-id>wz44</task-id><tool-use-id>toolu_wf</tool-use-id><status>completed</status></task-notification>'},
            {'type': 'assistant', 'timestamp': '2026-11-20T23:41:00.000Z', 'message': {'id': 'msg_2', 'content': [
                {'type': 'tool_use', 'id': 'toolu_bad', 'name': 'Agent', 'input': {'description': 'SON builder Z9 never started', 'subagent_type': 'lane-builder', 'prompt': 'x'}}]}},
            {'type': 'user', 'timestamp': '2026-11-20T23:41:01.000Z', 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'toolu_bad',
                                                                                            'content': "Agent type 'lane-builder' not found", 'is_error': True}]}},
        ]
        tp = os.path.join(d, sess + '.jsonl')
        _write_transcript(tp, rows)
        tr = board.read_transcript(tp)
        subs = board.read_subagents(d, sess)
        wfs = board.read_workflows(d, sess, tr)
        ag = board.build_agents(tr, subs, wfs)
        wl = [a for a in ag.values() if a.get('wf')]
        check('f4: the workflow run is a lane with its summary as description', len(wl) == 1 and 'B2 check' in wl[0]['desc'], str([a['desc'] for a in ag.values()]))
        check('f4: ... it is not shown as running (it completed)', bool(wl) and not board.agent_running(wl[0], datetime(2026, 11, 21, 12, 0)))
        check('f4: ... its worktree branch is worktree-<dirname>', bool(wl) and wl[0].get('branch') == 'worktree-wf_b5d2733e-0b0-1', str(wl and wl[0].get('branch')))
        check('f4: ... its kind is workflow', bool(wl) and wl[0]['kind'] == 'workflow', str(wl and wl[0]['kind']))
        check('f10: a failed Agent launch (is_error) is not an agent', not any('Z9' in a['desc'] for a in ag.values()), str([a['desc'] for a in ag.values()]))
        check('f4: script text is not kept', 'SECRET' not in json.dumps({k: str(v) for k, v in tr['uses'].items()}))


def t_f5_checklog():
    """BLOCK 5: a check-log line counts for its row=ID only."""
    line = "2026-11-21 11:00:00 row=P arm=AFTER run=out/x:012 verdict=ok question=don't test whether T, C1 and B1 hold"
    check('f5: row=P matches P', board.check_hits(line, ['P']))
    check('f5: ... and not T / C1 / B1 named in the question text', not any(board.check_hits(line, [k]) for k in ('T', 'C1', 'B1')))
    check('f5: no row= field: a whole token matches', board.check_hits('2026-11-21 11:00:00 ran C1 check', ['C1']))
    check('f5: no row= field: a substring does not', not board.check_hits('2026-11-21 11:00:00 ran C10 check', ['C1']))
    check('f5: row= is compared exactly, case-insensitively', board.check_hits('2026-11-21 11:00:00 row=b1 arm=BEFORE', ['B1']) and not board.check_hits('2026-11-21 11:00:00 row=B12 arm=BEFORE', ['B1']))


def t_f5b_pending():
    """C2 probe: row=A1 never matches a row=A12 line; a `verdict=pending` line (written before a launch) does not make a row CHECKED."""
    a12 = '2026-11-21 11:00:00 row=A12 arm=AFTER run=pending verdict=ok question=x'
    check('f5b: row=A1 does not match a row=A12 line', not board.check_hits(a12, ['A1']) and board.check_hits(a12, ['A12']))
    pend = '2026-11-21 11:00:00 row=A1 arm=AFTER run=pending verdict=pending question=x'
    done = '2026-11-21 11:09:00 row=A1 arm=AFTER run=out/x:003 verdict=ok question=x'
    check('f5b: a pending line is not a finished check', not board.check_hits(pend, ['A1'], done_only=True) and board.check_hits(pend, ['A1']))
    check('f5b: the line written after the run counts', board.check_hits(done, ['A1'], done_only=True))
    st, _, _ = board.lane_state(agent('c1', 'SON builder A1 x', T0, done=[(T0 + timedelta(minutes=20), 'completed')]), T0 + timedelta(hours=2),
                                {'t': T0 + timedelta(minutes=30)}, None, [T0 + timedelta(minutes=40) for l in (pend,) if board.check_hits(l, ['A1'], done_only=True)])
    check('f5b: merged with only a pending line -> not checked', st == 'merged, not checked', st)


def _refuses(text, block):
    try:
        board.write_fixed(text, block)
        return False
    except ValueError:
        return True


def t_f6_write_fixed():
    """BLOCK 6: --write-fixed never deletes text outside a well-formed marker pair."""
    block = 'a\nb'
    body = ['line %d' % i for i in range(200)]
    doc = '\n'.join(['# S', board.MARK_A, 'old'] + body) + '\n'
    check('f6: start marker without end marker refuses (the running log must survive)', _refuses(doc, block))
    two = '\n'.join(['# S', board.MARK_A, 'x', board.MARK_B, 'mid', board.MARK_A, 'y', board.MARK_B, 'z']) + '\n'
    check('f6: two marker pairs refuse', _refuses(two, block))
    fenced = '\n'.join(['# S', '```', board.MARK_A, 'x', board.MARK_B, '```', 'after']) + '\n'
    check('f6: a marker pair inside a code fence refuses', _refuses(fenced, block))
    inv = '# T\n' + board.MARK_B + '\nx\n' + board.MARK_A + '\n'
    check('f6: inverted markers refuse', _refuses(inv, block))
    big = '\n'.join(['# S', board.MARK_A] + ['old %d' % i for i in range(40)] + [board.MARK_B, 'tail']) + '\n'
    check('f6: a block of 40 lines is not the fixed block (refuses)', _refuses(big, block))
    bom = '\ufeff# S\n' + board.MARK_A + '\nold\n' + board.MARK_B + '\nrest\n'
    out = board.write_fixed(bom, block)
    check('f6: a BOM before the first line keeps everything outside', out.startswith('\ufeff# S\n' + board.MARK_A) and out.endswith(board.MARK_B + '\nrest\n') and 'old' not in out, repr(out[:60]))
    bom2 = '\ufeff' + board.MARK_A + '\nold\n' + board.MARK_B + '\nrest\n'
    out = board.write_fixed(bom2, block)
    check('f6: a BOM on the start-marker line itself does not hide it', out.startswith('\ufeff' + board.MARK_A) and 'old' not in out and out.endswith('rest\n') and out.count(board.MARK_A) == 1, repr(out[:80]))
    mixed = '# S\r\nl1\nl2\r\n' + board.MARK_A + '\r\nold\r\n' + board.MARK_B + '\nafter\r\nend\n'
    out = board.write_fixed(mixed, block)
    check('f6: mixed line endings outside the markers survive byte for byte', out.startswith('# S\r\nl1\nl2\r\n' + board.MARK_A) and out.endswith(board.MARK_B + '\nafter\r\nend\n'), repr(out))
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'f.md')
        raw = b'# S\r\n' + board.MARK_A.encode() + b'\r\nold\r\n' + board.MARK_B.encode() + b'\r\nafter\r\n'
        open(p, 'wb').write(raw)
        board.write_file_fixed(p, block, roots=[d])
        got = open(p, 'rb').read()
        check('f6: file writer keeps the bytes outside the markers (CRLF)', got.startswith(b'# S\r\n' + board.MARK_A.encode() + b'\r\n') and got.endswith(board.MARK_B.encode() + b'\r\nafter\r\n'))
        check('f6: file writer leaves no temp file behind', os.listdir(d) == ['f.md'], str(os.listdir(d)))
        try:
            board.write_file_fixed(p, block, roots=[os.path.join(d, 'sub')])
            check('f6: a path outside the allowed roots is refused', False)
        except SystemExit:
            check('f6: a path outside the allowed roots is refused', True)
        open(p, 'wb').write(('# S\n' + board.MARK_A + '\nonly start\n' + 'x\n' * 50).encode())
        before = open(p, 'rb').read()
        try:
            board.write_file_fixed(p, block, roots=[d])
        except (SystemExit, ValueError):
            pass
        check('f6: a refused write leaves the file untouched', open(p, 'rb').read() == before)
    check('f6: --fixed prints no markers', board.MARK_A not in board.fixed_text_for_cli('x') and board.MARK_B not in board.fixed_text_for_cli('x'))


def t_f7_reviews():
    """BLOCK 7: a review is tied to its lane by the row / token, also a review that started before the hand-back."""
    rows = ['B1', 'C2']
    lane = agent('b1x', 'SON B1 the board', T0, done=[(T0 + timedelta(minutes=60), 'completed')])
    lane['row'] = 'B1'
    rv = agent('r1', 'OPUS review B1 board', T0 + timedelta(minutes=30), done=[(T0 + timedelta(minutes=50), 'completed')])
    other = agent('r2', 'OPUS review C2 recipes', T0 + timedelta(minutes=30), done=[(T0 + timedelta(minutes=50), 'completed')])
    c = board.review_of_lane(lane, [rv, other], rows, T0 + timedelta(hours=3))
    check('f7: "SON B1 the board" (empty tag) is linked to "OPUS review B1" by the row token', c is not None and c['started'] == rv['start'], str(c))
    check("f7: a review that started before the lane's hand-back counts", c is not None and c['done'] == T0 + timedelta(minutes=50))
    check('f7: a review of another row is not linked', board.review_of_lane(lane, [other], rows, T0 + timedelta(hours=3)) is None)


def t_f8_classes():
    """BLOCK 8/9: builder class by meta, not by name; probe rows take probe kinds only, build rows builder class only."""
    a = agent('x1', 'SON B1 the board', T0, kind='other')
    a['worktree'] = True
    a['type'] = 'lane-builder'
    check('f8: an S106-named builder (kind other, worktree, lane-builder) is builder class', board.is_builder_class(a))
    f = agent('x2', 'SON fix round B1F', T0)
    f['worktree'] = True
    check('f8: a fix round is builder class', board.is_builder_class(f))
    p = agent('x3', 'OPUS C4 lane exe build path', T0, kind='probe')
    p['worktree'] = False
    p['type'] = 'general-purpose'
    check('f8: a probe whose name has "lane" is not builder class', not board.is_builder_class(p))
    check('f8: classify: no worktree, regex says builder, not first word -> not builder', board.classify('OPUS C4 lane exe build path', 'general-purpose', False) != 'builder')
    check('f8: classify: lane-builder type, kind other -> builder', board.classify('SON B1 the board', 'lane-builder', True) == 'builder')
    check('f8: classify: a review stays a review whatever the type', board.classify('OPUS review B1', 'reviewer', False) == 'review')
    spec = {'rows': [
        {'id': 'C4p', 'needs': [], 'match': [board.re.compile('C4 lane exe', board.re.I)], 'settle': 'done', '_kinds': board.PROBE_KINDS},
        {'id': 'C4', 'needs': ['C4p'], 'match': [board.re.compile(r'(?<![A-Za-z0-9])C4(?![A-Za-z0-9])')], 'settle': 'merge', '_kinds': board.BUILD_KINDS},
    ]}
    for r in spec['rows']:
        r['_rx'] = r['match']
    bld = agent('x4', 'SON builder C4 lane exe', T0, kind='builder')
    ag = {bld['id']: bld, p['id']: p}
    board.assign_rows(spec['rows'], ag, [])
    check('f9: a builder is not stolen by the probe row whose literal it contains', bld.get('row') == 'C4', str(bld.get('row')))
    check('f9: a probe is not taken by the build row', p.get('row') == 'C4p', str(p.get('row')))
    spec3 = {'rows': [{'id': 'C1', 'needs': [], 'match': [board.re.compile(r'(?<![A-Za-z0-9])C1(?![A-Za-z0-9])')], 'settle': 'merge', '_kinds': board.BUILD_KINDS}]}
    spec3['rows'][0]['_rx'] = spec3['rows'][0]['match']
    pc = agent('x6', 'OPUS probe C1', T0, kind='probe', done=[(T0 + timedelta(minutes=30), 'completed')])
    board.assign_rows(spec3['rows'], {pc['id']: pc}, [])
    board.compute_rows(spec3['rows'], {pc['id']: pc}, [], T0, T0 + timedelta(hours=2), ())
    check('f9: a finished probe leaves its build row "probed", not "started"', spec3['rows'][0]['state'] == 'probed', spec3['rows'][0]['state'])
    check('f9: ... and the row is still on the free-to-start list', 'C1' in [r['id'] for r in board.free_rows(spec3['rows'])])


def t_f8_bars():
    """BLOCK 8: bar 3 (waits)."""
    iv = [(0, 10), (5, 20), (30, 40)]
    check('bar3: merged union of wait intervals (no double count)', board.union_seconds(iv) == 30, str(board.union_seconds(iv)))


def t_f11_logs():
    """BLOCK 11: logs under release\\build and wt\\..., Jarrod's own play split from test runs."""
    with tempfile.TemporaryDirectory() as d:
        def mk(rel, when, udr):
            p = os.path.join(d, 'out', rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'w').write('[info] [wwe2k14] user_data_root: %s\n' % udr)
            ts = time.mktime(when.timetuple())
            os.utime(p, (ts, ts))
        w = datetime(2026, 11, 21, 3, 0)
        mk('build/x/logs/wwe2k14_001.log', w, 'C:\\r\\out\\bench\\userdata_work_s2')
        mk('release/build/logs/wwe2k14_002.log', w, 'C:\\r\\out\\bench\\userdata_frozen_a')
        mk('wt/foo/out/build/y/logs/wwe2k14_003.log', w, 'C:\\r\\out\\bench\\userdata_frozen_a')
        mk('build/x/logs/wwe2k14_004.log', w, 'C:\\Users\\Jarrod\\AppData\\Roaming\\wwe2k14')   # Jarrod's own play
        per = board.count_logs(d, datetime(2026, 11, 21, 0, 0), datetime(2026, 11, 21, 6, 0))
        total, parts = board.describe_logs(per)
        test, other = board.split_runs(per)
        check('f11: all four logs found (release\\build and wt\\... included)', total == 4, '%d %s' % (total, parts))
        check("f11: test runs 3, other (Jarrod's play) 1", (test, other) == (3, 1), '%s %s' % (test, other))


def main():
    for f in (t_rows, t_lanes, t_logs, t_fixed, t_stream, t_kinds, t_time, t_f1_merges, t_f1b_landings, t_f2_session, t_f3_clock, t_f4_f10_workflow,
              t_f5_checklog, t_f5b_pending, t_f6_write_fixed, t_f7_reviews, t_f8_classes, t_f8_bars, t_f11_logs):
        try:
            f()
        except Exception as e:   # a crashing plant is a failed plant
            import traceback
            traceback.print_exc()
            FAILS.append(f.__name__ + ' crashed: ' + repr(e))
            print('FAIL  %s crashed' % f.__name__)
    print('board_selftest: %s (%d failed)' % ('FAIL' if FAILS else 'PASS', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

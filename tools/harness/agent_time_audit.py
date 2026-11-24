r"""Agent wall-time audit (Session 103, WTA). Reads every subagent transcript (.output / .jsonl) of a day, splits each into
rounds (a new user message = a resumed round), and prints, per agent and per round: wall time, minutes in tool calls by kind
(compile, check_run, q, q_update, context_pack, read_search, edit, sleep_wait, git, other), model time (gap between a
result and the next call), output tokens and call count. Then a ranked table of the biggest sinks. Aggregates and short
labels only; never prints message text beyond the first 60 characters of a prompt (the agent's name line).

    py tools\harness\agent_time_audit.py [--dir DIR] [--since 2026-11-18T03:00] [--utc-offset 11] [--csv out.csv] [--top 15]
    py tools\harness\agent_time_audit.py --selftest

Default DIR = the newest `tasks` folder under %LOCALAPPDATA%\Temp\claude\*\*\tasks. Times in the transcripts are UTC;
--since is local time (UTC+offset). Re-run it at the end of every session (Jarrod: audits are how we stay efficient).
Note: a call that ran in the background returns at once, so its real duration is NOT in "tool" time (it shows as model time
or as a later Monitor / wait call): the table says so under "not measured".
"""
import argparse, csv, glob, json, os, re, sys, tempfile
from collections import defaultdict
from datetime import datetime, timedelta, timezone

KINDS = ['compile', 'check_run', 'q_update', 'context_pack', 'q', 'sleep_wait', 'read_search', 'edit', 'git', 'other']


def classify(name, inp):
    if name in ('Edit', 'Write', 'NotebookEdit'):
        return 'edit'
    if name in ('Read', 'Grep', 'Glob'):
        return 'read_search'
    if name in ('Monitor', 'TaskOutput'):
        return 'sleep_wait'
    if name in ('Bash', 'PowerShell'):
        c = (inp.get('command') or '')
        cl = c.lower()
        if re.search(r'\bsleep\s+\d|start-sleep|time\.sleep|wait-process|timeout\s+/t|waitfor\.py|tail\s+(-n\s*\S+\s+)?-f|until\s+grep|ping\s+-n\s+\d{2,}', cl):
            return 'sleep_wait'
        if 'context_pack' in cl:
            return 'context_pack'
        if re.search(r'q\.py\s+ours\s+update|q\s+ours\s+update', cl):
            return 'q_update'
        if re.search(r'build\.ps1|fast_build|build_rig|cmake|ninja|clang|cl\.exe|msbuild|\bbuild_|compile|cargo|make\b', cl):
            return 'compile'
        if re.search(r'tools[\\/]q[\\/]q\.py|\bq\.py\b|py\s+-I\s+tools[\\/]q', cl):
            return 'q'
        if re.search(r'rig|owned_check|_check\.py|check_|selftest|pytest|replay|bench|run_list|host_rig|upl_check|det_', cl):
            return 'check_run'
        if re.search(r'^\s*(git|gh)\b|\bgit\s', cl[:40]):
            return 'git'
        if re.search(r'\b(grep|rg|sed|cat|head|tail|ls|find|wc|awk|type|get-content|select-string)\b', cl[:60]):
            return 'read_search'
        return 'other'
    if name == 'Skill':
        return 'other'
    return 'other'


def load(path):
    recs = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                o = json.loads(line)
                if isinstance(o, dict):
                    recs.append(o)
            except ValueError:
                pass
    return recs


def ts(r):
    t = r.get('timestamp')
    if not t:
        return None
    return datetime.fromisoformat(t.replace('Z', '+00:00'))


def is_round_start(r):
    if r.get('type') != 'user':
        return False
    c = (r.get('message') or {}).get('content')
    if isinstance(c, str):
        return True
    if isinstance(c, list):
        return any(x.get('type') == 'text' for x in c) and not any(x.get('type') == 'tool_result' for x in c)
    return False


def analyse(recs):
    """-> (name, [round dicts])"""
    rounds, cur, name = [], None, ''
    open_calls = {}
    for r in recs:
        t = ts(r)
        if is_round_start(r) and t:
            if cur is None or cur['calls'] > 0 or cur['first'] is None:
                if not name:
                    c = r['message']['content']
                    if isinstance(c, list):
                        c = ' '.join(x.get('text', '') for x in c if x.get('type') == 'text')
                    name = re.sub(r'\s+', ' ', c)[:60]
                cur = dict(first=t, last=t, calls=0, tool=defaultdict(float), cnt=defaultdict(int), model=0.0,
                           out_tok=0, in_tok=0, prev_end=t, longs=[])
                rounds.append(cur)
            continue
        if cur is None or not t:
            continue
        cur['last'] = max(cur['last'], t)
        m = r.get('message') or {}
        u = m.get('usage') or {}
        cur['out_tok'] += u.get('output_tokens', 0) or 0
        c = m.get('content')
        if not isinstance(c, list):
            continue
        for x in c:
            if x.get('type') == 'tool_use':
                inp = x.get('input') or {}
                k = classify(x.get('name'), inp)
                if inp.get('run_in_background'):
                    k = k  # returns at once; real time is unmeasured
                if t > cur['prev_end']:
                    cur['model'] += (t - cur['prev_end']).total_seconds()
                open_calls[x['id']] = (cur, t, k, x.get('name'), re.sub(r'\s+', ' ', str(inp.get('command') or inp.get('description') or inp.get('file_path') or ''))[:70])
                cur['calls'] += 1
                cur['cnt'][k] += 1
            elif x.get('type') == 'tool_result':
                o = open_calls.pop(x.get('tool_use_id'), None)
                if o:
                    rc, s, k, nm, lab = o
                    d = max(0.0, (t - s).total_seconds())
                    rc['tool'][k] += d
                    rc['prev_end'] = max(rc['prev_end'], t)
                    if d > 120:
                        rc['longs'].append((d, k, lab))
                    cur['prev_end'] = max(cur['prev_end'], t)
    return name, rounds


def mins(x):
    return x / 60.0


def run(args):
    d = args.dir
    if not d:
        cands = glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Temp', 'claude', '*', '*', 'tasks'))
        d = max(cands, key=os.path.getmtime) if cands else ''
    files = sorted(glob.glob(os.path.join(d, '*.output')) + glob.glob(os.path.join(d, '*.jsonl')))
    since = None
    if args.since:
        since = datetime.fromisoformat(args.since).replace(tzinfo=timezone(timedelta(hours=args.utc_offset)))
    rows, sink = [], defaultdict(lambda: [0.0, set()])
    for f in files:
        if os.path.getsize(f) < 2000:
            continue
        recs = load(f)
        name, rounds = analyse(recs)
        if not rounds or (since and rounds[0]['first'] < since):
            continue
        aid = os.path.basename(f).split('.')[0].replace('agent-', '')
        for i, r in enumerate(rounds):
            wall = (r['last'] - r['first']).total_seconds()
            tool = sum(r['tool'].values())
            row = dict(agent=aid, name=name, round=i + 1, start=r['first'].astimezone(timezone(timedelta(hours=args.utc_offset))).strftime('%H:%M'),
                       wall=mins(wall), tool=mins(tool), model=mins(r['model']), calls=r['calls'], out_tok=r['out_tok'],
                       **{k: mins(r['tool'].get(k, 0.0)) for k in KINDS}, longs=r['longs'])
            rows.append(row)
            for k in KINDS:
                sink[k][0] += row[k]
                sink[k][1].add(aid)
            sink['model'][0] += row['model']; sink['model'][1].add(aid)
    print(f'{len(rows)} rounds in {len(set(r["agent"] for r in rows))} agents (dir {d}, since {args.since})')
    print('%-6s %-28s %2s %5s %6s %6s %6s %5s | %s' % ('start', 'agent', 'r', 'calls', 'wall', 'tool', 'model', 'tok_k', ' '.join('%7s' % k[:7] for k in KINDS)))
    for r in sorted(rows, key=lambda r: -r['wall'])[:args.top * 3]:
        print('%-6s %-28s %2d %5d %6.1f %6.1f %6.1f %5d | %s' % (r['start'], (r['name'].split(',')[0] + ' ' + r['agent'][:5])[:28], r['round'], r['calls'],
              r['wall'], r['tool'], r['model'], r['out_tok'] // 1000, ' '.join('%7.1f' % r[k] for k in KINDS)))
    print('\nRanked sinks (minutes across all agents, agents involved):')
    for k, (m, ag) in sorted(sink.items(), key=lambda x: -x[1][0])[:args.top]:
        print(f'  {k:13s} {m:8.1f} min  {len(ag):3d} agents')
    print('\nLong calls (> 2 min), longest first:')
    L = [(d_, r['name'].split(',')[0] + ' ' + r['agent'][:5], k, lab) for r in rows for (d_, k, lab) in r['longs']]
    for d_, n, k, lab in sorted(L, reverse=True)[:args.top * 2]:
        print(f'  {mins(d_):5.1f} min  {n:26s} {k:11s} {lab}')
    if args.csv:
        with open(args.csv, 'w', newline='', encoding='utf-8') as fh:
            w = csv.writer(fh)
            cols = ['agent', 'name', 'round', 'start', 'wall', 'tool', 'model', 'calls', 'out_tok'] + KINDS
            w.writerow(cols)
            for r in rows:
                w.writerow([('%.2f' % r[c]) if isinstance(r[c], float) else r[c] for c in cols])
    return rows


def selftest():
    t0 = datetime(2026, 11, 18, 1, 0, tzinfo=timezone.utc)
    def T(s):
        return (t0 + timedelta(seconds=s)).isoformat().replace('+00:00', 'Z')
    recs = [
        {'type': 'user', 'timestamp': T(0), 'message': {'role': 'user', 'content': 'Builder X, Session 1 do things'}},
        {'type': 'assistant', 'timestamp': T(10), 'message': {'usage': {'output_tokens': 100}, 'content': [{'type': 'tool_use', 'id': 'a', 'name': 'Bash', 'input': {'command': 'py tools\\q\\q.py ours update'}}]}},
        {'type': 'user', 'timestamp': T(100), 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'a'}]}},
        {'type': 'assistant', 'timestamp': T(130), 'message': {'content': [{'type': 'tool_use', 'id': 'b', 'name': 'Bash', 'input': {'command': 'pwsh tools\\build.ps1'}}]}},
        {'type': 'user', 'timestamp': T(430), 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'b'}]}},
        {'type': 'user', 'timestamp': T(1000), 'message': {'role': 'user', 'content': 'fix round please'}},
        {'type': 'assistant', 'timestamp': T(1010), 'message': {'content': [{'type': 'tool_use', 'id': 'c', 'name': 'Edit', 'input': {}}]}},
        {'type': 'user', 'timestamp': T(1011), 'message': {'content': [{'type': 'tool_result', 'tool_use_id': 'c'}]}},
    ]
    name, rounds = analyse(recs)
    assert len(rounds) == 2, rounds
    r1 = rounds[0]
    assert abs(r1['tool']['q_update'] - 90) < 1 and abs(r1['tool']['compile'] - 300) < 1, dict(r1['tool'])
    assert abs(r1['model'] - 40) < 1, r1['model']   # 10 s before call a + 30 s before call b
    assert rounds[1]['calls'] == 1 and rounds[1]['tool']['edit'] == 1
    assert classify('Bash', {'command': 'sleep 280'}) == 'sleep_wait'
    assert classify('Bash', {'command': 'py tools\\q\\context_pack.py b'}) == 'context_pack'
    print('selftest ok')


OOM_RE = re.compile(r'out of memory|std::bad_alloc|bad allocation|LLVM ERROR: out of memory|cannot allocate memory|C1060|C3859|not enough (space|memory)|fatal error: .*memory|ran out of memory|Stack dump', re.I)
SLOT_RE = re.compile(r'waiting for (a )?(gpu )?slot|slot[s]? (busy|full|taken)|queued|queue position|waiting for .*lock|run lock|runlock', re.I)


def contention(args):
    """Concurrency vs duration of compile calls, OOM hits and slot / lock waits. Prints aggregates."""
    d = args.dir
    files = sorted(glob.glob(os.path.join(d, '*.jsonl')) + glob.glob(os.path.join(d, '*.output')))
    since = datetime.fromisoformat(args.since).replace(tzinfo=timezone(timedelta(hours=args.utc_offset)))
    spans, calls, oom, slot = [], [], [], []
    for f in files:
        if os.path.getsize(f) < 20000:
            continue
        recs = load(f)
        ts_ = [ts(r) for r in recs if ts(r)]
        if not ts_ or ts_[0] < since:
            continue
        aid = os.path.basename(f).split('.')[0].replace('agent-', '')
        spans.append((ts_[0], ts_[-1], aid))
        op = {}
        for r in recs:
            t = ts(r); c = (r.get('message') or {}).get('content')
            if not t or not isinstance(c, list):
                continue
            for x in c:
                if x.get('type') == 'tool_use':
                    op[x['id']] = (t, classify(x.get('name'), x.get('input') or {}), re.sub(r'\s+', ' ', str((x.get('input') or {}).get('command') or ''))[:80])
                elif x.get('type') == 'tool_result' and x.get('tool_use_id') in op:
                    s, k, lab = op.pop(x['tool_use_id'])
                    body = x.get('content')
                    if isinstance(body, list):
                        body = ' '.join(b.get('text', '') for b in body if isinstance(b, dict))
                    body = str(body or '')
                    if k == 'compile':
                        calls.append((s, (t - s).total_seconds(), k, aid, lab))
                    if OOM_RE.search(body):
                        oom.append((s, (t - s).total_seconds(), aid, lab, OOM_RE.search(body).group(0)))
                    if SLOT_RE.search(body):
                        slot.append((s, (t - s).total_seconds(), aid, lab, SLOT_RE.search(body).group(0)))
    def conc(t):
        return sum(1 for a, b, _ in spans if a <= t <= b)
    print(f'{len(spans)} agents; compile calls {len(calls)}')
    for kind in ('compile',):
        b = defaultdict(list)
        for s, dur, k, aid, lab in calls:
            if k == kind and 20 < dur < 3000:
                n = conc(s); b['1-3' if n <= 3 else '4-6' if n <= 6 else '7-9' if n <= 9 else '10+'].append(dur)
        for key in ('1-3', '4-6', '7-9', '10+'):
            v = sorted(b.get(key, []))
            if v:
                print(f'  {kind:8s} agents alive {key:5s} n={len(v):3d} median {v[len(v)//2]/60:5.1f} min  mean {sum(v)/len(v)/60:5.1f} min')
    print(f'OOM-looking results: {len(oom)}')
    for s, dur, aid, lab, m in sorted(oom)[:30]:
        print(f'  {s.astimezone(timezone(timedelta(hours=args.utc_offset))):%H:%M} {dur/60:5.1f} min {aid[:6]} [{m}] {lab}')
    print(f'slot / lock wait mentions: {len(slot)}; minutes in those calls {sum(x[1] for x in slot)/60:.1f}')
    for s, dur, aid, lab, m in sorted(slot, key=lambda x: -x[1])[:12]:
        print(f'  {s.astimezone(timezone(timedelta(hours=args.utc_offset))):%H:%M} {dur/60:5.1f} min {aid[:6]} [{m}] {lab}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dir'); ap.add_argument('--since'); ap.add_argument('--utc-offset', type=float, default=11)
    ap.add_argument('--csv'); ap.add_argument('--contention', action='store_true'); ap.add_argument('--top', type=int, default=15); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        selftest()
    else:
        contention(a) if a.contention else run(a)

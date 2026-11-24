r"""Where does a lane's wall time go? (Session 108, probe P9, 2026-11-21.) Reads ONE session's transcripts (the main session .jsonl and its
subagents\*.jsonl under %USERPROFILE%\.claude\projects\<project>\) and prints, from the timestamps alone (no message text beyond agent
descriptions and short command labels):

  agents     per agent: type, model, effort (the transcript's own fields), turns (API requests), tool calls, wall, and wall split into
             model (API request sent -> last block of that reply), client (tool result / prompt in -> next API request sent: context build,
             post-tool work), tool (reply finished -> last tool result of that reply: tool run + PreToolUse hooks + permission check),
             waiting (between rounds: the agent stopped, the main session had not resumed it yet)
  classes    tool calls by class (Read, Grep, git verb, q verb, game_syntax, check.py, selftests, ...): count, total, median, p90, max
  handoffs   per lane, the main session's side: agent hand-back queued -> main session took it off the queue -> next action of the same
             lane (review spawned, fix round SendMessage, merge command, next builder spawned)
  serial     consecutive replies that each held exactly ONE read-only call (Read / Grep / Glob / q / a read-only shell command / read-only git):
             the upper bound on what issuing them together would save (later round-trips + all but the longest tool time)

    py -I tools\harness\lane_time.py [--session ID_OR_JSONL] [--utc-offset 11] [--top 10] [--chains] [--reviewer] [--runs] [--admit]
        [--reaction] [--repeats] [--agents] [--classes] [--handoffs] [--serial] [--all]
With no section flag, prints the chain table (S109 LT): one row per chain, first spawn -> last sheet in the chat / last merge, minutes, each minute
given ONE label (probe, wait on Jarrod, builder, reviewer own, merge wait, exe build, run, sheet, other), the chains whose wait exceeds their own
work ("drifting"), and every row that cannot be derived named as "not derivable: <what is missing>". The other sections sit behind their flags:
  --reviewer  the reviewer in four phases      --runs  one row per game run (launch, boot, game, sheet step, into the chat), its lane derived
  --admit     the admission gate's waits       --reaction  agent hand-back -> the main session's next act, and the main session's own time
  --repeats   the reviewer's repeat proofs     --classes  tool calls by class: "py other" named by script, wait_commit / wait_any counted as waiting
With no --session, the project's newest session that has a subagents folder. Works while the session is still running (the end of an open
chain is its last agent record, marked *) and from a worktree (out\ is read from the main checkout).
The chain's report (Jarrod's message before the first spawn) is not derivable: no record ties a message to a lane key; a run's lane = the
lane of the agent whose run_list call started it, else a token of its route script's name that is a lane key, else its exe folder's
lane_exe.tsv lane, and one main-session call that ran an exe on several routes is one check of the exe's lane when a route names it; the
night = any gap over 60 min with no record (system records and a compaction's summary do not count).
Tool time is measured from the tool_use block's own record (written when that block finished streaming) to its tool_result record, so it
includes the PreToolUse hooks and the auto-mode permission check. A call run with run_in_background returns at once: its real run is not here.
"""
import argparse
import glob
import json
import os
import re
import statistics
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

PROJ = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import agent_time_audit as ata  # noqa: E402  (load / ts: the shared transcript reader)

TZ = timezone(timedelta(hours=11))      # local time of the run logs / lane_exe.tsv / admit history (set from --utc-offset)
_MAIN = []


def main_root(repo=None):
    """the main checkout: out\\ (lane_exe.tsv, exe run logs, admission history) lives there, not in a worktree"""
    if _MAIN:
        return _MAIN[0]
    repo = repo or os.path.dirname(os.path.dirname(HERE))
    root = repo
    try:
        gc = subprocess.run(['git', '-C', repo, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                            timeout=30).stdout.strip()
        if gc and os.path.basename(gc.rstrip('\\/')) == '.git':
            root = os.path.dirname(gc.rstrip('\\/'))
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    _MAIN.append(root)
    return root


def OUT(*parts):
    return os.path.join(main_root(), 'out', *parts)

READ_TOOLS = {'Read', 'Grep', 'Glob'}
GIT_RO = {'log', 'show', 'diff', 'status', 'rev-parse', 'merge-base', 'branch', 'ls-files', 'blame', 'grep', 'cat-file', 'describe', 'worktree'}


def py_name(cmd):
    """the script a `py ...` command runs (basename), or what it is instead"""
    c = cmd.replace('\\', '/')
    m = re.search(r'(?:^|[\s;&|(])(?:py|python3?)(?:\.exe)?\s+(?:-[A-Za-z]+\s+)*(?:"?([^\s"]+\.py)|(-c)\b|(-)\s|(-)$)', c)
    if m:
        return os.path.basename(m.group(1)) if m.group(1) else 'inline python (-c / stdin)'
    m = re.search(r'([\w./-]+\.py)\b', c)
    return os.path.basename(m.group(1)) if m else 'py (no script named)'


def cmd_class(name, inp):
    """a short class label for one tool call"""
    if name not in ('Bash', 'PowerShell'):
        return name
    c = (inp.get('command') or '').strip()
    cl = c.lower()
    if 'game_syntax' in cl:
        return 'sh:game_syntax'
    if re.search(r'handback[\\/]+check\.py', cl):
        return 'sh:handback_check'
    if 'context_pack' in cl:
        return 'sh:context_pack'
    if re.search(r'tools[\\/]+q[\\/]+q\.py', cl):
        m = re.search(r'q\.py\s+(\w+)(?:\s+(\w+))?', cl)
        v = m.group(1) if m else '?'
        if v == 'ours' and m.group(2):
            v += ' ' + m.group(2)
        return 'sh:q ' + v
    if re.search(r'selftest|_test\.py|test_\w+\.py|pytest|unittest', cl):
        return 'sh:selftest/test'
    if re.search(r'tools[\\/]+lint[\\/]', cl):
        return 'sh:lint'
    if re.search(r'build\.ps1|fast_build|cmake|ninja|clang', cl):
        return 'sh:build'
    if re.search(r'tidy\.py', cl):
        return 'sh:tidy'
    if re.search(r'(^|[;&|(]\s*|\bcd\s+\S+\s*&&\s*)git\s', cl) or cl.startswith('git '):
        verbs = re.findall(r'\bgit\s+(?:-C\s+\S+\s+|-c\s+\S+\s+)*([a-z-]+)', cl)
        return 'sh:git ' + (verbs[0] if verbs else '?') + ('+' if len(set(verbs)) > 1 else '')
    if re.search(r'\b(py|python)\b', cl):
        n = py_name(c)
        if n in ('wait_commit.py', 'wait_any.py'):
            return 'wait:commit'        # blocked until another agent's commit: waiting, parallel to the builder, not tool time
        return 'sh:py ' + n if n.endswith('.py') else 'sh:py other (' + n + ')'
    if re.search(r'\b(grep|rg|sed|cat|head|tail|ls|find|wc|awk|get-content|select-string|get-childitem|test-path)\b', cl):
        return 'sh:read/search'
    return 'sh:other'


def read_only(name, inp):
    if name in READ_TOOLS:
        return True
    if name not in ('Bash', 'PowerShell'):
        return False
    k = cmd_class(name, inp)
    if k.startswith('sh:q ') and 'update' not in k:
        return True
    if k == 'sh:read/search':
        return not re.search(r'(>|\brm\b|remove-item|set-content|out-file|mv\b|move-item|cp\b|copy-item)', (inp.get('command') or '').lower())
    if k.startswith('sh:git '):
        v = k[7:].rstrip('+')
        return v in GIT_RO and not k.endswith('+')
    return False


def records(path):
    return ata.load(path)


def text_of(c):
    if isinstance(c, str):
        return c
    if isinstance(c, list):
        return ' '.join(x.get('text', '') for x in c if isinstance(x, dict) and x.get('type') == 'text')
    return ''


def parse_transcript(path):
    """one transcript -> dict(calls, active, first, last, handbacks, replies).
    A call: id, name, inp, cls, bg, req (its API request), start (its block's record), res (its result), err, head / tail of the result,
    dur = result - the END OF ITS REPLY (the tool is not run until the whole reply has streamed: an Edit that shares a reply with other
    blocks otherwise carries the model's streaming time), dur_block = result - its own block, and for a background call done (its
    <task-notification>, whose <tool-use-id> names the call), dur_bg and bgout (the `Output is being written to:` file's head / tail)."""
    recs = records(path)
    calls, by_id = [], {}
    req_ts, req_end, msg_end, notif = {}, {}, {}, {}
    rounds = []
    first = last = None
    for r in recs:
        t = ata.ts(r)
        if not t:
            continue
        first = first or t
        last = t
        typ = r.get('type')
        if typ == 'api-request':
            req_ts[r['id']] = t
        elif typ == 'assistant':
            m = r.get('message') or {}
            msg_end[m.get('id')] = max(msg_end.get(m.get('id'), t), t)
            ref = r.get('requestRef')
            if ref:
                req_end[ref] = max(req_end.get(ref, t), t)
            for x in m.get('content') or []:
                if isinstance(x, dict) and x.get('type') == 'tool_use' and x['id'] not in by_id:
                    c = dict(id=x['id'], name=x.get('name'), inp=x.get('input') or {}, start=t, req=ref, mid=m.get('id'), res=None,
                             err=False, head='', tail='')
                    c['bg'] = bool(c['inp'].get('run_in_background'))
                    by_id[x['id']] = c
                    calls.append(c)
        elif typ == 'user':
            c = (r.get('message') or {}).get('content')
            if isinstance(c, list):
                for x in c:
                    if isinstance(x, dict) and x.get('type') == 'tool_result' and x.get('tool_use_id') in by_id:
                        cc = by_id[x['tool_use_id']]
                        if cc['res'] is None:
                            cc['res'] = t
                            body = x.get('content')
                            body = body if isinstance(body, str) else text_of(body)
                            cc['err'] = bool(x.get('is_error'))
                            cc['head'] = (body or '')[:400]
                            cc['tail'] = (body or '')[-600:]
            if ata.is_round_start(r):
                rounds.append(t)
        blob = ''
        if typ == 'queue-operation' and r.get('operation') == 'enqueue':
            blob = r.get('content') or ''
        elif typ == 'attachment' and (r.get('attachment') or {}).get('type') == 'queued_command':
            p = r['attachment'].get('prompt')
            blob = p if isinstance(p, str) else ''
        elif typ == 'user':
            blob = text_of((r.get('message') or {}).get('content'))
        if '<task-notification>' in blob:
            for m in re.finditer(r'<tool-use-id>(toolu_\w+)</tool-use-id>', blob):
                notif.setdefault(m.group(1), t)
    for c in calls:
        end = req_end.get(c['req']) or msg_end.get(c['mid']) or c['start']
        c['end_reply'] = max(end, c['start'])
        c['cls'] = cmd_class(c['name'], c['inp'])
        c['dur_block'] = max(0.0, (c['res'] - c['start']).total_seconds()) if c['res'] else 0.0
        c['dur'] = max(0.0, (c['res'] - c['end_reply']).total_seconds()) if c['res'] else 0.0
        c['done'] = notif.get(c['id']) if c['bg'] else c['res']
        c['dur_bg'] = None
        c['bgout'] = ''
        if c['bg']:
            m = re.search(r'Output is being written to: (\S+?\.output)', c['head'] + c['tail'])
            if m and os.path.exists(m.group(1)):
                try:
                    with open(m.group(1), encoding='utf-8', errors='replace') as fh:
                        body = fh.read()
                    c['bgout'] = body[:400] + ' ... ' + body[-1500:]
                except OSError:
                    pass
            c['dur_bg'] = (c['done'] - c['start']).total_seconds() if c['done'] else None
    # active intervals: each round runs from its start to the last record before the next round start (idle between rounds is not counted)
    allts = sorted(t for t in (ata.ts(r) for r in recs) if t)
    starts = sorted(set(([first] if first else []) + rounds))
    active = []
    for i, s in enumerate(starts):
        nxt = starts[i + 1] if i + 1 < len(starts) else None
        inside = [x for x in allts if x >= s and (nxt is None or x < nxt)]
        if inside:
            active.append((s, inside[-1]))
    handbacks = [c['start'] for c in calls if c['name'] == 'SubagentHandback']
    replies = [(req_ts[k], req_end.get(k, req_ts[k])) for k in req_ts]
    # times (the night finder's input) leave out the records nobody produced at work: the client's system records and a compaction's
    # summary (S108: the 03:39 continuation summary, no reply after it, cut the 02:44-08:37 night into 55 "awake" minutes + a shorter night)
    live = sorted(t for t in (ata.ts(r) for r in recs if not auto_record(r)) if t)
    return dict(calls=calls, active=active, first=first, last=last, handbacks=handbacks, replies=replies, times=live)


def auto_record(r):
    """a record the client wrote with nobody at work: a system record (stop-hook summary, compact boundary) or the summary a compaction
    starts the continued conversation with"""
    if r.get('type') == 'system':
        return True
    return r.get('type') == 'user' and text_of((r.get('message') or {}).get('content')).startswith('This session is being continued')


def agent_stats(path):
    recs = records(path)
    meta = {}
    try:
        meta = json.load(open(path[:-6] + '.meta.json', encoding='utf-8'))
    except (OSError, ValueError):
        pass
    req_ts = {}                 # api-request id -> ts
    req_end = {}                # api-request id -> last assistant block ts
    req_calls = defaultdict(list)
    call_start, call_req, call_name, call_inp, res_ts = {}, {}, {}, {}, {}
    models, efforts = set(), set()
    rounds = []                 # round start ts (a user text message)
    events = []                 # (ts, kind, id)
    for r in recs:
        t = ata.ts(r)
        if not t:
            continue
        typ = r.get('type')
        if typ == 'api-request':
            req_ts[r['id']] = t
            events.append((t, 'req', r['id']))
        elif typ == 'assistant':
            ref = r.get('requestRef')
            m = r.get('message') or {}
            if m.get('model'):
                models.add(m['model'])
            if r.get('effort'):
                efforts.add(r['effort'])
            if ref:
                req_end[ref] = max(req_end.get(ref, t), t)
            for x in m.get('content') or []:
                if isinstance(x, dict) and x.get('type') == 'tool_use' and x['id'] not in call_start:
                    call_start[x['id']] = t
                    call_req[x['id']] = ref
                    call_name[x['id']] = x.get('name')
                    call_inp[x['id']] = x.get('input') or {}
                    req_calls[ref].append(x['id'])
        elif typ == 'user':
            c = (r.get('message') or {}).get('content')
            if isinstance(c, list):
                for x in c:
                    if isinstance(x, dict) and x.get('type') == 'tool_result' and x.get('tool_use_id') not in res_ts:
                        res_ts[x['tool_use_id']] = t
                        events.append((t, 'res', x['tool_use_id']))
            if ata.is_round_start(r):
                rounds.append(t)
                events.append((t, 'round', None))
        elif typ == 'attachment' and (r.get('attachment') or {}).get('type') == 'queued_command':
            events.append((t, 'queued', None))
    events.sort(key=lambda e: e[0])
    # walk the requests in time order: client = last input before the request -> request; model = request -> reply end;
    # tool = reply end -> last result of the reply's calls
    model = client = tool = wcall = 0.0
    parsed = parse_transcript(path)
    call_cls = {c['id']: c['cls'] for c in parsed['calls']}
    last_in = rounds[0] if rounds else None
    reqs = sorted(req_ts, key=lambda k: req_ts[k])
    per_req = []
    for i, rid in enumerate(reqs):
        t0 = req_ts[rid]
        # inputs between previous request and this one
        prev_inputs = [e[0] for e in events if e[1] in ('res', 'round', 'queued') and e[0] <= t0 and (i == 0 or e[0] >= req_ts[reqs[i - 1]])]
        if prev_inputs:
            last_in = max(prev_inputs)
        cg = (t0 - last_in).total_seconds() if last_in else 0.0
        end = req_end.get(rid, t0)
        mg = max(0.0, (end - t0).total_seconds())
        calls = req_calls.get(rid, [])
        # calls that wait for another agent's commit (wait_commit / wait_any) are waiting, not tool time
        rt = [res_ts[c] for c in calls if c in res_ts and call_cls.get(c) != 'wait:commit']
        wt = [res_ts[c] for c in calls if c in res_ts and call_cls.get(c) == 'wait:commit']
        tg = max(0.0, (max(rt) - end).total_seconds()) if rt else 0.0
        wg = max(0.0, (max(wt) - end).total_seconds()) if wt else 0.0
        # a reply with no tool call (a final message, or a retried request) ends the round: no tool span
        per_req.append(dict(id=rid, t0=t0, client=cg, model=mg, tool=tg, wcall=wg, calls=calls, end=end))
        model += mg
        wcall += wg
        client += cg if cg < 600 else 0.0          # > 10 min = the agent was stopped between rounds (counted as waiting)
        tool += tg
    ts_all = [ata.ts(r) for r in recs if ata.ts(r)]
    first, last = (min(ts_all), max(ts_all)) if ts_all else (None, None)
    wall = (last - first).total_seconds() if first else 0.0
    # waiting between rounds: from the last reply end before a round start to that round start
    waiting = 0.0
    ends = sorted(p['end'] for p in per_req)
    for rs in rounds[1:]:
        before = [e for e in ends if e <= rs]
        if before:
            waiting += (rs - before[-1]).total_seconds()
    calls = [c for c in parsed['calls'] if c['res']]        # dur = from the reply's end (site 2); dur_block = from the call's own block
    shas = ' '.join(re.findall(r'\b[0-9a-f]{7,40}\b', ' '.join(json.dumps(c['inp']) for c in calls if c['name'] == 'SubagentHandback')))
    return dict(text=shas, aid=os.path.basename(path)[6:-6], desc=meta.get('description', ''), atype=meta.get('agentType', ''),
                models=sorted(models), efforts=sorted(efforts), turns=len(reqs), ncalls=len(calls), wall=wall, model=model,
                client=client, tool=tool, wcall=wcall, waiting=waiting, rounds=len(rounds), first=first, last=last, calls=calls,
                per_req=per_req, parsed=parsed)


def main_events(path):
    """main session: agent spawns, hand-backs queued / taken, SendMessage resumes, merges, Jarrod's messages ('jarrod': a typed message or a
    queued_command attachment, images included, minus agent messages / task notices / the compaction summary), SendUserFile ('send'),
    background lane_exe / run_list start -> completion notice ('bg_start' / 'bg_done') -> [(ts, kind, agent_id, label)]"""
    out = []
    spawn_tool = {}
    recs = records(path)
    for r in recs:
        t = ata.ts(r)
        typ = r.get('type')
        if t and ((typ == 'attachment' and (r.get('attachment') or {}).get('type') == 'queued_command') or (typ == 'user' and not r.get('isSidechain') and not r.get('isMeta'))):
            c = r['attachment'].get('prompt') if typ == 'attachment' else (r.get('message') or {}).get('content')
            if isinstance(c, list) and any(isinstance(x, dict) and x.get('type') == 'tool_result' for x in c):
                c = None
            txt = text_of(c) if c is not None else ''
            img = isinstance(c, list) and any(isinstance(x, dict) and x.get('type') == 'image' for x in c)
            if (txt.strip() or img) and 'agent-message' not in txt and 'task-notification' not in txt and not txt.startswith('This session is being continued'):
                out.append((t, 'jarrod', '', '%d chars%s' % (len(txt), ' [image]' if img else '')))
        if typ == 'assistant' and t:
            for x in (r.get('message') or {}).get('content') or []:
                if isinstance(x, dict) and x.get('type') == 'tool_use' and x.get('name') == 'SendUserFile':
                    out.append((t, 'send', '', json.dumps(x.get('input') or {})[:300]))
        if typ == 'assistant' and t:
            for x in (r.get('message') or {}).get('content') or []:
                if not isinstance(x, dict) or x.get('type') != 'tool_use':
                    continue
                inp = x.get('input') or {}
                if x.get('name') == 'Agent':
                    spawn_tool[x['id']] = (t, inp.get('description', ''))
                elif x.get('name') == 'SendMessage':
                    out.append((t, 'resume', inp.get('to', ''), inp.get('summary', '')[:50]))
        elif typ == 'user' and t:
            c = (r.get('message') or {}).get('content')
            if isinstance(c, list):
                for x in c:
                    if isinstance(x, dict) and x.get('type') == 'tool_result' and x.get('tool_use_id') in spawn_tool:
                        body = x.get('content')
                        if isinstance(body, list):
                            body = ' '.join(y.get('text', '') for y in body if isinstance(y, dict))
                        m = re.search(r'agentId:\s*([0-9a-f]{17})', str(body or ''))
                        if m:
                            st, d = spawn_tool[x['tool_use_id']]
                            out.append((st, 'spawn', m.group(1), d[:50]))
        elif typ == 'queue-operation' and t:
            m = re.search(r'<agent-message from="([0-9a-f]{17})"', r.get('content') or '')
            if m:
                out.append((t, 'handback_queued' if r.get('operation') == 'enqueue' else 'handback_taken', m.group(1), ''))
    for c in parse_transcript(path)['calls']:
        j = json.dumps(c['inp'])
        if c['bg'] and c['name'] in ('Bash', 'PowerShell') and ('lane_exe' in j or 'run_list' in j):
            lab = 'lane_exe' if 'lane_exe' in j else 'run_list'
            out.append((c['start'], 'bg_start', '', lab))
            if c['done']:
                out.append((c['done'], 'bg_done', '', lab))
    out.sort(key=lambda e: e[0])
    res = []
    for e in out:      # one typed message can be recorded as a user record and as a queued attachment: keep the first within 5 s
        if e[1] == 'jarrod' and any(p[1] == 'jarrod' and p[3] == e[3] and (e[0] - p[0]).total_seconds() < 5 for p in res[-6:]):
            continue
        res.append(e)
    return res


def reply_seconds(path):
    """the main session's own model time: sum of API request sent -> last block of its reply (the reaction side of the report)"""
    return sum((e - s).total_seconds() for s, e in parse_transcript(path)['replies'] if e > s)


MERGE_KEY = r'[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)?'


def merge_keys(subj):
    """the lane keys a merge commit's subject names ('merge lane BLD7', 'Merge S108 lane X', 'merge probe MEASURE note', 'lanes A + B')"""
    m = re.search(r'merge (?:probe |lanes? )(' + MERGE_KEY + r'(?:\s*\+\s*[A-Z0-9-]+(?:\s+piece \d)?)*)', subj, re.I) or \
        re.search(r'Merge S\d+ lane (' + MERGE_KEY + ')', subj)
    keys = re.findall(MERGE_KEY, m.group(1)) if m else []
    return [k for k in keys if not re.fullmatch(r'S\d+', k)]


def git_merges(repo, since_utc, until_utc=None):
    """first-parent merges made in [since, until] on any ref (the session's own line: the main session merges lanes into its branch) ->
    [(utc, short hash, lane keys parsed from the subject, subject)]; a merge naming no lane has no keys"""
    cmd = ['git', '-C', repo, 'log', '--all', '--merges', '--first-parent', '--format=%h|%aI|%s', '--since=' + since_utc.isoformat()]
    if until_utc:
        cmd.append('--until=' + until_utc.isoformat())
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    res, seen = [], set()
    for line in out.splitlines():
        if line.count('|') < 2:
            continue
        h, when, subj = line.split('|', 2)
        if h in seen:
            continue
        seen.add(h)
        res.append((datetime.fromisoformat(when).astimezone(timezone.utc), h, merge_keys(subj), subj[:60]))
    return sorted(res)


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p * (len(v) - 1))))] if v else 0.0


ROLE_WORDS = r'(?:route|reviewer|review|probe-builder|builder|build|probe|lanes?)'


def lane_key(desc):
    """THE RULE for where the lane key sits in an agent's description: <model word> <role word>* <KEY>[:| ...], i.e. after the model word
    (OPUS / SON / Guide) and any run of role words (route, review(er), builder, build, probe, probe-builder, lane(s)), the next token, which
    must be a key (capitals / digits, one optional -SUFFIX: BLD7, SEAT-A0, TRON-B, EXEB-B1, P2), up to the colon or space that ends it.
    'SON builder SEAT-A0: the seat card format' -> SEAT-A0; 'OPUS review SEAT2: hand-over' -> SEAT2; 'OPUS route lane TRONROW: x' -> TRONROW;
    'other' when no key sits there ('OPUS reader A01', 'OPUS probe: OMG barricade recipe': a colon right after the role word names none)"""
    d = re.sub(r'^(OPUS|SON|Guide)\s+', '', desc or '')
    if d.startswith('inventory'):
        return 'OPT'
    d = re.sub(r'^(?:' + ROLE_WORDS + r'\s+)+', '', d)
    m = re.match(r'([A-Z][A-Z0-9]*(?:-[A-Z0-9]+)?)(?=[:\s]|$)', d)
    return m.group(1) if m else 'other'


def role_of(atype, desc):
    if (desc or '').split(' ')[1:2] == ['route']:
        return 'route'
    return {'lane-probe': 'probe', 'probe-builder': 'probe-builder', 'lane-builder': 'builder',
            'review-fixbuilder': 'reviewer'}.get(atype, 'other')


def lane_of(desc):
    return lane_key(desc)


def stem_of(key):
    """BLD7 -> BLD, EXEB-B1 -> EXEB, SHOT-A -> SHOT, AL1 -> AL; a one-letter stem keeps its digit (P2, G1)"""
    s = re.sub(r'-[A-Z0-9]+$', '', key)
    m = re.fullmatch(r'([A-Z]{2,})\d+', s)
    return m.group(1) if m else s


def chain_map(keys, links=()):
    """lane key -> chain name. A chain is the lane keys that share a stem (BLD .. BLD7, SHOT / SHOT-A / SHOT-B) or whose stems are a
    prefix of each other (AL1 / ALM / ALM2, GAV / GAVF), or that a brief links (links = [(lane, the lane / probe it continues)]); the chain
    is named by its shortest stem that is itself a lane key."""
    keys = sorted(set(keys))
    stems = {k: stem_of(k) for k in keys}
    parent = {s: s for s in set(stems.values())}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    ss = sorted(parent)
    for x, y in links:
        if x in stems and y in stems:
            parent[find(stems[x])] = find(stems[y])
    for a in ss:
        for b in ss:
            if a != b and len(a) >= 2 and b.startswith(a):
                parent[find(b)] = find(a)
    groups = defaultdict(list)
    for s in ss:
        groups[find(s)].append(s)
    name = {}
    for g in groups.values():
        exact = [s for s in g if s in keys]
        n = min(exact or g, key=lambda s: (len(s), s))
        for s in g:
            name[s] = n
    return {k: name[stems[k]] for k in keys}


def chain_of(key, keys=()):
    return chain_map(list(keys) + [key])[key]


LINK_RX = [re.compile(r'SETTLED STARTING POINT\s*\(\s*docs[\\/]+research[\\/]+s\d+[\\/]+(' + MERGE_KEY + r')_'),
           re.compile(r'on top of the held lane\s+(' + MERGE_KEY + r')\b', re.I)]


def prompt_links(text):
    """the lanes a builder's brief says it continues: the probe note it starts from ('SETTLED STARTING POINT (docs\\research\\s108\\CAA_x.md')
    or the held lane it builds on top of -> [lane keys]"""
    return [m.group(1) for rx in LINK_RX for m in rx.finditer(text or '')]


def first_prompt(path):
    for r in records(path):
        if r.get('type') == 'user':
            return text_of((r.get('message') or {}).get('content'))[:20000]
    return ''


def agents_chain_map(agents):
    """chain_map over the session's lane keys, with the links the briefs name (CAP continues the CAA probe)"""
    keys = [g['lane'] for g in agents if g['lane'] != 'other']
    return chain_map(keys, [(g['lane'], k) for g in agents for k in g.get('links', []) if k in keys and g['lane'] != 'other'])


def run_lane(route, exe_lane, keys):
    """a run's lane (no hand table): a token of the route script's name that IS a lane key (s108_ban_arena53 -> BAN), else the exe folder's
    lane_exe.tsv `lane` column (s108fan -> FAN), else None with what is missing -> (key or None, how / why)"""
    low = {k.lower(): k for k in keys}
    low.update({stem_of(k).lower(): stem_of(k) for k in keys})
    toks = [t for t in re.split(r'[^a-z0-9]+', os.path.splitext(os.path.basename(route or ''))[0].lower()) if t]
    hit = [t for t in toks if t in low]
    if hit:
        return low[max(hit, key=len)], 'route name'
    el = re.sub(r'^s\d+', '', (exe_lane or '').lower())
    if el in low:
        return low[el], 'exe folder lane'
    m = re.match(r'[a-z]{2,}', el)           # rds3 -> rds, cap3b -> cap
    if m and m.group(0) in low:
        return low[m.group(0)], 'exe folder lane (stem)'
    return None, 'not derivable: route %r names no lane and exe lane column %r is no lane key' % (os.path.basename(route or '?'), exe_lane or '-')


def find_nights(times, gap=3600):
    """the night = any gap over `gap` seconds between consecutive records (nobody at the PC) -> [(start, end)]"""
    ts_ = sorted(set(times))
    return [(a, b) for a, b in zip(ts_, ts_[1:]) if (b - a).total_seconds() > gap]


def plan_stop(g):
    """where a probe-builder's settling ends: its first hand-back when it stopped at its plan (>= 2 hand-backs); else its first
    commit of a plan / research note when a code commit follows; a run of note commits only = all probe; no note = all builder"""
    if len(g['handbacks']) >= 2:
        return g['handbacks'][0]
    cm = [(c['start'], bool(re.search(r'_plan|research', c['inp'].get('command') or ''))) for c in g['calls']
          if re.search(r'\bgit\b.*\bcommit\b', c['inp'].get('command') or '')]
    plans = [t for t, isplan in cm if isplan]
    if plans and any(not isplan and t > plans[0] for t, isplan in cm):
        return plans[0]
    if plans:
        return g['last']
    return g['first']


def builder_done(rev, agents):
    """the hand-back of the builder this reviewer reviews (same lane key; SHOT-A/B for SHOT): its LAST hand-back before the reviewer's last record"""
    keys = {rev['lane']}
    if rev['lane'].startswith('SHOT'):
        keys = {'SHOT-A', 'SHOT-B', 'SHOT'}
    hbs = [h for g in agents if g is not rev and g['lane'] in keys and g['role'] in ('builder', 'probe-builder')
           for h in g['handbacks'] if h <= rev['last']]
    return max(hbs) if hbs else None


def resolve_session(sess):
    sess = sess[:-6] if sess.endswith('.jsonl') else sess
    if not os.path.isabs(sess):
        cand = [os.path.join(PROJ, sess)] + glob.glob(os.path.join(os.path.dirname(PROJ), '*', sess))
        sess = next((c for c in cand if os.path.exists(c + '.jsonl')), cand[0])
    return sess


def load_session(sess):
    """-> (agents, main): every subagent transcript parsed (calls with dur / dur_block / done, active rounds, hand-backs) with its lane key
    and role; the main session parsed the same way, with main['events'] = main_events()"""
    sess = resolve_session(sess)
    agents = []
    for p in sorted(glob.glob(os.path.join(sess, 'subagents', '*.jsonl'))):
        a = parse_transcript(p)
        if not a['first']:
            continue
        try:
            meta = json.load(open(p[:-6] + '.meta.json', encoding='utf-8'))
        except (OSError, ValueError):
            meta = {}
        a['aid'] = os.path.basename(p)[6:-6]
        a['desc'] = meta.get('description', '')
        a['atype'] = meta.get('agentType', '')
        a['lane'] = lane_key(a['desc'])
        a['role'] = role_of(a['atype'], a['desc'])
        a['links'] = prompt_links(first_prompt(p))
        agents.append(a)
    main = parse_transcript(sess + '.jsonl')
    main['events'] = main_events(sess + '.jsonl')
    main['path'] = sess + '.jsonl'
    return agents, main


# ---------------------------------------------------------------- other sources (main checkout's out\)
def lane_exe_rows():
    rows = []
    p = OUT('exe', 'lane_exe.tsv')
    if not os.path.exists(p):
        return rows
    for line in open(p, encoding='utf-8').read().splitlines()[1:]:
        f = line.split('\t')
        if len(f) < 14:
            continue
        try:
            end = datetime.strptime(f[0], '%Y-%m-%d %H:%M:%S').replace(tzinfo=TZ).astimezone(timezone.utc)
            tot = int(f[12])
        except ValueError:
            continue
        rows.append(dict(name=f[1], lane=f[2], end=end, start=end - timedelta(seconds=tot), total=tot, result=f[13]))
    return rows


LOGTS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')


def run_logs(t0, t1, exes, keys):
    """every game run log (out\\*\\*\\logs\\wwe2k14_N.log) whose first line lies in [t0, t1]: first / last line, boot-to-menu, vsync, route,
    the sheet file's time, and its lane (run_lane) -> [dict]"""
    byname = {e['name']: e['lane'] for e in exes}
    out = []
    for f in sorted(glob.glob(OUT('*', '*', 'logs', 'wwe2k14_*.log'))):
        try:
            if datetime.fromtimestamp(os.path.getmtime(f), timezone.utc) < t0:      # last written before the session began
                continue
        except OSError:
            continue
        folder = os.path.basename(os.path.dirname(os.path.dirname(f)))
        num = int(re.search(r'_(\d+)\.log$', f).group(1))
        first = last = menu = vsync = route = None
        with open(f, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                m = LOGTS.match(line)
                if not m:
                    continue
                t = datetime.strptime(m.group(1)[:19], '%Y-%m-%d %H:%M:%S').replace(tzinfo=TZ).astimezone(timezone.utc)
                first = first or t
                last = t
                if vsync is None and "] vsync = '" in line:
                    vsync = line.split("vsync = '")[1][:5].rstrip("'")
                if route is None and 'wwe2k14_input_script =' in line:
                    route = line.split("'")[1].split('\\')[-1]
                if 'boot_to_menu.txt' in line and 'reached' in line:
                    menu = t
        if not first or not (t0 <= first <= t1):
            continue
        cdir = os.path.join(os.path.dirname(os.path.dirname(f)), 'captures')
        caps = glob.glob(os.path.join(cdir, '%03d*sheet*.jpg' % num)) + glob.glob(os.path.join(cdir, '%03d_p*' % num, '*sheet*.jpg'))
        sheet = min((datetime.fromtimestamp(os.path.getmtime(c), timezone.utc) for c in caps), default=None)
        key, how = run_lane(route, byname.get(folder), keys)
        out.append(dict(folder=folder, num=num, first=first, last=last, menu=menu, vsync=vsync, route=route, sheet=sheet,
                        key=key, how=how, chain=None))
    out.sort(key=lambda r: r['first'])
    for i, r in enumerate(out, 1):
        r['tag'] = str(i)
    for i, r in enumerate(out):      # neither names a lane: the nearest earlier run of the same route script with a derived lane (run 2: the base exe on run 1's route)
        if not r['key'] and r['route']:
            prev = [p for p in out[:i] if p['route'] == r['route'] and p['key']]
            if prev:
                r['key'], r['how'] = prev[-1]['key'], 'same route as run %s' % prev[-1]['tag']
    return out


def run_launcher(r, owners):
    """the call that started a run: the latest shell call before the run's first log line that invokes run_list.ps1 naming the run's route and
    was still running when the game started -> (call, owner agent or None for the main session) or None"""
    best = None
    for c, g in owners:
        cmd = c['inp'].get('command') or ''
        if c['name'] not in ('Bash', 'PowerShell') or not r['route'] or c['start'] > r['first'] or 'run_list.ps1' not in cmd or r['route'] not in cmd:
            continue
        end = c['done'] if c['bg'] else c['res']
        if end is not None and end < r['first']:
            continue
        if best is None or c['start'] > best[0]['start']:
            best = (c, g)
    return best


def exe_lane(folder, exes, merges, keys):
    """the lane an exe folder was built for: its lane_exe.tsv lane column when that is a lane key (s108prt -> PRT), else a lane key of the
    merge commit the folder is named for (lane-s108tip-cb873c14b -> 'merge lane BAN3' -> BAN3), else None"""
    k = run_lane('', next((e['lane'] for e in exes if e['name'] == folder), ''), keys)[0]
    if k:
        return k
    m = re.search(r'-([0-9a-f]{7,40})$', folder or '')
    for _when, h, ks, _subj in merges if m else []:
        if h.startswith(m.group(1)) or m.group(1).startswith(h):
            hit = [x for x in ks if x in keys]
            if hit:
                return hit[0]
    return None


def launch_lanes(runs, agents, main, exes, merges, keys):
    """a run's lane read from the call that started it, over the route / exe-folder guess: a run an agent launched is that agent's lane (the
    route lanes' own runs: S108's MENU route lane ran a route named for THB); one main-session call that ran an exe on several routes is ONE
    check, of the exe's lane when one of its routes names that lane (S108 runs 26 / 27: the BAN3 tip exe on the banner and the created-arena
    route, both BAN's check; the created-arena run is not CAA's, whose chain had merged at 02:09)"""
    owners = [(c, None) for c in main['calls']] + [(c, g) for g in agents for c in g['calls']]
    allk = sorted(set(keys) | {g['lane'] for g in agents if g['lane'] != 'other'})
    cmap = chain_map(allk, [(g['lane'], k) for g in agents for k in g.get('links', []) if k in allk and g['lane'] != 'other'])
    ch = lambda k: cmap.get(k, k) if k else None
    calls = defaultdict(list)
    for r in runs:
        hit = run_launcher(r, owners)
        if not hit:
            continue
        c, g = hit
        if g is not None and g['lane'] != 'other':
            r['key'], r['how'] = g['lane'], 'launched by agent %s (%s)' % (g['aid'][:10], g['lane'])
        elif g is None:
            calls[c['id']].append(r)
    for grp in calls.values():
        for r in grp:
            ek = exe_lane(r['folder'], exes, merges, allk)
            mates = [x for x in grp if x is not r and x['key'] and ch(x['key']) == ch(ek)]
            if ek and mates and ch(r['key']) != ch(ek):
                r['key'], r['how'] = mates[0]['key'], 'one call with run %s on the %s exe: a check of %s' % (mates[0]['tag'], ek, ch(ek))
    return runs


# ---------------------------------------------------------------- the chain table
PRIORITY = ['night', 'game run', 'exe build', 'run launch', 'sheet step', 'reviewer own', 'builder', 'probe',
            'reviewer beside build', 'route lane', 'read-back / reply wait', 'idle (cause unknown)', 'merge wait', 'main session / between']
CHAIN_GAP = timedelta(minutes=120)      # a lane key's next agent starting later than this after the chain's last agent record opens a new chain
WAIT_PENDING = timedelta(seconds=120)   # a hand-back / sheet sent this close before a gap's start = the lane was waiting for a reply at the gap's start


def build_chains(agents, merges):
    """THE CHAIN RULE: a chain is the agents of ONE exact lane key (LODGE, LODGE2, LODGE3 and SEAT-A0 / SEAT-A are all different keys) that overlap or
    follow each other within CHAIN_GAP of the chain's latest agent record, with no merge of that key between (a merge naming the key, made after the
    chain's latest record and before the next agent starts, closes the chain). A later agent of the key starts KEY#2, KEY#3 ...
    A reviewer / fix round carries its builder's key, so it joins the builder's chain. -> [dict(name, key, ag)] (each agent gets g['chain'])"""
    bykey = defaultdict(list)
    for g in agents:
        if g['lane'] != 'other':
            bykey[g['lane']].append(g)
    chains = []
    for key, gs in bykey.items():
        gs.sort(key=lambda g: g['first'])
        cur, reach = [], None
        for g in gs:
            closed = cur and (g['first'] > reach + CHAIN_GAP or any(key in m[2] and reach < m[0] < g['first'] for m in merges))
            if closed:
                chains.append((key, cur))
                cur, reach = [], None
            cur.append(g)
            reach = g['last'] if reach is None else max(reach, g['last'])
        if cur:
            chains.append((key, cur))
    seen, res = defaultdict(int), []
    for key, ag in sorted(chains, key=lambda c: min(g['first'] for g in c[1])):
        seen[key] += 1
        name = key if seen[key] == 1 else '%s#%d' % (key, seen[key])
        for g in ag:
            g['chain'] = name
        res.append(dict(name=name, key=key, ag=ag))
    return res


def chain_for_run(chains, key, t):
    """the chain a run of lane `key` at time t belongs to: among the chains of that exact key (else of its family: chain_map), the latest that began
    before t, else the earliest"""
    fam = agents_chain_map_keys([c['key'] for c in chains] + [key])
    cand = [c for c in chains if c['key'] == key] or [c for c in chains if fam.get(c['key']) == fam.get(key)]
    if not cand:
        return None
    before = [c for c in cand if min(g['first'] for g in c['ag']) <= t]
    return max(before, key=lambda c: min(g['first'] for g in c['ag'])) if before else min(cand, key=lambda c: min(g['first'] for g in c['ag']))


def agents_chain_map_keys(keys):
    return chain_map(sorted(set(keys)))


class Timeline:
    """seconds of [t0, t1], each given the highest-priority label whose interval covers it"""

    def __init__(self, t0, t1):
        self.t0, self.t1 = t0, t1
        self.iv = []

    def add(self, label, a, b):
        if a and b and b > a:
            self.iv.append((max(a, self.t0), min(b, self.t1), label))

    def split(self):
        pts = sorted({self.t0, self.t1} | {x for a, b, _ in self.iv for x in (a, b) if self.t0 <= x <= self.t1})
        res = defaultdict(float)
        for a, b in zip(pts, pts[1:]):
            labs = [l for (x, y, l) in self.iv if x <= a and y >= b]
            res[min(labs, key=PRIORITY.index) if labs else 'main session / between'] += (b - a).total_seconds()
        return res


REPORT_NOT_DERIVABLE = ('report time: not derivable (no record ties a Jarrod message to a lane key: S108\'s PRT was spawned in a turn an '
                        'agent\'s hand-back opened, the message before EXEB\'s spawn was BLD\'s verdict); each chain is measured from its first spawn')


def chain_table(agents, main, merges, exes, runs, ctx):
    """one row per chain: first spawn -> last sheet in the chat (or last merge, or the last agent record when the chain is still open), every second of
    that span given ONE label (PRIORITY). The report before the spawn is not derivable (REPORT_NOT_DERIVABLE). -> (rows, notes); a row: chain,
    keys, t0 (first spawn), t1, total, split, nruns, nag, open"""
    ev = main['events']
    sends = [e for e in ev if e[1] == 'send']
    msgs = sorted(e[0] for e in ev if e[1] == 'jarrod')
    nights = ctx.get('nights') or []
    callers = [c for g in [main] + agents for c in g['calls']]
    rows, notes = [], []
    chains = build_chains(agents, merges)
    for r in runs:
        c = chain_for_run(chains, r['key'], r['first']) if r['first'] and r['key'] else None
        r['chain'] = c['name'] if c else None
    for ch in sorted(chains, key=lambda c: min(g['first'] for g in c['ag'])):
        chain, keys, ag = ch['name'], [ch['key']], ch['ag']
        t_rep = min(g['first'] for g in ag)
        nxt0 = [min(g['first'] for g in c['ag']) for c in chains if c['key'] == ch['key'] and min(g['first'] for g in c['ag']) > t_rep]
        crun = [r for r in runs if r['first'] and r['chain'] == chain]
        sheet_ts = []
        for r in crun:
            s = [e[0] for e in sends if r['folder'] in e[3].replace('\\\\', '\\') and e[0] >= r['last']]
            r['sent'] = min(s) if s else None
            if r['sent']:
                sheet_ts.append(r['sent'])
        mk = [m for m in merges if ch['key'] in m[2] and m[0] >= t_rep and (not nxt0 or m[0] < min(nxt0))]
        t_end = max(sheet_ts + [m[0] for m in mk]) if (sheet_ts or mk) else max(g['last'] for g in ag)
        is_open = not (sheet_ts or mk)
        tl = Timeline(t_rep, max(t_end, t_rep))
        for a, b in nights:
            tl.add('night', a, b)
        for r in crun:
            tl.add('game run', r['first'], r['last'])
            tl.add('sheet step', r['last'], r.get('sent') or r['sheet'])
            starts = [c['start'] for c in callers if c['cls'] != 'Read' and 'run_list' in json.dumps(c['inp']) and r['route']
                      and r['route'] in json.dumps(c['inp']) and c['start'] <= r['first']]
            if starts:
                tl.add('run launch', max(starts), r['first'])
        used = {r['folder'] for r in crun}
        for e in exes:
            if e['name'] in used:
                tl.add('exe build', e['start'], e['end'])
        for g in ag:
            if g['role'] == 'reviewer':
                bdone = builder_done(g, agents)
                for s, e in g['active']:
                    if bdone and e > bdone:
                        tl.add('reviewer own', max(s, bdone), e)
                        tl.add('reviewer beside build', s, min(e, bdone))
                    else:
                        tl.add('reviewer beside build', s, e)
            elif g['role'] == 'probe':
                for s, e in g['active']:
                    tl.add('probe', s, e)
            elif g['role'] == 'probe-builder':
                ps = plan_stop(g)
                for s, e in g['active']:
                    if ps and s < ps:
                        tl.add('probe', s, min(e, ps))
                        if e > ps:
                            tl.add('builder', ps, e)
                    else:
                        tl.add('builder', s, e)
            elif g['role'] == 'route':
                for s, e in g['active']:
                    tl.add('route lane', s, e)
            else:
                for s, e in g['active']:
                    tl.add('builder', s, e)
            # a plan-stop hand-back -> the main session's next message to that agent; a probe's hand-back -> the chain's next spawn
            for hb in g['handbacks'][:-1] if g['role'] == 'probe-builder' else []:
                nxt = [c['start'] for c in main['calls'] if c['name'] == 'SendMessage' and c['inp'].get('to') == g['aid'] and c['start'] > hb]
                if nxt:
                    tl.add('read-back / reply wait', hb, min(nxt))
            if g['role'] == 'probe' and g['handbacks']:
                hb = g['handbacks'][-1]
                nxt = [h['first'] for h in ag if h['first'] > hb]
                if nxt:
                    tl.add('read-back / reply wait', hb, min(nxt))
        # a gap between two of the chain's own events that holds a message from Jarrod: waiting on his reply, up to that message ([L]: the
        # message may be about another lane); the rest of the gap is the main session's own
        evs = sorted(set([t_rep] + [g['first'] for g in ag] + [g['last'] for g in ag] + [h for g in ag for h in g['handbacks']] +
                         [r['first'] for r in crun] + [r['last'] for r in crun] + [r['sent'] for r in crun if r.get('sent')] +
                         [m[0] for m in mk] + [e['start'] for e in exes if e['name'] in used] +
                         [c['start'] for c in main['calls'] if c['name'] == 'SendMessage' and c['inp'].get('to') in {g['aid'] for g in ag}]))
        # THE WAITING RULE: a gap holding a Jarrod message is "wait on Jarrod" only when the lane was waiting for a reply at the gap's start (an
        # agent of the chain handed back, or a sheet of its run was sent into the chat, within WAIT_PENDING before it); otherwise nothing of the
        # lane was waiting on him and the span is "idle (cause unknown)" (a message in it may be about another lane: [L])
        pend = [h for g in ag for h in g['handbacks']] + [r['sent'] for r in crun if r.get('sent')]
        for x, y in zip(evs, evs[1:]):
            inside = [t for t in msgs if x < t < y]
            if inside:
                waiting = any(timedelta(0) <= x - h <= WAIT_PENDING for h in pend)
                tl.add('read-back / reply wait' if waiting else 'idle (cause unknown)', x, max(inside))
        lastagent = max(g['last'] for g in ag)
        for m in mk:
            if m[0] > lastagent:
                tl.add('merge wait', lastagent, m[0])
        sp = tl.split()
        rows.append(dict(chain=chain, keys=keys, t0=t_rep, t1=max(t_end, t_rep), total=(max(t_end, t_rep) - t_rep).total_seconds(), split=sp,
                         nruns=len(crun), nag=len(ag), open=is_open))
    others = [g for g in agents if g['lane'] == 'other']
    if others:
        cnt = defaultdict(int)
        for g in others:
            cnt[g['desc']] += 1
        notes.append('%d agents name no lane in their description (not in any chain); each description once, with its count:' % len(others))
        notes.extend('  %3d x %s' % (n, d) for d, n in sorted(cnt.items()))
    return sorted(rows, key=lambda r: r['t0']), notes


def LT_(t):
    return t.astimezone(TZ).strftime('%H:%M') if t else '-'


PROOF = [('compile', re.compile(r'(?:py|python)(?:\.exe)?\s+(?:-\w+\s+)*\S*game_syntax\.py')),
         ('check.py', re.compile(r'tools[\\/]+check[\\/]+check\.py|\bcheck\.py\b(?!.*handback)')),
         ('handback check', re.compile(r'handback[\\/]+check\.py')), ('selftest', re.compile(r'selftest|_unit\b|_unit\.py|_test\.py'))]
FAILRX = re.compile(r'\bFAIL(?:ED)?\b|\berror[: ]|Exit code [1-9]|rc=[1-9]|exit [1-9]\b', re.I)


def print_chains(rows, notes, runs):
    """the default report: one row per chain, minutes; then the not-derivable rows and the one drifting line"""
    print('== chains: first spawn -> last sheet in the chat / last merge (minutes; each minute ONE label: %s; night excluded) ==' % ' > '.join(PRIORITY[1:]))
    print('%-10s %5s %5s %5s | %5s %5s %5s %5s %5s | %5s %5s %5s %5s %5s | %4s %3s' % (
        'chain', 'spawn', 'end', 'total', 'probe', 'wait', 'idle', 'build', 'rev', 'merge', 'exe', 'run', 'sheet', 'other', 'runs', 'ag'))
    agg = defaultdict(float)
    drift, nights = [], 0.0
    for r in rows:
        sp = r['split']
        m = lambda k: sp.get(k, 0.0) / 60
        night = m('night')
        nights += night
        tot = r['total'] / 60 - night
        run_ = m('game run') + m('run launch')
        other = m('reviewer beside build') + m('route lane') + m('main session / between')
        wait = m('read-back / reply wait')
        work = m('probe') + m('builder') + m('reviewer own')
        print('%-10s %5s %5s %5.0f | %5.0f %5.0f %5.0f %5.0f %5.0f | %5.0f %5.0f %5.0f %5.0f %5.0f | %4d %3d%s' % (
            r['chain'][:10], LT_(r['t0']), LT_(r['t1']) + ('*' if r['open'] else ''), tot, m('probe'), wait, m('idle (cause unknown)'), m('builder'),
            m('reviewer own'), m('merge wait'), m('exe build'), run_, m('sheet step'), other, r['nruns'], r['nag'],
            '  (+%.0f min night)' % night if night else ''))
        for k, v in sp.items():
            if k != 'night':
                agg[k] += v
        if wait + m('merge wait') >= 10 and wait + m('merge wait') > work:
            drift.append('%s (wait %.0f > work %.0f min)' % (r['chain'], wait + m('merge wait'), work))
    tot = sum(agg.values())
    f = lambda *ks: sum(agg.get(k, 0.0) for k in ks) / 60
    print('all %d chains, %.0f chain-minutes: probe %.0f, wait on Jarrod %.0f, idle (cause unknown) %.0f, builder %.0f, reviewer own %.0f, merge wait %.0f, exe %.0f, run %.0f, sheet %.0f, other %.0f' % (
        len(rows), tot / 60, f('probe'), f('read-back / reply wait'), f('idle (cause unknown)'), f('builder'), f('reviewer own'), f('merge wait'), f('exe build'),
        f('game run', 'run launch'), f('sheet step'), f('reviewer beside build', 'route lane', 'main session / between')))
    print('drifting (wait on Jarrod / merge exceeds the lane\'s own work): ' + ('; '.join(drift) if drift else 'none'))
    print(REPORT_NOT_DERIVABLE)
    bad = [r for r in runs if not r['key']]
    if bad:
        print('run lane not derivable: %d of %d runs (%s)' % (len(bad), len(runs), '; '.join('run %s %s' % (r['tag'], r['how'].replace('not derivable: ', '')) for r in bad[:2]) + (' ...' if len(bad) > 2 else '')))
    print('* open: no sheet or merge yet, the end is the last agent record. KEY#2 = a later agent of the same exact key after a merge of it / over %d min '
          'idle. wait = "wait on Jarrod" [L]: a gap of the lane holding a Jarrod message counts as his only when an agent of the lane had handed back (or a '
          'sheet of its run was sent) within %d s before the gap began, up to his message; any other such gap is "idle" (cause unknown: nothing of the lane '
          'was waiting for a reply, the message may be about another lane).' % (CHAIN_GAP.total_seconds() // 60, WAIT_PENDING.total_seconds()))
    print('\n'.join(notes))


def reviewer_phases(agents):
    print('\n== reviewer: four phases (minutes of ACTIVE time; idle between rounds not counted) ==')
    print('  %-8s %-10s %6s %6s %6s %6s %6s %6s %6s %6s  %s' % ('lane', 'agent', 'beside', 'w_cmt', 'review', 'fixes', 'own', 'idle', 'wc_aft', 'tl_own', 'builder hand-back / first fix edit'))
    tot = defaultdict(float)
    for g in sorted([g for g in agents if g['role'] == 'reviewer'], key=lambda g: g['first']):
        bdone = builder_done(g, agents)
        wc = [c for c in g['calls'] if c['cls'] == 'wait:commit' and not c['bg']]
        wait = sum(c['dur'] for c in wc if not bdone or c['start'] < bdone)
        wait_after = sum(c['dur'] for c in wc if bdone and c['start'] >= bdone)
        fix = [c['start'] for c in g['calls'] if c['name'] in ('Edit', 'Write') and not str(c['inp'].get('file_path', '')).lower().endswith('.md')
               and 'temp' not in str(c['inp'].get('file_path', '')).lower() and (not bdone or c['start'] > bdone)]
        ffix = min(fix) if fix else None
        beside = review = fixes = 0.0
        for s, e in g['active']:
            s2 = s
            if bdone:
                beside += max(0.0, (min(e, bdone) - s).total_seconds())
                s2 = max(s, bdone)
            if e > s2:
                if ffix and ffix < e:
                    review += max(0.0, (min(e, ffix) - s2).total_seconds())
                    fixes += (e - max(s2, ffix)).total_seconds()
                else:
                    review += (e - s2).total_seconds()
        if not bdone:
            review += beside
            beside = 0.0
        beside_net = max(0.0, beside - wait)
        act = sum((e - s).total_seconds() for s, e in g['active'])
        idle = (g['last'] - g['first']).total_seconds() - act
        own = review + fixes
        tool_own = sum(c['dur'] for c in g['calls'] if not c['bg'] and c['res'] and c['cls'] != 'wait:commit' and (not bdone or c['start'] >= bdone))
        for k, v in (('beside', beside_net), ('wait_commit', wait), ('review', review), ('fixes', fixes), ('own', own), ('idle', idle),
                     ('wc_after', wait_after), ('tool_own', tool_own)):
            tot[k] += v
        print('  %-8s %-10s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f  %s %s' % (
            g['lane'], g['aid'][:10], beside_net / 60, wait / 60, review / 60, fixes / 60, own / 60, idle / 60, wait_after / 60, tool_own / 60, LT_(bdone), LT_(ffix)))
    print('  TOTAL              %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f' % tuple(tot[k] / 60 for k in (
        'beside', 'wait_commit', 'review', 'fixes', 'own', 'idle', 'wc_after', 'tool_own')))


def runs_section(runs, main, agents):
    print('\n== runs: lane (derived), launch (run_list call -> log first line), boot (-> main menu), game, sheet step (log end -> sheet file), '
          'in the chat (-> SendUserFile); seconds ==')
    sends = [e for e in main['events'] if e[1] == 'send']
    callers = [c for g in [main] + agents for c in g['calls']]
    tot = defaultdict(float)
    for r in runs:
        starts = [c['start'] for c in callers if 'run_list' in json.dumps(c['inp']) and r['route'] and r['route'] in json.dumps(c['inp']) and c['start'] <= r['first']]
        launch = (r['first'] - max(starts)).total_seconds() if starts else None
        boot = (r['menu'] - r['first']).total_seconds() if r['menu'] else None
        game = (r['last'] - r['first']).total_seconds()
        sheet = (r['sheet'] - r['last']).total_seconds() if r['sheet'] else None
        s = [e[0] for e in sends if r['folder'] in e[3].replace('\\\\', '\\') and e[0] >= r['last']]
        chat = (min(s) - (r['sheet'] or r['last'])).total_seconds() if s else None
        f = lambda x: '%6.0f' % x if x is not None else '     -'
        print('  run %-3s %-8s %s %-34s vsync=%-5s launch %s boot %s game %s sheet %s chat %s' % (
            r['tag'], r['key'] or 'not derivable', LT_(r['first']), (r['route'] or '?')[:34], r['vsync'], f(launch), f(boot), f(game), f(sheet), f(chat)))
        if not r['key']:
            print('      %s' % r['how'])
        for k, v in (('launch', launch), ('boot', boot), ('game', game), ('sheet', sheet), ('chat', chat)):
            if v is not None and v < 3600:
                tot[k] += v
                tot[k + '_n'] += 1
    print('  sums (minutes; runs counted): ' + ', '.join('%s %.1f (%d)' % (k, tot[k] / 60, tot[k + '_n']) for k in ('launch', 'boot', 'game', 'sheet', 'chat')))


def admit_section(agents, t0, t1):
    print('\n== admission gate (out\\harness\\admit\\history.tsv in the session window): grants, waits, and agent calls that sat in it ==')
    p = OUT('harness', 'admit', 'history.tsv')
    by = defaultdict(lambda: [0, 0.0])
    big = []
    if os.path.exists(p):
        lines = open(p, encoding='utf-8').read().splitlines()
        hdr = lines[0].split('\t')
        lo, hi = t0.astimezone(TZ).strftime('%Y-%m-%d %H:%M'), t1.astimezone(TZ).strftime('%Y-%m-%d %H:%M')
        for line in lines[1:]:
            f = dict(zip(hdr, line.split('\t')))
            if not f.get('time') or not (lo <= f['time'] <= hi):
                continue
            try:
                w = float(f.get('waited_s') or 0)
            except ValueError:
                w = 0.0
            by[f['kind']][0] += 1
            by[f['kind']][1] += w
            if w > 60:
                big.append((f['time'], f['kind'], w, f.get('outcome'), f.get('commit_left_gb')))
        print('  grants in the window: %d' % sum(v[0] for v in by.values()))
        for k, v in sorted(by.items()):
            print('  %-16s grants %4d  waited %6.1f min' % (k, v[0], v[1] / 60))
        for b in big:
            print('  > 60 s: %s %-14s waited %5.0f s  %s  commit left %s GB' % b)
    else:
        print('  not derivable: %s is missing' % p)
    n = 0
    for g in agents:
        for c in g['calls']:
            out = (c['head'] + c['tail']) if not c['bg'] else (c['bgout'] or '')
            if re.search(r'\[admit\]\s*waiting|admission gate', out, re.I) and (c['dur'] > 60 or (c['dur_bg'] or 0) > 60):
                n += 1
                print('  agent %s %-8s %-10s %s %6.0f s  %s' % (g['aid'][:10], g['lane'], g['role'], LT_(c['start']), c['dur'] or c['dur_bg'],
                                                           re.sub(r'\s+', ' ', (c['inp'].get('command') or ''))[:70]))
    if not n:
        print('  no agent call longer than 60 s names the gate in its result')


def reaction(main, agents, nights):
    """every agent hand-back -> the main session's next act for that lane (a SendMessage to the agent, a spawn of the same chain, a merge naming
    the agent's branch, a lane_exe naming the lane), night excluded; then the main session's own time"""
    print('\n== main-session reaction: agent hand-back -> next act on that lane (minutes; night excluded) ==')
    cmap = agents_chain_map(agents)
    acts = []
    for c in main['calls']:
        j = json.dumps(c['inp'])
        if c['name'] == 'SendMessage':
            acts.append((c['start'], c['inp'].get('to', ''), ''))
        elif c['name'] == 'Agent':
            acts.append((c['start'], '', cmap.get(lane_key(c['inp'].get('description', '')), '')))
        elif c['name'] in ('Bash', 'PowerShell'):
            for m in re.finditer(r'worktree-agent-([0-9a-f]{17})', j):
                if 'merge' in j:
                    acts.append((c['start'], m.group(1), ''))
            m = re.search(r'-Lane s\d+([a-z0-9]+)', j)
            if m:
                acts.append((c['start'], '', cmap.get(m.group(1).upper(), '')))
    rows = defaultdict(list)
    for g in agents:
        for i, hb in enumerate(g['handbacks']):
            final = i == len(g['handbacks']) - 1
            ch = cmap.get(g['lane'], '')
            nxt = [t for t, aid, lane in acts if t > hb and (aid == g['aid'] or (lane and lane == ch))]
            if not nxt:
                continue
            t = min(nxt)
            d = (t - hb).total_seconds()
            for n0, n1 in nights:
                d -= max(0.0, (min(t, n1) - max(hb, n0)).total_seconds())
            kind = ('plan stop' if g['role'] == 'probe-builder' and not final else 'probe done' if g['role'] == 'probe' else
                    'build done' if g['role'] in ('builder', 'probe-builder') else 'review done' if g['role'] == 'reviewer' else 'other')
            rows[kind].append((d, g['lane'], LT_(hb), LT_(t)))
    for k, v in sorted(rows.items()):
        ds = [x[0] for x in v]
        print('  %-12s n %3d  total %6.1f  median %5.1f  p90 %5.1f  max %5.1f   longest: %s' % (
            k, len(v), sum(ds) / 60, pct(ds, .5) / 60, pct(ds, .9) / 60, max(ds) / 60,
            ', '.join('%s %s->%s %.0f' % (x[1], x[2], x[3], x[0] / 60) for x in sorted(v, reverse=True)[:4])))
    wall = (main['last'] - main['first']).total_seconds()
    night = sum((b - a).total_seconds() for a, b in nights)
    model = sum((e - s).total_seconds() for s, e in main['replies'] if e > s)
    fg = sum(c['dur'] for c in main['calls'] if not c['bg'] and c['cls'] != 'wait:commit')
    print('  the main session itself: wall %.0f min (night %.0f); model replies %.0f; foreground tools %.0f' % (wall / 60, night / 60, model / 60, fg / 60))
    bg = defaultdict(lambda: [0, 0.0])
    for c in main['calls']:
        if c['bg'] and c['done']:
            k = 'lane_exe' if 'lane_exe' in json.dumps(c['inp']) else 'run_list' if 'run_list' in json.dumps(c['inp']) else c['cls']
            bg[k][0] += 1
            bg[k][1] += (c['done'] - c['start']).total_seconds()
    for k, v in sorted(bg.items(), key=lambda kv: -kv[1][1])[:6]:
        print('    background %-22s n %3d  %6.1f min (start -> completion notice)' % (k, v[0], v[1] / 60))
    print('  Jarrod messages: %d; files sent into the chat: %d' % (sum(e[1] == 'jarrod' for e in main['events']), sum(e[1] == 'send' for e in main['events'])))


def reviewer_repeats(agents):
    """the reviewer's compile / check / selftest runs split: before its own first fix edit (a repeat of the builder's proof) and after it
    (a check of its own fix); a background run's result is read from its output file"""
    print('\n== reviewer proof runs: repeat of the builder (no own edit yet) vs check of its own fix; failing = FAIL / error / non-zero ==')
    tab = defaultdict(lambda: [0, 0, 0.0])
    for g in agents:
        if g['role'] != 'reviewer':
            continue
        bd = builder_done(g, agents)
        fix = [c['start'] for c in g['calls'] if c['name'] in ('Edit', 'Write') and not str(c['inp'].get('file_path', '')).lower().endswith('.md')
               and 'temp' not in str(c['inp'].get('file_path', '')).lower() and (not bd or c['start'] > bd)]
        for c in g['calls']:
            cmd = c['inp'].get('command') or ''
            if c['name'] not in ('Bash', 'PowerShell'):
                continue
            for kind, rx in PROOF:
                if rx.search(cmd):
                    phase = 'own fix' if any(f < c['start'] for f in fix) else ('repeat (beside)' if bd and c['start'] < bd else 'repeat')
                    out = (c['bgout'] or '') if c['bg'] else c['head'] + c['tail']
                    bad = c['err'] or (FAILRX.search(out) and not re.search(r'\b0 FAIL|FAIL 0|0 failed', out))
                    cell = tab[(kind, phase)]
                    cell[0] += 1
                    cell[1] += bool(bad)
                    cell[2] += (c['dur_bg'] or 0.0) if c['bg'] else c['dur']
                    break
    for (kind, phase), v in sorted(tab.items()):
        print('  %-14s %-16s runs %4d  failing %3d  %6.1f min' % (kind, phase, v[0], v[1], v[2] / 60))


def new_sections(a, sess, tz):
    agents, main = load_session(sess)
    if not agents or not main['first']:
        print('no subagents under %s' % sess)
        return
    t0, t1 = main['first'], main['last']
    nights = find_nights(main['times'] + [t for g in agents for t in g['times']], 3600)
    show = lambda s: a.all or getattr(a, s)
    if show('chains') or show('runs'):
        exes = [e for e in lane_exe_rows() if t0 - timedelta(hours=1) <= e['end'] <= t1 + timedelta(hours=1)]
        keys = {g['lane'] for g in agents if g['lane'] != 'other'}
        merges = git_merges(a.repo, t0, t1 + timedelta(hours=2))
        runs = launch_lanes(run_logs(t0, t1, exes, keys), agents, main, exes, merges, keys)
        rows, notes = chain_table(agents, main, merges, exes, runs, dict(tz=tz, nights=nights))
        if show('chains'):
            print_chains(rows, (['nights (no record for over 60 min): ' + ', '.join('%s-%s' % (LT_(x), LT_(y)) for x, y in nights)] if nights else []) + notes, runs)
        if show('runs'):
            runs_section(runs, main, agents)
    if show('reviewer'):
        reviewer_phases(agents)
    if show('admit'):
        admit_section(agents, t0, t1)
    if show('reaction'):
        reaction(main, agents, nights)
    if show('repeats'):
        reviewer_repeats(agents)


def run(a):
    sess = a.session
    if not sess:
        cands = [p[:-len('subagents') - 1] for p in glob.glob(os.path.join(PROJ, '*', 'subagents'))]
        sess = max(cands, key=os.path.getmtime)
    global TZ
    sess = resolve_session(sess)
    tz = TZ = timezone(timedelta(hours=a.utc_offset))
    main_root(a.repo)
    newsec = a.chains or a.reviewer or a.runs or a.admit or a.reaction or a.repeats
    oldsec = a.agents or a.classes or a.handoffs or a.serial
    if not (newsec or oldsec or a.all):
        a.chains = True                 # the default: the chain table and the drifting line
    allsec = a.all
    agents = []
    if oldsec or allsec:
        agents = [agent_stats(p) for p in sorted(glob.glob(os.path.join(sess, 'subagents', '*.jsonl')))]
        agents = [g for g in agents if g['first']]
    L = lambda t: t.astimezone(tz).strftime('%H:%M:%S')
    if a.all or a.chains or a.reviewer or a.runs or a.admit or a.reaction or a.repeats:
        new_sections(a, sess, tz)
    if a.agents or allsec:
        print('== agents (minutes; wall = first..last record; model/client/tool/waiting as in the header) ==')
        print('(tool = reply end -> result, wait_commit / wait_any calls NOT in it; wcmt = minutes blocked in those; wait = idle between rounds)')
        print('%-10s %-12s %-18s %-6s %5s %5s %3s %6s %6s %6s %6s %6s %6s %5s  %s' % ('agent', 'type', 'model', 'effort', 'turns', 'calls', 'rd', 'wall', 'model', 'client', 'tool', 'wcmt', 'wait', 'm%', 'description'))
        tot = defaultdict(float)
        for g in sorted(agents, key=lambda g: g['first']):
            act = g['model'] + g['client'] + g['tool']
            print('%-10s %-12s %-18s %-6s %5d %5d %3d %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %4.0f%%  %s %s' % (
                g['aid'][:10], g['atype'][:12], ','.join(g['models'])[:18], ','.join(g['efforts']), g['turns'], g['ncalls'], g['rounds'],
                g['wall'] / 60, g['model'] / 60, g['client'] / 60, g['tool'] / 60, g['wcall'] / 60, g['waiting'] / 60, 100 * g['model'] / max(act, 1), L(g['first']), g['desc'][:44]))
            k = g['atype']
            for f in ('wall', 'model', 'client', 'tool', 'wcall', 'waiting', 'turns', 'ncalls'):
                tot[(k, f)] += g[f]
                tot[('ALL', f)] += g[f]
        print('\nby agent type (minutes, sums):')
        for k in sorted({k for k, _ in tot}):
            act = tot[(k, 'model')] + tot[(k, 'client')] + tot[(k, 'tool')]
            print('  %-12s turns %5d calls %5d wall %7.1f  model %7.1f (%3.0f%% of active)  client %6.1f  tool %7.1f  wait_commit %6.1f  waiting %6.1f' % (
                k, tot[(k, 'turns')], tot[(k, 'ncalls')], tot[(k, 'wall')] / 60, tot[(k, 'model')] / 60, 100 * tot[(k, 'model')] / max(act, 1),
                tot[(k, 'client')] / 60, tot[(k, 'tool')] / 60, tot[(k, 'wcall')] / 60, tot[(k, 'waiting')] / 60))
        mods = [p['model'] for g in agents for p in g['per_req'] if p['model'] > 0]
        cli = [p['client'] for g in agents for p in g['per_req'] if 0 < p['client'] < 600]
        print('  per turn: model median %.1f s p90 %.1f s; client median %.2f s p90 %.2f s' % (pct(mods, .5), pct(mods, .9), pct(cli, .5), pct(cli, .9)))
    if a.classes or allsec:
        by = defaultdict(list)
        byn = defaultdict(list)
        waitc = []
        for g in agents:
            for c in g['calls']:
                if c['cls'] == 'wait:commit':
                    waitc.append(c['dur'])          # waiting for another agent's commit, parallel to the builder: not tool time
                elif not c['bg']:
                    by[c['cls']].append(c['dur'])
                    byn[c['name']].append(c['dur'])
        print('(a call\'s time = its result minus the END OF ITS REPLY: an Edit sharing a reply with other blocks is not charged the model\'s streaming)')
        eds = [c for g in agents for c in g['calls'] if c['name'] == 'Edit' and not c['bg']]
        if eds:
            print('  Edit from its own block: median %.2f s total %.1f min; from the reply end (printed below): median %.2f s total %.1f min' % (
                pct([c['dur_block'] for c in eds], .5), sum(c['dur_block'] for c in eds) / 60, pct([c['dur'] for c in eds], .5), sum(c['dur'] for c in eds) / 60))
        print('  waiting, not tool: wait_commit / wait_any n %d  total %.1f min  median %.1f s  max %.1f s' % (
            len(waitc), sum(waitc) / 60, pct(waitc, .5), max(waitc or [0])))
        print('\n== tool calls by tool (foreground only; seconds) ==')
        for k, v in sorted(byn.items(), key=lambda kv: -sum(kv[1])):
            print('  %-18s n %4d  total %7.1f  median %6.2f  p90 %6.2f  max %7.1f' % (k, len(v), sum(v), pct(v, .5), pct(v, .9), max(v)))
        print('\n== tool calls by class, slowest total first (foreground only; seconds) ==')
        for k, v in sorted(by.items(), key=lambda kv: -sum(kv[1]))[:a.top * 3]:
            print('  %-24s n %4d  total %7.1f  median %6.2f  p90 %6.2f  max %7.1f' % (k, len(v), sum(v), pct(v, .5), pct(v, .9), max(v)))
        grp = defaultdict(list)
        for k, v in by.items():
            grp['sh:git (all verbs)' if k.startswith('sh:git') else 'sh:q (all verbs)' if k.startswith('sh:q ') else k].extend(v)
        print('\n== grouped (git verbs together, q verbs together), slowest total first ==')
        for k, v in sorted(grp.items(), key=lambda kv: -sum(kv[1]))[:a.top]:
            print('  %-24s n %4d  total %7.1f  median %6.2f  p90 %6.2f  max %7.1f' % (k, len(v), sum(v), pct(v, .5), pct(v, .9), max(v)))
        sh = [c for g in agents for c in g['calls'] if c['name'] in ('Bash', 'PowerShell')]
        gr = [c for g in agents for c in g['calls'] if c['name'] == 'Grep']
        print('\nshell calls (Bash + PowerShell): %d; Grep calls: %d; background calls: %d' % (len(sh), len(gr), sum(c['bg'] for g in agents for c in g['calls'])))
    if a.handoffs or allsec:
        ev = main_events(sess + '.jsonl')
        byid = {g['aid']: g for g in agents}
        lanes = defaultdict(list)
        for e in ev:
            g = byid.get(e[2])
            if g:
                lanes[lane_key(g['desc'])].append((e[0], e[1], g['atype'], g['desc'][:30]))
        if agents:      # merges: the session's first-parent merges, each put on the lanes its subject names
            for when, h, keys, subj in git_merges(a.repo, min(g['first'] for g in agents), max(g['last'] for g in agents) + timedelta(hours=12)):
                for k in keys:
                    if k in lanes:
                        lanes[k].append((when, 'merge', '', h))
            for ln in lanes:
                lanes[ln].sort(key=lambda x: x[0])
        print('\n== hand-offs per lane (main session timeline; gap = since the previous event of the same lane) ==')
        gaps = defaultdict(list)
        for ln in sorted(lanes):
            print('  lane %s' % ln)
            prev = None
            for t, k, at, d in lanes[ln]:
                gap = (t - prev[0]).total_seconds() if prev else 0.0
                tag = ''
                if prev and prev[1] == 'handback_queued' and k == 'handback_taken':
                    tag = 'queue wait'
                elif prev and prev[1] in ('handback_taken', 'handback_queued') and k in ('spawn', 'resume', 'merge'):
                    tag = 'main session -> next ' + k
                if tag:
                    gaps[tag.split(' -> ')[0] if tag == 'queue wait' else 'decide->' + k].append(gap)
                print('    %s %-16s %-12s %6.1f min %-26s %s' % (L(t), k, at, gap / 60, tag, d))
                prev = (t, k)
        print('  summary (minutes):')
        for k, v in sorted(gaps.items()):
            print('    %-18s n %3d  total %6.1f  median %5.2f  max %5.1f' % (k, len(v), sum(v) / 60, pct(v, .5) / 60, max(v) / 60))
    if a.serial or allsec:
        runs, cur = [], []
        for g in agents:
            for p in g['per_req']:
                cs = [c for c in g['calls'] if c['req'] == p['id']]
                if len(cs) == 1 and read_only(cs[0]['name'], cs[0]['inp']) and not cs[0]['bg']:
                    cur.append((p, cs[0]))
                    continue
                if len(cur) > 1:
                    runs.append(cur)
                cur = []
            if len(cur) > 1:
                runs.append(cur)
            cur = []
        save = sum(sum(p['model'] + p['client'] for p, _ in r[1:]) + sum(c['dur'] for _, c in r) - max(c['dur'] for _, c in r) for r in runs)
        n = sum(len(r) for r in runs)
        allcalls = sum(g['ncalls'] for g in agents)
        print('\n== serial read-only calls (upper bound: no dependency assumed) ==')
        print('  runs of >= 2 consecutive single-read-only replies: %d runs, %d calls (of %d calls); upper-bound saving %.1f min' % (len(runs), n, allcalls, save / 60))
        lens = defaultdict(int)
        for r in runs:
            lens[min(len(r), 6)] += 1
        print('  run lengths: ' + ', '.join('%s%s: %d' % (k, '+' if k == 6 else '', v) for k, v in sorted(lens.items())))
    return 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--session')
    ap.add_argument('--utc-offset', type=float, default=11)
    ap.add_argument('--repo', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--top', type=int, default=10)
    for f in ('chains', 'reviewer', 'runs', 'admit', 'reaction', 'repeats', 'agents', 'classes', 'handoffs', 'serial', 'all'):
        ap.add_argument('--' + f, action='store_true')
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except (AttributeError, ValueError):
        pass
    sys.exit(run(ap.parse_args()))

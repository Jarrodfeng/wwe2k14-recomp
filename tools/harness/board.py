r"""The board (Session 106, lane B1; audit docs\research\FABLE_AUDIT_S105.md F4a, E; contract docs\research\s106\B1_contract.md).

One command prints the state of a session from git, the main session's transcript and the tools' logs, read-only:
the clock and time since the start, each lane (agent) with its state and the time of every link (started, hand-backs, follow-ups, first
commit, merged), what each lane waits on and for how long, the plan rows that are free to start and not running, the game runs counted
from the game logs (THE run count), lanes / games alive.

    py -I tools\harness\board.py [--session ID] [--rows s106|s105|FILE] [--all] [--json]
    py -I tools\harness\board.py --fixed                       # the summary's fixed block (markdown)
    py -I tools\harness\board.py --write-fixed SESSIONNNN_SUMMARY.md
    py -I tools\harness\board.py --audit [--rows s105 --session ID]   # A1 / A2 / A3 tables of the audit's section A
    py -I tools\harness\board.py --selftest                    # the plants (also tools\harness\board_selftest.py)

Data is read from the MAIN checkout (--repo, default: found through git's common dir) by absolute path:
out\harness\runlock\history.tsv, out\build\*\logs, %LOCALAPPDATA%\wwe2k14-admit\history.tsv, the transcripts under ~\.claude\projects\<project>.
Git is read from the checkout the tool lives in (one object store). Transcripts are streamed line by line; only lines that can hold an Agent /
SendMessage / Workflow tool use or a task notification are parsed. Nothing prints message text: counts, times and the agents' own short
descriptions only. Times are shown in the OS's local time zone (never a constant offset). Writes nothing except --write-fixed FILE.

Sources and where each lives:
  transcript   <claude projects>\<project>\<session>.jsonl   Agent / SendMessage / Workflow tool uses, <task-notification> queue entries
  subagents    <claude projects>\<project>\<session>\subagents\agent-<id>.jsonl + .meta.json (description, toolUseId, worktreeBranch, model,
               finished). A Workflow run's agents have no Agent tool use: any meta whose toolUseId is not an Agent use is listed as a workflow lane.
  git          merges "Merge branch 'X' into <branch>", each merge's first commit (M^1..M^2)
  run lock     out\harness\runlock\history.tsv
  game logs    out\build\*\logs\wwe2k14_NNN.log, out\exe\*\logs\wwe2k14_NNN.log by write time
  compile queue %LOCALAPPDATA%\wwe2k14-admit\history.tsv
  plan rows    tools\harness\board_rows\<name>.json: {"session","branch","window":[start,end],"check_log","run_estimate","rows":[{"id","needs":[..],
               "match":[regex over agent descriptions / branch names],"settle":"merge"|"done","desc"}]}; first matching row wins (put probe rows first);
               a need "A|B" means either; "go:X" is satisfied by --go X.
"""
import argparse
import collections
import csv
import glob
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ROWS_DIR = os.path.join(HERE, 'board_rows')
FMT_GIT = '%Y-%m-%dT%H:%M:%S'


# ---------------------------------------------------------------- time
def utc_to_local(s):
    """'2026-11-20T05:33:47.1Z' -> naive local datetime, through the OS's time zone rules (not a constant offset)."""
    return datetime.strptime(s[:19], FMT_GIT).replace(tzinfo=timezone.utc).astimezone().replace(tzinfo=None)


def pt(s, fmt='%Y-%m-%d %H:%M:%S'):
    try:
        return datetime.strptime(s[:19], fmt)
    except (ValueError, TypeError):
        return None


class Clock:
    """Formats times as HH:MM, with +Nd when the day differs from the session's first day."""

    def __init__(self, base):
        self.base = base.date() if base else None

    def hm(self, t):
        if t is None:
            return '--:--'
        s = t.strftime('%H:%M')
        if self.base and t.date() != self.base:
            s += '+%dd' % (t.date() - self.base).days
        return s


def mins(a, b):
    return (b - a).total_seconds() / 60.0


def dur(minutes):
    if minutes is None:
        return '?'
    m = int(round(minutes))
    return '%dh%02d' % (m // 60, m % 60) if m >= 60 else '%d min' % m


# ---------------------------------------------------------------- paths
def git(args, cwd=None, timeout=60):
    try:
        r = subprocess.run(['git'] + args, cwd=cwd or ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
        return r.stdout
    except Exception:
        return ''


def main_checkout():
    common = git(['rev-parse', '--path-format=absolute', '--git-common-dir']).strip()
    if common:
        return os.path.dirname(common)
    return ROOT


def projects_dir():
    base = os.path.join(os.path.expanduser('~'), '.claude', 'projects')
    # the project folder name is the main checkout's path with separators turned into '-'
    want = re.sub(r'[^A-Za-z0-9]', '-', main_checkout())
    p = os.path.join(base, want)
    if os.path.isdir(p):
        return p
    c = glob.glob(os.path.join(base, '*wwe2k14-recomp'))
    return c[0] if c else p


def _tail_info(path, nbytes=65536):
    """(last internal timestamp (naive local) or None, last gitBranch or None) read from the END of a transcript: a file's write time is not
    its session's time (S106 B1F: a timestamp-less cost-state line made S101's file newer than S105's)."""
    try:
        with open(path, 'rb') as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - nbytes))
            tail = f.read().decode('utf-8', errors='replace')
    except OSError:
        return None, None
    ts = [m.group(1) for m in re.finditer(r'"timestamp":"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)', tail)]
    gb = re.findall(r'"gitBranch":"([^"]*)"', tail)
    return (utc_to_local(max(ts)) if ts else None), (gb[-1] if gb else None)


def pick_session(pdir, branch=None, now=None):
    """The default session: newest by last INTERNAL timestamp, preferring transcripts whose gitBranch is `branch`; two transcripts written in
    the last 30 min and nothing that says which is ours -> refuse (pass --session)."""
    now = now or datetime.now()
    info = []
    for p in glob.glob(os.path.join(pdir, '*.jsonl')):
        t, gb = _tail_info(p)
        if t is not None:
            info.append((t, gb, os.path.splitext(os.path.basename(p))[0]))
    if not info:
        return None
    info.sort()
    mine = [x for x in info if branch and x[1] == branch]
    pool = mine or info
    live = [x for x in pool if (now - x[0]) < timedelta(minutes=30)]
    if len(live) > 1 and not mine:
        raise SystemExit('board: %d transcripts were written in the last 30 min (%s); pass --session ID or set "session" in the rows file' %
                         (len(live), ', '.join(x[2][:8] for x in live)))
    return pool[-1][2]


def newest_session(pdir, branch=None):
    return pick_session(pdir, branch)


def clock_state(last, realnow, sub_mtimes, window):
    """(live, now). LIVE = the main transcript or any subagent file was written in the last 30 min; it is a label. With no fixed window the board's
    'now' is always the real now (an idle main session waiting on agents does not end the window: S106 B1F, finding 3). With a fixed window the
    old rule stays: a finished session's clock stops at its last write."""
    ws = [last] + [m for m in (sub_mtimes or []) if m]
    live = any((realnow - w) < timedelta(minutes=30) for w in ws if w)
    if window:
        return live, (realnow if live else last)
    return live, realnow


# ---------------------------------------------------------------- reader 1: the transcript (streamed)
_TS = '"timestamp":"'
_MID = re.compile(r'"id":"(msg_[A-Za-z0-9_]+)"')
_TUID = re.compile(r'"tool_use_id":"([^"]+)"')


def read_transcript(path):
    """Stream the transcript; returns dict(first, last, uses{id: {t,name,desc,type,to}}, notes[(t,task,tool_use,status)], assistant_msgs, user_msgs)."""
    out = {'first': None, 'last': None, 'uses': {}, 'notes': [], 'assistant_msgs': 0, 'user_msgs': 0}
    msgs = set()
    with open(path, encoding='utf-8', errors='replace') as f:
        for l in f:
            i = l.find(_TS)
            if i >= 0:
                s = l[i + len(_TS):i + len(_TS) + 19]
                if out['first'] is None or s < out['first']:
                    out['first'] = s
                if out['last'] is None or s > out['last']:
                    out['last'] = s
            if '"type":"queue-operation"' in l and '<task-notification>' in l:
                try:
                    d = json.loads(l)
                except ValueError:
                    continue
                if d.get('operation') != 'enqueue' or not d.get('timestamp'):
                    continue
                c = d.get('content') or ''
                if not isinstance(c, str):
                    continue
                m = re.search(r'<task-id>([^<]+)</task-id>', c)
                tu = re.search(r'<tool-use-id>([^<]+)</tool-use-id>', c)
                st = re.search(r'<status>([^<]+)</status>', c)
                if m:
                    out['notes'].append((utc_to_local(d['timestamp']), m.group(1), tu.group(1) if tu else '', st.group(1) if st else ''))
            elif '"type":"user"' in l and '"tool_result"' in l and out['uses']:
                ids = [x for x in _TUID.findall(l) if x in out['uses']]
                if not ids:
                    continue
                try:
                    d = json.loads(l)
                except ValueError:
                    continue
                c = (d.get('message') or {}).get('content')
                for b in c if isinstance(c, list) else []:
                    u = out['uses'].get(b.get('tool_use_id')) if isinstance(b, dict) and b.get('type') == 'tool_result' else None
                    if u is None:
                        continue
                    u['err'] = bool(b.get('is_error'))
                    bc = b.get('content')
                    txt = bc if isinstance(bc, str) else ' '.join(x.get('text', '') for x in bc if isinstance(x, dict)) if isinstance(bc, list) else ''
                    if u['name'].startswith('Workflow') and not u['err']:   # only these fields are kept, never the text
                        m1 = re.search(r'Run ID:\s*(wf_[A-Za-z0-9\-]+)', txt)
                        m2 = re.search(r'Summary:\s*([^\r\n]+)', txt)
                        m3 = re.search(r'Transcript dir:\s*([^\r\n]+)', txt)
                        u['run_id'] = m1.group(1) if m1 else ''
                        u['summary'] = m2.group(1).strip() if m2 else ''
                        u['tdir'] = m3.group(1).strip() if m3 else ''
            elif '"type":"assistant"' in l:
                mm = _MID.search(l)
                if mm:
                    msgs.add(mm.group(1))
                if '"tool_use"' in l and ('"name":"Agent"' in l or '"name":"SendMessage"' in l or '"name":"Workflow' in l or '"name":"Task"' in l):
                    try:
                        d = json.loads(l)
                    except ValueError:
                        continue
                    if not d.get('timestamp'):
                        continue
                    for b in (d.get('message') or {}).get('content') or []:
                        if b.get('type') != 'tool_use':
                            continue
                        nm = b.get('name') or ''
                        if nm in ('Agent', 'Task', 'SendMessage') or nm.startswith('Workflow'):
                            inp = b.get('input') or {}
                            out['uses'][b['id']] = {'t': utc_to_local(d['timestamp']), 'name': nm,
                                                    'desc': inp.get('description') or inp.get('name') or inp.get('workflow') or '',
                                                    'type': inp.get('subagent_type') or '', 'to': inp.get('to') or '', 'err': False}
    out['assistant_msgs'] = len(msgs)
    return out


def read_subagents(pdir, session):
    """meta.json of every subagent: {agent id: {desc, tool_use, branch, model, finished, worktree, first, last, mtime}}."""
    res = {}
    d = os.path.join(pdir, session, 'subagents')
    for mp in glob.glob(os.path.join(d, 'agent-*.meta.json')):
        try:
            m = json.load(open(mp, encoding='utf-8'))
        except Exception:
            continue
        aid = os.path.basename(mp)[len('agent-'):-len('.meta.json')]
        jp = mp[:-len('.meta.json')] + '.jsonl'
        mt = datetime.fromtimestamp(os.path.getmtime(jp)) if os.path.exists(jp) else None
        res[aid] = {'desc': m.get('description') or '', 'tool_use': m.get('toolUseId') or '', 'branch': m.get('worktreeBranch') or '',
                    'model': m.get('model') or '', 'finished': bool(m.get('finished')), 'worktree': bool(m.get('spawnedWithWorktree')),
                    'type': m.get('agentType') or '', 'mtime': mt, 'jsonl': jp}
    return res


KIND_RE = [('shot-checker', r'shot-?checker'), ('review', r'\breview'), ('adversary', r'adversar|skeptic'), ('fix', r'\bfix(?:\s*round)?\b'),
           ('probe', r'probe|settler|reader|research|survey'), ('pack', r'\bpack\b'), ('builder', r'builder|lane\b|tool lane')]
FIRST_WORD = {'builder': 'builder', 'fix': 'fix', 'review': 'review', 'reviewer': 'review', 'probe': 'probe', 'settler': 'probe', 'adversary': 'adversary',
              'skeptic': 'adversary', 'shot-checker': 'shot-checker', 'shotchecker': 'shot-checker', 'pack': 'pack', 'reader': 'probe', 'research': 'probe'}


def kind_of(desc):
    """The kind from the first word after the model prefix (OPUS / SON / HAIKU ...: "SON fix round A6S" is a fix, "OPUS adversary X fix" an adversary), else a regex."""
    d = (desc or '').strip()
    words = re.findall(r'[A-Za-z][A-Za-z\-]*', d)
    if words and words[0].upper() in ('OPUS', 'SON', 'SONNET', 'HAIKU', 'FABLE', 'OPS', 'HAI') and len(words) > 1:
        words = words[1:]
    if words and words[0].lower() in FIRST_WORD:
        return FIRST_WORD[words[0].lower()]
    for k, rx in KIND_RE:
        if re.search(rx, d, re.I):
            return k
    return 'other'


def read_workflows(pdir, session, tr):
    """Workflow runs named by the transcript's Workflow tool results: {run_id: {'agents': [{id,label,phase,branch,mtime}], 'summary', 'mtime'}}.
    Their agents live at <session>\\subagents\\workflows\\<runId>\\agent-*.meta.json (no toolUseId / worktreeBranch: the branch is
    'worktree-' + the worktree folder's name) and journal.jsonl ('started' / 'result' lines with agentId, label, phase)."""
    res = {}
    for tu, u in tr['uses'].items():
        if not u['name'].startswith('Workflow') or u.get('err') or not u.get('run_id'):
            continue
        run = u['run_id']
        wd = u.get('tdir') or os.path.join(pdir, session, 'subagents', 'workflows', run)
        if not os.path.isdir(wd):
            wd = os.path.join(pdir, session, 'subagents', 'workflows', run)
        ags = {}
        try:
            for l in open(os.path.join(wd, 'journal.jsonl'), encoding='utf-8', errors='replace'):
                try:
                    j = json.loads(l)
                except ValueError:
                    continue
                if j.get('type') == 'started' and j.get('agentId'):
                    ags.setdefault(j['agentId'], {'id': j['agentId'], 'label': j.get('label') or '', 'phase': j.get('phase') or '', 'branch': '', 'mtime': None})
        except OSError:
            pass
        for mp in glob.glob(os.path.join(wd, 'agent-*.meta.json')):
            aid = os.path.basename(mp)[len('agent-'):-len('.meta.json')]
            try:
                m = json.load(open(mp, encoding='utf-8'))
            except Exception:
                continue
            a = ags.setdefault(aid, {'id': aid, 'label': m.get('description') or '', 'phase': m.get('workflowPhase') or '', 'branch': '', 'mtime': None})
            wt = m.get('worktreePath') or ''
            if wt:
                a['branch'] = 'worktree-' + re.split(r'[\\/]', wt.rstrip('\\/'))[-1]
            jp = mp[:-len('.meta.json')] + '.jsonl'
            if os.path.exists(jp):
                a['mtime'] = datetime.fromtimestamp(os.path.getmtime(jp))
        res[run] = {'agents': list(ags.values()), 'summary': u.get('summary') or '', 'mtime': max([a['mtime'] for a in ags.values() if a['mtime']] or [None])}
    return res


PROBE_KINDS = ('probe', 'workflow', 'other', 'adversary', 'pack', 'shot-checker')   # what a probe row (settle: done) takes
BUILD_KINDS = ('builder', 'fix')                                                    # what a build row (settle: merge) takes
_TYPE_KIND = {'reviewer': 'review', 'shot-checker': 'shot-checker', 'lane-probe': 'probe', 'explorer': 'probe'}


def _first_word_kind(desc):
    words = re.findall(r'[A-Za-z][A-Za-z\-]*', (desc or '').strip())
    if words and words[0].upper() in ('OPUS', 'SON', 'SONNET', 'HAIKU', 'FABLE', 'OPS', 'HAI') and len(words) > 1:
        words = words[1:]
    return FIRST_WORD.get(words[0].lower()) if words else None


def classify(desc, atype='', worktree=False):
    """Kind from the agent's own facts first (agentType / worktree meta), the description second (S106 B1F: 'OPUS C4 lane exe build path' is a
    probe, 'SON B1 the board' is a builder). A name's regex alone never makes a builder: it needs a worktree, the lane-builder type or the word
    'builder' first."""
    t = _TYPE_KIND.get(atype or '')
    if t:
        return t
    k = kind_of(desc)
    if k in ('review', 'adversary', 'shot-checker', 'pack'):
        return k
    if k == 'fix':
        return 'fix'
    if atype == 'lane-builder' or (worktree and k in ('other', 'builder')):
        return 'builder'
    if k == 'builder' and _first_word_kind(desc) != 'builder':
        return 'other'
    return k


def is_builder_class(a):
    return a.get('kind') in BUILD_KINDS or (a.get('worktree') and a.get('kind') not in ('review', 'probe', 'adversary', 'workflow')) or a.get('type') == 'lane-builder'


def tag_of(desc):
    m = re.search(r'(?:builder|review|probe|adversary|fix(?:\s*round)?|gate|check|shot-?checker|pack)\s+([A-Za-z0-9][A-Za-z0-9\-_]*)', desc or '', re.I)
    return m.group(1).upper() if m else ''


def build_agents(tr, subs, wfs=None):
    """Agent lanes from Agent / Task / Workflow uses + notifications + subagent metas. A use whose tool result is an error (a launch that never
    started: "Agent type not found") is not an agent. A Workflow run is one lane (its summary, its worktree branch, its agents in a['wf_agents'])."""
    agents = collections.OrderedDict()
    by_tu = {}
    wfs = wfs or {}
    for tu, u in tr['uses'].items():
        if (u['name'] in ('Agent', 'Task') or u['name'].startswith('Workflow')) and not u.get('err'):
            by_tu[tu] = u
            if u['name'].startswith('Workflow') and u.get('summary'):
                u['desc'] = u['summary']
    meta_by_tu = {}
    for aid, m in subs.items():
        if m['tool_use']:
            meta_by_tu[m['tool_use']] = aid
    # notifications give the agent id (task-id); the tool-use id ties it to the start
    for t, task, tu, st in tr['notes']:
        if tu in by_tu:
            u = by_tu[tu]
            a = agents.setdefault(task, {'id': task, 'start': u['t'], 'desc': u['desc'], 'type': u['type'], 'wf': u['name'].startswith('Workflow'),
                                         'done': [], 'send': [], 'tool_use': tu})
            a['done'].append((t, st))
        elif task in agents:
            agents[task]['done'].append((t, st))
    for tu, u in by_tu.items():   # started, no notification yet (still running) or never reported
        aid = meta_by_tu.get(tu)
        if aid and aid not in agents:
            agents[aid] = {'id': aid, 'start': u['t'], 'desc': u['desc'], 'type': u['type'], 'wf': u['name'].startswith('Workflow'), 'done': [], 'send': [], 'tool_use': tu}
    for tu, u in by_tu.items():   # a start no notification and no meta ever named: kept, with a made-up id, so it still counts
        if not any(a['tool_use'] == tu for a in agents.values()):
            aid = 'tu_' + tu[-8:]
            agents[aid] = {'id': aid, 'start': u['t'], 'desc': u['desc'], 'type': u['type'], 'wf': u['name'].startswith('Workflow'), 'done': [], 'send': [], 'tool_use': tu}
    # workflow agents: metas whose tool-use is not an Agent / Task use
    for aid, m in subs.items():
        if aid in agents:
            continue
        u = tr['uses'].get(m['tool_use'])
        if u is not None and u['name'] in ('Agent', 'Task'):
            agents[aid] = {'id': aid, 'start': u['t'], 'desc': m['desc'] or u['desc'], 'type': m['type'], 'wf': False, 'done': [], 'send': [], 'tool_use': m['tool_use']}
        elif u is not None and u['name'].startswith('Workflow'):
            agents[aid] = {'id': aid, 'start': u['t'], 'desc': m['desc'] or u['desc'], 'type': m['type'], 'wf': True, 'done': [], 'send': [], 'tool_use': m['tool_use']}
    for tu, u in tr['uses'].items():
        if u['name'] == 'SendMessage' and u['to'] in agents:
            agents[u['to']]['send'].append(u['t'])
    for aid, a in agents.items():
        a['done'].sort()
        a['send'].sort()
        m = subs.get(aid) or {}
        a['branch'] = m.get('branch') or ''
        a['model'] = m.get('model') or ''
        a['worktree'] = m.get('worktree', False)
        a['finished_meta'] = m.get('finished', False)
        a['mtime'] = m.get('mtime')
        u = tr['uses'].get(a.get('tool_use')) or {}
        run = u.get('run_id') if u.get('name', '').startswith('Workflow') else ''
        if run and run in wfs:
            w = wfs[run]
            a['wf'] = True
            a['wf_run'] = run
            a['wf_agents'] = w['agents']
            a['alt_ids'] = [run]
            a['mtime'] = w['mtime'] or a['mtime']
            a['worktree'] = bool(w['agents'])
            a['branch'] = next((x['branch'] for x in w['agents'] if x['branch']), '')
        atype = a['type'] or m.get('type') or ''
        a['type'] = atype
        a['kind'] = 'workflow' if a['wf'] else classify(a['desc'], atype, a['worktree'])
        a['tag'] = tag_of(a['desc'])
        if not a['branch']:
            a['branch'] = 'worktree-agent-' + aid if a['worktree'] else ''
    return agents


# ---------------------------------------------------------------- reader 3: git
def git_ok(args, cwd=None):
    try:
        return subprocess.run(['git'] + args, cwd=cwd or ROOT, capture_output=True, timeout=60).returncode == 0
    except Exception:
        return False


def _reflog_entries(branch, repo):
    """[(commit hash, time the branch moved there)] oldest first, from the branch's reflog (S106 B1F2: the branch's own history of what it held when;
    its first-parent line is rewritten by a fast-forward to main). Consecutive duplicates dropped; [] when there is no reflog."""
    out = git(['reflog', 'show', '--date=format-local:' + FMT_GIT, '--format=%H\t%gd', branch], cwd=repo)
    res = []
    for line in reversed(out.splitlines()):
        p = line.split('\t')
        m = re.search(r'@\{(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)\}', p[1]) if len(p) > 1 else None
        t = pt(m.group(1), FMT_GIT) if m else None
        if t is not None and (not res or res[-1][0] != p[0]):
            res.append((p[0], t))
    return res


def _first_seen(entries, t0, repo):
    """{commit: time of the first reflog entry whose state held it}: for each entry in or after the window, `rev-list entry ^previous entry`
    (a fast-forward and a reset come out right: a commit is dated when the BRANCH first took it)."""
    res = {}
    jobs = [(i, e) for i, e in enumerate(entries) if i > 0 and e[1] >= t0]
    if not jobs and entries:
        return res

    def one(j):
        i, e = j
        return e[1], git(['rev-list', e[0], '^' + entries[i - 1][0]], cwd=repo).split()
    with ThreadPoolExecutor(max_workers=6) as ex:
        for t, hs in ex.map(one, jobs):
            for h in hs:
                if h not in res or t < res[h]:
                    res[h] = t
    return res


def read_merges(branch, t0, t1, repo=None):
    """Lanes that landed on `branch` in the window: [{t, sha, src, first_commit, parents}], and merges of the session branch INTO lane branches.
    A lane has landed when its branch tip is an ancestor of `branch` (S106 B1F finding 1: fast-forwards, merges into a staging branch and
    "Merge branch 'X'" with no target were never seen by the message regex); its time = the first first-parent commit of `branch` that
    contains it, as the reflog saw it (the fast-forward's time) else the commit's own time. The old message regex stays as a fallback for lane
    branches that no longer exist."""
    into, merged_in = _read_merges_by_message(branch, t0, t1, repo)
    entries = _reflog_entries(branch, repo)
    chain = []
    for line in git(['log', '--first-parent', '--since=' + t0.strftime(FMT_GIT), '--until=' + t1.strftime(FMT_GIT), '--date=format-local:' + FMT_GIT,
                     '--format=%H\t%cd\t%P\t%s', branch], cwd=repo).splitlines():
        p = line.split('\t')
        if len(p) >= 2:
            chain.append((p[0], pt(p[1], FMT_GIT), p[2].split() if len(p) > 2 else [], p[3] if len(p) > 3 else ''))
    seen = {m['src'] for m in into}
    found = []
    if chain or entries:
        # base = the branch's state before the window: the reflog entry before t0, else the parent of the oldest chain commit
        before = [e for e in entries if e[1] < t0]
        base = before[-1][0] if before else (chain[-1][2][0] if chain and chain[-1][2] else None)
        first_seen = _first_seen(entries, t0, repo) if entries else {}
        fe = ['for-each-ref', '--merged=' + branch] + (['--no-merged=' + base] if base else []) + ['--format=%(refname:short)\t%(objectname)\t%(parent)', 'refs/heads/']
        cands = []
        for ln in git(fe, cwd=repo).splitlines():   # merged into the branch but not into its state before the window: ~90 refs, not ~1000
            p_ = ln.split('\t')
            if len(p_) >= 2 and p_[0] and p_[0] not in (branch, 'main', 'master', 'public') and p_[1]:
                cands.append((p_[0], p_[1], p_[2].split() if len(p_) > 2 else []))

        def land(c):
            br, tip, pars = c
            if tip in first_seen:   # the branch's reflog: when it first held the lane (a ff / reset both read right)
                lt = first_seen[tip]
                if not (t0 <= lt <= t1):
                    return None
                lh = next((h for h, t in entries if t >= lt and git_ok(['merge-base', '--is-ancestor', tip, h], cwd=repo)), tip)
                return {'t': lt, 'sha': lh[:9], 'src': br, 'parents': [], 'first_commit': None, 'land': lh, 'tip': tip, 'tparents': pars}
            if not chain or not git_ok(['merge-base', '--is-ancestor', tip, chain[0][0]], cwd=repo):
                return None
            # fallback (no reflog): the oldest first-parent commit that holds it, stopped at the "Merge branch '<lane>'" commit when one exists
            lo, hi = 0, len(chain) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if git_ok(['merge-base', '--is-ancestor', tip, chain[mid][0]], cwd=repo):
                    lo = mid
                else:
                    hi = mid - 1
            lh, lt, lp, _s = chain[lo]
            for h_, t_, p_, s_ in chain[:lo + 1]:
                if s_.startswith("Merge branch '%s'" % br):
                    lh, lt, lp = h_, t_, p_
                    break
            return {'t': lt, 'sha': lh[:9], 'src': br, 'parents': lp, 'first_commit': None, 'land': lh, 'tip': tip, 'tparents': pars}
        with ThreadPoolExecutor(max_workers=6) as ex:   # independent read-only git calls; 6 at a time, no more
            found = [f for f in ex.map(land, cands) if f]
        # a staging branch (its tip is the merge of another lane that landed with it) is not a lane of its own
        tips = {f['tip']: f for f in found}
        drop = set()
        for f in found:
            ps = f['tparents']
            if len(ps) >= 2 and any(p in tips and tips[p] is not f and tips[p]['land'] == f['land'] for p in ps):
                drop.add(f['src'])
        for f in found:
            if f['src'] not in drop and f['src'] not in seen:
                into.append(f)
    into.sort(key=lambda m: m['t'])
    return into, merged_in


def session_chain(branch, since, repo=None):
    """[(hash, time)] newest first: the states the branch held, from its reflog (S106 B1F2: the state AT a lane's start, not main's history after a
    fast-forward). Falls back to the first-parent commits since `since` when there is no reflog."""
    ent = _reflog_entries(branch, repo)
    if ent:
        return list(reversed(ent))
    out = []
    for line in git(['log', '--first-parent', '--since=' + since.strftime(FMT_GIT), '--date=format-local:' + FMT_GIT, '--format=%H\t%cd', branch], cwd=repo).splitlines():
        h, _, t = line.partition('\t')
        if h and pt(t, FMT_GIT):
            out.append((h, pt(t, FMT_GIT)))
    return out


def lane_first_commit(br, branch, start, repo=None, chain=None):
    """Time of the lane's first commit made at or after its start (minus 5 min): commits reachable from the lane (first-parent) and not from the
    session branch as it stood at the lane's start. Older commits (a worktree cut from public history) are ignored; None when there is none."""
    if chain is not None:
        x = next((h for h, t in chain if t <= start), '')
    else:
        x = git(['rev-list', '-1', '--first-parent', '--before=' + start.strftime(FMT_GIT), branch], cwd=repo).strip()
    args = ['log', '--first-parent', '--reverse', '--date=format-local:' + FMT_GIT, '--format=%ad', br] + (['^' + x] if x else [])
    lo = start - timedelta(minutes=5)
    for l in git(args, cwd=repo).splitlines():
        t = pt(l, FMT_GIT)
        if t is not None and t >= lo:
            return t
    return None


def _read_merges_by_message(branch, t0, t1, repo=None):
    out = git(['log', '--merges', '--since=' + t0.strftime(FMT_GIT), '--until=' + t1.strftime(FMT_GIT), '--date=format-local:' + FMT_GIT,
               '--pretty=%ad\t%h\t%P\t%s', branch], cwd=repo)
    into, merged_in = [], []
    for line in out.splitlines():
        p = line.split('\t', 3)
        if len(p) < 4:
            continue
        t = pt(p[0], FMT_GIT)
        m = re.match(r"Merge branch '([^']+)' into (\S+)\s*(\(.*\))?\s*$", p[3])
        if m and m.group(2) == branch:
            into.append({'t': t, 'sha': p[1], 'src': m.group(1), 'parents': p[2].split()})
        elif m and m.group(1) == branch:
            merged_in.append({'t': t, 'sha': p[1], 'dst': m.group(2)})
    for mg in into:
        if len(mg['parents']) >= 2:
            fl = git(['log', '--reverse', '--date=format-local:' + FMT_GIT, '--format=%ad', mg['parents'][0] + '..' + mg['parents'][1]], cwd=repo).splitlines()
            mg['first_commit'] = pt(fl[0], FMT_GIT) if fl else None
        else:
            mg['first_commit'] = None
    return into, merged_in


def local_branches(repo=None):
    return set(git(['for-each-ref', '--format=%(refname:short)', 'refs/heads/'], cwd=repo).split())


# ---------------------------------------------------------------- reader 5: run lock
def lock_cat(name):
    n = (name or '').lower()
    if 'build_apk' in n or 'build_all' in n:
        return 'apk build'
    if 'build_sdk' in n or 'fast_build' in n:
        return 'sdk build'
    if n.startswith('build') or 'build.ps1' in n:
        return 'exe build'
    if 'tidy' in n:
        return 'tidy'
    if 'codegen' in n:
        return 'codegen'
    if 'release' in n:
        return 'release'
    if 'run_list' in n or 'run_and_triage' in n or 'sweep' in n or 'lanes' in n or 'run_stage' in n:
        return 'game run'
    if n.split(' ')[0] == 'q' or 'index' in n:
        return 'index'
    return 'other'


def read_runlock(data_root, t0, t1):
    path = os.path.join(data_root, 'out', 'harness', 'runlock', 'history.tsv')
    res = {'held': collections.defaultdict(list), 'waited': collections.defaultdict(list), 'open': {}, 'rows': 0}
    if not os.path.exists(path):
        return res
    opened = {}
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        r = csv.reader(f, delimiter='\t')
        next(r, None)
        for x in r:
            if len(x) < 8:
                continue
            t = pt(x[0])
            if t is None:
                continue
            tok = x[10] if len(x) > 10 else ''
            if x[1] == 'acquire':
                opened[tok] = (t, x[4], x[2])
            elif x[1] == 'release':
                opened.pop(tok, None)
            if not (t0 <= t <= t1):
                continue
            res['rows'] += 1
            c = lock_cat(x[4])
            try:
                if x[1] == 'release' and x[7]:
                    res['held'][c].append((float(x[7]), t, x[2]))
                if x[1] == 'acquire' and x[6]:
                    res['waited'][c].append((float(x[6]), t, x[2]))
            except ValueError:
                pass
    res['open'] = {k: v for k, v in opened.items() if k}
    return res


# ---------------------------------------------------------------- reader 6: game logs (THE run count)
LOG_RE = re.compile(r'^wwe2k14_(\d+)\.log$')


def _find_log_dirs(out_dir):
    """Every folder named `logs` under out\\ (build\\*, release\\build, wt\\<name>\\...: S106 B1F finding 11); never descends into one, nor into a .git."""
    res = []
    for root, dirs, files in os.walk(out_dir):
        if os.path.basename(root) == 'logs':
            res.append(root)
            dirs[:] = []
            continue
        dirs[:] = [d for d in dirs if d not in ('.git', 'node_modules', 'sdk-install-arm64', 'glslang-build')]
    return res


def _log_user_root(path):
    """The log's `user_data_root:` line (within its first 120 lines), or ''."""
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for i, l in enumerate(f):
                if i > 120:
                    break
                m = re.search(r'user_data_root:\s*(\S.*)$', l)
                if m:
                    return m.group(1).strip()
    except OSError:
        pass
    return ''


def count_logs(data_root, t0, t1, dirs=None):
    """Game logs written in [t0, t1] by write time, in every `logs` folder under out\\ (or `dirs`). Each folder is split by the log's
    `user_data_root:` line: a root under out\\bench\\userdata_* is a TEST run (key '<folder> [test]'), anything else is Jarrod's own play or
    unknown ('<folder> [other]')."""
    if dirs is None:
        dirs = _find_log_dirs(os.path.join(data_root, 'out'))
    per = collections.OrderedDict()
    for d in sorted(dirs):
        try:
            names = os.listdir(d)
        except OSError:
            continue
        kinds = collections.defaultdict(list)
        for fn in names:
            m = LOG_RE.match(fn)
            if not m:
                continue
            fp = os.path.join(d, fn)
            mt = datetime.fromtimestamp(os.path.getmtime(fp))
            if t0 <= mt <= t1:
                ur = _log_user_root(fp).replace('/', '\\').lower()
                kinds['test' if re.search(r'\\bench\\userdata_', ur) else 'other'].append((int(m.group(1)), mt))
        for kd, nums in kinds.items():
            key = os.path.relpath(os.path.dirname(d), os.path.join(data_root, 'out')) if data_root else d
            per['%s [%s]' % (key, kd)] = sorted(nums)
    return per


def split_runs(per):
    """(test runs, other) from count_logs' result."""
    t = sum(len(v) for k, v in per.items() if k.endswith('[test]'))
    return t, sum(len(v) for v in per.values()) - t


def describe_logs(per):
    total = sum(len(v) for v in per.values())
    parts = []
    for k, v in per.items():
        parts.append('%s %d (%03d-%03d)' % (k, len(v), v[0][0], v[-1][0]))
    return total, parts


# ---------------------------------------------------------------- reader 7: compile queue
def read_admit(t0, t1):
    base = os.environ.get('LOCALAPPDATA') or ''
    path = os.path.join(base, 'wwe2k14-admit', 'history.tsv')
    res = {'n': 0, 'outcomes': collections.Counter(), 'by_kind': collections.defaultdict(collections.Counter), 'avail_med_timeout': None}
    if not os.path.exists(path):
        return res
    av = []
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            t = pt(r.get('time') or '')
            if t is None or not (t0 <= t <= t1):
                continue
            res['n'] += 1
            res['outcomes'][r.get('outcome')] += 1
            res['by_kind'][r.get('kind')][r.get('outcome')] += 1
            if r.get('outcome') == 'timeout_fail':
                try:
                    av.append(float(r.get('avail_phys_gb')))
                except (TypeError, ValueError):
                    pass
    res['avail_med_timeout'] = statistics.median(av) if av else None
    return res


# ---------------------------------------------------------------- plan rows
def load_rows(spec):
    p = spec if os.path.isfile(spec) else os.path.join(ROWS_DIR, spec + '.json')
    if not os.path.isfile(p):
        raise SystemExit('board: plan rows file not found: ' + spec)
    d = json.load(open(p, encoding='utf-8'))
    for r in d['rows']:
        r['_rx'] = [re.compile(x, re.I) for x in r.get('match', [])]
        # kind_filter (S106 B1F finding 9): a probe row (settle: done) takes probe-kind agents only, a build row builder class only
        r['_kinds'] = (PROBE_KINDS if r.get('settle') == 'done' else BUILD_KINDS) if d.get('kind_filter') else None
    return d


def assign_rows(rows, agents, merges):
    """First matching row wins. Fills r['agents'], r['merges'] (merge events matched by agent branch or branch-name regex)."""
    for r in rows:
        r['agents'] = []
        r['merges'] = []
        r['others'] = []   # matched the row's words but not its kinds (a probe of a build row, a review): they never start or run the row
    for a in agents.values():
        for r in rows:
            if any(rx.search(a['desc'] or '') for rx in r['_rx']):
                ks = r.get('_kinds')
                if ks is not None and not (a['kind'] in ks or (r.get('settle') != 'done' and is_builder_class(a))):
                    r['others'].append(a)
                    continue
                r['agents'].append(a)
                a['row'] = r['id']
                break
    for mg in merges:
        placed = False
        for r in rows:
            ids = [a['id'] for a in r['agents']]
            if any(mg['src'] == 'worktree-agent-' + i or mg['src'] == (a_branch(r, i)) for i in ids) or any(rx.search(mg['src']) for rx in r['_rx']):
                r['merges'].append(mg)
                placed = True
                break
        mg['row'] = next((r['id'] for r in rows if mg in r['merges']), None) if placed else None


def a_branch(r, aid):
    for a in r['agents']:
        if a['id'] == aid:
            return a.get('branch')
    return None


def agent_running(a, now):
    ev = [(a['start'], 'start')] + [(t, 'send') for t in a['send']]
    last_go = max(t for t, _ in ev)
    last_done = max([t for t, _ in a['done']] + [datetime.min])
    return last_go > last_done


def compute_rows(rows, agents, merges, t_start, now, go=()):
    """Per row: started, done (time), free_from, state. A row is FREE when every need is done and no agent matches it."""
    by_id = {r['id']: r for r in rows}
    for r in rows:
        r['started'] = min([a['start'] for a in r['agents']] or [None]) if r['agents'] else None
        r['running'] = [a for a in r['agents'] if agent_running(a, now)]
        mt = [m['t'] for m in r['merges']]
        r.setdefault('others', [])
        if r.get('settle') == 'done':
            dn = [d for a in r['agents'] for d, st in a['done'] if st == 'completed']
            r['done'] = max(dn) if (dn and not r['running'] and len(dn) >= 1) else None
        else:
            r['done'] = min(mt) if (mt and not r['running']) else None   # the FIRST landing (a later fix-round merge does not move it)

    def need_time(n, depth=0):
        """time a need was satisfied, or None; 'go:X' is satisfied by --go X (at t_start)."""
        if n.startswith('go:'):
            return t_start if n[3:] in go else None
        ts = []
        for alt in n.split('|'):
            if alt.startswith('go:'):
                tt = t_start if alt[3:] in go else None
            else:
                tt = by_id[alt]['done'] if alt in by_id else None
            if tt is not None:
                ts.append(tt)
        return min(ts) if ts else None

    for r in rows:
        times = [need_time(n) for n in r.get('needs', [])]
        if any(t is None for t in times):
            r['free_from'] = None
            r['blocked_on'] = [n for n, t in zip(r.get('needs', []), times) if t is None]
        else:
            r['free_from'] = max(times + [t_start])
            r['blocked_on'] = []
        if r['done'] is not None:
            r['state'] = 'done'
        elif r['running']:
            r['state'] = 'running'
        elif r['agents'] or r['merges']:
            r['state'] = 'started'
        elif r['free_from'] is not None and [o for o in r['others'] if o['kind'] not in ('review',)]:
            r['state'] = 'probed'   # a probe / reader ran, no builder has started: still free to start, the board says how long it has waited
            r['probed_at'] = min(o['start'] for o in r['others'] if o['kind'] not in ('review',))
        elif r['free_from'] is not None:
            r['state'] = 'FREE'
        else:
            r['state'] = 'blocked'
    return rows


def free_rows(rows):
    """Rows free to start and not running (state FREE): the board's 'start these' list."""
    return [r for r in rows if r['state'] in ('FREE', 'probed')]


# ---------------------------------------------------------------- lanes: state and waits
def lane_state(a, now, merge, review_of, check_times, stale_min=45):
    """(state, waiting_on, wait_start) for one lane. merge = its merge event or None;
    review_of = {'started','done'} of a review agent with its tag (or None); check_times = list of times its check ran."""
    dones = a['done']
    last_done = dones[-1] if dones else None
    running = agent_running(a, now)
    if running:
        stale = a['mtime'] is not None and mins(a['mtime'], now) > stale_min
        if stale:
            return ('running? silent %s' % dur(mins(a['mtime'], now)), 'a tool call (no transcript write for %s)' % dur(mins(a['mtime'], now)), a['mtime'])
        t = max([a['start']] + a['send'])
        return ('running', '-', t)   # a background agent's Agent call returned at once: running is not "waiting on a tool call"
    if last_done is None:
        return ('no hand-back recorded', 'a notification', a['start'])
    t_done, status = last_done
    if status != 'completed':
        return ('handed back %s' % status.upper(), 'the main session (failed hand-back)', t_done)
    if merge is not None:
        if check_times is None:
            return ('merged', 'nothing (no check log)', merge['t'])
        later = [c for c in check_times if c >= merge['t']]
        if later:
            return ('merged, checked', '-', merge['t'])
        return ('merged, not checked', 'a check run', merge['t'])
    if is_builder_class(a):
        if review_of is None:
            return ('handed back, not reviewed', 'a review to start', t_done)
        if review_of.get('done') is not None and review_of['done'] >= t_done and not any(s > review_of['done'] for s in a['send']):
            return ('review done, fix not started', 'a fix round or a go', review_of['done'])
        if review_of.get('done') is None:
            return ('under review', 'its review', review_of['started'])
        return ('handed back, no merge', 'the main session', t_done)
    return ('done', '-', t_done)


# ---------------------------------------------------------------- the whole board
class Board:
    pass


def load_check_log(path):
    """Check-run log: lines starting with 'YYYY-MM-DD HH:MM'; returns [(time, line_lower)] or None when the file does not exist."""
    if not path or not os.path.exists(path):
        return None
    out = []
    for l in open(path, encoding='utf-8', errors='replace'):
        t = pt(l[:19]) or pt(l[:16], '%Y-%m-%d %H:%M')
        if t:
            out.append((t, l.rstrip('\r\n')))
    return out


_ROWFIELD = re.compile(r'(?:^|\s)row=([^\s,;]+)', re.I)


def check_done(line):
    """A runner line written BEFORE a launch (`run=pending verdict=pending`) is not a check result: only a line whose verdict= is not 'pending' counts
    as CHECKED (S106 C2 probe). A line without a verdict= field counts (older free-form logs)."""
    m = re.search(r'(?:^|\s)verdict=([^\s]*)', line, re.I)
    return not (m and m.group(1).lower() in ('pending', ''))


def check_hits(line, keys, done_only=False):
    """Does a check-log line belong to one of `keys` (row ids / tags)? The runner writes `... row=<ID> arm=... question=<text>`: the row= field
    is compared exactly (case-insensitively) and the rest of the line, the question text included, is never searched. A line with no row= field
    falls back to a whole-token match (a substring never counts: 'T' is not in "don't", 'C1' is not in 'C10')."""
    keys = [k for k in keys if k]
    if not keys or (done_only and not check_done(line)):
        return False
    m = _ROWFIELD.search(line)
    if m:
        return m.group(1).lower() in {k.lower() for k in keys}
    return any(re.search(r'(?<![A-Za-z0-9])' + re.escape(k) + r'(?![A-Za-z0-9])', line, re.I) for k in keys)


def review_of_lane(a, reviews, row_ids, now):
    """The review of lane `a` as {'started','done'} or None. A review belongs to a lane when the lane's row id / tag appears as a token in the
    review's description (the token after the model prefix: 'SON B1 the board' has no 'builder B1' tag, S106 B1F finding 7), and it started at
    or after the lane started (a review started on the first commit is before the hand-back and still counts). A running one wins, else the latest."""
    keys = {k.lower() for k in (a.get('row'), a.get('tag')) if k}
    if not keys:
        toks = {t.lower() for t in re.findall(r'[A-Za-z0-9][A-Za-z0-9\-_]*', a.get('desc') or '')} & {r.lower() for r in row_ids}
        keys = toks
    if not keys:
        return None
    cands = []
    for r in reviews:
        toks = {t.lower() for t in re.findall(r'[A-Za-z0-9][A-Za-z0-9\-_]*', r.get('desc') or '')}
        if toks & keys and r['start'] >= a['start']:
            cands.append(r)
    if not cands:
        return None
    run = [r for r in cands if agent_running(r, now)]
    r0 = run[0] if run else sorted(cands, key=lambda r: r['start'])[-1]
    dn = [t for t, st in r0['done'] if st == 'completed']
    return {'started': r0['start'], 'done': (max(dn) if dn and not agent_running(r0, now) else None)}


def collect(args):
    b = Board()
    b.data_root = os.path.normpath(args.repo) if args.repo else main_checkout()
    rows_spec = args.rows
    spec = load_rows(rows_spec) if rows_spec else None
    b.spec = spec
    pdir = projects_dir()
    b.session = args.session or (spec or {}).get('session') or pick_session(pdir, (spec or {}).get('branch') or args.branch)
    if not b.session:
        raise SystemExit('board: no transcript found under ' + pdir)
    tpath = os.path.join(pdir, b.session + '.jsonl')
    if not os.path.exists(tpath):
        raise SystemExit('board: transcript not found: ' + tpath)
    b.tr = read_transcript(tpath)
    first = utc_to_local(b.tr['first']) if b.tr['first'] else datetime.now()
    last = utc_to_local(b.tr['last']) if b.tr['last'] else datetime.now()
    realnow = datetime.now()
    w = (spec or {}).get('window')
    subs = read_subagents(pdir, b.session)
    b.subs = subs
    b.live, b.now = clock_state(last, realnow, [m['mtime'] for m in subs.values()], w)
    b.t0 = pt(w[0], '%Y-%m-%d %H:%M') if w else first
    b.t1 = pt(w[1], '%Y-%m-%d %H:%M') if w else b.now
    b.start = first
    b.clock = Clock(first)
    b.branch = args.branch or (spec or {}).get('branch') or git(['rev-parse', '--abbrev-ref', 'HEAD']).strip()
    b.wfs = read_workflows(pdir, b.session, b.tr)
    b.agents = build_agents(b.tr, subs, b.wfs)
    b.merges, b.merged_in = read_merges(b.branch, b.t0 - timedelta(hours=1), b.t1 + timedelta(minutes=5))
    b.lock = read_runlock(b.data_root, b.t0, b.t1)
    b.logs = count_logs(b.data_root, b.t0, b.t1)
    b.admit = read_admit(b.t0, b.t1)
    cl = (spec or {}).get('check_log')
    b.check_log = load_check_log(os.path.join(b.data_root, cl)) if cl else None
    b.go = args.go or []
    if spec:
        assign_rows(spec['rows'], b.agents, b.merges)
        compute_rows(spec['rows'], b.agents, b.merges, b.start, b.now, b.go)
    # lanes
    merge_by_branch = {}
    for mg in b.merges:
        merge_by_branch.setdefault(mg['src'], []).append(mg)
    reviews = [a for a in b.agents.values() if a['kind'] == 'review']
    row_ids = [r['id'] for r in (spec or {'rows': []})['rows']]
    brs = local_branches()
    chain = session_chain(b.branch, b.t0 - timedelta(days=2))
    for a in b.agents.values():
        ms = [m for br in {a['branch']} if br for m in merge_by_branch.get(br, [])]
        if a['tag'] and a['kind'] in ('builder', 'fix', 'other'):   # named branches (s105-a4b-fix): tag tokens are a run of the branch's tokens
            tt = a['tag'].lower().split('-')
            for mg in b.merges:
                bt = mg['src'].lower().split('-')
                if not mg['src'].startswith('worktree-agent-') and any(bt[i:i + len(tt)] == tt for i in range(len(bt))) and mg not in ms:
                    ms.append(mg)
        a['merges'] = sorted(ms, key=lambda m: m['t'])
        ids = [a['id']] + a.get('alt_ids', [])
        a['first_commit'] = None
        if a['branch'] and a['branch'] in brs and (a['worktree'] or a['merges']):
            a['first_commit'] = lane_first_commit(a['branch'], b.branch, a['start'], chain=chain)
        elif a['merges'] and a['merges'][0].get('first_commit') and a['merges'][0]['first_commit'] >= a['start'] - timedelta(minutes=5):
            a['first_commit'] = a['merges'][0]['first_commit']
        a['merged_in'] = [m['t'] for m in b.merged_in if a['branch'] and m['dst'] == a['branch']]
        rv = review_of_lane(a, reviews, row_ids, b.now) if (a['kind'] != 'review' and is_builder_class(a)) else None
        cts = None
        if b.check_log is not None:
            keys = [k for k in (a['tag'], a.get('row'), a['id'][:8]) if k]
            cts = [t for t, line in b.check_log if check_hits(line, keys, done_only=True)]
        a['state'], a['waiting_on'], a['wait_start'] = lane_state(a, b.now, a['merges'][-1] if a['merges'] else None, rv, cts)
        a['wait_min'] = mins(a['wait_start'], b.now) if a['wait_start'] else None
    return b


# ---------------------------------------------------------------- output: the board
def alive_counts(b):
    lanes = [a for a in b.agents.values() if agent_running(a, b.now) and not (a['mtime'] and mins(a['mtime'], b.now) > 45)]
    games = [v for v in b.lock['open'].values() if lock_cat(v[1]) == 'game run' and mins(v[0], b.now) < 120]
    return lanes, games


def run_summary(b):
    total, parts = describe_logs(b.logs)
    return total, parts


def print_board(b, show_all=False, out=sys.stdout):
    c = b.clock
    P = lambda s='': print(s, file=out)
    el = mins(b.start, b.now)
    P('BOARD  session %s  branch %s  %s' % (b.session[:8], b.branch, 'LIVE' if b.live else 'finished'))
    P('clock %s  started %s  elapsed %s  window %s -> %s' % (b.now.strftime('%Y-%m-%d %H:%M'), c.hm(b.start), dur(el), b.t0.strftime('%m-%d %H:%M'), b.t1.strftime('%m-%d %H:%M')))
    lanes, games = alive_counts(b)
    total, parts = run_summary(b)
    est = (b.spec or {}).get('run_estimate')
    tst, oth = split_runs(b.logs)
    P('GAME RUNS (game logs written in the window, by write time): %d%s (%d test runs on out\\bench\\userdata_*, %d other: Jarrod\'s own play or unknown) | %s' %
      (total, ' of ~%s estimated' % est if est else '', tst, oth, '; '.join(parts) or 'none'))
    P('ALIVE now: %d lane(s), %d game(s) holding the run lock | started %d agents, %d merges into %s' %
      (len(lanes), len(games), len(b.agents), len(b.merges), b.branch))
    P()
    P('LANES (%s)' % ('all' if show_all else 'not finished: alive, waiting or handed back; --all for every lane'))
    shown = 0
    for a in sorted(b.agents.values(), key=lambda x: x['start']):
        terminal = a['state'] in ('merged', 'merged, checked', 'done')
        if terminal and not show_all:
            continue
        shown += 1
        P('%s %-46s [%s] %s' % (a['id'][:8], (a['desc'] or '')[:46], a['kind'], a['state']))
        links = ['start %s' % c.hm(a['start'])]
        links += ['done %s%s' % (c.hm(t), '' if st == 'completed' else '(%s)' % st) for t, st in a['done']]
        if a['send']:
            links.append('follow-ups ' + ' '.join(c.hm(t) for t in a['send']))
        if a['first_commit']:
            links.append('first commit %s' % c.hm(a['first_commit']))
        if a['merged_in']:
            links.append('%s merged in %s' % (b.branch, ' '.join(c.hm(t) for t in a['merged_in'])))
        if a['merges']:
            links.append('MERGED %s' % ' '.join(c.hm(m['t']) for m in a['merges']))
        P('    ' + ' | '.join(links))
        if a['waiting_on'] not in ('-',) and a['wait_min'] is not None and a['state'] not in ('merged, checked',):
            P('    waits on: %s for %s' % (a['waiting_on'], dur(a['wait_min'])))
    if not shown:
        P('  (none)')
    if b.spec:
        P()
        P('PLAN ROWS (%s)' % b.spec.get('name', ''))
        free = free_rows(b.spec['rows'])
        for r in b.spec['rows']:
            if r['state'] == 'done' and not show_all:
                continue
            extra = ''
            if r['state'] == 'FREE':
                extra = 'free since %s, not started (%s)' % (c.hm(r['free_from']), dur(mins(r['free_from'], b.now)))
            elif r['state'] == 'probed':
                extra = 'probed %s, build not started (free since %s: %s)' % (c.hm(r['probed_at']), c.hm(r['free_from']), dur(mins(r['free_from'], b.now)))
            elif r['state'] == 'blocked':
                extra = 'blocked on ' + ', '.join(r['blocked_on'])
            elif r['state'] == 'running':
                extra = 'started %s, %d agent(s) running' % (c.hm(r['started']), len(r['running']))
            elif r['state'] == 'started':
                extra = 'started %s, no agent running, not merged' % c.hm(r['started'])
            elif r['state'] == 'done':
                extra = 'done %s' % c.hm(r['done'])
            P('  %-5s %-8s %s' % (r['id'], r['state'], extra))
        P('FREE TO START now: %s' % (', '.join(r['id'] for r in free) or 'none'))
    P()
    P('RUN LOCK (window): ' + '; '.join('%s %d holds %.0f min' % (k, len(v), sum(x[0] for x in v) / 60.0) for k, v in sorted(b.lock['held'].items(), key=lambda kv: -sum(x[0] for x in kv[1]))[:5]))
    P('COMPILE QUEUE (window): %d requests, %s' % (b.admit['n'], dict(b.admit['outcomes'])))
    return 0


# ---------------------------------------------------------------- fixed block
MARK_A, MARK_B = '<!-- board:fixed -->', '<!-- /board:fixed -->'


def fixed_block(b):
    c = b.clock
    total, parts = run_summary(b)
    est = (b.spec or {}).get('run_estimate')
    lanes, games = alive_counts(b)
    merged = [a for a in b.agents.values() if a['merges']]
    handed = [a for a in b.agents.values() if not a['merges'] and a['state'] in ('handed back, not reviewed', 'review done, fix not started', 'under review', 'handed back, no merge')]
    lines = ['**State** (board, %s): session %s on `%s`, started %s, %s elapsed; %d agents started, %d alive now.' %
             (b.now.strftime('%Y-%m-%d %H:%M'), b.session[:8], b.branch, c.hm(b.start), dur(mins(b.start, b.now)), len(b.agents), len(lanes)),
             '**Merge state:** %d lane branch merges into `%s` in the window; %d lanes handed back and not merged.' % (len(b.merges), b.branch, len(handed)),
             '**Runs:** %d game logs written in the window%s (%s).' % (total, ' against ~%s estimated' % est if est else '', '; '.join(parts) or 'none')]
    if b.spec:
        notdone = [r['id'] for r in b.spec['rows'] if r['state'] != 'done']
        lines.append('**Not done:** ' + (', '.join(notdone) or 'none') + '.')
        lines.append('**Free to start:** ' + (', '.join(r['id'] for r in free_rows(b.spec['rows'])) or 'none') + '.')
    return '\n'.join(lines)


MAX_BLOCK_LINES = 30


def _is_marker(line, mark):
    return line.lstrip('﻿').strip() == mark


def write_fixed(text, block):
    """Replace the text between the markers (adds them at the top when there are none). Everything outside the markers is byte-identical.
    REFUSES (ValueError, nothing written) unless the file holds no marker at all, or exactly one start marker and exactly one end marker, the
    start first, neither inside a ``` fence, and fewer than MAX_BLOCK_LINES lines between them (S106 B1F finding 6: a pasted `--fixed` output
    plus a missing end marker once deleted the whole running log). A BOM before the first line is kept and never hides a marker. The result is
    re-read: its start up to the start marker and its end from the end marker must equal the original's, byte for byte."""
    lines = text.splitlines(keepends=True)
    nl = '\r\n' if text.count('\r\n') * 2 >= max(1, text.count('\n')) else '\n'
    starts = [i for i, l in enumerate(lines) if _is_marker(l, MARK_A)]
    ends = [i for i, l in enumerate(lines) if _is_marker(l, MARK_B)]
    body = ''.join(l + nl for l in block.split('\n'))
    if not starts and not ends:
        bom = '﻿' if text.startswith('﻿') else ''
        rest = text[len(bom):]
        new = bom + MARK_A + nl + body + MARK_B + nl + nl + rest
        if not new.endswith(rest) or not new.startswith(bom + MARK_A):
            raise ValueError('write_fixed: verification of the added block failed')
        return new
    if len(starts) != 1 or len(ends) != 1:
        raise ValueError('write_fixed: need exactly one start and one end marker, found %d start / %d end' % (len(starts), len(ends)))
    ia, ib = starts[0], ends[0]
    if ib <= ia:
        raise ValueError('write_fixed: the end marker comes before the start marker')
    if ib - ia - 1 >= MAX_BLOCK_LINES:
        raise ValueError('write_fixed: %d lines between the markers is not the fixed block (limit %d)' % (ib - ia - 1, MAX_BLOCK_LINES))
    fence = False
    for i, l in enumerate(lines):
        if l.lstrip('﻿').lstrip().startswith('```'):
            fence = not fence
        if i in (ia, ib) and fence:
            raise ValueError('write_fixed: a marker inside a code fence')
    prefix = ''.join(lines[:ia + 1])
    suffix = ''.join(lines[ib:])
    new = prefix + body + suffix
    if not (new.startswith(prefix) and new.endswith(suffix) and len(new) == len(prefix) + len(body) + len(suffix)) or text[:len(prefix)] != new[:len(prefix)] or \
            text[len(text) - len(suffix):] != new[len(new) - len(suffix):]:
        raise ValueError('write_fixed: verification failed (bytes outside the markers changed)')
    return new


def write_file_fixed(path, block, roots=None):
    """Write the fixed block into FILE: a relative path is the main checkout's; a path outside `roots` (default: the main checkout and this checkout)
    is refused. Temp file + os.replace; the written file is re-read and its bytes outside the markers compared with the original's."""
    roots = roots or [main_checkout(), ROOT]
    if not os.path.isabs(path):
        path = os.path.join(main_checkout(), path)
    rp = os.path.realpath(path)
    if not any(os.path.commonpath([rp, os.path.realpath(r)]) == os.path.realpath(r) for r in roots if os.path.splitdrive(r)[0].lower() == os.path.splitdrive(rp)[0].lower()):
        raise SystemExit('board: refusing to write %s: outside %s' % (rp, ', '.join(roots)))
    with open(rp, 'rb') as f:
        orig = f.read()
    text = orig.decode('utf-8')
    new = write_fixed(text, block)
    nb = new.encode('utf-8')
    tmp = rp + '.board-tmp'
    with open(tmp, 'wb') as f:
        f.write(nb)
    os.replace(tmp, rp)
    with open(rp, 'rb') as f:
        got = f.read()
    pa = got.find(MARK_A.encode())
    ka = orig.find(MARK_A.encode())
    kb = orig.rfind(MARK_B.encode())
    gb = got.rfind(MARK_B.encode())
    if ka < 0:   # markers were added at the top: the original follows the added block untouched
        ok = got.endswith(orig.lstrip(b'\xef\xbb\xbf')) or got.endswith(orig)
    else:
        ok = got[:pa + len(MARK_A)] == orig[:ka + len(MARK_A)] and got[gb:] == orig[kb:]
    if not ok:
        with open(rp, 'wb') as f:   # put the original back
            f.write(orig)
        raise SystemExit('board: verification of the written file failed; the original was restored')
    return rp


def fixed_text_for_cli(block):
    """`--fixed` prints the block only, no markers (pasting markers into a summary is how a second pair got there)."""
    return block


def do_write_fixed(path, b):
    rp = write_file_fixed(path, fixed_block(b))
    print('board: fixed block written to ' + rp)
    print('board: a script write does not trigger the WebDAV upload hook: run  py tools\\harness\\upload_playbook_hook.py --end')
    return 0


# ---------------------------------------------------------------- bar 3 (waits)
WAIT_CAT = [('handback', re.compile(r'handback[\\/]+check\.py')),
            ('syntax', re.compile(r'game_syntax')),
            ('build', re.compile(r'fast_build|build(_sdk|_probes|_android|_launcher)?\.ps1|\bninja\b|\bcmake\b|\bclang(\+\+|-cl)?(\.exe)?\s|cl\.exe|msbuild|build_\w+\.ps1|build\w*\.bat')),
            ('wait', re.compile(r'\bsleep\s+\d|Start-Sleep|Wait-Process|Wait-Job|tail\s+(-\w+\s+)*-f\b|--pid|until\s|while\s.*(sleep|Start-Sleep|kill -0)|Get-Content\s.*-Wait|time\.sleep', re.I))]


def union_seconds(iv):
    """Total length of the union of (start, end) intervals (numbers)."""
    tot, cs, ce = 0.0, None, None
    for s_, e_ in sorted(iv):
        if cs is None:
            cs, ce = s_, e_
        elif s_ <= ce:
            ce = max(ce, e_)
        else:
            tot += ce - cs
            cs, ce = s_, e_
    if cs is not None:
        tot += ce - cs
    return tot


def agent_waits(jsonl):
    """Bar 3 for one agent transcript (the definition of docs\\research\\s105_audit\\readers\\waits.py): the merged union of its compile / check /
    wait tool calls (past audits' rows (s105 / s106) no longer reproduce BAR 3 exactly: the wait class of the old check scheduler is gone) (foreground Bash / PowerShell by command class, Monitor) over its wall time. Returns (wall_s, union_s) or None."""
    calls, res, t0, t1 = {}, {}, None, None
    try:
        f = open(jsonl, encoding='utf-8', errors='replace')
    except OSError:
        return None
    with f:
        for l in f:
            i = l.find(_TS)
            if i < 0:
                continue
            try:
                ts = utc_to_local(l[i + len(_TS):i + len(_TS) + 19]).timestamp()
            except ValueError:
                continue
            t0 = ts if t0 is None else min(t0, ts)
            t1 = ts if t1 is None else max(t1, ts)
            if '"tool_use"' in l and '"type":"assistant"' in l:
                try:
                    d = json.loads(l)
                except ValueError:
                    continue
                for b in (d.get('message') or {}).get('content') or []:
                    if isinstance(b, dict) and b.get('type') == 'tool_use' and b.get('name') in ('Bash', 'PowerShell', 'Monitor'):
                        inp = b.get('input') or {}
                        if inp.get('run_in_background'):
                            continue
                        k = 'wait' if b['name'] == 'Monitor' else next((k for k, rx in WAIT_CAT if rx.search(str(inp.get('command', '')))), None)
                        if k:
                            calls[b['id']] = (ts, k)
            elif '"tool_result"' in l:
                for tid in _TUID.findall(l):
                    if tid in calls:
                        res.setdefault(tid, ts)
    if t0 is None or t1 is None or t1 <= t0:
        return None
    iv = [(calls[t][0], res[t]) for t in res if res[t] >= calls[t][0]]
    return t1 - t0, union_seconds(iv)


# ---------------------------------------------------------------- audit tables
def print_audit(b, out=sys.stdout, split=None):
    c = b.clock
    P = lambda s='': print(s, file=out)
    P('A1. THE SESSION IN NUMBERS  (window %s -> %s)' % (b.t0.strftime('%m-%d %H:%M'), b.t1.strftime('%m-%d %H:%M')))
    P('  wall time: %s to %s = %.2f h' % (utc_to_local(b.tr['first']).strftime('%H:%M:%S'), utc_to_local(b.tr['last']).strftime('%H:%M:%S'),
                                         mins(utc_to_local(b.tr['first']), utc_to_local(b.tr['last'])) / 60.0))
    P('  main session: %d model replies (unique message ids); turns, idle gaps and context sizes are not ported (see mainx.py in the S105 audit folder)' % b.tr['assistant_msgs'])
    mc = collections.Counter((a['model'] or '?') for a in b.agents.values())
    P('  agents: %d started (by the subagent meta model key; ? = no key, inherits the session model): %s' % (len(b.agents), ', '.join('%d %s' % (v, k) for k, v in mc.most_common())))
    P('  compile queue: %d requests; %d timed out (%.0f %%)%s' % (b.admit['n'], b.admit['outcomes'].get('timeout_fail', 0),
                                                                   100.0 * b.admit['outcomes'].get('timeout_fail', 0) / max(1, b.admit['n']),
                                                                   '; median free memory at a timeout %.1f GB' % b.admit['avail_med_timeout'] if b.admit['avail_med_timeout'] is not None else ''))
    gh = sum(sum(x[0] for x in v) for k, v in b.lock['held'].items() if k == 'game run')
    bh = sum(x[0] for x in b.lock['held'].get('exe build', []))
    P('  run lock: %d game holds = %.0f run-minutes; %d exe-build holds = %.0f minutes (other builds: %s)' % (
        len(b.lock['held'].get('game run', [])), gh / 60.0, len(b.lock['held'].get('exe build', [])), bh / 60.0,
        ', '.join('%d %s' % (len(v), k) for k, v in b.lock['held'].items() if k.endswith('build') and k != 'exe build') or 'none'))
    total, parts = run_summary(b)
    P('  game logs in the window: %d  [%s]' % (total, '; '.join(parts)))
    # alive by hour
    P()
    P('  agents alive by hour (average over the hour; an agent is alive from its start / follow-up to the next hand-back):')
    spans = []
    for a in b.agents.values():
        ev = sorted([(a['start'], 1)] + [(t, 1) for t in a['send']] + [(t, -1) for t, _ in a['done']])
        cur, t_open = 0, None
        for t, d in ev:
            if d == 1 and cur == 0:
                t_open = t
            cur = max(0, cur + d) if d == -1 else (1 if cur == 0 else cur)
            if d == -1 and cur == 0 and t_open is not None:
                spans.append((t_open, t))
                t_open = None
        if t_open is not None:
            spans.append((t_open, b.now))
    h = b.t0.replace(minute=0, second=0)
    row = []
    while h < b.t1:
        he = h + timedelta(hours=1)
        tot = sum(max(0.0, (min(e, he) - max(s, h)).total_seconds()) for s, e in spans) / 3600.0
        row.append('%02d:00 %.1f' % (h.hour, tot))
        h = he
    P('    ' + '  '.join(row))
    # A2
    P()
    P('A2. EACH PLAN ROW, FROM FIRST PROBE TO CHECKED')
    if not b.spec:
        P('  (no --rows given)')
    else:
        P('  %-5s %-6s %-6s %-6s %-7s %-8s %s' % ('Row', 'Start', 'Built', 'Merged', 'Checked', 'Total', 'Biggest wait'))
        for r in b.spec['rows']:
            if not r['agents'] and not r['merges']:
                continue
            probe = r['started']
            bl = [a for a in r['agents'] if is_builder_class(a) and a['done']]
            built = min(a['done'][0][0] for a in bl) if bl else None
            merged = r['done'] or (max(m['t'] for m in r['merges']) if r['merges'] else None)
            chk = None
            if b.check_log is not None and merged:
                ct = [t for t, line in b.check_log if check_hits(line, [r['id']], done_only=True) and t >= merged]
                chk = min(ct) if ct else None
            ev = sorted([(a['start'], 'start ' + a['kind']) for a in r['agents']] + [(t, 'hand-back ' + a['kind']) for a in r['agents'] for t, _ in a['done']] +
                        [(t, 'follow-up') for a in r['agents'] for t in a['send']] +
                        [(m['t'], 'merge') for m in r['merges']])
            bw = ''
            if len(ev) > 1:
                gaps = [(mins(ev[i][0], ev[i + 1][0]), ev[i], ev[i + 1]) for i in range(len(ev) - 1)]
                g = max(gaps, key=lambda z: z[0])
                bw = '%s: %s (%s) -> %s (%s)' % (dur(g[0]), g[1][1], c.hm(g[1][0]), g[2][1], c.hm(g[2][0]))
            end = chk or merged
            total_s = dur(mins(probe, end)) if probe and end else '-'
            P('  %-5s %-6s %-6s %-6s %-7s %-8s %s' % (r['id'], c.hm(probe) if probe else '-', c.hm(built) if built else '-', c.hm(merged) if merged else '-',
                                                    c.hm(chk) if chk else '-', total_s, bw))
    # A3
    P()
    P('A3. THE KINDS OF WAITING')
    lanes = []
    for a in b.agents.values():
        if a['done'] and a['merges'] and is_builder_class(a):
            hb = a['done'][0][0]
            mg = min((m['t'] for m in a['merges'] if m['t'] >= hb), default=None)
            if mg:
                lanes.append((mins(hb, mg), a, hb, mg))
    lanes.sort(key=lambda z: -z[0])
    if lanes:
        v = sorted(z[0] for z in lanes)
        P('  1. BAR 1  first hand-back to merge (builder class with both): n %d, median %s, p90 %s' % (len(v), dur(v[len(v) // 2]), dur(v[int(len(v) * 0.9)])))
        if split:
            sp = pt(b.start.strftime('%Y-%m-%d ') + split, '%Y-%m-%d %H:%M')
            for lab, sel in (('hand-back before ' + split, [z[0] for z in lanes if z[2] < sp]), ('hand-back after ' + split, [z[0] for z in lanes if z[2] >= sp])):
                sel = sorted(sel)
                P('       %s: n %d, median %s' % (lab, len(sel), dur(sel[len(sel) // 2]) if sel else '-'))
        for m, a, hb, mg in lanes[:6]:
            P('       %-8s %-40s hand-back %s -> merge %s = %.0f min' % (a['id'][:8], (a['desc'] or '')[:40], c.hm(hb), c.hm(mg), m))
    P('  2. compile queue: %d of %d requests timed out' % (b.admit['outcomes'].get('timeout_fail', 0), b.admit['n']))
    sh = []
    for a in b.agents.values():
        sp_ = (b.subs.get(a['id']) or {}).get('jsonl')
        if sp_ and is_builder_class(a) and not a.get('wf'):
            w_ = agent_waits(sp_)
            if w_:
                sh.append((100.0 * w_[1] / w_[0], w_[0], w_[1]))
    if sh:
        v3 = sorted(x[0] for x in sh)
        P('     BAR 3  merged compile + check + wait calls over wall, builder class: n %d, median %.1f %%, p75 %.1f %%; %.1f h of %.1f h wall' %
          (len(v3), v3[len(v3) // 2], v3[int(0.75 * (len(v3) - 1))], sum(x[2] for x in sh) / 3600.0, sum(x[1] for x in sh) / 3600.0))
    waits = [(w, t, m, k) for k, v in b.lock['waited'].items() for (w, t, m) in v if w > 30]
    P('  3. run lock waits over 30 s: %d, %.0f min in total' % (len(waits), sum(w for w, _, _, _ in waits) / 60.0))
    if b.spec:
        fr = [(r['id'], r['free_from'], r['started']) for r in b.spec['rows'] if r['free_from'] is not None and r['started'] and mins(r['free_from'], r['started']) > 30]
        P('  4. rows free to start and not started for over 30 min:')
        for rid, ff, st in sorted(fr, key=lambda z: -mins(z[1], z[2])):
            P('       %-5s free from %s, started %s (%s)' % (rid, c.hm(ff), c.hm(st), dur(mins(ff, st))))
    return 0


def to_json(b):
    def j(t):
        return t.strftime('%Y-%m-%d %H:%M:%S') if isinstance(t, datetime) else t
    lanes, games = alive_counts(b)
    total, parts = run_summary(b)
    return {'session': b.session, 'branch': b.branch, 'now': j(b.now), 'start': j(b.start), 'live': b.live, 'game_logs': total, 'game_logs_by_folder': parts,
            'alive': {'lanes': len(lanes), 'games': len(games)},
            'lanes': [{'id': a['id'], 'desc': a['desc'], 'kind': a['kind'], 'state': a['state'], 'waiting_on': a['waiting_on'], 'wait_min': a['wait_min'],
                       'start': j(a['start']), 'done': [[j(t), s] for t, s in a['done']], 'send': [j(t) for t in a['send']], 'first_commit': j(a['first_commit']),
                       'merged': [j(m['t']) for m in a['merges']]} for a in b.agents.values()],
            'rows': [{'id': r['id'], 'state': r['state'], 'free_from': j(r['free_from']), 'started': j(r['started']), 'done': j(r['done'])} for r in (b.spec or {'rows': []})['rows']]}


def main(argv=None):
    ap = argparse.ArgumentParser(description='the session board (read-only)')
    ap.add_argument('--repo', help='the checkout whose out\\ is read (default: the main checkout)')
    ap.add_argument('--session', help='transcript session id (default: the newest .jsonl by write time)')
    ap.add_argument('--rows', help='plan rows: s105 | s106 | a json file')
    ap.add_argument('--branch', help='the session branch (default: from the rows file, else the current branch)')
    ap.add_argument('--go', action='append', help='satisfy a go:X need')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--fixed', action='store_true')
    ap.add_argument('--write-fixed', metavar='FILE')
    ap.add_argument('--audit', action='store_true')
    ap.add_argument('--split', help='HH:MM: A3 prints the hand-back-to-merge median before / after it')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        sys.path.insert(0, HERE)
        import board_selftest
        return board_selftest.main()
    t = time.time()
    b = collect(a)
    if a.json:
        print(json.dumps(to_json(b), indent=1))
    elif a.fixed:
        print(fixed_text_for_cli(fixed_block(b)))
    elif a.write_fixed:
        rc = do_write_fixed(a.write_fixed, b)
        print('board: fixed block written to ' + a.write_fixed)
        return rc
    elif a.audit:
        print_audit(b, split=a.split)
    else:
        print_board(b, show_all=a.all)
    print('(board: %.1f s)' % (time.time() - t), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
r"""Tool-use audit of a session's agents (S110; Jarrod: "auditing the tool use seems pretty good for optimizing probes").
One row per subagent: calls, replies, calls per reply (batching), q output cuts, the call kinds, and for builders the edits
(count, per file, median size) and the medit use (S110 PROVE: the `prove r/c/f` column: runs of tools\handback\prove.py, the commands and the failed commands their own last line
`prove: N passed, M failed[, K NONE]` printed, a background run's as read later from its output file or the file its stdout went to, once per run; a `git add` naming prove.py is not a run: one call per proof against one per command) (S110 MEDIT: runs of tools\edit\medit.py, also by a relative path; the blocks those runs
printed as APPLIED; the failed runs. Read from each run's own result, so an editfile of any name counts, a rerun after a failure is not
counted twice, and a script that prints medit's lines is not a run; set against the single Edit calls: the adoption of the multi-edit
tool; tools\harness\tool_calls_selftest.py). Reads the agents' transcripts only; writes nothing.
The agent name is the lane key of its description (tools\harness\lane_time.py's lane_key: SEAT-A, SEAT-B and SEAT-C stay three lanes).

  py -I tools\harness\tool_calls.py [SESSION_ID]     (default: the newest session folder that has subagents)
  py -I tools\harness\tool_calls.py SESSION_ID --edits   (adds the per-file edit table)
"""
import collections
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lane_time  # noqa: E402  (lane_key: the one rule for where a lane key sits in a description)

MEDIT_RUN = re.compile(r'(?:^|[\s"\'\\/])medit\.py\b')       # a run of the tool (medit_selftest.py / medit_known_case.py are not)
MEDIT_APPLIED = re.compile(r'^medit: (\d+) block\(s\) in \d+ file\(s\) applied', re.M)
MEDIT_FAILED = re.compile(r'^medit: NOTHING written', re.M)
MEDIT_CHECK = re.compile(r'^medit --check: ', re.M)
# a run of tools\handback\prove.py: python started on it (prove_selftest.py is not; nor is a `git add` / `git diff` that names the file)
PROVE_RUN = re.compile(r'(?:^|[\s;&|("\'])(?:py|python3?)(?:\.exe)?\s+(?:-\S+\s+)*["\']?(?:[^\s"\']*[\\/])?prove\.py\b')
# its last line: the commands of that run (also as a Read shows it, after a line number and a tab)
PROVE_TOTAL = re.compile(r'(?:^|\t)prove: (\d+) passed, (\d+) failed(?:, (\d+) NONE)?', re.M)
# a background run's own result only names its output file; the verdict is read from that file by a later call
PROVE_BG = re.compile(r'Output is being written to: (\S+?\.output)')
PROVE_TO = re.compile(r'(?<![\d&])>\s*["\']?([^\s"\'|;&]+)')     # `prove.py X > FILE`: the verdict is read from FILE

ROOT = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp')


def kind(name, inp):
    if name in ('Bash', 'PowerShell'):
        c = inp.get('command', '')
        if MEDIT_RUN.search(c):
            return 'medit'
        if PROVE_RUN.search(c):
            return 'prove'
        m = re.search(r'q[\\/]q\.py\s+(\w+)', c)
        if m:
            return 'q ' + m.group(1)
        if re.search(r'\b(grep|rg|findstr|Select-String)\b', c):
            return 'shell grep'
        if re.search(r'\bpy\b|python', c):
            return 'script run'
        return 'shell other'
    return name


def label(first_user_text, desc=None, agent_type=''):
    """the agent's name: the lane key of its description (lane_time.lane_key, the one rule), 'review:' before it for a reviewer; without a
    description (no .meta.json) the old reading of the first prompt, whose key stops at a hyphen (SEAT-A -> SEAT)"""
    if desc:
        key = lane_time.lane_key(desc)
        if key != 'other':
            return ('review:' if agent_type == 'review-fixbuilder' or re.match(r'(?:OPUS|SON)\s+review\b', desc) else '') + key
    m = re.search(r'\b(?:Review lane|Lane)\s+([A-Z][A-Z0-9_]+)', first_user_text or '')
    if not m:
        return desc[:16] if desc else '?'
    return ('review:' if 'Review lane' in first_user_text[:m.end()] else '') + m.group(1)


def meta_of(path):
    try:
        with open(path[:-len('.jsonl')] + '.meta.json', encoding='utf-8') as f:
            m = json.load(f)
        return m.get('description') or '', m.get('agentType') or ''
    except (OSError, ValueError):
        return '', ''


def result_text(c):
    """a tool_result's content: a string or a list of text parts"""
    if isinstance(c, list):
        return '\n'.join(x.get('text', '') for x in c if isinstance(x, dict))
    return c if isinstance(c, str) else ''


def audit(path):
    kinds = collections.Counter()
    per_msg = collections.Counter()
    edits = []
    medit = collections.Counter()       # blocks / failed / check: from the medit runs' own results
    medit_ids = set()
    prove = collections.Counter()       # commands / failed: from the prove.py runs' own last line
    prove_ids = set()
    prove_files = {}                    # a background run's output file -> the run's call id
    prove_reads = {}                    # a later call that names such a file -> the run's call id
    prove_last = {}                     # run call id -> (passed, failed, none) of its last line: a run read twice is one run
    cut = 0
    seen = set()
    name = None
    desc, atype = meta_of(path)
    for line in open(path, encoding='utf-8', errors='replace'):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        m = r.get('message') or {}
        content = m.get('content')
        if name is None and m.get('role') == 'user':
            text = content if isinstance(content, str) else ' '.join(b.get('text', '') for b in content or [] if isinstance(b, dict))
            name = label(text, desc, atype)
        if m.get('role') == 'user' and isinstance(content, list) and prove_ids:
            for b in content:
                if not (isinstance(b, dict) and b.get('type') == 'tool_result'):
                    continue
                tid = b.get('tool_use_id')
                pid = tid if tid in prove_ids else prove_reads.get(tid)
                if pid is None:
                    continue
                t = result_text(b.get('content'))
                f = PROVE_BG.search(t)
                if f and tid in prove_ids:
                    prove_files[os.path.normcase(f.group(1))] = pid
                for ok, bad, none in PROVE_TOTAL.findall(t):
                    prove_last[pid] = (int(ok), int(bad), int(none or 0))
        if m.get('role') == 'user' and isinstance(content, list) and medit_ids:
            for b in content:
                if isinstance(b, dict) and b.get('type') == 'tool_result' and b.get('tool_use_id') in medit_ids:
                    t = result_text(b.get('content'))
                    medit['blocks'] += sum(int(x) for x in MEDIT_APPLIED.findall(t))
                    medit['failed'] += len(MEDIT_FAILED.findall(t))
                    medit['check'] += len(MEDIT_CHECK.findall(t))
        if m.get('role') != 'assistant' or not isinstance(content, list):
            continue
        for b in content:
            if not (isinstance(b, dict) and b.get('type') == 'tool_use') or b.get('id') in seen:
                continue
            seen.add(b.get('id'))
            inp = b.get('input') or {}
            k = kind(b.get('name'), inp)
            kinds[k] += 1
            if k == 'medit':
                medit_ids.add(b.get('id'))
            if k == 'prove':
                prove_ids.add(b.get('id'))
                for to in PROVE_TO.findall(inp.get('command', '')):
                    prove_files[os.path.normcase(to)] = b.get('id')
            if prove_files:
                where = os.path.normcase('%s %s' % (inp.get('file_path', ''), inp.get('command', '')))
                for fp, pid in prove_files.items():
                    if fp in where:
                        prove_reads[b.get('id')] = pid
            per_msg[m.get('id')] += 1
            c = inp.get('command', '')
            if 'q.py' in c and re.search(r'\|\s*(head|tail|Select-Object\s+-(First|Last)|select\s+-(First|Last))', c, re.I):
                cut += 1
            if b.get('name') == 'Edit':
                edits.append((os.path.basename(inp.get('file_path', '')), len(inp.get('new_string', ''))))
    medit['runs'] = kinds.get('medit', 0)
    for ok, bad, none in prove_last.values():
        prove['commands'] += ok + bad + none
        prove['failed'] += bad + none
    prove['runs'] = kinds.get('prove', 0)
    return name or '?', kinds, per_msg, cut, edits, dict(medit), dict(prove)


def main(argv):
    show_edits = '--edits' in argv
    args = [a for a in argv if not a.startswith('--')]
    if args:
        folder = os.path.join(ROOT, args[0], 'subagents')
    else:
        cands = sorted(glob.glob(os.path.join(ROOT, '*', 'subagents')), key=os.path.getmtime)
        if not cands:
            print('no session with subagents under ' + ROOT)
            return 1
        folder = cands[-1]
    files = sorted(glob.glob(os.path.join(folder, 'agent-*.jsonl')), key=os.path.getmtime)
    print('session %s: %d agents (order: last write)' % (os.path.basename(os.path.dirname(folder)), len(files)))
    print('%-18s %5s %7s %9s %4s %5s %11s %9s  kinds' % ('agent', 'calls', 'replies', 'per reply', 'cut', 'Edit', 'medit r/b/f', 'prove r/c/f'))
    tot_edit, tot, tot_prove = 0, collections.Counter(), collections.Counter()
    for f in files:
        name, kinds, per_msg, cut, edits, md, pv = audit(f)
        tot_edit += len(edits)
        tot.update(md)
        tot_prove.update(pv)
        n = sum(kinds.values())
        reqs = max(len(per_msg), 1)
        top = ', '.join('%s %d' % kv for kv in kinds.most_common(6))
        print('%-18s %5d %7d %9.2f %4d %5d %11s %9s  %s' % (name, n, reqs, n / reqs, cut, len(edits), '%d/%d/%d' % (md.get('runs', 0), md.get('blocks', 0), md.get('failed', 0)),
                                                         '%d/%d/%d' % (pv.get('runs', 0), pv.get('commands', 0), pv.get('failed', 0)), top))
        if show_edits and edits:
            by = collections.Counter(e[0] for e in edits)
            sizes = sorted(e[1] for e in edits)
            print('    edits per file: %s; median new chars %d' % (', '.join('%s %d' % kv for kv in by.most_common()), sizes[len(sizes) // 2]))
    print('edits in all: Edit calls %d; medit runs %d (%d --check), %d blocks applied, %d failed runs (r/b/f = runs / blocks the runs printed as '
          'applied / runs that wrote NOTHING)' % (tot_edit, tot.get('runs', 0), tot.get('check', 0), tot.get('blocks', 0), tot.get('failed', 0)))
    print('proofs in all: prove.py runs %d, %d commands in them, %d of those commands not PASS (r/c/f = runs / commands the runs printed on their last line, '
          'a background run\'s as read from its output file / of them not PASS, NONE included; set against the "script run" calls)'
          % (tot_prove.get('runs', 0), tot_prove.get('commands', 0), tot_prove.get('failed', 0)))
    print('cut = q output piped through head / tail / Select -First (the rule says never). "shell grep" is any shell command holding a grep.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

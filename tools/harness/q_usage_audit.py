r"""Did an agent search with q first? (Session 103, 2026-11-17; Jarrod: "we should make sure that rule stands for probes, reviewers,
gates, and everything else too"; CLAUDE.md Agents "Search with q first".)

Reads subagent transcripts (.jsonl under %USERPROFILE%\.claude\projects\<project>\<session>\subagents\) and counts, per agent:
  q calls        shell commands running tools\q\q.py (any sub-command: ours / decomp / field / fact / fn ...)
  searches       Grep tool calls + shell grep / rg / findstr / Select-String that SEARCH (a folder, a glob, no path, -r) over
                 src\ tools\ probes\ generated\ docs\ thirdparty\: what q could answer
  reads          (S103 QX5) reading a file the agent already knows: sed -n / cat / head / tail / awk / Get-Content / Read of a named path, and a
                 grep / Grep whose only target is one named file. Counted apart: they are not searches q could replace, so they no longer drag
                 the ratio down (the TOTAL line also prints the share with them counted as searches)
  ratio          q / (q + searches); flagged LOW below --min (default 0.25) when searches >= 5
Prints only counts and the agent's description, never message contents.

    py -I tools\harness\q_usage_audit.py [SESSION_DIR_OR_JSONL ...] [--min 0.25] [--since HH:MM]
With no path: this project's newest session's subagents folder.
"""
import argparse
import glob
import json
import os
import re
import sys

PROJ = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp')
Q_RE = re.compile(r'tools[\\/]+q[\\/]+q\.py|\bq\.py\s+(ours|decomp|field|fact|fn|callers|callees|str|imm|addr|map|db)\b')
CODE_PATH = re.compile(r'(src|tools|probes|generated|docs|thirdparty)[\\/]')
READ_CMD = re.compile(r'(^|[|;&(\s])(sed\s+-n|cat|head|tail|awk|Get-Content|type)\b')
SEARCH_CMD = re.compile(r'(^|[|;&(\s])(grep|rg|findstr|Select-String)\b')
FILE_TOK = re.compile(r'^[\w./\\:$~-]+\.\w{1,6}$')
RECURSIVE = re.compile(r'(\s-[a-zA-Z]*[rR]\b|--recursive|-Recurse|\*|\s\.(\s|$)|/\s*$)')     # applied with quoted text removed: a "*" inside a pattern is not a glob
QUOTED = re.compile(r'"[^"]*"|\'[^\']*\'')
FIND_CMD = re.compile(r'(^|[|;&(\s])(find|Get-ChildItem|gci|ls|dir)\b')
BARE_DIR = re.compile(r'(^|\s)(src|tools|probes|generated|docs|thirdparty)(\s|$)')
PATTERN_FLAG = re.compile(r'^(-e|--regexp|-Pattern|-f|--file)$', re.I)


def one_file_target(cmd):
    """a grep / rg / findstr / Select-String whose path arguments are all single named files (no -r, no glob, no folder): a read of a known file"""
    m = SEARCH_CMD.search(cmd)
    seg = cmd[m.end():] if m else cmd
    if RECURSIVE.search(' ' + QUOTED.sub('""', seg)):
        return False
    raw = [t for t in re.split(r'\s+', seg.strip()) if t]
    has_pat_flag = any(PATTERN_FLAG.match(t) for t in raw)
    toks = [t.strip('\'"') for t in raw if not t.startswith('-')]
    if not has_pat_flag and toks:
        toks = toks[1:]                       # the first plain argument of grep / rg / findstr is the pattern, never a path ("foo\.bar" looks like a file name)
    paths = [t for t in toks if re.search(r'[\\/]', t) or FILE_TOK.match(t)]
    if not paths:
        return '|' in cmd[:m.start()] if m else False     # fed by a pipe (cat f | grep x, git show REV:f | grep x): a read
    return all(FILE_TOK.match(t) for t in paths) and len(paths) == len(toks)


def classify(name, inp):
    if name == 'Read':
        return 'read' if CODE_PATH.search(str(inp.get('file_path', ''))) else None
    if name == 'Grep':
        path = str(inp.get('path', '')).strip()
        if path and FILE_TOK.match(path.strip('"')) and not inp.get('glob'):
            return 'read'          # one named file
        p = path + ' ' + str(inp.get('glob', ''))
        return 'raw' if (not p.strip() or CODE_PATH.search(p) or 'src' in p) else None
    if name in ('Bash', 'PowerShell'):
        c = inp.get('command', '') or ''
        if Q_RE.search(c):
            return 'q'
        if re.search(r'\bgit\s+show\b', c) and SEARCH_CMD.search(c):
            return 'read'                     # git show REV:file | grep: one named file at a revision
        if SEARCH_CMD.search(c) and (CODE_PATH.search(c) or BARE_DIR.search(c)):
            return 'read' if one_file_target(c) else 'raw'
        if FIND_CMD.search(c) and (CODE_PATH.search(c) or BARE_DIR.search(c)) and not READ_CMD.search(c):
            return 'raw'                      # find src -type f: a tree walk q could answer
        if READ_CMD.search(c) and CODE_PATH.search(c):
            return 'read'
    return None


def audit(path):
    q = raw = rd = 0
    desc = ''
    try:
        meta = path[:-6] + '.meta.json'
        if os.path.exists(meta):
            desc = (json.load(open(meta, encoding='utf-8')).get('description') or '')[:60]
    except Exception:
        pass
    seen = set()
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get('type') != 'assistant':         # api-request-blob records repeat the same tool_use blocks
                continue
            cont = (r.get('message') or {}).get('content')
            if not isinstance(cont, list):
                continue
            for b in cont:
                if isinstance(b, dict) and b.get('type') == 'tool_use':
                    if b.get('id') in seen:
                        continue
                    seen.add(b.get('id'))
                    k = classify(b.get('name'), b.get('input') or {})
                    if k == 'q':
                        q += 1
                    elif k == 'raw':
                        raw += 1
                    elif k == 'read':
                        rd += 1
    return q, raw, rd, desc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--min', type=float, default=0.25)
    a = ap.parse_args()
    files = []
    paths = a.paths
    if not paths:
        sess = sorted(glob.glob(os.path.join(PROJ, '*', 'subagents')), key=os.path.getmtime)
        paths = sess[-1:] if sess else []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, '*.jsonl'))) if os.path.isdir(p) else [p]
    low = 0
    tq = tr = td = 0
    print('%-28s %5s %6s %5s %6s  %s' % ('agent', 'q', 'search', 'read', 'ratio', 'description'))
    for fpath in files:
        q, raw, rd, desc = audit(fpath)
        tq, tr, td = tq + q, tr + raw, td + rd
        if q + raw == 0:
            continue
        ratio = q / (q + raw)
        flag = 'LOW' if raw >= 5 and ratio < a.min else ''
        low += bool(flag)
        print('%-28s %5d %6d %5d %6.2f  %s %s' % (os.path.basename(fpath)[:28], q, raw, rd, ratio, desc, flag))
    print('agents LOW on q: %d' % low)
    print('TOTAL: %d agents, q %d, searches %d, reads %d; q share of the searches q could answer %.0f %% (if the reads were counted as searches too: %.0f %%)'
          % (len(files), tq, tr, td, 100.0 * tq / max(tq + tr, 1), 100.0 * tq / max(tq + tr + td, 1)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

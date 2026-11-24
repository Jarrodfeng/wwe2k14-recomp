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

    py -I tools\harness\q_usage_audit.py [SESSION_DIR_OR_JSONL ...] [--min 0.25] [--selftest]
S108: a miss is a REPO-WIDE search only (line-range reads of a known file, a grep in one named file, filters on command output, git diff / show / log are
not q questions); the TOTAL prints the old share beside the new one for one release.
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
RECURSIVE = re.compile(r'(\s-[a-zA-Z]{0,4}[rR][a-zA-Z]{0,4}\b|--recursive|-Recurse|\*|\s\.(\s|$)|/\s*$)')     # applied with quoted text removed: a "*" inside a pattern is not a glob; a flag cluster (-rn), not a PowerShell -Pattern
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


LS_ONLY = re.compile(r'^\s*(ls|dir)\b(?!.*(\s-[a-zA-Z]*R|\s/s\b))', re.I)     # re.I: any flag holding an r (-lR, -Recurse) counts as recursive
GIT_NOT_SEARCH = re.compile(r'^\s*git\s+(diff|show|log|blame|status|stash|merge-base|rev-parse)\b')
GIT_GREP = re.compile(r'\bgit(\s+-[Cc]\s+\S+|\s+--?[\w-]+(=\S+)?)*\s+grep\b')
CHAIN = re.compile(r'&&|\|\||;|\n')
WALKER = re.compile(r'(^|[;&(\s])(find|Get-ChildItem|gci)\b|(^|\s)(ls|dir)\b.*(\s-[a-zA-Z]*R|\s/s\b)', re.I)


def fed_by_walk(c):
    """a search stage fed by a tree walk (find src | xargs grep, Get-ChildItem -Recurse src | Select-String): many files, though the search names none"""
    st = c.split('|')
    for i in range(1, len(st)):
        if SEARCH_CMD.search(' ' + st[i]) or re.search(r'\bxargs\b', st[i]):
            return any(WALKER.search(x) for x in st[:i])
    return False


def classify(name, inp):
    """the miss definition (S108): only a REPO-WIDE search is a q miss. Not a q question: a line-range read of a known file, a grep inside one named file,
    a filter on command output, git diff / show / log, a listing of one folder. Those are 'read' (counted apart, never in the share).
    The git / ls exemption covers ONE pipeline whose later stages search nothing themselves (review: `git status && grep -rn W src` and
    `git log | grep -rn W src` are searches); `git grep` without a named file and a search fed by find / Get-ChildItem are searches."""
    k = classify_old(name, inp)
    if name not in ('Bash', 'PowerShell') or k == 'q':
        return k
    c = inp.get('command', '') or ''
    if (GIT_GREP.search(c) and k != 'read') or fed_by_walk(c):
        return 'raw'
    if k == 'raw' and not CHAIN.search(c):
        head, *tail = c.split('|')
        if (GIT_NOT_SEARCH.search(head) or LS_ONLY.search(head)) and not any(classify_old(name, {'command': s}) == 'raw' for s in tail):
            return 'read'
    return k


def classify_old(name, inp):
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


def selftest():
    """S108: only a repo-wide search is a miss. Each case is (tool, input, old class, new class); the 'git diff | grep' and 'ls' twins were 'raw' before."""
    cases = [
        ('Bash', {'command': 'grep -rn WORD src tools'}, 'raw', 'raw'),
        ('Grep', {'pattern': 'WORD'}, 'raw', 'raw'),
        ('Bash', {'command': 'git grep -n WORD main -- src'}, 'raw', 'raw'),
        ('Bash', {'command': 'git diff main -- src/a.cpp | grep -n WORD'}, 'read', 'read'),
        ('Bash', {'command': 'git log -p -- src tools | grep -rn WORD src'}, 'raw', 'raw'),     # review: the grep after the pipe names a folder and -r: a repo-wide search
        ('Bash', {'command': 'git status && grep -rn WORD src'}, 'raw', 'raw'),
        ('Bash', {'command': 'ls src && rg WORD src'}, 'raw', 'raw'),
        ('Bash', {'command': 'ls -lR src'}, 'raw', 'raw'),
        ('Bash', {'command': 'git grep -n WORD'}, None, 'raw'),                                   # git grep with no path = the whole tree
        ('Bash', {'command': 'git -C C:/x grep -n WORD -- src'}, 'raw', 'raw'),
        ('Bash', {'command': 'git grep -n WORD -- src/a.cpp'}, 'read', 'read'),
        ('Bash', {'command': 'find src -name "*.cpp" | xargs grep -n WORD'}, 'read', 'raw'),     # a search fed by a tree walk
        ('PowerShell', {'command': 'Get-ChildItem -Recurse src | Select-String WORD'}, 'read', 'raw'),
        ('PowerShell', {'command': 'Get-Content src/a.cpp | Select-String -Pattern WORD'}, 'read', 'read'),   # -Pattern is not a -r flag
        ('Bash', {'command': 'ls src/fx | grep blur'}, 'read', 'read'),
        ('Bash', {'command': 'ls src/fx'}, 'raw', 'read'),
        ('Bash', {'command': 'find src -type f -name "*.h"'}, 'raw', 'raw'),
        ('Bash', {'command': 'sed -n 10,40p src/a.cpp'}, 'read', 'read'),
        ('Bash', {'command': 'grep -n WORD src/a.cpp'}, 'read', 'read'),
        ('Bash', {'command': 'out/tools/x.exe 2>&1 | tail -3'}, 'read', 'read'),
        ('Bash', {'command': 'py tools\\q\\q.py ours refs X'}, 'q', 'q'),
    ]
    bad = 0
    for name, inp, old, new in cases:
        o, n = classify_old(name, inp), classify(name, inp)
        ok = (o, n) == (old, new)
        bad += not ok
        print('%s  %s %s: old %s new %s' % ('PASS' if ok else 'FAIL', name, inp, o, n))
    print('q_usage_audit selftest: %s' % ('all passed' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


def audit(path):
    q = raw = rd = raw_old = 0
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
                    if classify_old(b.get('name'), b.get('input') or {}) == 'raw':
                        raw_old += 1
                    if k == 'q':
                        q += 1
                    elif k == 'raw':
                        raw += 1
                    elif k == 'read':
                        rd += 1
    return q, raw, rd, desc, raw_old


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--min', type=float, default=0.25)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    files = []
    paths = a.paths
    if not paths:
        sess = sorted(glob.glob(os.path.join(PROJ, '*', 'subagents')), key=os.path.getmtime)
        paths = sess[-1:] if sess else []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, '*.jsonl'))) if os.path.isdir(p) else [p]
    low = 0
    tq = tr = td = tro = 0
    print('%-28s %5s %6s %5s %6s  %s' % ('agent', 'q', 'search', 'read', 'ratio', 'description'))
    for fpath in files:
        q, raw, rd, desc, rawo = audit(fpath)
        tq, tr, td, tro = tq + q, tr + raw, td + rd, tro + rawo
        if q + raw == 0:
            continue
        ratio = q / (q + raw)
        flag = 'LOW' if raw >= 5 and ratio < a.min else ''
        low += bool(flag)
        print('%-28s %5d %6d %5d %6.2f  %s %s' % (os.path.basename(fpath)[:28], q, raw, rd, ratio, desc, flag))
    print('agents LOW on q: %d' % low)
    print('TOTAL: %d agents, q %d, repo-wide searches %d, reads %d; q share of the repo-wide searches %.0f %% (S108 definition)'
          % (len(files), tq, tr, td, 100.0 * tq / max(tq + tr, 1)))
    print('       old definition (git diff / show filters and the like also counted as searches): searches %d, q share %.0f %%; reads counted as searches too: %.0f %%'
          % (tro, 100.0 * tq / max(tq + tro, 1), 100.0 * tq / max(tq + tro + td, 1)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

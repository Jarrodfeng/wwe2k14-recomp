"""Report only (not in the gate): how many of the four fix builders' Read and search tool calls landed on a file and line range the pack
already held (BPK "second number").

    py -I tools/builder/pack_transcripts.py [--dir SUBAGENTS_DIR]

Builders are found among the S103 subagent transcripts (default: the session 02866af2... subagents folder) as the transcript whose own
`git commit` printed the fix commit's short hash and whose first user message names the lane. Prints counts only, never message text.
Classification of a call follows tools/harness/agent_time_audit.py classify(): Read / Grep / Glob, and Bash / PowerShell read_search
commands. A call "landed in the pack" when:
  Read: the file has a pack block and the read range (offset .. offset+limit, default the whole file) overlaps it;
  search (Grep, or a grep / rg / Select-String / sed / cat command): a file:line in its result lies inside a pack block, or a sed -n A,Bp /
  Get-Content range of a pack file overlaps a block. Glob and searches with no file:line in their result are counted as "unplaceable".
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'harness'))
import pack  # noqa: E402
import pack_refind  # noqa: E402
import agent_time_audit as ata  # noqa: E402

DEF_DIR = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp', '02866af2-aea6-4d68-9d93-d5d891d16498',
                       'subagents')
LANES = {'RFX': ['RFX'], 'SVD': ['SVY', 'SVD'], 'TRN4': ['TRN4', 'TRN'], 'AUS2': ['AUSF', 'AUS2', 'AUS']}
REL_RX = re.compile(r'(?:wwe2k14-recomp(?:[\\/]+\.claude[\\/]+worktrees[\\/]+[^\\/]+)?|sdk-wt[\\/]+[^\\/]+)[\\/]+((?:src|tools|docs|probes|config)[\\/][^\s:"\'`|,;)]+)')
HIT_RX = re.compile(r'((?:[A-Za-z]:)?[^\s:"\'`|]*?(?:src|tools|probes)[\\/][^\s:"\'`|]+?\.\w+)[:(](\d+)[:)]')


def rel_of(p):
    if not p:
        return None
    m = REL_RX.search(p)
    if m:
        return m.group(1).replace('\\', '/')
    p = p.replace('\\', '/')
    m = re.search(r'(?:^|/)((?:src|tools|probes|docs)/.*)$', p)
    return m.group(1) if m else None


def first_user_text(recs):
    for r in recs:
        if r.get('type') == 'user':
            c = (r.get('message') or {}).get('content')
            if isinstance(c, str):
                return c
            if isinstance(c, list):
                return ' '.join(x.get('text', '') for x in c if isinstance(x, dict) and x.get('type') == 'text')
    return ''


def calls(recs):
    """-> [(name, input, result_text)]"""
    res, out = {}, []
    for r in recs:
        c = (r.get('message') or {}).get('content')
        if not isinstance(c, list):
            continue
        for x in c:
            if not isinstance(x, dict):
                continue
            if x.get('type') == 'tool_use':
                out.append([x.get('name'), x.get('input') or {}, x.get('id'), ''])
            elif x.get('type') == 'tool_result':
                t = x.get('content')
                if isinstance(t, list):
                    t = ' '.join(y.get('text', '') for y in t if isinstance(y, dict))
                res[x.get('tool_use_id')] = t or ''
    for o in out:
        o[3] = res.get(o[2], '')
    return [(n, i, t) for n, i, _, t in out]


def find_builders(d, commits, words):
    """transcripts that made one of `commits` (its git commit printed the short hash), or that name the lane in the first user message
    as "builder lane X" / "lane X", ran a git commit and mention one of the hashes (a `git commit -q` prints no hash)"""
    shorts = [c[:7] for c in commits]
    hits = []
    for f in sorted(os.listdir(d)):
        if not f.endswith('.jsonl'):
            continue
        p = os.path.join(d, f)
        try:
            with open(p, encoding='utf-8', errors='replace') as fh:
                raw = fh.read()
        except OSError:
            continue
        if not any(s in raw for s in shorts) or 'commit' not in raw:
            continue
        recs = []
        for line in raw.splitlines():
            try:
                recs.append(json.loads(line))
            except ValueError:
                pass
        cs = calls(recs)
        made = any(n in ('Bash', 'PowerShell') and 'commit' in (i.get('command') or '') and any(s in t for s in shorts) for n, i, t in cs)
        first = first_user_text(recs)
        lane = any(re.search(r'\b(?:builder\s+)?lane\s+' + w + r'\b', first) for w in words)
        committed = any(n in ('Bash', 'PowerShell') and 'git commit' in (i.get('command') or '') for n, i, t in cs)
        if made or (lane and committed):
            hits.append((p, cs))
    return hits


def overlaps(blocks, rel, lo, hi):
    return any(b.file == rel and b.start <= hi and lo <= b.end for b in blocks)


def judge(name, inp, text, blocks):
    """-> 'in' | 'out' | 'unplaceable' for one read / search call"""
    held = {b.file for b in blocks}
    if name == 'Read':
        rel = rel_of(inp.get('file_path'))
        if not rel:
            return 'unplaceable'
        off = int(inp.get('offset') or 1)
        lim = int(inp.get('limit') or 2000)
        return 'in' if overlaps(blocks, rel, off, off + lim - 1) else 'out'
    if name == 'Glob':
        return 'unplaceable'
    cmd = inp.get('command') or ''
    m = re.search(r"sed\s+-n\s+'?(\d+),(\d+)p'?\s+(\S+)", cmd)
    if m:
        rel = rel_of(m.group(3))
        if rel:
            return 'in' if overlaps(blocks, rel, int(m.group(1)), int(m.group(2))) else 'out'
    hits = [(rel_of(a), int(b)) for a, b in HIT_RX.findall(text or '')]
    hits = [(r, n) for r, n in hits if r]
    if not hits:
        # a Grep with a single path and line numbers in its result
        path = rel_of(inp.get('path') or '')
        if path and name == 'Grep':
            nums = [int(x) for x in re.findall(r'(?m)^(\d+)[:-]', text or '')]
            if nums:
                return 'in' if any(overlaps(blocks, path, n, n) for n in nums) else 'out'
        return 'unplaceable'
    return 'in' if any(overlaps(blocks, r, n, n) for r, n in hits) else 'out'


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=DEF_DIR)
    a = ap.parse_args(argv)
    if not os.path.isdir(a.dir):
        print(f'pack_transcripts: no transcripts at {a.dir}')
        return 2
    print('pack_transcripts: the fix builders\' Read / search calls vs the packs built for their fixes (counts only)')
    print(f'{"pairs":<9} {"fixes":<40} {"builders":>8} {"read+search":>11} {"in pack":>8} {"not in":>7} {"unplaceable":>11}')
    tot = {'in': 0, 'out': 0, 'unplaceable': 0}
    cases = {c[0]: c for c in pack_refind.CASES}
    # one RFX builder made the RFX commits AND SVY's c8aac66ef (found in its transcript): RFX and SVD are one row, judged against both packs
    for row in (('RFX', 'SVD'), ('TRN4',), ('AUS2',)):
        blocks, fixes, words = [], [], []
        for name in row:
            case = cases[name]
            notes = pack_refind.notes_of(case)      # the pinned note commits
            for c in case[3] + case[4]:              # probe-led + review-fix commits (one builder made both)
                parent = pack.git('rev-parse', c + '^1').strip()
                blocks += pack.build(notes, pack.GitTree(parent), use_q=False).blocks
                fixes.append(c)
            words += LANES[name]
        bs = find_builders(a.dir, fixes, words)
        cnt = {'in': 0, 'out': 0, 'unplaceable': 0}
        for _, cs in bs:
            for n, i, t in cs:
                if ata.classify(n, i) != 'read_search':
                    continue
                cnt[judge(n, i, t, blocks)] += 1
        for k in tot:
            tot[k] += cnt[k]
        print(f'{"+".join(row):<9} {" ".join(c[:9] for c in fixes):<40} {len(bs):>8} {sum(cnt.values()):>11} {cnt["in"]:>8} {cnt["out"]:>7} '
              f'{cnt["unplaceable"]:>11}' + ('   (no builder transcript made it: likely the main session)' if not bs else ''))
    n = sum(tot.values())
    placed = tot['in'] + tot['out']
    print(f'{"TOTAL":<6} {"":<10} {"":>19} {n:>11} {tot["in"]:>8} {tot["out"]:>7} {tot["unplaceable"]:>11}')
    if placed:
        print(f'of the placeable read / search calls, {tot["in"] / placed:.0%} landed on a range the pack already held')
    return 0


if __name__ == '__main__':
    sys.exit(main())

r"""Token-efficiency audit of subagent transcripts (Session 105, lane S105-TA; Jarrod: a token audit on top of the wall-clock one).

Reads subagent transcripts (.jsonl under %USERPROFILE%\.claude\projects\<project>\<session>\subagents\) and prints, per agent:
  turns (one per API response, deduplicated by message id); input tokens split cache read / cache creation / uncached, and output
  tokens (all exact, from each assistant message's usage block: input_tokens, cache_read_input_tokens, cache_creation_input_tokens,
  output_tokens); the context curve (cache read + creation + uncached input per turn: start, median, max, end); the largest tool
  results (tool name + a short descriptor = the command's first 80 chars or the file path; never the result's text) with the turn
  they arrived, how many turns carried them, and size x those turns = cumulative re-read cost; tool-class totals; repeated identical
  tool calls; the share of all input tokens that is the initial brief + setup files (the first turn's context carried by every later
  turn, plus results of reads of docs\agents\*, SESSION*_PLAN.md, STATUS / LESSONS / CLAUDE / common.md / pack files).
Prints only counts, sizes and short descriptors, never message contents (same privacy rule as q_usage_audit.py).
A tool result's token size is ESTIMATED as chars / 3.6 (transcripts store no per-result token count); usage-block numbers are exact.
"Re-read cost" is in token-reads (one token carried in the context of one later turn = 1).

    py -I tools\harness\token_audit.py AGENT_ID_OR_DESC_SUBSTRING [...] [--dir SUBAGENTS_DIR] [--top 12] [--compare] [--detail]
  A selector matching an agent id prefix (e.g. a6b32780) or a substring of its meta description ("OPUS probe A12") selects it.
  --compare   one summary table across the selected agents instead of the per-agent reports (--detail prints both)
  --classes   per-agent split by call class (q / raw read / raw search / other): calls, result tokens, carried cost (tokens x later turns),
              plus a cross-agent total; --finished selects every agent whose meta says finished true; --misses N lists the N biggest raw read/search results
With no --dir: this project's newest session's subagents folder.
"""
import argparse
import collections
import glob
import json
import os
import re
import statistics
import datetime
import sys

PROJ = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp')
CPT = 3.6
CLASSES = ('q', 'read', 'search', 'other')
SETUP_RE = re.compile(r'docs[\\/]+agents[\\/]|SESSION\d+_PLAN|STATUS\.md|LESSONS\.md|CLAUDE\.md|common\.md|pack|builder_brief', re.I)


def newest_dir():
    s = sorted(glob.glob(os.path.join(PROJ, '*', 'subagents')), key=os.path.getmtime)
    return s[-1] if s else ''


def meta_of(path):
    try:
        return json.load(open(path[:-6] + '.meta.json', encoding='utf-8'))
    except Exception:
        return {}


def text_len(c):
    if isinstance(c, str):
        return len(c)
    if isinstance(c, list):
        return sum(text_len(x.get('text', '') if isinstance(x, dict) else x) for x in c)
    return 0


def descriptor(name, inp):
    if name in ('Read', 'Edit', 'Write'):
        return str(inp.get('file_path', ''))[-80:]
    if name in ('Bash', 'PowerShell'):
        return ' '.join(str(inp.get('command', '')).split())[:80]
    if name == 'Grep':
        return ('grep ' + str(inp.get('pattern', ''))[:40] + ' ' + str(inp.get('path', ''))[-30:])[:80]
    if name == 'Glob':
        return str(inp.get('pattern', ''))[:80]
    return json.dumps(inp, sort_keys=True)[:80]


Q_RE = re.compile(r'q\.py|(^|[\s;&|])q\s+(ours|show|decomp|fact|fn|callers|str|addr|map|field|py|missed|db)\b')
_LEAD = r'(^|[;&]|\bdo\b|\bthen\b|\$\()\s*(\w+=\S*\s+)*(cd\s+\S+\s*(&&|;)\s*)?'    # a command starts a segment (not a pipe filter such as "build | tail")
READ_RE = re.compile(_LEAD + r'(cat|type|head|tail|Get-Content|gc|more|sed\s+-n|nl|wc)(\s|$)')
SEARCH_RE = re.compile(_LEAD + r'(grep|egrep|rg|Select-String|sls|findstr|ag|git\s+(-C\s+\S+\s+)?(grep|log\s+(\S+\s+)*-[SG]))(\s|$)')


def call_class(name, inp):
    """q | read | search | other (by tool name, and for Bash / PowerShell by the command text; the text is never stored)."""
    if name == 'Read':
        return 'read'
    if name == 'Grep':
        return 'search'
    if name in ('Bash', 'PowerShell'):
        cmd = str(inp.get('command', ''))
        if Q_RE.search(cmd):
            return 'q'
        if SEARCH_RE.search(cmd):
            return 'search'
        if READ_RE.search(cmd):
            return 'read'
    return 'other'


def ts_of(r):
    """Record timestamp (ISO, Z) -> seconds, or None."""
    try:
        return datetime.datetime.fromisoformat(r['timestamp'].replace('Z', '+00:00')).timestamp()
    except Exception:
        return None


def load(path):
    turns, seen_msg = [], {}
    calls, results = {}, {}
    first_user = 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            t = r.get('type')
            m = r.get('message') or {}
            if t == 'assistant':
                mid = m.get('id') or r.get('requestId') or r.get('uuid')
                u = m.get('usage') or {}
                if mid not in seen_msg:
                    seen_msg[mid] = len(turns)
                    turns.append({'in': u.get('input_tokens', 0) or 0, 'cr': u.get('cache_read_input_tokens', 0) or 0,
                                  'cc': u.get('cache_creation_input_tokens', 0) or 0, 'out': u.get('output_tokens', 0) or 0})
                else:
                    tt = turns[seen_msg[mid]]
                    tt['out'] = max(tt['out'], u.get('output_tokens', 0) or 0)
                ti = seen_msg[mid]
                if isinstance(m.get('content'), list):
                    for b in m['content']:
                        if isinstance(b, dict) and b.get('type') == 'tool_use' and b.get('id') not in calls:
                            inp = b.get('input') or {}
                            calls[b['id']] = (b.get('name'), descriptor(b.get('name'), inp), ti,
                                              (b.get('name'), json.dumps(inp, sort_keys=True)), call_class(b.get('name'), inp), ts_of(r))
            elif t == 'user':
                c = m.get('content')
                if isinstance(c, str):
                    if not first_user:
                        first_user = len(c)
                elif isinstance(c, list):
                    for b in c:
                        if isinstance(b, dict) and b.get('type') == 'tool_result':
                            results[b.get('tool_use_id')] = (text_len(b.get('content')), len(turns), ts_of(r))
                        elif isinstance(b, dict) and b.get('type') == 'text' and not first_user:
                            first_user = len(b.get('text', ''))
    return turns, results, calls, first_user


def analyse(path):
    turns, results, calls, first_user = load(path)
    n = len(turns)
    ctx = [t['in'] + t['cr'] + t['cc'] for t in turns]
    A = {'turns': n, 'in': sum(t['in'] for t in turns), 'cr': sum(t['cr'] for t in turns), 'cc': sum(t['cc'] for t in turns),
         'out': sum(t['out'] for t in turns), 'ctx': ctx, 'brief_chars': first_user}
    A['input_total'] = A['in'] + A['cr'] + A['cc']
    big = []
    setup_reread = 0
    for tid, (chars, arrive, t_end) in results.items():
        name, desc, ci, key, kc, t_start = calls.get(tid, ('?', '?', 0, None, 'other', None))
        tok = int(chars / CPT)
        later = max(0, n - arrive)
        wall = (t_end - t_start) if (t_end and t_start and t_end >= t_start) else None
        big.append({'wall': wall, 'kc': kc, 'ci': ci, 'tool': name, 'desc': desc, 'chars': chars, 'tok': tok, 'turn': arrive, 'later': later, 'cost': tok * later})
        if SETUP_RE.search(desc) and name in ('Read', 'Bash', 'PowerShell'):
            setup_reread += tok * later
    big.sort(key=lambda x: -x['cost'])
    A['big'] = big
    A['results_tok'] = sum(b['tok'] for b in big)
    A['results_cost'] = sum(b['cost'] for b in big)
    cls = collections.defaultdict(lambda: [0, 0, 0])
    for b in big:
        k = b['tool']
        if k in ('Bash', 'PowerShell'):
            d = b['desc']
            k = 'shell:q' if re.search(r'q\.py', d) else 'shell:git' if d.startswith('git') else 'shell:other'
        c = cls[k]
        c[0] += 1
        c[1] += b['tok']
        c[2] += b['cost']
    A['classes'] = dict(cls)
    kcs = {k: [0, 0, 0] for k in CLASSES}
    for v in calls.values():
        kcs[v[4]][0] += 1
    for b in big:
        c = kcs[b['kc']]
        c[1] += b['tok']
        c[2] += b['cost']
    A['kclasses'] = kcs
    A['walls'] = {k: [b['wall'] for b in big if b['kc'] == k and b['wall'] is not None] for k in CLASSES}
    rep = collections.Counter(v[3] for v in calls.values() if v[3])
    A['repeats'] = [(k[0], k[1][:70], c) for k, c in rep.items() if c > 1 and k[0] not in ('Edit', 'Write')]
    c0 = ctx[0] if ctx else 0
    A['initial_ctx'] = c0
    A['handbacks'] = [v[2] for v in calls.values() if v[0] == 'SubagentHandback']
    A['units'] = A['in'] + 0.1 * A['cr'] + 1.25 * A['cc'] + 5.0 * A['out']      # price-weighted input-token equivalents (cache read 0.1x, write 1.25x, output 5x)
    A['growth'] = (ctx[-1] - ctx[0]) if ctx else 0
    A['setup_cost'] = c0 * max(n - 1, 0) + setup_reread
    A['setup_cost_pct'] = 100.0 * A['setup_cost'] / max(A['input_total'], 1)
    return A


def classes_report(rows):
    """Per-agent q / read / search / other split (calls, result tokens, carried cost) and a cross-agent total."""
    tot = {k: [0, 0, 0] for k in CLASSES}
    print('%-46s %s' % ('agent', ''.join('| %-7s calls   tok      carried ' % k for k in CLASSES)))
    for nm, A in rows:
        print('%-46s %s' % (nm, ''.join('| %5d %8s %12s ' % (A['kclasses'][k][0], f"{A['kclasses'][k][1]:,}", f"{A['kclasses'][k][2]:,}") for k in CLASSES)))
        for k in CLASSES:
            for i in range(3):
                tot[k][i] += A['kclasses'][k][i]
    nc = sum(v[0] for v in tot.values()) or 1
    nt = sum(v[1] for v in tot.values()) or 1
    ncost = sum(v[2] for v in tot.values()) or 1
    print('\nCROSS-AGENT TOTAL over %d agents (class: calls, result tok, carried token-reads; shares):' % len(rows))
    for k in CLASSES:
        c, t, cost = tot[k]
        print('   %-7s %6d (%4.1f%%) %11s tok (%4.1f%%) %15s carried (%4.1f%%)' % (k, c, 100.0 * c / nc, f"{t:,}", 100.0 * t / nt, f"{cost:,}", 100.0 * cost / ncost))
    walls = {k: [] for k in CLASSES}
    for nm, A in rows:
        for k in CLASSES:
            walls[k] += A['walls'][k]
    print('\nTOKENS vs WALL TIME (call duration = tool_use timestamp -> tool_result timestamp; parallel calls overlap, so totals overstate elapsed time):')
    print('   %-7s %6s %11s %13s %10s %10s %10s' % ('class', 'calls', 'result tok', 'carried', 'wall tot s', 'median s', 'mean s'))
    for k in CLASSES:
        w = walls[k]
        print('   %-7s %6d %11s %13s %10.0f %10.1f %10.1f' % (k, tot[k][0], f"{tot[k][1]:,}", f"{tot[k][2]:,}", sum(w), statistics.median(w) if w else 0, (sum(w) / len(w)) if w else 0))
    return tot


def curve(ctx):
    if not ctx:
        return '-'
    return 'start %dk  median %dk  max %dk  end %dk' % (ctx[0] // 1000, statistics.median(ctx) // 1000, max(ctx) // 1000, ctx[-1] // 1000)


def report(name, A, top):
    print('=== %s ===' % name)
    print('turns %d | input %s (cache read %s, cache creation %s, uncached %s) | output %s' % (
        A['turns'], f"{A['input_total']:,}", f"{A['cr']:,}", f"{A['cc']:,}", f"{A['in']:,}", f"{A['out']:,}"))
    print('context per turn: ' + curve(A['ctx']))
    print('first user message %d chars; initial context %s tok; brief+setup share of all input tokens ~%.0f %%' % (
        A['brief_chars'], f"{A['initial_ctx']:,}", A['setup_cost_pct']))
    print('price-weighted units %s (cache read 0.1x, creation 1.25x, output 5x); SubagentHandback at turns %s' % (f"{int(A['units']):,}", A['handbacks']))
    print('context growth start->end %s tok vs estimated results %s + output %s (the estimate runs low: x%.1f)' % (
        f"{A['growth']:,}", f"{A['results_tok']:,}", f"{A['out']:,}", A['growth'] / max(A['results_tok'] + A['out'], 1)))
    print('tool results ~%s tok total, cumulative re-read cost ~%s token-reads' % (f"{A['results_tok']:,}", f"{A['results_cost']:,}"))
    print('by tool class (count, tok, re-read cost):')
    for k, (c, t, cost) in sorted(A['classes'].items(), key=lambda x: -x[1][2]):
        print('   %-12s %4d %9s %12s' % (k, c, f"{t:,}", f"{cost:,}"))
    print('top %d results by re-read cost (tool | descriptor | tok | arrived turn | later turns | cost):' % top)
    for b in A['big'][:top]:
        print('   %-10s %-80s %8s  t%-3d x%-3d %12s' % (b['tool'], b['desc'], f"{b['tok']:,}", b['turn'], b['later'], f"{b['cost']:,}"))
    rp = sorted(A['repeats'], key=lambda x: -x[2])[:8]
    print('repeated identical calls: %d distinct%s' % (len(A['repeats']), ''.join('\n   x%d %s %s' % (c, t, d) for t, d, c in rp)))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('selectors', nargs='*')
    ap.add_argument('--dir', default='')
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--compare', action='store_true')
    ap.add_argument('--detail', action='store_true')
    ap.add_argument('--classes', action='store_true', help='per-agent q / read / search / other split + cross-agent total')
    ap.add_argument('--finished', action='store_true', help='select every agent whose meta says finished true (selectors then optional)')
    ap.add_argument('--misses', type=int, default=0, help='with --classes: list the N raw read/search calls with the biggest carried cost')
    a = ap.parse_args()
    d = a.dir or newest_dir()
    files = sorted(glob.glob(os.path.join(d, '*.jsonl')))
    sel = []
    for s in a.selectors:
        for f in files:
            aid = os.path.basename(f)[6:-6]
            desc = meta_of(f).get('description', '') or ''
            if (aid.startswith(s) or s.lower() in desc.lower()) and f not in sel:
                sel.append(f)
    if a.finished:
        sel = [f for f in files if meta_of(f).get('finished') is True]
    if not sel:
        print('no agent matched', file=sys.stderr)
        return 1
    rows = []
    for f in sel:
        A = analyse(f)
        nm = '%s (%s)' % (os.path.basename(f)[6:-6][:12], (meta_of(f).get('description') or '')[:40])
        rows.append((nm, A))
        if a.classes:
            continue
        if not a.compare or a.detail:
            report(nm, A, a.top)
    if a.classes:
        classes_report(rows)
        if a.misses:
            allm = [(b['cost'], b['tok'], b['kc'], b['desc'], nm.split(' ')[0]) for nm, A in rows for b in A['big'] if b['kc'] in ('read', 'search')]
            allm.sort(reverse=True)
            print('\ntop %d raw read / search results by carried cost (cost | tok | class | agent | descriptor):' % a.misses)
            for cost, tok, kc, desc, ag in allm[:a.misses]:
                print('   %12s %7s %-6s %-12s %s' % (f"{cost:,}", f"{tok:,}", kc, ag, desc))
        return 0
    if a.compare or len(rows) > 1:
        print('%-46s %5s %11s %11s %9s %8s %7s %7s %6s %11s' % ('agent', 'turns', 'cache read', 'cache crt', 'uncached', 'output', 'ctxmax', 'ctxend', 'setup%', 'units'))
        for nm, A in rows:
            print('%-46s %5d %11s %11s %9s %8s %6dk %6dk %5.0f%% %11s' % (nm, A['turns'], f"{A['cr']:,}", f"{A['cc']:,}", f"{A['in']:,}", f"{A['out']:,}",
                  max(A['ctx'] or [0]) // 1000, (A['ctx'][-1] if A['ctx'] else 0) // 1000, A['setup_cost_pct'], f"{int(A['units']):,}"))
    return 0


if __name__ == '__main__':
    sys.exit(main())

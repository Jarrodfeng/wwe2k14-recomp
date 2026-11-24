#!/usr/bin/env python3
r"""Where do the SECONDS of one agent go? (S110 TURNTIME.) tool_calls.py counts calls and replies; this reads the message timestamps of ONE
agent's transcript and splits its wall time per turn.

  TURN        one assistant message (the records that share a message.id; each block is a record stamped when that block finished).
  MODEL TIME  the last tool result / user message before the turn -> the turn's last block: thinking + writing the reply (the model plus
              whatever the client does between getting the result and the reply; hidden reasoning is in it).
  TOOL TIME   the turn's last block -> the arrival of its LAST tool result. Calls of one reply run in parallel: the batch costs the slowest
              call (the per-kind table below uses each call's own result - end of reply, which is why its total can exceed the batch total).
  IDLE        wall - model - tool: the agent stopped between rounds (a resume) or a result queued; not model, not tool.
Usage fields (message.usage): input_tokens + cache_read + cache_creation = the input side of the turn; output_tokens (thinking included,
thinking_tokens is in output_tokens_details). A turn without usage is left out of the token rows.
Reuse: tool_calls.py (the session folder, meta_of, label = the agent's name) and lane_time.py (cmd_class: the tool kind; read_only: a
lookup) by import; the transcript reader is agent_time_audit.load / ts, as in lane_time.

  py -I tools\harness\turn_time.py --agent NAME_OR_ID [--session ID] [--csv PATH] [--top 8]
  py -I tools\harness\turn_time.py --compare A,B,C [--session ID]
NAME may carry @probe (no Edit call) or @builder (has Edit calls) when two agents share a name (WHITEFACE: the probe and the builder).
Output holds only tool names and 60-character argument summaries, never message text.
"""
import argparse
import csv
import glob
import json
import math
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import agent_time_audit as ata  # noqa: E402
import lane_time  # noqa: E402
import tool_calls  # noqa: E402

LOOKUP_TOOLS = {'Read', 'Grep', 'Glob'}
IDLE_GAP = 600.0        # a model time over this is the agent stopped between rounds, not thinking: counted as idle (reported)


def short(s, n=60):
    s = re.sub(r'\s+', ' ', str(s or '')).strip()
    return s if len(s) <= n else s[:n - 1] + '~'


def summarize_call(name, inp):
    """<= 60 characters of the call's argument: the q command, the file path (its tail), the first words of a Write"""
    inp = inp or {}
    if name in ('Bash', 'PowerShell'):
        c = inp.get('command') or ''
        m = re.search(r'q[\\/]q\.py\s+(.*)', c, re.S)
        return short('q ' + m.group(1) if m else c)
    p = inp.get('file_path') or inp.get('path') or ''
    if name == 'Write':
        return short((p[-30:] if len(p) > 30 else p) + ' | ' + (inp.get('content') or '')[:40])
    if name in ('Read', 'Edit', 'NotebookEdit'):
        return short(p[-60:])
    if name in ('Grep', 'Glob'):
        return short(inp.get('pattern') or '')
    if name == 'SubagentHandback':
        return 'hand-back'
    return short(json.dumps(inp))


def parse(path, tool_agg=max):
    """-> dict(turns, first, last). A turn: n, end (s from the first record), model, tool, calls [(id, name, inp)], call_dur {id: s},
    usage fields, hand (it calls SubagentHandback). tool_agg combines the calls of one reply (max = they run in parallel; the selftest's
    planted twin passes sum)"""
    recs = ata.load(path)
    msgs, order, res_ts = {}, [], {}
    last_user = None
    first = None
    for r in recs:
        t = ata.ts(r)
        if t is None:
            continue
        first = first or t
        typ = r.get('type')
        m = r.get('message') or {}
        if typ == 'user':
            c = m.get('content')
            if isinstance(c, list):
                for b in c:
                    if isinstance(b, dict) and b.get('type') == 'tool_result':
                        res_ts.setdefault(b.get('tool_use_id'), t)
            last_user = t
        elif typ == 'assistant' and m.get('id'):
            mid = m['id']
            if mid not in msgs:
                msgs[mid] = dict(prev=last_user or first, end=t, calls=[], usage={})
                order.append(mid)
            g = msgs[mid]
            g['end'] = max(g['end'], t)
            for b in m.get('content') or []:
                if isinstance(b, dict) and b.get('type') == 'tool_use' and all(b['id'] != c[0] for c in g['calls']):
                    g['calls'].append((b['id'], b.get('name'), b.get('input') or {}))
            u = m.get('usage')
            if isinstance(u, dict) and (u.get('output_tokens') or 0) >= (g['usage'].get('output_tokens') or 0):
                g['usage'] = u
    all_ts = [t for t in (ata.ts(r) for r in recs) if t]
    last = max(all_ts) if all_ts else first
    turns = []
    for i, mid in enumerate(order, 1):
        g = msgs[mid]
        done = [res_ts[c[0]] for c in g['calls'] if c[0] in res_ts]
        durs = {c[0]: max(0.0, (res_ts[c[0]] - g['end']).total_seconds()) for c in g['calls'] if c[0] in res_ts}
        u = g['usage']
        has = bool(u) and 'output_tokens' in u
        cr, cc = u.get('cache_read_input_tokens') or 0, u.get('cache_creation_input_tokens') or 0
        turns.append(dict(
            n=i, start=(g['prev'] - first).total_seconds(), end=(g['end'] - first).total_seconds(),
            model=max(0.0, (g['end'] - g['prev']).total_seconds()),
            tool=(max(0.0, tool_agg(durs.values())) if durs else 0.0), calls=g['calls'], call_dur=durs,
            has_usage=has, out=u.get('output_tokens') or 0, think=(u.get('output_tokens_details') or {}).get('thinking_tokens') or 0,
            inp=(u.get('input_tokens') or 0), cr=cr, cc=cc, ins=(u.get('input_tokens') or 0) + cr + cc,
            hand=any(c[1] == 'SubagentHandback' for c in g['calls'])))
    return dict(turns=turns, wall=(last - first).total_seconds() if first else 0.0)


def category(t):
    """what the turn was doing, by its calls (first rule that fits)"""
    cs = t['calls']
    if t['hand'] or (not cs and t is t.get('_last')):
        return 'final hand-back'
    if not cs:
        return 'text only (no call)'
    for _id, name, inp in cs:
        if name in ('Write', 'Edit') and re.search(r'docs[\\/]+research', inp.get('file_path') or ''):
            return 'wrote the note (docs\\research)'
    if all(lane_time.read_only(name, inp) for _id, name, inp in cs):
        return 'lookups only (q / Read / Grep / Glob)'
    if any(name in ('Bash', 'PowerShell') for _id, name, inp in cs):
        return 'ran scripts (Bash / PowerShell)'
    return 'edited / wrote other files'


def kind_of(name, inp):
    return lane_time.cmd_class(name, inp)


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, math.ceil(p * len(v)) - 1))] if v else 0.0


def stats(v):
    return (statistics.median(v), pct(v, 0.9), max(v)) if v else (0.0, 0.0, 0.0)


def analyse(path, tool_agg=max):
    d = parse(path, tool_agg)
    turns, wall = d['turns'], d['wall']
    if turns:
        turns[-1]['_last'] = turns[-1]
    for t in turns:
        t['cat'] = category(t)
        t['tps'] = (t['out'] / t['model']) if t['has_usage'] and t['model'] > 0 else None
    model = sum(t['model'] for t in turns)
    tool = sum(t['tool'] for t in turns)
    batches = [t['tool'] for t in turns if t['call_dur']]
    use = [t for t in turns if t['has_usage']]
    out = sum(t['out'] for t in use)
    use_model = sum(t['model'] for t in use)
    first_tool = next((t for t in turns if t['calls']), None)
    hand = next((t for t in reversed(turns) if t['hand']), turns[-1] if turns else None)
    by_cat, by_kind = {}, {}
    for t in turns:
        by_cat.setdefault(t['cat'], [0, 0.0, 0.0])
        by_cat[t['cat']][0] += 1
        by_cat[t['cat']][1] += t['model']
        by_cat[t['cat']][2] += t['tool']
        for cid, name, inp in t['calls']:
            if cid in t['call_dur']:
                k = by_kind.setdefault(kind_of(name, inp), [0, 0.0])
                k[0] += 1
                k[1] += t['call_dur'][cid]
    return dict(wall=wall, turns=turns, n=len(turns), model=model, tool=tool, idle=max(0.0, wall - model - tool),
                mstat=stats([t['model'] for t in turns]), tstat=stats(batches), nbatch=len(batches), out=out, out_turns=len(use),
                out_model=use_model, tps=(out / use_model if use_model > 0 else None),
                in_first=use[0]['ins'] if use else None, in_mid=use[len(use) // 2]['ins'] if use else None,
                in_last=use[-1]['ins'] if use else None, pre_tool=first_tool['end'] if first_tool else None,
                hand_model=hand['model'] if hand else None, hand_out=hand['out'] if hand and hand['has_usage'] else None,
                by_cat=by_cat, by_kind=by_kind)


def fmt_s(x):
    return '-' if x is None else '%.1f s' % x


def share(x, tot):
    return '%.0f%%' % (100.0 * x / tot) if tot else '-'


def rows(a):
    """the summary as [(label, text)]"""
    w = a['wall']
    r = [('wall time', fmt_s(w)), ('turns (assistant messages)', str(a['n'])),
         ('model time total', '%s (%s of wall)' % (fmt_s(a['model']), share(a['model'], w))),
         ('tool time total', '%s (%s of wall)' % (fmt_s(a['tool']), share(a['tool'], w))),
         ('idle (wall - model - tool)', '%s (%s of wall)' % (fmt_s(a['idle']), share(a['idle'], w))),
         ('model time / turn: median, p90, max', '%.1f, %.1f, %.1f s' % a['mstat']),
         ('tool time / batch: median, p90, max', '%.1f, %.1f, %.1f s (%d batches)' % (a['tstat'] + (a['nbatch'],))),
         ('output tokens', '%d over %d turns with usage' % (a['out'], a['out_turns'])),
         ('output tokens / s of model time', '-' if a['tps'] is None else '%.1f' % a['tps']),
         ('input side (input+cache) first / mid / last turn', '-' if a['in_first'] is None else '%d / %d / %d' % (a['in_first'], a['in_mid'], a['in_last'])),
         ('time before the first tool call', fmt_s(a['pre_tool'])),
         ('final hand-back turn: model time (output tokens)', '%s (%s)' % (fmt_s(a['hand_model']), '-' if a['hand_out'] is None else a['hand_out']))]
    return r


def what(t, n=3):
    cs = t['calls']
    if not cs:
        return 'text only'
    parts = ['%s %s' % (name, summarize_call(name, inp)) for _id, name, inp in cs[:n]]
    return '; '.join(parts) + ('; +%d more' % (len(cs) - n) if len(cs) > n else '')


def print_single(name, a, top):
    print('== %s ==' % name)
    for k, v in rows(a):
        print('  %-52s %s' % (k, v))
    print('\nthe %d slowest turns by model time (turn, model s, output tokens, what it did):' % top)
    for t in sorted(a['turns'], key=lambda t: -t['model'])[:top]:
        print('  #%-4d %7.1f s %6s tok  [%s] %s' % (t['n'], t['model'], t['out'] if t['has_usage'] else '-', t['cat'].split(' (')[0], what(t)))
    print('\nthe %d slowest tool batches (turn, tool s, the calls):' % top)
    for t in sorted([t for t in a['turns'] if t['call_dur']], key=lambda t: -t['tool'])[:top]:
        print('  #%-4d %7.1f s  %s' % (t['n'], t['tool'], what(t)))
    print('\nmodel time by what the turn was doing (turns, model s, share of model time, tool s of those turns):')
    for k, (n, m, tl) in sorted(a['by_cat'].items(), key=lambda kv: -kv[1][1]):
        print('  %-40s %4d turns %8.1f s %5s  tool %7.1f s' % (k, n, m, share(m, a['model']), tl))
    print('\ntool time by tool kind (calls, sum of each call\'s own wait, share of the sum):')
    tot = sum(v[1] for v in a['by_kind'].values())
    for k, (n, s) in sorted(a['by_kind'].items(), key=lambda kv: -kv[1][1])[:14]:
        print('  %-28s %4d calls %8.1f s %5s' % (k, n, s, share(s, tot)))


def print_compare(names, results):
    labels = [k for k, _ in rows(results[0])]
    cols = [dict(rows(a)) for a in results]
    wid = max(len(c) for c in names + ['model 99999.9 s (100% of wall)'])
    print('%-52s ' % '' + ' | '.join('%-*s' % (wid, n) for n in names))
    for k in labels:
        print('%-52s ' % k + ' | '.join('%-*s' % (wid, c[k]) for c in cols))
    cats = sorted({k for a in results for k in a['by_cat']})
    print('\nmodel time by what the turn was doing (seconds, share of that agent\'s model time):')
    for k in cats:
        print('%-52s ' % k + ' | '.join('%-*s' % (wid, '%.0f s (%s)' % (a['by_cat'].get(k, [0, 0, 0])[1], share(a['by_cat'].get(k, [0, 0, 0])[1], a['model']))) for a in results))


def write_csv(path, a):
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['turn', 'start_s', 'end_s', 'model_s', 'tool_s', 'out_tokens', 'thinking_tokens', 'input_tokens', 'cache_read', 'cache_creation',
                    'input_side', 'out_tok_per_s', 'category', 'calls'])
        for t in a['turns']:
            w.writerow([t['n'], '%.1f' % t['start'], '%.1f' % t['end'], '%.1f' % t['model'], '%.1f' % t['tool'],
                        t['out'] if t['has_usage'] else '', t['think'] if t['has_usage'] else '', t['inp'] if t['has_usage'] else '',
                        t['cr'] if t['has_usage'] else '', t['cc'] if t['has_usage'] else '', t['ins'] if t['has_usage'] else '',
                        '' if t['tps'] is None else '%.1f' % t['tps'], t['cat'], what(t, 8)])


# ---------------------------------------------------------------- finding the agent
def session_folder(sess):
    if sess:
        return os.path.join(tool_calls.ROOT, sess, 'subagents')
    cands = sorted(glob.glob(os.path.join(tool_calls.ROOT, '*', 'subagents')), key=os.path.getmtime)
    return cands[-1] if cands else None


def agent_name(path):
    desc, atype = tool_calls.meta_of(path)
    text = ''
    for r in ata.load(path)[:3]:
        c = (r.get('message') or {}).get('content')
        if r.get('type') == 'user' and isinstance(c, str):
            text = c
            break
    return tool_calls.label(text, desc, atype)


def edit_count(path):
    return sum(1 for line in open(path, encoding='utf-8', errors='replace') if '"name":"Edit"' in line and '"type":"tool_use"' in line)


def resolve(spec, folder):
    """NAME[@probe|@builder] or an agent id (prefix) -> the transcript path"""
    name, _, role = spec.partition('@')
    files = sorted(glob.glob(os.path.join(folder, 'agent-*.jsonl')), key=os.path.getmtime)
    hit = [f for f in files if os.path.basename(f)[6:-6].startswith(name)]
    if not hit:
        hit = [f for f in files if agent_name(f) == name]
    if role and len(hit) > 1:
        hit = [f for f in hit if (edit_count(f) == 0) == (role == 'probe')]
    if not hit:
        raise SystemExit('no agent %r in %s' % (spec, folder))
    if len(hit) > 1:
        raise SystemExit('%r is ambiguous (%s): add @probe (no Edit call) or @builder, or give the id' % (
            spec, ', '.join('%s %d Edit' % (os.path.basename(f)[6:-6], edit_count(f)) for f in hit)))
    return hit[0]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--agent')
    ap.add_argument('--compare')
    ap.add_argument('--session')
    ap.add_argument('--csv')
    ap.add_argument('--top', type=int, default=8)
    a = ap.parse_args(argv)
    folder = session_folder(a.session)
    if not folder or not (a.agent or a.compare):
        ap.error('--agent NAME_OR_ID or --compare A,B,C (and a session with subagents)')
    if a.compare:
        specs = [s.strip() for s in a.compare.split(',') if s.strip()]
        results = [analyse(resolve(s, folder)) for s in specs]
        print_compare(specs, results)
        return 0
    path = resolve(a.agent, folder)
    res = analyse(path)
    print_single('%s (%s)' % (agent_name(path), os.path.basename(path)[6:-6]), res, a.top)
    if a.csv:
        write_csv(a.csv, res)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

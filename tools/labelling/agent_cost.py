"""Session 68: token use of Agent-tool subagents, from their transcripts (prints numbers only).

    py tools\\labelling\\agent_cost.py AGENT_ID [AGENT_ID ...] [--session DIR] [--per N]

Sums the usage of every assistant message (once per message id) in
%USERPROFILE%\\.claude\\projects\\<project>\\<session>\\subagents\\agent-<id>.jsonl: fresh input, cache writes,
cache reads, output; plus turns and tool calls by tool name. --session defaults to the newest session folder that
holds the agent. --per N also prints the totals divided by N (e.g. functions labelled).
"""
import argparse
import collections
import glob
import json
import os
import sys

PROJECTS = os.path.join(os.path.expanduser('~'), '.claude', 'projects', 'C--Users-Jarrod-wwe2k14-recomp')


def find(agent, session):
    pat = os.path.join(session or os.path.join(PROJECTS, '*'), 'subagents', f'agent-{agent}.jsonl')
    hits = sorted(glob.glob(pat), key=os.path.getmtime)
    return hits[-1] if hits else None


def usage(path):
    seen, tot, tools, turns = set(), collections.Counter(), collections.Counter(), 0
    with open(path, encoding='utf-8') as f:
        for line in f:
            try:
                o = json.loads(line)
            except ValueError:
                continue
            m = o.get('message') or {}
            if m.get('role') != 'assistant' or not isinstance(m.get('usage'), dict):
                continue
            mid = m.get('id') or o.get('uuid')
            if mid in seen:
                continue
            seen.add(mid)
            turns += 1
            for k in ('input_tokens', 'cache_creation_input_tokens', 'cache_read_input_tokens', 'output_tokens'):
                tot[k] += m['usage'].get(k) or 0
            for c in m.get('content') or []:
                if isinstance(c, dict) and c.get('type') == 'tool_use':
                    tools[c.get('name')] += 1
    return turns, tot, tools


def save_json(path, out):
    """The last JSON array / object in the agent's reply text (agents told to answer in text and hand back 'done N';
    the answer stays out of the main session's context) -> out. Returns the number of items."""
    texts = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            try:
                m = json.loads(line).get('message') or {}
            except ValueError:
                continue
            if m.get('role') == 'assistant':
                texts += [c.get('text', '') for c in m.get('content') or [] if isinstance(c, dict)
                          and c.get('type') == 'text']
    dec = json.JSONDecoder()
    for t in reversed(texts):
        t = t.strip()
        if t.startswith('```'):
            t = t.split('\n', 1)[1].rsplit('```', 1)[0]
        for i, ch in enumerate(t):
            if ch in '[{':
                try:
                    val = dec.raw_decode(t[i:])[0]
                except ValueError:
                    continue
                with open(out, 'w', encoding='utf-8') as f:
                    json.dump(val, f, indent=1, ensure_ascii=False)
                return len(val) if isinstance(val, list) else len(val.get('functions', val))
    raise SystemExit(f'no JSON in the reply text of {path}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('agents', nargs='+')
    ap.add_argument('--session')
    ap.add_argument('--per', type=int, default=0)
    ap.add_argument('--save-json', help='(one agent) write the JSON from its reply text here')
    ap.add_argument('--run', action='store_true',
                    help='agents = workflow transcript dirs: sum every agent-*.jsonl in each, flag agents over 3 turns')
    args = ap.parse_args()
    if args.run:
        grand, agents, extra, tool_tot = collections.Counter(), 0, [], collections.Counter()
        for d in args.agents:
            for p in sorted(glob.glob(os.path.join(d, '**', 'agent-*.jsonl'), recursive=True)):
                turns, tot, tools = usage(p)
                if not turns:
                    continue
                agents += 1
                grand.update(tot)
                tool_tot.update(tools)
                if turns > 3:
                    extra.append(f'{os.path.basename(p)} {turns} turns {dict(tools)}')
        inp = sum(grand[k] for k in ('input_tokens', 'cache_creation_input_tokens', 'cache_read_input_tokens'))
        print(f'{agents} agents; input {inp:,} (cache write {grand["cache_creation_input_tokens"]:,}, cache read '
              f'{grand["cache_read_input_tokens"]:,}, fresh {grand["input_tokens"]:,}), output {grand["output_tokens"]:,}; '
              f'tools {dict(tool_tot)}')
        if args.per:
            print(f'per item ({args.per}): input {inp // args.per:,}, output {grand["output_tokens"] // args.per:,}')
        print(f'agents over 3 turns: {len(extra)}')
        for x in extra[:10]:
            print(' ', x)
        return
    if args.save_json:
        n = save_json(find(args.agents[0], args.session), args.save_json)
        print(f'{n} items -> {args.save_json}')
    grand = collections.Counter()
    for a in args.agents:
        p = find(a, args.session)
        if not p:
            print(f'{a}: no transcript found')
            continue
        turns, tot, tools = usage(p)
        grand.update(tot)
        inp = tot['input_tokens'] + tot['cache_creation_input_tokens'] + tot['cache_read_input_tokens']
        print(f'{a}: {turns} turns, tools {dict(tools)}; input {inp:,} (fresh {tot["input_tokens"]:,}, cache write '
              f'{tot["cache_creation_input_tokens"]:,}, cache read {tot["cache_read_input_tokens"]:,}), output '
              f'{tot["output_tokens"]:,}')
    inp = grand['input_tokens'] + grand['cache_creation_input_tokens'] + grand['cache_read_input_tokens']
    print(f'total: input {inp:,}, output {grand["output_tokens"]:,}')
    if args.per:
        print(f'per item ({args.per}): input {inp // args.per:,}, output {grand["output_tokens"] // args.per:,}')


if __name__ == '__main__':
    sys.exit(main())

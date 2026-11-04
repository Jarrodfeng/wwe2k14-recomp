r"""Where a subagent's (builder's) time went (Session 89; promoted from Session 87's agent_timeline.py): span, tool
calls, minutes waiting on tools vs model time between calls, output tokens, per tool, long calls, per 5 minutes.
Prints only aggregates and short labels (descriptions / paths / patterns), never message contents.

    py tools\harness\agent_timeline.py TRANSCRIPT.jsonl [--long 45]

TRANSCRIPT = a subagent's .jsonl transcript (under %USERPROFILE%\.claude\projects\<project>\<session>\subagents\).
Times print in this PC's local time. Session 87's step 3 builder: 39 min, 219 calls (180 small greps), ~29 min model
time - the reason for tools\q\qmap.py and docs\agents\builder_brief.md.
"""
import argparse
import json
import re
from collections import defaultdict
from datetime import datetime


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('transcript'); ap.add_argument('--long', type=float, default=45.0, help='list calls longer than S seconds')
    a = ap.parse_args()
    recs = []
    with open(a.transcript, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                recs.append(json.loads(line))
            except ValueError:
                pass

    def ts(r):
        t = r.get('timestamp')
        return datetime.fromisoformat(t.replace('Z', '+00:00')).astimezone() if t else None

    calls, order, first, last, out_tokens = {}, [], None, None, 0
    for r in recs:
        t = ts(r)
        if t:
            first = first or t; last = t
        m = r.get('message') or {}
        out_tokens += (m.get('usage') or {}).get('output_tokens', 0) or 0
        c = m.get('content')
        if not isinstance(c, list):
            continue
        for x in c:
            if x.get('type') == 'tool_use':
                inp = x.get('input') or {}
                label = inp.get('description') or inp.get('file_path') or inp.get('pattern') or inp.get('command') or ''
                calls[x['id']] = [t, x.get('name'), re.sub(r'\s+', ' ', str(label))[:70], None]
                order.append(x['id'])
            elif x.get('type') == 'tool_result':
                k = x.get('tool_use_id')
                if k in calls and calls[k][3] is None:
                    calls[k][3] = t
    if not first:
        raise SystemExit('agent_timeline: no timestamps in the transcript')
    by_tool, rows = defaultdict(lambda: [0, 0.0]), []
    tool_time = model_time = 0.0
    prev_end = first
    for k in order:
        s, name, label, e = calls[k]
        if s and prev_end and s > prev_end:
            model_time += (s - prev_end).total_seconds()
        d = (e - s).total_seconds() if (s and e) else 0.0
        tool_time += d
        by_tool[name][0] += 1; by_tool[name][1] += d
        rows.append((s, d, name, label))
        if e:
            prev_end = max(prev_end, e) if prev_end else e
    total = (last - first).total_seconds()
    print(f'span {first:%H:%M:%S} - {last:%H:%M:%S} = {total / 60:.1f} min; tool calls {len(order)}; waiting on tools '
          f'{tool_time / 60:.1f} min; model between calls {model_time / 60:.1f} min; output tokens {out_tokens}')
    print('by tool (calls, minutes):')
    for name, (n, d) in sorted(by_tool.items(), key=lambda x: -x[1][1]):
        print(f'  {str(name):12s} {n:4d}  {d / 60:6.1f}')
    print(f'long tool calls (> {a.long:g} s):')
    for s, d, name, label in rows:
        if d > a.long:
            print(f'  {s:%H:%M:%S}  {d / 60:5.1f} min  {str(name):8s} {label}')
    print('per 5 minutes (tool minutes / calls):')
    b = defaultdict(lambda: [0.0, 0])
    for s, d, name, label in rows:
        if s:
            key = s.replace(second=0, microsecond=0)
            key = key.replace(minute=key.minute - key.minute % 5)
            b[key][0] += d; b[key][1] += 1
    for key in sorted(b):
        print(f'  {key:%H:%M}  tools {b[key][0] / 60:4.1f} min  calls {b[key][1]:3d}')


if __name__ == '__main__':
    main()

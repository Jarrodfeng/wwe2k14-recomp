"""The longest tool calls of the named subagents (durations + the command's first characters)."""
import json, os, sys, glob, datetime, re
d = sys.argv[1]; since = sys.argv[2]; want = [w.lower() for w in sys.argv[3].split(',')]; top = int(sys.argv[4]) if len(sys.argv) > 4 else 10
def ts(s): return datetime.datetime.fromisoformat(s.replace('Z', '+00:00'))
for f in glob.glob(os.path.join(d, '**', '*.jsonl'), recursive=True):
    lines = []
    for line in open(f, encoding='utf-8', errors='replace'):
        try: o = json.loads(line)
        except Exception: continue
        if o.get('timestamp'): lines.append(o)
    if not lines or lines[-1]['timestamp'] < since: continue
    first = ''
    for o in lines:
        if o.get('type') == 'user':
            c = (o.get('message') or {}).get('content')
            if isinstance(c, str): first = c
            elif isinstance(c, list):
                for b in c:
                    if isinstance(b, dict) and b.get('type') == 'text': first = b.get('text', ''); break
            if first: break
    fl = ' '.join(first.split()).lower()
    hit = [w for w in want if w in fl[:60]]
    if not hit: continue
    pend = {}; calls = []
    for o in lines:
        t = ts(o['timestamp']); m = o.get('message') or {}
        if o.get('type') == 'assistant':
            for b in (m.get('content') or []):
                if isinstance(b, dict) and b.get('type') == 'tool_use':
                    i = b.get('input') or {}
                    desc = i.get('command') or i.get('file_path') or i.get('pattern') or ''
                    pend[b['id']] = (t, b.get('name'), ' '.join(str(desc).split())[:150], bool(i.get('run_in_background')), i.get('timeout'))
        if o.get('type') == 'user':
            c = m.get('content')
            if isinstance(c, list):
                for b in c:
                    if isinstance(b, dict) and b.get('type') == 'tool_result' and b.get('tool_use_id') in pend:
                        t0, n, dsc, bg, to = pend.pop(b['tool_use_id'])
                        calls.append(((t - t0).total_seconds(), t0.strftime('%H:%M:%S'), n, dsc, bg))
    calls.sort(reverse=True)
    tot = sum(c[0] for c in calls)
    print('==', hit[0], '| calls', len(calls), '| tool seconds', round(tot), '| calls > 60 s:', len([c for c in calls if c[0] > 60]), 'holding', round(sum(c[0] for c in calls if c[0] > 60)), 's')
    for c in calls[:top]:
        print('  %6.0f s  %s  %-10s %s%s' % (c[0], c[1], c[2], '[bg] ' if c[4] else '', c[3]))

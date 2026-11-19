"""Per-agent billed usage from the subagent transcripts of one session (read-only; prints numbers only, no transcript text beyond the agent's own description line)."""
import json, os, sys, glob, datetime
d = sys.argv[1]
since = sys.argv[2] if len(sys.argv) > 2 else ''
files = glob.glob(os.path.join(d, '**', '*.jsonl'), recursive=True)
rows = []
for f in files:
    n_turn = 0; inp = cr = cc = out = 0; t0 = t1 = None; tools = 0; model = ''; first_user = ''; peak = 0; seen = set()
    try:
        fh = open(f, encoding='utf-8', errors='replace')
    except OSError:
        continue
    for line in fh:
        try: o = json.loads(line)
        except Exception: continue
        ts = o.get('timestamp')
        if ts:
            t0 = t0 or ts; t1 = ts
        m = o.get('message') or {}
        if o.get('type') == 'user' and not first_user:
            c = m.get('content')
            if isinstance(c, str): first_user = c
            elif isinstance(c, list):
                for b in c:
                    if isinstance(b, dict) and b.get('type') == 'text': first_user = b.get('text', ''); break
        if o.get('type') == 'assistant':
            mid = m.get('id')
            u = m.get('usage') or {}
            for b in (m.get('content') or []):
                if isinstance(b, dict) and b.get('type') == 'tool_use': tools += 1
            if mid in seen: continue        # one API response is written as several lines (one per content block): count its usage once
            seen.add(mid)
            n_turn += 1
            model = m.get('model') or model
            i, r, c2, ot = u.get('input_tokens', 0) or 0, u.get('cache_read_input_tokens', 0) or 0, u.get('cache_creation_input_tokens', 0) or 0, u.get('output_tokens', 0) or 0
            inp += i; cr += r; cc += c2; out += ot
            peak = max(peak, i + r + c2)
    fh.close()
    if not n_turn: continue
    if since and (t1 or '') < since: continue
    rows.append(dict(f=os.path.basename(f), t0=t0, t1=t1, turns=n_turn, tools=tools, inp=inp, cr=cr, cc=cc, out=out, peak=peak, model=model, desc=' '.join(first_user.split())[:70]))
def mins(r):
    try:
        a = datetime.datetime.fromisoformat(r['t0'].replace('Z', '+00:00')); b = datetime.datetime.fromisoformat(r['t1'].replace('Z', '+00:00'))
        return (b - a).total_seconds() / 60
    except Exception: return 0
# price-weighted units: input 1, cache write 1.25, cache read 0.1, output 5 (relative to the model's own input price)
def units(r): return r['inp'] + 1.25 * r['cc'] + 0.1 * r['cr'] + 5 * r['out']
rows.sort(key=lambda r: r['t0'] or '')
print('agents', len(rows))
print('%-16s %-11s %5s %5s %6s %8s %8s %8s %7s %7s %8s  %s' % ('start(UTC)', 'model', 'turns', 'tools', 'min', 'cache_rd', 'cache_wr', 'input', 'output', 'peak', 'units', 'first line'))
tot = dict(cr=0, cc=0, inp=0, out=0, u=0.0)
for r in rows:
    print('%-16s %-11s %5d %5d %6.1f %7.1fM %7.0fk %7.0fk %6.0fk %6.0fk %7.2fM  %s' % ((r['t0'] or '')[5:16], r['model'].replace('claude-', '')[:11], r['turns'], r['tools'], mins(r), r['cr']/1e6, r['cc']/1e3, r['inp']/1e3, r['out']/1e3, r['peak']/1e3, units(r)/1e6, r['desc']))
    tot['cr'] += r['cr']; tot['cc'] += r['cc']; tot['inp'] += r['inp']; tot['out'] += r['out']; tot['u'] += units(r)
print('TOTAL cache_read %.1fM cache_write %.1fM input %.1fM output %.2fM units %.1fM' % (tot['cr']/1e6, tot['cc']/1e6, tot['inp']/1e6, tot['out']/1e6, tot['u']/1e6))
bym = {}
for r in rows:
    k = r['model'].replace('claude-', '')[:11]; a = bym.setdefault(k, [0, 0.0, 0, 0.0]); a[0] += 1; a[1] += units(r); a[2] += r['tools']; a[3] += mins(r)
for k, a in bym.items(): print('  model %-11s agents %3d units %.1fM tools %d agent-min %.0f' % (k, a[0], a[1]/1e6, a[2], a[3]))

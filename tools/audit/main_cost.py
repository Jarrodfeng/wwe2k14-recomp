"""The main session's own billed usage in a time window (numbers only)."""
import json, sys, collections
f, lo, hi = sys.argv[1], sys.argv[2], sys.argv[3]
seen = set(); turns = 0; inp = cr = cc = out = 0; peak = 0; ctx = []; tools = collections.Counter(); model = collections.Counter()
hours = collections.defaultdict(lambda: [0, 0.0, 0])
for line in open(f, encoding='utf-8', errors='replace'):
    try: o = json.loads(line)
    except Exception: continue
    t = o.get('timestamp') or ''
    if not (lo <= t < hi) or o.get('type') != 'assistant' or o.get('isSidechain'): continue
    m = o.get('message') or {}
    for b in (m.get('content') or []):
        if isinstance(b, dict) and b.get('type') == 'tool_use': tools[b.get('name')] += 1
    if m.get('id') in seen: continue
    seen.add(m.get('id')); u = m.get('usage') or {}
    i, r, c, ot = u.get('input_tokens', 0) or 0, u.get('cache_read_input_tokens', 0) or 0, u.get('cache_creation_input_tokens', 0) or 0, u.get('output_tokens', 0) or 0
    turns += 1; inp += i; cr += r; cc += c; out += ot; peak = max(peak, i + r + c); ctx.append(i + r + c); model[m.get('model')] += 1
    h = hours[t[5:13]]; h[0] += 1; h[1] += i + 1.25 * c + 0.1 * r + 5 * ot; h[2] = max(h[2], i + r + c)
units = inp + 1.25 * cc + 0.1 * cr + 5 * out
ctx.sort()
print('turns', turns, 'models', dict(model))
print('cache_read %.1fM cache_write %.2fM input %.2fM output %.2fM -> units %.1fM' % (cr/1e6, cc/1e6, inp/1e6, out/1e6, units/1e6))
print('  share of units: cache read %.0f %%, cache write %.0f %%, output %.0f %%' % (10*cr/units, 125*cc/units, 500*out/units))
if ctx: print('context per turn: median %.0fk, p90 %.0fk, peak %.0fk' % (ctx[len(ctx)//2]/1e3, ctx[9*len(ctx)//10]/1e3, peak/1e3))
print('tool calls:', tools.most_common(12))
for k in sorted(hours): print('  hour(UTC) %s turns %4d units %5.1fM peak ctx %4.0fk' % (k, hours[k][0], hours[k][1]/1e6, hours[k][2]/1e3))

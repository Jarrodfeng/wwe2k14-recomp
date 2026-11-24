"""Where each subagent's wall time and tool calls went (read-only; prints counts and minutes only)."""
import json, os, sys, glob, datetime, re, collections
d = sys.argv[1]; since = sys.argv[2]
def ts(s):
    return datetime.datetime.fromisoformat(s.replace('Z', '+00:00'))
def cat(name, inp):
    if name in ('Read', 'Grep', 'Glob'): return 'read'
    if name in ('Edit', 'Write', 'NotebookEdit'): return 'edit'
    if name in ('Bash', 'PowerShell'):
        c = (inp.get('command') or '').lower()
        if re.search(r'build_\w+\.ps1|build\.ps1|fast_build|ninja|clang|cmake|glslang|spirv|make_spv|cl\.exe|link\.exe', c): return 'build'
        if re.search(r'_check\.py|fview|rplay|rsc_draw|frame_rig|_unit|selftest|_lint|lint\.py|feature_diff|choice_score|choice_predict|_test\.exe|_test\.py|rig\.py|\.exe', c): return 'proof'
        if re.search(r'q\.py|list_function|fields\.py|fold\.py|seq_find|effect_table|select-string|get-content|\bcat\b|\brg\b|grep|findstr|get-childitem|\bls\b|test-path|\bhead\b|\btail\b|\bwc\b|\bsed -n', c): return 'read'
        if re.search(r'\bgit\b', c): return 'git'
        return 'other'
    return 'other'
files = glob.glob(os.path.join(d, '**', '*.jsonl'), recursive=True)
out = []
for f in files:
    lines = []
    for line in open(f, encoding='utf-8', errors='replace'):
        try: o = json.loads(line)
        except Exception: continue
        if o.get('timestamp'): lines.append(o)
    if not lines or lines[-1]['timestamp'] < since: continue
    first = ''
    pend = {}; catn = collections.Counter(); catt = collections.Counter(); think = 0.0; rounds = [[0, 0.0, None, None]]
    last_t = None; seen = set(); edits_files = collections.Counter(); read_files = collections.Counter(); builds_fail = 0
    for o in lines:
        t = ts(o['timestamp']); m = o.get('message') or {}
        if last_t is not None and (t - last_t).total_seconds() > 600:
            rounds.append([0, 0.0, None, None])      # a resume (a fix round) after > 10 min of silence
        r = rounds[-1]; r[2] = r[2] or t; r[3] = t
        if o.get('type') == 'user':
            c = m.get('content')
            if not first:
                if isinstance(c, str): first = c
                elif isinstance(c, list):
                    for b in c:
                        if isinstance(b, dict) and b.get('type') == 'text': first = b.get('text', ''); break
            if isinstance(c, list):
                for b in c:
                    if isinstance(b, dict) and b.get('type') == 'tool_result' and b.get('tool_use_id') in pend:
                        k, t0 = pend.pop(b['tool_use_id']); catt[k] += (t - t0).total_seconds()
        if o.get('type') == 'assistant':
            mid = m.get('id'); u = m.get('usage') or {}
            if mid not in seen:
                seen.add(mid)
                r[0] += 1
                r[1] += (u.get('input_tokens', 0) or 0) + 1.25 * (u.get('cache_creation_input_tokens', 0) or 0) + 0.1 * (u.get('cache_read_input_tokens', 0) or 0) + 5 * (u.get('output_tokens', 0) or 0)
            for b in (m.get('content') or []):
                if isinstance(b, dict) and b.get('type') == 'tool_use':
                    k = cat(b.get('name', ''), b.get('input') or {})
                    catn[k] += 1; pend[b['id']] = (k, t)
                    fp = (b.get('input') or {}).get('file_path')
                    if fp and k == 'edit': edits_files[os.path.basename(fp)] += 1
                    if fp and k == 'read': read_files[os.path.basename(fp)] += 1
        last_t = t
    wall = sum((r[3] - r[2]).total_seconds() for r in rounds if r[2]) / 60
    tool_wall = sum(catt.values()) / 60
    rereads = sum(v - 1 for v in read_files.values() if v > 1)
    out.append(dict(first=' '.join(first.split())[:46], t0=lines[0]['timestamp'][5:16], wall=wall, tool_wall=tool_wall, catn=catn, catt=catt, rounds=rounds, rereads=rereads, nread=sum(read_files.values()), top_edit=edits_files.most_common(2)))
out.sort(key=lambda r: r['t0'])
cats = ['read', 'edit', 'build', 'proof', 'git', 'other']
print('%-11s %-46s %5s | calls %s | tool-minutes %s | model-min | re-reads/reads | rounds (turns, units M)' % ('start', 'agent', 'wall', ' '.join('%5s' % c for c in cats), ' '.join('%5s' % c for c in cats)))
agg = collections.defaultdict(lambda: [collections.Counter(), collections.Counter(), 0.0, 0.0, 0])
for r in out:
    f = r['first']
    kind = 'builder' if re.search(r'^(builder lane|harness lane|lane l31b|lane p,)', f.lower()) else ('review' if f.lower().startswith('read-only review') else 'reader/probe')
    a = agg[kind]; a[0].update(r['catn']); a[1].update(r['catt']); a[2] += r['wall']; a[3] += r['tool_wall']; a[4] += 1
    rs = ' '.join('(%d, %.2f)' % (x[0], x[1]/1e6) for x in r['rounds'])
    print('%-11s %-46s %5.1f | %s | %s | %5.1f | %3d/%3d | %s' % (r['t0'], f, r['wall'], ' '.join('%5d' % r['catn'][c] for c in cats), ' '.join('%5.1f' % (r['catt'][c]/60) for c in cats), r['wall'] - r['tool_wall'], r['rereads'], r['nread'], rs))
print()
for k, a in agg.items():
    print('%-13s agents %2d wall %6.1f min; tool-min %6.1f; model-min %6.1f' % (k, a[4], a[2], a[3], a[2]-a[3]))
    print('   calls: ' + ' '.join('%s %d' % (c, a[0][c]) for c in cats))
    print('   tool minutes: ' + ' '.join('%s %.1f' % (c, a[1][c]/60) for c in cats))

import re, glob, sys, os
# usage: ab.py DIR TAG1 TAG2 ...   (logs DIR/TAG_I_CAPTURE.log); prints min over runs of mean / median / p95 per call, plus the median across runs
d = sys.argv[1]
tags = sys.argv[2:]
res = {}
for f in sorted(glob.glob(os.path.join(d, '*.log'))):
    b = os.path.basename(f)
    m = re.match(r'(\w+?)_(\d+)_(\w+)\.log', b)
    if not m:
        continue
    tag, i, n = m.groups()
    t = open(f, errors='ignore').read()
    row = {}
    for title, key in (('crowd members Inject', 'mem'), ('InjectCards', 'card')):
        k = t.find('] ' + title + ':')
        if k < 0:
            continue
        blk = t[k:k + 7000].split(chr(10))
        h = re.search(r'total ([\d.]+) ms \(([\d.]+) ms per call', blk[0])
        a = [l for l in blk if '  ALL ' in l][0]
        mm = re.search(r'median ([\d.]+) us\s+p95 ([\d.]+) us', a)
        row[key] = (float(h.group(2)) * 1000, float(mm.group(1)), float(mm.group(2)))
    res.setdefault((tag, n), []).append(row)
for n in ('brock', 'match', 'checkb'):
    for key in ('mem', 'card'):
        parts = []
        for tag in tags:
            L = [r[key] for r in res.get((tag, n), []) if key in r]
            if not L:
                continue
            parts.append('%s: mean %5.0f  median %5.0f  p95 %5.0f us/call (best of %d runs; median-of-runs median %5.0f)' % (
                tag, min(x[0] for x in L), min(x[1] for x in L), min(x[2] for x in L), len(L), sorted(x[1] for x in L)[len(L) // 2]))
        if parts:
            print('%-6s %-4s %s' % (n, key, ' | '.join(parts)))

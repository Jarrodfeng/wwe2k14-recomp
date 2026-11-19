import csv, sys, collections, re
p = sys.argv[1]
since = sys.argv[2] if len(sys.argv) > 2 else "2026-11-15 21:00:00"
rows = [r for r in csv.DictReader(open(p, newline='', encoding='utf-8', errors='replace'))]
allrows = rows
rows = [r for r in rows if r['when'] >= since]
def gid(r):
    # one gate run: its RUN_ID column (S103 GST review 11); rows of older gates have none: (when, root) as before. A check row's `when` is its own finish time, the whole-gate row's the end:
    # with a run id they group exactly, without one they group as they always did.
    return (r.get('run') or '', r['root']) if r.get('run') else (r['when'], r['root'])
def f(x):
    try: return float(x)
    except: return 0.0
whole = [r for r in rows if r['lane'] == '(whole gate)']
checks = [r for r in rows if r['lane'] != '(whole gate)']
print("since", since, "rows", len(rows), "whole-gate rows", len(whole), "check rows", len(checks))
print("all-time: rows", len(allrows), "sum check s", round(sum(f(r['seconds']) for r in allrows if r['lane'] != '(whole gate)')), "sum whole s", round(sum(f(r['seconds']) for r in allrows if r['lane']=='(whole gate)')))
tot = sum(f(r['seconds']) for r in checks)
wtot = sum(f(r['seconds']) for r in whole)
print("sum of check seconds %.0f (%.1f h); sum of whole-gate wall %.0f (%.1f h)" % (tot, tot/3600, wtot, wtot/3600))
print("cache column values:", collections.Counter(r['cache'] for r in checks).most_common(8))
print("verdicts:", collections.Counter(r['verdict'] for r in checks).most_common(8))
print("modes:", collections.Counter(r['mode'] for r in rows).most_common(8))
# classify check by suffix name
def kind(c):
    n = c.split('.', 1)[-1].lower()
    if re.search(r'build|compile|syntax|link', n): return 'build'
    if re.search(r'lint', n): return 'lint'
    if re.search(r'unit|selftest|self_test', n): return 'unit'
    if re.search(r'plant|twin|fault', n): return 'plant'
    return 'proof/replay'
bk = collections.defaultdict(lambda: [0, 0.0])
for r in checks:
    k = kind(r['check']); bk[k][0] += 1; bk[k][1] += f(r['seconds'])
print("\nby kind (count, seconds, share):")
for k, (n, s) in sorted(bk.items(), key=lambda x: -x[1][1]):
    print("  %-14s %5d %9.0f s  %5.1f %%" % (k, n, s, 100*s/tot if tot else 0))
# by check name
bc = collections.defaultdict(lambda: [0, 0.0])
for r in checks:
    n = r['check'].split('.', 1)[-1]; bc[n][0] += 1; bc[n][1] += f(r['seconds'])
print("\ntop 25 check names by total seconds (runs, total s, mean s):")
for n, (c, s) in sorted(bc.items(), key=lambda x: -x[1][1])[:25]:
    print("  %-34s %4d %8.0f %7.1f" % (n, c, s, s/c))
# by lane
bl = collections.defaultdict(lambda: [0, 0.0, set()])
for r in checks:
    bl[r['lane']][0] += 1; bl[r['lane']][1] += f(r['seconds']); bl[r['lane']][2].add(gid(r))
print("\ntop 25 lanes by total seconds (check rows, total s, distinct gate runs, s per gate run):")
for n, (c, s, w) in sorted(bl.items(), key=lambda x: -x[1][1])[:25]:
    print("  %-22s %4d %8.0f %3d %7.0f" % (n, c, s, len(w), s/max(1, len(w))))
# repeats: identical (lane, check) PASS run more than once
rep = collections.defaultdict(list)
for r in checks:
    rep[(r['lane'], r['check'])].append((r['when'], r['verdict'], f(r['seconds']), r['root'].replace('\\', '/').split('/')[-1]))
first = sum(v[0][2] for v in rep.values())
again = tot - first
print("\ndistinct (lane, check) pairs %d; seconds in FIRST run of each %.0f; seconds in re-runs %.0f (%.1f %%)" % (len(rep), first, again, 100*again/tot if tot else 0))
# re-runs from the main checkout (merged-branch gates)
main_s = sum(f(r['seconds']) for r in checks if r['root'].rstrip('\\/').lower().endswith('wwe2k14-recomp'))
print("seconds run from the MAIN checkout (merged-branch gates) %.0f (%.1f %%); from worktrees %.0f" % (main_s, 100*main_s/tot if tot else 0, tot-main_s))
fails = [r for r in checks if r['verdict'] not in ('PASS', 'SKIP', 'CACHED')]
print("non-PASS check rows:", len(fails), "seconds", round(sum(f(r['seconds']) for r in fails)))
print(collections.Counter(r['verdict'] for r in fails).most_common())
# whole gate walls
print("\nwhole-gate rows: n %d, wall sum %.0f s, mean %.0f s, max %.0f s" % (len(whole), wtot, wtot/max(1, len(whole)), max([f(r['seconds']) for r in whole] or [0])))
big = sorted(whole, key=lambda r: -f(r['seconds']))[:12]
for r in big:
    print("  ", r['when'], r['check'], r['verdict'], r['seconds'], r['root'].replace('\\', '/').split('/')[-1])
# concurrency: per whole-gate, sum of its checks vs wall
bywhen = collections.defaultdict(float)
for r in checks: bywhen[gid(r)] += f(r['seconds'])
ratio = []
for r in whole:
    s = bywhen.get(gid(r), 0.0)
    if f(r['seconds']) > 0: ratio.append(s / f(r['seconds']))
if ratio:
    ratio.sort(); print("sum(checks)/wall per gate: median %.2f, p10 %.2f, p90 %.2f" % (ratio[len(ratio)//2], ratio[len(ratio)//10], ratio[9*len(ratio)//10]))
# since the experiment 21:41 on 11-16
exp = [r for r in checks if r['when'] >= '2026-11-16 21:41:00']
print("\nsince 2026-11-16 21:41: check rows %d, seconds %.0f; whole gates %d, wall %.0f" % (len(exp), sum(f(r['seconds']) for r in exp), len([r for r in whole if r['when'] >= '2026-11-16 21:41:00']), sum(f(r['seconds']) for r in whole if r['when'] >= '2026-11-16 21:41:00')))
bk2 = collections.defaultdict(lambda: [0, 0.0])
for r in exp:
    k = kind(r['check']); bk2[k][0] += 1; bk2[k][1] += f(r['seconds'])
for k, (n, s) in sorted(bk2.items(), key=lambda x: -x[1][1]):
    print("  %-14s %5d %9.0f s" % (k, n, s))
bc2 = collections.defaultdict(lambda: [0, 0.0])
for r in exp:
    n = r['check']; bc2[n][0] += 1; bc2[n][1] += f(r['seconds'])
print(" top 15 checks since the experiment:")
for n, (c, s) in sorted(bc2.items(), key=lambda x: -x[1][1])[:15]:
    print("  %-44s %4d %8.0f %7.1f" % (n, c, s, s/c))

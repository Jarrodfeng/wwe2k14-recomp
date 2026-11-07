r"""Session 95 (the wall): the command thread's own CPU per frame in the match, per arm, from a run's [d3d][wall] lines.
Answers: did a switch cut the command thread's work? Run with --gpu_d3d_wall_stats=150 (and --gpu_d3d_wall_ab=MASK for an in-run A/B: both arms share one run's load;
cycles swell about 2x when builds share the machine, so compare arms in one run or use exclusive runs). Route: tools\input_scripts\short\s95_wall_match.txt.
    py tools\perf\wall_ab.py LOG [LOG ...]
"""
import re,sys,statistics
from collections import defaultdict
for f in sys.argv[1:]:
    on=False; arms=defaultdict(list); skip=2
    for l in open(f,encoding='utf-8',errors='replace'):
        if 'scene=match' in l: on=True; continue
        if 'scene=end' in l: on=False
        if on and '[d3d][wall] frame' in l:
            m=re.search(r'arm (\d+):.*cpu ([\d.]+) ms.*?([\d.]+) Mcycles, wall ([\d.]+)',l)
            if not m: m=re.search(r'()cpu ([\d.]+) ms.*?([\d.]+) Mcycles, wall ([\d.]+)',l)
            if skip>0: skip-=1; continue
            arms[m[1]].append((float(m[3]),float(m[2]),float(m[4])))
    meds={}
    for a,v in sorted(arms.items()):
        c=[x[0] for x in v]; meds[a]=statistics.median(c)
        print(f[-16:],'arm',a,'windows',len(c),'median Mcycles %.2f min %.2f cpu ms %.2f wall ms %.2f'%(statistics.median(c),min(c),statistics.median([x[1] for x in v]),statistics.median([x[2] for x in v])))
    if len(meds)==2:
        k=sorted(meds,key=lambda x:int(x or 0)); print('  change %.1f%%'%((meds[k[1]]/meds[k[0]]-1)*100))
    # paired: each armed window against the mean of its two neighbours (the match's load moves slowly; spikes are other work)
    seq=[]; on=False
    for l in open(f,encoding='utf-8',errors='replace'):
        if 'scene=match' in l: on=True
        if 'scene=end' in l: on=False
        m=re.search(r'arm (\d+):.*?, ([\d.]+) Mcycles',l)
        if on and m: seq.append((int(m[1]),float(m[2])))
    seq=seq[2:]
    r=[seq[i][1]/((seq[i-1][1]+seq[i+1][1])/2) for i in range(1,len(seq)-1) if seq[i][0]]
    if r: print('  paired median ratio %.3f of %d'%(statistics.median(r),len(r)))

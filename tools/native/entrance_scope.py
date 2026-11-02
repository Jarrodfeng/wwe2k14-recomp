r"""Session 82 (P2): the numbers behind docs\native\OWNED_ENTRANCES_SCOPE.md.

  py -I tools\native\entrance_scope.py SURFACE_LOG CENSUS_LOG BROCK_CENSUS_FRAME BRYAN_CENSUS_FRAME

SURFACE_LOG: a Surface log with [native][pass] lines of the passlog windows (out\s82\surface\owned1\x\multi\wwe2k14_044.log:
brock_* from frame 2659, bryan_* from 5833; the frames alternate the owned frame off / on, told apart here by whether a
frame has `owned ...` timer lines). CENSUS_LOG: a run with the owned frame's census ([native][owned-frame] frame F pass N
lines). Each census pass is paired, in order, with a pass of an owned-off Surface frame of the same entrance (same pitch,
draws within 1; the replay and the Surface route are the same game moment: tron + 300 / + 440), the Surface ms taken as
the median over the owned-off frames. Prints per entrance the pairing rate and the Surface ms by census status.
"""
import re
import statistics
import sys
from collections import defaultdict

PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) pitch (\d+) colour kind (\d+) depth (\d) '
                  r'draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+): ([0-9.]+) ms \(before it ([0-9.]+) ms\)(?: owned (\S+))?')
CENS = re.compile(r'owned-frame\] frame (\d+) pass (\d+): (owned|fallback) (\S+) (\S+)(?: -)? surface (\d+)/(\d+)/(\d+) box (\d+x\d+) '
                  r'draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+) copies (\d+)')
WHY = re.compile(r'; why: (.*)$')
RANGES = {'brock': (2659, 2780), 'bryan': (5833, 5950)}


def surface(path, lo, hi):
    frames = defaultdict(list)
    for line in open(path, errors='replace'):
        m = PASS.search(line)
        if not m:
            continue
        f = int(m.group(1))
        if lo <= f <= hi:
            frames[f].append(dict(n=int(m.group(2)), pitch=int(m.group(6)), draws=int(m.group(9)), vs=m.group(10), ps=m.group(11),
                                  ms=float(m.group(12)), before=float(m.group(13)), own=m.group(14), w=int(m.group(4)), h=int(m.group(5))))
    off = {f: p for f, p in frames.items() if not any(x['own'] for x in p)}
    on = {f: p for f, p in frames.items() if any(x['own'] for x in p)}
    return off, on


def census(path, frame):
    out = []
    for line in open(path, errors='replace'):
        m = CENS.search(line)
        if m and int(m.group(1)) == frame:
            why = WHY.search(line)
            out.append(dict(n=int(m.group(2)), st=m.group(3), a=m.group(4), b=m.group(5), pitch=int(m.group(7)), box=m.group(9),
                            draws=int(m.group(10)), vs=m.group(11), ps=m.group(12), copies=int(m.group(13)), why=why.group(1) if why else ''))
    return out


def pair(cens, spass):
    res, j = {}, 0
    for c in cens:
        for k in range(j, min(j + 30, len(spass))):
            s = spass[k]
            if s['pitch'] == c['pitch'] and abs(s['draws'] - c['draws']) <= max(1, int(0.05 * c['draws'])) and not s['own']:
                res[c['n']] = s
                j = k + 1
                break
    return res


def main():
    sl, cl = sys.argv[1], sys.argv[2]
    for ent, cf in (('brock', int(sys.argv[3])), ('bryan', int(sys.argv[4]))):
        lo, hi = RANGES[ent]
        off, on = surface(sl, lo, hi)
        cens = census(cl, cf)
        print(f'## {ent}: census frame {cf} ({len(cens)} passes, {sum(1 for c in cens if c["st"] == "owned")} owned); Surface frames '
              f'{len(off)} owned-off, {len(on)} owned-on')
        owned_census = any(c['st'] == 'owned' for c in cens)
        src = on if owned_census else off  # an entrance with owned passes: the owned-on frames' remaining truth passes
        cens_f = [c for c in cens if c['st'] != 'owned']
        src = {f: [x for x in p if not x['own']] for f, p in src.items()}
        sizes = {f: len(p) for f, p in src.items()}
        print('   Surface frames used (' + ('owned-on, truth passes only' if owned_census else 'owned-off') + ') pass counts:', sorted(set(sizes.values())))
        modal = statistics.mode(sizes.values()) if sizes else 0
        use = [f for f, n in sizes.items() if n == modal]
        off_all = off
        off = src
        ms_by_n = defaultdict(list)
        pairs = []
        for f in use:
            pr = pair(cens_f, off[f])
            pairs.append(len(pr))
            for n, s in pr.items():
                ms_by_n[n].append(s['ms'] + s['before'] * 0)  # the pass's own timer; 'before it' is the gap before it
        print(f'   paired census passes per Surface frame: {sorted(set(pairs))} of {len(cens_f)}')
        tot_use = statistics.median(sum(s['ms'] for s in off[f]) for f in use) if use else 0
        print(f'   Surface median GPU ms per frame, truth passes: {tot_use:.2f}')
        if on:
            own_ms = statistics.median(sum(x['ms'] for x in p if x['own']) for p in on.values())
            tot_on = statistics.median(sum(x['ms'] for x in p) for p in on.values())
            print(f'   owned-on frames: the owned pieces {own_ms:.2f} ms of {tot_on:.2f} ms summed over passes (median)')
        agg = defaultdict(lambda: [0, 0.0, 0, 0])  # key -> passes, ms, draws, copies
        unp = 0.0
        for c in cens_f:
            ms = statistics.median(ms_by_n[c['n']]) if ms_by_n.get(c['n']) else None
            key = 'owned' if c['st'] == 'owned' else 'fallback ' + c['a'] + (' (' + c['why'] + ')' if c['why'] else '')
            e = agg[key]
            e[0] += 1; e[2] += c['draws']; e[3] += c['copies']
            if ms is None:
                unp += 0
            else:
                e[1] += ms
        for k, (p, ms, dr, cp) in sorted(agg.items(), key=lambda kv: -kv[1][1]):
            print(f'   {k:42s} passes {p:3d} draws {dr:5d} copies {cp:3d}  Surface ms (paired) {ms:6.2f}')
        # per census pass: biggest
        rows = []
        for c in cens_f:
            ms = statistics.median(ms_by_n[c['n']]) if ms_by_n.get(c['n']) else None
            rows.append((ms or 0.0, c))
        print('   biggest passes (Surface ms owned-off, census pass):')
        for ms, c in sorted(rows, key=lambda r: -r[0])[:14]:
            print(f'     {ms:6.3f} ms  pass {c["n"]:2d} {c["st"]:8s} {c["a"]:10s} {c["why"]:14s} box {c["box"]:9s} draws {c["draws"]:4d} '
                  f'copies {c["copies"]} vs {c["vs"][:8]} ps {c["ps"][:8]}')


main()

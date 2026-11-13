r"""Lane 155 review S2 (S101): does any draw AFTER the bake passes depend on a constant register whose last writer is inside a bake iteration? Offline, 0 game runs.

The cut removes the bake iterations' register writes as well as their targets / clear / draw / copy; the capture's per-draw constant banks are the game's register file as the recorder left it, so a draw that does not
rewrite a register it reads would inherit the bake's value in the game (and the old value with the cut). For every frame sampled, with P = the last draw BEFORE the first bake pass and L = the last bake draw:
a draw's bank in the capture is its base bank plus the 16-dword blocks written since (bank_defs / bank_patches). For every draw X after the last bake pass the base chain is walked down; if it passes THROUGH a bake draw's bank, every block the bake changed (against P)
that no write on the chain touched is INHERITED (the game would have read the bake's value, the cut would leave the older one). A chain that never meets a bake bank (the banks are rebuilt, not derived) inherits nothing.
Old wording follows and is superseded:
(superseded by the chain walk below)

  py -I tools\crowd\bake_skip_regs.py LABEL=CAPTURE [...] [--frames A:B:STEP] [--plant inherit] [--json OUT]
Exit 1 when a register is INHERITED (or, with --plant inherit, when the planted inheritance is NOT found). Counts only."""
import argparse
import collections
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402
import bake_skip_inherit as bi  # noqa: E402

pcap = cc.pcap


def read_blocks():
    """effect name -> {'vertex': set of 16-dword blocks the effect's description reads, 'fragment': same} (the union over its techniques: a superset, so 'read' is never under-counted)."""
    out = {}
    d = os.path.join(cc.ROOT, 'src', 'renderer', 'shaders', 'fx')
    for fn in os.listdir(d):
        if fn.endswith('.fx.json'):
            j = json.load(open(os.path.join(d, fn), encoding='utf-8'))
            e = out.setdefault(j['effect'], {'vertex': set(), 'fragment': set()})
            for st in ('vertex', 'fragment'):
                for c in j.get('solve', {}).get(st, {}).get('constants', []):
                    for r in range(c['register'], c['register'] + max(1, c.get('count', 1))):
                        e[st].add(r // 4)
    return out


BLOCKS = read_blocks()


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    lo, hi, step = (int(x) if x else None for x in (spec.split(':') + ['', ''])[:3])
    return list(range(lo or 0, min(hi if hi is not None else total, total), step or 1))


def measure(cap, frames, plant):
    m = collections.Counter()
    regs = collections.Counter()
    progs = collections.defaultdict(set)
    for fi in frames:
        f = cap.frame(fi)
        D = f.draws
        if not len(D):
            continue
        veff = cap.effs(D['vs_hash'])
        copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
        passes = cc.walk(f, copies_t, cap)
        bk = [p for p in passes if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws)]
        if len(bk) != 20:
            continue
        m['frames'] += 1
        idx_ops, pos_ops = f.draw_ops()
        pos_of = {int(d): int(p) for d, p in zip(idx_ops, pos_ops)}
        first, last = bk[0].pos, bk[-1].pos
        before = [d for d in idx_ops if pos_of[int(d)] < first]
        after = [int(d) for d in idx_ops if pos_of[int(d)] > last and int(d) not in {x for p in bk for _, x in p.draws}]
        if not after:
            continue
        P, L = (int(before[-1]) if before else None), bk[-1].draws[0][1]
        bake_draws = {x for p in bk for _, x in p.draws}
        for which, key in (('vs', 'vs_bank'), ('ps', 'ps_bank')):
            bake_banks = {int(D[x][key]) for x in bake_draws}
            bp = (f.bank(int(D[P][key])) if P is not None else bi.root_bank(f, int(D[bk[0].draws[0][1]][key]))).view('u4').reshape(-1, 4)   # no draw before the bakes: the frame's start state
            bl = f.bank(int(D[L][key])).view('u4').reshape(-1, 4)
            cand_blocks = set(int(r) // 4 for r in np.nonzero((bp != bl).any(axis=1))[0])   # 16-dword blocks (4 registers) the bake changed from the pre-bake state
            if plant == 'inherit' and which == 'vs':
                m['planted'] += 1
            for d in after[:40] + after[40::15]:   # the first 40 draws after the bakes (the page pass, the floor, the first blocks) and every 15th after
                # the draw's bank is its base bank plus the blocks written since: walk the chain down; if it passes THROUGH a bake draw's bank, the blocks no write on the way touched are inherited from the bake
                patched, j, via_bake, depth = set(), int(D[d][key]), False, 0
                while True:
                    if j in bake_banks:
                        via_bake = True
                        break
                    dfn = f.bdefs[j] if len(f.bdefs) else None
                    if dfn is None or dfn['base'] == cc.capread.NO_BASE or depth > 5000:
                        break
                    for q in range(int(dfn['patch_count'])):
                        patched.add(int(f.bpatches[int(dfn['patch_first']) + q]['block']))
                    j, depth = int(dfn['base']), depth + 1
                m['draw_banks'] += 1
                if plant == 'inherit' and which == 'vs' and d == after[0]:
                    via_bake, patched = True, set()    # planted: this draw's chain is made to run through the bake and to write nothing
                if not via_bake:
                    m['chain_misses_bake'] += 1
                    continue
                m['chain_through_bake'] += 1
                inh = cand_blocks - patched
                eff = str(veff[d]).split('/')[0]
                stage = 'vertex' if which == 'vs' else 'fragment'
                for blk in inh:
                    if eff not in BLOCKS:
                        m['unknown_effect'] += 1
                        regs[(which + '?', 4 * blk)] += 1
                        progs[(which + '?', 4 * blk)].add(veff[d])
                        continue
                    if blk not in BLOCKS[eff][stage]:
                        m['inherited_unread'] += 1   # the bake's value sits in a register this draw's effect never reads
                        continue
                    m['inherited'] += 1
                    regs[(which, 4 * blk)] += 1
                    progs[(which, 4 * blk)].add(veff[d])
    return m, regs, progs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', choices=['inherit'])
    a = ap.parse_args()
    bad = False
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        cap = cc.Cap(path)
        m, regs, progs = measure(cap, parse_frames(a.frames, len(cap.frames)), a.plant)
        ok = (m['inherited'] == 0 and m['unknown_effect'] == 0) if not a.plant else (m['inherited'] > 0)
        print('%s  %-8s frames %d, draw banks compared %d: register blocks INHERITED AND READ by the draw effect %d (inherited but never read %d, effect unknown %d; draw banks whose chain runs through a bake bank: %d, not: %d)%s' % (
            'PASS' if ok else 'FAIL', label, m['frames'], m['draw_banks'], m['inherited'], m['inherited_unread'], m['unknown_effect'], m['chain_through_bake'], m['chain_misses_bake'], ('  (plant ' + a.plant + ')') if a.plant else ''))
        for (w, r), n in regs.most_common(60):
            print('        %s c%d: %d draws, programs %s' % (w, r, n, sorted(progs[(w, r)])[:3]))
        bad |= not ok or m['frames'] == 0
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

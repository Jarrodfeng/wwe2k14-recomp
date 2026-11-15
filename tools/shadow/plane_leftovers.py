r"""S102 lane P3 (offline step): the register-leftover census of the plane chain. Does any draw AFTER the chain read a constant register whose last writer is inside the chain? Offline, 0 game runs.

  py -I tools\shadow\plane_leftovers.py LABEL=CAPTURE [...] [--frames A:B:STEP] [--plant inherit] [--json OUT]

The question (OWN_THE_PASS.md 9a item 1): a node whose walker is skipped no longer writes the constant registers (VS c0-c16 the plane constants, c72 EyePos, c76.. the palette, c33-c40 the blur's, PS c34 ...) the walk
leaves in the device. If a later draw READS one of them without writing it itself, the skip changes the picture and the hook must leave the register (R4 section 3 item 2). Method (the same chain-walk as the bake's
bake_skip_regs.py, pointed at this pass; the "inherited" count is an UPPER bound):
  a capture bank is its base bank plus the 16-dword blocks (4 registers) written since (bank_defs / bank_patches). For a draw X after the chain, walk its base chain down: if it passes THROUGH a chain draw's bank, every
  block the chain left different from the state before it (P = the last draw before the chain, or the frame's root bank when the chain is the first thing in the frame) that no write on the way touched is INHERITED.
  It counts only if X's effect reads that block (the union over the effect's techniques, from src\renderer\shaders\fx\*.fx.json).
Dirty-but-equal caveat: the capture reader drops a register write that does not change its block (reader.h ~5156). A later draw that REWRITES a block with the value the chain left looks "not written" here, so it is
counted as inherited: the number below is therefore an upper bound on what the game's later draws really take from the chain. 0 inherited = proof that nothing is left to leave. Inherited > 0 = unproven, not "real".
Exit 1 when a register is inherited and read (or, with --plant inherit, when the planted inheritance is NOT found). Counts only."""
import argparse
import collections
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
import plane_chain as pc  # noqa: E402
import bake_skip_regs as br  # noqa: E402  (BLOCKS: effect -> {'vertex': blocks read, 'fragment': blocks read})
import bake_skip_inherit as bi  # noqa: E402

NO_BASE = bi.NO_BASE
START = {}   # the last inherits() call's pre-chain register file per stage ('vs_bank' / 'ps_bank'), rows of 4 words


def read_regs():
    """effect -> {'vertex': set of registers, 'fragment': set} the effect's description reads (the union over its techniques: a superset, so 'read' is never under-counted)."""
    out = {}
    d = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
    for fn in os.listdir(d):
        if fn.endswith('.fx.json'):
            j = json.load(open(os.path.join(d, fn), encoding='utf-8'))
            e = out.setdefault(j['effect'], {'vertex': set(), 'fragment': set()})
            for st in ('vertex', 'fragment'):
                for c in j.get('solve', {}).get(st, {}).get('constants', []):
                    for r in range(c['register'], c['register'] + max(1, c.get('count', 1))):
                        e[st].add(r)
    return out


REGS = read_regs()


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    lo, hi, step = (int(x) if x else None for x in (spec.split(':') + ['', ''])[:3])
    return list(range(lo or 0, min(hi if hi is not None else total, total), step or 1))


def inherits(cap, f, ch, fi=None):
    """-> list of (draw, 'vs_bank'|'ps_bank', blocks) for every draw AFTER the chain whose bank chain runs through a chain draw's bank: blocks = the 16-dword blocks the chain left different from before it that no write on the
    way touched (what the draw inherits from the chain). Also returns (n_draw_banks, n_through, cand_vs, cand_ps)."""
    D = f.draws
    idx = [int(i) for _, i in f.ops]
    chain_draws = [idx[o] for p in ch.passes for o in p['draws']]
    after = [idx[o] for o in range(ch.end_op, len(f.ops)) if int(f.ops[o][0]) == 2]
    before = [idx[o] for o in range(ch.first_op) if int(f.ops[o][0]) == 2]
    P, L = (before[-1] if before else None), chain_draws[-1]
    out, banks, through_n, cand_n = [], 0, 0, {}
    prev_last = None   # no draw before the chain in this frame: the register file persists across frames (checked: only the registers a frame writes differ), so the state before the chain = the LAST draw's bank of the previous frame
    if P is None and fi is not None and fi > 0:
        fp = cap.c.frame(fi - 1)
        pidx = [int(i) for _, i in fp.ops]
        pd = [pidx[o] for o in range(len(fp.ops)) if int(fp.ops[o][0]) == 2]
        prev_last = (fp, pd[-1]) if pd else None
    for which, key in (('vs', 'vs_bank'), ('ps', 'ps_bank')):
        chain_banks = {int(D[x][key]) for x in chain_draws}
        if P is not None:
            start = f.bank(int(D[P][key]))
        elif prev_last is not None:
            start = prev_last[0].bank(int(prev_last[0].draws[prev_last[1]][key]))
        else:
            start = bi.root_bank(f, int(D[chain_draws[0]][key]))   # the first frame of a capture: the root of the first chain draw's bank chain (an approximation)
        bp = start.view('u4').reshape(-1, 4)
        START[key] = bp.copy()   # (plane_poison.py --to-start: the register file the game would hold if the walk had not run)
        bl = f.bank(int(D[L][key])).view('u4').reshape(-1, 4)
        rows_changed = set(int(r) for r in np.nonzero((bp != bl).any(axis=1))[0])   # registers (rows of 4 words) the chain left different from before it
        cand = set(r // 4 for r in rows_changed)   # ... and the blocks (4 registers) that hold them
        cand_n[which] = len(cand)
        info = bi.chain_infos(f, chain_banks)
        for d in after:
            banks += 1
            through, patched = info(int(D[d][key]))
            if not through:
                continue
            through_n += 1
            blocks = cand - patched
            if blocks:
                out.append((d, key, {b: sorted(r for r in rows_changed if r // 4 == b) for b in blocks}))   # block -> the registers of it the chain changed
    return out, banks, through_n, cand_n


def measure(cap, frames, plant):
    m = collections.Counter()
    regs = collections.Counter()
    progs = collections.defaultdict(set)
    examples = {}
    for fi in frames:
        f = cap.c.frame(fi)
        ch, why = pc.find(cap, f)
        if ch is None:
            m['no_chain'] += 1
            continue
        D = f.draws
        veff = cap.effs(D['vs_hash'])
        m['frames'] += 1
        edits, banks, through, cand_n = inherits(cap, f, ch, fi)
        m['draw_banks'] += banks
        m['chain_through_plane'] += through
        m['chain_misses_plane'] += banks - through
        m['cand_blocks_vs'] += cand_n['vs']
        m['cand_blocks_ps'] += cand_n['ps']
        if plant == 'inherit' and not any(k == 'vs_bank' for _, k, _ in edits):   # planted: a later draw is made to inherit one block the chain changed (the check must then find it)
            idx = [int(i) for _, i in f.ops]
            d0 = next(idx[o] for o in range(ch.end_op, len(f.ops)) if int(f.ops[o][0]) == 2)
            edits.append((d0, 'vs_bank', {0: [0]}))
        for d, key, blocks in edits:
            which = 'vs' if key == 'vs_bank' else 'ps'
            stage = 'vertex' if which == 'vs' else 'fragment'
            eff = str(veff[d]).split('/')[0]
            for blk, rows in blocks.items():
                for reg in rows:
                    if eff not in REGS:
                        m['unknown_effect'] += 1
                        regs[(which + '?', reg)] += 1
                        progs[(which + '?', reg)].add(str(veff[d]))
                        continue
                    if reg not in REGS[eff][stage]:
                        m['inherited_unread'] += 1   # the chain's value sits in a register this draw's effect never reads
                        continue
                    m['inherited'] += 1
                    regs[(which, reg)] += 1
                    progs[(which, reg)].add(str(veff[d]))
                    examples.setdefault((which, reg), (fi, d))
    return m, regs, progs, examples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', choices=['inherit'])
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    bad = False
    out = {}
    for spec in a.captures:
        label, _, path = spec.partition('=')
        cap = pc.sc.Cap(path)
        m, regs, progs, ex = measure(cap, parse_frames(a.frames, len(cap.frames)), a.plant)
        ok = (m['inherited'] == 0 and m['unknown_effect'] == 0) if not a.plant else (m['inherited'] > 0)
        print('%s  %-8s frames %d (no chain %d), draw banks compared %d: register blocks INHERITED AND READ by the draw effect %d (inherited but never read %d, effect unknown %d; draw banks whose chain runs through a plane-chain bank: %d, not: %d; '
              'blocks the chain left different: vs %d, ps %d; counted per REGISTER the chain changed and the draw effect reads)%s' % ('PASS' if ok else 'FAIL', label, m['frames'], m['no_chain'], m['draw_banks'], m['inherited'], m['inherited_unread'], m['unknown_effect'], m['chain_through_plane'],
                                                                      m['chain_misses_plane'], m['cand_blocks_vs'], m['cand_blocks_ps'], ('  (plant ' + a.plant + ')') if a.plant else ''))
        for (w, r), n in regs.most_common(60):
            print('        %s c%d: %d draws, programs %s, first frame %d draw %d' % (w, r, n, sorted(progs[(w, r)])[:3], ex[(w, r)][0], ex[(w, r)][1]))
        out[label] = dict(frames=m['frames'], draw_banks=m['draw_banks'], inherited=m['inherited'], inherited_unread=m['inherited_unread'], unknown_effect=m['unknown_effect'], through=m['chain_through_plane'],
                          regs={'%s c%d' % k: n for k, n in regs.items()})
        bad |= not ok or m['frames'] == 0
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(out, open(a.json, 'w'), indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

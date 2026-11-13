r"""Lane 155 review S2 (S101): do all 20 bake iterations write the same constant-register blocks? Offline, 0 game runs.

The anchor rule keeps the LAST live thumbnail of a pass, on the argument that every iteration writes the same registers (one program), so the register state after the pass is the same whichever earlier iterations were cut.
This measures it: for iteration k the blocks (16 dwords = 4 registers) written on the chain between bake k-1's bank and bake k's bank (iteration 0: back to the frame's first bank), per frame; the claim holds in a frame when
every block iteration k wrote, with a value that differs from what iteration 19 leaves there, is also written by iteration 19 (else a cut iteration k would leave a value iteration 19 does not overwrite).
  py -I tools\crowd\bake_skip_writes.py LABEL=CAPTURE [...] [--frames A:B:STEP]
Exit 1 when any frame has a block written by an earlier iteration with a different value and NOT rewritten by iteration 19. Counts only."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402

pcap = cc.pcap
# c124-c127 = the bone slots 16 and 17 of the bake's array: the 17th bone of a 17-bone palette, referenced only by the meshes of types 14 and 18 (bake_pose_check); for every other type it holds the
# leftover of the last type that set it. A later draw that read it without writing it would depend on a leftover of a bake nobody is told about; no draw is known to.
ALLOWED = {124 // 4}
NO_BASE = cc.capread.NO_BASE


def written_between(f, j_from, j_to):
    """Blocks patched on the base chain from bank j_from down to bank j_to (exclusive); None when j_to is not on the chain (then down to the root)."""
    blocks, j, steps = set(), int(j_from), 0
    while j != j_to and steps < 100000:
        d = f.bdefs[j]
        for q in range(int(d['patch_count'])):
            blocks.add(int(f.bpatches[int(d['patch_first']) + q]['block']))
        if d['base'] == NO_BASE:
            return blocks, False
        j, steps = int(d['base']), steps + 1
    return blocks, j == j_to


def state_differences(cap, f, bk, D):
    """Non-constant state across the bake iterations (review 2): fixed state, dynamic state, bool/loop constants, the texture slot set and every slot's attributes but the image, the samplers.
    -> {field: number of iterations (against iteration 0) that differ}; the image of slot 0 is the type's own texture and is NOT a field here (the last iteration sets it)."""
    out = {}
    ref = None
    for k, p in enumerate(bk):
        d = p.draws[0][1]
        r = D[d]
        rows = f.slots[int(r['tex_first']):int(r['tex_first']) + int(r['tex_count'])]
        slot_set = tuple(sorted(int(x['slot']) for x in rows))
        attrs = tuple(sorted((int(x['slot']), int(x['view_format']), bytes(x['swizzle']), bytes(x['sign']), int(x['dimension']), int(x['min_level']), int(x['max_level']), float(x['lod_bias'])) for x in rows))
        smp = tuple(sorted((int(x['slot']), f.raw[cc.pcap.SEC['samplers']][int(x['sampler'])]) for x in rows))
        cur = {'fixed': f.raw[cc.pcap.SEC['fixeds']][int(r['fixed'])], 'dynamic': f.raw[cc.pcap.SEC['dynamics']][int(r['dynamic'])],
               'bool_loop': f.raw[cc.pcap.SEC['bool_loops']][int(r['bool_loop'])] if int(r['bool_loop']) < len(f.raw[cc.pcap.SEC['bool_loops']]) else b'', 'slot_set': slot_set,
               'slot_attrs': attrs, 'samplers': smp, 'layout': (int(f.layouts[int(r['layout'])]['stride']), int(r['source_prim']), int(r['index_32bit']))}
        if ref is None:
            ref = cur
            continue
        for key, v in cur.items():
            if v != ref[key]:
                out[key] = out.get(key, 0) + 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    a = ap.parse_args()
    bad = False
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        cap = cc.Cap(path)
        lo, hi, st = (int(x) if x else None for x in (a.frames.split(':') + ['', ''])[:3]) if a.frames else (0, None, 1)
        frames = range(lo or 0, min(hi or len(cap.frames), len(cap.frames)), st or 1)
        n = viol = linked = leftover = 0
        state_diff = {}
        for fi in frames:
            f = cap.frame(fi)
            D = f.draws
            if not len(D) or not len(f.bdefs):
                continue
            veff = cap.effs(D['vs_hash'])
            copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
            bk = [p for p in cc.walk(f, copies_t, cap) if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws)]
            if len(bk) != 20:
                continue
            n += 1
            for key in ('vs_bank', 'ps_bank'):
                banks = [int(D[p.draws[0][1]][key]) for p in bk]
                w19, ok = written_between(f, banks[19], banks[18])
                ok_all = ok
                diff_blocks = set()
                for k in range(19):
                    wk, okk = written_between(f, banks[k], banks[k - 1] if k else -1)
                    ok_all &= okk or k == 0
                    # blocks iteration k wrote whose value differs from the value iteration 19 leaves
                    b19 = f.bank(banks[19]).reshape(-1, 16)
                    bk_ = f.bank(banks[k]).reshape(-1, 16)
                    diff_blocks |= {b for b in wk if not np.array_equal(b19[b], bk_[b])}
                linked += int(ok_all)
            sd = state_differences(cap, f, bk, D)
            for key, v in sd.items():
                state_diff[key] = state_diff.get(key, 0) + v
                missing = diff_blocks - w19 - ALLOWED
                leftover += len((diff_blocks - w19) & ALLOWED)
                if missing:
                    viol += 1
                    if os.environ.get('SHOW'):
                        print('   ', key, 'frame', fi, 'registers', sorted(4 * b for b in missing))
        print('%s  %-8s frames %d: %d of %d bank chains link iteration to iteration; frames x stages with a block written by an earlier iteration with another value than iteration 19 leaves and not rewritten by 19: %d (plus the bone-16/17 block c124, %d times: a leftover)' % (
            'PASS' if viol == 0 and n else 'FAIL', label, n, linked, 2 * n, viol, leftover))
        print('%s  %-8s non-constant state across the bake iterations (fixed / dynamic / bool-loop / slot set / slot attributes / samplers / layout, iteration vs iteration 0): differing iterations %s' % (
            'PASS' if not {k: v for k, v in state_diff.items() if k != 'bool_loop'} else 'FAIL', label, (str(state_diff) + ' (bool_loop = the 160-byte bool / loop record every draw sets whole: it differs per type, so the last iteration decides what the pass leaves: the anchor)') if state_diff else 'none'))
        bad |= viol > 0 or n == 0 or bool({k: v for k, v in state_diff.items() if k != 'bool_loop'})
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

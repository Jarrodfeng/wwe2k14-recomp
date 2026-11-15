r"""S102 lane P3 (offline step): the POISON test of the register-leftover question. Offline, 0 game runs.

  py -I tools\shadow\plane_poison.py CAPTURE_DIR OUT_DIR [--only-block REG] [--frames A:B:STEP]

plane_leftovers.py counts, by a chain walk, which register blocks a draw AFTER the plane chain inherits from it (the blocks the chain left different from before it that no write on the way touched). That is an upper bound
(the capture reader drops a write that does not change its block, so a later draw that re-installs a block with the very value the chain left looks "not written": the dirty-but-equal caveat). This tool answers the real
question: it writes a copy of the capture in which EVERY block a later draw inherits from the chain is set to 42.0 (the draw's own bank, vertex and pixel stage), the ops untouched. fview replays it: if the presented
picture and the chain's pictures equal the original's on every frame, no later draw's result depends on a register the plane walk leaves behind (poisoning covers the dirty-but-equal blocks as well: a draw that really
re-wrote the block in the game is unaffected only if the value it reads is the one it wrote, and then it was not inherited; a draw that really inherited it shows the 42.0). `--only-block REG` poisons only the block holding
register REG (to name which one moves the picture when the answer is "some does").
Big files are HARD-LINKED. Counts only; scratch for under out\."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'crowd'))
import plane_chain as pc  # noqa: E402
import plane_leftovers as pl  # noqa: E402
import bake_skip_inherit as bi  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('out')
    ap.add_argument('--only-block', type=int, default=-1, help='poison only the block holding this register')
    ap.add_argument('--stage', choices=['vs', 'ps'], default='vs', help='the stage of --only-block')
    ap.add_argument('--frames', default='')
    ap.add_argument('--keep-reg', default='', help='comma list of registers (either stage) the HOOK would leave as the chain left them: never edited (e.g. 218)')
    ap.add_argument('--only-effect', default='', help='edit only the draws whose vertex effect name contains this text (e.g. yShadowMap)')
    ap.add_argument('--to-start', action='store_true', help='instead of 42.0: every inherited register gets the value it held BEFORE the chain (the previous frame last draw, or the last draw before the chain): the exact cut emulation (an upper bound: a later draw that really re-installed the register would not get it)')
    ap.add_argument('--control-reg', type=int, default=-1, help='CONTROL (tests): instead of the inherited registers, poison the vertex block holding this register (a register every colour draw reads, e.g. 56) in EVERY draw after the chain: the picture must change')
    a = ap.parse_args()
    if not os.path.isdir(a.capture):
        print('FAIL  the input must be a capture folder: ' + a.capture)
        return 1
    keep = set(int(x) for x in a.keep_reg.split(',') if x)
    cap = pc.sc.Cap(a.capture)
    sel = set(pl.parse_frames(a.frames, len(cap.frames)))
    os.makedirs(os.path.join(a.out, 'frames'), exist_ok=True)
    for name in os.listdir(a.capture):
        src, dst = os.path.join(a.capture, name), os.path.join(a.out, name)
        if os.path.isfile(src) and not os.path.exists(dst):
            os.link(src, dst)
    n_frames = n_edits = n_blocks = 0
    for fi, path in enumerate(cap.frames):
        dst = os.path.join(a.out, 'frames', os.path.basename(path))
        f = cap.c.frame(fi)
        ch, why = pc.find(cap, f) if fi in sel else (None, '')
        edits = []
        if ch is not None:
            found, _, _, _ = pl.inherits(cap, f, ch, fi)
            if a.control_reg >= 0:   # the control: a block every colour draw reads, poisoned in every later draw
                idx = [int(i) for _, i in f.ops]
                found = [(idx[o], 'vs_bank', {a.control_reg // 4: [a.control_reg // 4 * 4 + k for k in range(4)]}) for o in range(ch.end_op, len(f.ops)) if int(f.ops[o][0]) == 2]
            veff = cap.effs(f.draws['vs_hash']) if a.only_effect else None
            for d, key, blocks in found:
                if a.only_effect and a.only_effect not in str(veff[d]):
                    continue
                if a.only_block >= 0:
                    blocks = {b: r for b, r in blocks.items() if b == a.only_block // 4 and a.only_block in r} if key == a.stage + '_bank' else {}
                if blocks:
                    vals = {}
                    for b, rows in blocks.items():   # the draw's own block with only the REGISTERS the chain changed set to 42.0 (the neighbours keep their values)
                        cur = f.bank(int(f.draws[d][key])).view('u4').reshape(-1, 4)[4 * b:4 * b + 4].copy()
                        for r in rows:
                            if r in keep:
                                continue
                            if a.only_block < 0 or r == a.only_block:
                                cur[r - 4 * b] = pl.START[key][r] if a.to_start else 0x42280000
                        vals[b] = cur
                    edits.append((d, key, vals))
                    n_blocks += len(blocks)
        if os.path.exists(dst):
            os.remove(dst)
        if not edits:
            os.link(path, dst)
            continue
        with open(dst, 'wb') as fh:
            fh.write(bi.apply_edits(bytes(f.buf), f, edits))
        n_frames += 1
        n_edits += len(edits)
    print('poisoned %d blocks in %d draw banks of %d of %d frames -> %s' % (n_blocks, n_edits, n_frames, len(cap.frames), a.out))
    return 0 if n_edits else 1


if __name__ == '__main__':
    sys.exit(main())

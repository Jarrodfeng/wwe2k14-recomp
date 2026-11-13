r"""Lane 175 (S101): emulate the far cards' THIN record on a capture, offline. Writes a copy of a case zip in which, in every THIN frame, the op-15 draw of every card except the FIRST card of each person type's
loop is REMOVED from the frame's op list. That is exactly what the hook does (card_thin_core.h, card_thin_hook.h): the card's op-21 (c76-c78) is still recorded, so every register, the scene record's feed
and the replay bookkeeping stay the game's; only the 4-index strip draw is not recorded. A capture's draw carries its full constant bank and state, so removing a draw edits nothing else: there is NO
leftover register or texture state to emulate (the check asserts the bank, state and texture sections of every cut frame are byte-identical to the input). Frames are thin or full by the same schedule the gate runs: the first --warmup frames with cards are full,
then every --k-th frame with cards is a proof frame (full), all others thin.

  py -I tools\crowd\card_thin_cut.py CASE.zip OUT.zip [--k 32] [--warmup 3] [--plant NAME] [--decisions OUT.csv]
Plants (tests only):
  drop_first   the first card of each type is dropped too (the kept draw is the trigger, the state source and the proof: a frame with no card draw has none of them)
  keep_all     CONTROL: nothing is cut (the output must equal the input replay)
  no_proof     no proof frame (every frame after the warm-up is thin)
  swap_kept    the kept draw of each type is the SECOND card of the type (a record whose first-of-type check must refuse the frame)
Only the ops section is edited, in place (the table entry's count shrinks). Counts only; the output is a scratch file for under out\ (delete it after the comparison)."""
import argparse
import os
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402


def card_runs(cap, f):
    """-> (ops positions of the card draws in op order, their slot-0 images)."""
    D = f.draws
    if not len(D) or not f.has_origins:
        return [], []
    sl, tf, tc = f.slots, D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
    pos, img = [], []
    for k, (kind, idx) in enumerate(f.ops):
        if int(kind) != 2 or int(f.origins['call_site'][int(idx)]) != cc.SITE_CARD:
            continue
        i0 = 0
        for r in sl[int(tf[idx]):int(tf[idx]) + int(tc[idx])]:
            if int(r['slot']) == 0:
                i0 = int(r['image'])
        pos.append(k)
        img.append(i0)
    return pos, img


def world_sum(f, pos):
    """The hook's world tally of the frame, computed from the capture: the sum (mod 2^64) of FNV-1a over the 12 words c76-c78 of every card draw's bank (the op-21 the game recorded for that card)."""
    tot = 0
    for p in pos:
        d = f.draws[int(f.ops['index'][p])]
        w = f.bank(int(d['vs_bank']))[304:316]
        h = 0xcbf29ce484222325
        for x in w:
            h = ((h ^ int(x)) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        tot = (tot + h) & 0xFFFFFFFFFFFFFFFF
    return tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('out')
    ap.add_argument('--k', type=int, default=32)
    ap.add_argument('--warmup', type=int, default=3)
    ap.add_argument('--plant', choices=['drop_first', 'keep_all', 'no_proof', 'swap_kept'])
    ap.add_argument('--decisions')
    ap.add_argument('--counts', help='sidecar for fview --crowd-cards-thin-counts: hook_frame,seen,dropped per frame with cards')
    a = ap.parse_args()
    cap = cc.Cap(a.case)
    zin = cap.c.zf
    if zin is None:
        print('FAIL  the input must be a case zip')
        return 1
    new_frames, dec, counts = {}, [], {}
    with_cards = thin_frames = dropped = kept_ops = 0
    for fi, name in enumerate(cap.frames):
        f = cap.frame(fi)
        pos, img = card_runs(cap, f)
        if not pos:
            continue
        n_cards = with_cards
        with_cards += 1
        wsum = world_sum(f, pos)
        counts[int(f.guest_frame) - 1] = [len(pos), 0, wsum]   # the hook's frame stamp = the stream frame's - kHookToStreamOffset (1); overwritten below when frames are cut
        proof = n_cards < a.warmup or (a.plant != 'no_proof' and (n_cards - a.warmup) % a.k == 0)
        if proof or a.plant == 'keep_all':
            dec.append((cap.nums[fi], 'full', len(pos), 0))
            continue
        drop = np.zeros(len(f.ops), bool)
        seen = {}
        for p, im in zip(pos, img):
            c = seen.get(im, 0)
            seen[im] = c + 1
            keep = (c == 0) if a.plant != 'swap_kept' else (c == 1)
            if a.plant == 'drop_first':
                keep = False
            if not keep:
                drop[p] = True
        if a.plant == 'swap_kept':   # a type with a single card keeps it (there is no second one)
            for im, c in seen.items():
                if c == 1:
                    drop[[p for p, i2 in zip(pos, img) if i2 == im][0]] = False
        if not drop.any():
            dec.append((cap.nums[fi], 'thin', len(pos), 0))
            continue
        buf = bytearray(f.buf)
        es, cnt, off = f.secs[cc.pcap.SEC['ops']]
        keep_ops = np.frombuffer(bytes(f.buf), np.dtype([('kind', '<u4'), ('index', '<u4')]), cnt, off)[~drop]
        buf[off:off + 8 * len(keep_ops)] = keep_ops.tobytes()
        nsec = struct.unpack_from('<I', buf, 12)[0]
        for k in range(nsec):
            sid = struct.unpack_from('<I', buf, 32 + 24 * k)[0]
            if sid == cc.pcap.SEC['ops']:
                struct.pack_into('<Q', buf, 32 + 24 * k + 8, len(keep_ops))
        new_frames[name] = bytes(buf)
        counts[int(f.guest_frame) - 1] = [len(pos), int(drop.sum()), wsum]
        thin_frames += 1
        dropped += int(drop.sum())
        kept_ops += len(pos) - int(drop.sum())
        dec.append((cap.nums[fi], 'thin', len(pos), int(drop.sum())))
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_STORED, allowZip64=True) as zout:
        for info in zin.infolist():
            zout.writestr(info.filename, new_frames.get(info.filename) or zin.read(info.filename))
    if a.counts:
        with open(a.counts, 'w') as fh:
            fh.write('# hook_frame,card op-15 calls seen,dropped,sum of FNV-1a over the 12 words of every card c76-c78\n')
            for k in sorted(counts):
                fh.write('%d,%d,%d,%d\n' % (k, counts[k][0], counts[k][1], counts[k][2]))
    if a.decisions:
        with open(a.decisions, 'w') as fh:
            fh.write('# frame,kind,card draws before,dropped\n')
            for d in dec:
                fh.write('%d,%s,%d,%d\n' % d)
    print('thin: %d of %d frames with cards cut (%d card draws dropped, %d kept as trigger / state / proof) of %d frames%s -> %s' % (
        thin_frames, with_cards, dropped, kept_ops, len(cap.frames), (' (plant %s)' % a.plant) if a.plant else '', a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())

r"""Lane 155 (S101): emulate the bake skip on a capture, offline. Writes a copy of a case zip in which the ops of every bake pass the policy cuts are REMOVED from the frame's op list (SetTargets, Clear, Draw and Copy of
that thumbnail), exactly the span the game's own skip branch leaves out (sub_827FD698 iteration, bake_skip_core.h). fview replays the copy: the presented-surface hashes are then compared with the original replay.

  py -I tools\crowd\bake_skip_cut.py CASE.zip DECISIONS.csv OUT.zip [--plant NAME]
DECISIONS.csv = `frame,mask` per frame (bake_skip_test.exe --counts ... --decisions: bit t = the policy cuts type t's bake in that frame's pass); `frame` = the capture's frame number.
Only the ops section is edited, in place (the table entry's count shrinks; all other sections, offsets, the draw / copy / clear tables and the geometry are untouched, so nothing else can move).
Plants (tests only): cut_read (also cut the bake of the first type that has a card in the frame: a copy elided but the picture read: the stale-picture class lane 110's cards guard against).
Counts only; the output is a scratch file for under out\ (delete it after the comparison)."""
import argparse
import os
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402
import bake_skip_inherit as bi  # noqa: E402

pcap = cc.pcap


def bake_spans(cap, fi):
    """-> (spans [(first_op, last_op_exclusive)] of the frame's bake passes (1-20: a 15-type arena has 15) in order, n_ops) or ([], n)."""
    f = cap.frame(fi)
    D = f.draws
    n = len(f.ops)
    if not len(D):
        return [], n, f
    veff = cap.effs(D['vs_hash'])
    copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
    passes = cc.walk(f, copies_t, cap)
    starts = [p.pos for p in passes] + [n]
    spans, bps = [], []
    for i, p in enumerate(passes):
        if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws):
            spans.append((p.pos, starts[i + 1]))
            bps.append(p)
    bake_spans.last = (bps if 1 <= len(spans) <= 20 else [], veff)
    return (spans if 1 <= len(spans) <= 20 else []), n, f


def card_types(cap, f):
    """-> set of types (bake position) that have a card draw in the frame."""
    D = f.draws
    veff = cap.effs(D['vs_hash'])
    copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
    dest = []
    for p in cc.walk(f, copies_t, cap):
        if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws) and p.copies:
            dest.append(int(copies_t[p.copies[0][1]]['d']['image']))
    pos = {img: k for k, img in enumerate(dest)}
    out = set()
    sl, tf, tc = f.slots, D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
    for d in np.nonzero(f.origins['call_site'] == cc.SITE_CARD)[0]:
        for r in sl[int(tf[d]):int(tf[d]) + int(tc[d])]:
            if int(r['slot']) == 0 and int(r['image']) in pos:
                out.add(pos[int(r['image'])])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('decisions')
    ap.add_argument('out')
    ap.add_argument('--plant', choices=['cut_read'])
    ap.add_argument('--only-frames', default='', help='A:B, modify only the frames of that range of the capture (test arms)')
    ap.add_argument('--poison-inherit', action='store_true', help='test only (with --inherit-fix): the inherited blocks are set to 42.0 instead of the values the cut leaves')
    ap.add_argument('--inherit-fix', action='store_true', help='also give every later draw the constant registers the cut leaves it (review S2: the banks are the register file of the game, the writes of a cut iteration are gone)')
    a = ap.parse_args()
    masks = {}
    for line in open(a.decisions):
        if line.strip() and not line.startswith('#'):
            k, m = line.split(',')
            masks[int(k)] = int(m)
    cap = cc.Cap(a.case)
    zin = cap.c.zf
    if zin is None:
        print('FAIL  the input must be a case zip')
        return 1
    new_frames = {}
    cut_passes = cut_ops = cut_read_types = 0
    frames_cut = 0
    inherit_draw_banks = 0
    for fi, name in enumerate(cap.frames):
        num = cap.nums[fi]
        mask = masks.get(num, 0)
        if a.only_frames and not (int(a.only_frames.split(':')[0]) <= fi < int(a.only_frames.split(':')[1])):
            continue
        if a.plant == 'cut_read':
            spans0, n0, f0 = bake_spans(cap, fi)
            if spans0:
                ct = sorted(card_types(cap, f0))
                if ct:
                    mask |= 1 << ct[0]
                    cut_read_types += 1
        if not mask:
            continue
        spans, n, f = bake_spans(cap, fi)
        if not spans:
            continue
        drop = np.zeros(n, bool)
        for t in range(len(spans)):
            if mask >> t & 1:
                drop[spans[t][0]:spans[t][1]] = True
                cut_passes += 1
        if not drop.any():
            continue
        buf = bytearray(f.buf)
        es, cnt, off = f.secs[cc.pcap.SEC['ops']]
        keep = np.frombuffer(bytes(f.buf), np.dtype([('kind', '<u4'), ('index', '<u4')]), cnt, off)[~drop]
        buf[off:off + 8 * len(keep)] = keep.tobytes()
        nsec = struct.unpack_from('<I', buf, 12)[0]
        for k in range(nsec):
            sid = struct.unpack_from('<I', buf, 32 + 24 * k)[0]
            if sid == cc.pcap.SEC['ops']:
                struct.pack_into('<Q', buf, 32 + 24 * k + 8, len(keep))
        if a.inherit_fix:
            bps, veff = bake_spans.last
            edits = bi.inherit_edits(cap, f, mask, bps, veff, poison=a.poison_inherit)
            inherit_draw_banks += len(edits)
            if edits:
                buf = bytearray(bi.apply_edits(bytes(buf), f, edits))
        new_frames[name] = bytes(buf)
        cut_ops += int(drop.sum())
        frames_cut += 1
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_STORED, allowZip64=True) as zout:
        for info in zin.infolist():
            zout.writestr(info.filename, new_frames.get(info.filename) or zin.read(info.filename))
    print('cut %d bake passes (%d ops) in %d of %d frames%s%s -> %s' % (cut_passes, cut_ops, frames_cut, len(cap.frames), (', %d draw banks given the registers the cut leaves them' % inherit_draw_banks) if a.inherit_fix else '',
                                                                      (' (plant cut_read: %d frames cut a type that has cards)' % cut_read_types) if a.plant else '', a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())

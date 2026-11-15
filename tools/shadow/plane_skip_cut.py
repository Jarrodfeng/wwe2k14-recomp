r"""S102 lane P3 (offline step): the cut with EVERY plane-chain walk removed, emulated on a capture. Offline, 0 game runs.

  py -I tools\shadow\plane_skip_cut.py CAPTURE_DIR OUT_DIR [--only-frames A:B] [--plant keep_mask]

Writes a copy of a capture FOLDER in which the ops of the plane chain of every frame (the four passes: SetTargets, Clear, every Draw and the Copy of the mask, downscale, blur x and blur z: tools\shadow\plane_chain.py)
are REMOVED from the frame's op list, exactly the span the game's own walker leaves out when the node does not run (sub_827EF070 returning without sub_827EEF80; R4 row 1). Only each frame's ops section is edited, in place
(the table entry's count shrinks); every other section, the draw / copy / clear tables and the geometry are untouched, so nothing else moves. The big files (geometry, textures, images) are HARD-LINKED, not copied; the frames
are rewritten. fview replays the copy: its picture and counts are then compared with the original replay (own_plane_check.py: the arm `cut`).

This is the cut WITHOUT the constant registers the walk leaves behind (the bake's `--inherit-fix` is not reused: the P1 review showed the capture reader drops writes that do not change a block, so that emulation may give
later draws the wrong registers). The register-leftover question for this pass is settled by the census in plane_leftovers.py instead.
Plant keep_mask (tests only): the MASK pass (target, clear, draws, copy) is kept in the cut: the cut is then not 'the node did not run' and must differ from it wherever the mask has content.
Counts only; the output is scratch for under out\ (delete it after the comparison)."""
import argparse
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import plane_chain as pc  # noqa: E402

pcap = pc.sc.pcap


def cut_frame(f, drop):
    """-> the frame file's bytes with the ops where drop[i] is True removed (the ops section edited in place)."""
    buf = bytearray(f.buf)
    es, cnt, off = f.secs[pcap.SEC['ops']]
    keep = np.frombuffer(bytes(f.buf), np.dtype([('kind', '<u4'), ('index', '<u4')]), cnt, off)[~drop]
    buf[off:off + 8 * len(keep)] = keep.tobytes()
    nsec = struct.unpack_from('<I', buf, 12)[0]
    for k in range(nsec):
        sid = struct.unpack_from('<I', buf, 32 + 24 * k)[0]
        if sid == pcap.SEC['ops']:
            struct.pack_into('<Q', buf, 32 + 24 * k + 8, len(keep))
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('out')
    ap.add_argument('--only-frames', default='')
    ap.add_argument('--plant', choices=['keep_mask', 'duplicate_chain'])
    a = ap.parse_args()
    if not os.path.isdir(a.capture):
        print('FAIL  the input must be a capture folder: ' + a.capture)
        return 1
    cap = pc.sc.Cap(a.capture)
    lo, hi = (int(x) for x in a.only_frames.split(':')) if a.only_frames else (0, len(cap.frames))
    os.makedirs(os.path.join(a.out, 'frames'), exist_ok=True)
    for name in os.listdir(a.capture):   # the big files: hard links (no copy; a link is never written to)
        src, dst = os.path.join(a.capture, name), os.path.join(a.out, name)
        if os.path.isfile(src) and not os.path.exists(dst):
            os.link(src, dst)
    cut_frames = cut_ops = cut_passes = 0
    for fi, path in enumerate(cap.frames):
        name = os.path.basename(path)
        dst = os.path.join(a.out, 'frames', name)
        f = cap.c.frame(fi)
        ch, why = pc.find(cap, f) if lo <= fi < hi else (None, '')
        if ch is None:
            if why:
                print('FAIL  frame %d: the chain is irregular (%s)' % (fi, why))
                return 1
            if not os.path.exists(dst):
                os.link(path, dst)
            continue
        if a.plant == 'duplicate_chain':   # review F10c: the chain's op span is inserted a second time right after itself (a second chain in the frame)
            es, cnt, off = f.secs[pcap.SEC['ops']]
            allops = np.frombuffer(bytes(f.buf), np.dtype([('kind', '<u4'), ('index', '<u4')]), cnt, off)
            new = np.concatenate([allops[:ch.end_op], allops[ch.first_op:ch.end_op], allops[ch.end_op:]])
            buf = bytearray(f.buf)
            while len(buf) % 8:
                buf.append(0)
            at = len(buf)
            buf += new.tobytes()
            nsec = struct.unpack_from('<I', buf, 12)[0]
            for k in range(nsec):
                sid = struct.unpack_from('<I', buf, 32 + 24 * k)[0]
                if sid == pcap.SEC['ops']:
                    struct.pack_into('<QQ', buf, 32 + 24 * k + 8, len(new), at)
            if os.path.exists(dst):
                os.remove(dst)
            with open(dst, 'wb') as fh:
                fh.write(bytes(buf))
            cut_frames += 1
            continue
        drop = np.zeros(len(f.ops), bool)
        ops = ch.ops_of()
        if a.plant == 'keep_mask':
            p0 = ch.passes[0]
            for o in [p0['set_op']] + ([p0['clear_op']] if p0['clear_op'] >= 0 else []) + p0['draws'] + [p0['copy_op']]:
                ops.remove(o)
        drop[ops] = True
        if os.path.exists(dst):
            os.remove(dst)
        with open(dst, 'wb') as fh:
            fh.write(cut_frame(f, drop))
        cut_frames += 1
        cut_ops += int(drop.sum())
        cut_passes += 4
    print('cut %d plane-chain passes (%d ops) in %d of %d frames -> %s' % (cut_passes, cut_ops, cut_frames, len(cap.frames), a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())

r"""S102 lane G14: the pre-light face map, read from the captures alone (0 game runs). Evidence for src\renderer_adapter\prelight_direct.h.
  py -I tools\g14\g14_census.py CAPTURE_DIR [CAPTURE_DIR ...]
For EVERY frame of each capture: every copy whose source is a 384x64 image and whose destination is a 64x64 six-layer image is a face copy. Per (strip, cube) pair the six copies must be
  source rect {64 j, 0, 64, 64} -> dest rect {0, 0, 64, 64} layer L, and the (j, L) pairs are collected over all frames: the map must be ONE map (identity), never a flip.
Also: how many 384x64 images are read by any draw's texture slot (the strip's other readers), and by a copy that is not a face copy. Both must be 0.
Prints one summary line per capture and 'MAP identity' / 'MAP NOT identity'. Exit 1 on anything but identity or when a capture cannot be read."""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import numpy as np
import shadow_census as sc


def census(path):
    cap = sc.Cap(path)
    pairs = collections.Counter()      # (tile, layer, srcrect w/h, dstrect) over every face copy
    face_copies = frames_with = 0
    strip_readers = other_copy_readers = 0
    per_frame = collections.Counter()
    strips_seen = set()
    for fi in range(len(cap.frames)):
        f = cap.frame(fi)
        copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
        d = f.draws
        n = 0
        strips = set()
        for c in copies:
            s, dd = int(c['s']['image']), int(c['d']['image'])
            if s not in cap.imgs or dd not in cap.imgs:
                continue
            rs, rd = cap.imgs[s], cap.imgs[dd]
            if (int(rs['w']), int(rs['h'])) == (384, 64) and (int(rd['w']), int(rd['h'])) == (64, 64) and int(rd['layers']) == 6:
                sr, dr = c['sr'].tolist(), c['dr'].tolist()
                pairs[(sr[0] // 64, int(c['d']['layer']), tuple(sr[1:]), tuple(dr), int(c['s']['mip']), int(c['s']['layer']), int(c['d']['mip']))] += 1
                n += 1
                strips.add(s)
            elif (int(rs['w']), int(rs['h'])) == (384, 64):
                other_copy_readers += 1
        strips_seen |= strips
        if n:
            frames_with += 1
            face_copies += n
            per_frame[n] += 1
        if strips and len(d):
            tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
            for i in range(len(d)):
                for sl in f.slots[int(tf[i]):int(tf[i]) + int(tc[i])]:
                    if int(sl['image']) in strips:
                        strip_readers += 1
    return dict(frames=len(cap.frames), frames_with=frames_with, face_copies=face_copies, per_frame=dict(per_frame), pairs=pairs, strip_readers=strip_readers,
                other_copy_readers=other_copy_readers, strips=sorted(strips_seen))


def main():
    if len(sys.argv) < 2:
        print('FAIL usage: g14_census.py CAPTURE_DIR ...')
        return 1
    bad = 0
    maps = 0
    for path in sys.argv[1:]:
        try:
            r = census(path)
        except Exception as e:   # a capture that cannot be read fails loudly
            print('FAIL cannot read %s: %s' % (path, e))
            return 1
        if not r['pairs'] and r['strip_readers'] == 0:   # a capture with no pre-light bake at all (a match whose slot-0 lights never changed): nothing to map, nothing to read
            print('NOTE %s: %d frames, no pre-light face copies in this capture (no map to read)' % (path, r['frames']))
            continue
        identity = bool(r['pairs']) and all(t == l for (t, l, *_rest) in r['pairs']) and {(t, l) for (t, l, *_r) in r['pairs']} == {(i, i) for i in range(6)}
        shapes = {(k[2], k[3], k[4], k[5], k[6]) for k in r['pairs']}
        shape_ok = shapes == {((0, 64, 64), (0, 0, 64, 64), 0, 0, 0)} or all(s[1] == (0, 0, 64, 64) and s[0][1:] == (64, 64) for s in shapes)
        ok = identity and r['strip_readers'] == 0 and r['other_copy_readers'] == 0 and shape_ok
        print('%s %s: %d frames, %d with face copies (%d copies; per-frame copy counts %s), strips %s, tile->layer pairs %s, rect shapes %s, strip read by a draw %d, by another copy %d' % (
            'PASS' if ok else 'FAIL', os.path.basename(os.path.dirname(path.rstrip('/\\'))) or path, r['frames'], r['frames_with'], r['face_copies'], dict(sorted(r['per_frame'].items())), r['strips'],
            sorted({(t, l) for (t, l, *_r) in r['pairs']}), sorted(shapes), r['strip_readers'], r['other_copy_readers']))
        print('MAP identity (tile j -> layer j, no flip)' if identity else 'MAP NOT identity')
        bad += 0 if ok else 1
        maps += 1 if identity else 0
    if not maps:
        print('FAIL no capture held a pre-light face map')
        return 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

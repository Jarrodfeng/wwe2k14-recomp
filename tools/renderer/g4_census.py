r"""Lane G4 (S102): the dead-work census. For every frame of a capture: the crowd card bake passes and the pre-light bake passes, and for each copy
destination of those passes whether anything in the same frame reads it (a draw's texture slot, a copy source) and whether that
reader comes before the pass (reads the previous frame's content).
  py -I tools\renderer\g4_census.py CAPTURE_FOLDER [STEP]
Counts only. Exit 1 when the capture cannot be read."""
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
import cards_census as cc  # noqa: E402
import numpy as np  # noqa: E402

PRELIGHT_CALL = 0x82C20B90
PRELIGHT_PASS = 0x82C20AB8


def classify(f, p):
    if not p.draws or not f.has_origins:
        return None
    ps = set(int(f.origins[d]['pass_site']) for _, d in p.draws)
    cs = set(int(f.origins[d]['call_site']) for _, d in p.draws)
    if ps == {cc.SITE_BAKE_PASS}:
        return 'bake'
    if PRELIGHT_PASS in ps or cs == {PRELIGHT_CALL}:
        return 'prelight'
    return None


def census(cap, step):
    tot = collections.Counter()
    per_img = collections.defaultdict(lambda: [0, 0])
    for fi in range(0, len(cap.frames), step):
        f = cap.frame(fi)
        D = f.draws
        if not len(D):
            continue
        tot['frames'] += 1
        copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
        passes = cc.walk(f, copies_t, cap)
        tot['passes'] += len(passes)
        tot['copies'] += len(copies_t)
        tot['draws'] += len(D)
        sl = f.slots
        tf = D['tex_first'].astype(np.int64)
        tc = D['tex_count'].astype(np.int64)
        pos_of_draw = {}
        reads = collections.defaultdict(list)
        for k, (kind, idx) in enumerate(f.ops):
            kind, idx = int(kind), int(idx)
            if kind == 2:
                for r in sl[int(tf[idx]):int(tf[idx]) + int(tc[idx])]:
                    reads[int(r['image'])].append(k)
            elif kind == 3:
                reads[int(copies_t[idx]['s']['image'])].append(k)
        for p in passes:
            key = classify(f, p)
            if not key:
                continue
            tot[key + '_passes'] += 1
            tot[key + '_draws'] += len(p.draws)
            tot[key + '_copies'] += len(p.copies)
            for _, ci in p.copies:
                dimg = int(copies_t[ci]['d']['image'])
                rd = [pos for pos in reads.get(dimg, []) if pos != p.pos]
                per_img[(key, dimg)][0] += 1
                if not rd:
                    tot[key + '_copies_dead'] += 1
                else:
                    tot[key + '_copies_read'] += 1
                    per_img[(key, dimg)][1] += 1
                    if any(pos < p.pos for pos in rd):
                        tot[key + '_read_before'] += 1
    return tot, per_img


def main():
    path = sys.argv[1]
    step = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 1
    cap = cc.Cap(path)
    tot, per_img = census(cap, step)
    print(json.dumps(dict(tot), sort_keys=True))
    for (key, img), (frames, read) in sorted(per_img.items()):
        print('%s image %d: baked in %d frames, read in %d' % (key, img, frames, read))
    if not tot['frames']:
        print('FAIL no frames')
        sys.exit(1)


if __name__ == '__main__':
    main()

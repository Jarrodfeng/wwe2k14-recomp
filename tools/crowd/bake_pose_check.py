r"""Lane 155 (S101), Part 1: the pose source of the crowd card bake. Offline, 0 game runs.

Question (docs\renderer\CROWD_CARDS_PROMOTION_MAP.md 1.3, was [U]): where do the 20 bake draws' bone matrices (vs constants c76.., 3 rows a bone) come from?
Answer measured here: they are BIT-EXACT COPIES of the scene record's palette matrices of the same person type, from the record numbered like the stream frame
(not j-1, not j+1), bone b of the bake = palette slot perm[type][b], and perm is one fixed permutation per type (the bake model's bone list against the type
skeleton's 17 slots). The bake's pose is therefore the stands' pose: the type skeleton `sub_82509CB8` copies (docs\systems\crowd.md "Card pictures").

  py -I tools\crowd\bake_pose_check.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--emit-perm OUT] [-q]
CAPTURE = a case zip or capture folder that carries scene records (relative paths are read from the main checkout). Exit 1 when a check FAILs or an input cannot be read.
Checks (PASS / FAIL with counts):
  bones     every bone the bake's vertices reference (the u8x4 bone-id element, 255 = none) of bake k is bit-exactly one matrix of record(frame)'s palette of person type k.
  perm      the slot a (type, bone) takes is ONE slot over every frame of a capture (the intersection of the per-frame candidate sets is never empty).
  offset    the record numbered j +-1 / +-2 gives NO exact match for a bone of stream frame j (the pairing is exactly j, measured, not assumed).
  layers    a type's palette is the same array in every LOD group that carries it (the bake may read any).
  coverage  frames used, bakes per frame, matrices compared (a check that compared nothing must fail).
Plants (tests only; each must fail its named check): record_prev (record j-1 used) -> bones; identity_perm (bone b taken as slot b) -> bones; type_shift (bake k read against
type k+1) -> bones; one_frame_perm (a different permutation on one frame) -> perm.
Numbers only; nothing from the game is written (--json / --emit-perm are for under out\)."""
import argparse
import collections
import json
import os
import sys
import time
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402

pcap = cc.pcap
PLANTS = ('record_prev', 'identity_perm', 'type_shift', 'one_frame_perm')
N_SLOTS = 17         # the record's palette: 17 matrices a range (the rig's 17 nodes)
BONE_FMT_U8X4 = 41   # the adapter's Vulkan format number of R8G8B8A8_UINT (the bake layout's bone-id element)


class PoseError(Exception):
    pass


def bone_matrix(bank, b):
    """c76 + 3b + r = column r of the row-vector 4x4 (translation in row 3): -> M[4][4] float32 (the stream's bits)."""
    M = np.zeros((4, 4), '<f4')
    M[3, 3] = 1.0
    for r in range(3):
        M[0:4, r] = bank[4 * (cc.R_WORLD + 3 * b + r):4 * (cc.R_WORLD + 3 * b + r) + 4]
    return M


def palettes(raw):
    """-> {(type, lod_group): ndarray (n,4,4) float32} from the record's crowd objects."""
    magic, ver, nsec = cc.struct.unpack_from('<III', raw, 0)
    if magic != 0x4E435352 or ver != 1:
        raise PoseError('not a scene record v1')
    secs = {}
    for i in range(nsec):
        sid, es, cnt, off = cc.struct.unpack_from('<IIQQ', raw, 40 + 24 * i)
        secs[sid] = (es, cnt, off)
    if 1 not in secs or 4 not in secs or 5 not in secs:
        raise PoseError('scene record without strings / objects / bones')
    st = raw[secs[1][2]:secs[1][2] + secs[1][1]]
    es, cnt, off = secs[4]
    if es != cc.OBJECT_SIZE:
        raise PoseError('Object element size %d, this reader reads %d' % (es, cc.OBJECT_SIZE))
    bes, bcnt, boff = secs[5]
    if bes != 64:
        raise PoseError('bone element size %d' % bes)
    bones = np.frombuffer(raw, '<f4', bcnt * 16, boff).reshape(bcnt, 4, 4)
    out = {}
    for i in range(cnt):
        b = raw[off + es * i:off + es * (i + 1)]
        o = cc.struct.unpack_from('<I', b, 12)[0]
        if not o:
            continue
        nm = st[o - 1:st.index(b'\0', o - 1)]
        if not nm.startswith(b'crowd:'):
            continue
        bf, bc = cc.struct.unpack_from('<II', b, 80)
        if not bc:
            continue
        p = nm.split(b':')
        out[(int(p[2]), int(b[5]))] = bones[bf:bf + bc]
    return out


def bakes(cap, fi):
    """-> list of (bank float32 view, referenced bone ids set) of the frame's bake draws in stream order (bake k = person type k)."""
    f = cap.frame(fi)
    D = f.draws
    if not len(D):
        return []
    veff = cap.effs(D['vs_hash'])
    copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
    out = []
    for p in cc.walk(f, copies_t, cap):
        if p.size == (64, 64) and p.draws and all(veff[d] == cc.EFF_BAKE_VS for _, d in p.draws):
            d = p.draws[0][1]
            r = D[d]
            lay = f.layouts[int(r['layout'])]
            els = f.elements[int(lay['first']):int(lay['first']) + int(lay['count'])]
            ide = [int(e['off']) for e in els if int(e['fmt']) == BONE_FMT_U8X4]
            if len(ide) != 1:
                raise PoseError('frame %d: the bake layout has %d bone-id elements' % (fi, len(ide)))
            vc, st = int(r['vertex_count']), int(lay['stride'])
            blob = np.frombuffer(bytes(cap.geo[int(r['vertex_offset']):int(r['vertex_offset']) + int(r['vertex_bytes'])]), 'u1').reshape(vc, st)
            ids = blob[:, ide[0]:ide[0] + 4]
            used = sorted(set(int(x) for x in ids[ids != 255].ravel()))
            out.append((f.bank(int(r['vs_bank'])).copy().view('<f4'), used))
    return out


def measure(cap, frames, plant=None):
    m = collections.Counter()
    cand = {}                      # (type, bone) -> set of slots every frame so far allowed
    perm_frames = collections.defaultdict(collections.Counter)
    layers_diff = 0
    layer_groups = 0
    off_hits = collections.Counter()
    ids_seen = set()
    rng_swap = None
    for fi in frames:
        num = cap.nums[fi]
        raw = cap.record(num)
        if raw is None:
            m['frames_no_record'] += 1
            continue
        use_num = num - 1 if plant == 'record_prev' else num
        raw_used = cap.record(use_num) if use_num != num else raw
        if raw_used is None:
            m['frames_no_record'] += 1
            continue
        B = bakes(cap, fi)
        if not B:
            m['frames_no_bake'] += 1
            continue
        if len(B) > 20:
            raise PoseError('frame %d: %d bakes' % (fi, len(B)))
        P = palettes(raw_used)
        m['frames'] += 1
        m['bakes'] += len(B)
        by_type = collections.defaultdict(list)
        for (ty, lod), arr in P.items():
            by_type[ty].append(arr)
        for ty, arrs in by_type.items():
            layer_groups += 1
            if any(a.shape != arrs[0].shape or not np.array_equal(a.view('u4'), arrs[0].view('u4')) for a in arrs[1:]):
                layers_diff += 1
        for k, (bank, used) in enumerate(B):
            ty = k + 1 if plant == 'type_shift' else k
            if ty not in by_type:
                m['bake_type_without_palette'] += 1     # the type had no member this frame: nobody reads this picture (CROWD_CARDS_PROMOTION_MAP 1.3 readers)
                continue
            pal = by_type[ty][0]
            pv = pal.view('u4').reshape(len(pal), 16)
            for b in used:
                ids_seen.add(b)
                m['matrices'] += 1
                M = bone_matrix(bank, b)
                eq = np.nonzero((pv == M.view('u4').reshape(1, 16)).all(axis=1))[0]
                if plant == 'identity_perm':   # bone b taken as palette slot b: wrong wherever the bake's bone order is not the palette's
                    eq = np.array([b], int) if b < len(pal) and bool((pv[b] == M.view('u4').reshape(16)).all()) else np.array([], int)
                if plant == 'one_frame_perm' and fi == frames[len(frames) // 2] and len(eq):
                    eq = np.array([(int(eq[0]) + 1) % len(pal)], int)
                if not len(eq):
                    m['bones_unmatched'] += 1
                    continue
                m['bones_matched'] += 1
                key = (ty, b)
                s = set(int(x) for x in eq)
                if key not in cand:
                    cand[key] = s
                else:
                    inter = cand[key] & s
                    if not inter:
                        m['perm_broken'] += 1
                        perm_frames[key][fi] += 1
                    else:
                        cand[key] = inter
        # the offset check: the neighbours' records must not give exact matches (the record chosen is j exactly)
        if plant is None and fi % 7 == 0:
            for off in (-2, -1, 1):
                r2 = cap.record(num + off)
                if r2 is None:
                    continue
                P2 = palettes(r2)
                by2 = collections.defaultdict(list)
                for (ty, lod), arr in P2.items():
                    by2[ty].append(arr)
                for k, (bank, used) in enumerate(B[:20]):
                    if k not in by2 or k not in by_type:
                        continue
                    pv = by2[k][0].view('u4').reshape(len(by2[k][0]), 16)
                    for b in used:
                        M = bone_matrix(bank, b)
                        off_hits[off] += int((pv == M.view('u4').reshape(1, 16)).all(axis=1).any())
                        m['offset_tested_%+d' % off] += 1
    perm = {}
    ambiguous = 0
    for (ty, b), s in cand.items():
        if len(s) == 1:
            perm.setdefault(ty, {})[b] = next(iter(s))
        else:
            ambiguous += 1
            perm.setdefault(ty, {})[b] = sorted(s)
    return dict(m=m, perm=perm, ambiguous=ambiguous, layers_diff=layers_diff, layer_groups=layer_groups, off_hits=off_hits, ids_seen=sorted(ids_seen),
                perm_frames=perm_frames)


class Checks:
    def __init__(self, quiet=False):
        self.rows, self.quiet = [], quiet

    def add(self, label, name, ok, text):
        self.rows.append((label, name, ok, text))
        print('%s  %-9s %-9s %s' % ('PASS' if ok else 'FAIL', label, name, text), flush=True)


def evaluate(label, R, ck, plant):
    m = R['m']
    tot = m['bones_matched'] + m['bones_unmatched']
    ck.add(label, 'coverage', m['frames'] > 0 and m['matrices'] > 0 and m['bakes'] >= 20 * m['frames'] * 0.9,
           '%d frames with records and bakes (%d without a record, %d without a bake), %d bakes, %d bone matrices compared (bone ids referenced: %s); %d bakes of a type with no palette in the record (nobody reads that picture)'
           % (m['frames'], m['frames_no_record'], m['frames_no_bake'], m['bakes'], m['matrices'], ','.join(map(str, R['ids_seen'])), m['bake_type_without_palette']))
    ck.add(label, 'bones', tot > 0 and m['bones_unmatched'] == 0,
           '%d of %d referenced bone matrices are bit-exact (float32 bits) matrices of the record palette of their type; unmatched %d' % (m['bones_matched'], tot, m['bones_unmatched']))
    ck.add(label, 'perm', m['perm_broken'] == 0 and m['bones_matched'] > 0,
           'one slot per (type, bone) over all frames: %d (type, bone) pairs, %d with more than one equal slot all frames (identical matrices), %d frames broke a pair' % (
               sum(len(v) for v in R['perm'].values()), R['ambiguous'], m['perm_broken']))
    if plant is None:
        offs = ', '.join('%+d: %d of %d' % (o, R['off_hits'][o], m['offset_tested_%+d' % o]) for o in (-2, -1, 1))
        ok = all(m['offset_tested_%+d' % o] > 0 and R['off_hits'][o] == 0 for o in (-2, -1, 1))
        ck.add(label, 'offset', ok, 'record j+-k, bone matrices with an exact match (sampled every 7th frame): %s; the record numbered j gives all of them' % offs)
    ck.add(label, 'layers', R['layer_groups'] > 0 and R['layers_diff'] == 0,
           '%d (frame, type) palette sets: %d differ between the LOD groups that carry the type (expected 0: the record holds one palette per type)' % (R['layer_groups'], R['layers_diff']))


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    lo, hi, step = (int(x) if x else None for x in (spec.split(':') + ['', ''])[:3])
    return list(range(lo or 0, min(hi if hi is not None else total, total), step or 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--json')
    ap.add_argument('--emit-perm')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    ck = Checks(a.q)
    bad_input, out, perms = False, {}, {}
    for spec in a.captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        t0 = time.time()
        try:
            cap = cc.Cap(path)
            frames = parse_frames(a.frames, len(cap.frames))
            R = measure(cap, frames, a.plant)
        except (pcap.PError, cc.CensusError, PoseError, OSError, KeyError, IndexError, zipfile.BadZipFile) as e:
            print('FAIL  %-9s input     cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        evaluate(label, R, ck, a.plant)
        perms[label] = R['perm']
        out[label] = dict(counts=dict(R['m']), perm=R['perm'])
        print('      %-9s read %d frames in %.1f s' % (label, R['m']['frames'], time.time() - t0))
    if len(perms) > 1 and a.plant is None:
        labels = sorted(perms)
        base = perms[labels[0]]
        for lb in labels[1:]:
            both = [(t, b) for t in base for b in base[t] if t in perms[lb] and b in perms[lb][t]]
            same = sum(1 for t, b in both if base[t][b] == perms[lb][t][b])
            ck.add('%s~%s' % (labels[0], lb), 'permsame', bool(both) and same == len(both), 'the same slot for %d of %d (type, bone) pairs both captures hold (one arena: the permutation is the model\'s, not the scene\'s)' % (same, len(both)))
    if a.emit_perm:
        json.dump({lb: {str(t): {str(b): s for b, s in sorted(v.items())} for t, v in sorted(p.items())} for lb, p in perms.items()}, open(a.emit_perm, 'w'), indent=1)
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1, sort_keys=True)
    fails = [r for r in ck.rows if not r[2]]
    print('bake_pose_check: %d checks, %d FAIL%s' % (len(ck.rows), len(fails), ('  (plant ' + a.plant + ')') if a.plant else ''))
    return 1 if (fails or bad_input) else 0


if __name__ == '__main__':
    sys.exit(main())

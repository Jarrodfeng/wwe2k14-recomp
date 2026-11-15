r"""S102 P1: which constant registers of the crowd card bake's draws change, and with what (numbers only, offline, 0 game runs): the input list P0 must give the owned bake.

  py -I tools\crowd\bake_inputs_census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--frames A:B:STEP]
For each capture: over the frames' bake draws (the 64x64 AtlasTarget passes with ONE draw of the bake program), the vertex- and pixel-bank registers (vec4) that
  const    never change in the capture (the camera, material constants: these are constants the owned bake can hard-code or read once)
  frame    change from frame to frame but are EQUAL in all 20 passes of a frame (the light snapshot: ONE set per frame, copied 20 times by sub_827FD1D8)
  type     differ between the passes of a frame (the pose: VS c76 + 3 * bone, a bone-id range per type)
Prints the register ranges. The classification is what bake_pose_check.py / cards_census.py proved for the pose and the lights, counted here on every register of the bank."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402


def ranges(regs):
    out, s, p = [], None, None
    for r in sorted(regs):
        if s is None:
            s = p = r
        elif r == p + 1:
            p = r
        else:
            out.append((s, p)), (s := r), (p := r)
    if s is not None:
        out.append((s, p))
    return ', '.join('c%d' % a if a == b else 'c%d-c%d' % (a, b) for a, b in out) or '-'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--frames', default='')
    a = ap.parse_args()
    for spec in a.captures:
        label, path = spec.split('=', 1)
        cap = cc.Cap(path)
        fr = range(len(cap.frames))
        if a.frames:
            x = [int(v) for v in a.frames.split(':')]
            fr = [i for i in range(x[0], x[1], x[2] if len(x) > 2 else 1) if i < len(cap.frames)]
        vs_all, ps_all, per_frame_same, per_pass_diff = [], [], [], []
        npass = set()
        for fi in fr:
            f = cap.frame(fi)
            D = f.draws
            if not len(D):
                continue
            veff = cap.effs(D['vs_hash'])
            ct = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
            vb, pb = [], []
            for p in cc.walk(f, ct, cap):
                if p.size == (64, 64) and len(p.draws) == 1 and veff[p.draws[0][1]] == cc.EFF_BAKE_VS:
                    d = D[p.draws[0][1]]
                    vb.append(f.bank(int(d['vs_bank'])).copy().reshape(256, 4))
                    pb.append(f.bank(int(d['ps_bank'])).copy().reshape(256, 4))
            if not vb:
                continue
            npass.add(len(vb))
            vs_all.append(np.stack(vb)), ps_all.append(np.stack(pb))
        V, P = np.stack(vs_all), np.stack(ps_all)   # frames x passes x 256 x 4
        for nm, B in (('vertex', V), ('pixel', P)):
            const = np.all(B == B[0, 0], axis=(0, 1, 3))
            same_in_frame = np.all(B == B[:, :1], axis=(1, 3))            # frames x 256: equal across the passes of the frame
            frame_set = np.all(same_in_frame, axis=0) & ~const            # equal in every frame's passes, but not constant: the per-frame set
            type_set = ~np.all(same_in_frame, axis=0)                      # differs between passes in some frame
            # a type register that is the same for a given pass index in all frames (per-type constants) vs changing per frame
            per_type_const = np.all(B == B[:1], axis=(0, 3)).all(axis=0) & type_set
            print('%s %s bank (%d frames x %d passes): constant in the whole capture %d regs; the same in all passes of a frame but changing per frame %d regs [%s]; differing between passes %d regs [%s], of which constant per pass slot over the frames %d regs [%s]' % (
                label, nm, B.shape[0], B.shape[1], int(const.sum()), int(frame_set.sum()), ranges(np.nonzero(frame_set)[0]), int(type_set.sum()), ranges(np.nonzero(type_set)[0]),
                int(per_type_const.sum()), ranges(np.nonzero(per_type_const)[0])))
    return 0


if __name__ == '__main__':
    sys.exit(main())

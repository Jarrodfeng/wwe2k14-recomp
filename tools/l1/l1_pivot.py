r"""S102 lane L1 (review F6): is the response curve continuous at each lobe's pivot, from the captured constants? (0 game runs)
  py -I tools\l1\l1_pivot.py CAPTURE_DIR [FRAME ...]
For every pre-light draw of the chosen frames (default: every 10th frame) and both lobes: the lobe's value just below and just above the pivot (d = pivot -/+ 1e-4, and one cube texel's angle,
2 / 64 in d: the 64x64 face's texel pitch is about 1/32 of a radian near the face centre), with the same lines as fx_prelight.glsl. A step at the pivot would show as |lobe(pivot+) - lobe(pivot-)| > 0 as
the offset goes to 0; the report is the largest such value over every draw and the lobe's value one texel either side. Exit 1 on an unreadable capture, 1 when the step exceeds 0.01."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import numpy as np
import shadow_census as sc

PRE = 0x82C20B90


def lobe(d, tune, knots, shape):
    folded = tune[2] - np.clip(abs(d - tune[3]) + tune[3], 0.0, 1.0)
    p = -(np.sqrt(abs(shape[1] - 4.0 * tune[0] * folded)) + tune[1]) * shape[2]
    t = p
    curve = (1 - t) * ((1 - t) * knots[0] + t * knots[1]) + t * ((1 - t) * knots[1] + t * knots[2])
    return np.sign(d - tune[3]) * (curve - 0.5)


def main():
    if len(sys.argv) < 2:
        print('FAIL usage: l1_pivot.py CAPTURE_DIR [FRAME ...]')
        return 1
    cap = sc.Cap(sys.argv[1])
    frames = [int(x) for x in sys.argv[2:]] or list(range(0, len(cap.frames), 10))
    worst = 0.0
    seen = set()
    texel = 2.0 / 64.0
    for fi in frames:
        f = cap.frame(fi)
        d = f.draws
        cs = sc.pcap.queue_site(f.origins)
        for i in [i for i in range(len(d)) if int(cs[i]) == PRE]:
            v = f.bank(int(d[i]['ps_bank'])).view('<f4').reshape(-1, 4)
            for name, base in (('A', 204), ('B', 207)):
                tune, knots, shape = v[base].astype(np.float64), v[base + 1].astype(np.float64), v[base + 2].astype(np.float64)
                key = (name,) + tuple(np.round(tune, 4)) + tuple(np.round(shape, 4))
                lo, hi = lobe(tune[3] - 1e-4, tune, knots, shape), lobe(tune[3] + 1e-4, tune, knots, shape)
                step = abs(float(hi) - float(lo))
                worst = max(worst, step)
                if key not in seen:
                    seen.add(key)
                    print('frame %d lobe %s: pivot %.4f, curve parameter at the pivot %.5f, lobe just below %.6f just above %.6f (step %.6f); one texel below %.4f above %.4f' % (
                        fi, name, tune[3], -(np.sqrt(abs(shape[1] - 4.0 * tune[0] * (tune[2] - np.clip(tune[3], 0, 1)))) + tune[1]) * shape[2], lo, hi, step,
                        lobe(tune[3] - texel, tune, knots, shape), lobe(tune[3] + texel, tune, knots, shape)))
    print('largest step at a pivot over %d frames: %.6f' % (len(frames), worst))
    return 0 if worst <= 0.01 else 1


if __name__ == '__main__':
    sys.exit(main())

r"""S102 lane L2: structural lints of the plane bake (review F1, F7, F11) and the solve's unit test. 0 game runs.

  py -I tools\shadow\plane_bake_lint.py [--root DIR]

Text requirements (each also run against a copy with the requirement removed, which must FAIL: the planted twins):
  F1   an own draw whose shader reads a camera declares blend ranges: the composite's camera rows live in the VERTEX block (rdr_plane_bake.vert), the pixel block has none, and the hook declares
       `blend[0] = {16, 48}` for the composite (the 120 Hz pump interpolates only vertex-constant ranges)
  F7   the pixel stage requires a positive clip w at the solved ground point
  F11  the shadow-set observer is not fed by a pass of the bake (`!pb_dyn_on_`)
Unit test of the solve (F7): the composite finds the ground point that projects to a map pixel by a 2x2 solve of M = PlaneInv * CameraViewProj. A camera at mat height (w can be 0 or negative on the plane) and a point BEHIND the
camera must be rejected; the planted solve without the w test accepts the point behind."""
import argparse
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

REQS = [
    ('F1 vertex block holds the camera rows', 'src/renderer/shaders/rdr_plane_bake.vert', r'vec4 RowX;[\s\S]*?vec4 RowW;'),
    ('F1 pixel block holds no camera rows', 'src/renderer/shaders/rdr_plane_bake.frag', r'vec4 Unused0;'),
    ('F1 the hook declares the composite\'s blend range', 'src/renderer_scene/plane_shadow_ours.cpp', r'out\.own\.blend\[0\] = \{16, 48\}, out\.own\.blend_count = 1'),
    ('F7 positive w at the solved point', 'src/renderer/shaders/rdr_plane_bake.frag', r'w_at > 0\.0'),
    ('F11 the shadow-set observer skips the bake\'s passes', 'src/renderer_adapter/binder.h', r'!pb_dyn_on_\)\s*//\s*S12'),
]


def solve(M, py, a, b, check_w=True):
    """The pixel stage's solve for the ground point (x, z) whose projection is (a, b); None when rejected."""
    cw = M[3][1] * py + M[3][3]
    A = np.array([[M[0][0] - a * M[3][0], M[0][2] - a * M[3][2]], [M[1][0] - b * M[3][0], M[1][2] - b * M[3][2]]])
    c = np.array([M[0][1] * py + M[0][3] - a * cw, M[1][1] * py + M[1][3] - b * cw])
    det = A[0][0] * A[1][1] - A[0][1] * A[1][0]
    if abs(det) < 1e-12:
        return None
    x = (A[0][1] * c[1] - c[0] * A[1][1]) / det
    z = (c[0] * A[1][0] - A[0][0] * c[1]) / det
    w = M[3][0] * x + M[3][2] * z + cw
    if check_w and not w > 0:
        return None
    return x, z


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    root = ap.parse_args().root
    fails = 0
    for name, path, rx in REQS:
        text = open(os.path.join(root, path), encoding='utf-8').read()
        ok = re.search(rx, text) is not None
        planted = re.sub(rx, '', text, count=1)
        caught = re.search(rx, planted) is None and planted != text
        print(('PASS  ' if ok else 'FAIL  ') + name)
        print(('PASS  ' if caught else 'FAIL  ') + '  planted twin (requirement removed): ' + ('flagged' if caught else 'DID NOT APPLY / not flagged'))
        fails += (not ok) + (not caught)
    # unit test: a perspective camera at height 100 over a plane at y = 0, looking along +z; the plane's near part lies BEHIND the camera
    f = 1.0
    near, far = 1.0, 1000.0
    P = np.array([[f, 0, 0, 0], [0, f, 0, 0], [0, 0, far / (far - near), -far * near / (far - near)], [0, 0, 1, 0]])
    V = np.eye(4)
    V[1][3] = -100.0   # camera at y = 100
    M = P @ V
    ahead = np.array([10.0, 0.0, 200.0, 1.0])
    behind = np.array([10.0, 0.0, -200.0, 1.0])
    for label, pt, want in (('point ahead', ahead, True), ('point behind the camera', behind, False)):
        v = M @ pt
        a, b = v[0] / v[3], v[1] / v[3]
        got = solve(M, 0.0, a, b)
        okp = (got is not None) == want and (not want or (abs(got[0] - pt[0]) < 1e-6 and abs(got[1] - pt[2]) < 1e-6))
        got_planted = solve(M, 0.0, a, b, check_w=False)
        planted_wrong = (got_planted is not None) and not want
        print(('PASS  ' if okp else 'FAIL  ') + 'solve: ' + label + ' -> ' + ('accepted' if got else 'rejected'))
        if not want:
            print(('PASS  ' if planted_wrong else 'FAIL  ') + '  planted solve without the w test accepts the point behind the camera')
            fails += not planted_wrong
        fails += not okp
    # camera exactly at mat height: every plane point has w = distance along the view axis >= 0; the horizon line (w = 0) is rejected
    V2 = np.eye(4)
    M2 = P @ V2   # camera at y = 0 = the plane height
    got = solve(M2, 0.0, 0.0, 0.0)
    print(('PASS  ' if got is None else 'FAIL  ') + 'solve: camera at mat height, the view centre (a point at infinity) is rejected, not solved')
    fails += got is not None
    print('plane_bake_lint: %d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

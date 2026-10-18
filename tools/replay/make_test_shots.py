"""Session 16 item 2: two synthetic CineCam shot paths for the re-shoot check (our camera moves, no game data).

    py tools/replay/make_test_shots.py [--frames 400]

Writes out/harness/cinecam/paths/s16_shotA.txt (a sideways dolly past the ring from the hard camera's default spot,
FOV 44.7) and s16_shotB.txt (a push-in towards the ring with a slow pan and a tighter FOV 28), in the path format of
src/cinecam.h (rel x y z yaw pitch roll fov dof...). Both start from the hard camera pose seen in the S14 paths
(-1, -268, -1294, yaw -90, pitch 1.3).
"""
import argparse
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, 'out', 'harness', 'cinecam', 'paths')
DOF = '0 0 1000.000 100.000 500.000 180.000 4500.000 0.0009000'


def write(name, n, pose):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    with open(path, 'w', newline='\n') as f:
        f.write('# wwe2k14 cinecam path v1 anchor=any run=0 (synthetic: tools/replay/make_test_shots.py)\n')
        f.write('# rel x y z yaw pitch roll fov dof_mode dof_manual dof_focus d0 d1 d2 d3 strength\n')
        for i in range(n):
            t = i / max(1, n - 1)
            x, y, z, yaw, pitch, fov = pose(t)
            f.write('%d %.4f %.4f %.4f %.5f %.5f %.5f %.5f %s\n' % (i, x, y, z, yaw, pitch, 0.0, fov, DOF))
    print(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frames', type=int, default=400)
    a = ap.parse_args()
    write('s16_shotA.txt', a.frames, lambda t: (-400 + 800 * t, -268.0, -1294.0, -90.0, 1.3, 44.7))
    write('s16_shotB.txt', a.frames, lambda t: (-1.0, -268.0, -1294.0 + 450 * t, -90.0 + 12 * t, 1.3, 28.0))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
r"""S103 L8: scan capture folders for the frames that carry the stage light-map page pass (a full-page 512x512 quad sprite draw followed by sprite draws, copied to the page image).
  py -I tools\scene\l8_scan.py CAPDIR [CAPDIR...]
Prints, per capture, the frames that hold the pass (first and last frame number of each run of frames) and the base page image id + provenance. Numbers only."""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'solver'))
import capread as cr  # noqa: E402

FULL = np.array([0.0, 0.0, 1.0, 0.0, 0.0, 512.0, 0.0, 1.0, 1.0, 0.0, 512.0, 512.0, 1.0, 1.0, 1.0, 0.0, 512.0, 1.0, 0.0, 1.0], '<f4')


def page_pass(f, geo):
    """The draw index of a full-page quad (the pass's first draw), or -1."""
    for i, d in enumerate(f.draws):
        if d['vertex_count'] == 4 and d['vertex_bytes'] == 80 and d['tex_count'] == 1:
            o = int(d['vertex_offset'])
            v = np.frombuffer(bytes(geo[o:o + 80]), '<f4')
            if np.array_equal(v, FULL):
                return i
    return -1


def main():
    for arg in sys.argv[1:]:
        cap = Path(arg)
        frames = sorted((cap / 'frames').glob('frame_*.bin'))
        geo = np.memmap(cap / 'geometry.bin', dtype='u1', mode='r')
        runs = []
        for p in frames:
            f = cr.Frame(p.read_bytes())
            i = page_pass(f, geo)
            if i >= 0:
                d = f.draws[i]
                img = int(f.slots[d['tex_first']]['image'])
                n = 0
                j = i
                while j < len(f.draws) and f.draws[j]['vertex_count'] == 4 and f.draws[j]['vertex_bytes'] == 80:
                    n += 1
                    j += 1
                key = (img, n)
                if runs and runs[-1][2] == key and runs[-1][1] == f.capture_frame - 1:
                    runs[-1][1] = f.capture_frame
                else:
                    runs.append([f.capture_frame, f.capture_frame, key, i])
        print(cap.name, 'frames', len(frames), 'page pass runs (first, last, (base image, sprites), draw index):', runs if runs else 'NONE')


if __name__ == '__main__':
    main()

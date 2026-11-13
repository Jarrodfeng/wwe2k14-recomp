r"""Lane 136 (SH3 slice 1): the circle blob shadows' capture-side oracle dump. Offline, 0 game runs.

  py -I tools\shadow\circles_oracle.py dump CAPTURE OUT.circ [--frames A:B:STEP]
  py -I tools\shadow\circles_oracle.py info FILE.circ

Reads every frame of a capture (a case zip, a case folder or a capture folder) and writes, for each frame, the stream's circle draws (call site 0x828131E4, in draw order) as the vertex
constants they were drawn with: c0 ShadowParam, c4-c7 ViewProj, c8-c28 WorldPosArray (29 rows of 4 floats; c1-c3 zero), plus the disc mesh once (the first circle draw's vertex bytes in the
stream's host layout and its indices). It also checks, from OTHER draws of the same frame, the one constant the circle does not own: ViewProj = Trapezoidal x ViewProj of the props map's
light (relative error reported, limit 2e-6; docs\systems\shadows.md 7). The C++ side (src\renderer_scene\circle_oracle.h) turns this file into guest memory, drives the shipped writer's tracker
over it and compares the record it produced with these rows (tools\shadow\circles_unit.cpp); fview plays the same entries through the circle hook (--circles-oracle).

File (little endian): u32 'CIRC' 0x43524943, u32 version 1, u32 mesh_vertices, u32 mesh_indices, vertex bytes, u16 indices, u32 nframes, per frame { u32 capture_frame, u32 guest_frame,
u32 ndraws, ndraws x 29 x 4 f32 }. Numbers and constants only: nothing here is committed (outputs go under out\)."""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

MAGIC, VERSION = 0x43524943, 1


def frames_of(spec, n):
    if not spec:
        return list(range(n))
    a = [int(x) if x else None for x in spec.split(':')]
    a += [None] * (3 - len(a))
    return list(range(a[0] or 0, min(n, a[1] if a[1] is not None else n), a[2] or 1))


def dump(capture, out_path, spec=''):
    cap = sc.Cap(capture)
    frames = frames_of(spec, len(cap.frames))
    mesh = None
    body = bytearray()
    nf = 0
    worst_vp = 0.0
    n_draws = 0
    for fi in frames:
        f = cap.c.frame(fi)
        if not f.has_origins:
            continue
        d = f.draws
        o = f.origins
        cs = sc.pcap.queue_site(o)
        idx = np.nonzero(cs == sc.CALL_CIRCLE)[0]
        rows_all = []
        for i in idx:
            dr = d[i]
            vb = f.bank(int(dr['vs_bank']))
            rows = np.zeros((29, 4), np.float32)
            rows[0] = sc.regs(vb, 0, 1)[0]
            rows[4:8] = sc.regs(vb, 4, 4)
            rows[8:29] = sc.regs(vb, 8, 21)
            rows_all.append(rows)
            if mesh is None:
                stride, els = sc.layout_of(f, dr)
                if stride != 32 or int(dr['vertex_count']) != 128 or int(dr['index_count']) != 192 or int(dr['index_32bit']):
                    raise SystemExit('%s frame %d: the circle draw is not the 128-vertex 32-byte mesh (stride %d, %d vertices, %d indices)' % (capture, fi, stride, int(dr['vertex_count']), int(dr['index_count'])))
                vtx = np.ascontiguousarray(sc.vertex_rows(cap, dr, 32)).tobytes()
                ioff, ib = int(dr['index_offset']), int(dr['index_bytes'])
                ind = np.array(cap.c.geo[ioff:ioff + ib]).astype(np.uint8).tobytes()
                mesh = (vtx, ind)
        # the independent check: circle c4 = Trapezoidal x ViewProj of the props light (another draw's constants)
        if len(idx):
            pj = next((j for j in range(len(d)) if int(o[j]['pass_site']) == sc.SITE_MAP and float(abs(sc.regs(f.bank(int(d[j]['vs_bank'])), sc.R_VIEWPROJ, 4)[3][1] - 1.0)) < 1e-6), None)
            if pj is not None:
                vb = f.bank(int(d[pj]['vs_bank']))
                tv = sc.regs(vb, sc.R_TRAP, 4).astype(np.float64) @ sc.regs(vb, sc.R_VIEWPROJ, 4).astype(np.float64)
                for rows in rows_all:
                    err = float(np.abs(rows[4:8].astype(np.float64) - tv).max() / max(np.abs(tv).max(), 1e-9))
                    worst_vp = max(worst_vp, err)
        body += struct.pack('<III', fi, int(f.guest_frame) & 0xFFFFFFFF, len(rows_all))
        for rows in rows_all:
            body += rows.astype('<f4').tobytes()
        nf += 1
        n_draws += len(rows_all)
    if mesh is None:
        raise SystemExit('%s: no circle draw in %d frames (a capture without draw origins, or another scene)' % (capture, len(frames)))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as fh:
        fh.write(struct.pack('<IIII', MAGIC, VERSION, len(mesh[0]) // 32, len(mesh[1]) // 2))
        fh.write(mesh[0])
        fh.write(mesh[1])
        fh.write(struct.pack('<I', nf))
        fh.write(bytes(body))
    print('circles_oracle: %s: %d frames, %d circle draws, mesh %d vertices / %d indices; ViewProj = Trapezoidal x ViewProj (props light) worst relative error %.2e %s' %
          (capture, nf, n_draws, len(mesh[0]) // 32, len(mesh[1]) // 2, worst_vp, 'OK' if worst_vp < 2e-6 else 'FAIL'))
    return 0 if worst_vp < 2e-6 else 1


def info(path):
    b = open(path, 'rb').read()
    magic, ver, nv, ni = struct.unpack_from('<IIII', b, 0)
    if magic != MAGIC or ver != VERSION:
        raise SystemExit('%s: not a circles oracle file' % path)
    at = 16 + nv * 32 + ni * 2
    nf = struct.unpack_from('<I', b, at)[0]
    print('%s: mesh %d / %d, %d frames, %d bytes' % (path, nv, ni, nf, len(b)))
    return 0


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'dump':
        spec = ''
        if '--frames' in a:
            spec = a[a.index('--frames') + 1]
        sys.exit(dump(a[1], a[2], spec))
    if len(a) == 2 and a[0] == 'info':
        sys.exit(info(a[1]))
    print(__doc__)
    sys.exit(2)

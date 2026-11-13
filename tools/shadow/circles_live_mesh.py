r"""Lane 205 (check K3): the circle disc mesh against LIVE bytes. Offline, 0 game runs.

  py -I tools\shadow\circles_live_mesh.py dump CASE_ZIP OUT.lmesh

The first circle oracle took the mesh from the stream's host bytes and built its fake guest with the same byte rule the writer uses, so a wrong rule passed (K3: the u8x4 point-id word was kept in memory
order, 0 of 544 live circle draws equal). This reads the two things that are NOT made by that rule from a live case zip: the record's mesh (section 17 of capture\scene\frame_*.rsc, written by the
writer under test) and the stream's circle draws' vertex bytes (call site 0x828131E4). It prints, per draw, whether the stream's bytes equal the record's, and whether they equal the record's with the id word (word 4)
reversed (the verdict that tells an OLD record, written with the id word kept, from a NEW one), and writes the file circles_unit.exe --live-mesh reads:
  u32 'LMSH', u32 version 1, u32 vertices, u32 indices, record vertex bytes, stream vertex bytes (the first circle draw), u32 draws, u32 draws_equal_first (every circle draw's bytes equal the first's).
Numbers and bytes of the circle disc only (a fan-made-from-the-game's-files mesh of 128 vertices): written under out\, never committed."""
import os
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

SEC_MESH = 17


def record_mesh(zf):
    """The newest record in the zip that holds a mesh: (name, vertex bytes, index bytes)."""
    names = sorted((n for n in zf.namelist() if n.endswith('.rsc')), reverse=True)
    for n in names:
        b = zf.read(n)
        if len(b) < 40 or b[:4] != b'RSCN':
            continue
        count = struct.unpack_from('<I', b, 8)[0]
        for i in range(count):
            sid, esz, cnt, off = struct.unpack_from('<IIQQ', b, 40 + 24 * i)
            if sid == SEC_MESH and cnt == 1:
                nv, ni, stride, _ = struct.unpack_from('<IIII', b, off)
                v = b[off + 16:off + 16 + nv * stride]
                return n, bytes(v), bytes(b[off + 16 + 4096:off + 16 + 4096 + ni * 2]), nv, ni
    return None


def id_word_reversed(b):
    """The record with word 4 of every vertex (the u8x4 point ids) byte-reversed: what an OLD record (id word kept in memory order) needs to equal the stream."""
    a = np.frombuffer(b, '<u4').copy().reshape(-1, 8)
    a[:, 4] = a[:, 4].byteswap()
    return a.tobytes()


def dump(zip_path, out_path):
    zf = zipfile.ZipFile(zip_path)
    rec = record_mesh(zf)
    if rec is None:
        raise SystemExit('%s: no record with a circle mesh (section 17)' % zip_path)
    name, rv, ri, nv, ni = rec
    cap = sc.Cap(zip_path)
    first = None
    draws = equal_rec = equal_swapped = same_as_first = 0
    for fi in range(len(cap.frames)):
        f = cap.c.frame(fi)
        if not f.has_origins:
            continue
        cs = sc.pcap.queue_site(f.origins)
        for i in np.nonzero(cs == sc.CALL_CIRCLE)[0]:
            dr = f.draws[i]
            if int(dr['vertex_count']) != nv:
                continue
            sv = np.ascontiguousarray(sc.vertex_rows(cap, dr, 32)).tobytes()
            if first is None:
                first = sv
            draws += 1
            same_as_first += sv == first
            equal_rec += sv == rv
            equal_swapped += sv == id_word_reversed(rv)
    if first is None:
        raise SystemExit('%s: no circle draw in the stream' % zip_path)
    print('circles_live_mesh: %s: record %s (%d vertices / %d indices); %d circle draws: stream == record %d, stream == record with the id word reversed %d, stream == the first draw %d' %
          (os.path.basename(zip_path), name, nv, ni, draws, equal_rec, equal_swapped, same_as_first))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as fh:
        fh.write(struct.pack('<IIII', 0x48534D4C, 1, nv, ni))
        fh.write(rv)
        fh.write(first)
        fh.write(struct.pack('<II', draws, same_as_first))
    return 0 if draws and same_as_first == draws else 1


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'dump':
        sys.exit(dump(a[1], a[2]))
    print(__doc__)
    sys.exit(2)

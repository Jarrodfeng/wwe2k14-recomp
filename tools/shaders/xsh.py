"""Session 61: read the SDK's shader storage files (our own run output, under out\\; the microcode in them is game
data: never commit what this prints beyond hashes and counts).

    py tools\\shaders\\xsh.py FILE.xsh [--list]
    py tools\\shaders\\xsh.py FILE.vk.xpso [--list]

Formats (thirdparty\\rexglue-sdk-src, include\\rex\\graphics\\vulkan\\pipeline_cache.h + pipeline_cache.cpp):
- `.xsh`: 'XESH', LE32 version byte-swapped (0x20201219); records = LE64 XXH3_64bits of the microcode, LE32
  (dword count : 31, type : 1 - 0 vertex, 1 pixel), the microcode dwords in guest (big-endian) order.
- `.xpso`: 'XEPS', LE32 api (1 fsi, 0 fbo), LE32 version byte-swapped; records = LE64 XXH3 of the description,
  then the PipelineDescription (vertex hash, vertex modification, pixel hash, pixel modification, ...).
"""

import argparse
import struct
import sys

XSH_MAGIC = 0x48534558
XPSO_MAGIC = 0x53504558


def read_xsh(path):
    """[(hash, type, microcode bytes (guest order))] in file order; stops at a short record."""
    data = open(path, 'rb').read()
    magic, version_swapped = struct.unpack_from('<II', data, 0)
    if magic != XSH_MAGIC:
        raise ValueError(f'{path}: not an .xsh file')
    out = []
    p = 8
    while p + 12 <= len(data):
        h, bits = struct.unpack_from('<QI', data, p)
        count = bits & 0x7FFFFFFF
        stype = bits >> 31
        p += 12
        if p + count * 4 > len(data):
            break
        out.append((h, stype, data[p:p + count * 4]))
        p += count * 4
    return out


def read_xpso(path, record_size=None):
    """(api, version, [(description hash, vs hash, vs mod, ps hash, ps mod, raw description)])."""
    data = open(path, 'rb').read()
    magic, api, version_swapped = struct.unpack_from('<III', data, 0)
    if magic != XPSO_MAGIC:
        raise ValueError(f'{path}: not an .xpso file')
    version = struct.unpack('>I', struct.pack('<I', version_swapped))[0]
    if record_size is None:
        record_size = 68  # LE64 hash + the 60-byte PipelineDescription (file sizes - 12 divide by 68)
    out = []
    p = 12
    while p + record_size <= len(data):
        dh = struct.unpack_from('<Q', data, p)[0]
        desc = data[p + 8:p + record_size]
        vs, vsm, ps, psm = struct.unpack_from('<QQQQ', desc, 0)
        out.append((dh, vs, vsm, ps, psm, desc))
        p += record_size
    return api, version, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.file.lower().endswith('.xsh'):
        recs = read_xsh(a.file)
        vs = sum(1 for r in recs if r[1] == 0)
        print(f'{len(recs)} shaders ({vs} vertex, {len(recs) - vs} pixel), '
              f'{len({r[0] for r in recs})} distinct hashes, {sum(len(r[2]) for r in recs)} microcode bytes')
        if a.list:
            for h, t, mc in recs:
                print(f'{h:016X} {"VS" if t == 0 else "PS"} {len(mc) // 4}')
    else:
        api, version, recs = read_xpso(a.file)
        print(f'api {"fsi" if api else "fbo"}, version {version:08X}, {len(recs)} pipelines, '
              f'{len({r[1] for r in recs})} vertex / {len({r[3] for r in recs})} pixel shader hashes')
        if a.list:
            for dh, vs, vsm, ps, psm, _ in recs:
                print(f'{dh:016X} VS {vs:016X}/{vsm:016X} PS {ps:016X}/{psm:016X}')


if __name__ == '__main__':
    sys.exit(main())

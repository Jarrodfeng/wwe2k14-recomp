#!/usr/bin/env python3
r"""Lane 238: a SYNTHETIC capture for a shape no capture on disk holds (0 game runs). Writes a copy of a capture folder whose frame files carry one patched fact; rplay / the core test then
replay it like any capture (stream arm vs --post-ours arm). The patch is named in the output folder's SYNTH.txt so nobody mistakes it for a game capture.

  py -I tools\post\synth_frame_patch.py SRC_CAPTURE DST_CAPTURE --merge-point [--frames N]

  --merge-point  every m1_m2 draw (the glare merge, yBlur_m1_m2_fragment) gets its slot 3 bound with a POINT sampler (the same address modes): the fatal-4 frames of run 7131
                 ('the stream binds a m1_m2 slot 3 point; the pass binds it linear', 46 binds). Found by the pixel program's hash (the effect registry: src\renderer\shaders\spv\fx_registry.h).

The big files (geometry.bin, textures.bin) are hard-linked (no copy); frames/ are copied (patched). scene/ records are not copied (rplay replays a capture without them).
"""
import argparse
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REGISTRY = os.path.join(HERE, '..', '..', 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h')
SEC_DRAWS, SEC_SAMPLERS, SEC_TEXTURE_SLOTS = 4, 9, 10


def variant_hashes(name):
    """The pixel-program hashes the registry maps to the variant `name`."""
    text = open(REGISTRY, encoding='utf-8', errors='replace').read()
    start = text.index('inline constexpr Variant kVariants[] = {')
    end = text.index('inline constexpr HashEntry kHashes[] = {')
    idx = None
    n = 0
    for line in text[start:end].splitlines()[1:]:
        if line.startswith('  {"'):
            if line.startswith('  {"%s",' % name):
                idx = n
            n += 1
    if idx is None:
        raise SystemExit('variant %s is not in the registry' % name)
    return {int(m.group(1), 16) for m in re.finditer(r'\{0x([0-9A-Fa-f]+)ull, %d\}' % idx, text[end:])}


def sections(buf):
    magic, version, _cf, count = struct.unpack_from('<4I', buf, 0)
    if magic != 0x52464352:
        raise SystemExit('not a frame file')
    out = {}
    for s in range(count):
        sid, esize, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * s)
        out[sid] = (esize, cnt, off)
    return out


def patch_merge_point(buf, hashes):
    sec = sections(buf)
    de, dn, doff = sec[SEC_DRAWS]
    se, sn, soff = sec[SEC_SAMPLERS]
    te, tn, toff = sec[SEC_TEXTURE_SLOTS]
    samplers = [struct.unpack_from('<8B', buf, soff + i * se) for i in range(sn)]   # mag, min, mip, addr u, v, w, aniso, border
    patched = 0
    for d in range(dn):
        base = doff + d * de
        ps = struct.unpack_from('<Q', buf, base + 8)[0]
        if ps not in hashes:
            continue
        first, count = struct.unpack_from('<II', buf, base + 32)
        for t in range(first, first + count):
            tb = toff + t * te
            image, sampler, slot = struct.unpack_from('<IIB', buf, tb)
            if slot != 3:
                continue
            cur = samplers[sampler]
            if cur[0] == 0 and cur[1] == 0:
                continue   # already point
            want = [i for i, s in enumerate(samplers) if s[0] == 0 and s[1] == 0 and s[2:6] == cur[2:6]]
            if not want:   # none with the same address modes: any POINT sampler (a 1:1 merge samples texel centres, the address mode never matters there)
                want = [i for i, s in enumerate(samplers) if s[0] == 0 and s[1] == 0]
            if not want:
                raise SystemExit('no POINT sampler in the frame')
            struct.pack_into('<I', buf, tb + 4, want[0])
            patched += 1
    return patched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--merge-point', action='store_true')
    ap.add_argument('--frames', type=int, default=0, help='copy only the first N frames (0: all)')
    args = ap.parse_args()
    if not args.merge_point:
        print('nothing to patch (--merge-point)')
        return 2
    hashes = variant_hashes('yBlur_m1_m2_fragment')
    src_frames = os.path.join(args.src, 'frames')
    names = sorted(f for f in os.listdir(src_frames) if f.endswith('.bin'))
    if args.frames:
        names = names[:args.frames]
    os.makedirs(os.path.join(args.dst, 'frames'), exist_ok=True)
    for big in ('geometry.bin', 'textures.bin'):
        d = os.path.join(args.dst, big)
        if not os.path.exists(d):
            try:
                os.link(os.path.join(args.src, big), d)
            except OSError:
                shutil.copyfile(os.path.join(args.src, big), d)
    shutil.copyfile(os.path.join(args.src, 'images.bin'), os.path.join(args.dst, 'images.bin'))
    total = frames_hit = 0
    for n in names:
        buf = bytearray(open(os.path.join(src_frames, n), 'rb').read())
        k = patch_merge_point(buf, hashes)
        total += k
        frames_hit += 1 if k else 0
        open(os.path.join(args.dst, 'frames', n), 'wb').write(buf)
    open(os.path.join(args.dst, 'SYNTH.txt'), 'w').write('SYNTHETIC capture (tools\\post\\synth_frame_patch.py --merge-point): %d frames copied from %s, m1_m2 slot 3 rebound POINT in %d binds over %d frames. Not a game capture.\n' % (
        len(names), args.src, total, frames_hit))
    print('patched %d m1_m2 slot-3 binds in %d of %d frames -> %s' % (total, frames_hit, len(names), args.dst))
    return 0 if total else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
r"""Lane 277: a SYNTHETIC capture of the 30 Years cutscene's post chain shape (0 game runs):

    [copy_depth] t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine(fog_bloom: NO dof) monotone

Lane 296: --two-layer keeps the combine's dof permutation and makes the TWO-LAYER form of run 7201 (the 30 Years cutscene): 'copy_depth t2 copy_depthx2 t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine monotone'.
The far layer is a block in front of the blur's: [SET, downscale draw, COPY ->3, t2 (taps x3), COPY -> an image appended to images.bin] + two restores; the combine's slot 6 is rebound to the appended image.

No capture on disk holds it (run 7183's frames were not in any case zip). It is made from two real captures:
  VICTORY  frames of the victory scene (`copy_depth t2 box4_down t2 t1_t2x2 t1x8 m1_m2 combine`, combine = dof_fog_bloom, bloom / glare constants 0),
  FILM     one 30 Years frame with the film pass (`dof hole_fill combine monotone`).

  py -I tools\post\synth_t2_film.py VICTORY_CAPTURE FILM_CAPTURE DST_CAPTURE [--frames N] [--live-bloom] [--film-frame K]

What is patched in every victory frame that carries the HDR chain (a frame without it is copied unchanged):
  1. the combine's pixel program becomes yGlareEtc_fog_bloom (what run 7183 built): no dof permutation, so the leading t2 feeds only the bright pass;
  2. the film pass is added after the combine's depth clear: a copy of the combine's target into image 2 (the scene-colour image the combine just read), then a draw cloned from the combine's draw
     (same quad geometry / layout / state) with the FILM frame's film vertex + pixel programs, its one texture slot (image 2, the combine's point depth sampler) and its two constant banks;
  3. --live-bloom: the combine's bloom and glare strengths (ps registers 31 / 32, x) become non-zero (0.35 / 1.0), so the HDR chain is live work (the victory frames have 0 there).
The big files are hard-linked (geometry.bin is not changed). SYNTH.txt names every patch: this is not a game capture.
"""
import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from synth_frame_patch import variant_hashes   # the effect registry's pixel program hashes per variant

OPS, TARGETS, CLEARS, DRAWS, COPIES, PRESENTS, FIXEDS, DYNAMICS, SAMPLERS, TEXSLOTS, LAYOUTS, ELEMENTS, BANKS, BOOLLOOPS, CONTENTS, COPYEXTRAS, BANKDEFS, BANKPATCHES = range(1, 19)
DRAW_ORIGINS, THIN_FACTS = 64, 71
K_SET, K_CLEAR, K_DRAW, K_COPY, K_PRESENT = range(5)
NO_BASE = 0xFFFFFFFF


class Frame:
    def __init__(self, path):
        buf = open(path, 'rb').read()
        self.head = bytearray(buf[:32])
        magic, ver, cf, cnt = struct.unpack_from('<4I', buf, 0)
        if magic != 0x52464352:
            raise SystemExit('not a frame file: ' + path)
        self.order = []
        self.sec = {}
        for s in range(cnt):
            sid, es, n, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * s)
            self.order.append(sid)
            self.sec[sid] = [es, bytearray(buf[off:off + es * n])]

    def count(self, sid):
        es, b = self.sec[sid]
        return len(b) // es if es else 0

    def get(self, sid, i):
        es, b = self.sec[sid]
        return bytes(b[i * es:(i + 1) * es])

    def put(self, sid, i, data):
        es, b = self.sec[sid]
        assert len(data) == es
        b[i * es:(i + 1) * es] = data

    def append(self, sid, data):
        es, b = self.sec[sid]
        assert len(data) == es, (sid, len(data), es)
        b += data
        return len(b) // es - 1

    def write(self, path):
        out = bytearray(self.head)
        struct.pack_into('<I', out, 12, len(self.order))
        table = 32 + 24 * len(self.order)
        pos = (table + 7) & ~7
        entries = []
        body = bytearray()
        for sid in self.order:
            es, b = self.sec[sid]
            entries.append((sid, es, len(b) // es if es else 0, pos + len(body)))
            body += b
            while (pos + len(body)) % 8:
                body += b'\0'
        out = out[:32]
        for e in entries:
            out += struct.pack('<IIQQ', *e)
        out += b'\0' * (pos - len(out))
        out += body
        open(path, 'wb').write(out)

    def ops(self):
        es, b = self.sec[OPS]
        return [struct.unpack_from('<II', b, i * es) for i in range(len(b) // es)]

    def set_ops(self, ops):
        es, b = self.sec[OPS]
        b[:] = b''.join(struct.pack('<II', k, i) for k, i in ops)

    def expand_bank(self, di):
        """The full 4096-byte bank draw-bank index `di` stands for (BankDef base chain + patches), as the reader expands it (in order: the chain is long)."""
        cache = self.__dict__.setdefault('_banks', {})
        for i in range(len(cache), di + 1):
            base, full, pf, pc = struct.unpack('<IIII', self.get(BANKDEFS, i))
            if base == NO_BASE:
                bank = bytearray(self.get(BANKS, full))
            elif base < i:   # as the reader: a base at or after the def itself leaves the bank zero
                bank = bytearray(cache[base])
            else:
                bank = bytearray(4096)
            for p in range(pc):
                pt = self.get(BANKPATCHES, pf + p)
                block = struct.unpack_from('<I', pt, 0)[0]
                bank[64 * block:64 * block + 64] = pt[4:68]
            cache[i] = bytes(bank)
        return cache[di]


def draw_fields(frame, di):
    d = frame.get(DRAWS, di)
    vs, ps, vsp = struct.unpack_from('<QQQ', d, 0)
    return d, vs, ps, vsp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('victory')
    ap.add_argument('film')
    ap.add_argument('dst')
    ap.add_argument('--frames', type=int, default=0, help='first N frames only (0: all)')
    ap.add_argument('--first', type=int, default=0, help='start at this victory frame')
    ap.add_argument('--live-bloom', action='store_true')
    ap.add_argument('--film-frame', type=int, default=3)
    ap.add_argument('--two-layer', action='store_true', help='lane 296: keep the combine dof permutation and add the far layer (an earlier t2 + two restores in front): copy_depth t2 copy_depthx2 t2 box4_down ...')
    ap.add_argument('--far-copy-late', action='store_true', help='lane 296b: with --two-layer, the far t2 blur copy (Y -> far image) sits AFTER the later t2 draw, inside the claimed range')
    args = ap.parse_args()
    t2_hashes = variant_hashes('yBlur_t2_fragment')
    restore_hashes = variant_hashes('yPostBase_copy_depth_fragment')
    box4_hashes = variant_hashes('yDownScale_box4_down_fragment')

    mono_hashes = variant_hashes('yMonotone_monotone_fragment')
    old_combine = variant_hashes('yGlareEtc_dof_fog_bloom_fragment')
    new_combine = sorted(variant_hashes('yGlareEtc_fog_bloom_fragment'))[0]

    # the film pass template: its two programs, its texture slot, its two (expanded) constant banks
    ff = Frame(os.path.join(args.film, 'frames', 'frame_%06d.bin' % args.film_frame))
    tmpl = None
    for di in range(ff.count(DRAWS)):
        d, vs, ps, vsp = draw_fields(ff, di)
        if ps in mono_hashes:
            vb, pb = struct.unpack_from('<II', d, 40)
            tmpl = dict(vs=vs, ps=ps, vsp=vsp, tex=ff.get(TEXSLOTS, struct.unpack_from('<I', d, 32)[0]), vsbank=ff.expand_bank(vb), psbank=ff.expand_bank(pb))
            break
    if tmpl is None:
        raise SystemExit('no film pass in the film capture frame %d' % args.film_frame)

    src_frames = os.path.join(args.victory, 'frames')
    names = sorted(f for f in os.listdir(src_frames) if f.endswith('.bin'))[args.first:]
    if args.frames:
        names = names[:args.frames]
    os.makedirs(os.path.join(args.dst, 'frames'), exist_ok=True)
    for big in ('geometry.bin', 'textures.bin'):
        d = os.path.join(args.dst, big)
        if not os.path.exists(d):
            try:
                os.link(os.path.join(args.victory, big), d)
            except OSError:
                shutil.copyfile(os.path.join(args.victory, big), d)
    shutil.copyfile(os.path.join(args.victory, 'images.bin'), os.path.join(args.dst, 'images.bin'))
    far_image = None
    if args.two_layer:   # the far layer's blur lives in an image of its own: a copy of the image the blur's output goes to (the victory frames' image 4), appended to images.bin
        ip = os.path.join(args.dst, 'images.bin')
        raw = bytearray(open(ip, 'rb').read())
        magic, ver, count, pad = struct.unpack_from('<IIII', raw, 0)
        recs = [bytes(raw[16 + 24 * i:40 + 24 * i]) for i in range(count)]
        ids = [struct.unpack_from('<I', r, 0)[0] for r in recs]
        far_image = max(ids) + 1
        rec4 = next(r for r in recs if struct.unpack_from('<I', r, 0)[0] == 4)
        raw += struct.pack('<I', far_image) + rec4[4:]
        struct.pack_into('<I', raw, 8, count + 1)
        if args.far_copy_late:   # lane 296b: a second appended image Z (a copy of the record of image 254, the t2 target) is the NEAR t2's own target, so the far target Y keeps its writer
            z_image = far_image + 1
            rec_y = next(r for r in recs if struct.unpack_from('<I', r, 0)[0] == 254)
            raw += struct.pack('<I', z_image) + rec_y[4:]
            struct.pack_into('<I', raw, 8, count + 2)
        open(ip, 'wb').write(raw)

    patched = plain = 0
    for out_i, name in enumerate(names):
        fr = Frame(os.path.join(src_frames, name))
        ops = fr.ops()
        ci = None
        for oi, (k, ix) in enumerate(ops):
            if k == K_DRAW and struct.unpack_from('<Q', fr.get(DRAWS, ix), 8)[0] in old_combine:
                ci = oi
        has_hdr = any(k == K_DRAW and struct.unpack_from('<Q', fr.get(DRAWS, ix), 8)[0] in variant_cache_m1m2() for k, ix in ops)
        if ci is None or not has_hdr or ci + 1 >= len(ops) or ops[ci + 1][0] != K_CLEAR:
            plain += 1
            fr.head[8:12] = struct.pack('<I', out_i)
            fr.write(os.path.join(args.dst, 'frames', 'frame_%06d.bin' % out_i))
            continue
        cdi = ops[ci][1]
        cd = bytearray(fr.get(DRAWS, cdi))
        # 0. (--two-layer) the far layer in front of the blur: [SET, downscale draw, COPY ->3, t2, COPY ->4] = the blur's block; the new block before it has its own t2 draw (wider taps) whose output goes to the
        #    appended image, then two more restores (copy_depth), then the original block. The combine's slot 6 reads the appended image (the far blur), slot 2 the original (the near blur).
        if args.two_layer:
            bo = next((o for o, (k, ix) in enumerate(ops) if k == K_DRAW and struct.unpack_from('<Q', fr.get(DRAWS, ix), 8)[0] in box4_hashes), None)   # the chain's t2 is the last one before the bright pass
            t2o = max((o for o in range(bo) if ops[o][0] == K_DRAW and struct.unpack_from('<Q', fr.get(DRAWS, ops[o][1]), 8)[0] in t2_hashes), default=None) if bo is not None else None
            if t2o is None or t2o < 3 or [k for k, _ in ops[t2o - 3:t2o + 2]] != [K_SET, K_DRAW, K_COPY, K_DRAW, K_COPY]:
                plain += 1
                fr.head[8:12] = struct.pack('<I', out_i)
                fr.write(os.path.join(args.dst, 'frames', 'frame_%06d.bin' % out_i))
                continue
            ro = max((o for o in range(t2o - 3) if ops[o][0] == K_DRAW and struct.unpack_from('<Q', fr.get(DRAWS, ops[o][1]), 8)[0] in restore_hashes), default=None)
            if ro is None or ops[ro - 1][0] != K_SET:
                plain += 1
                fr.head[8:12] = struct.pack('<I', out_i)
                fr.write(os.path.join(args.dst, 'frames', 'frame_%06d.bin' % out_i))
                continue
            t2d = bytearray(fr.get(DRAWS, ops[t2o][1]))   # the far layer's blur: the same pass with 3x wider taps (so the two layers differ and a swapped slot moves the picture)
            pbank = bytearray(fr.expand_bank(struct.unpack_from('<I', t2d, 44)[0]))
            for r in range(13):
                for c in (0, 1):
                    v = struct.unpack_from('<f', pbank, 16 * r + 4 * c)[0]
                    struct.pack_into('<f', pbank, 16 * r + 4 * c, v * 3.0)
            struct.pack_into('<I', t2d, 44, fr.append(BANKDEFS, struct.pack('<IIII', NO_BASE, fr.append(BANKS, bytes(pbank)), 0, 0)))
            far_t2 = fr.append(DRAWS, bytes(t2d))
            for sid in (DRAW_ORIGINS, THIN_FACTS):
                if sid in fr.sec:
                    fr.append(sid, b'\0' * fr.sec[sid][0])
            cpo = bytearray(fr.get(COPIES, ops[t2o + 1][1]))
            struct.pack_into('<I', cpo, 8, far_image)
            far_copy = fr.append(COPIES, bytes(cpo))
            fr.append(COPYEXTRAS, fr.get(COPYEXTRAS, ops[t2o + 1][1]))
            restore = [ops[ro - 1], ops[ro]]
            late = args.far_copy_late   # lane 296b: the far copy moves behind the near t2 draw (SF1 class)
            block_a = ops[t2o - 3:t2o] + [(K_DRAW, far_t2)] + ([] if late else [(K_COPY, far_copy)])
            ops[t2o - 3:t2o - 3] = block_a + restore + restore
            if late:
                # lane 296b (SF1 class): [.. near t2 -> Z, far copy Y -> far image (AFTER the near t2, inside the claim; Y's writer is still the far t2), near copy Z -> img4 ..], and the box4_down that follows
                # renders into Y: a claimed draw writes the far copy's source. The rule before the fix dropped the copy without counting it: the pass sampled last frame's far blur.
                pt = t2o + len(block_a) + 4   # the original near t2
                assert ops[pt][0] == K_DRAW and ops[pt + 1][0] == K_COPY
                assert ops[pt - 3][0] == K_SET and ops[pt - 1][0] == K_COPY   # the near block [SET Y, downscale, COPY Y -> 3, t2, COPY Y -> 4] is retargeted whole to Z
                tg = bytearray(fr.get(TARGETS, ops[pt - 3][1]))
                y_image = struct.unpack_from('<I', tg, 0)[0]
                struct.pack_into('<I', tg, 0, z_image)
                set_z = fr.append(TARGETS, bytes(tg))
                nc = bytearray(fr.get(COPIES, ops[pt + 1][1]))
                struct.pack_into('<I', nc, 0, z_image)
                near_copy = fr.append(COPIES, bytes(nc))
                fr.append(COPYEXTRAS, fr.get(COPYEXTRAS, ops[pt + 1][1]))
                c3 = bytearray(fr.get(COPIES, ops[pt - 1][1]))
                struct.pack_into('<I', c3, 0, z_image)
                near_copy3 = fr.append(COPIES, bytes(c3))
                fr.append(COPYEXTRAS, fr.get(COPYEXTRAS, ops[pt - 1][1]))
                ops[pt - 3] = (K_SET, set_z)
                ops[pt - 1] = (K_COPY, near_copy3)
                bo2 = next(o for o in range(pt + 1, len(ops)) if ops[o][0] == K_DRAW and struct.unpack_from('<Q', fr.get(DRAWS, ops[o][1]), 8)[0] in box4_hashes)
                assert ops[bo2 - 1][0] == K_SET
                tg2 = bytearray(fr.get(TARGETS, ops[bo2 - 1][1]))
                struct.pack_into('<I', tg2, 0, y_image)
                ops[bo2 - 1] = (K_SET, fr.append(TARGETS, bytes(tg2)))
                ops[pt + 1] = (K_COPY, near_copy)
                ops.insert(pt + 1, (K_COPY, far_copy))
                ci += 1
            ci += len(block_a) + 4
            # the combine reads the far blur in slot 6
            c_tf, c_tc = struct.unpack_from('<II', cd, 32)
            first_new = None
            for t in range(c_tf, c_tf + c_tc):
                e = bytearray(fr.get(TEXSLOTS, t))
                if e[8] == 6:
                    struct.pack_into('<I', e, 0, far_image)
                n = fr.append(TEXSLOTS, bytes(e))
                first_new = n if first_new is None else first_new
            struct.pack_into('<II', cd, 32, first_new, c_tc)
        # 1. the combine without its dof permutation
        if not args.two_layer:
            struct.pack_into('<Q', cd, 8, new_combine)
        # 3. live bloom / glare: ps registers 31 / 32, x
        ps_bank_idx = struct.unpack_from('<I', cd, 44)[0]
        if args.live_bloom:
            bank = bytearray(fr.expand_bank(ps_bank_idx))
            struct.pack_into('<f', bank, 4 * (4 * 31), 0.35)
            struct.pack_into('<f', bank, 4 * (4 * 32), 1.0)
            fidx = fr.append(BANKS, bytes(bank))
            struct.pack_into('<I', cd, 44, fr.append(BANKDEFS, struct.pack('<IIII', NO_BASE, fidx, 0, 0)))
        fr.put(DRAWS, cdi, bytes(cd))
        # 2. the film pass: a copy of the combine's target into image 2, then the cloned draw
        last_copy = [ix for k, ix in ops if k == K_COPY and struct.unpack_from('<I', fr.get(COPIES, ix), 0)[0] == 8][-1]   # the final 8 -> X copy of the frame
        cp = bytearray(fr.get(COPIES, last_copy))
        struct.pack_into('<I', cp, 8, 2)
        ncopy = fr.append(COPIES, bytes(cp))
        fr.append(COPYEXTRAS, fr.get(COPYEXTRAS, last_copy))
        film = bytearray(fr.get(DRAWS, cdi))
        struct.pack_into('<QQQ', film, 0, tmpl['vs'], tmpl['ps'], tmpl['vsp'])
        c_tf, c_tc = struct.unpack_from('<II', cd, 32)
        sampler = None
        for t in range(c_tf, c_tf + c_tc):
            e = fr.get(TEXSLOTS, t)
            if e[8] == 3:   # the combine's depth slot: point, clamp
                sampler = struct.unpack_from('<I', e, 4)[0]
        if sampler is None:
            raise SystemExit('frame %s: the combine has no slot 3' % name)
        te = bytearray(tmpl['tex'])
        struct.pack_into('<II', te, 0, 2, sampler)
        tex_idx = fr.append(TEXSLOTS, bytes(te))
        struct.pack_into('<II', film, 32, tex_idx, 1)
        vb = fr.append(BANKDEFS, struct.pack('<IIII', NO_BASE, fr.append(BANKS, tmpl['vsbank']), 0, 0))
        pb = fr.append(BANKDEFS, struct.pack('<IIII', NO_BASE, fr.append(BANKS, tmpl['psbank']), 0, 0))
        struct.pack_into('<II', film, 40, vb, pb)
        ndraw = fr.append(DRAWS, bytes(film))
        for sid in (DRAW_ORIGINS, THIN_FACTS):
            if sid in fr.sec:
                fr.append(sid, b'\0' * fr.sec[sid][0])
        ops[ci + 2:ci + 2] = [(K_COPY, ncopy), (K_DRAW, ndraw)]
        fr.set_ops(ops)
        fr.head[8:12] = struct.pack('<I', out_i)
        fr.write(os.path.join(args.dst, 'frames', 'frame_%06d.bin' % out_i))
        patched += 1
    open(os.path.join(args.dst, 'SYNTH.txt'), 'w').write(
        'SYNTHETIC capture (tools/post/synth_t2_film.py%s): %d frames from %s; in %d of them (the victory frames with the HDR chain) the combine is %s and a film pass '
        '(programs / constants / slot of frame %d of %s) follows it; %d frames copied unchanged. Not a game capture.\n' % (
            (' --live-bloom: bloom 0.35 / glare 1.0' if args.live_bloom else '') + (' --two-layer' if args.two_layer else ''), len(names), args.victory, patched,
            'yGlareEtc_dof_fog_bloom (the stream own; a far layer in front: a second t2 with 3x wider taps into an appended image, bound to slot 6, two more restores before the near blur)' if args.two_layer else 'yGlareEtc_fog_bloom (no dof)',
            args.film_frame, args.film, plain))
    print('patched %d of %d frames (%d unchanged) -> %s' % (patched, len(names), plain, args.dst))
    return 0 if patched else 1


_M1M2 = None


def variant_cache_m1m2():
    global _M1M2
    if _M1M2 is None:
        _M1M2 = variant_hashes('yBlur_m1_m2_fragment')
    return _M1M2


if __name__ == '__main__':
    sys.exit(main())

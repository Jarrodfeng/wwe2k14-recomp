r"""Lane 139 (SH3 slice 2): the plane shadow's capture-side oracle dump. Offline, 0 game runs.

  py -I tools\shadow\plane_oracle.py dump CAPTURE OUT.plane [--frames A:B:STEP]
  py -I tools\shadow\plane_oracle.py info FILE.plane

Reads every frame of a capture (case zip, case folder or capture folder) and writes, per frame, the stream's plane-chain draws as the vertex constants they were drawn with:
  mask draws (pass site 0x82BF4D38, T_PlaneShadowMap), in draw order: the object, the part, c0..c16 (ShadowDistance c0-c6, PlanePos c7, c8, PlaneInv c9-c12, CameraViewProj c13-c16),
      EyePos c72 and the palette rows c76.. (trailing all-zero rows trimmed: the oracle cannot know how many rows the game uploaded);
  blur draws (call 0x827EEC84, T_PlaneShadowBlur, yBlur plane_x / plane_z): c33..c40 (8 rows).
It also CHECKS, from other draws of the same frame, the facts the record contract rests on and prints them (a failing fact makes the dump exit 1):
  - CameraViewProj c13-c16 is bit-equal to the colour pass's c56-c59 (the first colour draw of the frame);
  - the ShadowDistance rows c0-c6 are the sliding window of ONE float array (row i = array[i..i+3]); the array's tail value is the weight the blur's c35.x carries;
  - the blur's c33.x / c35.x / c40 are the module's [+96] / [+104] / the fit-point table entry, and its c36-c39 matrix is reported against camera x PlaneInv (informational).
The C++ side (src\renderer_scene\plane_oracle.h) lays this out as the register uploads the game makes and drives the SHIPPED tracker (src\scene_live\plane_hook.h) over it.

File (little endian): u32 'PLNE' 0x454E4C50, u32 version 1, u32 nframes, per frame { u32 capture_frame, u32 guest_frame, u32 n_mask, u32 n_blur,
  n_mask x { u32 object, u32 part, 17x4 f32 rows, 4 f32 eye, u32 pal_n, pal_n x 4 f32 },  n_blur x { 8x4 f32 rows } }. Numbers and constants only: nothing here is committed (outputs go under out\)."""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

MAGIC, VERSION = 0x454E4C50, 1
R_BLUR0 = 33
PAL0, PAL_MAX = 76, 63


def frames_of(spec, n):
    if not spec:
        return list(range(n))
    a = [int(x) if x else None for x in spec.split(':')]
    a += [None] * (3 - len(a))
    return list(range(a[0] or 0, min(n, a[1] if a[1] is not None else n), a[2] or 1))


def trim_palette(rows):
    n = len(rows)
    while n > 0 and not rows[n - 1].any():
        n -= 1
    return n


def dump(capture, out_path, spec=''):
    cap = sc.Cap(capture)
    frames = frames_of(spec, len(cap.frames))
    body = bytearray()
    nf = n_mask = n_blur = 0
    facts = dict(cam_eq=0, cam_ne=0, window_ok=0, window_bad=0, blur_c33_one=0, blur_c35_weight=0, blur_c35_other=0, planes=0, no_colour=0)
    pi_shape = set()
    mat_err = 0.0
    for fi in frames:
        f = cap.c.frame(fi)
        if not f.has_origins:
            continue
        d, o = f.draws, f.origins
        cs = sc.pcap.queue_site(o)
        ps = o['pass_site'].astype(np.int64)
        mi = np.nonzero(ps == sc.SITE_PLANE)[0]
        bi = np.nonzero(cs == sc.CALL_PBLUR)[0]
        if len(mi) == 0 and len(bi) == 0:
            continue
        colour = next((j for j in range(len(d)) if int(ps[j]) in (sc.SITE_COLOUR, sc.SITE_MIRROR)), None)
        cam56 = sc.regs(f.bank(int(d[colour]['vs_bank'])), sc.R_VIEWPROJ, 4).tobytes() if colour is not None else None
        masks = []
        for i in mi:
            vb = f.bank(int(d[i]['vs_bank']))
            rows = sc.regs(vb, 0, 17).astype('<f4')
            eye = sc.regs(vb, sc.R_EYE, 1)[0].astype('<f4')
            pal = sc.regs(vb, PAL0, PAL_MAX).astype('<f4')
            pn = trim_palette(pal)
            masks.append((int(o[i]['object']) & 0xFFFFFFFF, int(o[i]['part']) & 0xFFFFFFFF, rows, eye, pal[:pn]))
            # the facts
            facts['planes'] += 1
            if cam56 is None:
                facts['no_colour'] += 1
            elif rows[13:17].tobytes() == cam56:
                facts['cam_eq'] += 1
            else:
                facts['cam_ne'] += 1
            if all(np.array_equal(rows[r][1:], rows[r + 1][:3]) for r in range(6)):
                facts['window_ok'] += 1
            else:
                facts['window_bad'] += 1
            pi = rows[9:13]
            pi_shape.add('diag' if pi[0][1] == 0 and pi[1][0] == 0 and pi[2][2] == 1 and pi[3][3] == 1 else 'other')
        blurs = []
        for i in bi:
            vb = f.bank(int(d[i]['vs_bank']))
            br = sc.regs(vb, R_BLUR0, 8).astype('<f4')
            blurs.append(br)
            facts['blur_c33_one'] += int(br[0][0] == 1.0)
            if masks:
                wt = masks[0][2][6][3]
                if abs(br[2][0] - wt) < 1e-6:
                    facts['blur_c35_weight'] += 1
                else:
                    facts['blur_c35_other'] += 1
        body += struct.pack('<IIII', fi, int(f.guest_frame) & 0xFFFFFFFF, len(masks), len(blurs))
        for ob, pt, rows, eye, pal in masks:
            body += struct.pack('<II', ob, pt) + rows.tobytes() + eye.tobytes() + struct.pack('<I', len(pal)) + pal.tobytes()
        for br in blurs:
            body += br.tobytes()
        nf += 1
        n_mask += len(masks)
        n_blur += len(blurs)
    if nf == 0:
        raise SystemExit('%s: no plane-chain draw in %d frames (a capture without draw origins, another scene, or the wm30 arena which has no plane pass)' % (capture, len(frames)))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as fh:
        fh.write(struct.pack('<III', MAGIC, VERSION, nf))
        fh.write(bytes(body))
    bad = facts['cam_ne'] + facts['window_bad']
    print('plane_oracle: %s: %d frames, %d mask draws, %d blur draws; camera == colour c56-59 bit-equal %d of %d (not equal %d, no colour draw %d); ShadowDistance sliding window ok %d bad %d; '
          'blur c35.x == weight %d, other %d; PlaneInv shapes %s %s' % (capture, nf, n_mask, n_blur, facts['cam_eq'], facts['planes'], facts['cam_ne'], facts['no_colour'], facts['window_ok'],
                                                                      facts['window_bad'], facts['blur_c35_weight'], facts['blur_c35_other'], sorted(pi_shape), 'OK' if not bad else 'FAIL'))
    return 0 if not bad else 1


def info(path):
    b = open(path, 'rb').read()
    magic, ver, nf = struct.unpack_from('<III', b, 0)
    if magic != MAGIC or ver != VERSION:
        raise SystemExit('%s: not a plane oracle file' % path)
    print('%s: %d frames, %d bytes' % (path, nf, len(b)))
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

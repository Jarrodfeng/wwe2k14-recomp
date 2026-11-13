"""Texture coherence census (S101 lane 174, GATE_FAULTS row 17). Offline, 0 game runs.

Reads a capture folder (images.bin + frames\\frame_NNNNNN.bin, docs\\renderer\\CAPTURE_FORMAT.md) and, for ONE frame, lists:
  * every draw's texture slots grouped by (class, technique, effect, slot, image kind), where the image kind says whose bytes it is:
    file (a texture the game loaded: CPU bytes), gpu_copy (copy destination), surface (a render target read directly), movie (flush-driven plane);
  * every copy op with its rect and sizes, and whether the 8-aligned rectangle was cut to the images (the census case copy_rect_cut_to_images),
    grouped by (source role, dest size).
The census cases themselves (slot_without_object, slot_invalid_type_read_as_texture) are counted in the reader at fetch-constant level and are not
in the capture; this tool names which draws / images carry a GPU-sourced or flush-driven read, the class the cases belong to.

  py -I tools\\census\\texture_cases.py --capture DIR --frame N --ops OPS.json [--json OUT]
OPS.json = `fview --capture DIR --frame N --ops --json OPS.json` (gives class / technique / effect per draw). Without --ops, draws are keyed by hash.
Only counts, ids and names are printed: nothing from the game is written.
"""
import argparse, collections, json, os, struct, sys

ROLE = {}
KIND = {0: 'surface', 1: 'texture'}


def read_images(path):
    d = open(path, 'rb').read()
    magic, ver, count, _ = struct.unpack_from('<4I', d, 0)
    imgs = {}
    off = 16
    for i in range(count):
        (iid, w, h, dp, mips, layers, fmt, kind, usage, source, flags, gfmt, role, rflags, pad) = struct.unpack_from('<I3H11B', d, off)
        imgs[iid] = dict(id=iid, w=w, h=h, mips=mips, fmt=fmt, usage=usage, source=source, flags=flags, gfmt=gfmt, role=role, rflags=rflags)
        off += 24
    return imgs


def read_frame(path):
    d = open(path, 'rb').read()
    magic, ver, cf, nsec, gframe, interval, flags = struct.unpack_from('<4IQ2I', d, 0)
    secs = {}
    off = 32
    for i in range(nsec):
        sid, esz, cnt, o = struct.unpack_from('<2IQQ', d, off)
        secs[sid] = (esz, cnt, o)
        off += 24
    return d, secs, gframe


def section(d, secs, sid):
    if sid not in secs:
        return []
    esz, cnt, o = secs[sid]
    return [(d, o + i * esz, esz) for i in range(cnt)]


def kind_of(img):
    if img is None:
        return 'none'
    f = img['flags']
    if f & 4:
        return 'movie'
    if f & 2:
        return 'gpu_copy'
    if img['source'] == 0:
        return 'surface'
    return 'file'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frame', type=int, required=True)
    ap.add_argument('--ops')
    ap.add_argument('--json')
    a = ap.parse_args()
    imgs = read_images(os.path.join(a.capture, 'images.bin'))
    d, secs, gframe = read_frame(os.path.join(a.capture, 'frames', 'frame_%06d.bin' % a.frame))
    ops = {}
    if a.ops:
        for o in json.load(open(a.ops))['ops']:
            if o['kind'] == 'draw':
                ops[o['index']] = o
    draws = section(d, secs, 4)
    slots = section(d, secs, 10)
    copies = section(d, secs, 5)
    groups = collections.Counter()
    gids = collections.defaultdict(set)
    for di, (b, o, esz) in enumerate(draws):
        tf, tc = struct.unpack_from('<2I', b, o + 32)
        op = ops.get(di, {})
        for s in range(tf, tf + tc):
            sb, so, sesz = slots[s]
            image, sampler = struct.unpack_from('<2I', sb, so)
            slot = sb[so + 8]
            img = imgs.get(image)
            k = kind_of(img)
            key = (op.get('class', '?'), op.get('technique', '?'), op.get('effect', '?'), op.get('fate', '?'), slot, k,
                   (img['role'] if img else 0), (img['w'], img['h']) if img else (0, 0))
            groups[key] += 1
            gids[key].add(image)
    rows = [dict(cls=k[0], technique=k[1], effect=k[2], fate=k[3], slot=k[4], image_kind=k[5], role=k[6], size=list(k[7]), count=n, images=sorted(gids[k])[:6]) for k, n in groups.items()]
    rows.sort(key=lambda r: -r['count'])
    cgroups = collections.Counter()
    cids = collections.defaultdict(set)
    for ci, (b, o, esz) in enumerate(copies):
        simg, smip, slay, _, dimg, dmip, dlay, _ = struct.unpack_from('<I2BHI2BH', b, o)
        sr = struct.unpack_from('<2i2I', b, o + 16)
        dr = struct.unpack_from('<2i2I', b, o + 32)
        s, dd = imgs.get(simg), imgs.get(dimg)
        cut = None
        if s and dd:
            cut = (sr[2] > s['w'] or sr[3] > s['h'] or dr[0] + sr[2] > max(dd['w'] >> dmip, 1) or dr[1] + sr[3] > max(dd['h'] >> dmip, 1))
        cgroups[(s['role'] if s else 0, (s['w'], s['h']) if s else (0, 0), (dd['w'], dd['h']) if dd else (0, 0), dmip, tuple(sr[2:]), cut, bool(sr[2] % 8 or sr[3] % 8))] += 1
        cids[(s['role'] if s else 0, (s['w'], s['h']) if s else (0, 0), (dd['w'], dd['h']) if dd else (0, 0), dmip, tuple(sr[2:]), cut, bool(sr[2] % 8 or sr[3] % 8))].add((simg, dimg))
    crows = [dict(src_role=k[0], src_size=list(k[1]), dest_size=list(k[2]), dest_mip=k[3], rect=list(k[4]), rect_over_image=k[5], not_multiple_of_8=k[6], count=n, pairs=sorted(cids[k])[:4]) for k, n in cgroups.items()]
    crows.sort(key=lambda r: -r['count'])
    out = dict(frame=a.frame, guest_frame=gframe, draws=len(draws), copies=len(copies), slot_groups=rows, copy_groups=crows)
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)
    print('frame', a.frame, 'draws', len(draws), 'copies', len(copies))
    byk = collections.Counter()
    for r in rows:
        byk[r['image_kind']] += r['count']
    print('slot reads by image kind:', dict(byk))
    print('\nGPU-sourced / flush-driven slot reads (gpu_copy, surface, movie), by class/technique/effect/slot:')
    for r in rows:
        if r['image_kind'] in ('gpu_copy', 'surface', 'movie'):
            print('  %5d  %-10s %-26s %-24s fate=%-8s slot %d %-8s role %d %s ids %s' % (r['count'], r['cls'], r['technique'], r['effect'], r['fate'], r['slot'], r['image_kind'], r['role'], r['size'], r['images']))
    print('\ncopies by (source role, sizes, rect):')
    for r in crows[:40]:
        print('  %5d  src_role %d src %s -> dst %s mip %d rect %s not_x8=%s ids %s' % (r['count'], r['src_role'], r['src_size'], r['dest_size'], r['dest_mip'], r['rect'], r['not_multiple_of_8'], r['pairs']))


if __name__ == '__main__':
    main()

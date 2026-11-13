"""Session 101 lane 170: the 16-bit colour target audit (CARRY_DARKENING.md section 12). For a capture and frames, lists every colour image whose storage is
RGBA16F / RG16F (the game's k_16_16, k_16_16_16_16 and the _FLOAT forms), the effects that draw into it (with the draw's colour exp bias), the copies out of it,
and the draws that read it or a copy of it (fetch sign, target16_read, the two exp_adjust fields). Local numbers only; nothing from the capture is printed but ids, formats, effect names and counts.

  py -I tools\\fview\\target16_audit.py --fview EXE --capture DIR --frames 488,100 [--out FILE.json]
"""
import argparse, json, os, pathlib, struct, subprocess, sys, tempfile, collections

FMT = {16: 'RGBA16F', 17: 'RG16F'}
SEC_OPS, SEC_TARGETS, SEC_CLEARS, SEC_DRAWS, SEC_COPIES, SEC_PRESENTS, SEC_FIXEDS, SEC_DYN, SEC_SAMPLERS, SEC_SLOTS = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
SEC_COPY_EXTRAS = 16


def images(cap):
    b = open(pathlib.Path(cap) / 'images.bin', 'rb').read()
    n = struct.unpack_from('<I', b, 8)[0]
    out = {}
    for i in range(n):
        o = 16 + 24 * i
        iid, w, h, d, mips, layers, fmt, kind, usage, source, flags, gf, role, rf = struct.unpack_from('<IHHHBBBBBBBBBB', b, o)
        out[iid] = dict(id=iid, w=w, h=h, fmt=fmt, game_format=gf, flags=flags, role=role)
    return out


def frame_sections(path):
    b = open(path, 'rb').read()
    magic, ver, cf, nsec, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
    secs = {}
    o = 32
    for _ in range(nsec):
        sid, esz, cnt, off = struct.unpack_from('<IIQQ', b, o)
        o += 24
        secs[sid] = (esz, cnt, off)
    return b, secs


def sec(b, secs, sid):
    if sid not in secs:
        return []
    esz, cnt, off = secs[sid]
    return [b[off + i * esz: off + (i + 1) * esz] for i in range(cnt)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fview'); ap.add_argument('--capture'); ap.add_argument('--frames'); ap.add_argument('--out')
    import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
    fe.add_allow_stale(ap)
    a = ap.parse_args()
    a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev')
    cap = pathlib.Path(a.capture)
    imgs = images(cap)
    report = {}
    for fr in [int(x) for x in a.frames.split(',')]:
        tmp = pathlib.Path(tempfile.gettempdir()) / ('t16_audit_ops_%d_%d.json' % (os.getpid(), fr))
        tmp.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([a.fview, '--capture', str(cap), '--frame', str(fr), '--ops', '--json', str(tmp)], capture_output=True, text=True, timeout=300)
        if not tmp.exists():
            print('fview failed', fr, r.stdout[-300:], r.stderr[-300:]); continue
        ops = json.load(open(tmp))['ops']
        fb, secs = frame_sections(cap / 'frames' / ('frame_%06d.bin' % fr))
        draws = sec(fb, secs, SEC_DRAWS); fixeds = sec(fb, secs, SEC_FIXEDS); slots = sec(fb, secs, SEC_SLOTS)
        copies = sec(fb, secs, SEC_COPIES); extras = sec(fb, secs, SEC_COPY_EXTRAS)
        sixteen = {i for i, m in imgs.items() if m['fmt'] in FMT}
        cur = []
        writers = collections.defaultdict(lambda: collections.Counter())
        biases = collections.defaultdict(set)
        readers = collections.defaultdict(lambda: collections.Counter())
        copied = collections.defaultdict(set)   # source image -> dest images
        for op in ops:
            if op['kind'] == 'targets':
                cur = op.get('colors', [])
            elif op['kind'] == 'draw':
                d = draws[op['index']]
                fixed_i, = struct.unpack_from('<I', d, 24)
                tf, tc = struct.unpack_from('<II', d, 32)
                fx = fixeds[fixed_i]
                bias = struct.unpack_from('<4b', fx, len(fx) - 12)
                for k, c in enumerate(cur[:4]):
                    if c in sixteen:
                        writers[c][op.get('effect', '?') + '/' + op.get('technique', '?')] += 1
                        if bias[k]: biases[c].add(bias[k])
                for s in range(tf, tf + tc):
                    sl = slots[s]
                    image, sampler, slot, view, = struct.unpack_from('<IIBB', sl, 0)
                    sign = struct.unpack_from('<4B', sl, 14)
                    t16 = sl[21]
                    lodbias, t21, w3 = struct.unpack_from('<fii', sl, 24)
                    readers[image][(op.get('effect', '?'), 'slot%d' % slot, 'sign%s' % ''.join(map(str, sign)), 't16=%d' % t16, 't21=%d' % t21, 'w3=%d' % w3)] += 1
            elif op['kind'] == 'copy':
                c = copies[op['index']]
                src, dst = struct.unpack_from('<II', c, 0)[0], struct.unpack_from('<II', c, 8)[0]
                copied[src].add(dst)
        # the shadow-class table (lane 170, question a/b): every draw of class 'shadow' or 'wrestlers' reading a slot: technique -> target format, formats read
        shadow = collections.OrderedDict()
        cur = []
        for op in ops:
            if op['kind'] == 'targets': cur = op.get('colors', [])
            elif op['kind'] == 'draw' and op.get('class') in ('shadow',):
                d = draws[op['index']]
                tf, tc = struct.unpack_from('<II', d, 32)
                tgt = ','.join('%d:%s' % (c, imgs[c]['fmt']) for c in cur[:1])
                reads = sorted({'%d:fmt%d/gf%d' % (struct.unpack_from('<I', slots[x], 0)[0], imgs.get(struct.unpack_from('<I', slots[x], 0)[0], {}).get('fmt', -1), imgs.get(struct.unpack_from('<I', slots[x], 0)[0], {}).get('game_format', -1)) for x in range(tf, tf + tc)})
                shadow.setdefault((op.get('technique', '?'), op.get('effect', '?'), tgt, tuple(reads)), 0)
                shadow[(op.get('technique', '?'), op.get('effect', '?'), tgt, tuple(reads))] += 1
        print('  shadow-class draws (technique, effect, colour target id:rdr::Format, textures read id:fmt/game_format) x count; rdr::Format 1 RGBA8, 16 RGBA16F, 4 R8')
        for k, v in shadow.items(): print('    ', k, 'x%d' % v)
        report.setdefault('shadow_%d' % fr, [dict(technique=k[0], effect=k[1], target=k[2], reads=list(k[3]), draws=v) for k, v in shadow.items()])
        # the whole-frame census of the two exponent fields (lane 170, sibling audit): any draw with a colour exp bias on ANY target, any fetch with a non-zero exp_adjust
        nb = collections.Counter(); nx = collections.Counter(); ns = collections.Counter(); nd = len(draws)
        for op in ops:
            if op['kind'] != 'draw': continue
            d = draws[op['index']]
            fixed_i, = struct.unpack_from('<I', d, 24)
            fx = fixeds[fixed_i]
            if any(struct.unpack_from('<4b', fx, len(fx) - 12)): nb[op.get('effect', '?') + '/' + op.get('technique', '?')] += 1
            tf, tc = struct.unpack_from('<II', d, 32)
            for x in range(tf, tf + tc):
                t21, w3 = struct.unpack_from('<ii', slots[x], 28)
                if t21 or w3: nx[(op.get('effect', '?'), 't21=%d' % t21, 'w3=%d' % w3)] += 1
                if any(slots[x][14:18]): ns[(op.get('effect', '?'), tuple(slots[x][14:18]))] += 1
        print('  census: %d draws; colour exp bias != 0 on %d draws %s; fetch exp_adjust (word4 13-18 / word3 13-18) != 0 on %d slots %s; fetch with a sign != 0 on %d slots %s' % (
            nd, sum(nb.values()), dict(nb), sum(nx.values()), dict(nx), sum(ns.values()), dict(list(ns.items())[:6])))
        report.setdefault('census_%d' % fr, dict(bias=dict(nb), exp=[[list(k), v] for k, v in nx.items()], signs=[[list(k), v] for k, v in ns.items()]))
        rows = []
        for img in sorted(sixteen):
            m = imgs[img]
            dests = sorted(copied.get(img, ()))
            # follow copies of copies once
            rd = {}
            for t in [img] + dests:
                for k, v in readers.get(t, {}).items(): rd[(t,) + k] = v
            if not writers.get(img) and not dests and not rd:
                continue
            rows.append(dict(image=img, size='%dx%d' % (m['w'], m['h']), storage=FMT[m['fmt']], game_format=m['game_format'], writers=dict(writers.get(img, {})),
                             color_exp_bias=sorted(biases.get(img, ())), copies_to=dests, readers=[dict(image=k[0], effect=k[1], slot=k[2], sign=k[3], target16_read=k[4], t21=k[5], word3=k[6], draws=v) for k, v in rd.items()]))
        report[fr] = rows
        print('frame', fr, ':', len(rows), '16-bit images in use')
        for r_ in rows:
            print(' ', r_['image'], r_['size'], r_['storage'], 'gf', r_['game_format'], 'writers', r_['writers'], 'bias', r_['color_exp_bias'], 'copies_to', r_['copies_to'])
            for rr in r_['readers']:
                print('      reads via image', rr['image'], rr['effect'], rr['slot'], rr['sign'], rr['target16_read'], 't21', rr['t21'], 'w3', rr['word3'], 'x%d' % rr['draws'])
    if a.out:
        json.dump(report, open(a.out, 'w'), indent=1)


main()

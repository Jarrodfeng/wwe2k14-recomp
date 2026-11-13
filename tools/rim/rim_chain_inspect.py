"""Lane 151: what the rim light map's reduction and blur chain is, per frame of a capture (inputs, outputs, sizes, formats, samplers, constants, readers). Numbers only; reads the capture, no game.
  py -I tools\\rim\\rim_chain_inspect.py CAPTURE_DIR FRAME[,FRAME] [--vs] [--hex]"""
import sys, os, struct, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import shadow_census as sc
import numpy as np

cap = sc.Cap(sys.argv[1]); frames = [int(x) for x in sys.argv[2].split(',')]
show_vs = '--vs' in sys.argv
show_hex = '--hex' in sys.argv


def fl(bank, r, n=1):
    return [tuple(round(float(x), 5) for x in f_regs(bank, r + i)) for i in range(n)]


def imgdesc(i):
    r = cap.imgs.get(i)
    return 'img%d %dx%d fmt%d gf%d role%d' % (i, r['w'], r['h'], r['format'], r['gf'], r['role']) if r is not None else 'img%d ?' % i


for fi in frames:
    f = cap.frame(fi); d = f.draws; o = f.origins
    f_regs = lambda bank, r: f.bank(bank)[4 * r:4 * r + 4].view('<f4')
    ps = o['pass_site'].astype(np.int64); cs = sc.pcap.queue_site(o)
    copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
    ve, pe = cap.effs(d['vs_hash']), cap.effs(d['ps_hash'])
    passes = sc.walk(f, copies)
    print('=== frame', fi, 'guest', f.guest_frame, 'ops', len(f.ops), 'draws', len(d))
    kinds = []
    for p in passes:
        if not p.draws:
            continue
        ix = np.array(p.draws)
        kind = None
        if (ps[ix] == sc.SITE_RIM).any():
            kind = 'rim'
        elif ((cs[ix] == sc.CALL_RIMRED) & (pe[ix] == 'yBlur/flag_any')).any():
            kind = 'rim_reduce'
        elif (cs[ix] == sc.CALL_MINBLUR).any() and (pe[ix][cs[ix] == sc.CALL_MINBLUR][0] == 'yBlur/t2'):
            kind = 'rim_t2'
        if kind is None:
            continue
        kinds.append(kind)
        print('-- pass %s at op %d target %s depth %d clear %s draws %d copies %d' % (kind, p.pos, imgdesc(p.target), p.depth, p.clear, len(p.draws), len(p.copies)))
        if kind == 'rim':
            for ci in p.copies:
                c = copies[ci]
                print('  copy: %s -> %s src %s dst %s' % (imgdesc(int(c['s']['image'])), imgdesc(int(c['d']['image'])), [int(x) for x in c['sr']], [int(x) for x in c['dr']]))
            continue
        for di in p.draws:
            dr = d[di]
            fx = f.raw[sc.capread.SEC['fixeds']][dr['fixed']]; dy = f.raw[sc.capread.SEC['dynamics']][dr['dynamic']]
            vp = struct.unpack_from('<6f', dy, 0); sci = struct.unpack_from('<iiII', dy, 24); nd = struct.unpack_from('<12f', dy, 136)
            blend = struct.unpack_from('<8B', fx, 24)
            print('  draw %d: vs %s ps %s call %#x site %#x verts %d idx %d vbytes %d layout %d' % (di, ve[di], pe[di], cs[di], ps[di], dr['vertex_count'], dr['index_count'], dr['vertex_bytes'], dr['layout']))
            print('    viewport %s scissor %s ndc_scale %s ndc_off %s blend %s' % (tuple(round(v, 2) for v in vp[:4]), sci, tuple(round(v, 4) for v in nd[0:3]), tuple(round(v, 4) for v in nd[3:6]), blend))
            for s in f.slots[dr['tex_first']:dr['tex_first'] + dr['tex_count']]:
                smp = f.raw[sc.capread.SEC['samplers']][s['sampler']]
                print('    slot %d: %s view %d swz %s sampler %s' % (s['slot'], imgdesc(int(s['image'])), s['view_format'], list(s['swizzle']), smp.hex()))
            nreg = 4 if kind == 'rim_reduce' else 13
            print('    ps SampleOffsets c0..: %s' % (fl(dr['ps_bank'], 0, nreg),))
            print('    ps SampleWeights c16..: %s' % (fl(dr['ps_bank'], 16, nreg),))
            if show_vs:
                print('    vs c32-c33: %s' % (fl(dr['vs_bank'], 32, 2),))
            if show_hex:
                vb = f.vertices(dr).tobytes()
                print('    vertices(hex): %s' % vb.hex())
                if dr['index_bytes']:
                    print('    indices(hex): %s' % bytes(cap.geo[int(dr['index_offset']):int(dr['index_offset']) + int(dr['index_bytes'])]).hex())
        for ci in p.copies:
            c = copies[ci]
            print('  copy: %s mip%d -> %s mip%d  src %s dst %s filt %d' % (imgdesc(int(c['s']['image'])), c['s']['mip'], imgdesc(int(c['d']['image'])), c['d']['mip'], [int(x) for x in c['sr']], [int(x) for x in c['dr']], c['filt']))
    print('chain order:', kinds)
    s15 = collections.Counter()
    for j in range(len(d)):
        for s in f.slots[d[j]['tex_first']:d[j]['tex_first'] + d[j]['tex_count']]:
            if s['slot'] == 15:
                s15[(imgdesc(int(s['image'])), ve[j].split('/')[0])] += 1
    for k, v in sorted(s15.items()):
        print('slot15 reader', v, k)

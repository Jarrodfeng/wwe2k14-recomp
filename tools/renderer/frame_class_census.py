r"""S102 lane G11: the `frame` GPU class split into its passes, draws and techniques (offline, 0 game runs).
rplay writes the trace (`--frame-trace OUT.jsonl`, with `--gpu-stamps 1 --gpu-census 1` for the per-pass GPU ms):
  t=p  one frame-class pass: target image, size, load / store ops (0 = LOAD / STORE, 1 = CLEAR / DONT_CARE), the op range, the textures sampled
  t=d  one draw: area (viewport x scissor bound), blend of colour target 0, depth test / write, viewport, scissor
  t=o  the pixels that draw WROTE (a precise occlusion query: fragments past depth / stencil and the shader's discard)
  t=c  an in-pass clear
  t=g  the pass's GPU ms (census mode: a stamp after every step)
This tool joins the trace with the capture (the draw's op -> its origin: draw class, technique, pass site) and prints, per pass and per technique: draws, pixels written,
overdraw (pixels / frame-buffer pixels), blended share, full-screen quads, GPU ms. --json writes the same as a file. Counts and names only (hard rule 1).
  py -I tools\renderer\frame_class_census.py CAPTURE_DIR TRACE.jsonl [--frames N] [--json OUT] [--ratio R]
--ratio R = Surface / PC GPU-ms ratio for the frame class (the Surface frame class 4.07 ms over the PC's measured value, by default read from --pc-ms and --surface-ms)."""
import argparse, collections, json, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import pcap  # noqa: E402

CLASS_NAMES = ['unnamed', 'model_part', 'crowd', 'sprite', 'text', 'movie', 'particle', 'shadow', 'post', 'clear', 'other']
LOAD = {0: 'LOAD', 1: 'CLEAR', 2: 'DONT_CARE'}
STORE = {0: 'STORE', 1: 'DONT_CARE'}


def read_trace(path):
    rows = collections.defaultdict(lambda: dict(p=[], d={}, o={}, c=[], g={}))
    n = 0
    for line in open(path, encoding='utf-8'):
        if not line.strip():
            continue
        r = json.loads(line)
        n += 1
        f = rows[r['f']]
        t = r['t']
        if t == 'p':
            f['p'].append(r)
        elif t == 'd':
            f['d'][(r['p'], r['op'])] = r
        elif t == 'o':
            f['o'][(r['p'], r['op'])] = r['px']
        elif t == 'c':
            f['c'].append(r)
        elif t == 'g':
            f['g'][r['p']] = r['ms']
    if not n:
        raise SystemExit('FAIL: %s holds no trace lines' % path)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('trace')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--json', default='')
    ap.add_argument('--no-join', action='store_true', help='skip the capture join (a trace of a run that dropped ops: the builder\'s op numbers are no longer the capture\'s): passes and pixels only')
    ap.add_argument('--pc-ms', type=float, default=0.0, help='the PC frame class ms a frame (the roles line)')
    ap.add_argument('--surface-ms', type=float, default=4.07, help='the Surface frame class ms a frame (S102 stamps arm)')
    a = ap.parse_args()
    tr = read_trace(a.trace)
    fs = sorted(tr)
    # the trace's frame serials are the renderer's own; the first traced frame is the first rendered one with a frame-class pass. Frames the renderer dropped leave gaps: the capture
    # frame of serial f is looked up by the op range (a pass's first_op / end_op must land on draw ops of that capture frame).
    cap = pcap.Capture(a.capture)
    nf = min(len(fs), len(cap.frames))
    if a.frames:
        nf = min(nf, a.frames)
    per_pass = collections.defaultdict(lambda: collections.Counter())
    per_tech = collections.defaultdict(lambda: collections.Counter())
    per_site = collections.defaultdict(lambda: collections.Counter())
    pass_meta = {}
    bad_join = 0
    frame_px = 0
    big = 0
    totals = collections.Counter()
    for k in range(nf):
        f = fs[k]
        t = tr[f]
        cf = cap.frame(k)
        secs = cf.secs
        strings = b''
        if 67 in secs:
            es, cnt, off = secs[67]
            strings = bytes(cf.buf[off:off + cnt * es])

        def string(o):
            if not o:
                return ''
            j = strings.find(b'\0', o - 1)
            return strings[o - 1:j if j >= 0 else len(strings)].decode('utf-8', 'replace')
        # frame ops of the renderer == capture ops (rplay plays the capture's own op list)
        ops = cf.ops
        for pr in t['p']:
            idx = pr['p']
            key = idx
            fb_px = pr['w'] * pr['h']
            frame_px = fb_px
            pass_meta.setdefault(idx, dict(w=pr['w'], h=pr['h'], img=pr['img'], depth=pr['depth'], load=pr['load'], store=pr['store'], sampled=pr['sampled'], first_op=pr['first_op'], n=0))
            pass_meta[idx]['n'] += 1
            c = per_pass[idx]
            c['frames'] += 1
            c['ms'] += t['g'].get(idx, 0.0)
        for (p, op), d in sorted(t['d'].items()):
            if a.no_join:
                kl, tech, site = 'n/a', '-', 0
            elif op >= len(ops) or int(ops['kind'][op]) != 2:
                bad_join += 1
                continue
            else:
                oi = int(ops['index'][op])
            if a.no_join:
                pass
            elif cf.has_origins and oi < len(cf.origins):
                o = cf.origins[oi]
                kl = CLASS_NAMES[int(o['klass'])]
                tech = string(int(o['technique'])) or '-'
                site = int(o['pass_site'])
            else:
                kl, tech, site = 'no_origin', '-', 0
            px = t['o'].get((p, op), 0)
            fb_px = pass_meta[p]['w'] * pass_meta[p]['h']
            blended = d['blend'][0] == 1
            full = d['area'] >= 0.9 * fb_px
            for c in (per_pass[p], per_tech[(kl, tech)], per_site[site]):
                c['draws'] += 1
                c['px'] += px
                c['area'] += d['area']
                c['blended_draws'] += blended
                c['blended_px'] += px if blended else 0
                c['full_bound_draws'] += full
                c['full_px_draws'] += px >= 0.9 * fb_px
                c['zero_px_draws'] += px == 0
                c['verts'] += d['verts']
                c['depth_test_draws'] += d['dt']
            per_tech[(kl, tech)]['passes_seen'] |= 0
            per_tech[(kl, tech)]['p%d' % p] += 1
            per_site[site]['p%d' % p] += 1
        for cl in t['c']:
            per_pass[cl['p']]['inpass_clears'] += 1
    n = float(nf)
    print('frame-class trace: %d frames joined to the capture; %d draws whose op did not land on a draw op of the capture frame (must be 0)' % (nf, bad_join))
    print('')
    print('%-4s %-8s %-9s %-12s %-12s %5s %6s %9s %8s %7s %7s %6s %9s' % ('pass', 'size', 'image', 'load', 'store', 'draws', 'zeroPx', 'Kpx/frm', 'overdraw', 'blend%', 'fullbnd', 'inclr', 'ms(PC)'))
    out = dict(frames=nf, passes=[], techniques=[], sites=[])
    sum_px = 0
    sum_ms = 0.0
    for p in sorted(per_pass):
        c = per_pass[p]
        m = pass_meta[p]
        fbpx = m['w'] * m['h']
        px = c['px'] / n
        sum_px += px
        sum_ms += c['ms'] / n
        ld = '/'.join(LOAD.get(x, str(x)) for x in m['load'][:1] + m['load'][4:])
        st = '/'.join(STORE.get(x, str(x)) for x in m['store'][:1] + m['store'][4:])
        print('%-4d %4dx%-4d %-9d %-12s %-12s %5.1f %6.1f %9.1f %8.2f %6.1f%% %7.1f %6.1f %9.4f' % (
            p, m['w'], m['h'], m['img'], ld, st, c['draws'] / n, c['zero_px_draws'] / n, px / 1e3, px / fbpx, 100.0 * c['blended_px'] / c['px'] if c['px'] else 0.0, c['full_bound_draws'] / n,
            c['inpass_clears'] / n, c['ms'] / n))
        out['passes'].append(dict(pass_index=p, w=m['w'], h=m['h'], image=m['img'], depth=m['depth'], load=m['load'], store=m['store'], sampled=m['sampled'], draws=c['draws'] / n, zero_px_draws=c['zero_px_draws'] / n,
                                  px=px, overdraw=px / fbpx, blended_px_share=(c['blended_px'] / c['px'] if c['px'] else 0.0), full_bound_draws=c['full_bound_draws'] / n, full_px_draws=c['full_px_draws'] / n, ms=c['ms'] / n))
    fbpx = frame_px or 1
    print('total: %.1f Kpx written a frame = %.2f x the %d px frame buffer; GPU ms (PC census) %.4f' % (sum_px / 1e3, sum_px / fbpx, fbpx, sum_ms))
    print('')
    print('%-9s %-28s %6s %6s %9s %8s %7s %8s  %s' % ('class', 'technique', 'draws', 'zeroPx', 'Kpx/frm', 'overdrw', 'blend%', 'fullpx', 'passes (draws a frame)'))
    for (kl, tech), c in sorted(per_tech.items(), key=lambda kv: -kv[1]['px']):
        ps = ' '.join('p%s:%.1f' % (k[1:], v / n) for k, v in sorted(c.items()) if k.startswith('p') and k[1:].isdigit())
        print('%-9s %-28s %6.1f %6.1f %9.1f %8.2f %6.1f%% %8.2f  %s' % (kl, tech[:28], c['draws'] / n, c['zero_px_draws'] / n, c['px'] / n / 1e3, c['px'] / n / fbpx, 100.0 * c['blended_px'] / c['px'] if c['px'] else 0.0, c['full_px_draws'] / n, ps))
        out['techniques'].append(dict(klass=kl, technique=tech, draws=c['draws'] / n, zero_px_draws=c['zero_px_draws'] / n, px=c['px'] / n, overdraw=c['px'] / n / fbpx,
                                      blended_px_share=(c['blended_px'] / c['px'] if c['px'] else 0.0), full_px_draws=c['full_px_draws'] / n, passes=ps))
    print('')
    print('%-10s %6s %6s %9s %8s' % ('pass site', 'draws', 'zeroPx', 'Kpx/frm', 'overdrw'))
    for site, c in sorted(per_site.items(), key=lambda kv: -kv[1]['px']):
        print('0x%08X %6.1f %6.1f %9.1f %8.2f' % (site, c['draws'] / n, c['zero_px_draws'] / n, c['px'] / n / 1e3, c['px'] / n / fbpx))
        out['sites'].append(dict(site='0x%08X' % site, draws=c['draws'] / n, px=c['px'] / n))
    if a.pc_ms:
        r = a.surface_ms / a.pc_ms
        print('')
        print('Surface / PC ratio for the frame class: %.4f / %.4f = %.1fx (an ESTIMATE: the PC time of 5 small passes is mostly per-pass overhead, so the ratio is large and says nothing about the fill)' % (a.surface_ms, a.pc_ms, r))
        out['ratio'] = r
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)
    return 1 if bad_join else 0


if __name__ == '__main__':
    sys.exit(main())

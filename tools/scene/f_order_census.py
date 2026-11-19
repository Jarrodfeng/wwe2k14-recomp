r"""S103 lane F: the order of the see-through draws in a stream capture (offline, reads only).
  py -I tools\scene\f_order_census.py CAPTURE_DIR [--frames 20,110,200]
Per frame: the model-part / particle / crowd draws in op-list order, with the blend state, depth write, effect, technique and object, folded into
runs. Numbers and names only; nothing from the game is written."""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
import pcap  # noqa: E402


def strings(fr):
    sid = pcap.SEC['strings']
    if sid not in fr.secs:
        return b''
    es, cnt, off = fr.secs[sid]
    return bytes(fr.buf[off:off + cnt])


def name(blob, ref):
    if not ref:
        return ''
    o = int(ref) - 1
    e = blob.find(b'\0', o)
    return blob[o:e].decode('utf8', 'replace')


def draw_rows(fr):
    blob = strings(fr)
    di, pos = fr.draw_ops()
    rows = []
    for d_i, p in zip(di, pos):
        d = fr.draws[d_i]
        fx = fr.raw[pcap.SEC['fixeds']][int(d['fixed'])]
        o = fr.origins[d_i] if fr.has_origins else None
        rows.append(dict(i=int(d_i), pos=int(p), klass=pcap.CLASS_NAMES[int(o['klass'])] if o is not None else '?',
                         pass_site=int(o['pass_site']) if o is not None else 0, call_site=int(o['call_site']) if o is not None else 0,
                         object=int(o['object']) if o is not None else 0, part=int(o['part']) if o is not None else 0,
                         tech=name(blob, o['technique']) if o is not None else '',
                         asset=name(blob, o['asset']) if o is not None else '',
                         blend=fx[24], src=fx[25], dst=fx[26], zw=fx[9], zt=fx[8], zc=fx[10], atest=fx[56],
                         fx=pcap.effect_of(d['ps_hash']), vs=pcap.effect_of(d['vs_hash'])))
    return rows


def summary(rows):
    last = None
    n = 0
    objs = set()

    def flush():
        if last:
            print('  %-10s %-34s blend=%d dst=%d zw=%d at=%d  draws=%-4d objects=%d' % (last + (n, len(objs))))
    for r in rows:
        if r['klass'] in ('shadow', 'post', 'sprite', 'text', 'movie', 'clear', 'other'):
            continue
        key = (r['klass'], r['fx'].split('/')[0][:34], r['blend'], r['dst'] if r['blend'] else 0, r['zw'], r['atest'])
        if key == last:
            n += 1
            objs.add(r['object'])
            continue
        flush()
        last, n, objs = key, 1, {r['object']}
    flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cap')
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--summary', action='store_true', help='one line per group of consecutive draws with the same effect / state (objects merged)')
    a = ap.parse_args()
    cap = pcap.Capture(a.cap)
    for f in [int(x) for x in a.frames.split(',')]:
        fr = cap.frame(f)
        rows = draw_rows(fr)
        print('== frame', f, 'draws', len(rows), 'classes', dict(collections.Counter(r['klass'] for r in rows)))
        if a.summary:
            summary(rows)
            continue
        last = None
        n = 0
        for r in rows:
            if r['klass'] in ('shadow', 'post', 'sprite', 'text', 'movie', 'clear', 'other'):
                continue
            key = (r['klass'], r['tech'], r['blend'], r['zw'], r['zt'], r['atest'], r['fx'], r['object'])
            if key == last:
                n += 1
                continue
            if last:
                print('   x%d' % n)
            last = key
            n = 1
            print('  %-10s tech=%-14s blend=%d(%d>%d) zw=%d zt=%d at=%d fx=%-28s obj=%08X pass=%08X call=%08X asset=%s' % (
                r['klass'], r['tech'], r['blend'], r['src'], r['dst'], r['zw'], r['zt'], r['atest'], r['fx'][:28], r['object'], r['pass_site'],
                r['call_site'], r['asset'][:40]), end='')
        if last:
            print('   x%d' % n)


if __name__ == '__main__':
    main()

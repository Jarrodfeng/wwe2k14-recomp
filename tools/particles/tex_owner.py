r"""Lane 69 (S101, lane N of docs\renderer\PARTICLES_PROMOTION_MAP.md): which pac entry owns each particle / flash texture. Offline, 0 game runs.

For each capture, every draw of class particle (origin klass 6) and every crowd-flash draw (pass site 0x82C139FC) is found; its slot-0 image's captured bytes (textures.bin
RCTX blob, the content as of that frame) are joined to a texture in the player's own pacs by bytes (tools\ui\ui_pacs.py's hash index of every DDS in every pac, which
includes prtcls.pac and every bgNN.pac), and a raw window search (find_bytes.py's method) is tried for anything the hash join misses. Output: names, ids, counts only.

  py -I tools\particles\tex_owner.py NAME=CAPTURE [NAME=CAPTURE ...] [--plant corrupt_image] [--json OUT] [-q]
CAPTURE = a case zip or a capture folder (relative paths are looked up in the main checkout).
Checks (exit 1 when any FAILs or an input cannot be read):
  index     every pac was read.
  read      every capture opened and held draws of the class (a reader that finds nothing must fail).
  classes   per capture: the particle slot-0 image ids and the flash image ids (compared with expect_tex.json when it names the capture).
  content   every image has a captured content blob of the expected shape (RCTX).
  owner     every image is joined to a pac entry (exact: all captured bytes equal the entry's; mip0: only mip 0) or carries a stated [U] reason from expect_tex.json.
--plant corrupt_image: the first particle image's bytes are corrupted in the loaded copy; the join must say "not found" (so `owner` FAILs for it)."""
import argparse
import collections
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
for sub in ('ui', 'arena', 'solver', 'wm30'):
    sys.path.insert(0, os.path.join(ROOT, 'tools', sub))
sys.path.insert(0, HERE)
import join_census as jc  # noqa: E402
import pcap  # noqa: E402
import ui_join_census as uj  # noqa: E402
import ui_pacs as up  # noqa: E402

K_PARTICLE = 6
PARTICLE_SITES = (0x82A0DA58, 0x82A0E338, 0x82A0ED2C)  # census.py: the three particle methods (sprites, oriented quads, the third)
FLASH_PASS_SITE = 0x82C139FC
PLANTS = ('corrupt_image',)


def resolve(p):
    return p if os.path.isabs(p) else os.path.join(pcap.MAIN, p)


class Res:
    def __init__(self, quiet):
        self.checks, self.quiet = [], quiet

    def add(self, name, ok, detail):
        self.checks.append((name, ok, detail))
        print('%s  %-8s %s' % ('PASS' if ok else 'FAIL', name, detail))

    def info(self, t):
        if not self.quiet:
            print('      ' + t)


def collect(path, plant):
    """-> dict(images {id: dict(kinds, draws, frames, info)}, frames, counts, cap). Visits every frame in order so contents are as of the draw."""
    cap = jc.Capture(path)
    n = len(cap.frames)
    imgs = {}
    cache = {}
    counts = collections.Counter()
    for i in range(n):
        f = cap.frame(i)
        cap.advance_contents(f, i)
        if f.origins is None:
            raise pcap.PError('%s: no origins (a version-1 capture: no draw can be classed)' % path)
        for k in range(len(f.draws)):
            og = f.origins[k]
            kind = None
            if int(og['klass']) == K_PARTICLE or int(og['call_site']) in PARTICLE_SITES:
                kind = 'particle'
            elif int(og['pass_site']) == FLASH_PASS_SITE:
                kind = 'flash'
            if kind is None:
                continue
            d = f.draws[k]
            if int(d['tex_count']) < 1:
                counts[kind + '_untextured'] += 1
                continue
            img = int(f.slots[int(d['tex_first'])]['image'])
            counts[kind] += 1
            e = imgs.setdefault(img, dict(kinds=set(), draws=0, frames=set(), info=None, c=None))
            e['kinds'].add(kind)
            e['draws'] += 1
            e['frames'].add(i)
            e['c'] = cap.contents.get(img)
            e['info'] = uj.blob_info(cap, img, cache)  # the latest content as of the last use
    if plant == 'corrupt_image':
        first = min((im for im, e in imgs.items() if 'particle' in e['kinds'] and e['info']), default=None)
        if first is not None:
            e = imgs[first]
            arr = np.array(cap.tex, copy=True)
            info = e['info']
            for j in range(info['off'], info['off'] + info['db'], 61):  # every 61st byte: no 64-byte window can stay equal
                arr[j] ^= 0xFF
            cap._tex = arr
            e['info'] = uj.blob_info(cap, first, {})
            e['planted'] = True
    return dict(images=imgs, frames=n, counts=counts, cap=cap)


def raw_search(infos, cap):
    """A 64-byte window of each unjoined image's bytes searched in every decoded pac leaf (find_bytes.py's method). -> {image: [(pac, leaf, offset)]}"""
    needles = []
    for im, info in infos.items():
        data = bytes(cap.tex[info['off']:info['off'] + info['db']])
        mid = len(data) // 2
        needles.append(('%d' % im, data[mid:mid + 64]))
    hits, errors, _ = up.scan_needles(needles, log=lambda s: print('      ' + s))
    out = collections.defaultdict(list)
    for nid, rel, leaf, off in hits:
        out[int(nid)].append((rel, leaf, off))
    return out, errors


def analyse(label, path, plant, lk, expect, R):
    try:
        got = collect(resolve(path), plant)
    except (jc.CensusError, pcap.PError, FileNotFoundError, OSError) as e:
        R.add('read', False, '%s: %s' % (label, e))
        return None
    imgs, counts = got['images'], got['counts']
    R.add('read', sum(counts[k] for k in ('particle', 'flash')) > 0, '%s: %d frames, %d particle draws, %d flash draws, %d distinct slot-0 images' % (
        label, got['frames'], counts['particle'], counts['flash'], len(imgs)))
    pimg = sorted(im for im, e in imgs.items() if 'particle' in e['kinds'])
    fimg = sorted(im for im, e in imgs.items() if 'flash' in e['kinds'])
    R.info('%s particle images %s; flash images %s' % (label, pimg, fimg))
    ex = expect.get(label)
    if ex is not None:
        ok = pimg == ex['particle_images'] and fimg == ex['flash_images']
        R.add('classes', ok, '%s: particle %s, flash %s (expected %s / %s)' % (label, pimg, fimg, ex['particle_images'], ex['flash_images']))
    else:
        R.add('classes', bool(imgs), '%s: no expectation recorded; %d images' % (label, len(imgs)))
    rows, unjoined = {}, {}
    for im in sorted(imgs):
        info = imgs[im]['info']
        if info is None:
            rows[im] = dict(status='no_content')
            continue
        kind, cands = uj.join_image(info, lk)
        rows[im] = dict(status=kind, cands=cands, info=info)
        if kind == 'none':
            unjoined[im] = info
    nc = sum(r['status'] == 'no_content' for r in rows.values())
    R.add('content', nc == 0, '%s: %d of %d images hold a captured RCTX blob' % (label, len(rows) - nc, len(rows)))
    raw, errs = {}, []
    if unjoined:
        raw, errs = raw_search(unjoined, got['cap'])
        for im in unjoined:
            rows[im]['raw'] = raw.get(im, [])
    return dict(label=label, imgs=imgs, rows=rows, pimg=pimg, fimg=fimg, errs=errs, counts=counts)


def describe(r):
    if r['status'] in ('exact', 'mip0'):
        pacs = sorted({c[0] for c in r['cands']})
        names = sorted({c[3] for c in r['cands'] if c[3]})
        c0 = r['cands'][0]
        where = '%d pacs (%s .. %s)' % (len(pacs), pacs[0], pacs[-1]) if len(pacs) > 4 else ', '.join(pacs)
        return '%s: %s leaf %s%s' % (r['status'], where, c0[1], (' name %s' % ','.join(names[:3])) if names else '')
    if r['status'] == 'none':
        raw = r.get('raw') or []
        if raw:
            return 'raw window found: ' + ', '.join('%s %s +%#x' % tuple(h) for h in raw[:3])
        return 'not found'
    return r['status']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--expect', default=os.path.join(HERE, 'expect_tex.json'))
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--json')
    ap.add_argument('--reindex', action='store_true')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    R = Res(a.q)
    expect = json.load(open(a.expect, encoding='utf-8')) if os.path.exists(a.expect) else {}
    reasons = expect.get('_unknown', {})
    index = up.index_all(reindex=a.reindex, log=lambda s: print('      ' + s))
    bad = [rel for rel, e in index.items() if e.get('error')]
    R.add('index', bool(index) and not bad, '%d pacs indexed, %d with read errors %s' % (len(index), len(bad), bad[:3]))
    lk = uj.build_lookup(index)
    out = {}
    for spec in a.captures:
        label, _, path = spec.partition('=')
        res = analyse(label, path, a.plant, lk, expect, R)
        if res is None:
            continue
        out[label] = res
        print('  -- %s: image -> owner' % label)
        for im in sorted(res['rows']):
            r, e = res['rows'][im], res['imgs'][im]
            info = r.get('info') or {}
            line = 'image %5d  %-8s draws %6d frames %3d  %sx%s fmt %s mips %s  %s' % (im, '+'.join(sorted(e['kinds'])), e['draws'], len(e['frames']), info.get('w', '?'),
                                                                                       info.get('h', '?'), info.get('fmt', '?'), info.get('mips', '?'), describe(r))
            if e.get('planted'):
                line += '   <- PLANTED (bytes flipped)'
            print('    ' + line)
        miss = [im for im, r in res['rows'].items() if r['status'] in ('none', 'no_content') and not r.get('raw')]
        unk = [im for im in miss if str(im) in reasons.get(label, {})]
        for im in unk:
            print('    [U] image %d: %s' % (im, reasons[label][str(im)]))
        left = [im for im in miss if im not in unk]
        joined = sum(1 for r in res['rows'].values() if r['status'] in ('exact', 'mip0') or r.get('raw'))
        R.add('owner', not left, '%s: %d of %d images named, %d [U] with a stated reason, %d not found %s' % (label, joined, len(res['rows']), len(unk), len(left), left))
        if res['errs']:
            R.add('search', False, 'raw search errors %s' % res['errs'][:3])
    R.add('any', bool(out), '%d captures analysed' % len(out))
    if a.json:
        js = {lab: {str(im): dict(kinds=sorted(res['imgs'][im]['kinds']), draws=res['imgs'][im]['draws'], status=r['status'],
                                  owners=[list(c[:4]) for c in r.get('cands', [])][:20]) for im, r in res['rows'].items()} for lab, res in out.items()}
        json.dump(js, open(a.json, 'w'), indent=1)
    bad = [c for c in R.checks if not c[1]]
    print('%d checks, %d problems' % (len(R.checks), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

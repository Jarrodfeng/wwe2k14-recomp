"""Lane 135: the read-first census of the small passes (sweat map yLiquidMap, skin pre-light yPreLight). Offline, 0 game runs, numbers only.

  py -I tools\\small\\small_census.py LABEL=CAPTURE [LABEL=CAPTURE ...] [--step N] [--json OUT]

Per capture and site (0x8279A19C / 0x82799CB4 / 0x82799DC8 = yLiquidMap copy / copy / sweat, 0x82C20B90 = yPreLight sss) over every Nth frame:
 draws per frame (min..max), the targets (image id, size, format) and the passes (clear flag, draws), the texture slots each draw reads (slot -> image, size, format),
 the vertex-constant ranges that vary (ScreenScale / WorldParam / ScreenSize / LocalArea; SrcChan / DstChan), who reads the output images later in the frame
 (effect family, slot), and whether the output image is also an INPUT of the same site (the feedback loop) and persists across frames (the image id is the same every frame).
Exit 1 when a capture cannot be read (never a pass)."""
import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'shadow'))
import numpy as np  # noqa: E402
import shadow_census as sc  # noqa: E402

SITES = {0x8279A19C: 'sweat_copy_a', 0x82799CB4: 'sweat_copy_b', 0x82799DC8: 'sweat_apply', 0x82C20B90: 'prelight'}
LIQ_V = ['ScreenScale', 'WorldParam', 'ScreenSize', 'LocalArea']
LIQ_P = ['SrcChan', 'DstChan']
PRE_V = ['ScreenScale']
PRE_P = ['DifLghVec', 'DifLghCol', 'DifLghMaxD', 'DifLghMaxA', 'DifLghPrm', 'SpecPow', 'SS']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('caps', nargs='+')
    ap.add_argument('--step', type=int, default=20)
    ap.add_argument('--json')
    a = ap.parse_args()
    out = {}
    fails = 0
    for spec in a.caps:
        label, path = spec.split('=', 1)
        if not os.path.isabs(path):
            path = os.path.join(sc.ROOT if os.path.isdir(os.path.join(sc.ROOT, path)) else 'C:/Users/Jarrod/wwe2k14-recomp', path)
        cap = sc.Cap(path)
        n = len(cap.frames)
        per = collections.defaultdict(lambda: {'frames': 0, 'draws': [], 'targets': collections.Counter(), 'slots': collections.Counter(), 'readers': collections.Counter(), 'clear': collections.Counter(),
                                              'vs_const': collections.defaultdict(set), 'ps_const': collections.defaultdict(set), 'in_eq_out': 0, 'effects': collections.Counter(), 'pass_draws': collections.Counter()})
        outimgs = collections.defaultdict(set)
        for fi in range(0, n, a.step):
            f = cap.frame(fi)
            if not f.has_origins:
                continue
            d, o = f.draws, f.origins
            cs = o['call_site'].astype(np.int64)
            copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
            passes = sc.walk(f, copies)
            pass_of = {}
            for p in passes:
                for i in p.draws:
                    pass_of[i] = p
            bysite = collections.defaultdict(list)
            for i in range(len(d)):
                if int(cs[i]) in SITES:
                    bysite[SITES[int(cs[i])]].append(i)
            # draws that sample each output image later in the frame
            sl_img = f.slots['image'].astype(np.int64)
            sl_slot = f.slots['slot'].astype(np.int64)
            tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
            owner = np.repeat(np.arange(len(d)), tc) if len(d) else np.zeros(0, np.int64)
            base = np.concatenate([np.arange(x, x + y) for x, y in zip(tf, tc)]) if len(d) else np.zeros(0, np.int64)
            for site, ix in bysite.items():
                S = per[site]
                S['frames'] += 1
                S['draws'].append(len(ix))
                outs = set()
                for i in ix:
                    dr = d[i]
                    p = pass_of.get(i)
                    if p is not None:
                        t = cap.imgs[p.target]
                        S['targets'][(p.target, int(t['w']), int(t['h']), int(t['format']))] += 1
                        S['clear']['clear' if p.clear else 'noclear'] += 1
                        S['pass_draws'][len(p.draws)] += 1
                        outs.add(p.target)
                    S['effects'][(cap.eff(int(dr['vs_hash'])), cap.eff(int(dr['ps_hash'])))] += 1
                    ins = set()
                    for s in f.slots[int(dr['tex_first']):int(dr['tex_first']) + int(dr['tex_count'])]:
                        im = cap.imgs.get(int(s['image']))
                        S['slots'][(int(s['slot']), int(s['image']), int(im['w']) if im is not None else 0, int(im['h']) if im is not None else 0, int(im['format']) if im is not None else -1)] += 1
                        ins.add(int(s['image']))
                    if p is not None and p.target in ins:
                        S['in_eq_out'] += 1
                    eff = cap.eff(int(dr['vs_hash'])).split('/')[0]
                    vb, pb = f.bank(int(dr['vs_bank'])), f.bank(int(dr['ps_bank']))
                    for nm in (LIQ_V if eff == 'yLiquidMap' else PRE_V):
                        S['vs_const'][nm].add(tuple(np.round(sc.regs(vb, sc.reg(eff, 'vertex', nm)).reshape(-1)[:4], 5).tolist()))
                    for nm in (LIQ_P if eff == 'yLiquidMap' else PRE_P):
                        try:
                            S['ps_const'][nm].add(tuple(np.round(sc.regs(pb, sc.reg(eff, 'fragment', nm)).reshape(-1)[:4], 5).tolist()))
                        except sc.CensusError:
                            pass
                for img in outs:
                    outimgs[site].add(img)
                    m = np.nonzero(sl_img[base] == img)[0] if len(base) else []
                    for j in m:
                        di = int(owner[j])
                        e = cap.eff(int(d[di]['vs_hash'])).split('/')[0]
                        S['readers'][(img, int(sl_slot[base[j]]), e, SITES.get(int(cs[di]), 'other'))] += 1
        out[label] = {}
        for site, S in per.items():
            out[label][site] = {
                'frames_with_site': S['frames'], 'of_frames_sampled': len(range(0, n, a.step)), 'draws_per_frame': [min(S['draws']), max(S['draws'])],
                'targets': [list(k) + [v] for k, v in S['targets'].most_common()], 'clear': dict(S['clear']), 'pass_draws': dict(S['pass_draws']),
                'effects': [[list(k), v] for k, v in S['effects'].most_common()],
                'slots': [list(k) + [v] for k, v in sorted(S['slots'].items())], 'input_is_own_target': S['in_eq_out'],
                'readers': [list(k) + [v] for k, v in sorted(S['readers'].items())],
                'vs_const': {k: sorted(v)[:4] + ([('...', len(v))] if len(v) > 4 else []) for k, v in S['vs_const'].items()},
                'ps_const': {k: sorted(v)[:4] + ([('...', len(v))] if len(v) > 4 else []) for k, v in S['ps_const'].items()}}
        if not per:
            print('FAIL  %s: no small-pass draw in the sampled frames' % label)
            fails += 1
    text = json.dumps(out, indent=1, default=str)
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        open(a.json, 'w').write(text)
    else:
        print(text)
    return 1 if fails else 0


sys.exit(main())

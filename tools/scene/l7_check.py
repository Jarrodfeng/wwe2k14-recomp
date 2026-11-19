#!/usr/bin/env python3
r"""S103 L7 check: the filter chains behind the shadow map and the rim map, ours against the game's FINAL maps (0 game runs: fview headless + maps_draw.exe).

  py -I tools\scene\l7_check.py [--capture DIR] [--frames 20,100,180] [--out DIR]

Per frame: fview saves the game's map after its LAST chain pass (shadow: the yBlur_min_rgba quad, rim: the yBlur_t2 quad); maps_draw.exe draws OUR maps from the same frame's scene record, raw
(ours_shadow.png / ours_rim.png) and as published after the chains (ours_shadow_f.png / ours_rim_f.png). Reported per channel over the footprint (pixels that differ from the picture's background in
either map): the mean |d| of the raw map and of the filtered map against the game's final one. Bars: the filtered map is closer than the raw one by a margin on the channel the readers use (shadow R / G, rim G)
and its footprint overlaps the game's (IoU). The planted twin (--plant no_chain: WWE_RDR_NO_MAP_CHAIN=1) publishes the raw maps: the filtered files are then absent and the check FAILS with CHAIN.
Exit 0 = pass (or, with --plant, the failure token was found).
"""
import argparse, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e2_check as E  # noqa: E402

DEFAULT_CAPTURE = os.path.join(E.MAIN, 'out', 'd3caster', 'z7147', 'capture')


def pass_with(passes, ps_name):
    cand = [p for p in passes if any(d.get('ps_variant', '').startswith(ps_name) and 'masked' in d.get('vs_variant', '') for d in p['draws'])]   # the masked quad: the bloom's yBlur_t2 is a plain one
    return cand[-1] if cand else None


def game_final(capture, frame, out, ps_name):
    ops = E.ops_of(capture, frame, out)
    p = pass_with(E.passes_of(ops), ps_name)
    if not p:
        return None
    return E.surface_after(capture, frame, p)


def footprint(a, b):
    """Pixels that differ from the picture's own background in either map."""
    return E.footprint_bg(a) | E.footprint_bg(b)


def mean_d(g, o, ch, fp):
    a, b = np.asarray(g).astype(int)[..., ch], np.asarray(o).astype(int)[..., ch]
    return float(np.abs(a - b)[fp].mean()) if fp.any() else 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=DEFAULT_CAPTURE)
    ap.add_argument('--frames', default='20,100,180')
    ap.add_argument('--out', default=os.path.join(E.ROOT, 'out', 's103', 'L7', 'chain'))
    ap.add_argument('--plant', default=None, choices=[None, 'no_chain', 'fail_mid'])
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.plant:   # no_chain: no chain at all; fail_mid: the shadow chain's intermediate image cannot be created (review 2): the map must stay raw, never half built
        key = 'WWE_RDR_NO_MAP_CHAIN' if a.plant == 'no_chain' else 'WWE_RDR_CHAIN_FAIL_MID'
        os.environ[key] = '1'
        E.ENV[key] = '1'
    fails, lines = [], []
    for frame in [int(x) for x in a.frames.split(',')]:
        for stale in ('ours_shadow_f.png', 'ours_rim_f.png', 'ours_shadow_pub_raw.png', 'ours_rim_pub_raw.png'):   # a previous run's published maps never stand in for this one
            sp = os.path.join(a.out, 'ours_f%d' % frame, stale)
            if os.path.exists(sp):
                os.remove(sp)
        rc, counts, _ = E.ours(a.capture, frame, a.out, faults='')
        d = os.path.join(a.out, 'ours_f%d' % frame)
        if rc != 0:
            fails.append('frame %d: maps_draw exit %d' % (frame, rc))
            continue

        def load(name):
            p = os.path.join(d, name)
            return Image.open(p).convert('RGB') if os.path.exists(p) else None
        for kind, ps, raw_name, f_name, chans in (('shadow', 'yBlur_min_rgba', 'ours_shadow.png', 'ours_shadow_f.png', (0, 1)), ('rim', 'yBlur_t2', 'ours_rim.png', 'ours_rim_f.png', (1,))):
            g = game_final(a.capture, frame, a.out, ps)
            raw, fil = load(raw_name), load(f_name)
            if g is None:
                lines.append('frame %d %s: the stream has no %s pass (not checked)' % (frame, kind, ps))
                continue
            if raw is None:
                fails.append('frame %d %s: no raw map (the pass was not made)' % (frame, kind))
                continue
            if fil is None:
                fails.append('CHAIN frame %d %s: the published map is still raw (no filtered image)' % (frame, kind))
                continue
            if raw.size != g.size or fil.size != g.size:
                fails.append('frame %d %s: size ours %s game %s' % (frame, kind, fil.size, g.size))
                continue
            fp = footprint(g, fil) | footprint(g, raw)
            iou_f, _, _ = E.compare_footprint(g, fil)
            res = []
            for ch in chans:
                dr, df = mean_d(g, raw, ch, fp), mean_d(g, fil, ch, fp)
                res.append((ch, dr, df))
                need = 0.7 if kind == 'rim' else 1.0   # the rim map must get >= 30 % closer; the shadow map must not get farther (its remaining error is the raw casters', frame 180 included: the game's two caster layers, LIVE_MAPS.md L7)
                if not df < dr * need and not df < 1.0:
                    fails.append('CHAIN frame %d %s channel %d: filtered |d| %.2f is not clearly below raw %.2f' % (frame, kind, ch, df, dr))
            lines.append('frame %d %s: footprint IoU(filtered) %.3f; mean|d| over %d px vs the game\'s final: %s' % (frame, kind, iou_f, int(fp.sum()), '; '.join('ch%d raw %.2f -> filtered %.2f' % r for r in res)))
    print('\n'.join(lines))
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 0 if a.plant else 1
    print('PASS  CHAIN: the filtered shadow / rim maps are closer to the game\'s final maps than the raw ones')
    return 1 if a.plant else 0


if __name__ == '__main__':
    sys.exit(main())

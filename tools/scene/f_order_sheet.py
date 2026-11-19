r"""S103 lane F: the order sheet (the stream's side of the see-through order page; numbers and colours only, no game pictures).
  py -I tools\scene\f_order_sheet.py OUT.png [--cap NAME=DIR[:FRAME] ...]
Top: for each capture frame, the colour pass's model-part / crowd / character draws in order, one coloured cell per draw, coloured by group (SEE_THROUGH_ORDER.md section 2).
Bottom: for brock_losswin, each see-through object's place in the draw order, frame after frame, as the camera moves: lines that stay level are submit order (a far-to-near sort
would cross them)."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
import f_order_census as fo  # noqa: E402
import f_order_stability as fs  # noqa: E402,F401
import pcap  # noqa: E402

GROUPS = [
    ('audience base', '#8c6bb1'), ('wrestlers', '#2171b5'), ('stage LM + screens', '#41ab5d'), ('props', '#a1d99b'), ('crowd', '#fd8d3c'),
    ('stage see-through', '#e31a1c'), ('wrestler translucent', '#f768a1'), ('additive', '#fed976'), ('flash', '#bdbdbd'),
]


def group_of(r, after_crowd):
    fx = r['fx'].split('/')[0]
    if r['klass'] == 'crowd' or fx == 'yAudience' and r['klass'] != 'model_part':
        return 4
    if fx == 'yAudience':
        return 0
    if fx.startswith('yCh_'):
        return 6 if fx.endswith('trans') else 1
    if fx == 'yFlashDefault':
        return 8
    if r['blend'] and r['dst'] == 1 and fx != 'ySprite':
        return 7
    if fx in ('yBG_Default', 'yProp_UVScroll') and after_crowd:
        return 5
    if fx.endswith('LM') or fx == 'yMovieDefault':
        return 2
    if fx.startswith('yProp') or fx.startswith('yBG'):
        return 3
    return -1


def main():
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--root', default=None)
    a = ap.parse_args()
    root = a.root or os.path.join(pcap.MAIN, 'out', 'captures')
    rows_spec = [('brock_losswin f200', 'brock_losswin', 200), ('s100_s89k_a f3', 's100_s89k_a', 3), ('s100_wm30_300 f1', 's100_wm30_300', 1)]
    fig, axes = plt.subplots(len(rows_spec) + 1, 1, figsize=(14, 2.0 * len(rows_spec) + 4.2), gridspec_kw={'height_ratios': [1] * len(rows_spec) + [3]})
    for ax, (label, cap, f) in zip(axes, rows_spec):
        rows = fo.draw_rows(pcap.Capture(os.path.join(root, cap)).frame(f))
        seen_crowd = False
        cells = []
        for r in rows:
            if r['klass'] in ('shadow', 'post', 'sprite', 'text', 'movie', 'clear', 'other'):
                continue
            if r['klass'] == 'crowd' or r['fx'].startswith('yAudience') and r['atest'] == 0:
                seen_crowd = True
            g = group_of(r, seen_crowd)
            if g >= 0:
                cells.append(g)
        for i, g in enumerate(cells):
            ax.add_patch(plt.Rectangle((i, 0), 1, 1, color=GROUPS[g][1], lw=0))
        ax.set_xlim(0, max(1, len(cells)))
        ax.set_ylim(0, 1)
        ax.set_yticks([])
        ax.set_title('%s: %d draws in the order the game draws them' % (label, len(cells)), fontsize=9, loc='left')
    axes[0].legend(handles=[Patch(color=c, label='%d %s' % (i + 1, n)) for i, (n, c) in enumerate(GROUPS)], ncol=5, fontsize=7, loc='upper center', bbox_to_anchor=(0.5, 1.9))
    ax = axes[-1]
    cap = pcap.Capture(os.path.join(root, 'brock_losswin'))
    seqs = [(f, fs.see_through_sequence(fo.draw_rows(cap.frame(f)))) for f in range(0, len(cap.frames), 2)]
    objs = []
    for _, s in seqs:
        for o in s:
            if o not in objs:
                objs.append(o)
    objs.sort()
    cm = plt.get_cmap('tab10')
    for k, o in enumerate(objs):
        xs = [f for f, s in seqs if o in s]
        ys = [s.index(o) for f, s in seqs if o in s]
        ax.plot(xs, ys, '.', ms=3, color=cm(k % 10), label='%08X' % o)
    ax.set_xlabel('capture frame (camera moves)')
    ax.set_ylabel('place in the see-through order')
    ax.set_title('brock_losswin: each see-through object keeps its place as the camera moves (submit order, not far to near)', fontsize=9, loc='left')
    ax.legend(fontsize=6, ncol=6, loc='upper right')
    fig.tight_layout()
    fig.savefig(a.out, dpi=110)
    print('wrote', a.out)


if __name__ == '__main__':
    main()

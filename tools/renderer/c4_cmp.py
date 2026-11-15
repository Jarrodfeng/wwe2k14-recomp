r"""S102 lane C4: compares two rplay runs: colour-pass fragment / vertex invocations per group (--draw-census 2 lines) and the picture differences of the --shots frames.
  py -I tools\renderer\c4_cmp.py A.log B.log [A_shots B_shots]
Counts only (hard rule 1)."""
import re, sys, collections
DRAW_RE = re.compile(r'\[rplay\]\[draws\] ([\d.]+) draws (\d+) tris (\d+) px (\d+) vs (\d+) fs a frame: (.*)$')
GROUPS = collections.OrderedDict([('crowd_members', ['yAudDefault', 'yAudience_lit', 'yAudience_lm_uv']), ('crowd_cards', ['yAudienceCard']), ('arena', ['yBG_']), ('ring_props', ['yProp_']), ('wrestlers', ['yCh_']),
                                  ('movie_misc', ['yMovieDefault', 'yPostBase', 'yRect_texcoord', 'yDefault_base']), ('half_size', ['half|'])])
def group_of(n):
    if 'half|' in n: return 'half_size'
    for g, subs in GROUPS.items():
        if any(s in n for s in subs): return g
    return 'other'
def census(path):
    out = collections.defaultdict(lambda: [0.0, 0, 0, 0]); tot = [0.0, 0, 0, 0]
    for l in open(path, errors='replace'):
        m = DRAW_RE.search(l)
        if not m: continue
        d, tris, px, vs, fs = float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5).split()[0]) if False else None
    return out
def census2(path):
    rows = collections.defaultdict(lambda: [0.0, 0, 0])
    for l in open(path, errors='replace'):
        m = re.search(r'\[rplay\]\[draws\] ([\d.]+) draws (\d+) tris (\d+) px (\d+) vs (\d+) fs a frame: (.*)$', l)
        if m:
            g = group_of(m.group(6)); r = rows[g]; r[0] += float(m.group(1)); r[1] += int(m.group(4)); r[2] += int(m.group(5))
    return rows
if __name__ == '__main__':
    a, b = census2(sys.argv[1]), census2(sys.argv[2])
    print('%-14s %10s %12s %12s %9s' % ('group', 'draws/frame', 'fs off', 'fs on', 'change'))
    ta = tb = 0
    for g in list(GROUPS) + ['other']:
        if g in a or g in b:
            x, y = a.get(g, [0, 0, 0]), b.get(g, [0, 0, 0]); ta += x[2]; tb += y[2]
            print('%-14s %5.1f>%5.1f %12d %12d %+8.1f%%' % (g, x[0], y[0], x[2], y[2], 100.0 * (y[2] - x[2]) / max(1, x[2])))
    print('%-14s %11s %12d %12d %+8.1f%%' % ('TOTAL', '', ta, tb, 100.0 * (tb - ta) / max(1, ta)))
    if len(sys.argv) > 4:
        import numpy as np, os
        from PIL import Image
        for f in (60, 120, 180, 236):
            pa, pb = os.path.join(sys.argv[3], 'frame_%04d.png' % f), os.path.join(sys.argv[4], 'frame_%04d.png' % f)
            if not (os.path.exists(pa) and os.path.exists(pb)): print('missing', f); continue
            d = np.abs(np.asarray(Image.open(pa).convert('RGB')).astype(int) - np.asarray(Image.open(pb).convert('RGB')).astype(int)).max(axis=2)
            print('frame %d: differing px %d, >8: %d, >32: %d, max %d (of %d)' % (f, (d > 0).sum(), (d > 8).sum(), (d > 32).sum(), d.max(), d.size))

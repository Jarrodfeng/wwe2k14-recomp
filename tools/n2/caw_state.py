"""Lane 117 (R4): the Create a Superstar editor's page and highlighted row read from one frame's drawn text. 0 game runs.

read_state(frame_path, geom_bytes) -> dict(title, sub, rows=[(y, text, rgba)], highlighted, footer, rank list)
CLI: py -I tools/n2/caw_state.py CAPTURE_DIR [first last]  -> prints a line whenever the state changes.
Rules measured on the CAW walk zips (docs/research/s101/N2_EDITOR_DRIVER.md):
  - a drawn string = the runs of section 69 (one text-class draw each); position = first quad's x,y in the 1280x720 guest UI space;
  - colour = the draw's VS constant bank vec4 register 1 (rgba), not the vertices;
  - a string drawn twice in a row (drop shadow) counts once.
"""
import sys, os, struct, glob
sys.path.insert(0, os.path.dirname(__file__))
from caw_text_read import frame, get_name, expand_banks

WHITE = (1.0, 1.0, 1.0)


def read_runs(cap, path, geom):
    b, gf, secs, S, runs = frame(path)
    banks = expand_banks(b, secs)
    des, dc, do = secs[4]
    lay = [struct.unpack_from('<IIII', b, secs[11][2] + i * secs[11][0]) for i in range(secs[11][1])]
    out = []   # (draw, text, x, y, (r,g,b,a))
    last = None
    for d, s, u, f in runs:
        t = get_name(S, s)
        o = do + d * des
        tt = struct.unpack_from('<QQQIIIIIIIIQQIIII', b, o)
        vb, lo, vo, vc = tt[7], tt[10], tt[11], tt[15]
        stride = lay[lo][2]
        x, y = struct.unpack_from('<2f', geom, vo)[:2]
        col = struct.unpack_from('<4f', banks[vb], 16)
        out.append((d, t, round(x, 1), round(y, 1), tuple(round(c, 3) for c in col)))
    return gf, out


def dedupe(runs):
    """Merge consecutive same-string draws (a glyph quad each) and the shadow repeat: keep the first draw of each (text, x, y) group start."""
    groups = []
    for d, t, x, y, c in runs:
        if groups and groups[-1][1] == t and abs(groups[-1][3] - y) < 0.01 and groups[-1][4] == c and d == groups[-1][5] + 1:
            groups[-1][5] = d
            continue
        groups.append([d, t, x, y, c, d])
    return groups


def read_state(cap, path, geom):
    gf, runs = read_runs(cap, path, geom)
    g = dedupe(runs)
    # firsts only, by (text, x, y)
    seen = []
    for d, t, x, y, c, _ in g:
        if (t, x, y) not in [(a[1], a[2], a[3]) for a in seen]:
            seen.append((d, t, x, y, c))
    return gf, seen


if __name__ == '__main__':
    cap = sys.argv[1]
    geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
    fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else len(fs) - 1
    prev = None
    for n in range(lo, hi + 1):
        gf, seen = read_state(cap, fs[n], geom)
        # a tint: text whose rgb is not white and that is not in the chrome (footer y>=580 / icons)
        tinted = [(t, x, y, c) for d, t, x, y, c in seen if c[:3] != WHITE and len(t) > 1]
        key = (tuple(t for d, t, x, y, c in seen), tuple(tinted))
        if key != prev:
            print('frame', n, 'guest', gf, 'tinted:', tinted)
            print('    strings:', [(t, x, y) for d, t, x, y, c in seen if len(t) > 1][:24])
            prev = key


def is_hot(c):
    """The selected-row tint: a red (r clearly above g and b, g and b small). Pulses: r 0.39-0.68 measured."""
    return c[0] >= 0.15 and c[1] <= 0.25 * c[0] and c[2] <= 0.25 * c[0]

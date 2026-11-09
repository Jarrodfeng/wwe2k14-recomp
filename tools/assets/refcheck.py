r"""Session 98 step 15: the Arena Viewer as the reference for the main (TRUE NATIVE) renderer.

Jarrod: "we're looking at the raw assets here. So it's how the game is meant to look." For one game frame (screenshot) plus
the game's camera for that frame (a gpu_debug_draw_probe CSV: c56..c59 = view-projection rows, c72 = eye) this tool drives
the viewer (out\tools\wwe2k14_assets.exe, one launch per item, --script --hidden) to the same arena, applies the game camera
exactly, shoots at the frame's size, and compares per grid cell (16 x 9) and per named region. Flags are for EYES, not
verdicts. 0 game runs: it only reads files (screenshots, probe CSVs, the player's own game files through the viewer).

  py -I tools\assets\refcheck.py --name a5_cel --shot GAME.png --probe PROBE.csv --swap 22453 --arena 5
          [--look match|entrance [--wrestler ID]] [--time T] [--size 1280x720] [--ignore x0,y0,x1,y1 ...] [--place JSON ...]
          [--no-regions] [--out DIR]
  py -I tools\assets\refcheck.py --items ITEMS.json      (a list of the same fields: name, shot, probe, swap, arena, look,
                                                          wrestler, time, ignore (fractions of the frame), place, size)
An entrance frame needs its wrestler (`wrestler`): without it the viewer lights the arena with its own sets, not the game's
entrance rig (Session 98: x2-4 too bright). The report's "stage media" line says which movie frames the stage got; STAGE MEDIA
MISSING = the stage surfaces were drawn untextured (white): do not read the stage / backdrop / above-horizon ratios then.
  py -I tools\assets\refcheck.py --s97                   the Session 97 arena 5 frames (entrance 6728, celebration 22453)

--swap N | first | last   which guest frame of the probe CSV carries the camera (default first = right after the screenshot).
--ignore  x0,y0,x1,y1     fractions (0..1) of the frame left out of every metric (a wrestler, the HUD); repeatable.
--place   JSON            one viewer `place` command (wrestler id, attire, pos, yaw, motion ...), repeatable: wrestlers from the
                          probe's skin palettes are not decoded yet, so by default the frame is compared WITHOUT wrestlers
                          (mask them with --ignore).
Output (out\refcheck\<name>\): report.html, report.txt, report.json, game.png, viewer.png, sbs.png, heat.png, flags.png,
regions.png, viewer.jsonl.
"""
import argparse
import csv
import html
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
# a worktree has no game_data / out\tools: fall back to the main checkout (same rule as spots.py)
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'
EXE = os.environ.get('WWE_VIEWER_EXE') or next((p for p in (os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'),
                                                              os.path.join(MAIN, 'out', 'tools', 'wwe2k14_assets.exe')) if os.path.exists(p)), '')
GAME = os.environ.get('WWE_GAME') or os.path.join(MAIN, 'game_data', 'extracted')
OUT = os.path.join(REPO, 'out', 'refcheck')
S97 = os.path.join(MAIN, 'out', 'runtime', 's97')
# the launcher's tron cache (the stage movie frames, src\assets\stage_media.h DefaultDir); WWE_STAGE_MEDIA overrides
MEDIA = os.environ.get('WWE_STAGE_MEDIA') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons')

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import refcheck_lib as L  # noqa: E402

csv.field_size_limit(1 << 30)


# ------------------------------------------------------------------------------------------------------------- camera
def camera_from_probe(path, swap):
    """-> (swap, vp[16], eye[3]): the main camera of one guest frame = the commonest (c72 eye, c56..c59) among its draws
    (as tools\\assets\\probe_camera.py); c56..c59 are the ROWS of the view-projection in the files' own (-Y up) space."""
    import collections
    cnt = collections.Counter()
    first = {}
    swaps = []
    if isinstance(swap, str) and swap.startswith('index:'):
        # the K-th (0-based) distinct guest frame in the file: a census capture probes ONE frame per sample, in order
        k = int(swap[6:])
        with open(path, newline='') as f:
            order = []
            for row in csv.DictReader(f):
                s = int(row['swap'])
                if not order or order[-1] != s:
                    order.append(s)
                    if len(order) > k + 1:
                        break
        if len(order) <= k:
            raise SystemExit('probe CSV %s has only %d guest frames, wanted index %d' % (path, len(order), k))
        swap = order[k]
    want = None if swap in ('first', 'last') else int(swap)
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            s = int(row['swap'])
            if not swaps or swaps[-1] != s:
                swaps.append(s)
            if want is not None and s != want:
                continue
            if want is None and swap == 'first' and s != swaps[0]:
                break
            c = row['consts'].split()
            if len(c) < 292:
                raise SystemExit('probe CSV has fewer than 292 constants per draw: capture with --gpu_debug_draw_probe_consts=0-255')
            vp = tuple(round(float(x), 3) for x in c[224:240])
            eye = tuple(round(float(x), 1) for x in c[288:291])
            k = (s, eye, vp)
            cnt[k] += 1
            first.setdefault(k, [float(x) for x in c[224:240]] + [float(x) for x in c[288:291]])
    if swap == 'last':
        keep = {k: n for k, n in cnt.items() if k[0] == swaps[-1]}
        cnt = collections.Counter(keep)
    if not cnt:
        raise SystemExit('no draws for swap %s in %s (swaps: %s ...)' % (swap, path, swaps[:6]))
    k = cnt.most_common(1)[0][0]
    v = first[k]
    return k[0], v[:16], v[16:19]


# ----------------------------------------------------------------------------------------------------------- geometry
def arena_geo(arena):
    """Region geometry in file space from the player's own arena pac (tools\\assets\\spots.py readers), cached under out\\refcheck."""
    cache = os.path.join(OUT, '_geo_%02d.json' % arena)
    try:
        with open(cache) as f:
            return json.load(f)
    except (OSError, ValueError):
        pass
    import spots
    d = spots.pac.load(os.path.join(spots.GAME, 'pac', 'bg', 'bg%02d.pac' % arena))
    entries = list(spots.pac.entries(d, 8))
    rects = spots.arena_rects(d)
    geo = {'canvas': 318.8, 'canvas_y': -111.0, 'apron_half': 325.0, 'apron_y0': -111.0, 'floor_half': 1600.0, 'backdrop_h': 650.0}
    fl = spots.floor_in_rect(entries, dict(x0=-700, z0=-700, x1=700, z1=700))
    geo['floor_y'] = -(fl if fl is not None else 0.0)
    geo['apron_y1'] = geo['floor_y']
    if rects:
        stage = max(rects, key=lambda r: max(r['z0'], r['z1']))
        ramps = [r for r in rects if r is not stage and abs(r['x0'] - r['x1']) <= 700]
        ramp = min(ramps, key=lambda r: min(r['z0'], r['z1'])) if ramps else None
        for nm, r in (('stage', stage), ('ramp', ramp)):
            if r:
                y = spots.floor_in_rect(entries, r)
                geo[nm] = [min(r['x0'], r['x1']), min(r['z0'], r['z1']), max(r['x0'], r['x1']), max(r['z0'], r['z1']),
                           -(y if y is not None else 0.0)]
    os.makedirs(OUT, exist_ok=True)
    with open(cache, 'w') as f:
        json.dump(geo, f)
    return geo


# ----------------------------------------------------------------------------------------------------------- viewer
def run_viewer(item, vp, eye, size, outdir):
    shot = os.path.join(outdir, 'viewer.png')
    script = os.path.join(outdir, 'viewer.jsonl')
    look = item.get('look', 'match')
    lines = ['{"cmd":"load","kind":"arena","id":%d}' % item['arena']]
    if look == 'entrance':
        lines.append(json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': item.get('time', 140)}))
    elif 'time' in item:
        lines.append(json.dumps({'cmd': 'lights', 'mode': 'match', 'time': item['time']}))
    # Stage media + the entrance look (Session 98 brightness fix): the stage screens / pillars / wall take the movie frames of the
    # launcher's tron cache. Named explicitly so the viewer reports what it applied (a default folder stays silent): a run while
    # the cache was being rebuilt drew those surfaces untextured (white, lit by the stage lights: x4 on stage / backdrop).
    # The entrance look needs the frame's wrestler: his entrance light rig (act 25011, entrance_light.h), his media and his
    # bloom / glare strengths. Without one the viewer falls back to the arena's own lights (not the game's entrance lighting).
    st = {'cmd': 'stage'}
    if os.path.isdir(MEDIA):
        st['media'] = MEDIA.replace('\\', '/')
    if look == 'entrance' and item.get('wrestler') is not None:
        st['wrestler'] = int(item['wrestler'])
    if len(st) > 1:
        lines.append(json.dumps(st))
    if look == 'entrance' and item.get('wrestler') is None:
        lines.append('{"cmd":"look","entrance":true}')  # (after `stage`: a stage command without a wrestler turns the entrance look off)
    lines += [json.dumps(p) if isinstance(p, dict) else p for p in item.get('place', [])]
    lines += [json.dumps(c) if isinstance(c, dict) else c for c in item.get('viewer_cmds', [])]
    lines.append(json.dumps({'cmd': 'camera', 'viewproj': vp, 'eye': eye}))
    lines.append(json.dumps({'cmd': 'shot', 'path': shot.replace('\\', '/'), 'w': size[0], 'h': size[1]}))
    with open(script, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    if not EXE:
        raise SystemExit('wwe2k14_assets.exe not found (out\\tools; build with tools\\assets\\build_viewer.ps1) or set WWE_VIEWER_EXE')
    if os.path.exists(shot):
        os.remove(shot)
    p = subprocess.run([EXE, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad or not os.path.exists(shot):
        raise SystemExit('viewer failed: %s' % (bad[:3] or p.stderr[-300:]))
    warns = []
    for l in p.stdout.splitlines():  # every reply's warnings (the stage media note comes with the last arena (re)load)
        if '"warnings"' in l:
            try:
                warns += [w for w in json.loads(l).get('warnings', []) if w not in warns]
            except ValueError:
                pass
    return shot, warns


def media_status(item, warns):
    """One line on the stage movie frames the viewer applied (they decide the stage / backdrop / pillar brightness)."""
    if not os.path.isdir(MEDIA):
        return 'STAGE MEDIA MISSING: no tron cache at %s (stage screens / pillars / wall drawn untextured = white)' % MEDIA
    notes = [w for w in warns if w.startswith('stage media from')]
    if not notes or '=ok' not in notes[-1]:
        return 'STAGE MEDIA MISSING: the viewer applied no movie frame from %s (cache empty or being rebuilt?): stage surfaces untextured = white, compare is not valid there' % MEDIA
    s = 'stage media: ' + notes[-1][len('stage media from '):]
    if item.get('look') == 'entrance':
        s += ('; entrance look of wrestler %s (his light rig, media, bloom / glare)' % item['wrestler'] if item.get('wrestler') is not None
              else '; ENTRANCE WITHOUT WRESTLER: arena lights only, not the game\'s entrance rig (pass "wrestler")')
    return s


# ----------------------------------------------------------------------------------------------------------- one item
def read_img(path, size):
    return np.asarray(Image.open(path).convert('RGB').resize(size, Image.LANCZOS), dtype=np.float32)


def run_item(item):
    t0 = time.time()
    name = item['name']
    outdir = os.path.join(item.get('out') or OUT, name)
    os.makedirs(outdir, exist_ok=True)
    size = tuple(int(x) for x in item.get('size', '1280x720').split('x'))
    W, H = size
    swap, vp, eye = camera_from_probe(item['probe'], item.get('swap', 'first'))
    game = read_img(item['shot'], size)
    vshot, warns = run_viewer(item, vp, eye, size, outdir)
    view = read_img(vshot, size)
    geo = arena_geo(item['arena']) if not item.get('no_regions') else None
    regions = L.build_regions(vp, eye, geo, W, H) if geo else {'whole frame': np.ones((H, W), bool)}
    ign = np.zeros((H, W), bool)
    for x0, y0, x1, y1 in item.get('ignore', []):
        ign[int(y0 * H):int(y1 * H), int(x0 * W):int(x1 * W)] = True
    corners = []
    if geo:
        c, cy = geo['canvas'], geo['canvas_y']
        for nm, (x, z) in (('canvas -x-z', (-c, -c)), ('canvas +x-z', (c, -c)), ('canvas +x+z', (c, c)), ('canvas -x+z', (-c, c))):
            cl = L.project(vp, [(x, cy, z)])[0]
            if cl[3] > 1:
                px, py = (cl[0] / cl[3] * 0.5 + 0.5) * W, (1 - (cl[1] / cl[3] * 0.5 + 0.5)) * H
                if 0 <= px < W and 0 <= py < H:
                    corners.append((nm, (px, py)))
        # the posts' bases sit on the canvas corners (+-321): the same 4 points are what probe_ab lined up
    align = L.align_report(game, view, corners)
    # a small whole-frame offset (the screenshot is often a frame or two off the probed frame) is taken out before comparing,
    # so the cells compare like with like; the raw shift stays in the report (--no-align keeps the viewer frame as shot)
    ax, ay = align.get('median_dx'), align.get('median_dy')
    align['applied'] = None
    if item.get('align', True) and ax is not None and align['grid_patches'] >= 6 and (ax or ay) and abs(ax) <= 12 and abs(ay) <= 12:
        view = L.shift_image(view, int(ax), int(ay))
        align['applied'] = [int(ax), int(ay)]
    res = L.compare(game, view, regions, ign, tol=item.get('tol'))
    # pictures
    gi, vi = L.arr_img(game), L.arr_img(view)
    gi.save(os.path.join(outdir, 'game.png'))
    vi.save(os.path.join(outdir, 'viewer.png'))
    sbs = Image.new('RGB', (W * 2, H))
    sbs.paste(gi, (0, 0))
    sbs.paste(vi, (W, 0))
    sbs.save(os.path.join(outdir, 'sbs.png'))
    Image.fromarray(L.heat(res['diff'])).save(os.path.join(outdir, 'heat.png'))
    L.outline_image(game, res, regions, corners).save(os.path.join(outdir, 'flags.png'))
    L.region_border_image(view, regions).save(os.path.join(outdir, 'regions.png'))
    rep = {'name': name, 'arena': item['arena'], 'look': item.get('look', 'match'), 'swap': swap, 'size': [W, H],
           'eye': eye, 'viewer': EXE, 'seconds': round(time.time() - t0, 1), 'alignment': align,
           'regions': res['regions'], 'flags': res['flags'],
           'cells': [[{k: v for k, v in c.items()} for c in row] for row in res['cells']],
           'viewer_warnings_unique': sorted(set(warns))[:12], 'stage_media': media_status(item, warns),
           'entrance_wrestler': item.get('wrestler'),
           'wrestlers': 'placed' if item.get('place') else 'not placed (masked with ignore)',
           'ignore': item.get('ignore', [])}
    with open(os.path.join(outdir, 'report.json'), 'w') as f:
        json.dump(rep, f, indent=1)
    txt = text_report(rep)
    with open(os.path.join(outdir, 'report.txt'), 'w', encoding='utf-8') as f:
        f.write(txt)
    with open(os.path.join(outdir, 'report.html'), 'w', encoding='utf-8') as f:
        f.write(html_report(rep))
    return rep, txt


# ----------------------------------------------------------------------------------------------------------- reports
def align_line(a):
    cs = [c for c in a['corners'] if c['dx'] is not None]
    worst = max([max(abs(c['dx']), abs(c['dy'])) for c in cs], default=None)
    s = 'alignment: grid-patch median shift dx %s dy %s px (%d patches, 1280-wide frame)' % (
        a.get('median_dx', '?'), a.get('median_dy', '?'), a['grid_patches'])
    if cs:
        s += '; ring corners in frame %d, worst corner shift %d px (%s)' % (
            len(cs), worst, ', '.join('%s (%+d,%+d)' % (c['corner'].replace('canvas ', ''), c['dx'], c['dy']) for c in cs))
    else:
        s += '; no ring corner measurable in this frame'
    if a.get('applied'):
        s += '; the viewer frame was moved by (%+d,%+d) px before comparing' % tuple(a['applied'])
    if worst is not None and worst > 2:
        s += '  ALIGN CHECK: > 2 px (the viewer camera or the probe frame may not match the screenshot)'
    return s


def text_report(r):
    out = ['refcheck %s  arena %d  look %s  swap %d  %dx%d  (%.1f s)' % (r['name'], r['arena'], r['look'], r['swap'], r['size'][0], r['size'][1], r['seconds']),
           align_line(r['alignment']), 'wrestlers: ' + r['wrestlers'], r.get('stage_media', ''), '', 'REGIONS (mean colour diff 0..1, lum game -> viewer, 1-SSIM):']
    for n, s in r['regions'].items():
        if not s.get('pixels'):
            out.append('  %-44s (not in frame / ignored)' % n)
        else:
            out.append('  %-44s %4.1f%% of frame  dColour %.2f  lum %5.1f -> %5.1f (x%.2f)  1-SSIM %.2f' % (
                n, s['share'] * 100, s['colour_diff'], s['game_lum'], s['view_lum'], s['ratio'], s['structure']))
    big = [(n, s) for n, s in r['regions'].items() if s.get('pixels') and s['share'] >= 0.005 and (s['ratio'] > 1.6 or s['ratio'] < 0.6)]
    if big:
        out += ['', 'REGION FLAGS (whole region, luminance ratio viewer / game beyond 0.6 .. 1.6):']
        for n, s in big:
            out.append('  %-12s %-44s x%.2f  (game %.1f -> viewer %.1f), %.1f%% of the frame' % (
                'BRIGHT' if s['ratio'] > 1 else 'DARK', n, s['ratio'], s['game_lum'], s['view_lum'], s['share'] * 100))
    out += ['', 'FLAGS (one line per flag + region; cells rRcC of a 16x9 grid; for eyes, not verdicts):']
    if not r['flags']:
        out.append('  none')
    for f in r['flags']:
        out.append('  %-26s %-44s %2d cells (%4.1f%%)  lum game %5.1f / viewer %5.1f   %s' % (
            f['flag'], f['region'], f['cells'], f['frame_share'] * 100, f['game_lum'], f['view_lum'], ' '.join(f['at'][:14]) + (' ...' if len(f['at']) > 14 else '')))
    return '\n'.join(out) + '\n'


def html_report(r):
    e = html.escape
    rows = ''.join('<tr><td>%s</td><td>%.1f%%</td><td>%.2f</td><td>%.1f &rarr; %.1f (&times;%.2f)</td><td>%.2f</td></tr>' % (
        e(n), s['share'] * 100, s['colour_diff'], s['game_lum'], s['view_lum'], s['ratio'], s['structure'])
        for n, s in r['regions'].items() if s.get('pixels'))
    fl = ''.join('<li><b style="color:rgb%s">%s</b> in <i>%s</i>: %d cells (%.1f%% of the frame), lum game %.1f / viewer %.1f<br><small>%s</small></li>' % (
        L.flag_color(f['flag']), e(f['flag']), e(f['region']), f['cells'], f['frame_share'] * 100, f['game_lum'], f['view_lum'], e(' '.join(f['at'])))
        for f in r['flags']) or '<li>none</li>'
    return '''<!doctype html><meta charset="utf-8"><title>refcheck %(n)s</title>
<style>body{font:14px system-ui;background:#16181d;color:#ddd;margin:16px}img{max-width:100%%}table{border-collapse:collapse}
td,th{border:1px solid #444;padding:3px 8px;text-align:left}.g{display:grid;grid-template-columns:1fr 1fr;gap:8px}li{margin:4px 0}</style>
<h2>refcheck %(n)s &mdash; arena %(a)d, %(l)s look, guest frame %(s)d, %(w)dx%(h)d</h2>
<p>%(al)s<br>wrestlers: %(wr)s. Flags are for eyes, not verdicts.<br>%(sm)s</p>
<div class="g"><div>game<br><img src="game.png"></div><div>viewer (same camera)<br><img src="viewer.png"></div>
<div>difference (heat, 0..33%% colour diff)<br><img src="heat.png"></div><div>flagged cells (game frame; green = projected ring corners)<br><img src="flags.png"></div>
<div>regions on the viewer frame<br><img src="regions.png"></div></div>
<h3>Flags</h3><ul>%(fl)s</ul><h3>Regions</h3><table><tr><th>region</th><th>frame</th><th>colour diff</th><th>luminance game &rarr; viewer</th><th>1-SSIM</th></tr>%(rows)s</table>
''' % dict(n=e(r['name']), a=r['arena'], l=r['look'], s=r['swap'], w=r['size'][0], h=r['size'][1], al=e(align_line(r['alignment'])),
           wr=e(r['wrestlers']), sm=e(r.get('stage_media', '')), fl=fl, rows=rows)


# --------------------------------------------------------------------------------------------------------------- main
def s97_items():
    p = os.path.join(S97, 'drawprobe.csv')
    ign_cel = [[0.40, 0.08, 0.68, 1.0]]  # Randy in the celebration frame (a screenshot read, not an auto mask)
    return [
        {'name': 'a5_celebration_22453', 'shot': os.path.join(S97, 'game_a5_celebration.png'), 'probe': p, 'swap': 22453, 'arena': 5,
         'look': 'match', 'ignore': ign_cel},
        # swap 6728 = Randy Orton's entrance (id 161: out\runtime\s98\passes.md 1, s97\entrance_light.md: his rig at cue 0 t 68 = the probe's lights)
        {'name': 'a5_entrance_6728', 'shot': os.path.join(S97, 'game_a5_entrance_probe.png'), 'probe': p, 'swap': 6728, 'arena': 5,
         'look': 'entrance', 'wrestler': 161},
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name')
    ap.add_argument('--shot')
    ap.add_argument('--probe')
    ap.add_argument('--swap', default='first')
    ap.add_argument('--arena', type=int)
    ap.add_argument('--look', default='match')
    ap.add_argument('--time', type=float)
    ap.add_argument('--wrestler', type=int, help='entrance look: the wrestler whose entrance it is (his light rig, media, bloom / glare)')
    ap.add_argument('--size', default='1280x720')
    ap.add_argument('--ignore', action='append', default=[])
    ap.add_argument('--place', action='append', default=[])
    ap.add_argument('--no-regions', action='store_true')
    ap.add_argument('--no-align', action='store_true', help='compare the viewer frame as shot (default: take out a measured whole-frame offset <= 12 px)')
    ap.add_argument('--out')
    ap.add_argument('--items')
    ap.add_argument('--s97', action='store_true')
    a = ap.parse_args()
    items = []
    if a.s97:
        items = s97_items()
    elif a.items:
        with open(a.items, encoding='utf-8') as f:
            items = json.load(f)
    else:
        for k in ('name', 'shot', 'probe', 'arena'):
            if getattr(a, k) is None:
                ap.error('--%s is required (or --items / --s97)' % k)
        it = {'name': a.name, 'shot': a.shot, 'probe': a.probe, 'swap': a.swap, 'arena': a.arena, 'look': a.look, 'size': a.size,
              'ignore': [[float(x) for x in s.split(',')] for s in a.ignore], 'place': [json.loads(p) for p in a.place],
              'no_regions': a.no_regions, 'out': a.out, 'align': not a.no_align}
        if a.time is not None:
            it['time'] = a.time
        if a.wrestler is not None:
            it['wrestler'] = a.wrestler
        items = [it]
    rc = 0
    for it in items:
        try:
            rep, txt = run_item(it)
            print(txt)
            print('report: %s' % os.path.join(it.get('out') or OUT, it['name'], 'report.html'))
        except SystemExit as e:
            print('%s: FAILED %s' % (it.get('name'), e))
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main())

r"""S102 lane G6: what the GPU stamps call "other" (offline, 0 game runs). For a capture and the counts file rplay wrote for it (`rplay --counts OUT.json`), every first-target
image (colour, else depth) whose GPU class is `other` under the CURRENT role -> class mapping (the same table as binder.h GpuClassOfRole), with: size, format, the image's role (images.bin),
the creator row of the role entry (section 65) when it has one, how many passes a frame and draws a frame draw into it, the pixels written (counts.h area_px: the M1 upper bound), the
draw classes / techniques inside (section 64) and the copies that land in it. Ranked by area_px (pixels written x overdraw: the GPU-cost proxy; draws as the tie-break).
  py -I tools\renderer\gpu_other_census.py CAPTURE_DIR --counts OUT.json [--frames N] [--top K] [--json OUT]
  --table FILE.json  replace the built-in role -> class table (selftest / planted-fault use)
Counts and names only (hard rule 1)."""
import argparse, collections, json, os, struct, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'particles'))
sys.path.insert(0, os.path.join(HERE, '..', 'solver'))
import pcap  # noqa: E402

ROLE_NAMES = ['unnamed', 'scene_color', 'scene_depth', 'shadow_map', 'post_step', 'feedback_map', 'atlas_target', 'menu_picture', 'tron_picture', 'ui', 'present', 'other', 'rim_map', 'frame_buffer', 'copy_dest', 'light_strip']
# names.h SurfaceRole as of S102 G6 (the tool reads the role NAME list from names.h when it can, so a new role does not need an edit here)
NAMES_H = os.path.normpath(os.path.join(HERE, '..', '..', 'src', 'renderer_adapter', 'names.h'))
IMG_DT = np.dtype([('id', '<u4'), ('w', '<u2'), ('h', '<u2'), ('d', '<u2'), ('mips', 'u1'), ('layers', 'u1'), ('format', 'u1'), ('kind', 'u1'), ('usage', 'u1'), ('source', 'u1'),
                   ('flags', 'u1'), ('gf', 'u1'), ('role', 'u1'), ('role_flags', 'u1'), ('pad', 'u1', (4,))])
ROLE_DT = np.dtype([('image', '<u4'), ('role', 'u1'), ('index', 'u1'), ('flags', 'u1'), ('pad', 'u1'), ('name', '<u4'), ('creator_fn', '<u4')])
COPY_DT = np.dtype([('s_image', '<u4'), ('s_rest', 'u1', (4,)), ('d_image', '<u4'), ('d_rest', 'u1', (4,)), ('sr', '<i4', (4,)), ('dr', '<i4', (4,)), ('filt', 'u1'), ('p', 'u1', (3,))])
CLASS_NAMES = ['unnamed', 'model_part', 'crowd', 'sprite', 'text', 'movie', 'particle', 'shadow', 'post', 'clear', 'other']


def role_names():
    """The role names in names.h's order (SurfaceRoleName's table), so the census follows new roles."""
    try:
        text = open(NAMES_H, encoding='utf-8').read()
        i = text.index('inline const char* SurfaceRoleName')
        j = text.index('};', i)
        import re
        return re.findall(r'"([a-z_0-9]+)"', text[i:j])
    except Exception:
        return ROLE_NAMES


def class_of_role(name, table):
    return table.get(name, 'other')


# the mapping as binder.h GpuClassOfRole has it (the planted twin and the selftest replace it)
# (S102 G6: + ui / copies / prelight / feedback; frame_buffer and copy_dest are the roles RoleForStamps derives, see role_for_stamps below)
BUILTIN_TABLE = {'scene_color': 'colour', 'scene_depth': 'colour', 'shadow_map': 'shadow', 'rim_map': 'rim', 'post_step': 'post', 'atlas_target': 'cards', 'present': 'output',
                 'menu_picture': 'ui', 'tron_picture': 'ui', 'ui': 'ui', 'frame_buffer': 'frame', 'copy_dest': 'copies', 'light_strip': 'prelight', 'feedback_map': 'feedback'}
# the draw classes (section 64) a frame_buffer image may receive: the 2D / post / effects layer, never the 3D scene (model_part, crowd) or the shadow passes
FRAME_BUFFER_DRAWS = ('sprite', 'text', 'movie', 'particle', 'post', 'clear')


def screen_shaped(w, h):
    return w >= 64 and -256 <= w * 9 - h * 16 <= 256  # reader.h ScreenShapedRule (gpu_class_lint.py compares the C++ copies)


def pick_scene(imgs, names):
    """stamp_roles.h PickSceneSize: the largest screen-shaped scene_color record (not the 736x1280 roster picture), the newest on a tie."""
    best = None
    for r in imgs.values():
        if int(r['role']) < len(names) and names[int(r['role'])] == 'scene_color' and screen_shaped(int(r['w']), int(r['h'])):
            key = (int(r['w']) * int(r['h']), int(r['id']))
            if best is None or key > best[0]:
                best = (key, (int(r['w']), int(r['h'])))
    return best[1] if best else None


def role_for_stamps(r, names, scene):
    """binder.h RoleForStamps: a declared role stands; unnamed / other: written by copies (ImageFlags 2) -> copy_dest; a surface target of the scene target's size -> frame_buffer."""
    role = names[int(r['role'])] if int(r['role']) < len(names) else '?'
    if role not in ('unnamed', 'other'):
        return role
    if int(r['flags']) & 2:
        return 'copy_dest'
    if scene and int(r['source']) == 0 and (int(r['usage']) & 2) and (int(r['w']), int(r['h'])) == scene:
        return 'frame_buffer'
    return role


def load_counts(path, frames):
    d = json.loads(open(path, encoding='utf-8').read())
    if d.get('format') != 'rdr-counts' or not d.get('frames'):
        raise SystemExit('FAIL: %s is not a counts file with frames' % path)
    fr = d['frames'][:frames] if frames else d['frames']
    per = collections.defaultdict(lambda: dict(passes=0, draws=0, area=0, w=0, h=0, fmt=0, copies=0, frames=set()))
    totals = dict(passes=0, draws=0, area=0)
    for i, f in enumerate(fr):
        for p in f['pass']:
            e = per[p['target_image']]
            e['passes'] += 1
            e['draws'] += p['draws'] + p['indexed_draws']
            e['area'] += p['area_px']
            e['copies'] += p['copies']
            e['w'], e['h'], e['fmt'] = p['width'], p['height'], p['target_format']
            e['frames'].add(i)
            totals['passes'] += 1
            totals['draws'] += p['draws'] + p['indexed_draws']
            totals['area'] += p['area_px']
    return per, totals, len(fr)


def purity(rows, frames=100):
    """Draw-class purity (S102 G6 review). (1) a frame_buffer image receives only sprite / text / movie / particle / post / clear draws, plus a few model_part draws (Bryan: 11 to 19 a frame
    from the scene pass sites 0x82C0A950 / 0x82C139FC drawn after the post chain: measured, not a mistake) and NO crowd draw; (2) the crowd (811 draws a frame in the scene on Bryan) is only
    ever drawn into a colour-class image. Either way a scene target taken for the frame buffer, or left unnamed (--plant scene_unnamed), is reported."""
    bad = []
    for r in rows:
        cl = r['classes_all']
        if r['role'] == 'frame_buffer':
            for k, n in cl.items():
                if k in FRAME_BUFFER_DRAWS or k in ('unnamed', 'no_origin'):
                    continue
                if k == 'model_part' and n / frames <= 30:
                    continue
                bad.append('image %d (%dx%d) is frame_buffer but received %d %s draws' % (r['image'], r['w'], r['h'], n, k))
        if cl.get('crowd', 0) and r['cls'] != 'colour':
            bad.append('image %d (%dx%d, role %s, class %s) received %d crowd draws: the scene target is not named colour' % (r['image'], r['w'], r['h'], r['role'], r['cls'], cl['crowd']))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--counts', required=True)
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--json', default='')
    ap.add_argument('--table', default='')
    ap.add_argument('--plant', default='', help='scene_unnamed: the scene_color image (the largest screen-shaped one) is rewritten to role 0 before naming (the purity check must fail)')
    a = ap.parse_args()
    table = dict(BUILTIN_TABLE)
    if a.table:
        table = json.loads(open(a.table).read())
    names = role_names()
    cap = pcap.Capture(a.capture)
    scene = None
    per, totals, nframes = load_counts(a.counts, a.frames)
    raw = cap.read(os.path.join(a.capture, 'images.bin'))
    count = struct.unpack_from('<IIII', raw, 0)[2]
    imgs = {int(r['id']): r for r in np.frombuffer(raw, IMG_DT, count, 16).copy()}
    if a.plant == 'scene_unnamed':
        for r in imgs.values():
            if names[int(r['role'])] in ('scene_color', 'scene_depth') and screen_shaped(int(r['w']), int(r['h'])):
                r['role'] = 0
    scene = pick_scene(imgs, names)
    # per image: the draws' classes and techniques, the creator rows, the copies into it (walk the first nframes capture frames)
    klass = collections.defaultdict(collections.Counter)
    tech = collections.defaultdict(collections.Counter)
    creator = collections.defaultdict(collections.Counter)
    rolename_str = {}
    copies_in = collections.defaultdict(lambda: [0, collections.Counter()])
    n = min(nframes, len(cap.frames))
    for fi in range(n):
        f = cap.frame(fi)
        secs = f.secs
        s = secs.get(67)
        strings = bytes(f.buf[s[2]:s[2] + s[1]]) if False else b''
        if 67 in secs:
            es, cnt, off = secs[67]
            strings = bytes(f.buf[off:off + cnt * es])

        def string(o):
            if not o:
                return ''
            j = strings.find(b'\0', o - 1)
            return strings[o - 1:j if j >= 0 else len(strings)].decode('utf-8', 'replace')
        if 65 in secs:
            es, cnt, off = secs[65]
            if es == ROLE_DT.itemsize:
                for r in np.frombuffer(f.buf, ROLE_DT, cnt, off):
                    creator[int(r['image'])]['%08X %s' % (int(r['creator_fn']), string(int(r['name'])))] += 1
        cur = None
        di = 0
        for kind, idx in f.ops:
            kind = int(kind)
            if kind == 0:
                t = f.targets[int(idx)]
                cur = int(t['color'][0]['image']) if int(t['count']) else int(t['depth']['image'])
            elif kind == 2 and cur is not None:
                k = CLASS_NAMES[int(f.origins[int(idx)]['klass'])] if f.has_origins and int(idx) < len(f.origins) else 'no_origin'
                klass[cur][k] += 1
                if f.has_origins and int(idx) < len(f.origins):
                    t = string(int(f.origins[int(idx)]['technique']))
                    if t:
                        tech[cur][t] += 1
            elif kind == 3 and 5 in secs:
                es, cnt, off = secs[5]
                if es == COPY_DT.itemsize and int(idx) < cnt:
                    c = np.frombuffer(f.buf, COPY_DT, 1, off + es * int(idx))[0]
                    d = int(c['d_image'])
                    copies_in[d][0] += 1
    # the counts file names the RENDERER's image ids (rplay makes a few images of its own before the capture's, so they sit a constant `delta` above the capture's ids): find delta
    # as the shift under which the most counted targets have the capture image's own size
    def agree(dl):
        return sum(1 for img, e in per.items() if (imgs.get(img - dl) is not None and (int(imgs[img - dl]['w']), int(imgs[img - dl]['h'])) == (e['w'], e['h'])))
    delta = max(range(0, 64), key=lambda dl: (agree(dl), -dl))
    print('image id shift renderer - capture: %d (%d of %d counted targets agree on size; the rest are renderer-made images)' % (delta, agree(delta), len(per)))
    rows = []
    for rimg, e in per.items():
        img = rimg - delta
        r = imgs.get(img)
        if r is not None and (int(r['w']), int(r['h'])) != (e['w'], e['h']):
            r = None  # an image the renderer made itself (a scaled copy, our own pass): no capture record
        role = role_for_stamps(r, names, scene) if r is not None else 'renderer_made'
        cls = class_of_role(role, table)
        rows.append(dict(image=img if r is not None else -rimg, cls=cls, role=role, w=e['w'], h=e['h'], fmt=e['fmt'], passes_a_frame=e['passes'] / nframes, draws_a_frame=e['draws'] / nframes,
                         area_a_frame=e['area'] / nframes, draws=e['draws'], area=e['area'], frames=len(e['frames']),
                         src=int(r['source']) if r is not None else -1, flags=int(r['flags']) if r is not None else -1,
                         creator=creator[img].most_common(1)[0][0] if creator[img] else '',
                         classes=dict(klass[img].most_common(4)), classes_all=dict(klass[img]), techniques=dict(tech[img].most_common(3)), copies_in=copies_in[img][0] / n))
    rows.sort(key=lambda r: (-r['area'], -r['draws']))
    other = [r for r in rows if r['cls'] == 'other']
    t_area = sum(r['area'] for r in rows) or 1
    o_area = sum(r['area'] for r in other)
    print('frames %d; passes a frame %.1f (other %.1f); draws a frame %.1f (other %.1f); area_px a frame %.0f (other %.0f = %.1f %%)' % (
        nframes, totals['passes'] / nframes, sum(r['passes_a_frame'] for r in other), totals['draws'] / nframes, sum(r['draws_a_frame'] for r in other),
        totals['area'] / nframes, o_area / nframes, 100.0 * o_area / t_area))
    print('%-6s %-5s %-12s %-10s %-4s %5s %7s %10s %s' % ('image', 'size', 'role', 'cls', 'fmt', 'pass', 'draws', 'Mpx/frame', 'creator | draw classes | techniques | copies in/frame'))
    for r in other[:a.top]:
        print('%-6d %4dx%-4d %-12s %-6s %-4d %5.2f %7.1f %10.3f %s | %s | %s | %.2f' % (r['image'], r['w'], r['h'], r['role'], r['cls'], r['fmt'], r['passes_a_frame'], r['draws_a_frame'],
              r['area_a_frame'] / 1e6, r['creator'] or '-', r['classes'], r['techniques'], r['copies_in']))
    bad = purity(rows, nframes)
    for b in bad:
        print('FAIL purity: ' + b)
    if not bad:
        print('purity ok: frame_buffer images received only %s (+ a few model_part) draws, the crowd only went into colour-class images' % '/'.join(FRAME_BUFFER_DRAWS))
    if a.json:
        json.dump(dict(frames=nframes, rows=rows, other_share_area=o_area / t_area), open(a.json, 'w'), indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

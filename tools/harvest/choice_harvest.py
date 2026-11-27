#!/usr/bin/env python3
r"""S103 lane J1: the CHOICE HARVESTER. The game's own per-draw choice (which variant, blend, alpha rule and slots each mesh draws with), read from a capture and written as a
NAMES-AND-NUMBERS-ONLY table (Fable review section 2.2; DECISIONS 2026-11-16: a names-only table may be committed, like shader_list.txt). 0 game runs: offline, from a kept capture.

  py -I tools\harvest\choice_harvest.py CAPTURE[:ARENA] --scene NAME --look LOOK [--frames 300,340 | --every 40] [--out tools\harvest\choice_table\NAME.json] [--merge]
  (its selftest is tools\harvest\harvest_selftest.py: py -I tools\harvest\harvest_selftest.py [--case CASE.zip])

CAPTURE = a case zip (capture\ is unpacked once under out\harvest\unpack, scratch) or an unpacked capture folder (frames\, images.bin, geometry.bin, textures.bin).
ARENA   = the bg id the stage draws join against (omit for a scene with no stage: the class table is still written).
Reading (per frame, straight from the frame file; no fview, so every draw of the stream is named, also those a replay setting would leave out): the shader hashes give each draw's
vertex / pixel variant NAME (fx.json hashes, as fx_names.py); its fixed state the blend key of target 0 and the alpha test and ref; its bound texture slots each slot's ROLE; its
vertex layout the vertex-buffer streams; tools\arena\join_census.py's join (positions + counts + texture bytes,
as tools\arena\variant_table.py) names the (arena slot, mesh) of every stage-site model-part draw. Draws the join does not name are counted by class only.

Output (deterministic, sorted, no times; `format` 1):
  { format, scene, arena, looks:{look:{captures:[name], frames:N}},
    meshes:[ { file:"bgNN.pac#slot", slot, mesh, effect, tex, looks:{look:{draws, ambiguous, choices:[CHOICE]}} } ],
    classes:[ { look, class, technique, draws, choices:[CHOICE] } ],           # every draw, by class and technique (joined or not)
    unjoined:{ look:{class:count} } }                                           # model-part draws the join could not name, by class
  CHOICE = { vs, ps, defines:[FX_*...], blend:"off" | "on SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD a:ONE/ZERO/ADD", wmask, alpha:"off" | "<cmp> <ref>",
             slots:{ "<slot>": ROLE }, vtex:[slots >= 16], vstreams:[vertex-buffer source indices], n }
  ROLE = file | map | none | flat:black|white|grey | target:<SurfaceRole> | rt   (file = a game-file texture the capture names; map = a texture with content; rt = a render target with no role name)
Nothing from the game is written: names, define names, enum names and small integers only."""
import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))

FX_DIR = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
TABLE_DIR = os.path.join(HERE, 'choice_table')
FORMAT = 1
# the SurfaceRole names (names.h)
ROLES = {0: 'unnamed', 1: 'SceneColor', 2: 'SceneDepth', 3: 'ShadowMap', 4: 'PostStep', 5: 'FeedbackMap', 6: 'AtlasTarget', 7: 'MenuPicture', 8: 'TronPicture', 9: 'Ui', 10: 'Present', 11: 'Other', 12: 'RimMap'}
BLEND_FACTOR = ['ZERO', 'ONE', 'SRC_COLOR', 'ONE_MINUS_SRC_COLOR', 'DST_COLOR', 'ONE_MINUS_DST_COLOR', 'SRC_ALPHA', 'ONE_MINUS_SRC_ALPHA', 'DST_ALPHA', 'ONE_MINUS_DST_ALPHA',
                'CONST_COLOR', 'ONE_MINUS_CONST_COLOR', 'CONST_ALPHA', 'ONE_MINUS_CONST_ALPHA', 'SRC_ALPHA_SATURATE']
BLEND_OP = ['ADD', 'SUBTRACT', 'REVERSE_SUBTRACT', 'MIN', 'MAX']
COMPARE = ['NEVER', 'LESS', 'EQUAL', 'LEQUAL', 'GREATER', 'NOTEQUAL', 'GEQUAL', 'ALWAYS']
FIXED_SIZE = 72      # adapter_frame.h DrawFixed: DrawState 56, alpha_test @56, alpha_func @57, alpha_ref f32 @64 (asserted against the file's own element size)
SEC_FIXEDS = 7
STATE_BLEND0 = 24    # DrawState: program u16, layout u16, 12 u8 flags, front 4, back 4, blend[4] x 8 from 24 (src/renderer/api.h)


def fmt_factor(i):
    return BLEND_FACTOR[i] if i < len(BLEND_FACTOR) else 'F%d' % i


def fmt_op(i):
    return BLEND_OP[i] if i < len(BLEND_OP) else 'OP%d' % i


def blend_key(fx):
    """fx = the 72 bytes of one DrawFixed -> the blend key text of colour target 0 (enable, src / dst / op, alpha src / dst / op) and its write mask."""
    en, sc, dc, co, sa, da, ao, wm = struct.unpack_from('8B', fx, STATE_BLEND0)
    if not en:
        return 'off', wm
    return 'on %s/%s/%s a:%s/%s/%s' % (fmt_factor(sc), fmt_factor(dc), fmt_op(co), fmt_factor(sa), fmt_factor(da), fmt_op(ao)), wm


def alpha_key(fx):
    on, func = struct.unpack_from('2B', fx, 56)
    ref, = struct.unpack_from('<f', fx, 64)
    if not on:
        return 'off'
    return '%s %.6g' % (COMPARE[func] if func < len(COMPARE) else 'CMP%d' % func, ref)


class Fx:
    """shader hash -> variant name '<effect>_<variant>_<stage>' (the fx.json hashes, tools\\solver\\solve.py Effects, as tools\\scene\\fx_names.py) and the name's FX_* defines."""
    def __init__(self):
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'scene'))
        import fx_names
        self.fd = fx_names.Fx()
        self.defines = self.fd.defines

    def names(self, vs_hash, ps_hash):
        lab, _per, _why = self.fd.eff.resolve(vs_hash, ps_hash)
        pe = lab.split('|')[0] if lab else None
        vs = self.fd.name_of('vertex', vs_hash, pe) or ''
        ps = (self.fd.name_of('fragment', ps_hash, pe) or '') if ps_hash else ''
        return vs, ps

    def of(self, vs, ps):
        return sorted(set(self.defines.get(vs, []) + self.defines.get(ps, [])))


def sections(buf):
    magic, ver, cf, nsec, gf, pi, flags = struct.unpack_from('<IIIIQII', buf, 0)
    secs = {}
    for k in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * k)
        secs[sid] = (es, cnt, off)
    return secs


def read_images(folder):
    """images.bin -> {id: {w, h, role, target}}. The role byte sits at record offset 18 (id 4, w / h / d 6, then mips layers format kind usage source flags game_format role)."""
    b = open(os.path.join(folder, 'images.bin'), 'rb').read()
    cnt = struct.unpack_from('<I', b, 8)[0]
    out = {}
    for i in range(cnt):
        o = 16 + 24 * i
        iid, w, h, d = struct.unpack_from('<IHHH', b, o)
        usage, role = b[o + 14], b[o + 18]
        out[iid] = dict(w=w, h=h, role=ROLES.get(role, 'role%d' % role), target=bool(usage & 2))
    return out


def prepare(path):
    """-> (capture folder, name). A case zip is unpacked once (capture\\ only) under out\\harvest\\unpack (scratch)."""
    path = os.path.normpath(path)
    if not path.lower().endswith('.zip'):
        return path, os.path.basename(os.path.dirname(path) if os.path.basename(path) == 'capture' else path)
    name = os.path.splitext(os.path.basename(path))[0]
    dest = os.path.join(ROOT, 'out', 'harvest', 'unpack', name)
    folder = os.path.join(dest, 'capture')
    if not os.path.exists(os.path.join(dest, '.done')):
        os.makedirs(dest, exist_ok=True)
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                if n.startswith('capture/') and not n.endswith('/'):
                    z.extract(n, dest)
        open(os.path.join(dest, '.done'), 'w').write('1')
    return folder, name


class Role:
    """The role of one bound texture slot of a draw."""
    def __init__(self, images, cap):
        self.images, self.cap, self.cache = images, cap, {}

    def of(self, image, prov_files):
        if image == 0:
            return 'none'
        if image in prov_files:
            return 'file'
        info = self.images.get(image)
        if info and info['role'] not in ('unnamed', 'Other'):
            return 'target:' + info['role']
        if info and info['target']:
            return 'rt'
        if image in self.cache:
            return self.cache[image]
        r = 'map'
        if info and info['w'] * info['h'] <= 64:   # a tiny texture: flat colour?
            d = self.cap.image_data(image)
            if d is not None and len(d) > 0:
                vals = set(bytes(d)[:64])
                if vals == {0}:
                    r = 'flat:black'
                elif vals == {255}:
                    r = 'flat:white'
                elif len(vals) == 1:
                    r = 'flat:grey'
        self.cache[image] = r
        return r


def joined_draws(cap, arena, fi, fn, cache, fx):
    """-> {draw index: [(slot, mesh)]}: variant_table.joined_draws with a wider gate. A capture taken with the game's own renderer drawing (mirror off) carries NO DrawOrigin on most stage
    draws (class 'unnamed', site 0; run 7280: 14 of ~150 joined with the origin gate); such a draw is a stage draw when its vertex program is a yBG_ / yProp_ one, and has no part
    index to filter the mesh by (positions + counts + texture + uv + colour decide, as the census's tie rules)."""
    import join_census as jc
    cap.catch_up(fn)
    f = cap.frame(fn, None)
    cap.advance_contents(f, fn)
    o, d = f.origins, f.draws
    out = {}
    for i in range(len(d)):
        tagged = o is not None and int(o[i]['klass']) == jc.CLASS_MODEL_PART and int(o[i]['pass_site']) in (jc.SITE_A, jc.SITE_B)
        if not tagged:
            if o is not None and int(o[i]['klass']) not in (0,):
                continue
            vs, _ps = fx.names(int(d[i]['vs_hash']), int(d[i]['ps_hash']))
            if not vs.startswith(('yBG_', 'yProp_')):
                continue
        m = jc.match_draw(cap, f, i, fi, cache)
        if m.get('nogeo') or not m['hits']:
            continue
        hits = m['hits']
        if tagged:
            part = int(o[i]['part'])
            hits = [x for x in hits if x.index == part]
            if not hits:
                continue
        tex_img = next((int(s['image']) for s in f.slots[int(d[i]['tex_first']):int(d[i]['tex_first']) + int(d[i]['tex_count'])] if int(s['slot']) == 0), 0)
        if len(hits) > 1 and tex_img:
            payload = cap.image_data(tex_img)
            if payload is not None:
                keep = [x for x in hits if jc.diffuse_tex(arena, x) is not None and jc.compare_texture(payload, jc.diffuse_tex(arena, x)) in ('equal', 'tail', 'padded')]
                if keep:
                    hits = keep
        if len(hits) > 1:
            keep = [y for y in hits if jc.uv_matches(y, jc.draw_attr_arrays(cap, f, i))]
            if keep:
                hits = keep
        if len(hits) > 1:
            keep = [y for y in hits if jc.colour_matches(y, jc.draw_colour_arrays(cap, f, i))]
            if keep:
                hits = keep
        out[i] = sorted({(x.slot, x.index) for x in hits})
    return out


def harvest(cap, folder, arena_id, frames, look, fx, verbose=True):
    """-> (meshes, classes, unjoined, nframes) accumulated over the frames. cap = join_census.Capture on the folder."""
    import join_census as jc
    import solve
    import variant_table as vt
    import arena_files as af
    images = read_images(folder)
    roler = Role(images, cap)
    arena = fi = None
    if arena_id is not None:
        arena = af.load_arena(arena_id)
        fi = jc.FileIndex(arena, None)
    index_of = {cap.frame_number(i): i for i in range(len(cap.frames))}
    meshes = {}          # (slot, mesh) -> {'draws', 'ambiguous', 'choices': {key: n}}
    classes = {}         # (class, technique) -> {'draws', 'choices'}
    unjoined = collections.Counter()
    jcache = {}
    done = 0
    for fn in frames:
        if fn not in index_of:
            continue
        fi_idx = index_of[fn]
        joined = {}
        f = cap.frame(fi_idx, None)
        if arena is not None:
            joined = joined_draws(cap, arena, fi, fi_idx, jcache, fx)
        secs = sections(f.buf)
        es, cnt, off = secs.get(SEC_FIXEDS, (0, 0, 0))
        if es != FIXED_SIZE:
            raise SystemExit('fixed-state element size %d in frame %d, the harvester reads %d (adapter_frame.h DrawFixed changed)' % (es, fn, FIXED_SIZE))
        prov_files = {int(p['image']) for p in f.provenance if int(p['file_name'])} if len(f.provenance) else set()
        for i in range(len(f.draws)):
            d = f.draws[i]
            o = f.origins[i] if f.has_origins else None
            cl = solve.KLASS.get(int(o['klass']), '?') if o is not None else 'unnamed'
            tech = f.string(int(o['technique'])) if o is not None else ''
            vs_n, ps_n = fx.names(int(d['vs_hash']), int(d['ps_hash']))
            fixed = bytes(f.buf[off + int(d['fixed']) * es: off + (int(d['fixed']) + 1) * es])
            blend, wmask = blend_key(fixed)
            slots = {}
            for s in f.slots[int(d['tex_first']):int(d['tex_first']) + int(d['tex_count'])]:
                slots[int(s['slot'])] = roler.of(int(s['image']), prov_files)
            L = f.layouts[int(d['layout'])] if len(f.layouts) else None
            vstreams = sorted({int(e['src']) for e in f.elements[int(L['first']):int(L['first']) + int(L['count'])]}) if L is not None else []
            choice = dict(vs=vs_n, ps=ps_n, defines=fx.of(vs_n, ps_n), blend=blend, wmask=int(wmask), alpha=alpha_key(fixed),
                          slots={str(k): slots[k] for k in sorted(slots)}, vtex=[k for k in sorted(slots) if k >= 16], vstreams=vstreams)
            if not vs_n:
                choice['vs_hash'] = '%016X' % int(d['vs_hash'])   # a shader the fx table does not name: its hash (shader_list.txt ships hashes too)
            if not ps_n and int(d['ps_hash']):
                choice['ps_hash'] = '%016X' % int(d['ps_hash'])
            key = json.dumps(choice, sort_keys=True)
            c = classes.setdefault((cl, tech), {'draws': 0, 'choices': collections.Counter()})
            c['draws'] += 1
            c['choices'][key] += 1
            if i in joined:
                for sm in joined[i]:
                    m = meshes.setdefault(sm, {'draws': 0, 'ambiguous': 0, 'choices': collections.Counter()})
                    m['draws'] += 1
                    m['ambiguous'] += len(joined[i]) > 1
                    m['choices'][key] += 1
            elif o is not None and int(o['klass']) == jc.CLASS_MODEL_PART and int(o['pass_site']) in (jc.SITE_A, jc.SITE_B) and arena is not None:
                unjoined['%s/%s' % (cl, tech or 'stage-site')] += 1
        done += 1
        if verbose:
            print('  frame %d: %d draws, %d joined to a mesh' % (fn, len(f.draws), len(joined)))
    return meshes, classes, unjoined, done


def choices_out(counter):
    out = []
    for key, n in counter.items():
        c = json.loads(key)
        c['n'] = n
        out.append(c)
    out.sort(key=lambda c: (c['vs'], c['ps'], c['blend'], c['alpha'], json.dumps(c['slots'], sort_keys=True)))
    return out


def build_table(scene, arena_id, look, capname, nframes, meshes, classes, unjoined):
    arena = None
    if arena_id is not None:
        import arena_files as af
        arena = af.load_arena(arena_id)
    rows = []
    for (slot, mesh), m in sorted(meshes.items()):
        fm = arena.slots[slot][mesh] if arena is not None and slot in arena.slots and mesh < len(arena.slots[slot]) else None
        rows.append(dict(file='bg%02d.pac#%d' % (arena_id, slot), slot=slot, mesh=mesh, effect=fm.effect if fm else None, tex=fm.samplers.get('sampDiffusePrm') if fm else None,
                         looks={look: dict(draws=m['draws'], ambiguous=m['ambiguous'], choices=choices_out(m['choices']))}))
    cls = [dict(look=look, **{'class': k[0], 'technique': k[1]}, draws=v['draws'], choices=choices_out(v['choices'])) for k, v in sorted(classes.items())]
    return dict(format=FORMAT, scene=scene, arena=arena_id, looks={look: dict(captures=[capname], frames=nframes)}, meshes=rows, classes=cls,
                unjoined={look: dict(sorted(unjoined.items()))} if unjoined else {})


def dump(table, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(table, f, indent=1, sort_keys=True)
        f.write('\n')


def pick_frames(folder, spec, every):
    names = sorted(int(re.search(r'frame_(\d+)', n).group(1)) for n in os.listdir(os.path.join(folder, 'frames')) if n.startswith('frame_'))
    picked = [int(x) for x in spec.split(',')] if spec else names[::every][1:]   # skip frame 0 (boot)
    # S103 IXF: frames the capture integrity lint flags (a stale frame 0: another capture's geometry offsets) are never harvested; said, not silent
    if os.path.isfile(os.path.join(folder, 'geometry.bin')):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scene'))
        import capture_integrity
        flagged = set(capture_integrity.BadFrames(folder)[0])
        dropped = [f for f in picked if f in flagged]
        if dropped:
            print('choice_harvest: skipped frame(s) %s of %s (capture integrity lint: tools\\scene\\capture_integrity.py)' % (','.join(str(f) for f in dropped), folder))
            picked = [f for f in picked if f not in flagged]
    return picked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--scene', default='')
    ap.add_argument('--look', default='match')
    ap.add_argument('--frames', default='')
    ap.add_argument('--every', type=int, default=40)
    ap.add_argument('--out', default='')
    ap.add_argument('--game', default=os.path.join(r'C:\Users\Jarrod\wwe2k14-recomp', 'game_data', 'extracted'))
    ap.add_argument('--replace', action='store_true', help='overwrite an existing table that holds another capture')
    a = ap.parse_args()
    if not a.capture or not a.scene:
        ap.error('CAPTURE and --scene are required')
    path, arena_id = (a.capture.rsplit(':', 1)[0], int(a.capture.rsplit(':', 1)[1])) if re.search(r':\d+$', a.capture) and not re.search(r'^[A-Za-z]:\d+$', a.capture) else (a.capture, None)
    import join_census as jc
    folder, name = prepare(path)
    cap = jc.Capture(folder)
    frames = pick_frames(folder, a.frames, a.every)
    fx = Fx()
    meshes, classes, unjoined, n = harvest(cap, folder, arena_id, frames, a.look, fx)
    table = build_table(a.scene, arena_id, a.look, name, n, meshes, classes, unjoined)
    out = a.out or os.path.join(TABLE_DIR, '%s__%s.json' % (a.scene, a.look))   # one file per scene AND look: a second look of the scene adds a file, never replaces one
    if os.path.exists(out) and not a.replace:
        old = json.load(open(out, encoding='utf-8'))
        oc = {c for v in old.get('looks', {}).values() for c in v.get('captures', [])}
        if oc != {name}:
            raise SystemExit('%s already holds the capture(s) %s; this is %s. Use another --scene / --look, or --replace to overwrite it.' % (out, sorted(oc), name))
    dump(table, out)
    print('wrote %s: %d meshes, %d class rows, %d frames' % (out, len(table['meshes']), len(table['classes']), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())

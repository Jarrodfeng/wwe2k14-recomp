"""Per-texture comparator, the RenderDoc half (Session 82, harness builder). Run by tools\\native\\texdiff.ps1:

    qrenderdoc.exe --python tools\\native\\texdiff_rdc.py        (embedded Python 3.8; arguments come as environment variables)

  TEXDIFF_OFF, TEXDIFF_ON   the two .rdc captures (same frame, owned frame off / on)
  TEXDIFF_OUT               output folder (result.json, analyze.log, bins\\)
  TEXDIFF_MINW, TEXDIFF_MINH  a "scene" draw targets a colour image at least this big (default 1280 x 720)
  TEXDIFF_SLICE_CAP         bytes above which a mismatching texture is not saved to bins\\ (default 6 MB)

For every draw of the capture's second frame that renders into a big image (the scene and post passes), every texture its
pixel and vertex shaders sample is hashed (all slices of mip 0). A usage is keyed by (pixel shader hash, binding slot, how
many earlier scene draws used the same pixel shader): the same key in both captures is the same use of the same texture.
Result: every key whose texture contents differ, grouped by (size, format, hashes); keys present in one capture only; the
bytes of small mismatching textures (bins\\) so tools\\native\\texdiff.py can draw them side by side. Nothing from the game is
committed: out\\ is local only.
"""
import hashlib
import json
import os
import traceback

import renderdoc as rd

OFF = os.environ['TEXDIFF_OFF']
ON = os.environ['TEXDIFF_ON']
OUT = os.environ['TEXDIFF_OUT']
MINW = int(os.environ.get('TEXDIFF_MINW', '1280'))
MINH = int(os.environ.get('TEXDIFF_MINH', '720'))
SLICE_CAP = int(os.environ.get('TEXDIFF_SLICE_CAP', str(6 << 20)))
os.makedirs(os.path.join(OUT, 'bins'), exist_ok=True)
LOG = open(os.path.join(OUT, 'analyze.log'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def flatten(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            out.append(a)
        flatten(a.children, out)


def res_id(u):
    try:
        return u.descriptor.resource
    except Exception:
        try:
            return u.resources[0].resourceId
        except Exception:
            return None


class Cap(object):
    def __init__(self, name, path):
        self.name = name
        self.cap = rd.OpenCaptureFile()
        st = self.cap.OpenFile(path, '', None)
        log(name, 'open', st)
        st, self.ctl = self.cap.OpenCapture(rd.ReplayOptions(), None)
        log(name, 'capture', st)
        self.textures = {}
        for t in self.ctl.GetTextures():
            self.textures[t.resourceId] = t
        self.md5 = {}      # resource id (int) -> md5 of mip 0, all slices
        self.uses = {}     # key -> dict
        self.nscene = 0

    def texhash(self, rid):
        k = int(rid)
        if k in self.md5:
            return self.md5[k]
        t = self.textures[rid]
        h = hashlib.md5()
        n = 0
        for s in range(max(1, t.arraysize)):
            try:
                d = bytes(self.ctl.GetTextureData(rid, rd.Subresource(0, s, 0)))
            except Exception:
                d = b''
            h.update(d)
            n += len(d)
        self.md5[k] = (h.hexdigest(), n)
        return self.md5[k]

    def shader_hash(self, state, stage):
        try:
            refl = state.GetShaderReflection(stage)
            if refl is not None:
                raw = getattr(refl, 'rawBytes', None)
                if raw:
                    return hashlib.md5(bytes(raw)).hexdigest()[:10]
        except Exception:
            pass
        try:
            return 'id%d' % int(state.GetShader(stage))
        except Exception:
            return 'none'

    def scan(self):
        draws = []
        flatten(self.ctl.GetRootActions(), draws)
        half = len(draws) // 2
        log(self.name, 'draws', len(draws), 'textures', len(self.textures), 'scanning from draw', half)
        occ = {}
        for i, a in enumerate(draws):
            if i < half:
                continue
            self.ctl.SetFrameEvent(a.eventId, False)
            state = self.ctl.GetPipelineState()
            try:
                outs = state.GetOutputTargets()
            except Exception:
                outs = []
            big = False
            for o in outs:
                rid = getattr(o, 'resource', None) or getattr(o, 'resourceId', None)
                t = self.textures.get(rid)
                if t is not None and t.width >= MINW and t.height >= MINH:
                    big = True
            if not big:
                continue
            self.nscene += 1
            psh = self.shader_hash(state, rd.ShaderStage.Pixel)
            n = occ.get(psh, 0)
            occ[psh] = n + 1
            for stage, tag in ((rd.ShaderStage.Pixel, 'p'), (rd.ShaderStage.Vertex, 'v')):
                try:
                    ro = state.GetReadOnlyResources(stage)
                except Exception:
                    log('GetReadOnlyResources failed', traceback.format_exc())
                    continue
                if self.nscene <= 3:
                    log(self.name, 'scene draw', i, tag, 'read-only entries', len(ro), 'sample', [(str(res_id(u)), (res_id(u) in self.textures)) for u in list(ro)[:4]])
                for u in ro:
                    rid = res_id(u)
                    t = self.textures.get(rid) if rid is not None else None
                    if t is None:
                        continue
                    try:
                        slot = u.access.index
                    except Exception:
                        slot = -1
                    key = '%s|%s|%d|%d' % (psh, tag, slot, n)
                    md5, nbytes = self.texhash(rid)
                    self.uses[key] = {'rid': int(rid), 'w': t.width, 'h': t.height, 'fmt': str(t.format.Name()),
                                      'mips': t.mips, 'arr': t.arraysize, 'md5': md5, 'bytes': nbytes,
                                      'draw': i, 'event': a.eventId, 'ps': psh, 'slot': slot, 'stage': tag}
        log(self.name, 'scene draws', self.nscene, 'texture uses', len(self.uses), 'distinct textures', len(self.md5))

    def data(self, rid_int, slice_index=0):
        for rid, t in self.textures.items():
            if int(rid) == rid_int:
                return bytes(self.ctl.GetTextureData(rid, rd.Subresource(0, slice_index, 0)))
        return b''

    def close(self):
        try:
            self.ctl.Shutdown()
            self.cap.Shutdown()
        except Exception:
            pass


def byte_diff(a, b):
    """(share of differing bytes, first differing byte, last differing byte) of two equal-length byte strings."""
    if len(a) != len(b) or not a:
        return None
    x = (int.from_bytes(a, 'little') ^ int.from_bytes(b, 'little')).to_bytes(len(a), 'little')
    nz = len(x) - x.count(0)
    if not nz:
        return (0.0, -1, -1)
    first = next(i for i, c in enumerate(x) if c)
    last = len(x) - 1 - next(i for i, c in enumerate(reversed(x)) if c)
    return (nz / float(len(x)), first, last)


def scan_capture(name, path):
    """Opens one capture ALONE (a second capture open at the same time made the first one's descriptors read as null), scans
    it and closes it. Returns (uses, scene draws, distinct textures)."""
    c = Cap(name, path)
    c.scan()
    res = (c.uses, c.nscene, len(c.md5))
    c.close()
    return res


def fetch_bins(name, path, wanted):
    """wanted: {resource id: bin file path}; saves each texture's slice 0."""
    c = Cap(name, path)
    for rid, fn in wanted.items():
        try:
            open(fn, 'wb').write(c.data(rid))
        except Exception:
            log('bin failed', fn, traceback.format_exc())
    c.close()


def main():
    off_uses, off_scene, off_distinct = scan_capture('off', OFF)
    if os.environ.get('TEXDIFF_OFF_ONLY'):
        return
    on_uses, on_scene, on_distinct = scan_capture('on', ON)
    mism = []
    only_off = sorted(set(off_uses) - set(on_uses))
    only_on = sorted(set(on_uses) - set(off_uses))
    for key in sorted(set(off_uses) & set(on_uses)):
        a, b = off_uses[key], on_uses[key]
        if a['md5'] != b['md5'] or a['bytes'] != b['bytes'] or a['fmt'] != b['fmt']:
            mism.append(key)
    groups = {}
    for key in mism:
        a, b = off_uses[key], on_uses[key]
        gk = '%dx%d %s arr%d off %s on %s' % (a['w'], a['h'], a['fmt'], a['arr'], a['md5'][:8], b['md5'][:8])
        g = groups.setdefault(gk, {'desc': gk, 'w': a['w'], 'h': a['h'], 'fmt': a['fmt'], 'arr': a['arr'], 'mips': a['mips'],
                                   'on_w': b['w'], 'on_h': b['h'], 'on_fmt': b['fmt'],
                                   'off_md5': a['md5'], 'on_md5': b['md5'], 'bytes': a['bytes'], 'uses': 0,
                                   'first_key': key, 'first_off_draw': a['draw'], 'first_on_draw': b['draw'],
                                   'ps': a['ps'], 'slot': a['slot'], 'stage': a['stage'], 'keys': []})
        g['uses'] += 1
        if len(g['keys']) < 6:
            g['keys'].append(key)
    # A key whose texture differs but whose two hashes both appear somewhere in the OTHER capture's sampled set is a
    # draw-order shift (the owned frame draws a different number of draws, so 'the k-th draw of this shader' moves),
    # not a content difference: marked reordered and kept out of the main table.
    off_set = set(u['md5'] for u in off_uses.values())
    on_set = set(u['md5'] for u in on_uses.values())
    for g in groups.values():
        g['reordered'] = g['on_md5'] in off_set and g['off_md5'] in on_set
    out_groups = []
    want_off, want_on = {}, {}
    for n, (gk, g) in enumerate(sorted(groups.items(), key=lambda kv: (kv[1]['reordered'], -kv[1]['uses']))):
        g['idx'] = n
        if not g['reordered'] and g['bytes'] <= SLICE_CAP and g['on_w'] == g['w'] and g['on_h'] == g['h']:
            want_off[off_uses[g['first_key']]['rid']] = os.path.join(OUT, 'bins', 'g%d_off.bin' % n)
            want_on[on_uses[g['first_key']]['rid']] = os.path.join(OUT, 'bins', 'g%d_on.bin' % n)
        out_groups.append(g)
    if want_off:
        fetch_bins('off', OFF, want_off)
        fetch_bins('on', ON, want_on)
        for g in out_groups:
            fa = os.path.join(OUT, 'bins', 'g%d_off.bin' % g['idx'])
            fb = os.path.join(OUT, 'bins', 'g%d_on.bin' % g['idx'])
            if os.path.exists(fa) and os.path.exists(fb):
                st = byte_diff(open(fa, 'rb').read(), open(fb, 'rb').read())
                if st:
                    g['differing_byte_share'] = st[0]
                    g['first_diff_byte'] = st[1]
                    g['last_diff_byte'] = st[2]
    res = {'scene_draws_off': off_scene, 'scene_draws_on': on_scene, 'uses_off': len(off_uses), 'uses_on': len(on_uses),
           'distinct_textures_off': off_distinct, 'distinct_textures_on': on_distinct,
           'mismatching_uses': len(mism), 'groups': out_groups,
           'only_off': [dict(off_uses[k], key=k) for k in only_off[:60]], 'only_on': [dict(on_uses[k], key=k) for k in only_on[:60]],
           'only_off_count': len(only_off), 'only_on_count': len(only_on),
           'only_off_content': sorted(set(u['md5'][:8] + ' %dx%d %s' % (u['w'], u['h'], u['fmt']) for k, u in off_uses.items() if u['md5'] not in on_set))[:80],
           'only_on_content': sorted(set(u['md5'][:8] + ' %dx%d %s' % (u['w'], u['h'], u['fmt']) for k, u in on_uses.items() if u['md5'] not in off_set))[:80]}
    json.dump(res, open(os.path.join(OUT, 'result.json'), 'w'), indent=1)
    log('done: mismatching uses', len(mism), 'groups', len(out_groups))


try:
    main()
except Exception:
    log(traceback.format_exc())
    open(os.path.join(OUT, 'FAILED'), 'w').write('see analyze.log')
LOG.close()
os._exit(0)

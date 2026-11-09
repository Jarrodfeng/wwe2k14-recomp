"""Frame-replay harness, the RenderDoc half (Session 98, stage S2; design out\\s98\\replay\\design.md section B). Run by
tools\\assets\\replay\\replay_extract.ps1 (or by hand):

    qrenderdoc.exe --python tools\\assets\\replay\\rdc_extract.py    (embedded Python 3.8, no numpy; arguments are env vars)

  RDX_RDC      the .rdc capture
  RDX_OUT      output folder (out\\s98\\replay\\<name>\\; local only: game imagery, never committed)
  RDX_SHADERS  the --dump_shaders folder of the same run (guest microcode + translated SPIR-V, named by guest hash)
  RDX_MODE     'scan' (default) or 'dump'
  RDX_KEYS     dump mode: keys.json written by replay_index.py (which events / textures / pixels to save)

scan: every draw of the capture -> scan.json: event id, API call, index / vertex counts, the VS / PS SPIR-V md5 and the guest
  hash it belongs to (md5 against the --dump_shaders .bin files), the raw float constant blocks of both stages (the packed
  registers, decoded into game register numbers by replay_index.py), the 192 fetch constant dwords, every bound texture
  (binding name -> guest fetch slot, resource id, view mips) and sampler, blend / depth / cull / viewport, the colour and depth
  targets, the vertex fetch slice's md5 (from the shared-memory buffer) and the index slice's md5; the other actions (copies,
  dispatches, clears, present) for the frame anatomy; a table of every texture (size, format, mips, slices, type).
dump: for the events / resources keys.json names: textures as DDS (all mips / slices, own format) + PNG (mip 0; cubes as a
  cross), colour targets after (and optionally before) a draw as PNG (+ EXR for the named ones), each pass's last image as JPG
  (the anatomy strip), the presented image, the eye draw's coverage (pixels that changed across it) and PixelHistory at named
  pixels. Ends with os._exit(0) (qrenderdoc otherwise stays open).
"""
import hashlib
import json
import os
import struct
import sys
import time
import traceback

import renderdoc as rd

RDC = os.environ['RDX_RDC']
OUT = os.environ['RDX_OUT']
os.makedirs(OUT, exist_ok=True)
# qrenderdoc runs the script without __file__: the wrapper passes the folder (RDX_HERE)
sys.path.insert(0, os.environ.get('RDX_HERE') or os.path.dirname(os.path.abspath(globals().get('__file__', '.'))))
import replay_common as RC  # noqa: E402

SHADERS = os.environ.get('RDX_SHADERS', '')
MODE = os.environ.get('RDX_MODE', 'scan')
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, 'extract_%s.log' % MODE), 'w')
T0 = time.time()


def log(*a):
    LOG.write('[%7.1f] ' % (time.time() - T0) + ' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def rid_int(r):
    try:
        return int(r)
    except Exception:
        return 0


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)


def open_capture():
    cap = rd.OpenCaptureFile()
    st = cap.OpenFile(RDC, '', None)
    log('open', st)
    st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    log('replay', st)
    if ctl is None:
        raise RuntimeError('OpenCapture failed: %s' % st)
    return cap, ctl


def shader_dump_md5():
    """md5 of every translated SPIR-V file of the shader dump -> (guest hash, file name)."""
    out = {}
    if not SHADERS or not os.path.isdir(SHADERS):
        return out
    for fn in os.listdir(SHADERS):
        if '.vk' in fn and fn.startswith('shader_'):
            out[hashlib.md5(open(os.path.join(SHADERS, fn), 'rb').read()).hexdigest()] = (fn[7:23].upper(), fn)
    return out


def floats(b):
    return list(struct.unpack('<%df' % (len(b) // 4), b[:len(b) // 4 * 4]))


def dwords(b):
    return list(struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4]))


def enum_name(x):
    s = str(x)
    return s.split('.')[-1]


# ---------------------------------------------------------------------------------------------------------------- scan

def scan():
    dump_md5 = shader_dump_md5()
    log('shader dump files', len(dump_md5))
    ucode_cache = {}

    def ucode(guest, stage):
        k = (guest, stage)
        if k not in ucode_cache:
            p = os.path.join(SHADERS, 'shader_%s.ucode.%s' % (guest, 'vert' if stage == 'vs' else 'frag'))
            ucode_cache[k] = RC.ucode_info(p) if os.path.exists(p) else None
        return ucode_cache[k]

    cap, ctl = open_capture()
    sf = ctl.GetStructuredFile()
    textures = {}
    for t in ctl.GetTextures():
        textures[rid_int(t.resourceId)] = {
            'id': rid_int(t.resourceId), 'w': t.width, 'h': t.height, 'd': t.depth, 'mips': t.mips, 'arr': t.arraysize,
            'fmt': str(t.format.Name()), 'type': enum_name(t.type), 'cube': bool(t.cubemap), 'ms': t.msSamp,
            'bytes': t.byteSize}
    buffers = {}
    for b in ctl.GetBuffers():
        buffers[rid_int(b.resourceId)] = b.length
        _RID[rid_int(b.resourceId)] = b.resourceId
    # the guest shared memory: the 512 MiB storage buffer the shaders bind as xe_shared_memory
    shared = next((k for k, v in sorted(buffers.items()) if v == 536870912), 0)
    log('textures', len(textures), 'buffers', len(buffers), 'shared memory buffer', shared, buffers.get(shared))
    acts = []
    flatten(ctl.GetRootActions(), acts)
    draws, others = [], []
    banks = {}            # md5 of a VS 256-register block -> floats (deduplicated)
    spv = {}              # SPIR-V md5 -> guest hash / file
    ndraw = 0
    for a in acts:
        name = a.GetName(sf)
        if not (a.flags & rd.ActionFlags.Drawcall):
            if a.flags & (rd.ActionFlags.Dispatch | rd.ActionFlags.Copy | rd.ActionFlags.Clear | rd.ActionFlags.Present |
                          rd.ActionFlags.Resolve | rd.ActionFlags.BeginPass | rd.ActionFlags.EndPass):
                others.append({'event': a.eventId, 'name': name.split('(')[0], 'flags': str(a.flags).replace('ActionFlags.', ''),
                               'src': rid_int(getattr(a, 'copySource', 0)), 'dst': rid_int(getattr(a, 'copyDestination', 0)),
                               'outputs': [rid_int(o) for o in a.outputs if rid_int(o)]})
            continue
        ctl.SetFrameEvent(a.eventId, False)
        s = ctl.GetPipelineState()
        d = {'event': a.eventId, 'n': ndraw, 'call': name.split('(')[0], 'indexed': bool(a.flags & rd.ActionFlags.Indexed),
             'count': a.numIndices, 'instances': a.numInstances, 'index_offset': a.indexOffset, 'base_vertex': a.baseVertex,
             'vertex_offset': a.vertexOffset}
        ndraw += 1
        fetch = None
        for stage, tag in ((rd.ShaderStage.Vertex, 'vs'), (rd.ShaderStage.Pixel, 'ps')):
            r = s.GetShaderReflection(stage)
            if r is None:
                d[tag] = None
                continue
            m = hashlib.md5(bytes(r.rawBytes)).hexdigest()
            g = dump_md5.get(m)
            spv.setdefault(m, {'guest': g[0] if g else None, 'file': g[1] if g else None, 'bytes': len(r.rawBytes)})
            st = {'spv': m, 'guest': g[0] if g else None}
            for i, cb in enumerate(r.constantBlocks):
                # each GetBufferData is a GPU readback (~0.1 s): only the float constants and one fetch constant block
                if not cb.bufferBacked or not (cb.name == 'xe_uniform_float_constants' or
                                               (cb.name == 'xe_uniform_fetch_constants' and fetch is None)):
                    continue
                try:
                    u = s.GetConstantBlock(stage, i, 0).descriptor
                    raw = bytes(ctl.GetBufferData(u.resource, u.byteOffset, cb.byteSize))
                except Exception:
                    log('cb read failed', a.eventId, tag, cb.name, traceback.format_exc().splitlines()[-1])
                    continue
                if cb.name == 'xe_uniform_float_constants':
                    fl = floats(raw)
                    st['float_count'] = len(fl) // 4
                    info = ucode(g[0], tag) if g else None
                    st['float_map'] = 'ascending' if RC.float_register_map(info, len(fl) // 4) is not None else 'unknown'
                    if len(fl) == 1024:
                        h = hashlib.md5(raw).hexdigest()
                        banks.setdefault(h, fl)
                        st['float_bank'] = h
                    else:
                        st['floats'] = fl
                elif cb.name == 'xe_uniform_fetch_constants' and fetch is None:
                    fetch = dwords(raw)
            tex = []
            try:
                used = s.GetReadOnlyResources(stage)
            except Exception:
                used = []
            for u in used:
                rid = rid_int(u.descriptor.resource)
                if not rid:
                    continue
                idx = u.access.index
                bname = r.readOnlyResources[idx].name if 0 <= idx < len(r.readOnlyResources) else ''
                tb = RC.texture_binding(bname)
                tex.append({'binding': bname, 'fetch': tb[0] if tb else None, 'dim': tb[1] if tb else None, 'id': rid,
                            'first_mip': u.descriptor.firstMip, 'num_mips': u.descriptor.numMips})
            st['textures'] = tex
            smp = []
            try:
                for u in s.GetSamplers(stage):
                    sm = u.sampler
                    idx = u.access.index
                    smp.append({'binding': r.samplers[idx].name if 0 <= idx < len(r.samplers) else '',
                                'u': enum_name(sm.addressU), 'v': enum_name(sm.addressV), 'w': enum_name(sm.addressW),
                                'min': enum_name(sm.filter.minify), 'mag': enum_name(sm.filter.magnify),
                                'mip': enum_name(sm.filter.mip), 'bias': sm.mipBias, 'aniso': sm.maxAnisotropy})
            except Exception:
                pass
            st['samplers'] = smp
            d[tag] = st
        d['fetch'] = fetch
        # vertex data: the VS's first vertex fetch slot, read from the shared-memory buffer at this event
        if d.get('vs') and d['vs'].get('guest') and fetch:
            info = ucode(d['vs']['guest'], 'vs')
            if info and info['vfetch']:
                slot = min(info['vfetch'])
                vf = RC.vertex_fetch(fetch, slot)
                if vf:
                    stride = info['vfetch'][slot] * 4
                    d['vfetch'] = {'slot': slot, 'address': '%08X' % vf[0], 'bytes': vf[1], 'stride': stride,
                                   'vertices': vf[1] // stride if stride else None, 'slots': len(info['vfetch'])}
                    if shared and 0 < vf[1] <= (16 << 20):
                        try:
                            vb = bytes(ctl.GetBufferData(_rid(shared), vf[0], vf[1]))
                            d['vfetch']['md5'] = hashlib.md5(vb).hexdigest()
                            d['vfetch']['head'] = vb[:64].hex()
                        except Exception:
                            log('vfetch read failed', a.eventId, traceback.format_exc().splitlines()[-1])
        try:
            ib = s.GetIBuffer()
            if d['indexed'] and rid_int(ib.resourceId):
                off = ib.byteOffset + a.indexOffset * ib.byteStride
                d['ib'] = {'id': rid_int(ib.resourceId), 'offset': off, 'stride': ib.byteStride}
                if os.environ.get('RDX_INDEX_MD5') and a.numIndices * ib.byteStride <= (4 << 20):
                    d['ib']['md5'] = hashlib.md5(bytes(ctl.GetBufferData(ib.resourceId, off, a.numIndices * ib.byteStride))).hexdigest()
        except Exception:
            pass
        try:
            d['rt'] = [rid_int(o.resource) for o in s.GetOutputTargets() if rid_int(o.resource)]
            d['depth'] = rid_int(s.GetDepthTarget().resource)
        except Exception:
            d['rt'], d['depth'] = [], 0
        try:
            vk = ctl.GetVulkanPipelineState()
            b = vk.colorBlend.blends
            d['blend'] = [{'on': x.enabled, 'src': enum_name(x.colorBlend.source), 'dst': enum_name(x.colorBlend.destination),
                           'op': enum_name(x.colorBlend.operation), 'asrc': enum_name(x.alphaBlend.source),
                           'adst': enum_name(x.alphaBlend.destination), 'mask': x.writeMask} for x in b[:len(d['rt']) or 1]]
            ds = vk.depthStencil
            d['depth_state'] = {'test': ds.depthTestEnable, 'write': ds.depthWriteEnable, 'func': enum_name(ds.depthFunction),
                                'stencil': ds.stencilTestEnable}
            d['cull'] = enum_name(vk.rasterizer.cullMode)
            d['front_ccw'] = vk.rasterizer.frontCCW
            vps = vk.viewportScissor.viewportScissors
            if len(vps):
                v = vps[0].vp
                d['viewport'] = [v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth]
        except Exception:
            log('vk state failed', a.eventId, traceback.format_exc().splitlines()[-1])
        draws.append(d)
        if ndraw % 250 == 0:
            log('draws scanned', ndraw)
    res = {'rdc': RDC, 'draws': draws, 'others': others, 'textures': textures, 'buffers': buffers, 'shared_buffer': shared,
           'spirv': spv, 'vs_banks': banks, 'seconds': time.time() - T0}
    json.dump(res, open(os.path.join(OUT, 'scan.json'), 'w'))
    log('scan done: draws', len(draws), 'other actions', len(others), 'vs banks', len(banks), 'spirv', len(spv))
    ctl.Shutdown()
    cap.Shutdown()


_RID = {}


def _rid(i):
    """ResourceId object for an int, from the capture's resource list (filled in open)."""
    return _RID[i]


# ---------------------------------------------------------------------------------------------------------------- dump

def save(ctl, rid, path, ftype, mip=0, slc=0, cross=False):
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.destType = ftype
    ts.mip = mip
    ts.slice.sliceIndex = slc
    if cross:
        ts.slice.cubeCruciform = True
    ts.alpha = rd.AlphaMapping.Preserve if ftype in (rd.FileType.DDS, rd.FileType.EXR, rd.FileType.PNG) else rd.AlphaMapping.Discard
    try:
        ok = ctl.SaveTexture(ts, path)
    except Exception:
        log('SaveTexture failed', path, traceback.format_exc().splitlines()[-1])
        return False
    if not ok:
        log('SaveTexture returned false', path)
    return bool(ok)


def coverage(before, after, w, h, bpp):
    """Pixels whose bytes changed across a draw: (count, bbox, centroid) - pure Python, row by row."""
    row = w * bpp
    pts = []
    for y in range(h):
        a = before[y * row:(y + 1) * row]
        b = after[y * row:(y + 1) * row]
        if a == b:
            continue
        for x in range(w):
            if a[x * bpp:(x + 1) * bpp] != b[x * bpp:(x + 1) * bpp]:
                pts.append((x, y))
    if not pts:
        return {'pixels': 0}
    n = len(pts)
    xs = sorted(p[0] for p in pts)
    cx, cy = sum(p[0] for p in pts) // n, sum(p[1] for p in pts) // n
    # sample pixels the draw really covered: the one nearest the centroid, and the middle of the left / right halves
    # (two eyes are two blobs: their centroid can sit on the nose)
    def nearest(tx, ty, sel):
        return min(sel, key=lambda p: (p[0] - tx) ** 2 + (p[1] - ty) ** 2)
    split = xs[n // 2]
    left = [p for p in pts if p[0] < split] or pts
    right = [p for p in pts if p[0] >= split] or pts
    samples = [nearest(cx, cy, pts)]
    for half in (left, right):
        hx, hy = sum(p[0] for p in half) // len(half), sum(p[1] for p in half) // len(half)
        q = nearest(hx, hy, half)
        if q not in samples:
            samples.append(q)
    return {'pixels': n, 'bbox': [xs[0], min(p[1] for p in pts), xs[-1], max(p[1] for p in pts)], 'centroid': [cx, cy],
            'samples': [list(p) for p in samples]}


def dump():
    keys = json.load(open(os.environ['RDX_KEYS']))
    cap, ctl = open_capture()
    texinfo = {rid_int(t.resourceId): t for t in ctl.GetTextures()}
    for i, t in texinfo.items():
        _RID[i] = t.resourceId
    for b in ctl.GetBuffers():
        _RID[rid_int(b.resourceId)] = b.resourceId
    res = {'textures': {}, 'targets': [], 'passes': [], 'pixel_history': [], 'coverage': {}}
    for sub in ('tex', 'rt', 'pass'):
        os.makedirs(os.path.join(OUT, sub), exist_ok=True)
    # 1. textures (contents do not change within the frame for the material textures; saved at their first use event)
    for item in sorted(keys.get('textures', []), key=lambda x: x['event']):
        rid = item['id']
        if rid not in texinfo:
            continue
        ctl.SetFrameEvent(item['event'], True)
        t = texinfo[rid]
        base = os.path.join(OUT, 'tex', 't%d' % rid)
        ok_dds = save(ctl, t.resourceId, base + '.dds', rd.FileType.DDS, mip=-1, slc=-1)
        ok_png = save(ctl, t.resourceId, base + '.png', rd.FileType.PNG, cross=bool(t.cubemap))
        if not ok_dds:   # a format DDS cannot hold (e.g. R4G4B4A4): mip 0 slice 0 raw, in its own format
            try:
                open(base + '.bin', 'wb').write(bytes(ctl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))))
                ok_dds = True
                res.setdefault('raw_textures', []).append(rid)
            except Exception:
                log('raw texture failed', rid, traceback.format_exc().splitlines()[-1])
        if item.get('exr'):
            save(ctl, t.resourceId, base + '.exr', rd.FileType.EXR, cross=bool(t.cubemap))
        res['textures'][str(rid)] = {'dds': ok_dds and ('tex/t%d.bin' % rid if rid in res.get('raw_textures', []) else 'tex/t%d.dds' % rid), 'png': ok_png and 'tex/t%d.png' % rid,
                                     'event': item['event'], 'w': t.width, 'h': t.height, 'fmt': str(t.format.Name()),
                                     'mips': t.mips, 'arr': t.arraysize, 'cube': bool(t.cubemap)}
    log('textures saved', len(res['textures']))
    # 2. targets before / after named draws, and the anatomy (each pass's last draw)
    jobs = []
    for item in keys.get('targets', []):
        jobs.append((item['event'], 'after', item))
        if item.get('before'):
            jobs.append((item['event'] - 1, 'before', item))
    for item in keys.get('passes', []):
        jobs.append((item['event'], 'pass', item))
    jobs.sort(key=lambda j: j[0])
    raw_keep = {}
    for ev, kind, item in jobs:
        ctl.SetFrameEvent(ev, True)
        for rid in item['rt']:
            if rid not in texinfo:
                continue
            t = texinfo[rid]
            if kind == 'pass':
                fn = 'pass/p%03d_e%d_t%d.jpg' % (item['pass'], item['event'], rid)
                ok = save(ctl, t.resourceId, os.path.join(OUT, fn), rd.FileType.JPG)
                res['passes'].append({'pass': item['pass'], 'event': item['event'], 'rt': rid, 'file': ok and fn})
                continue
            stem = 'rt/e%d_%s_%s_t%d' % (item['event'], item.get('tag', 'draw'), kind, rid)
            ok = save(ctl, t.resourceId, os.path.join(OUT, stem + '.png'), rd.FileType.PNG)
            ent = {'event': item['event'], 'tag': item.get('tag'), 'kind': kind, 'rt': rid, 'png': ok and stem + '.png',
                   'w': t.width, 'h': t.height, 'fmt': str(t.format.Name())}
            if item.get('exr'):
                ok2 = save(ctl, t.resourceId, os.path.join(OUT, stem + '.exr'), rd.FileType.EXR)
                ent['exr'] = ok2 and stem + '.exr'
            if item.get('coverage') and rid == item['rt'][0]:
                raw = bytes(ctl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0)))
                raw_keep[(item['event'], kind)] = (raw, t)
                if kind == 'after':
                    open(os.path.join(OUT, stem + '.bin'), 'wb').write(raw)
                    ent['raw'] = stem + '.bin'
            res['targets'].append(ent)
    log('targets saved', len(res['targets']), 'pass images', len(res['passes']))
    # 3. coverage of the named draws, and PixelHistory at its centroid (+ named pixels)
    for item in keys.get('targets', []):
        if not item.get('coverage'):
            continue
        a = raw_keep.get((item['event'], 'before'))
        b = raw_keep.get((item['event'], 'after'))
        if not a or not b:
            continue
        t = b[1]
        bpp = len(b[0]) // max(1, t.width * t.height)
        cov = coverage(a[0], b[0], t.width, t.height, bpp)
        res['coverage'][str(item['event'])] = dict(cov, tag=item.get('tag'), rt=item['rt'][0], bpp=bpp)
        pts = list(cov.get('samples', [])) + list(item.get('pixels', []))
        for (x, y) in pts:
            try:
                # the history up to the end of the scene pass: the draws over the eye (lids, lashes) are listed too
                ctl.SetFrameEvent(item.get('history_until', item['event']), True)
                hist = ctl.PixelHistory(t.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            except Exception:
                log('PixelHistory failed', traceback.format_exc().splitlines()[-1])
                continue
            mods = []
            for m in hist:
                def col(v):
                    try:
                        return [round(c, 5) for c in list(v.col.floatValue)[:4]]
                    except Exception:
                        return None
                mods.append({'event': m.eventId, 'pre': col(m.preMod), 'post': col(m.postMod), 'shader_out': col(m.shaderOut),
                             'passed': m.Passed(), 'depth_failed': m.depthTestFailed, 'backface': m.backfaceCulled,
                             'scissor': m.scissorClipped, 'shader_discarded': m.shaderDiscarded,
                             'depth_post': getattr(m.postMod, 'depth', None)})
            res['pixel_history'].append({'event': item['event'], 'tag': item.get('tag'), 'x': x, 'y': y, 'rt': rid_int(t.resourceId),
                                         'mods': mods})
    log('coverage', len(res['coverage']), 'pixel histories', len(res['pixel_history']))
    # 4. the vertex slices of the named meshes (guest bytes, big-endian as in the file: the viewer mesh match)
    shared = next((i for i, b in ((rid_int(b.resourceId), b) for b in ctl.GetBuffers()) if b.length == 536870912), 0)
    res['vertices'] = {}
    if shared:
        os.makedirs(os.path.join(OUT, 'vtx'), exist_ok=True)
        for item in sorted(keys.get('vertices', []), key=lambda x: x['event']):
            try:
                ctl.SetFrameEvent(item['event'], True)
                vb = bytes(ctl.GetBufferData(_rid(shared), item['address'], item['bytes']))
            except Exception:
                log('vertex read failed', item, traceback.format_exc().splitlines()[-1])
                continue
            fn = 'vtx/%08X.bin' % item['address']
            open(os.path.join(OUT, fn), 'wb').write(vb)
            res['vertices']['%08X' % item['address']] = {'md5': hashlib.md5(vb).hexdigest(), 'head': vb[:64].hex(), 'file': fn,
                                                          'bytes': len(vb)}
    log('vertex slices', len(res['vertices']))
    # 5. the presented image
    pres = keys.get('present')
    if pres and pres.get('src') in texinfo:
        ctl.SetFrameEvent(pres['event'], True)
        t = texinfo[pres['src']]
        ok = save(ctl, t.resourceId, os.path.join(OUT, 'rt', 'final_present.png'), rd.FileType.PNG)
        res['final'] = {'event': pres['event'], 'rt': pres['src'], 'png': ok and 'rt/final_present.png', 'w': t.width, 'h': t.height,
                        'fmt': str(t.format.Name())}
    res['seconds'] = time.time() - T0
    json.dump(res, open(os.path.join(OUT, 'dump.json'), 'w'), indent=1)
    log('dump done')
    ctl.Shutdown()
    cap.Shutdown()


def main():
    if MODE == 'dump':
        dump()
    else:
        scan()


try:
    main()
except Exception:
    log(traceback.format_exc())
    open(os.path.join(OUT, 'FAILED_%s' % MODE), 'w').write('see extract_%s.log' % MODE)
LOG.close()
os._exit(0)

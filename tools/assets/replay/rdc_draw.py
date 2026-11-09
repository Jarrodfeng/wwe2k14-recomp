"""Frame replay S3 / S4, the capture half (Session 98): dumps whole draws from a RenderDoc capture so the CPU oracle
(tools\\assets\\replay\\oracle.py) can re-run the game's pixel maths and the viewer's on the SAME per-pixel inputs.

    qrenderdoc.exe --python tools\\assets\\replay\\rdc_draw.py      (embedded Python 3.8, no numpy; arguments are environment variables)
    (wrapper: tools\\assets\\replay\\rdc_draw.ps1)

  RDC      the .rdc capture
  OUT      output folder (one ev<event>\\ folder per draw + draws.json)
  ICOUNTS  comma list of index counts to select draws by (e.g. 1516 = Orton's eyeballs, ch161 key 0)
  PSMD5    optional comma list of md5 prefixes of the pixel shader's SPIR-V (md5 of out\\..\\shaders\\*.vk_fbo.bin.frag); only
           draws whose pixel shader matches one are dumped
  LIST     1 = only list every draw (event, indices, PS md5, VS md5) into draws_all.json, no dumps

Per draw (ev<event>\\): meta.json (state, viewport, constant blocks, texture roles from the reflection, the post-VS layout),
cb_<stage>_<set>_<bind>.bin (raw UBO bytes), tex_<bind>_f<face>.png (mip 0, decoded by RenderDoc) + tex_<bind>_f<face>.raw for
uncompressed formats, rt_before.raw / rt_after.raw (colour target 0 around the draw), depth_after.raw, postvs.bin + postvs_idx.bin.
Local only (game imagery and game shader data: out\\ is gitignored, rule 1).
"""
import hashlib
import json
import os
import struct
import traceback

import renderdoc as rd

RDC = os.environ['RDC']
OUT = os.environ['OUT']
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, 'rdc_draw.log'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def flatten(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            out.append(a)
        flatten(a.children, out)


def md5_of(refl):
    if refl is None or not getattr(refl, 'rawBytes', None):
        return ''
    return hashlib.md5(bytes(refl.rawBytes)).hexdigest()


def rid_of(d):
    for k in ('resource', 'resourceId'):
        v = getattr(d, k, None)
        if v is not None and v != rd.ResourceId.Null():
            return v
    try:
        return d.descriptor.resource
    except Exception:
        return None


def main():
    cap = rd.OpenCaptureFile()
    log('open', cap.OpenFile(RDC, '', None))
    st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    log('capture', st)
    textures = {t.resourceId: t for t in ctl.GetTextures()}
    draws = []
    flatten(ctl.GetRootActions(), draws)
    log('draws', len(draws))
    icounts = set(int(x) for x in os.environ.get('ICOUNTS', '').split(',') if x.strip())
    psmd5 = [x.strip().lower() for x in os.environ.get('PSMD5', '').split(',') if x.strip()]
    listing = os.environ.get('LIST') == '1'
    rows = []
    prev_eid = None
    for idx, a in enumerate(draws):
        want = listing or (a.numIndices in icounts)
        if not want:
            prev_eid = a.eventId
            continue
        ctl.SetFrameEvent(a.eventId, True)
        state = ctl.GetPipelineState()
        ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
        vs = state.GetShaderReflection(rd.ShaderStage.Vertex)
        pm, vm = md5_of(ps), md5_of(vs)
        row = {'index': idx, 'event': a.eventId, 'indices': a.numIndices, 'instances': a.numInstances, 'ps_md5': pm, 'vs_md5': vm}
        rows.append(row)
        if listing or (psmd5 and not any(pm.startswith(p) for p in psmd5)):
            prev_eid = a.eventId
            continue
        d = os.path.join(OUT, 'ev%d' % a.eventId)
        os.makedirs(d, exist_ok=True)
        meta = dict(row)
        vp = state.GetViewport(0)
        meta['viewport'] = [vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth]
        # constant blocks of both stages, raw
        meta['cblocks'] = []
        for stage, tag, refl in ((rd.ShaderStage.Vertex, 'vs', vs), (rd.ShaderStage.Pixel, 'ps', ps)):
            if refl is None:
                continue
            for i, cb in enumerate(refl.constantBlocks):
                try:
                    u = state.GetConstantBlock(stage, i, 0)
                    desc = getattr(u, 'descriptor', u)
                    res = rid_of(desc)
                    off = getattr(desc, 'byteOffset', 0)
                    size = getattr(desc, 'byteSize', 0)
                    data = bytes(ctl.GetBufferData(res, off, size)) if res is not None else b''
                    fn = 'cb_%s_%d_%d.bin' % (tag, getattr(cb, 'fixedBindSetOrSpace', 0), cb.fixedBindNumber)
                    open(os.path.join(d, fn), 'wb').write(data)
                    meta['cblocks'].append({'stage': tag, 'name': cb.name, 'set': getattr(cb, 'fixedBindSetOrSpace', 0),
                                            'bind': cb.fixedBindNumber, 'file': fn, 'bytes': len(data)})
                except Exception:
                    log('cblock failed', tag, i, traceback.format_exc())
        # textures the pixel stage reads
        meta['textures'] = []
        try:
            ro = state.GetReadOnlyResources(rd.ShaderStage.Pixel)
        except Exception:
            ro = []
        for u in ro:
            try:
                res = rid_of(u.descriptor)
                ri = u.access.index
                rr = ps.readOnlyResources[ri]
                t = textures.get(res)
                if t is None:
                    continue
                bind = rr.fixedBindNumber
                faces = 6 if t.cubemap else 1
                fmt = str(t.format.Name())
                ent = {'bind': bind, 'name': rr.name, 'w': t.width, 'h': t.height, 'mips': t.mips, 'arr': t.arraysize,
                       'cube': bool(t.cubemap), 'fmt': fmt, 'res': str(int(res)), 'faces': []}
                for f in range(faces):
                    base = 'tex_%d_f%d' % (bind, f)
                    ts = rd.TextureSave()
                    ts.resourceId = res
                    ts.destType = rd.FileType.PNG
                    ts.mip = 0
                    ts.slice.sliceIndex = f
                    ts.alpha = rd.AlphaMapping.Preserve
                    ok = ctl.SaveTexture(ts, os.path.join(d, base + '.png'))
                    fe = {'png': base + '.png', 'ok': str(ok)}
                    if 'BC' not in fmt and 'DXT' not in fmt:
                        raw = bytes(ctl.GetTextureData(res, rd.Subresource(0, f, 0)))
                        open(os.path.join(d, base + '.raw'), 'wb').write(raw)
                        fe['raw'] = base + '.raw'
                    ent['faces'].append(fe)
                meta['textures'].append(ent)
            except Exception:
                log('texture failed', traceback.format_exc())
        # colour target 0 after this draw, and before it (after the previous draw)
        try:
            outs = state.GetOutputTargets()
            rt = rid_of(outs[0]) if outs else None
            dt = rid_of(state.GetDepthTarget())
            if rt is not None:
                t = textures[rt]
                meta['rt'] = {'w': t.width, 'h': t.height, 'fmt': str(t.format.Name()), 'res': str(int(rt))}
                open(os.path.join(d, 'rt_after.raw'), 'wb').write(bytes(ctl.GetTextureData(rt, rd.Subresource(0, 0, 0))))
            if dt is not None:
                t = textures[dt]
                meta['depth'] = {'w': t.width, 'h': t.height, 'fmt': str(t.format.Name())}
                open(os.path.join(d, 'depth_after.raw'), 'wb').write(bytes(ctl.GetTextureData(dt, rd.Subresource(0, 0, 0))))
        except Exception:
            log('targets failed', traceback.format_exc())
        # post-VS outputs (per vertex), the layout from the VS output signature
        try:
            pv = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            vb = bytes(ctl.GetBufferData(pv.vertexResourceId, pv.vertexByteOffset, 0)) if pv.vertexResourceId != rd.ResourceId.Null() else b''
            open(os.path.join(d, 'postvs.bin'), 'wb').write(vb)
            ib = b''
            if pv.indexResourceId != rd.ResourceId.Null():
                ib = bytes(ctl.GetBufferData(pv.indexResourceId, pv.indexByteOffset, pv.numIndices * pv.indexByteStride))
                open(os.path.join(d, 'postvs_idx.bin'), 'wb').write(ib)
            sig = []
            for s in vs.outputSignature:
                sig.append({'var': s.varName, 'sem': s.semanticName, 'semidx': s.semanticIndex, 'reg': s.regIndex,
                            'comps': s.compCount, 'sys': str(s.systemValue), 'type': str(s.varType)})
            meta['postvs'] = {'stride': pv.vertexByteStride, 'bytes': len(vb), 'idx_stride': pv.indexByteStride,
                              'num_indices': pv.numIndices, 'base_vertex': pv.baseVertex, 'idx_bytes': len(ib),
                              'topology': str(pv.topology), 'sig': sig, 'nearPlane': pv.nearPlane, 'farPlane': pv.farPlane}
        except Exception:
            log('postvs failed', traceback.format_exc())
        # previous draw's target, for the "before" picture
        if prev_eid is not None and 'rt' in meta:
            try:
                ctl.SetFrameEvent(prev_eid, True)
                st2 = ctl.GetPipelineState()
                outs2 = st2.GetOutputTargets()
                rt2 = rid_of(outs2[0]) if outs2 else None
                meta['rt_before_same'] = (rt2 is not None and str(int(rt2)) == meta['rt']['res'])
                rid = int(meta['rt']['res'])
                for r_, t_ in textures.items():
                    if int(r_) == rid:
                        open(os.path.join(d, 'rt_before.raw'), 'wb').write(bytes(ctl.GetTextureData(r_, rd.Subresource(0, 0, 0))))
                meta['rt_before_event'] = prev_eid
            except Exception:
                log('before failed', traceback.format_exc())
        json.dump(meta, open(os.path.join(d, 'meta.json'), 'w'), indent=1)
        log('dumped', a.eventId, a.numIndices, pm[:12])
        prev_eid = a.eventId
    json.dump(rows, open(os.path.join(OUT, 'draws_all.json' if listing else 'draws.json'), 'w'), indent=0)
    # PIXELS="x,y;x,y" (+ PH_EVENT = an event whose colour target 0 is the one to trace): every event that touched each pixel
    pix = [p for p in os.environ.get('PIXELS', '').split(';') if p.strip()]
    if pix:
        ctl.SetFrameEvent(int(os.environ['PH_EVENT']), True)
        rt = rid_of(ctl.GetPipelineState().GetOutputTargets()[0])
        names = {a.eventId: a for a in draws}
        hist = {}
        for p in pix:
            x, y = (int(v) for v in p.split(','))
            mods = ctl.PixelHistory(rt, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            out = []
            for m in mods:
                a = names.get(m.eventId)
                out.append({'event': m.eventId, 'indices': a.numIndices if a else -1, 'pre': list(m.preMod.col.floatValue),
                            'post': list(m.postMod.col.floatValue), 'shader_out': list(m.shaderOut.col.floatValue),
                            'passed': bool(m.Passed()), 'depth_fail': bool(m.depthTestFailed)})
            hist[p] = out
        json.dump(hist, open(os.path.join(OUT, 'pixel_history.json'), 'w'), indent=0)
        log('pixel history', len(hist))
    log('done', len(rows))
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
open(os.path.join(OUT, 'DONE'), 'w').write('1')
LOG.close()
os._exit(0)

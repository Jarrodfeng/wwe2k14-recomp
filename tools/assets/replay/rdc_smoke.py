"""Session 99 (smoke density): dumps a run of particle draws from a RenderDoc capture for the per-sprite comparison with the
viewer (tools\\assets\\smoke_compare.py). qrenderdoc.exe --python tools\\assets\\replay\\rdc_smoke.py (embedded Python, no numpy);
arguments are environment variables:

  RDC     the .rdc capture
  OUT     output folder
  FIRST   (optional) the first event of the run; default: the longest run of consecutive draws with the same pixel shader and
          4-6 indices (the particle quads)
  PIXELS  "x,y;x,y" pixels to trace (pixel history on the run's colour target)

Writes run.json (per draw: event, indices, post-VS vertices = every output as floats, blend state of target 0), rt_before.raw /
rt_after.raw (the colour target before the first and after the last draw of the run), depth.raw, the textures of the first draw
(png) and pixel_history.json. Local only (game imagery: out\\ is gitignored, rule 1).
"""
import json
import os
import struct
import traceback

import renderdoc as rd

RDC = os.environ['RDC']
OUT = os.environ['OUT']
# (review) local output only: OUT must be under <repo>\out\ (this checkout's or the main checkout's; both gitignored)
_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
_ALLOWED = [os.path.realpath(os.path.join(r, 'out')) for r in {_REPO, r'C:\Users\Jarrod\wwe2k14-recomp'}]
if not any(os.path.commonpath([os.path.realpath(OUT), al]) == al for al in _ALLOWED
           if os.path.splitdrive(al)[0].lower() == os.path.splitdrive(os.path.realpath(OUT))[0].lower()):
    open(os.path.join(os.environ.get('TEMP', '.'), 'rdc_smoke_refused.txt'), 'w').write('OUT must be under %s\n' % _ALLOWED)
    os._exit(2)
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, 'rdc_smoke.log'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def flatten(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            out.append(a)
        flatten(a.children, out)


def rid_of(d):
    for k in ('resource', 'resourceId'):
        v = getattr(d, k, None)
        if v is not None and v != rd.ResourceId.Null():
            return v
    try:
        return d.descriptor.resource
    except Exception:
        return None


def ps_id(ctl, a):
    ctl.SetFrameEvent(a.eventId, False)
    st = ctl.GetPipelineState()
    return str(st.GetShader(rd.ShaderStage.Pixel)), str(st.GetShader(rd.ShaderStage.Vertex))


def main():
    cap = rd.OpenCaptureFile()
    log('open', cap.OpenFile(RDC, '', None))
    st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    textures = {t.resourceId: t for t in ctl.GetTextures()}
    draws = []
    flatten(ctl.GetRootActions(), draws)
    log('draws', len(draws))
    first = int(os.environ.get('FIRST', '0') or 0)
    # candidate runs: consecutive draws with 4..6 indices
    runs, cur = [], []
    for i, a in enumerate(draws):
        if 4 <= a.numIndices <= 6:
            cur.append(i)
        else:
            if cur:
                runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    runs.sort(key=len, reverse=True)
    log('runs', [(len(r), draws[r[0]].eventId) for r in runs[:8]])
    run = None
    if first:
        for r in runs:
            if any(draws[i].eventId == first for i in r):
                run = [i for i in r if draws[i].eventId >= first]
    else:
        # split the longest runs by shader pair, keep the longest same-shader stretch
        best = []
        for r in runs[:6]:
            seg, key = [], None
            for i in r:
                k = ps_id(ctl, draws[i])
                if k != key and seg:
                    if len(seg) > len(best):
                        best = seg
                    seg = []
                key = k
                seg.append(i)
            if len(seg) > len(best):
                best = seg
        run = best
    log('run', len(run), draws[run[0]].eventId, draws[run[-1]].eventId)
    rows = []
    rt = dt = None
    for n, i in enumerate(run):
        a = draws[i]
        ctl.SetFrameEvent(a.eventId, True)
        state = ctl.GetPipelineState()
        vs = state.GetShaderReflection(rd.ShaderStage.Vertex)
        row = {'event': a.eventId, 'indices': a.numIndices}
        try:
            cbs = state.GetColorBlends()
            b = cbs[0]
            row['blend'] = {'enable': bool(b.enabled), 'src': str(b.colorBlend.source), 'dst': str(b.colorBlend.destination),
                            'op': str(b.colorBlend.operation), 'asrc': str(b.alphaBlend.source), 'adst': str(b.alphaBlend.destination),
                            'mask': int(b.writeMask)}
        except Exception:
            row['blend'] = 'err ' + traceback.format_exc()[-200:]
        try:
            pv = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            vb = bytes(ctl.GetBufferData(pv.vertexResourceId, pv.vertexByteOffset, 0))
            stride = pv.vertexByteStride
            nv = min(len(vb) // stride if stride else 0, 8)
            row['stride'] = stride
            row['verts'] = [list(struct.unpack_from('<%df' % (stride // 4), vb, k * stride)) for k in range(nv)]
            if pv.indexResourceId != rd.ResourceId.Null():
                ib = bytes(ctl.GetBufferData(pv.indexResourceId, pv.indexByteOffset, pv.numIndices * pv.indexByteStride))
                fmt = '<%d%s' % (pv.numIndices, 'H' if pv.indexByteStride == 2 else 'I')
                row['idx'] = list(struct.unpack_from(fmt, ib, 0)) if len(ib) >= pv.numIndices * pv.indexByteStride else []
            row['topology'] = str(pv.topology)
            if n == 0:
                row['sig'] = [{'var': s.varName, 'sem': s.semanticName, 'idx': s.semanticIndex, 'reg': s.regIndex,
                               'comps': s.compCount, 'sys': str(s.systemValue)} for s in vs.outputSignature]
        except Exception:
            row['postvs'] = 'err ' + traceback.format_exc()[-300:]
        if n == 0:
            vp = state.GetViewport(0)
            row['viewport'] = [vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth]
            outs = state.GetOutputTargets()
            rt = rid_of(outs[0]) if outs else None
            dt = rid_of(state.GetDepthTarget())
            ro = state.GetReadOnlyResources(rd.ShaderStage.Pixel)
            ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
            row['textures'] = []
            for u in ro:
                try:
                    res = rid_of(u.descriptor)
                    t = textures.get(res)
                    if t is None:
                        continue
                    bind = ps.readOnlyResources[u.access.index].fixedBindNumber
                    ts = rd.TextureSave()
                    ts.resourceId = res
                    ts.destType = rd.FileType.PNG
                    ts.mip = 0
                    ts.alpha = rd.AlphaMapping.Preserve
                    fn = 'tex_%d_%s.png' % (bind, int(res))
                    ctl.SaveTexture(ts, os.path.join(OUT, fn))
                    row['textures'].append({'bind': bind, 'res': int(res), 'w': t.width, 'h': t.height, 'fmt': str(t.format.Name()),
                                            'file': fn})
                except Exception:
                    log('tex', traceback.format_exc())
            try:  # the pixel shader as RenderDoc disassembles it (the translated SPIR-V: game shader data, local only)
                pipe = state.GetGraphicsPipelineObject()
                targets = ctl.GetDisassemblyTargets(True)
                txt = ctl.DisassembleShader(pipe, ps, targets[0])
                open(os.path.join(OUT, 'ps_disasm.txt'), 'w').write(txt)
            except Exception:
                log('disasm', traceback.format_exc())
            if rt is not None:
                t = textures[rt]
                row['rt'] = {'w': t.width, 'h': t.height, 'fmt': str(t.format.Name()), 'res': int(rt)}
            if dt is not None:
                t = textures[dt]
                row['depth'] = {'w': t.width, 'h': t.height, 'fmt': str(t.format.Name()), 'res': int(dt)}
            # PS constants
            try:
                row['ps_cb'] = []
                for k, cb in enumerate(ps.constantBlocks):
                    u = state.GetConstantBlock(rd.ShaderStage.Pixel, k, 0)
                    desc = getattr(u, 'descriptor', u)
                    res = rid_of(desc)
                    data = bytes(ctl.GetBufferData(res, getattr(desc, 'byteOffset', 0), min(getattr(desc, 'byteSize', 0), 4096)))
                    row['ps_cb'].append({'name': cb.name, 'floats': list(struct.unpack_from('<%df' % (len(data) // 4), data, 0))[:64]})
            except Exception:
                log('pscb', traceback.format_exc())
        rows.append(row)
    json.dump(rows, open(os.path.join(OUT, 'run.json'), 'w'))
    # targets: before the first draw (state at the previous event) and after the last
    prev = draws[run[0] - 1].eventId
    ctl.SetFrameEvent(prev, True)
    if rt is not None:
        open(os.path.join(OUT, 'rt_before.raw'), 'wb').write(bytes(ctl.GetTextureData(rt, rd.Subresource(0, 0, 0))))
    if dt is not None:
        open(os.path.join(OUT, 'depth.raw'), 'wb').write(bytes(ctl.GetTextureData(dt, rd.Subresource(0, 0, 0))))
    ctl.SetFrameEvent(draws[run[-1]].eventId, True)
    if rt is not None:
        open(os.path.join(OUT, 'rt_after.raw'), 'wb').write(bytes(ctl.GetTextureData(rt, rd.Subresource(0, 0, 0))))
    pix = [p for p in os.environ.get('PIXELS', '').split(';') if p.strip()]
    if pix and rt is not None:
        hist = {}
        for p in pix:
            x, y = (int(v) for v in p.split(','))
            mods = ctl.PixelHistory(rt, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            hist[p] = [{'event': m.eventId, 'pre': list(m.preMod.col.floatValue), 'post': list(m.postMod.col.floatValue),
                        'shader_out': list(m.shaderOut.col.floatValue), 'passed': bool(m.Passed())}
                       for m in mods if draws[run[0]].eventId <= m.eventId <= draws[run[-1]].eventId]
        json.dump(hist, open(os.path.join(OUT, 'pixel_history.json'), 'w'), indent=0)
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

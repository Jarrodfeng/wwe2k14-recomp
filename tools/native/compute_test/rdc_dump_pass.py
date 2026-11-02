"""Offline pass tests, the capture half (Session 82, harness builder): dumps one pass's inputs and real output from a RenderDoc
capture, raw, so compute_test.py can run a new compute shader on them and compare. Run by dump_pass.ps1:

    qrenderdoc.exe --python tools\\native\\compute_test\\rdc_dump_pass.py     (embedded Python 3.8; arguments are environment variables)

  RDC    the .rdc capture (tools\\native\\texdiff.ps1 makes them; or gpu_native_debug_rdc_now)
  OUT    output folder (manifest.json + one .bin per texture)
  DRAW   the index of the draw / dispatch in the capture's flattened action list (negative = from the end), or
  EVENT  the event id
  PS     optional: instead of DRAW, the first 8+ hex digits of a pixel shader's hash (md5 of its SPIR-V, as texdiff prints) and
         NTH (default 0): the NTH draw of the capture that uses it

For that event: every texture a shader stage samples (in_<stage><slot>.bin), every read-write texture of a dispatch
(rw_<stage><slot>.bin: the state AFTER the event), the colour targets (out<i>.bin: after the event) and the depth target
(depth.bin); raw bytes of slice 0, mip 0, in the texture's own format (compressed formats stay compressed). manifest.json lists
file, role, size, RenderDoc format name and the run_compute.exe format name where one exists (cfmt). Local only (game imagery:
out\\ is gitignored).
"""
import json
import os
import traceback

import renderdoc as rd

RDC = os.environ['RDC']
OUT = os.environ['OUT']
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, 'dump.log'), 'w')
CFMT = {'R8G8B8A8_UNORM': 'rgba8', 'B8G8R8A8_UNORM': 'bgra8', 'R8_UNORM': 'r8', 'R8G8_UNORM': 'rg8', 'R16_FLOAT': 'r16f',
        'R16G16_FLOAT': 'rg16f', 'R16G16B16A16_FLOAT': 'rgba16f', 'R32_FLOAT': 'r32f', 'R32G32_FLOAT': 'rg32f',
        'R32G32B32A32_FLOAT': 'rgba32f', 'R32_UINT': 'r32u', 'R16G16B16A16_SNORM': 'rgba16s'}


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def flatten(actions, out):
    for a in actions:
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            out.append(a)
        flatten(a.children, out)


def rid_of(u):
    try:
        return u.descriptor.resource
    except Exception:
        try:
            return u.resources[0].resourceId
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
    event = os.environ.get('EVENT')
    target = None
    if event:
        target = next((a for a in draws if a.eventId == int(event)), None)
    elif os.environ.get('PS'):
        want, nth, seen = os.environ['PS'].lower(), int(os.environ.get('NTH', '0')), 0
        import hashlib
        for a in draws:
            ctl.SetFrameEvent(a.eventId, False)
            refl = ctl.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
            if refl is not None and getattr(refl, 'rawBytes', None) and hashlib.md5(bytes(refl.rawBytes)).hexdigest().startswith(want):
                if seen == nth:
                    target = a
                    break
                seen += 1
    else:
        i = int(os.environ.get('DRAW', '-1'))
        target = draws[i] if -len(draws) <= i < len(draws) else None
    if target is None:
        log('no such draw; draws in capture:', len(draws))
        return
    ctl.SetFrameEvent(target.eventId, False)
    state = ctl.GetPipelineState()
    log('event', target.eventId, 'name', target.GetName(ctl.GetStructuredFile()), 'draws', len(draws))
    manifest = {'event': target.eventId, 'files': []}

    def dump(role, rid, fname):
        t = textures.get(rid)
        if t is None:
            return
        try:
            data = bytes(ctl.GetTextureData(rid, rd.Subresource(0, 0, 0)))
        except Exception:
            log('GetTextureData failed', role, traceback.format_exc())
            return
        open(os.path.join(OUT, fname), 'wb').write(data)
        name = str(t.format.Name())
        manifest['files'].append({'file': fname, 'role': role, 'w': t.width, 'h': t.height, 'fmt': name, 'cfmt': CFMT.get(name, ''),
                                  'mips': t.mips, 'arr': t.arraysize, 'bytes': len(data)})

    for stage, tag in ((rd.ShaderStage.Vertex, 'v'), (rd.ShaderStage.Pixel, 'p'), (rd.ShaderStage.Compute, 'c')):
        try:
            ro = state.GetReadOnlyResources(stage)
        except Exception:
            ro = []
        for u in ro:
            rid = rid_of(u)
            if rid is not None and rid in textures:
                try:
                    slot = u.access.index
                except Exception:
                    slot = -1
                dump('in %s slot %d' % (tag, slot), rid, 'in_%s%d.bin' % (tag, slot))
        try:
            rw = state.GetReadWriteResources(stage)
        except Exception:
            rw = []
        for u in rw:
            rid = rid_of(u)
            if rid is not None and rid in textures:
                try:
                    slot = u.access.index
                except Exception:
                    slot = -1
                dump('rw %s slot %d (after the event)' % (tag, slot), rid, 'rw_%s%d.bin' % (tag, slot))
    try:
        for i, o in enumerate(state.GetOutputTargets()):
            rid = getattr(o, 'resource', None) or getattr(o, 'resourceId', None)
            if rid:
                dump('colour target %d (after the event)' % i, rid, 'out%d.bin' % i)
        d = state.GetDepthTarget()
        rid = getattr(d, 'resource', None) or getattr(d, 'resourceId', None)
        if rid:
            dump('depth target (after the event)', rid, 'depth.bin')
    except Exception:
        log('targets failed', traceback.format_exc())
    json.dump(manifest, open(os.path.join(OUT, 'manifest.json'), 'w'), indent=1)
    log('done', len(manifest['files']), 'files')
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    log(traceback.format_exc())
    open(os.path.join(OUT, 'FAILED'), 'w').write('see dump.log')
LOG.close()
os._exit(0)

"""Session 95 (item F1, gpu_d3d_decouple phase 2): build the LAYER-TRUE (multiview) variants of our own pixel shaders that read a target drawn
inside a multiview pass: the post combines (src\\post\\*.frag) and the side chain / copies (src\\scene\\side_*.frag, audience_copy, liquid_map_copy),
and the view-1-camera variants of the own crowd's vertex shaders (src\\scene\\own_crowd_{match,entrance,card}.vert).

Why: with gpu_d3d_decouple the owned passes draw 2 views into a 2-layer image. Our hand-written shaders read their inputs at layer 0
(vec3(uv, 0.0) / ivec3(p, 0)) in BOTH views, so view 1 of the last pass presents layer 0 again: the blended in-between is thrown away.
A variant reads layer  min(gl_ViewIndex, layers - 1)  instead (an input with fewer layers than views is read at its last layer; one layer -> layer 0).

Same idea as build_multiview_vs.py: the plain sources and headers are never touched; a temp copy of src\\post and src\\scene is rewritten and
compiled. Every  texture / textureLod / texelFetch (sampler2DArray(T, S), COORD ...)  gets COORD wrapped:  XeL(COORD, T, S)  (XeLi for texelFetch).
textureSize() calls are left alone (they only use .xy). The tool FAILS the build, and writes NOTHING, when a  sampler2DArray(  use in a shader or in
any file it includes is left in neither form, so no shader silently keeps layer 0.
Material shaders (arena_*, wrestler_*, sprites, the crowd's pixel shaders) stay at layer 0 on purpose (their inputs come from the tick's own picture).
Own crowd vertex shaders: the CrowdScene block gets  vec4 view_proj1[4]  at vec4 24-27 (after the pixel shader's alpha_info at 23); the clip position
reads view_proj in view 0 and view_proj1 in view 1 (the SDK fills view_proj1 with the blended camera, vcp RecordOwnCrowd).
Output: <dir>\\spirv\\<name>_mv.h (array <name>_mv_spv); the MEDIUMP compiles of the two original combines too (<name>_mp_mv.h); the crowd vertex shaders
<name>_mv_vert.h (array <name>_mv_vert_spv). tools\\post\\build_shaders.ps1 runs this after its own compiles.

    py tools\\shaders\\build_multiview_ps.py [name ...]
"""
import glob, os, re, shutil, subprocess, sys, tempfile

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
POST = os.path.join(R, 'src', 'post')
SCENE = os.path.join(R, 'src', 'scene')
MEDIUMP = {'match_combine_original', 'entrance_combine_original'}
SCENE_LIST = re.compile(r'^(side_.*|audience_copy|liquid_map_copy)$')
CROWD = ('own_crowd_match', 'own_crowd_entrance', 'own_crowd_card')
INC = re.compile(r'#include\s+"([^"]+)"')

HELPERS = '''// Session 95 (gpu_d3d_decouple): the multiview variant (tools/shaders/build_multiview_ps.py): inputs are read at the view's layer.
float XeLayer(texture2DArray t, sampler s) { return float(min(gl_ViewIndex, textureSize(sampler2DArray(t, s), 0).z - 1)); }
vec3 XeL(vec3 c, texture2DArray t, sampler s) { return vec3(c.xy, c.z + XeLayer(t, s)); }
ivec3 XeLi(ivec3 c, texture2DArray t, sampler s) { return ivec3(c.xy, c.z + int(XeLayer(t, s))); }
'''

CALL = re.compile(r'\b(textureLod|texture|texelFetch)\(\s*sampler2DArray\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*,')


def find_glslang():
    c = sorted(glob.glob(r'C:\VulkanSDK\*\Bin\glslangValidator.exe'), reverse=True)
    if c:
        return c[0]
    p = shutil.which('glslangValidator')
    if p:
        return p
    raise SystemExit('glslangValidator not found (Vulkan SDK)')


def rewrite(text):
    """Wrap the coordinate of every array sample. Returns (text, wrapped_count, textureSize_count, total_count)."""
    total = len(re.findall(r'sampler2DArray\(', text))
    sizes = len(re.findall(r'textureSize\(\s*sampler2DArray\(', text))
    out, i, n = [], 0, 0
    while True:
        m = CALL.search(text, i)
        if not m:
            out.append(text[i:])
            break
        out.append(text[i:m.end()])
        k, depth = m.end(), 0
        while True:  # the coordinate argument: up to the top-level ',' or the call's ')'
            ch = text[k]
            if ch in '([':
                depth += 1
            elif ch in ')]':
                if depth == 0:
                    break
                depth -= 1
            elif ch == ',' and depth == 0:
                break
            k += 1
        coord = text[m.end():k]
        fn = 'XeLi' if m.group(1) == 'texelFetch' else 'XeL'
        out.append(' %s(%s, %s, %s)' % (fn, coord.strip(), m.group(2), m.group(3)))
        i = k
        n += 1
    return ''.join(out), n, sizes, total


def closure_counts(path, seen=None):
    """(wrapped, textureSize, total) of the sampler2DArray( uses in the files `path` includes, recursively (counted on their original text)."""
    seen = seen if seen is not None else set()
    n = sz = tot = 0
    text = open(path, encoding='utf-8').read()
    for inc in INC.findall(text):
        q = os.path.normpath(os.path.join(os.path.dirname(path), inc))
        if q in seen or not os.path.exists(q):
            continue
        seen.add(q)
        _, a, b, c = rewrite(open(q, encoding='utf-8').read())
        n, sz, tot = n + a, sz + b, tot + c
        a2, b2, c2 = closure_counts(q, seen)
        n, sz, tot = n + a2, sz + b2, tot + c2
    return n, sz, tot


def crowd_rewrite(text):
    """The own crowd's vertex shaders read view 1's view-projection (CrowdScene vec4 24-27; vec4 23 is the pixel shader's alpha_info) at gl_ViewIndex 1."""
    text = text.replace('} scene;', "  uvec4 alpha_info_unused;  // (the pixel shader's alpha_info)\n  vec4 view_proj1[4];       // Session 95: view 1's view_proj (the blended camera)\n} scene;", 1)
    text, n = re.subn(r'scene\.view_proj\[(\d)\]', r'(gl_ViewIndex == 0 ? scene.view_proj[\1] : scene.view_proj1[\1])', text)
    return text, n


def words_header(data, ident, src_name, defs, stage):
    words = ['0x%08X' % int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    lines = ['    ' + ', '.join(words[i:i + 8]) + ',' for i in range(0, len(words), 8)]
    head = ['#pragma once',
            '// Session 95: the multiview variant of ../%s.%s, generated by tools/shaders/build_multiview_ps.py%s' % (src_name, stage, defs),
            '// (pixel shaders: inputs read at layer min(gl_ViewIndex, layers - 1); own crowd vertex shaders: view 1 reads the blended camera).',
            '// Do not edit: edit the source and run the script.',
            '#include <cstdint>', '', 'inline constexpr uint32_t %s_spv[] = {' % ident] + lines + ['};', '']
    return '\r\n'.join(head), len(words)


def main():
    only = set(sys.argv[1:])
    glslang = find_glslang()
    tmp = tempfile.mkdtemp(prefix='s95mvps_')
    outputs = []  # (path, text): written only when nothing failed
    bad = []
    try:
        for d, name in ((POST, 'post'), (SCENE, 'scene')):
            os.makedirs(os.path.join(tmp, name), exist_ok=True)
            for g in glob.glob(os.path.join(d, '*.glsl')):
                text, _, _, _ = rewrite(open(g, encoding='utf-8').read())
                if os.path.basename(g) == 'own_crowd_vs.glsl':
                    text, _ = crowd_rewrite(text)
                open(os.path.join(tmp, name, os.path.basename(g)), 'w', encoding='utf-8', newline='\n').write(text)
        todo = []
        for f in sorted(glob.glob(os.path.join(POST, '*.frag'))):
            todo.append((f, 'post', POST, 'frag'))
        for f in sorted(glob.glob(os.path.join(SCENE, '*.frag'))):
            if SCENE_LIST.match(os.path.splitext(os.path.basename(f))[0]):
                todo.append((f, 'scene', SCENE, 'frag'))
        for base in CROWD:
            todo.append((os.path.join(SCENE, base + '.vert'), 'scene', SCENE, 'vert'))
        for f, sub, d, stage in todo:
            base = os.path.splitext(os.path.basename(f))[0]
            if only and base not in only:
                continue
            text = open(f, encoding='utf-8').read()
            n = 0
            if stage == 'vert':
                text, n = crowd_rewrite(text)
                if text.count('(gl_ViewIndex == 0') == 0:
                    # the .vert may include own_crowd_vs.glsl (rewritten in the temp copy) instead of reading view_proj itself
                    if '#include "own_crowd_vs.glsl"' not in text:
                        bad.append('%s: no view_proj read found' % base)
                        continue
            else:
                text, n, sz, tot = rewrite(text)
                # the included files count too: every sampler2DArray( use in the closure must be wrapped or a textureSize
                cn, csz, ctot = closure_counts(f)
                if tot + ctot != n + sz + cn + csz:
                    bad.append('%s: %d sampler2DArray( uses (with includes), %d wrapped, %d textureSize' % (base, tot + ctot, n + cn, sz + csz))
                    continue
                if n == 0 and cn == 0:
                    print('%s: reads no array texture, no variant' % base)
                    continue
            m = re.match(r'(#version[^\n]*\n)', text)
            assert m, base
            helpers = HELPERS if stage == 'frag' else ''
            text = m.group(1) + '#extension GL_EXT_multiview : require\n' + helpers + text[m.end():]
            tf = os.path.join(tmp, sub, base + '.' + stage)
            open(tf, 'w', encoding='utf-8', newline='\n').write(text)
            variants = [('', [])]
            if base in MEDIUMP:
                variants.append(('_mp', ['-DMEDIUMP']))
            if base in CROWD:  # Session 95 (P7, gpu_native_crowd_input): the attribute form too
                variants.append(('_vi', ['-DCROWD_VERTEX_INPUT']))
            for suffix, defs in variants:
                spv = os.path.join(tmp, base + suffix + '.spv')
                r = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.1'] + defs + ['-S', stage, '-o', spv, tf], capture_output=True, text=True)
                if r.returncode:
                    bad.append('%s%s: glslang: %s' % (base, suffix, (r.stdout + r.stderr).strip().splitlines()[-4:]))
                    continue
                ident = base + suffix + ('_mv' if stage == 'frag' else '_mv_vert')
                if suffix == '_vi':
                    ident = base + '_mv_vi_vert'
                hdr, nw = words_header(open(spv, 'rb').read(), ident, base, (' with ' + defs[0] if defs else ''), stage)
                outputs.append((os.path.join(d, 'spirv', ident + '.h'), hdr))
                print('%s%s: %s -> %s.h (%d words)' % (base, suffix, ('%d array reads layer-true' % n) if stage == 'frag' else 'view 1 camera', ident, nw))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if bad:
        print('FAILED (a sampler2DArray use left at layer 0, or a compile error); nothing written:')
        for b in bad:
            print('  ' + b)
        sys.exit(1)
    for path, hdr in outputs:  # (written only after every variant built)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'w', encoding='utf-8', newline='').write(hdr)
    print('made %d' % len(outputs))


main()

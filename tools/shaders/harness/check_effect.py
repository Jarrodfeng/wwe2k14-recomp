#!/usr/bin/env python3
r"""Session 100: proves a hand-written effect (src\renderer\shaders\fx\<effect>.vert/.frag + .fx.json) matches the game's
original shaders, offline, 0 game runs.   py tools\shaders\harness\check_effect.py <effect> [--seed N] [--rounds N]
[--hash HASH] [--verbose] [--root DIR]

How: for every variant and hash of the .fx.json
  reference = the game's microcode (from the player's own pacs, check_effect_ucode.py) -> the SDK translator
              (check_effect_translate.exe, default modification, all interpolators kept) -> SPIR-V (cached under
              out\s100\harness\ref) -> spirv-cross -> a compute shader;
  ours      = the variant's GLSL (glslang -E with its defines/includes) -> a compute shader;
  both run (check_effect_run.exe, GPU) over the same random inputs; every output is compared.
Everything derived from the game stays under out\s100\ (rule 1).

Fragment: interpolators (random vec4 per invocation), float constants (random vec4 per 64-invocation set; the reference
reads them dense-packed by the translator's own register map, ours by the JSON's field->register map; the literal
constants of the container are written into the last registers for the reference), bool constants (random 256 bits,
uvec4[2] as the JSON's Bools member), textures = smooth fake functions of (slot, coordinate) the same for both
(the half-texel nudge, derivatives and sign modes are NOT exercised), outputs = every colour output; kill (discard) is
compared as an output; colours are compared only where neither side killed.
Vertex: the hardware epilogue is neutralised (flags: no w division, ndc scale 1 / offset 0) so the reference position
is the position "as the game wrote it". Compared: position and interpolators 0-15 (unwritten = 0).

VERTEX FETCH (the decision). The containers hold the unpatched originals: the vfetch format / stride / offset are
placeholders the game patches per draw (docs\systems\shaders.md section 3), so the original reads nothing meaningful.
check_effect.py therefore patches the reference's vfetch instructions: fetch k (k = the vfetch instruction's index in
program order, full and mini both counted) is made a float fetch whose width is the type our shader declares at the
location the JSON maps fetch k to (float = 32_FLOAT, vec2 = 32_32, vec3 = 32_32_32, vec4 = 32_32_32_32), stride
4 x (fetch count) dwords, offset 4 x k dwords, endian none; fetch constants are set to base 0, type 3. Ours receives
the same random floats at its mapped locations. Fetches the JSON does not map get a vec4 of their own random data (ours
does not read it). So it proves the arithmetic and channel use AFTER the fetch, and that our declared widths give the same
result as the original under that width (a shader reading .w of a 2-wide fetch sees the hardware default 1 / 0 in
the original; ours must do the same). Packed/normalised vertex formats (bytes, shorts, 10-10-10-2) and the real
vertex declaration are NOT covered: they are checked in the frame player. Integer vertex inputs are not supported.

Attribute "range": [lo, hi] and "integer": true (optional, per attribute in the JSON, e.g. a bone index): that vertex input is
drawn uniformly from the range (whole numbers, hi included, if integer) instead of random floats. The reference reads the same
buffer, so both sides see identical values. Default: the random floats above.
Loop constants: variant field "loop_constants": {"31": [count, start, step]} (the containers hold no defaults); a shader with a
loop and no value FAILS with a message. Depth export (oDepth / gl_FragDepth) is compared as output "depth".

Inputs are finite: the game never sends infinities, so nothing here pushes a shader towards the 360's float rules. 3% of the vectors are all
zero, the real hazard (normalizing a zero vector, a power of zero). Ours only has to be right where the game's result is a number.

Pass: |ours - ref| <= 1e-3 * max(largest component of ours or ref in that output, 0.1); outputs where the reference is not finite are not
compared; ours not finite where the reference is finite is a failure, except a chance 0 / 0 (at most 0.05% of the inputs). Inputs where the
reference itself moves by more than a quarter of that when the inputs move by about three rounding errors are skipped (nothing can be matched
there). Exit 0 only if every inventory hash is in a variant and passes.
"""
import argparse, glob, json, os, re, struct, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import check_effect_ucode as uc

H = os.path.join(REPO, 'out', 's100', 'harness')
BIN = os.path.join(H, 'bin')
TRANSLATE = os.path.join(BIN, 'check_effect_translate.exe')
RUNNER = os.path.join(BIN, 'check_effect_run.exe')
DISASM = os.path.join(REPO, 'out', 's100', 'disasm', 'ucode_disasm.exe')
SDKBIN = None
for d in sorted(glob.glob(r'C:\VulkanSDK\*'), reverse=True):
    if os.path.exists(os.path.join(d, 'Bin', 'glslangValidator.exe')):
        SDKBIN = os.path.join(d, 'Bin')
        VKSDK = d
        break
if not SDKBIN:
    raise SystemExit('no Vulkan SDK under C:\\VulkanSDK')
def tool(n): return os.path.join(SDKBIN, n)

CSET = 1024 + 8 + 32     # dwords per constant set: 256 vec4 floats, 256 bool bits, 32 loop uvec4... (8 uvec4 = 32 dwords)
SET_INV = 64             # invocations sharing one constant set
FETCH_FMT = {1: 36, 2: 37, 3: 57, 4: 38}   # k_32_FLOAT, k_32_32_FLOAT, k_32_32_32_FLOAT, k_32_32_32_32_FLOAT

def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('%s failed:\n%s' % (what, (r.stdout + r.stderr)[-2500:]))
    return r.stdout

def ensure_tools():
    os.makedirs(BIN, exist_ok=True)
    if not os.path.exists(TRANSLATE):
        raise SystemExit('missing %s: run pwsh -NoProfile -File tools\\shaders\\harness\\check_effect_build_translate.ps1' % TRANSLATE)
    if not os.path.exists(RUNNER):
        env = os.environ.copy()
        env['PATH'] += r';C:\Program Files\LLVM\bin'
        r = subprocess.run(['clang++', '-O1', os.path.join(HERE, 'check_effect_run.cpp'), '-I' + VKSDK + r'\Include', '-L' + VKSDK + r'\Lib',
                            '-lvulkan-1', '-o', RUNNER], capture_output=True, text=True, env=env)
        if r.returncode:
            raise SystemExit('building the runner failed:\n' + r.stderr[-2000:])

# ---------------------------------------------------------------- reference side
def disassemble(code, kind='VS'):
    """The SDK's own disassembly of the microcode, as text."""
    if not os.path.exists(DISASM):
        raise SystemExit('missing %s: run tools\\shaders\\packets\\build_disasm.ps1' % DISASM)
    inp = os.path.join(H, 'dis_in_%d.bin' % os.getpid()); outp = os.path.join(H, 'dis_out_%d.bin' % os.getpid())
    open(inp, 'wb').write(struct.pack('<II', 0 if kind == 'VS' else 1, len(code) // 4) + code)
    run([DISASM, inp, outp], 'disassembler')
    d = open(outp, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    return d[4:4 + n].decode('utf-8', 'replace')

def vfetch_indices(code):
    """Instruction indices of every vfetch in program order."""
    return [int(m.group(1)) for m in re.finditer(r'/\*\s*(\d+)\s*\*/\s+(?:\([^)]*\)\s*)?vfetch_(?:full|mini)', disassemble(code))]

LAST = {}   # the arrays of the latest comparison, for explain_failure.py

def literal_registers(first, lits):
    """The registers the literals sit in: `first` is literal_base's list, or None for the default block at the top."""
    if isinstance(first, (list, tuple)):
        return list(first)
    start = first if first is not None else 256 - len(lits)
    return range(start, start + len(lits))

def literal_base(h, kind, code, lits):
    """The registers the container's literals sit in, in order (a list), or None when the shader reads none of them (their place does not matter).
    The literals are the directly read float registers no named parameter covers. The block ends at the highest of them and runs down from there,
    stepping over registers a named parameter holds: the game's blocks are not always contiguous (S100, skin: the 4th of 8 literals at c243 or c247,
    the 5th to 8th at c252-c255, named parameters in between)."""
    if not lits:
        return None
    named = uc.named_float_registers(h)
    free = sorted({int(r) for r in re.findall(r'\bc(\d+)\b', disassemble(code, kind))} - named)
    if not free:
        return None
    top = free[-1]
    near = [r for r in free if r > top - 32]   # a low register read without a name (seen: c0) gets random data on both sides, so it can stay
    contiguous = list(range(top - len(lits) + 1, top + 1))
    if contiguous[0] >= 0 and set(near) <= set(contiguous):
        return contiguous
    # Split: the read registers hold the last literals, top down; the unread first ones continue below the lowest of them (S100, skin wrinkle
    # shaders: 8 literals, read c243 and c252-c255; the 4th literal is c243, so the first three sit at c240-c242).
    places = sorted(near, reverse=True)[:len(lits)]
    r = min(places) - 1
    while len(places) < len(lits) and r >= 0:
        if r not in named and r not in places:
            places.append(r)
        r -= 1
    return sorted(places) if len(places) == len(lits) else None

def patch_vfetch(code, widths):
    """widths[k] = float component count of fetch k (see the header). Returns patched microcode bytes (big-endian dwords)."""
    idx = vfetch_indices(code)
    w = list(struct.unpack('>%dI' % (len(code) // 4), code))
    stride = 4 * len(idx)
    for k, i in enumerate(idx):
        d1, d2 = w[i * 3 + 1], w[i * 3 + 2]
        d1 &= ~((1 << 12) | (1 << 13) | (1 << 14) | (0x3F << 16) | (0x3F << 24))   # signed / integer / rf mode, format, exp adjust: all 0
        d1 |= FETCH_FMT[widths.get(k, 4)] << 16
        d2 = (d2 & ~(0xFF | (0x7FFFFF << 8))) | (stride & 0xFF) | ((4 * k) << 8)
        w[i * 3 + 1], w[i * 3 + 2] = d1, d2
    return struct.pack('>%dI' % len(w), *w), len(idx)

def translate_many(items):
    """items: [(cache key, kind, microcode)] -> {key: (spv path, map dict)} (cached by key under out\\s100\\harness\\ref)."""
    d = os.path.join(H, 'ref'); os.makedirs(d, exist_ok=True)
    todo = [it for it in items if not os.path.exists(os.path.join(d, it[0] + '.spv'))]
    if todo:
        buf = b''.join(struct.pack('<II', 0 if k == 'VS' else 1, len(c) // 4) + c for _, k, c in todo)
        bi = os.path.join(H, 'tr_in_%d.bin' % os.getpid()); od = os.path.join(H, 'tr_out_%d' % os.getpid())
        open(bi, 'wb').write(buf)
        run([TRANSLATE, bi, od], 'translator')
        for n, (key, _, _) in enumerate(todo):
            for ext in ('spv', 'map'):
                os.replace(os.path.join(od, '%d.%s' % (n, ext)), os.path.join(d, '%s.%s' % (key, ext)))
    out = {}
    for key, _, _ in items:
        m = {}
        for ln in open(os.path.join(d, key + '.map')).read().split('\n'):
            f = ln.split()
            if f:
                m[f[0]] = [int(x) for x in f[1:]]
        out[key] = (os.path.join(d, key + '.spv'), m)
    return out

CROSSED = {}

def cross(spv):
    if spv not in CROSSED:
        CROSSED[spv] = run([tool('spirv-cross.exe'), '--version', '450', '--vulkan-semantics', spv], 'spirv-cross')
    return CROSSED[spv]

CROSS_FLAT = ('#define SPIRV_CROSS_FLATTEN\n#define SPIRV_CROSS_BRANCH\n#define SPIRV_CROSS_UNROLL\n#define SPIRV_CROSS_LOOP\n')
SPV_INTR = '#extension GL_EXT_spirv_intrinsics : require\n'

def strip_directives(t):
    t = re.sub(r'#version [^\n]*\n', '', t)
    t = re.sub(r'#if defined\(GL_EXT_control_flow_attributes\).*?#endif\n', CROSS_FLAT, t, flags=re.S)
    t = re.sub(r'#extension [^\n]*\n', '', t)
    t = re.sub(r'#line[^\n]*\n', '', t)
    return t

def blocks_to_structs(t, keep=()):
    """uniform blocks -> private structs (the compute wrapper fills them); returns text and {instance: [member lines]}."""
    def f(m):
        if m.group(1) in keep:
            return m.group(0)
        body = re.sub(r'layout\(offset = \d+\)\s*', '', m.group(2))
        return 'struct %s_t {%s}; %s_t %s;' % (m.group(1), body, m.group(1), m.group(3))
    t = re.sub(r'layout\([^)]*\)\s*uniform\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;', f, t, flags=re.S)
    return t

def tex_consts(t, ref):
    """texture / sampler declarations -> int ids (slot; +1000 for cubes); returns text."""
    def f(m):
        name = m.group(2)
        mm = re.match(r'xe_(?:texture|sampler)(\d+)_', name)
        ident = int(mm.group(1)) if mm else 0
        if m.group(1) == 'textureCube':
            ident += 1000
        return 'const int %s = %d;' % (name, ident)
    return re.sub(r'layout\(set = \d+, binding = \d+\)\s*uniform\s+(texture2DArray|textureCube|texture3D|sampler)\s+(\w+)\s*;', f, t)

TEX_PRELUDE = r'''
vec4 fake2D(int id, vec2 p) {
  float s = float(id) * 0.731;
  vec4 a = vec4(2.3, 3.1, 1.7, 2.9) + s * 0.05;
  vec4 b = vec4(1.9, 2.7, 3.3, 1.3) + s * 0.03;
  vec4 ph = vec4(0.3, 1.1, 2.0, 2.9) + s;
  return 0.5 + 0.5 * sin(a * p.x + b * p.y + ph + 0.4 * sin(p.x * 5.0 + s));
}
vec4 fakeCube(int id, vec3 dir) {
  float lengthSq = dot(dir, dir);
  vec3 d = lengthSq > 0.0 ? dir * inversesqrt(lengthSq) : vec3(0.0, 0.0, 1.0);
  float s = float(id) * 0.531;
  return 0.5 + 0.5 * sin(vec4(2.1 * d.x + 1.3 * d.y, 1.7 * d.y - 2.2 * d.z, 2.9 * d.z + 0.8 * d.x, d.x - d.y + d.z) * 1.7 + vec4(0.2, 0.9, 1.6, 2.4) + s);
}
vec4 fakeTexture(int id, vec3 p) { return id >= 1000 ? fakeCube(id - 1000, p) : fake2D(id, p.xy); }
vec4 fakeTexture(int id, vec2 p) { return fakeTexture(id, vec3(p, 0.0)); }
#define texture(t, c) fakeTexture(t, c)
#define textureGrad(t, c, dx, dy) fakeTexture(t, c)
#define textureLod(t, c, l) fakeTexture(t, c)
#define textureSize(t, l) ivec2(1)
#define sampler2DArray(t, s) (t)
#define samplerCube(t, s) (t)
#define dFdx(x) ((x) * 0.0)
#define dFdy(x) ((x) * 0.0)
#define dFdxCoarse(x) ((x) * 0.0)
#define dFdyCoarse(x) ((x) * 0.0)
#define dFdxFine(x) ((x) * 0.0)
#define dFdyFine(x) ((x) * 0.0)
#define fwidth(x) ((x) * 0.0)
'''

LOADERS = r'''
vec4 loadC(uint set, uint reg) { uint b = set * %dU + reg * 4U; return vec4(uintBitsToFloat(cb.c[b]), uintBitsToFloat(cb.c[b + 1U]), uintBitsToFloat(cb.c[b + 2U]), uintBitsToFloat(cb.c[b + 3U])); }
uvec4 loadB(uint set, uint k) { uint b = set * %dU + 1024U + k * 4U; return uvec4(cb.c[b], cb.c[b + 1U], cb.c[b + 2U], cb.c[b + 3U]); }
''' % (CSET, CSET)

TO4 = r'''
vec4 to4(float v) { return vec4(v, 0.0, 0.0, 0.0); }
vec4 to4(vec2 v) { return vec4(v, 0.0, 0.0); }
vec4 to4(vec3 v) { return vec4(v, 0.0); }
vec4 to4(vec4 v) { return v; }
'''

def preprocess(path, stage, defs):
    """glslang -E: includes and #if resolved."""
    r = subprocess.run([tool('glslangValidator.exe'), '-E', '-S', stage, '-DFX_PROOF'] + ['-D' + d for d in defs] + [path], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('glslang -E failed on %s:\n%s' % (path, r.stdout + r.stderr))
    return strip_directives(r.stdout)

SAMPLE_READ = re.compile(r'\b(textures|cubes|volumes)\s*\[\s*(\d+)\s*\]')
SAMPLE_DYNAMIC = re.compile(r'\b(?:textures|cubes|volumes)\s*\[\s*[^\]\s\d]')
SAMPLE_DECL = re.compile(r'uniform\s+\w+\s+(textures|cubes|volumes)\s*\[\s*\d+\s*\]\s*;')

def sampled_slots(text, stage):
    """({game slot: kind} that preprocessed shader text samples with literal indices, True when it also indexes with a variable (a loop: then only the
    hand list knows which slots). A vertex shader's textures[i] is the game's vertex fetch slot 16 + i. The variant's "samplers" come from this, not from
    a hand list alone: an option switched on by a #define in the file is invisible to a list's "when" (S100)."""
    base = 16 if stage in ('vertex', 'vert') else 0
    body = SAMPLE_DECL.sub('', text)
    found = {}
    for kind, index in SAMPLE_READ.findall(body):
        found[base + int(index)] = {'textures': '2d', 'cubes': 'cube', 'volumes': '3d'}[kind]
    return found, bool(SAMPLE_DYNAMIC.search(body))

FAST_REJECT = False   # set by the solver: a candidate wrong on most inputs is dropped before the slower stability runs
COMPILED_REF = {}     # the game's side compiles to the same thing for every candidate tried against it

def quick_reject(mine, ref):
    with np.errstate(all='ignore'):
        scale = np.maximum(np.maximum(np.nan_to_num(np.abs(mine)).max(axis=-1, keepdims=True), np.nan_to_num(np.abs(ref)).max(axis=-1, keepdims=True)), 0.1)
        return bool(((np.abs(mine - ref) / scale > 1e-3) & np.isfinite(ref)).any(axis=(1, 2)).mean() > 0.55)

def compile_comp(text, tag):
    if tag.endswith('_ref') and text in COMPILED_REF and os.path.exists(COMPILED_REF[text]):
        return COMPILED_REF[text]
    d = os.path.join(H, 'work', str(os.getpid())); os.makedirs(d, exist_ok=True)
    src = os.path.join(d, tag + '.comp'); spv = os.path.join(d, tag + '.spv')
    if tag.endswith('_ref'):
        COMPILED_REF[text] = spv
    open(src, 'w').write(text)
    r = subprocess.run([tool('glslangValidator.exe'), '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-o', spv, src], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('glslang failed on %s (%s):\n%s' % (tag, src, (r.stdout + r.stderr)[-2500:]))
    return spv

def constant_fill(text, instance, entries, cs):
    """Ours: statements loading the JSON's field->register map into the block instance; also the raw Bools member."""
    decl = {}
    for m in re.finditer(r'\b(vec4|uvec4)\s+(\w+)\s*(?:\[(\d+)\])?\s*;', re.search(r'struct \w+_t \{(.*?)\};\s*\w+_t ' + instance, text, re.S).group(1)):
        decl[m.group(2)] = (m.group(1), int(m.group(3)) if m.group(3) else None)
    st = []
    for e in entries:
        f = e['field']
        if f not in decl:
            raise RuntimeError('constant field %s is not a member of the constants block' % f)
        for k in range(e.get('count', 1)):
            tgt = '%s.%s%s' % (instance, f, '[%d]' % k if decl[f][1] is not None else '')
            if decl[f][1] is None and k:
                raise RuntimeError('field %s is not an array but count > 1' % f)
            st.append('  %s = loadC(%s, %du);' % (tgt, cs, e['register'] + k))
    if 'Bools' in decl:
        for k in range(2):
            st.append('  %s.Bools[%d] = loadB(%s, %du);' % (instance, k, cs, k))
    return st

# ---------------------------------------------------------------- fragment
def ps_ref(spv_text, mp):
    t = strip_directives(spv_text)
    t = tex_consts(t, True)
    t = re.sub(r'layout\([^)]*\)\s*(?:restrict\s+)?(?:readonly\s+)?buffer\s+XeSharedMemory\s*\{.*?\}\s*\w+\s*;', '', t, flags=re.S)
    t = blocks_to_structs(t)
    t = re.sub(r'0\.00146484375 / float\(bitfieldExtract\([^;]*?\) \+ 1u\)', '0.0', t)   # no half-texel nudge in this comparison
    t = re.sub(r'0\.00146484375 / (?:float\([^;]*?\)|[A-Za-z_]\w*)', '0.0', t)   # the same nudge split into temporaries (inside loops; S100 yRadialBlur)
    ins = [(int(a), b) for a, b in re.findall(r'layout\(location = (\d+)\)\s*in vec4 (\w+);', t)]
    t = re.sub(r'layout\(location = \d+\)\s*in vec4 (\w+);', r'vec4 \1;', t)
    outs = [(int(a), b) for a, b in re.findall(r'layout\(location = (\d+)\)\s*(?:invariant\s+)?out vec4 (\w+);', t)]
    t = re.sub(r'layout\(location = \d+\)\s*(?:invariant\s+)?out vec4 (\w+);', r'vec4 \1;', t)
    t = re.sub(r'gl_SampleMask\[0\]\s*=[^;]*;', '', t)
    t = t.replace('gl_FragDepth', 'g_depth').replace('gl_FragCoord', 'g_fragcoord').replace('discard;', '{ g_kill = true; return; }').replace('void main()', 'void shaderMain()')
    # float constants: dense array filled from the registers
    fm = re.search(r'float_constants\[(\d+)\]', t)
    nf = int(fm.group(1)) if fm else 0
    if mp['dynamic'][0]:
        regs = list(range(nf))
    else:
        regs = [r for r in range(256) if (mp['float'][r // 64] >> (r % 64)) & 1]
    pre = ['  xe_uniform_system_constants.flags = 7u << 16;', '  xe_uniform_system_constants.alpha_to_mask = 0u;',
           '  xe_uniform_system_constants.alpha_test_reference = 0.0;', '  xe_uniform_system_constants.color_exp_bias = vec4(1.0);',
           '  for (int k = 0; k < 2; ++k) xe_uniform_system_constants.texture_swizzled_signs[k] = uvec4(0u);',
           '  for (int k = 0; k < 48; ++k) xe_uniform_fetch_constants.fetch_constants[k] = uvec4(0u);']
    for j, r in enumerate(regs[:nf]):
        pre.append('  xe_uniform_float_constants.float_constants[%d] = loadC(cs, %du);' % (j, r))
    for k in range(2):
        pre.append('  xe_uniform_bool_loop_constants.bool_constants[%d] = loadB(cs, %du);' % (k, k))
    for k in range(8):
        pre.append('  xe_uniform_bool_loop_constants.loop_constants[%d] = loadB(cs, %du);' % (k, k + 2))
    fill = ['  %s = loadV(%du);' % (n, loc) for loc, n in ins]
    return t, fill, pre, [(l, n) for l, n in outs]

def ps_wrap(body, fill, pre, outs):
    head = ('#version 450\n' + SPV_INTR + '#extension GL_EXT_scalar_block_layout : require\nlayout(local_size_x = 64) in;\n'
            'layout(std430, set = 0, binding = 0) readonly buffer InBuf { float d[]; } inb;\n'
            'layout(std430, set = 0, binding = 1) readonly buffer ConstBuf { uint c[]; } cb;\n'
            'layout(std430, set = 0, binding = 2) writeonly buffer OutBuf { vec4 o[]; } outb;\n'
            'float g_depth = -12345.0; bool g_kill = false; vec4 g_fragcoord = vec4(0.0); uint g_i = 0U;\n'
            'vec4 loadV(uint k) { uint b = g_i * 64U + k * 4U; return vec4(inb.d[b], inb.d[b + 1U], inb.d[b + 2U], inb.d[b + 3U]); }\n'
            + LOADERS + TEX_PRELUDE)
    return head


def build_ps(kind, text, fills, pre, outs, cvt):
    """kind: 'ref' or 'mine'. text = the shader body; returns the compute source."""
    head = ps_wrap(text, fills, pre, outs)
    main = ['void main() {', '  g_i = gl_GlobalInvocationID.x;', '  uint cs = g_i / %dU;' % SET_INV, '  g_fragcoord = loadV(15U);'] + pre + fills + ['  shaderMain();',
            '  outb.o[g_i * 10U] = vec4(g_kill ? 1.0 : 0.0, 0.0, 0.0, 0.0);']
    for loc, n in outs:
        main.append('  outb.o[g_i * 10U + %du] = %s;' % (loc + 1, cvt(n)))
    main.append('  outb.o[g_i * 10U + 9U] = vec4(g_depth, 0.0, 0.0, 0.0);')
    main.append('}')
    return head + TO4 + text + '\n' + '\n'.join(main) + '\n'

def ps_mine(path, defs, variant):
    t = preprocess(path, 'frag', defs)
    t = re.sub(r'layout\([^)]*\)\s*uniform\s+sampler(?:2D|Cube)\s+(?:textures|cubes)\s*\[\d+\]\s*;', '', t)
    t = 'const int textures[16] = int[16](0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);\nconst int cubes[16] = int[16](1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,1015);\n' + t
    t = blocks_to_structs(t)
    m = re.search(r'struct \w+_t \{.*?\};\s*\w+_t (\w+);', t, re.S)
    inst = m.group(1) if m else None
    ins = re.findall(r'layout\(location = (\d+)\)\s*(?:flat\s+|smooth\s+)?in\s+(float|vec2|vec3|vec4)\s+(\w+)\s*;', t)
    t = re.sub(r'layout\(location = \d+\)\s*(?:flat\s+|smooth\s+)?in\s+(?:float|vec2|vec3|vec4)\s+\w+\s*;', lambda m: re.sub(r'layout\([^)]*\)\s*(?:flat\s+|smooth\s+)?in\s+', '', m.group(0)), t)
    outs = re.findall(r'layout\(location = (\d+)\)\s*out\s+(float|vec2|vec3|vec4)\s+(\w+)\s*;', t)
    t = re.sub(r'layout\(location = \d+\)\s*out\s+((?:float|vec2|vec3|vec4)\s+\w+\s*;)', r'\1', t)
    t = t.replace('gl_FragDepth', 'g_depth').replace('gl_FragCoord', 'g_fragcoord').replace('discard;', '{ g_kill = true; return; }')
    t = re.sub(r'\bvoid\s+main\s*\(\s*\)', 'void shaderMain()', t)
    return t, ins, outs, inst

def ps_conv(typ):
    return {'float': '.x', 'vec2': '.xy', 'vec3': '.xyz', 'vec4': ''}[typ]

# ---------------------------------------------------------------- vertex
def vs_ref(spv_text, mp):
    t = strip_directives(spv_text)
    t = tex_consts(t, True)   # vertex textures (the game's fetch slots 16 and up) read the same fake functions as pixel ones
    t = re.sub(r'layout\([^)]*\)\s*uniform\s+(?:texture\w*|sampler\w*)\s+\w+\s*;', '', t)
    t = blocks_to_structs(t)
    t = re.sub(r'out\s+gl_PerVertex\s*\{[^}]*\}\s*;', '', t)
    t = re.sub(r'0\.00146484375 / float\(bitfieldExtract\([^;]*?\) \+ 1u\)', '0.0', t)   # no half-texel nudge in this comparison, as for pixel shaders
    t = re.sub(r'0\.00146484375 / (?:float\([^;]*?\)|[A-Za-z_]\w*)', '0.0', t)   # split into temporaries, as for pixel shaders
    outs = [(int(a), b) for a, b in re.findall(r'layout\(location = (\d+)\)\s*(?:invariant\s+)?out vec4 (\w+);', t)]
    t = re.sub(r'layout\(location = \d+\)\s*(?:invariant\s+)?out vec4 (\w+);', r'vec4 \1;', t)
    t = re.sub(r'invariant\s+gl_Position\s*;', '', t)
    t = t.replace('gl_VertexIndex', 'g_vi').replace('gl_Position', 'g_pos').replace('void main()', 'void shaderMain()')
    fm = re.search(r'float_constants\[(\d+)\]', t)
    nf = int(fm.group(1)) if fm else 0
    regs = list(range(nf)) if mp['dynamic'][0] else [r for r in range(256) if (mp['float'][r // 64] >> (r % 64)) & 1]
    S = 'xe_uniform_system_constants.'
    pre = ['  %sflags = 1024u;' % S, '  %svertex_index_endian = 0u;' % S, '  %sline_loop_closing_index = 0xFFFFFFFFu;' % S, '  %svertex_base_index = 0;' % S,
           '  %svertex_index_min = 0u;' % S, '  %svertex_index_max = 0xFFFFFFu;' % S, '  %sndc_scale = vec3(1.0);' % S, '  %sndc_offset = vec3(0.0);' % S,
           '  for (int k = 0; k < 2; ++k) %stexture_swizzled_signs[k] = uvec4(0u);' % S,
           '  for (int k = 0; k < 48; ++k) xe_uniform_fetch_constants.fetch_constants[k] = uvec4(3u, 0u, 3u, 0u);']
    for j, r in enumerate(regs[:nf]):
        pre.append('  xe_uniform_float_constants.float_constants[%d] = loadC(cs, %du);' % (j, r))
    for k in range(2):
        pre.append('  xe_uniform_bool_loop_constants.bool_constants[%d] = loadB(cs, %du);' % (k, k))
    for k in range(8):
        pre.append('  xe_uniform_bool_loop_constants.loop_constants[%d] = loadB(cs, %du);' % (k, k + 2))
    return t, pre, outs

def vs_mine(path, defs, attrs):
    t = preprocess(path, 'vert', defs)
    t = re.sub(r'layout\([^)]*\)\s*uniform\s+sampler2D\s+textures\s*\[\d+\]\s*;', 'const int textures[16] = int[16](16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31);', t)   # textures[i] in a vertex shader is the game's vertex fetch slot 16 + i
    t = blocks_to_structs(t)
    m = re.search(r'struct \w+_t \{.*?\};\s*\w+_t (\w+);', t, re.S)
    inst = m.group(1) if m else None
    ins = {int(a): (b, c) for a, b, c in re.findall(r'layout\(location = (\d+)\)\s*in\s+(float|vec2|vec3|vec4|int|uint|ivec\d|uvec\d)\s+(\w+)\s*;', t)}
    t = re.sub(r'layout\(location = \d+\)\s*in\s+((?:float|vec2|vec3|vec4|int|uint|ivec\d|uvec\d)\s+\w+\s*;)', r'\1', t)
    outs = [(int(a), b, c) for a, b, c in re.findall(r'layout\(location = (\d+)\)\s*out\s+(float|vec2|vec3|vec4)\s+(\w+)\s*;', t)]
    t = re.sub(r'layout\(location = \d+\)\s*out\s+((?:float|vec2|vec3|vec4)\s+\w+\s*;)', r'\1', t)
    t = re.sub(r'out\s+gl_PerVertex\s*\{[^}]*\}\s*;', '', t)
    t = t.replace('gl_VertexIndex', 'g_vi').replace('gl_Position', 'g_pos')
    t = re.sub(r'\bvoid\s+main\s*\(\s*\)', 'void shaderMain()', t)
    return t, ins, outs, inst

def build_vs(kind, text, pre, outs_ref, mine_info, S, entries):
    head = ('#version 450\n' + SPV_INTR + '#extension GL_EXT_scalar_block_layout : require\nlayout(local_size_x = 64) in;\n'
            + ('layout(std430, set = 0, binding = 0) readonly buffer InBuf { uint d[]; } inb;\n' if kind == 'mine' else '')
            + 'layout(std430, set = 0, binding = 1) readonly buffer ConstBuf { uint c[]; } cb;\n'
            'layout(std430, set = 0, binding = 2) writeonly buffer OutBuf { vec4 o[]; } outb;\n'
            'int g_vi = 0; vec4 g_pos = vec4(0.0); uint g_i = 0U;\n' + LOADERS + TO4 + TEX_PRELUDE)
    if kind == 'ref':
        text = text.replace('layout(set = 0, binding = 0, std430) readonly buffer XeSharedMemory', 'layout(std430, set = 0, binding = 0) readonly buffer XeSharedMemory')
    main = ['void main() {', '  g_i = gl_GlobalInvocationID.x;', '  uint cs = g_i / %dU;' % SET_INV, '  g_vi = int(g_i);']
    if kind == 'ref':
        main += pre + ['  %s = vec4(0.0);' % n for _, n in outs_ref] + ['  shaderMain();', '  outb.o[g_i * 17U] = g_pos;']
        for loc, n in outs_ref:
            main.append('  outb.o[g_i * 17U + %du] = %s;' % (loc + 1, n))
    else:
        outs = mine_info[2]
        main += mine_info[4] + ['  shaderMain();', '  outb.o[g_i * 17U] = g_pos;']
        for loc, typ, n in outs:
            main.append('  outb.o[g_i * 17U + %du] = to4(%s);' % (loc + 1, n))
    main.append('}')
    return head + text + '\n' + '\n'.join(main) + '\n'

# ---------------------------------------------------------------- inputs / compare
def rnd(rng, shape, special):
    """Finite inputs only: the game never sends infinities. special = 3% of the vectors are all zero (an unused light, a degenerate normal: the real hazards)."""
    a = rng.uniform(-2, 2, shape).astype(np.float32)
    nice = rng.choice(np.array([0.0, 0.5, 1.0, -1.0, 0.25, 2.0, 0.1], np.float32), size=shape)
    a = np.where(rng.random(shape) < 0.3, nice, a)
    if special:
        vectors = shape if shape[-1] == 4 else shape + (1,)
        zero = rng.random(shape[:-1] + (shape[-1] // 4,) if shape[-1] != 4 else shape[:-1]) < 0.03
        if shape[-1] != 4:
            zero = np.repeat(zero, 4, axis=-1)
            zero = np.pad(zero, [(0, 0)] * (zero.ndim - 1) + [(0, shape[-1] - zero.shape[-1])])
        else:
            zero = zero[..., None]
        a = np.where(zero, 0.0, a)
    return a.astype(np.float32)

def make_consts(rng, nsets, lits, special, loops=None, base=None, entries=(), bool_choices=None, bool_off=()):
    f = rnd(rng, (nsets, 256, 4), special)
    for e in entries:   # a constant with a real-world range (an exponent is never negative): "range": [lo, hi] on its JSON entry
        if 'range' in e:
            lo, hi = e['range']
            shape = (nsets, e.get('count', 1), 4)
            f[:, e['register']:e['register'] + e.get('count', 1), :] = np.where(rng.random(shape) < 0.05, np.float32(max(lo, 0.0)), rng.uniform(lo, hi, shape).astype(np.float32))
        if e.get('rect'):   # a texture rectangle (x, y) .. (z, w), never empty: "rect": true (S100: WrinkUV / MuscleUV, 1 / (zw - xy))
            shape = (nsets, e.get('count', 1))
            lo_x, lo_y = rng.uniform(0.0, 0.6, shape), rng.uniform(0.0, 0.6, shape)
            rect = np.stack([lo_x, lo_y, lo_x + rng.uniform(0.1, 0.4, shape), lo_y + rng.uniform(0.1, 0.4, shape)], axis=-1)
            f[:, e['register']:e['register'] + e.get('count', 1), :] = rect.astype(np.float32)
    for reg, l in zip(literal_registers(base, lits), lits):
        f[:, reg, :] = np.array(l, np.float32)
    b = rng.integers(0, 2 ** 32, (nsets, 8), dtype=np.uint64).astype(np.uint32)
    if bool_choices:   # only the combinations the game really sets (bone counts): "bool_choices": [[], [1], [1, 2], [1, 2, 3]]; every bit named there follows the choice
        named_bits = sorted({bit for choice in bool_choices for bit in choice})
        for s, pick in enumerate(rng.integers(0, len(bool_choices), nsets)):
            for bit in named_bits:
                b[s, bit >> 5] = int(b[s, bit >> 5]) & ~(1 << (bit & 31)) & 0xFFFFFFFF
            for bit in bool_choices[int(pick)]:
                b[s, bit >> 5] = int(b[s, bit >> 5]) | (1 << (bit & 31))
    for bit in bool_off or ():   # "bool_off": [4]: a bit the game never sets for these draws (seen in captures), always clear
        b[:, bit >> 5] &= np.uint32(~(1 << (bit & 31)) & 0xFFFFFFFF)
    loop = np.zeros((nsets, 32), np.uint32)
    for i, (cnt, start, step) in (loops or {}).items():
        loop[:, i] = (cnt & 0xFF) | ((start & 0xFF) << 8) | ((step & 0xFF) << 16)
    return np.concatenate([f.reshape(nsets, 1024).view(np.uint32), b, loop], axis=1).astype(np.uint32)

def unstable_inputs(ref, runner_cmd, inputs, consts, wd, tag, rng, keep=None, exact=(), mine=None):
    """Inputs where the game's own result moves by more than a quarter of the tolerance when every input moves by about three rounding errors (a division by a
    near-zero, a power of a near-cancelling sum). Nothing can be matched there, and in-game values are nowhere near them. Returns a bool per input."""
    fin, fc, outp = os.path.join(wd, tag + '_in2.bin'), os.path.join(wd, tag + '_const2.bin'), os.path.join(wd, tag + '_ref2.out')
    unstable = np.zeros(len(ref), bool)
    floats = consts[:, :1024].astype(np.uint32).view(np.float32)
    away = rng.uniform(0.5e-6, 1e-6, floats.shape).astype(np.float32)
    # An exact zero next to non-zero components of the same vector (a view straight up: x = z = 0) is a degenerate direction real data never
    # has; a multiplying nudge leaves it at zero, so it also moves by a small amount added (S100: hair half vectors went 0 / 0 there and were
    # never skipped). All-zero vectors stay exact: they test the zero-safe paths on purpose.
    partial = np.zeros(inputs.shape, bool)
    if inputs.ndim >= 2 and inputs[0].size % 4 == 0:   # vertex inputs are (n, S), pixel inputs (n, interpolators, 4)
        vec = inputs.reshape(len(inputs), -1, 4)
        partial = ((vec == 0) & (vec != 0).any(axis=-1, keepdims=True)).reshape(inputs.shape)
        if keep is not None and inputs.ndim == 2:
            partial &= np.asarray(keep)[None, :inputs.shape[1]]
    away_in = rng.uniform(0.5e-6, 1e-6, inputs.shape).astype(np.float32)
    for sign in (1.0, -1.0):   # both directions, so an exact tie flips in one of them
        nudge = 1.0 + rng.uniform(-4e-7, 4e-7, inputs.shape).astype(np.float32)
        if keep is not None:
            nudge = np.where(keep, nudge, 1.0)
        (inputs * nudge + np.where(partial, sign * away_in, 0.0)).astype(np.float32).tofile(fin)
        moved = consts.copy()
        # constants also move by a small amount added, so exact ties and 0 / 0 between constants (two equal random values, a zero sample offset) count as unstable
        moved[:, :1024] = (floats * (1.0 + rng.uniform(-4e-7, 4e-7, floats.shape).astype(np.float32)) + sign * away).astype(np.float32).view(np.uint32)
        for reg in exact:   # the shader's own literals are exact (a 3 that scales a bone index must stay 3)
            moved[:, reg * 4:reg * 4 + 4] = consts[:, reg * 4:reg * 4 + 4]
        moved.tofile(fc)
        cmd = list(runner_cmd); cmd[2] = outp
        run(cmd + [fin, fc], 'runner (nudged)')
        again = np.fromfile(outp, np.float32).reshape(ref.shape)
        with np.errstate(all='ignore'):
            fa, fb = np.where(np.isfinite(ref), np.abs(ref), 0.0), np.where(np.isfinite(again), np.abs(again), 0.0)
            e = np.abs(ref - again) / np.maximum(np.maximum(fa.max(axis=-1, keepdims=True), fb.max(axis=-1, keepdims=True)), 0.1)
        e = np.where(np.isfinite(e), e, 0.0)
        unstable |= (e > 2.5e-4).any(axis=(1, 2))
        if mine is not None:
            # Our side under the same nudge: a cusp where the game's rounding happens to hold still (a power of exactly 0 when view and normal
            # are exactly parallel, S100) is as unmatchable as one where it does not. Only finite results count, so a NaN of ours (a zero
            # vector normalised) is never excused.
            ours, ours_cmd = mine
            cmd = list(ours_cmd); cmd[2] = os.path.join(wd, tag + '_mine2.out')
            run(cmd + [fin, fc], 'runner (nudged, ours)')
            ours_again = np.fromfile(cmd[2], np.float32).reshape(ours.shape)
            with np.errstate(all='ignore'):
                fa, fb = np.where(np.isfinite(ours), np.abs(ours), 0.0), np.where(np.isfinite(ours_again), np.abs(ours_again), 0.0)
                em = np.abs(ours - ours_again) / np.maximum(np.maximum(fa.max(axis=-1, keepdims=True), fb.max(axis=-1, keepdims=True)), 0.1)
            finite = (np.isfinite(ours) & np.isfinite(ours_again)).all(axis=(1, 2))
            unstable |= (np.where(np.isfinite(em), em, 0.0) > 2.5e-4).any(axis=(1, 2)) & finite
    return unstable

def compare(a, b, names, kill=None):
    """a, b: (n, slots, 4) float32. Returns (worst err, slot name, index, ours, ref)."""
    nan = np.isnan(a) & np.isnan(b)
    same_inf = np.isinf(a) & np.isinf(b) & (np.sign(a) == np.sign(b))
    with np.errstate(all='ignore'):
        fa, fb = np.where(np.isfinite(a), np.abs(a), 0.0), np.where(np.isfinite(b), np.abs(b), 0.0)
        e = np.abs(a - b) / np.maximum(np.maximum(fa.max(axis=-1, keepdims=True), fb.max(axis=-1, keepdims=True)), 0.1)   # relative to the whole vector: a small channel next to a huge one is rounding
    e = np.where(nan | same_inf | ~np.isfinite(b), 0.0, e)   # where the game's own result is not a number there is nothing to match
    ours_only = ~np.isfinite(a) & np.isfinite(b)
    if ours_only.any(axis=(1, 2)).sum() <= max(2, len(a) // 2000):   # a chance 0 / 0 (two random constants equal), not a hazard: a real one hits about 3% of the inputs
        e = np.where(ours_only, 0.0, e)
    e = np.where(np.isnan(e), np.inf, e)
    if kill is not None:
        e = np.where(kill[:, None, None], 0.0, e)
    worst = (0.0, None, -1)
    for s, nm in enumerate(names):
        w = float(e[:, s].max()) if len(e) else 0.0
        if w > worst[0] or worst[1] is None:
            i = int(np.argmax(e[:, s].max(axis=1)))
            worst = (w, nm, i)
    return worst

def loop_values(var, mp):
    """Loop constants (aL): the JSON's optional variant field loop_constants {"index": [count, start, step]}; the containers hold
    no defaults (constant table lists none). A shader that uses a loop constant nobody gave a value for is a FAIL."""
    given = {int(k): tuple(int(x) for x in v) for k, v in var.get('loop_constants', {}).items()}
    need = [i for i in range(32) if (mp['loop'][0] >> i) & 1]
    miss = [i for i in need if i not in given]
    if miss:
        raise RuntimeError('the original has a loop (loop constant i%s) and the variant gives no value: add "loop_constants": {"%d": [count, start, step]} '
                           '(the container holds no default)' % (','.join(map(str, miss)), miss[0]))
    return given

def fmt(v):
    return '[' + ' '.join('%.6g' % x for x in v) + ']'

# ---------------------------------------------------------------- per-stage checks
def check_fragment(fxdir, effect, var, h, ucode, rng, n, special, verbose):
    kind, code, lits = ucode
    src = os.path.join(fxdir, effect + '.frag')
    key = '%016X' % h
    spv, mp = translate_many([(key, 'PS', code)])[key]
    loops = loop_values(var, mp)
    ref_text, fills_r, pre_r, outs_r = ps_ref(cross(spv), mp)
    mine_text, ins_m, outs_m, inst = ps_mine(src, var.get('defines', []), var)
    fills_m = [('  %s = loadV(%su)%s;' % (nm, loc, ps_conv(typ))) for loc, typ, nm in ins_m]
    pre_m = constant_fill(mine_text, inst, var.get('constants', []), 'cs') if inst else []
    types = {int(l): t for l, t, _ in outs_m}
    names_m = [(int(l), nm) for l, t, nm in outs_m]
    cvt_m = {nm: 'to4(%s)' % nm for _, nm in names_m}
    ref_src = build_ps('ref', ref_text, fills_r, pre_r, outs_r, lambda nm: nm)
    mine_src = build_ps('mine', mine_text, fills_m, pre_m, names_m, lambda nm: 'to4(%s)' % nm)
    # inputs
    n = (n // SET_INV) * SET_INV
    d = rnd(rng, (n, 16, 4), special)
    w_channel = rnd(rng, (n, 16), False)   # zero directions, never a zero w: an interpolated w can be a projective divisor, and a pixel never gets one of 0
    d[:, :, 3] = np.where(w_channel == 0.0, 1.0, w_channel)
    lit_first = literal_base(h, 'PS', code, lits)
    consts = make_consts(rng, n // SET_INV, lits, special, loops, lit_first, var.get('constants', []), var.get('bool_choices'), var.get('bool_off', ()))
    wd = os.path.join(H, 'work', str(os.getpid())); os.makedirs(wd, exist_ok=True)
    fin, fc = os.path.join(wd, 'ps_in.bin'), os.path.join(wd, 'ps_const.bin')
    d.tofile(fin); consts.tofile(fc)
    res = {}
    for tag, text in (('ref', ref_src), ('mine', mine_src)):
        spvc = compile_comp(text, 'ps_%s_%s_%s' % (effect, key, tag))
        if tag == 'ref':
            spvc_ref = spvc
        outp = os.path.join(wd, 'ps_%s.out' % tag)
        run([RUNNER, spvc, outp, str(n * 10 * 16), str(n // 64), fin, fc], 'runner (' + tag + ')')
        res[tag] = np.fromfile(outp, np.float32).reshape(n, 10, 4)
    kr, km = res['ref'][:, 0, 0] > 0.5, res['mine'][:, 0, 0] > 0.5
    locs_r = {l for l, _ in outs_r}; locs_m = {l for l, _ in names_m}
    problems = []
    if locs_r != locs_m:
        problems.append(('missing output', 'ref writes colour %s, ours %s' % (sorted(locs_r), sorted(locs_m))))
    kd = int((kr != km).sum())
    if kd:
        i = int(np.argmax(kr != km))
        problems.append(('kill', '%d inputs differ (first #%d: ours %s ref %s)' % (kd, i, bool(km[i]), bool(kr[i]))))
    both = ~(kr | km)
    sl = sorted(locs_r | locs_m)
    cols = [l + 1 for l in sl]
    names = ['colour%d' % l for l in sl]
    if (res['ref'][:, 9, 0] != -12345.0).any() or (res['mine'][:, 9, 0] != -12345.0).any():   # depth export (oDepth / gl_FragDepth), -12345 = not written
        cols.append(9); names.append('depth')
    A = res['mine'][:, cols, :][both]
    B = res['ref'][:, cols, :][both]
    ok_in = np.nonzero(both)[0]
    if FAST_REJECT and len(A) and quick_reject(A, B):
        return 9.0, 'colour0', [('mismatch', 'wrong on most inputs')], None, d, consts
    unstable = unstable_inputs(res['ref'], [RUNNER, spvc_ref, None, str(n * 10 * 16), str(n // 64)], d, consts, wd, 'ps', rng, None, literal_registers(lit_first, lits), mine=(res['mine'], [RUNNER, spvc, None, str(n * 10 * 16), str(n // 64)]))[both]
    if len(unstable) and unstable.mean() > 0.5:
        problems.append(('unstable', 'the proof would skip %.0f%% of the inputs, so it says nothing' % (100.0 * unstable.mean())))
    w, nm, i = compare(A, B, names, unstable) if len(A) else (0.0, 'colour0', 0)
    LAST.update(mine=A, ref=B, names=names, skip=unstable, rows=ok_in)
    info = None
    if w > 1e-3:
        gi = int(ok_in[i]); k = names.index(nm)
        info = (w, nm, gi, A[i][k], B[i][k])
    return w, nm, problems, info, d, consts

def check_vertex(fxdir, effect, var, h, ucode, rng, n, special, verbose):
    kind, code, lits = ucode
    src = os.path.join(fxdir, effect + '.vert')
    mine_text, ins, outs, inst = vs_mine(src, var.get('defines', []), var)
    attrs = var.get('attributes', [])
    nfetch = len(vfetch_indices(code))
    widths = {}
    for a in attrs:
        typ = ins.get(a['location'], (None,))[0]
        if typ is None:
            raise RuntimeError('attribute location %d is not declared as an input in the shader' % a['location'])
        if typ not in ('float', 'vec2', 'vec3', 'vec4'):
            raise RuntimeError('attribute location %d has type %s: only float / vec2 / vec3 / vec4 inputs are supported' % (a['location'], typ))
        if a['fetch'] >= nfetch:
            raise RuntimeError('attribute fetch %d, but the shader has only %d vfetch instructions' % (a['fetch'], nfetch))
        widths[a['fetch']] = {'float': 1, 'vec2': 2, 'vec3': 3, 'vec4': 4}[typ]
    pcode, nf = patch_vfetch(code, widths)
    S = 4 * max(nf, 1)
    key = '%016X_%s' % (h, '_'.join('%d%d' % (k, widths.get(k, 4)) for k in range(nf)))
    spv, mp = translate_many([(key, 'VS', pcode)])[key]
    loops = loop_values(var, mp)
    ref_text, pre_r, outs_r = vs_ref(cross(spv), mp)
    # ours: attribute fill
    fill = []
    for a in attrs:
        typ, name = ins[a['location']]
        sw = {'float': '.x', 'vec2': '.xy', 'vec3': '.xyz', 'vec4': ''}[typ]
        b = 'g_i * %du + %du' % (S, 4 * a['fetch'])
        fill.append('  %s = vec4(uintBitsToFloat(inb.d[%s]), uintBitsToFloat(inb.d[%s + 1U]), uintBitsToFloat(inb.d[%s + 2U]), uintBitsToFloat(inb.d[%s + 3U]))%s;' % (name, b, b, b, b, sw))
    pre_m = fill + (constant_fill(mine_text, inst, var.get('constants', []), 'cs') if inst else [])
    ref_src = build_vs('ref', ref_text, pre_r, outs_r, None, S, None)
    mine_src = build_vs('mine', mine_text, None, None, (mine_text, ins, outs, inst, pre_m), S, None)
    n = (n // SET_INV) * SET_INV
    data = rnd(rng, (n, S), special)
    for at in attrs:   # optional "range": [lo, hi] (+ "integer": true), see the header
        if 'range' in at:
            lo, hi = at['range']
            wdt = widths[at['fetch']]
            if at.get('integer'):
                v = np.floor(rng.uniform(lo, hi + 1, (n, wdt))).clip(lo, hi).astype(np.float32)
            else:
                v = rng.uniform(lo, hi, (n, wdt)).astype(np.float32)
            data[:, 4 * at['fetch']:4 * at['fetch'] + wdt] = v
    lit_first = literal_base(h, 'VS', code, lits)
    consts = make_consts(rng, n // SET_INV, lits, special, loops, lit_first, var.get('constants', []), var.get('bool_choices'), var.get('bool_off', ()))
    wd = os.path.join(H, 'work', str(os.getpid())); os.makedirs(wd, exist_ok=True)
    fin, fc = os.path.join(wd, 'vs_in.bin'), os.path.join(wd, 'vs_const.bin')
    data.tofile(fin); consts.tofile(fc)
    res = {}
    for tag, text in (('ref', ref_src), ('mine', mine_src)):
        spvc = compile_comp(text, 'vs_%s_%s_%s' % (effect, key, tag))
        if tag == 'ref':
            spvc_ref = spvc
        outp = os.path.join(wd, 'vs_%s.out' % tag)
        run([RUNNER, spvc, outp, str(n * 17 * 16), str(n // 64), fin, fc], 'runner (' + tag + ')')
        res[tag] = np.fromfile(outp, np.float32).reshape(n, 17, 4)
    names = ['position'] + ['interp%d' % i for i in range(16)]
    if FAST_REJECT and quick_reject(res['mine'], res['ref']):
        return 9.0, 'position', [('mismatch', 'wrong on most inputs')], None, data, consts
    keep = np.ones(S // 4 * 4, bool)
    for at in attrs:
        if at.get('integer'):
            keep[4 * at['fetch']:4 * at['fetch'] + 4] = False   # a bone index nudged below a whole number would pick another bone
    unstable = unstable_inputs(res['ref'], [RUNNER, spvc_ref, None, str(n * 17 * 16), str(n // 64)], data, consts, wd, 'vs', rng, keep, literal_registers(lit_first, lits), mine=(res['mine'], [RUNNER, spvc, None, str(n * 17 * 16), str(n // 64)]))
    w, nm, i = compare(res['mine'], res['ref'], names, unstable)
    LAST.update(mine=res['mine'], ref=res['ref'], names=names, skip=unstable, rows=np.arange(n))
    problems = []
    if unstable.mean() > 0.5:
        problems.append(('unstable', 'the proof would skip %.0f%% of the inputs, so it says nothing' % (100.0 * unstable.mean())))
    info = None
    if w > 1e-3:
        info = (w, nm, i, res['mine'][i][names.index(nm)], res['ref'][i][names.index(nm)])
    return w, nm, problems, info, data, consts

# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('effect')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--rounds', type=int, default=4, help='thousands of random inputs per hash (x 4096)')
    ap.add_argument('--hash', default=None, help='only this hash (hex)')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--root', default=None, help='folder holding <effect>.fx.json and the shaders (default src\\renderer\\shaders\\fx)')
    ap.add_argument('--no-special', action='store_true', help='no all-zero vectors in the random inputs')
    ap.add_argument('--inventory', default=os.path.join(REPO, 'db', 'shaders', 'inventory.json'))
    a = ap.parse_args()
    fxdir = a.root or os.path.join(REPO, 'src', 'renderer', 'shaders', 'fx')
    ensure_tools()
    inv = json.load(open(a.inventory))['effects']
    if a.effect not in inv:
        raise SystemExit('effect %s is not in the inventory' % a.effect)
    inv_h = {}
    for s in inv[a.effect]['hashes']:
        k, hx = s.split()[:2]
        inv_h[int(hx, 16)] = k
    jpath = os.path.join(fxdir, a.effect + '.fx.json')
    if not os.path.exists(jpath):
        raise SystemExit('missing %s' % jpath)
    spec = json.load(open(jpath))
    jobs = []   # (stage, variant, hash)
    covered = {}
    for stage, kind in (('vertex', 'VS'), ('fragment', 'PS')):
        for var in spec.get(stage, []):
            for hx in var.get('hashes', []):
                h = int(hx, 16)
                jobs.append((stage, var, h))
                covered[h] = covered.get(h, 0) + 1
    results = []
    for h, k in sorted(inv_h.items()):
        if a.hash and h != int(a.hash, 16):
            continue
        if h not in covered:
            results.append({'hash': '%016X' % h, 'kind': k, 'variant': None, 'pass': False, 'worst': None, 'output': 'missing', 'detail': 'in the inventory, in no variant'})
    ucodes = uc.get(a.effect, [h for _, _, h in jobs if not a.hash or h == int(a.hash, 16)])
    for stage, var, h in jobs:
        if a.hash and h != int(a.hash, 16):
            continue
        rng = np.random.default_rng(a.seed + (h & 0xFFFF))
        rec = {'hash': '%016X' % h, 'kind': 'VS' if stage == 'vertex' else 'PS', 'variant': var.get('name')}
        if h not in inv_h:
            rec.update(**{'pass': False, 'worst': None, 'output': 'unknown hash', 'detail': 'not in the effect\'s inventory'})
        elif h not in ucodes:
            rec.update(**{'pass': False, 'worst': None, 'output': 'no microcode', 'detail': 'hash not found in the game files'})
        elif covered[h] > 1:
            rec.update(**{'pass': False, 'worst': None, 'output': 'duplicate', 'detail': 'hash listed in more than one variant'})
        else:
            try:
                f = check_vertex if stage == 'vertex' else check_fragment
                w, nm, problems, info, _, _ = f(fxdir, var.get('source', a.effect), var, h, ucodes[h], rng, 4096 * a.rounds, not a.no_special, a.verbose)
                bad = bool(problems) or w > 1e-3
                out = nm
                det = ''
                if problems:
                    out, det = problems[0]
                elif info:
                    det = 'input #%d ours %s ref %s' % (info[2], fmt(info[3]), fmt(info[4]))
                rec.update(**{'pass': not bad, 'worst': w, 'output': out, 'detail': det})
            except Exception as e:
                rec.update(**{'pass': False, 'worst': None, 'output': 'error', 'detail': str(e)})
        results.append(rec)
    npass = sum(1 for r in results if r['pass'])
    for r in results:
        print('%s %s %-5s %-24s %s worst %s  %s%s' % ('PASS' if r['pass'] else 'FAIL', r['kind'], r['hash'], (r['variant'] or '-'), '', ('%.3e' % r['worst']) if r['worst'] is not None else '-', r['output'],
                                                     ('  ' + r['detail'].split('\n')[0][:300]) if (r['detail'] and (not r['pass'] or a.verbose)) else ''))
        if a.verbose and r['detail'] and '\n' in r['detail']:
            print(r['detail'])
    print('%s: %d of %d hashes pass' % (a.effect, npass, len(results)))
    os.makedirs(H, exist_ok=True)
    json.dump({'effect': a.effect, 'seed': a.seed, 'results': results, 'all_pass': npass == len(results) and bool(results)}, open(os.path.join(H, 'result_%s.json' % a.effect), 'w'), indent=1)
    sys.exit(0 if npass == len(results) and results else 1)

if __name__ == '__main__':
    main()

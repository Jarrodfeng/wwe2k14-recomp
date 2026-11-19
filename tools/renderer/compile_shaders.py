#!/usr/bin/env python3
"""Compiles our hand-written renderer shaders to optimised, validated SPIR-V headers in src\\renderer\\shaders\\spv\\.
Only out-of-date shaders are rebuilt. Vulkan 1.1 target (the floor).

  src\\renderer\\shaders\\*.vert / *.frag        one header each (kSpv_<name>_<stage>)
  src\\renderer\\shaders\\fx\\<effect>.fx.json    one header per effect, every variant, plus an alpha-test twin for each fragment variant
                                                  whose JSON does not say "alpha_test": false; and fx_registry.h, the table the binder reads

  py tools\\renderer\\compile_shaders.py [--force]
"""
import concurrent.futures
import functools
import glob
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SOURCE = os.path.join(ROOT, 'src', 'renderer', 'shaders')
FX = os.path.join(SOURCE, 'fx')
OUT = os.path.join(SOURCE, 'spv')
SDK_ROOT = r'C:\VulkanSDK'
SDK = os.path.join(SDK_ROOT, sorted(os.listdir(SDK_ROOT))[-1], 'Bin')


def run(tool, *args):
    done = subprocess.run([os.path.join(SDK, tool), *args], capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError(f'{tool} failed:\n{done.stdout}{done.stderr}')


# S103 GSP: a per-shader content-hash cache (out\shaders\cache; env SPV_CACHE_DIR moves it; SPV_CACHE=0 turns it off). A compile result is looked up by a key over everything that decides it: the script's
# own bytes, the glslang / spirv-opt / spirv-val versions (and the exes' size + mtime), the flags, the defines, the shader text and the content of every file it includes (transitively). Only a variant whose key
# changed recompiles; the words are still compared with the committed headers by write_if_changed, so --check notices a stale header exactly as before. CACHE_STATS is printed at the end.
# GSP2: the default cache folder is the MAIN checkout's (every worktree shares it: a cold run is ~270 s). That is safe only because an entry is believed only after it proves itself (see pack / unpack: key, length and sha256 over key + bytes are
# checked on every read) and is stored only when the inputs did not change while glslang ran (cached_spv / cached_preprocess re-key after the build).
def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        common = ''
    return os.path.dirname(common) if common else ROOT


CACHE_DIR = os.environ.get('SPV_CACHE_DIR') or os.path.join(main_checkout(), 'out', 'shaders', 'cache')
CACHE_ON = os.environ.get('SPV_CACHE', '1') != '0'
CACHE_STATS = {'hit': 0, 'miss': 0, 'pp_hit': 0, 'pp_miss': 0, 'unstored': 0, 'rejected': 0}
STATS_LOCK = threading.Lock()   # the compiles run on 12 threads: `+= 1` on a dict value is not atomic
ENTRY_MAGIC = b'SPVC1\n'
_HOOK_FIRED = []


def bump(name):
    with STATS_LOCK:
        CACHE_STATS[name] += 1


def twin_hook():
    """TEST ONLY (spv_cache_twin.py): SPV_TWIN_EDIT=<file>|<text> appends <text> to <file> once, between a shader's key and its compile: the planted "an include edited while the compile runs" race."""
    spec = os.environ.get('SPV_TWIN_EDIT')
    if not spec:
        return
    with STATS_LOCK:
        if _HOOK_FIRED:
            return
        _HOOK_FIRED.append(1)
    path, _, text = spec.partition('|')
    with open(path, 'a', encoding='utf-8', newline='') as f:
        f.write(text.replace('\\n', '\n'))
INCLUDE_ANY = re.compile(r'^[ \t]*#[ \t]*include[ \t]+["<]([^">]+)[">]', re.M)


@functools.lru_cache(maxsize=1)
def tool_stamp():
    parts = [hashlib.sha256(open(os.path.abspath(__file__), 'rb').read()).hexdigest()]
    for tool in ('glslangValidator.exe', 'spirv-opt.exe', 'spirv-val.exe'):
        exe = os.path.join(SDK, tool)
        st = os.stat(exe)
        done = subprocess.run([exe, '--version'], capture_output=True, text=True)
        parts.append(f'{tool}|{st.st_size}|{int(st.st_mtime)}|{done.stdout}{done.stderr}')
    return hashlib.sha256('\n'.join(parts).encode()).hexdigest()


def _include_files(text, seen):
    for name in INCLUDE_ANY.findall(text):
        for folder in (FX, SOURCE):
            candidate = os.path.join(folder, name)
            if os.path.exists(candidate):
                if candidate not in seen:
                    with open(candidate, encoding='utf-8') as f:
                        seen[candidate] = f.read()
                    _include_files(seen[candidate], seen)
                break


def content_key(kind, text, defines, extra=''):
    """Key of one glslang input: the tools, the kind, the flags, the defines, the text and every include it reaches (their paths and bytes)."""
    seen = {}
    _include_files(text, seen)
    h = hashlib.sha256()
    for part in (tool_stamp(), kind, extra, '\x00'.join(defines), text):
        h.update(part.encode('utf-8'))
        h.update(b'\x01')
    for path in sorted(seen):
        h.update(os.path.relpath(path, ROOT).replace('\\', '/').encode())
        h.update(b'\x02')
        h.update(seen[path].encode('utf-8'))
        h.update(b'\x01')
    return h.hexdigest()


def pack(key, data):
    """One cache entry: magic, length, sha256 over (key + bytes), bytes. An entry is bound to its key: a file copied from another key, a truncated or edited file, or a bare .spv dropped in is rejected on read."""
    return ENTRY_MAGIC + struct.pack('<Q', len(data)) + hashlib.sha256(key.encode() + data).digest() + data


def unpack(key, blob):
    n = len(ENTRY_MAGIC) + 8 + 32
    if len(blob) < n or not blob.startswith(ENTRY_MAGIC):
        return None
    (length,) = struct.unpack('<Q', blob[len(ENTRY_MAGIC):len(ENTRY_MAGIC) + 8])
    data = blob[n:]
    if length != len(data) or hashlib.sha256(key.encode() + data).digest() != blob[len(ENTRY_MAGIC) + 8:n]:
        return None
    return data


def cache_get(key, ext):
    if not CACHE_ON:
        return None
    try:
        with open(os.path.join(CACHE_DIR, key + ext), 'rb') as f:
            blob = f.read()
    except OSError:
        return None
    data = unpack(key, blob)
    if data is None:
        bump('rejected')   # a damaged / foreign entry is a miss, never a result
    return data


def cache_put(key, ext, data):
    if not CACHE_ON:
        return
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        tmp = os.path.join(CACHE_DIR, f'{key}{ext}.{os.getpid()}.{threading.get_ident()}.tmp')
        with open(tmp, 'wb') as f:
            f.write(pack(key, data))
        os.replace(tmp, os.path.join(CACHE_DIR, key + ext))
    except OSError:
        pass


def _spv_words(data):
    """The words of a cached SPIR-V file, or None when it is not one (a damaged cache entry is a miss, never a result)."""
    if len(data) % 4 or len(data) < 20:
        return None
    words = struct.unpack(f'<{len(data) // 4}I', data)
    return words if words[0] == 0x07230203 else None


def cached_spv(kind, text, defines, build, reread=None):
    """build() returns the optimised file's bytes; looked up first by content_key. After the build the key is computed again from what is on disk NOW (`reread()` gives the shader's current text; the includes are re-read by content_key):
    the entry is stored only when the key did not change while glslang ran (an edit in between would have stored the new SPIR-V under the old key)."""
    key = content_key(kind, text, defines)
    got = cache_get(key, '.spv')
    words = _spv_words(got) if got is not None else None
    if words is not None:
        bump('hit')
        return words
    bump('miss')
    twin_hook()
    data = build()
    if content_key(kind, reread() if reread else text, defines) == key:
        cache_put(key, '.spv', data)
    else:
        bump('unstored')
    return struct.unpack(f'<{len(data) // 4}I', data)


def cached_preprocess(args, defines, path, stage_flag=''):
    """glslang -E output, cached by the source file and its includes (stored only when they did not change while glslang ran)."""
    with open(path, encoding='utf-8') as f:
        text = f.read()
    key = content_key('E', text, defines, extra=stage_flag)
    got = cache_get(key, '.pp')
    if got is not None:
        bump('pp_hit')
        return got.decode('utf-8')
    bump('pp_miss')
    twin_hook()
    done = subprocess.run([os.path.join(SDK, 'glslangValidator.exe'), *args], capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError(f'preprocess failed:\n{done.stdout}{done.stderr}')
    with open(path, encoding='utf-8') as f:
        now = f.read()
    if content_key('E', now, defines, extra=stage_flag) == key:
        cache_put(key, '.pp', done.stdout.encode('utf-8'))
    else:
        bump('unstored')
    return done.stdout


def _build_spv(src, defines):
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'raw.spv')
        opt = os.path.join(tmp, 'opt.spv')
        run('glslangValidator.exe', '-V', '--target-env', 'vulkan1.1', '-I' + SOURCE, '-I' + FX, *[f'-D{d}' for d in defines], '-o', raw, src)
        run('spirv-opt.exe', '-O', '--strip-debug', '--target-env=vulkan1.1', '-o', opt, raw)
        run('spirv-val.exe', '--target-env', 'vulkan1.1', opt)
        with open(opt, 'rb') as f:
            return f.read()


def compile_words(path, defines):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    return cached_spv('file' + os.path.splitext(path)[1], text, defines, lambda: _build_spv(path, defines), reread=lambda: open(path, encoding='utf-8').read())


HEADER_INJECT = '#define main fx_user_main\n'
SYSTEM_BLOCK = 'layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;\n'


# Vertex constants the display may interpolate between two game ticks (DESIGN.md Display and decoupling; the frame pump): transforms and the
# positions of the camera, lights and planes. Colours, light directions and parameters are never blended: the newest tick's value is used.
BLEND_NAMED = {'EyePos', 'LightPos', 'LightEye', 'PlanePos', 'TraMovPos', 'PlaneInv', 'Trapezoidal', 'WorldPosArray'}
BLEND_PARTS = ('Matrix', 'ViewProj', 'World', 'Proj', 'View', 'WVP')


def blendable(field):
    return field in BLEND_NAMED or any(part in field for part in BLEND_PARTS)


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"[ \t]*$', re.M)


def inline_includes(text, depth=0):
    """Includes pasted in, so a rewrite of declarations sees the ones a family's include holds (S100: the character effects' bone indices live in
    fx_character.glsl, and their "integer" flags did nothing). Include guards stay for the preprocessor."""
    if depth > 16:
        raise RuntimeError('includes nest too deep')
    def paste(match):
        for folder in (FX, SOURCE):
            candidate = os.path.join(folder, match.group(1))
            if os.path.exists(candidate):
                with open(candidate, encoding='utf-8') as f:
                    return inline_includes(f.read(), depth + 1)
        raise RuntimeError(f'include not found: {match.group(1)}')
    return INCLUDE.sub(paste, text)


TEXTURE_DECL = re.compile(r'layout\s*\([^)]*\)\s*uniform\s+\w+\s+(?:textures|cubes|volumes)\s*\[\s*\d+\s*\]\s*;')
TEXTURE_USE = re.compile(r'\b(textures|cubes|volumes)\s*\[\s*([^\]]*?)\s*\]')
COMMENTS = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def bind_textures(body, stage):
    """Every game slot its own binding (set 1, binding = the game's slot; a vertex shader's textures[i] is slot 16 + i), declared as the type the
    shader samples it as. Authors write textures[i] / cubes[i] (the proof harness reads that form); the shipped shader never aliases a 2D and a
    cube array on one binding, which validation rejects and some mobile drivers get wrong (S100, rule 6: the 360's one slot space for every kind).
    Returns (declarations, body)."""
    body = TEXTURE_DECL.sub('', COMMENTS.sub('', body))
    base = 16 if stage == 'vert' else 0
    used = {}
    def use(match):
        kind, index = match.group(1), match.group(2)
        if not index.isdigit():
            raise RuntimeError(f'{kind}[{index}]: a texture slot picked by a variable cannot have its own binding; write the slots out')
        name = f'fx_{kind}_{index}'
        used[name] = ({'textures': 'sampler2D', 'cubes': 'samplerCube', 'volumes': 'sampler3D'}[kind], base + int(index))
        return name
    body = TEXTURE_USE.sub(use, body)
    return ''.join(f'layout(set = 1, binding = {b}) uniform {t} {n};\n' for n, (t, b) in sorted(used.items(), key=lambda u: u[1][1])), body


def top_insert(body, text):
    """After the #version, #extension and '#define main' lines, which must come first; before every #include (a family's include can hold the whole shader)."""
    lines = body.split('\n')
    last = max(i for i, line in enumerate(lines) if i == 0 or line.lstrip().startswith(('#extension', '#define main')))
    lines.insert(last + 1, text)
    return '\n'.join(lines)


def active_output(path, defines):
    """The colour output 0 of a fragment shader with these options, or None (depth only). Read from the preprocessed shader: a family's include
    declares outputs for several options (S100: searching the effect's own file missed them, and alpha-tested variants of those families shipped
    without their alpha test)."""
    out_text = cached_preprocess(['-E', '-S', 'frag', '-I' + SOURCE, '-I' + FX, *[f'-D{d}' for d in defines], path], list(defines), path, '-S frag')
    match = re.search(r'layout\s*\(\s*location\s*=\s*0\s*\)\s*out\s+vec4\s+(\w+)\s*;', out_text)
    return match.group(1) if match else None


def fx_target16_divisor(target_16):
    """lane 170: the value stored for a shader value v is v / divisor; 1.0 for every reader sign (see wrapped_source).
    Plants only: env FX_T16_DIVISOR (64 = the old rule back, 0.015625 = the old factor applied the other way). A set FX_T16_DIVISOR is
    shouted on stderr every time (review S2): a leftover variable in the shell must never bake a plant into a committed header."""
    plant = os.environ.get('FX_T16_DIVISOR')
    if plant is None:
        return 1.0
    sys.stderr.write('compile_shaders: WARNING FX_T16_DIVISOR=%s is set: the 16-bit target rule is a PLANT (lane 170); never commit headers built '
                     'with it\n' % plant)
    return float(plant)


def wrapped_source(path, stage, alpha_test, target_16, integer_locations=(), defines=()):
    """The real shader: the author's main() plus what the build adds after it. The proving harness compares the shader without this.
    integer_locations: vertex inputs that arrive as integers (bone indices). The author reads a float; the build declares the integer and casts."""
    with open(path, encoding='utf-8') as f:
        text = f.read()
    first_newline = text.index('\n') + 1
    body = text[:first_newline] + HEADER_INJECT + text[first_newline:] + '\n#undef main\n'
    if stage == 'vert':
        body = inline_includes(top_insert(body, 'invariant gl_Position;\n#define FX_TEXTURE_DECLARATIONS'))
        declarations, body = bind_textures(body, stage)
        body = body.replace('#define FX_TEXTURE_DECLARATIONS', declarations, 1)
        casts = {}
        def as_integer(match):
            location, kind, name = match.group(1), match.group(2), match.group(3)
            if int(location) not in integer_locations:
                return match.group(0)
            raw = {'float': 'uint', 'vec2': 'uvec2', 'vec3': 'uvec3', 'vec4': 'uvec4'}[kind]
            # The same name can be declared in several #if branches, even with different types: only the live branch defines the macros.
            casts[name] = f'#ifdef FX_HAS_{name}\n  {name} = FX_CAST_{name}(fx_raw_{name});\n#endif\n'
            return f'layout(location = {location}) in {raw} fx_raw_{name};\n{kind} {name};\n#define FX_HAS_{name}\n#define FX_CAST_{name} {kind}'
        body = re.sub(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(float|vec[234])\s+(\w+)\s*;', as_integer, body)
        # Every interpolator is 4 wide on both sides (the pixel side below): a vertex shader of one effect may feed a pixel shader of another, and
        # without maintenance4 the widths must match exactly (S100: yRect's vec4 into ySprite's vec2). The author writes the components it declared.
        widened = {}
        def widen_out(match):
            location, qualifier, kind, name = match.group(1), match.group(2) or '', match.group(3), match.group(4)
            pad = {'float': ', 0.0, 0.0, 0.0', 'vec2': ', 0.0, 0.0', 'vec3': ', 0.0'}[kind]
            widened[name] = f'#ifdef FX_OUT_{name}\n  fx_out_{name} = FX_WIDEN_{name}({name});\n#endif\n'
            return f'layout(location = {location}) {qualifier}out vec4 fx_out_{name};\n{kind} {name};\n#define FX_OUT_{name}\n#define FX_WIDEN_{name}(v) vec4(v{pad})'
        body = re.sub(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*((?:flat|smooth|noperspective)\s+)?out\s+(float|vec2|vec3)\s+(\w+)\s*;', widen_out, body)
        if 'VELOCITY' in defines:
            # Settings parity S10b (DLSS, the wrestlers' own motion): the constant block becomes a struct, bound twice (this tick's at binding 0,
            # the previous tick's at binding 1, the same std140 layout), copied into a global `vc` before each run of the author's main.
            block = re.compile(r'layout\s*\(\s*set\s*=\s*0\s*,\s*binding\s*=\s*0\s*,\s*std140\s*\)\s*uniform\s+VertexConstants\s*\{(.*?)\}\s*vc\s*;', re.S)
            if not block.search(body):
                raise RuntimeError('VELOCITY: no VertexConstants block named vc')
            body = block.sub(lambda m: 'struct FxVc {' + m.group(1) + '};\n'
                             'layout(set = 0, binding = 0, std140) uniform VertexConstants { FxVc d; } fx_vc_now;\n'
                             'layout(set = 0, binding = 1, std140) uniform PrevVertexConstants { FxVc d; } fx_vc_prev;\n'
                             'FxVc vc;\n', body, count=1)
            body += 'layout(location = 14) out vec4 fx_vel_now;\nlayout(location = 15) out vec4 fx_vel_prev;\n'
            body += SYSTEM_BLOCK + 'void main() {\n' + ''.join(casts.values())
            body += '  vc = fx_vc_prev.d;\n  fx_user_main();\n  fx_vel_prev = gl_Position;\n'
            body += '  vc = fx_vc_now.d;\n  fx_user_main();\n  fx_vel_now = gl_Position;\n' + ''.join(widened.values())
        else:
            body += SYSTEM_BLOCK + 'void main() {\n' + ''.join(casts.values()) + '  fx_user_main();\n' + ''.join(widened.values())
        body += '  gl_Position = vec4(gl_Position.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * gl_Position.w, gl_Position.w);\n}\n'
        return body
    body = inline_includes(top_insert(body, '#define FX_TEXTURE_DECLARATIONS'))
    declarations, body = bind_textures(body, stage)
    body = body.replace('#define FX_TEXTURE_DECLARATIONS', declarations, 1)
    # Every pixel-stage input is 4 wide: a vertex shader of one effect may feed a pixel shader of another, and an output wider than its input is
    # invalid without maintenance4 (S100: yRect's vec4 into ySprite's vec2). The author's name reads the components it declared.
    def widen(match):
        location, qualifier, kind, name = match.group(1), match.group(2) or '', match.group(3), match.group(4)
        swizzle = {'float': 'x', 'vec2': 'xy', 'vec3': 'xyz'}[kind]
        return f'layout(location = {location}) {qualifier}in vec4 fx_in_{name};\n#define {name} fx_in_{name}.{swizzle}'
    body = re.sub(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*((?:flat|smooth|noperspective)\s+)?in\s+(float|vec2|vec3)\s+(\w+)\s*;', widen, body)
    out = active_output(path, defines) if (alpha_test or target_16) else None
    if not out:
        return body + 'void main() {\n  fx_user_main();\n}\n'   # no colour output (a depth-only shader) or nothing to add
    body += SYSTEM_BLOCK + 'void main() {\n  fx_user_main();\n'
    if alpha_test:
        # The raw alpha, before anything else (GAME_RULES_DRAWS A1-A5). NaN passes only "always" and "not equal" (A3).
        body += f'  float fx_a = {out}.a;\n  int fx_func = int(fx_sys.alpha.y);\n  bool fx_pass;\n  if (fx_func == 7) fx_pass = true;\n'
        body += '  else if (isnan(fx_a)) fx_pass = fx_func == 5;\n'
        body += '  else fx_pass = (fx_a < fx_sys.alpha.x && (fx_func & 1) != 0) || (fx_a == fx_sys.alpha.x && (fx_func & 2) != 0) || (fx_a > fx_sys.alpha.x && (fx_func & 4) != 0);\n'
        body += '  if (!fx_pass) discard;\n'
    if target_16:
        # What the game's readers would have fetched (GAME_RULES_DRAWS O4): NaN to 0, -32..32, then the divisor for how readers take it.
        # lane 170 (CARRY_DARKENING.md section 12): NO divisor any more. This used to divide by 32 ('signed') or 64 ('unsigned'): the 360's fixed-point storage (v / 32
        # in 16 bits, read back as UNORM: v / 64), which our float targets do not have and the readers' arithmetic (written in the game's value terms) never undoes. It left
        # every wrestler shadow map ~64x too faint, and, because the divisor was baked into the fragment variant, it also hit the draws of the SAME variant that fill an
        # 8-bit target (the props map). Our storage holds the value the shader wrote (clamped to the 360's range); readers get it back whatever the fetch's sign.
        # fx_target16_divisor() is the one place the rule lives; the plant (env FX_T16_DIVISOR, selftest only) puts the old factor back.
        divisor = fx_target16_divisor(target_16)
        body += f'  {out} = clamp(mix({out}, vec4(0.0), isnan({out})), vec4(-32.0), vec4(32.0)) / {divisor};\n'
    return body + '}\n'


def block_layout(path, defines, block):
    """std140 offsets of the constant block's members, in the order the shader declares them. Every member is vec4 or a vec4 array, or the Bools pair."""
    out_text = cached_preprocess(['-E', '-I' + SOURCE, '-I' + FX, *[f'-D{d}' for d in defines], path], list(defines), path)
    match = re.search(block + r'\s*\{(.*?)\}', out_text, re.S)
    offsets = {}
    size = 0
    if match:
        for kind, name, count in re.findall(r'(u?vec4)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;', match.group(1)):
            elements = int(count) if count else 1
            offsets[name] = size
            size += 16 * elements
    return offsets, size


def _build_text(text, defines, stage):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'shader.' + stage)
        with open(src, 'w', encoding='utf-8') as f:
            f.write(text)
        return _build_spv(src, defines)


def compile_text(text, defines, stage):
    return cached_spv('text:' + stage, text, defines, lambda: _build_text(text, defines, stage))


STALE = []   # S102 R18-B2: --check mode: the headers that would change (nothing is written)


def write_if_changed(path, text):
    # S101: write a generated header only when its content changed (line endings aside: git checks them out CRLF). Rewriting all of them
    # on every run dirtied the tree and made two runs at once collide on the files (gate flake, 'Invalid argument', twice on 2026-11-11).
    try:
        with open(path, encoding='utf-8', newline='') as f:
            if f.read().replace('\r\n', '\n') == text:
                return False
    except OSError:
        pass
    if '--check' in sys.argv:   # S102 R18-B2: the headers are current when a compile changes none of them
        STALE.append(os.path.basename(path))
        return True
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    return True


def array_text(name, words):
    lines = [', '.join(f'0x{w:08x}' for w in words[i:i + 8]) for i in range(0, len(words), 8)]
    return f'inline constexpr uint32_t {name}[] = {{\n' + ',\n'.join(lines) + '\n};\n'


def compile_plain(path, defines=(), suffix=''):
    name = os.path.basename(path).replace('.', '_') + suffix
    words = compile_words(path, list(defines))
    write_if_changed(os.path.join(OUT, name + '.h'), f'// Generated from {os.path.basename(path)} by tools\\renderer\\compile_shaders.py. Our own shader.\n#pragma once\n#include <cstdint>\n' + array_text(f'kSpv_{name}', words))


# S102 lane L1 (docs\renderer\PRELIGHT_DIRECT.md section 8): the character effects whose pixel stage samples the pre-light cube at slot 13. Each such fragment variant gets a TWIN
# `<name>_pl` (FX_PRELIGHT_ANALYTIC: the four lights evaluated per pixel, fx_prelight.glsl): no hashes (only the binder picks it, by name), the cube sampler of slot 13 removed, and the
# block member PreLightK (18 vec4) added as a constant range from register ANALYTIC_REGISTER (a register no game shader of these effects uses: the binder overwrites the range with the
# bake's own constants; what the bank holds there is never read).
ANALYTIC_EFFECTS = ('yCh_skin', 'yCh_skin_flex', 'yCh_gear', 'yCh_gear_flex', 'yCh_trans', 'yCh_Aniso_trans', 'yCh_TransTiling', 'yCh_cloth_trans', 'yCh_Anisotropic', 'yCh_Reflect', 'yCh_cloth', 'yCh_cloth_flex')
ANALYTIC_REGISTER = 232
ANALYTIC_COUNT = 18


def analytic_twins(variants):
    twins = []
    for v in variants:
        if not any(s.get('slot') == 13 and s.get('kind') == 'cube' for s in v.get('samplers', [])):
            continue
        twins.append({**v, 'name': v['name'] + '_pl', 'defines': list(v.get('defines', [])) + ['FX_PRELIGHT_ANALYTIC'], 'hashes': [],
                      'constants': list(v.get('constants', [])) + [{'field': 'PreLightK', 'register': ANALYTIC_REGISTER, 'count': ANALYTIC_COUNT}],
                      'samplers': [s for s in v.get('samplers', []) if s.get('slot') != 13]})
    return twins


# S102 lane R18-B2 (docs/renderer/COMBINE_LITE.md): the owned combine's lite twins, `<name>_lite1` (FX_LITE: the exact early-outs) and `<name>_lite2` (+ FX_LITE_FXAA: the flat-pixel exit, a LOOK
# lever), for every combine permutation but the luminance pass (FX_ADAPT; `base` has nothing for level 1 to skip). No hashes (only the post pass picks them, by name). The planted twins
# and the tool-only mark twin exist for the permutations the captures use (tools/r18/r18_b2_check.py): litep1 = the DoF blur never fetched, litep2 = the bloom never fetched, litep3 = the flat
# threshold 10x, litem = the pixels the flat exit takes drawn magenta (an instrument: the share of flat pixels). The existing variants are untouched.
LITE_EFFECT = 'yGlareEtc'
LITE_PLANT_SET = ('dof', 'dof_fog', 'dof_bloom', 'dof_fog_bloom', 'bloom', 'fog_bloom')


# S103 SLV2 (docs/renderer/DEAD_WORK.md, the depth-convert fold): the `_vd` twins of the two shaders that read the game's depth in the viewer's scenes (the combine yGlareEtc, slot 3, with FX_DOF or FX_FOG;
# the DoF downsample yDownScale DOF, slot 1). FX_VDEPTH folds rdr_post_depth.frag's conversion into the depth fetch (gameDepth, the same expression): the raw 0..1 depth is bound and the converted depth image
# (the R32F dconv_ pass) is not made. One constant range VDepthK = (q, nq, 9.92) at VD_REGISTER (no game shader of these effects reads it). No hashes (only the post pass picks them, by name). The combine's
# `_vd` bases get the lite / plant twins too (lite_twins), named `<stem>_vd_lite1` etc., so CombineLiteName(base = `..._vd_fragment`) composes the suffixes unchanged.
VD_EFFECTS = {'yGlareEtc': ('FX_DOF', 'FX_FOG'), 'yDownScale': ('DOF',)}
VD_REGISTER = 40


def vd_twins(effect, variants):
    twins = []
    for v in variants:
        d = list(v.get('defines', []))
        if 'FX_ADAPT' in d or not any(x in d for x in VD_EFFECTS.get(effect, ())):
            continue
        twins.append({**v, 'name': v['name'] + '_vd', 'defines': d + ['FX_VDEPTH'], 'hashes': [], 'alpha_test': False,
                      'constants': list(v.get('constants', [])) + [{'field': 'VDepthK', 'register': VD_REGISTER, 'count': 1}]})
    return twins


def lite_twins(variants):
    twins = []
    for v in variants:
        d = list(v.get('defines', []))
        if 'FX_ADAPT' in d:
            continue
        base = {**v, 'hashes': [], 'alpha_test': False}

        def add(suffix, extra):
            twins.append({**base, 'name': v['name'] + suffix, 'defines': d + extra})
        if 'FX_DOF' in d or 'FX_BLOOM' in d:   # review F6: level 1 changes only the DoF / bloom fetches; any other permutation's _lite1 would be word-for-word its base and counted as "taken"
            add('_lite1', ['FX_LITE'])
        add('_lite2', ['FX_LITE', 'FX_LITE_FXAA'])
        add('_litem', ['FX_LITE', 'FX_LITE_FXAA', 'FX_LITE_MARK'])   # the mark instrument exists for EVERY permutation (wm30's flat share read 0 with the first, smaller set)
        if (v['name'][:-3] if v['name'].endswith('_vd') else v['name']) in LITE_PLANT_SET:
            if 'FX_DOF' in d:
                add('_litep1', ['FX_LITE', 'FX_PLANT_DOF'])
            if 'FX_BLOOM' in d:
                add('_litep2', ['FX_LITE', 'FX_PLANT_BLOOM'])
            add('_litep3', ['FX_LITE', 'FX_LITE_FXAA', 'FX_PLANT_WIDE'])
    return twins


def spirv_samples(words):
    """Static count of the image sample instructions (OpImageSampleImplicitLod 87, ExplicitLod 88, OpImageFetch 95) in a SPIR-V word list."""
    n, i = 0, 5
    while i < len(words):
        op, count = words[i] & 0xFFFF, words[i] >> 16
        if count == 0:
            break
        n += op in (87, 88, 95)
        i += count
    return n


def compile_effect(json_path):
    """One header per effect. Returns the registry rows for it."""
    with open(json_path, encoding='utf-8') as f:
        spec = json.load(f)
    effect = spec['effect']
    def source_of(variant, extension):   # a variant may reuse another effect's shader ("source", written by solve_cross.py)
        return os.path.join(FX, variant.get('source', effect) + extension)
    text = [f'// Generated from {effect}.fx.json by tools\\renderer\\compile_shaders.py. Our own shaders.\n#pragma once\n#include <cstdint>\n']
    rows = {'effect': effect, 'vertex': [], 'fragment': []}
    vertex_variants = list(spec.get('vertex', []))
    if effect.startswith('yCh_'):  # S10b: the wrestlers' velocity twins
        vertex_variants += [{**v, 'name': v['name'] + '_velocity', 'defines': list(v.get('defines', [])) + ['VELOCITY'], 'hashes': []}
                            for v in spec.get('vertex', [])]
    for variant in vertex_variants:
        vert = source_of(variant, '.vert')
        symbol = f'kFx_{effect}_{variant["name"]}_vert'
        defines = variant.get('defines', [])
        integer_locations = {a['location'] for a in variant.get('attributes', []) if a.get('integer')}
        text.append(array_text(symbol, compile_text(wrapped_source(vert, 'vert', False, None, integer_locations, defines=defines), defines, 'vert')))
        offsets, size = block_layout(vert, defines, 'VertexConstants')
        rows['vertex'].append({**variant, 'symbol': symbol, 'offsets': offsets, 'block_bytes': size})
    fragment_variants = list(spec.get('fragment', []))
    if effect in ANALYTIC_EFFECTS:   # S102 L1: the analytic pre-light twins (fx_prelight.glsl), one per variant that samples the light cube at slot 13
        fragment_variants += analytic_twins(fragment_variants)
    vd = vd_twins(effect, fragment_variants)   # S103 SLV2: the depth-convert fold twins (their lite / plant twins below)
    if effect == LITE_EFFECT:   # S102 R18-B2
        fragment_variants += lite_twins(fragment_variants) + vd + lite_twins(vd)
    else:
        fragment_variants += vd
    for variant in fragment_variants:
        frag = source_of(variant, '.frag')
        symbol = f'kFx_{effect}_{variant["name"]}_frag'
        defines = variant.get('defines', [])
        target_16 = variant.get('target_16')
        words = compile_text(wrapped_source(frag, 'frag', False, target_16, defines=defines), defines, 'frag')
        text.append(array_text(symbol, words))
        if effect == LITE_EFFECT:   # S102 R18-B2: the static sample count per variant, printed (read by tools/r18/r18_b2_check.py)
            print(f'[fetches] {effect}_{variant["name"]}: {spirv_samples(words)} image samples (static)')
        offsets, size = block_layout(frag, defines, 'FragmentConstants')
        row = {**variant, 'symbol': symbol, 'symbol_alpha': None, 'offsets': offsets, 'block_bytes': size}
        if variant.get('alpha_test', True):
            row['symbol_alpha'] = symbol + '_alpha'
            text.append(array_text(row['symbol_alpha'], compile_text(wrapped_source(frag, 'frag', True, target_16, defines=defines), defines, 'frag')))
        rows['fragment'].append(row)
    write_if_changed(os.path.join(OUT, f'fx_{effect}.h'), '\n'.join(text))
    return rows


# S102 G10 (review F2): the OUTPUT-ALPHA SOURCE of a variant, emitted into the registry so the binder can prove a draw opaque. out_alpha = 1 when the colour output's alpha is
# MdlAlpha.x * MatAmbCol.w * MatDifCol.w (vertex constants 185 / 189 / 190) times the vertex colour's alpha and nothing else: no texture alpha (FX_ALPHA_OUT, FX_OVERLAY, FX_SCROLL,
# FX_CAA), no edge or spec fade (FX_EDGE_FADE, FX_SPEC_FADE). Read from src\renderer\shaders\fx\fx_bg.glsl (vertex: `alpha = vc.MdlAlpha.x * vc.MatAmbCol.w * vc.MatDifCol.w`, outColor.w =
# alpha * inColor.w; fragment: outColor.w = inColor.w, or diffuse.w * inColor.w under FX_ALPHA_OUT). Only the effects whose alpha lines were read are listed. vc_fetch = the fetch feeding
# the vertex colour (`layout(location = 2) in vec4 inColor`), 255 = none.
ALPHA_EFFECTS = ('yBG_DefaultLM', 'yBG_Default', 'yBG_UVScrollLM', 'yBG_UVScroll', 'yBG_BumpMapLM', 'yBG_BumpMap')
ALPHA_BREAKS = {'vertex': {'FX_EDGE_FADE', 'FX_SPEC_FADE'}, 'fragment': {'FX_ALPHA_OUT', 'FX_OVERLAY', 'FX_SCROLL', 'FX_CAA'}}


def source_defines(effect, v, stage):
    """S102 C4 (review F8): the macros the variant's own source file #defines (and its #include "...glsl" files, one level): FX_SCROLL is defined INSIDE the UVScroll fragments, not in the variant's define list."""
    import re
    path = os.path.join(FX, v.get('source', effect) + ('.frag' if stage == 'fragment' else '.vert'))
    found = set()
    try:
        text = open(path, encoding='utf-8').read()
    except OSError:
        return {'<unreadable source>'}
    for m in re.finditer(r'^\s*#\s*define\s+(\w+)', text, re.M):
        found.add(m.group(1))
    for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
        try:
            for m in re.finditer(r'^\s*#\s*define\s+(\w+)', open(os.path.join(FX, inc), encoding='utf-8').read(), re.M):
                found.add(m.group(1))
        except OSError:
            pass
    return found


def alpha_meta(effect, stage, v):
    if effect not in ALPHA_EFFECTS or (set(v.get('defines', [])) | source_defines(effect, v, stage)) & ALPHA_BREAKS[stage]:
        return 0, 255
    if stage == 'fragment':
        # fx_bg.glsl bg_fragment_main (yBG_Default*; the yBG_UVScroll* fragments define FX_SCROLL inside their own source and are in the break list, review F8): outColor.w = inColor.w under FX_ALPHA_SPEC, else diffuse.w * inColor.w (the TEXTURE's alpha: out_alpha 2 below);
        # the BumpMap pixel stage: inColor.w, or diffuse.w * inColor.w under FX_ALPHA_OUT (already in ALPHA_BREAKS).
        if effect.startswith('yBG_BumpMap'):
            return 1, 255
        # S102 C4: out_alpha 2 = the plain light-mapped / default fragments: outColor.w = diffuse.w * inColor.w, diffuse = textures[0] (fx_bg.glsl line ~620-640): provable when the draw's slot-0 texture has no alpha
        # (binder.h AlphaOpaque: the slot's swizzle / format / content scan). The break list (FX_ALPHA_OUT, FX_OVERLAY, FX_SCROLL, FX_CAA) already left the variants that blend a second texture's alpha in.
        if 'FX_ALPHA_SPEC' in (set(v.get('defines', [])) | source_defines(effect, v, stage)):
            return 1, 255
        assert not ((set(v.get('defines', [])) | source_defines(effect, v, stage)) & ALPHA_BREAKS[stage]), 'out_alpha 2 variant with a break-list define: ' + v.get('name', '?')  # review F8 self-check
        return 2, 255
    regs = {c['register'] for c in v.get('constants', []) if c['field'] in v['offsets']}
    fetch = [a['fetch'] for a in v.get('attributes', []) if a['location'] == 2]
    if not {185, 189, 190} <= regs or not fetch:
        return 0, 255
    return 1, fetch[0]


def write_registry(all_rows):
    """A table of every variant: game hash -> spirv, constant ranges, attributes, samplers."""
    out = ['// Generated by tools\\renderer\\compile_shaders.py. Hashes and our own shaders only.', '#pragma once', '#include <cstdint>', '#include <cstddef>']
    for rows in all_rows:
        out.append(f'#include "fx_{rows["effect"]}.h"')
    out += ['', 'namespace rdr::fx {', 'struct ConstantRange { uint16_t reg, count, offset; uint8_t blend; };', 'struct Attribute { uint8_t fetch, location, integer; };',
            'struct Sampler { uint8_t slot, kind; };',
            'struct Variant { const char* name; const uint32_t* spirv; size_t bytes; const uint32_t* spirv_alpha; size_t bytes_alpha; const ConstantRange* constants; uint16_t constant_count; uint16_t block_bytes; uint16_t bools_offset; const Attribute* attributes; uint16_t attribute_count; const Sampler* samplers; uint16_t sampler_count; uint8_t out_alpha; uint8_t vc_fetch; };',
            'struct HashEntry { uint64_t hash; uint32_t variant; };', '']
    variants, hashes = [], []
    for rows in all_rows:
        for stage in ('vertex', 'fragment'):
            for v in rows[stage]:
                index = len(variants)
                ident = f'{rows["effect"]}_{v["name"]}_{stage}'
                ranges = []
                for c in v.get('constants', []):
                    if c['field'] in v['offsets']:  # a member this variant's defines compile out is simply not copied
                        blend = 1 if stage == 'vertex' and blendable(c['field']) else 0
                        ranges.append(f'{{{c["register"]}, {c["count"]}, {v["offsets"][c["field"]]}, {blend}}}')
                if ranges:
                    out.append(f'inline constexpr ConstantRange kC_{ident}[] = {{{", ".join(ranges)}}};')
                bools = v['offsets'].get('Bools', 0xFFFF)
                kinds = {'2d': 0, 'cube': 1, 'volume': 2, '3d': 2}
                samplers = [f'{{{s["slot"]}, {kinds.get(s.get("kind", "2d"), 0)}}}' for s in v.get('samplers', [])]
                if samplers:
                    out.append(f'inline constexpr Sampler kS_{ident}[] = {{{", ".join(samplers)}}};')
                attrs = [f'{{{a["fetch"]}, {a["location"]}, {1 if a.get("integer") else 0}}}' for a in v.get('attributes', [])]
                if attrs:
                    out.append(f'inline constexpr Attribute kA_{ident}[] = {{{", ".join(attrs)}}};')
                alpha = v.get('symbol_alpha')
                out_alpha, vc_fetch = alpha_meta(rows['effect'], stage, v)
                variants.append('{"%s", %s, sizeof(%s), %s, %s, %s, %d, %d, %d, %s, %d, %s, %d, %d, %d}' % (
                    ident, v['symbol'], v['symbol'], alpha or 'nullptr', f'sizeof({alpha})' if alpha else '0',
                    f'kC_{ident}' if ranges else 'nullptr', len(ranges), v['block_bytes'], bools, f'kA_{ident}' if attrs else 'nullptr', len(attrs),
                    f'kS_{ident}' if samplers else 'nullptr', len(samplers), out_alpha, vc_fetch))
                for h in v['hashes']:
                    hashes.append((int(h, 16), index))
    hashes.sort()
    out.append('inline constexpr Variant kVariants[] = {\n  ' + ',\n  '.join(variants or ['{}']) + '\n};')
    out.append('inline constexpr HashEntry kHashes[] = {\n  ' + ',\n  '.join(f'{{0x{h:016X}ull, {i}}}' for h, i in hashes or [(0, 0)]) + '\n};')
    biggest = max([v['block_bytes'] for rows in all_rows for v in rows['fragment']] + [16])
    out.append(f'inline constexpr uint32_t kMaxFragmentBlockBytes = {biggest};   // the largest packed pixel block of any variant (lane 151: the replace seam\'s lazily made buffer)')
    out.append('}  // namespace rdr::fx\n')
    write_if_changed(os.path.join(OUT, 'fx_registry.h'), '\n'.join(out))


def main():
    os.makedirs(OUT, exist_ok=True)
    force = '--force' in sys.argv or '--check' in sys.argv   # S102 R18-B2 review F5: --check recompiles everything (a header newer than its source hid a stale one)
    newest_include = max([os.path.getmtime(p) for p in glob.glob(os.path.join(FX, '*.glsl')) + glob.glob(os.path.join(SOURCE, '*.glsl'))], default=0)

    plain = sorted(glob.glob(os.path.join(SOURCE, '*.vert')) + glob.glob(os.path.join(SOURCE, '*.frag')))
    plain_todo = []
    for path in plain:
        header = os.path.join(OUT, os.path.basename(path).replace('.', '_') + '.h')
        if force or not os.path.exists(header) or os.path.getmtime(header) < max(os.path.getmtime(path), newest_include):
            plain_todo.append(path)

    # S102 lane L1b (docs\renderer\PRELIGHT_DIRECT.md section 9): the light-cube compute shader (rdr_skin_once.comp is built by tools\shaders\build_skin_once.py, not here) and its planted twin (the faces swapped)
    for comp, defines, suffix in (('rdr_light_cubes.comp', (), ''), ('rdr_light_dirs.comp', (), ''), ('rdr_light_dirs.comp', ('FACE_SWAP',), '_swap')):
        path = os.path.join(SOURCE, comp)
        header = os.path.join(OUT, comp.replace('.', '_') + suffix + '.h')
        if force or not os.path.exists(header) or os.path.getmtime(header) < max(os.path.getmtime(path), newest_include):
            plain_todo.append((path, defines, suffix))
    effects = sorted(glob.glob(os.path.join(FX, '*.fx.json')))
    failed = 0
    all_rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        for path, future in [((p[0] if isinstance(p, tuple) else p), pool.submit(compile_plain, *(p if isinstance(p, tuple) else (p,)))) for p in plain_todo]:
            try:
                future.result()
            except RuntimeError as error:
                failed += 1
                print(f'FAILED {os.path.basename(path)}\n{error}')
        for path, future in [(p, pool.submit(compile_effect, p)) for p in effects]:
            try:
                all_rows.append(future.result())
            except (RuntimeError, KeyError, ValueError) as error:
                failed += 1
                print(f'FAILED {os.path.basename(path)}\n{error}')
    write_registry(sorted(all_rows, key=lambda r: r['effect']))
    print(f'spv cache ({CACHE_DIR if CACHE_ON else "off"}): {CACHE_STATS["hit"]} compiles reused, {CACHE_STATS["miss"]} compiled, {CACHE_STATS["pp_hit"]} preprocesses reused, {CACHE_STATS["pp_miss"]} run, {CACHE_STATS["rejected"]} damaged entries rejected, {CACHE_STATS["unstored"]} not stored (inputs changed during the build)')
    print(f'shaders: {len(plain)} plain, {len(plain_todo)} rebuilt; {len(effects)} effects compiled; {failed} failed')
    if '--check' in sys.argv:   # S102 R18-B2
        print('compile_shaders --check: ' + ('headers are current' if not STALE else 'STALE headers: ' + ', '.join(STALE)))
        return 1 if (failed or STALE) else 0
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

"""Session 95 (vertex input, Lane A of out\\s95\\android_structural.md): build the VERTEX-INPUT variants (_vi) of ALL of our vertex shaders that the
SDK registers (every row of src\\scene_vs_swap.h's kArenaVertexShaders; the own crowd shaders are not in it).

For each source src\\scene\\<name>.vert the variant is compiled with  -DXE_VERTEX_INPUT -DXE_VI_DWORDS=<stride>  into src\\scene\\spirv\\<name>_vi_vert.h
(array <name>_vi_vert_spv). Under the define xenos_vs.glsl takes the vertex from vertex attributes (the console's own big-endian bytes, swapped in the shader
as the shared memory path does) instead of the storage buffer, and the guest vertex index is gl_VertexIndex as it is. The attribute rule (uvec4 chunks, then a
uvec2 and / or uint tail) is rex/graphics/pipeline/shader/vertex_input_layout.h's ViAttributes.

  * Stride and fetch constant come from the table in src\\scene_vs_swap.h (one row uses fetch constant 0); the source's own XeVertexAddress arguments must agree.
  * The highest dword the shader reads is computed from the source (helpers followed) and the build FAILS when it reaches the stride.
  * The module goes through spirv-opt -O --target-env=vulkan1.0 and spirv-val at the same target. the plain modules are NOT optimised (build_shaders.ps1 leaves them glslang output, so gpu_native_vertex_input=0 changes no shader byte).
  * A source that git does not track is refused (a row built from the game's own shader dump stays local, CLAUDE.md rule 1).

    py tools\\shaders\\build_vertex_input_vs.py [--out DIR] [--list] [--check] [--mirror] [name ...]
        --list   print stride / fetch constant / highest dword per row, write nothing
        --check  as --list, exit 1 on any problem
        --mirror rewrite the generated parts of src\\scene_vs_mirror.h (one include pair and one Variant per row)
"""
import glob, os, re, shutil, subprocess, sys, tempfile

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.environ.get('XE_SCENE_SRC', os.path.join(R, 'src', 'scene'))
TABLE = os.path.join(R, 'src', 'scene_vs_swap.h')
TARGET = 'vulkan1.0'
CR, NL = chr(13), chr(10)


def sdk_tool(name):
    c = sorted(glob.glob(r'C:\VulkanSDK\*\Bin\%s.exe' % name), reverse=True)
    if c:
        return c[0]
    p = shutil.which(name)
    if p:
        return p
    raise SystemExit('%s not found (Vulkan SDK)' % name)


def table_rows():
    """-> {source name: (stride, fetch_constant)} from scene_vs_swap.h; fails when two rows of one shader disagree."""
    out = {}
    row = re.compile(r'\{0x[0-9A-Fa-f]+ull,\s*(\w+)_vert_spv,.*,\s*(?:kDynamic|0),\s*(\d+),\s*(\d+)\},?\s*$')
    for line in open(TABLE, encoding='utf-8').read().splitlines():
        m = row.search(line)
        if not m:
            continue
        name, stride, fc = m.group(1), int(m.group(2)), int(m.group(3))
        if name in out and out[name] != (stride, fc):
            raise SystemExit('%s: rows disagree (%s vs %s)' % (name, out[name], (stride, fc)))
        out[name] = (stride, fc)
    if not out:
        raise SystemExit('no rows parsed from ' + TABLE)
    return out


def tracked(path):
    r = subprocess.run(['git', 'ls-files', '--error-unmatch', path], cwd=R, capture_output=True, text=True)
    return r.returncode == 0


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return '\n'.join(l.split('//')[0] for l in text.splitlines())


def expand(path, skip=('xenos_vs.glsl',)):
    out = []
    base = os.path.dirname(path)
    for line in open(path, encoding='utf-8').read().splitlines():
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        if m:
            if m.group(1) not in skip:
                out.append(expand(os.path.join(base, m.group(1)), skip))
        else:
            out.append(line)
    return '\n'.join(out)


def find_body(text, start):
    """text[start] is the '{' of a function; -> index after its '}'."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i + 1
    raise SystemExit('unbalanced braces')


def int_value(tok, defs):
    tok = tok.strip()
    seen = 0
    while tok in defs and seen < 8:
        tok = defs[tok].strip()
        seen += 1
    if re.fullmatch(r'\d+', tok):
        return int(tok)
    return None


def analyse(name, stride_fc):
    """-> (max dword read, problems list). stride_fc = the table's (stride, fetch constant)."""
    path = os.path.join(SRC, name + '.vert')
    code = strip_comments(expand(path))
    problems = []
    defs = {}
    for m in re.finditer(r'^\s*#define\s+(\w+)\s+(\S+)\s*$', code, flags=re.M):
        defs[m.group(1)] = m.group(2)
    # helpers taking the fetch address: name -> highest dword read relative to the parameter
    helper = {'XeFetchDword': 0, 'XeFetchFloat': 0}
    funcs = []  # (name, param, body span)
    for m in re.finditer(r'^[ \t]*[\w]+(?:\s+\w+)?[ \t]+(\w+)\(\s*int\s+(\w+)\s*\)\s*\{', code, flags=re.M):
        end = find_body(code, m.end() - 1)
        funcs.append((m.group(1), m.group(2), m.start(), end))
    arg = r'\s*%s(?:\s*\+\s*(\w+))?\s*\)'
    for _ in range(4):  # fixed point over helpers calling helpers
        for fname, param, s, e in funcs:
            body = code[s:e]
            best = None
            for hname, h in list(helper.items()):
                if hname == fname:
                    continue
                for mm in re.finditer(r'\b%s\(%s' % (hname, arg % re.escape(param)), body):
                    k = 0 if mm.group(1) is None else int_value(mm.group(1), defs)
                    if k is None:
                        problems.append('%s: unresolved offset %r in helper %s' % (name, mm.group(1), fname))
                        continue
                    best = max(best if best is not None else -1, k + h)
            if best is not None:
                helper[fname] = max(helper.get(fname, -1), best)
    # the shader's own reads: outside the helper definitions
    blank = list(code)
    for fname, param, s, e in funcs:
        if fname in helper:
            for i in range(s, e):
                blank[i] = ' '
    own = ''.join(blank)
    mx = -1
    for hname, h in helper.items():
        for mm in re.finditer(r'\b%s\(([^()]*(?:\([^()]*\)[^()]*)*)\)' % hname, own):
            a = mm.group(1).strip()
            m2 = re.fullmatch(r'at(?:\s*\+\s*(\w+))?', a)
            if not m2:
                problems.append('%s: %s(%s) is not at / at + N' % (name, hname, a))
                continue
            k = 0 if m2.group(1) is None else int_value(m2.group(1), defs)
            if k is None:
                problems.append('%s: unresolved offset %r in %s' % (name, m2.group(1), hname))
                continue
            mx = max(mx, k + h)
    # the source's own XeVertexAddress calls must agree with the table
    seen = set()
    for mm in re.finditer(r'\bXeVertexAddress\(([^;]*?)\)\s*;', own):
        args = [x.strip() for x in mm.group(1).split(',')]
        if len(args) == 1:
            seen.add((8, 95))
        elif len(args) == 3:
            s_, f_ = int_value(args[1], defs), int_value(args[2], defs)
            if s_ is None or f_ is None:
                problems.append('%s: XeVertexAddress(%s) is not literal' % (name, mm.group(1)))
            else:
                seen.add((s_, f_))
    for m in re.finditer(r'\bXeVertexAddress\(([^;]*?)\)\s*;', code):  # (calls inside the include helpers, with macros)
        pass
    if len(seen) > 1 or (seen and seen != {stride_fc}):
        problems.append('%s: source fetches %s but the table says %s' % (name, sorted(seen), stride_fc))
    if not seen:
        # the fetch call sits in a shared include (vs_ch_gear.glsl ...): the whole expanded text
        for mm in re.finditer(r'\bXeVertexAddress\(([^;]*?)\)\s*;', code):
            args = [x.strip() for x in mm.group(1).split(',')]
            if len(args) == 3:
                s_, f_ = int_value(args[1], defs), int_value(args[2], defs)
                if (s_, f_) != stride_fc:
                    problems.append('%s: include fetches %s but the table says %s' % (name, (s_, f_), stride_fc))
    if mx < 0:
        problems.append('%s: no vertex fetch found' % name)
    elif mx >= stride_fc[0]:
        problems.append('%s: reads dword +%d of a %d-dword vertex' % (name, mx, stride_fc[0]))
    if 'XeSharedMemoryLoad' in own:
        problems.append('%s: reads the shared memory outside the vertex fetch' % name)
    return mx, problems


def write_mirror(rows):
    """Rewrites the generated part of src/scene_vs_mirror.h: one include pair and one Variant per row."""
    path = os.path.join(R, 'src', 'scene_vs_mirror.h')
    text = open(path, encoding='utf-8', newline='').read()
    crlf = CR in text
    text = text.replace(CR + NL, NL)
    names = sorted(n for n in rows if not n.startswith('own_crowd'))
    inc = []
    for n in names:
        inc.append('#include "scene/spirv/%s_vert.h"' % n)
        inc.append('#include "scene/spirv/%s_vi_vert.h"' % n)
    var = ['    {%s_vert_spv, %s_vi_vert_spv, std::size(%s_vi_vert_spv)},' % (n, n, n) for n in names]
    for tag, lines in (('INCLUDES', inc), ('VARIANTS', var)):
        a = text.index('// GENERATED-%s-BEGIN' % tag + NL) + len('// GENERATED-%s-BEGIN' % tag + NL)
        b = text.index('// GENERATED-%s-END' % tag)
        text = text[:a] + NL.join(lines) + NL + text[b:]
    open(path, 'w', encoding='utf-8', newline='').write(text.replace(NL, CR + NL) if crlf else text)
    print('wrote', path, len(names), 'variants')


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit('%s failed' % what)
    return r


def main():
    args = sys.argv[1:]
    out = os.path.join(SRC, 'spirv')
    if '--out' in args:
        i = args.index('--out')
        out = args[i + 1]
        del args[i:i + 2]
    list_only = '--list' in args or '--check' in args
    check = '--check' in args
    names = [a for a in args if not a.startswith('--')]
    rows = table_rows()
    glslang, opt, val = sdk_tool('glslangValidator'), sdk_tool('spirv-opt'), sdk_tool('spirv-val')
    if not list_only:
        os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='s95vi_')
    bad = 0
    built = 0
    try:
        for name in sorted(rows):
            if names and name not in names:
                continue
            if name.startswith('own_crowd'):
                continue
            stride, fc = rows[name]
            path = os.path.join(SRC, name + '.vert')
            if not os.path.exists(path):
                print('%s: no source %s' % (name, path))
                bad += 1
                continue
            if not tracked(path):
                print('%s: source is not tracked by git: no _vi header written (CLAUDE.md rule 1)' % name)
                bad += 1
                continue
            if not 1 <= stride <= 12:
                print('%s: stride %d outside 1..12' % (name, stride))
                bad += 1
                continue
            mx, problems = analyse(name, (stride, fc))
            print('%-34s stride %2d fc %2d  reads dwords 0..%d%s' % (name, stride, fc, mx, '' if mx == stride - 1 else '  (tail %d unread)' % (stride - 1 - mx)))
            if problems:
                bad += len(problems)
                for p in problems:
                    print('   PROBLEM', p)
                continue
            if list_only:
                continue
            spv, spv2 = os.path.join(tmp, name + '.spv'), os.path.join(tmp, name + '.opt.spv')
            run([glslang, '-V', '--target-env', TARGET, '-DXE_VERTEX_INPUT', '-DXE_VI_DWORDS=%d' % stride, '-S', 'vert', '-o', spv, path], 'glslangValidator on ' + name)
            run([opt, '-O', '--target-env=' + TARGET, spv, '-o', spv2], 'spirv-opt on ' + name)
            run([val, '--target-env', TARGET, spv2], 'spirv-val on ' + name)
            data = open(spv2, 'rb').read()
            words = ['0x%08X' % int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
            ident = name + '_vi_vert'
            lines = ['    ' + ', '.join(words[i:i + 8]) + ',' for i in range(0, len(words), 8)]
            header = ['#pragma once',
                      '// Session 95: the vertex-input variant of ../%s.vert, generated by tools\\shaders\\build_vertex_input_vs.py (-DXE_VERTEX_INPUT -DXE_VI_DWORDS=%d, spirv-opt -O,' % (name, stride),
                      '// --target-env=%s): the %d-dword vertex comes from vertex attributes (the console\'s own bytes, swapped here), index = gl_VertexIndex.' % (TARGET, stride),
                      '// Do not edit: edit the .vert and run the script.',
                      '#include <cstdint>', '', 'inline constexpr uint32_t %s_spv[] = {' % ident] + lines + ['};', '']
            open(os.path.join(out, ident + '.h'), 'w', encoding='utf-8', newline='').write('\r\n'.join(header))
            built += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if '--mirror' in sys.argv and not bad:
        write_mirror(rows)
    print('rows %d, problems %d%s' % (len(rows), bad, '' if list_only else ', headers written %d' % built))
    if bad:
        sys.exit(1)


if __name__ == '__main__':
    main()

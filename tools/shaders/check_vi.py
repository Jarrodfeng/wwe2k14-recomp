#!/usr/bin/env python3
"""Session 95 (vertex input, Lane A): static checks of the shipped _vi vertex modules (src/scene/spirv/*_vi_vert.h).

Per module (parsed from its header, disassembled with spirv-dis, validated with spirv-val --target-env vulkan1.0):
  * no StorageBuffer variable and no Uniform variable of a BufferBlock struct (the vertex comes from attributes, not the shared memory),
  * no OpArrayLength,
  * no Function or Private variable whose type is or contains an array,
  * Invariant on the position (the gl_PerVertex member 0, BuiltIn Position),
  * a clean spirv-val.
Also a cross-check that the module's input locations are within the rule of vertex_input_layout.h (uvec4 / uvec2 / uint, location < 16).

  py tools/shaders/check_vi.py [--plain] [name ...]     (--plain: the same checks minus 'no storage' on the plain twin, for the diff report)
Exit 1 on any failure.
"""
import glob, os, re, subprocess, sys, tempfile

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPV = os.path.join(R, 'src', 'scene', 'spirv')


def tool(name):
    c = sorted(glob.glob(r'C:\VulkanSDK\*\Bin\%s.exe' % name), reverse=True)
    if not c:
        raise SystemExit(name + ' not found')
    return c[0]


def words_of(header):
    text = open(header, encoding='utf-8').read()
    body = text[text.index('= {'):]
    return [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{8}', body)]


def check(header, dis, val, plain):
    name = os.path.basename(header)
    ws = words_of(header)
    tmp = tempfile.NamedTemporaryFile(suffix='.spv', delete=False)
    tmp.write(b''.join(w.to_bytes(4, 'little') for w in ws))
    tmp.close()
    problems = []
    try:
        r = subprocess.run([val, '--target-env', 'vulkan1.0', tmp.name], capture_output=True, text=True)
        if r.returncode:
            problems.append('spirv-val: ' + (r.stderr or r.stdout).strip().splitlines()[0][:160])
        text = subprocess.run([dis, tmp.name], capture_output=True, text=True).stdout
    finally:
        os.remove(tmp.name)
    types = {}
    for m in re.finditer(r'^\s*(%\w+) = (OpType\w+)(.*)$', text, flags=re.M):
        types[m.group(1)] = (m.group(2), m.group(3).split())
    ptr = {}
    for k, (op, a) in types.items():
        if op == 'OpTypePointer':
            ptr[k] = (a[0], a[1])
    blocks = set(re.findall(r'OpDecorate (%\w+) BufferBlock', text)) | set(re.findall(r'OpDecorate (%\w+) Block', text))
    bufblock = set(re.findall(r'OpDecorate (%\w+) BufferBlock', text))

    def has_array(t, seen=()):
        if t not in types or t in seen:
            return False
        op, a = types[t]
        if op in ('OpTypeArray', 'OpTypeRuntimeArray'):
            return True
        if op == 'OpTypeStruct':
            return any(has_array(x, seen + (t,)) for x in a)
        return False

    inputs = []
    for m in re.finditer(r'^\s*(%\w+) = OpVariable (%\w+) (\w+)', text, flags=re.M):
        v, pt, sc = m.groups()
        if pt not in ptr:
            continue
        pointee = ptr[pt][1]
        if sc == 'StorageBuffer' and not plain:
            problems.append('StorageBuffer variable %s' % v)
        if sc == 'Uniform' and pointee in bufblock and not plain:
            problems.append('BufferBlock (storage) variable %s' % v)
        if sc in ('Function', 'Private') and has_array(pointee):
            problems.append('%s %s array variable %s' % (sc, 'surviving', v))
        if sc == 'Input':
            inputs.append((v, pointee))
    if 'OpArrayLength' in text and not plain:
        problems.append('OpArrayLength')
    if not re.search(r'OpMemberDecorate %\w+ 0 Invariant', text) and not re.search(r'OpDecorate %\w+ Invariant', text):
        problems.append('no Invariant on the position')
    if not plain:
        for v, pointee in inputs:
            loc = re.search(r'OpDecorate %s Location (\d+)' % re.escape(v), text)
            if not loc:
                continue  # (gl_VertexIndex is a BuiltIn)
            op, a = types[pointee]
            ok = (op == 'OpTypeVector' and len(a) == 2 and a[1] in ('2', '4') and types.get(a[0], ('',))[0] == 'OpTypeInt') or op == 'OpTypeInt'
            if not ok or int(loc.group(1)) >= 16:
                problems.append('input %s at location %s is not uvec4 / uvec2 / uint' % (v, loc.group(1)))
    return name, problems


def main():
    args = sys.argv[1:]
    plain = '--plain' in args
    names = [a for a in args if not a.startswith('--')]
    dis, val = tool('spirv-dis'), tool('spirv-val')
    pat = '*_vert.h' if plain else '*_vi_vert.h'
    files = sorted(f for f in glob.glob(os.path.join(SPV, pat)) if (plain and not f.endswith(('_vi_vert.h', '_inst_vert.h', '_mv_vert.h'))) or not plain)
    files = [f for f in files if not os.path.basename(f).startswith('own_crowd') and (os.path.basename(f).startswith(('arena_vs_', 'vs_')))]
    bad = 0
    n = 0
    # Session 95 (Lane A review): every dynamic bone register read in the sources goes through XeBone / XeBone8 (a raw  fc.c[76 + a]  could leave the 256 registers).
    raw = re.compile(r'fc\.c\[\s*\d+\s*\+\s*(?!i\b)[A-Za-z_]\w*\s*\]')
    # (`8 + i` in vs_prop.glsl is the light loop index 0..n, not a data-driven register)
    for src in sorted(glob.glob(os.path.join(R, 'src', 'scene', '*.vert')) + glob.glob(os.path.join(R, 'src', 'scene', 'vs_*.glsl')) + glob.glob(os.path.join(R, 'src', 'scene', 'xenos_vs*.glsl'))):
        b = os.path.basename(src)
        if not b.startswith(('arena_vs_', 'vs_', 'xenos_vs')):
            continue
        hits = [m.group(0) for m in raw.finditer(open(src, encoding='utf-8').read())]
        if hits:
            bad += 1
            print('%-40s FAIL %d raw dynamic register reads, e.g. %s' % (b, len(hits), hits[0]))
    for f in files:
        base = os.path.basename(f)[:-len('_vi_vert.h' if not plain else '_vert.h')]
        if names and base not in names:
            continue
        n += 1
        name, problems = check(f, dis, val, plain)
        if problems:
            bad += 1
            print('%-40s FAIL %s' % (name, '; '.join(problems)))
    print('modules %d, failures %d' % (n, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

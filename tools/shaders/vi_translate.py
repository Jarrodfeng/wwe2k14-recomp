#!/usr/bin/env python3
"""Session 95, Lane C: the bar on the modules tools\\shaders\\vi_translate.ps1 made. Every module passes spirv-val at its own target (pc = SPIR-V 1.5, vulkan1.2; phone = SPIR-V 1.0, vulkan1.0, with no
layout switch); every attribute module has no shared memory variable, no storage block, no OpArrayLength, and exactly the input locations the translator's form line says it reads.

  py tools\\shaders\\vi_translate.py <the -Out folder>
  py tools\\shaders\\vi_translate.py <the -Out folder> --compare [--rounds N] [--verts N]

--compare runs each pc pair on the GPU through harness\\vs_harness.py's compute form, the same random vertices and constants for both: the plain module reads the shared memory with the index prologue, the
attribute module gets the raw words of vertex i as its inputs and gl_VertexIndex = i. The outputs must be bit-equal. Needs out\\harness_vs\\vkrun_vs.exe (harness\\vs_harness.py says how to build it). Not a game run.
"""
import argparse, concurrent.futures, glob, os, re, subprocess, sys

SDK = r'C:\VulkanSDK'


def sdk_bin(name):
    for d in sorted(os.listdir(SDK), reverse=True):
        p = os.path.join(SDK, d, 'Bin', name + '.exe')
        if os.path.exists(p):
            return p
    raise SystemExit('missing ' + name)


VAL, DIS = sdk_bin('spirv-val'), sdk_bin('spirv-dis')


def check(path, reads):
    name = os.path.basename(path)
    problems = []
    target = 'vulkan1.2' if '_pc_' in name else 'vulkan1.0'
    r = subprocess.run([VAL, '--target-env', target, path], capture_output=True, text=True)
    if r.returncode:
        problems.append('spirv-val: ' + (r.stdout + r.stderr).strip().splitlines()[0][:200])
    attr = name.endswith('_attr.spv')
    if attr:
        text = subprocess.run([DIS, path], capture_output=True, text=True).stdout
        for bad in ('StorageBuffer', 'BufferBlock', 'OpArrayLength', 'xe_shared_memory', 'XeSharedMemory'):
            if bad in text:
                problems.append('has ' + bad)
        names = dict(re.findall(r'OpName (%\S+) "(\S+)"', text))
        have = 0
        for var, loc in re.findall(r'OpDecorate (%\S+) Location (\d+)', text):
            if names.get(var, '').startswith('xe_in_vertex_'):
                have |= 1 << int(loc)
        want = reads.get(name[:16])
        if want is None or have != want:
            problems.append(f'inputs {have:04X}, form says {want if want is None else format(want, "04X")}')
    return name, attr, problems


def attributes(stride):
    """vertex_input_layout.h's rule for one binding: (location, first word, words)."""
    word = location = 0
    while word < stride:
        left = stride - word
        words = 4 if left >= 4 else 2 if left >= 2 else 1
        yield location, word, words
        location += 1
        word += words


def compare_source(cross, stride):
    """The attribute module's GLSL with its inputs filled from the harness's shared memory, raw, as a vertex buffer would give them."""
    declared = {}

    def declaration(m):
        declared[int(m.group(1))] = m.group(2)
        return '%s xe_in_vertex_%s;' % (m.group(2), m.group(1))

    text = re.sub(r'layout\(location = (\d+)\) in (uvec\d|uint) xe_in_vertex_\d+;', declaration, cross)
    fill = ['    uint xe_vi = uint(gl_VertexIndex);']
    for location, first, words in attributes(stride):
        if location in declared:
            parts = ', '.join('xe_vi_mem.shared_memory[xe_vi * %du + %du]' % (stride, first + i) for i in range(words))
            fill.append('    xe_in_vertex_%d = %s(%s);' % (location, declared[location], parts))
    block = 'layout(set = 1, binding = 0, std430) buffer XeSharedMemory\n{\n    uint shared_memory[];\n} xe_vi_mem;\n\n'
    text, count = re.subn(r'void main\(\)\s*\{\n', lambda m: block + m.group(0) + '\n'.join(fill) + '\n', text, count=1)
    assert count == 1
    return text


def compare_pair(H, plain, attr, stride, rounds, verts, work, runner):
    cross = lambda path: subprocess.run([H.sdk_bin('spirv-cross.exe'), '--version', '450', '--vulkan-semantics', path], capture_output=True, text=True, check=True).stdout
    os.makedirs(work, exist_ok=True)
    inp = os.path.join(work, 'in.bin')
    open(inp, 'wb').write(H.make_inputs(rounds, verts, 1, 0))
    res = {}
    for tag, source in (('plain', cross(plain)), ('attr', compare_source(cross(attr), stride))):
        text, outs = H.to_compute(source.replace('gl_InstanceIndex', '0'), verts)  # (a merged draw's first member)
        comp, spv, outp = (os.path.join(work, tag + e) for e in ('.comp', '.spv', '.out'))
        open(comp, 'w').write(text)
        H.compile_comp(comp, spv)
        subprocess.run([runner, spv, inp, outp, str(rounds), str(verts)], capture_output=True, text=True, check=True)
        res[tag] = H.np.fromfile(outp, H.np.uint32)
    if res['plain'].size == 0 or res['plain'].size != res['attr'].size:
        return 'sizes differ'
    differ = int((res['plain'] != res['attr']).sum())
    return differ


def compare(out, rounds, verts):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'harness'))
    import vs_harness as H
    H.VI = True  # (no vertex base in the inputs: an attribute draw has the identity)
    runner = r'C:\Users\Jarrod\wwe2k14-recomp\out\harness_vs\vkrun_vs.exe'
    forms = {}
    for line in open(os.path.join(out, 'translate.txt'), encoding='utf-8'):
        m = re.match(r'shader (\S+) form yes bindings (\d+) vi (\S+) reads \S+(.*)', line)
        if m:
            forms[m.group(1)] = (int(m.group(2)), int(m.group(3), 16), m.group(4).split())
    pairs, skipped = [], {}
    for plain in sorted(glob.glob(os.path.join(out, 'spv', '*_pc_plain.spv'))):
        name = os.path.basename(plain)
        bindings, vi, constants = forms[name[:16]]
        # (the harness's inputs give fetch constants 95 and 0 base 0, and 12 dwords a vertex)
        if bindings != 1 or vi > 12 or not re.match(r'vf(95|0):', constants[0]):
            skipped[name[:16]] = constants
            continue
        pairs.append((plain, plain.replace('_plain.spv', '_attr.spv'), vi))
    print(f'pairs {len(pairs)}, skipped shaders {len(skipped)} {sorted(skipped.items())[:6]}')

    def one(job):
        i, (plain, attr, stride) = job
        return os.path.basename(plain), compare_pair(H, plain, attr, stride, rounds, verts, os.path.join(out, 'cmp', str(i % 4)), runner)

    bad = 0
    # Four work folders, one job at a time in each.
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        for group in range(0, len(pairs), 4):
            for name, result in pool.map(one, list(enumerate(pairs))[group:group + 4]):
                if result != 0:
                    bad += 1
                    print(name, 'differing dwords', result)
    print(f'compared {len(pairs)} pairs, {rounds} rounds x {verts} vertices, not bit-equal {bad}')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--compare', action='store_true')
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--verts', type=int, default=2048)
    a = ap.parse_args()
    if a.compare:
        return compare(a.out, a.rounds, a.verts)
    out = a.out
    reads = {}
    for line in open(os.path.join(out, 'translate.txt'), encoding='utf-8'):
        m = re.match(r'shader (\S+) form yes bindings \d+ vi \S+ reads (\S+)', line)
        if m:
            reads[m.group(1)] = int(m.group(2), 16)
    files = sorted(glob.glob(os.path.join(out, 'spv', '*.spv')))
    bad = 0
    attrs = 0
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for name, attr, problems in pool.map(lambda f: check(f, reads), files):
            attrs += attr
            if problems:
                bad += 1
                if bad <= 40:
                    print(name, '; '.join(problems))
    print(f'modules {len(files)}, attribute {attrs}, with problems {bad}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())

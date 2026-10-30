"""Session 22: compile an SDK compute shader with the out-of-tree glslangValidator (Session 7c) and write the
vulkan_spirv header in the format of the Session 7c direct-load headers (comment, -H disassembly in #if 0, the
SPIR-V words as a uint32_t array, 8 per line).

    py tools/perf/gen_spirv_header.py SOURCE.glsl NAME DEFINE[,DEFINE...] OUT.h [--compare EXISTING.h]
        [--stage comp|frag|vert] [--session 'Session NN']

--compare: only check that the words equal EXISTING.h's array (proves the toolchain reproduces a header)."""
import argparse
import os
import re
import subprocess
import tempfile

GLSLANG = r'C:\Users\Jarrod\wwe2k14-recomp\out\glslang-build\StandAlone\glslangValidator.exe'


def words_of(header_text):
    body = header_text.split('[] = {', 1)[1].split('};', 1)[0]
    return [int(w, 16) for w in re.findall(r'0x[0-9A-Fa-f]{8}', body)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source')
    ap.add_argument('name')
    ap.add_argument('defines')
    ap.add_argument('out')
    ap.add_argument('--compare')
    # Session 73 (SGSR present effect): other shader stages; the default keeps the Session 22 compute headers.
    ap.add_argument('--stage', default='comp', choices=['comp', 'frag', 'vert'])
    ap.add_argument('--session', default='Session 22')
    a = ap.parse_args()
    defs = [d for d in a.defines.split(',') if d]
    with tempfile.TemporaryDirectory() as td:
        spv = os.path.join(td, 'out.spv')
        cmd = [GLSLANG, '-V', '--target-env', 'vulkan1.0'] + [f'-D{d}' for d in defs] + ['-S', a.stage, '-H', '-o',
                                                                                           spv, a.source]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout, r.stderr)
            raise SystemExit(f'glslangValidator failed ({r.returncode})')
        data = open(spv, 'rb').read()
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    if a.compare:
        old = words_of(open(a.compare, encoding='utf-8').read())
        print(f'{len(words)} words, existing {len(old)}: {"IDENTICAL" if old == words else "DIFFERENT"}')
        raise SystemExit(0 if old == words else 1)
    lines = r.stdout.splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith('// Module Version'))
    disasm = lines[start:]
    while disasm and not disasm[-1].strip():
        disasm.pop()
    out = [f'// wwe2k14 patch ({a.session}): generated from ../{os.path.basename(a.source)} with',
           f'// glslangValidator -V --target-env vulkan1.0 {" ".join("-D" + d for d in defs)} -S {a.stage} (glslang 11.0.0 from '
           f'the', "// SDK's thirdparty/glslang submodule, built out of tree, no spirv-opt; tools\\perf\\gen_spirv_header.py);",
           '// disassembly from its -H option.', '#if 0'] + disasm + ['#endif', '', f'const uint32_t {a.name}[] = {{']
    for i in range(0, len(words), 8):
        out.append('    ' + ' '.join(f'0x{w:08X},' for w in words[i:i + 8]))
    out.append('};')
    open(a.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
    print(f'{a.out}: {len(words)} words')


if __name__ == '__main__':
    main()

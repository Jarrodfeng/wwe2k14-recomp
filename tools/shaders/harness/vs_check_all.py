#!/usr/bin/env python3
"""Session 87 (stage 4): runs vs_harness.py for every shader of src\\scene_vs_swap.h's table against the SDK translation of its
guest vertex shader (the dumped module with the most interpolators; the native FMA one when it has as many), and prints one
line each. The comparison is on all records: a shader applies the guest's multiply rule (AvMulX) wherever the translation does.
  py tools\\shaders\\harness\\vs_check_all.py [--wild] [--opt] [--dump DIR] [name ...]      (--wild: data with unbounded floats / raw bits)"""
import glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DUMP = os.path.join(R, 'out', 's70', 'shader_dump')
sys.path.insert(0, HERE)


def table():
    ns = {}
    src = open(os.path.join(HERE, 'gen_header.py'), encoding='utf-8').read()
    start = src.index('T = [')
    end = src.index('\n]\n', start) + 3
    exec('ARENA = None\n' + src[start:end], ns)
    extra = os.path.join(HERE, 'vs_table_extra.py')
    if os.path.exists(extra):
        exec(open(extra, encoding='utf-8').read(), ns)
    return ns['T']


def best_ref(h):
    """The dumped module with the most interpolators for the guest shader; a native FMA module (_e00000002; never one of our own
    replacements, ext bit 0x10) preferred over a plain one. Looks in out/s87/shader_dump (the milestone run's) first, then out/s70."""
    files = []
    for d in DUMPS + [os.path.join(R, 'out', 's87', 'shader_dump'), DUMP]:
        files += glob.glob(os.path.join(d, 'shader_%s_*.vk.bin.vert' % h))
    cand = []
    for f in files:
        m = re.search(r'_([0-9A-F]{16})(?:_e([0-9a-f]{8}))?\.vk', f)
        ext = int(m.group(2), 16) if m.group(2) else 0
        if ext & 0x10:
            continue  # (a module made with the vertex swap on holds our own code)
        cand.append((int(m.group(1), 16) & 0xFFFF, ext & 2 != 0, 's87' in f, f))
    if not cand:
        return None, False
    best = max(cand)
    return best[3], not best[1]


DUMPS = []  # Session 95: --dump DIR (repeatable) puts a fresh --dump_shaders folder first


def main():
    argv = sys.argv[1:]
    while '--dump' in argv:
        i = argv.index('--dump')
        DUMPS.append(argv[i + 1])
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith('--')]
    wild = '--wild' in argv
    opt = '--opt' in argv  # Session 95: ours as shipped (glslang + spirv-opt -O)
    bad = 0
    for row in table():
        h, f = row[0], row[1]
        if args and f not in args:
            continue
        ref, plain = best_ref(h)
        if not ref:
            print('%-30s no dumped module' % f)
            continue
        cmd = [sys.executable, os.path.join(HERE, 'vs_harness.py'), '--mine', os.path.join(R, 'src', 'scene', f + '.vert'),
               '--ref', ref, '--rounds', '16', '--verts', '2048']
        if plain:
            cmd.append('--plain')
        if wild:
            cmd.append('--wild')
        if opt:
            cmd.append('--opt')
        if '--noclamp' in argv:
            cmd.append('--noclamp')
        if '--bone-safe' in argv:
            cmd.append('--bone-safe')
            cmd.append('--dynamic')
        if len(row) > 7 and row[7] == 'dynamic':
            cmd.append('--dynamic')
        r = subprocess.run(cmd, capture_output=True, text=True)
        lines = [l for l in r.stdout.strip().splitlines() if l.startswith(('RESULT', 'records compared'))]
        res = ' | '.join(lines) if lines else (r.stdout + r.stderr)[-300:]
        print('%-30s %s %s' % (f, os.path.basename(ref)[7:40], res))
        bad += 'PASS' not in res
    print('failures', bad)


if __name__ == '__main__':
    main()

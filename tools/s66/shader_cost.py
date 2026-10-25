"""Offline shader cost on an AMD integrated GPU (Session 65 prep): every translated shader of a dump, compiled by
Radeon GPU Analyzer 2.15 (thirdparty\\rga, MIT) for gfx1103 (Radeon 780M / 760M / 740M, RDNA3: the closest target
RGA 2.15 still has to the Steam Deck's RDNA2 Van Gogh, gfx1033; relative changes carry over, absolute numbers do not).

A dump comes from one run with --dump_shaders=DIR (the Vulkan path writes shader_<hash>_<modification>.vk[_fsi].bin.vert
/ .frag = SPIR-V, next to the guest microcode). Per shader: vector / scalar registers used (fewer = more waves in
flight), spills, machine-code bytes and instruction count. With two dumps (before / after a translator change) the
shaders are paired by file name and the totals and the biggest changes are printed. Output under out\\ only (the
shaders are derived from the game's: never commit them or the ISA).

    py tools\\s66\\shader_cost.py DUMP_DIR [AFTER_DIR] [--asic gfx1103] [--jobs 12] [--top 15] [--csv OUT.csv]
"""
import argparse
import concurrent.futures as cf
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RGA = os.path.join(R, 'thirdparty', 'rga', 'rga.exe')
NAME = re.compile(r'^shader_[0-9A-F]+_[0-9A-F]+\.vk(_fsi|_fbo|_native)?\.bin\.(vert|frag)$')  # Session 66: + vk_native (NATIVE's own pixel shaders)
INSTR = re.compile(r'^\s+(s_|v_|buffer_|image_|global_|ds_|exp |flat_|scratch_)')


def cost(path, asic):
    stage = path.rsplit('.', 1)[1]
    work = tempfile.mkdtemp(prefix='rga_')
    try:
        spv = os.path.join(work, f's.{stage}.spv')   # RGA reads .vert / .frag as GLSL text: give it a .spv name
        shutil.copyfile(path, spv)
        p = subprocess.run([RGA, '-s', 'vk-spv-offline', '-c', asic, f'--{stage}', spv,
                            '-a', os.path.join(work, 'stats.csv'), '--isa', os.path.join(work, 'isa.txt')],
                           capture_output=True, text=True)
        stats = [f for f in os.listdir(work) if f.endswith(f'stats_{stage}.csv')]
        isa = [f for f in os.listdir(work) if f.endswith(f'isa_{stage}.txt')]
        if p.returncode != 0 or not stats:
            return None
        row = next(csv.DictReader(open(os.path.join(work, stats[0]))))
        n = sum(1 for ln in open(os.path.join(work, isa[0]), errors='replace') if INSTR.match(ln)) if isa else -1
        return dict(vgpr=int(row['USED_VGPRs']), sgpr=int(row['USED_SGPRs']),
                    spills=int(row['VGPR_SPILLS']) + int(row['SGPR_SPILLS']), isa=int(row['ISA_SIZE']), instr=n)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def scan(d, asic, jobs, kind=''):
    files = sorted(f for f in os.listdir(d) if NAME.match(f) and (not kind or f'.{kind}.bin.' in f))
    out, failed = {}, []
    with cf.ThreadPoolExecutor(jobs) as ex:
        for f, c in zip(files, ex.map(lambda f: cost(os.path.join(d, f), asic), files)):
            (out.__setitem__(f, c) if c else failed.append(f))
    return out, failed


def totals(c):
    keys = ('vgpr', 'sgpr', 'spills', 'isa', 'instr')
    return {k: sum(v[k] for v in c.values()) for k in keys}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('after', nargs='?')
    ap.add_argument('--asic', default='gfx1103')
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--csv', default='')
    ap.add_argument('--kind', default='', help='only one kind: vk (vertex + emulated fbo), vk_fsi, vk_fbo, vk_native')
    a = ap.parse_args()
    if not os.path.exists(RGA):
        sys.exit(f'no {RGA}')
    before, fb = scan(a.dump, a.asic, a.jobs, a.kind)
    print(f'{a.dump}: {len(before)} shaders compiled for {a.asic}, {len(fb)} failed'
          + (f' (first: {fb[0]})' if fb else ''))
    for stage in ('vert', 'frag'):
        s = {k: v for k, v in before.items() if k.endswith(stage)}
        if s:
            t = totals(s)
            print(f'  {stage}: {len(s)} shaders, mean VGPRs {t["vgpr"] / len(s):.1f}, mean instructions {t["instr"] / len(s):.0f}, '
                  f'machine code {t["isa"] / 1024:.0f} KB, spills {t["spills"]}')
    if a.csv:
        with open(a.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['shader', 'vgpr', 'sgpr', 'spills', 'isa', 'instr'])
            for k, v in sorted(before.items()):
                w.writerow([k, v['vgpr'], v['sgpr'], v['spills'], v['isa'], v['instr']])
    if not a.after:
        return
    after, fa = scan(a.after, a.asic, a.jobs, a.kind)
    common = sorted(set(before) & set(after))
    print(f'{a.after}: {len(after)} compiled, {len(fa)} failed; {len(common)} shaders in both')
    for stage in ('vert', 'frag'):
        ks = [k for k in common if k.endswith(stage)]
        if not ks:
            continue
        tb = totals({k: before[k] for k in ks})
        ta = totals({k: after[k] for k in ks})
        print(f'  {stage} ({len(ks)}): ' + ', '.join(
            f'{k} {tb[k]} -> {ta[k]} ({100.0 * (ta[k] - tb[k]) / tb[k]:+.1f}%)' if tb[k] else f'{k} {tb[k]} -> {ta[k]}'
            for k in ('instr', 'vgpr', 'isa', 'spills')))
    ch = sorted(common, key=lambda k: after[k]['instr'] - before[k]['instr'])
    print(f'biggest instruction changes (of {len(common)}):')
    for k in ch[:a.top] + [x for x in ch[-a.top:] if x not in ch[:a.top]]:
        b, f = before[k], after[k]
        if b['instr'] != f['instr'] or b['vgpr'] != f['vgpr']:
            print(f'  {k:<60} instr {b["instr"]:>5} -> {f["instr"]:<5} VGPR {b["vgpr"]:>3} -> {f["vgpr"]}')


if __name__ == '__main__':
    main()

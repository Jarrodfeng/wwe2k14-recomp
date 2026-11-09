"""Frame replay (Session 98): which of OUR scene shaders a captured draw ran, and the guest register each packed float constant is.
  ps md5 (rdc_draw meta) -> the dumped translation file (out\\...\\shaders\\shader_<guest>_<mods>.vk_fbo.bin.frag, md5 of the SPIR-V)
  -> guest hash -> src\\scene_swap.h row -> src\\scene\\<name>.frag -> its `#define CNNN fc.c[i]` table (register NNN = slot i).
Reads local dumps only; writes nothing."""
import glob
import hashlib
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))


def md5_table(shader_dir):
    out = {}
    for f in glob.glob(os.path.join(shader_dir, 'shader_*.vk*.bin.frag')):
        out[hashlib.md5(open(f, 'rb').read()).hexdigest()] = os.path.basename(f).split('_')[1]
    return out


def swap_table():
    """guest hash -> scene frag file name (src\\scene_swap.h)."""
    t = {}
    txt = open(os.path.join(REPO, 'src', 'scene_swap.h'), encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'\{0x([0-9A-Fa-f]{16})ull,\s*([a-z0-9_]+)_spv', txt):
        t[m.group(1).upper()] = m.group(2)
    for m in re.finditer(r'kWrestler([A-Za-z0-9]+?)(Entrance)?\s*=\s*0x([0-9A-Fa-f]{16})ull', txt):  # (the shared-binary rows)
        snake = re.sub(r'(?<!^)([A-Z])', r'_\1', re.sub(r'Match$', '', m.group(1))).lower()  # (kWrestlerSkinMatch = wrestler_skin)
        t.setdefault(m.group(3).upper(), 'wrestler_%s%s' % (snake, '_entrance' if m.group(2) else ''))
    return t


def frag_regs(name):
    """register -> slot from the frag's `#define CNNN fc.c[i]` lines; plus its float array size."""
    p = os.path.join(REPO, 'src', 'scene', name.replace('_frag', '') + '.frag')
    if not os.path.exists(p):
        return None
    regs = {}
    for m in re.finditer(r'#define\s+C(\d+)\s+fc\.c\[(\d+)\]', open(p, encoding='utf-8', errors='replace').read()):
        regs[int(m.group(1))] = int(m.group(2))
    return regs


def resolve(ps_md5, shader_dir):
    g = md5_table(shader_dir).get(ps_md5)
    if not g:
        return None, None, None
    name = swap_table().get(g.upper())
    return g, name, frag_regs(name) if name else None


if __name__ == '__main__':
    import sys
    sd = sys.argv[1]
    for md in sys.argv[2:]:
        g, n, r = resolve(md, sd)
        print(md[:12], g, n, (sorted(r) if r else None))

"""Frame-replay harness, shared helpers (Session 98, stage S2; design out\\s98\\replay\\design.md section B).

Imported by rdc_extract.py (qrenderdoc's embedded Python 3.8) and replay_index.py (system Python 3.12): standard library only.
Reads the guest shader microcode text that --dump_shaders writes (shader_<hash>.ucode.vert|frag, local, out\\ only) to learn
which float constant registers a translated shader packs into its xe_uniform_float_constants block, and the vertex fetch slot
and stride of a vertex shader. No game data is stored here.
"""
import os
import re

_REL = re.compile(r'\bc\[')
_REG = re.compile(r'(?<![\w\[])c(\d+)\b')
_VF = re.compile(r'vfetch_full\s+\S+,\s*\S+,\s*vf(\d+)(.*)$')
_STRIDE = re.compile(r'Stride=(\d+)')


def dump_index(shader_dir):
    """{guest hash (16 hex, upper): {'vert'|'frag': [translated .bin file names]}} plus the microcode text paths."""
    out = {}
    if not shader_dir or not os.path.isdir(shader_dir):
        return out
    for fn in os.listdir(shader_dir):
        m = re.match(r'shader_([0-9A-Fa-f]{16})(?:_([0-9A-Fa-f]{16}))?\.(ucode\.)?(?:bin\.)?(vk[a-z_]*\.bin\.)?(vert|frag)$', fn)
        if not m:
            continue
        e = out.setdefault(m.group(1).upper(), {})
        if fn.endswith('.ucode.vert') or fn.endswith('.ucode.frag'):
            e['ucode_text'] = fn
        elif '.vk' in fn:
            e.setdefault('translated', []).append(fn)
    return out


def ucode_info(path):
    """From one microcode text file: the float registers it reads (sorted, 0-255 of its own stage), whether it indexes the
    bank (c[...+a0]: the translation then binds the whole 256-register bank), and the vertex fetch slots with stride (dwords)."""
    regs, rel, vfetch = set(), False, {}
    try:
        text = open(path, 'r', errors='replace').read()
    except Exception:
        return None
    for line in text.splitlines():
        code = line.split('*/', 1)[-1]
        if _REL.search(code):
            rel = True
        for m in _REG.finditer(code):
            regs.add(int(m.group(1)))
        m = _VF.search(code)
        if m:
            s = _STRIDE.search(m.group(2))
            vfetch.setdefault(int(m.group(1)), int(s.group(1)) if s else 0)
    return {'regs': sorted(r for r in regs if r < 256), 'relative': rel, 'vfetch': vfetch}


def float_register_map(info, packed_count):
    """The game register (0-255, of the stage) of each packed float constant slot, or None when it can't be told.
    The translator packs the registers the shader uses in ascending order; an indexed shader gets all 256."""
    if packed_count == 256:
        return list(range(256))
    if info is None:
        return None
    regs = info['regs']
    if len(regs) == packed_count:
        return regs
    return None


def vertex_fetch(fetch_dwords, slot):
    """Decodes vertex fetch constant vf<slot> (2 dwords: type | dword address, endian | size in dwords) from the 192-dword
    fetch constant block: (byte address, byte size) or None. The microcode text prints vf<95 - storage index>
    (SDK translator_disasm.cpp:331), so vf0 is the block's last pair (checked: the eye VS's vf0 = 0x1C405000, 578 x 36 B)."""
    i = (95 - slot) * 2
    if fetch_dwords is None or i + 1 >= len(fetch_dwords):
        return None
    d0, d1 = fetch_dwords[i], fetch_dwords[i + 1]
    if d0 & 3 != 3:
        return None
    return (d0 & 0xFFFFFFFC, ((d1 >> 2) & 0xFFFFFF) * 4)


def texture_fetch(fetch_dwords, slot):
    """Decodes texture fetch constant tf<slot> (6 dwords) far enough to name the guest texture: base address, format id,
    width / height (2D / cube), dimension (0 1D, 1 2D, 2 3D, 3 cube), tiled, pitch."""
    i = slot * 6
    if fetch_dwords is None or i + 5 >= len(fetch_dwords):
        return None
    d = fetch_dwords[i:i + 6]
    if d[0] & 3 != 2:
        return None
    return {'address': '%08X' % (d[1] & 0xFFFFF000), 'format': d[1] & 0x3F, 'w': (d[2] & 0x1FFF) + 1,
            'h': ((d[2] >> 13) & 0x1FFF) + 1, 'dimension': (d[5] >> 9) & 3, 'tiled': (d[0] >> 31) & 1,
            'pitch': ((d[0] >> 22) & 0x1FF) * 32, 'mip_address': '%08X' % (d[5] & 0xFFFFF000)}


_TEXNAME = re.compile(r'xe_texture(\d+)_(\w+?)_([us])$')


def texture_binding(name):
    """'xe_texture13_cube_u' -> (13, 'cube', 'u'); other names -> None."""
    m = _TEXNAME.match(name or '')
    return (int(m.group(1)), m.group(2), m.group(3)) if m else None

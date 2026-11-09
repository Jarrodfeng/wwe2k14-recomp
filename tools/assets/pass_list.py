"""Session 98: list the render passes of a gpu_anatomy_capture CSV (one frame), grouped by consecutive identical state.

  py -I tools/assets/pass_list.py anatomy_NNN_K.csv [--keep-crowd]

One line per run of draws with the same (pixel shader, vertex shader, surface pitch, MSAA, colour format, viewport w x h,
depth flag); 'C' lines are the resolves (copy extent, destination page). Colour format codes: 0 = 8888, 4 = 16_16,
5 = 16_16_16_16 (rb_color_info0 bits 16-19). The crowd's pixel shaders are left out unless --keep-crowd. Local files only.
"""
import csv
import struct
import sys

csv.field_size_limit(10 ** 9)


def f32(h):
    try:
        return struct.unpack('>f', struct.pack('>I', int(h, 16)))[0]
    except Exception:
        return 0.0


def main():
    fn = sys.argv[1]
    keep = '--keep-crowd' in sys.argv
    rows = [r for r in csv.DictReader(open(fn)) if keep or r['ps_hash'][:8] not in ('EFC4EFCB', '803205E3')]
    prev, n, out = None, 0, []
    for r in rows:
        if r['kind'] == 'draw':
            sp = int(r['rb_surface_info'], 16)
            ci = int(r['rb_color_info0'], 16)
            w = abs(f32(r['pa_cl_vport_xscale'])) * 2
            h = abs(f32(r['pa_cl_vport_yscale'])) * 2
            key = ('D', r['ps_hash'][:8], r['vs_hash'][:8], 'pitch', sp & 0x3fff, 'msaa', (sp >> 16) & 3,
                   'fmt', (ci >> 16) & 0xf, '%dx%d' % (w, h), 'Z' if int(r['rb_depth_info'], 16) & 0xfff else '-')
        else:
            key = ('C', 'extent', r.get('copy_pitch'), 'dest', r.get('copy_dest'))
        if key == prev:
            n += 1
        else:
            if prev:
                out.append((prev, n))
            prev, n = key, 1
    if prev:
        out.append((prev, n))
    for k, c in out:
        print(c, *k)


if __name__ == '__main__':
    main()

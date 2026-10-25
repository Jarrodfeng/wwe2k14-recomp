"""Session 61: where each shader the SDK met comes from in the player's own files (docs\\systems\\shaders.md).

    py tools\\shaders\\locate.py index                      -> out\\shaders\\index.json (hash -> file / leaf / effect)
    py tools\\shaders\\locate.py match FILE.xsh [FILE.xsh ...] [--list OUT.txt] [--verbose]

`index` reads the four files that hold shaders (Session 61's scan of every archive: tools\\shaders\\scan_archives.py)
and the XEX image (out\\harness\\index\\image.bin: the D3D library's own shaders, as raw microcode, not containers).
`match` finds every stored shader of the given storage files:
- exact: its XXH3 is a container's microcode in the files;
- patched vertex shader (the library rewrites a vertex shader's fetch instructions to the draw's vertex declaration,
  sub_82CBE0C0 / sub_82CBD910): the same-length original with the fewest differing dwords, and the XOR mask of each
  differing dword (the declaration's fields; never the microcode itself);
- image: raw microcode in the XEX image (exact, or patched as above against the image's bytes at the same address).
`--list` writes the shader part of the shipped list (docs\\systems\\shaders.md, "The list"): hashes, sources and XOR
masks only.
"""

import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'wm30'))
import pac  # noqa: E402
import shaderlib  # noqa: E402
import xsh  # noqa: E402

SHADER_FILES = ['pac/_startup.pac', 'pac/car/car.pac', 'pac/edit/edit.pac']
IMAGE = os.path.join(shaderlib.REPO, 'out', 'harness', 'index', 'image.bin')
IMAGE_BASE = 0x82000000
INDEX = os.path.join(shaderlib.REPO, 'out', 'shaders', 'index.json')


def leaves_with_shaders(rel):
    data = pac.load(os.path.join(shaderlib.GAME, rel.replace('/', os.sep)))
    for name, dec in pac.entries(data):
        if b'\x10\x2a\x11' in dec:
            yield name, dec


def build_index():
    """{hash: {file, leaf, effect, container, kind, dwords}} (the first place a shader is found) + microcode by hash
    (in memory only)."""
    index = {}
    code = {}
    for rel in SHADER_FILES:
        for leaf, dec in leaves_with_shaders(rel):
            table = shaderlib.effect_table(dec) or []
            for off, kind, _, mc in shaderlib.containers(dec):
                h = '%016X' % shaderlib.xxh3(mc)
                code.setdefault(h, (kind, mc))
                if h in index:
                    continue
                effect = next((n for n, o, s in table if o <= off < o + s), None)
                index[h] = {'file': rel, 'leaf': leaf, 'effect': effect, 'container': off, 'kind': kind,
                            'dwords': len(mc) // 4}
    return index, code


def cmd_index(_):
    index, _ = build_index()
    os.makedirs(os.path.dirname(INDEX), exist_ok=True)
    json.dump(index, open(INDEX, 'w'), indent=1, sort_keys=True)
    by = collections.Counter((v['file'], v['kind']) for v in index.values())
    print('%d shaders -> %s' % (len(index), INDEX))
    for (f, k), n in sorted(by.items()):
        print('  %-22s %s %d' % (f, k, n))
    return 0


def xor_patch(base, mc):
    """[(dword index, xor mask)] turning base into mc (same length)."""
    out = []
    for i in range(len(mc) // 4):
        a = struct.unpack_from('>I', base, 4 * i)[0]
        b = struct.unpack_from('>I', mc, 4 * i)[0]
        if a != b:
            out.append((i, a ^ b))
    return out


def cmd_match(a):
    index, code = build_index()
    image = open(IMAGE, 'rb').read() if os.path.exists(IMAGE) else b''
    by_len = collections.defaultdict(list)
    for h, (kind, mc) in code.items():
        by_len[(kind, len(mc))].append((h, mc))
    stored = {}
    for f in a.xsh:
        for h, t, mc in xsh.read_xsh(f):
            stored.setdefault('%016X' % h, ('VS' if t == 0 else 'PS', mc))
    rows = []
    counts = collections.Counter()
    for h, (kind, mc) in sorted(stored.items()):
        if h in index:
            rows.append((h, kind, len(mc) // 4, 'exact', None, []))
            counts['exact ' + kind] += 1
            continue
        i = image.find(mc) if image else -1
        if i >= 0 and i % 4 == 0:
            rows.append((h, kind, len(mc) // 4, 'image', IMAGE_BASE + i, []))
            counts['image ' + kind] += 1
            continue
        best = None
        for bh, bmc in by_len.get((kind, len(mc)), []):
            p = xor_patch(bmc, mc)
            if best is None or len(p) < len(best[1]):
                best = (bh, p)
        # the library's own patched vertex shaders: the same address in the image, same length
        img_best = None
        if image:
            j = image.find(mc[:16])
            while j >= 0 and j % 4 == 0 and j + len(mc) <= len(image):
                p = xor_patch(image[j:j + len(mc)], mc)
                if img_best is None or len(p) < len(img_best[1]):
                    img_best = (IMAGE_BASE + j, p)
                j = image.find(mc[:16], j + 4)
        if img_best and (best is None or len(img_best[1]) <= len(best[1])) and len(img_best[1]) <= len(mc) // 8:
            rows.append((h, kind, len(mc) // 4, 'image-patched', img_best[0], img_best[1]))
            counts['image-patched ' + kind] += 1
        elif best and len(best[1]) <= max(4, len(mc) // 8):
            rows.append((h, kind, len(mc) // 4, 'patched', best[0], best[1]))
            counts['patched ' + kind] += 1
        else:
            rows.append((h, kind, len(mc) // 4, 'unknown', None, []))
            counts['unknown ' + kind] += 1
    print('%d stored shaders: %s' % (len(stored), ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    if a.verbose:
        for h, kind, n, how, src, p in rows:
            where = index[h]['file'] + ' ' + index[h]['leaf'] + ' ' + str(index[h]['effect']) if how == 'exact' else (
                '%016X' % 0 if src is None else (('0x%08X' % src) if isinstance(src, int) else src))
            print('  %s %s %4d %-13s %s %s' % (h, kind, n, how, where, ('%d dwords patched' % len(p)) if p else ''))
    if a.list:
        with open(a.list, 'w', newline='\n') as o:
            o.write('# wwe2k14 shader list v1 (Session 61): hashes, sources and XOR masks only (docs/systems/shaders.md)\n')
            for h, kind, n, how, src, p in rows:
                if how == 'unknown':
                    continue
                line = 'S %s %s %d %s' % (h, kind, n, how)
                if how in ('patched', 'image-patched', 'image'):
                    line += ' ' + (('%08X' % src) if isinstance(src, int) else src)
                if p:
                    line += ' ' + ' '.join('%d:%08X' % (i, m) for i, m in p)
                o.write(line + '\n')
        print('list ->', a.list)
    return 1 if counts and any(k.startswith('unknown') for k in counts) else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('index')
    m = sub.add_parser('match')
    m.add_argument('xsh', nargs='+')
    m.add_argument('--list')
    m.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    return cmd_index(a) if a.cmd == 'index' else cmd_match(a)


if __name__ == '__main__':
    sys.exit(main())

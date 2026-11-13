#!/usr/bin/env python3
"""Session 100: one command builds the per-effect shader packets (offline, no game run).

    py tools\\shaders\\packets\\build_packets.py [--disasm-exe PATH]

Reads _startup.pac, car.pac, edit.pac, CAB.pac and the XEX image's containers (the player's own files), finds every
distinct shader (XXH3 of the microcode, as shaderlib), the effect(s) it belongs to, parses each container's constant table
(CTAB), disassembles with the SDK's own disassembler (tools\\shaders\\packets\\ucode_disasm.cpp, built by
build_disasm.ps1), groups near-duplicates per effect, and writes:
  out\\s100\\shader_packets\\<effect>[.partN].md   (microcode listings: rule 1, stay under out\\s100\\, <= 80 KB each)
  docs\\renderer\\SHADER_INVENTORY.md, db\\shaders\\inventory.json  (names, counts, hashes ONLY, no microcode)
"""
import argparse
import collections
import difflib
import glob
import json
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
import pac  # noqa: E402
import shaderlib as sl  # noqa: E402

GAME = sl.GAME
OUT = os.path.join(REPO, 'out', 's100', 'shader_packets')
IMAGE = os.path.join(REPO, 'out', 'harness', 'index', 'image.bin')
PACS = [('_startup', 'pac/_startup.pac'), ('car', 'pac/car/car.pac'), ('edit', 'pac/edit/edit.pac'), ('CAB', 'pac/cab/CAB.pac')]
CAP = 80 * 1024
NL = chr(10)
REGSET = {0: 'bool', 1: 'int4', 2: 'float4', 3: 'sampler'}


def effect_table_ok(leaf):
    try:
        t = sl.effect_table(leaf)
    except Exception:
        return None
    if not t:
        return None
    for n, o, s in t:
        if not re.fullmatch(r'[A-Za-z_]\w*', n) or o + s > len(leaf) or s == 0:
            return None
    return t


def parse_ctab(b, off, vsize):
    """D3D constant table inside the container's virtual part: [(name, set, register, count)]."""
    w = struct.unpack_from('>%dI' % (vsize // 4), b, off)
    for i, x in enumerate(w):
        if x in (0xFFFE0300, 0xFFFF0300) and i >= 2 and w[i - 2] == 0x1C:
            base = off + (i - 2) * 4
            n, info = w[i + 1], w[i + 2]
            res = []
            for k in range(n):
                e = base + info + 20 * k
                if e + 20 > off + vsize:
                    break
                no, rs, ri, rc, _r, _t, _d = struct.unpack_from('>IHHHHII', b, e)
                p = base + no
                q = b.find(b'\0', p, off + vsize + 64)
                res.append((b[p:q].decode('ascii', 'replace'), REGSET.get(rs, str(rs)), ri, rc))
            return res
    return None


def literals(b, off, vsize, moff):
    n = moff // 4
    if not n:
        return []
    fl = struct.unpack_from('>%df' % n, b, off + vsize)
    return [tuple(fl[i:i + 4]) for i in range(0, n - n % 4, 4)]


def norm(text):
    out = []
    for ln in text.splitlines():
        ln = re.sub(r'^/\*\s*[\d.]+\s*\*/\s*', '', ln.strip())
        if not ln:
            continue
        out.append(re.sub(r'\b([rcoi])\d+', r'\1#', ln))
    return out


def raw_lines(text):
    return [re.sub(r'^/\*\s*[\d.]+\s*\*/\s*', '', l.strip()) for l in text.splitlines() if l.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disasm-exe', default=os.path.join(REPO, 'out', 's100', 'disasm', 'ucode_disasm.exe'))
    a = ap.parse_args()
    t0 = time.time()
    shaders = {}  # (kind, hash) -> code, ctab, lits, pacs
    effects = collections.OrderedDict()
    unparsed = []

    def add_blob(name, pacname, variant, b):
        cs = sl.containers(b)
        if not cs:
            return
        e = effects.setdefault(name, {'pacs': collections.defaultdict(set), 'shaders': collections.OrderedDict(), 'tech': set(), 'params': set(), 'blobs': 0})
        e['pacs'][pacname].add(variant)
        e['blobs'] += 1
        last_end = 0
        for off, kind, coff, code in cs:
            key = (kind, sl.xxh3(code))
            vsize, psize = struct.unpack_from('>II', b, off + 4)
            desc = struct.unpack_from('>I', b, off + 24)[0]
            moff = struct.unpack_from('>I', b, off + desc)[0]
            s = shaders.setdefault(key, {'code': code, 'ctab': parse_ctab(b, off, vsize), 'lits': literals(b, off, vsize, moff), 'pacs': set()})
            s['pacs'].add(pacname)
            e['shaders'].setdefault(key, set()).add(pacname)
            last_end = max(last_end, off + vsize + psize)
        for m in re.finditer(rb'[A-Za-z_]\w{2,}', b[last_end:]):
            st = m.group().decode()
            if st.startswith('T_'):
                e['tech'].add(st)
            elif st.startswith(('g_', 'samp')):
                e['params'].add(st)

    for pacname, rel in PACS:
        data = pac.load(os.path.join(GAME, rel.replace('/', os.sep)))
        for lname, leaf in pac.entries(data, 6):
            if b'\x10\x2a\x11' not in leaf:
                continue
            t = effect_table_ok(leaf)
            if t:
                for n, o, s in t:
                    add_blob(n, pacname, lname, leaf[o:o + s])
            else:
                unparsed.append((pacname, lname, len(leaf)))
                add_blob('%s_%s' % (pacname, lname.replace('/', '_')), pacname, lname, leaf)
    img = IMAGE if os.path.exists(IMAGE) else os.path.join(GAME, 'default.xex')
    add_blob('_xex_image', 'xex', 'image', open(img, 'rb').read())
    print('gathered %d distinct shaders, %d effects, %.0fs' % (len(shaders), len(effects), time.time() - t0))

    # disassemble each distinct shader once
    keys = list(shaders)
    buf = bytearray()
    for k in keys:
        c = shaders[k]['code']
        buf += struct.pack('<II', 0 if k[0] == 'VS' else 1, len(c) // 4) + c
    dd = os.path.join(REPO, 'out', 's100', 'disasm')
    os.makedirs(dd, exist_ok=True)
    inb, outb = os.path.join(dd, 'batch_in.bin'), os.path.join(dd, 'batch_out.bin')
    open(inb, 'wb').write(buf)
    subprocess.run([a.disasm_exe, inb, outb], check=True)
    d = open(outb, 'rb').read()
    p = 0
    for k in keys:
        n = struct.unpack_from('<I', d, p)[0]
        shaders[k]['dis'] = d[p + 4:p + 4 + n].decode('utf-8', 'replace')
        p += 4 + n
    print('disassembled, %.0fs' % (time.time() - t0))

    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, '*.md')):
        os.remove(f)
    inv = {}
    for name, e in effects.items():
        groups = []
        nl = {k: norm(shaders[k]['dis']) for k in e['shaders']}
        for k in e['shaders']:
            g0 = next((g for g in groups if g['kind'] == k[0] and g['nl'] == nl[k]), None)
            if g0:
                g0['members'].append((k, 'identical apart from register numbers'))
                continue
            placed = False
            for g in groups:
                if g['kind'] != k[0] or abs(len(nl[k]) - len(g['nl'])) > max(4, len(g['nl']) // 8):
                    continue
                sm = difflib.SequenceMatcher(None, g['nl'], nl[k], autojunk=False)
                if sm.real_quick_ratio() >= 0.85 and sm.quick_ratio() >= 0.85 and sm.ratio() >= 0.85:
                    a_, b_ = raw_lines(shaders[g['lead']]['dis']), raw_lines(shaders[k]['dis'])
                    diff = [l for l in difflib.unified_diff(a_, b_, lineterm='', n=0) if not l.startswith(('---', '+++', '@@'))]
                    more = len(diff) - 24
                    txt = '\n'.join('    ' + l for l in diff[:24]) + ('\n    ... %d more differing lines' % more if more > 0 else '')
                    g['members'].append((k, 'near (similarity %.2f); differs from lead:\n%s' % (sm.ratio(), txt)))
                    placed = True
                    break
            if not placed:
                groups.append({'kind': k[0], 'lead': k, 'nl': nl[k], 'members': [(k, 'LEAD')]})
        e['groups'] = groups
        gidx = {m[0]: i for i, g in enumerate(groups, 1) for m in g['members']}
        nvs = sum(1 for k in e['shaders'] if k[0] == 'VS')
        nps = len(e['shaders']) - nvs
        head = ['# Effect %s' % name, '',
                'pacs: %s' % ', '.join('%s (%s)' % (p_, ', '.join(sorted(v))) for p_, v in e['pacs'].items()),
                'blobs (variants): %d; distinct shaders: %d VS, %d PS; groups: %d' % (e['blobs'], nvs, nps, len(groups)), '',
                'techniques (T_ strings in the blob): %s' % (', '.join(sorted(e['tech'])) or 'none found'),
                'passes: NOT parsed (the FXL pass tables are not decoded; technique -> shader mapping unconfirmed)', '',
                'effect parameters (strings): %s' % (', '.join(sorted(e['params'])) or 'none'), '']
        blocks = []
        for gi, g in enumerate(groups, 1):
            lead = shaders[g['lead']]
            ct = lead['ctab']
            L = ['## Group %d: %s, %d shader(s); lead %016X' % (gi, g['kind'], len(g['members']), g['lead'][1]), '']
            if ct is None:
                L.append('constant table: not found')
            else:
                cons = [c for c in ct if c[1] != 'sampler']
                smp = [c for c in ct if c[1] == 'sampler']
                L.append('constants: ' + ('; '.join('%s %s c%d x%d' % c for c in cons) or 'none'))
                L.append('samplers: ' + ('; '.join('%s s%d' % (c[0], c[2]) for c in smp) or 'none'))
            if lead['lits']:
                L.append('literal constants (physical part before the microcode, vec4 each, in order): ' + ' '.join('(%s)' % ','.join('%g' % x for x in v) for v in lead['lits'][:32]) + (' ...' if len(lead['lits']) > 32 else ''))
            L += ['', '```', lead['dis'].rstrip(), '```', '', 'members:']
            for k, how in g['members']:
                L.append('- %016X %s [%s] %s' % (k[1], k[0], ','.join(sorted(e['shaders'][k])), 'lead' if how == 'LEAD' else how))
            L.append('')
            blocks.append('\n'.join(L))
        pieces = []
        for blk in blocks:  # a group with very many members is cut at line boundaries
            if len(blk) <= CAP - 4000:
                pieces.append(blk)
                continue
            piece, n = [], 0
            for ln in blk.split(NL):
                if n + len(ln) > CAP - 8000 and piece:
                    pieces.append(NL.join(piece))
                    piece, n = ['(group %d members, continued)' % int(re.match(r'## Group (\d+)', blk).group(1))], 0
                piece.append(ln)
                n += len(ln) + 1
            pieces.append(NL.join(piece))
        blocks = pieces
        parts, cur, nb = [], list(head), 0
        size = sum(len(x.encode('utf-8')) + 1 for x in cur)
        for blk in blocks:
            if size + len(blk.encode('utf-8')) > CAP and nb:
                parts.append('\n'.join(cur))
                cur = ['# Effect %s (cont.)' % name, '']
                size, nb = 0, 0
            cur.append(blk)
            nb += 1
            size += len(blk.encode('utf-8')) + 1
        parts.append('\n'.join(cur))
        pkt = []
        for i, txt in enumerate(parts, 1):
            fn = '%s.md' % name if len(parts) == 1 else '%s.part%d.md' % (name, i)
            open(os.path.join(OUT, fn), 'w', encoding='utf-8').write(txt)
            pkt.append((fn, len(txt.encode('utf-8'))))
        inv[name] = {'pacs': {p_: sorted(v) for p_, v in e['pacs'].items()}, 'vs': nvs, 'ps': nps, 'groups': len(groups),
                     'techniques': sorted(e['tech']), 'packet_files': pkt, 'packet_bytes': sum(s for _, s in pkt),
                     'hashes': sorted('%s %016X g%d' % (k[0], k[1], gidx[k]) for k in e['shaders'])}
    print('packets written, %.0fs' % (time.time() - t0))

    seen = set()
    new_by = {}
    for pn in ('_startup', 'car', 'edit', 'CAB', 'xex'):
        ks = {k for k, s in shaders.items() if pn in s['pacs']} - seen
        new_by[pn] = (len(ks), sum(1 for k in ks if k[0] == 'VS'), sum(1 for k in ks if k[0] == 'PS'))
        seen |= ks
    byhash = {k[1] for k in shaders}
    found = unknown = patched_ok = patched_bad = img_lines = 0
    bad = []
    for ln in open(os.path.join(REPO, 'db', 'shaders', 'shader_list.txt'), encoding='utf-8'):
        f = ln.split()
        if len(f) < 5 or f[0] != 'S':
            continue
        if f[4] == 'exact':
            if int(f[1], 16) in byhash:
                found += 1
            else:
                unknown += 1
                bad.append(f[1])
        elif f[4] == 'patched':
            if int(f[5], 16) in byhash:
                patched_ok += 1
            else:
                patched_bad += 1
                bad.append(f[1] + ' (patched base ' + f[5] + ')')
        else:
            img_lines += 1
    nv = sum(1 for k in shaders if k[0] == 'VS')
    tot = {'distinct_shaders_all_sources': len(shaders), 'vs': nv, 'ps': len(shaders) - nv, 'effects': len(effects),
           'groups': sum(len(e['groups']) for e in effects.values()), 'new_by_source_in_order': new_by,
           'shader_list_exact_found': found, 'shader_list_exact_unknown': unknown, 'patched_resolved': patched_ok,
           'patched_unresolved': patched_bad, 'image_lines_not_checked': img_lines, 'unresolved': bad,
           'leaves_without_effect_table': unparsed}
    json.dump({'totals': tot, 'effects': inv}, open(os.path.join(REPO, 'db', 'shaders', 'inventory.json'), 'w'), indent=1)
    L = ['# Shader inventory (Session 100)', '',
         'Names, counts and hashes only; no microcode. Generated by `py tools\\shaders\\packets\\build_packets.py`. Packets (with listings) are under `out\\s100\\shader_packets\\` (gitignored, rule 1).', '',
         '## Totals', '',
         '- distinct shaders (all sources, by XXH3 of microcode): %d (%d VS, %d PS) in %d effects, %d groups' % (len(shaders), nv, len(shaders) - nv, len(effects), tot['groups']),
         '- new per source, in order (total / VS / PS): ' + '; '.join('%s %d / %d / %d' % ((k,) + v) for k, v in new_by.items()),
         '- shader_list.txt `S exact`: %d found, %d unknown; `patched`: %d bases found, %d not; image lines (not checked here): %d' % (found, unknown, patched_ok, patched_bad, img_lines),
         '- leaves with containers but no valid effect table (grouped as pseudo-effects `<pac>_<leaf>`): %s' % (', '.join('%s %s' % (p_, n) for p_, n, _ in unparsed) or 'none'),
         '', '## Per effect', '', '| effect | pacs | VS | PS | groups | techniques | packet KB |', '|---|---|---|---|---|---|---|']
    for name, v in sorted(inv.items(), key=lambda kv: -kv[1]['packet_bytes']):
        L.append('| %s | %s | %d | %d | %d | %d | %.1f |' % (name, ', '.join(v['pacs']), v['vs'], v['ps'], v['groups'], len(v['techniques']), v['packet_bytes'] / 1024))
    L += ['', 'Hashes per effect (kind, hash, group) are in `db\\shaders\\inventory.json`.']
    os.makedirs(os.path.join(REPO, 'docs', 'renderer'), exist_ok=True)
    open(os.path.join(REPO, 'docs', 'renderer', 'SHADER_INVENTORY.md'), 'w', encoding='utf-8').write('\n'.join(L) + '\n')
    print(json.dumps({k: v for k, v in tot.items() if k != 'unresolved'}, indent=1, default=str)[:3000])
    print('unresolved:', bad[:10])
    print('done %.0fs' % (time.time() - t0))


if __name__ == '__main__':
    main()

"""Session 98 step 16: the naming table behind "click to inspect" in the Arena Viewer.

Lists every drawn piece of every arena (bgNN.pac) and wrestler (chNNN.pac) - source (pac + slot / key / entry), first node
name, mesh material class, texture names, triangles, flags (r / y / a / s / m) - and checks the friendly-name rules of
src\\assets\\part_names.h against it (the rules live in RULES below and are mirrored by hand into the header; --check-header
verifies both agree on a sample).

  py -I tools\\assets\\part_names.py --arenas [--ids 5] [--wrestlers N] [--out DIR] [--game DIR]

Reads the player's own game files, read-only. Writes a markdown report (default: <main checkout>\\out\\runtime\\s98\\part_names.md)
with frequency tables and the coverage numbers. Nothing from the game is committed: the report stays under out\\.
Reuses tools\\wm30\\pac.py (XCTD unpack through out\\tools\\xctd_unpack.exe, PACH walking) and the JBOY layout of
src\\assets\\jboy.h (body-relative offsets, mesh record 0xB8, strips).
"""
import argparse
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# A builder worktree has no game_data / out: fall back to the main checkout.
MAIN = os.environ.get('WWE_MAIN', r'C:\Users\Jarrod\wwe2k14-recomp')
if not os.path.isdir(os.path.join(REPO, 'game_data')):
    ROOT = MAIN
else:
    ROOT = REPO
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import pac  # noqa: E402

pac.REPO = ROOT
pac.GAME = os.path.join(ROOT, 'game_data', 'extracted')
pac.UNPACK = os.path.join(ROOT, 'out', 'tools', 'xctd_unpack.exe')
pac.CACHE = os.path.join(ROOT, 'out', 'wm30', 'cache')


# ---------------------------------------------------------------------------------------------------------------- JBOY

def cstr(b):
    return b.split(b'\0', 1)[0].decode('latin1')


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def parse_jboy(blob):
    """-> dict(nodes=[names], textures=[names], meshes=[dict(verts, tris, shader, diffuse, normal, lightmap, mask, params)]) or None"""
    if blob[:4] != b'JBOY' or len(blob) < 0x48:
        return None
    body = blob[8:8 + be32(blob, 4)]
    nmesh, mesh_tab, nnode, ntex, node_tab, tex_tab = (be32(body, o) for o in (0x10, 0x14, 0x18, 0x1C, 0x20, 0x24))
    if nmesh > 512 or nnode > 4096 or ntex > 256:
        return None
    nodes = [cstr(body[node_tab + 80 * i:node_tab + 80 * i + 16]) for i in range(nnode)]
    texs = [cstr(body[tex_tab + 16 * i:tex_tab + 16 * i + 16]) for i in range(ntex)]
    meshes = []
    for mi in range(nmesh):
        rec = mesh_tab + 0xB8 * mi
        n = be32(body, rec)
        if n == 0 or n > 400000:
            continue
        shader = cstr(body[rec + 0x7C:rec + 0x8C])
        ib = be32(body, rec + 0x9C)
        tris = 0
        try:
            first = be32(body, ib + 8)
            ng = (first - ib) // 12
            for g in range(ng):
                cnt, ptr = be32(body, ib + 12 * g + 4), be32(body, ib + 12 * g + 8)
                idx = struct.unpack_from('>%dH' % cnt, body, ptr)
                run = 0
                for i in range(len(idx)):
                    if idx[i] == 0xFFFF:
                        run = 0
                        continue
                    run += 1
                    if run < 3:
                        continue
                    a, b, c = idx[i - 2], idx[i - 1], idx[i]
                    if a >= n or b >= n or c >= n or a == b or b == c or a == c:
                        continue
                    tris += 1
        except struct.error:
            pass
        npal = min(be32(body, rec + 8), 20)
        palette = []
        for k in range(npal):
            pv = be32(body, rec + 0x0C + 4 * k)
            if 1 <= pv <= nnode:
                palette.append(nodes[pv - 1])
        params = {}
        nparam, plist = be32(body, rec + 0x94), be32(body, rec + 0x98)
        for k in range(min(nparam, 512)):
            try:
                po = be32(body, plist + 4 * k)
                name = cstr(body[po:po + 16])
                kind = be32(body, po + 16) >> 16
                if kind == 0x0F:
                    ti = struct.unpack_from('>i', body, po + 20)[0]
                    params[name] = texs[ti] if 0 <= ti < len(texs) else None
            except struct.error:
                pass
        diffuse = params.get('g_mDiffusePrm') or params.get('sampDiffusePrm')
        meshes.append(dict(verts=n, tris=tris, shader=shader, diffuse=diffuse, palette=palette,
                           normal=params.get('g_mNormalFxPrm') or params.get('sampNormalPrm'),
                           lightmap=params.get('sampLightMapPrm'), mask=params.get('g_mMaskPrm'),
                           samplers=sorted(k for k in params)))
    return dict(nodes=nodes, textures=texs, meshes=meshes)


# ---------------------------------------------------------------------------------------------------------------- flags

def parse_flags(text):
    """Entry 50001: lines '<slot> <flags...> ; comment'. -> {slot: set(tokens)} plus l(N), m(N)."""
    out = {}
    for line in text.replace('\r', '').split('\n'):
        m = re.match(r'\s*(\d+)\b(.*)$', line)
        if not m:
            continue
        slot = int(m.group(1))
        body = m.group(2).split(';', 1)[0]
        toks = set(body.split())
        for t in re.findall(r'[lmf]\([^)]*\)', body):
            toks.add(t)
        out.setdefault(slot, set()).update(toks)
    return out


# ---------------------------------------------------------------------------------------------------------------- rules
# The friendly-name rules. Order matters: first match wins. Each rule: (category, friendly, tests) where tests are
# regexes applied to the lower-cased "haystack" of the piece: node name, diffuse texture, other texture names, material.
# Keep in step with src\assets\part_names.h (same table, same order).
# (field, regex) pairs: f = first node name, t = first/diffuse texture name, o = any texture name, m = material class,
# s = kind/source label ('slot 5', 'crowd', 'ring', 'prop').
sys.path.insert(0, HERE)
from part_rules import RULES, Rule, classify  # noqa: E402,F401  (the rules table lives in part_rules.py)


def load_pac(path):
    return pac.load(path)


def arena_pieces(arena_id):
    """-> list of dict(source, key, node, tex, textures, tris, verts, shader, flags) for bgNN.pac (stage slots + ring set + stage keys +
    crowd bodies) and bgEtc props that the loader adds."""
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    if not os.path.exists(path):
        return []
    data = load_pac(path)
    start = 0x4000 if data[:4] == b'EPAC' else 0
    ents = dict()
    # first PACH
    p = start
    while p + 8 <= len(data) and data[p:p + 4] != b'PACH':
        p += 0x800
    if data[p:p + 4] != b'PACH':
        return []
    lst = pac.pach(data, p)
    ents = {}
    for eid, raw in lst:
        ents.setdefault(eid, raw)
    flags = {}
    if 50001 in ents:
        flags = parse_flags(pac.decode(ents[50001]).decode('latin1'))
    out = []
    keys = list(range(0, 400)) + [951, 952, 953, 960, 964, 973, 974] + list(range(800, 810))
    for key in keys:
        if key not in ents:
            continue
        blob = pac.decode(ents[key])
        j = parse_jboy(blob)
        if not j:
            continue
        src = 'stagekey' if 800 <= key <= 809 else 'ring' if key >= 951 else 'slot'
        fl = sorted(flags.get(key, ()))
        for me in j['meshes']:
            out.append(dict(pac='bg%02d.pac' % arena_id, kind='arena', src=src, key=key, additive='a' in fl,
                            mirrored=('r' in fl) or ('y' in fl), arena=arena_id, node=j['nodes'][0] if j['nodes'] else '',
                            nodes=j['nodes'], tex=me['diffuse'] or '', palette=me['palette'], textures=[t for t in j['textures']],
                            tris=me['tris'], verts=me['verts'], shader=me['shader'],
                            flags=sorted(flags.get(key, ()))))
    # crowd bodies (20000 + type): one piece per body; the seat text is not needed for names
    if 20000 in ents:
        nb = pac.decode(ents[20000])
        if nb[:4] == b'PACH':
            for eid, raw in pac.pach(nb):
                if 20000 <= eid < 20100:
                    j = parse_jboy(pac.decode(raw))
                    if not j:
                        continue
                    for me in j['meshes']:
                        out.append(dict(pac='bg%02d.pac' % arena_id, kind='crowd', key=eid,
                                        node=j['nodes'][0] if j['nodes'] else '', nodes=j['nodes'], tex=me['diffuse'] or '', palette=me['palette'],
                                        textures=j['textures'], tris=me['tris'], verts=me['verts'], shader=me['shader'],
                                        flags=[]))
    return out


def props_pieces():
    """bgEtc.pac PACH 9 entries the loader draws: 29/32/35 steps, 6/7 desk."""
    path = os.path.join(pac.GAME, 'pac', 'bg', 'bgEtc.pac')
    data = load_pac(path)
    start = 0x4000 if data[:4] == b'EPAC' else 0
    # walk PACH archives
    pos, tops = start, []
    while pos + 8 <= len(data):
        if data[pos:pos + 4] == b'PACH':
            cnt = struct.unpack_from('<I', data, pos + 4)[0]
            lst = pac.pach(data, pos)
            end = pos + 8 + 12 * cnt + max([struct.unpack_from('<II', data, pos + 8 + 12 * i + 4)[0] +
                                           struct.unpack_from('<II', data, pos + 8 + 12 * i + 4)[1] for i in range(cnt)] + [0])
            tops.append(lst)
            pos = (end + 0x7FF) & ~0x7FF
        else:
            pos += 0x800
    out = []
    if len(tops) > 9:
        ents = dict(tops[9])
        for key in (6, 7, 29, 32, 35):
            if key not in ents:
                continue
            nb = pac.decode(ents[key])
            if nb[:4] != b'PACH':
                continue
            sub = dict(pac.pach(nb))
            if 0 not in sub:
                continue
            j = parse_jboy(pac.decode(sub[0]))
            if not j:
                continue
            for me in j['meshes']:
                out.append(dict(pac='bgEtc.pac', kind='prop', key=key, node=j['nodes'][0] if j['nodes'] else '',
                                nodes=j['nodes'], tex=me['diffuse'] or '', palette=me['palette'], textures=j['textures'], tris=me['tris'],
                                verts=me['verts'], shader=me['shader'], flags=[]))
    return out


def wrestler_pieces(wid, attire=0):
    path = os.path.join(pac.GAME, 'pac', 'ch', 'ch%03d.pac' % wid)
    if not os.path.exists(path):
        return []
    data = load_pac(path)
    start = 0x4000 if data[:4] == b'EPAC' else 0
    pos, pachs = start, []
    while pos + 8 <= len(data):
        if data[pos:pos + 4] == b'PACH':
            cnt = struct.unpack_from('<I', data, pos + 4)[0]
            lst = pac.pach(data, pos)
            end = pos + 8 + 12 * cnt + max([struct.unpack_from('<II', data, pos + 8 + 12 * i + 4)[0] +
                                           struct.unpack_from('<II', data, pos + 8 + 12 * i + 4)[1] for i in range(cnt)] + [0])
            pachs.append(lst)
            pos = (end + 0x7FF) & ~0x7FF
        else:
            pos += 0x800
    models = [l for l in pachs if any(e == 0 for e, _ in l)]
    if attire >= len(models):
        return []
    ents = {}
    for eid, raw in models[attire]:
        ents.setdefault(eid, raw)
    out = []
    for key in sorted(ents):
        if not (key in (0, 26, 27) or 10000 <= key < 40000):
            continue
        j = parse_jboy(pac.decode(ents[key]))
        if not j:
            continue
        for me in j['meshes']:
            out.append(dict(pac='ch%03d.pac' % wid, kind='wrestler', key=key, node=j['nodes'][0] if j['nodes'] else '',
                            nodes=j['nodes'], tex=me['diffuse'] or '', palette=me['palette'], textures=j['textures'], tris=me['tris'],
                            verts=me['verts'], shader=me['shader'], flags=[], attire=attire))
    return out



# ---------------------------------------------------------------------------------------------------------------- report

def all_arena_ids():
    d = os.path.join(pac.GAME, 'pac', 'bg')
    return sorted(int(f[2:4]) for f in os.listdir(d) if re.match(r'bg\d\d\.pac$', f))


def all_wrestler_ids():
    d = os.path.join(pac.GAME, 'pac', 'ch')
    return sorted(int(f[2:5]) for f in os.listdir(d) if re.match(r'ch\d\d\d\.pac$', f))


def prefix_of(name):
    """Frequency-table key: lower case, digit runs -> #, trailing single-letter suffix kept."""
    return re.sub(r'\d+', '#', name.lower())


def coverage(pieces):
    """-> (named tris, total tris) counting every mesh once as stored."""
    named = total = 0
    for p in pieces:
        c, f, i = classify(p)
        total += p['tris']
        if c != 'Unknown':
            named += p['tris']
    return named, total


def emit_header(path):
    """Write the generated rule table into src\\assets\\part_names.h between the GENERATED markers."""
    kinds = {'any': 0, 'arena': 1, 'wrestler': 2, 'crowd': 3, 'prop': 4}

    def q(s):
        return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    rows = []
    for r in RULES:
        rows.append('    {%s, %s, %d, %d, %d, %s, %s, %s, %s, %d},' % (q(r.cat), q(r.name), kinds[r.kind], r.lo, r.hi, q(r.node), q(r.tex),
                                                                      q(r.shader), q(r.pal), r.add))
    text = open(path, encoding='utf-8').read()
    a, b = '// ---- GENERATED RULES BEGIN', '// ---- GENERATED RULES END'
    i, j = text.index(a), text.index(b)
    head = text[:i + len(a)]
    head = head[:head.rindex('\n') + 1] if False else head
    new = text[:text.index('\n', i) + 1] + '\n'.join(rows) + '\n' + text[j:]
    open(path, 'w', encoding='utf-8', newline='\n').write(new)
    return len(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ids', default='', help='arena ids (default: all)')
    ap.add_argument('--wrestlers', type=int, default=0, help='how many wrestlers (attire 0) to scan (default 0 = a spread of 12)')
    ap.add_argument('--out', default=os.path.join(MAIN, 'out', 'runtime', 's98', 'part_names.md'))
    ap.add_argument('--emit-header', default='', help='path of part_names.h: regenerate its rule table from part_rules.py')
    ap.add_argument('--game', default='')
    ap.add_argument('--crosscheck', default='', help='write a TSV of distinct inputs + Python answers for part_names_test.cpp')
    a = ap.parse_args()
    if a.game:
        pac.GAME = a.game
    if a.emit_header:
        print('rules written:', emit_header(a.emit_header))
        return 0
    ids = [int(x) for x in a.ids.split(',') if x.strip()] or all_arena_ids()
    props = props_pieces()
    L = ['# Part names (Session 98 step 16)', '',
         'Generated by `tools\\assets\\part_names.py` from the player\'s own files; the friendly-name rules are `tools\\assets\\part_rules.py` '
         '(mirrored into `src\\assets\\part_names.h`). Triangles are counted once per stored mesh (the crowd and the mirrored / '
         'repeated pieces are drawn many times, so this is the loader\'s mesh list, not the screen count).', '']
    per_arena = {}
    all_pieces = []
    freq = collections.Counter()
    freq_tex = collections.Counter()
    cats = collections.Counter()
    friendly = collections.Counter()
    unknown = collections.Counter()
    unknown_ex = {}
    for aid in ids:
        ps = arena_pieces(aid) + [dict(x, arena=aid) for x in props]
        per_arena[aid] = ps
        for p in ps:
            c, f, ri = classify(p)
            p['cat'], p['friendly'], p['rule'] = c, f, ri
            freq[(p['kind'] if p['kind'] != 'arena' else p.get('src', 'slot'), prefix_of(p['node']))] += p['tris']
            freq_tex[prefix_of(p['tex'])] += p['tris']
            cats[c] += p['tris']
            friendly[(c, f)] += p['tris']
            if c == 'Unknown':
                k = (prefix_of(p['node']), prefix_of(p['tex']))
                unknown[k] += p['tris']
                unknown_ex.setdefault(k, 'bg%02d slot %d' % (aid, p['key']))
        all_pieces += ps
    # ---- coverage
    L += ['## Coverage (share of triangles with a non-Unknown friendly name)', '']
    L += ['| arena | meshes | tris | named | named without crowd |', '|---|---|---|---|---|']
    avg_all, avg_nc = [], []
    for aid in ids:
        ps = per_arena[aid]
        n, t = coverage(ps)
        nc = [p for p in ps if p['kind'] != 'crowd']
        n2, t2 = coverage(nc)
        avg_all.append(100.0 * n / max(t, 1))
        avg_nc.append(100.0 * n2 / max(t2, 1))
        L.append('| bg%02d | %d | %d | %.1f %% | %.1f %% |' % (aid, len(ps), t, avg_all[-1], avg_nc[-1]))
    L += ['', '**Average over %d arenas: %.1f %% (all meshes), %.1f %% (without crowd bodies).**' % (len(ids), sum(avg_all) / len(avg_all),
                                                                                                   sum(avg_nc) / len(avg_nc)), '']
    # ---- wrestlers
    wids = all_wrestler_ids()
    sel = wids[::max(1, len(wids) // (a.wrestlers or 12))][:(a.wrestlers or 12)] if wids else []
    wp = []
    for w in sel:
        for ps in [wrestler_pieces(w, 0)]:
            for p in ps:
                c, f, ri = classify(p)
                p['cat'], p['friendly'], p['rule'] = c, f, ri
            wp += ps
    wn, wt = coverage(wp)
    L += ['## Wrestler pieces (attire 0 of %d wrestlers: %s)' % (len(sel), ', '.join('ch%03d' % w for w in sel)), '',
          'Named: %.1f %% of %d triangles.' % (100.0 * wn / max(wt, 1), wt), '']
    wf = collections.Counter()
    for p in wp:
        wf[(p['cat'], p['friendly'])] += p['tris']
    L += ['| category | friendly name | tris |', '|---|---|---|']
    L += ['| %s | %s | %d |' % (c, f, t) for (c, f), t in wf.most_common()]
    L.append('')
    # ---- categories
    L += ['## Categories (arenas, triangles)', '', '| category | tris |', '|---|---|']
    L += ['| %s | %d |' % (c, t) for c, t in cats.most_common()]
    L += ['', '## Friendly names (arenas)', '', '| category | friendly name | tris |', '|---|---|---|']
    L += ['| %s | %s | %d |' % (c, f, t) for (c, f), t in friendly.most_common()]
    # ---- what each rule caught (for review)
    ex = collections.defaultdict(collections.Counter)
    for p in all_pieces:
        ex[p['rule']][(prefix_of(p['node']), prefix_of(p['tex']))] += p['tris']
    L += ['', '## Rules: what each caught (top 5 node / texture pairs)', '', '| # | category | friendly name | tris | examples |', '|---|---|---|---|---|']
    for i, r in enumerate(RULES):
        if i in ex:
            L.append('| %d | %s | %s | %d | %s |' % (i, r.cat, r.name, sum(ex[i].values()),
                                                   '; '.join('`%s` / `%s`' % k for k, _ in ex[i].most_common(5))))
    # ---- unknowns
    L += ['', '## Still Unknown (arenas): node prefix / texture prefix / tris / example', '', '| node | texture | tris | example |', '|---|---|---|---|']
    L += ['| `%s` | `%s` | %d | %s |' % (n, t, v, unknown_ex[(n, t)]) for (n, t), v in unknown.most_common(60)]
    # ---- frequency tables
    L += ['', '## Name-prefix frequency (node names, digits -> #, by triangles over all scanned arenas)', '', '| source | node prefix | tris |', '|---|---|---|']
    L += ['| %s | `%s` | %d |' % (k, n, v) for (k, n), v in freq.most_common(120)]
    L += ['', '## Diffuse texture prefix frequency (top 80)', '', '| texture | tris |', '|---|---|']
    L += ['| `%s` | %d |' % (t, v) for t, v in freq_tex.most_common(80)]
    # ---- arena 5 full listing
    if 5 in per_arena:
        L += ['', '## Arena 5 (bg05): every drawn piece', '', '| source | node | texture | tris | flags | friendly | category |', '|---|---|---|---|---|---|---|']
        for p in per_arena[5]:
            if p['kind'] == 'crowd' and p['key'] >= 20040:
                continue
            L.append('| %s %d | `%s` | `%s` | %d | %s | %s | %s |' % (p['pac'], p['key'], p['node'], p['tex'], p['tris'], ' '.join(p.get('flags', [])),
                                                                     p['friendly'], p['cat']))
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, 'w', encoding='utf-8').write('\n'.join(L) + '\n')
    print('wrote', a.out)
    if a.crosscheck:  # one line per distinct input, Python's answer: the C++ test (tools\assets\part_names_test.cpp) must agree
        seen = {}
        for p in all_pieces + wp:
            k = (p['kind'], p['key'], p['node'], p['tex'], p['shader'], ';'.join(p.get('palette') or []), int(bool(p.get('additive'))))
            seen[k] = (p['friendly'], p['cat'])
        with open(a.crosscheck, 'w', encoding='utf-8', newline='\n') as f:
            for k, (fr, c) in seen.items():
                f.write('\t'.join([k[0], str(k[1]), k[2], k[3], k[4], k[5], str(k[6]), fr, c]) + '\n')
        print('crosscheck lines:', len(seen))
    print('average named: %.1f %% (all), %.1f %% (no crowd); arena 5: %.1f %%; wrestlers: %.1f %%' % (
        sum(avg_all) / len(avg_all), sum(avg_nc) / len(avg_nc),
        avg_all[ids.index(5)] if 5 in ids else -1, 100.0 * wn / max(wt, 1)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

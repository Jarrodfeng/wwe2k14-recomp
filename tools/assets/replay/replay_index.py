"""Frame-replay harness, the index (Session 98, stage S2; design out\\s98\\replay\\design.md section B). System Python 3.12:

    py tools\\assets\\replay\\replay_index.py [--dir out\\s98\\replay] [rp_stage rp_ramp rp_ringin rp_ring]

Per capture folder <dir>\\<name>\\ it reads scan.json (rdc_extract.py scan) and, when present, dump.json (rdc_extract.py dump),
joins the capture's guest draws with the draw probe (<dir>\\probe.csv: the probe frame whose constants match the capture
best), and writes:
  index.json   every draw: event, guest draw, VS / PS guest hash, material class, the float constants of both stages in the
               game's register numbering (c0..c255 of each stage; the pixel bank is the probe's 256..511), textures (fetch
               slot, resource, size, format, the guest fetch constant), sampler, vertex / index data (address, stride,
               vertices, md5), blend / depth / cull, colour + depth targets, the viewer mesh it maps to (Orton, ch161 attire
               PACH 1); the pass list (frame anatomy); the saved files (dump.json)
  vs_banks.json  the 256-register vertex banks (skinned / indexed shaders), deduplicated by md5
  keys.json    what rdc_extract.py dump saves: Orton's draws' textures and targets, the eye draw's before / after / coverage /
               pixel history, each pass's last image, the post combine and the presented image
and <dir>\\index_summary.md. Local only (out\\ is gitignored): game imagery and data.
"""
import argparse
import array
import bisect
import collections
import csv
import difflib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'assets'))
import replay_common as RC  # noqa: E402

ORTON, ORTON_ATTIRE = 161, 1
EYE_PS = 'BBB2E028DCF36E8D'


# ------------------------------------------------------------------------------------------------ material classes

def material_table():
    """Guest PS hash -> (class, name), from our own src\\scene_swap.h (one table, ours)."""
    path = os.path.join(REPO, 'src', 'scene_swap.h')
    text = open(path, encoding='utf-8', errors='replace').read()
    out = {}
    named = {
        'kWrestlerBodyMatch': ('wrestler gear', 'body / gear material (match)'),
        'kWrestlerBodyEntrance': ('wrestler gear', 'body / gear material (entrance)'),
        'kWrestlerSkinMatch': ('wrestler skin', 'skin (match)'),
        'kWrestlerSkinEntrance': ('wrestler skin', 'skin (entrance)'),
        'kWrestlerHeadMatch': ('wrestler skin head', 'head skin (match)'),
        'kWrestlerHeadEntrance': ('wrestler skin head', 'head skin (entrance)'),
        'kWrestlerHair': ('wrestler hair', 'hair / anisotropic'),
        'kWrestlerSkinDecal': ('wrestler skin', 'skin decal (match)'),
        'kWrestlerSkinDecalEntrance': ('wrestler skin', 'skin decal (entrance)'),
        'kWrestlerSkinPlain': ('wrestler skin', 'skin plain (match)'),
        'kWrestlerSkinPlainEntrance': ('wrestler skin', 'skin plain (entrance)'),
        'kCrowdMatch': ('crowd', 'crowd (match)'),
        'kCrowdEntrance': ('crowd', 'crowd (entrance)'),
    }
    for m in re.finditer(r'inline constexpr uint64_t (k\w+) = 0x([0-9A-Fa-f]{16})ull;', text):
        if m.group(1) in named:
            out[m.group(2).upper()] = named[m.group(1)]
    for m in re.finditer(r'\{0x([0-9A-Fa-f]{16})ull,\s*(\w+)_spv,[^{}]*?"([^"]+)"\}', text, re.S):
        h, sym, name = m.group(1).upper(), m.group(2), m.group(3)
        if h in out:
            continue
        low = name.lower()
        if sym.startswith('wrestler_reflect'):
            cls = 'wrestler eyes / reflect'
        elif sym.startswith('wrestler_trans'):
            cls = 'wrestler trans (hair / lids)'
        elif sym.startswith('wrestler_skin'):
            cls = 'wrestler skin'
        elif sym.startswith('arena') or 'yprop' in low or 'ybg' in low:
            cls = 'arena / stage' + (' lightmap' if 'lit' in low else '')
        elif 'crowd' in low or 'audience' in low:
            cls = 'crowd'
        elif 'movie' in low or 'titantron' in low:
            cls = 'screen / movie'
        elif 'sprite' in low:
            cls = 'sprite / overlay'
        elif any(k in low for k in ('bloom', 'depth of field', 'glare')):
            cls = 'post'
        elif any(k in low for k in ('shadow', 'rim light', 'prelight', 'lighting cubes', 'sweat', 'liquid')):
            cls = 'lighting build (shadow / rim / cubes)'
        elif any(k in low for k in ('blur', 'downscale')):
            cls = 'blur / downscale'
        else:
            cls = 'other'
        out[h] = (cls, name)
    return out


# ------------------------------------------------------------------------------------------------ probe

def _num(s):
    try:
        return float(s)
    except ValueError:   # MSVC prints '-nan(ind)', '1.#INF' ...
        s = s.lower()
        return float('-inf' if s.startswith('-') and 'inf' in s else 'inf' if 'inf' in s else 'nan')


class Probe:
    def __init__(self, path):
        self.frames = collections.OrderedDict()   # swap -> list of rows
        csv.field_size_limit(1 << 30)
        with open(path, newline='') as f:
            r = csv.reader(f)
            hdr = next(r)
            H = {k: i for i, k in enumerate(hdr)}
            for row in r:
                c = array.array('f', map(_num, row[H['consts']].split()))
                self.frames.setdefault(int(row[0]), []).append({
                    'draw': int(row[H['draw']]), 'vs': row[H['vs_hash']].upper(), 'count': int(row[H['index_count']]),
                    'vfetch_base': row[H['vfetch_base']], 'vfetch_bytes': int(row[H['vfetch_bytes']]), 'consts': c})


def stage_regs(st, banks, info):
    """{register: [x, y, z, w]} for one stage of a draw, in the game's numbering, or (None, why)."""
    if not st:
        return None, 'no shader'
    fl = banks.get(st['float_bank']) if st.get('float_bank') else st.get('floats')
    if fl is None:
        return None, 'no float constants'
    n = len(fl) // 4
    regs = RC.float_register_map(info, n)
    if regs is None:
        return None, 'register map unknown (%d packed, ucode lists %s)' % (n, len(info['regs']) if info else 'none')
    return {r: fl[4 * k:4 * k + 4] for k, r in enumerate(regs)}, None


def close(a, b):
    return all(abs(x - y) <= 1e-5 * max(1.0, abs(x), abs(y)) for x, y in zip(a, b))


# ------------------------------------------------------------------------------------------------ Orton's meshes

def part_of(p):
    pal = ' '.join(p.get('palette') or [])
    sh = p['shader']
    if sh == 'yCh_Reflect' and 'eye' in pal:
        return 'eyes'
    if sh.startswith('yCh_trans'):
        return 'lids / brows (trans)'
    if sh.startswith('yCh_hair'):
        return 'hair'
    if sh.startswith('yCh_skin'):
        if any(k in pal for k in ('kuchi', 'hoho', 'meziri', 'hana')):
            return 'head'
        if 'Neck' in pal and 'Bip Head' in pal and 'Clavicle' not in pal:
            return 'body (neck)'
        if 'Hand' in pal and 'Finger' in pal:
            return 'body (hand)'
        return 'body'
    if sh.startswith('yCh_gear') or sh.startswith('yCh_'):
        if 'Foot' in pal:
            return 'boots'
        if 'Wrist' in pal or 'Hand' in pal:
            return 'arm gear (wrist / hand)'
        if 'Clavicle' in pal:
            return 'shirt / torso gear'
        if 'Thigh' in pal:
            return 'trunks'
        return 'gear'
    return 'other'


def orton_pieces():
    try:
        import part_names as PN
        ps = PN.wrestler_pieces(ORTON, ORTON_ATTIRE)
    except Exception as e:
        print('replay_index: no viewer pieces for ch%03d (%s)' % (ORTON, e))
        return []
    out = []
    for i, p in enumerate(ps):
        out.append({'mesh': i, 'key': p['key'], 'node': p['node'], 'shader': p['shader'], 'verts': p['verts'],
                    'tris': p['tris'], 'bones': len(p.get('palette') or []), 'palette': (p.get('palette') or [])[:6],
                    'part': part_of(p)})
    return out


def map_orton(draws, pieces, window=0x100000):
    """Viewer mesh <- guest vertex buffer: the address window holding the most of Orton's vertex counts; inside it, meshes
    with the same count are given out in address order (the file order: the loader places them in order) [order assumed]."""
    want = collections.Counter(p['verts'] for p in pieces)
    by_addr = {}
    for d in draws:
        vf = d.get('vfetch')
        if vf and vf.get('vertices') in want:
            by_addr.setdefault(int(vf['address'], 16), vf['vertices'])
    addrs = sorted(by_addr)
    best = None   # (score, -span, lo, hi): the most counts matched, then the tightest address span
    for i, lo in enumerate(addrs):
        j = bisect.bisect_left(addrs, lo + window)
        got = collections.Counter()
        for k in range(i, j):   # grow the window one buffer at a time: the tightest span reaching each score
            got[by_addr[addrs[k]]] += 1
            score = sum((got & want).values())
            cand = (score, -(addrs[k] - lo), lo, addrs[k])
            if best is None or cand > best:
                best = cand
    if best is None:
        return {}, 0
    inwin = [a for a in addrs if best[2] <= a <= best[3]]
    # equal counts: the loader lays the meshes out in file order or in reverse; the meshes with unique counts tell which
    uniq = [(p['mesh'], a) for p in pieces if want[p['verts']] == 1 for a in inwin if by_addr[a] == p['verts']]
    uniq.sort(key=lambda t: t[1])
    rising = sum(1 for x, y in zip(uniq, uniq[1:]) if y[0] > x[0])
    reverse = rising < (len(uniq) - 1) / 2.0
    out = {}
    for verts in want:
        ps = [p for p in pieces if p['verts'] == verts]
        ads = sorted((a for a in inwin if by_addr[a] == verts), reverse=reverse)
        for p, a in zip(ps, ads):
            out['%08X' % a] = dict(p, ambiguous=len(ps) > 1 or len(ads) > 1, layout='reverse file order' if reverse else 'file order')
    # a piece kept in another pack entry (e.g. the lids, key 20002) lives outside the window: taken when its count is unique
    found = set(m['mesh'] for m in out.values())
    for p in pieces:
        if p['mesh'] in found:
            continue
        ads = [a for a in addrs if by_addr[a] == p['verts']]
        if len(ads) == 1 and sum(1 for q in pieces if q['verts'] == p['verts']) == 1:
            out['%08X' % ads[0]] = dict(p, ambiguous=False, layout='outside the window: the only buffer of this count in the frame')
    return out, best[0]


# ------------------------------------------------------------------------------------------------ one capture

def index_capture(cdir, name, probe, mats, pieces):
    scan = json.load(open(os.path.join(cdir, 'scan.json')))
    shaders = os.path.join(os.path.dirname(cdir), 'shaders')
    ucache = {}

    def info(guest, stage):
        k = (guest, stage)
        if k not in ucache:
            p = os.path.join(shaders, 'shader_%s.ucode.%s' % (guest, 'vert' if stage == 'vs' else 'frag'))
            ucache[k] = RC.ucode_info(p) if guest and os.path.exists(p) else None
        return ucache[k]

    banks = scan['vs_banks']
    tex = {int(k): v for k, v in scan['textures'].items()}
    draws = scan['draws']
    guest = [d for d in draws if d.get('vs') and d['vs'].get('guest')]
    # ---- join with the probe: the frame whose draws line up best and whose constants equal the capture's
    best = None
    if probe:
        cap_keys = [(d['vs']['guest'], d['count']) for d in guest]
        for swap, rows in probe.frames.items():
            sm = difflib.SequenceMatcher(None, cap_keys, [(r['vs'], r['count']) for r in rows], autojunk=False)
            pairs = []
            for blk in sm.get_matching_blocks():
                pairs += [(blk.a + k, blk.b + k) for k in range(blk.size)]
            eq = tot = 0
            for ai, bi in pairs[::max(1, len(pairs) // 300)]:
                d, r = guest[ai], rows[bi]
                for st, off in (('vs', 0), ('ps', 256)):
                    regs, _ = stage_regs(d.get(st), banks, info(d[st]['guest'] if d.get(st) else None, st))
                    if regs:
                        for reg, v in regs.items():
                            tot += 1
                            eq += close(v, r['consts'][4 * (off + reg):4 * (off + reg) + 4])
            score = (eq / tot if tot else 0, len(pairs))
            if best is None or score > best[0]:
                best = (score, swap, dict((a, b) for a, b in pairs))
    probe_rows = probe.frames[best[1]] if best else []
    pmap = best[2] if best else {}
    gpos = {id(d): k for k, d in enumerate(guest)}
    # ---- Orton
    # ---- per draw
    vs_out_banks = {}
    rows = []
    for d in draws:   # vertex fetch from the fetch constants (scans before the vf<95 - n> fix decoded the wrong pair)
        vi = info(d['vs']['guest'], 'vs') if d.get('vs') and d['vs'].get('guest') else None
        old = d.get('vfetch') or {}
        d['vfetch'] = None
        if vi and vi['vfetch'] and d.get('fetch'):
            slot = min(vi['vfetch'])
            vf = RC.vertex_fetch(d['fetch'], slot)
            if vf:
                stride = vi['vfetch'][slot] * 4
                d['vfetch'] = {'slot': slot, 'address': '%08X' % vf[0], 'bytes': vf[1], 'stride': stride,
                               'vertices': vf[1] // stride if stride else None, 'slots': len(vi['vfetch'])}
                if old.get('address') == d['vfetch']['address'] and old.get('md5'):
                    d['vfetch']['md5'], d['vfetch']['head'] = old['md5'], old.get('head')
    omap, oscore = map_orton(draws, pieces) if pieces else ({}, 0)
    for d in draws:
        g = gpos.get(id(d))
        pr = probe_rows[pmap[g]] if g is not None and g in pmap else None
        e = {'event': d['event'], 'n': d['n'], 'call': d['call'], 'count': d['count'], 'instances': d['instances'],
             'guest_draw': pr['draw'] if pr else None}
        for st in ('vs', 'ps'):
            s = d.get(st)
            e[st] = s['guest'] if s else None
            e[st + '_spirv'] = s['spv'] if s else None
        cls, mname = mats.get(e['ps'] or '', (None, None))
        if cls is None:
            if not e['ps'] and e['vs']:
                cls = 'depth only (no pixel shader)'
            elif e['vs'] is None:
                cls = 'host (render-target transfer / clear / present)'
            else:
                cls = 'unclassified'
        e['class'], e['material'] = cls, mname
        for st, off in (('vs', 0), ('ps', 256)):
            s = d.get(st)
            regs, why = stage_regs(s, banks, info(e[st], st))
            if regs is None:
                e[st + '_consts'] = None
                e[st + '_consts_note'] = why
                continue
            match = None
            if pr:
                ok = sum(close(v, pr['consts'][4 * (off + r):4 * (off + r) + 4]) for r, v in regs.items())
                match = '%d/%d' % (ok, len(regs))
            if s.get('float_bank'):
                vs_out_banks.setdefault(s['float_bank'], {'c%d' % r: v for r, v in regs.items()})
                e[st + '_consts'] = {'bank': s['float_bank']}
            else:
                e[st + '_consts'] = {'c%d' % r: [round(x, 7) for x in v] for r, v in regs.items()}
            e[st + '_consts_probe_equal'] = match
        texs = []
        for st in ('vs', 'ps'):
            s = d.get(st) or {}
            for t in s.get('textures', []):
                ti = tex.get(t['id'], {})
                texs.append({'stage': st, 'fetch': t['fetch'], 'binding': t['binding'], 'id': t['id'], 'w': ti.get('w'),
                             'h': ti.get('h'), 'fmt': ti.get('fmt'), 'mips': ti.get('mips'), 'arr': ti.get('arr'),
                             'cube': ti.get('cube'), 'guest': RC.texture_fetch(d.get('fetch'), t['fetch']) if t['fetch'] is not None else None})
        e['textures'] = texs
        e['samplers'] = (d.get('ps') or {}).get('samplers', [])
        e['vertex'] = d.get('vfetch')
        if e['vertex'] and pr:
            e['vertex']['probe_base'] = pr['vfetch_base']
            e['vertex']['probe_bytes'] = pr['vfetch_bytes']
        e['index'] = d.get('ib')
        e['rt'] = [{'id': r, 'w': tex.get(r, {}).get('w'), 'h': tex.get(r, {}).get('h'), 'fmt': tex.get(r, {}).get('fmt')} for r in d.get('rt', [])]
        e['depth'] = {'id': d.get('depth'), 'fmt': tex.get(d.get('depth') or 0, {}).get('fmt')} if d.get('depth') else None
        for k in ('blend', 'depth_state', 'cull', 'front_ccw', 'viewport'):
            e[k] = d.get(k)
        vf = d.get('vfetch')
        if vf and vf.get('address') in omap:
            m = omap[vf['address']]
            e['orton'] = {'mesh': m['mesh'], 'part': m['part'], 'shader': m['shader'], 'verts': m['verts'], 'key': m['key'],
                          'bones': m['bones'], 'palette': m['palette'], 'ambiguous': m['ambiguous'], 'layout': m.get('layout')}
            side = cls.startswith('lighting build') or cls.startswith('depth only')
            e['orton']['role'] = (mname or cls) if side else 'colour'
        rows.append(e)
    # the colour draw's vertex shader family names the head meshes for sure (0E92FB10 = the head skin VS)
    head_vs, body_vs = '0E92FB108A675C39', '3F91EC05EDABDF7B'
    for e in rows:
        o = e.get('orton')
        if o and o['role'] == 'colour':
            if e['vs'] == head_vs and o['part'] != 'head':
                o['part'] = 'head'
            elif e['vs'] == body_vs and o['part'] == 'head':
                o['part'] = 'body (neck / shoulder)'
            omap['%08X' % int(e['vertex']['address'], 16)]['part'] = o['part']
    for e in rows:
        o = e.get('orton')
        if o:
            o['part'] = omap[e['vertex']['address']]['part']
    # ---- passes: runs of draws with the same targets
    passes = []
    for e in rows:
        key = (tuple(r['id'] for r in e['rt']), (e['depth'] or {}).get('id'))
        if not passes or passes[-1]['key'] != key:
            passes.append({'key': key, 'pass': len(passes), 'first_event': e['event'], 'draws': 0, 'classes': collections.Counter(),
                           'rt': e['rt'], 'depth': e['depth'], 'orton_draws': 0})
        p = passes[-1]
        p['last_event'] = e['event']
        p['draws'] += 1
        p['classes'][e['class']] += 1
        p['orton_draws'] += 1 if e.get('orton') and e['orton']['role'] == 'colour' else 0
    others = scan.get('others', [])
    for p in passes:
        p['actions_before'] = collections.Counter(o['name'] for o in others if o['event'] < p['first_event'] and
                                                  (p['pass'] == 0 or o['event'] > passes[p['pass'] - 1]['last_event']))
    scene = max(passes, key=lambda p: (p['orton_draws'], p['draws'])) if passes else None
    for p in passes:
        p['role'] = ('scene' if p is scene else 'post (after the scene)' if scene and p['pass'] > scene['pass'] else 'before the scene')
    # ---- the key draws for the dump
    orton_colour = [e for e in rows if e.get('orton') and e['orton']['role'] == 'colour']
    eye = [e for e in orton_colour if e['orton']['part'] == 'eyes' and e['ps'] == EYE_PS] or \
          [e for e in orton_colour if e['orton']['part'] == 'eyes']
    eye_ev = eye[-1]['event'] if eye else None
    post_guest = [e for e in rows if e['ps'] and e['vs'] and scene and e['event'] > scene['last_event'] and
                  e['rt'] and (e['rt'][0]['w'] or 0) >= 1280]
    combine = post_guest[-1] if post_guest else None
    present = next((o for o in reversed(others) if 'Present' in o['name']), None)
    keys = {'textures': [], 'targets': [], 'passes': [], 'present': None, 'vertices': []}
    vseen = set()
    for e in rows:
        if e.get('orton') and e['vertex'] and e['vertex']['address'] not in vseen:
            vseen.add(e['vertex']['address'])
            keys['vertices'].append({'event': e['event'], 'address': int(e['vertex']['address'], 16), 'bytes': e['vertex']['bytes']})
    seen_tex = set()
    for e in orton_colour:
        for t in e['textures']:
            if t['id'] not in seen_tex:
                seen_tex.add(t['id'])
                keys['textures'].append({'id': t['id'], 'event': e['event'], 'exr': e['event'] == eye_ev and bool(t.get('cube'))})
        keys['targets'].append({'event': e['event'], 'rt': [r['id'] for r in e['rt']], 'tag': 'orton_%s_m%d' % (
            re.sub(r'\W+', '_', e['orton']['part']).strip('_'), e['orton']['mesh']), 'before': e['event'] == eye_ev,
            'coverage': e['event'] == eye_ev, 'exr': e['event'] == eye_ev,
            # pixel history up to Orton's last colour draw (the lids / lashes come after the eyes)
            'history_until': max(x['event'] for x in orton_colour) if e['event'] == eye_ev else e['event']})
    if eye_ev:   # the eye draw's other inputs too (shadow / lighting maps are not Orton-only textures)
        pass
    if scene:
        keys['targets'].append({'event': scene['last_event'], 'rt': [r['id'] for r in scene['rt']], 'tag': 'scene_end', 'exr': True})
    if combine:
        keys['targets'].append({'event': combine['event'], 'rt': [r['id'] for r in combine['rt']], 'tag': 'post_combine', 'exr': True})
    for p in passes:
        if p['rt']:
            keys['passes'].append({'pass': p['pass'], 'event': p['last_event'], 'rt': [p['rt'][0]['id']]})
    if present and present.get('src'):
        keys['present'] = {'event': present['event'], 'src': present['src']}
    elif rows and rows[-1]['rt']:
        keys['present'] = {'event': rows[-1]['event'], 'src': rows[-1]['rt'][0]['id']}
    json.dump(keys, open(os.path.join(cdir, 'keys.json'), 'w'), indent=1)
    json.dump(vs_out_banks, open(os.path.join(cdir, 'vs_banks.json'), 'w'))
    dump = None
    dp = os.path.join(cdir, 'dump.json')
    if os.path.exists(dp):
        dump = json.load(open(dp))
        files_by_event = collections.defaultdict(list)
        for t in dump.get('targets', []):
            files_by_event[t['event']].append(t)
        for e in rows:
            v = (dump.get('vertices') or {}).get((e['vertex'] or {}).get('address'))
            if v:
                e['vertex'].update(md5=v['md5'], head=v['head'], file=v['file'])
            if e['event'] in files_by_event:
                e['target_files'] = files_by_event[e['event']]
            for t in e['textures']:
                f = dump.get('textures', {}).get(str(t['id']))
                if f:
                    t['files'] = [x for x in (f.get('dds'), f.get('png')) if x]
    for p in passes:
        p['classes'] = dict(p['classes'])
        p['actions_before'] = dict(p['actions_before'])
        p['key'] = list(p['key'][0]) + [p['key'][1]]
        if dump:
            p['image'] = next((x['file'] for x in dump.get('passes', []) if x['pass'] == p['pass']), None)
    idx = {'capture': scan['rdc'], 'draws_total': len(rows), 'guest_draws': len(guest),
           'probe_frame': best[1] if best else None,
           'probe_join': {'constants_equal_share': round(best[0][0], 4), 'aligned_draws': best[0][1]} if best else None,
           'orton_mapping': {'pieces': len(pieces), 'meshes_found': len(omap), 'score': oscore,
                             'by_address': {a: {k: m.get(k) for k in ('mesh', 'part', 'shader', 'verts', 'bones', 'ambiguous', 'layout')}
                                            for a, m in sorted(omap.items())}},
           'eye_event': eye_ev, 'post_combine_event': combine['event'] if combine else None,
           'scene_pass': scene['pass'] if scene else None, 'passes': passes, 'draws': rows,
           'dump': {k: dump.get(k) for k in ('coverage', 'pixel_history', 'final', 'seconds')} if dump else None,
           'scan_seconds': scan.get('seconds')}
    json.dump(idx, open(os.path.join(cdir, 'index.json'), 'w'), indent=1)
    return idx


# ------------------------------------------------------------------------------------------------ summary

def fmt4(v):
    return '(' + ', '.join('%.4g' % x for x in v) + ')'


def summary(dirp, results):
    L = ['# Frame-replay index (Session 98 S2): Orton\'s draws per capture', '',
         'Written by `tools\\assets\\replay\\replay_index.py` from `rdc_extract.py` output (run 6818, host render-target path). '
         'Local only. Registers are each stage\'s own c0..c255 (the pixel bank = probe columns 256..511). '
         'Mesh mapping: vertex count in the address window holding most of ch161 attire PACH 1\'s counts; equal counts are given '
         'out in address order (marked *).', '']
    for name, idx in results:
        L += ['## %s' % name, '']
        pj = idx['probe_join'] or {}
        L.append('- draws %d (guest %d); probe frame %s (constants equal %.1f%%, %d draws aligned); scene pass %s; eye event %s; post combine event %s; scan %.0f s' % (
            idx['draws_total'], idx['guest_draws'], idx['probe_frame'], 100 * pj.get('constants_equal_share', 0), pj.get('aligned_draws', 0),
            idx['scene_pass'], idx['eye_event'], idx['post_combine_event'], idx['scan_seconds'] or 0))
        agree = collections.Counter()
        for e in idx['draws']:
            for st in ('vs', 'ps'):
                m = e.get(st + '_consts_probe_equal')
                if m:
                    a_, b_ = m.split('/')
                    agree[(st, 'equal' if a_ == b_ else 'differs: ' + (e['vs'] or '?'))] += 1
        L.append('- constants vs the probe frame, draws per stage: ' + '; '.join('%s %s %d' % (k[0], k[1], v) for k, v in sorted(agree.items(), key=lambda kv: -kv[1])[:6]))
        om = idx['orton_mapping']
        L.append('- Orton meshes found %d / %d viewer pieces (vertex-count score %d)' % (om['meshes_found'], om['pieces'], om['score']))
        L += ['', '| part | mesh | file material | verts | buffer | colour draw (event) | VS | PS | material class | PS consts = probe | other passes (event: PS) |',
              '|---|---|---|---|---|---|---|---|---|---|---|']
        od = [e for e in idx['draws'] if e.get('orton')]
        by_mesh = collections.defaultdict(list)
        for e in od:
            by_mesh[e['orton']['mesh']].append(e)
        order = ['head', 'body', 'body (neck / shoulder)', 'body (hand)', 'eyes', 'lids / brows (trans)', 'hair', 'shirt / torso gear',
                 'arm gear (wrist / hand)', 'trunks', 'boots', 'gear', 'other']
        for mesh, es in sorted(by_mesh.items(), key=lambda kv: (order.index(kv[1][0]['orton']['part']) if kv[1][0]['orton']['part'] in order else 99, kv[0])):
            o = es[0]['orton']
            col = [e for e in es if e['orton']['role'] == 'colour']
            side = [e for e in es if e['orton']['role'] != 'colour']
            for c in col or [None]:
                L.append('| %s | %d%s | %s | %d | %s | %s | %s | %s | %s | %s | %s |' % (
                    o['part'], mesh, '*' if o['ambiguous'] else ('+' if 'outside' in (o.get('layout') or '') else ''), o['shader'], o['verts'],
                    es[0]['vertex']['address'], c['event'] if c else '-', c['vs'] if c else '-', (c['ps'] or '-') if c else '-',
                    ('%s (%s)' % (c['class'], c['material'])) if c else '-', c.get('ps_consts_probe_equal') if c else '-',
                    ', '.join('%d: %s' % (e['event'], (e['ps'] or 'none')[:8]) for e in side)))
        if not od:
            L.append('| (none found) | | | | | | | | | | |')
        L.append('')
        L.append('(* equal vertex counts, assigned by address order; + outside the address window, the only buffer of its count)')
        eye = next((e for e in idx['draws'] if e['event'] == idx['eye_event']), None)
        if eye:
            L += ['', '### Eye draw (event %d, guest draw %s): inputs' % (eye['event'], eye['guest_draw']), '',
                  '- VS %s, PS %s (%s); %d indices, vertices %s at %s, stride %s' % (
                      eye['vs'], eye['ps'], eye['material'], eye['count'], (eye['vertex'] or {}).get('vertices'),
                      (eye['vertex'] or {}).get('address'), (eye['vertex'] or {}).get('stride')),
                  '- blend %s; depth %s; cull %s; target %s, depth %s' % (
                      eye['blend'], eye['depth_state'], eye['cull'], ['%s %sx%s %s' % (r['id'], r['w'], r['h'], r['fmt']) for r in eye['rt']],
                      eye['depth']),
                  '- PS constants (probe equal %s):' % eye.get('ps_consts_probe_equal'), '']
            pc = eye.get('ps_consts') or {}
            L.append('  ' + '; '.join('%s %s' % (k, fmt4(v)) for k, v in sorted(pc.items(), key=lambda kv: int(kv[0][1:]))))
            L += ['', '- textures:', '']
            for t in eye['textures']:
                g = t.get('guest') or {}
                L.append('  - %s tf%s `%s`: resource %s, %sx%s %s, mips %s%s; guest %s fmt %s %sx%s%s' % (
                    t['stage'], t['fetch'], t['binding'], t['id'], t['w'], t['h'], t['fmt'], t['mips'], ' cube' if t.get('cube') else '',
                    g.get('address'), g.get('format'), g.get('w'), g.get('h'), ('; files ' + ', '.join(t['files'])) if t.get('files') else ''))
            if eye.get('samplers'):
                L.append('- samplers: ' + '; '.join('%s %s/%s %s-%s-%s' % (s['binding'], s['u'], s['v'], s['min'], s['mag'], s['mip']) for s in eye['samplers']))
            dmp = idx.get('dump') or {}
            cov = (dmp.get('coverage') or {}).get(str(eye['event']))
            if cov:
                L.append('- coverage (pixels changed by the draw): %s' % cov)
            for ph in (dmp.get('pixel_history') or []):
                if ph['event'] == eye['event']:
                    L.append('- pixel history at (%d, %d): %s' % (ph['x'], ph['y'], '; '.join(
                        'e%d %s->%s%s' % (m['event'], fmt4(m['pre'] or []), fmt4(m['post'] or []), '' if m['passed'] else ' (failed)')
                        for m in ph['mods'])))
            if eye.get('target_files'):
                L.append('- saved: ' + ', '.join(x.get('png') or '' for x in eye['target_files']))
        L += ['', '### Frame anatomy (passes)', '', '| pass | events | draws | target | role | classes |', '|---|---|---|---|---|---|']
        for p in idx['passes']:
            rt = p['rt'][0] if p['rt'] else {}
            top = ', '.join('%s %d' % kv for kv in sorted(p['classes'].items(), key=lambda kv: -kv[1])[:3])
            tgt = '%s %sx%s %s' % (rt['id'], rt['w'], rt['h'], rt['fmt']) if rt and rt.get('w') else \
                  'depth only %s %s' % ((p['depth'] or {}).get('id'), (p['depth'] or {}).get('fmt'))
            L.append('| %d | %d-%d | %d | %s | %s | %s |%s' % (p['pass'], p['first_event'], p['last_event'], p['draws'], tgt, p['role'], top,
                                                         (' `%s`' % p['image']) if p.get('image') else ''))
        L.append('')
    open(os.path.join(dirp, 'index_summary.md'), 'w', encoding='utf-8').write('\n'.join(L) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=os.path.join(REPO, 'out', 's98', 'replay'))
    ap.add_argument('--no-probe', action='store_true')
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    names = a.names or sorted(n for n in os.listdir(a.dir) if os.path.isfile(os.path.join(a.dir, n, 'scan.json')))
    pp = os.path.join(a.dir, 'probe.csv')
    probe = Probe(pp) if (not a.no_probe and os.path.exists(pp)) else None
    mats = material_table()
    pieces = orton_pieces()
    results = []
    for n in names:
        cdir = os.path.join(a.dir, n)
        if not os.path.exists(os.path.join(cdir, 'scan.json')):
            print('replay_index: %s has no scan.json' % n)
            continue
        idx = index_capture(cdir, n, probe, mats, pieces)
        results.append((n, idx))
        print('%s: draws %d, probe frame %s %s, Orton meshes %d, eye event %s, dump %s' % (
            n, idx['draws_total'], idx['probe_frame'], idx['probe_join'], idx['orton_mapping']['meshes_found'], idx['eye_event'],
            'yes' if idx['dump'] else 'no'))
    if results:
        order = {'rp_stage': 0, 'rp_ramp': 1, 'rp_ringin': 2, 'rp_ring': 3}
        results.sort(key=lambda r: order.get(r[0], 9))
        summary(a.dir, results)


if __name__ == '__main__':
    main()

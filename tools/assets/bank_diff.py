r"""Session 98 frame-replay S0: the Arena Viewer's draw constants vs the game's, register by register (0 game runs).

For a refcheck item (Orton at guest frames 8341 / 8941 / 10141 / 10741 of run 6816: same placement / camera / entrance time as
out\s98\arena_fix2\items.json) this
  1. (--dump) drives the viewer (one hidden launch per item, hard 120 s) with the item's `place` / `stage` commands and the game's
     camera, then {"cmd":"bank_dump"}: every mesh's DrawCB exactly as the viewer feeds it (renderer.cpp BankDump, read-only);
  2. (--diff) matches the viewer's meshes to the probe's draws of that frame (Orton's skin / head / eyes / hair / gear / trans draws by
     vertex count + shader family; a few arena / crowd / stage draws by VS family), maps the viewer's fields into the game's register
     numbers with tools\assets\replay\regmap.json and prints per draw, register by register, game vs viewer and the difference,
     grouped by meaning; ranks the biggest input differences per material into out\s98\replay\s0_report.md.

  py -I tools\assets\bank_diff.py --dump --diff [--items ITEMS.json] [--only ar_8341,...] [--probe-dir DIR] [--out DIR]

The probe is run 6816 (out\s98\run1: consts.npy [N,512,4] = vertex bank c0-255 then pixel bank c256-511, meta.json, probe.csv).
Everything stays under out\ (gitignored); nothing from the game is written into the repo.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'
EXE = os.environ.get('WWE_VIEWER_EXE') or next((p for p in (os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe'),
                                                              os.path.join(MAIN, 'out', 'tools', 'wwe2k14_assets.exe')) if os.path.exists(p)), '')
GAME = os.environ.get('WWE_GAME') or os.path.join(MAIN, 'game_data', 'extracted')
MEDIA = os.environ.get('WWE_STAGE_MEDIA') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-launcher', 'trons')
PROBE_DIR = os.path.join(MAIN, 'out', 's98', 'run1')
ITEMS = os.path.join(MAIN, 'out', 's98', 'arena_fix2', 'items.json')
OUT = os.path.join(MAIN, 'out', 's98', 'replay')
REGMAP = os.path.join(HERE, 'replay', 'regmap.json')
VS_SWAP = os.path.join(REPO, 'src', 'scene_vs_swap.h')
DEFAULT_ONLY = ['ar_8341', 'ar_8941', 'ar_10141', 'ar_10741']

# vertex strides of the wrestler VS families (dwords * 4), out\s98\run1\_attire.py
STRIDE = {'3F91EC05': 48, 'F709D14F': 48, '0E92FB10': 48, '9CB11A97': 44, 'B1568876': 36, 'F006439D': 36, '56DD2E94': 48}
# the wrestler colour-pass VS hash -> material class (probe run 6816; _lib.SKIN_VS + the eye / trans hashes of out\s98\replay\design.md E)
WR_CLASS = {'3F91EC05EDABDF7B': 'skin', 'F709D14F30FCC95C': 'gear', '0E92FB108A675C39': 'head', '56DD2E94E20F2826': 'hair',
            'B15688766DA713DC': 'eyes', 'F006439D879C5FDB': 'skin', '9CB11A97F230260C': 'trans'}  # (F006439D = the lean 9-dword skin VS: its c142 = the skin set's; 9CB11A97 = the 11-dword reflect VS the eyelid / lash layer uses)
ORTON_VB = (0x1C3F0000, 0x1C490000)  # Orton's vertex buffers in run 6816 (out\s98\run1\_rigmatch.py)


# ------------------------------------------------------------------------------------------------------------- probe
class Probe:
    def __init__(self, d):
        m = json.load(open(os.path.join(d, 'meta.json')))
        self.hdr, self.rows = m['hdr'], m['rows']
        self.h = {k: i for i, k in enumerate(self.hdr)}
        self.c = np.load(os.path.join(d, 'consts.npy'), mmap_mode='r')
        self.swap = np.array([int(r[0]) for r in self.rows])
        self.vs = [r[self.h['vs_hash']] for r in self.rows]
        self.names = {}
        try:
            for line in open(VS_SWAP, encoding='utf-8', errors='replace'):
                mm = re.search(r'\{0x([0-9A-F]{16})ull,.*"([^"]+)"', line)
                if mm:
                    self.names[mm.group(1)] = mm.group(2)
        except OSError:
            pass

    def vb(self, i):
        return int(self.rows[i][self.h['vfetch_base']], 16)

    def vbytes(self, i):
        return int(self.rows[i][self.h['vfetch_bytes']])

    def idxc(self, i):
        return int(self.rows[i][self.h['index_count']])

    def draw(self, i):
        return int(self.rows[i][self.h['draw']])

    def reg(self, i, bank, r):
        return np.array(self.c[i, (256 if bank == 'ps' else 0) + r], dtype=np.float64)

    def frame(self, s):
        return [int(i) for i in np.nonzero(self.swap == s)[0]]

    def camera(self, s):
        """-> (vp[16], eye[3]) the commonest (c72 eye, c56..c59) among the frame's draws (refcheck.camera_from_probe's rule)."""
        cnt, first = collections.Counter(), {}
        for i in self.frame(s):
            vp = np.array(self.c[i, 56:60], dtype=np.float64).reshape(-1)
            eye = np.array(self.c[i, 72, :3], dtype=np.float64)
            k = (tuple(np.round(vp, 3)), tuple(np.round(eye, 1)))
            cnt[k] += 1
            first.setdefault(k, (vp, eye))
        k = cnt.most_common(1)[0][0]
        return first[k]

    def orton_color_draws(self, s):
        """Orton's wrestler colour pass of frame s: the last block of draws in his vertex-buffer range whose VS is in WR_CLASS."""
        out = []
        for i in self.frame(s):
            if self.vs[i] in WR_CLASS and ORTON_VB[0] <= self.vb(i) < ORTON_VB[1]:
                out.append(i)
        return out


# ------------------------------------------------------------------------------------------------------------- dump
def build_script(item, vp, eye, dump_path):
    lines = ['{"cmd":"load","kind":"arena","id":%d}' % item['arena']]
    if item.get('look') == 'entrance':
        lines.append(json.dumps({'cmd': 'lights', 'mode': 'entrance', 'time': item.get('time', 140)}))
    st = {'cmd': 'stage'}
    if os.path.isdir(MEDIA):
        st['media'] = MEDIA.replace('\\', '/')
    if item.get('look') == 'entrance' and item.get('wrestler') is not None:
        st['wrestler'] = int(item['wrestler'])
    if len(st) > 1:
        lines.append(json.dumps(st))
    lines += [json.dumps(p) if isinstance(p, dict) else p for p in item.get('place', [])]
    lines.append(json.dumps({'cmd': 'camera', 'viewproj': [float(x) for x in vp], 'eye': [float(x) for x in eye]}))
    lines.append(json.dumps({'cmd': 'shot', 'path': dump_path.replace('.bank.json', '.viewer.png').replace(chr(92), '/'), 'w': 1280, 'h': 720}))  # (the picture first: the frame state the dump must equal)
    lines.append(json.dumps({'cmd': 'bank_dump', 'path': dump_path.replace('\\', '/'), 'w': 1280, 'h': 720}))
    return lines


def dump_item(item, probe, outdir):
    os.makedirs(outdir, exist_ok=True)
    s = int(item['swap'])
    vp, eye = probe.camera(s)
    dump = os.path.join(outdir, item['name'] + '.bank.json')
    script = os.path.join(outdir, item['name'] + '.viewer.jsonl')
    open(script, 'w').write('\n'.join(build_script(item, vp, eye, dump)) + '\n')
    if os.path.exists(dump):
        os.remove(dump)
    if not EXE:
        raise SystemExit('wwe2k14_assets.exe not found (build with tools\\assets\\build_viewer.ps1) or set WWE_VIEWER_EXE')
    p = subprocess.run([EXE, '--game', GAME, '--script', script, '--exit', '--hidden'], capture_output=True, text=True, timeout=120)
    open(os.path.join(outdir, item['name'] + '.viewer.log'), 'w').write(p.stdout + '\n--stderr--\n' + p.stderr)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad or not os.path.exists(dump):
        raise SystemExit('viewer failed for %s: %s' % (item['name'], (bad[:3] or p.stderr[-300:])))
    return dump




# ------------------------------------------------------------------------------------------------------------- diff
ARENA_CLASSES = ('arena', 'stage', 'props', 'crowd')
WR_CLASSES = ('skin', 'head', 'hair', 'gear', 'eyes', 'trans')
CLASS_ORDER = ('skin', 'head', 'gear', 'eyes', 'hair', 'trans', 'arena', 'stage', 'props', 'crowd')
EPS_REL = 0.05  # relative differences are measured against max(|game|, |viewer|, EPS_REL)


def rows_of(regmap_path):
    rows = []
    for r in json.load(open(regmap_path))['rows']:
        if r.get('dup') or r.get('optional'):
            continue
        rows.append(r)
    return rows


def applies(row, cls):
    cl = row.get('classes', ['any'])
    if 'any' in cl:
        return True
    if 'any-wr' in cl:
        return cls in WR_CLASSES
    return cls in cl


def game_vals(probe, i, row, cls):
    g = row['game']
    bank = 'vs' if cls in ARENA_CLASSES else g['bank']
    regs = g['reg'] if isinstance(g['reg'], list) else [g['reg']]
    if g.get('prodcomp'):  # the product of one register's components (c199.x x c199.y)
        return [(regs[0], np.array([np.prod(probe.reg(i, bank, regs[0])[g['comp']])]))]
    if 'mul' in g:  # a product of registers (e.g. c197 x c198)
        prod = np.ones(len(g['comp']))
        for r in g['mul']:
            prod = prod * probe.reg(i, bank, r)[g['comp']]
        return [(regs[0], prod)]
    return [(r, probe.reg(i, bank, r)[g['comp']]) for r in regs]


def viewer_vals(row, cb, nregs):
    """-> list (per register of the row) of arrays, or (kind, values) for const / implicit / None."""
    v = row['viewer']
    if v is None:
        return 'none', None
    if 'const' in v:
        return 'const', [np.array(v['const'], dtype=np.float64)] * nregs
    if 'implicit' in v:
        return 'implicit', [np.array(v['implicit'], dtype=np.float64)] * nregs
    f = np.array(cb[v['field']], dtype=np.float64)
    comp = v['comp']
    if f.ndim == 2:
        idx = v.get('idx', list(range(nregs)))
        return 'fed', [f[k][comp] for k in idx]
    val = f[comp]
    if v.get('prodcomp'):
        val = np.array([np.prod(val)])
    if 'mulfield' in v:
        val = val * np.array(cb[v['mulfield']], dtype=np.float64)[comp]
    return 'fed', [val] * nregs


def compare_rows(probe, i, cb, rows, cls):
    """-> list of dicts, one per (row, register): name, group, game, viewer, diff, rel, kind, note."""
    out = []
    for row in rows:
        if row['group'] in ('camera', 'bones') or not applies(row, cls):
            continue
        gv = game_vals(probe, i, row, cls)
        kind, vv = viewer_vals(row, cb, len(gv))
        for n, (reg, g) in enumerate(gv):
            e = {'name': row['name'] + ('' if len(gv) == 1 else ' [c%d]' % reg), 'group': row['group'], 'game': g, 'kind': kind, 'note': row.get('note') or (row['viewer'] or {}).get('note', '')}
            if vv is None:
                e.update(viewer=None, diff=None, rel=None)
            else:
                v = vv[n]
                if len(v) != len(g):
                    v = np.resize(v, len(g))
                d = v - g
                rel = float(np.max(np.abs(d) / np.maximum(np.maximum(np.abs(g), np.abs(v)), EPS_REL)))
                e.update(viewer=v, diff=d, rel=rel)
            out.append(e)
    return out


def fmt(a):
    return '(' + ', '.join('%.4g' % x for x in a) + ')' if a is not None else '-'


# ---- the game's draws of a frame, by class
def game_wrestler_draws(probe, s):
    """-> [(i, class)] Orton's colour-pass draws in draw order (the last draw of each vertex buffer = the colour pass)."""
    last = {}
    for i in probe.frame(s):
        if probe.vs[i] in WR_CLASS and (ORTON_VB[0] <= probe.vb(i) < ORTON_VB[1] or WR_CLASS[probe.vs[i]] == 'trans'):
            last.setdefault(probe.vb(i), i)
            if WR_CLASS[probe.vs[i]] != 'trans':
                last[probe.vb(i)] = i  # (the colour pass is the last draw of a buffer; the eyelid layer's draws are all colour)
    out = []
    for i in sorted(last.values()):
        cls = WR_CLASS[probe.vs[i]]
        out.append((i, cls))
    return out


def game_verts(probe, i):
    st = STRIDE.get(probe.vs[i][:8])
    return probe.vbytes(i) // st if st else None


def match_wrestler(probe, s, draws):
    """Viewer wrestler meshes <-> game colour draws, in order, by vertex count (+ the head / skin split by the game's VS family)."""
    gd = game_wrestler_draws(probe, s)
    used = set()
    pairs, unmatched = [], []
    vmesh = [d for d in draws if d['mat']['wrestler'] and d['model'] >= 0]
    # the actor model only
    wr_models = {d['model'] for d in vmesh}
    for d in vmesh:
        hit = None
        for k, (i, cls) in enumerate(gd):
            if k in used:
                continue
            if game_verts(probe, i) == d['verts']:
                hit = k
                break
        if hit is None:
            unmatched.append(d)
            continue
        used.add(hit)
        i, cls = gd[hit]
        pairs.append((i, cls, d))
    unmatched_game = [gd[k][0] for k in range(len(gd)) if k not in used]
    return pairs, unmatched, unmatched_game


def vclass_arena(d):
    """The viewer arena-side class of a draw: crowd / props / stage (lightmapped) / arena."""
    if d['mat']['crowd']:
        return 'crowd'
    sh = d.get('shader', '')
    if sh.startswith('yProp') or d['kind'] == 4:
        return 'props'
    if d['cb']['flags2'][0] > 0.5 or d['mat']['has_lightmap']:
        return 'stage'
    return 'arena'


def gclass_arena(probe, i):
    nm = probe.names.get(probe.vs[i], probe.vs[i])
    if probe.vs[i].startswith('C1028A64') or nm.startswith('yAud'):
        return 'crowd'
    if nm.startswith('yProp'):
        return 'stage' if 'LM' in nm else 'props'
    if nm.startswith('yBG'):
        return 'stage' if 'LM' in nm else 'arena'
    return None


def tuple_of(vals):
    return tuple(round(float(x), 3) for x in np.concatenate([np.ravel(v) for v in vals]))


def game_arena_groups(probe, s):
    """class -> {light tuple: [draw indices]}, from the VS bank (arena shaders are vertex-lit)."""
    cl = collections.defaultdict(lambda: collections.defaultdict(list))
    for i in probe.frame(s):
        if ORTON_VB[0] <= probe.vb(i) < ORTON_VB[1]:
            continue
        c = gclass_arena(probe, i)
        if not c:
            continue
        t = tuple_of([probe.reg(i, 'vs', k)[:3] for k in (0, 1, 2, 3, 181, 182, 189, 190)])
        cl[c][t].append(i)
    return cl


def viewer_arena_groups(draws):
    cl = collections.defaultdict(lambda: collections.defaultdict(list))
    for d in draws:
        if d['mat']['wrestler'] or d['blended'] and False:
            continue
        c = vclass_arena(d)
        cb = d['cb']
        t = tuple_of([np.array(cb['lcol'][k][:3]) for k in range(4)] + [np.array(cb['sky'][:3]), np.array(cb['ground'][:3]), np.array(cb['mat_amb'][:3]), np.array(cb['mat_dif'][:3])])
        cl[c][t].append(d)
    return cl


def nearest(gt, vgroups):
    best, bd = None, 1e18
    for t, ds in vgroups.items():
        e = sum(abs(a - b) for a, b in zip(gt[:18], t[:18]))  # c0-c3 rgb, c181, c182 (what the rig sets)
        if e < bd:
            best, bd = t, e
    return best, bd


def bones_check(probe, draws_idx, models, s):
    """Set comparison of the skinning palettes' origin images: every non-zero game bone vs the nearest viewer bone (file space)."""
    gt = set()
    for i in draws_idx:
        for k in range(50):
            t = np.array([probe.reg(i, 'vs', 76 + 3 * k + r)[3] for r in range(3)])
            if 1.0 < np.abs(t).sum() and np.abs(t).max() < 5000.0:  # (unused palette rows hold garbage up to 6e4)
                gt.add(tuple(np.round(t, 2)))
    gt = np.array(sorted(gt))
    m = next((m for m in models if m['has_wrestler'] and m['bones']), None)
    if m is None or not len(gt):
        return None
    vt = []
    sn, cs = m['rot_sin'], m['rot_cos']
    for b in m['bones']:
        x, y, z = b[12], b[13], b[14]  # row-vector 4x4: the translation row (image of the origin)
        x, z = x * cs + z * sn, -x * sn + z * cs
        vt.append((x + m['offset'][0], -(y + m['offset'][1]), z + m['offset'][2]))  # viewer world (Y up) -> file space (-Y up)
    vt = np.array(vt)
    dist = np.array([np.min(np.linalg.norm(vt - g, axis=1)) for g in gt])
    return {'game_bones': len(gt), 'viewer_bones': len(vt), 'median': float(np.median(dist)), 'p90': float(np.percentile(dist, 90)), 'max': float(dist.max()),
            'within1': int((dist < 1.0).sum())}


def camera_check(probe, dump, s):
    vp, eye = probe.camera(s)
    m = np.array(dump['frame']['viewProj'], dtype=np.float64).reshape(4, 4)  # viewer rows (row-vector form)
    g = np.zeros((4, 4))
    for r in range(4):
        for c in range(4):
            g[r, c] = m[c, r] * (-1.0 if c == 1 else 1.0)
    vc = np.array(dump['frame']['cam'], dtype=np.float64)
    ev = np.array([vc[0], -vc[1], vc[2]])
    return {'vp_maxdiff': float(np.max(np.abs(g - vp.reshape(4, 4)))), 'eye_diff': float(np.max(np.abs(ev - eye)))}


def film_check(probe, dump, s):
    """Viewer film / bloom / glare vs the game's fullscreen film quad (VS 032E8FAC) and combine (yGlareEtc quad) constants."""
    post = dump['post']
    out = {'viewer_film': post['film'], 'viewer_bloom': post['bloom'], 'viewer_glare': post['glare'], 'game': []}
    film = np.array(post['film'], dtype=np.float64)
    for i in probe.frame(s):
        nm = probe.names.get(probe.vs[i], probe.vs[i])
        if probe.vs[i].startswith('032E8FAC'):
            hits = []
            for bank in ('vs', 'ps'):
                for r in range(256):
                    v = probe.reg(i, bank, r)
                    if np.abs(v[:3] - film[:3]).max() < 0.01 and abs(v[3] - film[3]) < 0.02 and np.abs(v).sum() > 0:
                        hits.append('%s c%d=%s' % (bank, r, fmt(v)))
            out['game'].append('film quad draw %d: %s' % (probe.draw(i), '; '.join(hits[:3]) or 'no register equals the viewer film colour'))
        if nm.startswith('yGlareEtc'):
            out['game'].append('combine/glare quad draw %d (%s): ps c31=%s c32=%s' % (probe.draw(i), nm, fmt(probe.reg(i, 'ps', 31)), fmt(probe.reg(i, 'ps', 32))))
    return out


def load_dump(bankdir, name, tag=''):
    return json.load(open(os.path.join(bankdir, name + '.bank' + tag + '.json')))


def analyse_frame(probe, dump, s, rows):
    """-> {'wr': [(class, game draw no, viewer mesh no, comparisons)], 'arena': {...}, 'cam', 'bones', 'film', 'unmatched'}"""
    res = {'wr': [], 'arena': {}, 'unmatched': []}
    pairs, unm, ungame = match_wrestler(probe, s, dump['draws'])
    for i, cls, d in pairs:
        res['wr'].append({'cls': cls, 'game_draw': probe.draw(i), 'game_i': i, 'mesh': d['mesh'], 'verts': d['verts'], 'part': d.get('part', ''), 'shader': d.get('shader', ''),
                          'cmp': compare_rows(probe, i, d['cb'], rows, cls)})
    res['unmatched'] = ['viewer mesh %d (%s, %d verts) has no game draw' % (d['mesh'], d.get('shader', '?'), d['verts']) for d in unm] + \
                       ['game draw %d (%s, %s verts) has no viewer mesh' % (probe.draw(i), probe.names.get(probe.vs[i], probe.vs[i]), game_verts(probe, i)) for i in ungame]
    gg, vg = game_arena_groups(probe, s), viewer_arena_groups(dump['draws'])
    for c in ARENA_CLASSES:
        lst = []
        for gt, gds in sorted(gg.get(c, {}).items(), key=lambda kv: -len(kv[1]))[:3]:
            vt, e = nearest(gt, vg.get(c, {}))
            if vt is None:
                lst.append({'n_game': len(gds), 'n_viewer': 0, 'cmp': [], 'light_err': None, 'game_draw': probe.draw(gds[0]), 'gtuple': gt, 'vtuple': None})
                continue
            vd = vg[c][vt][0]
            cm = compare_rows(probe, gds[0], vd['cb'], rows, c)
            lst.append({'n_game': len(gds), 'n_viewer': len(vg[c][vt]), 'cmp': cm, 'light_err': e, 'gtuple': gt, 'vtuple': vt, 'game_draw': probe.draw(gds[0]), 'vs': probe.names.get(probe.vs[gds[0]], probe.vs[gds[0]])})
        res['arena'][c] = lst
    res['cam'] = camera_check(probe, dump, s)
    res['bones'] = bones_check(probe, [p[0] for p in pairs], dump['models'], s)
    res['film'] = film_check(probe, dump, s)
    return res


def print_frame(name, s, res, out):
    P = out.append
    P('\n==== %s (guest frame %d) ====' % (name, s))
    P('camera: viewProj max |diff| %.5g, eye max |diff| %.5g' % (res['cam']['vp_maxdiff'], res['cam']['eye_diff']))
    b = res['bones']
    P('bones: ' + ('game %d non-zero palette bones, viewer %d; nearest-viewer-bone distance (file units): median %.2f, p90 %.2f, max %.2f; %d within 1.0' % (b['game_bones'], b['viewer_bones'], b['median'], b['p90'], b['max'], b['within1']) if b else 'not comparable'))
    f = res['film']
    P('film/post: viewer film %s bloom %.3g glare %.3g | game: %s' % (fmt(f['viewer_film']), f['viewer_bloom'], f['viewer_glare'], ' | '.join(f['game']) or 'no film / combine draw found'))
    for u in res['unmatched']:
        P('UNMATCHED: ' + u)
    for w in res['wr']:
        P('\n-- wrestler draw %d (%s, viewer mesh %d "%s" %s, %d verts) --' % (w['game_draw'], w['cls'], w['mesh'], w['part'], w['shader'], w['verts']))
        P(table(w['cmp']))
    for c in ARENA_CLASSES:
        for g in res['arena'][c]:
            P('\n-- %s: game light group of %d draws (first draw %d, VS %s) vs nearest viewer group of %d meshes (light err %s) --' % (c, g['n_game'], g['game_draw'], g.get('vs', '?'), g['n_viewer'], '%.3f' % g['light_err'] if g['light_err'] is not None else '-'))
            P(table(g['cmp']))


def table(cmp):
    lines = ['  %-46s %-30s %-30s %-28s %s' % ('register', 'game', 'viewer', 'diff (viewer - game)', 'status')]
    cur = None
    for e in sorted(cmp, key=lambda e: e['group']):
        if e['group'] != cur:
            cur = e['group']
            lines.append('  [%s]' % cur)
        if e['kind'] == 'none':
            st = 'NOT MODELLED'
            lines.append('    %-44s %-30s %-30s %-28s %s' % (e['name'][:44], fmt(e['game']), '-', '-', st))
            continue
        ok = e['rel'] is not None and e['rel'] < 0.02
        st = {'fed': 'ok' if ok else 'DIFF x%.2f' % ratio(e), 'const': 'hard-coded ' + ('ok' if ok else 'DIFF x%.2f' % ratio(e)), 'implicit': 'assumed ' + ('ok' if ok else 'DIFF x%.2f' % ratio(e))}[e['kind']]
        lines.append('    %-44s %-30s %-30s %-28s %s' % (e['name'][:44], fmt(e['game']), fmt(e['viewer']), fmt(e['diff']), st))
    return '\n'.join(lines)


def ratio(e):
    g, v = np.abs(e['game']), np.abs(e['viewer'])
    k = int(np.argmax(np.abs(e['diff']) / np.maximum(np.maximum(g, v), EPS_REL)))
    return float(v[k] / g[k]) if g[k] > 1e-9 else float('inf')


def ranked(results_by_class, top=8):
    """results_by_class: class -> list of (frame name, comparisons). -> top rows by worst relative difference over the frames."""
    agg = {}
    for cls, lst in results_by_class.items():
        for fname, cmp in lst:
            for e in cmp:
                if e['kind'] == 'none' or e['rel'] is None:
                    continue
                k = (cls, e['group'], e['name'])
                a = agg.setdefault(k, {'rel': 0.0, 'frames': [], 'e': e, 'kind': e['kind']})
                if e['rel'] > a['rel']:
                    a['rel'], a['e'] = e['rel'], e
                if e['rel'] >= 0.02:
                    a['frames'].append(fname)
    return agg


def md_report(items_res, probe, out_path, regmap_path, before=None):
    L = []
    P = L.append
    P('# Session 98 replay S0: the Arena Viewer\'s draw constants vs the game\'s (probe run 6816), 0 game runs')
    P('')
    P('Generated by `py -I tools\\assets\\bank_diff.py --dump --diff` (viewer `bank_dump` + `tools\\assets\\replay\\regmap.json`). Frames: Orton at guest 8341 / 8941 / 10141 / 10741 (the refcheck items of `out\\s98\\arena_fix2\\items.json`). [C] = numbers from the probe / dump; the register meanings are the scene shaders\' own constant lists.')
    P('')
    by_cls = collections.defaultdict(list)
    for name, s, res in items_res:
        for w in res['wr']:
            by_cls[w['cls']].append((name, w['cmp']))
        for c in ARENA_CLASSES:
            ok = [g for g in res['arena'][c] if g['light_err'] is not None and g['light_err'] < 0.1]
            for g in ok[:1]:  # the biggest group the viewer reproduces (nearest light tuple within 0.1); the others are listed below
                by_cls[c].append((name, g['cmp']))
    agg = ranked(by_cls)
    P('## Ranked input differences per material')
    P('')
    P('Score = worst relative difference over the matched draws of the four frames (|viewer - game| / max(|game|, |viewer|, 0.05)); only rows >= 0.02 are listed. "fed" = the viewer feeds the value; "assumed" = the viewer HLSL assumes a fixed value and reads nothing; "hard-coded" = a literal in the HLSL. A register the viewer does not model at all is listed under "not modelled".')
    for cls in CLASS_ORDER:
        if cls not in by_cls:
            continue
        rows = sorted([(a['rel'], k, a) for k, a in agg.items() if k[0] == cls and a['rel'] >= 0.02], key=lambda t: -t[0])
        P('')
        P('### %s' % cls)
        if not rows:
            P('no input difference >= 2 percent on the compared registers.')
        for rel, (c, grp, nm), a in rows[:8]:
            e = a['e']
            P('- **%s** [%s, %s] game %s, viewer %s (x%.2f), worst-case rel %.2f; frames: %s' % (nm, grp, a['kind'], fmt(e['game']), fmt(e['viewer']), ratio(e), rel, ', '.join(sorted(set(a['frames']))) or '-'))
        nm_rows = sorted({(e['group'], e['name'], fmt(e['game'])) for _, cm in by_cls[cls] for e in cm if e['kind'] == 'none' and np.abs(e['game']).sum() > 0})
        if nm_rows:
            P('- not modelled in the viewer (game values non-zero): ' + '; '.join('%s %s' % (n, g) for _, n, g in nm_rows[:8]))
    P('')
    P('## Arena / props / crowd light groups the viewer does not reproduce (no viewer group within 0.1 of the game\'s c0-c3, c181, c182)')
    P('')
    P('Groups are the game draws of a class with identical light tuples (vertex bank c0-c3 rgb, c181, c182, c189, c190), the three biggest per class and frame; "nearest" = the viewer group with the smallest L1 distance over c0-c3 rgb + sky + ground.')
    for name, s, res in items_res:
        for c in ARENA_CLASSES:
            for g in res['arena'][c]:
                if g['light_err'] is not None and g['light_err'] < 0.1:
                    continue
                gt = g['gtuple']
                vt = g['vtuple']
                P('- %s %s: %d game draws (first %d, %s) c0-c3 %s sky %s ground %s | nearest viewer group (L1 %s): %s' % (
                    name, c, g['n_game'], g['game_draw'], g.get('vs', '?'), [list(gt[3 * k:3 * k + 3]) for k in range(4)], list(gt[12:15]), list(gt[15:18]),
                    '%.2f' % g['light_err'] if g['light_err'] is not None else '-',
                    ('c0-c3 %s sky %s ground %s' % ([list(vt[3 * k:3 * k + 3]) for k in range(4)], list(vt[12:15]), list(vt[15:18]))) if vt else 'none'))
    P('')
    P('## Per frame: camera, skeleton, film')
    P('')
    for name, s, res in items_res:
        b = res['bones']
        f = res['film']
        P('- **%s** (guest %d): camera viewProj max diff %.2g, eye %.2g | bones: %s | film/post: viewer film %s bloom %.3g glare %.3g; game %s' % (
            name, s, res['cam']['vp_maxdiff'], res['cam']['eye_diff'],
            ('%d game palette bones vs %d viewer bones, nearest-bone distance median %.2f p90 %.2f max %.2f (file units; palette rows hold some junk)' % (b['game_bones'], b['viewer_bones'], b['median'], b['p90'], b['max'])) if b else 'n/a',
            fmt(f['viewer_film']), f['viewer_bloom'], f['viewer_glare'], ' | '.join(f['game']) or 'none found'))
        for u in res['unmatched']:
            P('  - UNMATCHED: ' + u)
    return L


# ------------------------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', action='store_true', help='launch the viewer once per item (hard 120 s each) and write <name>.bank.json')
    ap.add_argument('--diff', action='store_true', help='diff the dumps against the probe: out\\s98\\replay\\s0_tables.txt + s0_report.md (generated part)')
    ap.add_argument('--items', default=ITEMS)
    ap.add_argument('--only', default=','.join(DEFAULT_ONLY))
    ap.add_argument('--probe-dir', default=PROBE_DIR)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--vb', help='the entrant''s vertex-buffer window LO,HI (hex; default Orton''s in run 6816), e.g. 1C300000,1C400000')
    ap.add_argument('--bank-dir', help='where the <name>.bank.json dumps are (default OUT\\bank)')
    ap.add_argument('--tag', default='', help='diff <name>.bank<TAG>.json instead (e.g. .c3baaa1_rigstub, the dump of the build whose rig evaluation was lost)')
    a = ap.parse_args()
    global ORTON_VB
    if a.vb:
        ORTON_VB = tuple(int(x, 16) for x in a.vb.split(','))
    raw = json.load(open(a.items))
    if isinstance(raw, dict):  # an entrance_look items file (name -> item)
        raw = [dict(v, name=k) for k, v in raw.items() if not k.startswith('_')]
    items = [it for it in raw if it['name'] in a.only.split(',')]
    probe = Probe(a.probe_dir)
    bankdir = a.bank_dir or os.path.join(a.out, 'bank')
    if a.dump:
        for it in items:
            print('dump', it['name'], '->', dump_item(it, probe, bankdir), flush=True)
    if a.diff:
        rows = rows_of(REGMAP)
        items_res, tables = [], []
        for it in items:
            try:
                dump = load_dump(bankdir, it['name'], a.tag)
            except OSError:
                print('no dump for', it['name'], a.tag)
                continue
            s = int(it['swap'])
            res = analyse_frame(probe, dump, s, rows)
            items_res.append((it['name'], s, res))
            print_frame(it['name'], s, res, tables)
        suffix = a.tag.replace('.', '_') if a.tag else ''
        open(os.path.join(a.out, 's0_tables%s.txt' % suffix), 'w').write('\n'.join(tables) + '\n')
        md = md_report(items_res, probe, None, REGMAP)
        open(os.path.join(a.out, 's0_report%s.md' % suffix), 'w').write('\n'.join(md) + '\n')
        print('\n'.join(md))


if __name__ == '__main__':
    main()

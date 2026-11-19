r"""S103 lane T1: the effect files' technique tables, read from the PLAYER's own files at run time (docs\formats\effect_techniques.md).

  py -I tools\shaders\effect_table.py [--file pac/_startup.pac ...] [--effect NAME] [--json OUT.json] [--names-only OUT.json] [--list]
  py -I tools\shaders\effect_table.py --check            acceptance: the file's table against the open set (arena 4's committed choice tables)
  py -I tools\shaders\effect_table.py --selftest         --check plus the planted twins (each must fail)

For every effect: technique index -> name -> passes -> the vertex / pixel shader (container offset, XXH3 hash of its microcode, the name our fx.json gives that hash)
plus the pass's render-state blocks (raw words, decoded: unconfirmed). Layout (all words big-endian, offsets from the start of the effect body = blob + 12):
  body+0x200  offset of the technique pointer table      body+0x20C  technique count
  technique   +0 name offset, +4 pass count, +16 + 4*pass the pass pointer            (the game: [[effect+16]+512], count [effect+24], word pass+4: sub_82BAEFD8)
  pass        +0 name, +8 id (the pass block), +12 vertex-side constants, +16 pixel-side constants      (sub_82BAE370 reads +8 / +12 / +16)
  pass block  +0..+0x20 nine state-block offsets, +72 -> the cell of the vertex shader, +76 -> the cell of the pixel shader      (sub_82D045B8: [[id+72]] / [[id+76]])
  the shader's container follows its cell (the SDK's container, shaderlib.containers)
Nothing from the game is written but under out\ (--json): names, counts, offsets, hashes and raw state words; --names-only drops the raw state words. Run from a worktree it reads the
main checkout's game_data (tools\shaders\harness\dataroot.py)."""
import argparse
import bisect
import collections
import glob
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'wm30'))
import shaderlib  # noqa: E402

REPO = shaderlib.REPO
FX_DIR = os.path.join(REPO, 'src', 'renderer', 'shaders', 'fx')
SHADER_FILES = ['pac/_startup.pac', 'pac/car/car.pac', 'pac/edit/edit.pac']
CHOICE_DIR = os.path.join(REPO, 'tools', 'harvest', 'choice_table')
OPEN_TABLES = ['arena04_*.json', 'a04__*.json']   # the open set; no other arena's table is ever read
T_OFF, T_COUNT = 0x200, 0x20C


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def cstr(b, o):
    e = b.find(b'\0', o)
    return b[o:e].decode('ascii', 'replace')


class FxNames:
    """XXH3 hash of a shader's microcode -> '<effect>_<variant>_<stage>' from the fx.json files (as tools\\solver\\solve.py Effects / feature_diff.Fx.name_of)."""
    def __init__(self):
        self.by_hash = {'vertex': collections.defaultdict(list), 'fragment': collections.defaultdict(list)}
        self.defines = {}
        for fn in sorted(glob.glob(os.path.join(FX_DIR, '*.fx.json'))):
            d = json.load(open(fn, encoding='utf-8'))
            for stage in ('vertex', 'fragment'):
                for v in d.get(stage, []):
                    nm = '%s_%s_%s' % (d['effect'], v['name'], stage)
                    self.defines[nm] = list(v.get('defines', []))
                    for h in v.get('hashes', []):
                        self.by_hash[stage][int(h, 16)].append((d['effect'], nm))

    def name(self, kind, h, prefer=None):
        c = self.by_hash['vertex' if kind == 'VS' else 'fragment'].get(h, [])
        if not c:
            return None
        for e, nm in c:
            if e == prefer:
                return nm
        return c[0][1]


# S103 B1: the pass's SAMPLER block (pass+16; sub_82BAE370 op 6, 0x82BAE5B0-0x82BAE6D4; docs\formats\effect_techniques.md "sampler block"):
#   +0..+127 eight 16-byte masks of the states the block touches; +128 / +132 / +136 the counts of lists A (value from an int parameter), B (from a float parameter), C (an immediate);
#   entries from +140, 8 bytes each, list A then B then C: u16 stage, u16 state offset, u32 src. Applied as device[468 + offset](device, stage, value).
# state offset: 0 address U, 4 V, 8 W, 12 border, 16 min filter, 20 mag filter, 24 mip filter, 28 LOD bias (sub_82CAAFD0 / sub_82CAAA00 / sub_82CAABA0 / sub_82CAAF60).
STATE_NAMES = {0: 'u', 4: 'v', 8: 'w', 12: 'border', 16: 'min', 20: 'mag', 24: 'mip', 28: 'bias'}


def sampler_block(body, off, problems):
    n = len(body)
    if off == 0:
        return []
    if off + 140 > n:
        problems.append('sampler block %#x out of range' % off)
        return []
    ca, cb, cc = u32(body, off + 128), u32(body, off + 132), u32(body, off + 136)
    if ca + cb + cc > 512 or off + 140 + 8 * (ca + cb + cc) > n:
        problems.append('sampler block %#x: counts %d/%d/%d out of range' % (off, ca, cb, cc))
        return []
    out = []
    for i in range(ca + cb + cc):
        e = off + 140 + 8 * i
        stage, st = struct.unpack_from('>HH', body, e)
        out.append({'list': 'ABC'[0 if i < ca else 1 if i < ca + cb else 2], 'stage': stage, 'state': st, 'src': u32(body, e + 4)})
    return out


def sampler_table(res):
    """effect name -> {stage: {'u','v','w','mag','min','mip'}} (the first pass's entry wins; conflicts counted). The Python twin of src/renderer_scene/sampler_table.h."""
    tab, conflicts = {}, 0
    for e in res.values():
        if e['effect'].lower() in tab:
            continue   # the first effect of a name wins (as read_rule.h / sampler_table.h ParseLeaf)
        t = tab.setdefault(e['effect'].lower(), {})
        for tech in e['techniques']:
            for p in tech['passes']:
                for s in p['samplers']:
                    nm = STATE_NAMES.get(s['state'])
                    if nm is None or s['list'] != 'C':
                        continue
                    d = t.setdefault(s['stage'], {})
                    if nm in d and d[nm] != s['src']:
                        conflicts += 1
                    d.setdefault(nm, s['src'])
    return tab, conflicts


def parse_effect(blob, fx=None, effect=None):
    """One effect blob (3-word header + body) -> {'techniques': [...], 'problems': [...]}."""
    problems = []
    if len(blob) < 12 or u32(blob, 4) + 12 > len(blob):
        return {'techniques': [], 'problems': ['blob header: body size beyond the blob']}
    body = blob[12:12 + u32(blob, 4)]
    n = len(body)
    conts = shaderlib.containers(body)
    starts = [c[0] for c in conts]
    info = {}
    for off, kind, code, mc in conts:
        h = shaderlib.xxh3(mc)
        info[off] = (kind, h)
    out = []
    try:
        tab, cnt = u32(body, T_OFF), u32(body, T_COUNT)
    except struct.error:
        return {'techniques': [], 'problems': ['body too short for the header']}
    if not (0 < cnt < 4096 and tab + 4 * cnt <= n):
        return {'techniques': [], 'problems': ['technique table out of range (table %#x count %d)' % (tab, cnt)]}

    def shader(cell, kind):
        """The container after a shader cell: the first container at or after the cell (cells sit 8 or 16 bytes before it)."""
        i = bisect.bisect_left(starts, cell)
        if i >= len(starts) or starts[i] - cell > 0x20 or info[starts[i]][0] != kind:
            problems.append('%s cell %#x: no %s container within 0x20' % (kind, cell, kind))
            return None
        off = starts[i]
        h = info[off][1]
        return {'cell': cell, 'container': off, 'hash': '%016X' % h, 'name': fx.name(kind, h, effect) if fx else None}

    for ti in range(cnt):
        t = u32(body, tab + 4 * ti)
        if t + 20 > n:
            problems.append('technique %d: struct out of range' % ti)
            continue
        npass = u32(body, t + 4)
        if not 0 < npass < 64:
            problems.append('technique %d: pass count %d' % (ti, npass))
            continue
        tech = {'index': ti, 'name': cstr(body, u32(body, t)), 'passes': []}
        for p in range(npass):
            ps = u32(body, t + 16 + 4 * p)
            pid = u32(body, ps + 8)
            if pid + 80 > n:
                problems.append('technique %d pass %d: block out of range' % (ti, p))
                continue
            vs = shader(u32(body, pid + 72), 'VS')
            px = shader(u32(body, pid + 76), 'PS')
            states = []
            for k in range(9):
                sb = u32(body, pid + 4 * k)
                words = [u32(body, sb + 4 * j) for j in range(4)] if sb + 16 <= n else []
                if any(words):
                    states.append({'block': k, 'offset': sb, 'words': words})
            tech['passes'].append({'pass': p, 'name': cstr(body, u32(body, ps)), 'id': pid, 'vs': vs, 'ps': px, 'state': states,
                                   'consts': [u32(body, ps + 12), u32(body, ps + 16)],
                                   'samplers': sampler_block(body, u32(body, ps + 16), problems)})
        out.append(tech)
    return {'techniques': out, 'problems': problems}


def load_all(files=None, only=None):
    import pac
    fx = FxNames()
    res = {}
    for rel in files or SHADER_FILES:
        data = pac.load(os.path.join(shaderlib.GAME, rel.replace('/', os.sep)))
        for leaf, dec in pac.entries(data):
            table = shaderlib.effect_table(dec) if b'\x10\x2a\x11' in dec else None
            if not table:
                continue
            for name, off, size in table:
                if only and name != only:
                    continue
                key = '%s|%s|%s' % (rel, leaf, name)
                res[key] = dict(parse_effect(dec[off:off + size], fx, name), file=rel, leaf=leaf, effect=name)
    return res


# ---- acceptance ---------------------------------------------------------------------------------------------------------------
# review 1.3's seen map: (effect, technique index) -> defines the colour pair must (and must not) carry
EXPECT = {
    ('yBG_Default', 0): (set(), {'FX_VERTEX_LM', 'FX_SPECULAR', 'FX_ALPHA_SPEC', 'FX_EDGE_FADE', 'FX_SPEC_FADE'}),
    ('yBG_Default', 1): ({'FX_VERTEX_LM'}, {'FX_SPECULAR', 'FX_ALPHA_SPEC', 'FX_EDGE_FADE'}),
    ('yBG_Default', 2): ({'FX_SPECULAR'}, {'FX_VERTEX_LM', 'FX_ALPHA_SPEC', 'FX_SPEC_FADE'}),
    ('yBG_Default', 6): ({'FX_SPECULAR', 'FX_ALPHA_SPEC'}, {'FX_VERTEX_LM', 'FX_SPEC_FADE'}),
    ('yBG_Default', 7): ({'FX_SPECULAR', 'FX_ALPHA_SPEC', 'FX_VERTEX_LM'}, {'FX_SPEC_FADE'}),
    ('yBG_Default', 0xA): ({'FX_EDGE_FADE'}, {'FX_VERTEX_LM', 'FX_SPECULAR'}),
    ('yBG_Default', 0xF): ({'FX_SPECULAR', 'FX_SPEC_FADE', 'FX_ALPHA_SPEC'}, {'FX_VERTEX_LM'}),
}


def defs(fx, nm):
    return set(fx.defines.get(nm, [])) if nm else set()


def choices_seen():
    """effect -> {(vs, ps)} of the open set's meshes (arena 4)."""
    seen = collections.defaultdict(set)
    files = []
    for pat in OPEN_TABLES:
        files += glob.glob(os.path.join(CHOICE_DIR, pat))
    for fn in sorted(files):
        d = json.load(open(fn, encoding='utf-8'))
        if d.get('arena') != 4:
            raise SystemExit('refusing %s: arena %s is not the open set' % (fn, d.get('arena')))
        for m in d.get('meshes', []):
            for lk in m['looks'].values():
                for c in lk['choices']:
                    seen[m['effect']].add((c['vs'], c['ps']))
    return seen, len(files)


def check(res, fx, mutate=None, quiet=False):
    """-> list of failure strings. mutate: planted twin (name) applied to the technique table before checking."""
    fails = []
    ytab = {}
    for key, e in res.items():
        if e['file'] == SHADER_FILES[0]:
            ytab[e['effect']] = e
    if mutate == 'off_by_one':
        # the file's technique indices read one entry late (a planted off-by-one)
        for e in ytab.values():
            for t in e['techniques']:
                t['index'] += 1
    say = (lambda *a: None) if quiet else print
    for (eff, ti), (want, never) in sorted(EXPECT.items()):
        e = ytab.get(eff)
        t = next((x for x in e['techniques'] if x['index'] == ti), None) if e else None
        if not t:
            fails.append('%s technique %#x: not in the file table' % (eff, ti))
            continue
        # a technique's colour pair: some pass whose pair carries the wanted defines; report all passes' pairs
        best = None
        for p in t['passes'][:1]:   # pass 0 = the colour pass (pass 2 = its shadow twin, pass 1 = base)
            v, s = (p['vs'] or {}).get('name'), (p['ps'] or {}).get('name')
            d = defs(fx, v) | defs(fx, s)
            if want <= d and not (never & d):
                best = (p['pass'], v, s)
                break
        say('%s technique %#x %-20s -> %s' % (eff, ti, t['name'], best or 'NO PASS matches %s / never %s' % (sorted(want), sorted(never))))
        if not best:
            fails.append('%s technique %#x (%s): no pass carries %s without %s' % (eff, ti, t['name'], sorted(want), sorted(never)))
    # UVScroll 0 / 2 / 4 / 6 = one to four layers; yProp_Default 0 plain, 3 spec + alpha-gloss
    layers = {0: 1, 2: 2, 4: 3, 6: 4}
    for ti, n in layers.items():
        e = ytab.get('yBG_UVScroll')
        t = next((x for x in e['techniques'] if x['index'] == ti), None) if e else None
        ok = False
        for p in (t['passes'][:1] if t else []):
            v, s = (p['vs'] or {}).get('name'), (p['ps'] or {}).get('name')
            d = defs(fx, v) | defs(fx, s)
            if 'FX_LAYER%d' % n in d if n > 1 else not any('FX_LAYER' in x for x in d) and (v or s):
                ok = True
                say('yBG_UVScroll technique %d (%s) -> pass %d %s / %s' % (ti, t['name'], p['pass'], v, s))
                break
        if not ok:
            fails.append('yBG_UVScroll technique %d: no pass with %d layer(s)' % (ti, n))
    for ti, (want, never) in {0: (set(), {'FX_ALPHA_GLOSS', 'FX_SPECULAR'}), 3: ({'FX_ALPHA_GLOSS'}, set())}.items():
        e = ytab.get('yProp_Default')
        t = next((x for x in e['techniques'] if x['index'] == ti), None) if e else None
        ok = False
        for p in (t['passes'][:1] if t else []):
            v, s = (p['vs'] or {}).get('name'), (p['ps'] or {}).get('name')
            d = defs(fx, v) | defs(fx, s)
            if want <= d and not (never & d) and (v or s):
                ok = True
                say('yProp_Default technique %d (%s) -> pass %d %s / %s' % (ti, t['name'], p['pass'], v, s))
                break
        if not ok:
            fails.append('yProp_Default technique %d: no pass carries %s without %s' % (ti, sorted(want), sorted(never)))
    # the open set: every (vs, ps) pair a mesh of these effects drew in arena 4 is a pair of some technique / pass of the file's effect or its LM / shadow twin
    seen, nfiles = choices_seen()
    filepairs = collections.defaultdict(set)
    for e in ytab.values():
        for t in e['techniques']:
            for p in t['passes']:
                filepairs[e['effect']].add(((p['vs'] or {}).get('name'), (p['ps'] or {}).get('name')))
    allpairs = set().union(*filepairs.values()) if filepairs else set()
    miss = []
    tot = 0
    for eff in ('yBG_Default', 'yBG_UVScroll', 'yProp_Default'):
        for pr in sorted(seen.get(eff, ())):
            tot += 1
            if pr not in allpairs:
                miss.append((eff, pr))
    say('open set: %d tables, %d distinct (effect, vs, ps) in yBG_Default / yBG_UVScroll / yProp_Default; %d not a pair of any file technique' % (nfiles, tot, len(miss)))
    for eff, pr in miss:
        fails.append('arena 4 drew %s %s / %s: not in the file table' % (eff, pr[0], pr[1]))
    return fails


def strip_state(res):
    out = {}
    for k, e in res.items():
        e2 = dict(e)
        e2['techniques'] = [dict(t, passes=[{kk: vv for kk, vv in p.items() if kk != 'state'} for p in t['passes']]) for t in e['techniques']]
        out[k] = e2
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--file', action='append')
    ap.add_argument('--effect')
    ap.add_argument('--json')
    ap.add_argument('--names-only')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    res = load_all(a.file, a.effect)
    nprob = sum(len(e['problems']) for e in res.values())
    print('%d effects, %d techniques, %d passes, %d layout problems' % (len(res), sum(len(e['techniques']) for e in res.values()),
          sum(len(t['passes']) for e in res.values() for t in e['techniques']), nprob))
    for k, e in res.items():
        for pr in e['problems'][:3]:
            print('  PROBLEM', k, pr)
    if a.list:
        for k, e in res.items():
            for t in e['techniques']:
                for p in t['passes']:
                    print('%s  T%-2d %-22s p%d %-4s VS %-48s PS %s' % (k, t['index'], t['name'], p['pass'], p['name'], (p['vs'] or {}).get('name') or (p['vs'] or {}).get('hash'),
                          (p['ps'] or {}).get('name') or (p['ps'] or {}).get('hash')))
    for path, data in ((a.json, res), (a.names_only, strip_state(res))):
        if path:
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            json.dump(data, open(path, 'w'), indent=1, sort_keys=True)
            print('wrote', path)
    rc = 0
    if a.check or a.selftest:
        fx = FxNames()
        fails = check(res, fx)
        print('CHECK', 'PASS' if not fails else 'FAIL')
        for f in fails:
            print('  FAIL', f)
        rc = 1 if fails else 0
        if a.selftest:
            if rc:
                return rc
            import copy
            for twin in ('off_by_one',):
                bad = check(copy.deepcopy(res), fx, mutate=twin, quiet=True)
                print('twin %-12s %s (%d failures)' % (twin, 'CAUGHT' if bad else 'NOT CAUGHT', len(bad)))
                if not bad:
                    rc = 1
    if nprob and (a.check or a.selftest):
        rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main())

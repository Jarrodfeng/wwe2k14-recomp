"""q createscale (S111 CREATEX): the model-create scale per family, game against ours.

    py tools/q/q.py createscale [--func sub_X] [--chain] [--json]

The game creates a model through vtable slot 56 (sub_82BDB460) from a 76-byte create record that sub_82BD2068 initialises
(record +0, the scale, = 10.0 from image word 0x82192140). A family stores its own value at record +0 between the init and the
create (1.0, the character's scale ...) or leaves the 10.0. For every call of the init (q's caller index), this finds each create
that takes that record (the slot-56 call with the record in r5, a direct sub_82BDB460 call, or a call into a create wrapper), reads
the stores to record +0 between the two off the function's listing (qslice's facts), and gives the value in force: a constant, or
the default 10.0 when nothing stores it, or an expression. The key is the last sub_82C351E8 (find by key) argument before the create.
The ours side reads OUR source text for the scale our loader applies to that family, and the verdict is OK / MISMATCH / NO-OURS.

Reads the generated listing and our source; 0 game runs. Derived from the game: print only, never commit the output.
"""
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qslice  # noqa: E402

REPO_OURS = os.path.dirname(os.path.dirname(HERE))      # this checkout (the worktree): our source lives here
INIT = 0x82BD2068            # the create record's init: record +0 = 10.0
INIT_WORD = 0x82192140       # the image word that init stores at +0
CREATE = 0x82BDB460          # the model create (vtable slot 56)
SLOT = 56
WRAPPERS = {0x82648090: 'sub_82648090', 0x82C18668: 'sub_82C18668'}   # calls that carry the record on to the create (q slice settled)
KEYFN = 0x82C351E8           # pach_table_find_index_by_key16(r3 = pach, r4 = key)
ARG_REGS = ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10')


class CreateScaleError(Exception):
    pass


# ------------------------------------------------------------------ the families: what each creator makes, and where ours applies its scale
# (creator address, key or None for any) -> (family name, ours rule). Ours rules: blood / crowd_body / crowd_skel / crowd_prop read our
# source for the value; raw = ours loads the file floats unscaled (value 1); char = the per-body table (body_scale.h); none = not loaded.
FAMILIES = [
    (0x824ECAD8, 32000, 'Blood splat (bg key 32000)', 'blood'),
    (0x824ECAD8, 31000, 'Crowd camera flash (bg key 31000)', 'raw'),
    (0x824ECAD8, 5000, 'EtcObj (bg key 5000)', 'none'),
    (0x8250A200, None, 'Crowd bodies (keys 20000+n ...)', 'crowd_body'),
    (0x82507408, None, 'Crowd skeleton (key 20200)', 'crowd_skel'),
    (0x825074C8, None, 'Crowd skeleton (key 20200)', 'crowd_skel'),
    (0x8250ACA8, None, 'Audience props (keys 40000 / 40100)', 'crowd_prop'),
    (0x8250B1E8, None, 'Sign boards / foam hands (keys 40999+k)', 'crowd_prop'),
    (0x824E78C8, None, 'Ring plane shadow', 'raw'),
    (0x824E7690, None, 'Ring turnbuckle models', 'raw'),
    (0x824EB400, None, 'Ring / stage MakeModel', 'raw'),
    (0x824EC3C0, None, 'Titantron screens', 'raw'),
    (0x824ED698, None, 'Stage slots', 'raw'),
    (0x825771E8, None, 'STG structures', 'raw'),
    (0x8258E270, None, 'Weapons / ring steps (MakeSkinModel)', 'raw'),
    (0x8272D710, None, 'Entrance event props', 'raw'),
    (0x83421B30, None, 'Character plane-shadow proxy', 'raw'),
    (0x8271ED10, None, 'Wrestler body (Factory_Model)', 'char'),
    (0x8271EF80, None, 'Wrestler sub-models', 'char'),
    (0x825A20A0, None, 'Base character / attire pieces', 'char'),
    (0x8259E720, None, 'SUBM attire pieces', 'char'),
]
# creators whose record init feeds no create (settled by CREATESCALE.md): a row 'no create' is the answer there
NO_CREATE = {0x8250C1D0: 'crowd builder', 0x8271D9D8: 'part-tag tables take the record by value', 0x82BD98C8: 'the model ctor\'s own +528 copy'}


def family_of(creator, key):
    for c, k, name, rule in FAMILIES:
        if c == creator and (k is None or k == key):
            return name, rule
    return None, None


# ------------------------------------------------------------------ ours: what scale our loaders apply (from source text)
def _read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as fh:
            return fh.read()
    except OSError:
        return None


def _body(text, head):
    """the text of the function whose head line starts at `head` up to the first line that is a lone closing brace"""
    i = text.find(head)
    if i < 0:
        return None
    j = text.find('\n}\n', i)
    return text[i:j if j > 0 else len(text)]


HEADER = 'src/assets/model_create_scale.h'      # the one place the scale values live (kDefaultCreateScale, kCrowdCreateScale ...)
TOKEN = r'[\w:.]+'                              # a literal (10.0f) or a constant's name (create_scale::kCrowdCreateScale)


def _num(s):
    return float(s.rstrip('fF'))


def resolve(root, tok, depth=0):
    """-> the value of a scale token our source uses: a number literal, or a constant defined in model_create_scale.h (followed through
    `constexpr float A = B;`); None when it names nothing there."""
    tok = tok.strip()
    if re.fullmatch(r'\d+\.?\d*[fF]?|\.\d+[fF]?', tok):
        return _num(tok)
    h = _read(root, HEADER)
    if h is None or depth > 8:
        return None
    m = re.search(r'\b' + re.escape(tok.split('::')[-1]) + r'\s*=\s*(' + TOKEN + r')\s*;', h)
    return resolve(root, m.group(1), depth + 1) if m else None


def _scaled(root, tok, where):
    """-> (value, text) for a site that applies the token `tok`"""
    v = resolve(root, tok)
    if v is None:
        return None, f'{where}: {tok} (its value was not found in {HEADER})'
    return v, f'{where}: {tok}' + ('' if re.fullmatch(r'[\d.]+[fF]?', tok) else f' = {v:g} ({HEADER})')


def ours_value(rule, root):
    """-> (value or None, text, found): value = the scale our loader applies (None: none applied = 1); found False = no rule source."""
    if rule == 'blood':
        a = _read(root, 'src/assets/asset_load.h')
        body = _body(a, 'inline bool LoadBloodEntry(') if a else None
        if body is None:
            return None, 'LoadBloodEntry not found in src/assets/asset_load.h', False
        m = re.search(r'ApplyCreateScale\(\s*\w+\s*,\s*(' + TOKEN + r')\s*\)', body)
        if not m:
            return None, 'asset_load.h LoadBloodEntry: no create scale applied (file floats)', True
        v, txt = _scaled(root, m.group(1), 'asset_load.h LoadBloodEntry ApplyCreateScale')
        return v, txt, True
    if rule == 'crowd_body':
        vals = []
        for rel in ('src/renderer_scene/live_crowd.cpp', 'src/renderer_scene/crowd_ours.cpp'):
            t = _read(root, rel)
            m = re.search(r'constexpr float kBodyScale\s*=\s*(' + TOKEN + r')\s*;', t or '')
            if m:
                vals.append(_scaled(root, m.group(1), rel.rsplit('/', 1)[1] + ' kBodyScale') + (rel,))
        if not vals:
            return None, 'no kBodyScale in live_crowd.cpp / crowd_ours.cpp', False
        txt = '; '.join(t for _v, t, _f in vals)
        if any(v is None for v, _t, _f in vals) or len({v for v, _t, _f in vals}) > 1:
            return None, txt + ' (unresolved or the two disagree)', True
        return vals[0][0], txt, True
    if rule == 'crowd_skel':
        a = _read(root, 'src/assets/asset_load.h') or ''
        m = re.search(r'const float sc = (' + TOKEN + r') \*', a)
        if not m:
            return None, 'no crowd member scale `sc = SCALE *` in asset_load.h', False
        v, txt = _scaled(root, m.group(1), 'asset_load.h crowd member matrix sc')
        return v, txt + ' x lerp (viewer; live = the game\'s palette x stored vertex x scale)', True
    if rule == 'crowd_prop':
        t = _read(root, 'src/assets/crowd_prop_game_space.cpp')
        body = _body(t, 'bool ConvertJboy(') if t else None
        if body is None:
            return None, 'ConvertJboy not found in crowd_prop_game_space.cpp', False
        m = re.search(r'constexpr float s\s*=\s*(' + TOKEN + r')\s*;', body) or re.search(r'\bw\[0\]\s*\*\s*(' + TOKEN + r')', body)
        if not m:
            return None, 'crowd_prop_game_space.cpp ConvertJboy: no position scale (file floats)', True
        v, txt = _scaled(root, m.group(1), 'crowd_prop_game_space.cpp ConvertJboy')
        return v, txt, True
    if rule == 'raw':
        return 1.0, 'loader reads the file floats as they are (no create scale in our source)', True
    if rule == 'char':
        t = _read(root, 'src/renderer_scene/body_scale.h')
        if t is None or 'class BodyScaleTable' not in t:
            return None, 'no BodyScaleTable', False
        return 'table', 'body_scale.h BodyScaleTable: one measured per-body float (S103 W2) on the palette rows', True
    return None, 'not loaded by ours', False


# ------------------------------------------------------------------ the game side
def fmt(v):
    return f'{v:.6g}'


def const_of(src, e):
    """the constant an expression is (int constant; a float image word), else None"""
    if e[0] == 'c':
        return e[1]
    if e[0] == 'ld' and e[2][0] == 'c' and 0x82000000 <= e[2][1] < 0x83000000:
        w = src.word(e[2][1])
        if w is not None:
            return struct.unpack('>f', struct.pack('>I', w))[0]
    return None


def classify(F, call, rec):
    """what a call that takes the record is: 'create' (slot-56 call / direct create), 'wrapper X', or None"""
    k = call['k']
    regs = call['regs']
    if k in ('V', 'TAILV'):
        ctr = call.get('ctr')
        while ctr and ctr[0] == 'def' and F['defs'].get(f'{ctr[1]}@{ctr[2]:X}') is not None:    # a long value qdecomp named: its full form
            ctr = F['defs'][f'{ctr[1]}@{ctr[2]:X}']
        if regs.get('r5') == rec and ctr and ctr[0] == 'ld' and ctr[2][0] == 'add' and ctr[2][2] == SLOT:
            return 'create'
        return None
    t = call['t']
    if t == CREATE and any(regs.get(r) == rec for r in ARG_REGS):
        return 'create'
    if t in WRAPPERS and any(regs.get(r) == rec for r in ARG_REGS):
        return 'via ' + WRAPPERS[t]
    return None


def key_at(sl, F, rel):
    """the key the function looked up last before the create: the last find-by-key call before it (a constant, or its expression)"""
    best = None
    for r, c in sorted(F['calls'].items()):
        if r < rel and c.get('t') == KEYFN and 'r4' in c['regs']:
            best = c['regs']['r4']
    if best is None:
        return None, ''
    if best[0] == 'c':
        return int(best[1]), str(int(best[1]))
    return None, sl.clip(sl.render(F, best, None, 'expr'), 40)


def analyse(src, addr, init=INIT, create=CREATE, only=None):
    """-> rows, one per (init, create) pair of the function at addr; an init that no create takes is one row with no create."""
    F = src.facts(addr)
    if F is None:
        return []
    sl = qslice.Slicer(src)
    default = src.word(INIT_WORD)
    default = struct.unpack('>f', struct.pack('>I', default))[0] if default is not None else 10.0
    inits = sorted(r for r, c in F['calls'].items() if c.get('t') == init)
    rows = []
    for ir in inits:
        rec = F['calls'][ir]['regs'].get('r3')
        base = {'creator': addr, 'creator_name': F['name'], 'init': ir}
        if rec is None or not qdecomp.is_stack(rec):
            rows.append(dict(base, create=None, note='the record is not a stack local here (' + (sl.clip(sl.render(F, rec, None, 'expr'), 50) if rec else '?') + '): not followed'))
            continue
        off = qdecomp.stack_off(rec)
        nxt = min([r for r in inits if r > ir and F['calls'][r]['regs'].get('r3') == rec] or [1 << 30])
        found = False
        for cr in sorted(r for r in F['calls'] if ir < r < nxt):
            call = F['calls'][cr]
            kind = classify(F, call, rec)
            if kind is None:
                continue
            found = True
            stores = [s for s in F['stores'] if s['stk'] == off and s['w'] == 4 and ir < s['o'] < cr]
            vals = []
            for s in stores:
                c = const_of(src, s['val'])
                vals.append((s['o'], c, sl.clip(sl.render(F, s['val'], None, 'expr'), 90), s['val']))
            if not stores:
                g = {'kind': 'const', 'value': default, 'text': fmt(default) + ' (the init default: nothing stores record +0)'}
            elif all(v[1] is not None for v in vals) and len({v[1] for v in vals}) == 1:
                g = {'kind': 'const', 'value': float(vals[0][1]), 'text': fmt(float(vals[0][1])) + f' (stored at +0x{vals[0][0]:X})'}
            else:
                g = {'kind': 'expr', 'value': None, 'text': ' | '.join(f'{v[2]} (+0x{v[0]:X})' for v in vals)}
                g['vals'] = vals
            kv, ktxt = key_at(sl, F, cr)
            rows.append(dict(base, create=cr, how=kind, key=kv, key_text=ktxt, game=g))
        if not found:
            rows.append(dict(base, create=None, note='no create takes this record'))
    return rows


def judge(row, root):
    """fills family / ours / verdict / mark on one analysed row"""
    if row['create'] is None:
        row['family'] = NO_CREATE.get(row['creator'], '(init only)')
        row.update(ours='-', verdict='n/a', mark='C')
        return row
    name, rule = family_of(row['creator'], row['key'])
    row['family'] = name or f'(unclassed: {row["creator_name"]})'
    g = row['game']
    if rule is None:
        row.update(ours='no ours rule for this creator', verdict='NO-OURS', mark='L')
        return row
    ov, otxt, found = ours_value(rule, root)
    row['ours'] = otxt
    row['ours_value'] = ov if ov != 'table' else None
    if rule == 'none' or not found:
        row.update(verdict='NO-OURS', mark='C' if rule == 'none' else 'L')
        return row
    if ov == 'table':
        row.update(verdict='OK' if g['kind'] == 'expr' else 'MISMATCH', mark='L')    # a per-body float: same float is [L] (S103 W2)
        return row
    gv = g['value']
    if gv is None:
        row.update(verdict='MISMATCH', mark='L')     # an expression scale against a fixed ours value
        return row
    ovv = 1.0 if ov is None else ov
    row.update(verdict='OK' if math.isclose(gv, ovv, rel_tol=1e-6) else 'MISMATCH',
               mark='C' if rule in ('blood', 'crowd_body', 'crowd_prop', 'crowd_skel') else 'L')
    if ov is None and row['verdict'] == 'MISMATCH':
        row['ours'] += ' (value 1)'
    return row


def run(src, root=REPO_OURS, init=INIT, func=None):
    cs = src.callers(init)
    addrs = sorted({s for s, _site, kind in cs if kind == 'call' and s != init})
    if func is not None:
        addrs = [a for a in addrs if a == func]
    rows = []
    for a in addrs:
        for r in analyse(src, a, init):
            rows.append(judge(r, root))
    return rows


ORDER = {'MISMATCH': 0, 'NO-OURS': 1, 'OK': 2, 'n/a': 3}


def table_lines(rows):
    rows = sorted(rows, key=lambda r: (ORDER[r['verdict']], r['creator'], r['init']))
    out = ['| # | creator init -> create | family | key | game scale | ours | verdict |', '|---|---|---|---|---|---|---|']
    for i, r in enumerate(rows, 1):
        cr = f'{r["creator_name"]} +0x{r["init"]:X}' + (f' -> +0x{r["create"]:X} ({r["how"]})' if r['create'] is not None else f' ({r.get("note", "")})')
        game = (r['game']['text'] if r['create'] is not None else '-') + (' [C]' if r['create'] is not None else '')
        out.append(f'| {i} | {cr} | {r["family"]} | {r.get("key_text") or "-"} | {game} | {r["ours"]} | **{r["verdict"]}** [{r["mark"]}] |')
    cnt = {}
    for r in rows:
        cnt[r['verdict']] = cnt.get(r['verdict'], 0) + 1
    out.append('')
    out.append(f'{len(rows)} rows: ' + ', '.join(f'{k} {cnt[k]}' for k in ORDER if k in cnt))
    return out


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q createscale', description=__doc__.split('\n')[0])
    ap.add_argument('--func', help='only this creator (sub_X)')
    ap.add_argument('--chain', action='store_true', help='for an expression scale: the q slice chain of the store')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args(argv)
    src = qslice.QSource(con, use_cache=not a.no_cache)
    func = None
    if a.func:
        import q as qmod
        try:
            func = qmod.resolve(con, a.func)[0]
        except Exception as ex:  # noqa: BLE001 - q.py run as __main__ owns another QError class
            if type(ex).__name__ == 'QError':
                raise CreateScaleError(str(ex))
            raise
    rows = run(src, REPO_OURS, INIT, func)
    if a.json:
        for r in rows:
            r.get('game', {}).pop('vals', None)
        print(json.dumps(rows, indent=1, default=str))
        return 0
    print(f'q createscale: record init sub_{INIT:08X} (record +0 = {fmt(struct.unpack(">f", struct.pack(">I", src.word(INIT_WORD)))[0])}), create sub_{CREATE:08X} (slot {SLOT}); '
          f'ours = {REPO_OURS}')
    print('\n'.join(table_lines(rows)))
    if a.chain:
        sl = qslice.Slicer(src)
        for r in rows:
            g = r.get('game')
            if g and g['kind'] == 'expr':
                F = src.facts(r['creator'])
                print(f'\n{r["creator_name"]} +0x{r["create"]:X} (record +0):')
                for line in qslice.tree_lines(sl.value(F, g['vals'][0][3], 0, None, f'store +0x{g["vals"][0][0]:X}'), 1):
                    print(line)
    return 0

r"""qexprvals (S111 EXPRVAL): `q exprdiff --values`. For each exprdiff row whose two sides are material-record values (a named parameter of
the record, a lane of it, or a default), evaluate BOTH sides on every record of the effect in the asset index (asset.s6.db, all sources:
game and mods; one record = one material row = one mesh) and count the records where they differ. A structural DIFF with no data
difference reads `DIFF, 0 of M records differ`; a row whose game value is the image constant and ours the Material default counts the
records that lack the name.

The value of a leaf on a record:
  game  mat:NAME[i]       the record's NAME[i]; a record without it = the game writes nothing (the register keeps the last draw's value):
                          counted apart, not compared
        mt:MEMBER[i]      the same, for a row whose game side is one parameter (the row's own game name), bound to our member
        code:NAME[i]      only the names GAME_CODE_VALUES declares (a fixed image constant the game's code writes): the record's own value
                          when it names NAME [L: the part's material write comes after], else the constant
        a number          itself (an image constant)
  ours  mt:MEMBER[i]      the record's value of the parameter(s) our loader reads into MEMBER[i] (the inverse of qexprdiff.load_mat_bind,
                          first named one), else Material's default for MEMBER (parsed from src/assets/asset_model.h struct Material)
Every other leaf (ours:*, bind:*, code: of another name, a ?cut leaf) is no record value: the row prints `not a record value: LEAF`.
"""
import json
import math
import re
import sqlite3

# (descriptor name) -> (the four lanes of the constant, where it comes from). The game's code writes it when the record names nothing.
GAME_CODE_VALUES = {
    'g_fvLightLimit': ((100.0, 100.0, 100.0, 1.0),
                       'the light handler sub_82BE06B8 +BF4 from the image constant 0x8206B310 [C: docs/research/s111/REFLW.md]; a record that '
                       'names it overrides [L: written after the light handler]'),
}
# members whose default BeginRecord (src/assets/material_floats.h) changes for the yCh family
YCH_DEFAULTS = {'spec_pow': 1.0}
EXAMPLES = 3


class NotRecord(Exception):
    """a leaf of the row is not a value of a material record"""


def open_index():
    import qasset
    p = qasset.db_path()
    try:
        con = sqlite3.connect('file:%s?mode=ro' % p, uri=True)
        con.row_factory = sqlite3.Row
        con.execute('SELECT COUNT(*) FROM material').fetchone()
    except sqlite3.Error as e:
        raise RuntimeError('q exprdiff --values: no asset index at %s (%s): run  py tools/q/q.py asset update' % (p, e))
    return con


# ------------------------------------------------------------------ our Material defaults
def _split_commas(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '{(':
            depth += 1
        elif ch in '})':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    return out + [cur]


def _num(t):
    t = t.strip().rstrip('fF').rstrip('uU')
    if t in ('true', 'false'):
        return 1.0 if t == 'true' else 0.0
    return float(t)


def material_defaults(text):
    """-> {'ref_col[3]': 1.0, 'spec_pow': 300.0, ...} of the default member initializers of `struct Material` (comments already masked)"""
    i = text.find('struct Material')
    if i < 0:
        return {}
    j = text.find('\n};', i)
    body = text[i:j if j > 0 else len(text)]
    body = body[body.find('{') + 1:]
    out = {}
    for stmt in body.split(';'):
        m = re.match(r'\s*(?:float|int|bool|uint32_t|int32_t)\s+(.*)$', stmt, re.S)
        if not m:
            continue
        for d in _split_commas(m.group(1)):
            dm = re.match(r'\s*(\w+)\s*(?:\[(\d+)\])?\s*=\s*(.+?)\s*$', d, re.S)
            if not dm:
                continue
            name, n, val = dm.group(1), dm.group(2), dm.group(3)
            try:
                if val.startswith('{'):
                    for k, x in enumerate(v for v in val.strip('{} \n').split(',') if v.strip()):
                        out[f'{name}[{k}]'] = _num(x)
                elif n is not None:
                    continue
                else:
                    out[name] = _num(val)
            except ValueError:
                continue
    return out


def invert(mat_bind):
    """{fam: {member text: [(name, index), ...]}} from {fam: {'NAME[i]': member text}}"""
    out = {}
    for fam, tbl in mat_bind.items():
        inv = out.setdefault(fam, {})
        for k, member in tbl.items():
            m = re.match(r'^(\w+)\[(\d+)\]$', k)
            if m:
                inv.setdefault(member, []).append((m.group(1), int(m.group(2))))
    return out


# ------------------------------------------------------------------ the records
class Records:
    """the records of an effect (exact name, case-insensitive), as distinct parameter sets with their counts"""

    def __init__(self, con):
        self.con = con
        self.cache = {}

    def sets(self, effect):
        k = effect.lower()
        if k not in self.cache:
            out = []
            for r in self.con.execute('SELECT params, COUNT(*) AS n, MIN(entry_id) AS first FROM material WHERE effect_lc = ? GROUP BY params', (k,)):
                try:
                    p = json.loads(r['params'])
                except ValueError:
                    continue
                out.append((p, r['n'], r['first'], r['params']))
            self.cache[k] = out
        return self.cache[k]

    def ids(self, effect, params_text, n=EXAMPLES):
        return [r[0] for r in self.con.execute('SELECT entry_id FROM material WHERE effect_lc = ? AND params = ? ORDER BY entry_id LIMIT ?',
                                               (effect.lower(), params_text, n))]


def _param(rec, name, idx):
    v = rec.get(name)
    if isinstance(v, (list, tuple)) and 0 <= idx < len(v) and isinstance(v[idx], (int, float)):
        return float(v[idx])
    return None


def _split_leaf(x):
    m = re.match(r'^(\w+):(\w+?)(?:\[(\d+)\])?$', x)
    return (m.group(1), m.group(2), int(m.group(3) or 0)) if m else (None, x, 0)


# ------------------------------------------------------------------ evaluation
class Ctx:
    def __init__(self, inv, defaults):
        self.inv, self.defaults = inv, defaults


def side_values(X, tree, side, row, rec, fam, ctx, used):
    """-> {leaf: value} for every leaf of `tree` on one record; None as a value = the record gives the game nothing (game side only).
    `used` collects what stood in for a missing name ('lack NAME')"""
    env = {}
    for lf in sorted(X.leaves(tree) | X._cond_leaves(tree)):
        kind, name, idx = _split_leaf(lf)
        if '?' in lf:
            raise NotRecord(lf)
        if side == 'game':
            if kind == 'mat':
                env[lf] = _param(rec, name, idx)
                if env[lf] is None:
                    used.add(name)
            elif kind == 'mt' and tree[0] == 'leaf' and row.get('_gn'):
                gname, gidx = row['_gn']
                env[lf] = _param(rec, gname, gidx)
                if env[lf] is None:
                    used.add(gname)
            elif kind == 'code' and name in GAME_CODE_VALUES:
                v = _param(rec, name, idx)
                if v is None:
                    v = GAME_CODE_VALUES[name][0][idx]
                    used.add(name)
                env[lf] = v
            else:
                raise NotRecord(lf)
        else:
            if kind != 'mt':
                raise NotRecord(lf)
            member = lf[3:]
            names = ctx.inv.get(fam, {}).get(member, [])
            v = None
            for nm, ix in names:
                v = _param(rec, nm, ix)
                if v is not None:
                    break
            if v is None:
                v = 0.0 if 'no_ours_default' in X.PLANT else ctx.defaults.get(member)   # (selftest twin: a default that is always 0)
                if v is not None and fam == 'ych' and member in YCH_DEFAULTS:
                    v = YCH_DEFAULTS[member]
                if v is None:
                    raise NotRecord(lf + ' (no Material default)')
                used.add(names[0][0] if names else member)
            env[lf] = v
    return env


def eval_row(X, row, recs, ctx):
    """-> dict(m, differ, absent, lack, examples) of one row over every record of its effect, or raises NotRecord"""
    g, o = row.get('_g'), row.get('_o')
    if g is None or o is None:
        raise NotRecord('the row has no expression pair (a draw input or PASS-NEVER)')
    fam = 'ych' if row['effect'].lower().startswith('ych') else 'other'
    res = dict(m=0, differ=0, absent=0, lack=0, examples=[], lack_names=set(), differ_ids=[])
    for rec, n, first, ptxt in recs.sets(row['effect']):
        used_g, used_o = set(), set()
        genv = side_values(X, g, 'game', row, rec, fam, ctx, used_g)
        oenv = side_values(X, o, 'ours', row, rec, fam, ctx, used_o)
        res['m'] += n
        if any(v is None for v in genv.values()):
            res['absent'] += n
            continue
        if used_g | used_o:
            res['lack'] += n
            res['lack_names'] |= used_g | used_o
        try:
            a, b = X._ev(g, genv), X._ev(o, oenv)
        except (ZeroDivisionError, X._NoEval, KeyError) as e:
            raise NotRecord('cannot evaluate: %s' % type(e).__name__)
        if not math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-9):
            res['differ'] += n
            res['differ_ids'].append((ptxt, first, a, b))
    return res


def group_values(X, rows, recs, ctx):
    """one exprdiff group (rows of one field / lane over several effects) -> dict(verdict, m, differ, absent, lack, effects, examples, lack_names) | dict(why)"""
    out = dict(m=0, differ=0, absent=0, lack=0, effects=0, examples=[], lack_names=set())
    seen = set()
    for r in rows:
        if r['effect'] in seen:
            continue
        seen.add(r['effect'])
        try:
            e = eval_row(X, r, recs, ctx)
        except NotRecord as ex:
            return dict(why=str(ex))
        for k in ('m', 'differ', 'absent', 'lack'):
            out[k] += e[k]
        out['effects'] += 1
        out['lack_names'] |= e['lack_names']
        for ptxt, first, a, b in e['differ_ids']:
            if len(out['examples']) < EXAMPLES:
                for i in recs.ids(r['effect'], ptxt, EXAMPLES - len(out['examples'])):
                    out['examples'].append((i, r['effect'], a, b))
    return out


def fmt(v):
    return '%g' % v


def lines(gv, verdict):
    """the printed lines of one group's values"""
    if 'why' in gv:
        return ['values: not a record value: %s' % gv['why']]
    if not gv['m']:
        return ['values: %d effect(s) covered, 0 records of them in the asset index (nothing to compare)' % gv['effects']]
    cmp_ = gv['m'] - gv['absent']
    s = '%s, %d of %d records differ' % (verdict, gv['differ'], gv['m'])
    extra = []
    if gv['absent']:
        extra.append('%d record(s) name no parameter the game reads: it writes nothing, not compared' % gv['absent'])
    if gv['lack']:
        extra.append('%d of the %d compared lack %s: a default stands in' % (gv['lack'], cmp_, ', '.join(sorted(gv['lack_names']))))
    out = ['values: ' + s + '  [%d effect(s), asset index, all sources]' % gv['effects'] + ('; ' + '; '.join(extra) if extra else '')]
    for i, eff, a, b in gv['examples']:
        out.append('       e.g. entry %d (%s): game %s, ours %s' % (i, eff, fmt(a), fmt(b)))
    return out

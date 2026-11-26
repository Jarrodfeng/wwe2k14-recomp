r"""Self-test of `q guestdiff` (S111 GUESTDIFF): `py -I tools/q/qguestdiff_selftest.py [--root DIR]`   (0 game runs)

A. Synthetic text (invented, no game code): the block walker's write forms (`mat.F = `, `mat.F[i] = `, `slot = &mat.F`, the vec4
   chain's `? mat.F` arm, the `(c ? mat.F : mat.G) = v` lvalue), what is NOT a write (`slot == &mat.G`, `dst == mat.G`, a read in a
   condition), names from the line / the enclosing block / the tested field, the arena, prop and plant conditions, the Material
   defaults parser. Each must read back as written.
B. Real data (needs the asset index; else SKIP), our code frozen at two commits, the two holes found by hand on S111 RE-FOUND:
     1. at FROZEN_HAIR (a8a57ec72, the brief's commit): hair_col1 / hair_col2 / hair_shift / hair_exp are LOOK (stock fills them by
        name, the guest build keeps the default, ResolveChar SpeCol1 / SpeCol2 / HairSpeParam reads them for yCh_Aniso_trans, the
        CAW records carry other values); diffuse / normal BOTH;
     2. at FROZEN_CUBE (147954109, the ENVCUBE landing: at a8a57ec72 the stock loader had no env_cube yet, so the cube hole cannot
        be stated there): env_cube is LOOK, read at slot 10 (EnvCube).
C. Planted twins on the FROZEN_HAIR text, each run through the same check that passes in B and seen to FAIL on it (caught):
     guest gains a hair_col1 write (LOOK must go: BOTH), guest loses the Normal case (normal must become LOOK), guest gains a field
     the loader never fills (GUEST-ONLY), the SpeCol1 resolver line renamed (hair_col1 no longer LOOK: nothing reads it), every
     CAW g_f4SpeCol1 value set to the default (SAME), the name dropped from the CAW records (NOREC); and two that must NOT change
     the verdict: the planted hair_col1 write behind `if (flat_diffuseless)` (a debug plant, not a fill: still LOOK), and a helper
     call passing `mat` that has no definition (named as unfollowed, never silently dropped).
D. The followed calls (S111 GUESTDIFF review), our code frozen at FROZEN_GUESTHAIR (752a63e1e, the GUESTHAIR landing: jboy.h's float
   chain moved into src/assets/material_floats.h ApplyFloat, which the stock loader calls and the guest build reaches through
   guestfloats::ApplyRows): every float field LOOK at FROZEN_HAIR is BOTH there, the hair fields' fills on both sides are read in
   material_floats.h, no field FROZEN_HAIR has goes missing (before the review 44 did: neither side's call was followed). Planted
   twins: the ApplyRows call dropped, ApplyRows no longer handing `mat` to ApplyFloat (the second level), the call put behind
   `if (g_draw_plant == 18)` (a plant): hair_col1 must stop being BOTH in each.
E. The NOREAD reasons (S111 UNREADNAMES, tools/q/qguestdiff_noread.py): a glob row covers its names and a new name stays bare
   (synthetic); on the working tree every CAW name no path reads has a reason and no row is stale. Planted twins: the IdMapLv row
   dropped (its eight names must come back bare), a row for a name no record carries (must be reported stale).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import argparse
import copy
import json
import os
import subprocess
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qguestdiff as G   # noqa: E402
import qtexdiff as T     # noqa: E402
import qguestdiff_noread as NR   # noqa: E402

FROZEN_HAIR = 'a8a57ec72'   # S111: the brief's commit (the hair floats hole stated against it)
FROZEN_CUBE = '147954109'   # S111 ENVCUBE landing: the first commit whose loader fills env_cube (the guest build unchanged)
FROZEN_GUESTHAIR = '752a63e1e'   # S111 GUESTHAIR landing: both paths fill the float fields through material_floats.h ApplyFloat
FLOAT_ROWS = 'src/renderer_scene/guest_model_float_rows.h'
TABLE = 'src/assets/material_floats.h'
FAILS, CAUGHT = [], []
MEMO = {}   # the fx registry and the src searches per ref, read once for every run below


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, msg):
    """msg = the check's failure message on a planted case; None = the check still passed on it (not caught: a defect)."""
    if msg is None:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name}')
    else:
        print(f'ok   planted twin caught: {name} ({msg})')


# ------------------------------------------------------------------ A. synthetic text
SYN_MODEL = r'''
struct Material {
  std::string shader;  // a name
  int diffuse = -1, normal = -1;
  float ambient[4] = {1, 1, 1, 1};   // a comment, with = and , inside
  float spec_pow = 300, hair_tile[2] = {10, 1};
  bool has_x = false;
};
'''
SYN_JBOY = r'''
    Material mat;
    mat.shader = CString(r.b + rec + 0x7C, 16);
    mat.vertex_lm = o.arena && mat.shader.compare(0, 4, "yBG_") == 0;
    if (pname == "g_mDiffusePrm" || pname == "sampDiffusePrm") slot = &mat.diffuse;
    else if (pname == "sampLightMapPrm" && !skin_streams) slot = &mat.lightmap;
    if (slot == &mat.diffuse) mat.diffuse_name = CString(r.b, 16);
    float* dst = pname == "g_f4MatAmbCol"    ? mat.ambient
                 : pname == "g_f4SpeCol1"    ? mat.hair_col1
                                             : nullptr;
    if (dst == mat.ambient) mat.has_x = true;
    if (pname == "g_f4IdMapCol") {
      for (int c = 0; c < 4; ++c) mat.idmap_col[c] = r.f32(po);
    }
    else if (pname == "g_fHairTileV") mat.hair_tile[1] = r.f32(po + 20);
    else if (pname.rfind("g_fScroll", 0) == 0) {
      mat.scroll[2 * layer + comp] = r.f32(po + 20), mat.scroll_have = true;
    }
    if (o.arena && TqfClass(sl)) {
      mat.vertex_alpha_lt255 = true;
    }
    if (load_detail::OldPlant() && pname == "g_fX") mat.fur_level = 1.f;
    if (pname == "g_fY" && !load_detail::OldPlant()) mat.dif_amb_level = 2.f;
    m.materials.push_back(mat);
'''
SYN_GUEST = r'''
    assets::Material mat;
    mat.shader = shader;
      switch (slot) {
        case TexSlot::Normal: mat.normal = ti; break;
      }
      (li == 0 ? mat.logo7 : mat.logo4) = ti;
    if (!have_diffuse) {
      if (flat_diffuseless) {
        for (int k = 0; k < 4; ++k) mat.diffuse_color[k] = mat.ambient[k] = 1.f;
      }
    }
    Helper(mat, 3);
    out.materials.push_back(mat);
'''
SYN_CALLS = r'''
    assets::Material mat;
    if (g_draw_plant != 18) mkey += ns::Apply(t, mat);
    Outer(Inner(mat), 3);
    Logged(mat.shader);
    if (o.arena) ArenaOnly(mat);
    int* slot = ns::Table(mat, pname, ns::Ctx{o.arena, skin_streams}, false);
    if (g_draw_plant == 3) Fix(mat);
    out.materials.push_back(mat);
'''


def part_a():
    d = G.parse_defaults(SYN_MODEL)
    check('A defaults: every declarator', sorted(d) == ['ambient', 'diffuse', 'hair_tile', 'has_x', 'normal', 'shader', 'spec_pow'], str(sorted(d)))
    check('A defaults: values', d['ambient']['vals'] == [1, 1, 1, 1] and d['spec_pow']['vals'] == [300] and d['hair_tile']['vals'] == [10, 1]
          and d['diffuse']['vals'] == [-1] and d['has_x']['vals'] == [0] and d['shader']['vals'] is None, str({k: v['vals'] for k, v in d.items()}))
    fills, _c = G.walk(SYN_JBOY, r'^\s*Material mat;', r'^\s*m\.materials\.push_back\(mat\);', 'syn', 'mat', G.jboy_names, False)
    fills = G.resolve_via(fills)
    by = {}
    for f in fills:
        by.setdefault(f['field'], []).append(f)
    check('A no write from a comparison (slot == &mat.G, dst == mat.G)', [f['line'] for f in by['diffuse']] == [5] and len(by['ambient']) == 1,
          str([(f['field'], f['line']) for f in fills]))
    check('A names on the line', by['diffuse'][0]['names'] == ['g_mDiffusePrm', 'sampDiffusePrm'] and by['hair_col1'][0]['names'] == ['g_f4SpeCol1'], '')
    check('A names through the tested field', by['diffuse_name'][0]['names'] == ['g_mDiffusePrm', 'sampDiffusePrm'] and by['has_x'][0]['names'] == ['g_f4MatAmbCol'],
          str(by['diffuse_name'][0]) + str(by['has_x'][0]))
    check('A names from the block', by['idmap_col'][0]['names'] == ['g_f4IdMapCol'] and by['scroll'][0]['names'] == ['g_fScroll*'] and by['scroll_have'][0]['names'] == ['g_fScroll*'], '')
    check('A indices', by['hair_tile'][0]['idx'] == 1 and by['idmap_col'][0]['idx'] == 'var' and by['ambient'][0]['idx'] is None, '')
    check('A derived (no name)', by['shader'][0]['names'] == [] and not by['shader'][0]['arena'], str(by['shader'][0]))
    check('A arena / prop conditions', by['vertex_lm'][0]['arena'] and by['vertex_alpha_lt255'][0]['arena'] and by['lightmap'][0]['arena'] is False, '')
    check('A plant conditions', by['fur_level'][0]['plant'] and not by['dif_amb_level'][0]['plant'], '')
    gf, calls = G.walk(SYN_GUEST, r'^\s*assets::Material mat;', r'^\s*out\.materials\.push_back\(mat\);', 'syn', 'mat',
                       lambda ln: ['g_mNormalFxPrm'] if 'TexSlot::Normal' in ln else [], True)
    gby = {f['field']: f for f in gf}
    check('A guest: the ternary lvalue', 'logo7' in gby and 'logo4' in gby, str(sorted(gby)))
    check('A guest: a switch case takes its slot names', gby['normal']['names'] == ['g_mNormalFxPrm'], str(gby['normal']))
    check('A guest: flat_diffuseless is a plant', gby['diffuse_color']['plant'] and gby['ambient']['plant'] and not gby['normal']['plant'], '')
    check('A guest: a free call passing mat is named, a method call is not', [c['fn'] for c in calls] == ['Helper'], str(calls))
    _cf, cc = G.walk(SYN_CALLS, r'^\s*assets::Material mat;', r'^\s*out\.materials\.push_back\(mat\);', 'syn', 'mat', G.literal_names, True)
    check('A calls: one behind `if (`, the inner of two nested, none for a field read', [c['fn'] for c in cc] == ['ns::Apply', 'Inner', 'ArenaOnly', 'ns::Table', 'Fix'], str(cc))
    check('A calls: the call line\'s plant condition rides with the call', [c['plant'] for c in cc] == [False, False, False, False, True], str(cc))
    check('A calls: an arena `if (` rides with the call; an `o.arena` ARGUMENT does not (S111 UNIFY2 review)', [c['arena'] for c in cc] == [False, False, True, False, False], str(cc))
    check('A names in a followed body: is("X"), NameStarts(n, "X", ..) = X*',
          G.fn_names('  else if (NameStarts(n, "g_fScroll", fold)) {') == ['g_fScroll*'] and G.fn_names('  : is("g_f4SpeCol1")    ? mat.hair_col1') == ['g_f4SpeCol1'], '')


# ------------------------------------------------------------------ B / C. real data
def patched(ref, edits):
    base = T.default_reader(ref)

    def read(p):
        t = base(p)
        for path, old, new in edits:
            if path == p:
                if t.count(old) != 1:
                    raise SystemExit(f'selftest: the anchor {old!r} is in {p} at {ref} {t.count(old)} times (must be 1)')
                t = t.replace(old, new)
        return t
    return read


def rows_at(ref, edits=(), caw=None):
    rows, ours, _caw, problems = G.compute(patched(ref, edits), ref, caw, memo=MEMO)
    return {r['field']: r for r in rows}, ours, problems


def verdict_is(rows, field, want):
    """None when the field's verdict is `want`, else the message (the check's failure)."""
    r = rows.get(field)
    got = r['verdict'] if r else 'no row'
    return None if got == want else f'{field} is {got}, not {want}'


def part_bc():
    caw = G.load_caw()
    if caw is None:
        print('SKIP B / C / D: no asset index (q asset builds it)')
        return None
    rows, _o, p = rows_at(FROZEN_HAIR, caw=caw)
    check('B frozen hair: no PROBLEM', not p, '; '.join(p))
    for f, cn in (('hair_col1', 'SpeCol1'), ('hair_col2', 'SpeCol2'), ('hair_shift', 'HairSpeParam'), ('hair_exp', 'HairSpeParam')):
        m = verdict_is(rows, f, 'LOOK')
        check(f'B {FROZEN_HAIR} {f} STOCK-ONLY LOOK', m is None, m or '')
        r = rows.get(f) or {}
        check(f'B {FROZEN_HAIR} {f} read by ResolveChar {cn} for yCh_Aniso_trans',
              any(x['by'] == f'ResolveChar {cn}' and 'yCh_Aniso_trans' in x['effects'] for x in r.get('reads', [])), str(r.get('reads')))
        check(f'B {FROZEN_HAIR} {f} guest fills nothing', not r.get('guest'), str(r.get('guest')))
    for f in ('diffuse', 'normal'):
        m = verdict_is(rows, f, 'BOTH')
        check(f'B {FROZEN_HAIR} {f} BOTH', m is None, m or '')
    rc, _o, p = rows_at(FROZEN_CUBE, caw=caw)
    check('B frozen cube: no PROBLEM', not p, '; '.join(p))
    m = verdict_is(rc, 'env_cube', 'LOOK')
    check(f'B {FROZEN_CUBE} env_cube STOCK-ONLY LOOK', m is None, m or '')
    check(f'B {FROZEN_CUBE} env_cube read at slot 10', any(x['by'].startswith('slot 10 ') for x in (rc.get('env_cube') or {}).get('reads', [])),
          str((rc.get('env_cube') or {}).get('reads')))

    GU, CH = G.GUEST, G.CHAR
    r1, _o, _p = rows_at(FROZEN_HAIR, [(GU, 'mat.shader = shader;', 'mat.shader = shader;\n    if (NameEq16(r->effect, "g_f4SpeCol1")) mat.hair_col1[0] = 1.f;')], caw)
    twin('C guest gains a hair_col1 write by name: hair_col1 still LOOK', verdict_is(r1, 'hair_col1', 'LOOK'))
    check('C guest gains a hair_col1 write by name: BOTH', verdict_is(r1, 'hair_col1', 'BOTH') is None, verdict_is(r1, 'hair_col1', 'BOTH') or '')
    r1b, _o, _p = rows_at(FROZEN_HAIR, [(GU, 'mat.shader = shader;', 'mat.shader = shader;\n    mat.hair_col1[0] = 1.f;')], caw)
    twin('C guest gains a hair_col1 write by no name (the CAW name g_f4SpeCol1 not covered): judged BOTH', verdict_is(r1b, 'hair_col1', 'BOTH'))
    check('C guest gains a hair_col1 write by no name: PARTIAL', verdict_is(r1b, 'hair_col1', 'PARTIAL') is None, verdict_is(r1b, 'hair_col1', 'PARTIAL') or '')
    r2, _o, _p = rows_at(FROZEN_HAIR, [(GU, 'case TexSlot::Normal: mat.normal = ti; break;', '')], caw)
    twin('C guest loses the Normal case: normal still BOTH', verdict_is(r2, 'normal', 'BOTH'))
    check('C guest loses the Normal case: normal LOOK', verdict_is(r2, 'normal', 'LOOK') is None, verdict_is(r2, 'normal', 'LOOK') or '')
    r3, _o, _p = rows_at(FROZEN_HAIR, [(GU, 'mat.shader = shader;', 'mat.shader = shader;\n    mat.crowd = true;')], caw)
    twin('C guest gains crowd (the loader never fills it): no GUEST-ONLY row', None if verdict_is(r3, 'crowd', 'GUEST-ONLY') else 'crowd GUEST-ONLY')
    r4, _o, _p = rows_at(FROZEN_HAIR, [(CH, 'if (name == "SpeCol1") {', 'if (name == "SpeCol1_planted") {')], caw)
    twin('C the SpeCol1 resolver line renamed: hair_col1 still LOOK', verdict_is(r4, 'hair_col1', 'LOOK'))
    c5 = copy.deepcopy(caw)
    for eff, vals in c5['names']['g_f4SpeCol1'].items():
        c5['names']['g_f4SpeCol1'][eff] = {json.dumps([0.0, 0.0, 0.0, 0.0]): sum(vals.values())}
    r5, _o, _p = rows_at(FROZEN_HAIR, caw=c5)
    twin('C every CAW g_f4SpeCol1 at the default: hair_col1 still LOOK', verdict_is(r5, 'hair_col1', 'LOOK'))
    check('C every CAW g_f4SpeCol1 at the default: SAME', verdict_is(r5, 'hair_col1', 'SAME') is None, verdict_is(r5, 'hair_col1', 'SAME') or '')
    c6 = copy.deepcopy(caw)
    del c6['names']['g_f4SpeCol1']
    r6, _o, _p = rows_at(FROZEN_HAIR, caw=c6)
    twin('C g_f4SpeCol1 dropped from the CAW records: hair_col1 still LOOK', verdict_is(r6, 'hair_col1', 'LOOK'))
    check('C g_f4SpeCol1 dropped from the CAW records: NOREC', verdict_is(r6, 'hair_col1', 'NOREC') is None, verdict_is(r6, 'hair_col1', 'NOREC') or '')
    r7, _o, _p = rows_at(FROZEN_HAIR, [(GU, 'if (flat_diffuseless) {', 'if (flat_diffuseless) {\n        mat.hair_col1[0] = 1.f;')], caw)
    m = verdict_is(r7, 'hair_col1', 'LOOK')
    check('C a hair_col1 write behind flat_diffuseless is a plant: still LOOK', m is None, m or '')
    # UNIFYMAT step 0: a stock fill MOVED into a helper (defined beside LoadJboy) must not vanish: still LOOK, now read in the helper; its
    # twin, the same helper taking a const Material& (it can no longer write), must lose the row (so the check can fail)
    chain = 'else if (pname == "g_fFairy") mat.fairy = r.f32(po + 20);'
    moved = 'else if (pname == "g_fFairy") PutFairy(mat, pname, r.f32(po + 20));'
    head = 'inline bool LoadJboy(const Bytes& blob, const JboyOptions& o, Model& m, std::string& err) {'
    helper = 'inline void PutFairy({}Material& m, const std::string& pname, float x) {{\n  if (pname == "g_fFairy") m.fairy = x;\n}}\n'
    for const in ('', 'const '):
        rh, oh, _p = rows_at(FROZEN_HAIR, [(G.JBOY, chain, moved), (G.JBOY, head, helper.format(const) + head)], caw)
        r = rh.get('fairy') or {}
        put = [c for c in oh['calls'] if c['side'] == 'stock' and c['fn'] == 'PutFairy']
        msg = None if r.get('verdict') == 'LOOK' and r.get('stock_names') == ['g_fFairy'] and len(put) == 1 and put[0]['path'] == G.JBOY and put[0]['fills'] == 1 \
            else f"fairy is {r.get('verdict', 'no row')}, names {r.get('stock_names')}, PutFairy calls {put}"
        if const:
            twin('C the moved fairy helper takes a const Material& (no write): fairy still LOOK', msg)
        else:
            check('C a stock fill moved into a helper beside LoadJboy does not vanish: fairy LOOK, read in the helper', msg is None, msg or '')
    _r8, o8, _p = rows_at(FROZEN_HAIR, [(GU, 'mat.shader = shader;', 'mat.shader = shader;\n    NoSuchHelper(mat);')], caw)
    c8 = [c for c in o8['calls'] if c['fn'] == 'NoSuchHelper']
    check('C a helper call passing mat with no definition is named as unfollowed', len(c8) == 1 and c8[0]['path'] is None, str(o8['calls']))
    return caw


def part_d(caw):
    if subprocess.run(['git', '-C', G.REPO, 'cat-file', '-e', FROZEN_GUESTHAIR + '^{commit}'], capture_output=True).returncode:
        check(f'D {FROZEN_GUESTHAIR} is in this repository', False, 'the GUESTHAIR landing is gone: freeze D on the commit that holds ApplyRows')
        return
    base, _o, _p = rows_at(FROZEN_HAIR, caw=caw)
    rows, o, p = rows_at(FROZEN_GUESTHAIR, caw=caw)
    check('D frozen GUESTHAIR: no PROBLEM', not p, '; '.join(p))
    floats = sorted(f for f, r in base.items() if r['verdict'] == 'LOOK' and not any(caw['kind'].get(n) == 'sampler' for n in r['stock_names']))
    check(f'D {FROZEN_HAIR} has float LOOK fields to carry', len(floats) >= 10, str(floats))
    for f in floats:
        m = verdict_is(rows, f, 'BOTH')
        check(f'D {FROZEN_GUESTHAIR} {f} (LOOK at {FROZEN_HAIR}) BOTH', m is None, m or '')
    for f in ('hair_col1', 'hair_col2', 'hair_shift', 'hair_exp'):
        r = rows.get(f) or {}
        for side in ('guest', 'stock'):
            check(f'D {f}: the {side} fill is read in {TABLE}', any(x['at'].startswith(TABLE + ':') for x in r.get(side, [])), str(r.get(side)))
    gcalls = [c for c in o['calls'] if c['side'] == 'guest' and c['path']]
    check('D the guest chain ApplyRows -> ApplyFloat is followed', any(c['fn'].endswith('ApplyRows') for c in gcalls) and any(c['fn'].endswith('ApplyFloat') for c in gcalls), str(o['calls']))
    gone = sorted(set(base) - set(rows))
    check(f'D no field {FROZEN_HAIR} has goes missing at {FROZEN_GUESTHAIR}', not gone, str(gone))
    call = 'mkey += guestfloats::ApplyRows(t.guest_models, model, p, mat);'
    t1, _o, _p = rows_at(FROZEN_GUESTHAIR, [(G.GUEST, call, '(void)0;')], caw)
    twin('D the ApplyRows call dropped: hair_col1 still BOTH', verdict_is(t1, 'hair_col1', 'BOTH'))
    t2, _o, _p = rows_at(FROZEN_GUESTHAIR, [(FLOAT_ROWS, 'assets::matfloat::ApplyFloat(mat, ', 'assets::matfloat::ApplyFloat(scratch, ')], caw)
    twin('D ApplyRows no longer hands mat to ApplyFloat: hair_col1 still BOTH', verdict_is(t2, 'hair_col1', 'BOTH'))
    t3, _o, _p = rows_at(FROZEN_GUESTHAIR, [(G.GUEST, 'if (g_draw_plant != 18) ' + call, 'if (g_draw_plant == 18) ' + call)], caw)
    twin('D the ApplyRows call behind a draw plant: hair_col1 still BOTH', verdict_is(t3, 'hair_col1', 'BOTH'))


def part_e(caw):
    bare, why, _stale = NR.split(['g_fIdMapLv3', 'g_f4GlowMskCol2', 'g_fNotARealName'])
    check('E synthetic: a glob row covers its names, a new name stays bare',
          bare == ['g_fNotARealName'] and [p for p, _m, _r, _n in why] == ['g_f4GlowMskCol[1-4]', 'g_fIdMapLv[0-7]'], f'{bare} {why}')
    _rows, ours, _c, p = G.compute(T.default_reader(None), None, caw, memo=MEMO)
    check('E working tree: no PROBLEM', not p, '; '.join(p))
    un = G.unclaimed(ours, caw)
    bare, _why, stale = NR.split(un)
    check('E working tree: every CAW name no path reads has a NOREAD reason', not bare, str(bare))
    check('E working tree: no NOREAD row is stale', not stale, str(stale))
    gap = tuple(r for r in NR.NOREAD if r[0] != 'g_fIdMapLv[0-7]')
    b2, _w, _s = NR.split(un, gap)
    twin('E the IdMapLv row dropped: every name still covered', f'{len(b2)} names bare' if b2 else None)
    _b, _w, s3 = NR.split(un, NR.NOREAD + (('g_fNoSuchName', 'C', 'planted'),))
    twin('E a row for a name no record carries: not reported stale', f'stale {s3}' if s3 else None)


def guarded(name, fn, *args):
    """one part; a crash is a FAIL of that part (the others still run)."""
    try:
        return fn(*args)
    except Exception:   # noqa: BLE001
        check(f'{name} ran', False, traceback.format_exc(limit=3))
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    G.REPO = T.REPO = root
    T.FX_DIR = os.path.join(root, 'src', 'renderer', 'shaders', 'fx')
    guarded('A', part_a)
    caw = guarded('B / C', part_bc)
    if caw is not None:
        guarded('D', part_d, caw)
        guarded('E', part_e, caw)
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

r"""Self-test of `q exprdiff` (S111 EXPRDIFF): `py -I tools/q/qexprdiff_selftest.py [--synthetic-only]`   (0 game runs)

A. Synthetic expression pairs (invented text in the slicer's notation and in C++, no game code), through the production compare
   (qexprdiff.judge after the production binding / substitution):
   A1 a dropped factor (game a * b, ours a) = GAME-ONLY LEAF naming b; A2 a wrong offset (the game reads +384, ours' leaf is bound to
   +388) = DIFF; A3 a swapped operand order (game (a + b) * 2, ours 2 * (b + a)) = OK by the normal form itself; A4 a loop leaf = UNSETTLED;
   A5 an invented clamp phi: ours `d > 0 ? min(d / K, 1) : 1` = OK (by evaluation), ours `(far - near) * 0.001` (the clamp dropped) = DIFF;
   A6 the resolver-branch walker on invented text (set4 lanes, a ternary, a local, an inlined return-lambda, a memcpy of a member, if / else,
   an early return, a loop and a writer-lambda left unsplit, the caller's zero); A7 the material-name table read from invented ApplyFloat
   text (a vec4 chain, a 3-lane member, a family pick, scalars with an element, a NameStarts suffix family).
   Planted twins (each a defect of the tool, WWE_QEXPRDIFF_PLANT, run in a child): no_commute (A3 fails), no_offset (A2 fails),
   no_loop (A4 fails), no_factor (A1 fails), no_eval (A5 fails), no_ours_default (A8a fails: S111 EXPRVAL, `--values` counted on invented records,
     tools/q/qexprvals_selftest.py): each must make part A fail (exit 1).
B. Real data (needs the q index and generated/; else SKIP), the re-finds of this week's fixes, through the production path (collect):
   B1 MATFEED (080aba8ec): yCh_skin PS c156.x (g_fHdLigLvl) and yCh_skin_flex PS c249.x (g_f4MuscleParam) are GAME-ONLY LEAF on the
      parent tree, OK after (the material arm beside the body's live row);
   B2 RIMGAME (d97ed8903): yCh_skin PS c162.z (the rim depth term, sliced at the game's writer) is DIFF on the parent tree (the clamp
      dropped; `q constdiff` does NOT flag it: the old engine's miss) and OK after;
   B3 MATSIZE: the model draw's palette row address (the splat rows) is OK on the working tree; a planted ours text without the -1
      (palette_ptr + 64u * node) is DIFF against the same game slice;
   B4 constdiff's own re-finds through this tool at its FROZEN tree: PS c144 (SpecCubPrm3, game material) not OK, PS c161 (RimLightParam,
      the game's image constants, our equal literal) OK, a planted .y = 0.5 DIFF; yDefault PS c168 PASS-NEVER;
   B5 the fold, the whole working tree: every lane `q constdiff` flags (a whole DIFF row's game-fed lanes, a DIFF-by-lane row's lanes) is
      GAME-ONLY LEAF / DIFF here, and every PASS-NEVER row's lanes PASS-NEVER (0 mismatches).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qexprdiff as X   # noqa: E402

FAILS, CAUGHT = [], []
PLANTS = ('no_commute', 'no_offset', 'no_loop', 'no_factor', 'no_eval', 'no_ours_default')


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def pair(game_text, ours_text, gbind=None, obind=None, consts=None, env=None, lambdas=None):
    """the production path of a declared input: parse both, bind, substitute, judge"""
    g = X.parse_game(game_text)
    g = X.substitute(g, X.bind_table(gbind or {}, X.parse_game))
    g = X.substitute(g, X.bind_table(consts or {}, X.parse_game))
    o = X.parse_cpp(X._clean(ours_text), env, lambdas)
    o = X.substitute(o, X.bind_table(obind or {}, X.parse_cpp))
    o = X.ours_leaves_bound(o, '', {'ych': {}, 'other': {}})
    return X.judge(g, o)


# ------------------------------------------------------------------ A
def part_a():
    v, w = pair('([in_r3+16] * [in_r3+20])', 's', {'[in_r3+16]': 's'}, {'s': 's'})
    check('A1 a dropped factor = GAME-ONLY LEAF naming the factor', v == 'GAME-ONLY LEAF' and '20' in w, f'{v} {w}')
    v, w = pair('[in_r3+20] * [in_r3+16]', 'b * s', {'[in_r3+16]': 's', '[in_r3+20]': 'b'}, {'s': 's', 'b': 'b'})
    check('A1 twin: the factor kept = OK', v == 'OK', f'{v} {w}')
    v, w = pair('[in_r3+384]', 'far', {'[in_r3+388]': 'far'}, {'far': 'far'})
    check('A2 a wrong offset = DIFF', v == 'DIFF', f'{v} {w}')
    v, w = pair('(([in_r3+4] + [in_r3+8]) * 2)', '2.f * (b + a)', {'[in_r3+4]': 'a', '[in_r3+8]': 'b'}, {'a': 'a', 'b': 'b'})
    check('A3 a swapped operand order = OK by the normal form', v == 'OK' and w == '', f'{v} {w!r}')
    v, w = pair('(([in_r3 + 4*?loop + 12] << 6) + in_r5)-64', 'p + 64u * (n - 1u)', {'in_r5': 'p'}, {'p': 'p', 'n': 'n'})
    check('A4 a loop leaf = UNSETTLED', v == 'UNSETTLED' and '?loop' in w, f'{v} {w}')
    v, w = pair('((in_r5 + ([in_r3+12] << 6)) - 64)', 'p + 64u * (n - 1u)', {'in_r5': 'p', '[in_r3+12]': 'n'}, {'p': 'p', 'n': 'n'})
    check('A4 twin: the word bound whole = OK', v == 'OK' and w == '', f'{v} {w!r}')
    clamp = ('phi(([in_r3+8] - [in_r3+4]) <= 0: 1[0x80000010] | (([in_r3+8] - [in_r3+4]) / [in_r3+40]) < 0: 0 | '
             '(([in_r3+8] - [in_r3+4]) / [in_r3+40]) <= 1: (([in_r3 + 8] - [in_r3 + 4]) / [in_r3 + 40]) | '
             '(([in_r3+8] - [in_r3+4]) / [in_r3+40]) > 1: 1[0x80000010])')
    gb, ob, ks = {'[in_r3+8]': 'far', '[in_r3+4]': 'near'}, {'c.r[3]': 'far', 'c.r[2]': 'near'}, {'[in_r3+40]': 1000.0}
    env = {'d': X.parse_cpp('c.r[3] - c.r[2]')}
    v, w = pair(clamp, 'd > 0.f ? std::min(d / 1000.f, 1.f) : 1.f', gb, ob, ks, env)
    check('A5 the clamp as ours writes it = OK (by evaluation)', v == 'OK', f'{v} {w}')
    v, w = pair(clamp, '(c.r[3] - c.r[2]) * 0.001f', gb, ob, ks)
    check('A5 the clamp dropped = DIFF', v == 'DIFF', f'{v} {w}')
    # A6 the branch walker
    lam = X.resolver_lambdas('const auto pw = [&](float v) { return (c.analytic && v == 0.f) ? 1.f : v; };')
    lam['__writers__'] = {'rows'}
    br = X.Branch(lam, X.cond_free)
    lanes = br.run(' { const float d = mt.a[1] - mt.a[0]; set4(0, mt.k, pw(mt.p), d * 2.f, c.on ? mt.q : 0.f); return true; }')
    check('A6 set4 lanes: a member, an inlined lambda (a phi), a local, a ternary',
          X.canon(lanes[(0, 0)]) == 'mt.k' and lanes[(0, 1)][0] == 'phi' and X.canon(lanes[(0, 2)]) == '-2*mt.a[0] + 2*mt.a[1]'
          and lanes[(0, 3)][0] == 'phi', str(lanes))
    lanes = X.Branch(lam, X.cond_free).run(' { if (c.x) { std::memcpy(out, mt.amb, 16); return true; } out[1] = 2.f; return true; }')
    check('A6 an early return: memcpy lanes on one path, out[1] / the caller\'s zero on the other',
          lanes[(0, 2)] == ('phi', ((('leaf', 'c.x'), ('leaf', 'mt.amb[2]')), (None, X.ZERO)))
          and lanes[(0, 1)] == ('phi', ((('leaf', 'c.x'), ('leaf', 'mt.amb[1]')), (None, ('num', 2.0)))), str(lanes))
    lanes = X.Branch(lam, X.cond_free).run(' { for (int i = 0; i < 3; ++i) out[i] = ls.sky[i]; out[3] = 1.f; return true; }')
    check('A6 a loop is unsplit (its lanes one ours leaf), a later out[3] kept',
          lanes.get('*', ('', ''))[1].startswith('ours?:') and lanes[(0, 3)] == ('num', 1.0), str(lanes))
    lanes = X.Branch(lam, X.cond_free).run(' { rows(c.vp, 4); return true; }')
    check('A6 a writer-lambda call is unsplit', '*' in lanes, str(lanes))
    # A7 the material-name table
    syn = r'''
    float* dst = is("g_f4Amb") ? mat.ambient
                 : is(mat.ych_table ? "g_f4SpecCol" : "g_f4SpecularCol") ? mat.spec_col
                 : is("g_f4Ref") ? mat.ref_col : nullptr;
    for (int c = 0; c < ((dst == mat.ref_col) ? 3 : 4); ++c) dst[c] = v[c];
    if (is("g_f4Id")) {   // a comment
      for (int c = 0; c < 4; ++c) mat.idmap_col[c] = v[c];
    }
    if (is("g_fLvl")) mat.lvl = x;
    else if (is("g_fHd")) mat.hd[1] = x, mat.has = true;
    else if (is(mat.ych_table ? "g_fSpecPow" : "g_iSpecularPow")) mat.spec_pow = x;
    else if (n.size() == 13 && NameStarts(n, "g_f4UV_", fold) && n[12] >= 'x') mat.uv[n[12] - 'x'] = x;
    '''
    mb = X.load_mat_bind(lambda p: syn if p.endswith('material_floats.h') else '')
    ok = (mb['ych'].get('g_f4Amb[2]') == 'ambient[2]' and mb['ych'].get('g_f4SpecCol[1]') == 'spec_col[1]'
          and mb['other'].get('g_f4SpecularCol[1]') == 'spec_col[1]' and 'g_f4Ref[3]' not in mb['ych'] and mb['ych'].get('g_f4Ref[2]') == 'ref_col[2]'
          and mb['ych'].get('g_f4Id[3]') == 'idmap_col[3]' and mb['ych'].get('g_fLvl[0]') == 'lvl' and mb['ych'].get('g_fHd[0]') == 'hd[1]'
          and mb['ych'].get('g_fSpecPow[0]') == 'spec_pow' and mb['other'].get('g_iSpecularPow[0]') == 'spec_pow'
          and mb['ych'].get('g_f4UV_y[0]') == 'uv[1]')
    check('A7 the material-name table (vec4 chain, 3-lane member, family pick, scalars, a suffix family)', ok, str(mb))
    import qexprvals_selftest   # A8: --values on invented records (S111 EXPRVAL)
    qexprvals_selftest.run(X, check)


# ------------------------------------------------------------------ B
def find(rows, inp):
    return next((r for r in rows if r['input'] == inp), None)


def part_b():
    try:
        import q as Q
        con = Q.connect(Q.DB)
        game = X.GameSlices(con)
        game.text('sub_82C04DF0+525C', 'r10')
    except Exception as e:  # noqa: BLE001 - no q index / generated sources: the real part is skipped, said
        print(f'SKIP B (no q index / generated sources: {type(e).__name__}: {e})')
        return
    runs = {}

    def at(ref):
        if ref not in runs:
            runs[ref] = X.collect(con, ref, game=game)
        return runs[ref]
    pre, post = at('080aba8ec^')[0], at('080aba8ec')[0]
    for inp in ('yCh_skin:ps.c156.x', 'yCh_skin_flex:ps.c249.x'):
        a, b = find(pre, inp), find(post, inp)
        check(f'B1 MATFEED {inp}: GAME-ONLY LEAF before the fix', a is not None and a['verdict'] == 'GAME-ONLY LEAF', str(a and (a['verdict'], a['why'])))
        check(f'B1 MATFEED {inp}: OK after', b is not None and b['verdict'] == 'OK', str(b and (b['verdict'], b['why'])))
    rows_pre, _p, k_pre = at('d97ed8903^')
    rows_post = at('d97ed8903')[0]
    a, b = find(rows_pre, 'yCh_skin:ps.c162.z'), find(rows_post, 'yCh_skin:ps.c162.z')
    check('B2 RIMGAME c162.z: DIFF before the fix (sliced at the game\'s writer)', a is not None and a['verdict'] == 'DIFF', str(a and (a['verdict'], a['why'])))
    check('B2 RIMGAME c162.z: OK after', b is not None and b['verdict'] == 'OK', str(b and (b['verdict'], b['why'])))
    cd = next((r for r in k_pre.rows if r['effect'] == 'yCh_skin' and r['stage'] == 'ps' and r['reg'] == 162), None)
    check('B2 the old engine (q constdiff) does not flag c162 before the fix', cd is not None and not cd['diff'], str(cd and cd['diff']))
    rows, problems, k = at(None)
    r = next((x for x in rows if x['game_kind'] == 'GAME-SLICE' and 'MATSIZE' in x['input']), None)
    check('B3 MATSIZE splat row address: OK', r is not None and r['verdict'] == 'OK', str(r and (r['verdict'], r['why'])))
    d = X.DRAW_INPUTS[0]
    g = game.tree(d['site'], d['operand'])
    tbl = X.bind_table(d['bind'], X.parse_game)
    for nm, (site, opnd) in d['bind_slices'].items():
        tbl[X.canon(game.tree(site, opnd))] = ('leaf', 'bind:' + nm)
    g = X.substitute(g, tbl)
    o = X.ours_leaves_bound(X.substitute(X.parse_cpp('palette_ptr + 64u * node'), X.bind_table(d['ours'], X.parse_cpp)), '', {'ych': {}, 'other': {}})
    v, w = X.judge(g, o)
    check('B3 twin: the -1 dropped = DIFF', v == 'DIFF', f'{v} {w}')
    fz = at('cabe12ee1')[0]
    a = [x for x in fz if x['input'].startswith('yCh_skin:ps.c144.')]
    check('B4 constdiff re-find c144 (FROZEN): not OK', bool(a) and all(x['verdict'] in ('GAME-ONLY LEAF', 'DIFF') for x in a), str([(x['input'], x['verdict']) for x in a]))
    a = [x for x in rows if x['input'].startswith('yCh_skin:ps.c161.')]
    check('B4 c161 GAME-CONST, our equal literal: OK', len(a) == 4 and all(x['verdict'] == 'OK' for x in a), str([(x['input'], x['verdict']) for x in a]))
    g = ('num', 0.83)
    v, w = X.judge(g, ('num', 0.5))
    check('B4 twin: c161.y planted 0.5 = DIFF', v == 'DIFF', f'{v} {w}')
    a = [x for x in rows if x['input'].startswith('yDefault:ps.c168.')]
    check('B4 yDefault PS c168 (ShadowPrm0): PASS-NEVER', bool(a) and all(x['verdict'] == 'PASS-NEVER' for x in a), str([(x['input'], x['verdict']) for x in a]))
    # B5 the fold: constdiff's flagged lanes are flagged here
    by = {}
    for x in rows:
        by.setdefault((x['effect'], x['stage'], x['reg'], x['lane']), []).append(x['verdict'])
    bad, seen = [], 0
    for cr in k.rows:
        lanes = []
        if cr['pass_never']:
            want = ('PASS-NEVER',)
            lanes = [(vv, ln) for gr in cr['game_rows'] if gr['kind'] in ('GAME-MATERIAL', 'GAME-CODE', 'GAME-CONST') for vv, ln in gr['lanes']]
        elif cr['diff']:
            want = ('GAME-ONLY LEAF', 'DIFF')
            for gr in cr['game_rows']:
                if gr['kind'] not in X.CD.GAME_FED + ('GAME-CONST',):
                    continue
                for vv, ln in gr['lanes']:
                    if gr['comp'] == 0 and ln >= X.name_width(gr['name']):
                        continue
                    if cr['diff_lanes'] and X.CD.lane_kind([b for b in (k.resolvers[cr['resolver']].get(cr['field']) or [])
                                                            if not b['plant'] and b['kind'] != 'FALSE'], (vv, ln)) != 'LITERAL':
                        continue
                    lanes.append((vv, ln))
        else:
            continue
        for vv, ln in lanes:
            got = by.get((cr['effect'], cr['stage'], cr['reg'] + vv, 'xyzw'[ln]), [])
            seen += 1
            if not got or not any(g_ in want for g_ in got):
                bad.append(f"{cr['effect']}:{cr['stage']}.c{cr['reg'] + vv}.{'xyzw'[ln]} constdiff {'PASS-NEVER' if cr['pass_never'] else 'DIFF'} -> {got}")
    check(f'B5 the fold: every lane constdiff flags is flagged here ({seen} lanes)', seen > 0 and not bad, '; '.join(bad[:6]) + (f' (+{len(bad) - 6})' if len(bad) > 6 else ''))
    check('B no PROBLEM on the working tree', not problems, '; '.join(problems[:3]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--synthetic-only', action='store_true')
    a = ap.parse_args()
    part_a()
    if not X.PLANT and not a.synthetic_only:
        for p in PLANTS:
            r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--synthetic-only'], capture_output=True, text=True,
                               env=dict(os.environ, WWE_QEXPRDIFF_PLANT=p), timeout=300)
            fails = [ln for ln in r.stdout.splitlines() if ln.startswith('FAIL')]
            if r.returncode == 1 and fails:
                print(f'ok   planted twin caught: {p} ({fails[0][5:90]})')
            else:
                CAUGHT.append(p)
                print(f'NOT CAUGHT planted twin: {p} (exit {r.returncode})')
        part_b()
    print(f'qexprdiff selftest: {len(FAILS)} failed, {len(CAUGHT)} planted twin(s) not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

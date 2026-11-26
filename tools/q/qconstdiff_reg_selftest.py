r"""Self-test of `q constdiff --byreg` (S111 BYREG): `py -I tools/q/qconstdiff_reg_selftest.py [--root DIR]`   (0 game runs)

A. live maps, synthetic text (invented): a literal array, an array filled by a memcpy after its `= {}`, a zero array, a context
   field, a register no range starts at, a register no put reaches (the block's zero), a member bound in another file.
B. post, synthetic text: a Regs bank with a mixed literal / context local, a for-loop register, a need() whose name is built by a
   function of the file (the candidates are the composable variants only), an unset register (the bank's zero).
C. particles, synthetic text: a block written by a loop, an alias, a memcpy, a never-written range; a block whose size does not
   match its variant must be a PROBLEM.
D. the join, synthetic game rows: code writer vs a literal (DIFF) / a context value (OK); an image constant equal (CONST) / unequal
   (DIFF); a scalar-store write (its unstored lanes not compared); no writer (UNSETTLED); a pass the model draw never picks (PASS-NEVER);
   a writer of another layout (its start register is no range start of the effect) is not joined; a lane ours computes against an
   image constant carries a NOTE.
E. real data (needs the q index and the generated listing; else SKIP), our sources AT THE TIP (the working tree under --root): one
   known case per family (live maps yRimLightMap c162 OK from the record; yShadowMap c64 PARTIAL by the props' identity; post
   yRetroFilm c4 OK from sub_82C28A70; the combine's c33 found through CombineVariantName, its c20 joined to its own writer
   sub_82C1C1A0 and not to the bloom chain's c16+16; particles ySoftParticle c211 CONST against sub_82254E30) and a planted twin of
   each that must turn into a DIFF. Each plant is a regex anchored on the statement it removes and must match exactly once: when
   the code moves the check FAILS by name (re-anchor it), it never reads an old commit.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconstdiff as Q      # noqa: E402
import qconstdiff_reg as R  # noqa: E402

FAILS, CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, msg):
    """msg = what the checker reported on the planted case; None = it accepted it (a defect)."""
    if msg is None:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name}')
    else:
        print(f'ok   planted twin caught: {name} ({msg})')


def lanes(sites, variant, reg):
    """{lane: [kinds]} the sites write into (variant, reg)."""
    out = {}
    for s in sites:
        if s['variant'] == variant:
            for (r, i), kv in s['writes'].items():
                if r == reg:
                    out.setdefault(i, []).append(kv[0] if kv[0] != 'LITERAL' else ('LITERAL', kv[1]))
    return out


# ------------------------------------------------------------------ A. live maps
LM_INIT = r'''
void Hook::Init() { s.a_vs = md::FindVariant("yFx_a_vertex"); }
void Hook::Init2() { fs_b_ = md::FindVariant("yFx_b_fragment"); }
'''
LM = r'''
void Hook::Draw() {
  const rdr::fx::Variant& vvar = rdr::fx::kVariants[s.a_vs];
  const rdr::fx::Variant& fvar = rdr::fx::kVariants[fs_b_];
  static const float kLit[4] = {2.f, -2.f, -1.f, 1.f};
  float filled[4] = {};
  std::memcpy(filled, in.rows, sizeof filled);
  float zero4[4] = {};
  std::vector<uint8_t> block(vvar.block_bytes, 0);
  md::PutRegisters(block, vvar, 10, kLit, 1);
  md::PutRegisters(block, vvar, 11, filled, 1);   // "a comment, kLit"
  md::PutRegisters(block, vvar, 12, zero4, 1);
  md::PutRegisters(block, fvar, 3, p.col, 1);
  md::PutRegisters(block, fvar, 99, p.col, 1);
}
'''
VARS = {'yFx_a_vertex': {'effect': 'yFx', 'stage': 'vs', 'consts': [('Lit', 10, 1), ('Filled', 11, 1), ('Zero', 12, 1), ('Unput', 13, 1)]},
        'yFx_b_fragment': {'effect': 'yFx', 'stage': 'ps', 'consts': [('Col', 3, 1)]}}
# review (S111 BYREG): a data name is looked up in the function that makes the put, never in an earlier function of the file
LM_SCOPE = r'''
void Hook::A() {
  const rdr::fx::Variant& vvar = rdr::fx::kVariants[s.a_vs];
  const float rows[4] = {0.f, 0.f, 0.f, 0.f};
  md::PutRegisters(block, vvar, 10, rows, 1);
}
void Hook::B(const float* rows) {
  const rdr::fx::Variant& vvar = rdr::fx::kVariants[s.a_vs];
  md::PutRegisters(block, vvar, 11, rows, 1);
}
'''


def part_a():
    files = {'lm_init.cpp': LM_INIT, 'lm.cpp': LM, R.POST[0]: POST_TEXT, R.PARTICLES[0]: PT}
    gf = {}
    for p in ('lm_init.cpp', 'lm.cpp'):
        for k, v in R.find_names(Q._mask(files[p])).items():
            gf.setdefault(k, []).extend(v)
    sites, probs = R.parse_live_maps('lm.cpp', LM, VARS, gf)
    check('A parses with no problem', probs == [], str(probs))
    check('A literal array lane by lane', lanes(sites, 'yFx_a_vertex', 10) == {0: [('LITERAL', 2.0)], 1: [('LITERAL', -2.0)], 2: [('LITERAL', -1.0)], 3: [('LITERAL', 1.0)]},
          str(lanes(sites, 'yFx_a_vertex', 10)))
    check('A an array a memcpy fills after `= {}` is OURS', lanes(sites, 'yFx_a_vertex', 11) == {i: ['OURS'] for i in range(4)}, str(lanes(sites, 'yFx_a_vertex', 11)))
    check('A a zero array is LITERAL 0', lanes(sites, 'yFx_a_vertex', 12) == {i: [('LITERAL', 0.0)] for i in range(4)}, str(lanes(sites, 'yFx_a_vertex', 12)))
    check('A a member bound in another file (fs_b_) reaches its variant', lanes(sites, 'yFx_b_fragment', 3) == {i: ['OURS'] for i in range(4)}, str(lanes(sites, 'yFx_b_fragment', 3)))
    check('A a register no range starts at writes nothing', any(s['variant'] is None and 'c99' in s.get('note', '') for s in sites), str([s for s in sites if not s['variant']]))
    allsites, _p2 = R.load_ours(lambda p: files[p], {**VARS, **PVARS}, ['lm_init.cpp', 'lm.cpp'])   # (the particle text's blocks are not these variants: its problems are not this check's)
    check('A an unput register of a written variant is the zero fill', lanes(allsites, 'yFx_a_vertex', 13) == {i: [('LITERAL', 0.0)] for i in range(4)},
          str(lanes(allsites, 'yFx_a_vertex', 13)))
    planted, _p = R.parse_live_maps('lm.cpp', LM.replace('  std::memcpy(filled, in.rows, sizeof filled);\n', ''), VARS, gf)
    twin('A the filled array without its memcpy still read OURS', 'now LITERAL' if lanes(planted, 'yFx_a_vertex', 11) == {i: [('LITERAL', 0.0)] for i in range(4)} else None)
    unbound, _p = R.parse_live_maps('lm.cpp', LM, VARS, {})
    twin('A the cross-file member found without the other file', 'unbound: writes nothing' if not lanes(unbound, 'yFx_b_fragment', 3) else None)
    scoped, p3 = R.parse_live_maps('lm_scope.cpp', LM_SCOPE, VARS, gf)
    check('A the scope case parses with no problem', p3 == [], str(p3))
    check('A a literal array of the same function is LITERAL', lanes(scoped, 'yFx_a_vertex', 10) == {i: [('LITERAL', 0.0)] for i in range(4)}, str(lanes(scoped, 'yFx_a_vertex', 10)))
    twin('A a pointer parameter named like an earlier function\'s literal array read as that literal',
         'OURS' if lanes(scoped, 'yFx_a_vertex', 11) == {i: ['OURS'] for i in range(4)} else None)


# ------------------------------------------------------------------ B. post
POST_TEXT = r'''
bool PostPass::Record(int x) {
  const int vs_q = Find("yFx_q_vertex");
  int ps = -1;
  if (!need("yPo_a_fragment", ps)) return false;
  Regs r;
  const float c0[4] = {p.rate, 0.f, 0.f, 0.f};
  r.Set(0, c0), r.Set(1, p.col);
  pass(t, w, h, vs_q, ps, r, {{0, src, false}});
  Regs q;
  for (int i = 0; i < 3; ++i) { const float v[4] = {float(i) * p.k, 0.f, 0.f, 0.f}; q.Set(i, v); }
  const std::string name = Name(p);
  if (!need(name.c_str(), ps)) return false;
  pass(t, w, h, vs_q, ps, q, {{0, src, false}});
  return true;
}
std::string PostPass::Name(const P& p) { std::string n = "yPo_"; if (p.x) n += "big_"; return n + "fragment"; }
'''
PVARS = {'yPo_a_fragment': {'effect': 'yPo', 'stage': 'ps', 'consts': [('Rate', 0, 1), ('Col', 1, 1), ('Unset', 2, 1)]},
         'yPo_big_fragment': {'effect': 'yPo', 'stage': 'ps', 'consts': [('Tap', 0, 3)]},
         'yPo_other_fragment': {'effect': 'yPo', 'stage': 'ps', 'consts': [('Tap', 0, 3)]}}


def part_b():
    sites, probs = R.parse_post(R.POST[0], POST_TEXT, PVARS)
    check('B parses with no problem', probs == [], str(probs))
    c0 = lanes(sites, 'yPo_a_fragment', 0)
    check('B a mixed local: x OURS, y..w LITERAL 0', c0 == {0: ['OURS'], 1: [('LITERAL', 0.0)], 2: [('LITERAL', 0.0)], 3: [('LITERAL', 0.0)]}, str(c0))
    check('B a context field is OURS', lanes(sites, 'yPo_a_fragment', 1) == {i: ['OURS'] for i in range(4)})
    check('B an unset register is the bank zero', lanes(sites, 'yPo_a_fragment', 2) == {i: [('LITERAL', 0.0)] for i in range(4)}, str(lanes(sites, 'yPo_a_fragment', 2)))
    check('B the loop register range 0..2 reaches the composed variant', all(lanes(sites, 'yPo_big_fragment', r).get(0) == ['OURS'] for r in (0, 1, 2)),
          str([lanes(sites, 'yPo_big_fragment', r) for r in (0, 1, 2)]))
    twin('B a variant whose name is not composable from the literals drawn', 'not drawn' if not lanes(sites, 'yPo_other_fragment', 0) else None)
    bad, p2 = R.parse_post(R.POST[0], POST_TEXT.replace('q.Set(i, v)', 'q.Set(j, v)'), PVARS)
    twin('B a register that is no literal / constexpr / loop variable accepted', p2[0] if p2 else None)


# ------------------------------------------------------------------ C. particles
PT = r'''
constexpr uint32_t kBlockBytes = 48;
constexpr uint32_t kOffA = 0, kOffB = 16;
void LiveParticles::Record(int s) {
  float block[kBlockBytes / 4];
  std::memset(block, 0, sizeof block);
  for (int k = 0; k < 4; ++k) block[kOffA / 4 + k] = ctx.vp[k];
  float* b = block + kOffB / 4;
  b[0] = 1.f;
  std::memcpy(block + kOffB / 4 + 1, src.v, 8);
  fb.PushConstants(block, kBlockBytes);
}
'''
TVARS = {'yPt_base_vertex': {'effect': 'yPt', 'stage': 'vs', 'consts': [('A', 5, 1), ('B', 7, 1), ('C', 9, 1)]}}


def part_c():
    sites, probs = R.parse_particles(R.PARTICLES[0], PT, TVARS, {'block': 'yPt_base_vertex'})
    check('C parses with no problem', probs == [], str(probs))
    check('C a loop write: c5 all OURS', lanes(sites, 'yPt_base_vertex', 5) == {i: ['OURS'] for i in range(4)}, str(lanes(sites, 'yPt_base_vertex', 5)))
    check('C an alias write, a memcpy, an unwritten lane: c7 = (1, OURS, OURS, 0)',
          lanes(sites, 'yPt_base_vertex', 7) == {0: [('LITERAL', 1.0)], 1: ['OURS'], 2: ['OURS'], 3: [('LITERAL', 0.0)]}, str(lanes(sites, 'yPt_base_vertex', 7)))
    check('C a never-written range is the zero', lanes(sites, 'yPt_base_vertex', 9) == {i: [('LITERAL', 0.0)] for i in range(4)})
    _s, p2 = R.parse_particles(R.PARTICLES[0], PT.replace('kBlockBytes = 48', 'kBlockBytes = 64'), TVARS, {'block': 'yPt_base_vertex'})
    twin('C a block whose size is not its variant accepted', p2[0] if p2 else None)
    # review (S111 BYREG): a memcpy into `&block[i]` is the same write as into `block + i`; a destination naming a block that the
    # parser cannot read is a PROBLEM, never skipped (a skipped write left the floats at the array's zero: a false LITERAL)
    amp, p3 = R.parse_particles(R.PARTICLES[0], PT.replace('std::memcpy(block + kOffB / 4 + 1, src.v, 8);', 'std::memcpy(&block[kOffB / 4 + 1], src.v, 8);'),
                                TVARS, {'block': 'yPt_base_vertex'})
    twin('C a memcpy into &block[i] skipped (its floats read as the zero)',
         'OURS' if not p3 and lanes(amp, 'yPt_base_vertex', 7) == {0: [('LITERAL', 1.0)], 1: ['OURS'], 2: ['OURS'], 3: [('LITERAL', 0.0)]} else None)
    _s, p4 = R.parse_particles(R.PARTICLES[0], PT.replace('std::memcpy(block + kOffB / 4 + 1, src.v, 8);', 'std::memcpy(reinterpret_cast<char*>(block) + 20, src.v, 8);'),
                               TVARS, {'block': 'yPt_base_vertex'})
    twin('C a memcpy destination naming a block the parser cannot read skipped silently', p4[0] if p4 else None)
    _s, p5 = R.parse_particles(R.PARTICLES[0], PT.replace('float* b = block + kOffB / 4;', 'float* b = &block[kOffB / 4];'), TVARS, {'block': 'yPt_base_vertex'})
    check('C an alias `float* b = &block[i]` is read as `block + i`', not p5 and lanes(_s, 'yPt_base_vertex', 7).get(0) == [('LITERAL', 1.0)], str((p5, lanes(_s, 'yPt_base_vertex', 7))))


# ------------------------------------------------------------------ D. the join
class Img:
    def word(self, a):
        return {0x82000010: 0x3F800000}.get(a)


def site(variant, reg, kinds, line=1):
    return {'family': 'test', 'path': 'x.cpp', 'line': line, 'variant': variant,
            'writes': {(reg, i): (k[0], k[1], line) for i, k in enumerate(kinds)}}


def hit(reg, data, r5='sp+80', what='float range', count=1):
    return {'func': 0x1000, 'func_name': 'f', 'site': 0x1010, 'stage': 'ps', 'what': what, 'reg': {'kind': 'literal', 'value': reg},
            'count': {'kind': 'literal', 'value': count}, 'data': data, 'r5': r5}


def part_d():
    var = {'yJ_a_fragment': {'effect': 'yJ', 'stage': 'ps', 'consts': [('P', 3, 1), ('Q', 4, 1), ('S', 5, 1), ('T', 6, 1)]}}
    fields = {('yJ', 'ps', r): {'field': f, 'count': 1, 'variants': {'yJ_a_fragment'}} for f, r in (('P', 3), ('Q', 4), ('S', 5), ('T', 6))}
    paths = {'yJ': {'roots': [0x1000], 'vtables': [], 'funcs': {0x1000: 'loads "yJ"'}, 'model': False}}
    hits = [hit(3, 'sp+80: no store seen before the call'), hit(4, '([0x82000010], [0x82000010], [0x82000010], [0x82000010])'),
            hit(6, 'sp+84[f32] = [0x82000010]', r5='sp+84'),
            hit(2, 'sp+96: no store seen before the call', r5='sp+96', count=4)]   # another layout's range c2..c5 (no yJ range starts at c2)
    L, O = ('LITERAL', 0.0), ('OURS', None)
    one = ('LITERAL', 1.0)

    def flag(sites):
        rows = R.build_rows(sites, var, fields, paths, hits, Img())
        return {r['reg']: r['flag'] for r in rows}
    f = flag([site('yJ_a_fragment', 3, [L] * 4), site('yJ_a_fragment', 4, [one] * 4), site('yJ_a_fragment', 5, [L] * 4), site('yJ_a_fragment', 6, [one, L, L, L])])
    check('D code writer vs literal = DIFF', f.get(3) == 'DIFF', str(f))
    check('D image constant equal = CONST', f.get(4) == 'CONST', str(f))
    twin('D a writer of another layout (r4 = 2 count 4) joined to c5', 'UNSETTLED' if f.get(5) == 'UNSETTLED' else None)
    check('D scalar store x = 1 equal, y..w unseen = CONST', f.get(6) == 'CONST', str(f))
    g = flag([site('yJ_a_fragment', 3, [O] * 4), site('yJ_a_fragment', 4, [('LITERAL', 2.0)] * 4), site('yJ_a_fragment', 6, [L, L, L, L])])
    twin('D code writer vs a context value flagged', 'OK' if g.get(3) == 'OK' else None)
    twin('D an unequal literal against an image constant accepted', 'DIFF' if g.get(4) == 'DIFF' else None)
    twin('D a scalar-store lane x = 0 against the constant 1 accepted', 'DIFF' if g.get(6) == 'DIFF' else None)
    nr = {r['reg']: r for r in R.build_rows([site('yJ_a_fragment', 4, [O] * 4)], var, fields, paths, hits, Img())}
    twin('D a lane ours computes against an image constant shown with no NOTE', nr[4]['computed_vs_const'][0] if nr.get(4) and nr[4].get('computed_vs_const') else None)
    pm = {'yJ': {**paths['yJ'], 'model': True}}
    rows = R.build_rows([site('yJ_a_fragment', 3, [L] * 4)], var, fields, pm, hits, Img(), game=({'rows': [], 'groups': [], 'alias': {}}, {}),
                        passes={'yJ': {'yJ_a_fragment': {4}}}, flags={w: set() for w, _a, _y in Q.PASS_TERMS})
    check('D a register read only by a pass the model draw never picks = PASS-NEVER', rows and rows[0]['flag'] == 'PASS-NEVER', str(rows and rows[0]['flag']))


# ------------------------------------------------------------------ E. real data
class AnchorGone(Exception):
    pass


def tip(root, edits=None):
    """our sources as the working tree under root holds them; edits = [(path, regex, replacement)], each regex matching once."""
    cache = {}

    def read(p):
        if p not in cache:
            with open(os.path.join(root, p), encoding='utf-8') as fh:
                cache[p] = fh.read()
        s = cache[p]
        for (ep, rx, new) in (edits or []):
            if ep == p:
                s, n = re.subn(rx, new, s)
                if n != 1:
                    raise AnchorGone(f'{p}: the plant anchor {rx!r} matches {n} times (want 1): re-anchor the twin on the statement it removes')
        return s
    return read


def line_of_rx(root, p, rx):
    """the 1-based line of the one match of rx in the tip's p, or None."""
    with open(os.path.join(root, p), encoding='utf-8') as fh:
        s = fh.read()
    ms = list(re.finditer(rx, s))
    return s.count('\n', 0, ms[0].start()) + 1 if len(ms) == 1 else None


def part_e(root):
    try:
        import q as qq
        con = qq.connect(qq.DB)
        Q.load_game(con)
    except Exception as e:  # noqa: BLE001
        print(f'SKIP E (no q index or generated listing: {e})')
        return
    live = sorted(os.path.relpath(p, root).replace(os.sep, '/') for p in glob.glob(os.path.join(root, R.LIVE_MAPS_GLOB)))
    fx = os.path.join(root, 'src', 'renderer', 'shaders', 'fx')
    variants, fields = R.load_variants(fx), Q.load_fields(fx)
    sites, probs = R.load_ours(tip(root), variants, live)
    check('E the tip\'s sources parse with no problem', probs == [], str(probs))
    effects = sorted({variants[s['variant']]['effect'] for s in sites if s['variant']})
    g = R.load_game_side(con, effects)
    check('E the game side loads with no problem', g[-1] == [], str(g[-1]))
    paths, hits, img, game, mats, totals, consts, passes, flags, _p = g

    def rows_of(edits=None):
        s, pr = R.load_ours(tip(root, edits), variants, live)
        return {(r['effect'], r['stage'], r['reg']): r for r in R.build_rows(s, variants, fields, paths, hits, img, game, mats, totals, consts, passes, flags)}

    def planted(name, edits, key):
        try:
            return rows_of(edits)[key]['flag']
        except AnchorGone as e:
            check(f'E plant anchor for "{name}"', False, str(e))
            return 'ANCHOR GONE'
    rows = rows_of()
    check('E three families', {f for r in rows.values() for f in r['families']} == {'live_maps', 'post', 'particles'}, str({f for r in rows.values() for f in r['families']}))
    rd = rows.get(('yRimLightMap', 'vs', 162))
    check('E live maps: yRimLightMap vs c162 RimLightDepth is OURS (the record, live_map_rim.cpp:170) against descriptor #45: OK',
          rd and rd['flag'] == 'OK' and rd['ours'] == 'OURS', str(rd and (rd['flag'], rd['ours_at'])))
    tz = rows.get(('yShadowMap', 'vs', 64))
    props = 'src/renderer_scene/live_map_props.cpp'
    il = line_of_rx(root, props, r'md::PutRegisters\([^;]*,\s*64\s*,\s*identity\s*,')
    check('E live maps: yShadowMap vs c64 Trapezoidal PARTIAL, the props identity arm (the one `PutRegisters(..., 64, identity, ...)`)',
          il is not None and tz and tz['flag'] == 'PARTIAL' and any(f'live_map_props.cpp:{il}' in p for p in tz['partial']), str((il, tz and tz['partial'])))
    rf = rows.get(('yRetroFilm', 'ps', 4))
    check('E post: yRetroFilm ps c4 g_Range OK, written by sub_82C28A70 (slot 2 of vtable 0x8206D744)',
          rf and rf['flag'] == 'OK' and any(gr['func'] == '0x82C28A70' for gr in rf['game_rows']), str(rf and rf['flag']))
    mg = rows.get(('yGlareEtc', 'ps', 33))
    check('E post: the combine (a need() through CombineVariantName) reaches yGlareEtc ps c33 MiddleGray, written by sub_82C1C768',
          mg and any(gr['func'] == '0x82C1C768' for gr in mg['game_rows']), str(mg and mg['flag']))
    dp = rows.get(('yGlareEtc', 'ps', 20))
    check('E post: the combine\'s ps c20 DOFParam joined to its own writer sub_82C1C1A0 (depth 3: driver -> T_Final -> sub_82C1C768 -> it)',
          dp and any(gr['func'] == '0x82C1C1A0' for gr in dp['game_rows']), str(dp and [gr['why'][:60] for gr in dp['game_rows']]))
    twin('E the bloom chain\'s c16+16 writes (another layout) joined to the combine\'s c20',
         'left out' if dp and not any('r4 = 16 count 16' in gr['why'] for gr in dp['game_rows']) else None)
    fo = rows.get(('ySoftParticle', 'ps', 211))
    check('E particles: ySoftParticle ps c211 FadeOutDistance CONST (y = 0) against sub_82254E30 (slot 1 of the node table 0x82000B84)',
          fo and fo['flag'] == 'CONST' and any(gr['func'] == '0x82254E30' for gr in fo['game_rows']), str(fo and (fo['flag'], fo['const_eq'])))
    rim = 'src/renderer_scene/live_map_rim.cpp'
    p1 = planted('rim', [(rim, r'std::memcpy\(\s*rim_depth\s*,[^;]*;', '(void)0;')], ('yRimLightMap', 'vs', 162))
    twin('E c162 with the record memcpy removed (a zero literal) not DIFF', 'DIFF' if p1 == 'DIFF' else None)
    post = 'src/renderer_scene/post_pass.cpp'
    p2 = planted('retro', [(post, r'\br\.Set\(\s*4\s*,\s*p\.retro_range\s*\)', '(void)0')], ('yRetroFilm', 'ps', 4))
    twin('E yRetroFilm c4 left unset (the bank zero) not DIFF', 'DIFF' if p2 == 'DIFF' else None)
    part = 'src/renderer_scene/live_particles.cpp'
    p3 = planted('soft', [(part, r'\bfsb\[\s*0\s*\]\s*=[^;]*;', 'fsb[0] = 0.25f;')], ('ySoftParticle', 'ps', 211))
    twin('E ySoftParticle c211.x a literal not DIFF', 'DIFF' if p3 == 'DIFF' else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    part_a()
    part_b()
    part_c()
    part_d()
    part_e(root)
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

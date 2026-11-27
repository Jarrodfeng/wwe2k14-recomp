r"""Self-test of `q constdiff` (S111 CONSTDIFF): `py -I tools/q/qconstdiff_selftest.py [--root DIR]`   (0 game runs)

A. Synthetic resolver text (invented, no game code): a literal branch, a material branch, a context branch, a zero list, a
   dev-plant branch (getenv switch), a *Plant() branch, a nested branch, an else branch, and an unmapped root: each kind,
   line and plant mark must come out as written, and the unmapped root must be a PROBLEM.
B. Synthetic listing (invented instructions): a common table, a 3-character prefix sub-table, a full-name sub-table and a
   prefix whose rebase branches into another's fill code: rows (name, register, count, component, type), groups and the
   alias must be read back.
C. Real data (needs generated/ and the q index; else SKIP): the name table (209 rows, 7 groups; g_f4SpecCol = register 144 at
   row 0x8392A6A0, g_f4RimLightParam = 161 at 0x8392B600), the descriptor list (47 names, g_f4RimLightParam among them), and
   the two cases found by hand in S111 RE-FOUND as DIFF rows from the resolver text frozen at commit FROZEN:
   PS c144 at src/renderer_scene/char_effects.cpp and PS c161 at src/renderer_scene/char_effects.cpp (rdr_scene::ResolveChar).
D. Planted twins on the same frozen text: the c144 line fed from the material (the DIFF must go: the "c144 is DIFF at 648"
   check must FAIL), and the MatDifCol line replaced by a literal (a NEW DIFF must appear: the "MatDifCol is not DIFF" check
   must FAIL); an unmapped root in the resolver must make the run a PROBLEM.
Review (S111): A also reads the lanes (set4 arguments, out[N] writes, a local, a memcpy / a lambda left unsplit, a root passed whole
to a helper, a literal else arm); B a PS-bank row feeds only the PS register, with its lane; C VS c168 of yCh_skin takes only the
VS-bank row, Param1 is not DIFF; D a planted twin: Param1 .z made a literal beside the material .x must be a DIFF by lane.
RIMGAME review (S111): E an invented copier + constructor (no game code): the GAME-CONST walk reads the four lanes through the
copier's stack spill and 64-bit stores, the constructor's image-constant stores and the node vtable that holds the copier; twins: a
lane whose constructor store is missing, and a node vtable without the copier, must be problems, never a value. C c161 is now
GAME-CONST (1, 0.83, 1, 0) read from sub_827E9D90 / sub_827EA7F0, and our equal literal at :610 is NOT a DIFF; D a planted twin:
its .y made 0.5 (unequal to the game's 0.83) must be a DIFF by lane.
CONSTPASS (S111): F real data (needs the q index and the player's effect files; else SKIP): the pass flags read from sub_82BDA038 (the +4
shadow word: 25 names, no ydefault); yDefault PS c168-c170 (ShadowPrm0..2) RE-FOUND as PASS-NEVER, not DIFF (the SHADOWNAME false alarm);
yDefault VS c190 not PASS-NEVER; yBG_Default PS c168 ShadowPrm as it was without the class. Twins: ych_skin's string renamed 'ydefault'
(a name planted into the list) must make c168 drawn; ybg_default's renamed out of it must make yBG_Default's c168 PASS-NEVER.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import struct
import argparse
import inspect
import json
import os
import tempfile
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconstdiff as Q   # noqa: E402

FROZEN = 'cabe12ee1'      # the S111 session tip the two cases were found against (char_effects.cpp)
CHAR = 'src/renderer_scene/char_effects.cpp'
FAILS, CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, msg):
    """msg = the checker's failure message on a planted case; None = the checker accepted it (a defect)."""
    if msg is None:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name}')
    else:
        print(f'ok   planted twin caught: {name} ({msg})')


# ------------------------------------------------------------------ A. the resolver parse
SYN = r'''
bool ResolveX(const std::string& name, uint32_t vec4s, const Ctx& c, float* out) {
  const auto rows = [&](const float* m, uint32_t n) { for (uint32_t k = 0; k < n; ++k) out[k] = m[k]; };
  const assets::Material& mt = *c.mat;
  static const bool old = std::getenv("X_OLD") != nullptr;
  if (old && name == "Alpha") { set4(0, 9.f, 9.f, 9.f, 9.f); return true; }
  if (name == "Alpha") { set4(0, 1.f, 0.5f, 1.f, 0.f); return true; }   // a literal "with a string"
  if (name == "Beta") { std::memcpy(out, mt.beta, 16); return true; }
  if (name == "Gamma" || name == "Gamma2") { set4(0, c.eye[0], 0.f, 0.f, 1.f); return true; }
  if (Load::GammaPlant() && name == "Delta") return true;
  if (c.wet) {
    if (name == "Delta") { set4(0, mt.delta[0], 0.f, 0.f, 0.f); return true; }
  }
  if (name == "Eps") {
    if (c.flag) set4(0, 0.f, 1.f, 0.f, 0.f);
    else set4(0, 1.f, 0.f, 0.f, 0.f);
    return true;
  }
  if (name == "Zeta" || name == "Eta")
    return true;
  if (name == "Mixed") { set4(0, mt.m[0], 1.f, 1.f, 0.f); return true; }
  if (name == "Front") { out[0] = float(c.t), out[1] = 2.f; return true; }
  if (name == "Local") { const float v = mt.v; set4(0, v, 0.f, v, 1e7f); return true; }
  if (name == "Rows") { rows(c.vp, 4); return true; }
  if (name == "Helper") { FillHelper(out, mt); return true; }
  if (name == "Arm") { if (c.live) set4(0, mt.a[0], mt.a[1], 0.f, 0.f); else set4(0, 0.f, 0.f, 1.f, 1.f); return true; }
  return false;
}
'''


def part_a():
    br, probs = Q.parse_resolver(SYN, r'^bool ResolveX\(', 'syn.cpp')
    lines = SYN.split('\n')
    lit_line = next(i + 1 for i, l in enumerate(lines) if 'name == "Alpha") { set4(0, 1.f' in l)
    k = lambda f: Q.ours_kind(br.get(f))[0]
    check('A literal', k('Alpha') == 'LITERAL', str(br.get('Alpha')))
    check('A plant skipped (getenv switch)', [b['plant'] for b in br['Alpha']] == [True, False], str(br['Alpha']))
    check('A literal line', br['Alpha'][1]['line'] == lit_line, str(br['Alpha']))
    check('A material = GAME', k('Beta') == 'GAME')
    check('A context = OURS, two names', k('Gamma') == 'OURS' and k('Gamma2') == 'OURS')
    check('A *Plant() skipped, nested branch read', k('Delta') == 'GAME' and br['Delta'][0]['plant'], str(br.get('Delta')))
    check('A if / else of literals = LITERAL... the condition reads c', k('Eps') == 'OURS', str(br.get('Eps')))
    check('A zero list', k('Zeta') == 'LITERAL' and br['Zeta'][0]['kind'] == 'ZERO')
    check('A no branch = UNFED', k('Omega') == 'UNFED')
    check('A no problems', probs == [], str(probs))
    ln = lambda f: br[f][0]['lanes']
    check('A lanes: set4 with one material lane', ln('Mixed') == {(0, 0): 'GAME', (0, 1): 'LITERAL', (0, 2): 'LITERAL', (0, 3): 'LITERAL'}, str(ln('Mixed')))
    check('A lanes: out[N] writes (the rest the caller\'s zero)', ln('Front') == {(0, 0): 'OURS', (0, 1): 'LITERAL'}
          and Q.lane_kind(br['Front'], (0, 3)) == 'LITERAL', str(ln('Front')))
    check('A lanes: a local built from the material', ln('Local') == {(0, 0): 'GAME', (0, 1): 'LITERAL', (0, 2): 'GAME', (0, 3): 'LITERAL'}, str(ln('Local')))
    check('A lanes: a memcpy is not split', ln('Beta') is None and Q.lane_kind(br['Beta'], (0, 2)) == 'GAME', str(ln('Beta')))
    check('A lanes: a resolver lambda that writes out is not split', ln('Rows') is None and Q.lane_kind(br['Rows'], (1, 2)) == 'OURS', str(ln('Rows')))
    check('A a root passed whole to a helper = its kind (GAME)', k('Helper') == 'GAME', str(br.get('Helper')))
    check('A a literal else arm inside a fed branch is marked', br['Arm'][0]['lit_arm'] and not br['Mixed'][0]['lit_arm'], str(br.get('Arm')))
    check('A lanes: if / else of literals = every lane LITERAL', Q.lane_kind(br['Eps'], (0, 0)) == 'LITERAL' and Q.lane_kind(br['Eps'], (0, 1)) == 'LITERAL',
          str(ln('Eps')))
    bad = SYN.replace('mt.beta', 'zz.beta')
    _b, p2 = Q.parse_resolver(bad, r'^bool ResolveX\(', 'syn.cpp')
    twin('A unmapped root accepted', p2[0] if p2 else None)


# ------------------------------------------------------------------ B. the listing walk
def part_b():
    import qdecomp
    COPY, CMP = Q.COPY_FN, Q.STRNCMP_FN
    S = {0x82000100: 'g_f4Common', 0x82000120: 'ych', 0x82000130: 'g_f4Own', 0x82000140: 'ybg', 0x82000150: 'yprop',
         0x82000160: 'g_fShared', 0x82000170: 'yfull', 0x82000180: 'g_fFull', 0x82000190: 'g_mSamp'}
    a = [0x82100000]

    def I(t):
        mn, _, ops = t.partition(' ')
        x = qdecomp.Insn(a[0], mn, ops, 0)
        a[0] += 4
        return x

    def absolute(reg, v):
        hi, lo = (v >> 16) & 0xFFFF, v & 0xFFFF
        if lo >= 0x8000:
            hi += 1
            lo -= 0x10000
        return [f'lis {reg},{hi - 0x10000 if hi >= 0x8000 else hi}', f'addi {reg},{reg},{lo}']

    def row(off, src, reg, comp, typ):
        return (['addi r3,r31,%d' % off] + absolute('r4', src) + [f'bl 0x{COPY:x}', f'li r11,{reg}', f'sth r11,{off + 16}(r31)', 'li r10,1',
                f'sth r10,{off + 18}(r31)', f'li r9,{comp}', f'stw r9,{off + 20}(r31)', f'li r11,{typ}', f'stw r11,{off + 24}(r31)'])

    def buf(dst, src):
        return absolute('r3', dst) + absolute('r4', src) + [f'bl 0x{COPY:x}']

    def cmp(bufaddr, n):
        return absolute('r4', bufaddr) + [f'li r5,{n}', 'mr r3,r21', f'bl 0x{CMP:x}']

    t = []
    t += absolute('r31', 0x83000400) + row(0, 0x82000100, 72, 0, 3)                       # common
    t += buf(0x830003C0, 0x82000120) + buf(0x830003B0, 0x82000140) + buf(0x830003A0, 0x82000150) + buf(0x83000390, 0x82000170)
    t += absolute('r26', 0x83000390) + ['ld r9,0(r26)']                                     # full-name compare with 'yfull'
    t += absolute('r31', 0x83000300) + row(0, 0x82000180, 50, 0, 1)
    t += cmp(0x830003C0, 3) + absolute('r31', 0x83000200) + row(0, 0x82000130, 144, 4, 3) + row(32, 0x82000190, 9, 0, 7)
    t += cmp(0x830003B0, 3) + absolute('r31', 0x83000100)
    br_at = len(t)
    t += ['b 0xFFFFFFFF']
    t += cmp(0x830003A0, 3) + absolute('r31', 0x83000000)
    fill = len(t)
    t += row(0, 0x82000160, 205, 0, 1)
    ins = [I(x) for x in t]
    ins[br_at].ops = f'0x{ins[fill].addr:x}'
    tb = Q.game_table(ins, S.get)
    names = {r['name']: r for r in tb['rows']}
    check('B rows read', set(names) == {'g_f4Common', 'g_fFull', 'g_f4Own', 'g_mSamp', 'g_fShared'}, str(sorted(names)))
    check('B register / count / component / type', (names['g_f4Own']['reg'], names['g_f4Own']['count'], names['g_f4Own']['comp'], names['g_f4Own']['type']) == (144, 1, 4, 3),
          str(names.get('g_f4Own')))
    modes = [(g['mode'], g['text']) for g in tb['groups']]
    check('B groups', modes == [('common', None), ('full', 'yfull'), ('prefix', 'ych'), ('prefix', 'ybg'), ('prefix', 'yprop')], str(modes))
    check('B alias (ybg shares yprop fill)', tb['alias'] == {0x83000100: 0x83000000}, str(tb['alias']))
    g = lambda e: sorted(r['name'] for r in Q.group_rows(tb, e))
    check('B yCh_skin = common + ych (sampler row dropped)', g('yCh_skin') == ['g_f4Common', 'g_f4Own'], str(g('yCh_skin')))
    check('B yBG_Default = common + shared fill', g('yBG_Default') == ['g_f4Common', 'g_fShared'], str(g('yBG_Default')))
    check('B yFull (full compare)', g('yFull') == ['g_f4Common', 'g_fFull'], str(g('yFull')))
    check('B other effect = common only', g('yToneMap') == ['g_f4Common'], str(g('yToneMap')))
    src = lambda st: Q.game_source('yCh_skin', st, 144, 'Own', 1, tb, {}, None, None, None)['rows']
    check('B a PS-bank row (+24 = 3) feeds the PS register, with its lane (+20 = 4 -> y)', [(r['name'], r['lanes']) for r in src('ps')] == [('g_f4Own', [(0, 1)])],
          str(src('ps')))
    check('B a PS-bank row is never a VS register\'s source', src('vs') == [], str(src('vs')))


# ------------------------------------------------------------------ E. GAME-CONST: the copier / constructor walk
def part_e():
    import qdecomp
    G, VT, KA, KB, KZ = 0x83001000, 0x82001000, 0x82002000, 0x82002004, 0x82002008
    COPIER, CTOR = 0x82100800, 0x82100900
    f2w = lambda v: struct.unpack('>I', struct.pack('>f', v))[0]
    words = {VT: 0x82100700, VT + 4: COPIER, KA: f2w(1.0), KB: f2w(0.83), KZ: 0}

    class Img:
        def word(self, a):
            return words.get(a)

    def hl(v):
        hi, lo = (v >> 16) & 0xFFFF, v & 0xFFFF
        if lo >= 0x8000:
            hi, lo = hi + 1, lo - 0x10000
        return (hi - 0x10000 if hi >= 0x8000 else hi), lo

    def ins(start, text):
        out, a = [], start
        for t in text:
            mn, _, ops = t.partition(' ')
            out.append(qdecomp.Insn(a, mn, ops, 0))
            a += 4
        return out
    gh, gl = hl(G)
    zh, zl = hl(KZ)
    copier = ['mflr r12', 'bl 0x82c78dfc', 'mr r31,r3', f'lis r11,{gh}', f'lwz r30,{gl}(r11)', 'mr r3,r30', 'bl 0x82bcbf78',
              'addi r29,r31,-20', 'bl 0x827e9cd0', 'lfs f10,44(r31)', 'lfs f9,56(r31)', 'lfs f12,48(r31)', f'lis r11,{zh}',
              f'lfs f13,{zl}(r11)', 'stfs f12,88(r1)', 'stfs f13,92(r1)', 'stfs f9,84(r1)', 'stfs f10,80(r1)', 'addi r10,r1,80',
              'ld r7,8(r10)', 'ld r6,0(r10)', 'std r6,320(r30)', 'std r7,328(r30)']
    vh, vl = hl(VT)
    ah, al = hl(KA)
    bh, bl_ = hl(KB)
    ctor = ['mflr r12', 'bl 0x82c78df8', 'mr r31,r3', f'lis r7,{vh}', f'addi r4,r7,{vl}', 'stw r4,20(r31)', f'lis r10,{ah}',
            f'lfs f31,{al}(r10)', 'bl 0x82baf4d0', f'lis r9,{bh}', f'lfs f13,{bl_}(r9)', 'stfs f31,64(r31)', 'stfs f31,68(r31)',
            'stfs f13,76(r31)']
    vals, why = Q.game_const(ins(COPIER, copier), ins(CTOR, ctor), COPIER, G, 320, Img())
    want = [1.0, struct.unpack('>f', struct.pack('>f', 0.83))[0], 1.0, 0.0]
    check('E GAME-CONST lanes through the spill, the 64-bit stores and the node at +20', vals == want, f'{vals} {why}')
    v2, w2 = Q.game_const(ins(COPIER, copier), ins(CTOR, [t for t in ctor if t != 'stfs f13,76(r31)']), COPIER, G, 320, Img())
    twin('E a lane with no constructor store resolved anyway', w2 if v2 is None else None)
    words[VT + 4] = 0x82100704
    v3, w3 = Q.game_const(ins(COPIER, copier), ins(CTOR, ctor), COPIER, G, 320, Img())
    twin('E a node vtable without the copier accepted', w3 if v3 is None else None)


# ------------------------------------------------------------------ C / D. real data
def frozen_reader(root, edits=None):
    cache = {}

    def read(p):
        if p not in cache:
            cache[p] = subprocess.run(['git', '-C', root, 'show', f'{FROZEN}:{p}'], capture_output=True, text=True, encoding='utf-8',
                                      check=True).stdout
        s = cache[p]
        for (ep, old, new) in (edits or []):
            if ep == p:
                if s.count(old) != 1:
                    raise SystemExit(f'plant anchor not unique in {p}: {old}')
                s = s.replace(old, new)
        return s
    return read


def expect_diff(rows, stage, reg, field, at):
    hits = [r for r in rows if r['effect'].startswith('yCh_') and r['stage'] == stage and r['reg'] == reg and r['field'] == field]
    if not hits:
        return f'no yCh_ row for {stage} c{reg} {field}'
    bad = [r['effect'] for r in hits if not (r['diff'] and at in r['ours_at'])]
    return f'{len(bad)} of {len(hits)} rows not DIFF at {at}: {bad[:4]}' if bad else None


def expect_no_diff(rows, stage, reg, field):
    hits = [r for r in rows if r['effect'].startswith('yCh_') and r['stage'] == stage and r['reg'] == reg and r['field'] == field]
    if not hits:
        return f'no yCh_ row for {stage} c{reg} {field}'
    bad = [r['effect'] for r in hits if r['diff']]
    return f'{len(bad)} of {len(hits)} rows DIFF: {bad[:4]}' if bad else None


def part_cd(root):
    sys.path.insert(0, os.path.join(root, 'tools', 'q'))
    try:
        import q as qq
        con = qq.connect(qq.DB)
        game = Q.load_game(con)
    except Exception as e:  # noqa: BLE001
        print(f'SKIP C / D (no q index or generated listing: {e})')
        return
    table, desc = game
    rows_by = {r['name']: r for r in table['rows'] if r['name'] in ('g_f4SpecCol', 'g_f4RimLightParam')}
    check('C name table: 209 rows, 7 groups', (len(table['rows']), len(table['groups'])) == (209, 7), f"{len(table['rows'])} / {len(table['groups'])}")
    sc, rl = rows_by.get('g_f4SpecCol'), rows_by.get('g_f4RimLightParam')
    check('C c144 row', sc and (sc['reg'], sc['addr'], sc['site']) == (144, 0x8392A6A0, 0x82BAC644), str(sc))
    check('C c161 row', rl and (rl['reg'], rl['addr'], rl['site']) == (161, 0x8392B600, 0x82BAB2B0), str(rl))
    check('C descriptor list: 47 names, c161 name among them', len(desc) == 47 and 'g_f4RimLightParam' in desc, str(len(desc)))
    fields = Q.load_fields(os.path.join(root, 'src', 'renderer', 'shaders', 'fx'))
    mats, totals = Q.material_names()
    if mats is None:
        print('note: no asset index: c144 cannot be told GAME-MATERIAL; its check is SKIPPED')
    writers, _e = Q.const_writers(con)
    consts, cprobs = Q.load_consts(con)
    check('C GAME-CONST chains resolve with no problem', cprobs == [], str(cprobs))
    rc = (consts.get('g_f4RimLightParam') or {}).get('values')
    check('C GAME-CONST g_f4RimLightParam = (1, 0.83, 1, 0) from sub_827E9D90 / sub_827EA7F0',
          rc == [1.0, struct.unpack('>f', struct.pack('>f', 0.83))[0], 1.0, 0.0], str(rc))

    def rows_of(edits=None):
        res, probs = Q.load_resolvers(frozen_reader(root, edits))
        rows, _sk = Q.build_rows(fields, res, game, mats, totals, writers, consts=consts)
        return rows, probs

    rows, probs = rows_of()
    check('C frozen resolvers parse with no problem', probs == [], str(probs))
    if mats is not None:
        m = expect_diff(rows, 'ps', 144, 'SpecCubPrm3', f'LITERAL {CHAR}:648')
        check('C RE-FOUND: ps c144 DIFF at char_effects.cpp:648', m is None, m or '')
    m = expect_no_diff(rows, 'ps', 161, 'RimLightParam')
    check('C ps c161 RimLightParam: the literal at :610 equals the game constant, not DIFF (S111 RIMGAME)', m is None, m or '')
    gk = sorted({g['kind'] for r in rows if r['effect'].startswith('yCh_') and r['stage'] == 'ps' and r['reg'] == 161 for g in r['game_rows'] if g['name']})
    check('C ps c161 game row is GAME-CONST', gk == ['GAME-CONST'], str(gk))
    m = expect_no_diff(rows, 'ps', 190, 'MatDifCol')
    check('C ps c190 MatDifCol (fed from the material) is not DIFF', m is None, m or '')
    m = expect_no_diff(rows, 'ps', 247, 'Param1')
    check('C ps c247 Param1 (x / z from the material, y / w unwritten by the game) is not DIFF', m is None, m or '')
    sv = [g for r in rows if r['effect'] == 'yCh_skin' and r['stage'] == 'vs' and r['reg'] == 168 for g in r['game_rows'] if g['name']]
    check('C vs c168 of yCh_skin: the VS-bank g_f4SphereOccP only (g_f4ShadowPrm1 is a PS-bank row)', [g['name'] for g in sv] == ['g_f4SphereOccP'],
          str([g['name'] for g in sv]))

    fixed, _p = rows_of([(CHAR, 'if (name == "SpecCubPrm3") { set4(0, 1.f, 1.f, 1.f, 0.f); return true; }',
                          'if (name == "SpecCubPrm3") { std::memcpy(out, mt.ambient, 16); return true; }')])
    if mats is not None:
        twin('D c144 fed from the material, still DIFF at 648', expect_diff(fixed, 'ps', 144, 'SpecCubPrm3', f'LITERAL {CHAR}:648'))
    lit, _p = rows_of([(CHAR, 'if (name == "MatDifCol") { std::memcpy(out, mt.diffuse_color, 16); return true; }',
                        'if (name == "MatDifCol") { set4(0, 1.f, 1.f, 1.f, 1.f); return true; }')])
    if mats is not None:
        twin('D MatDifCol made a literal, no new DIFF', expect_no_diff(lit, 'ps', 190, 'MatDifCol'))
    lane, _p = rows_of([(CHAR, 'set4(0, mt.fur_level, 0.f, mt.dif_amb_level, 0.f)', 'set4(0, mt.fur_level, 0.f, 0.f, 0.f)')])
    if mats is not None:
        twin('D Param1 .z (g_fDifAmbLevel) made a literal beside a material .x, no DIFF by lane', expect_no_diff(lane, 'ps', 247, 'Param1'))
    uneq, _p = rows_of([(CHAR, 'if (name == "RimLightParam") { set4(0, 1.f, 0.83f, 1.f, 0.f);', 'if (name == "RimLightParam") { set4(0, 1.f, 0.5f, 1.f, 0.f);')])
    twin('D c161 .y 0.5 against the game constant 0.83, no DIFF', expect_no_diff(uneq, 'ps', 161, 'RimLightParam'))
    m = expect_diff(uneq, 'ps', 161, 'RimLightParam', f'LITERAL {CHAR}:610')
    check('D c161 .y 0.5: the DIFF is at :610', m is None, m or '')
    _r, p3 = rows_of([(CHAR, 'if (name == "MatAmbCol") { std::memcpy(out, mt.ambient, 16); return true; }',
                       'if (name == "MatAmbCol") { std::memcpy(out, zz.ambient, 16); return true; }')])
    twin('D unmapped root in the real resolver accepted', p3[0] if p3 else None)
    fr, _p = Q.load_resolvers(frozen_reader(root))
    part_f(con, lambda **kw: Q.build_rows(fields, fr, game, mats, totals, writers, consts=consts, **kw)[0])


# ------------------------------------------------------------------ F. PASS-NEVER (S111 CONSTPASS)
def part_f(con, rows_with):
    """yDefault's shadow-only rows (PS c168-c170 ShadowPrm0..2) are PASS-NEVER: sub_82BDA038 sets no +4 word for 'ydefault', so the
    model draw never adds the shadow passes (the SHADOWNAME false alarm, re-found); yBG_Default's shadow row stays as it was; a name
    planted into the list (ych_skin renamed 'ydefault') and one renamed out of it (ybg_default) must each flip the result."""
    if not hasattr(Q, 'load_pass_flags'):
        check('F the tool has the PASS-NEVER class', False, 'qconstdiff has no load_pass_flags')
        return
    passes, perr = Q.effect_passes()
    if passes is None:
        print(f"SKIP F (the player's effect files: {perr})")
        return
    flags, fprobs = Q.load_pass_flags(con)
    check('F pass flags read from sub_82BDA038 with no problem', fprobs == [] and flags is not None, str(fprobs))
    if flags is None:
        return
    s4 = flags.get(4, set())
    check('F the +4 (shadow) word: 25 names, ych_skin / ybg_default / yprop_psm among them, ydefault / ybg_ps / ybg_uvscroll not',
          len(s4) == 25 and {'ych_skin', 'ybg_default', 'yprop_psm'} <= s4 and not {'ydefault', 'ybg_ps', 'ybg_uvscroll'} & s4, str(sorted(s4)))
    pick = lambda rs, eff, st, reg: next((r for r in rs if r['effect'] == eff and r['stage'] == st and r['reg'] == reg), None)
    rows, base = rows_with(passes=passes, flags=flags), rows_with()
    yd = [pick(rows, 'yDefault', 'ps', reg) for reg in (168, 169, 170)]
    check('F RE-FOUND: yDefault PS c168-c170 (ShadowPrm0..2) are PASS-NEVER, not DIFF',
          all(r and r['pass_never'] and not r['diff'] for r in yd), str([(r['field'], r['diff'], r['pass_never']) if r else None for r in yd]))
    md = pick(rows, 'yDefault', 'vs', 190)
    check('F yDefault VS c190 MatDifCol (the base passes read it) is not PASS-NEVER', bool(md) and not md['pass_never'], str(md and md['pass_never']))
    bg, bg0 = pick(rows, 'yBG_Default', 'ps', 168), pick(base, 'yBG_Default', 'ps', 168)
    same = bool(bg and bg0) and not bg['pass_never'] and all(bg[k] == bg0[k] for k in ('ours', 'game', 'diff', 'diff_lanes', 'partial'))
    check('F yBG_Default PS c168 ShadowPrm (a shadow pass the game draws) stays as it was', same, str(bg and bg['pass_never']))
    # CONSTPASS review: OUR wrestler path draws a cube-specular row's non-cube twin in its place (char_effects.cpp ChooseCharEffectTable
    # -> CubeSibling), so yCh_TransTiling / yCh_cloth_trans p0 / p1 (cube_specular_fragment) draw their base_fragment in ours and its
    # c0 / c8 are read by a drawn pass: not PASS-NEVER. Their shadow-only c168 stays PASS-NEVER.
    for eff in ('yCh_TransTiling', 'yCh_cloth_trans'):
        for reg in (0, 8):
            r = pick(rows, eff, 'ps', reg)
            check(f'F {eff} PS c{reg} (read by the base fragment ours draws as the p0 / p1 cube twin) is not PASS-NEVER',
                  bool(r) and not r['pass_never'], str(r and r['pass_never']))
        r = pick(rows, eff, 'ps', 168)
        check(f'F {eff} PS c168 ShadowPrm (shadow variants only) is PASS-NEVER', bool(r) and bool(r['pass_never']), str(r and r['pass_never']))
    # a reachable pass whose shader the registry cannot name: its constants are unknown, so no register of that effect is PASS-NEVER
    un = getattr(Q, 'UNNAMED', '?unnamed')
    syn = {'yX': {'yX_shadow_fragment': {2, 3}}}
    fl = {w: {'yother'} for w, _a, _y in Q.PASS_TERMS}
    check('F unit: a shadow-only variant of an effect whose name sets no word is PASS-NEVER',
          bool(Q.pass_never('yX', {'yX_shadow_fragment'}, syn, fl)))
    syn['yX'][un] = {0}
    check('F unit: not PASS-NEVER when a reachable pass has a shader the registry cannot name',
          Q.pass_never('yX', {'yX_shadow_fragment'}, syn, fl) is None)
    added, _p = Q.load_pass_flags(con, plant={'ych_skin': 'ydefault'})
    r = pick(rows_with(passes=passes, flags=added), 'yDefault', 'ps', 168) if added else None
    twin("F 'ydefault' planted into the list (ych_skin renamed), yDefault c168 still PASS-NEVER", None if (r is None or r['pass_never']) else 'no longer PASS-NEVER')
    removed, _p = Q.load_pass_flags(con, plant={'ybg_default': 'ybg_renamed'})
    r = pick(rows_with(passes=passes, flags=removed), 'yBG_Default', 'ps', 168) if removed else None
    twin("F 'ybg_default' renamed out of the list, yBG_Default c168 not PASS-NEVER", 'now PASS-NEVER' if (r and r['pass_never']) else None)


# ------------------------------------------------------------------ G. effect_passes' cache (CONSTPASS review: 7 s per call uncached)
def part_g():
    """The map is cached by the inputs' size and mtime: a warm call reads the file (a planted map comes back), a key that differs
    from the inputs is rebuilt (the planted stale cache must not be used)."""
    if 'cache_path' not in inspect.signature(Q.effect_passes).parameters:
        check('G effect_passes takes a cache_path (cached by its inputs)', False, 'no cache')
        return
    tmp = tempfile.mkdtemp(prefix='constpass_g_')
    p = os.path.join(tmp, 'effect_passes.json')
    a, err = Q.effect_passes(cache_path=p)
    if a is None:
        print(f"SKIP G (the player's effect files: {err})")
        return
    check('G the cache file is written', os.path.exists(p))
    with open(p, encoding='utf-8') as fh:
        d = json.load(fh)
    d['passes'] = {'yPlanted': {'yPlanted_x_fragment': [7]}}
    with open(p, 'w', encoding='utf-8') as fh:
        json.dump(d, fh)
    b, _e = Q.effect_passes(cache_path=p)
    check('G a warm call reads the cache (the planted map comes back)', b == {'yPlanted': {'yPlanted_x_fragment': {7}}}, str(b)[:200])
    d['key'] = 'stale'
    with open(p, 'w', encoding='utf-8') as fh:
        json.dump(d, fh)
    c, _e = Q.effect_passes(cache_path=p)
    twin('G a cache whose key differs from the inputs is used', 'rebuilt' if c == a else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    Q.REPO = root
    Q.FX_DIR = os.path.join(root, 'src', 'renderer', 'shaders', 'fx')
    sys.path.insert(0, os.path.join(root, 'tools', 'q'))
    part_a()
    part_b()
    part_e()
    part_cd(root)
    part_g()
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

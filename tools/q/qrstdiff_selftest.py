r"""Self-test of `q rstdiff` (S111 RSTDIFF): `py -I tools/q/qrstdiff_selftest.py`   (0 game runs)

A. Synthetic (invented text and instructions, no game code): the expression reader (a plant ternary takes its shipped arm, an
   unknown condition joins both arms, a named constant, a local, a one-line helper with a bound and with an unbound argument,
   pass_state::Of(pcls).field from a table, a game word), the block walker (if / else under a known condition, a comma chain,
   a conditional assignment joining the value before it), the game-window reader (the last call of a set window, every call of
   an adds window, a non-constant r4 = VAR, the reset as the base only where no set window sets the field), the listing's
   paths (a branch-joined r4 is both literals, a set window's two arms join, a path past the set or around the window keeps
   the inherited value) and the judge (a reversed GEQUAL = our LEQUAL only for a mirrored kind).
B. Real data (needs generated/ and the q index; else SKIP), our code read at two frozen refs:
     1. FROZEN b2a29d266 (S111 PARTZ note, before the fix landed): particles zfunc DIFF, the game has ALWAYS (0x82A0AED8 +84) and
        ours never (live_particles.cpp:207, LEQUAL): the Kane flare;
     2. TIP 2401ccb49: particles zfunc OK (ParticleDepthCompare: ALWAYS and LEQUAL), crowd_flash ztest DIFF with ours at
        front_end_flashes.h:101 and the game's 0 at 0x823B27F0 +354 (S110 FLASHFRONT: the `replace` row of q seam flash), the
        scene classes' zfunc OK (GEQUAL from the default reset 0x82C26ED0 = our LEQUAL through ForwardZ);
     3. TIP, the paths (S111 RSTDIFF review): the flash mesh's cull 0 / 1 (0x82C0F528 +E30) a DIFF, not the text-order OK; the
        rim map's per-mesh cull (0x82BF9878 +1D00) both values; the lens element's z test 0 / 1 (0x827D5E98 +598).
     4. TIP, S111 RSTCOVER-B: the colour mask and the stencil enable read from every setter (qrstdiff_inherit.py): 0xF / off at
        the particles, the flash, the lens element and the FADE (the lens element's 0xF against our rgb-only 0x7: a DIFF); the
        lens element's cull 0 from the T_Sprite leaf (0x82BC4890 +154); the FADE's cull 0 on both arms (0x82BC3C18 +78); the
        particles' blend factors from the emitter codes 0 / 1 / 2 only (0x82A0AED8 +DC); no UNSETTLED row left.
C. Planted twins, each must be CAUGHT: (1) ParticleDepthCompare made LEQUAL on both arms (the tip's particles zfunc must turn
   DIFF); (2) the flash's shipped arm made 0 (its ztest DIFF must go: also proves the plant arm is not the one read); (3) the
   particle kind without its adds window (the tip's zfunc must turn DIFF: ours has ALWAYS, the game never); (4) scene_lit not
   mirrored (its zfunc must turn DIFF); (5) a window moved off its calls (a PROBLEM); (6) pass_state.h's Lit row given GREATER
   (ForwardZ -> LESS: scene_lit zfunc must turn DIFF).
   S111 RSTCOVER-B: (7) the stencil save / restore pair dropped (the stencil rows turn UNSETTLED, naming 0x82C1FB60); (8) the
   z prepass's arm dropped (the mask rows turn UNSETTLED, naming 0x82BEF748); (9) the lens element without its T_Sprite leaf
   window (its cull turns UNSETTLED); (10) the T_Rect leaf's +78 arm dropped (the FADE cull turns UNSETTLED); (11) the
   particle entry's +DC arm dropped (blend_src turns UNSETTLED); (12) our particle mask made 0x7 (its cmask turns DIFF);
   (13) BlendOf's mask made 0x7 (the FADE cmask turns DIFF: the builder is read).
   A (synthetic) also covers the census: a save / restore through the stack, a leak named, an early return (and its arm), a
   jump-table arm, an argument-word restore without and with its saver.
E. (S111 CASTCULL review) A kind's helper absent at the ref read: skipped only at a ref older than the helper's first commit (the
   frozen TIP predates caster_cull.h); a planted ref whose history holds it (HEAD) must raise.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import copy
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrstdiff as R   # noqa: E402

FROZEN = 'b2a29d266'   # S111 PARTZ note: the particle fix carried, not landed (live_particles.cpp:207 `state.depth_compare = 3;`)
TIP = '2401ccb49'      # the S111 tip this tool was stated against (the PARTZ fix in, FLASHFRONT's per-pixel test in)
FAILS, CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, caught, detail=''):
    if not caught:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name} {detail}')
    else:
        print(f'ok   planted twin caught: {name}')


# ------------------------------------------------------------------ A. synthetic
SYN_H = r'''
inline constexpr uint8_t kLe = 3, kAl = 7;
inline uint8_t Pick(const Batch& b) { return b.always ? kAl : kLe; }
constexpr uint8_t Fwd(uint8_t f) { return f == 6 ? 3 : f == 4 ? 1 : f; }
'''
SYN_CPP = r'''
void Thing::Draw() {
  rdr::DrawState st{};
  st.cull = 0;
  const bool zt = ctx.scene && (row.state & 16) != 0;
  st.depth_test = g_plant_off.load() ? 0 : 1, st.depth_write = 0;   // a comment, st.cull = 9;
  st.depth_compare = Pick(b);
  if (ctx.scene) {
    st.blend[0].enable = zt ? 1 : 0;
    st.stencil_test = 1;
  } else {
    st.stencil_test = 5;
  }
  if (it.pass) st.cull = 2;
  st.blend[0].write_mask = Q2D_PLANT(3) ? 0xF : 0x7;
  rdr::TextureSet set{};
}
'''


def part_a():
    ctx = R.Ctx([R.CD._mask(SYN_H)], 'const bool loc = it.x; int n = 4;', {}, [r'\brow\.state\b'],
                {'lit': {'name': 'lit', 'game_z_func': 6}, 'odd': {'name': 'odd', 'game_z_func': 4}}, ['lit'])
    check('A plant ternary: shipped arm', R.eval_val('g_plant_flash_no_ztest.load(std::memory_order_relaxed) ? 0 : 1', ctx) == {1})
    check('A plant macro, negated', R.eval_val('!Q2D_PLANT(60) ? 5 : 6', ctx) == {5})
    check('A unknown condition: both arms', R.eval_val('it.depth_write ? 1 : 0', ctx) == {0, 1})
    check('A named constant', R.eval_val('kAl', ctx) == {7})
    check('A local', R.eval_val('n', ctx) == {4})
    check('A helper, unbound argument', R.eval_val('Pick(b)', ctx) == {3, 7})
    check('A helper, bound argument', R.eval_val('ns::Fwd(6)', ctx) == {3} and R.eval_val('Fwd(4)', ctx) == {1} and R.eval_val('Fwd(7)', ctx) == {7})
    check('A table field through a helper', R.eval_val('pass_state::Fwd(pass_state::Of(pcls).game_z_func)', ctx) == {3})
    ctx.classes = ['lit', 'odd']
    check('A two table rows join', R.eval_val('Fwd(Of(pcls).game_z_func)', ctx) == {3, 1})
    check('A game word in a condition', R.eval_val('(row.state & 32) != 0 ? 1 : 0', ctx) == {R.GAME})
    check('A a rule of ours', next(iter(R.eval_val('family_cull::Cull(mp, a, b)', ctx))).startswith('RULE'))
    kind = {'kind': 'syn', 'fields': R.FIELDS}
    o = {'file': 'syn.cpp', 'after': r'Thing::Draw\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': ['syn.h'],
         'known': {r'ctx\.scene': True}, 'game_words': [r'\brow\.state\b']}
    st, start = R.ours_state(kind, o, {'syn.cpp': SYN_CPP, 'syn.h': SYN_H}.__getitem__)
    v = lambda f: set(st[f]['vals'])
    check('A block start line', start == 3, str(start))
    check('A comma chain', v('ztest') == {1} and v('zwrite') == {0}, f"{v('ztest')} {v('zwrite')}")
    check('A helper in a block', v('zfunc') == {3, 7}, str(v('zfunc')))
    check('A known-true if taken, else skipped', v('stencil') == {1}, str(v('stencil')))
    check('A game word through a local', v('blend') == {R.GAME}, str(v('blend')))
    check('A conditional assignment joins', v('cull') == {0, 2} and st['cull']['cond'], str(v('cull')))
    check('A a comment is not read', 9 not in v('cull'))
    check('A plant arm in a mask', v('cmask') == {7}, str(v('cmask')))

    from qdecomp import Insn
    F = 0x82000000
    ins = lambda off, mn, ops: Insn(F + off, mn, ops, 0)
    prog = [ins(0x0, 'li', 'r4,0'), ins(0x4, 'bl', '0x82BB75A0'), ins(0x8, 'li', 'r4,1'), ins(0xC, 'bl', '0x82BB75A0'),
            ins(0x10, 'li', 'r4,1'), ins(0x14, 'bl', '0x82BB78E0'), ins(0x18, 'lwz', 'r4,8(r31)'), ins(0x1C, 'bl', '0x82BB7630'),
            ins(0x20, 'li', 'r4,0'), ins(0x24, 'bl', '0x82BB7AD8')]
    reset = [Insn(R.RESET_FN + 0x84, 'li', 'r4,6', 0), Insn(R.RESET_FN + 0x88, 'bl', '0x82CA9750', 0),
             Insn(R.RESET_FN + 0x8C, 'li', 'r4,1', 0), Insn(R.RESET_FN + 0x90, 'bl', '0x82CA9720', 0),
             Insn(R.RESET_FN + 0x94, 'li', 'r4,1', 0), Insn(R.RESET_FN + 0x98, 'bl', '0x82CA96E0', 0)]
    io = lambda a: reset if a == R.RESET_FN else prog
    k = {'kind': 'g', 'base': True, 'game': [{'fn': F, 'lo': 0, 'hi': 0x10, 'mode': 'set', 'why': 'set'},
                                             {'fn': F, 'lo': 0x10, 'hi': 0x18, 'mode': 'adds', 'why': 'adds'},
                                             {'fn': F, 'lo': 0x18, 'hi': 0x28, 'mode': 'set', 'why': 'set 2'}]}
    g, p = R.game_state(k, io)
    check('A game: no problem', not p, str(p))
    check('A game: the last call of a set window', set(g['blend']['vals']) == {1}, str(g['blend']))
    check('A game: an adds window joins the reset', set(g['zfunc']['vals']) == {6, 7}, str(g['zfunc']))
    check('A game: a set window replaces the reset', set(g['zwrite']['vals']) == {0}, str(g['zwrite']))
    check('A game: the reset where nothing sets it', set(g['ztest']['vals']) == {1}, str(g['ztest']))
    check('A game: a non-constant r4 is VAR', g['blend_src']['var'] and not g['blend_src']['vals'], str(g['blend_src']))
    k2 = dict(k, game=[{'fn': F, 'lo': 0x100, 'hi': 0x110, 'mode': 'set', 'why': 'empty'}])
    check('A game: an empty window is a PROBLEM', any('no recorder call' in x for x in R.game_state(k2, io)[1]))
    gst = {'ztest': {'vals': {1: []}, 'var': []}, 'blend': {'vals': {1: []}, 'var': []}}
    gz = {'vals': {6: ['x']}, 'var': [], 'unset': False}
    oz = {'vals': {3: ['y']}, 'cond': []}
    check('A judge: mirrored GEQUAL = LEQUAL', R.judge({'mirror': True}, 'zfunc', gz, oz, gst, {})[0] == 'OK')
    check('A judge: not mirrored, a DIFF', R.judge({'mirror': False}, 'zfunc', gz, oz, gst, {})[0] == 'DIFF')
    check('A judge: inherited is UNSETTLED', R.judge({}, 'cmask', R._gv(), {'vals': {15: []}, 'cond': []}, gst, {})[0] == 'UNSETTLED')

    # S111 RSTDIFF review: the listing's paths. A literal that differs by path is every literal, never the last one in the text
    # (sub_82C0F528 +E18..+E30, the flash mesh's cull: a 1 with a branch over a later 0, read as 0 alone, was a false OK).
    pj = [ins(0x0, 'li', 'r4,1'), ins(0x4, 'bne', 'cr6,0x8200000C'), ins(0x8, 'li', 'r4,0'), ins(0xC, 'bl', '0x82BB7C28')]
    v = R.calls(pj, F, R.WRAPPERS)
    check('A paths: a branch-joined r4 holds both literals', v and v[0][2] == frozenset({0, 1}), str(v))
    # a set window with two alternative paths (the T_Rect leaf 0x82BC3C18 +38..+74: blend 1 on one, blend 0 on the other)
    alt = [ins(0x0, 'beq', 'cr6,0x82000014'), ins(0x4, 'li', 'r4,1'), ins(0x8, 'bl', '0x82BB75A0'), ins(0xC, 'b', '0x8200001C'),
           ins(0x10, 'nop', ''), ins(0x14, 'li', 'r4,0'), ins(0x18, 'bl', '0x82BB75A0'), ins(0x1C, 'blr', '')]
    ka = {'kind': 'alt', 'game': [{'fn': F, 'lo': 0, 'hi': 0x1C, 'mode': 'set', 'why': 'alt'}]}
    g, p = R.game_state(ka, lambda a: alt)
    check('A paths: a set window joins its alternative paths', set(g['blend']['vals']) == {0, 1} and not p, f"{g['blend']} {p}")
    # a path that skips the window's only z func set keeps the inherited value: the default reset's for a scene draw
    skip = [ins(0x0, 'beq', 'cr6,0x8200000C'), ins(0x4, 'li', 'r4,1'), ins(0x8, 'bl', '0x82BB78E0'), ins(0xC, 'blr', '')]
    ks = {'kind': 'skip', 'base': True, 'game': [{'fn': F, 'lo': 0, 'hi': 0xC, 'mode': 'set', 'why': 'skip'}]}
    g, p = R.game_state(ks, lambda a: reset if a == R.RESET_FN else skip)
    check('A paths: a path past the set keeps the reset', set(g['zfunc']['vals']) == {6, 7} and not p, f"{g['zfunc']} {p}")
    ks2 = dict(ks, base=False)
    g, p = R.game_state(ks2, lambda a: skip)
    check('A paths: a path past the set, no reset read: UNSETTLED', R.judge(ks2, 'zfunc', g['zfunc'], {'vals': {7: []}, 'cond': []}, g, {})[0] == 'UNSETTLED',
          str(g['zfunc']))
    # a path from before the window straight to its end draws with what came before: it joins as inherited
    byp = [ins(0x0, 'beq', 'cr6,0x8200000C'), ins(0x4, 'li', 'r4,0'), ins(0x8, 'bl', '0x82BB7898'), ins(0xC, 'blr', '')]
    kb = {'kind': 'byp', 'game': [{'fn': F, 'lo': 0x4, 'hi': 0xC, 'mode': 'set', 'why': 'byp'}]}
    g, p = R.game_state(kb, lambda a: byp)
    check('A paths: a path around the window joins as inherited', set(g['ztest']['vals']) == {0} and g['ztest'].get('inh') and not p, f"{g['ztest']} {p}")


# ------------------------------------------------------------------ B / C. real data
def reader(ref, edits=None):
    base = R.default_reader(ref)

    def read(rel):
        t = base(rel)
        for old, new in (edits or {}).get(rel, ()):
            if t.count(old) != 1:
                raise SystemExit(f'planted edit not found once in {rel} at {ref}: {old!r}')
            t = t.replace(old, new)
        return t
    return read


def rows_of(con, ref, edits=None, kinds=None):
    io = R.insns_reader(con)
    return R.build_rows(kinds or R.KINDS, io, reader(ref, edits))


def row(rows, kind, field, ctx=None):
    return next((r for r in rows if r['kind'] == kind and r['field'] == field and (ctx is None or r['ctx'] == ctx)), None)


def part_bc():
    try:
        import q as qq
        con = qq.connect(qq.DB)
        R.CD._insns(con, 0x82A0AED8)
        for ref in (FROZEN, TIP):
            subprocess.run(['git', 'cat-file', '-e', f'{ref}^{{commit}}'], cwd=R.REPO, check=True, capture_output=True)
    except Exception as e:  # noqa: BLE001 - no index / listing / refs on this machine
        print(f'SKIP B / C (no q index, generated listing or frozen refs: {e})')
        return
    fz, pz = rows_of(con, FROZEN)
    check('B frozen: no problem', not pz, str(pz))
    r = row(fz, 'particles', 'zfunc')
    check('B1 frozen: the Kane flare particles zfunc DIFF', r and r['flag'] == 'DIFF' and 'the game has ALWAYS' in r['why']
          and 'live_particles.cpp:207' in r['ours'] and '0x82A0AED8 +84' in r['game'], str(r))
    tp, pt = rows_of(con, TIP)
    check('B tip: no problem', not pt, str(pt))
    r = row(tp, 'particles', 'zfunc')
    check('B2 tip: particles zfunc OK', r and r['flag'] == 'OK' and r['ours_vals'] == ['3', '7'], str(r))
    r = row(tp, 'crowd_flash', 'ztest')
    check('B2 tip: the flash depth test DIFF (q seam flash replace)', r and r['flag'] == 'DIFF' and 'front_end_flashes.h:101' in r['ours']
          and '0x823B27F0 +354' in r['game'], str(r))
    bad = [(x['kind'], x['ctx']) for x in tp if x['kind'].startswith('scene_') and x['field'] == 'zfunc' and x['flag'] != 'OK']
    check('B2 tip: every scene class zfunc OK', not bad, str(bad))
    # S111 RSTDIFF review: the false OKs the text-order reader gave. The flash mesh's cull is 1 or 0 by a branch
    # (sub_82C0F528 +E18..+E30): both values, so ours' two-sided sprite is a DIFF; the rim map's cull is also set per mesh by the
    # T_RimLightMap pass function (sub_82BF9878 +1D00), 0 or 1.
    r = row(tp, 'crowd_flash', 'cull')
    check('B3 tip: the flash mesh cull read over both paths: DIFF', r and r['flag'] == 'DIFF' and '1 @0x82C0F528 +E30' in r['game']
          and '0 @0x82C0F528 +E30' in r['game'], str(r))
    r = row(tp, 'rim_map', 'cull')
    check('B3 tip: the rim map cull per mesh (0x82BF9878 +1D00): DIFF', r and r['flag'] == 'DIFF' and r['game'].count('0x82BF9878 +1D00') == 2, str(r))
    r = row(tp, 'lens_element', 'ztest')
    check('B3 tip: the lens element z test from its texture index is 0 or 1', r and r['game_vals'] == ['0', '1'], str(r))
    fx, ferr = R.effect_rows()
    if fx is None:
        print(f'SKIP B effect files ({ferr})')
    else:
        check('B effect files: no pass carries a compared state', fx['passes'] > 0 and not fx['rows'] and fx['fill_only'] > 0, str({k: v for k, v in fx.items() if k != 'rows'}))

    LP, FL, PSH = 'src/renderer_scene/live_particles.h', 'src/renderer_scene/front_end_flashes.h', R.PS
    t1, _ = rows_of(con, TIP, {LP: [('b.depth_always ? kParticleCompareAlways : kParticleCompareLessOrEqual',
                                     'b.depth_always ? kParticleCompareLessOrEqual : kParticleCompareLessOrEqual')]})
    twin('C1 ParticleDepthCompare made LEQUAL', (row(t1, 'particles', 'zfunc') or {}).get('flag') == 'DIFF')
    t2, _ = rows_of(con, TIP, {FL: [('g_plant_flash_no_ztest.load(std::memory_order_relaxed) ? 0 : 1',
                                     'g_plant_flash_no_ztest.load(std::memory_order_relaxed) ? 1 : 0')]})
    twin('C2 the flash shipped arm made 0', (row(t2, 'crowd_flash', 'ztest') or {}).get('flag') == 'OK')
    ks = copy.deepcopy(R.KINDS)
    pk = next(k for k in ks if k['kind'] == 'particles')
    pk['game'] = [w for w in pk['game'] if w['mode'] != 'adds']
    t3, _ = rows_of(con, TIP, kinds=[pk])
    twin('C3 the particle adds window dropped', (row(t3, 'particles', 'zfunc') or {}).get('flag') == 'DIFF')
    ks = copy.deepcopy(R.KINDS)
    lk = next(k for k in ks if k['kind'] == 'scene_lit')
    lk['mirror'] = False
    t4, _ = rows_of(con, TIP, kinds=[lk])
    twin('C4 scene_lit not mirrored', (row(t4, 'scene_lit', 'zfunc') or {}).get('flag') == 'DIFF')
    ks = copy.deepcopy(R.KINDS)
    fk = next(k for k in ks if k['kind'] == 'crowd_flash')
    fk['game'][0]['lo'], fk['game'][0]['hi'] = 0x10, 0x20
    _t5, p5 = rows_of(con, TIP, kinds=[fk])
    twin('C5 a window moved off its calls', any('no recorder call' in x for x in p5))
    t6, _ = rows_of(con, TIP, {PSH: [('{"lit", 6, 1.f / 255.f, 6,', '{"lit", 6, 1.f / 255.f, 4,')]},
                    kinds=[k for k in R.KINDS if k['kind'] == 'scene_lit'])
    twin('C6 the Lit row given GREATER', (row(t6, 'scene_lit', 'zfunc') or {}).get('flag') == 'DIFF')
    part_cover(con)   # S111 RSTCOVER-B


# ------------------------------------------------------------------ S111 RSTCOVER-B: the inherited-state census
def part_a_inherit():
    """Synthetic programs (no game code) through qrstdiff_inherit.census."""
    from qdecomp import Insn
    I = R.RCB

    def getter(a, w):
        return [Insn(a, 'lwz', 'r3,0(r3)', 0), Insn(a + 4, 'bl', '0x82BA7238', 0), Insn(a + 8, 'lwz', f'r3,{w}(r3)', 0), Insn(a + 12, 'blr', '', 0)]
    init = ([Insn(I.INIT_FN, 'li', 'r11,1', 0)] + [Insn(I.INIT_FN + 4 + 4 * k, 'stw', f'r11,{w}(r3)', 0) for k, w in enumerate((92, 96, 100, 104, 108))]
            + [Insn(I.INIT_FN + 0x20, 'li', 'r10,0', 0), Insn(I.INIT_FN + 0x24, 'stw', 'r10,28(r3)', 0)])
    F1, F2, F3, F4, F5, F6, F7 = 0x82100000, 0x82110000, 0x82120000, 0x82130000, 0x82140000, 0x82150000, 0x82160000

    def at(F):
        return lambda off, mn, ops: Insn(F + off, mn, ops, 0)
    a, b, c, d, e, f, g = at(F1), at(F2), at(F3), at(F4), at(F5), at(F6), at(F7)
    lists = {
        0x82BB82C8: getter(0x82BB82C8, 92), 0x82BB81B0: getter(0x82BB81B0, 108), 0x82BB8228: getter(0x82BB8228, 28), I.INIT_FN: init,
        # F1: save +92 through the stack, set 0, restore the saved value
        F1: [a(0x0, 'bl', '0x82BB82C8'), a(0x4, 'stw', 'r3,80(r1)'), a(0x8, 'li', 'r4,0'), a(0xC, 'bl', '0x82BB7970'),
             a(0x10, 'lwz', 'r4,80(r1)'), a(0x14, 'bl', '0x82BB7970'), a(0x18, 'blr', '')],
        # F2: set 0 and return (a leak)
        F2: [b(0x0, 'li', 'r4,0'), b(0x4, 'bl', '0x82BB7970'), b(0x8, 'blr', '')],
        # F3: set 0, an early return past the reset to 1
        F3: [c(0x0, 'li', 'r4,0'), c(0x4, 'bl', '0x82BB7970'), c(0x8, 'beq', f'cr6,0x{F3 + 0x14:08X}'), c(0xC, 'li', 'r4,1'),
             c(0x10, 'bl', '0x82BB7970'), c(0x14, 'blr', '')],
        # F4: set 0, then a switch whose arm (no path from the entry) returns
        F4: [d(0x0, 'li', 'r4,0'), d(0x4, 'bl', '0x82BB7970'), d(0x8, 'bctr', ''), d(0xC, 'li', 'r3,0'), d(0x10, 'blr', '')],
        # F5 / F6: a saver of the stencil word into its argument +20 (setting the start value 0) and its restorer
        F5: [e(0x0, 'mr', 'r31,r3'), e(0x4, 'bl', '0x82BB8228'), e(0x8, 'stw', 'r3,20(r31)'), e(0xC, 'li', 'r4,0'),
             e(0x10, 'bl', '0x82BB76C0'), e(0x14, 'blr', '')],
        F6: [f(0x0, 'mr', 'r31,r3'), f(0x4, 'lwz', 'r4,20(r31)'), f(0x8, 'bl', '0x82BB76C0'), f(0xC, 'blr', '')],
        # F7 (S111 RSTCOVER-B review): set 0, THEN read the getter (it returns the 0 just set) and "restore" that: a leak, not a restore
        F7: [g(0x0, 'li', 'r4,0'), g(0x4, 'bl', '0x82BB7970'), g(0x8, 'bl', '0x82BB82C8'), g(0xC, 'mr', 'r4,r3'),
             g(0x10, 'bl', '0x82BB7970'), g(0x14, 'blr', '')],
    }

    def run(name, fns, arms=None, pairs=None):
        sa, sp = dict(I.ARMS), dict(I.PAIRS)
        I.ARMS.clear(), I.PAIRS.clear()
        I.ARMS.update(arms or {}), I.PAIRS.update(pairs or {})
        try:
            return I.census(name, lists.__getitem__, lambda addrs: list(fns))
        finally:
            I.ARMS.clear(), I.PAIRS.clear()
            I.ARMS.update(sa), I.PAIRS.update(sp)
    r = run('cmask', [F1])
    check('A inherit: a save / restore through the stack leaves the mask as found', r['value'] == 0xF and not r['leaks'], str(r))
    r = run('cmask', [F1, F2])
    check('A inherit: a setter that leaves 0 is named', r['value'] is None and any('0x82110000' in x and '+92 = 0' in x for x in r['leaks']), str(r))
    r = run('cmask', [F3])
    check('A inherit: an early return past the reset is a leak', r['value'] is None and any('0x82120000 returns at +14' in x for x in r['leaks']), str(r))
    r = run('cmask', [F3], arms={F3: {0x8: ('fall', 'synthetic')}})
    check('A inherit: the same, its branch read one way by an arm', r['value'] == 0xF and r['arms'], str(r))
    r = run('cmask', [F4])
    check('A inherit: a jump-table arm keeps the set made before the switch', r['value'] is None and any('0x82130000 returns at +10' in x for x in r['leaks']), str(r))
    r = run('stencil', [F5, F6])
    check('A inherit: a restore from an argument word without its pair is a leak', r['value'] is None and any('argument word +20' in x for x in r['leaks']), str(r))
    r = run('stencil', [F5, F6], pairs={F6: {'saver': F5, 'why': 'synthetic'}})
    check('A inherit: the same with its saver paired: restored', r['value'] == 0 and r['pairs'] and not r['leaks'], str(r))
    # S111 RSTCOVER-B review: a getter read AFTER the function's own set of its word returns that set value, not the one found
    r = run('cmask', [F7])
    check('A inherit: a getter read after its own set is no save (a leak)', r['value'] is None and any('0x82160000' in x and '+92 = VAR' in x for x in r['leaks']), str(r))

    # S111 RSTCOVER-B review: a kind whose game window is in a setter's own function draws inside that setter's set section
    # (S111 RSTCOVER-A's circle blobs inside 0x82813228): the start value is not its state, so the field is not filled.
    def io(addr):
        return lists[addr]
    io.callers_of = lambda addrs: [F1]
    io.inherit_cache = {}
    st = {'cmask': R._gv(), 'stencil': R._gv()}
    I.fill(st, io, [F1])
    check('A inherit: a kind whose window function is a setter is not filled (named)', st['cmask']['unset'] is False and not st['cmask']['vals']
          and any('0x82100000' in x for x in st['cmask']['inh']), str(st['cmask']))
    st = {'cmask': R._gv(), 'stencil': R._gv()}
    I.fill(st, io, [0x82170000])
    check('A inherit: the same census for a kind outside the setter: filled', set(st['cmask']['vals']) == {15} and not st['cmask']['inh'], str(st['cmask']))


def part_cover(con):
    """B4 / C7-C13 (our code at TIP): the 11 rows RSTDIFF left UNSETTLED."""
    I = R.RCB
    tp, pt = rows_of(con, TIP)
    check('B4 tip: no problem', not pt, str(pt))
    for kd in ('particles', 'crowd_flash', 'fade'):
        r = row(tp, kd, 'cmask')
        check(f'B4 tip: {kd} cmask 0xF inherited from every setter: OK', r and r['flag'] == 'OK' and r['game_vals'] == ['15']
              and 'inherited: all 9 setters' in r['game'], str(r))
    r = row(tp, 'lens_element', 'cmask')
    check('B4 tip: the lens element cmask, the game 0xF, ours rgb only: DIFF', r and r['flag'] == 'DIFF' and r['game_vals'] == ['15']
          and r['ours_vals'] == ['7'], str(r))
    # S111 RSTCOVER-B review: TIP's rgb-only line removed (as the review's quads2d_draw.cpp:182 does): BlendOf's 0xF is read, the row is OK
    t14, _ = rows_of(con, TIP, {'src/renderer_scene/quads2d_draw.cpp': [('state.blend[0].write_mask = 0x7;', '')]}, kinds=[k for k in R.KINDS if k['kind'] == 'lens_element'])
    r = row(t14, 'lens_element', 'cmask')
    check('B4 tip: the lens element without our rgb-only line: BlendOf 0xF, OK', r and r['flag'] == 'OK' and r['ours_vals'] == ['15'], str(r))
    for kd in ('particles', 'crowd_flash', 'lens_element', 'fade'):
        r = row(tp, kd, 'stencil')
        check(f'B4 tip: {kd} stencil off inherited (with the 0x82C1FA98 / 0x82C1FB60 pair): OK', r and r['flag'] == 'OK'
              and r['game_vals'] == ['0'] and 'inherited: all 6 setters' in r['game'] and '1 by a save / restore pair' in r['game'], str(r))
    r = row(tp, 'lens_element', 'cull')
    check('B4 tip: the lens element cull 0 from the T_Sprite leaf: OK', r and r['flag'] == 'OK' and '0 @0x82BC4890 +154' in r['game']
          and 'inherited' not in r['game'], str(r))
    r = row(tp, 'fade', 'cull')
    check('B4 tip: the FADE cull 0 on both arms of +78: OK', r and r['flag'] == 'OK' and r['game_vals'] == ['0'] and 'inherited' not in r['game'], str(r))
    r = row(tp, 'particles', 'blend_src')
    check('B4 tip: the particles blend_src from the codes 0 / 1 / 2 only: OK', r and r['flag'] == 'OK' and 'inherited' not in r['game'], str(r))
    cover_a = {k['kind'] for k in getattr(R, 'KINDS_COVER_A', [])}   # (the merge with RSTCOVER-A: its 13 kinds are not this part's 11 rows)
    left = [(x['kind'], x['field']) for x in tp if x['flag'] == 'UNSETTLED' and x['kind'] not in cover_a]
    check('B4 tip: no UNSETTLED row left', not left, str(left))

    def kinds_named(*names):
        return [k for k in copy.deepcopy(R.KINDS) if k['kind'] in names]
    saved = dict(I.PAIRS)
    I.PAIRS.clear()
    try:
        t7, _ = rows_of(con, TIP, kinds=kinds_named('particles'))
    finally:
        I.PAIRS.update(saved)
    r = row(t7, 'particles', 'stencil')
    twin('C7 the stencil save / restore pair dropped', bool(r) and r['flag'] == 'UNSETTLED' and '0x82C1FB60' in r['game'], str(r))
    saved = dict(I.ARMS)
    I.ARMS.clear()
    try:
        t8, _ = rows_of(con, TIP, kinds=kinds_named('particles'))
    finally:
        I.ARMS.update(saved)
    r = row(t8, 'particles', 'cmask')
    twin('C8 the z prepass arm dropped', bool(r) and r['flag'] == 'UNSETTLED' and '0x82BEF748' in r['game'], str(r))
    ks = kinds_named('lens_element')
    ks[0]['game'] = [w for w in ks[0]['game'] if w['fn'] != 0x82BC4890]
    t9, _ = rows_of(con, TIP, kinds=ks)
    twin('C9 the lens element without its T_Sprite leaf window', (row(t9, 'lens_element', 'cull') or {}).get('flag') == 'UNSETTLED')
    ks = kinds_named('fade')
    for w in ks[0]['game']:
        w.pop('arms', None) if w['fn'] == 0x82BC3C18 and w['lo'] == 0x84 else None
    t10, _ = rows_of(con, TIP, kinds=ks)
    twin('C10 the T_Rect leaf +78 arm dropped', (row(t10, 'fade', 'cull') or {}).get('flag') == 'UNSETTLED')
    ks = kinds_named('particles')
    for w in ks[0]['game']:
        w.pop('arms', None) if w['fn'] == 0x82A0AED8 else None
    t11, _ = rows_of(con, TIP, kinds=ks)
    twin('C11 the particle entry +DC arm dropped', (row(t11, 'particles', 'blend_src') or {}).get('flag') == 'UNSETTLED')
    t12, _ = rows_of(con, TIP, {'src/renderer_scene/live_particles.cpp': [('state.blend[0].write_mask = 0xF;', 'state.blend[0].write_mask = 0x7;')]},
                     kinds=kinds_named('particles'))
    twin('C12 our particle mask made 0x7', (row(t12, 'particles', 'cmask') or {}).get('flag') == 'DIFF')
    t13, _ = rows_of(con, TIP, {'src/renderer_scene/quads2d_blocks.h': [('out.write_mask = 0xF;', 'out.write_mask = 0x7;')]},
                     kinds=kinds_named('fade'))
    twin('C13 BlendOf mask made 0x7 (the builder is read)', (row(t13, 'fade', 'cmask') or {}).get('flag') == 'DIFF')


# ------------------------------------------------------------------ S111 RSTCOVER-A: the 13 kinds of the map, 2D and post blocks
# A (synthetic): a BlendState reference with switch case arms and a chained assignment, a window's slots filter, z func moot when
# ours does not test depth. D (real, TIP): one row per new kind, and per kind a planted twin of our code that must flip it; the
# ours-only block is listed apart and a lost anchor is a PROBLEM. Each check fails on the tool before RSTCOVER-A (no kind, no reader).
SYN_SW = r'''
void Two::Draw() {
  rdr::DrawState state{};
  rdr::BlendState& b = state.blend[0];
  switch (item.blend) {
    case 1:  // additive
      b.enable = 1, b.src_color = 6, b.dst_color = 1;
      break;
    case 2:  // opaque
      b.src_color = b.src_alpha = 1, b.dst_color = b.dst_alpha = 0;
      break;
    default:
      b.enable = 1, b.src_color = 6, b.dst_color = 7;
      break;
  }
  rdr::TextureSet textures;
}
'''


# S111 RSTCOVER-A review: three forms the first reader dropped without a word (each a value lost, so a lenient read that can fake an OK):
# an `auto&` reference, a scoped-enum case label (`case Mode::Add:`), a chain whose compared field is not the first target, and a whole
# BlendState assigned through the reference.
SYN_SW2 = r'''
void Three::Draw() {
  rdr::DrawState state{};
  auto& b = state.blend[0];
  switch (item.mode) {
    case Mode::Add:
      b.enable = 1;
      break;
    case Mode::Copy:
      b = kCopyBlend;
      break;
    default:
      b.dst_alpha = b.dst_color = 9;
      break;
  }
  rdr::TextureSet textures;
}
'''


def part_cover_a_review():
    kind = {'kind': 'syn3', 'fields': R.FIELDS}
    o = {'file': 'sw2.cpp', 'after': r'Three::Draw\b', 'end': r'rdr::TextureSet\s+textures\b'}
    st, _start = R.ours_state(kind, o, {'sw2.cpp': SYN_SW2}.__getitem__)
    v = lambda f: set(st[f]['vals'])
    check('A5 an auto& reference and a scoped case label: blend joins 1', 1 in v('blend'), str(v('blend')))
    check('A5 a chain: the compared field in the middle is read', 9 in v('blend_dst'), str(v('blend_dst')))
    check('A5 a whole BlendState through the reference is a rule of ours', any(isinstance(x, str) and x.startswith('RULE') for x in v('blend_src')), str(v('blend_src')))
    from qdecomp import Insn
    F = 0x82000000
    outer = [Insn(F, 'li', 'r4,1', 0), Insn(F + 4, 'bl', '0x82BB78E0', 0), Insn(F + 8, 'blr', '', 0)]
    k = {'kind': 'sl2', 'game': [{'fn': F, 'lo': 0, 'hi': 8, 'mode': 'set', 'slots': ('zfunc',), 'why': 'a slot named by its field'}]}
    _g, p = R.game_state(k, lambda a: outer)
    check('A5 a slots name that is no recorder slot is a PROBLEM', any('name no recorder slot' in x for x in p), str(p))


def part_cover_a_syn():
    kind = {'kind': 'syn2', 'fields': R.FIELDS}
    o = {'file': 'sw.cpp', 'after': r'Two::Draw\b', 'end': r'rdr::TextureSet\s+textures\b'}
    st, _start = R.ours_state(kind, o, {'sw.cpp': SYN_SW}.__getitem__)
    v = lambda f: set(st[f]['vals'])
    check('A4 a BlendState reference, case arms: blend joins 0 / 1', v('blend') == {0, 1}, str(v('blend')))
    check('A4 a chained assignment takes the last right-hand side', v('blend_src') == {0, 1, 6} and v('blend_dst') == {0, 1, 7}, f"{v('blend_src')} {v('blend_dst')}")
    from qdecomp import Insn
    F = 0x82000000
    ins = lambda off, mn, ops: Insn(F + off, mn, ops, 0)
    outer = [ins(0x0, 'li', 'r4,1'), ins(0x4, 'bl', '0x82BB7C28'), ins(0x8, 'li', 'r4,0'), ins(0xC, 'bl', '0x82BB75A0'), ins(0x10, 'blr', '')]
    G = 0x82100000
    inner = [Insn(G, 'li', 'r4,0', 0), Insn(G + 4, 'bl', '0x82BB7C28', 0), Insn(G + 8, 'blr', '', 0)]
    io = lambda a: outer if a == F else inner
    k = {'kind': 'sl', 'game': [{'fn': F, 'lo': 0, 'hi': 0x10, 'mode': 'set', 'slots': ('blend',), 'why': 'outer'},
                                {'fn': G, 'lo': 0, 'hi': 8, 'mode': 'set', 'why': 'inner'}]}
    g, p = R.game_state(k, io)
    check("A4 a window's slots: the outer cull is not joined", set(g['cull']['vals']) == {0} and set(g['blend']['vals']) == {0} and not p,
          f"{g['cull']} {g['blend']} {p}")
    gz = {'vals': {6: ['x']}, 'var': [], 'unset': False}
    gst = {'ztest': {'vals': {}, 'var': [], 'inh': ['y']}, 'blend': {'vals': {}, 'var': [], 'inh': []}}
    ost = {'ztest': {'vals': {0: ['z']}, 'cond': []}}
    check('A4 z func moot when ours does not test depth', R.judge({'mirror': True}, 'zfunc', gz, {'vals': {0: []}, 'cond': []}, gst, ost)[0] == 'OK')


COVER_A = [   # (kind, field, the flag at TIP, texts the game column holds, text the ours column holds, the planted edit, the flag it must give)
    ('map_chain', 'cull', 'OK', ['0 @0x827E89C0 +64', '0x827EA098 +1F4'], 'live_map_chain.cpp:69',
     ('src/renderer_scene/live_map_chain.cpp', 'st.front_face = 1;', 'st.front_face = 1, st.cull = 2;'), 'DIFF'),
    ('circle_blobs', 'cull', 'DIFF', ['1 @0x827E95D8 +88'], 'live_map_circles.cpp:126',
     ('src/renderer_scene/live_map_circles.cpp', 'state.cull = 0;', 'state.cull = 2;'), 'OK'),
    ('props_casters', 'cull', 'DIFF', ['1 @0x827E95D8 +88'], 'live_map_props.cpp:74',
     ('src/renderer_scene/live_map_props.cpp', 'state.cull = 0;', 'state.cull = 2;'), 'OK'),
    ('shadow_casters', 'zfunc', 'OK', ['GEQUAL (ours-side LEQUAL) @0x827E95D8 +7C'], 'live_map_shadow.cpp:389',
     ('src/renderer_scene/live_map_shadow.cpp', 'state.depth_compare = 3;', 'state.depth_compare = 6;'), 'DIFF'),
    ('plane_mask', 'zwrite', 'OK', ['1 @0x827EE190 +30'], 'live_map_plane.cpp:208',
     ('src/renderer_scene/live_map_plane.cpp', 'state.depth_write = 1, state.depth_compare = 3;', 'state.depth_write = 0, state.depth_compare = 3;'), 'DIFF'),
    ('plane_rope', 'zfunc', 'OK', ['GEQUAL (ours-side LEQUAL) @0x827EEF80 +98'], 'live_map_rope.cpp:232',
     ('src/renderer_scene/live_map_rope.cpp', 'state.depth_compare = 3;', 'state.depth_compare = 7;'), 'DIFF'),
    ('plane_downscale', 'cull', 'DIFF', ['1 @0x827EEF80 +80'], 'live_map_plane.cpp:346',
     ('src/renderer_scene/live_map_plane.cpp', 'ds.front_face = 1;', 'ds.front_face = 1, ds.cull = 2;'), 'OK'),
    ('plane_blur', 'ztest', 'OK', ['0 @0x827EEAE0 +80'], 'live_map_plane.cpp:392',
     ('src/renderer_scene/live_map_plane.cpp', 'bs.depth_test = 0, bs.depth_write = 0;', 'bs.depth_test = 1, bs.depth_write = 0;'), 'DIFF'),
    ('prelight', 'stencil', 'OK', ['0 @0x82C1FA98 +AC'], 'live_map_prelight.cpp:82',
     ('src/renderer_scene/live_map_prelight.cpp', 'state.primitive_restart = 1;', 'state.primitive_restart = 1, state.stencil_test = 1;'), 'DIFF'),
    ('sweat', 'blend', 'DIFF', ['1 @0x8279ACD8 +B4'], 'live_map_sweat.cpp:182',
     ('src/renderer_scene/live_map_sweat.cpp', 'st.blend[0].write_mask = write_mask;', 'st.blend[0].write_mask = write_mask, st.blend[0].enable = 1;'), 'OK'),
    ('sprite_2d', 'blend', 'OK', ['1 @0x82BC4890 +DAC', '0 @0x82BC4890 +DD4'], 'pass2d.cpp:569',
     ('src/renderer_scene/pass2d.cpp', 'rdr::BlendState& b = state.blend[0];', 'rdr::BlendState b = state.blend[0];'), 'DIFF'),
    ('post_quad', 'blend', 'OK', ['0 @0x82C2BF00 +84', '0x82C1E1D0 +FC'], 'post_pass.cpp:343',
     ('src/renderer_scene/post_pass.cpp', 'st.vertex_layout = Layout(vs);', 'st.vertex_layout = Layout(vs), st.blend[0].enable = 1;'), 'DIFF'),
    ('post_ghost', 'blend', 'DIFF', ['1 @0x82BC4890 +DAC', '0 @0x82BC4890 +DD4'], 'post_pass.cpp:639',
     ('src/renderer_scene/post_pass.cpp', 'st.blend[0].write_mask = 0xF, st.blend[1].write_mask = 0xF;',
      'st.blend[0].write_mask = 0xF, st.blend[1].write_mask = 0xF; if (first) st.blend[0].enable = 1;'), 'OK'),
]


def part_cover_a(con):
    names = {k['kind'] for k in R.KINDS}
    missing = [c[0] for c in COVER_A if c[0] not in names]
    check('D the RSTCOVER-A kinds exist', not missing, str(missing))
    kinds = [k for k in R.KINDS if k['kind'] in {c[0] for c in COVER_A}]
    tp, pt = rows_of(con, TIP, kinds=kinds or None) if not missing else ([], ['kinds missing'])
    check('D tip: no problem', not pt, str(pt))
    for kind, field, flag, games, ours, (rel, old, new), planted in COVER_A:
        r = row(tp, kind, field)
        check(f'D {kind} {field} {flag}', r and r['flag'] == flag and all(g in r['game'] for g in games) and ours in r['ours'], str(r))
        if missing:
            twin(f'D {kind} planted {old!r}', False, 'the kind is missing')
            continue
        t, _ = rows_of(con, TIP, {rel: [(old, new)]}, kinds=[k for k in R.KINDS if k['kind'] == kind])
        twin(f'D {kind} planted {old!r} -> {planted}', (row(t, kind, field) or {}).get('flag') == planted, str(row(t, kind, field)))
    # (the merge with RSTCOVER-B) the circle wrapper 0x82813228 is itself a colour-mask setter: it turns channels R / G off at +CC / +D8
    # (wrappers 0x82BB79B8 / 0x82BB7A00, recorder words +96 / +100) BEFORE its draw at +F4, so the census's "inherited 0xF" is not the
    # circle's mask (that would be B | A = 0xC): the row stays UNSETTLED, naming the setter.
    r = row(tp, 'circle_blobs', 'cmask')
    check('D circle_blobs cmask: the draw runs inside a mask setter, not the census value', r and r['flag'] == 'UNSETTLED'
          and '0x82813228' in r['game'] and 'itself a setter' in r['game'] and 'inherited: all' not in r['game'], str(r))
    # ... and a window function that is a setter only AFTER its draw (the plane proxy draw 0x827EE190 restores the stencil at +10C,
    # its draw is at +E0) leaves the draw the inherited value: the census fills it.
    r = row(tp, 'plane_mask', 'stencil')
    check('D plane_mask stencil: a setter that restores only after the draw still inherits: OK', r and r['flag'] == 'OK'
          and 'inherited: all' in r['game'], str(r))
    oo = getattr(R, 'ours_only', None)
    got = oo(R.default_reader(TIP))[0] if oo else []
    check('D the dconv block is ours-only (no game draw)', [(f, ln) for f, ln, _w in got] == [('src/renderer_scene/post_pass.cpp', 405)], str(got))
    if oo:
        bad = oo(reader(TIP, {'src/renderer_scene/post_pass.cpp': [('depth_program_made_ = true;', 'depth_program_built_ = true;')]}))[1]
        twin('D an ours-only anchor lost is a PROBLEM', bool(bad), str(bad))


def part_cover_a_run():
    part_cover_a_syn()
    part_cover_a_review()
    try:
        import q as qq
        con = qq.connect(qq.DB)
        R.CD._insns(con, 0x82A0AED8)
        subprocess.run(['git', 'cat-file', '-e', f'{TIP}^{{commit}}'], cwd=R.REPO, check=True, capture_output=True)
    except Exception as e:  # noqa: BLE001 - no index / listing / refs on this machine
        print(f'SKIP D (no q index, generated listing or the tip ref: {e})')
        return
    part_cover_a(con)
# ------------------------------------------------------------------ end S111 RSTCOVER-A


# ------------------------------------------------------------------ S111 CASTCULL review: a helper missing at a --ref
CASTER_H = 'src/renderer_scene/caster_cull.h'


def _missing(ref, rel):
    cls = getattr(R, 'MissingAtRef', None)   # the error default_reader raises when the path is not in the ref
    return cls(f'git show {ref}:{rel}: missing', ref, rel) if cls else R.RstDiffError(f'git show {ref}:{rel}: missing')


def part_helper_ref():
    """A kind's helper absent at the ref read is skipped only when the ref is OLDER than the helper's first commit (the frozen
    refs predate caster_cull.h); absent at a ref whose history holds it (deleted or renamed since) is an error, never silence."""
    try:
        subprocess.run(['git', 'cat-file', '-e', f'HEAD:{CASTER_H}'], cwd=R.REPO, check=True, capture_output=True)
        subprocess.run(['git', 'cat-file', '-e', f'{TIP}^{{commit}}'], cwd=R.REPO, check=True, capture_output=True)
    except Exception as e:  # noqa: BLE001 - no git history here
        print(f'SKIP E (no git history with {CASTER_H}: {e})')
        return
    o = {'file': 'syn/thing.cpp', 'after': r'Thing::Draw\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': [CASTER_H]}

    def at(ref):
        def read(rel):
            if rel == 'syn/thing.cpp':
                return SYN_CPP
            raise _missing(ref, rel)
        return read

    try:
        R.ours_state({'kind': 'syn'}, o, at(TIP))
        older = 'skipped'
    except R.RstDiffError as e:
        older = f'raised: {e}'
    check('E a helper added after the ref read (the frozen tip, before caster_cull.h) is skipped', older == 'skipped', older)
    try:
        R.ours_state({'kind': 'syn'}, o, at('HEAD'))
        newer = 'skipped'
    except R.RstDiffError as e:
        newer = f'raised: {e}'
    twin('E a helper missing at a ref whose history holds it is an error, not skipped', newer.startswith('raised'), newer)
    try:
        R.default_reader(TIP)(CASTER_H)
        got = 'read'
    except Exception as e:  # noqa: BLE001
        got = type(e).__name__
    check('E default_reader: a path not in the ref raises MissingAtRef', got == 'MissingAtRef', got)


def main():
    part_a()
    part_a_inherit()   # S111 RSTCOVER-B
    part_bc()
    part_cover_a_run()
    part_helper_ref()   # S111 CASTCULL review
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

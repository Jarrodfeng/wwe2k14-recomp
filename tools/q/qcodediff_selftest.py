r"""Self-test of `q codediff` (S111 CODEDIFF): `py -I tools/q/qcodediff_selftest.py`   (0 game runs)

A. Invented fact rows (q ostate / q gstate --facts shapes) and invented q pairs rows, through the production join qcodediff.run with a
   dict reader for the DEPARTS markers, the row files and the mod switch:
   each verdict (DIFF, GAME-ONLY, OURS-ONLY, UNSETTLED, OK, DEPARTED); each unit conversion (alpha func code 5 = GEQUAL = Vk 6; z func
   code 2 = GEQUAL through pass_state.h ForwardZ = LEQUAL 3; Vk cull 2 = on; blend code 1 = ADD SRC_ALPHA ONE against our Vk numbers,
   no GAME-ONLY blend code beside them; no_blend never compared); moot (no z test); the caller's InstancedDraw field deferred; a game
   set on one path only (an rstdiff `adds` window) and a game function of several values left UNSETTLED; a citation on a fact's line
   pairing it with the nearest setter up the game's call graph; our func 7 as the untested arm; the DrawState zero's factors with the
   blend-off row; DEPARTS honoured, a thin reason, a missing row ref, a class naming the
   wrong row file and a mod departure outside its switch each refused (and a mod behind floor_blood::On accepted).
   Planted twins, each seen to FAIL its check: raw alpha codes, no ForwardZ, no caller deferral, a row ref not looked up, a mod
   accepted anywhere, no UNSETTLED for a several-value game function, no pair scope, func 7 compared as a func, the zero factors compared.
B. Real re-finds (our code at frozen refs, the game index; SKIP without the refs): at 13ded28c9^1 (the CROWDATEST parent) the crowd
   member draw FrontEnd::DrawInstanced vs sub_82519508 alpha_test is a DIFF (game 0, ours the material flag) and at the tip it is not;
   at b2a29d266 (the PARTZ note, RSTDIFF's re-find) LiveParticles vs sub_82A0AED8 z_func is a DIFF (game ALWAYS, ours LEQUAL), at the
   tip not. S111 CDARMS at the tip: FrontEnd::BindMaterial's wrestler colour arm stands against the translucent node sub_8259FAE8 only
   (UNSETTLED: a rule of ours); no row against the shadow-map / rim passes (sub_82C01FE0, sub_82BF9878), the nodes that set no alpha
   test (sub_825A3908, sub_82C04DF0) or the mask sampler's sub_8339F0E0.
C. S111 CDARMS (tools/q/qcdarms.py): a planted bind (BindMaterial's shape: a wrestler arm that returns, then the material store) read by
   q ostate (qostate.collect, dict reader) and joined by qcodediff.run: the arm stands against the cited node that sets the field (its
   comment block above the store); not against the map pass its comment's scope words (`colour pass only`) leave out (its technique
   T_ShadowMap), the opaque node that sets none, a sampler citation elsewhere in the arm or a claim outside every arm; the material arm
   has no pair. Twins: one_arm (every store one unit), no_scope_words, arm_anywhere.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qcodediff as C   # noqa: E402

FAILS, CAUGHT = [], []
PARENT, FIX = '13ded28c9^1', '13ded28c9'
PARTZ = 'b2a29d266'


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


PS = 'constexpr uint8_t ForwardZ(uint8_t vk_func) { return vk_func == 6 ? 3 : vk_func == 4 ? 1 : vk_func; }\n'
CPP = 'src/renderer_scene/syn_codediff.cpp'
INST = 'src/renderer_scene/syn_inst.cpp'
DR = 'src/renderer_scene/syn_draw.cpp'
FAULTY_PS = 'constexpr uint8_t ForwardZ(uint8_t vk_func) { return vk_func == 6 ? 1 : vk_func; }\n'   # a fault in OUR ForwardZ
SRC = {
    DR: '\n'.join([   # FrontEnd::DrawInstanced's shape (S111 CDFOLLOW): the blend follows `tested`, the caller's InstancedDraw::alpha_test
        'uint32_t Syn::DrawI(const InstancedDraw& d) {',                                           # 1
        '  const bool tested = d.alpha_test < 0 ? bool(b.alpha_test) : d.alpha_test != 0;',      # 2
        '  rdr::DrawState state{};',                                                             # 3
        '  if (tested && pass_state::Of(pcls).tested_blends) state.blend[0].enable = 1;',       # 4
        '  fb.Dynamic(tested ? AlphaDynamic(d) : base);',                                        # 5
        '}', '']),
    CPP: '\n'.join([
        'void Syn::Draw() {',                                                              # 1
        '  rdr::DrawState state{};',                                                       # 2
        '  state.depth_test = 0;',                                                         # 3
        '  // DEPARTS: habit 360_HABITS:15; the additive rows are drawn untested, the habit row',   # 4
        '  state.cull = 0;',                                                               # 5
        '  state.stencil_test = 0;   // DEPARTS: habit 360_HABITS:15; short',              # 6
        '  x = 1;   // DEPARTS: game-bug-fix GAME_BUG_FIXES:GBF-99; the flaming table fire is put out',     # 7
        '  y = 1;   // DEPARTS: habit GAME_BUG_FIXES:GBF-3; a habit row named in the bug file',            # 8
        '  z = 1;   // DEPARTS: mod DECISIONS:2026-11-27 18:4x; the permanent blood mod keeps the splats',  # 9
        '  w = 1;   // DEPARTS: mod DECISIONS:2026-11-27 18:4x; the permanent blood mod keeps the splats',  # 10
        '  v = 1;   // a cite of sub_83000100 (the draw leaf)',                             # 11
        '  // DEPARTS: habit 360_HABITS:15 @sub_83000F00; the cull habit against this node only',   # 12
        '  u = 0;',                                                                        # 13
        '  t = 0;   // DEPARTS: habit 360_HABITS:15 @0x83000F00; a scope that is not a game function name',   # 14
        '}', '']),
    'src/renderer_scene/syn_switch.h': '// the switch: --wwe2k14_mods_permanent_blood\ninline bool On() { return g_on; }\n',
    'src/launcher/report_facts.inc': 'mod | wwe2k14_mods_permanent_blood | permanent blood | src/start_settings.cpp\n',
    'docs/renderer/360_HABITS.md': '| # | What | Where |\n|---|---|---|\n| 15 | The gm alpha test on additive rows | x |\n',
    'docs/GAME_BUG_FIXES.md': '| ID | Bug |\n|---|---|\n| GBF-3 | A flaming table fire stays lit |\n',
    'docs/DECISIONS.md': '- 2026-11-27 18:4x | Permanent mat blood is a MOD\n',
    INST: '\n'.join([   # FrontEnd::DrawInstanced's shape: the blend follows a local that is the caller's InstancedDraw::alpha_test
        'void Syn::Inst(const InstancedDraw& d) {',                                              # 1
        '  const bool tested = d.alpha_test < 0 ? bool(b.alpha_test) : d.alpha_test != 0;',     # 2
        '  rdr::DrawState state{};',                                                            # 3
        '  if (tested && tb) state.blend[0].enable = 1;',                                       # 4
        '  bool tb2 = tested;',                                                                 # 5
        '  fb.Dynamic(tested ? AlphaDynamic(d) : base);',                                       # 6
        '}', '']),
}


def reader(rel):
    if rel not in SRC:
        raise C.O.OstateError(f'{rel}: not planted')
    return SRC[rel]


def of(fn, field, value, at, kind='set', cond=None, **kw):
    f = {'side': 'ours', 'fn': fn, 'field': field, 'value': value, 'cond': cond, 'at': at, 'kind': kind}
    f.update(kw)
    return f


def gf(fn, field, value, at, kind='set'):
    return {'side': 'game', 'fn': fn, 'field': field, 'value': value, 'cond': None, 'at': at, 'kind': kind}


def prow(ours, game, line, src='comment', file=CPP):
    return {'src': src, 'ours': ours, 'kind': 'func', 'file': file, 'line': line, 'unit': file, 'game': game[4:], 'off': '', 'text': '', 'rel': 'MIRRORS', 'why': ''}


BL = f'{CPP}:1'
G2D = f"sub_{[k for k in C.R.KINDS if k['kind'] == 'sprite_2d'][0]['game'][0]['fn']:08X}"   # a real 2D kind's game function (mirror False)
G = {n: f'sub_830000{i:02X}' for i, n in enumerate(('diff', 'units', 'moot', 'uns', 'rec', 'six', 'leaf', 'seven', 'other', 'marks'))}
GP = 'sub_82A0AED8'   # a real rstdiff `adds` window: lo +84 (the particle entry's ALWAYS on one path)


def blk(fn, field, value, ln, kind='set', cond=None, **kw):
    return of(fn, field, value, f'{CPP}:{ln}', kind=kind, cond=cond, block=BL, **kw)


OURS = [
    blk('Syn::Diff', 'z_enable', 1, 3),
    blk('Syn::Units', 'alpha_test', 1, 3, 'alpha'), blk('Syn::Units', 'alpha_func', 6, 3, 'alpha'), blk('Syn::Units', 'z_func', 3, 3),
    blk('Syn::Units', 'cull', 2, 3), blk('Syn::Units', 'blend_enable', 1, 3), blk('Syn::Units', 'blend_op', 0, 3),
    blk('Syn::Units', 'blend_src', 6, 3), blk('Syn::Units', 'blend_dst', 1, 3),
    of('Syn::Units', 'no_blend', 1, f'{CPP}:3', kind='caller'),
    blk('Syn::Moot', 'z_func', 7, 3),
    blk('Syn::Uns', 'z_write', 1, 3), blk('Syn::Uns', 'alpha_ref', {'rule': 'row.alpha_ref'}, 3, 'alpha'), blk('Syn::Uns', 'stencil_enable', 1, 3),
    of('Syn::Rec', 'alpha_test', 0, f'{CPP}:10', kind='caller', cond='On() && i < n'),
    blk('Syn::Rec', 'alpha_test', {'caller': 'InstancedDraw::alpha_test', 'expr': 'd.alpha_test != 0'}, 3, 'alpha', cond='!(d.alpha_test < 0)'),
    blk('Syn::Rec', 'alpha_test', {'material': 'b.alpha_test'}, 3, 'alpha', cond='d.alpha_test < 0'),
    blk('Syn::Part', 'z_func', 3, 3), blk('Syn::Part', 'z_func', 7, 3), blk('Syn::Part', 'blend_enable', 0, 3),
    blk('Syn::Cite', 'cull', 2, 11),
    blk('Syn::Marks', 'colour_write_mask', 7, 5), blk('Syn::Marks', 'stencil_enable', 0, 6, 'default'), blk('Syn::Marks', 'alpha_test', 1, 3, 'alpha'),
    blk('Syn::Marks', 'alpha_ref', 1, 7, 'alpha'), blk('Syn::Marks', 'blend_enable', 1, 8), blk('Syn::Marks', 'z_write', 0, 9),
    blk('Syn::Func7', 'alpha_func', 6, 3, 'alpha', cond='tested'), blk('Syn::Func7', 'alpha_func', 7, 3, 'alpha', cond='!(tested)'),
    blk('Syn::Fdef', 'blend_enable', 0, 2, 'default'), blk('Syn::Fdef', 'blend_enable', 1, 3, cond='it.blend'),
    blk('Syn::Fdef', 'blend_src', 0, 2, 'default'), blk('Syn::Fdef', 'blend_src', 6, 3, cond='it.blend'),
    blk('Syn::Kind', 'blend_enable', 0, 3, kinds=['scene_lit (generic)']), blk('Syn::Kind', 'z_enable', 1, 3, kinds=['scene_lit (generic)']),
] + [   # SEETHRUBLEND's shape: ONE block drawing every family (FrontEnd::Record), its see-through kind narrowed to z, its blend arms
    # another family's (the it.blend ONE arm is the stage `a` slots'), citations elsewhere in the function (a claim on a scan line,
    # the screen grab's binding comment) and one on a blend arm's own line
    of('Syn::Seethru', f, v, f'{CPP}:{ln}', kind=k, cond=c, block=f'{CPP}:20', kinds=['scene_see_through (generic)', 'scene_lit (generic)'])
    for f, v, ln, k, c in (('z_enable', 1, 22, 'set', None), ('blend_enable', 0, 20, 'default', None),
                           ('blend_enable', 1, 24, 'set', '!it.blend && at_on && tested_blends'), ('blend_src', 6, 25, 'set', '!it.blend && at_on'),
                           ('blend_dst', 7, 26, 'set', '!it.blend && at_on'), ('blend_enable', 1, 28, 'set', 'it.blend'),
                           ('blend_dst', 1, 29, 'set', 'it.blend'), ('blend_dst', 7, 29, 'set', 'it.blend'))
] + [   # its per-class arm (AlphaDynamic's ref read per pass class): the arm whose condition names the see-through family
    of('Syn::Seethru', 'alpha_ref', 1, f'{CPP}:30', kind='alpha', cond='at_on && pass class see_through', block=f'{CPP}:20',
       kinds=['scene_see_through (generic)', 'scene_lit (generic)'], classes=['see_through']),
    of('Syn::Seethru', 'alpha_ref', 192, f'{CPP}:30', kind='alpha', cond='at_on && pass class crowd', block=f'{CPP}:20',
       kinds=['scene_see_through (generic)', 'scene_lit (generic)'], classes=['crowd']),
] + [
    blk('Syn::Scope', 'cull', 0, 13),
    of('Syn::Inst', 'blend_enable', 0, f'{INST}:3', kind='default', block=f'{INST}:3'),
    of('Syn::Inst', 'blend_enable', 1, f'{INST}:4', cond='tested && tb', block=f'{INST}:3'),
    of('Syn::Inst', 'alpha_test', {'caller': 'InstancedDraw::alpha_test', 'expr': 'd.alpha_test != 0'}, f'{INST}:6', kind='alpha',
       cond='!(d.alpha_test < 0)', block=f'{INST}:3'),
    of('Syn::Inst', 'alpha_func', 6, f'{INST}:6', kind='alpha', cond='tested', block=f'{INST}:3'),
] + [   # q2d::Append's shape: one block, each arm tagged with its own rstdiff kind (the in-scene lens row, the final-target fade row)
    of('Syn::Arms', 'z_enable', 1, f'{CPP}:42', block=f'{CPP}:40', kinds=['lens_element (in-scene row)']),
    of('Syn::Arms', 'z_enable', 0, f'{CPP}:40', kind='default', block=f'{CPP}:40', kinds=['fade (final-target row)']),
] + [   # S111 CDFOLLOW: LiveCrowd::Record's shape. One function, three InstancedDraw declarations (members 60, cards 65, a bake that
    # leaves alpha_test at -1, 70), each reaching the draw Syn::DrawI; a claim on the line above the cards' declaration names the
    # members' node (CARDCULL's claim on the card loop)
    of('Syn::DrawI', 'blend_enable', 0, f'{DR}:3', kind='default', block=f'{DR}:3'),
    of('Syn::DrawI', 'blend_enable', 1, f'{DR}:4', cond='tested && pass_state::Of(pcls).tested_blends', block=f'{DR}:3'),
    of('Syn::DrawI', 'alpha_test', {'caller': 'InstancedDraw::alpha_test', 'expr': 'd.alpha_test != 0'}, f'{DR}:5', kind='alpha',
       cond='!(d.alpha_test < 0)', block=f'{DR}:3'),
    of('Syn::DrawI', 'alpha_test', {'material': 'b.alpha_test'}, f'{DR}:5', kind='alpha', cond='d.alpha_test < 0', block=f'{DR}:3'),
    of('Syn::Crowd', 'alpha_test', 0, f'{CPP}:61', kind='caller', decl=f'{CPP}:60', calls='DrawI', store='InstancedDraw::alpha_test'),
    of('Syn::Crowd', 'alpha_test', 1, f'{CPP}:66', kind='caller', decl=f'{CPP}:65', calls='DrawI', store='InstancedDraw::alpha_test'),
    of('Syn::Crowd', 'alpha_test', {'material': 'Material::alpha_test (left at -1)'}, f'{CPP}:71', kind='caller', decl=f'{CPP}:70',
       calls='DrawI', left=True),
] + [   # S111 PARTZ2: a kTable row no draw reads (q ostate's `dead`) against the game function its addr cell names
    of('pass_state::kTable[dead]', 'z_func', 1, 'src/renderer_scene/pass_state.h:10', kind='table', game=['sub_83001500'],
       dead='PassClass::Dead: ClassOf never returns it and no file of ours names it'),
] + [   # a 2D kind's pair (rstdiff mirror False): the game's GEQUAL stays GEQUAL; ours GEQUAL
    of('Syn::Flat', 'z_func', 6, f'{CPP}:80', block=f'{CPP}:80', kinds=['sprite_2d (synthetic)']),
]
GAMEF = [
    gf(G['diff'], 'z_enable', 0, '0x83000010'),
    gf(G['units'], 'alpha_test', 1, '0x83000110'), gf(G['units'], 'alpha_func', 5, '0x83000114'), gf(G['units'], 'z_func', 2, '0x83000118'),
    gf(G['units'], 'cull', 1, '0x8300011C'), gf(G['units'], 'blend_enable', 1, '0x83000120'), gf(G['units'], 'blend_code', 1, '0x83000124'),
    gf(G['units'], 'z_write', {'restore': '0x83000104'}, '0x83000128', 'restore'),
    gf(G['moot'], 'z_enable', 0, '0x83000210'), gf(G['moot'], 'z_func', 1, '0x83000214'),
    gf(G['uns'], 'z_write', {'reg': 'in_r4'}, '0x83000310'), gf(G['uns'], 'alpha_test', 1, '0x83000314'), gf(G['uns'], 'alpha_ref', 1, '0x83000318'),
    gf(G['rec'], 'alpha_test', 1, '0x83000410'), gf(G['rec'], 'cull', 1, '0x83000414'), gf(G['rec'], 'z_enable', 1, '0x83000418'),
    gf(GP, 'z_func', 1, '0x82A0AF5C'),
    gf(G['six'], 'blend_enable', 1, '0x83000510'), gf(G['six'], 'blend_enable', 0, '0x83000514'),
    gf(G['leaf'], 'z_write', 1, '0x83000610'),
    gf(G['seven'], 'cull', 1, '0x83000710'), gf(G['other'], 'cull', 1, '0x83000810'),
    gf(G['marks'], 'colour_write_mask', 15, '0x83000910'), gf(G['marks'], 'stencil_enable', 1, '0x83000914'), gf(G['marks'], 'alpha_test', 1, '0x83000918'),
    gf(G['marks'], 'alpha_ref', 192, '0x8300091C'), gf(G['marks'], 'blend_enable', 0, '0x83000920'), gf(G['marks'], 'z_write', 1, '0x83000924'),
    gf('sub_83000A00', 'alpha_test', 1, '0x83000A10'), gf('sub_83000A00', 'alpha_func', 5, '0x83000A14'),
    gf('sub_83000B00', 'blend_enable', 1, '0x83000B10'), gf('sub_83000B00', 'blend_code', 0, '0x83000B14'),
    gf('sub_82BF5C58', 'blend_enable', 1, '0x82BF5D14'), gf('sub_82BF5C58', 'z_enable', 1, '0x82BF5D18'),
    gf('sub_8259FAE8', 'z_enable', 1, '0x8259FC38'), gf('sub_8259FAE8', 'blend_enable', 1, '0x8259FC44'), gf('sub_8259FAE8', 'blend_code', 0, '0x8259FC5C'),
    gf('sub_8259FAE8', 'alpha_ref', 1, '0x8259FCDC'),
    gf('sub_82BC4890', 'z_enable', 1, '0x82BC4900'),
    gf('sub_83000F00', 'cull', 1, '0x83000F10'), gf('sub_83001000', 'cull', 1, '0x83001010'),
    gf('sub_83000E00', 'blend_enable', 0, '0x83000E10'), gf('sub_83000E00', 'alpha_test', 1, '0x83000E14'), gf('sub_83000E00', 'alpha_func', 3, '0x83000E18'),
    gf('sub_83000C00', 'blend_enable', 0, '0x83000C10'), gf('sub_83000D00', 'blend_enable', 1, '0x83000D10'), gf('sub_83000D00', 'z_enable', 1, '0x83000D14'),
    gf('sub_83001300', 'alpha_test', 0, '0x83001310'), gf('sub_83001300', 'blend_enable', 0, '0x83001314'),
    gf('sub_83001400', 'alpha_test', 1, '0x83001410'), gf('sub_83001400', 'blend_enable', 1, '0x83001414'),
    gf(G2D, 'z_func', 2, f'0x{int(G2D[4:], 16) + 0x10:08X}'),
    gf('sub_83001500', 'z_func', 2, '0x83001510'),
]
PAIRS = {'Syn::Diff': [prow('Syn::Diff', G['diff'], 1)], 'Syn::Units': [prow('Syn::Units', G['units'], 1)], 'Syn::Moot': [prow('Syn::Moot', G['moot'], 1)],
         'Syn::Uns': [prow('Syn::Uns', G['uns'], 1)], 'Syn::Rec': [prow('Syn::Rec', G['rec'], 1, src='claim')],
         'Syn::Part': [prow('Syn::Part', GP, 1), prow('Syn::Part', G['six'], 1)],
         'Syn::Cite': [prow('Syn::Cite', G['leaf'], 11), prow('Syn::Cite', G['seven'], 1), prow('Syn::Cite', G['other'], 1)],
         'Syn::Marks': [prow('Syn::Marks', G['marks'], 1)],
         'Syn::Func7': [prow('Syn::Func7', 'sub_83000A00', 1)], 'Syn::Fdef': [prow('Syn::Fdef', 'sub_83000B00', 1)],
         'Syn::Seethru': [prow('Syn::Seethru', 'sub_8259FAE8', 5, src='claim'), prow('Syn::Seethru', 'sub_8259FAE8', 14),
                          prow('Syn::Seethru', 'sub_83000C00', 31), prow('Syn::Seethru', 'sub_83000D00', 23)],
         'Syn::Arms': [prow('Syn::Arms', 'sub_82BC4890', 50)],
         'Syn::Scope': [prow('Syn::Scope', 'sub_83000F00', 1), prow('Syn::Scope', 'sub_83001000', 1)],
         'Syn::Inst': [prow('Syn::Inst', 'sub_83000E00', 1, file=INST)],
         'Syn::Crowd': [prow('Syn::Crowd', 'sub_83001300', 61), prow('Syn::Crowd', 'sub_83001400', 66),
                        prow('Syn::Crowd', 'sub_83001300', 64, src='claim')],
         'Syn::Flat': [prow('Syn::Flat', G2D, 80, src='claim')]}
CALLERS = {G['leaf']: [G['seven']]}


def diff(plant=''):
    C.PLANT = plant
    try:
        marks = C.Marks(reader, lambda cvars: ['src/renderer_scene/syn_switch.h'])
        return C.run(OURS, GAMEF, PAIRS, C.Units(PS), marks, lambda g: CALLERS.get(g, []))
    finally:
        C.PLANT = ''


def row(rows, fn, game, field, block=BL):
    got = [r for r in rows if r['fn'] == fn and r['game'] == game and r['field'] == field and r['block'] == block]
    return got[0] if len(got) == 1 else None


def v(rows, fn, game, field, block=BL):
    r = row(rows, fn, game, field, block)
    return r['verdict'] if r else None


def refused(r, words):
    return bool(r) and r['verdict'] == 'DIFF' and words in ' '.join(r.get('departs_refused', ()))


def part_a():
    rs = diff()
    U = G['units']
    check('A DIFF: both set, values differ (z_enable 1 vs 0)', v(rs, 'Syn::Diff', G['diff'], 'z_enable') == 'DIFF', str(row(rs, 'Syn::Diff', G['diff'], 'z_enable')))
    check('A unit: alpha func code 5 = GEQUAL = Vk 6 (enums.json names)', v(rs, 'Syn::Units', U, 'alpha_func') == 'OK', str(row(rs, 'Syn::Units', U, 'alpha_func')))
    twin('A raw alpha codes: 5 against 6 turns DIFF', v(diff('raw_afunc'), 'Syn::Units', U, 'alpha_func') == 'DIFF')
    check('A unit: z func code 2 = GEQUAL -> ForwardZ -> LEQUAL 3', v(rs, 'Syn::Units', U, 'z_func') == 'OK', str(row(rs, 'Syn::Units', U, 'z_func')))
    twin('A no ForwardZ: the reversed compare turns DIFF', v(diff('no_forwardz'), 'Syn::Units', U, 'z_func') == 'DIFF')
    check("A unit: Vk cull 2 (back) = the game's cull on", v(rs, 'Syn::Units', U, 'cull') == 'OK', str(row(rs, 'Syn::Units', U, 'cull')))
    check('A unit: blend code 1 = ADD SRC_ALPHA ONE against our Vk 0 / 6 / 1',
          all(v(rs, 'Syn::Units', U, f) == 'OK' for f in C.DERIVED), str([row(rs, 'Syn::Units', U, f) for f in C.DERIVED]))
    check('A the game blend code beside our op / src / dst is no GAME-ONLY row', row(rs, 'Syn::Units', U, 'blend_code') is None)
    check('A no_blend is never compared', not [r for r in rs if r['field'] == 'no_blend'])
    check('A a restore is not a set (no z_write row)', row(rs, 'Syn::Units', U, 'z_write') is None)
    r = row(rs, 'Syn::Moot', G['moot'], 'z_func')
    check("A moot: z func with the game's z test off", r and r['verdict'] == 'OK' and 'moot' in r['why'], str(r))
    check('A UNSETTLED: a rule of ours', v(rs, 'Syn::Uns', G['uns'], 'alpha_ref') == 'UNSETTLED', str(row(rs, 'Syn::Uns', G['uns'], 'alpha_ref')))
    check('A UNSETTLED: a game value not a constant', v(rs, 'Syn::Uns', G['uns'], 'z_write') == 'UNSETTLED', str(row(rs, 'Syn::Uns', G['uns'], 'z_write')))
    check('A OURS-ONLY: the game function sets no stencil', v(rs, 'Syn::Uns', G['uns'], 'stencil_enable') == 'OURS-ONLY')
    R5 = G['rec']
    check("A a caller unit is compared only on the fields it states (alpha_test): no row for the game's cull (S111 CDFOLLOW)",
          row(rs, 'Syn::Rec', R5, 'cull', None) is None, str(row(rs, 'Syn::Rec', R5, 'cull', None)))
    check('A a caller-store function is not compared on z (no z_enable row)', row(rs, 'Syn::Rec', R5, 'z_enable', None) is None)
    r = row(rs, 'Syn::Rec', R5, 'alpha_test')
    check("A the caller's InstancedDraw field: the draw's alpha test is the caller's (OK, deferred)", r and r['verdict'] == 'OK' and 'caller' in r['why'], str(r))
    twin('A no caller deferral: the material arm turns DIFF', v(diff('no_defer'), 'Syn::Rec', R5, 'alpha_test') == 'DIFF')
    check('A a game set on one path only (an rstdiff adds window): ours LEQUAL beside ALWAYS is UNSETTLED', v(rs, 'Syn::Part', GP, 'z_func') == 'UNSETTLED',
          str(row(rs, 'Syn::Part', GP, 'z_func')))
    check('A a game function of several values (blend 1 and 0), ours one of them: UNSETTLED', v(rs, 'Syn::Part', G['six'], 'blend_enable') == 'UNSETTLED')
    twin('A no UNSETTLED for several values: turns DIFF', v(diff('no_unsettled'), 'Syn::Part', G['six'], 'blend_enable') == 'DIFF')
    check("A a citation on the fact's line: the cull stands against the cited leaf's caller (its nearest setter), not the other pair",
          v(rs, 'Syn::Cite', G['seven'], 'cull') == 'OK' and v(rs, 'Syn::Cite', G['other'], 'cull') == 'GAME-ONLY',
          str([r for r in rs if r['fn'] == 'Syn::Cite' and r['field'] == 'cull']))
    twin('A no pair scope: the cited cull is compared with the other pair too', v(diff('no_scope'), 'Syn::Cite', G['other'], 'cull') == 'OK')
    check('A our func 7 (ALWAYS) is the untested arm, not a func: GEQUAL beside it is OK', v(rs, 'Syn::Func7', 'sub_83000A00', 'alpha_func') == 'OK',
          str(row(rs, 'Syn::Func7', 'sub_83000A00', 'alpha_func')))
    twin('A func 7 compared as a func: turns DIFF', v(diff('always_func'), 'Syn::Func7', 'sub_83000A00', 'alpha_func') == 'DIFF')
    check("A the DrawState zero's factors go with its blend-off row (blend_src OK, blend_enable carries the 0)",
          v(rs, 'Syn::Fdef', 'sub_83000B00', 'blend_src') == 'OK' and v(rs, 'Syn::Fdef', 'sub_83000B00', 'blend_enable') == 'DIFF',
          str([r for r in rs if r['fn'] == 'Syn::Fdef']))
    twin('A the zero factors compared: blend_src turns DIFF', v(diff('factor_defaults'), 'Syn::Fdef', 'sub_83000B00', 'blend_src') == 'DIFF')
    check("A an rstdiff kind that narrows its fields (scene_lit: z test / func) pairs for those only (z_enable OK, no blend row)",
          v(rs, 'Syn::Kind', 'sub_82BF5C58', 'z_enable') == 'OK' and row(rs, 'Syn::Kind', 'sub_82BF5C58', 'blend_enable') is None,
          str([r for r in rs if r['fn'] == 'Syn::Kind']))
    twin("A the kind's fields not honoured: the blend turns DIFF", v(diff('kind_fields'), 'Syn::Kind', 'sub_82BF5C58', 'blend_enable') == 'DIFF')
    SB = f'{CPP}:20'
    st = [r for r in rs if r['fn'] == 'Syn::Seethru']
    check("A one block of every family: citations elsewhere in the function do not pair its blend arms with the see-through node (its z "
          "and its see-through class arm only, OK)",
          v(rs, 'Syn::Seethru', 'sub_8259FAE8', 'z_enable', SB) == 'OK' and v(rs, 'Syn::Seethru', 'sub_8259FAE8', 'alpha_ref', SB) == 'OK'
          and not [r for r in st if r['game'] == 'sub_8259FAE8' and r['field'] not in ('z_enable', 'alpha_ref')],
          str([(r['game'], r['field'], r['verdict'], r['pair']) for r in st]))
    twin("A the class arm not paired with its family's node: no alpha_ref row",
         row(diff('no_class_arm'), 'Syn::Seethru', 'sub_8259FAE8', 'alpha_ref', SB) is None)
    check("A a citation by a binding line, not by a state fact (the screen grab's), pairs nothing", not [r for r in st if r['game'] == 'sub_83000C00'],
          str([(r['game'], r['field'], r['verdict']) for r in st if r['game'] == 'sub_83000C00']))
    check("A a citation on the line above a fact pairs that fact's field only (blend_enable, no z_enable row)",
          row(rs, 'Syn::Seethru', 'sub_83000D00', 'blend_enable', SB) is not None and row(rs, 'Syn::Seethru', 'sub_83000D00', 'z_enable', SB) is None,
          str([(r['game'], r['field'], r['verdict']) for r in st if r['game'] == 'sub_83000D00']))
    nf = getattr(C, 'new_failing', None)
    now = [{'fn': 'F', 'game': 'sub_83001100', 'field': 'cull', 'verdict': 'DIFF'}, {'fn': 'F', 'game': 'sub_83001200', 'field': 'cull', 'verdict': 'DIFF'},
           {'fn': 'F', 'game': 'sub_83001200', 'field': 'z_write', 'verdict': 'OK'}]
    was = [{'fn': 'F', 'game': 'sub_83001100', 'field': 'cull', 'verdict': 'DIFF'}]
    check('A check row, a db/claims / db/seams edit: only the failing rows the base does not fail (a new pair from the db edit)',
          nf is not None and [r['game'] for r in nf(now, was)] == ['sub_83001200'], 'qcodediff.new_failing missing' if nf is None else str(nf(now, was)))
    C.PLANT = 'db_all_fail'
    try:
        twin('A every failing row of the tree on a db edit (not only the new ones): 2 rows', nf is not None and len(nf(now, was)) == 2)
    finally:
        C.PLANT = ''
    IB = f'{INST}:3'
    r = row(rs, 'Syn::Inst', 'sub_83000E00', 'blend_enable', IB)
    check("A a blend arm on a local that is the caller's InstancedDraw::alpha_test (`tested`) is the caller's: OK beside the game's blend off",
          r and r['verdict'] == 'OK' and 'caller' in r['why'], str(r))
    r = row(rs, 'Syn::Inst', 'sub_83000E00', 'alpha_func', IB)
    check("A the alpha func under the same local keeps its value (its gate is the alpha_test row): GEQUAL against the game's LEQUAL is a DIFF",
          r and r['verdict'] == 'DIFF', str(r))
    twin("A the local not resolved: the caller's blend arm turns DIFF", v(diff('no_local_defer'), 'Syn::Inst', 'sub_83000E00', 'blend_enable', IB) == 'DIFF')
    AB = f'{CPP}:40'
    check("A a block's arm tagged with its own kind stands against that kind's function only (the fade arm's z off is not the lens "
          "row's); one kind per fact keeps a citation away from the block (block_sources)",
          v(rs, 'Syn::Arms', 'sub_82BC4890', 'z_enable', AB) == 'OK', str(row(rs, 'Syn::Arms', 'sub_82BC4890', 'z_enable', AB)))
    twin("A every arm against every kind's function: the lens row's z test turns UNSETTLED / DIFF",
         v(diff('kind_any_arm'), 'Syn::Arms', 'sub_82BC4890', 'z_enable', AB) in ('DIFF', 'UNSETTLED'))
    tw = [r for r in diff('cite_anywhere') if r['fn'] == 'Syn::Seethru' and r['game'] == 'sub_8259FAE8']
    twin("A a citation anywhere pairs the whole block: the see-through node's blend turns DIFF",
         any(r['field'] in ('blend_enable', 'blend_dst') and r['verdict'] == 'DIFF' for r in tw))
    M = G['marks']
    r = row(rs, 'Syn::Marks', M, 'colour_write_mask')
    check('A DEPARTS honoured (habit 360_HABITS:15, on the line above): DEPARTED', r and r['verdict'] == 'DEPARTED' and r['departs'][0]['ref'] == '15', str(r))
    check('A a thin DEPARTS reason fires (stays DIFF)', refused(row(rs, 'Syn::Marks', M, 'stencil_enable'), 'under 12'), str(row(rs, 'Syn::Marks', M, 'stencil_enable')))
    check('A a missing row ref fires (GAME_BUG_FIXES:GBF-99)', refused(row(rs, 'Syn::Marks', M, 'alpha_ref'), 'no such row'), str(row(rs, 'Syn::Marks', M, 'alpha_ref')))
    twin('A the row ref not looked up: GBF-99 accepted', v(diff('any_ref'), 'Syn::Marks', M, 'alpha_ref') == 'DEPARTED')
    check('A a habit naming the bug file fires', refused(row(rs, 'Syn::Marks', M, 'blend_enable'), '360_HABITS row'), str(row(rs, 'Syn::Marks', M, 'blend_enable')))
    check('A a mod departure outside its switch fires', refused(row(rs, 'Syn::Marks', M, 'z_write'), "mod's switch"), str(row(rs, 'Syn::Marks', M, 'z_write')))
    twin('A a mod accepted anywhere: the unswitched one DEPARTED', v(diff('mod_anywhere'), 'Syn::Marks', M, 'z_write') == 'DEPARTED')
    r = row(rs, 'Syn::Rec', R5, 'alpha_test', None)
    check('A a mod behind its switch (On(), defined beside the mod cvar) is DEPARTED', r and r['verdict'] == 'DEPARTED', str(r))
    m = C.Marks(reader)
    check('A scan: every bad marker of a file is named (a scope that is not sub_XXXXXXXX too)', sorted(ln for ln, _w in m.scan(CPP)) == [6, 7, 8, 14],
          str(m.scan(CPP)))
    r1, r2 = row(rs, 'Syn::Scope', 'sub_83000F00', 'cull'), row(rs, 'Syn::Scope', 'sub_83001000', 'cull')
    check('A a scoped DEPARTS (@sub_83000F00) is honoured on its pair', r1 and r1['verdict'] == 'DEPARTED' and r1['departs'][0].get('games') == ['sub_83000F00'], str(r1))
    check('A ... and not on another pair of the same block (stays DIFF, says the scope)', refused(r2, 'scoped to sub_83000F00'), str(r2))
    twin('A the scope ignored: the other pair turns DEPARTED', v(diff('no_marker_scope'), 'Syn::Scope', 'sub_83001000', 'cull') == 'DEPARTED')


def part_cdfollow():
    """S111 CDFOLLOW: caller units per InstancedDraw declaration, the draw fields a caller's alpha_test gates, the -1 callers said,
    the z mirror per kind from the game's own tables"""
    rs = diff()
    M, K, L = f'{CPP}:60', f'{CPP}:65', f'{CPP}:70'
    check('D members unit vs its node: alpha_test OK and the gated blend OK (0, the DrawState zero)',
          v(rs, 'Syn::Crowd', 'sub_83001300', 'alpha_test', M) == 'OK' and v(rs, 'Syn::Crowd', 'sub_83001300', 'blend_enable', M) == 'OK',
          str([r for r in rs if r['fn'] == 'Syn::Crowd' and r['block'] == M]))
    r = row(rs, 'Syn::Crowd', 'sub_83001400', 'blend_enable', K)
    check("D cards unit vs its own citation's node: the blend its alpha_test 1 turns on (DrawI's `tested` arm) is compared: OK",
          v(rs, 'Syn::Crowd', 'sub_83001400', 'alpha_test', K) == 'OK' and r and r['verdict'] == 'OK' and r['ours'][0]['value'] == '1', str(r))
    twin('D the gate not read: no blend row for the cards', row(diff('no_gate'), 'Syn::Crowd', 'sub_83001400', 'blend_enable', K) is None)
    check("D the cards unit has no row against the members' node (the claim beside it pairs it, its facts stand against their own citation)",
          not [r for r in rs if r['fn'] == 'Syn::Crowd' and r['block'] == K and r['game'] == 'sub_83001300'],
          str([(r['game'], r['field'], r['verdict']) for r in rs if r['fn'] == 'Syn::Crowd' and r['block'] == K]))
    tw = [r for r in diff('one_unit') if r['fn'] == 'Syn::Crowd' and r['game'] == 'sub_83001400' and r['field'] == 'alpha_test']
    twin('D one unit per function: the -1 bake stands against the cards node (alpha_test not OK)', bool(tw) and tw[0]['verdict'] != 'OK', str(tw))
    lr = [r for r in rs if r['fn'] == 'Syn::Crowd' and r['block'] == L]
    check('D a unit that leaves alpha_test at -1, no citation by it: UNSETTLED rows (alpha_test, the gated blend) with the reason, no pair',
          {r['field'] for r in lr} == {'alpha_test', 'blend_enable'} and all(r['verdict'] == 'UNSETTLED' and r['game'] == C.NO_PAIR and 'at -1' in r['why'] for r in lr), str(lr))
    twin('D the -1 unit skipped: no row', not [r for r in diff('left_skip') if r['fn'] == 'Syn::Crowd' and r['block'] == L])
    FB = f'{CPP}:80'
    check("D a 2D kind's pair (mirror False): the game's GEQUAL is not mirrored: OK against ours GEQUAL", v(rs, 'Syn::Flat', G2D, 'z_func', FB) == 'OK',
          str(row(rs, 'Syn::Flat', G2D, 'z_func', FB)))
    twin("D every pair mirrored: the 2D GEQUAL turns DIFF", v(diff('mirror_all'), 'Syn::Flat', G2D, 'z_func', FB) == 'DIFF')
    check('D the game z goes through its own tables (ZFUNC_CODE, MIRROR), not our ForwardZ: a fault in ours (6 -> 1) leaves code 2 = LEQUAL',
          C.Units(FAULTY_PS).game('z_func', 2) == [('z_func', 3)], str(C.Units(FAULTY_PS).game('z_func', 2)))
    C.PLANT = 'game_forwardz'
    try:
        twin('D the game side through our ForwardZ: the fault is on both sides (LESS)', C.Units(FAULTY_PS).game('z_func', 2) == [('z_func', 1)])
    finally:
        C.PLANT = ''
    r = row(rs, 'pass_state::kTable[dead]', 'sub_83001500', 'z_func', None)
    check('D a kTable row no draw reads: moot, with the reason (not a DIFF)', r and r['verdict'] == 'OK' and 'no draw reads' in r['why'], str(r))
    twin('D the dead row compared: LESS against LEQUAL turns DIFF', v(diff('dead_rows'), 'pass_state::kTable[dead]', 'sub_83001500', 'z_func', None) == 'DIFF')
    m = C.Marks(reader)
    rows_ = {'Syn::Mk': [prow('Syn::Mk', 'sub_83000F00', 12)]}
    p, _c = C.pairs_of([of('Syn::Mk', 'cull', 0, f'{CPP}:13', block=BL)], rows_, {'sub_83000F00'}, {}, m.lines)
    check("D a DEPARTS marker's own line (its @sub_X scope) is no citation", not p.get('Syn::Mk'), str(dict(p)))
    C.PLANT = 'marker_cites'
    try:
        p2, _c = C.pairs_of([of('Syn::Mk', 'cull', 0, f'{CPP}:13', block=BL)], rows_, {'sub_83000F00'}, {}, m.lines)
        twin('D the marker line read as a citation: it pairs', bool(p2.get('Syn::Mk')))
    finally:
        C.PLANT = ''


ARM_CPP = 'src/renderer_scene/syn_bind.cpp'
ARM_SRC = '\n'.join([
    'namespace rdr_scene {',                                                                                   # 1
    'const Bound& FE::BindArm(const Mat& mat, bool chr, int ps) {',                                            # 2
    '  b->debug = false;',                                                                                     # 3
    '  auto b = std::make_unique<Bound>();',                                                                   # 4 (a claim: the model draw)
    '  if (chr) {',                                                                                            # 5
    '    b->chr = true;',                                                                                      # 6
    '    // the colour pass: the translucent node sub_83002000 tests GEQUAL 1, the opaque node sub_83002200 sets none; the map',   # 7
    '    // pass sub_83002100 turns the test off (this bind serves the colour pass only)',                    # 8
    '    b->alpha_test = VariantHasTwin(ps);',                                                                 # 9
    '    b->slot = Mask();   // the mask slot of sub_83002300 (a sampler rule)',                              # 10
    '    return *b;',                                                                                          # 11
    '  }',                                                                                                     # 12
    '  b->alpha_test = mat.alpha_test;',                                                                       # 13
    '  return *b;',                                                                                            # 14
    '}',                                                                                                       # 15
    'void FE::DrawArm(const Mat& mat, rdr::FrameBuilder& fb, const rdr::DynamicState& dyn) {',                 # 16
    '  const Bound& b = BindArm(mat, true, 3);',                                                               # 17
    '  rdr::DrawState state{};',                                                                               # 18
    '  state.depth_test = 1;',                                                                                 # 19
    '  op.state = fb.State(state);',                                                                           # 20
    '  op.dynamic = fb.Dynamic(b.alpha_test ? AlphaDynamic(dyn, 0) : dyn);',                                   # 21
    '}',                                                                                                       # 22
    '}', ''])
ARM_GAME = [gf('sub_83002000', 'alpha_test', 1, '0x83002010'), gf('sub_83002100', 'alpha_test', 0, '0x83002110'),
            gf('sub_83002200', 'z_enable', 1, '0x83002210'), gf('sub_83002300', 'alpha_test', 0, '0x83002310')]
ARM_PASSES = {'sub_83002100': 'T_ShadowMap', 'sub_83002200': ''}


def arm_rows(plant='', wide_plant=''):
    files = {ARM_CPP: ARM_SRC, C.O.PS: PS}

    def read(rel):
        if rel in files:
            return files[rel]
        raise C.O.OstateError(f'{rel}: not planted')
    os.environ['WWE_QOSTATEWIDE_PLANT'] = wide_plant
    try:
        facts = C.O.collect(read=read, files=[ARM_CPP])[0]
    finally:
        os.environ['WWE_QOSTATEWIDE_PLANT'] = ''
    fn = next(f['fn'] for f in facts if f['fn'].endswith('BindArm'))
    pr = {fn: [prow(fn, 'sub_83002000', 7, file=ARM_CPP), prow(fn, 'sub_83002200', 7, file=ARM_CPP), prow(fn, 'sub_83002100', 8, file=ARM_CPP),
               prow(fn, 'sub_83002300', 10, file=ARM_CPP), prow(fn, 'sub_83002200', 4, src='claim', file=ARM_CPP)]}
    C.PLANT = plant
    try:
        rows = C.run(facts, ARM_GAME, pr, C.Units(PS), C.Marks(read), lambda g: [], passes=ARM_PASSES)
    finally:
        C.PLANT = ''
    return [r for r in rows if r['fn'] == fn]


def part_arms():
    rs = arm_rows()
    got = sorted((r['block'], r['game'], r['field'], r['verdict']) for r in rs)
    want = [(f'{ARM_CPP}:5', 'sub_83002000', 'alpha_test', 'UNSETTLED')]
    check('C the wrestler arm: only against the node that sets the field (UNSETTLED, a rule of ours); the map pass, the opaque node, the '
          'sampler citation and the claim outside the arms none; the material arm unpaired', got == want, str(got))
    tw = arm_rows('one_arm')
    twin('C one unit per function (one_arm): the material flag stands against a citation of the wrestler arm',
         any(r['game'] == 'sub_83002300' for r in tw) or any('mat.alpha_test' in o['value'] for r in tw for o in r['ours']), str([(r['game'], r['verdict']) for r in tw]))
    tw = arm_rows('no_scope_words')
    twin('C the scope words not read: a row against the map pass sub_83002100', any(r['game'] == 'sub_83002100' for r in tw), str([(r['game'], r['verdict']) for r in tw]))
    tw = arm_rows('arm_anywhere')
    twin('C an arm takes every citation: the material arm is paired', any(r['block'] == f'{ARM_CPP}:2' for r in tw), str([(r['block'], r['game']) for r in tw]))
    tw = arm_rows(wide_plant='no_arm')
    twin("C q ostate writes no arm (OSTATEWIDE's one unit): rows against the map pass or the sampler come back",
         any(r['game'] in ('sub_83002100', 'sub_83002300') for r in tw), str([(r['game'], r['verdict']) for r in tw]))


def part_b():
    try:
        for ref in (PARENT, FIX, PARTZ):
            subprocess.run(['git', 'cat-file', '-e', f'{ref}^{{commit}}'], cwd=C.REPO, check=True, capture_output=True)
    except Exception as e:  # noqa: BLE001 - no history here
        print(f'SKIP B (no frozen refs: {e})')
        return

    def pick(rows, fn, game, field):
        return [r for r in rows if r['fn'].endswith(fn) and r['game'] == game and r['field'] == field]
    tip = C.collect(None, rank=False)[0]
    bm = sorted((r['game'], r['field'], r['verdict']) for r in tip if r['fn'] == 'rdr_scene::FrontEnd::BindMaterial')
    check('B3 tip (CDARMS): BindMaterial\'s wrestler colour arm vs the translucent node only (UNSETTLED); no shadow / rim pass, no node '
          'that sets no alpha test, no mask sampler row', bm == [('sub_8259FAE8', 'alpha_test', 'UNSETTLED')], str(bm))
    par = C.collect(PARENT, rank=False)[0]
    p = pick(par, 'FrontEnd::DrawInstanced', 'sub_82519508', 'alpha_test')
    check('B1 parent: the crowd member draw vs sub_82519508 alpha_test is a DIFF (game 0, ours the material flag)',
          len(p) == 1 and p[0]['verdict'] == 'DIFF' and p[0]['game_vals'][0]['value'] == '0' and 'material' in p[0]['ours'][0]['value'], str(p))
    t = pick(tip, 'FrontEnd::DrawInstanced', 'sub_82519508', 'alpha_test')
    check('B1 tip: not flagged (the caller states it)', t and all(r['verdict'] not in C.FAILING for r in t), str(t))
    pz = C.collect(PARTZ, rank=False)[0]
    z = pick(pz, 'LiveParticles::Record', 'sub_82A0AED8', 'z_func')
    check('B2 b2a29d266: the Kane flare z func (particles vs sub_82A0AED8) is a DIFF: the game ALWAYS, ours LEQUAL',
          len(z) == 1 and z[0]['verdict'] == 'DIFF' and 'ALWAYS (ours never)' in z[0]['why'], str(z))
    t = [r for r in tip if 'LiveParticles' in r['fn'] and r['game'] == 'sub_82A0AED8' and r['field'] == 'z_func']
    check('B2 tip: not a DIFF (ours draws ALWAYS for those rows)', t and all(r['verdict'] not in C.FAILING for r in t), str(t))


def main():
    part_a()
    part_cdfollow()
    part_arms()
    part_b()
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

r"""Self-test of `q draworder` (S111 DRAWORDER): `py -I tools/q/qdraworder_selftest.py`   (0 game runs)

A. Planted orders: an invented render_categories table and invented sources (FrontEnd::Record's stages and loops, body_order.h's turns,
   rf::Classes and kCRows, SeeRank, a hook gate, AttachFrameHooks), through the production join qdraworder.run with a dict reader:
   an opaque-opaque swap (ring 24 / seats 27, both opaque with depth write) is OK whatever the order; a blended-vs-opaque swap (the faded
   body 13 drawn after the ring 24) is a DIFF, and OK once Record calls OrderOpaque (the Body turn ahead of After); the blended list's
   rank from SeeRank; a hook's stage from its gate; the blended loop recorded before the opaque one flips a blended / opaque pair; a
   planted twin's arm (`if (g_plant_...) DrawFlashesAfterPost`) is not our order; two families of one category with no stated position,
   a family with no doc row and a state phrase the doc lacks are UNSETTLED (the phrase also listed as a problem); an opaque pair with a
   [U] game order is OK.
   Planted twins, each seen to FAIL its check: any_swap (every opposite order flagged: the opaque-opaque swap turns DIFF), opaque_only
   (the state rule off: the blended-vs-opaque swap turns OK).
A2. (S111 DRAWORDER2) the entrance scene, the gm category-36 family and the late-additive family on planted sources: SeeRank in its real form
   (the LateAdditive helper line, the planted twin's arm skipped); a rank line it cannot read is a PROBLEM (twin skip_unread: the parser before,
   which skipped it silently); `--scene entrance` flips DRTR (33 -> 43 by the doc's re-key row) against a category-36 glass from OK to DIFF
   (twin entrance_off); a re-keyed body is the blended list's character rank only when Record calls TakeLateBodies and StampItems marks it;
   GmNodeObjectFlags without kObjNodeLate drops the gm family to rank 0 (DIFF against the DRTR it draws after).
B. Real re-find (our code at frozen refs; SKIP without them): FADEORDER at ada3abd50^1 (the bodies after the ring / arena / props while a
   faded body blends): bodies vs ring, arena and props are DIFF; at the tip (the files on disk) they are OK; no anchor lost at either.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdraworder as D   # noqa: E402

FAILS, CAUGHT = [], []
FADE_PARENT = 'ada3abd50^1'


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


HEAD = ('| Cat | What registers there (builder / slot 1) | Reset | State its nodes set or inherit | Passes | Re-keys | Ours | Marks |\n'
        '|---|---|---|---|---|---|---|---|\n')
PASSES = '| Pass | Function | Categories it walks (in order) | State it sets around them | Mark |\n|---|---|---|---|---|\n'


def doc(extra=''):
    return (PASSES + '\n' + HEAD
            + '| 13..21 | each body node | R | bodies draw tested + blended | C | - | - | [C] |\n'
            + '| 24 | ring node | R | ring: blend off, alpha test off | C | - | - | [C] |\n'
            + '| 27 | seats node; bench node | R | seats: blend off, z write 1 | C | - | - | [C] |\n'
            + '| 30 | glass node | R | glass: blend on | C | - | - | [C] |\n'
            + '| 47 | parts node | R | parts: blend code | C | - | - | [C] |\n' + extra)


def fams(extra=()):
    return [D.Fam('bodies', r'each body node', 'blend', r'tested \+ blended', ('opaque', ('wrestlers',), ('char_slot', 'char_effect'))),
            D.Fam('ring', r'ring node', 'opaque', r'blend off, alpha test off', ('opaque', ('ring',), None)),
            D.Fam('seats', r'seats node', 'opaque', r'seats: blend off', ('opaque', ('seats',), None)),
            D.Fam('glass', r'glass node', 'blend', r'glass: blend on', ('blended', ('ring',), frozenset())),
            D.Fam('parts', r'parts node', 'blend', r'parts: blend code', ('hook', 'particles'), 'blend')] + list(extra)


def record(order_opaque=False, blended_first=False, plant_flash=False):
    loops = ['  for (const Item& it : opaque) emit(it);', '  run_stage(Stage::AfterOpaque);', '  for (const Item& it : blended) emit(it);']
    if blended_first:
        loops = [loops[2], loops[1], loops[0]]
    return '\n'.join(['bool FrontEnd::Record(const SceneFrame& frame) {',
                      '  const auto run_stage = [&](Stage s) { };',
                      '  run_stage(Stage::Maps);', '  run_stage(Stage::BeforeOpaque);',
                      '  body_order::OrderOpaque(opaque, [&](const Item& i) { return 0u; });' if order_opaque else '  // body_order::OrderOpaque(opaque  (a comment: not a call)',
                      *loops, '  run_stage(Stage::AfterBlended);',
                      '  if (g_plant_flash.load()) DrawFlashesAfterPost(frame, fb);' if plant_flash else '',
                      '  run_stage(Stage::BeforePost);', '  if (!post_pass_->Record(fb)) return false;',
                      '  if (!g_plant_flash.load()) DrawFlashesAfterPost(frame, fb);', '  run_stage(Stage::AfterPost);', '}', ''])


BODY_ORDER = ('enum class Turn : uint8_t { BeforeBodies = 0, Body = 1, After = 2 };\n'
              'inline constexpr Turn TurnOf(bool character_mesh, bool before_bodies) { return character_mesh ? Turn::Body : before_bodies ? Turn::BeforeBodies : Turn::After; }\n')
# S111 FADECAW's form: the Body turn by object class (BodyObject's real arm `char_slot >= 0`; the effect test only the planted twin's arm)
BODY_ORDER_CLASS = ('enum class Turn : uint8_t { BeforeBodies = 0, Body = 1, After = 2 };\n'
                    'inline bool g_plant_effect_body = false;\n'
                    'inline bool BodyObject(int char_slot, bool char_effect) { return g_plant_effect_body ? char_effect : char_slot >= 0; }\n'
                    'inline constexpr Turn TurnOf(bool body, bool before_bodies) { return body ? Turn::Body : before_bodies ? Turn::BeforeBodies : Turn::After; }\n')
CLASSES = ('const std::vector<ClassEntry>& Classes() {\n  static const std::vector<ClassEntry> table = {\n'
           '      c::kCRows[1],   // seats first (a comment naming {"x", Family::Y} is not a row)\n      c::kCRows[0],\n'
           '      {"wrestlers", Family::Wrestlers, &ApplyWrestlers},\n  };\n  return table;\n}\n')
CROWS = 'const ClassEntry kCRows[2] = {\n    {"ring", Family::Ring, &ApplyRing},\n    {"seats", Family::Seats, &ApplySeats},\n};\n'
SEE = ('inline int SeeRank(const SeeKey& k) {\n  if (k.place_block && !g_plant.load()) return 1;\n  if (k.blend == 2) return 3;\n  return k.character ? 1 : 0;\n}\n'
       'inline bool SeeThroughBefore(SeeOrder mode, const SeeKey& a, const SeeKey& b) {\n  return false;\n}\n')
ATTACH = 'void AttachFrameHooks(SceneFrame& frame, const SceneTables& rec, FrameHooks& h) {\n  frame.stage_hooks.push_back(h.particles_.get());\n}\n'
# SeeRank in its real form (S111 CHAMBERGLASS): the LateAdditive helper, the planted twin's arms
SEE2 = ('inline bool LateAdditive(const SeeKey& k) { return k.blend == 2 && k.late_node && !g_plant_late_node_off.load(std::memory_order_relaxed); }\n'
        'inline int SeeRank(const SeeKey& k) {\n'
        '  if (k.place_block && !g_plant_draw_place_off.load(std::memory_order_relaxed)) return 1;   // a comment\n'
        '  if (g_plant_late_before_additive.load(std::memory_order_relaxed)) return SeeRankBeforeChamberGlass(k);\n'
        '  if (LateAdditive(k)) return 4;\n'
        '  if (k.blend == 2) return 2;\n'
        '  if (k.late_node && !g_plant_late_node_off.load(std::memory_order_relaxed)) return 3;\n'
        '  return k.character ? 1 : 0;\n}\n'
        'inline bool SeeThroughBefore(SeeOrder mode, const SeeKey& a, const SeeKey& b) {\n  return false;\n}\n')
PLACE_ROWS = '    items[i].place_block = p && (p->block || late);\n'
# S111 REKEYORDER2: the re-keyed rank in its real form (an `||` group and the place term in the Rekeyed helper), and SeeThroughBefore ordering
# ranks 1 and 5 by place (the rank named by its constant)
SEE3 = (SEE2.replace('inline int SeeRank(const SeeKey& k) {\n',
                     'inline constexpr int32_t kPlaceAfterLateNodes = int32_t(scene_inputs::char_draw_list::kMaxNodes);\n'
                     'inline constexpr int kRankRekeyed = 5;   // a comment\n'
                     'inline bool Rekeyed(const SeeKey& k) { return (k.place_block || k.character) && k.place >= kPlaceAfterLateNodes'
                     ' && !g_plant_rekeyed_rank_off.load(std::memory_order_relaxed) && !g_plant_draw_place_off.load(std::memory_order_relaxed); }\n'
                     'inline int SeeRank(const SeeKey& k) {\n  if (Rekeyed(k)) return 5;   // a comment\n')
        .replace('inline bool SeeThroughBefore(SeeOrder mode, const SeeKey& a, const SeeKey& b) {\n  return false;\n}',
                 'inline bool SeeThroughBefore(SeeOrder mode, const SeeKey& a, const SeeKey& b) {\n  const int ra = SeeRank(a), rb = SeeRank(b);\n'
                 '  if ((ra == 1 || ra == kRankRekeyed) && !sa && a.place != b.place && !g_plant_draw_place_off.load(std::memory_order_relaxed)) return a.place < b.place;\n'
                 '  return false;\n}'))
DRAW_LISTS_FILE = 'src/scene_live/char_draw_list.h'
LISTS = ('inline constexpr uint32_t kMaxCats = 128, kMaxNodes = 256;\n'
         'inline constexpr uint32_t kDrawLists[3] = {kCharTranslucentCat, 41, 43};   // a comment\n')
GM_STATE = ('inline uint32_t GmNodeObjectFlags(uint32_t key, float mdl_alpha, bool plant_old = false) {\n  if (plant_old) return 0u;\n'
            '  return splat_state::NodeObjectFlags(false) | uint32_t(kObjNodeLate);\n}\n')
REKEY = ('\n| Move | Function | What moves | Mark |\n|---|---|---|---|\n'
         "| entrance re-key | `sub_827FBFE0` | each slot character's opaque node 13 + slot -> 42, its DRTR 33 -> 43; each prop's own node 26 -> 41 (43 for ids 1), its CWDT and WATR 33 -> 43 | [C] |\n")


def reader(**over):
    files = {D.RECORD: record(), D.BODY_ORDER: BODY_ORDER, D.CLASSES: CLASSES, D.CROWS: CROWS, D.SEE: SEE, D.ATTACH: ATTACH,
             D.PLACE_ROWS: PLACE_ROWS, D.GM_STATE: GM_STATE,
             'src/renderer_scene/live_particles.cpp': 'void LiveParticles::Record(Stage stage) {\n  if (stage != Stage::AfterBlended || !ctx.frame) return;\n}\n',
             'src/renderer_scene/live_crowd.cpp': '  if (stage != Stage::AfterOpaque || !t) return;\n',
             'src/renderer_scene/frame_hooks.h': '    if (stage == Stage::BeforeOpaque && !early_.empty()) a();\n    else if (stage == Stage::AfterBlended && !rows_.empty()) b();\n'}
    files.update(over)
    return files.get


def row(res, a, b):
    for r in res['rows']:
        if {r['a'], r['b']} == {a, b}:
            return r
    return None


def v(res, a, b):
    r = row(res, a, b)
    return r['order'] if r else None


def part_a():
    base = D.run(reader(), doc(), fams())
    check('A0 no problems on the planted doc and sources', base['problems'] == [], base['problems'])
    m = base['model']
    check('A0 Record phases in code order (the lambda line and a commented OrderOpaque are not phases)',
          m['phases'] == ['Maps', 'BeforeOpaque', 'OPAQUE', 'AfterOpaque', 'BLENDED', 'AfterBlended', 'BeforePost', 'POST', 'FLASHES', 'AfterPost']
          and not m['order_opaque'], m['phases'])
    check('A0 Classes record order through kCRows names', m['classes'] == ['seats', 'ring', 'wrestlers'], m['classes'])
    check('A0 SeeRank read', m['ranks'] == [('place_block', 1), ('blend2', 3)] and m['rank_else'] == ('character', 1, 0), (m['ranks'], m['rank_else']))
    # 1. the opaque-opaque swap: game ring (24) before seats (27), ours seats' row first: no pixel can change -> OK
    r = row(base, 'ring', 'seats')
    check('A1 opaque-opaque swap is OK', r['order'] == 'OK' and r['game'] == -1 and r['ours'] == 1, r)
    tw = D.run(reader(), doc(), fams(), plant='any_swap')
    twin('any_swap (every opposite order a DIFF)', v(tw, 'ring', 'seats') != 'OK', v(tw, 'ring', 'seats'))
    # 2. the blended-vs-opaque swap: the body (13, blended) recorded after the ring (24) -> DIFF
    r = row(base, 'bodies', 'ring')
    check('A2 blended-vs-opaque swap is a DIFF', r['order'] == 'DIFF' and r['game'] == -1 and r['ours'] == 1, r)
    tw = D.run(reader(), doc(), fams(), plant='opaque_only')
    twin('opaque_only (the state rule off)', v(tw, 'bodies', 'ring') != 'DIFF', v(tw, 'bodies', 'ring'))
    # 3. OrderOpaque: the Body turn ahead of After
    oo = D.run(reader(**{D.RECORD: record(order_opaque=True)}), doc(), fams())
    check('A3 OrderOpaque puts the body turn first: OK', oo['model']['order_opaque'] and v(oo, 'bodies', 'ring') == 'OK' and v(oo, 'bodies', 'seats') == 'OK',
          (oo['model']['turns'], v(oo, 'bodies', 'ring')))
    oo2 = D.run(reader(**{D.RECORD: record(order_opaque=True), D.BODY_ORDER: BODY_ORDER.replace('return character_mesh ? Turn::Body : before_bodies ? Turn::BeforeBodies : Turn::After',
                                                                                                'return before_bodies ? Turn::BeforeBodies : Turn::After')}), doc(), fams())
    check('A3 a TurnOf without the Body arm puts the body in the else turn: DIFF again', v(oo2, 'bodies', 'ring') == 'DIFF', v(oo2, 'bodies', 'ring'))
    # 3b. the class form (FADECAW): `body` = BodyObject = char_slot >= 0; a yCh_ mesh of a non-character object (chprop) is not a body
    chf = fams([D.Fam('chprop', r'bench node', 'blend', r'seats: blend off', ('opaque', ('ring',), ('char_effect',)))])
    oc = D.run(reader(**{D.RECORD: record(order_opaque=True), D.BODY_ORDER: BODY_ORDER_CLASS}), doc(), chf)
    check('A3b class form: TurnOf body read as char_slot (the plant arm skipped)', oc['model']['turn_of'].get('char_slot') == 'Body'
          and 'char_effect' not in oc['model']['turn_of'], oc['model']['turn_of'])
    check('A3b class form: bodies vs ring OK', v(oc, 'bodies', 'ring') == 'OK', row(oc, 'bodies', 'ring'))
    check('A3b class form: a yCh_ mesh of a non-character object takes the After turn', 'turn After' in oc['ours']['chprop'], oc['ours']['chprop'])
    oe = D.run(reader(**{D.RECORD: record(order_opaque=True)}), doc(), chf)
    check('A3b effect form (FADEORDER): the same yCh_ mesh takes the Body turn', 'turn Body' in oe['ours']['chprop'], oe['ours']['chprop'])
    ob = D.run(reader(**{D.RECORD: record(order_opaque=True), D.BODY_ORDER: BODY_ORDER_CLASS.replace('return g_plant_effect_body ? char_effect : char_slot >= 0;', 'return x;')}), doc(), chf)
    check('A3b an unread BodyObject: no Body turn (bodies in the else turn: DIFF)', v(ob, 'bodies', 'ring') == 'DIFF', row(ob, 'bodies', 'ring'))
    # 4. the blended list and a hook
    check('A4 blended glass (30) after the opaque body (13): OK', v(base, 'bodies', 'glass') == 'OK', row(base, 'bodies', 'glass'))
    bf = D.run(reader(**{D.RECORD: record(blended_first=True)}), doc(), fams())
    check('A4 the blended loop before the opaque one: glass vs ring DIFF', v(bf, 'glass', 'ring') == 'DIFF', row(bf, 'glass', 'ring'))
    check('A4 hook parts at AfterBlended after the blended glass: OK', v(base, 'glass', 'parts') == 'OK', row(base, 'glass', 'parts'))
    early = D.run(reader(**{'src/renderer_scene/live_particles.cpp': '  if (stage != Stage::BeforeOpaque || !ctx.frame) return;\n'}), doc(), fams())
    check('A4 the hook gate moved to BeforeOpaque: parts (47) vs glass DIFF', v(early, 'glass', 'parts') == 'DIFF', row(early, 'glass', 'parts'))
    lost = D.run(reader(**{'src/renderer_scene/live_particles.cpp': '  // no gate\n'}), doc(), fams())
    check('A4 a lost gate: UNSETTLED and a problem', v(lost, 'glass', 'parts') == 'UNSETTLED' and any('particles' in p for p in lost['problems']), lost['problems'])
    # 5. a planted twin's arm is not our order
    pf = D.run(reader(**{D.RECORD: record(plant_flash=True)}), doc(), fams())
    check('A5 the plant arm skipped: FLASHES after POST', pf['model']['phases'].index('FLASHES') > pf['model']['phases'].index('POST'), pf['model']['phases'])
    # 6. [U] game orders and unknown states
    ex = fams([D.Fam('bench', r'bench node', 'blend', r'seats: blend off', ('opaque', ('ring',), None)),
               D.Fam('stool', r'bench node', 'opaque', r'seats: blend off', ('opaque', ('ring',), None)),
               D.Fam('canvas', None, None, None, ('opaque', ('ring',), None)),
               D.Fam('lamp', r'glass node', 'blend', r'no such phrase', ('opaque', ('seats',), None))])
    u = D.run(reader(), doc(), ex)
    check('A6 one category, no stated position: UNSETTLED', v(u, 'seats', 'bench') == 'UNSETTLED', row(u, 'seats', 'bench'))
    check('A6 no doc row, a blended partner: UNSETTLED', v(u, 'canvas', 'bodies') == 'UNSETTLED', row(u, 'canvas', 'bodies'))
    check('A6 one category, no stated position, both opaque: OK', v(u, 'seats', 'stool') == 'OK', row(u, 'seats', 'stool'))
    check('A6 no doc row (state unknown), an opaque partner: UNSETTLED', v(u, 'canvas', 'ring') == 'UNSETTLED', row(u, 'canvas', 'ring'))
    check('A6 a state phrase the doc lacks: see unknown, listed', u['game']['lamp']['see'] is None and any('lamp' in p for p in u['problems']), u['problems'])


def part_a2():
    d2 = doc('| 33 | each DRTR node | R | DRTR: blend on | C | - | - | [C] |\n'
             '| 36 | chamber CDTA (pod glass, spotlight code 1); the gm draw manager (`sub_827C7BD8` +1636: node +4 is 36) | R | chamber as 35; gm manager at 36: z write 0 | C | - | - | [C] |\n'
             + REKEY)
    drtr = D.Fam('drtr', r'each DRTR node', 'blend', r'DRTR: blend on', ('blended', ('ring',), frozenset({'character'})), rekey=r'its DRTR 33 -> (\d+)')
    glass = D.Fam('glass2', r'chamber CDTA', 'nozw', r'chamber as 35', ('blended', ('ring',), frozenset({'late_node'})), pos=0, pos_re=r'pod glass, spotlight code 1')
    spot = D.Fam('spot', r'chamber CDTA', 'blend', r'spotlight code 1', ('blended', ('ring',), frozenset({'blend2', 'late_node'})), see_cell=1, pos=1,
                 pos_re=r'pod glass, spotlight code 1')
    gm = D.Fam('gm', r'the gm draw manager', 'nozw', r'gm manager at 36', ('blended', ('ring',), 'gm_preds'))
    bodies = D.Fam('bodies', r'each body node', 'blend', r'tested \+ blended', ('opaque', ('wrestlers',), ('char_slot', 'char_effect')), rekey=r'opaque node 13 \+ slot -> (\d+)',
                   ent_ours=('blended', ('wrestlers',), frozenset({'place_block'})))
    fs = [drtr, glass, spot, gm, bodies]
    rd = reader(**{D.SEE: SEE2, D.RECORD: record().replace('  run_stage(Stage::Maps);', '  draw_place::TakeLateBodies(a, b);\n  run_stage(Stage::Maps);')})
    base = D.run(rd, d2, fs)
    check('A7 no problems on the planted real-form SeeRank (LateAdditive read, the plant arm skipped)', base['problems'] == [], base['problems'])
    check('A7 the LateAdditive line is read as blend2 + late_node = rank 4', ('blend2+late_node', 4) in base['model']['ranks'] and ('late_node', 3) in base['model']['ranks']
          and len(base['model']['ranks']) == 4, base['model']['ranks'])
    check('A7 the late additive family draws at rank 4, the glass at 3', 'rank 4' in base['ours']['spot'] and 'rank 3' in base['ours']['glass2'], base['ours'])
    check('A7 glass then spotlight (the doc names them in that order): OK', v(base, 'glass2', 'spot') == 'OK', row(base, 'glass2', 'spot'))
    no_la = SEE2.replace('  if (LateAdditive(k)) return 4;\n', '')
    nl = D.run(reader(**{D.SEE: no_la}), d2, fs)
    check('A7 without the LateAdditive rank the spotlight draws before the glass: DIFF', v(nl, 'glass2', 'spot') == 'DIFF', row(nl, 'glass2', 'spot'))
    # a rank line the parser cannot read is a problem
    bad = SEE2.replace('inline bool LateAdditive(const SeeKey& k) { return k.blend == 2 && k.late_node && !g_plant_late_node_off.load(std::memory_order_relaxed); }\n', '')
    bd = D.run(reader(**{D.SEE: bad}), d2, fs)
    check('A7 a LateAdditive line with no readable helper is a problem, not skipped', any('LateAdditive' in p for p in bd['problems']), bd['problems'])
    tw = D.run(reader(**{D.SEE: bad}), d2, fs, plant='skip_unread')
    twin('skip_unread (an unread rank line skipped silently)', not any('LateAdditive' in p for p in tw['problems']), tw['problems'])
    odd = D.run(reader(**{D.SEE: SEE2.replace('  if (k.blend == 2) return 2;', '  if (k.blend == 2) return Rank2();')}), d2, fs)
    check('A7 a rank line returning a call is a problem', any('Rank2' in p for p in odd['problems']), odd['problems'])
    # the entrance scene
    check('A8 default scene: DRTR (33) then the category-36 glass, ours the same: OK', v(base, 'drtr', 'glass2') == 'OK' and base['game']['drtr']['cats'] == [33], row(base, 'drtr', 'glass2'))
    en = D.run(rd, d2, fs, scene='entrance')
    check('A8 entrance scene: DRTR re-keyed to 43 from the doc row: after the glass in the game, before it in ours: DIFF',
          en['game']['drtr']['cats'] == [43] and v(en, 'drtr', 'glass2') == 'DIFF', (en['game']['drtr']['cats'], row(en, 'drtr', 'glass2')))
    tw = D.run(rd, d2, fs, plant='entrance_off', scene='entrance')
    twin('entrance_off (the scene ignored: the pair stays OK)', v(tw, 'drtr', 'glass2') != 'DIFF', v(tw, 'drtr', 'glass2'))
    nr = D.run(rd, d2.replace('its DRTR 33 -> 43', 'its DRTR moves'), fs, scene='entrance')
    check('A8 a re-key phrase the doc lacks is a problem and the family keeps its category', any('drtr' in p for p in nr['problems']) and nr['game']['drtr']['cats'] == [33], nr['problems'])
    check('A8 the re-keyed body is the blended list character rank (TakeLateBodies + place_block read)', 'blended list rank 1' in en['ours']['bodies'], en['ours']['bodies'])
    check('A8 the body at 42 against the glass at 36: after in the game, before in ours: DIFF', v(en, 'bodies', 'glass2') == 'DIFF' and en['game']['bodies']['cats'] == [42], row(en, 'bodies', 'glass2'))
    check('A8 default scene: the body stays in the opaque list, 13..21 before the glass: OK', 'opaque list' in base['ours']['bodies'] and v(base, 'bodies', 'glass2') == 'OK', base['ours']['bodies'])
    nt = D.run(reader(**{D.SEE: SEE2}), d2, fs, scene='entrance')
    check('A8 Record without TakeLateBodies: the body stays in the opaque list, listed as a problem', 'opaque list' in nt['ours']['bodies'] and any('TakeLateBodies' in p for p in nt['problems']), nt['problems'])
    # the gm family
    check('A9 gm rows take the late-node rank 3 (GmNodeObjectFlags ors kObjNodeLate)', 'rank 3' in base['ours']['gm'] and v(base, 'drtr', 'gm') == 'OK', base['ours']['gm'])
    old = D.run(reader(**{D.SEE: SEE2, D.GM_STATE: GM_STATE.replace(' | uint32_t(kObjNodeLate)', '')}), d2, fs)
    check('A9 GmNodeObjectFlags without kObjNodeLate: the gm row falls to rank 0, before the DRTR it draws after: DIFF', 'rank 0' in old['ours']['gm'] and v(old, 'drtr', 'gm') == 'DIFF', (old['ours']['gm'], row(old, 'drtr', 'gm')))


def part_a3():
    """A10. S111 REKEYORDER2: SeeRank's `if (Rekeyed(k)) return 5;` (the helper's `(k.place_block || k.character)` group and its place term
    `k.place >= kPlaceAfterLateNodes`), SeeThroughBefore ordering ranks 1 and 5 by place"""
    d2 = doc('| 33 | each DRTR node | R | DRTR: blend on | C | - | - | [C] |\n'
             '| 36 | chamber CDTA (pod glass, spotlight code 1); the gm draw manager (`sub_827C7BD8` +1636: node +4 is 36) | R | chamber as 35; gm manager at 36: z write 0 | C | - | - | [C] |\n'
             + REKEY)
    drtr = D.Fam('drtr', r'each DRTR node', 'blend', r'DRTR: blend on', ('blended', ('wrestlers',), frozenset({'character'})), rekey=r'its DRTR 33 -> (\d+)')
    glass = D.Fam('glass2', r'chamber CDTA', 'nozw', r'chamber as 35', ('blended', ('ring',), frozenset({'late_node'})), pos=0, pos_re=r'pod glass, spotlight code 1')
    spot = D.Fam('spot', r'chamber CDTA', 'blend', r'spotlight code 1', ('blended', ('ring',), frozenset({'blend2', 'late_node'})), see_cell=1, pos=1,
                 pos_re=r'pod glass, spotlight code 1')
    gm = D.Fam('gm', r'the gm draw manager', 'nozw', r'gm manager at 36', ('blended', ('ring',), 'gm_preds'))
    bodies = D.Fam('bodies', r'each body node', 'blend', r'tested \+ blended', ('opaque', ('wrestlers',), ('char_slot', 'char_effect')), rekey=r'opaque node 13 \+ slot -> (\d+)',
                   ent_ours=('blended', ('wrestlers',), frozenset({'place_block'})))
    fs = [drtr, glass, spot, gm, bodies]
    rec = record().replace('  run_stage(Stage::Maps);', '  draw_place::TakeLateBodies(a, b);\n  run_stage(Stage::Maps);')
    rk = reader(**{D.SEE: SEE3, DRAW_LISTS_FILE: LISTS, D.RECORD: rec})
    en = D.run(rk, d2, fs, scene='entrance')
    m = en['model']
    check('A10 no problems: the || group, the place term and the place-ordered ranks are read', en['problems'] == [], en['problems'])
    check('A10 the re-keyed line is two alternatives at rank 5, read first', m['ranks'][:2] == [('place_block+place_late', 5), ('character+place_late', 5)], m['ranks'])
    check('A10 SeeThroughBefore orders ranks 1 and 5 by place (kRankRekeyed read as 5)', m.get('place_ranks') == {1, 5}, m.get('place_ranks'))
    check('A10 entrance: DRTR (43) and the re-keyed body (42) take rank 5', 'rank 5' in en['ours']['drtr'] and 'rank 5' in en['ours']['bodies'], en['ours'])
    late = [(x, y, v(en, x, y)) for x in ('drtr', 'bodies') for y in ('glass2', 'spot', 'gm')]
    check('A10 entrance: DRTR and the body after the 36 glass, the late additive and the gm rows: OK', all(o == 'OK' for _, _, o in late), late)
    r = row(en, 'drtr', 'bodies')
    check('A10 entrance: the body (42) and DRTR (43) in the place order of rank 5: ours follows the game list, OK', r['ours'] == 'G' and r['order'] == 'OK', r)
    de = D.run(rk, d2, fs)
    check('A10 default scene: no family past list 33, DRTR stays rank 1 before the glass: OK', de['problems'] == [] and 'rank 1' in de['ours']['drtr']
          and v(de, 'drtr', 'glass2') == 'OK', (de['problems'], de['ours']['drtr']))
    tw = D.run(rk, d2, fs, plant='place_late_off', scene='entrance')
    twin('place_late_off (the place term never true: the re-keyed families stay in rank 1, before the glass)', v(tw, 'drtr', 'glass2') == 'DIFF', v(tw, 'drtr', 'glass2'))
    tw = D.run(rk, d2, fs, plant='place_rank1_only', scene='entrance')
    twin('place_rank1_only (SeeThroughBefore read as ordering rank 1 only)', row(tw, 'drtr', 'bodies')['ours'] != 'G', row(tw, 'drtr', 'bodies'))
    bad = D.run(reader(**{D.SEE: SEE3.replace('ra == kRankRekeyed', 'ra == kNoSuchRank'), DRAW_LISTS_FILE: LISTS, D.RECORD: rec}), d2, fs, scene='entrance')
    check('A10 a place-order rank that is not read is a problem', any('kNoSuchRank' in p for p in bad['problems']), bad['problems'])
    bad = D.run(reader(**{D.SEE: SEE3.replace('= int32_t(scene_inputs::char_draw_list::kMaxNodes);', '= 300;'), DRAW_LISTS_FILE: LISTS, D.RECORD: rec}), d2, fs,
                scene='entrance')
    check('A10 a place threshold that is not the first place of list 1 is a problem', any('kPlaceAfterLateNodes' in p for p in bad['problems']), bad['problems'])
    bad = D.run(reader(**{D.SEE: SEE3, D.RECORD: rec}), d2, fs, scene='entrance')
    check('A10 the walked lists unread (no char_draw_list.h): a problem, and no family takes the place term', any('kDrawLists' in p for p in bad['problems'])
          and 'rank 1' in bad['ours']['drtr'], (bad['problems'], bad['ours']['drtr']))


def part_b():
    if subprocess.run(['git', 'rev-parse', '--verify', '--quiet', FADE_PARENT], cwd=D.ROOT, capture_output=True).returncode != 0:
        print(f'SKIP B ({FADE_PARENT} not in this clone)')
        return
    with open(os.path.join(D.ROOT, D.DOC), encoding='utf-8') as f:
        text = f.read()
    old = D.run(D.git_reader(FADE_PARENT), text)
    tip = D.run(D.disk_reader(), text)
    check('B no anchor lost at the FADEORDER parent', old['problems'] == [], old['problems'])
    check('B no anchor lost at the tip', tip['problems'] == [], tip['problems'])
    for other in ('ring', 'arena', 'props'):
        check(f'B re-find FADEORDER: bodies vs {other} DIFF at {FADE_PARENT}', v(old, 'bodies', other) == 'DIFF', row(old, 'bodies', other))
        check(f'B bodies vs {other} OK at the tip', v(tip, 'bodies', other) == 'OK', row(tip, 'bodies', other))


def main():
    part_a()
    part_a2()
    part_a3()
    part_b()
    print(f'draworder selftest: {len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 1 if FAILS else (2 if CAUGHT else 0)


if __name__ == '__main__':
    sys.exit(main())

r"""q rstdiff: per draw kind, the RENDER STATE the GAME sets against the state OURS builds (S111 RSTDIFF).

    py tools/q/q.py rstdiff [--kind K] [--flag DIFF|UNSETTLED|PASS-NEVER|OK ...] [--json] [--ref REF]

The player's effect files carry no blend / alpha / depth / cull state (every pass's render-state block, pass+12, holds at most
fill mode = 0: docs/research/s103/T2_render_states.md 0; this tool re-checks it on every call, cached), so the game's state is
set by CODE, per draw system, through the per-thread state recorder (vtable 0x8206C6D0, wrappers 0x82BB75A0 .. 0x82BB7C70).
One row per (draw kind, field, our site). Fields: ztest, zwrite, zfunc, blend, blend_op / blend_src / blend_dst (colour),
cull (enable), cmask (colour write mask), stencil.
Sources of truth (read on every call; nothing about the game committed here):
  game  - each kind's game windows (KINDS): the recorder-wrapper calls between two offsets of a function's listing, with the
          literal r4 each passes, read over the listing's PATHS (a forward data flow: r4 that differs by path is every literal it
          can be; a call whose r4 is not a constant is VAR: the value comes from data). A `set` window gives the state at its end
          hi: per slot, the last call on each path to hi (a path that sets none adds the inherited value); several set windows
          join (alternative draw methods, or a callee's per-part set). A window's `arms` read a branch one way ('taken' / 'fall'),
          each with its reason in KINDS. An `adds` window is a conditional set on one path (its literals join the set: the
          particle entry's ALWAYS). A field no window sets, or a path that keeps it, is inherited: for a scene draw from the
          default reset sub_82C26ED0 (z func / write / enable, read from its device-setter calls), else UNSETTLED.
        - the recorder's code maps: z func code -> compare (sub_82C237D8 +38..+B0, q seam depth), blend code -> (op, src, dst)
          (sub_82C23168, T2 2), cull 0 = off, else on.
        - the effect files (tools/shaders/effect_table.py's pacs): a pass whose block sets a compared row would be a per-pass
          source: PASS-NEVER when the model draw never picks that pass (qconstdiff.reachable_passes), else UNSETTLED.
  ours  - each kind's DrawState block (`rdr::DrawState X{};` up to an end line) in our file, parsed with file:line: X.field = EXPR
          (comma-chained too) in order, under its if / else conditions. EXPR is evaluated to a set of values: literals, ternaries
          (both arms; a plant / fault condition is OFF: the shipped arm), named constexpr constants and one-line helpers of the
          kind's files (live_particles.h ParticleDepthCompare, pass_state.h ForwardZ), pass_state::Of(pcls).field = the kind's
          pass_state.h kTable rows, locals of the block; a value read from the game's own word at the draw (the kind's game words:
          the 2D row's state, the emitter's blend) is GAME; anything else is a RULE of ours.
Compare: the game's z func is mirrored into our forward depth for a main-view kind (GEQUAL -> LEQUAL, GREATER -> LESS and back:
q seam depth), so a reversed-depth equivalent is never a DIFF; z func / z write are moot when neither side tests depth, the
blend factors when neither side blends.
Flags: DIFF the sets differ; OK equal (or ours follows the game's own word); UNSETTLED the game's value is not read (inherited
with no reset read, or from data where ours is a literal) or ours is a rule of ours: never OK; PASS-NEVER (effect-file rows only).
A window's `slots` takes only those slots from it (an outer function's state the draw inherits; S111 RSTCOVER-A). A block of ours
with no game draw at all is in OURS_ONLY with its reason, listed apart from the blocks no kind compares.
Exit 2 on a PROBLEM (a window with no recorder call, an anchor not found, a kind whose game windows disagree with KINDS' offsets).
"""
import argparse
import fnmatch
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402
import qrstdiff_inherit as RCB   # noqa: E402  (S111 RSTCOVER-B: a state no window sets, read from every setter of it)
import qostate as O   # noqa: E402  (S111 OSTATE: the ours-side C++ evaluator lives in qostate.py, one for rstdiff and ostate)
from qostate import (FIELDS, OURS_FIELD, WHOLE_BLEND, FX_ROWS, PLANT_RE, START_RE, GAME, PS, FE, MissingAtRef, born_after,  # noqa: E402,F401
                     default_reader, line_of, _depth_split, split_ternary, strip_parens, Ctx, eval_cond, eval_val, CASE_RE, CHAIN_RE,
                     ASSIGN_RE, WHOLE_RE, assign_targets, statements, _one_stmt, parse_table, builder_mask, ours_state, effect_rows)
RstDiffError = O.OstateError   # the evaluator's error (MissingAtRef is one): rstdiff's callers catch it by this name

REPO = CD.REPO
FLAGS = ('DIFF', 'UNSETTLED', 'PASS-NEVER', 'OK')
RANK = {'DIFF': 3, 'UNSETTLED': 2, 'PASS-NEVER': 1, 'OK': 0}

# The recorder wrappers (sub_82BA7238 + a vcall on the thread block's state object, vtable 0x8206C6D0): T2 2, q seam 2d_layer.
WRAPPERS = {0x82BB75A0: 'blend', 0x82BB75E8: 'atest', 0x82BB7630: 'blend_code', 0x82BB7678: 'afunc_code', 0x82BB7928: 'aref',
            0x82BB7898: 'ztest', 0x82BB7AD8: 'zwrite', 0x82BB78E0: 'zfunc_code', 0x82BB7C28: 'cull', 0x82BB76C0: 'stencil',
            0x82BB7970: 'cmask'}
RESET_FN = 0x82C26ED0                    # render-command op 80: the default reset of a scene draw's depth state
RESET_SETTERS = {0x82CA9750: 'zfunc_hw', 0x82CA9720: 'zwrite', 0x82CA96E0: 'ztest'}   # device setters (render states 11 / 12 / 10)
ZFUNC_CODE = {0: 0, 1: 7, 2: 6, 3: 4, 5: 1, 6: 3, 7: 2, 8: 5}   # sub_82C237D8: recorder code -> compare (Xenos = VkCompareOp numbers)
CMP = {0: 'NEVER', 1: 'LESS', 2: 'EQUAL', 3: 'LEQUAL', 4: 'GREATER', 5: 'NOTEQUAL', 6: 'GEQUAL', 7: 'ALWAYS'}
MIRROR = {6: 3, 3: 6, 4: 1, 1: 4}        # a reversed-depth compare -> its forward twin (q seam depth; pass_state.h ForwardZ)
BLEND_CODES = {0: ('ADD', 'SRC_ALPHA', 'INV_SRC_ALPHA'), 1: ('ADD', 'SRC_ALPHA', 'ONE'), 2: ('REVSUB', 'SRC_ALPHA', 'ONE'),
               5: ('ADD', 'ONE', 'ZERO')}   # sub_82C23168 (T2 2: each branch's li pairs)
VK_FACTOR = {0: 'ZERO', 1: 'ONE', 2: 'SRC_COLOR', 3: 'INV_SRC_COLOR', 4: 'DST_COLOR', 5: 'INV_DST_COLOR', 6: 'SRC_ALPHA',
             7: 'INV_SRC_ALPHA', 8: 'DST_ALPHA', 9: 'INV_DST_ALPHA'}
VK_OP = {0: 'ADD', 1: 'SUB', 2: 'REVSUB', 3: 'MIN', 4: 'MAX'}
SCENE_FIELDS = ('ztest', 'zfunc')

# One kind = one draw system of the game and the block(s) of ours that draw the same thing. Offsets are the listing's (q decomp).
KINDS = [
    {'kind': 'particles', 'mirror': True, 'base': True, 'fields': FIELDS,
     'what': 'the particle draw methods (category 47, in the scene)',
     # +F8 (each method): `beq` on the engine display's word +336 (0x83929A68). The scene arm is the TAKEN one (+114: blend on, alpha
     # test off, z write off); the fall-through arm (+FC..+110: blend off, z write not set) runs while +336 is set: from
     # engine_end_frame_present (0x82BA84E8 +9C) until a target bind clears it (0x82BA7B40 +44, 0x82BB3910 +4C, the post driver
     # 0x82C1E4C0 +D8) [L: which draws see a set +336 is not read]. The T_Rect leaf 0x82BC3C18 +48 has the same test.
     # +1A4: `beq` skips the cull-off call when the cull saved at +B4 (sub_82BB8398, the recorder's +112) is already 0: cull is 0 on
     # both arms, so the fall-through arm (the call) is the one read.
     'game': [{'fn': 0x82A0D0E0, 'lo': 0xF8, 'hi': 0x1B4, 'mode': 'set', 'why': 'particle draw method a', 'arms': {0xF8: 'taken', 0x1A4: 'fall'}},
              {'fn': 0x82A0DAC8, 'lo': 0xF8, 'hi': 0x1B4, 'mode': 'set', 'why': 'particle draw method b', 'arms': {0xF8: 'taken', 0x1A4: 'fall'}},
              {'fn': 0x82A0E3D8, 'lo': 0xF8, 'hi': 0x1B4, 'mode': 'set', 'why': 'particle draw method c', 'arms': {0xF8: 'taken', 0x1A4: 'fall'}},
              # S111 RSTCOVER-B: +DC `bne` skips the blend-code call for an emitter code (emitter +196 = its PRTC header block +12)
              # other than 0 / 1 / 2; every emitter of the player's prtcls.pac has 0, 1 or 2 (S103 F5 census, tools/scene/f5_blend_codes.py:
              # 3,555 / 74,894 / 74 of 78,523; docs/formats/particle_items.md "Lane F5"), so the skip is read as not taken.
              {'fn': 0x82A0AED8, 'lo': 0xE0, 'hi': 0xE8, 'mode': 'set', 'why': "the entry draw: the emitter's blend code", 'arms': {0xDC: 'fall'}},
              {'fn': 0x82A0AED8, 'lo': 0x84, 'hi': 0x88, 'mode': 'adds', 'why': 'the entry draw, when the sub-record word +220 is 0'}],
     'ours': [{'file': 'src/renderer_scene/live_particles.cpp', 'after': r'LiveParticles::Record\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}',
               'helpers': ['src/renderer_scene/live_particles.h'], 'game_words': [r'\bb\.blend\b']}]},
    {'kind': 'crowd_flash', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': "the crowd camera flashes (category 52, after the post): the item's set section, then its draw sub_82C0F528 (vtable slot +168)",
     'game': [{'fn': 0x823B27F0, 'lo': 0x330, 'hi': 0x388, 'mode': 'set', 'why': 'flash_item_draw, before its +3D4 draw'},
              {'fn': 0x82C0F528, 'lo': 0xE30, 'hi': 0xE34, 'mode': 'set',
               'why': "crowd_flash_pass_draw: the mesh's own cull, 1 when its record word +0x8C has bit 0x2 (+E10..+E2C; every flash mesh has it: docs/formats/model_mesh_record.md)"}],
     'ours': [{'file': 'src/renderer_scene/front_end_flashes.h', 'after': r'DrawFlashesAfterPost\b', 'end': r'rdr::DynamicState\s+dynamic\s*\{\s*\}'}]},
    {'kind': 'lens_element', 'mirror': True, 'base': True, 'fields': FIELDS,
     'what': "the lens flare's element, an in-scene 2D row (category 36)",
     'game': [{'fn': 0x827D5E98, 'lo': 0x508, 'hi': 0x5A0, 'mode': 'set', 'why': 'the lens element draw (z test from the texture index at +598)'},
              # S111 RSTCOVER-B: each element is drawn by its object's slot 1 (+614 vcall +4): vtable 0x8206A7B8 -> sub_82BC8060 +70 and
              # 0x82033708 -> sub_827D5520 +DC (built by the lens ctor sub_827D6B28: sub_82BC3000 +30 / +258), both into the T_Sprite
              # leaf, which draws the element at +DF8. The leaf saves the cull at +128 (the recorder's +112) and skips the cull-off
              # call at +148 only when it is already 0: cull 0 on both arms. Only the cull is taken from this window ('slots').
              {'fn': 0x82BC4890, 'lo': 0x0, 'hi': 0xDF8, 'mode': 'set', 'slots': ('cull',), 'arms': {0x148: 'fall'},
               'why': 'the T_Sprite leaf (ui_sprite_text_draw) before the element draw at +DF8: cull off'}],
     'ours': [{'file': 'src/renderer_scene/quads2d_draw.cpp', 'after': r'\bAppend\s*\(', 'end': r'rdr::DynamicState\s+dyn\s*\{\s*\}',
               'known': {r'ctx\.scene_depth': True}, 'game_words': [r'\brow\.state\b', r'^blend$'], 'ctx': 'in-scene row',
               'blend_builder': {'file': 'src/renderer_scene/quads2d_blocks.h', 'fn': r'\bBlendOf\s*\(', 'var': 'out'}}]},
    {'kind': 'fade', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': 'the FADE (category 57, the final target): its rect draw, through the T_Rect leaf',
     'game': [{'fn': 0x823B3B48, 'lo': 0x68, 'hi': 0xB8, 'mode': 'set', 'why': 'the FADE rect draw'},
              # +38 (the rect's word +268 is 0) and +48 (the display's +336 is set, the particles' test) both go to +68: blend off.
              # +48 is read as the scene's (fall-through) arm; +38 is the rect's own data: both arms kept.
              {'fn': 0x82BC3C18, 'lo': 0x54, 'hi': 0x74, 'mode': 'set', 'why': "the T_Rect leaf: the rect object's blend code, or blend off", 'arms': {0x48: 'fall'}},
              # S111 RSTCOVER-B: +78 `beq` skips the cull-off call when the cull saved at +24 (sub_82BB8398, the recorder's +112) is
              # already 0: cull 0 on both arms, so the fall-through arm (the call) is the one read (as the particle methods' +1A4).
              {'fn': 0x82BC3C18, 'lo': 0x84, 'hi': 0x88, 'mode': 'set', 'why': 'the T_Rect leaf: cull off', 'arms': {0x78: 'fall'}}],
     'ours': [{'file': 'src/renderer_scene/quads2d_draw.cpp', 'after': r'\bAppend\s*\(', 'end': r'rdr::DynamicState\s+dyn\s*\{\s*\}',
               'known': {r'ctx\.scene_depth': False}, 'game_words': [r'\brow\.state\b', r'^blend$'], 'ctx': 'final-target row',
               'blend_builder': {'file': 'src/renderer_scene/quads2d_blocks.h', 'fn': r'\bBlendOf\s*\(', 'var': 'out'}}]},
    {'kind': 'rim_map', 'mirror': True, 'base': False, 'fields': ('ztest', 'zwrite', 'zfunc', 'blend', 'cull'),
     'what': 'the rim map (its own target; every target bind re-sets the reversed viewport, q seam depth)',
     'game': [{'fn': 0x827EA098, 'lo': 0xF4, 'hi': 0x128, 'mode': 'set', 'why': 'the rim map draw'},
              # the categories walk (+138, sub_827E97A0) reaches the T_RimLightMap pass function, which sets the cull per mesh before
              # each part's draw (model_part_draw at +24E0): 1 when the mesh record's +0x8C has bit 0x2, the scene's rule
              {'fn': 0x82BF9878, 'lo': 0x1CE8, 'hi': 0x1D04, 'mode': 'set', 'why': "pass_rimlight_draw_parts: the mesh's own cull (record +0x8C bit 0x2)"}],
     'ours': [{'file': 'src/renderer_scene/live_map_rim.cpp', 'after': r'LiveMapsHook::RecordRim\b', 'end': r'rdr::DrawOp\s+op\s*\{\s*\}'}]},
    {'kind': 'scene_lit', 'mirror': True, 'base': True, 'fields': SCENE_FIELDS, 'classes': ['lit', 'hair'],
     'what': 'the model lit pass (wrestlers, props, hair): pass_state Lit / Hair',
     'game': [{'fn': 0x82BF5C58, 'lo': 0xBC, 'hi': 0xD8, 'mode': 'set', 'why': 'pass_lambert_draw start'}],
     'ours': [{'file': FE, 'after': r'FrontEnd::Record\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': [PS], 'ctx': 'generic'}]},
    {'kind': 'scene_arena', 'mirror': True, 'base': True, 'fields': SCENE_FIELDS, 'classes': ['arena'],
     'what': 'the ring / stage colour pass (yBG_*): pass_state Arena',
     'game': [{'fn': 0x825D0438, 'lo': 0x98, 'hi': 0xE4, 'mode': 'set', 'why': 'ring_task_colour_draw_callback'},
              {'fn': 0x82BF5C58, 'lo': 0xBC, 'hi': 0xD8, 'mode': 'set', 'why': 'pass_lambert_draw start'}],
     'ours': [{'file': FE, 'after': r'FrontEnd::Record\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': [PS], 'ctx': 'generic'}]},
    {'kind': 'scene_crowd', 'mirror': True, 'base': True, 'fields': SCENE_FIELDS, 'classes': ['crowd'],
     'what': 'the crowd cards / members (yAudience*): pass_state Crowd',
     'game': [{'fn': 0x82519508, 'lo': 0x178, 'hi': 0x1B8, 'mode': 'set', 'why': 'audience_draw_entry'},
              {'fn': 0x825185B0, 'lo': 0x200, 'hi': 0x21C, 'mode': 'set', 'why': 'the audience blocks'}],
     'ours': [{'file': FE, 'after': r'FrontEnd::Record\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': [PS], 'ctx': 'generic'},
              {'file': FE, 'after': r'FrontEnd::DrawInstanced\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': [PS], 'ctx': 'instanced'}]},
    {'kind': 'scene_see_through', 'mirror': True, 'base': True, 'fields': SCENE_FIELDS, 'classes': ['see_through', 'see_through_cut'],
     'what': "the character's translucent draw (yCh_trans, the SUBM pieces): pass_state SeeThrough / SeeThroughCut",
     'game': [{'fn': 0x8259FAE8, 'lo': 0x150, 'hi': 0x184, 'mode': 'set', 'why': 'character_translucent_draw'}],
     'ours': [{'file': FE, 'after': r'FrontEnd::Record\b', 'end': r'rdr::TextureSet\s+set\s*\{\s*\}', 'helpers': [PS], 'ctx': 'generic'}]},
]

# ------------------------------------------------------------------ S111 RSTCOVER-A: the map, 2D and post blocks (docs/research/s111/RSTCOVER_A.md)
# The 14 DrawState blocks RSTDIFF listed as not compared. A window's 'slots' takes only those recorder slots from it: an OUTER
# function's state the draw inherits (the shadow module frame's z func / cull, the plane mask pass's blend) without the slots the
# inner draw function sets again (the tool joins the windows of different functions; it does not order them).
SHADOW_FRAME = {'fn': 0x827E95D8, 'lo': 0x54, 'hi': 0xAC, 'mode': 'set', 'slots': ('zfunc_code', 'cull'),
                'why': 'shadow_module_render_frame: z func code 2, cull 1 (winding VAR at +94) before the props map at +AC'}
SHADOW_PASS = {'fn': 0x82C01FE0, 'lo': 0xE0, 'hi': 0xF8, 'mode': 'set',
               'why': 'the T_ShadowMap pass (vtable slot +188): blend 0, alpha test 0 before its part loop (its +98 apply4 is the early-out path)'}
PLANE_PASS = {'fn': 0x827EEF80, 'lo': 0x40, 'hi': 0xA8, 'mode': 'set',
              'why': 'the plane mask pass (node sub_827EF070 +6C): alpha test 0, blend 0, cull 1, winding 1, z func code 2 around its walk sub_827EED58 at +A8'}
CASTER_CULL = 'src/renderer_scene/caster_cull.h'   # S111 CASTCULL: the four caster kinds' cull / face constants, read by name
KINDS_COVER_A = [
    {'kind': 'map_chain', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': "the maps' filter quads (ChainQuad): the shadow min blurs and the rim t2 blur",
     'game': [{'fn': 0x827E89C0, 'lo': 0x30, 'hi': 0x98, 'mode': 'set',
               'why': 'the shadow min blur (called twice by sub_827E9180 at the frame +110; its quad sub_82C1F420 at +29C)'},
              {'fn': 0x827EA098, 'lo': 0xF4, 'hi': 0x21C, 'mode': 'set',
               'why': "the rim map draw's second section: cull 0, z test 0 before its t2 quad sub_82C1F420 at +330"}],
     'ours': [{'file': 'src/renderer_scene/live_map_chain.cpp', 'after': r'static\s+void\s+ChainQuad\s*\(', 'end': r'rdr::DynamicState\s+dyn\s*\{\s*\}'}]},
    {'kind': 'circle_blobs', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': 'the circle blob shadows into the props map (T_CircleShadow sub_82812E90, called by its wrapper at +F4)',
     'game': [dict(SHADOW_FRAME),
              {'fn': 0x82813228, 'lo': 0x9C, 'hi': 0xF4, 'mode': 'set',
               'why': 'circle_shadow_pass_wrapper: blend 1, code 0, z write 0, z test 0, colour mask groups 14 / 15 (not read: slot 13 only)'}],
     'ours': [{'file': 'src/renderer_scene/live_map_circles.cpp', 'after': r'LiveMapsHook::RecordCircles\b', 'end': r'const\s+uint32_t\s+state_id\s*=\s*fb\.State'}]},
    {'kind': 'props_casters', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': "the props map's casters (sub_827E7F00 at the frame +AC, through the T_ShadowMap pass)",
     'game': [dict(SHADOW_FRAME),
              {'fn': 0x827E7F00, 'lo': 0x5C, 'hi': 0x94, 'mode': 'set', 'why': 'the props map pass: blend 0, alpha test 0'},
              dict(SHADOW_PASS)],
     'ours': [{'file': 'src/renderer_scene/live_map_props.cpp', 'after': r'LiveMapsHook::DrawPropCasters\b', 'end': r'const\s+uint32_t\s+state_id\s*=\s*fb\.State', 'helpers': [CASTER_CULL]}]},
    {'kind': 'shadow_casters', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': 'map B, the shadow map (sub_827E8080 at the frame +D8, through the T_ShadowMap pass)',
     # map A (sub_827E8448 at +D0) sets z func code 6 and puts back the value it saved (+4C..+58 getters, +1C0): map B draws with the frame's
     'game': [dict(SHADOW_FRAME, hi=0xD8, why='shadow_module_render_frame: z func code 2, cull 1 (winding VAR at +94) before map B at +D8'),
              dict(SHADOW_PASS)],
     'ours': [{'file': 'src/renderer_scene/live_map_shadow.cpp', 'after': r'const\s+auto\s+draw_mesh\s*=', 'end': r'rdr::DrawOp\s+op\s*\{\s*\}', 'helpers': [CASTER_CULL]}]},
    {'kind': 'plane_mask', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': "the plane shadow mask's wrestler proxies (sub_827EE190, called by the walk sub_827EED58 at +F0)",
     'game': [dict(PLANE_PASS),
              {'fn': 0x827EE190, 'lo': 0x20, 'hi': 0xE0, 'mode': 'set', 'why': "plane_proxy_draw_with_plane_pos: z write 1 before the proxy model's draw at +E0"}],
     'ours': [{'file': 'src/renderer_scene/live_map_plane.cpp', 'after': r'LiveMapsHook::RecordPlane\b', 'end': r'rdr::DrawOp\s+op\s*\{\s*\}', 'helpers': [CASTER_CULL]}]},
    {'kind': 'plane_rope', 'mirror': True, 'base': False, 'fields': FIELDS,
     # sub_825CECE0 (the stencil + reverse-subtract main-view draw) returns at once when the plane module exists ([0x83924758] != 0,
     # +5C); then the ring task sub_825CF2F8 appends its proxies to the module's list (+2F4..+318, module+1176), which sub_827EE190
     # draws inside the mask walk: the mask pass's state [C, S111 CASTCULL]
     'what': "the ring's rope proxies (entry 956) in the plane mask: the mask pass's state",
     'game': [dict(PLANE_PASS)],
     'ours': [{'file': 'src/renderer_scene/live_map_rope.cpp', 'after': r'LiveMapsHook::DrawRopeProxies\b', 'end': r'rdr::DrawOp\s+op\s*\{\s*\}', 'helpers': [CASTER_CULL]}]},
    {'kind': 'plane_downscale', 'mirror': True, 'base': False, 'fields': FIELDS,
     # the downscale sub_82815840 (vtable +8 of module+48, call site 0x828159FC) calls no recorder wrapper but the winding (sub_82BB7C70)
     'what': 'the plane mask 512 -> 256 downscale (yDownScale_box4): the mask pass state it inherits',
     'game': [dict(PLANE_PASS)],
     'ours': [{'file': 'src/renderer_scene/live_map_plane.cpp', 'after': r's\.ds_ready\s*=\s*renderer_\.AddProgramChecked', 'end': r'rdr::DynamicState\s+dd2\s*\{\s*\}'}]},
    {'kind': 'plane_blur', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': 'the plane blurs plane_x / plane_z (sub_827EEAE0, called twice by the walk at +180 / +190)',
     'game': [dict(PLANE_PASS, slots=('blend', 'atest')),
              {'fn': 0x827EEAE0, 'lo': 0x58, 'hi': 0xB4, 'mode': 'set', 'why': 'the plane blur: z test 0, z write 0, z func code 1, cull 0 before its draw at +1CC'}],
     'ours': [{'file': 'src/renderer_scene/live_map_plane.cpp', 'after': r'const\s+uint32_t\s+bdyn\s*=\s*fb\.Dynamic', 'end': r'const\s+uint32_t\s+bstate\s*='}]},
    {'kind': 'prelight', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': 'the light-cube bake strip (sub_82C20BC8: the state at +58, the draw sub_82C20AB8 at +FC)',
     'game': [{'fn': 0x82C1FA98, 'lo': 0x24, 'hi': 0xB0, 'mode': 'set', 'why': 'the small-pass state set: cull, z test, z write, blend, alpha test, stencil 0'}],
     'ours': [{'file': 'src/renderer_scene/live_map_prelight.cpp', 'after': r'LiveMapsHook::RecordPreLight\b', 'end': r'const\s+uint32_t\s+state_id\s*=\s*fb\.State'}]},
    {'kind': 'sweat', 'mirror': True, 'base': False, 'fields': FIELDS,
     'what': 'the liquid (sweat / blood) map passes (node sub_8279ACD8)',
     'game': [{'fn': 0x8279ACD8, 'lo': 0x44, 'hi': 0xE0, 'mode': 'set',
               'why': 'sweat_map_node_render_entry: cull 0, z test 0, z write 0, blend 1, alpha test 0, stencil 0 (the blend code inherited)'}],
     'ours': [{'file': 'src/renderer_scene/live_map_sweat.cpp', 'after': r'const\s+auto\s+make_state\s*=', 'end': r'return\s+fb\.State\s*\(\s*st\s*\)'}]},
    {'kind': 'sprite_2d', 'mirror': False, 'base': False, 'fields': FIELDS,
     'what': "our 2D layer's sprites and text (the game's T_Sprite / T_XfmFont draw sub_82BC4890)",
     'game': [{'fn': 0x82BC4890, 'lo': 0x128, 'hi': 0xDF8, 'mode': 'set',
               'why': "ui_sprite_text_draw: cull 0, then blend / alpha test / z off, then the sprite's blend code (or blend off) before its draw at +E20"}],
     'ours': [{'file': 'src/renderer_scene/pass2d.cpp', 'after': r'for\s*\(\s*const\s+DrawItem\s*&\s*item\s*:\s*items\s*\)', 'end': r'rdr::TextureSet\s+textures\b'}]},
    {'kind': 'post_quad', 'mirror': False, 'base': False, 'fields': FIELDS,
     # the node sets z test / stencil / cull for the whole walk; each effect run method then its own blend before its quad. DoF
     # (sub_82C2BBE8), defocus (sub_82C29478) and halation (sub_82C29818) set no state: they draw with what the walk left [L]
     'what': "the post chain's full-target quads (node sub_82C1E840, slot 2 of its 10 effects)",
     'game': [{'fn': 0x82C1E840, 'lo': 0x44, 'hi': 0xC0, 'mode': 'set', 'why': 'the post node: z test 0, stencil 0, cull 0 before the effects walk at +C0'},
              {'fn': 0x82C2BF00, 'lo': 0x84, 'hi': 0xD8, 'mode': 'set', 'why': 'yAntialiasing run: blend 0, code 0'},
              # S111 POSTATEST: the driver is an OUTER function of the combine quad: T_Final (0x82C1E1D0 +F0..+120) sets alpha test,
              # blend, z test, z write and cull again before the quad (sub_82C1D9C0 +78); only the driver's blend code (+BC) reaches it.
              {'fn': 0x82C1E4C0, 'lo': 0x64, 'hi': 0xC4, 'mode': 'set', 'slots': ('blend_code',),
               'why': 'the combine driver: blend code 0 (its alpha test 1 at +80, blend, z and cull are set again by T_Final before the quad)'},
              {'fn': 0x82C1E1D0, 'lo': 0xD4, 'hi': 0x13C, 'mode': 'set', 'why': 'T_Final: blend 0, z test 0, z write 0, cull 0'},
              {'fn': 0x82C1E398, 'lo': 0x54, 'hi': 0x70, 'mode': 'set', 'why': 'the HDR / tone-map chain: blend 0'},
              {'fn': 0x82C2A868, 'lo': 0x5C, 'hi': 0x84, 'mode': 'set', 'why': 'yMonotone run: blend 0'},
              {'fn': 0x82C28F68, 'lo': 0x44, 'hi': 0x98, 'mode': 'set', 'why': 'yEmboss run: blend 0, code 0'},
              {'fn': 0x82C28A70, 'lo': 0x98, 'hi': 0xE8, 'mode': 'set', 'why': 'yRetroFilm run: blend 0, code 0'}],
     'ours': [{'file': 'src/renderer_scene/post_pass.cpp', 'after': r'const\s+auto\s+pass\s*=\s*\[', 'end': r'rdr::DynamicState\s+dyn\s*\{\s*\}'}]},
    {'kind': 'post_ghost', 'mirror': False, 'base': False, 'fields': FIELDS,
     'what': "yAfterImage, the ghost (run sub_82C2AF80: its sprite sub_82C2AE20 at +54 blends the previous frame's copy back)",
     'game': [{'fn': 0x82C1E840, 'lo': 0x44, 'hi': 0xC0, 'mode': 'set', 'slots': ('cull', 'stencil'), 'why': 'the post node: stencil 0, cull 0'},
              {'fn': 0x82C2AE20, 'lo': 0x5C, 'hi': 0xD8, 'mode': 'set', 'slots': ('ztest', 'zwrite'),
               'why': 'the afterimage sprite: z write 0, z test 0 (its blend 1 at +78 is set again by the leaf below on every path to the draw)'},
              # S111 RSTCOVER-A review: the sprite at +32 is a 0x8206A7B8 object (ctor sub_82BC2F80 from sub_82C2AB90 +6C); its draw at +134
              # is vtable +4 = sub_82BC8060, which calls the T_Sprite leaf at +70 when the sprite's +60 (the alpha byte, vtable +40
              # sub_82270A08 at +120) is not 0. The leaf sets the blend again before its draw: on with the sprite's code, or off.
              {'fn': 0x82BC4890, 'lo': 0x128, 'hi': 0xDF8, 'mode': 'set', 'slots': ('blend', 'blend_code'),
               'why': "the T_Sprite leaf the sprite's draw reaches: blend 1 with the sprite's own code (+DAC / +DC4) or blend 0 (+DD4)"}],
     'ours': [{'file': 'src/renderer_scene/post_pass.cpp', 'after': r'ghost_program_made_\s*=\s*true', 'end': r'rdr::DynamicState\s+dyn\s*\{\s*\}'}]},
]
KINDS += KINDS_COVER_A
# Blocks of ours with NO game draw: (file, the anchor the block follows, why). The tool lists them apart, never as "not compared".
OURS_ONLY = [
    ('src/renderer_scene/post_pass.cpp', r'depth_program_made_\s*=\s*true',
     "our viewer depth -> the game's depth for the post shaders; the game reads its own depth resolve (a copy: GAME_RULES_TEXTURES_FRAME.md C6), no draw"),
]
# ------------------------------------------------------------------ end S111 RSTCOVER-A


def hx(a):
    return f'0x{a:08X}'


# ------------------------------------------------------------------ the game: recorder calls in a listing window
# The listing is read over its PATHS (S111 RSTDIFF review): a register literal that differs by path is the set of them, and a set
# window's state is what reaches its end on every path. Read in text order alone, a literal 1 with a branch over a later literal 0
# was 0 (the flash mesh's cull, sub_82C0F528 +E18..+E30) and a leaf's two arms were one (0x82BC3C18 +54 / +70): false OKs.
INH = 'INH'                              # a path through the window that sets nothing: the value inherited from before it
VOLATILE = ('r0', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12')
NO_FALL = ('b', 'blr', 'bctr')           # the next instruction is not reached from these
MAX_LITS = 8                             # a register with more literals than this is unknown (a loop counter)


def _target(x):
    """-> the branch target of a b / bc-form instruction (not a call), else None."""
    mn = x.mn.rstrip('+-')
    if not mn.startswith('b') or mn in ('bl', 'bctrl', 'blrl') or mn.endswith(('lr', 'ctr', 'lrl', 'ctrl')):
        return None
    ops = [o.strip() for o in x.ops.split(',')] if x.ops else []
    try:
        return int(ops[-1], 16)
    except (ValueError, IndexError):
        return None


def _step(x, st, fn, targets, win):
    """The state after x: registers as frozensets of literals (absent = unknown); with a window, ('S', slot) = the frozenset of
    (value, offset) the window's last call of that slot set on this path (value None = VAR)."""
    st = dict(st)
    ops = [o.strip() for o in x.ops.split(',')] if x.ops else []
    mn = x.mn
    if mn in ('bl', 'bctrl', 'blrl'):
        if mn == 'bl' and win:
            try:
                tgt = int(ops[0], 16)
            except (ValueError, IndexError):
                tgt = None
            off = x.addr - fn
            if tgt in targets and win[0] <= off < win[1]:
                v = st.get('r4')
                st[('S', targets[tgt])] = frozenset((k, off) for k in v) if v else frozenset({(None, off)})
        for r in VOLATILE:
            st.pop(r, None)
        return st
    try:
        if mn == 'li':
            st[ops[0]] = frozenset({int(ops[1], 0) & 0xFFFFFFFF})
        elif mn == 'lis':
            st[ops[0]] = frozenset({(int(ops[1], 0) & 0xFFFF) << 16})
        elif mn in ('addi', 'ori'):
            b, k = st.get(ops[1]), int(ops[2], 0)
            if b:
                st[ops[0]] = frozenset((((v + k) if mn == 'addi' else (v | k)) & 0xFFFFFFFF) for v in b)
            else:
                st.pop(ops[0], None)
        elif mn == 'mr':
            if ops[1] in st:
                st[ops[0]] = st[ops[1]]
            else:
                st.pop(ops[0], None)
        elif ops and re.match(r'^r\d+$', ops[0]) and not mn.startswith(('st', 'cmp', 'b')):
            st.pop(ops[0], None)
    except (ValueError, IndexError):
        if ops:
            st.pop(ops[0], None)
    return st


def _join(a, b):
    """Two path states met at one instruction: a register keeps the union of its literals when both paths know it; a window
    slot a path did not set counts as INH on that path."""
    if a is None:
        return dict(b)
    out = {}
    for k in set(a) | set(b):
        if isinstance(k, tuple):
            out[k] = a.get(k, frozenset({(INH, None)})) | b.get(k, frozenset({(INH, None)}))
        elif k in a and k in b:
            u = a[k] | b[k]
            if len(u) <= MAX_LITS:
                out[k] = u
    return out


def flow(insns, fn, targets, win=None):
    """-> (in-state per instruction index, {address: index}): a forward data flow over the listing to a fixed point. Code no path
    from the entry reaches (a jump table's arms) starts from an unknown state."""
    idx = {x.addr: i for i, x in enumerate(insns)}
    n = len(insns)
    entered = {0}   # an instruction some branch or fall-through enters is never a seed (an edge an arm cuts stays cut)
    for i, x in enumerate(insns):
        if x.mn.rstrip('+-') not in NO_FALL:
            entered.add(i + 1)
        t = _target(x)
        if t is not None and t in idx:
            entered.add(idx[t])
    state = [None] * n
    pending = set()
    seed = 0
    while True:
        while seed < n and (state[seed] is not None or (seed in entered and seed != 0)):
            seed += 1
        if seed >= n:
            break
        state[seed] = {}
        pending.add(seed)
        while pending:
            i = min(pending)
            pending.discard(i)
            x = insns[i]
            out = _step(x, state[i], fn, targets, win)
            succ = []
            arm = (win[2] if win and len(win) > 2 else {}).get(x.addr - fn)   # a branch the kind's table reads one way (its 'arms')
            if x.mn.rstrip('+-') not in NO_FALL and i + 1 < n and arm != 'taken':
                succ.append(i + 1)
            t = _target(x)
            if t is not None and t in idx and arm != 'fall':
                succ.append(idx[t])
            for j in succ:
                m = _join(state[j], out)
                if m != state[j]:
                    state[j] = m
                    pending.add(j)
    return state, idx


def _lit(v):
    """frozenset of literals -> an int (one), the frozenset (several) or None (unknown)."""
    return None if not v else next(iter(v)) if len(v) == 1 else v


def calls(insns, fn, targets):
    """-> [(offset, target, r4)] for every `bl` to one of targets; r4 = an int, a frozenset of ints (paths that differ) or None
    (not a literal), tracked through li / lis / addi / ori / mr over every path to the call."""
    state, _idx = flow(insns, fn, targets)
    out = []
    for i, x in enumerate(insns):
        if x.mn != 'bl':
            continue
        try:
            tgt = int(x.ops.split(',')[0], 16)
        except (ValueError, IndexError):
            continue
        if tgt in targets:
            out.append((x.addr - fn, tgt, _lit((state[i] or {}).get('r4'))))
    return out


def _vals(v):
    """r4 as calls() gives it -> the list of its values (None = VAR)."""
    return sorted(v) if isinstance(v, frozenset) else [v]


def window(insns, w):
    """-> ({slot: [(value or None or INH, offset)]}, problems) for [lo, hi) of w['fn']. An `adds` window: every call in it, each
    value (a conditional set that joins the inherited value). A `set` window: per slot, what the LAST call of the window on each
    path to hi set (INH for a path that set nothing, a path from before lo straight to hi included): the state at hi. A path that
    leaves the window past hi is not read (it skips the draw at hi, or the table's hi is wrong: see Limits)."""
    fn, lo, hi = w['fn'], w['lo'], w['hi']
    got, problems = {}, []
    if w['mode'] == 'adds':
        for off, tgt, v in calls(insns, fn, WRAPPERS):
            if lo <= off < hi:
                got.setdefault(WRAPPERS[tgt], []).extend((k, off) for k in _vals(v))
    else:
        win = (lo, hi, w.get('arms', {}))
        state, idx = flow(insns, fn, WRAPPERS, win)
        if fn + hi in idx:
            end = state[idx[fn + hi]]
        else:   # hi is the function's end: what its last instruction hands on
            last = max((i for i, x in enumerate(insns) if x.addr < fn + hi), default=None)
            end = (_step(insns[last], state[last], fn, WRAPPERS, win)
                   if last is not None and state[last] is not None and insns[last].mn.rstrip('+-') not in NO_FALL else None)
        if end is None:
            problems.append(f"{hx(fn)} +{lo:X}..+{hi:X} ({w['why']}): no path reaches the window end +{hi:X}")
        else:
            in_window = [k for k in end if isinstance(k, tuple) and any(v != INH for v, _o in end[k])]
            for k in sorted(in_window):
                got[k[1]] = sorted(end[k], key=lambda vo: (str(vo[0]), vo[1] or 0))
    if w.get('slots'):   # S111 RSTCOVER-A / -B: a window that names its slots gives only those (an outer function's state the inner draw
        # inherits, or a leaf whose other states the kind reads elsewhere); a name that is no recorder slot is a PROBLEM, not a silent drop
        unknown = sorted(set(w['slots']) - set(WRAPPERS.values()))
        if unknown:
            problems.append(f"{hx(fn)} +{lo:X}..+{hi:X} ({w['why']}): slots {unknown} name no recorder slot ({', '.join(sorted(set(WRAPPERS.values())))})")
        got = {k: v for k, v in got.items() if k in w['slots']}
    if not got:
        problems.append(f"{hx(fn)} +{lo:X}..+{hi:X} ({w['why']}): no recorder call in the window")
    return got, problems


def reset_state(insns):
    """-> {field: (hardware value, offset)} from the default reset's device-setter calls."""
    out = {}
    for off, tgt, v in calls(insns, RESET_FN, RESET_SETTERS):
        if isinstance(v, int):
            out[RESET_SETTERS[tgt]] = (v, off)
    return out


def _gv():
    return {'vals': {}, 'var': [], 'inh': [], 'unset': True}


def game_state(kind, insns_of):
    """-> ({field: {'vals': {value: [where]}, 'var': [where], 'unset': bool}}, problems): the kind's state at its draw."""
    st, problems = {f: _gv() for f in FIELDS}, []
    raw = {}   # slot -> {'vals': {v: [where]}, 'var': [...]}
    for w in kind['game']:
        try:
            got, p = window(insns_of(w['fn']), w)
        except (CD.DiffError, Exception) as e:  # noqa: BLE001 - a listing that cannot be read is a PROBLEM, said
            problems.append(f"{hx(w['fn'])}: {e}")
            continue
        problems += p
        for slot, seq in got.items():
            r = raw.setdefault(slot, {'vals': {}, 'var': [], 'inh': [], 'set': False})
            r['set'] = r['set'] or w['mode'] == 'set'
            for v, off in seq:
                if v == INH:
                    r['inh'].append(f"{hx(w['fn'])} +{w['lo']:X}..+{w['hi']:X} (a path that sets none)")
                    continue
                where = f"{hx(w['fn'])} +{off:X}"
                if v is None:
                    r['var'].append(where)
                else:
                    r['vals'].setdefault(v, []).append(where)

    def put(field, v, where):
        g = st[field]
        g['unset'] = False
        g['vals'].setdefault(v, []).append(where)

    for slot, r in raw.items():
        for v, ws in r['vals'].items():
            for where in ws:
                if slot == 'zfunc_code':
                    put('zfunc', ZFUNC_CODE.get(v, f'code {v}'), where)
                elif slot == 'blend_code':
                    op, src, dst = BLEND_CODES.get(v, (f'code {v}',) * 3)
                    put('blend_op', op, where), put('blend_src', src, where), put('blend_dst', dst, where)
                elif slot == 'cull':
                    put('cull', 0 if v == 0 else 1, where)
                elif slot in st:
                    put(slot, v, where)
        fs = (('zfunc',) if slot == 'zfunc_code' else ('blend_op', 'blend_src', 'blend_dst') if slot == 'blend_code'
              else (slot,) if slot in st else ())
        for where in r['var']:
            for f in fs:
                st[f]['var'].append(where)
                st[f]['unset'] = False
        for where in r['inh']:
            for f in fs:
                st[f]['inh'].append(where)
    if kind.get('base'):
        try:
            rs = reset_state(insns_of(RESET_FN))
        except (CD.DiffError, Exception) as e:  # noqa: BLE001
            rs = {}
            problems.append(f'{hx(RESET_FN)}: {e}')
        for field, key, slot in (('zfunc', 'zfunc_hw', 'zfunc_code'), ('zwrite', 'zwrite', 'zwrite'), ('ztest', 'ztest', 'ztest')):
            r = raw.get(slot, {})
            if key in rs and (not r.get('set') or r.get('inh')):   # a set window replaces the reset on its paths; an adds window, or a path that sets none, joins it
                v, off = rs[key]
                put(field, v, f'{hx(RESET_FN)} +{off:X} (the default reset)')
                st[field]['inh'] = []
        if not rs:
            problems.append(f'{hx(RESET_FN)}: no depth setter read from the default reset')
    problems += RCB.fill(st, insns_of, [(w['fn'], w['hi']) for w in kind['game']])   # S111 RSTCOVER-B: a field no window sets (colour mask, stencil), read from every setter of it in the game
    return st, problems


COVER_DIRS = ('src/renderer_scene', 'src/renderer_host')


def ours_only(read):
    """-> ([(file, start line, why)], problems) for OURS_ONLY: the first `rdr::DrawState X{};` after each anchor."""
    out, problems = [], []
    for rel, anchor, why in OURS_ONLY:
        try:
            t = CD._mask(read(rel))
        except (OSError, RstDiffError) as e:
            problems.append(f'ours-only {rel}: {e}')
            continue
        a = re.search(anchor, t)
        sm = re.compile(START_RE).search(t, a.end()) if a else None
        if not sm:
            problems.append(f'ours-only {rel}: no `rdr::DrawState X{{}};` after /{anchor}/')
            continue
        out.append((rel, line_of(t, sm.start()), why))
    return out, problems


def uncovered(covered, read):
    """-> ['file:line'] of every `rdr::DrawState X{};` in COVER_DIRS that no kind's ours block starts at (our draws not compared)."""
    out = []
    for d in COVER_DIRS:
        for p in sorted(glob.glob(os.path.join(REPO, d.replace('/', os.sep), '*.*'))):
            if not p.endswith(('.cpp', '.h')):
                continue
            rel = os.path.relpath(p, REPO).replace(os.sep, '/')
            try:
                t = CD._mask(read(rel))
            except (OSError, RstDiffError):
                continue
            for m in re.finditer(START_RE, t):
                ln = line_of(t, m.start())
                if (rel, ln) not in covered:
                    out.append(f'{rel}:{ln}')
    return out


def effect_flags(res, flags):
    """-> rows for the effect passes that carry a compared state row: PASS-NEVER when the model draw never picks the pass."""
    out = []
    for eff, ti, p, field, v in (res or {}).get('rows', []):
        never = flags is not None and p not in CD.reachable_passes(eff, flags)
        out.append({'kind': f'effect {eff}', 'field': field, 'flag': 'PASS-NEVER' if never else 'UNSETTLED', 'ctx': f't{ti} p{p}',
                    'game': f'{field} = {v} in the effect file', 'ours': 'not compared (no effect-file state path of ours)',
                    'why': 'the model draw never picks this pass' if never else 'a per-pass state of the effect file: ours not read'})
    return out


# ------------------------------------------------------------------ the join
def _norm_ours(field, v):
    if not isinstance(v, int):
        return v
    if field == 'cull':
        return 0 if v == 0 else 1
    if field in ('blend_src', 'blend_dst'):
        return VK_FACTOR.get(v, f'vk {v}')
    if field == 'blend_op':
        return VK_OP.get(v, f'vk {v}')
    return v


def _show(field, v):
    if field == 'zfunc' and isinstance(v, int):
        return CMP.get(v, str(v))
    if field == 'cmask' and isinstance(v, int):
        return f'0x{v:X}'
    return str(v)


def judge(kind, field, g, o, gst, ost):
    """-> (flag, why)."""
    if field in ('zfunc', 'zwrite') and set(gst['ztest']['vals']) == {0} and not gst['ztest']['var'] and not gst['ztest'].get('inh'):
        return 'OK', 'moot: the game does not test depth here (the ztest row carries any difference)'
    if field in ('blend_op', 'blend_src', 'blend_dst') and set(gst['blend']['vals']) == {0} and not gst['blend']['var'] and not gst['blend'].get('inh'):
        return 'OK', 'moot: the game does not blend here (the blend row carries any difference)'
    if field in ('zfunc', 'zwrite') and set((ost.get('ztest') or {}).get('vals') or {None}) == {0}:
        return 'OK', 'moot: ours does not test depth here (the ztest row carries any difference)'
    if field in ('blend_op', 'blend_src', 'blend_dst') and set((ost.get('blend') or {}).get('vals') or {None}) == {0}:
        return 'OK', 'moot: ours does not blend here (the blend row carries any difference)'
    ov = {_norm_ours(field, v) for v in o['vals']}
    rules = sorted(v for v in ov if isinstance(v, str) and v.startswith('RULE'))
    if g['unset']:
        return 'UNSETTLED', 'the game draw does not set it: inherited, the setter not read'
    if rules:
        return 'UNSETTLED', f'ours is a rule of ours ({rules[0][6:]})'
    if g['var']:
        if ov == {GAME}:
            return 'OK', 'both follow the game\'s own word'
        return 'UNSETTLED', f"the game's value comes from data ({', '.join(g['var'])}); ours " + ('partly follows the game' if GAME in ov else 'a literal')
    if GAME in ov:
        return ('OK', "ours follows the game's own word") if ov == {GAME} else ('UNSETTLED', 'ours partly follows the game, partly literal')
    gv = set(g['vals'])
    if field == 'zfunc' and kind.get('mirror'):
        gv = {MIRROR.get(v, v) for v in gv}
    miss, extra = gv - ov, ov - gv
    if g.get('inh') and not miss:   # some path keeps a value from before the draw that no window reads
        return 'UNSETTLED', f"on a path the game draw keeps an inherited value ({', '.join(g['inh'])}): its setter not read"
    if gv == ov:
        return 'OK', ''
    bits = []
    if miss:
        bits.append('the game has ' + ', '.join(_show(field, v) for v in sorted(miss, key=str)) + ' (ours never)')
    if extra:
        bits.append('ours has ' + ', '.join(_show(field, v) for v in sorted(extra, key=str)) + ' (the game never)')
    return 'DIFF', '; '.join(bits)


def fmt_game(field, g, mirror):
    parts = []
    for v, ws in sorted(g['vals'].items(), key=lambda kv: str(kv[0])):
        fw = f' (ours-side {CMP.get(MIRROR.get(v, v))})' if field == 'zfunc' and mirror and isinstance(v, int) and MIRROR.get(v, v) != v else ''
        parts.append(f"{_show(field, v)}{fw} @{', '.join(ws)}")
    parts += [f"VAR @{w}" for w in g['var']]
    parts += [f"inherited @{w}" for w in g.get('inh', [])]
    return '; '.join(parts) or 'not set (inherited)'


def fmt_ours(field, o):
    parts = [f"{_show(field, _norm_ours(field, v))} @{', '.join(sorted(set(ws)))}" for v, ws in sorted(o['vals'].items(), key=lambda kv: str(kv[0]))]
    return '; '.join(parts) + (f"  [conditional: {'; '.join(o['cond'])}]" if o['cond'] else '')


def build_rows(kinds, insns_of, read, covered=None):
    rows, problems = [], []
    for k in kinds:
        gst, p = game_state(k, insns_of)
        problems += [f"{k['kind']}: {x}" for x in p]
        for o in k['ours']:
            try:
                ost, start = ours_state(k, o, read)
            except (RstDiffError, CD.DiffError) as e:
                problems.append(f"{k['kind']}: {e}")
                continue
            if covered is not None:
                covered.add((o['file'], start))
            for f in k['fields']:
                flag, why = judge(k, f, gst[f], ost[f], gst, ost)
                rows.append({'kind': k['kind'], 'ctx': o.get('ctx', ''), 'field': f, 'flag': flag, 'why': why,
                             'game': fmt_game(f, gst[f], k.get('mirror')), 'ours': fmt_ours(f, ost[f]),
                             'game_vals': sorted(map(str, gst[f]['vals'])), 'ours_vals': sorted(map(str, ost[f]['vals']))})
    return rows, problems


def insns_reader(con):
    cache = {}

    def insns_of(a):
        if a not in cache:
            cache[a] = CD._insns(con, a)
        return cache[a]
    insns_of.callers_of = RCB.callers_reader(con)   # S111 RSTCOVER-B: the setters of an inherited state (q's call edges), and their census
    insns_of.inherit_cache = {}
    return insns_of


def run(con, a, read=None, kinds=None, insns_of=None):
    kinds = kinds or KINDS
    if a.kind:
        kinds = [k for k in kinds if fnmatch.fnmatchcase(k['kind'], a.kind)]
        if not kinds:
            raise RstDiffError(f"no kind matches {a.kind!r} (kinds: {', '.join(k['kind'] for k in KINDS)})")
    read = read or default_reader(a.ref)
    covered = set()
    rows, problems = build_rows(kinds, insns_of or insns_reader(con), read, covered)
    fx, ferr = effect_rows()
    flags, _fe = CD.load_pass_flags(con) if fx and fx['rows'] else (None, None)
    rows += effect_flags(fx, flags)
    sel = [r for r in rows if not a.flag or r['flag'] in a.flag]
    if a.json:
        print(json.dumps({'rows': sel, 'effect_files': fx, 'problems': problems}, indent=1, default=str))
    else:
        cur = None
        for r in sel:
            if (r['kind'], r['ctx']) != cur:
                cur = (r['kind'], r['ctx'])
                k = next((x for x in kinds if x['kind'] == r['kind']), None)
                print(f"== {r['kind']}" + (f" ({r['ctx']})" if r['ctx'] else '') + (f"  {k['what']}" if k else ''))
            print(f"{r['flag']:10s} {r['field']:9s} game {r['game']}")
            print(f"{'':21s}ours {r['ours']}" + (f"  -- {r['why']}" if r['why'] else ''))
        tot = {f: sum(1 for r in sel if r['flag'] == f) for f in FLAGS}
        print(f"totals: {len(sel)} rows (of {len(rows)}); " + ', '.join(f'{f} {tot[f]}' for f in FLAGS))
        d = {}
        for r in sel:
            if r['flag'] == 'DIFF':
                d.setdefault((r['kind'], r['field'], r['why']), []).append(r['ctx'] or '-')
        print(f'distinct DIFF: {len(d)}')
        for (kd, f, why), ctxs in sorted(d.items()):
            print(f"  {kd} {f}: {why}  ({', '.join(ctxs)})")
        if fx:
            print(f"effect files: {fx['passes']} passes; {fx['fill_only']} set only fill mode (row 13), {fx['empty']} set nothing, "
                  f"{len(fx['rows'])} compared rows, {fx['other']} other rows: " + ('no pass carries blend / alpha / depth / cull state' if not fx['rows'] else 'see the effect rows'))
        else:
            print(f'effect files: not read ({ferr}): a per-pass state would go unseen')
        print(f'sources: the recorder calls of each kind\'s windows (q decomp offsets), the default reset sub_{RESET_FN:08X}; ours '
              + ', '.join(sorted({o['file'] for k in kinds for o in k['ours']})) + (f' at {a.ref}' if a.ref else ''))
        print('not compared: alpha test / ref (ours carries them in the push block system[8] / [9]), the alpha-channel blend factors, '
              'stencil ops; the scene classes compare z test / func only (their blend, cull and z write are per-part rules of ours)')
        if not a.kind:
            oo, op = ours_only(read)
            problems += op
            un = uncovered(covered | {(f, ln) for f, ln, _w in oo}, read)
            print(f"our DrawState blocks no kind compares ({', '.join(COVER_DIRS)}): {len(un)}: {', '.join(un) or 'none'}")
            print(f"our DrawState blocks with no game draw (OURS_ONLY): {len(oo)}: " + ('; '.join(f'{f}:{ln} ({w})' for f, ln, w in oo) or 'none'))
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


def main(argv, con):
    ap = argparse.ArgumentParser(prog='q rstdiff', description=__doc__.split('\n\n')[0])
    ap.add_argument('--kind', help='a draw kind or a glob (scene_*)')
    ap.add_argument('--flag', nargs='+', choices=FLAGS, help='only rows of these flags')
    ap.add_argument('--ref', help='read our code at this git ref (git show) instead of the working tree')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        return run(con, a)
    except (RstDiffError, CD.DiffError) as e:
        print(f'q rstdiff: {e}', file=sys.stderr)
        return 2

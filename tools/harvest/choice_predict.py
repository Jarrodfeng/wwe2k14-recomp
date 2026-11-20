#!/usr/bin/env python3
r"""S103 lane P: the BLIND PREDICTOR of the decompilation experiment (docs\agents\s103\decomp\PLAN.md; Fable review Q4).

Predicts, per (model file, mesh), the game's colour-pass CHOICE from the player's own files and the claim rows ONLY (out\s103\decomp\claims_*.jsonl:
R1-R6, V and the main session's R5 correction). It never opens a harvested choice table or a capture: it was written without seeing one.

  py -I tools\harvest\choice_predict.py ARENA [--look entrance|match] [--set NAME=VALUE ...] [--out OUT.json]
  py -I tools\harvest\choice_predict.py ARENA --branches      every branch, its claim and instruction range, and how many rows took each outcome
  py -I tools\harvest\choice_predict.py ARENA --assumed       the named inputs the claims leave [U] (default and the claim that supports it)
  py -I tools\harvest\choice_predict.py --lint                Q4.4 lints on this file's rule code
  py -I tools\harvest\choice_predict.py [ARENA] --plant       planted twins: every flag / input flipped must change the output (synthetic records + the arena)

Output has the choice table's shape (tools\harvest\choice_harvest.py docstring):
  { format, scene:"predict", arena, looks:{look:{captures:[], frames:0}}, meshes:[{file:"bgNN.pac#slot", slot, mesh, effect, tex, looks:{look:{draws:0, ambiguous:0,
    choices:[CHOICE]}}, game:{...}}], assumed:{...} }
  CHOICE = { vs, ps, defines, blend, wmask, alpha, slots, vtex, vstreams, n:1 }, plus `game` = the choice in the game's own terms (effect alternative, technique, pass,
  render / sampler states, cull) for the scorer.
Rule code is the functions named rule_*: every branch there calls br(ID, cond); BRANCHES[ID] holds the claim id and the instruction range it was read from.
Nothing from the game is written: names, small integers and enum names only."""
import argparse
import ast
import collections
import copy
import glob
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))

FORMAT = 1
FX_DIR = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')

# ---- branches: id -> (claim, instruction range, what) ----------------------------------------------------------------------------
BRANCHES = {
    'alias':        ('R3 key substitution / V(2)', 'sub_82BDB460 0x82BDC354-0x82BDC40C', 'record word +100 > 1 and the key matches an alias entry -> key replaced, arr392 skipped'),
    'arr384':       ('R3 model+384 / R1 rec[name]', 'sub_82BDB460 0x82BDC320-0x82BDC444', 'own effect = registry lookup of the lowercased record +0x7C name'),
    'arr388_twin':  ('R3 model+388 / V(3)', 'sub_82BDB460 0x82BDC554-0x82BDC668', "'<name>LM' (LM at min(len,13)) resolves -> twin, else copy of arr384"),
    'arr392_reg2':  ('R3 model+392', 'sub_82BDB460 0x82BDC678-0x82BDC744', 'second registry non-null and no substitution and the name resolves there, else copy of arr384'),
    'arr396_gear':  ('R3 model+396 / R2 model+608', 'sub_82BDB460 0x82BDC444-0x82BDC538', 'ych_cloth -> ych_gear, ych_cloth_flex -> ych_gear_flex, and model+608 = 1'),
    'sel_388':      ('R3 colour-pass selector / V', 'sub_82C04DF0 0x82C05904-0x82C059E8', 'selector word +1184 != 0 -> arr388'),
    'sel_392':      ('R3 colour-pass selector / V', 'sub_82C04DF0 0x82C05998-0x82C059E4', 'model+604 != 0 -> arr392'),
    'sel_396':      ('R3 colour-pass selector / V', 'sub_82C04DF0 0x82C05998-0x82C059E4', 'model+608 != 0 and model+612 != 0 -> arr396'),
    'stage_sel':    ('R7 LM rule / R3 selector setters / R10 class1', 'sub_824E9E10 0x824EA038-0x824EA080', 'stage draw: selector 0 for every slot when the phase word 0x836F3940 == 1; else 1 iff slot record +10 bit 0x40 is clear (no x / xe token)'),
    'sel_cycled':   ('R8 cycled slot / UV: the cycle task never sets the selector word', 'sub_824E82A0 0x824E8554-0x824E8578; sub_83420EA0', 'a slot of a 60010-60025 cycle group gets the no-pass bit and is drawn by the cycle task, which never sets +1184: it draws with the outer selector (0 on every sealed capture -> no_lm)'),
    'sel_group':    ('R10 class1 LM: selector only set for group-0 slots', 'sub_824E9E10 0x824EA038-0x824EA03C; sub_825B9C90 0x825B9DA4 / 0x825B9E14; sub_825B9970 0x825B9AB4', 'the per-slot selector is set only when r6 != 0: group 0 passes 1, groups 1-4 pass 0 and draw with the outer selector'),
    'shadow_p':     ('R10 class3 per-slot shadow = p token / R11 exact rule', 'sub_824E9E10 0x824E9F78-0x824E9F9C; call 0x824EA034; sub_824E8628 0x824E8C40-0x824E8C70', "the shadow task runs for a slot when rec+9 bit 0x04 (phase == 1) / bit 0x02 (else) is set; 'pg' 0x04, 'pe' 0x02, bare 'p' 0x06"),
    'shadow_reset': ('R11 shadow bit reset per slot (corrects R10 class3 stickiness)', 'sub_824E9E10 0x824EA268-0x824EA26C; sub_827E72F0 0x827E7304-0x827E731C', 'the bit is cleared after every slot draw: per slot, never inherited'),
    'shadow_gate':  ('R11 extra gates for a p slot', 'sub_824E9E10 0x824E9FA0-0x824EA034; sub_82699B50 0x82699B68-0x82699C54; sub_82BD5260', 'model+600 bit 0, or no vis object, or sub_82699B50(obj,0), or (r and sub_82699B50(obj,1))'),
    'skinned_slot': ('R10 class4 stage-slot model creator', 'sub_824ED698 0x824ED6F4-0x824ED724; sub_822AA2F8', 'descriptor +12 (model+540) = 1 iff slot in 390..397, or 81..88 when [stage+1216] is in 200..249'),
    'slot_parse':   ('R7 byte +10 bits / R8 slot record', 'sub_824E8628 0x824E8B20-0x824E8B28; 0x824E8C88-0x824E8DF4', 'entry 50001 token letters -> slot record bytes +6 / +7 / +8 / +9 / +10 (stage + 1220 + 12*slot)'),
    'stage_begin':  ('R8 stage pass begin', 'sub_825B9860 0x825B9888-0x825B98C4', 'z 1, blend enable 1, alpha test 1, alpha func code 5 (GEQUAL), ref 1, blend code 0'),
    'cycled':       ('R8 cycled slot groups / stage_visibility.md 3', 'sub_83420EA0 0x83421074-0x8342112C', 'a slot of a 60010-60025 cycle group: blend 1, alpha test 1, ref 0, func code 6 (GREATER), z write 1, blend code 0; a -> ref 1 + code 1; z -> z write 0'),
    'slot_s_ref':   ('R8 alpha test / ref per slot', 'sub_824E9E10 0x824EA0EC-0x824EA108', "'s' (rec+8 0x20) and rec+7 != 0 -> ALPHAREF = rec+7"),
    'slot_a_o':     ('R8 blend per slot', 'sub_824E9E10 0x824EA0CC-0x824EA130', "'a' (rec+8 0x40) or 'o' (rec+10 0x04) -> blend code 1, ref 1, func code 5"),
    'slot_z':       ('R8 z write per slot', 'sub_824E9E10 0x824EA134-0x824EA154', "'z' (rec+8 0x08) -> z write 0 for the draw"),
    'draw_group':   ('R8 slot draw groups', 'sub_824E7190 0x824E71C8-0x824E72A4', 'group 0 none of s a i j o; 1 s; 2 a; 3 i; 4 j or o; rec+8 0x80, rec+9 0x80 clear or rec+9 0x08 -> not drawn by these passes'),
    'blend_code':   ('R8 blend code (method 4)', 'sub_82C23168 0x82C231B0-0x82C23398', 'blend code -> (BLENDOP, SRCBLEND, DESTBLEND); other codes record nothing'),
    'afunc_code':   ('R8 alpha func code (method 22)', 'sub_82C233B8 0x82C23404-0x82C2346C', 'alpha func code -> ALPHAFUNC value'),
    'rec0':         ('R1 rec+0 / V', 'sub_82BDA038 0x82BDA678-0x82BDA7A0', 'rec+0 = 0 for the 9 ybg default / bumpmap / reflect (+lm, +rdc) names, else 1 (R1 rec[all] 0x82BDBFA0-0x82BDBFC8)'),
    'rec4':         ('R1 rec+4 / V', 'sub_82BDA038 0x82BDA7E8-0x82BDAD88', 'rec+4 = 1 for the 14 ych_*, the 9 ybg names, yprop_psm, yprop_psmlm'),
    'rec8':         ('R1 rec+8 / V', 'sub_82BDA038 0x82BDAAB4-0x82BDAB18', 'rec+8 = 1 for ybg_psm, yprop_psm, yprop_psmlm'),
    'rec12':        ('R1 rec+12 / V(4)', 'sub_82BDA038 0x82BDAB1C-0x82BDACC4', 'rec+12 = 1 for the 13 ych_* except ych_aniso_trans'),
    'rec16':        ('R1 rec+16 / V', 'sub_82BDA038 0x82BDACC8-0x82BDAD0C', 'rec+16 = pointer to the g_mSpecPowPrm value word when found and non-zero (rec+12 names only)'),
    'rec20':        ('R1 rec+20 / V', 'sub_82BDA038 0x82BDAD10-0x82BDAD48', 'rec+20 = 1 for ych_aniso_trans'),
    'pass_base':    ('R9 pass value', 'sub_82C04DF0 0x82C0585C-0x82C058AC', 'base = 1 iff model+540 == 1, else 0 ([sp+328])'),
    'p4f88_rec12':  ('review 1.2 sub_82BD4F88 / R9 pass value', 'sub_82BD4F88 0x82BD4F88-0x82BD4FEC', 'rec+12 == 0 -> base'),
    'p4f88_616':    ('review 1.2 sub_82BD4F88', 'sub_82BD4F88 0x82BD4FB0', '[model+616]: 1 -> +4; 0 -> base when rec+16 points at a word >= 0 (signed) else +8; 2 -> +8; >= 3 -> base'),
    'p5ee0_bit0':   ('review 1.2 sub_82BD5EE0 / R2 model+540', 'sub_82BD5EE0 (49 insns)', 'rec+0 == 0 and [model+540] == 1 -> clear bit 0'),
    'p5ee0_shadow': ('R9 pass value / pass 2 = shadow / table setter 1 / R10 class3', 'sub_82BD5EE0 0x82BD5F10-0x82BD5F44; sub_827E8270 0x827E8294-0x827E82BC', 'rec+4 != 0 and the context table 0x83929A58[ctx] != 0 at the slot draw -> +2'),
    'p5ee0_camera': ('review 1.2 sub_82BD5EE0 / R2 record+1164', 'sub_82BD5EE0; sub_82BCC3B0 0x82BCC3B0-0x82BCC3F0', 'rec+8 != 0 and camera word +1164 != 0 -> +4'),
    'p5ee0_aniso':  ('review 1.2 sub_82BD5EE0 / R2 model+620', 'sub_82BD5EE0 0x82BD5F88', 'rec+20 != 0 and [model+620] == 1 -> +16'),
    'tech_range':   ('review 1.1 sub_82BAEFD8', 'sub_82BAEFD8 (40 insns)', 'technique >= [effect+24] -> "Invalid Effect Technique Index": nothing applied'),
    'pass_entry':   ('review 1.1 sub_82BAEFD8 / R4 pass[+8]', 'sub_82BAEFD8; sub_82BAE370 0x82BAE430-0x82BAE43C', 'technique entry, pass word (pass + 4) -> the pass block -> shader pair'),
    'rs_group':     ('R4 pass[+12] / pass value sources', 'sub_82BAE370 0x82BAE4B4-0x82BAE5AC', 'render-state entries: group 1 effect value slot, group 2 float vector, group 3 immediate'),
    'rs_apply':     ('R4 pass apply gate', 'sub_82BAEFD8 0x82BAF040-0x82BAF05C', 'both state blocks applied unconditionally on the colour route'),
    'rs_inherit':   ('R4 blend / alpha test / write mask', 'sub_82C04DF0 0x82C05044 / 0x82C0C9F4', 'a state the pass block does not set keeps the value the colour pass found at entry'),
    'ss_group':     ('R4 pass[+16]', 'sub_82BAE370 0x82BAE5CC-0x82BAE6D4', 'sampler-state entries (sampler, id, value) from the pass block'),
    'blend_off':    ('B_d3d_api 3.2 idx 15 / reader.h DecodeBlend', 'setter 0x82CA9050', 'blend enable 0 -> control ONE/ZERO -> off'),
    'cull_single':  ('R4 cull per mesh / V batch 2 (4)', 'sub_82C04DF0 0x82C0A880-0x82C0A914; sub_82BD38B8 0x82BD38D4-0x82BD38F8', 'record word +0x8C bit 0x2: one sub-pass, cull on; clear: t = [model+276+4k] (k outside 0..1 -> 0; null slot -> [model+276]): 0 one sub-pass cull off, 1 two (mode 2 then 1), other: not drawn'),
    'cull_copy':    ('SCU / CUL_ADV D', 'sub_82C04DF0 0x82C0A918-0x82C0A930 (r26); 0x82C0B920-0x82C0B968 (swap, draw 0x82C0B94C, restore)', 'an `r` slot\'s mirrored copy of a bit-set record: the winding swapped (r26 = 2 if the entry is 1, else 1) around its draw'),
    'draw_gate':    ('V batch 2 (4)', 'sub_82C04DF0 0x82C0A918-0x82C0A968 (draw 0x82C0A950)', 'drawn only if [model+832] != 0 or [model+624] != 0'),
    'slot_fixed':   ('R5 tex.fixed', 'sub_82BD5CE8/DC0/E58 0x82BD5D80; 0x82BD5E18; 0x82BD5EA0', 'slots 12 / 13 / 15 bound from the shared object, not the material'),
    'slot_mat':     ('R5 tex.slot / tex.table / tex.value', 'sub_82C04DF0 0x82C08108-0x82C08134; 0x82C080CC-0x82C080E8', 'a material slot: the mesh parameter value indexes the model texture table (the file textures)'),
    'stream_decl':  ('R5 stream.decl', 'sub_82BDED40 0x82BDEDB4-0x82BDEE88', 'streams = the vertex declaration of the record +96 entry'),
}

# ---- the game's own names (cited; no other name list is allowed in rule code) --------------------------------------------------
# R1 rec+0 / V (0x82BDA678-0x82BDA7A0): the 9 names whose rec+0 is 0
KEYS_REC0 = ('ybg_default', 'ybg_defaultlm', 'ybg_defaultrdc', 'ybg_bumpmap', 'ybg_bumpmaplm', 'ybg_bumpmaprdc', 'ybg_reflect', 'ybg_reflectlm', 'ybg_reflectrdc')
# R1 rec+4 / V (0x82BDA7E8-0x82BDAD88): the 14 ych_* names
KEYS_YCH = ('ych_skin', 'ych_skin_flex', 'ych_trans', 'ych_trans_tiling', 'ych_grar', 'ych_grar_flex', 'ych_gear', 'ych_gear_flex', 'ych_cloth', 'ych_cloth_flex',
            'ych_anisotropic', 'ych_aniso_trans', 'ych_reflect', 'ych_ls')
KEYS_REC4 = KEYS_YCH + KEYS_REC0 + ('yprop_psm', 'yprop_psmlm')
# R1 rec+8 / V (0x82BDAAB4-0x82BDAB18)
KEYS_REC8 = ('ybg_psm', 'yprop_psm', 'yprop_psmlm')
# R1 rec+12 / V(4) (0x82BDAB1C-0x82BDACC4): ych_* minus ych_aniso_trans
KEYS_REC12 = tuple(k for k in KEYS_YCH if k != 'ych_aniso_trans')
# R1 rec+20 / V (0x82BDAD10-0x82BDAD48)
KEY_REC20 = 'ych_aniso_trans'
# R3 model+396 (0x82BDC444-0x82BDC538): strings 0x8206B190 / 0x8206B1AC, 0x8206B180 / 0x8206B19C
GEAR_TWIN = {'ych_cloth': 'ych_gear', 'ych_cloth_flex': 'ych_gear_flex'}
# R1 rec+16 (0x82BDACC8-0x82BDAD0C): string 0x8201E958
SPECPOW_NAME = 'g_mSpecPowPrm'
# R8 slot record / R7 byte +10 bits (sub_824E8628, jump table 0x824E8948 by letter - 'a'): token letter -> (record byte, bit)
SLOT_LETTER = {'a': (8, 0x40), 's': (8, 0x20), 'k': (8, 0x10), 'z': (8, 0x08), 'n': (8, 0x04), 'r': (8, 0x02), 'y': (8, 0x01),
               'e': (6, 0x01), 'h': (6, 0x04), 'g': (6, 0x08), 'c': (9, 0x40), 'i': (10, 0x10), 'j': (10, 0x08), 'o': (10, 0x04), 'u': (10, 0x01)}
SLOT_X = {'x': 0xC0, 'xg': 0x80, 'xe': 0x40}          # R7 byte +10 bits: 0x824E8CE4 / 0x824E8CC4 / 0x824E8CD8 (rec+10)
SLOT_W_BARE = (6, 0x02)                                # R8: 'w' sets rec+6 0x02 (bare token only)
SLOT_P = {'pg': 0x04, 'pe': 0x02, 'p': 0x06}          # R10 class3: parser case 'p' loc_824E8C08 (0x824E8C40-0x824E8C70), rec+9
SKINNED_SLOTS = (390, 397)                             # R10 class4: sub_824ED698 0x824ED6F4-0x824ED724
SKINNED_SLOTS_200 = (81, 88)                           # R10 class4: when sub_822AA2F8([stage+1216]) (200..249)
STAGE_1216_RANGE = (200, 249)                          # R10 class4: sub_822AA2F8
# R8 blend code (sub_82C23168 0x82C231B0-0x82C23398): code -> (BLENDOP, SRCBLEND, DESTBLEND), 360 D3DBLEND / D3DBLENDOP values
BLEND_CODE = {0: (0, 6, 7), 1: (0, 6, 1), 2: (4, 6, 1), 3: (0, 0, 4), 4: (0, 9, 0), 5: (0, 1, 0), 6: (1, 1, 10), 10: (0, 1, 1), 11: (3, 1, 1), 12: (0, 7, 6)}
# R8 alpha func code (sub_82C233B8 0x82C23404-0x82C2346C): code -> ALPHAFUNC value
PLANT_NO_SELCYC = False   # planted twin only: the cycled-slot selector branch disabled (the 4 sealed-set-2 UVScroll misses must come back)
PLANT_STICKY = False   # planted twin only: R10's refuted carry-over reading (--plant must see it change the output)
AFUNC_CODE = {0: 0, 1: 7, 2: 1, 3: 3, 4: 2, 5: 6, 6: 4, 7: 5}

# render-state ids = byte offsets into the device's setter table (R4 pass[+12]: [device + id + 64]; docs\native\d3d\B_d3d_api.md 3.2 "Off" column)
RS = {'fill': 52, 'cull': 56, 'blend_en': 60, 'blend_sep': 64, 'src': 72, 'dst': 76, 'op': 80, 'asrc': 84, 'adst': 88, 'aop': 92,
      'atest': 96, 'aref': 100, 'afunc': 104, 'wmask': 212}
RS_NAME = {v: k for k, v in RS.items()}
# sampler-state ids (B_d3d_api.md 3.3 "Off"): 0 / 4 / 8 wrap U V W, 16 / 20 min / mag, 24 mip
SS_NAME = {0: 'addr_u', 4: 'addr_v', 8: 'addr_w', 12: 'border', 16: 'min', 20: 'mag', 24: 'mip'}
# Xenos blend factor / op -> VkBlendFactor / VkBlendOp (src\renderer_adapter\reader.h VkBlendFactor / VkBlendOp; B3 / B4)
XB_FACTOR = [0, 1, 0, 0, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14]
XB_OP = [0, 1, 3, 4, 2]
# the harvest's printed names (choice_harvest.py BLEND_FACTOR / BLEND_OP / COMPARE)
VK_FACTOR = ['ZERO', 'ONE', 'SRC_COLOR', 'ONE_MINUS_SRC_COLOR', 'DST_COLOR', 'ONE_MINUS_DST_COLOR', 'SRC_ALPHA', 'ONE_MINUS_SRC_ALPHA', 'DST_ALPHA', 'ONE_MINUS_DST_ALPHA',
             'CONST_COLOR', 'ONE_MINUS_CONST_COLOR', 'CONST_ALPHA', 'ONE_MINUS_CONST_ALPHA', 'SRC_ALPHA_SATURATE']
VK_OP = ['ADD', 'SUBTRACT', 'REVERSE_SUBTRACT', 'MIN', 'MAX']
COMPARE = ['NEVER', 'LESS', 'EQUAL', 'LEQUAL', 'GREATER', 'NOTEQUAL', 'GEQUAL', 'ALWAYS']

# ---- named inputs the claims leave [U] -----------------------------------------------------------------------------------------
ASSUMED = {
    'alias_table':      ({}, 'R3 key substitution / V(2): the 3 entries at 0x83629CCC are unread; default = no substitution'),
    'registry':         ('_startup.pac', 'R1 rec[name] / R3 model+384: registry [0x83929A88] contents unread; default = the effects of _startup.pac key 0 (docs\\formats\\effect_techniques.md)'),
    'registry2':        (False, 'R3 model+392: [0x83929A94] contents unread; default = absent (arr392 = copy of arr384); only read when model+604 != 0'),
    'phase_word':       ({'entrance': 4, 'match': 1, 'match_scene': 4}, 'R7 r19 / R10 class2 writers: 0x836F3940 is 4 while the scene manager plays a scene of type 1-9 (sub_8231D4B8 0x8231D4D0; sub_8231D0D0 1 only for type 10), 1 otherwise in a match. Per look: entrance 4, match 1, match_scene 4 (an in-match scene: victory, in-match scene kinds); a "match" capture frame that is really a scene is a known limit'),
    'outer_selector':   ({'entrance': 0, 'match': 0, 'match_scene': 0}, 'R10 class1 [L]: groups 1-4 (s a i j o) draw with the selector in force at stage-pass begin; its writer is the frame outer context (candidate: the crowd draw sub_82513A38 sets 1, restored only in sub_82513530) [U]; default 0 per look'),
    'shadow_module':    (1, 'R11 shadow task body / R9 table setter 1: the task writes 1 only when [0x83924748] exists and its +12 != 0; default 1 (active)'),
    'shadow_gate_pass': (1, 'R11 extra gates: model+600 bit 0 (writer [U], likely the model file header) OR no vis object OR sub_82699B50(obj,0) (1 when [module+292] == 0, node tests at 1 / 2, 0 at >= 3; [module+292] and the vis state are [U]) OR (r and sub_82699B50(obj,1)); default 1 (passes)'),
    'stage_1216':       ('arena', 'R10 class4 [L]: [stage+1216] is the arena id (sub_822AA2F8 range 200..249); default = the bg pac number'),
    'rec9_bit80':       (1, 'R8 slot draw groups: a slot is drawn only with rec+9 bit 0x80 set; no token sets it and the writer is unread; default = set (drawn)'),
    'prop_state':       ('stage', 'R8: every bg pac model is a stage slot (entries 0..398); a prop drawn outside the stage passes is assumed to get the same pass state'),
    'model540':         (None, 'R10 class4: read from the slot id (skinned_slot); a value here overrides it for every mesh (also the generic setter sub_82BCDCE8, 59 sites, is not traced)'),
    'model604':         (0, 'R2 model+604: init 0, only external setter ran in caw only; default 0'),
    'model612':         (1, 'R2 model+612: init 1, no later store found'),
    'model616':         (0, 'R2 model+616 / V batch 2 (1): for CHARACTER models a per-character input (sub_825A1D48 0x825A1DB0 / sub_825A1E38 0x825A1EA0 from sub_825A3908: [obj+5060], 3 -> 2 or 0 by the light set); arena / prop models: no setter found, 0 [L]'),
    'model620':         (0, 'R2 model+620 / V batch 2 (1): for CHARACTER models a per-character input (0x825A1E14 / 0x825A1F04: [obj+5068], 2 -> 1 or 0 by a vtable+116 call); arena / prop models: no setter found, 0 [L]'),
    'camera_1164':      ({'entrance': 0, 'match': 0, 'match_scene': 0}, 'R2 record+1164: only sub_82608CB0 sets 1 around its own draw; default 0 per look'),
    'inherited_state':  ({'blend_en': 0, 'blend_sep': 0, 'src': 1, 'dst': 0, 'op': 0, 'asrc': 1, 'adst': 0, 'aop': 0, 'atest': 0, 'aref': 0, 'afunc': 7, 'wmask': 15},
                         'R4 / R8: states neither the stage pass begin, the slot draw nor the pass block set (separate alpha, alpha-channel factors, colour write mask: "no setter found", R8) keep D3D defaults (mask 0xF)'),
    'blend_sep_off':    ('copy', 'B_d3d_api 3.2 idx 16 "flag 30 of +12284": with separate alpha off the alpha factors are taken = the colour factors [L, D3D9 rule]'),
    'cull_k':           (None, 'R4 cull per mesh / V batch 2 (4): the slot k = [[thread+1048]+32]; None = null [r13]+1048 (k = 0, t = [model+276]); k outside 0..1 -> t = 0; default None'),
    'draw_gate':        (1, 'V batch 2 (4): the mesh is drawn only if [model+832] != 0 or [model+624] != 0; writers unread; default 1 (drawn)'),
    'model_callbacks':  (None, 'V batch 2 (5): [model+316](model,[model+320]) per model and [model+324](model,[model+328]) per mesh; the one sub_825A3908 installs (characters) records command 44 only; stage / prop models: callbacks unread, assumed none'),
    'role_fixed':       ({12: 'target:ShadowMap', 13: 'map', 15: 'map'}, 'R5 tex.fixed: what the shared object at 0x83930000-8796 holds is [U]; default roles by guess of the slot 12 shadow sampler, listed'),
    'role_vtex':        ('map', 'vertex sampler slots (>= 16, fx.json): no claim names their source (the light-map page, arena_lightmap.md); default map'),
    'role_material':    ('file', 'R5 tex.table: a material slot binds an entry of the model texture table = the file textures [L]'),
    'slots_from_shader': (True, 'R5 tex.absent: a slot the material does not bind keeps its previous binding; predicted slots = the samplers the chosen pair reads (fx.json) only'),
    'param_bound':      (None, 'R4 pass value sources: a group 1 / 2 state value lives in an effect value slot [E+296]; where that lies in the file is [U]: such a state keeps the inherited value and is listed'),
}


def assumed_value(name, overrides):
    return overrides.get(name, ASSUMED[name][0])


# ---- named inputs a probe settled (no longer ASSUMED; a plant may still override them) ------------------------------------------
SETTLED = {
    'cull_t':          (0, 'SCU / CUL.md 1, CUL_ADV.md A [C]: t = [model+276+4k]; the model base constructor sub_82BD39C8 writes 0 (0x82BD3A54 / 0x82BD3A58); the only indexed setter '
                           'sub_82BD3900 is called from wrestler draw helpers (sub_8259E008, sub_8259FAE8) and sub_83368C90 (a character draw method): a stage model keeps t = 0'),
    'stage_zwrite_inherited': (1, 'SZW / ZW2 [C capture: 726 `s` draws in 20 captures of 13 arenas, both looks, z write 1 at fixed-state byte +9] + ZW3 [C: after the wrestler translucent node too]: sub_825B9860 (the group begin) never sets '
                                  'ZWRITEENABLE (no slot-18 call), the value is what the previous render node left, which is 1 on every path read; who sets the standing 1 at frame start is [U] (ZW1)'),
    'cull_entry_mode': (2, 'SCU / CUL.md 1, CUL_ADV.md B [C]: the winding at entry = recorder state +116: init 2 (sub_82C22F88, store 0x82C22FF8); the camera hand-off sub_823C79A8 sets 2 '
                           'for a camera slot with [+176] == 0 (0x823C7D68-0x823C7D74), 1 only for a mirrored camera (+176 set: no stage pass of ours draws through one, [U] whether a live slot sets it)'),
}


def settled_value(name, overrides):
    return overrides.get(name, SETTLED[name][0])


# ---- the branch recorder --------------------------------------------------------------------------------------------------------
class Branches:
    def __init__(self):
        self.taken = collections.defaultdict(collections.Counter)

    def br(self, bid, cond, label=None):
        if bid not in BRANCHES:
            raise KeyError('branch %s has no claim' % bid)
        self.taken[bid]['%s=%s' % (label, bool(cond)) if label is not None else bool(cond)] += 1
        return cond


# ---- the player's files ---------------------------------------------------------------------------------------------------------
def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def cstr(b, o):
    e = b.find(b'\0', o)
    return b[o:e].decode('ascii', 'replace')


class Effects:
    """The registry: lowercased effect name -> {name, body, parsed} from the player's _startup.pac (effect_table.py's reader)."""
    def __init__(self, rel='pac/_startup.pac'):
        import effect_table as et
        import pac
        import shaderlib
        self.fx = et.FxNames()
        self.by_lower = {}
        data = pac.load(os.path.join(shaderlib.GAME, rel.replace('/', os.sep)))
        for leaf, dec in pac.entries(data):
            table = shaderlib.effect_table(dec) if b'\x10\x2a\x11' in dec else None
            if not table:
                continue
            for name, off, size in table:
                blob = dec[off:off + size]
                body = blob[12:12 + u32(blob, 4)]
                self.by_lower.setdefault(name.lower(), dict(name=name, body=body, parsed=et.parse_effect(blob, self.fx, name)))

    def get(self, lower):
        return self.by_lower.get(lower)


def mesh_records(arena_id):
    """(slot, mesh) -> raw record fields the claims name: name16 +0x7C, word +0x8C, word +0x90, word +100 (0x64), params (+0x94 / +0x98)."""
    import arena_files as af
    raw = {}
    orig = af.parse_jboy

    def wrap(slot, blob, with_positions=True):
        out = orig(slot, blob, with_positions)
        body = blob[8:8 + af.be32(blob, 4)]
        mtab = af.be32(body, 0x14)
        for mi in range(af.be32(body, 0x10)):
            rec = mtab + mi * af.MESH_REC
            words = {}
            nparam, plist = af.be32(body, rec + 0x94), af.be32(body, rec + 0x98)
            params = {}
            for k in range(min(nparam, 512)):
                po = af.be32(body, plist + 4 * k)
                params[af._cstr(body[po:po + 16])] = af.be32(body, po + 20)
            raw[(slot, mi)] = dict(name=af._cstr(body[rec + 0x7C:rec + 0x7C + 16]), w8c=af.be32(body, rec + 0x8C), w90=af.be32(body, rec + 0x90),
                                   w64=af.be32(body, rec + 0x64), param_words=params)
        return out

    af.parse_jboy = wrap
    af._CACHE.clear()
    try:
        arena = af.load_arena(arena_id, with_positions=False)
    finally:
        af.parse_jboy = orig
        af._CACHE.clear()
    texts = slot_texts(arena_id)
    cyc = cycled_slots(arena_id)
    for (slot, mi), rec in raw.items():
        rec['slot_tokens'] = texts.get(slot, [])
        rec['cycled'] = slot in cyc
        rec['stage_1216'] = arena_id  # [R10 class4 L] [stage+1216] is the arena id (input stage_1216)
    return arena, raw


def _entries(arena_id, want):
    import stage_vis as sv
    return sv.read_entries(arena_id, want)


def slot_texts(arena_id):
    """Entry 50001 (the slot text, docs\\formats\\arena_slot_flags.md): slot -> its tokens (filecensus_flags.parse_slot_flags' line rule)."""
    e = _entries(arena_id, {50001})
    out = {}
    if 50001 in e:
        for line in e[50001].decode('latin1').replace('\r', '').split('\n'):
            m = re.match(r'\s*(\d+)\b(.*)$', line)
            if m:
                out.setdefault(int(m.group(1)), []).extend(m.group(2).split(';', 1)[0].split())
    return out


def cycled_slots(arena_id):
    """The slots of the time-cycle groups, entries 60010-60025 ((slot, ticks) pairs; stage_visibility.md section 3, stage_vis.py groups)."""
    out = set()
    for _k, d in _entries(arena_id, lambda x: 60010 <= x <= 60025).items():
        nums = [int(x) for x in re.findall(r'-?\d+', d.decode('cp932', 'replace'))]
        out.update(nums[0::2])
    return out


class FxJson:
    """Our fx.json: variant name -> defines, sampler slots, vertex fetch slots (a property of the shader, Q4.3)."""
    def __init__(self):
        self.v = {}
        for fn in sorted(glob.glob(os.path.join(FX_DIR, '*.fx.json'))):
            d = json.load(open(fn, encoding='utf-8'))
            for stage in ('vertex', 'fragment'):
                for v in d.get(stage, []):
                    nm = '%s_%s_%s' % (d['effect'], v['name'], stage)
                    self.v[nm] = dict(defines=list(v.get('defines', [])), samplers=[s['slot'] for s in v.get('samplers', [])],
                                      fetch=[a['fetch'] for a in v.get('attributes', [])])

    def get(self, nm):
        return self.v.get(nm or '', dict(defines=[], samplers=[], fetch=[]))


# ---- rule code (lints apply to every rule_* function) ---------------------------------------------------------------------------
def rule_effect_arrays(B, rec, reg, inp):
    """-> (arr384, arr388, arr392, arr396 lowercased effect names or None, model608, substituted)."""
    key = rec['name'].lower()  # [R1 rec[name]] key = record +0x7C lowercased in place
    sub = False
    alias = assumed_value('alias_table', inp)
    if B.br('alias', rec['w64'] > 1 and key in alias):  # [R3 key substitution] 0x82BDC354-0x82BDC40C, record word +100 > 1
        key, sub = alias[key], True
    a384 = key if B.br('arr384', reg.get(key) is not None) else None  # [R3 model+384] 0x82BDC41C-0x82BDC43C
    twin = (key[:13] + 'lm')  # [R3 model+388] 'L','M',0 at position len, capped to 13; resolver lowercases (sub_82BAF4D0)
    a388 = twin if B.br('arr388_twin', reg.get(twin) is not None) else a384  # [R3 model+388 / V(3)] fallback = copy of arr384
    a392 = a384
    if B.br('arr392_reg2', assumed_value('registry2', inp) and not sub):  # [R3 model+392] 0x82BDC678-0x82BDC744
        a392 = a384  # second registry contents [U]: same-name lookup assumed to give the same effect
    a396, m608 = a384, 0
    if B.br('arr396_gear', key in GEAR_TWIN):  # [R3 model+396 / R2 model+608] 0x82BDC444-0x82BDC538
        a396, m608 = GEAR_TWIN[key], 1
    return a384, a388, a392, a396, m608, sub


def rule_slot_record(B, tokens):
    """-> {6, 7, 8, 9, 10: byte} of the 12-byte slot record ([R8 slot record] sub_824E8628 0x824E8B20-0x824E8B28)."""
    r = {6: 0, 7: 0, 8: 0, 9: 0, 10: 0}
    for t in tokens:
        if B.br('slot_parse', t in SLOT_X, 'x token'):  # [R7 byte +10 bits] 0x824E8C88-0x824E8DF4
            r[10] |= SLOT_X[t]
        elif B.br('shadow_p', t in SLOT_P, 'p token'):  # [R10 class3 per-slot shadow] loc_824E8C08 0x824E8C40-0x824E8C70
            r[9] |= SLOT_P[t]
        elif B.br('slot_parse', t[:1] == 't', 't number'):  # [R8 slot record] case 0x824E8B00: rec+7 = the number after t
            m = re.search(r'\d+', t)
            r[7] = int(m.group(0)) & 0xFF if m else 0  # [R8 slot record] stb 7 (a byte)
        elif B.br('slot_parse', t == 'w', 'bare w'):  # [R8 slot record] w sets rec+6 0x02, bare token only
            r[SLOT_W_BARE[0]] |= SLOT_W_BARE[1]
        elif B.br('slot_parse', t[:1] in SLOT_LETTER, 'letter'):  # [R8 slot record] jump table 0x824E8948, letter - 'a'
            byte, bit = SLOT_LETTER[t[:1]]
            r[byte] |= bit
    return r


def rule_selector(B, srec, group, look, inp, cycled=False):
    """The stage draw's selector word +1184 ([R7 LM rule] sub_824E9E10 0x824EA038-0x824EA080)."""
    if B.br('sel_cycled', cycled and not PLANT_NO_SELCYC, 'cycled slot'):  # [R8 cycled slot groups / UV] sub_824E82A0 0x824E8554-0x824E8578 sets the no-pass bit; the cycle task sub_83420EA0 never sets +1184: outer selector
        return assumed_value('outer_selector', inp)[look]
    if B.br('sel_group', group != 0, 'group 1-4'):  # [R10 class1] r6 = 0 from sub_825B9970 0x825B9AB4: the setter is skipped
        return assumed_value('outer_selector', inp)[look]
    if B.br('stage_sel', assumed_value('phase_word', inp)[look] == 1, 'phase==1'):  # [R7 LM rule] li r4,0 at 0x824EA050 for every slot
        return 0
    bit40 = (srec[10] & 0x40) != 0
    B.br('stage_sel', not bit40, 'bit40 clear')  # [R7 LM rule / R3 selector setters] (~byte[slot_rec+10] >> 6) & 1
    return 0 if bit40 else 1  # [R7 LM rule] selector 1 iff bit 0x40 clear


def rule_shadow_slots(B, slot_recs, look, inp):
    """-> the set of slots whose colour draw carries the shadow bit ([R11 exact rule: +2 on a stage slot] sub_824E9E10 0x824E9F78-0x824EA26C):
    per slot, never inherited (sub_827E72F0(0) at 0x824EA26C clears it after every slot's model draw)."""
    out = set()
    if not B.br('shadow_p', assumed_value('shadow_module', inp) != 0, 'module active'):  # [R11 shadow task body] 0x827E8410-0x827E8430: module null or +12 == 0 -> 0
        return out
    bit = 0x04 if assumed_value('phase_word', inp)[look] == 1 else 0x02  # [R10 class3 / R11 exact rule] pg in phase 1, pe otherwise
    on = False  # only the planted twin PLANT_STICKY carries the bit on (R10's refuted [L] reading)
    for slot in sorted(slot_recs):
        srec, group = slot_recs[slot]
        if group is None:  # [R8 slot draw groups / R11 exact rule] a slot no pass draws never reaches the task
            on = False
            continue
        runs = B.br('shadow_p', (srec[9] & bit) != 0, 'own p bit')  # [R11 exact rule] the slot's own rec+9 bit for the phase
        runs = runs and B.br('shadow_gate', assumed_value('shadow_gate_pass', inp) != 0)  # [R11 extra gates] 0x824E9FA0-0x824EA034
        if B.br('shadow_reset', not PLANT_STICKY, 'per slot'):  # [R11 shadow bit reset per slot] 0x824EA268-0x824EA26C
            on = runs
        else:
            on = on or runs
        if on:  # [R11 exact rule] +2 for this slot's draw only
            out.add(slot)
    return out


def rule_model540(B, slot, rec, inp):
    if assumed_value('model540', inp) is not None:  # [R10 class4] an explicit model540 input overrides the slot rule (assumed list)
        return assumed_value('model540', inp)
    s1216 = rec.get('stage_1216', 0)
    lo, hi = SKINNED_SLOTS_200 if B.br('skinned_slot', STAGE_1216_RANGE[0] <= s1216 <= STAGE_1216_RANGE[1], 'stage+1216 in 200..249') else SKINNED_SLOTS  # [R10 class4] sub_822AA2F8
    return 1 if B.br('skinned_slot', lo <= slot <= hi, 'slot in range') else 0  # [R10 class4] sub_824ED698 0x824ED724


def rule_draw_group(B, srec, inp):
    """-> group 0..4 or None ([R8 slot draw groups] sub_824E7190 0x824E71C8-0x824E72A4)."""
    if B.br('draw_group', (srec[8] & 0x80) != 0 or not assumed_value('rec9_bit80', inp) or (srec[9] & 0x08) != 0, 'not drawn'):  # [R8 slot draw groups]
        return None
    if B.br('draw_group', (srec[8] & 0x20) != 0, 's'):  # [R8 slot draw groups] group 1
        return 1
    if B.br('draw_group', (srec[8] & 0x40) != 0, 'a'):  # [R8 slot draw groups] group 2
        return 2
    if B.br('draw_group', (srec[10] & 0x10) != 0, 'i'):  # [R8 slot draw groups] group 3
        return 3
    if B.br('draw_group', (srec[10] & 0x0C) != 0, 'j/o'):  # [R8 slot draw groups] group 4
        return 4
    return 0


def rule_blend_code(B, st, code):
    if B.br('blend_code', code in BLEND_CODE):  # [R8 blend code (method 4)] other codes record nothing
        st['op'], st['src'], st['dst'] = BLEND_CODE[code]


def rule_stage_state(B, srec, cycled, inp):
    """-> the render state at the colour pass of a stage slot, before the pass block (z write included)."""
    st = dict(assumed_value('inherited_state', inp))
    st['zwrite'] = settled_value('stage_zwrite_inherited', inp)   # SZW: a SETTLED input (ZW2 / ZW3), no longer a literal
    B.br('afunc_code', True)  # [R8 alpha func code (method 22)] codes 5 / 6 below go through AFUNC_CODE
    if B.br('cycled', cycled):  # [R8 cycled slot groups] sub_83420EA0 0x83421074-0x8342112C
        st.update(blend_en=1, atest=1, aref=0, afunc=AFUNC_CODE[6])
        rule_blend_code(B, st, 0)
        if B.br('slot_a_o', (srec[8] & 0x40) != 0, 'a (cycled)'):  # [R8 cycled slot groups] a -> ref 1, blend code 1
            st['aref'] = 1
            rule_blend_code(B, st, 1)
        if B.br('slot_z', (srec[8] & 0x08) != 0 and st['zwrite'] != 0, 'z (cycled)'):  # [R8 cycled slot groups] z -> z write 0 (only when z write is on: sub_824E9E10 tests the cached +84 first)
            st['zwrite'] = 0
        return st
    B.br('stage_begin', True)  # [R8 stage pass begin] sub_825B9860 0x825B9888-0x825B98C4
    st.update(blend_en=1, atest=1, aref=1, afunc=AFUNC_CODE[5])
    rule_blend_code(B, st, 0)
    if B.br('slot_s_ref', (srec[8] & 0x20) != 0 and srec[7] != 0):  # [R8 alpha test / ref per slot] 0x824EA0EC-0x824EA108
        st['aref'] = srec[7]
    if B.br('slot_a_o', (srec[8] & 0x40) != 0 or (srec[10] & 0x04) != 0, 'a/o'):  # [R8 blend per slot] 0x824EA0CC-0x824EA130
        rule_blend_code(B, st, 1)
        st['aref'] = 1
        st['afunc'] = AFUNC_CODE[5]
    if B.br('slot_z', (srec[8] & 0x08) != 0 and st['zwrite'] != 0, 'z'):  # [R8 z write per slot] 0x824EA134-0x824EA154 (only when z write is on: the cached +84 test, ZWR_ADV B)
        st['zwrite'] = 0
    return st


def rule_choose_array(B, arrays, sel, m604, m608, m612):
    a384, a388, a392, a396 = arrays
    if B.br('sel_388', sel != 0):  # [R3 colour-pass selector / V] 0x82C05904-0x82C059E8
        return a388, 388
    if B.br('sel_392', m604 != 0):  # [R3 colour-pass selector / V] 0x82C05998-0x82C059E4
        return a392, 392
    if B.br('sel_396', m608 != 0 and m612 != 0):  # [R3 colour-pass selector / V] 0x82C05998-0x82C059E4
        return a396, 396
    return a384, 384


def rule_rec_flags(B, effect_lower, rec):
    """The 24-byte record at [model+480] + 24*i ([R1 rec[all]] 0x82BDBFA0-0x82BDBFC8: +0 = 1, +4..+20 = 0, then sub_82BDA038)."""
    r = {0: 1, 4: 0, 8: 0, 12: 0, 16: None, 20: 0}
    n = effect_lower or ''
    if B.br('rec0', n in KEYS_REC0):  # [R1 rec+0 / V] 0x82BDA678-0x82BDA7A0
        r[0] = 0
    if B.br('rec4', n in KEYS_REC4):  # [R1 rec+4 / V] 0x82BDA7E8-0x82BDAD88
        r[4] = 1
    if B.br('rec8', n in KEYS_REC8):  # [R1 rec+8 / V] 0x82BDAAB4-0x82BDAB18
        r[8] = 1
    if B.br('rec12', n in KEYS_REC12):  # [R1 rec+12 / V(4)] 0x82BDAB1C-0x82BDACC4
        r[12] = 1
        w = rec['param_words'].get(SPECPOW_NAME)
        if B.br('rec16', w is not None and w != 0):  # [R1 rec+16 / V] 0x82BDACC8-0x82BDAD0C: pointer to word +20 when found and != 0
            r[16] = w
    if B.br('rec20', n == KEY_REC20):  # [R1 rec+20 / V] 0x82BDAD10-0x82BDAD48
        r[20] = 1
    return r


def rule_pass_index(B, r, shadow_on, look, inp, m616, m540, m620):
    v = 1 if B.br('pass_base', m540 == 1) else 0  # [R9 pass value] 0x82C0585C-0x82C058AC: base = 1 iff model+540 == 1
    if not B.br('p4f88_rec12', r[12] == 0):  # [review 1.2 sub_82BD4F88] rec+12 == 0 -> base
        if B.br('p4f88_616', m616 == 1, '616==1'):  # [review 1.2 sub_82BD4F88] 0x82BD4FB0
            v += 4
        elif B.br('p4f88_616', m616 == 0, '616==0'):  # [review 1.2 sub_82BD4F88] word >= 0 (signed) -> base, else +8
            w = r[16]
            if not B.br('p4f88_616', w is not None and w < 0x80000000, 'rec16 word >= 0'):
                v += 8
        elif B.br('p4f88_616', m616 == 2, '616==2'):  # [review 1.2 sub_82BD4F88]
            v += 8
    if B.br('p5ee0_bit0', r[0] == 0 and m540 == 1):  # [review 1.2 sub_82BD5EE0 / R2 model+540]
        v &= ~1
    if B.br('p5ee0_shadow', r[4] != 0 and shadow_on):  # [R9 pass value / R10 class3] the context table at this slot's draw
        v += 2
    if B.br('p5ee0_camera', r[8] != 0 and assumed_value('camera_1164', inp)[look] != 0):  # [review 1.2 sub_82BD5EE0 / R2 sub_82BCC3B0]
        v += 4
    if B.br('p5ee0_aniso', r[20] != 0 and m620 == 1):  # [review 1.2 sub_82BD5EE0 / R2 model+620]
        v += 16
    return v


def rule_pass_blocks(B, eff, tech, pidx):
    """-> (pass dict from effect_table, render-state entries, sampler-state entries, problems)."""
    body = eff['body']
    techs = eff['parsed']['techniques']
    tcount = u32(body, 0x20C)
    if B.br('tech_range', tech >= tcount):  # [review 1.1 sub_82BAEFD8] "Invalid Effect Technique Index"
        return None, [], [], ['technique %d >= count %d' % (tech, tcount)]
    t = next((x for x in techs if x['index'] == tech), None)
    if not B.br('pass_entry', t is not None and pidx < len(t['passes'])):  # [review 1.1 sub_82BAEFD8 / R4 pass[+8]]
        return None, [], [], ['pass %d not in technique %d (%d passes) [U]' % (pidx, tech, len(t['passes']) if t else 0)]
    p = t['passes'][pidx]
    tp = u32(body, u32(body, 0x200) + 4 * tech)
    ps = u32(body, tp + 16 + 4 * pidx)
    rs, ss = [], []
    b = u32(body, ps + 12)  # [R4 pass[+12]] render-state list: counts +16 / +20 / +24, 8-byte entries from +28
    counts = [u32(body, b + o) for o in (16, 20, 24)]
    e = b + 28
    for g, c in enumerate(counts):
        for _ in range(c):
            sid, val = u32(body, e), u32(body, e + 4)
            B.br('rs_group', True, 'group%d' % (g + 1))  # [R4 pass value sources] 0x82BAE4B4-0x82BAE598
            rs.append(dict(id=sid, group=g + 1, value=val if g == 2 else None, slot=val if g != 2 else None))
            e += 8
    b = u32(body, ps + 16)  # [R4 pass[+16]] sampler-state list: counts +128 / +132 / +136, entries from +140
    counts = [u32(body, b + o) for o in (128, 132, 136)]
    e = b + 140
    for g, c in enumerate(counts):
        for _ in range(c):
            w0, val = u32(body, e), u32(body, e + 4)
            B.br('ss_group', True, 'group%d' % (g + 1))  # [R4 pass[+16]] 0x82BAE5CC-0x82BAE6D4
            ss.append(dict(sampler=w0 >> 16, id=w0 & 0xFFFF, group=g + 1, value=val if g == 2 else None))
            e += 8
    return p, rs, ss, []


def rule_states(B, rs, inp, start):
    """-> (state dict, list of param-bound ids). [R4 pass apply gate] blocks applied unconditionally; [R4 blend / alpha / mask] others inherited."""
    st = dict(start)
    bound = []
    B.br('rs_apply', True)  # [R4 pass apply gate] 0x82BAF040-0x82BAF05C
    for e in rs:
        nm = RS_NAME.get(e['id'])
        if nm is None:  # [R4 pass[+12]] a state id this predictor does not score (B_d3d_api 3.2 names the ids) is kept in game.render_states only
            continue
        if B.br('rs_inherit', e['value'] is None, 'param-bound'):  # [R4 pass value sources] group 1 / 2: value slot [U]
            bound.append(nm)
            continue
        st[nm] = e['value']
    return st, bound


def rule_blend_alpha(B, st, inp):
    """-> (blend text, wmask, alpha text) in the harvest's form (choice_harvest.blend_key / alpha_key)."""
    def fac(x):
        return VK_FACTOR[XB_FACTOR[x]] if x < len(XB_FACTOR) else 'F%d' % x  # [reader.h VkBlendFactor] B3 map, out of range printed raw

    def op(x):
        return VK_OP[XB_OP[x]] if x < len(XB_OP) else 'OP%d' % x  # [reader.h VkBlendOp] B4 map, out of range printed raw
    src, dst, cop = st['src'], st['dst'], st['op']
    asrc, adst, aop = (st['asrc'], st['adst'], st['aop']) if st['blend_sep'] else (src, dst, cop)  # [B_d3d_api 3.2 idx 16] assumed: blend_sep_off
    if B.br('blend_off', not st['blend_en'] or (src, dst, cop, asrc, adst, aop) == (1, 0, 0, 1, 0, 0)):  # [reader.h DecodeBlend] enable = control != 0x00010001
        blend = 'off'
    else:
        blend = 'on %s/%s/%s a:%s/%s/%s' % (fac(src), fac(dst), op(cop), fac(asrc), fac(adst), op(aop))
    alpha = 'off' if not st['atest'] else '%s %.6g' % (COMPARE[st['afunc'] & 7], st['aref'] / 255.0)  # [B_d3d_api 3.2 idx 24-26] ref = u32 / 255
    return blend, st['wmask'] & 0xF, alpha


def rule_cull(B, rec, inp):
    if B.br('cull_single', (rec['w8c'] & 0x2) != 0, 'bit2 set'):  # [R4 cull per mesh] 0x82C0A880-0x82C0A914
        mode = settled_value('cull_entry_mode', inp)
        out = dict(subpasses=1, cull=True, mode=mode)
        if B.br('cull_copy', 'r' in (rec.get('slot_tokens') or []), 'r copy'):  # [R4 cull per mesh; SCU / CUL_ADV D] 0x82C0B920-0x82C0B968: the `r` copy (draw 0x82C0B94C) swaps the winding
            out['copy_mode'] = 2 if mode == 1 else 1  # [R4 cull per mesh; SCU / CUL_ADV D] r26 = 2 if the entry is 1, else 1 (0x82C0A918-0x82C0A930)
        return out
    k = assumed_value('cull_k', inp)
    t = settled_value('cull_t', inp)  # [R4 cull per mesh / V batch 2 (4)] null slot pointer -> [model+276]
    if B.br('cull_single', k is not None and k not in (0, 1), 'k outside 0..1'):  # [V batch 2 (4)] sub_82BD38B8 returns 0
        t = 0
    if B.br('cull_single', t == 0, 't==0'):  # [R4 cull per mesh] t = sub_82BD38B8 = [model+276+4k]: 0 one sub-pass cull off
        return dict(subpasses=1, cull=False)
    if B.br('cull_single', t == 1, 't==1'):  # [R4 cull per mesh] 1 two sub-passes, mode 2 then 1; other t not drawn
        return dict(subpasses=2, cull=True, modes=[2, 1])
    return dict(subpasses=0, drawn=False)


def rule_slots(B, vs, ps, fxj, inp):
    slots = {}
    for s in sorted(set(fxj.get(vs)['samplers']) | set(fxj.get(ps)['samplers'])):
        if B.br('slot_fixed', s in assumed_value('role_fixed', inp) and s < 16, 'fixed'):  # [R5 tex.fixed] slots 12 / 13 / 15
            slots[str(s)] = assumed_value('role_fixed', inp)[s]
        elif B.br('slot_mat', s < 16, 'material'):  # [R5 tex.slot / tex.table] descriptor +16 register, model texture table
            slots[str(s)] = assumed_value('role_material', inp)
        else:
            slots[str(s)] = assumed_value('role_vtex', inp)
    B.br('stream_decl', True)  # [R5 stream.decl] 0x82BDEDB4-0x82BDEE88 (the vertex shader's fetch slots, fx.json)
    vstreams = sorted(set(fxj.get(vs)['fetch']))
    return slots, [int(k) for k in slots if int(k) >= 16], vstreams


# ---- driver ---------------------------------------------------------------------------------------------------------------------
def rule_draw_gate(B, inp):
    return B.br('draw_gate', assumed_value('draw_gate', inp) != 0)  # [V batch 2 (4)] [model+832] != 0 or [model+624] != 0


def predict_mesh(B, rec, slot, look, reg, fxj, inp, ctx=None):
    ctx = ctx or {}
    if not rule_draw_gate(B, inp):
        return None, dict(problem='not drawn (draw gate)')
    a384, a388, a392, a396, m608, sub = rule_effect_arrays(B, rec, reg, inp)
    srec = rule_slot_record(B, rec.get('slot_tokens', []))
    group = rule_draw_group(B, srec, inp)
    sel = rule_selector(B, srec, group, look, inp, cycled=bool(rec.get('cycled')))
    eff_l, arr = rule_choose_array(B, (a384, a388, a392, a396), sel, assumed_value('model604', inp), m608, assumed_value('model612', inp))
    g = dict(selector=sel, array=arr, effect=None, technique=rec['w90'], pass_index=None, substituted=sub, slot_record={str(k): v for k, v in srec.items()},
             draw_group=group, cycled=bool(rec.get('cycled')))
    if eff_l is None:
        g['problem'] = 'effect %r not in the registry' % rec['name']
        return None, g
    eff = reg[eff_l]
    g['effect'] = eff['name']
    # the classifier reads the name of the effect object behind the arr384 handle ([R1 rec[name]] 0x82BDC320-0x82BDC444)
    r = rule_rec_flags(B, a384, rec)
    m540 = rule_model540(B, slot, rec, inp)
    shadow_on = slot in ctx.get(look, set())
    pidx = rule_pass_index(B, r, shadow_on, look, inp, assumed_value('model616', inp), m540, assumed_value('model620', inp))
    g.update(rec=r, pass_index=pidx, model540=m540, shadow=shadow_on)
    p, rs, ss, probs = rule_pass_blocks(B, eff, rec['w90'], pidx)
    if p is None:
        g['problem'] = '; '.join(probs)
        return None, g
    st, bound = rule_states(B, rs, inp, rule_stage_state(B, srec, rec.get('cycled'), inp))
    blend, wmask, alpha = rule_blend_alpha(B, st, inp)
    vs, ps = (p['vs'] or {}).get('name') or '', (p['ps'] or {}).get('name') or ''
    slots, vtex, vstreams = rule_slots(B, vs, ps, fxj, inp)
    g.update(pass_name=p['name'], vs_hash=(p['vs'] or {}).get('hash'), ps_hash=(p['ps'] or {}).get('hash'),
             render_states=[dict(state=RS_NAME.get(e['id'], 'id%d' % e['id']), group=e['group'], value=e['value']) for e in rs],
             sampler_states=[dict(sampler=e['sampler'], state=SS_NAME.get(e['id'], 'id%d' % e['id']), value=e['value']) for e in ss],
             param_bound=bound, cull=rule_cull(B, rec, inp), zwrite=st.get('zwrite'))
    defines = sorted(set(fxj.get(vs)['defines'] + fxj.get(ps)['defines']))
    choice = dict(vs=vs, ps=ps, defines=defines, blend=blend, wmask=wmask, alpha=alpha, slots=slots, vtex=vtex, vstreams=vstreams, n=1)
    return choice, g


def predict(arena_id, looks, inp, B=None, cache={}):
    B = B or Branches()
    if 'eff' not in cache:
        cache['eff'], cache['fx'] = Effects(), FxJson()
    if arena_id not in cache:
        cache[arena_id] = mesh_records(arena_id)
    effs, fxj = cache['eff'], cache['fx']
    reg = effs.by_lower
    arena, raw = cache[arena_id]
    ctx = shadow_context(B, sorted(((sl, r) for (sl, _m), r in raw.items()), key=lambda x: x[0]), looks, inp)
    rows = []
    for (slot, mi), rec in sorted(raw.items()):
        fm = arena.slots[slot][mi]
        lk, games = {}, {}
        for look in looks:
            ch, g = predict_mesh(B, rec, slot, look, reg, fxj, inp, ctx)
            lk[look] = dict(draws=0, ambiguous=0, choices=[ch] if ch else [])
            games[look] = g
        rows.append(dict(file='bg%02d.pac#%d' % (arena_id, slot), slot=slot, mesh=mi, effect=fm.effect, tex=fm.samplers.get('sampDiffusePrm'), looks=lk, game=games))
    table = dict(format=FORMAT, scene='predict', arena=arena_id, looks={lk_: dict(captures=[], frames=0) for lk_ in looks}, meshes=rows,
                 assumed={k: ASSUMED[k][1] for k in ASSUMED}, inputs={k: _jsonable(assumed_value(k, inp)) for k in ASSUMED})
    return table, B


def shadow_context(B, recs, looks, inp):
    """look -> the shadow slots (rule_shadow_slots over the slot records of this model file)."""
    slot_recs = {}
    for slot, rec in recs:
        if slot not in slot_recs:
            srec = rule_slot_record(Branches(), rec.get('slot_tokens', []))
            slot_recs[slot] = (srec, rule_draw_group(Branches(), srec, inp))
    return {look: rule_shadow_slots(B, slot_recs, look, inp) for look in looks}


def _jsonable(v):
    if isinstance(v, (set, frozenset)):
        return sorted(v)
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in v.items()}
    return v


def parse_sets(sets):
    inp = {}
    for s in sets or []:
        k, _, v = s.partition('=')
        if k not in ASSUMED:
            raise SystemExit('unknown input %s (see --assumed)' % k)
        d = ASSUMED[k][0]
        if k == 'shadow_table':
            look, _, val = v.partition(':')
            cur = {lk: list(x) for lk, x in inp.get(k, d).items()}
            cur[look] = [int(x) for x in val.split(',') if x]
            inp[k] = cur
        elif isinstance(d, dict) and k in ('camera_1164', 'phase_word'):
            look, _, val = v.partition(':')
            cur = dict(inp.get(k, d))
            if val:
                cur[look] = int(val, 0)
            else:
                cur = {lk: int(look, 0) for lk in cur}
            inp[k] = cur
        elif isinstance(d, bool):
            inp[k] = v.lower() in ('1', 'true', 'yes')
        elif isinstance(d, int):
            inp[k] = int(v, 0)
        else:
            inp[k] = json.loads(v)
    return inp


# ---- lints (Q4.4) ---------------------------------------------------------------------------------------------------------------
FORBIDDEN = [r'\btexel', r'\bmean\b', r'threshold', r'\barena_id\b', r'\barena\s*==', r'\bmesh\s*==', r'\bslot\s*==\s*\d', r'\bmi\s*==', r'percent', r'\bratio\b']
ALLOWED_STR_SOURCES = set(KEYS_REC4) | set(KEYS_REC8) | set(GEAR_TWIN) | set(GEAR_TWIN.values()) | {SPECPOW_NAME, 'lm'}


def lint():
    src = open(os.path.abspath(__file__), encoding='utf-8').read()
    lines = src.splitlines()
    tree = ast.parse(src)
    fails = []
    nbr = 0
    for fn in tree.body:
        if not (isinstance(fn, ast.FunctionDef) and fn.name.startswith('rule_')):
            continue
        for node in ast.walk(fn):
            if isinstance(node, (ast.If, ast.IfExp)):
                ln = node.lineno
                txt = lines[ln - 1] + (lines[ln - 2] if ln >= 2 else '')
                if 'B.br(' not in lines[ln - 1] and 'B.br(' not in lines[ln - 1 + 0]:
                    # a plain if must sit under a branch's comment (same or previous line) citing a claim
                    if not re.search(r'#.*\[(R\d|review|B_d3d|reader\.h|V)', txt):
                        fails.append('%s:%d: branch without a claim citation' % (fn.name, ln))
            if isinstance(node, ast.Call) and getattr(node.func, 'attr', None) == 'br':
                nbr += 1
                a0 = node.args[0]
                if not (isinstance(a0, ast.Constant) and a0.value in BRANCHES):
                    fails.append('%s:%d: br() id not in BRANCHES' % (fn.name, node.lineno))
                elif not re.search(r'#.*\[(R\d|review|B_d3d|reader\.h|V)', lines[node.lineno - 1] + (lines[node.lineno - 2] if node.lineno >= 2 else '')):
                    # a br() on a line without its own comment must be the continuation of a commented branch above it
                    if not any(re.search(r'#.*\[(R\d|review|B_d3d|reader\.h|V)', lines[k]) for k in range(max(0, node.lineno - 4), node.lineno)):
                        fails.append('%s:%d: br(%s) without a claim comment' % (fn.name, node.lineno, a0.value))
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                v = node.value
                if re.fullmatch(r'[a-z_]+', v) and (v.startswith(('ybg', 'ych', 'yprop')) or v.endswith('.pac')) and v not in ALLOWED_STR_SOURCES:
                    fails.append('%s:%d: name %r outside the game-own lists' % (fn.name, node.lineno, v))
            if isinstance(node, (ast.Set, ast.List, ast.Tuple)) and len(node.elts) > 3 and all(isinstance(e, ast.Constant) and isinstance(e.value, str) for e in node.elts):
                fails.append('%s:%d: inline name list in rule code' % (fn.name, node.lineno))
        body = ast.get_source_segment(src, fn) or ''
        for pat in FORBIDDEN:
            for m in re.finditer(pat, body):
                fails.append('%s: forbidden token %r' % (fn.name, m.group(0)))
    for bid, (claim, rng, what) in BRANCHES.items():
        if not re.search(r'0x[0-9A-F]{6,8}|\(\d+ insns\)|setter 0x', rng):
            fails.append('BRANCHES[%s]: no instruction range' % bid)
        if bid not in src.split('BRANCHES = {', 1)[1].split('\n}', 1)[1]:
            fails.append('BRANCHES[%s]: never used in rule code' % bid)
    print('lint: %d rule functions, %d br() calls, %d branches declared, %d problems' % (
        sum(1 for f in tree.body if isinstance(f, ast.FunctionDef) and f.name.startswith('rule_')), nbr, len(BRANCHES), len(fails)))
    for f in fails:
        print('  LINT', f)
    return 0 if not fails else 1


# ---- planted twins --------------------------------------------------------------------------------------------------------------
def _synthetic(reg):
    """Synthetic mesh records (no game data): one per game-own name group, word +0x90 = 0, so every flag has a row it can change."""
    names = ['yBG_Default', 'yBG_PS', 'yBG_DefaultLM', 'yProp_PSM', 'yCh_skin', 'yCh_Aniso_trans', 'yCh_cloth', 'yProp_Default']
    out = []
    for i, n in enumerate(names):
        out.append((90 + i, dict(name=n, w8c=0, w90=0, w64=0, param_words={SPECPOW_NAME: 0x3F800000} if n.lower() in KEYS_REC12 else {})))
    # one yBG_Default record per slot-text token the new rules read, and one cycled slot
    for j, toks in enumerate((['a'], ['s', 't(64)'], ['z'], ['x'], ['xe'], ['o'], ['i'], ['j'], ['k'], ['xg'])):
        out.append((120 + j, dict(name='yBG_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=toks)))
    # R10: a pe slot then a later plain slot (stickiness), a pg slot, and skinned prop slots (390 / 81 under stage+1216 = 210)
    out.append((150, dict(name='yBG_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=['pe'])))
    out.append((151, dict(name='yBG_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=[])))
    out.append((152, dict(name='yBG_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=['pg'])))
    out.append((390, dict(name='yProp_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=[])))
    out.append((81, dict(name='yProp_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=[], stage_1216=210)))
    out.append((140, dict(name='yBG_Default', w8c=0, w90=0, w64=0, param_words={}, slot_tokens=['a'], cycled=True)))
    # SCU: a cull-bit record (+0x8C = 6) in an `r` slot: the one-sided draw and the mirrored copy's swapped winding
    out.append((160, dict(name='yBG_Default', w8c=6, w90=0, w64=0, param_words={}, slot_tokens=['r'])))
    return out


PLANTS = {
    # name: (inputs override, description)
    'phase':         ({'phase_word': {'entrance': 1, 'match': 4}}, 'phase word swapped between the looks'),
    'model604':      ({'model604': 1, 'phase_word': {'entrance': 1, 'match': 1}, 'registry2': True}, 'arr392 chosen'),
    'model612_0':    ({'model612': 0, 'phase_word': {'entrance': 1, 'match': 1}}, 'arr396 never chosen'),
    'rec9_bit80':    ({'rec9_bit80': 0}, 'no slot drawn by the stage passes'),
    'model616_1':    ({'model616': 1}, 'pass +4 for the rec+12 names'),
    'model616_2':    ({'model616': 2}, 'pass +8 for the rec+12 names'),
    'model540':      ({'model540': 1}, 'model540 forced 1 on every mesh'),
    'model620':      ({'model620': 1}, 'pass +16 for ych_aniso_trans'),
    'shadow_module': ({'shadow_module': 0}, 'shadow module off: no p2'),
    'shadow_gate':   ({'shadow_gate_pass': 0}, 'shadow gate fails: no p2'),
    'outer_sel':     ({'outer_selector': {'entrance': 1, 'match': 1, 'match_scene': 1}}, 'outer selector 1 for groups 1-4'),
    'camera':        ({'camera_1164': {'entrance': 1, 'match': 1}}, 'pass +4 for the rec+8 names'),
    'inherited':     ({'inherited_state': dict(ASSUMED['inherited_state'][0], wmask=7, blend_sep=1)}, 'inherited write mask 7 + separate alpha on'),
    'role_fixed':    ({'role_fixed': {12: 'rt', 13: 'rt', 15: 'rt'}}, 'fixed slot roles'),
    'cull_t':        ({'cull_t': 1}, 'two sub-passes for two-sided records (t = 1)'),
    'cull_entry':    ({'cull_entry_mode': 1}, 'the entry winding of a mirrored camera (mode 1)'),
    'draw_gate':     ({'draw_gate': 0}, 'nothing drawn'),
}


def _sig(B, recs, slot_of, looks, inp, reg, fxj):
    out = []
    ctx = shadow_context(B, recs, looks, inp)
    for slot, rec in recs:
        for look in looks:
            ch, g = predict_mesh(B, rec, slot_of(slot), look, reg, fxj, inp, ctx)
            out.append(json.dumps([ch, {k: v for k, v in g.items() if k in ('effect', 'technique', 'pass_index', 'array', 'cull', 'rec', 'zwrite', 'draw_group', 'model540', 'shadow')}], sort_keys=True, default=str))
    return out


def plant(arena_id, looks):
    effs, fxj = Effects(), FxJson()
    reg = effs.by_lower
    syn = _synthetic(reg)
    real = []
    if arena_id is not None:
        _arena, raw = mesh_records(arena_id)
        real = [(s, r) for (s, m), r in sorted(raw.items())]
    base_syn = _sig(Branches(), syn, lambda s: s, looks, {}, reg, fxj)
    base_real = _sig(Branches(), real, lambda s: s, looks, {}, reg, fxj) if real else []
    rc = 0
    for name, (ov, what) in PLANTS.items():
        s = _sig(Branches(), syn, lambda x: x, looks, ov, reg, fxj)
        ns = sum(1 for a, b in zip(s, base_syn) if a != b)
        nr = None
        if real:
            r = _sig(Branches(), real, lambda x: x, looks, ov, reg, fxj)
            nr = sum(1 for a, b in zip(r, base_real) if a != b)
        ok = ns > 0
        rc |= 0 if ok else 1
        print('plant %-12s %-48s synthetic: %3d of %d rows changed %s%s' % (name, what, ns, len(s), 'CAUGHT' if ok else 'NOT CAUGHT',
              '' if nr is None else '; arena %d: %d of %d rows changed%s' % (arena_id, nr, len(base_real), '' if nr else ' (not exercised by this arena)')))
    # name-group twins: rename a game-own key (the rec flag groups) and the output must change
    # every pass-index input on, so each name group's flag reaches the output
    on = {'camera_1164': {'entrance': 1, 'match': 1, 'match_scene': 1}, 'model540': 1, 'model620': 1, 'model616': 2}
    ref = _sig(Branches(), syn, lambda x: x, looks, on, reg, fxj)
    for grp in ('KEYS_REC0', 'KEYS_REC4', 'KEYS_REC8', 'KEYS_REC12', 'KEY_REC20'):
        saved = globals()[grp]
        globals()[grp] = () if grp != 'KEY_REC20' else '-'
        try:
            s = _sig(Branches(), syn, lambda x: x, looks, on, reg, fxj)
        finally:
            globals()[grp] = saved
        n = sum(1 for a, c in zip(s, ref) if a != c)
        rc |= 0 if n else 1
        print('plant %-12s %-48s synthetic: %3d of %d rows changed %s' % (grp.lower(), 'the %s name group emptied' % grp, n, len(s), 'CAUGHT' if n else 'NOT CAUGHT'))
    # slot-text twins (R7 / R8): each token's record bit removed from the parser table must change the output; and the cycled flag dropped
    ref = _sig(Branches(), syn, lambda x: x, looks, {}, reg, fxj)
    twins = [('letter_' + c, 'SLOT_LETTER', c) for c in ('a', 's', 'z', 'o', 'i', 'j')] + [('x_' + c, 'SLOT_X', c) for c in ('x', 'xe')]
    twins += [('blend_code1', 'BLEND_CODE', 1), ('afunc_code5', 'AFUNC_CODE', 5), ('p_pe', 'SLOT_P', 'pe'), ('p_pg', 'SLOT_P', 'pg')]
    for name, table, key in twins:
        tab = globals()[table]
        saved = tab[key]
        if table == 'BLEND_CODE':
            tab[key] = (0, 1, 1)
        elif table == 'AFUNC_CODE':
            tab[key] = 7
        elif table == 'SLOT_P':
            tab[key] = 0
        else:
            del tab[key]
        try:
            sg = _sig(Branches(), syn, lambda x: x, looks, {}, reg, fxj)
        finally:
            tab[key] = saved
        n = sum(1 for a, c in zip(sg, ref) if a != c)
        rc |= 0 if n else 1
        print('plant %-12s %-48s synthetic: %3d of %d rows changed %s' % (name, '%s[%r] removed / altered' % (table, key), n, len(sg), 'CAUGHT' if n else 'NOT CAUGHT'))
    for name, glob_, val in (('skinned_rng', 'SKINNED_SLOTS', (0, -1)), ('skinned_200', 'STAGE_1216_RANGE', (0, -1))):
        saved = globals()[glob_]
        globals()[glob_] = val
        try:
            sg = _sig(Branches(), syn, lambda x: x, looks, {}, reg, fxj)
        finally:
            globals()[glob_] = saved
        n = sum(1 for a, c in zip(sg, ref) if a != c)
        rc |= 0 if n else 1
        print('plant %-12s %-48s synthetic: %3d of %d rows changed %s' % (name, '%s emptied' % glob_, n, len(sg), 'CAUGHT' if n else 'NOT CAUGHT'))
    global PLANT_STICKY
    PLANT_STICKY = True
    try:
        sg = _sig(Branches(), syn, lambda x: x, looks, {}, reg, fxj)
    finally:
        PLANT_STICKY = False
    n = sum(1 for a, c in zip(sg, ref) if a != c)
    rc |= 0 if n else 1
    print('plant %-12s %-48s synthetic: %3d of %d rows changed %s' % ('sticky', 'the shadow bit carried to later slots (R10 [L])', n, len(sg), 'CAUGHT' if n else 'NOT CAUGHT'))
    syn2 = [(sl, dict(r, cycled=False)) for sl, r in syn]
    sg = _sig(Branches(), syn2, lambda x: x, looks, {}, reg, fxj)
    n = sum(1 for a, c in zip(sg, ref) if a != c)
    rc |= 0 if n else 1
    print('plant %-12s %-48s synthetic: %3d of %d rows changed %s' % ('cycled', 'the cycle-group flag dropped', n, len(sg), 'CAUGHT' if n else 'NOT CAUGHT'))
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('arena', nargs='?', type=int)
    ap.add_argument('--look', action='append', choices=['entrance', 'match', 'match_scene'])
    ap.add_argument('--set', action='append')
    ap.add_argument('--out')
    ap.add_argument('--branches', action='store_true')
    ap.add_argument('--assumed', action='store_true')
    ap.add_argument('--lint', action='store_true')
    ap.add_argument('--plant', action='store_true')
    ap.add_argument('--plant-no-sel-cycled', action='store_true', help='planted twin: the sel_cycled branch off (scoring must lose rows)')
    a = ap.parse_args()
    looks = a.look or ['entrance', 'match']
    global PLANT_NO_SELCYC
    PLANT_NO_SELCYC = a.plant_no_sel_cycled
    if a.lint:
        return lint()
    if a.assumed:
        for k, (d, why) in ASSUMED.items():
            print('%-18s default %-40s %s' % (k, json.dumps(_jsonable(d))[:40], why))
        return 0
    if a.plant:
        return plant(a.arena, looks)
    if a.arena is None:
        ap.error('ARENA required')
    inp = parse_sets(a.set)
    table, B = predict(a.arena, looks, inp)
    if a.branches:
        for bid, (claim, rng, what) in BRANCHES.items():
            t = B.taken.get(bid, {})
            print('%-13s %-40s %-55s %s' % (bid, claim, rng, ', '.join('%s:%d' % (k, v) for k, v in sorted(t.items(), key=str)) or 'NOT EXERCISED (read, not tested)'))
    n = sum(1 for m in table['meshes'] for lk in m['looks'].values() if lk['choices'])
    tot = sum(len(m['looks']) for m in table['meshes'])
    print('arena %d: %d meshes, %d of %d (mesh, look) predicted, %d with a problem' % (a.arena, len(table['meshes']), n, tot, tot - n))
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        json.dump(table, open(a.out, 'w'), indent=1, sort_keys=True)
        print('wrote', a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())

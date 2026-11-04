# Session 87 (stage 4): later batches' rows of gen_header.py's table (same columns; flags 'dynamic' = kDynamic: the shader
# indexes the whole 256-register float block, skinned shaders). exec'd by gen_header.py and vs_check_all.py after T.
T += [
 ('0BBBF514D70CF0F4', 'vs_skin_shadow', 'yShadowMap, skinned', '14 / 91 / 88', 'shadow_map (DA494C93)', 12, 95, 'dynamic'),
 ('0D7075D53FCA3F02', 'vs_skin_shadow11', 'yShadowMap, skinned, 11-dword vertices', '0 / 1 / 0', 'shadow_map (DA494C93)', 11, 95, 'dynamic'),
 ('BDAF8155EBF110C2', 'vs_skin_shadow_rigid', 'yShadowMap, rigid', '22 / 22 / 38', 'shadow_map (DA494C93)', 8, 95, 'dynamic'),
 ('222898168D2A226A', 'vs_skin_shadow_rigid9', 'yShadowMap, rigid, 9-dword vertices', '3 / 12 / 12', 'shadow_map (DA494C93)', 9, 95, 'dynamic'),
 ('E8AC5C3074483B96', 'vs_skin_rim', 'yRimLightMap, skinned', '19 / 59 / 60', 'rim_light_map (D03E7D4C)', 12, 95, 'dynamic'),
 ('3F63A55561994118', 'vs_skin_rim11', 'yRimLightMap, skinned, 11-dword vertices', '0 / 1 / 2', 'rim_light_map (D03E7D4C)', 11, 95, 'dynamic'),
 ('A441E84E647C3D9F', 'vs_skin_rim_rigid', 'yRimLightMap, rigid', '4 / 9 / 10', 'rim_light_map (D03E7D4C)', 9, 95, 'dynamic'),
]
T += [
 ('20395ADD7E10418F', 'vs_blur_mask', 'yBlur, vertex-side mask lookup', '3 / 3 / 3', 'shadow blurs (632189BA, 7C650AA1, 694A6D14)', 6, 95, 0),
 ('3834DE7318E253D5', 'vs_default_lit', 'yDefault, lit mesh', '4 / 4 / 0', 'B5853824 (bench census)', 8, 95, 'dynamic'),
 ('63170EAB428C7EFA', 'vs_circle_shadow', 'yCircleShadow', '3 / 5 / 5', 'circle_shadow (D05BFEA1)', 8, 95, 'dynamic'),
 ('A878C3FD5E27FB35', 'vs_plane_shadow', 'yPlaneShadowMap, rigid', '24 / 24 / 24', 'debug_primitive (A4A965C1)', 8, 95, 'dynamic'),
 ('A96C8C4D28D28F80', 'vs_plane_shadow_skin', 'yPlaneShadowMap, weighted', '8 / 8 / 24', 'debug_primitive (A4A965C1)', 11, 95, 'dynamic'),
]
T += [
 ('080ACF65B1E3A6E1', 'vs_prop_lit', 'yProp_Default, four interpolators', '39 / 47 / 55', 'B73643DD, 072B4BDE (bench census)', 8, 95, 'dynamic'),
 ('A0C9935FB47C5A28', 'vs_prop_bump_skin', 'yProp_BumpMap, with the depth fade', '24 / 24 / 26', 'AFB4E9E0 (bench census)', 8, 95, 'dynamic'),
 ('CED7FD64EC433C69', 'vs_prop_lm', 'yProp_DefaultLM, three interpolators', '23 / 23 / 0', 'arena_prop (047BBF32)', 8, 95, 'dynamic'),
 ('2A2E40D0D036855D', 'vs_prop_lit_lean', 'yProp_Default, two interpolators', '1 / 1 / 31', 'arena_prop (468C06E8)', 8, 95, 'dynamic'),
 ('962408F804CE9071', 'vs_prop_bump_flat', 'yProp_BumpMap, without the depth fade', '9 / 12 / 6', '741439F5 (bench census)', 8, 95, 'dynamic'),
 ('7D652F24E7C76062', 'vs_prop_lm_lit', 'yProp_DefaultLM, five interpolators', '8 / 8 / 0', 'F7FA89EF (bench census)', 8, 95, 'dynamic'),
 ('FE6B770B3CC9FA15', 'vs_prop_shadow_lm', 'yProp_PSMLM', '1 / 1 / 0', 'unknown', 8, 95, 'dynamic'),
 ('34E8768C641C186C', 'vs_prop_shadow', 'yProp_PSM', '0 / 0 / 1', 'unknown', 8, 95, 'dynamic'),
 ('54C931B2B2BDF71B', 'arena_vs_bg_bump', 'yBG_BumpMapLM', '1 / 1 / 0', 'unknown', 8, 95, 0),
]
T += [
 ('F709D14F30FCC95C', 'vs_ch_gear_w12', 'yCh_gear, 12-dword vertices', '19 / 28 / 27', 'B1F58C0A, 1B66FF87 (bench census)', 12, 95, 'dynamic'),
 ('3F91EC05EDABDF7B', 'vs_ch_skin_a', 'yCh_skin, with the material lookup', '0 / 20 / 21', 'F75AE782, E86BD728 (bench census)', 12, 95, 'dynamic'),
 ('0E92FB108A675C39', 'vs_ch_skin_fx', 'yCh_skin, with two lookups', '0 / 8 / 8', 'unknown', 12, 95, 'dynamic'),
 ('A6767C4D4522DFAA', 'vs_ch_gear_w9', 'yCh_gear, 9-dword vertices', '3 / 3 / 4', 'unknown', 9, 95, 'dynamic'),
 ('A8E30EED87562597', 'vs_ch_reflect_w9', 'yCh_Reflect, 9-dword vertices', '2 / 4 / 4', 'unknown', 9, 95, 'dynamic'),
 ('56DD2E94E20F2826', 'vs_ch_anisotropic', 'yCh_Anisotropic', '0 / 4 / 4', 'unknown', 12, 95, 'dynamic'),
 ('D345ACFD67B8D83F', 'vs_ch_reflect_w12', 'yCh_Reflect, 12-dword vertices', '0 / 2 / 4', 'unknown', 12, 95, 'dynamic'),
 ('9CB11A97F230260C', 'vs_ch_reflect_w11', 'yCh_Reflect, 11-dword vertices', '0 / 2 / 4', 'unknown', 11, 95, 'dynamic'),
 ('B15688766DA713DC', 'vs_ch_reflect_w9p', 'yCh_Reflect, 9-dword vertices, with the projected position', '0 / 2 / 2', 'unknown', 9, 95, 'dynamic'),
 ('F006439D879C5FDB', 'vs_ch_skin_lean', 'yCh_skin, 9-dword vertices', '0 / 2 / 2', 'unknown', 9, 95, 'dynamic'),
]
T += [
 ('1D3A5C6685E70D68', 'vs_flash', 'yFlashDefault', '6 / 5 / 0', 'flash (9A39D20F, bench census)', 8, 95, 'dynamic'),
 ('BDFD77B5991F86C8', 'vs_blur_taps_a', 'yBlur kernel', '1 / 1 / 1', 'the shadow blurs (bench census)', 3, 95, 0),
 ('1E2294D64E4D2958', 'vs_blur_taps_b', 'yBlur kernel, second form', '1 / 1 / 1', 'the shadow blurs (bench census)', 3, 95, 0),
]

# Shaders built from the game's own shader dump (gen_local_vs.py, ucode_gen.py) would be listed here: their .vert files and SPIR-V
# headers are never committed (CLAUDE.md rule 1), so gen_header.py guards their include and row with __has_include. None remain: every
# shader of the table is hand-written and tracked. (GEN_FLOATS would hold the register list of a generated packed one.)
GENERATED = set()
GEN_FLOATS = {}
# The crowd's individual figures and the atlas quad (the instanced crowd runs are drawn by our own crowd program, own_crowd_*, not here).
T += [
 ('0E2A3DF5BAA2CA18', 'vs_crowd_figure_match', 'yAudDefault, the match\'s spectator figures', '20 / 20 / 30', 'ED94EC2F (the census pair)', 11, 95, 'dynamic'),
 ('CB5B16DA1312E5AB', 'vs_crowd_figure_entrance', 'yAudDefault, the entrances\' spectator figures', '0 / 5 / 0', 'unconfirmed', 11, 95, 'dynamic'),
 ('5344BF064B17B7C2', 'vs_crowd_bake', 'yAudience, the crowd atlas quad', 'unknown', 'crowd atlas copy (7ABDFEFB)', 4, 95, 0),
]

"""Session 98 step 16: the friendly-name rules for "click to inspect" (arena and wrestler parts).

Our own annotations, derived from the pieces' own node / texture / material names (a prefix table, no game data):
  node    = the first node name of the piece's model (arena slots carry descriptive names: ar_fence01_a, Sta_Seat1F_b ...)
  tex     = the mesh's diffuse texture name
  shader  = the material class (yCh_skin, yCh_gear, yBG_Default ...)
  palette = a wrestler mesh's skin-palette bone names
A rule matches when its kind / key range / additive / shader / palette tests pass and, if it names node and/or tex
substrings, at least ONE of the two named tests hits (a rule that names only one field tests only that one).
First match wins. src\\assets\\part_names.h is generated from this list by `part_names.py --emit-header`, so the
Python report and the C++ lookup cannot drift apart.
"""


class Rule:
    __slots__ = ('cat', 'name', 'kind', 'lo', 'hi', 'node', 'tex', 'shader', 'add', 'pal')

    def __init__(self, cat, name, node='', tex='', kind='any', keys=(0, 0), shader='', add=-1, pal=''):
        self.cat, self.name, self.kind, self.lo, self.hi = cat, name, kind, keys[0], keys[1]
        self.node, self.tex, self.shader, self.add, self.pal = node, tex, shader, add, pal


def _any(subs, text):
    return any(s in text for s in subs.split('|'))


def matches(r, p, node, tex, sh, pal):
    if r.kind != 'any' and r.kind != p['kind']:
        return False
    if r.hi and not (r.lo <= p['key'] <= r.hi):
        return False
    if r.add == 1 and not p.get('additive'):
        return False
    if r.shader and not _any(r.shader, sh):
        return False
    if r.pal and not _any(r.pal, pal):
        return False
    if r.node or r.tex:
        if not ((r.node and _any(r.node, node)) or (r.tex and _any(r.tex, tex))):
            return False
    return True


def classify(p):
    """p = dict(kind, key, node, tex, shader, additive, palette[list]) -> (category, friendly, rule index); no rule = Unknown."""
    node, tex, sh = p['node'].lower(), (p.get('tex') or '').lower(), (p.get('shader') or '').lower()
    pal = ';'.join(p.get('palette') or []).lower()
    for i, r in enumerate(RULES):
        if matches(r, p, node, tex, sh, pal):
            return r.cat, r.name, i
    return 'Unknown', 'Unknown part (slot %d)' % p['key'], -1


R = Rule
RULES = [
    # ---- wrestlers (palette = bone names of the mesh's skin palette) ----
    R('Head', 'Eyes', kind='wrestler', pal='l_eye|r_eye'),
    R('Hair', 'Hair', kind='wrestler', pal='kami_'),
    R('Hair', 'Hair', kind='wrestler', shader='aniso_trans'),
    R('Head', 'Eyelids and Lashes', kind='wrestler', pal='mabuta|meziri', shader='trans'),
    R('Hair', 'Hair Piece', kind='wrestler', keys=(10000, 19999)),
    R('Gear', 'Accessory Piece', kind='wrestler', keys=(20000, 39999)),
    R('Body', 'Torso and Arms', kind='wrestler', shader='skin', pal='spine|clavicle|bip pelvis|bip;'),
    R('Body', 'Hands', kind='wrestler', shader='skin', pal='finger|hand'),
    R('Body', 'Forearms', kind='wrestler', shader='skin', pal='forearm'),
    R('Body', 'Legs', kind='wrestler', shader='skin', pal='thigh|calf|foot|toe'),
    R('Body', 'Torso and Arms', kind='wrestler', shader='skin', pal='upperarm'),
    R('Head', 'Face and Head', kind='wrestler', shader='skin', pal='head|neck|kuchi|hoho|mayu|hana'),
    R('Body', 'Body Skin', kind='wrestler', shader='skin'),
    R('Attire', 'Trunks, Pants and Top', kind='wrestler', shader='gear', pal='spine|pelvis|clavicle|thigh_u|bip;'),
    R('Gear', 'Gloves and Hand Tape', kind='wrestler', shader='gear', pal='finger|hand'),
    R('Pads', 'Arm Pads and Sleeves', kind='wrestler', shader='gear', pal='upperarm|forearm|elbow'),
    R('Gear', 'Boots and Leg Wear', kind='wrestler', shader='gear', pal='foot|calf|toe|thigh'),
    R('Gear', 'Face Detail (beard, paint, mask)', kind='wrestler', shader='gear', pal='head|neck|kuchi|hoho|mayu|hana'),
    R('Attire', 'Attire', kind='wrestler', shader='gear'),
    R('Gear', 'Accessory', kind='wrestler', shader='trans'),
    R('Gear', 'Accessory', kind='wrestler', shader='reflect'),
    R('Body', 'Body Part', kind='wrestler'),

    # ---- crowd ----
    R('Crowd', 'Crowd Member', kind='crowd'),
    R('Crowd', 'Crowd (flat audience cards)', tex='2d_audi|2d_kyaku|2d_aud'),

    # ---- the entrance stage and video screens: keys 800-809 are drawn by code; slots 301-304 are the screens switched off ----
    R('Titantron', 'Titantron Screen', kind='arena', keys=(800, 809), tex='titantron_movie'),
    R('Mini-Tron', 'Mini-Tron', kind='arena', keys=(800, 809), tex='mini_tron'),
    R('Stage Header', 'Stage Header and Wall', kind='arena', keys=(800, 809), tex='header&wall'),
    R('Titantron', 'Main Screen and Banner', kind='arena', keys=(800, 809), tex='main&banner'),
    R('Titantron', 'Stage Screen', kind='arena', keys=(800, 809)),
    R('Titantron', 'Titantron (screen off)', node='wall_tron'),
    R('Stage Header', 'Stage Header Screen (off)', node='head_tron'),
    R('Mini-Tron', 'Mini-Tron (screen off)', node='mini_tron'),
    R('Titantron', 'Banner Screen (off)', node='banner_tron'),

    # ---- the ring set (keys 951-982) and the bgEtc props ----
    R('Ring', 'Ring Base (under the ring)', node='ring_sita'),
    R('Ring Post', 'Ring Post', kind='arena', keys=(951, 959), node='ar_post', tex='rin_pos'),
    R('Turnbuckle', 'Turnbuckle', kind='arena', keys=(951, 982), tex='rin_tb2'),
    R('Turnbuckle', 'Turnbuckle Cover', kind='arena', keys=(951, 982), node='ar_cover', tex='rin_tb'),
    R('Ring', 'Ring Canvas', kind='arena', keys=(951, 982), node='ar_ring', tex='rin_mat'),
    R('Ring Ropes', 'Ring Ropes', kind='arena', keys=(951, 982), node='ar_rope', tex='ar_rop'),
    R('Ring Apron', 'Ring Apron (skirt)', kind='arena', keys=(951, 982), tex='rin_ep'),
    R('Ring Steps', 'Ring Steps', kind='prop', keys=(29, 35)),
    R('Ring Steps', 'Ring Steps', tex='ar_stair'),
    R('Announce Desk', 'Announce Desk', kind='prop', keys=(6, 7)),
    R('Announce Desk', 'Announce Chair', node='ji_chair', tex='ji_chair'),
    R('Announce Desk', 'Announce Desk', tex='ji_desk|ji_mon|ji_top|ji_bl|ji_meta'),

    # ---- ringside: flags, barricade, floor, mats, chairs ----
    R('Signage / Banner', 'Flag', node='flag', tex='flag'),
    R('Barricade Corner', 'Barricade Corner Sign', keys=(390, 399), tex='ar_fen_wwe'),
    R('Barricade Corner', 'Barricade Corner', keys=(390, 399), tex='ar_fence|rin_fen'),
    R('Barricade', 'Barricade Shadow', node='fen_sdw|fence_sha|sak_s', tex='fen_sdw|fence_sha|sak_sh'),
    R('Barricade', 'Barricade Sign (WWE)', tex='ar_fen_wwe'),
    R('Barricade', 'Barricade', node='ar_fence', tex='ar_fence|fence_b'),
    R('Barricade', 'Barricade Fence Panel', node='ar_sak|_sak', tex='ar_sak'),
    R('Floor Mat', 'Floor Mat', node='dwnmt|downmt', tex='ar_dm'),
    R('Floor', 'Ringside Floor', node='ar_ground'),
    R('Seats', 'Ringside Chair', node='arena_chair', tex='stand_chair'),

    # ---- lights, glows, shadows (additive pieces first: beams, cones, glare) ----
    R('Light Cone / Glare', 'Moving Spotlight Beam', node='glow_mo', add=1),
    R('Light Cone / Glare', 'Light Beam', tex='beam_', add=1),
    R('Light Cone / Glare', 'Light Cone / Glare', add=1),
    R('Spotlight', 'Moving Spotlight', node='glow_mo|_mo_l|mo1l|mo0l|lt_mo|lt_mov|glow_mol'),
    R('Light Cone / Glare', 'Haze / Fog', node='fog', tex='haze'),
    R('Light Cone / Glare', 'Light Glow', node='glow', tex='glow|beam_|flare'),
    R('Shadow', 'Soft Shadow', node='shad|shdw|_shd', tex='shdw|shad|_shd'),
    R('Lighting Truss', 'Lighting Truss', node='truss|_ten0|ar_ten|ten_|pipe|tube|gate_truss', tex='truss|ar_ten0|ar_ten1|ar_ten2'),
    R('Spotlight', 'Stage Light', node='_lt|_lig|light|lamp|denkyu|fluor|neon|led', tex='light_0|light0|lig0|pas_light|_lig|lamp|denkyu|head_light'),

    # ---- stage: titantron, ramp, curtains, equipment ----
    R('Titantron', 'Titantron', node='titan', tex='titan'),
    R('Ramp', 'Ramp Steps', tex='slo_step'),
    R('Ramp', 'Entrance Ramp', node='slo_|slope|ramp|_slo|bridge', tex='slope|slo_|ramp'),
    R('Stage', 'Stage Curtain', node='maku|curtain|gallery', tex='curtain|maku'),
    R('Stage Pillar', 'Speaker Tower', node='pylon', tex='speaker'),
    R('Stage Pillar', 'Stage Pillar', node='pylon|pillar|hasira', tex='hasira|pillar'),
    R('Stage Header', 'Stage Entrance Wall', node='_ent', tex='_ent0|_ent1|_ent2|_ent3|_ent4|tiwall'),
    R('Stage', 'Black Backdrop Panel', node='black_p', tex='p_bla|black_p'),
    R('Signage / Banner', 'WWE Logo Sign', node='wcore|logo|kanban', tex='wlogo|wweobj|wwelogo|logo'),
    R('Stage', 'Stage Equipment (cases, speakers, cables)', tex='ar_case|ar_cast|ar_cord|ar_speaker|speaker|cast'),

    # ---- stands, walls, ceilings, signage ----
    R('Signage / Banner', 'Advertising Billboard', node='billboard', tex='ad_'),
    R('Seats', 'VIP Box Seats', node='vip_box|boxseat|sta_vip_|met_vip|dome_st_vip', tex='fleet_vip|msg_vip'),
    R('Shadow', 'Stands Shadow', node='seat1f_shd|shd'),
    R('Signage / Banner', 'Banner', node='banner', tex='banner'),
    R('Signage / Banner', 'Sign / Scoreboard', node='sign|score|kanban|name', tex='ppv_sign|msg_score|msg_name|msg_flag'),
    R('Stands', 'Railing / Fence', node='st_fence|_fence|handrail', tex='st_fence'),
    R('Stands', 'Stands', node='seat|_stand|stand_|tuuro|aud', tex='fleet_fence|fleet_exit|fleet_sh|msg_fence|trump_seat|trump_wall|dome_st|met_st_seat|cp_wall|cp_stand'),
    R('Ceiling', 'Ceiling / Roof', node='ceil|cellin|roof|tenjou|tenjyou|dome_score', tex='roof|ceil|tenjou|tennjyou'),
    R('Wall', 'Window', node='window', tex='mado'),
    R('Wall', 'Door / Exit', node='door|exit|gate|fl_door', tex='door|exit'),
    R('Wall', 'Stairs', node='stair|kaidan|sheat', tex='stair|kaidan'),
    R('Wall', 'Wall', node='wall|kabe', tex='wall|kabe|cas_bg|_bg'),
    R('Floor', 'Floor', node='floor|room', tex='floor|ground|carpet'),
    R('Prop', 'Ladder', node='ladder|tlc_l', tex='ladder'),
    R('Prop', 'Chair', node='tlc_c', tex='tlc_c|pchair'),
    R('Prop', 'Table', node='tlc_t', tex='tlc_t'),
    R('Prop', 'Car', node='mib_car|trailer', tex='mib_car|pk_tra'),
    R('Prop', 'Locker Room Bench', node='bench', tex='loc_pipe'),
    R('Prop', 'Locker', node='_lc', tex='loc_seat'),
    R('Scenery', 'Palm Tree', node='palmtree|cp_tree', tex='_tree|cp_tr|cp_yashi'),
    R('Scenery', 'Sky', node='_sky', tex='_sky'),
    R('Scenery', 'Background Scenery', node='cp_bg|tower', tex='cp_hotel|cp_tower|cp_wood|_tower'),
    R('Lighting Truss', 'Cable / Wire', node='wire|rope|frame|flame|line', tex='_wire|ten_rope|light_line'),
    R('Prop', 'Backstage Prop', node='locker|parking|cable|duct|table|chair|case|plate|basket|tomato|wineg|isu|kabin|trophy|cloth|barbell|shelf|portrait|vincebust|drum|sprink|object|tab|clock|kyan|kaiga|yuka', tex='cr_|loc_|pk_|pas_|park_|78|bos_|stg68|tableware|nun_|hana|toma|chikyuu|jani_|kyan|kaiga|yuka'),
    R('Stage', 'Stage Structure', node='obj|_sol|star|noise|wwe|parm|s_board', tex='_sol|_s_iron|_noise|noise|_wwe|_parm|s_board'),
]

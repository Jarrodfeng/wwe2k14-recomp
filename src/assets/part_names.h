// Session 98 step 16 ("click to inspect" in the Arena Viewer): the friendly name of a clicked part of an arena or a
// wrestler, with the literal asset name under it. Header-only, plain C++17, no Windows or SDK headers (it builds for
// Android too). Our own annotations: short substrings of the pieces' own node / texture / material names mapped to plain
// names; no game data.
//
//   PartKey k; k.kind = PartKind::kArena; k.pac = "bg05.pac"; k.key = 392; k.node = "root"; k.texture = "ar_fence";
//   PartName n = assets::FriendlyPartName(k);   // {"Barricade Corner", "ar_fence", "bg05.pac slot 392", "Barricade Corner"}
//
// What the viewer must hand over per mesh (the loader knows all of it; Mesh::part already says "slot 392" / "piece 10000" /
// "crowd 3" / "ring steps 32"; the node name and the skin palette need two small additions to Mesh, see the hand-back):
//   kind      kArena (stage slots 0-399, ring set 951-982, stage-screen keys 800-809), kWrestler (body / hair / gear pieces),
//             kCrowd (audience bodies 20000+), kProp (bgEtc props: ring steps 29/32/35, announce desk 6/7)
//   pac       "bg05.pac" / "ch107.pac" / "bgEtc.pac" (only for the source line)
//   key       the pac entry: slot or key (arena, prop), entry id (wrestler: 0 body, 10000 hair ...), 20000+N (crowd)
//   node      the first node name of the piece's model (arenas: "ar_fence01_a"; empty is fine)
//   texture   the mesh's diffuse texture name ("ar_fence", "rin_mat00"; empty is fine)
//   shader    the material class ("yCh_skin", "yBG_Default"; only wrestlers need it)
//   palette   wrestlers: the mesh's skin-palette bone names joined by ';' ("Bip Head;Bip Neck"); empty falls back to the
//             material class ("Body Skin", "Attire", "Hair")
//   additive  the slot is blended additively (flag `a` of entry 50001): glows, beams, fog
//
// The rule table below is GENERATED from tools\assets\part_rules.py (`py -I tools\assets\part_names.py --emit-header
// src\assets\part_names.h`); edit the Python list, not the table. First rule that matches wins; a rule that names node and
// texture substrings matches when either hits; kind / key range / shader / palette / additive must all hold.
#pragma once

#include <cstddef>
#include <string>

namespace assets {

enum class PartKind { kArena = 1, kWrestler = 2, kCrowd = 3, kProp = 4 };

struct PartKey {
  PartKind kind = PartKind::kArena;
  std::string pac;      // "bg05.pac"
  int key = 0;          // slot / key / entry
  std::string node;     // first node name of the piece's model
  std::string texture;  // diffuse texture name
  std::string shader;   // material class
  std::string palette;  // wrestler skin-palette bone names, ';' separated
  bool additive = false;
};

struct PartName {
  std::string friendly;  // "Barricade Corner"
  std::string literal;   // "ar_fence01_a" (the node name; the texture name when the node name is generic)
  std::string source;    // "bg05.pac slot 392"
  std::string category;  // "Barricade Corner", "Ring Ropes", ..., "Unknown"
  int rule = -1;         // index into the rule table (-1 = no rule matched)
};

namespace part_names_detail {

struct Rule {
  const char* category;
  const char* name;
  int kind;  // 0 any, else PartKind
  int lo, hi;  // key range, 0..0 = any
  const char* node;    // '|'-separated lower-case substrings, "" = not tested
  const char* tex;
  const char* shader;
  const char* pal;
  int additive;  // 1 = only additive pieces, -1 = any
};

inline const Rule* Rules(size_t& n) {
  static const Rule kRules[] = {
// ---- GENERATED RULES BEGIN (tools\assets\part_names.py --emit-header) ----
    {"Head", "Eyes", 2, 0, 0, "", "", "", "l_eye|r_eye", -1},
    {"Hair", "Hair", 2, 0, 0, "", "", "", "kami_", -1},
    {"Hair", "Hair", 2, 0, 0, "", "", "aniso_trans", "", -1},
    {"Head", "Eyelids and Lashes", 2, 0, 0, "", "", "trans", "mabuta|meziri", -1},
    {"Hair", "Hair Piece", 2, 10000, 19999, "", "", "", "", -1},
    {"Gear", "Accessory Piece", 2, 20000, 39999, "", "", "", "", -1},
    {"Body", "Torso and Arms", 2, 0, 0, "", "", "skin", "spine|clavicle|bip pelvis|bip;", -1},
    {"Body", "Hands", 2, 0, 0, "", "", "skin", "finger|hand", -1},
    {"Body", "Forearms", 2, 0, 0, "", "", "skin", "forearm", -1},
    {"Body", "Legs", 2, 0, 0, "", "", "skin", "thigh|calf|foot|toe", -1},
    {"Body", "Torso and Arms", 2, 0, 0, "", "", "skin", "upperarm", -1},
    {"Head", "Face and Head", 2, 0, 0, "", "", "skin", "head|neck|kuchi|hoho|mayu|hana", -1},
    {"Body", "Body Skin", 2, 0, 0, "", "", "skin", "", -1},
    {"Attire", "Trunks, Pants and Top", 2, 0, 0, "", "", "gear", "spine|pelvis|clavicle|thigh_u|bip;", -1},
    {"Gear", "Gloves and Hand Tape", 2, 0, 0, "", "", "gear", "finger|hand", -1},
    {"Pads", "Arm Pads and Sleeves", 2, 0, 0, "", "", "gear", "upperarm|forearm|elbow", -1},
    {"Gear", "Boots and Leg Wear", 2, 0, 0, "", "", "gear", "foot|calf|toe|thigh", -1},
    {"Gear", "Face Detail (beard, paint, mask)", 2, 0, 0, "", "", "gear", "head|neck|kuchi|hoho|mayu|hana", -1},
    {"Attire", "Attire", 2, 0, 0, "", "", "gear", "", -1},
    {"Gear", "Accessory", 2, 0, 0, "", "", "trans", "", -1},
    {"Gear", "Accessory", 2, 0, 0, "", "", "reflect", "", -1},
    {"Body", "Body Part", 2, 0, 0, "", "", "", "", -1},
    {"Crowd", "Crowd Member", 3, 0, 0, "", "", "", "", -1},
    {"Crowd", "Crowd (flat audience cards)", 0, 0, 0, "", "2d_audi|2d_kyaku|2d_aud", "", "", -1},
    {"Titantron", "Titantron Screen", 1, 800, 809, "", "titantron_movie", "", "", -1},
    {"Mini-Tron", "Mini-Tron", 1, 800, 809, "", "mini_tron", "", "", -1},
    {"Stage Header", "Stage Header and Wall", 1, 800, 809, "", "header&wall", "", "", -1},
    {"Titantron", "Main Screen and Banner", 1, 800, 809, "", "main&banner", "", "", -1},
    {"Titantron", "Stage Screen", 1, 800, 809, "", "", "", "", -1},
    {"Titantron", "Titantron (screen off)", 0, 0, 0, "wall_tron", "", "", "", -1},
    {"Stage Header", "Stage Header Screen (off)", 0, 0, 0, "head_tron", "", "", "", -1},
    {"Mini-Tron", "Mini-Tron (screen off)", 0, 0, 0, "mini_tron", "", "", "", -1},
    {"Titantron", "Banner Screen (off)", 0, 0, 0, "banner_tron", "", "", "", -1},
    {"Ring", "Ring Base (under the ring)", 0, 0, 0, "ring_sita", "", "", "", -1},
    {"Ring Post", "Ring Post", 1, 951, 959, "ar_post", "rin_pos", "", "", -1},
    {"Turnbuckle", "Turnbuckle", 1, 951, 982, "", "rin_tb2", "", "", -1},
    {"Turnbuckle", "Turnbuckle Cover", 1, 951, 982, "ar_cover", "rin_tb", "", "", -1},
    {"Ring", "Ring Canvas", 1, 951, 982, "ar_ring", "rin_mat", "", "", -1},
    {"Ring Ropes", "Ring Ropes", 1, 951, 982, "ar_rope", "ar_rop", "", "", -1},
    {"Ring Apron", "Ring Apron (skirt)", 1, 951, 982, "", "rin_ep", "", "", -1},
    {"Ring Steps", "Ring Steps", 4, 29, 35, "", "", "", "", -1},
    {"Ring Steps", "Ring Steps", 0, 0, 0, "", "ar_stair", "", "", -1},
    {"Announce Desk", "Announce Desk", 4, 6, 7, "", "", "", "", -1},
    {"Announce Desk", "Announce Chair", 0, 0, 0, "ji_chair", "ji_chair", "", "", -1},
    {"Announce Desk", "Announce Desk", 0, 0, 0, "", "ji_desk|ji_mon|ji_top|ji_bl|ji_meta", "", "", -1},
    {"Signage / Banner", "Flag", 0, 0, 0, "flag", "flag", "", "", -1},
    {"Barricade Corner", "Barricade Corner Sign", 0, 390, 399, "", "ar_fen_wwe", "", "", -1},
    {"Barricade Corner", "Barricade Corner", 0, 390, 399, "", "ar_fence|rin_fen", "", "", -1},
    {"Barricade", "Barricade Shadow", 0, 0, 0, "fen_sdw|fence_sha|sak_s", "fen_sdw|fence_sha|sak_sh", "", "", -1},
    {"Barricade", "Barricade Sign (WWE)", 0, 0, 0, "", "ar_fen_wwe", "", "", -1},
    {"Barricade", "Barricade", 0, 0, 0, "ar_fence", "ar_fence|fence_b", "", "", -1},
    {"Barricade", "Barricade Fence Panel", 0, 0, 0, "ar_sak|_sak", "ar_sak", "", "", -1},
    {"Floor Mat", "Floor Mat", 0, 0, 0, "dwnmt|downmt", "ar_dm", "", "", -1},
    {"Floor", "Ringside Floor", 0, 0, 0, "ar_ground", "", "", "", -1},
    {"Seats", "Ringside Chair", 0, 0, 0, "arena_chair", "stand_chair", "", "", -1},
    {"Light Cone / Glare", "Moving Spotlight Beam", 0, 0, 0, "glow_mo", "", "", "", 1},
    {"Light Cone / Glare", "Light Beam", 0, 0, 0, "", "beam_", "", "", 1},
    {"Light Cone / Glare", "Light Cone / Glare", 0, 0, 0, "", "", "", "", 1},
    {"Spotlight", "Moving Spotlight", 0, 0, 0, "glow_mo|_mo_l|mo1l|mo0l|lt_mo|lt_mov|glow_mol", "", "", "", -1},
    {"Light Cone / Glare", "Haze / Fog", 0, 0, 0, "fog", "haze", "", "", -1},
    {"Light Cone / Glare", "Light Glow", 0, 0, 0, "glow", "glow|beam_|flare", "", "", -1},
    {"Shadow", "Soft Shadow", 0, 0, 0, "shad|shdw|_shd", "shdw|shad|_shd", "", "", -1},
    {"Lighting Truss", "Lighting Truss", 0, 0, 0, "truss|_ten0|ar_ten|ten_|pipe|tube|gate_truss", "truss|ar_ten0|ar_ten1|ar_ten2", "", "", -1},
    {"Spotlight", "Stage Light", 0, 0, 0, "_lt|_lig|light|lamp|denkyu|fluor|neon|led", "light_0|light0|lig0|pas_light|_lig|lamp|denkyu|head_light", "", "", -1},
    {"Titantron", "Titantron", 0, 0, 0, "titan", "titan", "", "", -1},
    {"Ramp", "Ramp Steps", 0, 0, 0, "", "slo_step", "", "", -1},
    {"Ramp", "Entrance Ramp", 0, 0, 0, "slo_|slope|ramp|_slo|bridge", "slope|slo_|ramp", "", "", -1},
    {"Stage", "Stage Curtain", 0, 0, 0, "maku|curtain|gallery", "curtain|maku", "", "", -1},
    {"Stage Pillar", "Speaker Tower", 0, 0, 0, "pylon", "speaker", "", "", -1},
    {"Stage Pillar", "Stage Pillar", 0, 0, 0, "pylon|pillar|hasira", "hasira|pillar", "", "", -1},
    {"Stage Header", "Stage Entrance Wall", 0, 0, 0, "_ent", "_ent0|_ent1|_ent2|_ent3|_ent4|tiwall", "", "", -1},
    {"Stage", "Black Backdrop Panel", 0, 0, 0, "black_p", "p_bla|black_p", "", "", -1},
    {"Signage / Banner", "WWE Logo Sign", 0, 0, 0, "wcore|logo|kanban", "wlogo|wweobj|wwelogo|logo", "", "", -1},
    {"Stage", "Stage Equipment (cases, speakers, cables)", 0, 0, 0, "", "ar_case|ar_cast|ar_cord|ar_speaker|speaker|cast", "", "", -1},
    {"Signage / Banner", "Advertising Billboard", 0, 0, 0, "billboard", "ad_", "", "", -1},
    {"Seats", "VIP Box Seats", 0, 0, 0, "vip_box|boxseat|sta_vip_|met_vip|dome_st_vip", "fleet_vip|msg_vip", "", "", -1},
    {"Shadow", "Stands Shadow", 0, 0, 0, "seat1f_shd|shd", "", "", "", -1},
    {"Signage / Banner", "Banner", 0, 0, 0, "banner", "banner", "", "", -1},
    {"Signage / Banner", "Sign / Scoreboard", 0, 0, 0, "sign|score|kanban|name", "ppv_sign|msg_score|msg_name|msg_flag", "", "", -1},
    {"Stands", "Railing / Fence", 0, 0, 0, "st_fence|_fence|handrail", "st_fence", "", "", -1},
    {"Stands", "Stands", 0, 0, 0, "seat|_stand|stand_|tuuro|aud", "fleet_fence|fleet_exit|fleet_sh|msg_fence|trump_seat|trump_wall|dome_st|met_st_seat|cp_wall|cp_stand", "", "", -1},
    {"Ceiling", "Ceiling / Roof", 0, 0, 0, "ceil|cellin|roof|tenjou|tenjyou|dome_score", "roof|ceil|tenjou|tennjyou", "", "", -1},
    {"Wall", "Window", 0, 0, 0, "window", "mado", "", "", -1},
    {"Wall", "Door / Exit", 0, 0, 0, "door|exit|gate|fl_door", "door|exit", "", "", -1},
    {"Wall", "Stairs", 0, 0, 0, "stair|kaidan|sheat", "stair|kaidan", "", "", -1},
    {"Wall", "Wall", 0, 0, 0, "wall|kabe", "wall|kabe|cas_bg|_bg", "", "", -1},
    {"Floor", "Floor", 0, 0, 0, "floor|room", "floor|ground|carpet", "", "", -1},
    {"Prop", "Ladder", 0, 0, 0, "ladder|tlc_l", "ladder", "", "", -1},
    {"Prop", "Chair", 0, 0, 0, "tlc_c", "tlc_c|pchair", "", "", -1},
    {"Prop", "Table", 0, 0, 0, "tlc_t", "tlc_t", "", "", -1},
    {"Prop", "Car", 0, 0, 0, "mib_car|trailer", "mib_car|pk_tra", "", "", -1},
    {"Prop", "Locker Room Bench", 0, 0, 0, "bench", "loc_pipe", "", "", -1},
    {"Prop", "Locker", 0, 0, 0, "_lc", "loc_seat", "", "", -1},
    {"Scenery", "Palm Tree", 0, 0, 0, "palmtree|cp_tree", "_tree|cp_tr|cp_yashi", "", "", -1},
    {"Scenery", "Sky", 0, 0, 0, "_sky", "_sky", "", "", -1},
    {"Scenery", "Background Scenery", 0, 0, 0, "cp_bg|tower", "cp_hotel|cp_tower|cp_wood|_tower", "", "", -1},
    {"Lighting Truss", "Cable / Wire", 0, 0, 0, "wire|rope|frame|flame|line", "_wire|ten_rope|light_line", "", "", -1},
    {"Prop", "Backstage Prop", 0, 0, 0, "locker|parking|cable|duct|table|chair|case|plate|basket|tomato|wineg|isu|kabin|trophy|cloth|barbell|shelf|portrait|vincebust|drum|sprink|object|tab|clock|kyan|kaiga|yuka", "cr_|loc_|pk_|pas_|park_|78|bos_|stg68|tableware|nun_|hana|toma|chikyuu|jani_|kyan|kaiga|yuka", "", "", -1},
    {"Stage", "Stage Structure", 0, 0, 0, "obj|_sol|star|noise|wwe|parm|s_board", "_sol|_s_iron|_noise|noise|_wwe|_parm|s_board", "", "", -1},
// ---- GENERATED RULES END ----
  };
  n = sizeof kRules / sizeof kRules[0];
  return kRules;
}

inline std::string Lower(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return r;
}

// true when any of the '|'-separated substrings of `subs` occurs in `text`
inline bool AnyOf(const char* subs, const std::string& text) {
  const char* p = subs;
  while (true) {
    const char* e = p;
    while (*e && *e != '|') ++e;
    const std::string s(p, size_t(e - p));
    if (!s.empty() && text.find(s) != std::string::npos) return true;
    if (!*e) return false;
    p = e + 1;
  }
}

inline bool Generic(const std::string& node) {
  return node.empty() || node == "root" || node == "vector" || node == "null";
}

}  // namespace part_names_detail

inline PartName FriendlyPartName(const PartKey& k) {
  using namespace part_names_detail;
  size_t n = 0;
  const Rule* rules = Rules(n);
  const std::string node = Lower(k.node), tex = Lower(k.texture), sh = Lower(k.shader), pal = Lower(k.palette);
  PartName out;
  out.literal = Generic(node) && !k.texture.empty() ? k.texture : k.node;
  if (out.literal.empty()) out.literal = k.texture;
  const char* what = k.kind == PartKind::kWrestler ? "entry" : k.kind == PartKind::kCrowd ? "crowd body" : "slot";
  out.source = (k.pac.empty() ? std::string("?") : k.pac) + " " + what + " " + std::to_string(k.key);
  for (size_t i = 0; i < n; ++i) {
    const Rule& r = rules[i];
    if (r.kind != 0 && r.kind != int(k.kind)) continue;
    if (r.hi != 0 && (k.key < r.lo || k.key > r.hi)) continue;
    if (r.additive == 1 && !k.additive) continue;
    if (*r.shader && !AnyOf(r.shader, sh)) continue;
    if (*r.pal && !AnyOf(r.pal, pal)) continue;
    if (*r.node || *r.tex) {
      const bool hit = (*r.node && AnyOf(r.node, node)) || (*r.tex && AnyOf(r.tex, tex));
      if (!hit) continue;
    }
    out.friendly = r.name, out.category = r.category, out.rule = int(i);
    return out;
  }
  out.friendly = "Unknown part (slot " + std::to_string(k.key) + ")";
  out.category = "Unknown";
  return out;
}

}  // namespace assets

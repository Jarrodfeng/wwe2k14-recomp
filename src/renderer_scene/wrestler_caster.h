// S111 LAYERS review (docs\research\s111\LAYERS.md section Review): which of a wrestler's parts the game's caster passes draw. ONE predicate for every caster pass of ours
// (live_map_shadow.cpp CasterOf and the map B loop, live_map_rim.cpp, live_map_chain.cpp MeasureRimDepth, live_map_plane.cpp) and the units / rigs that copy their set
// (tools\scene\castcull_unit.cpp Real, tools\scene\frame_rig.cpp SkinnedBox). Pure: no renderer, no game.
//
// The game [C listings]:
//  - A caster pass draws a model's parts through the same draw NODES as the colour pass: in a shadow-map render (flag table 0x83929A48) model slot 13 / 31 hand their layer to
//    the shadow pass (vtable +188, sub_82C01FE0; sub_82C13C88 +0x50, sub_82C13DA8 +0x54), in the rim render (table 0x83929A60) to the rim pass (+196; +0x80 / +0x84). The pass
//    tests only the part's tag against that layer (sub_82C01FE0 0x82C0211C..0x82C02130), so WHICH parts cast is decided by which nodes the map walks:
//      the opaque node (sub_825A3908, categories 13..21 and 42: sub_827E6F40) - the stock body, a created character's layers 0..4;
//      the translucent node (DRTR sub_825ABC20, category 33: sub_825ABB98 +0x28 -> sub_8259FAE8) - every SUBM piece of a stock owner (whole), a created character's layers 5..7.
//  - Map A (sub_827E8448 +0x114) walks 13..21 + 42 only. Map B (sub_827E7338) walks them, then 33 (and 43) unless the scene word 0x836F3940 is 1 (+0xBC / +0xE8; mode 1 is
//    only taken when the word is 1). The rim map (sub_827EA098 +0x138 -> sub_827E97A0 +0x7C..+0xB8) walks 13..21 + 42 and 33 + 43 always. The plane pass: not read [U]
//    (ours keeps the opaque node's parts there, as before).
//  - The translucent node turns the alpha test on (+0x168), but the caster effects bind no picture (yShadowMap / yRimLightMap: every pass's sampler list is empty in
//    src\renderer\shaders\fx\*.fx.json), so a translucent part casts SOLID, as ours draws it.
// The key is Material::translucent_draw (the draw split, assets\char_draw_layer.h), never Material::alpha_blend: the front end and the loaders also set alpha_blend for other
// reasons (a texel cut-out blend, front_end.cpp; a slot blend, asset_load.h), which say nothing about the node that draws the part.
#pragma once
#include <cstdint>

#include "../assets/asset_model.h"

namespace rdr_scene::wrestler_caster {

enum class Pass : uint8_t { MapA, MapB, Rim, Plane };

// Does pass `p` walk the translucent node (category 33)? mapb_mode0 = ours draws map B's mode 0 (sub_827E8080: not the wrestlers-only mode 1, the switch on, the scene word
// known); phase_word = the scene word (-1 = not in the record: no).
inline bool WalksTranslucent(Pass p, bool mapb_mode0, int64_t phase_word) {
  if (p == Pass::Rim) return true;
  if (p == Pass::MapB) return mapb_mode0 && phase_word >= 0 && phase_word != 1;
  return false;   // map A; the plane pass [U]
}

// The part is drawn by the character's translucent node.
inline bool InTranslucentNode(const assets::Material& m) { return m.translucent_draw; }

// THE predicate: a mesh of material `m` is a wrestler caster of a pass that walks (walks_translucent) or does not walk the translucent node.
// S111 UNIFY5 review: "a wrestler part" is Material::char_part (the part belongs to a character model, set by both builders), not the effect name: a created superstar's yDefault part casts like its yCh_skin parts.
inline bool Casts(const assets::Material& m, bool walks_translucent) { return m.char_part && (walks_translucent || !InTranslucentNode(m)); }

// S111 PIECEROUTE review (scene_live\slot22_pieces.h; docs\research\s111\PIECEROUTE.md Review): a pieces-only owner (kObjPiecesOnly: the game drew no body, the character's translucent node drew its
// SUBM pieces through slot 22) casts where that node is walked: map B walks categories 33 / 43 unless the scene word is 1 (sub_827E7338 +0xCC / +0xF0 / +0x154), the rim map always (sub_827EA098); each walk
// calls the enabled node (sub_82C2F960 +0x6C..+0x84) and kind 4 / kind 2 slot 22 reads no pass flag. A visible body is decided as before (Casts and each pass's translucent rule).
inline bool ObjectMayCast(bool visible, bool pieces_only, bool walks_translucent) { return visible || (pieces_only && walks_translucent); }
// The meshes it casts: its SUBM piece meshes only (`piece_mesh` = piece_row_rule.h PiecesOnlyMeshDrawn), never its body.
inline bool MeshCastsFor(bool visible, bool pieces_only, bool piece_mesh) { return visible || (pieces_only && piece_mesh); }

}  // namespace rdr_scene::wrestler_caster

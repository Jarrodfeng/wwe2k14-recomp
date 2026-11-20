// The order of the see-through draws in our colour pass (Session 103, lane F; docs\renderer\SEE_THROUGH_ORDER.md). Pure: no renderer, no game.
//
// What the game does [C: tools\scene\f_order_stability.py, brock_losswin + brock_losswin_rt, 536 frames with a moving camera, 8,114 object pairs, 0 flipped]:
// the see-through objects (the stage's glass / net / banner pieces, the wrestlers' hair and translucent gear) are drawn in SUBMIT order (the order the scene
// walk reaches the objects), not far to near. Across the groups [C: tools\scene\f_order_census.py --summary, three arenas]: the stage's alpha-blended pieces
// first, then the wrestlers' translucent parts, then every additive piece (light beams, glows). The viewer's own rule (and the front-end's default) is far to
// near, which flips pairs as the camera moves.
//
// Game mode: three ranks, each in the record's own order (a contributor writes objects in the game's walk order; the stable sort keeps it, and keeps a
// two-pass hair piece's front-face pass before its back-face pass; pass 1 culls back faces, i.e. draws the front faces):
//   rank 0  alpha blend, not a character effect   rank 1  alpha blend, a character effect (yCh_*)   rank 2  additive (src alpha / one)
// Exception (the loader's own blends, not the game's see-through parts): a material the asset loader made blended from an a / s slot (cutout 2) is not a game
// see-through piece; it stays far to near, after the other items of its rank. A prop cut-out the loader made blended (cutout 1) is drawn WITH depth write
// by the front-end (a cut-out, as the game draws it) and stays in the record order.
// Distance mode: far to near over everything (today's behaviour, byte for byte).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rdr_scene {

enum class SeeOrder : uint8_t {
  Distance = 0,  // the viewer's rule: far to near (default; today)
  Game = 1,      // the game's: submit order inside three ranks
};

// The mode the environment asks for, for the offline tools: WWE_RDR_ORDER=game | distance (anything else = the frame's own choice). The variable is read once.
inline bool SeeOrderFromEnv(SeeOrder& out) {
  static const int mode = [] {
    const char* e = std::getenv("WWE_RDR_ORDER");
    if (!e) return -1;
    if (std::strcmp(e, "game") == 0) return int(SeeOrder::Game);
    if (std::strcmp(e, "distance") == 0) return int(SeeOrder::Distance);
    return -1;
  }();
  if (mode < 0) return false;
  out = SeeOrder(mode);
  return true;
}

// What the order reads of one see-through item.
struct SeeKey {
  uint8_t blend = 1;       // 1 alpha, 2 additive (the front-end's Item::blend)
  bool character = false;  // the material is a character effect (yCh_*)
  uint8_t cutout = 0;      // Material::cutout_blend: 2 = an a / s slot blend (stays far to near)
  float dist2 = 0;         // squared distance of the mesh centre to the eye
  // S105 SZW (docs/research/harness/SZW_contract.md, ZW1): the game's order inside its `s` and `a` groups for a cutout == 2 item: -1 = none (far to near, the rule before); else the position in the game's list =
  // (light-set run, first seen first) * 1024 + stage slot, so the group draws in ascending slot number inside light-set runs (sub_825B9F10 + sub_825B9970), never by distance. Filled by FrontEnd::Record only behind wwe2k14_rdr_stage_zwrite.
  int32_t order = -1;
};

inline int SeeRank(const SeeKey& k) { return k.blend == 2 ? 2 : (k.character ? 1 : 0); }

inline bool SeeThroughBefore(SeeOrder mode, const SeeKey& a, const SeeKey& b) {
  if (mode == SeeOrder::Game) {
    const int ra = SeeRank(a), rb = SeeRank(b);
    if (ra != rb) return ra < rb;
    const bool sa = a.cutout == 2, sb = b.cutout == 2;
    if (sa != sb) return sb;          // the loader's slot blends after the game's own pieces of the rank
    if (sa && a.order >= 0 && b.order >= 0) return a.order < b.order;   // S105 SZW: the game's own list order (slot ascending inside light-set runs), not by distance
    return sa && a.dist2 > b.dist2;   // ... far to near among themselves; everything else keeps the record order
  }
  return a.dist2 > b.dist2;
}

// Stable-sorts a list of items; `key_of(item)` gives its SeeKey. The input order is the record's submit order.
template <class T, class KeyOf>
void SortSeeThrough(SeeOrder mode, std::vector<T>& items, KeyOf key_of) {
  std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) { return SeeThroughBefore(mode, key_of(a), key_of(b)); });
}

}  // namespace rdr_scene

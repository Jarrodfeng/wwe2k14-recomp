#pragma once
// wwe2k14 - Session 100 (Jarrod: "Crowd culling occasionally still happens on screen ... distance based culling, crowd far away on
// screen, but it's far too aggressive perhaps?"). The game's stands crowd changes form by distance: the crowd manager's
// sub_825132A8 (crowd_lod_update_distances, docs\systems\arena_look.md "Crowd cards (LOD)") writes a near distance N (guest
// 0x83762F94) and a step S (0x83762F98) every frame from the camera height and FOV; each stands member whose clip w is at least
// N + 3 S is drawn as one flat T_Audience2D card (an impostor picture the game renders at run time) instead of its 3D body.
//
// --wwe2k14_crowd_card_distance = F (default 1 = the game's own): after the game's update, N and S are multiplied by F, so the 3D
// bodies reach F times as far before the cards take over (F large = no cards). Addresses from Session 98's listings and probes
// (arena_look.md, [C]). One line logs the game's own N and S the first time, so the real values are known.

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(wwe2k14_crowd_card_distance, 1.0, "wwe2k14",
                      "Session 100: the stands crowd's 3D-body distance as a multiple of the game's own (its near distance N and step S, "
                      "recomputed each frame by sub_825132A8, times this; past N + 3 S a member is a flat 2D card). 1 = the game's own; "
                      "large = 3D bodies everywhere (src/crowd_cards.h)");

namespace wwe2k14::crowd_cards {

inline constexpr uint32_t kNearDistance = 0x83762F94u, kStep = 0x83762F98u;  // Session 98 (arena_look.md), [C]

inline float LoadF32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address, 4);
  return std::bit_cast<float>(std::byteswap(be));
}
inline void StoreF32(uint8_t* base, uint32_t address, float value) {
  const uint32_t be = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(base + address, &be, 4);
}

inline std::atomic<uint32_t> g_logged{0};

}  // namespace wwe2k14::crowd_cards

namespace wwe2k14::crowd_cards {
inline std::atomic<uint64_t> g_updates{0};
}  // Session 87's scene_crowd.h already hooks the card loop sub_82512F70 (~1,400 card draws a frame in entrances)

REX_EXTERN(__imp__sub_825132A8);
REX_HOOK_RAW(sub_825132A8) {
  using namespace wwe2k14::crowd_cards;
  __imp__sub_825132A8(ctx, base);
  const float n = LoadF32(base, kNearDistance), s = LoadF32(base, kStep);
  const double factor = REXCVAR_GET(wwe2k14_crowd_card_distance);
  if (g_logged.fetch_add(1) == 0) {
    REXLOG_INFO("[crowd] card distance: the game's near distance N {:.1f}, step S {:.1f} (cards from N + 3 S = {:.1f}); x {:.2f}", n, s,
                n + 3.0f * s, factor);
  }
  if (g_updates.fetch_add(1) % 600 == 599) {
    REXLOG_INFO("[crowd] distance updates {}; N {:.1f} S {:.1f}", g_updates.load(), n, s);
  }
  if (factor != 1.0 && factor > 0.0) {
    StoreF32(base, kNearDistance, float(n * factor));
    StoreF32(base, kStep, float(s * factor));
  }
}

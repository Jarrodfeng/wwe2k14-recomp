// S101 lane 86 (SH0, docs\renderer\SHADOW_PROMOTION_MAP.md section 6): which draws are the dynamic shadow casters, and which render targets are
// shadow maps, decided by the draw's ORIGIN (names.h DrawOrigin: where the game submitted it) and its target's SURFACE ROLE, not by shader hashes.
// Read by the binder for SHADOWS off (g_shadows) and the half-rate maps (g_shadow_half): both ask the same question (IsCaster), so they cannot disagree.
// No game or SDK includes (the checker tools\shadow\shadow_roles_check.cpp builds this header on its own).
//
// The sites (guest addresses; all [C] from the Check B census, tools\shadow\shadow_census.py, SHADOW_PROMOTION_MAP.md 1.1):
//   0x82C04634  the call of the part-draw sub_82BDED40 inside T_ShadowMap (sub_82C01FE0): the props map, wrestler map A, map B casters
//   0x82BF4D38  the same call inside T_PlaneShadowMap (sub_82BF22C8): the ring mat's plane mask casters
//   0x828131E4  the recorder call of the circle shadow (sub_82812E90, T_CircleShadow): one blob per character
//   0x82BFBD58  the same call inside T_RimLightMap (sub_82BF9878): the rim light map, which is NOT a shadow (the wrestler materials read it for the rim)
// A draw with no name (a version-1 capture, or the reader's names off) falls back to the old shader-hash lists (Legacy*), one draw at a time.
#pragma once
#include <cstdint>
#include <unordered_set>

#include "names.h"

namespace rdr_adapter::shadow_roles {

inline constexpr uint32_t kMapPassSite = 0x82C04634u;
inline constexpr uint32_t kPlanePassSite = 0x82BF4D38u;
inline constexpr uint32_t kCircleCallSite = 0x828131E4u;
inline constexpr uint32_t kRimPassSite = 0x82BFBD58u;

// ---- the old hash lists (Session 85 / S12): the fallback for unnamed draws, and the twin the checker compares against --------------------
inline bool LegacyChainNot(uint64_t ps_hash) {
  return ps_hash == 0xD03E7D4C719B0EDBull || ps_hash == 0x632189BAFAE233F0ull || ps_hash == 0x7ABDFEFBCADB4BF9ull;
}
// The old path's casters (SDK vulkan/command_processor.cpp DynamicShadowCasterKind): the wrestler map caster, the circle shadow, the ring mask /
// props casters (a flat pixel shader with one of two vertex shaders, so the main pass's draws of the same meshes stay).
inline bool LegacyCaster(uint64_t ps_hash, uint64_t vs_patched_hash) {
  if (ps_hash == 0xDA494C9383E18961ull || ps_hash == 0xD05BFEA12F4350ADull) return true;
  return ps_hash == 0xA4A965C189287B99ull && (vs_patched_hash == 0xA96C8C4D28D28F80ull || vs_patched_hash == 0xA878C3FD5E27FB35ull);
}

// ---- dev switches (tools only; a player build never sets them) ------------------------------------------------------------------------
// g_hash_keying: classify by the old hash lists only (the picture A/B in tools\shadow\shadow_roles_check.py, and the planted fault `hash_only`).
// g_rim_as_shadow: the planted fault `rim_as_shadow` (the rim light map counted as a shadow map).
inline bool g_hash_keying = false;
inline bool g_rim_as_shadow = false;

inline bool Named(const DrawOrigin* o) { return o && o->op != 0; }
inline bool IsRimRole(uint8_t role) { return role == uint8_t(SurfaceRole::RimMap); }
inline bool Is2D(DrawClass c) { return c == DrawClass::Sprite || c == DrawClass::Text || c == DrawClass::Movie; }

// A dynamic shadow caster: the draw was submitted by a shadow pass (or the circle shadow). The rim is kept out by its origin; the rim_map role is read only by IsChainNot (S101 review F2: that one creator row is [L], so a wrong role must never hide a real caster).
// `target_role` = names.h SurfaceRole of the colour (else depth) target the draw goes into (0 when unknown).
inline bool IsCaster(const DrawOrigin* o, uint64_t ps_hash, uint64_t vs_patched_hash, uint8_t target_role) {
  if (g_hash_keying || !Named(o)) return LegacyCaster(ps_hash, vs_patched_hash);
  if (o->pass_site == kMapPassSite || o->pass_site == kPlanePassSite || o->call_site == kCircleCallSite) return true;
  return g_rim_as_shadow && o->pass_site == kRimPassSite;
}

// A draw that makes its target something other than a shadow map: the rim pass (and the rim chain's draws, which go into the rim_map role) or a
// 2D draw (the copy quad). A target one of these draws into is never skipped by the half-rate maps (the SDK's ShadowChainNotShadow rule).
inline bool IsChainNot(const DrawOrigin* o, uint64_t ps_hash, uint8_t target_role) {
  if (g_hash_keying || !Named(o)) return LegacyChainNot(ps_hash) || (IsRimRole(target_role) && !g_rim_as_shadow);
  if (g_rim_as_shadow) return Is2D(o->klass);
  return IsRimRole(target_role) || o->pass_site == kRimPassSite || Is2D(o->klass);
}

// The half-rate maps' learning (binder.h): the game's image ids of the targets casters draw into, and of the targets other draws also go into.
struct Sets {
  std::unordered_set<uint32_t> targets, excluded;
  // One draw into `image` (the colour target, else the depth target) whose role is `role`.
  void Observe(const DrawOrigin* o, uint64_t ps_hash, uint64_t vs_patched_hash, uint32_t image, uint8_t role) {
    if (!image) return;
    if (IsCaster(o, ps_hash, vs_patched_hash, role)) targets.insert(image);
    else if (IsChainNot(o, ps_hash, role)) excluded.insert(image);
  }
  bool Skips(uint32_t image) const { return targets.count(image) && !excluded.count(image); }
  bool Empty() const { return targets.empty(); }
};

}  // namespace rdr_adapter::shadow_roles

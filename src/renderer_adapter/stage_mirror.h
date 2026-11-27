// S103 T7 (--wwe2k14_dump_targets=F1,F2,..., dev diagnosis, default empty = off): the STAGE MIRROR's game side. At each listed guest frame the game's frame is walked once and every
// in-between render target is named by ROLE from the draws that wrote it (the origin's technique, the generic post queue sites, the target's surface role); the end-of-frame contents of each
// are read back and written under out\s103\stage_mirror\ as game_<role>[.<n>]_f<frame>.rgba8 (u32 width, u32 height, RGBA8 rows) + .png, with a manifest stage_mirror_f<frame>.txt.
// OUR maps (LiveMaps by MapRole) are written beside them as ours_<role>[_<p0>]_f<frame>.rgba8 by LiveSceneDraw::SetMapDump (the mirror drawer); tools\scene\stage_mirror.py pairs them.
// No game or SDK includes (names.h / adapter_frame.h only): tools\scene\stage_mirror_lint.py reads this file as text. Off: the frame list is empty and nothing below runs.
// Marks: the role table is DECLARED from origin_table.h's technique names (db\origin_table.json); the three [U] rows are named in the table. Readback caveat: a target that the frame
// reuses (a blur ping-pong) is read once, at the END of the frame, so a role holds the last write of its last image; ReadImage scales RGBA16F by the image's peak (both sides alike).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>
#include <string>
#include <vector>

#include "adapter_frame.h"
#include "names.h"

namespace rdr_adapter::stage_mirror {

// The generic post queue sites (post_census.py SITE_QUEUE / SITE_COMBINE; origin_table.h comment "Op 68 inside a post scope").
inline constexpr uint32_t kPostQueueSite = 0x82C1F48Cu;
inline constexpr uint32_t kPostCombineSite = 0x82C1F674u;

inline std::vector<uint64_t> ParseFrames(const std::string& list) {
  std::vector<uint64_t> out;
  for (size_t at = 0; at < list.size();) {
    const size_t comma = list.find(',', at);
    const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (!item.empty()) out.push_back(std::strtoull(item.c_str(), nullptr, 10));
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return out;
}

struct TechRole { const char* technique; const char* role; };
// technique (origin_table.h kPasses / kSubmitters) -> the role of the target it draws into. [U] marks a row whose role is inferred from the creator row names in surface_role_table.h, not measured.
inline constexpr TechRole kTechRoles[] = {
    {"T_ShadowMap", "shadow_map"},              // props map, wrestler maps A / B: one role, indexed by image in first-draw order
    {"T_ShadowMapBlur", "shadow_blur"},
    {"T_PlaneShadowMap", "plane_mask"},
    {"T_PlaneShadowBlur", "plane_blur"},
    {"T_RimLightMap", "rim_map"},
    {"T_GaussianBlur5x5", "rim_node_quad"},      // [U] 0x827EA098 is the rim node's frame function (it sits beside the rim creator row 0x827EA4F0); the rim blur itself draws T_ShadowMapBlur (RIM_MAP_OURS.md:56, rim_chain_ours.h (kRimBlurCallSite))
    {"T_CircleShadow", "circles"},
    {"T_EffectMask_DownScale", "effect_mask"},   // [U]
    {"T_Audience_LightMap", "audience_lightmap"},
    {"T_CalculateAdaptedLum", "lum_adapt"},
    {"T_SampleLum", "lum_sample"},
    {"T_DownScale2x2DownColor", "bloom_down"},
    {"T_Bloom", "bloom"},
    {"T_Star", "glare_star"},
    {"T_Final", "post_final"},
    {"T_PostEffectHalation", "halation"},
    {"T_DownScale2x2_DOF", "dof_down"},
    {"T_GaussianBlur", "dof_blur"},
    {"T_PostEffectDefocusAdjustHue", "dof_hue"},
    {"T_PartsBlur", "parts_blur"},
    {"T_Antialiasing", "antialias"},
};

// The role a draw writes under, "" when none of the table's rows names it. `target_role` = names.h SurfaceRole of the colour target.
inline const char* RoleOfDraw(const DrawOrigin* o, const char* technique, uint8_t target_role) {
  if (o && (o->call_site == kPostCombineSite || o->part == kPostCombineSite)) return "final_combine";   // post_census.py: exactly one per chain frame
  // The rim chain's blur draws T_ShadowMapBlur INTO a rim_map target: by the target's role it is the rim blur, never a wrestler shadow blur (it would also crowd a wrestler blur out of kMaxPerRole).
  if (SurfaceRole(target_role) == SurfaceRole::RimMap && !(technique && !std::strcmp(technique, "T_RimLightMap"))) return "rim_blur";
  if (technique && *technique)
    for (const TechRole& r : kTechRoles)
      if (!std::strcmp(technique, r.technique)) return r.role;
  switch (SurfaceRole(target_role)) {
    case SurfaceRole::LightStrip: return "prelight_strip";
    case SurfaceRole::RimMap: return "rim_map";
    case SurfaceRole::FeedbackMap: return "sweat_map";
    default: break;
  }
  if (o && o->klass == DrawClass::Post) return "post_other";
  return "";
}

struct Found {
  std::string role;
  uint32_t index = 0;        // n-th distinct image of the role, first-draw order
  uint32_t guest_image = 0;
  uint32_t draws = 0;
  std::string technique;
  std::string last_writer;   // F2: the role of the LAST draw into this image in the frame (a different role = the image shows that role's pass, e.g. a map blurred in place)
};

constexpr uint32_t kMaxPerRole = 4;

// One walk of the frame's ops (FrameT = LoadedFrame: ops, targets, draws, surface_roles, OriginOf, String): the colour target each Draw writes, its role, the images per role. Pure (no renderer).
template <class FrameT>
inline void CollectTargets(const FrameT& frame, std::vector<Found>& out, uint32_t& dropped) {
  uint32_t color = 0;
  std::vector<std::pair<uint32_t, const char*>> last;   // image -> role of its last writer (role strings are literals)
  dropped = 0;
  for (uint32_t i = 0; i < frame.ops.count; ++i) {
    const Op& op = frame.ops[i];
    if (op.kind == OpKind::SetTargets) {
      const auto& t = frame.targets[op.index];
      color = t.color_count > 0 ? uint32_t(t.color[0].image) : 0;
      continue;
    }
    if (op.kind != OpKind::Draw || !color) continue;
    const DrawOrigin* o = frame.OriginOf(op.index);
    const char* tech = o ? frame.String(o->technique) : "";
    uint8_t trole = 0;
    for (uint32_t r = 0; r < frame.surface_roles.count; ++r)
      if (frame.surface_roles[r].image == color) { trole = uint8_t(frame.surface_roles[r].role); break; }
    const char* role = RoleOfDraw(o, tech, trole);
    if (!*role) continue;
    {
      bool set = false;
      for (auto& l : last)
        if (l.first == color) { l.second = role; set = true; break; }
      if (!set) last.emplace_back(color, role);
    }
    Found* hit = nullptr;
    uint32_t seen = 0;
    for (Found& f : out)
      if (f.role == role) { ++seen; if (f.guest_image == color) hit = &f; }
    if (hit) { ++hit->draws; continue; }
    if (seen >= kMaxPerRole) { ++dropped; continue; }
    Found f;
    f.role = role, f.index = seen, f.guest_image = color, f.draws = 1, f.technique = tech;
    out.push_back(std::move(f));
  }
  for (Found& f : out)
    for (const auto& l : last)
      if (l.first == f.guest_image && f.last_writer.empty()) f.last_writer = l.second;
}

// File stems: game_<role>.<n>_f<frame> / ours_<role>.<n>_f<frame> (the stage_mirror.py pairing keys).
inline std::string FileStem(const char* side, const std::string& role, uint32_t index, uint64_t frame) {
  return std::string(side) + "_" + role + "." + std::to_string(index) + "_f" + std::to_string(frame);
}

// The raw dump both sides write (the L12 format, read by stage_page_check.py / stage_mirror.py): u32 width, u32 height (LE), then RGBA8 rows. False = a write failed.
inline FILE* OpenWrite(const std::filesystem::path& p) {   // a wide-character open on Windows (a path with a non-ASCII folder name)
#ifdef _WIN32
  return _wfopen(p.c_str(), L"wb");
#else
  return std::fopen(p.string().c_str(), "wb");
#endif
}
inline bool WriteText(const std::filesystem::path& p, const std::string& text) {
  FILE* f = OpenWrite(p);
  if (!f) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  std::fclose(f);
  return ok;
}
inline bool WriteRaw(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  FILE* f = OpenWrite(path);
  if (!f) return false;
  const uint32_t wh[2] = {w, h};
  const bool ok = std::fwrite(wh, sizeof(wh), 1, f) == 1 && std::fwrite(rgba.data(), 1, rgba.size(), f) == rgba.size();
  std::fclose(f);
  return ok;
}

}  // namespace rdr_adapter::stage_mirror

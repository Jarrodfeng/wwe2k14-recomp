// S103 P7 (landing 2): the blend tag of an OWN draw, for the scene host's decoupled 120 (the tween) without the command stream. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// rdr::TickBlender (renderer\frame_blend.h) mixes the vertex constants of a draw found in the previous tick and in this one. On the stream path the binder keys the draws (binder.h BlendRanges); the
// host has no binder, so each own drawer calls TagOwnBlend after it has set the draw's vs_constants: the ranges are the vertex shader's constants the shader build marked interpolable
// (fx::ConstantRange::blend: the model-view-projection, camera and light positions, the bone palette) MINUS the map lookups (review F4: PlaneInv, the shadow / rim / plane map matrices: the own
// plane / shadow / rim passes are not tagged, so a lookup matrix mixed here would slide against its map every other 120 Hz frame), the base key is what the draw IS (`what`: the object id's TEXT, asset,
// mesh ... made by the caller) with the vertex variant, and the top bit of the final key is set (rdr::kOwnBlendKeyBit: never a binder key). Nothing happens unless the owner switched tags on
// (FrameBuilder::SetBlendTags): the stream build never does.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "fx_tables.h"

namespace rdr_scene {

// TEST ONLY (the rig's planted twins, tools\scene\host_rig.py): 1 = the key takes the raw Object::id (review F1's bug), 2 = a card BAKE is tagged like a member (review F2's bug). Never set outside the rig.
inline std::atomic<int> g_own_blend_plant{0};
// What the own tags did (the rig prints it): the instanced draws tagged, the card-bake draws seen and how many of them were tagged (must be 0).
struct OwnBlendCensus { std::atomic<uint64_t> instanced_tagged{0}, bake_seen{0}, bake_tagged{0}; };
inline OwnBlendCensus g_own_blend_census;

// The 64-bit hash of an object's id TEXT (FNV-1a; never 0). Object::id is a byte offset into the record's string blob: it is NOT the same for the same object in two ticks (review F1).
inline uint64_t OwnNameHash(std::string_view s) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
  return h ? h : 1;
}

// A constant field the shader uses to LOOK UP a map (a shadow / rim / plane map matrix): never mixed between ticks.
inline bool OwnBlendIsMapLookup(const char* field) {
  if (!field) return false;
  static const char* const kParts[] = {"PlaneInv", "Shadow", "Rim", "TexProj", "ViewLight", "PlaneMat", "PlaneView", "PlaneProj"};
  for (const char* p : kParts)
    if (std::strstr(field, p)) return true;
  return false;
}

// The offsets (bytes, into the vertex block) of the map-lookup fields of a variant, by field name through the fxt table (built once for every variant).
inline const std::vector<uint16_t>& OwnBlendMapLookupOffsets(int variant_index) {
  static std::vector<std::vector<uint16_t>> by_variant;
  static std::once_flag once;
  std::call_once(once, [] {
    constexpr size_t n = sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]);
    by_variant.assign(n, {});
    for (size_t i = 0; i < n; ++i) {
      if (!rdr::fx::kVariants[i].name) continue;
      for (const fxt::VariantTable& t : fxt::kTables) {
        if (std::strcmp(t.variant, rdr::fx::kVariants[i].name) != 0) continue;
        for (uint32_t k = 0; k < t.field_count; ++k)
          if (OwnBlendIsMapLookup(t.fields[k].name)) by_variant[i].push_back(t.fields[k].offset);
        break;
      }
    }
  });
  static const std::vector<uint16_t> none;
  return variant_index >= 0 && size_t(variant_index) < by_variant.size() ? by_variant[size_t(variant_index)] : none;
}

// The ranges an own draw of this vertex variant blends (<= 16), for a block of `block_bytes` bytes. Pure: the unit / rig check calls it.
inline uint32_t OwnBlendRanges(const rdr::fx::Variant& vertex, int variant_index, uint32_t block_bytes, rdr::BlendRange* ranges /*[16]*/) {
  const std::vector<uint16_t>& lookups = OwnBlendMapLookupOffsets(variant_index);
  uint32_t count = 0;
  for (uint32_t r = 0; r < vertex.constant_count && count < 16; ++r) {
    const rdr::fx::ConstantRange& range = vertex.constants[r];
    const uint32_t bytes = uint32_t(range.count) * 16u;
    if (!range.blend || range.offset + bytes > block_bytes) continue;
    bool lookup = false;
    if (g_own_blend_plant.load(std::memory_order_relaxed) != 3)   // (plant 3: the lookups are kept, the review F4 bug)
      for (uint16_t o : lookups) lookup = lookup || (o >= range.offset && o < range.offset + bytes);
    if (!lookup) ranges[count++] = {range.offset, bytes};
  }
  return count;
}

// `what` = a 64-bit identity of the draw that is stable between ticks (never a pointer into a frame's own storage, never a raw record string offset). `block_bytes` = the size of the vertex constant
// block that was pushed (the drawer's own number, not a copy of pack()'s formula).
inline void TagOwnBlend(rdr::FrameBuilder& fb, rdr::DrawOp& op, const rdr::fx::Variant& vertex, int variant_index, uint32_t block_bytes, uint64_t what) {
  if (!fb.BlendTagsOn()) return;
  rdr::BlendRange ranges[16];
  const uint32_t count = OwnBlendRanges(vertex, variant_index, block_bytes, ranges);
  uint64_t base = (what * 0x9E3779B97F4A7C15ull) ^ (uint64_t(uint32_t(variant_index)) + 0xA5A5C3D2E1F0B987ull);
  base = (base ^ (base >> 29)) * 0x100000001b3ull;
  fb.TagBlend(op, ranges, count, base);
}

}  // namespace rdr_scene

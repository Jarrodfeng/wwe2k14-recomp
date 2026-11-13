// The crowd's far cards, drawn by us: the GPU half. See crowd_cards.h and docs\renderer\CROWD_CARDS.md.
#include "crowd_cards.h"

#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

#include "../renderer/shaders/spv/fx_registry.h"

namespace rdr_scene {
namespace {

constexpr uint16_t kRGB32F = 106, kRG16F = 83, kRGBA32F = 109;  // VkFormat numbers (R32G32B32_SFLOAT, R16G16_SFLOAT, R32G32B32A32_SFLOAT), as the adapter's layouts hold them
constexpr uint16_t kHalfOne = 0x3C00;

struct QuadVertex {  // 16 bytes: position, then the uv as two half floats
  float pos[3];
  uint16_t u, v;
};

int VariantIndex(const char* name) {
  for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i)
    if (rdr::fx::kVariants[i].name && std::strcmp(rdr::fx::kVariants[i].name, name) == 0) return int(i);
  return -1;
}

}  // namespace

const std::unordered_set<uint64_t>& HashesOfVariant(const char* variant_name) {
  static std::unordered_map<std::string, std::unordered_set<uint64_t>> cache;
  auto found = cache.find(variant_name);
  if (found != cache.end()) return found->second;
  std::unordered_set<uint64_t>& set = cache[variant_name];
  const int variant = VariantIndex(variant_name);
  if (variant >= 0)
    for (const rdr::fx::HashEntry& h : rdr::fx::kHashes)
      if (int(h.variant) == variant) set.insert(h.hash);
  return set;
}

CardsDrawer::CardsDrawer(rdr::Renderer& renderer, const Options& options) : renderer_(renderer), options_(options) {
  vs_ = VariantIndex("yAudienceCard_cards_vertex");
  vs_lm_vertex_ = VariantIndex("yAudienceCard_cards_lm_vertex_vertex");   // lane 219
  if (vs_ < 0) return;
  rdr::VertexLayoutDesc desc{};
  desc.strides[0] = sizeof(QuadVertex);
  desc.strides[1] = 48;
  desc.per_instance[1] = 1;
  desc.attributes[desc.attribute_count++] = {0, 0, kRGB32F, 0};   // inPosition
  desc.attributes[desc.attribute_count++] = {2, 0, kRG16F, 12};   // inUv
  desc.attributes[desc.attribute_count++] = {4, 1, kRGBA32F, 0};  // the world rows (the game's c76, c77, c78)
  desc.attributes[desc.attribute_count++] = {5, 1, kRGBA32F, 16};
  desc.attributes[desc.attribute_count++] = {6, 1, kRGBA32F, 32};
  layout_ = renderer_.AddVertexLayout(desc);
}

uint32_t CardsDrawer::Draw(rdr_adapter::CrowdInject& in, const cards::Plan& plan) {
  refusal_.clear();
  if (vs_ < 0 || layout_ == 0xFFFF || !in.builder || !in.program || in.ps < 0 || !in.images || plan.runs.empty() || plan.instances.empty()) return refusal_ = "drawer: not ready (no program, layout, images or runs)", 0;
  // Lane 219: ours follows the vertex stage the game's first card draw runs: lm_uv (the page read in the pixel stage, slot 13) or lm_vertex (page read in the vertex stage, once per corner, vertex slot 16). Any other stage is not ours.
  const char* stream_vs = in.vs >= 0 ? rdr::fx::kVariants[in.vs].name : nullptr;
  int vs = -1;
  if (stream_vs && std::strcmp(stream_vs, "yAudience_lm_uv_vertex") == 0) vs = vs_;
  else if (stream_vs && std::strcmp(stream_vs, "yAudience_lm_vertex_vertex") == 0) vs = vs_lm_vertex_;
  if (vs < 0) return ++stats_.other_program, refusal_ = std::string("drawer: other program ") + (stream_vs ? stream_vs : "(none)"), 0;
  const rdr::fx::Variant& variant = rdr::fx::kVariants[vs];
  if (!in.vs_block || in.vs_block_bytes != variant.block_bytes) return refusal_ = "drawer: block size", 0;  // the first card draw's constant block must be the cards stage's 128 bytes (lm_uv and lm_vertex share the layout)
  for (const cards::Run& run : plan.runs)
    if (run.picture >= in.image_count || in.images[run.picture] == rdr::kNoImage) return ++stats_.no_image, refusal_ = "drawer: no image", 0;  // a picture without a renderer image: the stream's cards stay
  rdr::FrameBuilder& builder = *in.builder;

  // The card quad (the measured blob, ours): the feet at y 0 and v 1, the head at y -250 and v 0.
  QuadVertex quad[4];
  cards::CardQuad(reinterpret_cast<uint8_t*>(quad));  // (the same bytes the core checks the stream's blob against)
  if (options_.uv_swapped)
    for (QuadVertex& q : quad) std::swap(q.u, q.v);
  const uint32_t quad_offset = builder.PushGeometry(quad, sizeof quad, 16);
  const uint32_t instance_offset = builder.PushGeometry(plan.instances.data(), uint32_t(plan.instances.size() * sizeof(cards::Instance)), 16);

  rdr::DrawState state = in.state;
  const bool alpha = in.alpha_test && !options_.no_alpha_test;
  const bool twin_built = alpha && rdr::fx::kVariants[in.ps].spirv_alpha != nullptr;  // review F4: the twin AS BUILT, not as asked
  state.program = in.program(vs, in.ps, alpha);
  state.vertex_layout = layout_;
  state.topology = 4;          // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: our quad is a strip, whatever the borrowed state says (review F2)
  state.primitive_restart = 0;  // (not indexed)
  rdr::DrawOp base{};
  base.state = builder.State(state);
  base.dynamic = builder.Dynamic(in.dynamic);
  base.vs_constants = builder.PushConstants(in.vs_block, in.vs_block_bytes);
  base.fs_constants = builder.PushConstants(in.fs_block, in.fs_block_bytes);
  base.index = {rdr::kNoBuffer, 0};
  base.count = 4;
  uint32_t drawn = 0;
  for (const cards::Run& run : plan.runs) {
    rdr::TextureSet set = in.textures;
    rdr::TextureSlot& slot = set.slots[0];
    slot.image = in.images[run.picture];
    if (options_.copy_swap_lost) {  // planted: the picture's red / blue exchange undone (or done twice): blue and red people
      uint8_t eff[4];
      for (int c = 0; c < 4; ++c) eff[c] = slot.swizzle[c] ? slot.swizzle[c] : uint8_t(3 + c);  // VkComponentSwizzle: 3 R, 4 G, 5 B, 6 A; 0 = identity
      std::swap(eff[0], eff[2]);
      std::memcpy(slot.swizzle, eff, sizeof eff);
    }
    rdr::DrawOp op = base;
    op.textures = builder.Textures(set);
    op.vertex[0] = {rdr::kFrameGeometry, quad_offset};
    op.vertex[1] = {rdr::kFrameGeometry, instance_offset + run.first * uint32_t(sizeof(cards::Instance))};
    op.instances = run.count;
    builder.Draw(op);
    ++stats_.draws;
    stats_.alpha_wanted += in.alpha_test;
    stats_.alpha_twin += twin_built;
    drawn += run.count;
  }
  return drawn;
}

}  // namespace rdr_scene

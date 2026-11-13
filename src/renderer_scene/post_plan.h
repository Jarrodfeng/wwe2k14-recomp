// Session 101, lane PS: the post plan (docs\renderer\POST_PLAN.md, POST_PROMOTION_MAP.md section 7 lane PS).
// GPU-free. A frame's post draws (read from a capture or the live frame through the stream reader) -> a PostPlan: the shape of the game's chain
// (layers, the bloom source, the dead work, film, the per-frame tables and filters) in the terms PostPass takes (PostParams).
//
// Frames are recognised by STRUCTURE, never by hash: the one combine (a yGlareEtc fragment), then a walk back through the known variant sequences
//   [restore]* ( dof hole_fill ) [ restore* ( dof hole_fill ) ] [ box4_down t2 t1_t2 t1_t2 t1x8 m1_m2 ] combine [ monotone ]
// Which program a draw is comes from the effect registry (the draw's shader hash -> the variant name, as the binder does); the plan never reads
// Draw::vs_patched_hash (the old kPostChainLastVs): the chain's end is the structural last chain draw.
// What the plan decides (all measured by lane PC, POST_PROMOTION_MAP.md section 9):
//   layers     one per dof + hole_fill pair (Bryan's entrance has two). The combine's slot 2 is the LAST layer's blur, slot 6 the first layer's
//              (the game's "no halo": the background blur never holds the wrestlers). The earlier layers stay the stream's: the pass takes
//              their image as PostParams::far_blur.
//   bloom      the source is the DoF chain even when the combine has no dof permutation (editor, cards): PostParams::dof_chain.
//   dead work  c31.x == 0 -> no bloom blur, c32.x == 0 -> no glare (Bryan, brock_tron, cards: 13 draws + 14 copies a frame); both -> no bright pass.
//   filter     the dof depth sampler is POINT where the frame has a restore (copy_depth), LINEAR where it has none (8 of 8 valid captures).
//   film       a monotone draw after the combine.
//   tables     c20 / c21 / c27 / c29 / c30 / c31 / c32, the dof / hole_fill / bright / bloom / glare tap tables, the film bank: read from the
//              stream's own draws by register (the registers the pass writes, post_pass.cpp Record).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "post_pass.h"

namespace rdr_adapter {
struct LoadedFrame;
class CaptureReader;
}  // namespace rdr_adapter

namespace rdr_scene {

enum class ChainKind : uint8_t { CopyDepth, Dof, HoleFill, Box4Down, T2, T12, T1, M1M2, Combine, Monotone, Retro, Count };
const char* ChainKindName(ChainKind k);  // "copy_depth", "dof", "hole_fill", "box4_down", "t2", "t1_t2", "t1", "m1_m2", "combine", "monotone", "retro"

struct ChainSlot {
  uint8_t slot = 0;
  uint32_t image = 0;     // capture image id (0: a fetch of nothing)
  uint8_t filter = 0;     // 0 point, 1 linear, 2 mixed
  uint8_t addr_u = 2, addr_v = 2;   // S101 lane PO: the stream sampler's address modes (VkSamplerAddressMode: 0 repeat, 2 clamp to edge)
  int32_t writer = -2;    // the chain draw that last wrote this image (copies followed); -1 another draw; -2 not written in this frame
};

struct ChainDraw {
  ChainKind kind = ChainKind::Count;
  uint32_t draw = 0, op = 0;       // index into the frame's draws / ops
  uint32_t target = 0;             // the colour target (the depth when there is none)
  uint32_t target_w = 0, target_h = 0;
  uint32_t site = 0;               // call site (0 when the frame carries no origins)
  int ps_variant = -1;             // rdr::fx::kVariants index
  uint64_t vs_patched_hash = 0;    // ONLY for the planted fault last_pass_by_hash: the plan never reads it otherwise
  std::vector<ChainSlot> slots;
  float regs[256][4] = {};         // the pixel constant bank (copied: the frame may go away)
  const ChainSlot* Slot(int s) const {
    for (const ChainSlot& c : slots) if (c.slot == s) return &c;
    return nullptr;
  }
};

struct ChainCopy { uint32_t op = 0, src = 0, dst = 0; };

// The post draws of one frame in op order, the frame's copies, and the facts around them.
struct StreamChain {
  std::vector<ChainDraw> draws;
  std::vector<ChainCopy> copies;      // every copy of the frame
  bool named = false;                 // the frame carries draw origins
  uint32_t later_draws = 0;           // non-chain draws after the combine (the HUD etc.: they stay the stream's)
  uint32_t draws_in_frame = 0;
};

// Reads the frame's post draws. False + error when the frame cannot be read (a missing table); a frame with no post draws is true with draws empty.
bool ReadStreamChain(const rdr_adapter::CaptureReader& cap, const rdr_adapter::LoadedFrame& frame, StreamChain& out, std::string& error);
// S101 lane PO: the same without a CaptureReader (the live path): `size` answers a capture image id's size in the game's pixels (false: unknown, the sizes stay 0).
using ChainImageSize = std::function<bool(uint32_t image, uint32_t& w, uint32_t& h)>;
bool ReadStreamChain(const ChainImageSize& size, const rdr_adapter::LoadedFrame& frame, StreamChain& out, std::string& error);

enum class PostShape : uint8_t { None, Match, Film, Entrance, TwoLayer, Editor, Cards, Other };
const char* PostShapeName(PostShape s);  // "none", "match", "film", "entrance", "two_layer", "editor", "cards", "other"

struct LayerTables {
  float dof_offsets[4][4] = {};
  float dof_param2_down[4] = {};
  float hole_offsets[12][4] = {};
  float hole_weights[13][4] = {};
};

struct PostLayer {
  uint32_t dof = 0, hole_fill = 0;       // indices into StreamChain::draws
  uint32_t restores_before = 0;          // copy_depth draws directly before this layer's dof
  uint32_t colour_image = 0, depth_image = 0;
  uint8_t stream_filter = 0;             // the depth slot's filter in the stream (0 point, 1 linear, 2 mixed)
  bool depth_linear = false;             // the plan's filter (the rule: linear where the frame has no restore)
  bool owned = false;                    // the last layer: recorded by our pass. Earlier layers stay the stream's
  uint32_t blur_target = 0;              // the hole_fill's target
  LayerTables tables;
};

struct PostPlan {
  bool ok = false;
  std::string refusal;                   // why not (a shape that is not known, a combine permutation that is not ours yet ...)
  PostShape shape = PostShape::None;
  std::string sequence;                  // the chain's variant sequence, run-length form ("copy_depth dof hole_fill t1x8 ... combine")
  std::vector<PostLayer> layers;         // chronological
  uint32_t restores = 0;                 // copy_depth draws in the chain
  // the combine
  uint32_t combine = 0;                  // index into StreamChain::draws
  std::string combine_variant;           // "dof_fog_bloom" (the registry name without yGlareEtc_ and _fragment)
  bool dof = false, fog = false, bloom_variant = false;
  uint32_t combine_target = 0;           // the image the stream's combine writes (ours writes the same one)
  uint32_t slot_image[9] = {};           // the combine's texture slots (0 where not bound)
  std::string slots;                     // "s0 s2 s3 s6 s7 s8"
  int slot2_layer = -1, slot6_layer = -1;  // which layer's blur the slot holds (index into layers; -1 none)
  bool far_blur_separate = false;        // slot 6 holds an earlier layer's blur
  uint32_t far_blur_image = 0;           // capture image id of that blur
  // bloom / glare
  bool hdr_drawn = false;                // the bright pass + bloom + glare chain is in the stream
  bool bloom_source_dof = false;         // the bloom chain reads the DoF chain although the combine has no dof permutation
  bool bloom_zero = false, glare_zero = false;  // c31.x == 0, c32.x == 0
  uint32_t bloom_side_draws = 0, glare_side_draws = 0, bloom_side_copies = 0, glare_side_copies = 0;  // lane PC's census definitions (box4_down, t2, t1_t2 / t1, m1_m2)
  uint32_t skipped_draws = 0, skipped_copies = 0;  // what the plan does not record (dead work)
  bool skip_dof_chain = false;           // no dof, no bloom: the dof + hole_fill passes are dead
  std::vector<uint32_t> skipped_draw_list;  // indices into StreamChain::draws
  // film
  bool film = false;
  // lane 140: "t2 combine [monotone]": the depth-of-field blur is one yBlur_t2 pass over an image the stream made (no layers); the combine's slots 2 and 6 hold its output
  bool t2_blur = false;
  // lane 224: "combine [monotone]": the chain is the combine alone (a fog-only permutation: no dof layer, no HDR chain, no bloom permutation; the CAW editor, victory fades). The claimed range is the combine.
  bool combine_only = false;
  // lane 238: the claimed range starts here (index into StreamChain::draws) when there is no dof layer: the t2 blur's draw, or the combine of a combine-only chain. The restores (copy_depth) in front of it stay the stream's.
  uint32_t claim_first = 0;
  uint32_t t2_index = 0;                 // lane 238: plan.t2_blur: the blur's yBlur_t2 draw (a second t2 in the frame, after the bright pass, is the bloom's)
  uint32_t blur_source_image = 0;        // capture image id the t2 draw reads (slot 0)
  // lane 296: the two-layer t2 form 'copy_depth t2 copy_depthx2 t2 box4_down ...' (the 30 Years cutscene, runs 7183 / 7201): the EARLIER t2 (index into StreamChain::draws, -1 none) is the far layer's blur.
  // It stays the stream's (like Bryan's layer 1): the combine's slot 6 reads its output (far_blur_separate), slot 2 the t2_index one.
  int32_t t2_far = -1;
  std::string t2_far_note;               // lane 296b: the first-accepted-frame log line's text (slot images, writers, target sizes, far copy op vs claim_first)
  // where the chain ends / what the plan reads
  uint32_t chain_first_draw = 0, chain_last_draw = 0;  // frame draw indices: the owned region's first draw (the last layer's dof) / the chain's last draw
  uint32_t chain_last = 0;               // index into StreamChain::draws
  bool chain_end_found = false;
  uint32_t null_inputs = 0;              // chain fetch slots reading image 0 (the S100 null_input family: the plan is made, the caller should not trust the picture)
  uint32_t later_draws = 0;
  PostParams params;                     // the per-frame tables in PostPass's terms
};

// Planted faults for the offline tests (tools\post\post_plan_test.cpp): each must fail its named check and nothing else.
// lane 140 plants: OldRestoreRule (depth_linear = restores == 0, the rule before the pass followed the stream's sampler), T2SourceStale (the t2's source taken from the combine's slot 2: the dropped copy's destination),
// T2SlotsForeign (the writer check of slots 2 / 6 is skipped and slot 6 is fed another image), T2DefaultTables
enum class PlanPlant : uint8_t { None, LayerMerged, DeadBloomKept, DofFilterFixed, LastPassByHash, OldRestoreRule, T2SourceStale, T2SlotsForeign, T2NeedsDof, T2OneOnly, FarCopyUnchecked };
inline PlanPlant g_plan_plant = PlanPlant::None;

PostPlan BuildPostPlan(const StreamChain& chain);

}  // namespace rdr_scene

// Session 101, lane PO: our own post chain in the game's frame. See post_ours.h.
#include "post_ours.h"

#include <cstdio>
#include <cstring>

#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"

namespace rdr_scene {
namespace {

bool HasVariant(const char* name) {
  for (const auto& v : rdr::fx::kVariants)
    if (v.name && std::strcmp(v.name, name) == 0) return true;
  return false;
}

// The variants PostPass::Record asks for, found before any pass is recorded (a missing one would leave a half chain in the frame).
bool AllVariantsPresent(const PostParams& plan) {
  static const char* const fixed[] = {"yAfterImage_quad_vertex", "yGlareEtc_corner_uv_vertex", "yDownScale_dof_fragment", "yBlur_hole_fill_fragment", "yDownScale_box4_down_fragment",
                                      "yBlur_t2_fragment", "yBlur_t1_t2_fragment", "yBlur_t1_fragment", "yBlur_m1_m2_fragment", "yMonotone_monotone_fragment"};
  for (const char* n : fixed)
    if (!HasVariant(n)) return false;
  return HasVariant(PostPass::CombineVariantName(plan).c_str());   // (the one function the pass itself names the combine with)
}

}  // namespace

PostOurs::PostOurs(rdr::Renderer& renderer, const PostOursOptions& options) : renderer_(renderer), options_(options), pass_(renderer) {}

PostOurs::~PostOurs() {
  if (null_ != rdr::kNoImage) renderer_.DestroyImage(null_);
  if (fixed_ != rdr::kNoImage) renderer_.DestroyImage(fixed_);
}

rdr::ImageId PostOurs::NullImage() {
  if (null_ == rdr::kNoImage) {
    rdr::ImageDesc d;
    d.width = d.height = 1;
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageSampled;
    null_ = renderer_.CreateImage(d);
    const uint8_t zero[4] = {};
    if (null_ != rdr::kNoImage) renderer_.UpdateImage(null_, 0, 0, zero, 4);
  }
  return null_;
}

// The planted fixed_texel picture: the scene colour at the game's own 1280x720 whatever the render scale (the combine's texel then comes out as 1/1280 x 1/720).
rdr::ImageId PostOurs::FixedImage(uint32_t w, uint32_t h) {
  if (fixed_ == rdr::kNoImage || fixed_w_ != w || fixed_h_ != h) {
    if (fixed_ != rdr::kNoImage) renderer_.DestroyImage(fixed_);
    rdr::ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h);
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    fixed_ = renderer_.CreateImage(d);
    fixed_w_ = w, fixed_h_ = h;
  }
  return fixed_;
}

bool PostOurs::PlanFrame(const rdr_adapter::PostPlanIn& in, rdr_adapter::PostClaim& claim) {
  claim = rdr_adapter::PostClaim{};
  decision_ = DecidePost(in, options_.faults, options_.limits);
  const PostDecision& d = decision_;
  if (d.no_chain) {
    claim.no_chain = true;
    return false;
  }
  if (d.ok && !AllVariantsPresent(d.params)) {
    decision_.ok = false, decision_.failed = true, decision_.keep = PostKeep::NotReady, decision_.detail = "a post variant is missing from the registry";
  }
  if (!decision_.ok) {
    ++tally_.kept[uint8_t(decision_.keep)];
    // lane 140: one line per (reason, specific check, chain), so a second reason or a second check on a known chain is named too (Check J: 'slot' hid behind 'filter'); the digits of the detail are left out of the key
    std::string stem;
    for (char ch : decision_.detail.substr(0, 60)) if (ch < '0' || ch > '9') stem += ch;
    const std::string seen_key = "[" + std::string(PostKeepName(decision_.keep)) + ":" + stem + ":" + decision_.sequence + "]";
    if (refused_seen_.find(seen_key) == std::string::npos && refused_named_ >= 64) {   // a hard cap: after 64 named refusals one line says the rest are not named
      if (!refused_capped_) refused_capped_ = true, rdr::Log("[rdr][post] further refusals are not named (64 distinct reasons logged); the census counts them");
    } else if (refused_seen_.find(seen_key) == std::string::npos) {   // once per distinct chain: the census tool's input for a new shape
      refused_seen_ += seen_key;
      ++refused_named_;
      rdr::Log("[rdr][post] chain stays the stream's (%s): %s; sequence: %s", PostKeepName(decision_.keep), decision_.detail.c_str(), decision_.sequence.c_str());
    }
    return false;
  }
  if (decision_.failed) ++tally_.planted_unchecked;   // a check failed but the planted run's checks are off
  claim = decision_.claim;
  if (d.plan.t2_far >= 0) {   // lane 296b (SF2): counted apart from lane 277's film frames; one line on the first
    if (!said_t2_far_) said_t2_far_ = true, rdr::Log("[rdr][post] two-layer t2 frame accepted: %s", d.plan.t2_far_note.c_str());
  }
  return true;
}

uint32_t PostOurs::Inject(rdr_adapter::PostInject& in) {
  const PostDecision& d = decision_;
  const auto map = [&in](uint32_t id) { return id && id < in.image_count ? in.images[id] : rdr::kNoImage; };
  rdr::ImageId colour = map(d.colour), depth = d.null_depth ? NullImage() : map(d.depth);
  const rdr::ImageId out = map(d.out);
  PostParams p = d.params;
  p.far_blur = d.far_blur ? map(d.far_blur) : rdr::kNoImage;
  if (d.t2_default_tables) {   // lane 140 planted t2_default_tables
    const PostParams def = DefaultPostParams(1280, 720, true);
    std::memcpy(p.bloom_t2_offsets, def.bloom_t2_offsets, sizeof p.bloom_t2_offsets);
    std::memcpy(p.bloom_t2_weights, def.bloom_t2_weights, sizeof p.bloom_t2_weights);
    std::memcpy(p.blur_t2_offsets, def.bloom_t2_offsets, sizeof p.blur_t2_offsets);   // lane 238: the blur has its own tables
    std::memcpy(p.blur_t2_weights, def.bloom_t2_weights, sizeof p.blur_t2_weights);
  }
  if (d.blur_src) p.blur_source = map(d.blur_src);   // lane 140: the t2 blur's source (the stream's own picture)
  if (p.t2_blur && p.blur_source == rdr::kNoImage) {
    ++tally_.kept[uint8_t(PostKeep::NotReady)];
    return 0;
  }
  const bool needs_depth = p.dof || p.fog || p.dof_chain;   // lane 238: the base-permutation combine (CAW editor) reads no depth
  if (colour == rdr::kNoImage || (needs_depth && depth == rdr::kNoImage) || out == rdr::kNoImage || (d.far_blur && p.far_blur == rdr::kNoImage)) {
    ++tally_.kept[uint8_t(PostKeep::NotReady)];
    return 0;
  }
  rdr::FrameBuilder& b = *in.builder;
  uint32_t passes = 0;
  {
    if (d.fixed_texel_copy) {  // planted (lane 34 review): the combine's picture at the game's size, so its texel is 1/1280 x 1/720
      const rdr::ImageId fixed = FixedImage(p.width, p.height);
      if (fixed == rdr::kNoImage) {
        ++tally_.kept[uint8_t(PostKeep::NotReady)];
        return 0;
      }
      rdr::CopyOp c{};
      c.source.image = colour, c.dest.image = fixed;
      c.source_rect = {0, 0, p.width, p.height};
      c.dest_rect = c.source_rect;
      c.filter = rdr::CopyFilter::Linear;
      b.Copy(c);
      colour = fixed;
    }
    std::string error;
    if (!pass_.Record(b, colour, depth, out, p, error)) {
      if (!said_record_error_) said_record_error_ = true, rdr::Log("[rdr][post] the pass could not record: %s", error.c_str());
      ++tally_.kept[uint8_t(PostKeep::NotReady)];
      return 0;
    }
    passes = pass_.passes_recorded();
    // The check behind the planted no_combine (S100 07:25, "the washed-out picture"): the recorded chain holds the combine. Passes are already in the builder here, so a failure is a LATE one:
    // the binder puts the stream's last target switch back and draws the frame's chain the stream's way.
    if (!pass_.combined()) {
      ++tally_.kept[uint8_t(PostKeep::Combine)];
      if (!options_.faults.checks_off) return 0;
    }
    if (d.inject_fail_late && !options_.faults.checks_off) {   // planted: one pass is recorded, then the injection fails
      ++tally_.kept[uint8_t(PostKeep::NotReady)];
      return 0;
    }
  }
  ++tally_.ours;
  if (d.plan.t2_far >= 0) ++tally_.t2_far;   // lane 296b
  if (d.plan.combine_only) ++tally_.combine_only;
  ++tally_.shapes[uint8_t(d.shape)];
  tally_.passes += passes;
  tally_.dead_draws += d.dead_draws, tally_.dead_copies += d.dead_copies;
  tally_.bloom0 += d.bloom0, tally_.glare0 += d.glare0, tally_.dof0 += d.dof0;
  return passes;
}

std::string PostOurs::Census(uint64_t dropped_draws, uint64_t dropped_copies, uint64_t dropped_clears, uint64_t dropped_targets) const {
  const Tally& t = tally_;
  uint64_t kept = 0;
  for (uint64_t k : t.kept) kept += k;
  char line[1536];
  std::snprintf(line, sizeof line,
                "[rdr][post] ours frames %llu, fallback %llu, shapes {match %llu, entrance %llu, two_layer %llu, film %llu, editor %llu, cards %llu, other %llu}, dropped draws %llu copies %llu clears %llu targets %llu, "
                "dead work skipped %llu draws %llu copies (bloom %llu, glare %llu, dof %llu), kept %llu (shape %llu, permutation %llu, slot %llu, null_input %llu, chain_end %llu, filter %llu, "
                "unscaled %llu, texel %llu, interleaved %llu, reads_chain %llu, not ready %llu, combine %llu, depth_clear %llu, writes_inputs %llu), passes %llu, t2_far %llu",
                (unsigned long long)t.ours, (unsigned long long)kept, (unsigned long long)t.shapes[uint8_t(PostShape::Match)], (unsigned long long)t.shapes[uint8_t(PostShape::Entrance)],
                (unsigned long long)t.shapes[uint8_t(PostShape::TwoLayer)], (unsigned long long)t.shapes[uint8_t(PostShape::Film)], (unsigned long long)t.shapes[uint8_t(PostShape::Editor)],
                (unsigned long long)t.shapes[uint8_t(PostShape::Cards)], (unsigned long long)t.shapes[uint8_t(PostShape::Other)], (unsigned long long)dropped_draws, (unsigned long long)dropped_copies,
                (unsigned long long)dropped_clears, (unsigned long long)dropped_targets, (unsigned long long)t.dead_draws, (unsigned long long)t.dead_copies, (unsigned long long)t.bloom0, (unsigned long long)t.glare0,
                (unsigned long long)t.dof0, (unsigned long long)kept, (unsigned long long)t.kept[uint8_t(PostKeep::Shape)], (unsigned long long)t.kept[uint8_t(PostKeep::Permutation)],
                (unsigned long long)t.kept[uint8_t(PostKeep::Slot)], (unsigned long long)t.kept[uint8_t(PostKeep::NullInput)], (unsigned long long)t.kept[uint8_t(PostKeep::ChainEnd)],
                (unsigned long long)t.kept[uint8_t(PostKeep::Filter)], (unsigned long long)t.kept[uint8_t(PostKeep::Unscaled)], (unsigned long long)t.kept[uint8_t(PostKeep::Texel)],
                (unsigned long long)t.kept[uint8_t(PostKeep::Interleaved)], (unsigned long long)t.kept[uint8_t(PostKeep::ReadsChain)], (unsigned long long)t.kept[uint8_t(PostKeep::NotReady)],
                (unsigned long long)t.kept[uint8_t(PostKeep::Combine)], (unsigned long long)t.kept[uint8_t(PostKeep::DepthClear)], (unsigned long long)t.kept[uint8_t(PostKeep::WritesInputs)], (unsigned long long)t.passes, (unsigned long long)t.t2_far);
  std::string text = line;
  if (t.planted_unchecked) text += ", planted fault ran unchecked on " + std::to_string((unsigned long long)t.planted_unchecked) + " frames";
  if (no_chain_frames_) text += " (frames with no post chain: " + std::to_string((unsigned long long)no_chain_frames_) + ")";
  if (t.combine_only) text += " (combine alone: " + std::to_string((unsigned long long)t.combine_only) + ")";   // lane 224
  if (persist_only_frames_) text += " (persist-only binds, not planned: " + std::to_string((unsigned long long)persist_only_frames_) + ")";   // S101 lane 224
  return text;
}

}  // namespace rdr_scene

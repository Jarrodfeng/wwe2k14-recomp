// See plane_shadow_ours.h. Session 101, lane 139 (SH3 slice 2).
#include "plane_shadow_ours.h"

#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/shaders/spv/rdr_plane_blur_frag.h"   // lane 159: our programs
#include "../renderer/shaders/spv/rdr_plane_blur_vert.h"
#include "../renderer/shaders/spv/rdr_plane_mask_frag.h"
#include "../renderer/shaders/spv/rdr_plane_mask_vert.h"

namespace rdr_scene {

using rdr_adapter::KeepWhy;

PlaneShadowOurs::PlaneShadowOurs(rdr::Renderer& renderer, const plane::Faults& faults) : faults_(faults), board_(faults.duplicate_record) {
  rdr::ProgramDesc mask;
  mask.name = "rdr_plane_mask";
  mask.vertex_spirv = kSpv_rdr_plane_mask_vert, mask.vertex_bytes = sizeof(kSpv_rdr_plane_mask_vert);
  mask.fragment_spirv = kSpv_rdr_plane_mask_frag, mask.fragment_bytes = sizeof(kSpv_rdr_plane_mask_frag);
  if (!renderer.AddProgramChecked(mask, mask_program_)) mask_program_ = 0;
  rdr::ProgramDesc blur;
  blur.name = "rdr_plane_blur";
  blur.vertex_spirv = kSpv_rdr_plane_blur_vert, blur.vertex_bytes = sizeof(kSpv_rdr_plane_blur_vert);
  blur.fragment_spirv = kSpv_rdr_plane_blur_frag, blur.fragment_bytes = sizeof(kSpv_rdr_plane_blur_frag);
  if (!renderer.AddProgramChecked(blur, blur_program_)) blur_program_ = 0;
}

void PlaneShadowOurs::SetRecord(const std::shared_ptr<const SceneTables>& record) {
  if (!record || (record->plane_draws.empty() && record->plane_frames.empty())) return;
  board_.Add(record->plane_frames, record->plane_draws);
}

bool PlaneShadowOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  ++stats_.frames;
  stream_stamp_ = uint32_t(frame.header.guest_frame);
  if (faults_.late_record && stats_.frames > 30) stream_stamp_ += 6;   // R late_record: from frame 30 the stream is seen 6 frames ahead of the record
  blur_ordinal_ = 0;
  mask_nth_.clear();
  if (!board_.stamps()) {
    ++stats_.frames_not_ready;
    return false;
  }
  return true;
}

bool PlaneShadowOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++stats_.claimed;
  const bool blur = in.origin && in.origin->call_site == plane::kBlurCallSite && in.origin->pass_site != plane::kMaskSite;
  ++(blur ? stats_.claimed_blur : stats_.claimed_mask);
  const uint32_t object = in.origin ? in.origin->object : 0, part = in.origin ? in.origin->part : 0;
  uint32_t nth = blur ? blur_ordinal_++ : mask_nth_[(uint64_t(object) << 32) | part]++;
  if (faults_.nth_shift) ++nth;   // R nth_shift: the record is read one entry off
  // The stream draws we know (all three seen in every capture): the mask is the skinned proxy program with the pass-through pixel program, the blur one of its two passes with the shadow-blur pixel program.
  const bool vs_ok = in.vs >= 0 && size_t(in.vs) < std::size(rdr::fx::kVariants);
  const bool ps_ok = in.ps >= 0 && size_t(in.ps) < std::size(rdr::fx::kVariants);
  const char* vname = vs_ok ? rdr::fx::kVariants[in.vs].name : "";
  const char* pname = ps_ok ? rdr::fx::kVariants[in.ps].name : "";
  const bool blur_x = blur && std::strcmp(vname, "yBlur_plane_x_vertex") == 0;
  const bool variant_ok = blur ? ((blur_x || std::strcmp(vname, "yBlur_plane_z_vertex") == 0) && std::strcmp(pname, "yBlur_shadow_blur_fragment") == 0)
                               : (std::strcmp(vname, "yPlaneShadowMap_skinned_vertex") == 0 && std::strcmp(pname, "yDebugPrimitive_base_fragment") == 0);
  if (!variant_ok) {
    ++stats_.kept_variant;
    why = KeepWhy::Variant;
    return false;
  }
  // Our programs have no alpha-test twin: a stream draw that asks for the test stays the stream's (counted; none of the plane chain's draws ask).
  if (in.frame && in.draw && in.frame->fixeds[in.draw->fixed].alpha_test) {
    ++stats_.kept_state;
    why = KeepWhy::Mismatch;
    return false;
  }
  const rdr::fx::Variant& v = rdr::fx::kVariants[in.vs];
  // The stream draw's row of a register, or null when the program has no such register.
  const auto row_of = [&](uint32_t reg) -> const float* {
    for (uint32_t i = 0; i < v.constant_count; ++i) {
      const rdr::fx::ConstantRange& c = v.constants[i];
      if (reg >= c.reg && reg < uint32_t(c.reg) + c.count) {
        const uint32_t at = c.offset + 16u * (reg - c.reg);
        return (in.vs_block && at + 16u <= in.vs_block_bytes) ? reinterpret_cast<const float*>(in.vs_block + at) : nullptr;
      }
    }
    return nullptr;
  };

  // ---- the join: by the stamp (+ object, part, nth). The offset is learned from the first entry that verifies.
  const PlaneDraw* held = nullptr;
  int used_delta = 0;
  plane::RegTable table;
  uint32_t compared = 0;
  bool any_candidate = false;
  // lane 205 (log first): the first candidate that was there and did not verify, for the window line: which stamp, the nth, which row and lane, both sides' words, and the entry's `have` bits.
  plane::MatchDiff diff;
  uint32_t diff_stamp = 0, diff_have = 0;
  bool incomplete_seen = false;
  const auto try_stamp = [&](uint32_t stamp, int d) {
    const PlaneDraw* h = board_.Find(stamp, blur ? kPlaneBlur : kPlaneMask, object, part, nth);
    if (!h) return false;
    any_candidate = true;
    if (blur) plane::BuildBlur(*h, table, faults_);
    else plane::BuildMask(board_.FrameAt(stamp), *h, faults_, board_.CameraBefore(stamp), table);
    uint32_t n = 0;
    bool owned = false;
    for (uint32_t r = 0; r < 64; ++r) owned = owned || table.have[r];
    if (!owned) {   // an entry with nothing to prove (a blur entry whose constants were not all uploaded this frame): counted as `incomplete`, not as a constants mismatch
      incomplete_seen = true;
      if (!diff.set) diff_stamp = stamp, diff_have = h->have;
      return false;
    }
    if (!faults_.no_verify) {
      plane::MatchDiff local;
      if (!plane::Matches(table, row_of, &n, &local) || n == 0) {
        if (!diff.set && local.set) diff = local, diff_stamp = stamp, diff_have = h->have;
        return false;
      }
    }
    compared = n;
    held = h, used_delta = d;
    return true;
  };
  if (delta_known_) try_stamp(stream_stamp_ - uint32_t(delta_), delta_);
  else
    for (int d = plane::kDeltaFirst; d <= plane::kDeltaLast && !held; ++d) try_stamp(stream_stamp_ - uint32_t(d), d);
  if (!held) {
    // Why. A real mismatch (an entry was there and it was not the stream's); LATE (no entry for this frame and the newest stamp held is older than the one wanted: the record is behind the stream);
    // or unjoined (nothing near this frame). A known offset that misses counts toward the relearn in all three cases: a late record is an offset that moved (the circles' F3, lane 136).
    uint32_t newest = 0;
    const uint32_t wanted = stream_stamp_ - uint32_t(delta_known_ ? delta_ : plane::kDeltaLast);
    const bool older_only = board_.newest_stamp(newest) && int32_t(newest - wanted) < 0;
    if (any_candidate && incomplete_seen && !diff.set) {
      ++stats_.kept_mismatch;
      ++stats_.kept_incomplete;
      why = KeepWhy::Mismatch;
      if (blur && first_refused_.empty()) first_refused_ = FormatRefusal(diff, diff_stamp, nth, diff_have, true);
    } else if (any_candidate) {
      ++stats_.kept_mismatch;
      ++stats_.kept_rows;
      why = KeepWhy::Mismatch;
      if (blur && first_refused_.empty()) first_refused_ = FormatRefusal(diff, diff_stamp, nth, diff_have, false);
    } else if (older_only) {
      ++stats_.kept_mismatch;
      ++stats_.kept_late;
      why = KeepWhy::Mismatch;
    } else {
      ++stats_.kept_unjoined;
      why = KeepWhy::Unjoined;
    }
    if (delta_known_ && !locked_ && ++misses_ >= plane::kRelearn) delta_known_ = false, misses_ = 0, ++stats_.relearned;
    return false;
  }
  if (!delta_known_) {
    delta_known_ = true, delta_ = faults_.stamp_lock ? std::min(used_delta + 1, plane::kDeltaLast) : used_delta;   // R stamp_lock: learned once, one frame too far, never searched again
    if (faults_.stamp_lock) locked_ = true;
  }
  misses_ = 0;

  // ---- our block: the record's rows where it owns them (proven equal to the stream's above), the stream's own copy of what it does not own (EyePos, the palette, the bone switches, c34)
  const auto stream_row = [&](uint32_t reg, float* dst) {   // the stream draw's row, zero when the program has none
    if (const float* r = row_of(reg)) std::memcpy(dst, r, 16);
    else std::memset(dst, 0, 16);
  };
  const auto pick = [&](uint32_t reg, float* dst) {   // the record's row when it has one, else the stream's
    if (table.have[reg]) std::memcpy(dst, table.v + 4 * reg, 16);
    else stream_row(reg, dst);
  };
  out = rdr_adapter::ReplaceOut{};
  out.own.set = true;
  out.own.stream_geometry = true;   // the stream's vertex / index streams, layout, topology and counts stay
  out.own.topology = 3;
  if (blur) {
    // c34 is the blur's coverage threshold, a PIXEL constant the record does not own: the stream's, read from its pixel block.
    const rdr::fx::Variant* pv = &rdr::fx::kVariants[in.ps];
    const float* threshold = nullptr;
    for (uint32_t i = 0; i < pv->constant_count && !threshold; ++i) {
      const rdr::fx::ConstantRange& c = pv->constants[i];
      if (34 >= c.reg && 34 < uint32_t(c.reg) + c.count && in.fs_block && c.offset + 16u * (34u - c.reg) + 16u <= in.fs_block_bytes)
        threshold = reinterpret_cast<const float*>(in.fs_block + c.offset + 16u * (34u - c.reg));
    }
    if (!threshold) {
      ++stats_.kept_inputs;
      why = KeepWhy::Mismatch;
      return false;
    }
    pick(33, blur_vs_);
    blur_vs_[1] = blur_vs_[2] = blur_vs_[3] = 0.0f;   // lane 205: the game writes only x (stack leftovers in y z w, listing 0x827ECDA8-0x827ECDAC); our shader reads BlurStep.x only, and nothing may depend on a leftover
    for (uint32_t r = 0; r < 4; ++r) pick(36 + r, blur_vs_ + 4 + 4 * r);
    pick(40, blur_vs_ + 20);
    blur_vs_[24] = blur_x ? 1.0f : 0.0f, blur_vs_[25] = 0.0f, blur_vs_[26] = blur_x ? 0.0f : 1.0f, blur_vs_[27] = blur_x ? -1.0f : 1.0f;
    std::memcpy(fs_, threshold, 16);
    out.own.program = blur_program_;
    out.own.vs_constants = reinterpret_cast<const uint8_t*>(blur_vs_), out.own.vs_bytes = sizeof(blur_vs_);
    out.own.blend[0] = {16, 64}, out.own.blend_count = 1;   // S2: PlaneMatrix follows the camera between ticks
  } else {
    // EyePos c72 and the bone palette c76.. are the stream's (no uploader / producer read: plane_sites.h); so are the bone-count switches.
    const float* eye = row_of(72);
    const float* palette = row_of(76);
    if (!eye || !palette || v.bools_offset == 0xFFFF || v.bools_offset + 32u > in.vs_block_bytes || !row_of(76 + 62)) {
      ++stats_.kept_inputs;
      why = KeepWhy::Mismatch;
      return false;
    }
    std::memcpy(mask_vs_, eye, 16);
    pick(7, mask_vs_ + 4);
    for (uint32_t r = 0; r < 7; ++r) {   // fade[r] is the x of row r (the sliding window), fade[7] unused
      float row[4];
      pick(r, row);
      mask_vs_[8 + r] = row[0];
    }
    mask_vs_[15] = 0.0f;
    for (uint32_t r = 0; r < 4; ++r) pick(9 + r, mask_vs_ + 16 + 4 * r);
    for (uint32_t r = 0; r < 4; ++r) pick(13 + r, mask_vs_ + 32 + 4 * r);
    std::memcpy(mask_vs_ + 48, in.vs_block + v.bools_offset, 32);
    std::memcpy(mask_vs_ + 56, palette, 63 * 16);   // (the palette is one contiguous range of the stream's block)
    std::memset(fs_, 0, sizeof(fs_));
    out.own.program = mask_program_;
    out.own.vs_constants = reinterpret_cast<const uint8_t*>(mask_vs_), out.own.vs_bytes = sizeof(mask_vs_);
    out.own.blend[0] = {0, 32}, out.own.blend[1] = {64, 128}, out.own.blend[2] = {224, 1008}, out.own.blend_count = 3;   // S2: EyePos + PlanePos, PlaneInv + camera, the bone palette follow the tick blend
  }
  out.own.fs_constants = reinterpret_cast<const uint8_t*>(fs_), out.own.fs_bytes = sizeof(fs_);
  ++stats_.replaced;
  ++(blur ? stats_.replaced_blur : stats_.replaced_mask);
  ++stats_.by_delta[size_t(used_delta - plane::kDeltaFirst)];
  stats_.rows_compared += compared;
  if (!blur) {
    if (stats_.replaced_mask == 1 || compared < stats_.rows_min) stats_.rows_min = compared;
    if (compared > stats_.rows_max) stats_.rows_max = compared;
  }
  return true;
}

// lane 205: one refused blur draw per census window as hex, so the next live run names the lane (stream vs record words, the stamp the entry was found at, the nth, the entry's have bits).
std::string PlaneShadowOurs::FormatRefusal(const plane::MatchDiff& d, uint32_t stamp, uint32_t nth, uint32_t have, bool incomplete) {
  char b[400];
  if (incomplete || !d.set) {
    std::snprintf(b, sizeof b, " | first refused blur: entry incomplete, stamp %u nth %u have 0x%X (the blur bit 0x2 is %s)", stamp, nth, have, (have & kPlaneDrawHaveBlur) ? "set" : "NOT set");
    return b;
  }
  std::snprintf(b, sizeof b, " | first refused blur: c%u lane %u stamp %u nth %u have 0x%X stream %08X %08X %08X %08X record %08X %08X %08X %08X", d.reg, d.lane, stamp, nth, have, d.stream[0], d.stream[1], d.stream[2], d.stream[3],
                d.record[0], d.record[1], d.record[2], d.record[3]);
  return b;
}

std::string PlaneShadowOurs::WindowDetail() const {
  std::string s = Report() + first_refused_;
  first_refused_.clear();   // once per window
  return s;
}

std::string PlaneShadowOurs::Report() const {
  char b[1000];
  std::snprintf(b, sizeof b,
                "plane_shadow: frames %llu (not ready %llu) claimed %llu (mask %llu, blur %llu) replaced %llu (mask %llu, blur %llu) | kept: unjoined %llu, mismatch %llu (constants %llu, incomplete %llu, late %llu), variant %llu, state %llu, inputs %llu | join offset %d (stamps held %zu, relearned %llu) | rows verified %llu (mask draws min %llu max %llu) | joined with casters' mesh %llu, on the stream's mesh %llu",
                (unsigned long long)stats_.frames, (unsigned long long)stats_.frames_not_ready, (unsigned long long)stats_.claimed, (unsigned long long)stats_.claimed_mask,
                (unsigned long long)stats_.claimed_blur, (unsigned long long)stats_.replaced, (unsigned long long)stats_.replaced_mask, (unsigned long long)stats_.replaced_blur,
                (unsigned long long)stats_.kept_unjoined, (unsigned long long)stats_.kept_mismatch, (unsigned long long)stats_.kept_rows, (unsigned long long)stats_.kept_incomplete, (unsigned long long)stats_.kept_late,
                (unsigned long long)stats_.kept_variant, (unsigned long long)stats_.kept_state, (unsigned long long)stats_.kept_inputs, delta_known_ ? delta_ : 99, board_.stamps(), (unsigned long long)stats_.relearned, (unsigned long long)stats_.rows_compared,
                (unsigned long long)stats_.rows_min, (unsigned long long)stats_.rows_max, (unsigned long long)stats_.joined,
                (unsigned long long)stats_.alone);
  return b;
}

ReplaceHookHandle MakePlaneShadowHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  plane::Faults f;
  std::string err;
  if (!plane::Faults::Parse(args.fault, f, err)) {
    h.error = err;
    return h;
  }
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  auto hook = std::make_unique<PlaneShadowOurs>(*args.renderer, f);
  if (!hook->ok()) {
    h.error = "plane shadow: not started (the renderer refused one of the plane programs)";
    return h;
  }
  PlaneShadowOurs* raw = hook.get();
  h.report = [raw]() { return raw->Report(); };
  h.set_record = [raw](std::shared_ptr<const SceneTables> r) { raw->SetRecord(r); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene

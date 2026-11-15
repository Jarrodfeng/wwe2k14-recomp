// See plane_shadow_ours.h. Session 101, lane 139 (SH3 slice 2).
#include "plane_shadow_ours.h"

#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer_adapter/plane_bake.h"   // S102 L2
#include "../renderer/shaders/spv/rdr_plane_bake_frag.h"   // S102 L2: the bake lever's fullscreen program
#include "../renderer/shaders/spv/rdr_plane_bake_vert.h"
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
  // S102 L2: the bake's fullscreen passes (no vertex buffer). Optional: a driver that refuses the module leaves the bake lever off (g_shared.bake_program_ok stays false), the chain is unchanged.
  rdr::ProgramDesc bake;
  bake.name = "rdr_plane_bake";
  bake.vertex_spirv = kSpv_rdr_plane_bake_vert, bake.vertex_bytes = sizeof(kSpv_rdr_plane_bake_vert);
  bake.fragment_spirv = kSpv_rdr_plane_bake_frag, bake.fragment_bytes = sizeof(kSpv_rdr_plane_bake_frag);
  if (renderer.AddProgramChecked(bake, bake_program_)) {
    bake_layout_ = renderer.AddVertexLayout(rdr::VertexLayoutDesc{});
    rdr_adapter::plane_bake::g_shared.bake_program_ok = true;
  } else {
    bake_program_ = 0;
    rdr_adapter::plane_bake::g_shared.bake_program_ok = false;
  }
}

// ---- S102 L2: what the hook learns about the rope proxy, and the bake's draws --------------------------------------------------------------------------------------------------------------
void PlaneShadowOurs::NoteEye(uint32_t object, const float* eye) {
  EyeSet* set = nullptr;
  for (uint32_t i = 0; i < eyes_used_; ++i)
    if (eyes_[i].object == object) set = &eyes_[i];
  if (!set) {
    if (eyes_used_ >= std::size(eyes_)) return;
    set = &eyes_[eyes_used_++];
    *set = EyeSet{};
    set->object = object;
  }
  for (uint32_t i = 0; i < set->n; ++i)
    if (set->eye[i][0] == eye[0] && set->eye[i][1] == eye[1] && set->eye[i][2] == eye[2]) return;
  if (set->n < 4) std::memcpy(set->eye[set->n++], eye, 12);
}
void PlaneShadowOurs::FinishEyes() {
  rdr_adapter::plane_bake::Shared& sh = rdr_adapter::plane_bake::g_shared;
  uint32_t found = 0;
  for (uint32_t i = 0; i < eyes_used_; ++i)
    if (eyes_[i].n >= 3) found = eyes_[i].object;
  if (found && found == sh.rope_object) ++sh.rope_object_frames;
  else if (found) sh.rope_object = found, sh.rope_object_frames = 1;
  else if (eyes_used_ == 0) {}   // a frame with no mask draw says nothing
  else sh.rope_object_frames = 0;
  eyes_used_ = 0;
}

// The fullscreen passes (soften x / z, composite): the binder draws the blur-z stream draw as a template (its place, target, fixed state), the program, count and both blocks are ours.
bool PlaneShadowOurs::ReplaceBakePass(const rdr_adapter::ReplaceIn&, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  namespace pb = rdr_adapter::plane_bake;
  const pb::Call& c = pb::g_call;
  const pb::Shared& sh = pb::g_shared;
  if (!bake_program_) {
    why = KeepWhy::Variant;
    return false;
  }
  std::memset(bake_vs_, 0, sizeof(bake_vs_));
  std::memset(bake_fs_, 0, sizeof(bake_fs_));
  const bool composite = c.variant == pb::kVariantComposite;
  bake_fs_[0] = c.variant == pb::kVariantSoftenX ? 0.0f : c.variant == pb::kVariantSoftenZ ? 1.0f : 2.0f;
  bake_fs_[1] = c.target_w ? 1.0f / float(c.target_w) : 0.0f;   // (soften: the step of the source = the target's own size, as the passes are the same size)
  bake_fs_[2] = composite && pb::g_fault == pb::Fault::CompositeMin ? 1.0f : composite && pb::g_fault == pb::Fault::NoBake ? 2.0f : 0.0f;
  bake_fs_[3] = c.bake_size ? pb::g_soft / (2.0f * c.extent / float(c.bake_size)) : 1.0f;   // the gaussian's sigma in texels: g_soft world units at every bake size
  if (composite) {
    std::memcpy(bake_vs_ + 4, c.m + 0, 16);     // row x   (the VERTEX block: the frame pump interpolates it between ticks)
    std::memcpy(bake_vs_ + 8, c.m + 4, 16);     // row y
    std::memcpy(bake_vs_ + 12, c.m + 12, 16);   // row w
    bake_fs_[16] = c.centre[0], bake_fs_[17] = c.centre[1], bake_fs_[18] = (pb::g_fault == pb::Fault::Scale5 ? 0.95f : 1.0f) / c.extent, bake_fs_[19] = c.plane_y;
    bake_fs_[20] = pb::g_fault == pb::Fault::HalfTexel ? 2.0f / 256.0f : 0.0f;   // PLANT: the bake read two texels of the GAME's 256 map off (the 360's half-pixel quirk and a map texel): 2.5 units, 5-6 pixels on the mat
    bake_fs_[21] = pb::g_fault == pb::Fault::NoWrestler ? 0.0f : 1.0f;
    bake_fs_[22] = pb::g_gain;
  }
  out = rdr_adapter::ReplaceOut{};
  out.own.set = true;
  out.own.program = bake_program_;
  out.own.vertex_layout = bake_layout_;
  out.own.topology = 3;
  out.own.count = 3;
  out.own.vs_constants = reinterpret_cast<const uint8_t*>(bake_vs_), out.own.vs_bytes = sizeof(bake_vs_);
  if (composite && pb::g_fault != pb::Fault::CompositeTickCamera) out.own.blend[0] = {16, 48}, out.own.blend_count = 1;   // F1: M follows the camera between game ticks (the planted composite_tick_camera leaves it at the tick's)
  out.own.fs_constants = reinterpret_cast<const uint8_t*>(bake_fs_), out.own.fs_bytes = sizeof(bake_fs_);
  out.slots[0].set = true;
  out.slots[0].image = rdr::ImageId(c.slot0);
  return true;
}

void PlaneShadowOurs::SetRecord(const std::shared_ptr<const SceneTables>& record) {
  if (!record || (record->plane_draws.empty() && record->plane_frames.empty())) return;
  board_.Add(record->plane_frames, record->plane_draws);
}

bool PlaneShadowOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  ++stats_.frames;
  FinishEyes();   // S102 L2: the previous frame's eyes decide which object is the rope proxy
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
  if (rdr_adapter::plane_bake::g_call.variant >= rdr_adapter::plane_bake::kVariantSoftenX) return ReplaceBakePass(in, out, why);   // S102 L2: the bake's own fullscreen draws (not the chain's: no record join, no census)
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
    fs_[1] = fs_[2] = fs_[3] = 0.0f;   // S102 P3 (review F3): only c34.x is written by the game (sub_827ECD10, module+100 at 0x827ECDB4 -> PS c34 at 0x827ECDCC); y, z, w are uninitialised stack words and nothing may depend on them
    ++stats_.stream_c34;   // S102 P3: still the stream's
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
    NoteEye(object, eye);   // S102 L2
    pick(7, mask_vs_ + 4);
    for (uint32_t r = 0; r < 7; ++r) {   // fade[r] is the x of row r (the sliding window), fade[7] unused
      float row[4];
      pick(r, row);
      mask_vs_[8 + r] = row[0];
    }
    mask_vs_[15] = 0.0f;
    for (uint32_t r = 0; r < 4; ++r) pick(9 + r, mask_vs_ + 16 + 4 * r);
    for (uint32_t r = 0; r < 4; ++r) pick(13 + r, mask_vs_ + 32 + 4 * r);
    {   // S102 L2: this frame's screen fit M = PlaneInv * CameraViewProj and the plane's position, from a draw whose rows were just proven against the stream's own
      rdr_adapter::plane_bake::Shared& sh = rdr_adapter::plane_bake::g_shared;
      const float* pinv = mask_vs_ + 16;
      const float* cam = mask_vs_ + 32;
      for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < 4; ++j) {
          double acc = 0;
          for (uint32_t k = 0; k < 4; ++k) acc += double(pinv[4 * i + k]) * double(cam[4 * k + j]);
          sh.m[4 * i + j] = float(acc);
        }
      sh.plane_x = mask_vs_[4], sh.plane_y = mask_vs_[5], sh.plane_z = mask_vs_[6];
      sh.proj_valid = true;
      ++sh.proj_frame_token;
    }
    std::memcpy(mask_vs_ + 48, in.vs_block + v.bools_offset, 32);
    if (faults_.palette_prev && have_prev_palette_) std::memcpy(mask_vs_ + 56, prev_palette_, sizeof(prev_palette_));   // S102 P3 PLANT palette_prev: the previous draw's palette (the wrong proxy's)
    else std::memcpy(mask_vs_ + 56, palette, 63 * 16);   // (the palette is one contiguous range of the stream's block)
    if (faults_.palette_prev) {   // S102 P3 (review F11): the 1,008-byte copy runs only for the planted fault, never on the default path
      std::memcpy(prev_palette_, palette, sizeof(prev_palette_));
      have_prev_palette_ = true;
    }
    ++stats_.stream_eye, ++stats_.stream_palette, ++stats_.stream_bools;   // S102 P3: still the stream's
    std::memset(fs_, 0, sizeof(fs_));
    out.own.program = mask_program_;
    out.own.vs_constants = reinterpret_cast<const uint8_t*>(mask_vs_), out.own.vs_bytes = sizeof(mask_vs_);
    out.own.blend[0] = {0, 32}, out.own.blend[1] = {64, 128}, out.own.blend[2] = {224, 1008}, out.own.blend_count = 3;   // S2: EyePos + PlanePos, PlaneInv + camera, the bone palette follow the tick blend
    if (rdr_adapter::plane_bake::g_call.variant == rdr_adapter::plane_bake::kVariantBake) {   // S102 L2: a rope draw into the WORLD-plane bake: the screen fit is replaced by an ortho matrix over the plane (x, z) around the bake's centre
      namespace pb = rdr_adapter::plane_bake;
      const pb::Call& c = pb::g_call;
      const float inv = 1.0f / c.extent;
      float cx = c.centre[0], cz = c.centre[1];
      if (pb::g_fault == pb::Fault::WrongArena) cx += 60.0f;   // PLANT: another arena's plane position
      std::memset(mask_vs_ + 16, 0, 32 * sizeof(float));
      mask_vs_[16] = 1.0f, mask_vs_[21] = 1.0f, mask_vs_[26] = 1.0f, mask_vs_[31] = 1.0f;                 // PlaneInv = identity
      mask_vs_[32] = inv, mask_vs_[35] = -cx * inv;                                                       // X = (x - cx) / extent
      mask_vs_[38] = inv, mask_vs_[39] = -cz * inv;                                                       // Y = (z - cz) / extent
      mask_vs_[47] = 1.0f;                                                                                // W = 1  (Z = 0: the shader sets z from the darkness)
      out.own.blend_count = 0;   // the bake is not tweened between ticks
    }
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
  char tail[240];   // S102 P3: what is still the stream's in a replaced draw (PLANE_OURS_WHOLE.md section 3)
  std::snprintf(tail, sizeof tail, " | still the stream's, per replaced draw: EyePos %llu, palette %llu, bone switches %llu (mask draws), c34 %llu (blur draws)", (unsigned long long)stats_.stream_eye, (unsigned long long)stats_.stream_palette,
                (unsigned long long)stats_.stream_bools, (unsigned long long)stats_.stream_c34);
  return std::string(b) + tail;
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

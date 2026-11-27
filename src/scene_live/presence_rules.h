// S103 S2: hold by PRESENCE, not by class (docs\renderer\S103_FABLE_REVIEW_2.md Q1 1.2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure, header-only, no game / SDK / renderer includes beyond our own record tables: particle_source.h (bit 11), crowd_contrib.cpp (bit 10), live.cpp (the switches) and tools\scene\s2_unit.cpp include it.
//
// Until S2 two required inputs were held for EVERY frame by a class-wide constant (particle_source.h kStampParticles, crowd_snapshot_core.h kCrowdOursWhole), so no frame could ever be OURS.
// The per-frame verdicts already existed under those constants. With its switch on, each rule stamps the frames that do NOT contain the unproven thing:
//   bit 11 particles: stamp when the frame's pass status is whole (WholePass: no reason bit, nothing unreadable, no other-class item). An item with an unnamed texture (kPiTexture), an
//                     unknown blend code (kPiBlend: a code above 2; 0 / 1 / 2 are [C], particle_reader.h kBlendCodesConfirmed), an other-class item, an unreadable read, a dropped sprite or no camera holds that frame only.
//                     A frame with no particle item at all is whole. A frame whose pass did not run in the paired hook frame is NOT stamped (Take returns false: the source's stale_node /
//                     no_node counters, unchanged): the pairing of the node's clock is [U] until a run shows `pair (0 ...)`.
//   bit 10 crowd:     stamp when the snapshot is otherwise whole AND no visible sign holder, foam hand or sign picture is present (crowd_snapshot_core.h HolderPresent).
// Each switch is ON by default in the game (live.cpp REXCVAR_DEFINE_BOOL ..._by_presence, default true; the header value below is only the unit tests' start state; S103 B4X comment fix). Under --wwe2k14_rdr_scene_force_inputs they change no
// decision, only the stamps and the census.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "../renderer_scene/particle_tables.h"

namespace rdr_scene::live::presence {

// --wwe2k14_rdr_scene_particles_by_presence / --wwe2k14_rdr_scene_crowd_by_presence (live.cpp sets them at start from the cvars, which default TRUE; tests set them directly; the header default false is only the unit tests' start state).
inline std::atomic<bool> g_particles{false};
inline std::atomic<bool> g_crowd{false};
// S103 C1 review fix 2: --wwe2k14_rdr_scene_crowd_screen_trust (default false). Off: a sign holder the reader's port of the singles' screen test (sub_8276E2C0) hides still counts as PRESENT for bit 10
// (fail closed: the port is unproven). On only after a run's census shows `screen_check ... screen_diff 0` with eq > 0.
inline std::atomic<bool> g_crowd_screen_trust{false};

// ---- bit 11: the per-frame particle verdict. The FIRST failing reason, in this order (one count per taken frame).
enum class PartWhy : uint32_t { WholeEmpty = 0, WholeItems, NoCamera, Unreadable, OtherClass, Texture, Blend, Dropped, Malformed, ScreenCam, MultiNode, kCount };
inline const char* PartWhyName(PartWhy w) {
  static const char* const n[] = {"whole_empty", "whole_items", "no_camera", "unreadable", "other_class", "texture", "blend", "dropped", "malformed", "screen_cam", "multi_node"};
  return uint32_t(w) < uint32_t(PartWhy::kCount) ? n[uint32_t(w)] : "?";
}
#ifdef RDR_S2_PLANTS
inline int g_part_plant = 0;   // test only: 1 = the blend reason is ignored (an unproven blend code passes); 2 = the texture reason is ignored; 3 = the PF census column ignores the stream draws
#endif
// `sprites` = the sprites the snapshot holds (0 = an empty pass: nothing to draw). kPiPivot is info only since F3 (particle_source.h), never a reason here.
inline PartWhy ParticleVerdict(const rdr_scene::ParticleStatus& s, uint64_t sprites) {
  uint32_t r = s.reasons;
#ifdef RDR_S2_PLANTS
  if (g_part_plant == 1) r &= ~uint32_t(rdr_scene::kPiBlend);
  if (g_part_plant == 2) r &= ~uint32_t(rdr_scene::kPiTexture);
#endif
  if (r & rdr_scene::kPiNoCamera) return PartWhy::NoCamera;
  if ((r & rdr_scene::kPiUnreadable) || s.unreadable) return PartWhy::Unreadable;
  if ((r & rdr_scene::kPiOtherClass) || s.other_class) return PartWhy::OtherClass;
  if (r & rdr_scene::kPiTexture) return PartWhy::Texture;
  if (r & rdr_scene::kPiBlend) return PartWhy::Blend;
  if (r & rdr_scene::kPiDropped) return PartWhy::Dropped;
  if (r & rdr_scene::kPiMalformed) return PartWhy::Malformed;
  if (r & rdr_scene::kPiScreenCam) return PartWhy::ScreenCam;   // S103 MP: drawn with the node's 2D screen camera; the flat drawer for them is not built: Held
  return sprites ? PartWhy::WholeItems : PartWhy::WholeEmpty;
}
inline bool IsWhole(PartWhy w) { return w == PartWhy::WholeEmpty || w == PartWhy::WholeItems; }
// The stamp rule: the class constant (kStampParticles) OR the presence switch, and the frame is whole.
inline bool StampParticles(bool class_constant, PartWhy w) { return (class_constant || g_particles.load(std::memory_order_relaxed)) && IsWhole(w); }

struct PartCensus {
  std::atomic<uint64_t> why[uint32_t(PartWhy::kCount)] = {};
  std::atomic<uint64_t> stamped{0};
  // S103 PF (is "whole_empty" a lie?): the stream's particle-class draws (the binder: DrawOrigin call_site = a particle recorder site) seen on frames where the reader's LATEST verdict was whole_empty.
  // Clocks: the binder notes once per stream frame, the reader once per hook frame it could pair, so a frame can be paired with the verdict one frame older (the reader's pairing is +1, particle_source.h):
  // read the column as a rate over a window, not a per-frame proof. If the reader never ran (no verdict yet) the frame is not counted as empty.
  std::atomic<uint32_t> last_why{uint32_t(PartWhy::kCount)};
  std::atomic<uint64_t> stream_frames{0}, empty_drawn_frames{0}, empty_drawn_draws{0};
  // S103 MPBF 8: the same column for frames the source declared EMPTY through TakeEmpty (no node call in the frame): that path never calls Note(), so last_why never says whole_empty for it. A verdict (Note) clears the flag.
  std::atomic<bool> last_empty_pass{false};
  std::atomic<uint64_t> te_empty_frames{0}, te_drawn_frames{0}, te_drawn_draws{0};
  void NoteEmptyPass() {
    last_empty_pass.store(true, std::memory_order_relaxed);
    te_empty_frames.fetch_add(1, std::memory_order_relaxed);
  }
  void NoteStreamFrame(uint64_t particle_draws) {
    if (!particle_draws) return;
#ifdef RDR_S2_PLANTS
    if (g_part_plant == 3) return;
#endif
    if (last_empty_pass.load(std::memory_order_relaxed)) {
      te_drawn_frames.fetch_add(1, std::memory_order_relaxed);
      te_drawn_draws.fetch_add(particle_draws, std::memory_order_relaxed);
    }
    stream_frames.fetch_add(1, std::memory_order_relaxed);
    if (last_why.load(std::memory_order_relaxed) == uint32_t(PartWhy::WholeEmpty)) {
      empty_drawn_frames.fetch_add(1, std::memory_order_relaxed);
      empty_drawn_draws.fetch_add(particle_draws, std::memory_order_relaxed);
    }
  }
  void Note(PartWhy w, bool stamped_now) {
    last_why.store(uint32_t(w), std::memory_order_relaxed);
    last_empty_pass.store(false, std::memory_order_relaxed);
    why[uint32_t(w)].fetch_add(1, std::memory_order_relaxed);
    if (stamped_now) stamped.fetch_add(1, std::memory_order_relaxed);
  }
  // ", presence (switch on|off): stamped S; frames by verdict [whole_empty A whole_items B no_camera ...]"
  std::string Str() const {
    std::string s = ", presence (switch ";
    s += g_particles.load(std::memory_order_relaxed) ? "on" : "off";
    s += "): stamped " + std::to_string((unsigned long long)stamped.load(std::memory_order_relaxed)) + "; frames by verdict [";
    for (uint32_t i = 0; i < uint32_t(PartWhy::kCount); ++i)
      s += std::string(i ? " " : "") + PartWhyName(PartWhy(i)) + " " + std::to_string((unsigned long long)why[i].load(std::memory_order_relaxed));
    s += "]";
    s += ", stream particle draws on whole_empty frames: frames " + std::to_string((unsigned long long)empty_drawn_frames.load(std::memory_order_relaxed)) + ", draws " +
         std::to_string((unsigned long long)empty_drawn_draws.load(std::memory_order_relaxed)) + " (of " + std::to_string((unsigned long long)stream_frames.load(std::memory_order_relaxed)) + " stream frames with particle draws)";
    s += ", stream particle draws on TakeEmpty frames: empty frames " + std::to_string((unsigned long long)te_empty_frames.load(std::memory_order_relaxed)) + ", stream frames " +
         std::to_string((unsigned long long)te_drawn_frames.load(std::memory_order_relaxed)) + ", draws " + std::to_string((unsigned long long)te_drawn_draws.load(std::memory_order_relaxed));
    return s;
  }
};
inline PartCensus& Particles() {
  static PartCensus c;
  return c;
}

}  // namespace rdr_scene::live::presence

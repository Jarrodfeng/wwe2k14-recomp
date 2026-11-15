// The crowd card bake, owned (S102 lane P1, offline step; docs\renderer\CARD_BAKE_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// CardBakeOurs is the BakeHook behind `g_own_card_bake` (fview / rplay --own-card-bake). The binder takes the 15-20 bake passes of a frame out of the stream's walk and re-issues them as
// one group where the first one sat; this class says WHICH person type each pass is and gives the pose registers (VS c76 + 3 * bone: the 20-bone LOD-2 body's matrices) from the scene
// record's palette of that type: bit-exact copies of the record's palette matrices, one fixed bone -> palette-slot permutation per type, record numbered like the stream frame
// (tools\crowd\bake_pose_check.py, S101 lane 155). Everything else of the pass (program, geometry, lights, camera, textures, targets) is still the stream's own pass table: these are the
// stand-ins the live version (P0's node switch) replaces, listed in CARD_BAKE_OURS.md section 5.
//
// The bone -> slot permutation is LEARNED here from the stream's own bank (the intersection, over frames, of the slots whose matrix equals the bank's bone, as bake_pose_check does); the live
// version reads it from the bake model's bone list. A bone whose slot is not resolved yet keeps the stream's matrix (counted). No game or SDK includes. Render thread only.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "../renderer_adapter/bake_hook.h"
#include "record_io.h"

namespace rdr_scene {

// Planted faults (--bake-fault NAME): each must show in the card-picture check.
struct CardBakeFaults {
  bool wrong_slot = false;   // pass k copies into the picture of pass k - 1 (our bake writing to the wrong card slot)
  bool wrong_pose = false;   // the pose is taken from the NEXT palette slot (a wrong permutation)
  bool no_pose = false;      // control: the hook claims the pass but leaves the stream's bank (must be byte-equal to the game's)
  bool stale = false;        // from the 31st frame on, the pose is taken from the PREVIOUS frame's record (the permutation is learned by then: a stale record shows)
  static bool Parse(const std::string& list, CardBakeFaults& out, std::string& error);
};

struct CardBakeStats {
  uint64_t frames = 0, frames_no_record = 0, passes = 0, passes_no_type = 0;
  uint64_t bones_written = 0, bones_equal = 0, bones_differ = 0, bones_unresolved = 0, bones_unused = 0, perm_conflicts = 0;
};

class CardBakeOurs final : public rdr_adapter::BakeHook {
 public:
  explicit CardBakeOurs(const CardBakeFaults& faults) : faults_(faults) {}
  // The record numbered like the stream frame (NOT the crowd's j-1). Null = none.
  void SetRecord(std::shared_ptr<const SceneTables> record);
  bool FrameReady(const rdr_adapter::LoadedFrame& frame, uint32_t pass_count) override;
  uint32_t Pose(const rdr_adapter::BakePassIn& in, const rdr_adapter::ConstantBank& stream, rdr_adapter::ConstantBank& out) override;
  uint32_t CopyDest(const rdr_adapter::BakePassIn& in) override;
  std::string Census() const override;
  const CardBakeStats& stats() const { return stats_; }

 private:
  static constexpr uint32_t kTypes = 20, kSlots = 17;
  struct Palette { uint32_t first = 0, count = 0; bool have = false; };
  struct Bone { uint32_t cand = (1u << kSlots) - 1; uint32_t seen = 0; };   // candidate palette slots (bit s), frames a slot-matching bone was seen
  CardBakeFaults faults_;
  std::shared_ptr<const SceneTables> record_, prev_record_;
  std::array<Palette, kTypes> palette_{}, prev_palette_{};
  std::array<std::array<Bone, rdr_adapter::kBakeMaxBones>, kTypes> bone_{};
  std::unordered_map<uint32_t, int> pass_type_;   // the picture a pass copies into (the game's image id) -> the person type, learned from the stream's bank on first sight (a pass index is not a type when G4 cut some passes)
  uint32_t prev_dest_ = 0;
  CardBakeStats stats_;
};

}  // namespace rdr_scene

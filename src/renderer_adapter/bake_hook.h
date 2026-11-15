// S102 P1: the crowd card bake, owned (docs\renderer\CARD_BAKE_OURS.md): the seam between the binder's walk of a replayed frame and the code that poses the bake.
// The binder knows the bake's passes (a 64 x 64 role-AtlasTarget SetTargets, a Clear, the bake draw, the Copy into the type's picture), takes every op of every pass out of the
// walk, and re-issues them as ONE group at the first pass's place (so the pictures exist exactly where the game made them). The hook says which person type a pass is and
// gives the pose registers from the scene record; everything else of the draw (program, geometry, lights, camera, textures) is still the stream's own pass table this step
// (the stand-ins P0 must replace: docs\renderer\CARD_BAKE_OURS.md section 5). No game or SDK includes.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

#include "capture_reader.h"

namespace rdr_adapter {

inline std::atomic<bool> g_own_card_bake{false};   // S102 P1: live --wwe2k14_rdr_own_card_bake, fview / rplay --own-card-bake[=none]; default off
inline std::atomic<bool> g_own_card_bake_none{false};  // test arm: the passes are taken out and NOT re-issued ("no bake at all": the cards read what the pictures held)

inline bool g_own_card_bake_before_dead = false;   // PLANTED FAULT p1_before_g4 (fview --bake-before-dead): the old plan order; P1 takes the passes G4 cuts

inline bool g_own_card_bake_skip_leak = false;   // PLANTED FAULT p1_skip_leak (fview --bake-skip-leak): the half-rate shadow skip stays on for the bake group (review F2 unfixed)

// The bake registers: VS c76 + 3 * bone + row (row-vector 4x4, three registers a bone, the translation in the fourth component).
inline constexpr uint32_t kBakeBoneReg = 76u;
inline constexpr uint32_t kBakeMaxBones = 20u;

struct BakePassIn {
  const LoadedFrame* frame = nullptr;
  uint32_t pass = 0;          // 0.. in stream order
  uint32_t pass_count = 0;
  uint32_t draw_index = 0;    // the pass's bake draw
  uint32_t copy_dest = 0;     // the game's image id the pass copies into (the type's picture)
};

class BakeHook {
 public:
  virtual ~BakeHook() = default;
  // Once per frame with bake passes, at the first pass: is there a record to pose from? False: the passes stay the stream's.
  virtual bool FrameReady(const LoadedFrame& frame, uint32_t pass_count) = 0;
  // The person type (0..19) of pass `in.pass`, and the vertex bank the pass draws with: `out` starts as a copy of the stream's bank; the hook overwrites the bone registers it knows from the
  // record. Returns the bone matrices written (0 = the stream's bank is used as is).
  virtual uint32_t Pose(const BakePassIn& in, const ConstantBank& stream, ConstantBank& out) = 0;
  // The picture a pass copies into (planted fault: another slot). Default: the stream's.
  virtual uint32_t CopyDest(const BakePassIn& in) { return in.copy_dest; }
  // Census line (the binder prints it with the windows).
  virtual std::string Census() const { return {}; }
};

// What the binder did, per run of the walk.
struct BakeCounters {
  uint64_t frames_ours = 0, frames_fallback = 0;     // frames whose passes ours re-issued / frames the stream's stayed (no record, hook not ready)
  uint64_t passes_found = 0, passes_ours = 0;        // bake passes found in the stream / re-issued by ours
  uint64_t ops_taken = 0;                            // stream ops left out of the walk (4 a pass)
  uint64_t draws_ours = 0, copies_ours = 0;          // re-issued
  uint64_t not_a_bake = 0;                           // 64 x 64 atlas-target passes whose draws are not the bake program (left alone)
};

}  // namespace rdr_adapter

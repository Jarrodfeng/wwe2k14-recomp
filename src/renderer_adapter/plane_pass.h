// S102 P3: the plane shadow chain (mask + downscale + two blurs), owned as one pass (docs\renderer\PLANE_OURS_WHOLE.md; the contract docs\renderer\OWN_THE_PASS.md).
// The binder finds the chain in a frame's ops (a SetTargets whose first colour target has the role ShadowMap, [Clear], the mask draws of pass site 0x82BF4D38, Copy; the downscale pass
// (call site 0x828159FC); the two blur passes (call site 0x827EEC84)), takes EVERY op of the four passes out of the stream's walk and re-issues them as ONE group at the first pass's place:
// the targets, clears and copies of the pass table, and the draws through the normal bind with the plane hook (lane 159) making the mask and blur draws from the record. What is still the stream's
// in this offline step is the list in PLANE_OURS_WHOLE.md section 3 (the proxy meshes, EyePos c72, the palette, the bone switches, the blur's c34, the downscale draw, the pass table's
// target / clear / copy records). No game or SDK includes.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace rdr_adapter {

inline constexpr uint32_t kPlaneDownCallSite = 0x828159FCu;   // the downscale draw's call site (CALL_DOWN in tools\shadow\shadow_census.py)
inline constexpr uint32_t kPlaneBlurCallSite = 0x827EEC84u;   // the two blur draws (blur x then blur z); the mask's pass site is kPlaneShadowSite (draw_replace_hook.h)

inline std::atomic<bool> g_own_plane{false};        // S102 P3: live --wwe2k14_rdr_own_plane (NOT wired in this step: needs P0's node switch), fview / rplay --own-plane[=none]; default off
inline std::atomic<bool> g_own_plane_none{false};   // test arm: the chain's ops are taken out and NOT re-issued ("no plane chain at all": the ring mat reads the pictures the game left)

// PLANTED FAULTS (fview --own-plane-fault NAME). The record-side twins (wrong record frame, wrong proxy palette) are the plane hook's: --replace-fault plane.stamp_lock,no_verify / plane.palette_prev.
enum class OwnPlaneFault : uint8_t { None = 0, NoCopy, SwapBlur, WrongDest, NoAliasEnd, ForceAlias };
inline bool g_own_plane_force_overlap = false;    // PLANTED (fview --own-plane-fault force_overlap): the first chain op counts as dropped by another planner (the group must decline, re-issuing 0 dropped ops)
inline bool g_own_plane_ignore_overlap = false;   // PLANTED (ignore_overlap): the overlap check is off (the group then re-issues dropped ops: the gate must catch it)
inline OwnPlaneFault g_own_plane_fault = OwnPlaneFault::None;   // no_copy: the mask pass's copy into its picture is forgotten; swap_blur: the two blur passes run z then x; wrong_dest: the downscale copies into the mask's picture
inline bool ParseOwnPlaneFault(const std::string& name, std::string& error) {
  if (name.empty() || name == "none") g_own_plane_fault = OwnPlaneFault::None;
  else if (name == "no_copy") g_own_plane_fault = OwnPlaneFault::NoCopy;
  else if (name == "swap_blur") g_own_plane_fault = OwnPlaneFault::SwapBlur;
  else if (name == "wrong_dest") g_own_plane_fault = OwnPlaneFault::WrongDest;
  else if (name == "force_alias") g_own_plane_fault = OwnPlaneFault::ForceAlias;   // TEST: the downscale pass's copy is treated as elided (alias 221 -> 249): the blur copies after it must end the alias (G8 / IssueCopy)
  else if (name == "force_overlap") g_own_plane_force_overlap = true;
  else if (name == "ignore_overlap") g_own_plane_ignore_overlap = true;
  else if (name == "no_alias_end") g_own_plane_fault = OwnPlaneFault::NoAliasEnd;   // the shared copy helper does not end an alias before a real copy (F5 plant)
  else {
    error = "unknown own-plane fault '" + name + "' (no_copy, swap_blur, wrong_dest, no_alias_end, force_alias, force_overlap, ignore_overlap)";
    return false;
  }
  return true;
}

// What the binder did, per run of the walk.
struct PlaneCounters {
  uint64_t frames_chain = 0;        // frames in which a plane chain was found (the four passes in the expected order)
  uint64_t frames_ours = 0;         // frames whose chain ours re-issued as one group
  uint64_t frames_complete = 0;     // ... of which every mask and blur draw was made by the plane hook from the record (the stream's draws left: the downscale only)
  uint64_t frames_partial = 0;      // ... of which at least one mask / blur draw stayed the stream's (the hook refused or was not ready): the group still ran, the frame is the game's for those draws
  uint64_t frames_declined = 0;     // a chain was found but this frame's passes stay the stream's (no hook, an irregular chain, a path the group does not cover): counted by reason below
  uint64_t declined_no_hook = 0, declined_irregular = 0, declined_role = 0, declined_other = 0;
  uint64_t declined_overlap = 0;    // S102 P3 (review F4): a chain op is in another planner's drop set (G4 dead work, the post claim, the bake group, G3's turns): the stream's walk keeps the frame
  uint64_t declined_blank = 0;      // S102 P3 (G4b): G4b skips this frame's blank plane chain (plane_blank_on_): the chain is theirs, no op of it is ours
  uint64_t declined_extra = 0;      // S102 P3 (review F10c): the frame holds mask / downscale / blur site draws outside the one chain found (a second chain, a stray draw): declined
  uint64_t overlap_issued = 0;      // chain ops that another planner had dropped and the group issued all the same (0 unless the planted ignore_overlap fault): the gate asserts 0
  uint64_t passes = 0, ops_taken = 0;
  uint64_t targets = 0, clears = 0, draws = 0, copies = 0;      // issued by the group
  uint64_t mask_draws = 0, blur_draws = 0, down_draws = 0;
  uint64_t draws_ours = 0, draws_stream = 0;                      // mask + blur draws the plane hook made / the stream's that stayed (the downscale draw is counted in down_draws only)
  uint64_t copies_aliased = 0;
  uint64_t alias_ended = 0;   // real copies of the group that ended an alias on their destination (G8's EndAliasOf through IssueCopy): the planted no_alias_end gives 0
};

}  // namespace rdr_adapter
